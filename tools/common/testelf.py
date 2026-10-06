# SPDX-License-Identifier: GPL-2.0-or-later
"""Build synthetic PS4-style ELF modules in memory for tests (no game files needed)."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

ID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

PT_LOAD, PT_DYNAMIC, PT_TLS = 1, 2, 7
PT_SCE_DYNLIBDATA, PT_SCE_PROCPARAM = 0x61000000, 0x61000001
PT_GNU_EH_FRAME = 0x6474E550

DT_NULL, DT_NEEDED, DT_INIT = 0, 1, 0x0C
DT_SCE_MODULE_INFO, DT_SCE_NEEDED_MODULE = 0x6100000D, 0x6100000F
DT_SCE_EXPORT_LIB, DT_SCE_IMPORT_LIB = 0x61000013, 0x61000015
DT_SCE_JMPREL, DT_SCE_PLTRELSZ = 0x61000029, 0x6100002D
DT_SCE_RELA, DT_SCE_RELASZ = 0x6100002F, 0x61000031
DT_SCE_STRTAB, DT_SCE_STRSZ = 0x61000035, 0x61000037
DT_SCE_SYMTAB, DT_SCE_SYMTABSZ = 0x61000039, 0x6100003F

STB_LOCAL, STB_GLOBAL = 0, 1
STT_OBJECT, STT_FUNC = 1, 2


class StrTab:
    def __init__(self):
        self.blob = bytearray(b"\0")
        self.index: dict[str, int] = {}

    def add(self, s: str) -> int:
        if s not in self.index:
            self.index[s] = len(self.blob)
            self.blob += s.encode() + b"\0"
        return self.index[s]


def modval(name_off: int, major: int, minor: int, mod_id: int) -> int:
    return name_off | (minor << 32) | (major << 40) | (mod_id << 48)


def encode_id(value: int) -> str:
    return ID_ALPHABET[value] if value < 64 else ID_ALPHABET[value >> 6] + ID_ALPHABET[value & 63]


def wrap_fself(elf: bytes, extra_flags: int = 0) -> bytes:
    """Wrap a plain ELF in a minimal fake-signed SELF container."""
    blocked = 0x800
    e_phoff, = struct.unpack_from("<Q", elf, 32)
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 54)
    phdrs = [struct.unpack_from("<IIQQQQQQ", elf, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    with_data = [(i, p) for i, p in enumerate(phdrs) if p[5]]

    headers = elf[:e_phoff + e_phnum * e_phentsize]
    data_start = 32 + len(with_data) * 32 + len(headers)
    entries, payload = b"", b""
    for i, p in with_data:
        flags = blocked | (i << 20) | extra_flags
        entries += struct.pack("<QQQQ", flags, data_start + len(payload), p[5], p[6])
        payload += elf[p[2]:p[2] + p[5]]
    self_header = b"\x4f\x15\x3d\x1d" + bytes(20) + struct.pack("<HH", len(with_data), 0) + bytes(4)
    return self_header + entries + headers + payload


@dataclass
class Sym:
    name: str            # raw dynamic symbol name, e.g. "NID#B#B" or "local_name"
    type: int = STT_FUNC
    bind: int = STB_GLOBAL
    defined: bool = False
    value: int = 0


@dataclass
class Rel:
    offset: int
    type: int
    sym: int = 0         # 1-based index into Module.syms (0 = none)
    addend: int = 0
    plt: bool = False    # put in DT_SCE_JMPREL instead of DT_SCE_RELA


@dataclass
class Module:
    e_type: int = 0xFE10                 # ET_SCE_EXEC_ASLR; 0xFE18 for a PRX
    name: str = "eboot"
    code: bytes = b"\xc3"                # text segment content at vaddr 0
    functions: list[tuple[int, int]] = field(default_factory=list)  # (start, size) for .eh_frame
    data: bytes = b""                    # data segment content at data_vaddr
    data_vaddr: int = 0x10000
    entry: int = 0
    init: int | None = None
    needed: list[tuple[str, int]] = field(default_factory=list)       # needed modules
    import_libs: list[tuple[str, int]] = field(default_factory=list)
    export_libs: list[tuple[str, int]] = field(default_factory=list)
    syms: list[Sym] = field(default_factory=list)
    rels: list[Rel] = field(default_factory=list)
    tls: tuple[int, int, int] | None = None   # (vaddr, filesz, memsz), inside data
    sdk_version: int | None = None


def _uleb(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def _eh_frame(functions: list[tuple[int, int]], base: int) -> tuple[bytes, bytes, int]:
    """Return (eh_frame, eh_frame_hdr, hdr vaddr) for functions, eh_frame placed at `base`."""
    cie_body = (struct.pack("<I", 0) + b"\x01zR\x00" + _uleb(1) + b"\x78" + b"\x10" + _uleb(1)
                + b"\x1b")
    while (len(cie_body) + 4) % 8:
        cie_body += b"\x00"
    frame = bytearray(struct.pack("<I", len(cie_body)) + cie_body)
    fdes = []
    for start, size in functions:
        pos = base + len(frame)
        body = (struct.pack("<I", pos + 4 - base) + struct.pack("<i", start - (pos + 8))
                + struct.pack("<I", size) + _uleb(0))
        while (len(body) + 4) % 4:
            body += b"\x00"
        fdes.append((start, pos))
        frame += struct.pack("<I", len(body)) + body
    frame += struct.pack("<I", 0)
    hdr_vaddr = base + len(frame)
    hdr = bytearray(b"\x01\x1b\x03\x3b")
    hdr += struct.pack("<i", base - (hdr_vaddr + 4))
    hdr += struct.pack("<I", len(fdes))
    for start, pos in sorted(fdes):
        hdr += struct.pack("<ii", start - hdr_vaddr, pos - hdr_vaddr)
    return bytes(frame), bytes(hdr), hdr_vaddr


def build_module(m: Module) -> bytes:
    st = StrTab()
    dyn: list[tuple[int, int]] = []
    for name, mid in m.needed:
        dyn.append((DT_NEEDED, st.add(name + ".prx")))
        dyn.append((DT_SCE_NEEDED_MODULE, modval(st.add(name), 1, 1, mid)))
    dyn.append((DT_SCE_MODULE_INFO, modval(st.add(m.name), 1, 1, 0)))
    for name, lid in m.import_libs:
        dyn.append((DT_SCE_IMPORT_LIB, modval(st.add(name), 1, 0, lid)))
    for name, lid in m.export_libs:
        dyn.append((DT_SCE_EXPORT_LIB, modval(st.add(name), 1, 0, lid)))
    if m.init is not None:
        dyn.append((DT_INIT, m.init))

    symtab = bytearray(24)
    for s in m.syms:
        symtab += struct.pack("<IBBHQQ", st.add(s.name), (s.bind << 4) | s.type, 0,
                              1 if s.defined else 0, s.value, 0)
    rela = b"".join(struct.pack("<QQq", r.offset, (r.sym << 32) | r.type, r.addend)
                    for r in m.rels if not r.plt)
    jmprel = b"".join(struct.pack("<QQq", r.offset, (r.sym << 32) | r.type, r.addend)
                      for r in m.rels if r.plt)

    dynlib = bytearray()
    str_off = len(dynlib); dynlib += st.blob
    sym_off = len(dynlib); dynlib += symtab
    rela_off = len(dynlib); dynlib += rela
    jmp_off = len(dynlib); dynlib += jmprel
    dyn += [(DT_SCE_STRTAB, str_off), (DT_SCE_STRSZ, len(st.blob)),
            (DT_SCE_SYMTAB, sym_off), (DT_SCE_SYMTABSZ, len(symtab)),
            (DT_SCE_RELA, rela_off), (DT_SCE_RELASZ, len(rela)),
            (DT_SCE_JMPREL, jmp_off), (DT_SCE_PLTRELSZ, len(jmprel)), (DT_NULL, 0)]
    dynamic = b"".join(struct.pack("<qQ", t, v) for t, v in dyn)

    # Text segment: code, then .eh_frame and .eh_frame_hdr (read-only data).
    text = bytearray(m.code)
    while len(text) % 8:
        text += b"\xcc"
    hdr_vaddr = hdr_size = 0
    if m.functions:
        frame, hdr, hdr_vaddr = _eh_frame(m.functions, len(text))
        text += frame + hdr
        hdr_size = len(hdr)
    procparam = struct.pack("<QIIQ", 24, 0x4942524F, 1, m.sdk_version) if m.sdk_version else b""

    phdr_specs = []  # (type, flags, vaddr, content, memsz)
    phdr_specs.append((PT_LOAD, 5, 0, bytes(text), len(text)))
    if m.data:
        phdr_specs.append((PT_LOAD, 6, m.data_vaddr, m.data, len(m.data)))
    phnum = len(phdr_specs) + 2 + (1 if m.tls else 0) + (1 if hdr_vaddr else 0) + (1 if procparam else 0)

    offset = 64 + phnum * 56
    blobs, phdrs, seg_offset = [], [], {}
    for p_type, flags, vaddr, content, memsz in phdr_specs:
        seg_offset[vaddr] = offset
        phdrs.append(struct.pack("<IIQQQQQQ", p_type, flags, offset, vaddr, 0, len(content), memsz, 0x4000))
        blobs.append(content)
        offset += len(content)
    if m.tls:
        tv, tf, tm = m.tls
        phdrs.append(struct.pack("<IIQQQQQQ", PT_TLS, 4, seg_offset[m.data_vaddr] + tv - m.data_vaddr,
                                 tv, 0, tf, tm, 8))
    if hdr_vaddr:
        phdrs.append(struct.pack("<IIQQQQQQ", PT_GNU_EH_FRAME, 4, seg_offset[0] + hdr_vaddr, hdr_vaddr,
                                 0, hdr_size, hdr_size, 4))
    for p_type, content in ((PT_DYNAMIC, dynamic), (PT_SCE_DYNLIBDATA, bytes(dynlib))):
        phdrs.append(struct.pack("<IIQQQQQQ", p_type, 4, offset, 0, 0, len(content), 0, 8))
        blobs.append(content)
        offset += len(content)
    if procparam:
        phdrs.append(struct.pack("<IIQQQQQQ", PT_SCE_PROCPARAM, 4, offset, 0x40000, 0,
                                 len(procparam), len(procparam), 8))
        blobs.append(procparam)
    assert len(phdrs) == phnum
    ident = b"\x7fELF" + bytes([2, 1, 1, 9]) + bytes(8)
    header = ident + struct.pack("<HHIQQQIHHHHHH", m.e_type, 0x3E, 1, m.entry, 64, 0, 0, 64, 56,
                                 phnum, 64, 0, 0)
    return header + b"".join(phdrs) + b"".join(blobs)
