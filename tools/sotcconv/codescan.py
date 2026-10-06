# SPDX-License-Identifier: GPL-2.0-or-later
"""Find the instructions the runtime must patch, by disassembling real code only.

Executable PS4 segments also hold read-only data, so a linear sweep decodes
strings as bogus instructions (an ASCII 'd' is the fs segment prefix). We take
function ranges from .eh_frame (via the PT_GNU_EH_FRAME header table) and
disassemble each function with capstone.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

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
