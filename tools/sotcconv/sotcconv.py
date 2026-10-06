#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotcconv: link a decrypted PS4 eboot and its bundled PRX modules into a boot image.

The image is what runtime/ loads: every module's segments laid out in one
address range, every relocation normalized, cross-module imports (eboot ->
libc/Fios2, Fios2 -> libc, LibcInternal -> libc) resolved ahead of time, the
remaining system imports listed for the HLE runtime, and the instructions the
runtime must patch (fs TLS reads, SSE4a) decoded exactly.

Usage:
    python sotcconv.py --game D:/CUSA08809/uroot -o build/game/boot.img
    python sotcconv.py --game <dir> --shadps4 ../shadps4 -o boot.img --report build/game/boot.txt

The format is documented in runtime/include/bootimg.h; keep the two in sync.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "elfscan"))
import elfscan  # noqa: E402

import codescan  # noqa: E402

MAGIC = b"SOTCIMG1"
VERSION = 1
PAGE = 0x4000  # PS4 page size

# Relocation kinds (bootimg.h).
REL_IMAGE = 0   # *(u64*)(base+offset) = base + value + addend
REL_IMPORT = 1  # *(u64*)(base+offset) = import[value] + addend
REL_RAW = 2     # *(u64*)(base+offset) = value + addend (TLS module ids, DTPOFF)

# Import kinds.
IMP_FUNC = 1
IMP_DATA = 2

# Segment protection bits (bootimg.h), same as ELF p_flags.
PROT_X, PROT_W, PROT_R = 1, 2, 4

# Libraries the game's libc.prx exports a superset of. Imports from these
# that no module exports by (library, NID) are matched by NID against libc.
LIBC_ALIASES = {"libSceLibcInternal", "libSceLibcInternalExt"}

DEFAULT_MODULES = ["sce_module/libc.prx", "sce_module/libSceFios2.prx"]


class ConvertError(Exception):
    pass


@dataclass
class Module:
    name: str
    info: elfscan.ElfInfo
    base: int = 0        # offset of the module in the image
    size: int = 0
    tls_modid: int = 0


@dataclass
class Import:
    library: str
    nid: str
    name: str | None
    kind: int
    refs: int = 0


@dataclass
class Image:
    modules: list[Module]
    size: int = 0
    segments: list[tuple[int, int, int, int]] = field(default_factory=list)  # (img_off, filesz, memsz, prot)
    segment_data: list[bytes] = field(default_factory=list)
    relocs: list[tuple[int, int, int, int]] = field(default_factory=list)    # (offset, kind, value, addend)
    imports: list[Import] = field(default_factory=list)
    patches: list[codescan.PatchSite] = field(default_factory=list)
    entry: int = 0
    procparam: int = 0
    sdk_version: int = 0
    stats: Counter = field(default_factory=Counter)
    cross_links: Counter = field(default_factory=Counter)


def layout(modules: list[Module]) -> int:
    offset = 0
    for i, mod in enumerate(modules):
        loads = [s for s in mod.info.segments if s.type in (elfscan.PT_LOAD, elfscan.PT_SCE_RELRO)]
        if not loads:
            raise ConvertError(f"{mod.name}: no loadable segments")
        end = max(s.vaddr + s.memsz for s in loads)
        mod.base = offset
        mod.size = (end + PAGE - 1) & ~(PAGE - 1)
        mod.tls_modid = i + 1
        offset += mod.size
    return offset


def build(modules: list[Module], names: dict[str, str]) -> Image:
    img = Image(modules=modules)
    img.size = layout(modules)
    main = modules[0].info

    img.entry = modules[0].base + main.entry
    pp = main.segment(elfscan.PT_SCE_PROCPARAM)
    img.procparam = modules[0].base + pp.vaddr if pp else 0
    img.sdk_version = main.sdk_version or 0

    # Exports by (library, nid) and by nid alone, as image offsets.
    exports: dict[tuple[str, str], tuple[int, Module]] = {}
    exports_by_nid: dict[str, tuple[int, Module]] = {}
    for mod in modules:
        for sym in mod.info.exports:
            target = mod.base + sym.value
            exports[(sym.library, sym.nid)] = (target, mod)
            if mod.name == "libc":
                exports_by_nid.setdefault(sym.nid, (target, mod))

    import_index: dict[tuple[str, str], int] = {}

    for mod in modules:
        info = mod.info
        for seg in info.segments:
            if seg.type not in (elfscan.PT_LOAD, elfscan.PT_SCE_RELRO):
                continue
            img.segments.append((mod.base + seg.vaddr, seg.filesz, seg.memsz, seg.flags & 7))
            img.segment_data.append(info.data[seg.offset:seg.offset + seg.filesz])

        for rel in info.rela:
            where = mod.base + rel.offset
            img.stats[elfscan.R_X86_64_NAMES.get(rel.type, str(rel.type))] += 1
            if rel.type == 8:  # RELATIVE
                img.relocs.append((where, REL_IMAGE, mod.base + rel.addend, 0))
            elif rel.type == 16:  # DTPMOD64
                img.relocs.append((where, REL_RAW, mod.tls_modid, 0))
            elif rel.type == 17:  # DTPOFF64
                sym = info.symbols[rel.sym]
                img.relocs.append((where, REL_RAW, sym.value, rel.addend))
            elif rel.type in (1, 6, 7):  # 64, GLOB_DAT, JUMP_SLOT
                addend = rel.addend if rel.type == 1 else 0
                sym = info.symbols[rel.sym]
                if sym.defined or sym.bind == "LOCAL":
                    img.relocs.append((where, REL_IMAGE, mod.base + sym.value, addend))
                    continue
                hit = exports.get((sym.library, sym.nid))
                if hit is None and sym.library in LIBC_ALIASES:
                    hit = exports_by_nid.get(sym.nid)
                if hit is not None:
                    target, owner = hit
                    img.relocs.append((where, REL_IMAGE, target, addend))
                    img.cross_links[f"{mod.name} -> {owner.name} ({sym.library})"] += 1
                    continue
                key = (sym.library or "?", sym.nid)
                if key not in import_index:
                    import_index[key] = len(img.imports)
                    kind = IMP_DATA if sym.type == "OBJECT" else IMP_FUNC
                    img.imports.append(Import(key[0], sym.nid, names.get(sym.nid), kind))
                index = import_index[key]
                img.imports[index].refs += 1
                img.relocs.append((where, REL_IMPORT, index, addend))
            else:
                name = elfscan.R_X86_64_NAMES.get(rel.type, f"type {rel.type}")
                raise ConvertError(f"{mod.name}: unsupported relocation {name} at {rel.offset:#x}")

        sites, nfuncs = codescan.scan(info)
        img.stats[f"functions:{mod.name}"] = nfuncs
        for site in sites:
            site.vaddr += mod.base
            img.patches.append(site)
    return img


# ---------------------------------------------------------------------------
# Serialization (runtime/include/bootimg.h)
# ---------------------------------------------------------------------------

HEADER = struct.Struct("<8sIIQQQQQIIIIIIQ")  # see bootimg_header
MODULE = struct.Struct("<IIQQQQQQQQQQQ")    # bootimg_module
SEGMENT = struct.Struct("<QQQQI4x")         # bootimg_segment
RELOC = struct.Struct("<QQqI4x")            # bootimg_reloc
IMPORT = struct.Struct("<IIII")             # bootimg_import
PATCH = struct.Struct("<QBBBBBB2xq")        # bootimg_patch


def serialize(img: Image) -> bytes:
    strings = bytearray(b"\0")
    string_index: dict[str, int] = {}

    def s(text: str) -> int:
        if text not in string_index:
            string_index[text] = len(strings)
            strings.extend(text.encode() + b"\0")
        return string_index[text]

    modules = b""
    for mod in img.modules:
        tls = mod.info.segment(elfscan.PT_TLS)
        init = mod.info.init
        hdr, hdr_size, eh_frame, eh_size = codescan.eh_frame_info(mod.info, mod.size)
        modules += MODULE.pack(
            s(mod.name), mod.tls_modid, mod.base, mod.size,
            mod.base + init if init is not None else 0xFFFFFFFFFFFFFFFF,
            mod.base + tls.vaddr if tls else 0, tls.filesz if tls else 0,
            tls.memsz if tls else 0, max(tls.align, 1) if tls else 1,
            mod.base + hdr if hdr else 0, hdr_size, mod.base + eh_frame if hdr else 0, eh_size)

    imports = b"".join(
        IMPORT.pack(s(i.library), s(i.nid), s(i.name or ""), i.kind) for i in img.imports)
    relocs = b"".join(RELOC.pack(off, value, addend, kind) for off, kind, value, addend in img.relocs)
    patches = b"".join(
        PATCH.pack(p.vaddr, p.kind, p.length, p.a, p.b, p.c, p.d, p.disp) for p in img.patches)

    seg_table_size = SEGMENT.size * len(img.segments)
    fixed = (HEADER.size + len(modules) + seg_table_size + len(relocs) + len(imports)
             + len(patches) + len(strings))
    data_start = (fixed + 15) & ~15
    segments, blob, pos = b"", bytearray(), data_start
    for (img_off, filesz, memsz, prot), data in zip(img.segments, img.segment_data):
        segments += SEGMENT.pack(img_off, pos, filesz, memsz, prot)
        blob += data
        pos += len(data)

    header = HEADER.pack(
        MAGIC, VERSION, len(img.modules), img.size, img.entry, img.procparam, img.sdk_version,
        len(strings), len(img.segments), len(img.relocs), len(img.imports), len(img.patches),
        0, 0, data_start)
    out = header + modules + segments + relocs + imports + patches + bytes(strings)
    return out + bytes(data_start - len(out)) + bytes(blob)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def report(img: Image) -> str:
    lines = [f"image size {img.size:#x}, entry {img.entry:#x}, procparam {img.procparam:#x}, "
             f"sdk {img.sdk_version:#x}"]
    for mod in img.modules:
        tls = mod.info.segment(elfscan.PT_TLS)
        lines.append(f"  module {mod.name:<8} base {mod.base:#010x} size {mod.size:#010x} "
                     f"init {mod.info.init if mod.info.init is None else hex(mod.info.init)} "
                     f"tls {tls.memsz if tls else 0} bytes (modid {mod.tls_modid})")
    lines.append(f"relocations: {len(img.relocs)} {dict(img.stats)}")
    lines.append("cross-module links:")
    lines += [f"  {v:5} {k}" for k, v in sorted(img.cross_links.items())]
    by_lib = Counter(i.library for i in img.imports)
    named = sum(1 for i in img.imports if i.name)
    lines.append(f"system imports for the runtime: {len(img.imports)} ({named} named)")
    lines += [f"  {v:4} {k}" for k, v in sorted(by_lib.items())]
    kinds = Counter(p.kind for p in img.patches)
    lines.append(f"patch sites: {len(img.patches)} "
                 f"(fs {kinds[codescan.PATCH_FS]}, extrq {kinds[2] + kinds[3]}, "
                 f"insertq {kinds[4] + kinds[5]})")
    odd = [p for p in img.patches if p.kind == codescan.PATCH_FS and p.c != codescan.FS_ABS]
    if odd:
        lines.append(f"  WARNING: {len(odd)} fs sites need trampolines on Windows:")
        lines += [f"    {p.vaddr:#x} {p.text}" for p in odd[:20]]
    lines.append("imports:")
    for i in sorted(img.imports, key=lambda i: (i.library, i.name or "~" + i.nid)):
        lines.append(f"  {i.library:<28} {i.nid}  {i.name or '?':<44} "
                     f"{'data' if i.kind == IMP_DATA else 'func'} refs={i.refs}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", type=Path, required=True, help="game folder holding eboot.bin")
    ap.add_argument("--module", action="append", default=None,
                    help="extra/override PRX paths relative to --game (default: libc, Fios2)")
    ap.add_argument("--shadps4", type=Path, help="shadPS4 checkout, to name imports")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--report", type=Path, help="write a text report here")
    args = ap.parse_args(argv)

    paths = [args.game / "eboot.bin"] + [args.game / m for m in (args.module or DEFAULT_MODULES)]
    try:
        modules = []
        for path in paths:
            info = elfscan.parse(path)
            name = info.module_info.name if info.module_info else path.stem
            modules.append(Module("eboot" if path.name == "eboot.bin" else name, info))
        names = {}
        if args.shadps4:
            names = {nid: fn for nid, (_lib, fn) in elfscan.load_shadps4(args.shadps4).items()}
            names.update(elfscan.load_aerolib(args.shadps4))
        img = build(modules, names)
    except (ConvertError, elfscan.ElfError, OSError) as exc:
        print(f"sotcconv: {exc}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(serialize(img))
    text = report(img)
    if args.report:
        args.report.write_text(text, encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stdout.write("\n".join(text.split("\nimports:\n")[0].splitlines()) + "\n")
    print(f"wrote {args.output} ({args.output.stat().st_size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
