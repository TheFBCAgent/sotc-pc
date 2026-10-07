# SPDX-License-Identifier: GPL-2.0-or-later
"""Find the instructions the runtime must patch, by disassembling real code only.

Executable PS4 segments also hold read-only data, so a linear sweep decodes
strings as bogus instructions (an ASCII 'd' is the fs segment prefix). We take
function ranges from .eh_frame (via the PT_GNU_EH_FRAME header table) and
disassemble each function with capstone.
"""

from __future__ import annotations

import struct
from collections import Counter
from dataclasses import dataclass, field

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone import x86 as csx86

import elfscan

# Patch kinds, shared with runtime/include/bootimg.h.
PATCH_FS = 1          # instruction with an fs segment prefix
PATCH_EXTRQ_IMM = 2   # extrq xmm, imm8 (len), imm8 (idx)
PATCH_EXTRQ_REG = 3   # extrq xmm, xmm
PATCH_INSERTQ_IMM = 4  # insertq xmm, xmm, imm8 (len), imm8 (idx)
PATCH_INSERTQ_REG = 5  # insertq xmm, xmm

# For PATCH_FS, operand forms:
FS_ABS = 1    # [disp32] only, no base or index: rewritable in place on Windows
FS_OTHER = 2  # base and/or index register: needs a trampoline on Windows


@dataclass
class PatchSite:
    vaddr: int
    kind: int
    length: int
    # Kind-specific bytes, see bootimg.h:
    #   FS:      a=prefix offset, b=disp32 offset (0xFF if none), c=form, d=0, disp=displacement
    #   EXTRQ/INSERTQ: a=dst xmm, b=src xmm (0xFF for imm forms), c=len, d=idx
    a: int = 0
    b: int = 0
    c: int = 0
    d: int = 0
    disp: int = 0
    text: str = ""


class AddressMap:
    """Translate virtual addresses to file offsets using PT_LOAD segments."""

    def __init__(self, info: elfscan.ElfInfo):
        self.data = info.data
        self.loads = [s for s in info.segments if s.type in (elfscan.PT_LOAD, elfscan.PT_SCE_RELRO)]

    def offset(self, vaddr: int) -> int:
        for s in self.loads:
            if s.vaddr <= vaddr < s.vaddr + s.filesz:
                return s.offset + vaddr - s.vaddr
        raise ValueError(f"vaddr {vaddr:#x} is not backed by file data")

    def read(self, vaddr: int, size: int) -> bytes:
        off = self.offset(vaddr)
        return self.data[off:off + size]


def _uleb(buf: bytes, pos: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, pos


def _read_encoded(amap: AddressMap, vaddr: int, enc: int, datarel_base: int) -> tuple[int, int]:
    """Read a DW_EH_PE-encoded pointer at vaddr. Returns (value, size)."""
    if enc == 0xFF:  # DW_EH_PE_omit
        return 0, 0
    fmt = enc & 0x0F
    size, code = {0x00: (8, "<Q"), 0x01: (None, None), 0x02: (2, "<H"), 0x03: (4, "<I"),
                  0x04: (8, "<Q"), 0x0A: (2, "<h"), 0x0B: (4, "<i"), 0x0C: (8, "<q")}[fmt]
    if size is None:
        raise ValueError("uleb128 pointer encoding not supported")
    value = struct.unpack(code, amap.read(vaddr, size))[0]
    app = enc & 0x70
    if app == 0x10:    # pcrel
        value += vaddr
    elif app == 0x30:  # datarel
        value += datarel_base
    elif app != 0x00:
        raise ValueError(f"unsupported pointer application {app:#x}")
    return value & 0xFFFFFFFFFFFFFFFF, size


def eh_frame_info(info: elfscan.ElfInfo, module_size: int) -> tuple[int, int, int, int]:
    """(eh_frame_hdr vaddr, hdr size, eh_frame vaddr, eh_frame size), as shadPS4's
    Module::LoadModuleToMemory computes them; all zero if absent."""
    hdr = info.segment(elfscan.PT_GNU_EH_FRAME)
    if hdr is None:
        return 0, 0, 0, 0
    amap = AddressMap(info)
    ptr_enc = amap.read(hdr.vaddr + 1, 1)[0]
    eh_frame, _ = _read_encoded(amap, hdr.vaddr + 4, ptr_enc, hdr.vaddr)
    size = hdr.vaddr - eh_frame if hdr.vaddr > eh_frame else module_size - hdr.vaddr
    return hdr.vaddr, hdr.memsz, eh_frame, size


def function_ranges(info: elfscan.ElfInfo) -> list[tuple[int, int]]:
    """(start, end) of every function described in .eh_frame, sorted."""
    hdr = info.segment(elfscan.PT_GNU_EH_FRAME)
    if hdr is None:
        return []
    amap = AddressMap(info)
    base = hdr.vaddr
    version, ptr_enc, count_enc, table_enc = amap.read(base, 4)
    if version != 1:
        raise ValueError(f"unknown .eh_frame_hdr version {version}")
    pos = base + 4
    _, size = _read_encoded(amap, pos, ptr_enc, base)
    pos += size
    count, size = _read_encoded(amap, pos, count_enc, base)
    pos += size

    cie_cache: dict[int, int] = {}
    ranges = []
    for _ in range(count):
        start, size = _read_encoded(amap, pos, table_enc, base)
        pos += size
        fde, size = _read_encoded(amap, pos, table_enc, base)
        pos += size
        try:
            pc_begin, pc_range = _parse_fde(amap, fde, cie_cache)
        except (ValueError, KeyError, struct.error, IndexError):
            ranges.append((start, None))
            continue
        ranges.append((pc_begin, pc_begin + pc_range))

    ranges.sort(key=lambda r: r[0])
    # Fill unknown lengths with the next function start.
    out = []
    for i, (start, end) in enumerate(ranges):
        if end is None:
            end = ranges[i + 1][0] if i + 1 < len(ranges) else start
        if end > start:
            out.append((start, end))
    return out


def _parse_fde(amap: AddressMap, fde: int, cie_cache: dict[int, int]) -> tuple[int, int]:
    length = struct.unpack("<I", amap.read(fde, 4))[0]
    pos = fde + 4
    if length == 0xFFFFFFFF:
        pos += 8
    cie_ptr = struct.unpack("<I", amap.read(pos, 4))[0]
    cie = pos - cie_ptr
    pos += 4
    if cie not in cie_cache:
        cie_cache[cie] = _parse_cie_fde_encoding(amap, cie)
    enc = cie_cache[cie]
    pc_begin, size = _read_encoded(amap, pos, enc, 0)
    pos += size
    pc_range, _ = _read_encoded(amap, pos, enc & 0x0F, 0)
    return pc_begin, pc_range


def _parse_cie_fde_encoding(amap: AddressMap, cie: int) -> int:
    length = struct.unpack("<I", amap.read(cie, 4))[0]
    buf = amap.read(cie, min(length + 4, 256))
    pos = 4 + 4  # length + CIE id
    version = buf[pos]
    pos += 1
    end = buf.index(b"\0", pos)
    aug = buf[pos:end].decode()
    pos = end + 1
    if "eh" in aug:
        pos += 8
    _, pos = _uleb(buf, pos)        # code alignment
    _, pos = _uleb(buf, pos)        # data alignment (sleb, same length)
    if version == 1:
        pos += 1
    else:
        _, pos = _uleb(buf, pos)    # return register
    enc = 0x00  # absptr
    if aug.startswith("z"):
        _, pos = _uleb(buf, pos)    # augmentation data length
        for ch in aug[1:]:
            if ch == "R":
                enc = buf[pos]
                pos += 1
            elif ch == "L":
                pos += 1
            elif ch == "P":
                penc = buf[pos]
                pos += 1
                pos += {0x00: 8, 0x02: 2, 0x03: 4, 0x04: 8, 0x0A: 2, 0x0B: 4, 0x0C: 8}[penc & 0x0F]
            elif ch in "SB":
                pass
            else:
                raise ValueError(f"unknown CIE augmentation {aug!r}")
    return enc


def _xmm(insn, reg) -> int:
    name = insn.reg_name(reg)
    if not name.startswith("xmm"):
        raise ValueError(f"unexpected register {name}")
    return int(name[3:])


def scan(info: elfscan.ElfInfo) -> tuple[list[PatchSite], int]:
    """Return (patch sites, number of functions scanned)."""
    amap = AddressMap(info)
    lite = Cs(CS_ARCH_X86, CS_MODE_64)
    full = Cs(CS_ARCH_X86, CS_MODE_64)
    full.detail = True
    sites: list[PatchSite] = []
    funcs = function_ranges(info)

    for start, end in funcs:
        try:
            code = amap.read(start, end - start)
        except ValueError:
            continue
        for addr, size, mnem, ops in lite.disasm_lite(code, start):
            is_sse4a = mnem in ("extrq", "insertq")
            if not is_sse4a and "fs:" not in ops:
                continue
            raw = code[addr - start:addr - start + size]
            insn = next(full.disasm(raw, addr))
            if is_sse4a:
                sites.append(_sse4a_site(insn))
            else:
                sites.append(_fs_site(insn, raw))
    return sites, len(funcs)


def _fs_site(insn, raw: bytes) -> PatchSite:
    prefix_off = raw.index(0x64)
    mem = next((op.mem for op in insn.operands if op.type == csx86.X86_OP_MEM), None)
    disp_off = insn.disp_offset if insn.disp_size == 4 else 0xFF
    absolute = mem is not None and mem.base == 0 and mem.index == 0 and disp_off != 0xFF
    return PatchSite(
        vaddr=insn.address, kind=PATCH_FS, length=insn.size,
        a=prefix_off, b=disp_off, c=FS_ABS if absolute else FS_OTHER,
        disp=mem.disp if mem is not None else 0,
        text=f"{insn.mnemonic} {insn.op_str}",
    )


def _sse4a_site(insn) -> PatchSite:
    ops = insn.operands
    regs = [o for o in ops if o.type == csx86.X86_OP_REG]
    imms = [o.imm & 0xFF for o in ops if o.type == csx86.X86_OP_IMM]
    text = f"{insn.mnemonic} {insn.op_str}"
    if insn.mnemonic == "extrq":
        if imms:
            return PatchSite(insn.address, PATCH_EXTRQ_IMM, insn.size, _xmm(insn, regs[0].reg), 0xFF,
                             imms[0], imms[1], text=text)
        return PatchSite(insn.address, PATCH_EXTRQ_REG, insn.size, _xmm(insn, regs[0].reg),
                         _xmm(insn, regs[1].reg), text=text)
    if imms:
        return PatchSite(insn.address, PATCH_INSERTQ_IMM, insn.size, _xmm(insn, regs[0].reg),
                         _xmm(insn, regs[1].reg), imms[0], imms[1], text=text)
    return PatchSite(insn.address, PATCH_INSERTQ_REG, insn.size, _xmm(insn, regs[0].reg),
                     _xmm(insn, regs[1].reg), text=text)


# ---------------------------------------------------------------------------
# Red-zone analysis and protection (workstream S, docs/m3/s.md)
#
# A SysV leaf function may keep up to 128 bytes of live data below rsp. On
# Windows the kernel builds the exception frame below rsp; with the AVX state
# in use its XSAVE area reaches up to rsp-0x10, so a fault taken while such
# data is live destroys it (runtime/tests/test_fault.c proves it). M3's GPU
# page tracking turns guest memory accesses into faults, so every memory
# access that runs while the red zone is live can corrupt it.
#
# The model follows shadPS4's static red-zone protection (src/core/cpu_patches.cpp:
# DecodeCodeInstruction, AnalyzeRedZoneLiveness, PatchSegmentStatically;
# GPL-2.0-or-later, Copyright 2024 shadPS4 Emulator Project): a 128-bit
# liveness mask relative to rsp, translated across known rsp adjustments and
# solved backwards over the function's control flow. Functions with indirect
# jumps, unknown rsp changes or an escaping red-zone address are treated as
# live everywhere. Each memory access through a non-stack pointer that runs
# with a live red zone moves into a trampoline that runs it with rsp lowered:
#     lea rsp,[rsp-128]; <access>; lea rsp,[rsp+128]; <displaced code>; jmp back
# The original bytes become `jmp trampoline` plus int3 padding. scan() does not
# use any of this; sotcconv calls redzone_patch() (see docs/m3/s.patch).
# ---------------------------------------------------------------------------

RZ_SIZE = 128
RZ_ALL = (1 << RZ_SIZE) - 1
JMP_SIZE = 5
LEA_DOWN = bytes.fromhex("488d642480")      # lea rsp, [rsp-0x80]
LEA_UP = bytes.fromhex("488da42480000000")  # lea rsp, [rsp+0x80]
_NOT_ACCESSES = {"lea", "nop", "prefetch", "prefetchw", "prefetcht0", "prefetcht1",
                 "prefetcht2", "prefetchnta"}
_SP_REGS = {"rsp", "esp", "sp", "spl"}


@dataclass
class RzInsn:
    address: int                # module vaddr
    size: int
    mnemonic: str
    raw: bytes
    jump: bool = False          # jmp or jcc
    cond: bool = False          # jcc, loop, jrcxz
    call: bool = False
    ret: bool = False
    terminator: bool = False    # no fall-through
    target: int | None = None   # direct branch/call target
    indirect: bool = False      # jmp/call through a register or memory
    rip_disp_off: int = -1      # offset of a rip-relative disp32, -1 if none
    uses_sp: bool = False       # explicit rsp operand
    sp_delta: int | None = None  # known rsp adjustment
    sp_unknown: bool = False    # writes rsp in an unmodelled way
    rz_use: int = 0
    rz_def: int = 0
    rz_unmodeled: bool = False  # takes the address of the red zone
    accesses: bool = False      # memory access through a non-stack pointer
    is_write: bool = False      # ... that may write
    ymm: bool = False
    fs: bool = False            # fs-prefixed (PATCH_FS site)
    live: int = 0


@dataclass
class RzStats:
    functions: int = 0
    rz_functions: int = 0           # functions with a red-zone access
    rz_leaf: int = 0                # ... without calls
    rz_conservative: int = 0        # ... analysed as live everywhere
    risky_functions: int = 0        # functions with at least one risky site
    risky_sites: int = 0            # non-stack memory accesses with a live red zone
    risky_writes: int = 0           # ... that may write
    risky_ymm_functions: int = 0    # risky functions that use ymm registers
    patched: int = 0                # risky sites moved into trampolines
    spans: int = 0                  # jmp patches written
    unpatchable: Counter = field(default_factory=Counter)
    unprotected: list = field(default_factory=list)  # module vaddrs of risky sites left as is
    trampoline_bytes: int = 0

    def add(self, other: "RzStats") -> None:
        for name in ("functions", "rz_functions", "rz_leaf", "rz_conservative", "risky_functions",
                     "risky_sites", "risky_writes", "risky_ymm_functions", "patched", "spans",
                     "trampoline_bytes"):
            setattr(self, name, getattr(self, name) + getattr(other, name))
        self.unpatchable.update(other.unpatchable)
        self.unprotected += other.unprotected

    def text(self) -> str:
        return (f"{self.functions} functions; {self.rz_functions} use the red zone "
                f"({self.rz_leaf} leaf, {self.rz_conservative} analysed conservatively); "
                f"{self.risky_functions} have {self.risky_sites} non-stack memory accesses with a "
                f"live red zone ({self.risky_writes} may write; {self.risky_ymm_functions} of "
                f"those functions use ymm); protected {self.patched} in {self.spans} trampolines "
                f"({self.trampoline_bytes} bytes), not protected "
                f"{sum(self.unpatchable.values())} {dict(sorted(self.unpatchable.items()))}")


def _is_sp(insn, reg: int) -> bool:
    return reg != 0 and insn.reg_name(reg) in _SP_REGS


def _rz_decode(insn) -> RzInsn:
    groups = set(insn.groups)
    m = insn.mnemonic
    d = RzInsn(insn.address, insn.size, m, bytes(insn.bytes))
    d.jump = csx86.X86_GRP_JUMP in groups
    d.call = csx86.X86_GRP_CALL in groups
    d.ret = csx86.X86_GRP_RET in groups or m in ("iretq", "iretd", "sysret", "sysretq")
    d.cond = d.jump and m not in ("jmp", "ljmp")
    d.terminator = d.ret or (d.jump and not d.cond) or m in ("ud2", "hlt")
    d.fs = 0x64 in insn.prefix
    d.ymm = "ymm" in insn.op_str
    ops = insn.operands
    if (d.jump or d.call) and ops:
        if ops[0].type == csx86.X86_OP_IMM:
            d.target = ops[0].imm
        else:
            d.indirect = True

    for i, op in enumerate(ops):
        if op.type == csx86.X86_OP_REG:
            d.uses_sp |= _is_sp(insn, op.reg)
            continue
        if op.type != csx86.X86_OP_MEM:
            continue
        mem = op.mem
        if insn.reg_name(mem.base) == "rip":
            d.rip_disp_off = insn.disp_offset
            continue
        stack = _is_sp(insn, mem.base) or _is_sp(insn, mem.index)
        d.uses_sp |= stack
        if not stack:
            if m not in _NOT_ACCESSES and not d.fs:
                d.accesses = True
                # capstone's access flags are unreliable for some AVX stores.
                d.is_write |= bool(op.access & 2) or op.access == 0 or (i == 0 and len(ops) > 1)
            continue
        if mem.index != 0 or mem.disp >= 0 or m == "lea":
            continue
        start = max(mem.disp, -RZ_SIZE)
        end = min(mem.disp + max(op.size, 1), 0)
        if end <= start:
            continue
        bits = ((1 << (end - start)) - 1) << (start + RZ_SIZE)
        # A use unless known to be a pure write: being wrong errs towards live.
        if op.access != 2:
            d.rz_use |= bits
        else:
            d.rz_def |= bits

    _, written = insn.regs_access()
    if any(_is_sp(insn, r) for r in written) and not (d.call or d.ret):
        if m in ("push", "pushfq", "pushf"):
            d.sp_delta = -8
        elif m in ("pop", "popfq", "popf"):
            d.sp_delta = 8
        elif (m in ("add", "sub") and len(ops) == 2 and ops[0].type == csx86.X86_OP_REG
              and _is_sp(insn, ops[0].reg) and ops[1].type == csx86.X86_OP_IMM):
            d.sp_delta = ops[1].imm if m == "add" else -ops[1].imm
        elif (m == "lea" and _is_sp(insn, ops[0].reg) and _is_sp(insn, ops[1].mem.base)
              and ops[1].mem.index == 0):
            d.sp_delta = ops[1].mem.disp
        else:
            d.sp_unknown = True  # mov rsp, rbp; leave; and rsp, -N; ...
    if m == "lea" and d.sp_delta is None and any(
            o.type == csx86.X86_OP_MEM and _is_sp(insn, o.mem.base) and o.mem.disp < 0
            for o in ops):
        d.rz_unmodeled = True
    return d


def _translate(mask: int, delta: int) -> int:
    """Red-zone liveness after an instruction that adds `delta` to rsp, seen
    from before it: the byte at rsp_after+o is at rsp_before+o+delta."""
    if delta == 0 or not mask:
        return mask
    return (mask << delta if delta > 0 else mask >> -delta) & RZ_ALL


def _rz_liveness(insns: list[RzInsn], conservative: bool) -> None:
    if conservative:
        for d in insns:
            d.live = RZ_ALL
        return
    index = {d.address: i for i, d in enumerate(insns)}
    live_in = [0] * len(insns)
    changed = True
    while changed:
        changed = False
        for i in range(len(insns) - 1, -1, -1):
            d = insns[i]
            out = 0
            if not d.terminator and i + 1 < len(insns):
                out |= live_in[i + 1]
            if d.jump and d.target is not None and d.target in index:
                out |= live_in[index[d.target]]
            if d.sp_delta is not None:
                out = _translate(out, d.sp_delta)
            new = d.rz_use | (out & ~d.rz_def & RZ_ALL)
            if new != live_in[i]:
                live_in[i] = new
                changed = True
    for d, live in zip(insns, live_in):
        d.live = live


def _relocate(d: RzInsn, src: int, dst: int) -> bytes | None:
    """Re-encode `d`, located at image offset `src`, to run at image offset `dst`."""
    raw = bytearray(d.raw)
    if d.target is not None:
        target = src + (d.target - d.address)
        if d.mnemonic == "jmp":
            opcode, length = b"\xe9", 5
        elif d.call:
            opcode, length = b"\xe8", 5
        elif 0x70 <= raw[0] <= 0x7F:
            opcode, length = bytes([0x0F, 0x80 | (raw[0] & 0x0F)]), 6
        elif raw[0] == 0x0F and 0x80 <= raw[1] <= 0x8F:
            opcode, length = bytes(raw[:2]), 6
        else:
            return None  # loop, jrcxz, prefixed branches
        rel = target - (dst + length)
        return opcode + struct.pack("<i", rel) if -2**31 <= rel < 2**31 else None
    if d.rip_disp_off >= 0:
        target = src + d.size + struct.unpack_from("<i", raw, d.rip_disp_off)[0]
        rel = target - (dst + d.size)
        if not -2**31 <= rel < 2**31:
            return None
        struct.pack_into("<i", raw, d.rip_disp_off, rel)
    return bytes(raw)


def _span(insns: list[RzInsn], i: int, targets: set[int], avoid: set[int], mod_base: int,
          has_indirect_jump: bool, first: int) -> list[int] | str:
    """Indices of the instructions to move for the site insns[i], or why it can't be moved.
    The span starts at i, or up to 4 instructions earlier (not before `first`) when a
    later instruction is a branch target."""
    d = insns[i]
    if d.uses_sp or d.sp_delta is not None or d.sp_unknown:
        return "stack-dependent"
    if d.call or d.jump or d.ret:
        return "control-flow"
    problem = ""
    for s in range(i, max(first, i - 4) - 1, -1):
        span = _span_from(insns, s, i, targets, avoid, mod_base, has_indirect_jump)
        if not isinstance(span, str):
            return span
        problem = problem or span
        if s < i and (insns[s].terminator or insns[s].jump or insns[s].call):
            break
    return problem


def _span_from(insns, s, i, targets, avoid, mod_base, has_indirect_jump) -> list[int] | str:
    span, size, k = [], 0, s
    while size < JMP_SIZE or k <= i:
        if k >= len(insns):
            return "function-end"
        e = insns[k]
        if k != s and (e.address in targets or has_indirect_jump):
            # A jump into the middle of the patch would land on int3 padding;
            # with jump tables the targets are unknown.
            return "branch-target" if e.address in targets else "indirect-jumps"
        if e.fs:
            return "fs-site"
        # No calls: a C++ exception unwinding through a trampoline (no FDE) would terminate.
        if e.indirect or e.call or (e.cond and e.raw[0] in (0xE0, 0xE1, 0xE2, 0xE3)):
            return "control-flow"
        if k < i and (e.jump or e.call or e.terminator or e.sp_unknown):
            return "control-flow"
        if any(mod_base + a in avoid for a in range(e.address, e.address + e.size)):
            return "relocation"
        span.append(k)
        size += e.size
        if e.terminator and (size < JMP_SIZE or k < i):
            return "function-end"
        k += 1
    return span


def redzone_patch(info: elfscan.ElfInfo, mod_base: int, tramp_base: int,
                  avoid: set[int] | None = None):
    """Analyse one module and build its red-zone trampolines.

    mod_base: image offset of the module. tramp_base: image offset where this
    module's trampolines go (within 2 GiB of the module). avoid: image offsets
    that must not change (relocated words, other patch sites).
    Returns (edits, trampolines, stats): edits are (image offset, bytes) to
    write over the module's code; trampolines is the code for tramp_base."""
    amap = AddressMap(info)
    lite = Cs(CS_ARCH_X86, CS_MODE_64)
    full = Cs(CS_ARCH_X86, CS_MODE_64)
    full.detail = True
    avoid = avoid or set()
    stats = RzStats()
    edits: list[tuple[int, bytes]] = []
    tramp = bytearray()

    for start, end in function_ranges(info):
        stats.functions += 1
        try:
            code = amap.read(start, end - start)
        except ValueError:
            continue
        # Cheap filter: some operand addresses memory below rsp.
        if b"\x24" not in code:  # rsp-based operands always have a SIB byte
            continue
        if not any("[rsp - " in ops for _, _, _, ops in lite.disasm_lite(code, start)):
            continue
        insns = [_rz_decode(x) for x in full.disasm(code, start)]
        complete = bool(insns) and insns[-1].address + insns[-1].size >= end
        if not any(d.rz_use or d.rz_def or d.rz_unmodeled for d in insns):
            continue
        stats.rz_functions += 1
        stats.rz_leaf += not any(d.call for d in insns)
        addrs = {d.address for d in insns}
        targets = {d.target for d in insns if d.jump and d.target is not None}
        has_indirect_jump = any(d.jump and d.indirect for d in insns)
        conservative = (not complete or has_indirect_jump
                        or any(d.rz_unmodeled or d.sp_unknown for d in insns)
                        or any(start <= t < end and t not in addrs for t in targets))
        stats.rz_conservative += conservative
        _rz_liveness(insns, conservative)

        risky = [i for i, d in enumerate(insns) if d.accesses and d.live]
        if not risky:
            continue
        stats.risky_functions += 1
        stats.risky_sites += len(risky)
        stats.risky_writes += sum(insns[i].is_write for i in risky)
        stats.risky_ymm_functions += any(d.ymm for d in insns)

        risky_set = set(risky)
        covered_until, first = -1, 0
        for i in risky:
            d = insns[i]
            if d.address < covered_until:
                continue  # already moved, and wrapped, by the previous span
            span = _span(insns, i, targets, avoid, mod_base, has_indirect_jump, first)
            if isinstance(span, str):
                stats.unpatchable[span] += 1
                stats.unprotected.append(d.address)
                continue
            at = tramp_base + len(tramp)
            body = bytearray()
            for k in span:
                e = insns[k]
                if k in risky_set:
                    body += LEA_DOWN
                enc = _relocate(e, mod_base + e.address, at + len(body))
                if enc is None:
                    break
                body += enc
                if k in risky_set:
                    body += LEA_UP
            else:
                last = insns[span[-1]]
                cont = last.address + last.size
                if not last.terminator:
                    rel = (mod_base + cont) - (at + len(body) + 5)
                    body += b"\xe9" + struct.pack("<i", rel)
                head = insns[span[0]].address
                site = mod_base + head
                rel = at - (site + 5)
                edits.append((site, b"\xe9" + struct.pack("<i", rel) + b"\xcc" * (cont - head - 5)))
                tramp += body
                stats.patched += sum(k in risky_set for k in span if k >= i)
                stats.spans += 1
                covered_until, first = cont, span[-1] + 1
                continue
            stats.unpatchable["relocation"] += 1
            stats.unprotected.append(d.address)
    stats.trampoline_bytes = len(tramp)
    return edits, bytes(tramp), stats


def main(argv: list[str] | None = None) -> int:
    """Report mode: python codescan.py --redzone eboot.bin [libc.prx ...]"""
    import argparse
    from pathlib import Path
    ap = argparse.ArgumentParser(description="Red-zone report for decrypted PS4 modules")
    ap.add_argument("--redzone", action="store_true", required=True,
                    help="count functions and memory accesses that run with a live red zone")
    ap.add_argument("--list", action="store_true", help="list the sites left unprotected")
    ap.add_argument("files", nargs="+", type=Path)
    args = ap.parse_args(argv)
    total = RzStats()
    for path in args.files:
        _, _, stats = redzone_patch(elfscan.parse(path), 0, 0x40000000)
        total.add(stats)
        print(f"{path.name}: {stats.text()}")
        if args.list and stats.unprotected:
            print("  unprotected sites: " + " ".join(f"{a:#x}" for a in stats.unprotected))
    if len(args.files) > 1:
        print(f"total: {total.text()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
