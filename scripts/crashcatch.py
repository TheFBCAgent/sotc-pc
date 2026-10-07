#!/usr/bin/env python3
"""Run sotc-runtime under a minimal Win32 debugger and report the exceptions it hits.

The runtime's own fault handler can't report faults whose exception frame
can't be pushed (e.g. a blown fiber stack), because the process dies before
any handler runs. A debugger sees these as first-chance events. This prints
each access violation / illegal instruction with registers, the module+offset
of rip, and stack words that point into the guest image.

Usage: python scripts/crashcatch.py [--seconds N] [--image build/game/boot.img]
Needs SOTC_GAME_DIR. Windows only.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 0x2
EXCEPTION_DEBUG_EVENT = 1
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
OUTPUT_DEBUG_STRING_EVENT = 8
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
CONTEXT_FULL = 0x10000B
INTERESTING = {0xC0000005: "access violation", 0xC000001D: "illegal instruction",
               0xC00000FD: "stack overflow", 0xC0000094: "divide by zero",
               0xC0000409: "fast fail / stack buffer overrun", 0xC0000374: "heap corruption"}


class EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [("ExceptionCode", wt.DWORD), ("ExceptionFlags", wt.DWORD),
                ("ExceptionRecord", ctypes.c_void_p), ("ExceptionAddress", ctypes.c_void_p),
                ("NumberParameters", wt.DWORD), ("ExceptionInformation", ctypes.c_uint64 * 15)]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wt.DWORD)]


class EXIT_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("dwExitCode", wt.DWORD)]


class OUTPUT_DEBUG_STRING_INFO(ctypes.Structure):
    _fields_ = [("lpDebugStringData", ctypes.c_void_p), ("fUnicode", wt.WORD),
                ("nDebugStringLength", wt.WORD)]


class CREATE_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wt.HANDLE), ("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("lpBaseOfImage", ctypes.c_void_p), ("dwDebugInfoFileOffset", wt.DWORD),
                ("nDebugInfoSize", wt.DWORD), ("lpThreadLocalBase", ctypes.c_void_p),
                ("lpStartAddress", ctypes.c_void_p), ("lpImageName", ctypes.c_void_p),
                ("fUnicode", wt.WORD)]


class LOAD_DLL_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wt.HANDLE), ("lpBaseOfDll", ctypes.c_void_p),
                ("dwDebugInfoFileOffset", wt.DWORD), ("nDebugInfoSize", wt.DWORD),
                ("lpImageName", ctypes.c_void_p), ("fUnicode", wt.WORD)]


class _U(ctypes.Union):
    _fields_ = [("Exception", EXCEPTION_DEBUG_INFO), ("ExitProcess", EXIT_PROCESS_DEBUG_INFO),
                ("DebugString", OUTPUT_DEBUG_STRING_INFO),
                ("CreateProcessInfo", CREATE_PROCESS_DEBUG_INFO),
                ("LoadDll", LOAD_DLL_DEBUG_INFO), ("pad", ctypes.c_byte * 160)]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD),
                ("dwThreadId", wt.DWORD), ("u", _U)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_uint64), ("High", ctypes.c_int64)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P1Home", ctypes.c_uint64), ("P2Home", ctypes.c_uint64),
                ("P3Home", ctypes.c_uint64), ("P4Home", ctypes.c_uint64),
                ("P5Home", ctypes.c_uint64), ("P6Home", ctypes.c_uint64),
                ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD),
                ("SegGs", wt.WORD), ("SegSs", wt.WORD), ("EFlags", wt.DWORD)] + \
               [(r, ctypes.c_uint64) for r in
                ("Dr0", "Dr1", "Dr2", "Dr3", "Dr6", "Dr7", "Rax", "Rcx", "Rdx", "Rbx", "Rsp",
                 "Rbp", "Rsi", "Rdi", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
                 "Rip")] + \
               [("FltSave", ctypes.c_byte * 512), ("VectorRegister", M128A * 26),
                ("VectorControl", ctypes.c_uint64), ("DebugControl", ctypes.c_uint64),
                ("LastBranchToRip", ctypes.c_uint64), ("LastBranchFromRip", ctypes.c_uint64),
                ("LastExceptionToRip", ctypes.c_uint64),
                ("LastExceptionFromRip", ctypes.c_uint64)]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
                ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
                ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE),
                ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


k32.OpenThread.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.GetFinalPathNameByHandleW.argtypes = [wt.HANDLE, wt.LPWSTR, wt.DWORD, wt.DWORD]


def read(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t()
    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)):
        return b""
    return buf.raw[: n.value]


def load_guest_modules():
    """Guest module ranges from boot.txt ("module NAME base 0x... size 0x..." lines), if present."""
    mods = []
    txt = ROOT / "build" / "game" / "boot.txt"
    if txt.exists():
        for line in txt.read_text(errors="replace").splitlines():
            m = re.match(r"\s*module\s+(\S+).*?base\s+(0x[0-9a-fA-F]+).*?size\s+(0x[0-9a-fA-F]+)", line)
            if m:
                mods.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return mods


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--image", default=str(ROOT / "build" / "game" / "boot.img"))
    ap.add_argument("--exe", default=str(ROOT / "build" / "sotc-runtime.exe"),
                    help="runtime executable (e.g. build-vk/sotc-runtime.exe)")
    ap.add_argument("--all", action="store_true", help="report every first-chance exception")
    ap.add_argument("--dump", action="append", default=[], metavar="REG[+OFF]:LEN",
                    help="hex-dump memory at a register (e.g. r14:0x80) when reporting")
    args = ap.parse_args()
    if os.name != "nt":
        sys.exit("crashcatch: Windows only")
    exe = Path(args.exe).resolve()
    os.environ.setdefault("SOTC_USER_DIR", str(ROOT / "build" / "user"))
    log = open(ROOT / "build" / "game" / "crashcatch.log", "wb")
    # Inherit a file handle for the child's stdout/stderr so the pipe never fills.
    import msvcrt
    h_log = msvcrt.get_osfhandle(log.fileno())
    k32.SetHandleInformation(wt.HANDLE(h_log), 1, 1)

    si = STARTUPINFO(cb=ctypes.sizeof(STARTUPINFO))
    si.dwFlags = 0x100  # STARTF_USESTDHANDLES
    si.hStdOutput = si.hStdError = h_log
    pi = PROCESS_INFORMATION()
    cmd = f'"{exe}" "{args.image}"'
    if not k32.CreateProcessW(None, ctypes.create_unicode_buffer(cmd), None, None, True,
                              DEBUG_ONLY_THIS_PROCESS, None, str(ROOT), ctypes.byref(si),
                              ctypes.byref(pi)):
        sys.exit(f"crashcatch: CreateProcess failed ({ctypes.get_last_error()})")

    dlls = {}
    exe_base = None
    deadline = time.time() + args.seconds
    ev = DEBUG_EVENT()
    reported = 0
    while True:
        if time.time() > deadline:
            print("crashcatch: timeout; still running")
            k32.TerminateProcess(pi.hProcess, 0)
        if not k32.WaitForDebugEvent(ctypes.byref(ev), 500):
            continue
        status = DBG_CONTINUE
        code = ev.dwDebugEventCode
        if code == CREATE_PROCESS_DEBUG_EVENT:
            exe_base = ev.u.CreateProcessInfo.lpBaseOfImage
        elif code == LOAD_DLL_DEBUG_EVENT:
            info = ev.u.LoadDll
            name = ctypes.create_unicode_buffer(520)
            if info.hFile and k32.GetFinalPathNameByHandleW(info.hFile, name, 520, 0):
                dlls[info.lpBaseOfDll] = os.path.basename(name.value)
        elif code == EXCEPTION_DEBUG_EVENT:
            er = ev.u.Exception.ExceptionRecord
            first = ev.u.Exception.dwFirstChance
            exc = er.ExceptionCode
            if exc == 0x80000003 and reported == 0 and first:
                pass  # initial loader breakpoint
            else:
                status = DBG_EXCEPTION_NOT_HANDLED
                if exc in INTERESTING or args.all:
                    reported += 1
                    report(pi.hProcess, ev.dwThreadId, er, first, exe_base, dlls, args.dump)
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            print(f"crashcatch: process exited with {ev.u.ExitProcess.dwExitCode:#x}")
            k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE)
            break
        k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status)
    log.close()
    print(f"crashcatch: runtime log in {ROOT / 'build' / 'game' / 'crashcatch.log'}")


def describe(addr, exe_base, dlls):
    if exe_base and exe_base <= addr < exe_base + 0x2000000:
        return f"sotc-runtime.exe+{addr - exe_base:#x}"
    best = None
    for base, name in dlls.items():
        if base <= addr and (best is None or base > best[0]):
            best = (base, name)
    if best and addr - best[0] < 0x4000000:
        return f"{best[1]}+{addr - best[0]:#x}"
    if 0x10000000 <= addr < 0x40000000:
        return f"guest image+{addr - 0x10000000:#x}"
    return f"{addr:#x}"


def report(hproc, tid, er, first, exe_base, dlls, dumps=()):
    name = INTERESTING.get(er.ExceptionCode, "exception")
    print(f"\n=== {name} {er.ExceptionCode:#x} ({'first' if first else 'SECOND'} chance) "
          f"thread {tid}")
    print(f"  at {describe(er.ExceptionAddress or 0, exe_base, dlls)}")
    if er.ExceptionCode == 0xC0000005 and er.NumberParameters >= 2:
        kind = {0: "read", 1: "write", 8: "execute"}.get(er.ExceptionInformation[0], "?")
        addr = er.ExceptionInformation[1]
        print(f"  {kind} of {addr:#x}")
        mbi = MEMORY_BASIC_INFORMATION()
        if k32.VirtualQueryEx(hproc, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            print(f"  page: base {mbi.BaseAddress or 0:#x} alloc {mbi.AllocationBase or 0:#x} "
                  f"size {mbi.RegionSize:#x} state {mbi.State:#x} protect {mbi.Protect:#x} "
                  f"type {mbi.Type:#x}")
    ht = k32.OpenThread(0x001F03FF, False, tid)
    ctx = CONTEXT()
    ctx.ContextFlags = CONTEXT_FULL
    if ht and k32.GetThreadContext(ht, ctypes.byref(ctx)):
        regs = ["Rax", "Rbx", "Rcx", "Rdx", "Rsi", "Rdi", "Rbp", "Rsp", "R8", "R9", "R10", "R11",
                "R12", "R13", "R14", "R15", "Rip"]
        print("  " + " ".join(f"{r.lower()}={getattr(ctx, r):#x}" for r in regs))
        # TEB stack bounds via NtCurrentTeb isn't available remotely without NtQueryInformationThread;
        # print raw stack words that look like code addresses instead.
        stack = read(hproc, ctx.Rsp, 8 * 512)
        shown = 0
        for i in range(0, len(stack), 8):
            v = int.from_bytes(stack[i:i + 8], "little")
            d = describe(v, exe_base, dlls)
            if not d.startswith("0x") and shown < 24:
                print(f"  [rsp+{i:#05x}] {d}")
                shown += 1
        if not stack:
            print("  (stack unreadable)")
        for spec in dumps:
            where, _, length = spec.partition(":")
            m = re.fullmatch(r"(\w+)([+-]0x[0-9a-fA-F]+|[+-]\d+)?", where)
            base = getattr(ctx, m.group(1).capitalize()) + (int(m.group(2), 0) if m.group(2) else 0)
            data = read(hproc, base, int(length or "0x80", 0))
            print(f"  dump {spec} @ {base:#x}:")
            for i in range(0, len(data), 16):
                words = " ".join(f"{int.from_bytes(data[j:j + 8], 'little'):016x}"
                                 for j in range(i, min(i + 16, len(data)), 8))
                print(f"    +{i:#05x}: {words}")
    if ht:
        k32.CloseHandle(ht)


if __name__ == "__main__":
    main()
