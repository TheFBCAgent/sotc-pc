#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Out-of-process thread sampler for Windows (workstream X).

Attaches to a running process, and for each thread whose name (GetThreadDescription)
contains one of the filters, repeatedly: SuspendThread, GetThreadContext (RIP, RSP),
read the stack (ReadProcessMemory) and keep the words that look like return addresses
into the exe (the preceding bytes are a call), ResumeThread. Exe addresses are named with
llvm-symbolizer (the exe's PDB); other modules (ntdll, kernel32, drivers) by their
nearest export; anything else (guest code) by address.

Prints, per filter: the top leaf functions (where RIP was) and the top inclusive frames
(leaf or anywhere on the scanned stack, counted once per sample), as percentages.

With --callers REGEX, also prints, for the samples whose leaf name matches REGEX, the first
1-3 exe frames on the stack (nearest first), e.g. to tell which code a kernel wait comes from.
Stack scanning keeps stale return addresses too, so treat deeper frames as hints.

Usage:
  python scripts/sample.py sotc-runtime.exe 15 GpuCommandProcessor JobWorker
  python scripts/sample.py sotc-runtime.exe 15 GpuCommandProcessor --top 500       --callers "NtGdiDdDDIWaitForSynchronizationObjectFromCpu|ZwWaitForMultipleObjects"
  python scripts/sample.py <pid> 10                    # every thread, grouped by name
  python scripts/sample.py --self-test                 # no game needed

Thread names: shadPS4's threads (Common::SetCurrentThreadName: "GpuCommandProcessor",
"sotcgpu:Present", ...) and the runtime's (plat_thread_set_name: "Game:Main", guest
pthread names such as the game's job workers, "sotc:gpu-shots", ...) all use
SetThreadDescription. Filters are case-insensitive substrings.
"""
import argparse
import bisect
import collections
import ctypes
import ctypes.wintypes as wt
import os
import re
import shutil
import struct
import subprocess
import sys
import time

if sys.platform != "win32":
    sys.exit("sample.py: Windows only")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPTHREAD = 0x4
LIST_MODULES_ALL = 3
CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_SIZE = 1232  # sizeof(CONTEXT) on x64; 16-byte aligned
CTX_FLAGS, CTX_RSP, CTX_RIP = 0x30, 0x98, 0xF8
INVALID_HANDLE = ctypes.c_void_p(-1).value


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG),
                ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD),
                ("EntryPoint", ctypes.c_void_p)]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


def _sig(fn, res, *args):
    fn.restype = res
    fn.argtypes = list(args)


_sig(k32.OpenProcess, wt.HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
_sig(k32.OpenThread, wt.HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
_sig(k32.CloseHandle, wt.BOOL, wt.HANDLE)
_sig(k32.CreateToolhelp32Snapshot, wt.HANDLE, wt.DWORD, wt.DWORD)
_sig(k32.Thread32First, wt.BOOL, wt.HANDLE, ctypes.POINTER(THREADENTRY32))
_sig(k32.Thread32Next, wt.BOOL, wt.HANDLE, ctypes.POINTER(THREADENTRY32))
_sig(k32.Process32FirstW, wt.BOOL, wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
_sig(k32.Process32NextW, wt.BOOL, wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
_sig(k32.SuspendThread, wt.DWORD, wt.HANDLE)
_sig(k32.ResumeThread, wt.DWORD, wt.HANDLE)
_sig(k32.GetThreadContext, wt.BOOL, wt.HANDLE, ctypes.c_void_p)
_sig(k32.ReadProcessMemory, wt.BOOL, wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
     ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
_sig(k32.VirtualQueryEx, ctypes.c_size_t, wt.HANDLE, ctypes.c_void_p,
     ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t)
_sig(k32.GetThreadDescription, ctypes.c_long, wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p))
_sig(k32.LocalFree, ctypes.c_void_p, ctypes.c_void_p)
_sig(k32.QueryFullProcessImageNameW, wt.BOOL, wt.HANDLE, wt.DWORD, wt.LPWSTR,
     ctypes.POINTER(wt.DWORD))
_sig(psapi.EnumProcessModulesEx, wt.BOOL, wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD,
     ctypes.POINTER(wt.DWORD), wt.DWORD)
_sig(psapi.GetModuleInformation, wt.BOOL, wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODULEINFO),
     wt.DWORD)
_sig(psapi.GetModuleFileNameExW, wt.DWORD, wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD)


# ----------------------------------------------------------------- process access

def find_pid(name_or_pid):
    if str(name_or_pid).isdigit():
        return int(name_or_pid)
    want = os.path.basename(name_or_pid).lower()
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE:
        raise OSError(ctypes.get_last_error(), "CreateToolhelp32Snapshot")
    found = []
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            if e.szExeFile.lower() == want:
                found.append(e.th32ProcessID)
            ok = k32.Process32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    if not found:
        sys.exit(f"sample.py: no process named {name_or_pid}")
    if len(found) > 1:
        print(f"sample.py: {len(found)} processes named {name_or_pid}; using pid {found[0]}",
              file=sys.stderr)
    return found[0]


class Process:
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise OSError(ctypes.get_last_error(), f"OpenProcess({pid})")
        buf = ctypes.create_unicode_buffer(1024)
        n = wt.DWORD(len(buf))
        self.image = buf.value if k32.QueryFullProcessImageNameW(self.h, 0, buf,
                                                                   ctypes.byref(n)) else ""

    def close(self):
        k32.CloseHandle(self.h)

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
            if got.value == 0:
                return b""
        return buf.raw[:got.value]

    def region_end(self, addr):
        mbi = MEMORY_BASIC_INFORMATION()
        if not k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi),
                                  ctypes.sizeof(mbi)):
            return addr
        return (mbi.BaseAddress or 0) + mbi.RegionSize

    def modules(self):
        arr = (ctypes.c_void_p * 1024)()
        need = wt.DWORD(0)
        if not psapi.EnumProcessModulesEx(self.h, arr, ctypes.sizeof(arr), ctypes.byref(need),
                                          LIST_MODULES_ALL):
            raise OSError(ctypes.get_last_error(), "EnumProcessModulesEx")
        out = []
        for i in range(min(need.value // ctypes.sizeof(ctypes.c_void_p), len(arr))):
            mi = MODULEINFO()
            if not psapi.GetModuleInformation(self.h, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)):
                continue
            name = ctypes.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW(self.h, arr[i], name, len(name))
            out.append(Module(self, mi.lpBaseOfDll, mi.SizeOfImage, name.value))
        out.sort(key=lambda m: m.base)
        return out

    def threads(self):
        """{tid: name} of the process's threads."""
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
        if snap == INVALID_HANDLE:
            return {}
        tids = []
        try:
            e = THREADENTRY32()
            e.dwSize = ctypes.sizeof(e)
            ok = k32.Thread32First(snap, ctypes.byref(e))
            while ok:
                if e.th32OwnerProcessID == self.pid:
                    tids.append(e.th32ThreadID)
                ok = k32.Thread32Next(snap, ctypes.byref(e))
        finally:
            k32.CloseHandle(snap)
        out = {}
        for tid in tids:
            h = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
            if not h:
                continue
            p = ctypes.c_wchar_p()
            name = ""
            if k32.GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value is not None:
                name = p.value
                k32.LocalFree(ctypes.cast(p, ctypes.c_void_p))
            k32.CloseHandle(h)
            out[tid] = name
        return out


class Module:
    def __init__(self, proc, base, size, path):
        self.proc, self.base, self.size, self.path = proc, base, size, path
        self.name = os.path.basename(path)
        self._exports = None

    @property
    def end(self):
        return self.base + self.size

    def exports(self):
        """Sorted [(address, name)] from the export table in the target's memory."""
        if self._exports is not None:
            return self._exports
        self._exports = []
        try:
            hdr = self.proc.read(self.base, 0x1000)
            lfanew = struct.unpack_from("<I", hdr, 0x3C)[0]
            opt = lfanew + 24
            magic = struct.unpack_from("<H", hdr, opt)[0]
            dd = opt + (112 if magic == 0x20B else 96)
            rva, size = struct.unpack_from("<II", hdr, dd)
            if rva == 0:
                return self._exports
            ed = self.proc.read(self.base + rva, 40)
            nfuncs, nnames, afuncs, anames, aords = struct.unpack_from("<IIIII", ed, 20)
            funcs = struct.unpack(f"<{nfuncs}I", self.proc.read(self.base + afuncs, 4 * nfuncs))
            names = struct.unpack(f"<{nnames}I", self.proc.read(self.base + anames, 4 * nnames))
            ords = struct.unpack(f"<{nnames}H", self.proc.read(self.base + aords, 2 * nnames))
            for name_rva, ordinal in zip(names, ords):
                f = funcs[ordinal]
                if rva <= f < rva + size:
                    continue  # forwarder
                raw = self.proc.read(self.base + name_rva, 256)
                self._exports.append((self.base + f, raw.split(b"\0", 1)[0].decode("ascii", "replace")))
            self._exports.sort()
        except (struct.error, IndexError):
            pass
        return self._exports

    def name_of(self, addr):
        ex = self.exports()
        i = bisect.bisect_right(ex, (addr, "￿")) - 1
        if i >= 0:
            return f"{self.name}!{ex[i][1]}"
        return f"{self.name}+{addr - self.base:#x}"


# ----------------------------------------------------------------- exe and symbols

def pe_image_base(path):
    with open(path, "rb") as f:
        data = f.read(0x1000)
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    opt = lfanew + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:
        return struct.unpack_from("<Q", data, opt + 24)[0]
    return struct.unpack_from("<I", data, opt + 28)[0]


def pe_entry_rva(path):
    with open(path, "rb") as f:
        data = f.read(0x1000)
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<I", data, lfanew + 24 + 16)[0]


def find_symbolizer(explicit=None):
    for cand in [explicit, os.environ.get("LLVM_SYMBOLIZER"), shutil.which("llvm-symbolizer")]:
        if cand and os.path.exists(cand):
            return cand
    return None


def symbolize(symbolizer, exe, vas):
    """{va: (function, 'file:line')} with llvm-symbolizer; `vas` are addresses in the exe's
    preferred image (llvm-symbolizer takes COFF addresses as VAs, ImageBase included)."""
    out = {}
    if not vas or not symbolizer:
        return out
    vas = sorted(vas)
    inp = "".join(f"{va:#x}\n" for va in vas)
    try:
        res = subprocess.run([symbolizer, f"--obj={exe}", "--no-inlines", "--demangle",
                              "--relative-address=false"],
                             input=inp, capture_output=True, text=True, timeout=600)
        text = res.stdout
        if res.returncode != 0 and not text:
            # older versions lack --relative-address
            res = subprocess.run([symbolizer, f"--obj={exe}", "--no-inlines", "--demangle"],
                                 input=inp, capture_output=True, text=True, timeout=600)
            text = res.stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        print(f"sample.py: llvm-symbolizer failed: {e}", file=sys.stderr)
        return out
    blocks = [b for b in text.replace("\r\n", "\n").split("\n\n") if b.strip()]
    for va, block in zip(vas, blocks):
        lines = block.strip().split("\n")
        func = lines[0].strip()
        loc = lines[1].strip() if len(lines) > 1 else ""
        loc = os.path.basename(loc.rsplit(":", 1)[0]) if loc and not loc.startswith("??") else ""
        out[va] = (func, loc)
    return out


# ----------------------------------------------------------------- sampling

def is_call_site(code, off):
    """True if the bytes before code[off] end with a call instruction."""
    if off >= 5 and code[off - 5] == 0xE8:
        return True  # call rel32
    if off >= 6 and code[off - 6] == 0xFF and (code[off - 5] & 0x38) == 0x10:
        return True  # call [rip+disp32] / [reg+disp32]
    if off >= 2 and code[off - 2] == 0xFF and (code[off - 1] & 0x38) == 0x10:
        return True  # call reg / [reg]
    if off >= 3 and code[off - 3] == 0xFF and (code[off - 2] & 0x38) == 0x10:
        return True  # call [reg+disp8] / with REX
    if off >= 7 and code[off - 7] == 0xFF and (code[off - 6] & 0x38) == 0x10:
        return True  # call [reg+sib+disp32]
    if off >= 4 and code[off - 4] == 0xFF and (code[off - 3] & 0x38) == 0x10:
        return True
    return False


def sample(proc, seconds, filters, interval, scan_bytes, exe_mod, exe_code, log=None):
    """Returns ({group: [samples]}, {group: set(tids)}); a sample is (rip, [return addrs])."""
    ctx_raw = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
    ctx_addr = (ctypes.addressof(ctx_raw) + 15) & ~15
    lowered = [f.lower() for f in filters]
    groups = collections.defaultdict(list)
    group_tids = collections.defaultdict(set)
    handles = {}  # tid -> (handle, group)
    exe_lo, exe_hi = (exe_mod.base, exe_mod.end) if exe_mod else (0, 0)
    deadline = time.perf_counter() + seconds
    next_enum = 0.0
    try:
        while time.perf_counter() < deadline:
            now = time.perf_counter()
            if now >= next_enum:
                next_enum = now + 0.5
                names = proc.threads()
                for tid in list(handles):
                    if tid not in names:
                        k32.CloseHandle(handles.pop(tid)[0])
                for tid, name in names.items():
                    if tid in handles:
                        continue
                    if lowered:
                        g = next((f for f, l in zip(filters, lowered) if l in name.lower()), None)
                        if g is None:
                            continue
                    else:
                        g = name or "(unnamed)"
                    h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                       | THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                    if h:
                        handles[tid] = (h, g)
                        group_tids[g].add((tid, name))
            for tid, (h, g) in handles.items():
                if k32.SuspendThread(h) == 0xFFFFFFFF:
                    continue
                try:
                    ctypes.memset(ctx_addr, 0, CONTEXT_SIZE)
                    struct.pack_into("<I", (ctypes.c_char * CONTEXT_SIZE).from_address(ctx_addr),
                                     CTX_FLAGS, CONTEXT_CONTROL)
                    if not k32.GetThreadContext(h, ctypes.c_void_p(ctx_addr)):
                        continue
                    rsp = ctypes.c_uint64.from_address(ctx_addr + CTX_RSP).value
                    rip = ctypes.c_uint64.from_address(ctx_addr + CTX_RIP).value
                    n = min(scan_bytes, max(0, proc.region_end(rsp) - rsp)) & ~7
                    stack = proc.read(rsp, n) if n else b""
                finally:
                    k32.ResumeThread(h)
                rets = []
                if exe_code:
                    for (w,) in struct.iter_unpack("<Q", stack[:len(stack) & ~7]):
                        if exe_lo <= w < exe_hi and is_call_site(exe_code, w - exe_lo):
                            rets.append(w)
                groups[g].append((rip, rets))
            time.sleep(interval)
    finally:
        for h, _ in handles.values():
            k32.CloseHandle(h)
    return groups, group_tids


def callers(samples, name, pattern, depth=3):
    """For the samples whose leaf name matches `pattern` (a regex): {leaf: (count,
    [Counter of the first-k exe-frame chains for k = 1..depth])}. `samples` are (rip, rets)
    with rets nearest first; `name(addr)` names an address. A chain is a tuple of names."""
    rx = re.compile(pattern)
    out = {}
    for rip, rets in samples:
        leaf = name(rip)
        if not rx.search(leaf):
            continue
        n, chains = out.setdefault(leaf, (0, [collections.Counter() for _ in range(depth)]))
        frames = [name(r) for r in rets]
        # Recursion and repeated stale words: drop immediate repeats.
        frames = [f for i, f in enumerate(frames) if i == 0 or f != frames[i - 1]]
        for k in range(depth):
            chains[k][tuple(frames[:k + 1]) if frames else ("(no exe frame)",)] += 1
        out[leaf] = (n + 1, chains)
    return out


def print_callers(samples, name, pattern, top, out=sys.stdout):
    total = len(samples)
    res = callers(samples, name, pattern)
    if not res:
        print(f"  callers of /{pattern}/: no matching leaf", file=out)
        return
    for leaf, (n, chains) in sorted(res.items(), key=lambda kv: -kv[1][0]):
        print(f"  callers of {leaf}: {n} samples ({100.0 * n / total:.1f}% of the thread)",
              file=out)
        for k, ctr in enumerate(chains):
            print(f"    first {k + 1} exe frame(s), % of these samples:", file=out)
            for chain, c in ctr.most_common(min(top, 12)):
                print(f"      {100.0 * c / n:5.1f}%  {'  <-  '.join(chain)}", file=out)


def report(groups, group_tids, modules, exe_mod, exe_path, symbolizer, top, out=sys.stdout,
           callers_re=None):
    starts = [m.base for m in modules]

    def module_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < modules[i].end:
            return modules[i]
        return None

    # Symbolize every exe address once.
    exe_addrs = set()
    for samples in groups.values():
        for rip, rets in samples:
            if exe_mod and exe_mod.base <= rip < exe_mod.end:
                exe_addrs.add(rip)
            exe_addrs.update(rets)
    pref = pe_image_base(exe_path) if exe_mod and exe_path else 0
    delta = (exe_mod.base - pref) if exe_mod else 0
    syms = symbolize(symbolizer, exe_path, {a - delta for a in exe_addrs}) if exe_mod else {}

    def name(addr):
        if exe_mod and exe_mod.base <= addr < exe_mod.end:
            func, loc = syms.get(addr - delta, ("??", ""))
            if func in ("??", ""):
                return f"{exe_mod.name}+{addr - exe_mod.base:#x}"
            return f"{func} ({loc})" if loc else func
        m = module_of(addr)
        if m:
            return m.name_of(addr)
        return f"guest/unknown {addr & ~0xff:#x}"

    names = {}

    def cached(addr):
        if addr not in names:
            names[addr] = name(addr)
        return names[addr]

    for g in sorted(groups, key=lambda k: -len(groups[k])):
        samples = groups[g]
        total = len(samples)
        tids = sorted(group_tids.get(g, ()))
        tnames = ", ".join(f"{n or '?'}[{t}]" for t, n in tids[:8])
        more = f" +{len(tids) - 8}" if len(tids) > 8 else ""
        print(f"\n== {g}: {len(tids)} thread(s), {total} samples  ({tnames}{more})", file=out)
        if not total:
            continue
        leaf = collections.Counter(cached(rip) for rip, _ in samples)
        incl = collections.Counter()
        for rip, rets in samples:
            incl.update({cached(rip)} | {cached(r) for r in rets})
        print("  top leaf:", file=out)
        for n, c in leaf.most_common(top):
            print(f"    {100.0 * c / total:5.1f}%  {n}", file=out)
        print("  top inclusive (leaf or on stack):", file=out)
        for n, c in incl.most_common(top):
            print(f"    {100.0 * c / total:5.1f}%  {n}", file=out)
        if callers_re:
            print_callers(samples, cached, callers_re, top, out)


def run(target, seconds, filters, interval_ms=2.0, scan_kib=64, top=25, exe=None,
        symbolizer=None, out=sys.stdout, callers_re=None):
    pid = find_pid(target)
    proc = Process(pid)
    try:
        modules = proc.modules()
        exe_path = exe or proc.image
        exe_name = os.path.basename(proc.image).lower()
        exe_mod = next((m for m in modules if m.name.lower() == exe_name), None)
        exe_code = b""
        if exe_mod:
            # The exe's image, for the call-site check on stack words.
            exe_code = proc.read(exe_mod.base, exe_mod.size)
            if len(exe_code) < exe_mod.size:
                chunks = []
                for off in range(0, exe_mod.size, 0x1000):
                    page = proc.read(exe_mod.base + off, 0x1000)
                    chunks.append(page.ljust(0x1000, b"\0"))
                exe_code = b"".join(chunks)
        sym = find_symbolizer(symbolizer)
        if not sym:
            print("sample.py: llvm-symbolizer not found; exe frames stay as offsets",
                  file=sys.stderr)
        print(f"sampling pid {pid} ({proc.image}) for {seconds}s, filters {filters or 'all'}, "
              f"exe base {exe_mod.base if exe_mod else 0:#x}", file=out)
        groups, group_tids = sample(proc, seconds, filters, interval_ms / 1000.0,
                                    scan_kib * 1024, exe_mod, exe_code)
        for f in filters:
            groups.setdefault(f, [])
        report(groups, group_tids, modules, exe_mod, exe_path, sym, top, out, callers_re)
        return groups
    finally:
        proc.close()


# ----------------------------------------------------------------- self-test

CHILD = r"""
import ctypes, threading, time, math
k32 = ctypes.WinDLL("kernel32")
k32.SetThreadDescription.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
k32.GetCurrentThread.restype = ctypes.c_void_p
def busy():
    k32.SetThreadDescription(k32.GetCurrentThread(), "SelfTestBusy")
    x = 0.0
    while True:
        x += math.sqrt(x + 1.0)
def sleeper():
    k32.SetThreadDescription(k32.GetCurrentThread(), "SelfTestSleeper")
    while True:
        k32.Sleep(1)
threading.Thread(target=busy, daemon=True).start()
threading.Thread(target=sleeper, daemon=True).start()
print("ready", flush=True)
time.sleep(60)
"""


def self_test_callers():
    """--callers on synthetic samples: leaf filter, chain depth, repeat folding."""
    names = {1: "ntdll.dll!ZwWaitForMultipleObjects", 2: "nvoglv64.dll!vkGetInstanceProcAddr",
             3: "ntdll.dll!NtGdiDdDDIWaitForSynchronizationObjectFromCpu",
             10: "Scheduler::Wait", 11: "Scheduler::Finish", 12: "BufferCache::Download",
             20: "SotcSideCopy", 21: "BufferCache::Download"}
    samples = [(3, [10, 11, 12]), (3, [10, 10, 11, 12]), (3, [10, 11, 99]), (1, [20, 21]),
               (1, []), (2, [10])]
    name = lambda a: names.get(a, f"x{a}")
    res = callers(samples, name, "NtGdiDdDDIWait|ZwWaitForMultipleObjects")
    n3, c3 = res.get(names[3], (0, None))
    n1, c1 = res.get(names[1], (0, None))
    good = (names[2] not in res and n3 == 3 and n1 == 2
            and c3[0][("Scheduler::Wait",)] == 3
            and c3[1][("Scheduler::Wait", "Scheduler::Finish")] == 3
            and c3[2][("Scheduler::Wait", "Scheduler::Finish", "BufferCache::Download")] == 2
            and c1[0][("SotcSideCopy",)] == 1 and c1[0][("(no exe frame)",)] == 1)
    print(f"self-test: --callers on synthetic samples: {'ok' if good else 'FAIL'}")
    return good


def self_test(symbolizer=None):
    ok = self_test_callers()
    # 1. Symbolizer address convention, on a built exe if one exists.
    sym = find_symbolizer(symbolizer)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for exe in [os.path.join(root, d, "sotc-runtime.exe") for d in ("build-vk", "build")]:
        if os.path.exists(exe) and sym:
            va = pe_image_base(exe) + pe_entry_rva(exe)
            func = symbolize(sym, exe, [va]).get(va, ("??", ""))[0]
            good = "CRTStartup" in func or "main" in func.lower()
            print(f"self-test: {os.path.relpath(exe, root)} entry {va:#x} -> {func}: "
                  f"{'ok' if good else 'FAIL'}")
            ok &= good
            break
    else:
        print("self-test: no built sotc-runtime.exe or llvm-symbolizer; symbolizer check skipped")
    # 2. Sample a child Python process with two named threads.
    child = subprocess.Popen([sys.executable, "-I", "-c", CHILD], stdout=subprocess.PIPE,
                             text=True)
    try:
        if child.stdout.readline().strip() != "ready":
            print("self-test: child did not start")
            return False
        time.sleep(0.3)
        groups = run(str(child.pid), 2, ["SelfTestBusy", "SelfTestSleeper"], top=8,
                     symbolizer=symbolizer, callers_re=r"ntdll\.dll!")
        busy, sleeper = groups.get("SelfTestBusy", []), groups.get("SelfTestSleeper", [])
        good = len(busy) > 50 and len(sleeper) > 50
        print(f"\nself-test: {len(busy)} busy and {len(sleeper)} sleeper samples: "
              f"{'ok' if good else 'FAIL'}")
        ok &= good
        # The sleeper sits in a kernel wait: its leaf must be named by an ntdll export.
        proc = Process(child.pid)
        try:
            mods = proc.modules()
            starts = [m.base for m in mods]
            leaf_names = collections.Counter()
            for rip, _ in sleeper:
                i = bisect.bisect_right(starts, rip) - 1
                if i >= 0 and rip < mods[i].end:
                    leaf_names[mods[i].name_of(rip)] += 1
        finally:
            proc.close()
        top_leaf = leaf_names.most_common(1)[0][0] if leaf_names else "?"
        good = top_leaf.lower().startswith("ntdll.dll!")
        print(f"self-test: sleeper leaf {top_leaf}: {'ok' if good else 'FAIL'}")
        ok &= good
    finally:
        child.kill()
        child.wait()
    print("self-test:", "PASS" if ok else "FAIL")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("process", nargs="?", help="process name (sotc-runtime.exe) or pid")
    ap.add_argument("seconds", nargs="?", type=float, default=10.0)
    ap.add_argument("filters", nargs="*", help="thread-name substrings (default: every thread)")
    ap.add_argument("--interval", type=float, default=2.0, help="ms between rounds (default 2)")
    ap.add_argument("--scan", type=int, default=64, help="KiB of stack to scan (default 64)")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--exe", help="exe with the PDB (default: the process image)")
    ap.add_argument("--symbolizer", help="llvm-symbolizer path (default: PATH)")
    ap.add_argument("--callers", metavar="REGEX",
                    help="for leaves matching REGEX, list the first 1-3 exe frames on the stack")
    ap.add_argument("--self-test", action="store_true", help="sample a child Python process")
    args = ap.parse_args()
    if args.self_test:
        sys.exit(0 if self_test(args.symbolizer) else 1)
    if not args.process:
        ap.error("process is required")
    run(args.process, args.seconds, args.filters, args.interval, args.scan, args.top, args.exe,
        args.symbolizer, callers_re=args.callers)


if __name__ == "__main__":
    main()
