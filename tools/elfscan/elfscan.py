#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""elfscan: inventory a decrypted PS4 ELF (eboot.bin / .prx / .sprx).

Lists the modules and libraries the binary links against, every imported
symbol (by NID, resolved to a name when possible), relocation statistics and
byte-pattern candidates for things the native runtime has to patch (SSE4a,
%fs TLS access).

Usage:
    python elfscan.py eboot.bin
    python elfscan.py eboot.bin --shadps4 path/to/shadps4 --format md > docs/imports.md
    python elfscan.py eboot.bin --names names.txt --format json
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# PS4 ELF constants (see shadPS4 src/core/loader/elf.h)
# ---------------------------------------------------------------------------

SELF_MAGIC = b"\x4f\x15\x3d\x1d"

ET_NAMES = {
    0xFE00: "ET_SCE_EXEC",
    0xFE10: "ET_SCE_EXEC_ASLR",
    0xFE18: "ET_SCE_DYNAMIC",
    0xFE0C: "ET_SCE_STUBLIB",
    0xFE0F: "ET_SCE_DYNEXEC",
}

PT_LOAD = 1
PT_DYNAMIC = 2
PT_TLS = 7
PT_SCE_DYNLIBDATA = 0x61000000
PT_SCE_PROCPARAM = 0x61000001
PT_SCE_MODULE_PARAM = 0x61000002
PT_SCE_RELRO = 0x61000010
PT_GNU_EH_FRAME = 0x6474E550
PT_SCE_COMMENT = 0x6FFFFF00
PT_SCE_VERSION = 0x6FFFFF01

PT_NAMES = {
    0: "PT_NULL", PT_LOAD: "PT_LOAD", PT_DYNAMIC: "PT_DYNAMIC", 3: "PT_INTERP",
    PT_TLS: "PT_TLS", PT_SCE_DYNLIBDATA: "PT_SCE_DYNLIBDATA",
    PT_SCE_PROCPARAM: "PT_SCE_PROCPARAM", PT_SCE_MODULE_PARAM: "PT_SCE_MODULE_PARAM",
    PT_SCE_RELRO: "PT_SCE_RELRO", PT_GNU_EH_FRAME: "PT_GNU_EH_FRAME",
    PT_SCE_COMMENT: "PT_SCE_COMMENT", PT_SCE_VERSION: "PT_SCE_VERSION",
}

DT_NULL = 0
DT_NEEDED = 1
DT_INIT = 0x0C
DT_FINI = 0x0D
DT_SCE_MODULE_INFO = 0x6100000D
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_EXPORT_LIB = 0x61000013
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F

R_X86_64_NAMES = {
    1: "R_X86_64_64", 6: "R_X86_64_GLOB_DAT", 7: "R_X86_64_JUMP_SLOT",
    8: "R_X86_64_RELATIVE", 16: "R_X86_64_DTPMOD64", 17: "R_X86_64_DTPOFF64",
    18: "R_X86_64_TPOFF64",
}

STB_NAMES = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}
STT_NAMES = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 6: "TLS"}

PF_X = 1

# ---------------------------------------------------------------------------
# NIDs
# ---------------------------------------------------------------------------

NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
ID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def name_to_nid(name: str) -> str:
    """Compute the 11-character NID Sony's linker assigns to a symbol name."""
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    value = struct.unpack("<Q", digest[:8])[0]
    return base64.b64encode(struct.pack(">Q", value), altchars=b"+-").decode().rstrip("=")


def decode_id(text: str) -> int:
    """Decode the short library/module id that follows a NID ("NID#L#M")."""
    value = 0
    for ch in text:
        value = (value << 6) | ID_ALPHABET.index(ch)
    return value


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------


class ElfError(Exception):
    pass


@dataclass
class Segment:
    type: int
    flags: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    align: int

    @property
    def type_name(self) -> str:
        return PT_NAMES.get(self.type, hex(self.type))


@dataclass
class ModuleRef:
    id: int
    name: str
    version: str


@dataclass
class Symbol:
    index: int
    raw_name: str
    nid: str
    library: str | None
    module: str | None
    bind: str
    type: str
    defined: bool
    value: int


@dataclass
class Reloc:
    offset: int
    type: int
    sym: int
    addend: int


@dataclass
class ElfInfo:
    path: str
    elf_type: str
    entry: int
    segments: list[Segment]
    sdk_version: int | None = None
    module_info: ModuleRef | None = None
    needed_files: list[str] = field(default_factory=list)
    needed_modules: list[ModuleRef] = field(default_factory=list)
    import_libs: list[ModuleRef] = field(default_factory=list)
    export_libs: list[ModuleRef] = field(default_factory=list)
    symbols: list[Symbol] = field(default_factory=list)
    relocations: Counter = field(default_factory=Counter)
    reloc_targets: Counter = field(default_factory=Counter)
    patterns: dict[str, list[int]] = field(default_factory=dict)
    # Raw material for the converter (tools/sotcconv).
    data: bytes = field(default=b"", repr=False)
    dynamic: dict[int, list[int]] = field(default_factory=dict, repr=False)
    rela: list[Reloc] = field(default_factory=list, repr=False)

    @property
    def init(self) -> int | None:
        values = self.dynamic.get(DT_INIT)
        return values[0] if values else None

    def segment(self, p_type: int) -> Segment | None:
        return next((s for s in self.segments if s.type == p_type), None)

    @property
    def imports(self) -> list[Symbol]:
        return [s for s in self.symbols if not s.defined and s.raw_name]

    @property
    def exports(self) -> list[Symbol]:
        return [s for s in self.symbols if s.defined and s.bind != "LOCAL" and s.raw_name]


def _cstr(blob: bytes, offset: int) -> str:
    end = blob.find(b"\0", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("utf-8", errors="replace")


def _modref(value: int, strtab: bytes) -> ModuleRef:
    name = _cstr(strtab, value & 0xFFFFFFFF)
    minor = (value >> 32) & 0xFF
    major = (value >> 40) & 0xFF
    return ModuleRef(id=(value >> 48) & 0xFFFF, name=name, version=f"{major}.{minor}")


# Byte patterns worth knowing about before writing the loader. These are raw
# byte matches, so they are candidates, not a disassembly.
PATTERNS = {
    # SSE4a, present on Jaguar but not on Intel CPUs.
    "sse4a_extrq": re.compile(rb"\x66\x0f[\x78\x79]"),
    "sse4a_insertq": re.compile(rb"\xf2\x0f[\x78\x79]"),
    # mov r64, qword ptr fs:[0] -> guest TLS base reads.
    "fs_tls_base_read": re.compile(rb"\x64[\x48\x4c]\x8b[\x04\x0c\x14\x1c\x24\x2c\x34\x3c]\x25\x00\x00\x00\x00"),
}


SELF_SEG_ENCRYPTED = 0x2
SELF_SEG_COMPRESSED = 0x8
SELF_SEG_BLOCKED = 0x800


def unwrap_self(data: bytes, path: Path) -> bytes:
    """Rebuild a plain ELF from a fake-signed SELF (fSELF).

    Layout (see shadPS4 src/core/loader/elf.h): a 32-byte SELF header, one
    32-byte entry per SELF segment, then the embedded ELF header and program
    headers. Each "blocked" SELF segment holds the file data of the program
    header whose index is (flags >> 20). Retail SELFs are encrypted and cannot
    be read without the console's keys.
    """
    num_segments = struct.unpack_from("<H", data, 24)[0]
    elf_off = 32 + num_segments * 32
    if data[elf_off:elf_off + 4] != b"\x7fELF":
        raise ElfError(f"{path}: SELF without an embedded ELF header")
    e_phoff, = struct.unpack_from("<Q", data, elf_off + 32)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, elf_off + 54)

    phdrs = []
    for i in range(e_phnum):
        phdrs.append(struct.unpack_from("<IIQQQQQQ", data, elf_off + e_phoff + i * e_phentsize))
    size = max([e_phoff + e_phnum * e_phentsize] + [p[2] + p[5] for p in phdrs])
    out = bytearray(size)
    out[:e_phoff + e_phnum * e_phentsize] = data[elf_off:elf_off + e_phoff + e_phnum * e_phentsize]

    for i in range(num_segments):
        flags, offset, filesz, _memsz = struct.unpack_from("<QQQQ", data, 32 + i * 32)
        if not flags & SELF_SEG_BLOCKED:
            continue
        if flags & (SELF_SEG_ENCRYPTED | SELF_SEG_COMPRESSED):
            raise ElfError(
                f"{path} is an encrypted retail SELF. It must be decrypted on your own PS4 "
                "(GoldHEN / Homebrew Store dumper) before it can be read on a PC.")
        phdr_index = (flags >> 20) & 0xFFF
        if phdr_index >= e_phnum:
            continue
        p_offset = phdrs[phdr_index][2]
        out[p_offset:p_offset + filesz] = data[offset:offset + filesz]
    return bytes(out)


def parse(path: Path) -> ElfInfo:
    data = path.read_bytes()
    if data[:4] == SELF_MAGIC:
        data = unwrap_self(data, path)
    if data[:4] != b"\x7fELF":
        raise ElfError(f"{path} is not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise ElfError(f"{path} is not a little-endian ELF64 file")

    (e_type, _machine, _version, e_entry, e_phoff, _shoff, _flags, _ehsize,
     e_phentsize, e_phnum) = struct.unpack_from("<HHIQQQIHHH", data, 16)

    segments = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        segments.append(Segment(p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz, p_align))

    info = ElfInfo(
        path=str(path),
        elf_type=ET_NAMES.get(e_type, hex(e_type)),
        entry=e_entry,
        segments=segments,
        data=data,
    )

    by_type = {s.type: s for s in segments}

    procparam = by_type.get(PT_SCE_PROCPARAM)
    if procparam and procparam.filesz >= 24:
        _size, magic, _count, sdk = struct.unpack_from("<QIIQ", data, procparam.offset)
        if magic == 0x4942524F:  # "ORBI"
            info.sdk_version = sdk

    dynamic = by_type.get(PT_DYNAMIC)
    dynlib = by_type.get(PT_SCE_DYNLIBDATA)
    if dynamic is None or dynlib is None:
        _scan_patterns(info, data)
        return info  # statically linked or stripped: nothing more to resolve

    dynlib_blob = data[dynlib.offset:dynlib.offset + dynlib.filesz]
    tags: dict[int, list[int]] = defaultdict(list)
    for off in range(dynamic.offset, dynamic.offset + dynamic.filesz, 16):
        tag, value = struct.unpack_from("<qQ", data, off)
        if tag == DT_NULL:
            break
        tags[tag].append(value)
    info.dynamic = dict(tags)

    def single(tag: int, default: int = 0) -> int:
        values = tags.get(tag)
        return values[0] if values else default

    str_off, str_size = single(DT_SCE_STRTAB), single(DT_SCE_STRSZ)
    strtab = dynlib_blob[str_off:str_off + str_size]

    info.needed_files = [_cstr(strtab, v) for v in tags.get(DT_NEEDED, [])]
    info.needed_modules = [_modref(v, strtab) for v in tags.get(DT_SCE_NEEDED_MODULE, [])]
    info.import_libs = [_modref(v, strtab) for v in tags.get(DT_SCE_IMPORT_LIB, [])]
    info.export_libs = [_modref(v, strtab) for v in tags.get(DT_SCE_EXPORT_LIB, [])]
    if DT_SCE_MODULE_INFO in tags:
        info.module_info = _modref(single(DT_SCE_MODULE_INFO), strtab)

    import_libs = {lib.id: lib.name for lib in info.import_libs}
    export_libs = {lib.id: lib.name for lib in info.export_libs}
    import_mods = {mod.id: mod.name for mod in info.needed_modules}
    export_mods = {info.module_info.id: info.module_info.name} if info.module_info else {}

    sym_off, sym_size = single(DT_SCE_SYMTAB), single(DT_SCE_SYMTABSZ)
    for index, off in enumerate(range(sym_off, sym_off + sym_size, 24)):
        st_name, st_info, _other, st_shndx, st_value, _size = \
            struct.unpack_from("<IBBHQQ", dynlib_blob, off)
        raw = _cstr(strtab, st_name)
        nid, library, module = raw, None, None
        parts = raw.split("#")
        if len(parts) == 3:
            nid = parts[0]
            libs, mods = (export_libs, export_mods) if st_shndx else (import_libs, import_mods)
            try:
                library = libs.get(decode_id(parts[1]), f"lib#{parts[1]}")
                module = mods.get(decode_id(parts[2]), f"mod#{parts[2]}")
            except ValueError:
                pass
        info.symbols.append(Symbol(
            index=index, raw_name=raw, nid=nid, library=library, module=module,
            bind=STB_NAMES.get(st_info >> 4, str(st_info >> 4)),
            type=STT_NAMES.get(st_info & 0xF, str(st_info & 0xF)),
            defined=st_shndx != 0, value=st_value,
        ))

    for start_tag, size_tag in ((DT_SCE_RELA, DT_SCE_RELASZ), (DT_SCE_JMPREL, DT_SCE_PLTRELSZ)):
        start, size = single(start_tag), single(size_tag)
        for off in range(start, start + size, 24):
            r_offset, r_info, addend = struct.unpack_from("<QQq", dynlib_blob, off)
            rtype, sym = r_info & 0xFFFFFFFF, r_info >> 32
            info.rela.append(Reloc(r_offset, rtype, sym, addend))
            info.relocations[R_X86_64_NAMES.get(rtype, f"type_{rtype}")] += 1
            if sym:
                info.reloc_targets[sym] += 1

    _scan_patterns(info, data)
    return info


def _scan_patterns(info: ElfInfo, data: bytes) -> None:
    for name, pattern in PATTERNS.items():
        hits = []
        for seg in info.segments:
            if seg.type != PT_LOAD or not seg.flags & PF_X:
                continue
            blob = data[seg.offset:seg.offset + seg.filesz]
            hits.extend(seg.vaddr + m.start() for m in pattern.finditer(blob))
        info.patterns[name] = hits


# ---------------------------------------------------------------------------
# Name databases
# ---------------------------------------------------------------------------

LIB_FUNCTION_RE = re.compile(r'LIB_(?:FUNCTION|OBJ)\s*\(\s*"([A-Za-z0-9+\-]{11})"\s*,\s*"([^"]+)"(.*?)\)\s*;', re.S)
# Last identifier in the argument list; also unwraps ORBIS(fn) / HOST_CALL(fn) / &fn.
LAST_IDENT_RE = re.compile(r"([A-Za-z_][\w:]*)\W*$")
AEROLIB_RE = re.compile(r'STUB\(\s*"([A-Za-z0-9+\-]{11})"\s*,\s*([A-Za-z_]\w*)\s*\)')


def load_shadps4(root: Path) -> dict[str, tuple[str, str]]:
    """Map NID -> (library, function) for everything shadPS4 implements.

    The function is shadPS4's own implementation name, which is not always
    Sony's name; use load_aerolib() for the real symbol names.
    """
    known: dict[str, tuple[str, str]] = {}
    src = root / "src" if (root / "src").is_dir() else root
    for path in src.rglob("*.cpp"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for nid, lib, rest in LIB_FUNCTION_RE.findall(text):
            match = LAST_IDENT_RE.search(rest.strip())
            known[nid] = (lib, match.group(1) if match else "?")
    return known


def load_aerolib(root: Path) -> dict[str, str]:
    """Map NID -> Sony symbol name from shadPS4's aerolib table (all known NIDs)."""
    src = root / "src" if (root / "src").is_dir() else root
    path = src / "core" / "aerolib" / "aerolib.inl"
    if not path.is_file():
        return {}
    return dict(AEROLIB_RE.findall(path.read_text(encoding="utf-8", errors="replace")))


def load_names(path: Path) -> dict[str, str]:
    """Map NID -> name from a plain list of symbol names (one per line)."""
    names = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        name = line.strip()
        if name and not name.startswith("#"):
            names[name_to_nid(name)] = name
    return names


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def build_report(info: ElfInfo, names: dict[str, str], shad: dict[str, tuple[str, str]]) -> dict:
    by_lib: dict[str, list[dict]] = defaultdict(list)
    for sym in info.imports:
        name = names.get(sym.nid) or (shad[sym.nid][1] if sym.nid in shad else None)
        by_lib[f"{sym.module or '?'}/{sym.library or '?'}"].append({
            "nid": sym.nid,
            "name": name,
            "type": sym.type,
            "in_shadps4": sym.nid in shad,
            "refs": info.reloc_targets.get(sym.index, 0),
        })
    for entries in by_lib.values():
        entries.sort(key=lambda e: (e["name"] is None, e["name"] or e["nid"]))

    total = len(info.imports)
    return {
        "file": info.path,
        "elf_type": info.elf_type,
        "entry": hex(info.entry),
        "sdk_version": hex(info.sdk_version) if info.sdk_version is not None else None,
        "module": info.module_info.__dict__ if info.module_info else None,
        "segments": [
            {"type": s.type_name, "flags": s.flags, "vaddr": hex(s.vaddr),
             "filesz": s.filesz, "memsz": s.memsz}
            for s in info.segments
        ],
        "needed_files": info.needed_files,
        "needed_modules": [m.__dict__ for m in info.needed_modules],
        "import_libs": [m.__dict__ for m in info.import_libs],
        "export_libs": [m.__dict__ for m in info.export_libs],
        "summary": {
            "imports": total,
            "named": sum(1 for e in by_lib.values() for x in e if x["name"]),
            "in_shadps4": sum(1 for e in by_lib.values() for x in e if x["in_shadps4"]),
            "exports": len(info.exports),
        },
        "relocations": dict(info.relocations),
        "patterns": {k: {"count": len(v), "first": [hex(a) for a in v[:16]]}
                     for k, v in info.patterns.items()},
        "imports_by_library": dict(sorted(by_lib.items())),
    }


def render_text(r: dict, markdown: bool) -> str:
    h = (lambda t: f"\n## {t}\n") if markdown else (lambda t: f"\n== {t} ==")
    out = []
    out.append(f"# elfscan: {Path(r['file']).name}" if markdown else f"elfscan: {r['file']}")
    out.append(h("Overview"))
    mod = r["module"]
    rows = [
        ("ELF type", r["elf_type"]),
        ("Entry", r["entry"]),
        ("SDK version", r["sdk_version"] or "-"),
        ("Module", f"{mod['name']} {mod['version']}" if mod else "-"),
        ("Imports", f"{r['summary']['imports']} "
                    f"(named {r['summary']['named']}, known to shadPS4 {r['summary']['in_shadps4']})"),
        ("Exports", r["summary"]["exports"]),
    ]
    if markdown:
        out += ["| | |", "|---|---|"] + [f"| {k} | {v} |" for k, v in rows]
    else:
        out += [f"  {k:<12} {v}" for k, v in rows]

    out.append(h("Segments"))
    for s in r["segments"]:
        out.append(f"{'- ' if markdown else '  '}{s['type']:<20} vaddr={s['vaddr']:<12} "
                   f"filesz={s['filesz']:<10} memsz={s['memsz']:<10} flags={s['flags']}")

    out.append(h("Needed"))
    for f in r["needed_files"]:
        out.append(f"{'- ' if markdown else '  '}file   {f}")
    for m in r["needed_modules"]:
        out.append(f"{'- ' if markdown else '  '}module {m['name']} {m['version']} (id {m['id']})")
    for m in r["import_libs"]:
        out.append(f"{'- ' if markdown else '  '}lib    {m['name']} {m['version']} (id {m['id']})")

    out.append(h("Relocations"))
    for k, v in sorted(r["relocations"].items()):
        out.append(f"{'- ' if markdown else '  '}{k}: {v}")

    out.append(h("Patch candidates (raw byte matches in executable segments)"))
    for k, v in r["patterns"].items():
        first = ", ".join(v["first"][:8])
        out.append(f"{'- ' if markdown else '  '}{k}: {v['count']}" + (f"  [{first}]" if first else ""))

    out.append(h("Imports by module/library"))
    for lib, entries in r["imports_by_library"].items():
        done = sum(1 for e in entries if e["in_shadps4"])
        if markdown:
            out.append(f"\n### {lib} ({len(entries)} imports, {done} known to shadPS4)\n")
            out.append("| NID | Name | Type | Refs | shadPS4 |")
            out.append("|---|---|---|---|---|")
            for e in entries:
                out.append(f"| `{e['nid']}` | {e['name'] or ''} | {e['type']} | {e['refs']} | "
                           f"{'yes' if e['in_shadps4'] else ''} |")
        else:
            out.append(f"\n  {lib}  ({len(entries)} imports, {done} known to shadPS4)")
            for e in entries:
                flag = "*" if e["in_shadps4"] else " "
                out.append(f"   {flag} {e['nid']}  {e['name'] or '?':<48} {e['type']:<7} refs={e['refs']}")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", type=Path, help="decrypted eboot.bin / .prx / .sprx")
    ap.add_argument("--shadps4", type=Path, help="shadPS4 source checkout, to name NIDs it implements")
    ap.add_argument("--names", type=Path, action="append", default=[],
                    help="text file of symbol names, one per line (NIDs are computed)")
    ap.add_argument("--format", choices=["text", "md", "json"], default="text")
    ap.add_argument("-o", "--output", type=Path, help="write to file instead of stdout")
    args = ap.parse_args(argv)

    try:
        info = parse(args.elf)
    except (ElfError, OSError, struct.error) as exc:
        print(f"elfscan: {exc}", file=sys.stderr)
        return 1

    names: dict[str, str] = {}
    for path in args.names:
        names.update(load_names(path))
    shad = load_shadps4(args.shadps4) if args.shadps4 else {}
    if args.shadps4:
        names = {**load_aerolib(args.shadps4), **names}

    report = build_report(info, names, shad)
    text = json.dumps(report, indent=2) + "\n" if args.format == "json" \
        else render_text(report, markdown=args.format == "md")

    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
