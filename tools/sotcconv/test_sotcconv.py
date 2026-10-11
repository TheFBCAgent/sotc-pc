# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for sotcconv: link a synthetic eboot against a synthetic libc.prx."""

import contextlib
import dataclasses
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent / "elfscan"), str(HERE.parent / "common")]
import codescan  # noqa: E402
import elfscan  # noqa: E402
import sotcconv  # noqa: E402
from testelf import (STB_LOCAL, STT_OBJECT, Module, Rel, Sym, build_module,  # noqa: E402
                     encode_id)

nid = elfscan.name_to_nid
R_64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE, R_DTPMOD64 = 1, 6, 7, 8, 16

# libc.prx: exports memcpy and strlen (library "libc", id 0) and has TLS.
LIBC = Module(
    e_type=0xFE18, name="libc", init=0x10,
    code=b"\x90" * 0x10 + b"\xc3" + b"\x90" * 0x2F,
    functions=[(0x10, 1)],
    data=b"TLSI" + bytes(28), data_vaddr=0x10000, tls=(0x10000, 4, 16),
    export_libs=[("libc", 0)],
    syms=[Sym(f"{nid('memcpy')}#A#A", defined=True, value=0x20),
          Sym(f"{nid('strlen')}#A#A", defined=True, value=0x30)],
    rels=[Rel(0x10010, R_DTPMOD64)],
)

# eboot: code with one real fs read and EXTRQ inside a function, plus a decoy
# fs byte pattern in data that is not code.
FS_READ = b"\x64\x48\x8b\x04\x25\x00\x00\x00\x00"   # mov rax, fs:[0]
EXTRQ = b"\x66\x0f\x79\xc1"                          # extrq xmm0, xmm1
EBOOT_CODE = b"\x90" * 0x40 + FS_READ + EXTRQ + b"\xc3" + b"\x90" * 0x10 + FS_READ
EBOOT = Module(
    e_type=0xFE10, name="eboot", entry=0x40, sdk_version=0x04508101,
    code=EBOOT_CODE, functions=[(0x40, len(FS_READ) + len(EXTRQ) + 1)],
    data=bytes(0x40), data_vaddr=0x10000, tls=(0x10020, 0, 32),
    needed=[("libc", 3), ("libkernel", 1)],
    import_libs=[("libc", 3), ("libkernel", 1), ("libSceLibcInternal", 2)],
    syms=[
        Sym(f"{nid('memcpy')}#{encode_id(3)}#{encode_id(3)}"),               # 1: libc import
        Sym(f"{nid('sceKernelUsleep')}#{encode_id(1)}#{encode_id(1)}"),       # 2: kernel import
        Sym(f"{nid('strlen')}#{encode_id(2)}#{encode_id(1)}"),                # 3: LibcInternal
        Sym(f"{nid('__stack_chk_guard')}#{encode_id(1)}#{encode_id(1)}", type=STT_OBJECT),  # 4
        Sym("local_table", bind=STB_LOCAL, defined=True, value=0x10030),      # 5
    ],
    rels=[
        Rel(0x10000, R_RELATIVE, addend=0x40),
        Rel(0x10008, R_JUMP_SLOT, sym=1, plt=True),
        Rel(0x10010, R_JUMP_SLOT, sym=2, plt=True),
        Rel(0x10018, R_GLOB_DAT, sym=3),
        Rel(0x10020, R_GLOB_DAT, sym=4),
        Rel(0x10028, R_64, sym=5, addend=8),
    ],
)


class ConverterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        game = Path(cls.tmp.name)
        (game / "sce_module").mkdir()
        (game / "eboot.bin").write_bytes(build_module(EBOOT))
        (game / "sce_module" / "libc.prx").write_bytes(build_module(LIBC))
        cls.game = game
        modules = [sotcconv.Module("eboot", elfscan.parse(game / "eboot.bin")),
                   sotcconv.Module("libc", elfscan.parse(game / "sce_module" / "libc.prx"))]
        names = {nid("sceKernelUsleep"): "sceKernelUsleep",
                 nid("__stack_chk_guard"): "__stack_chk_guard"}
        cls.img = sotcconv.build(modules, names)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def relocs_at(self, module_index: int, offset: int):
        where = self.img.modules[module_index].base + offset
        return [r for r in self.img.relocs if r[0] == where]

    def test_layout(self):
        eboot, libc = self.img.modules
        self.assertEqual(eboot.base, 0)
        self.assertEqual(libc.base % sotcconv.PAGE, 0)
        self.assertGreaterEqual(libc.base, 0x10040)
        self.assertEqual(self.img.entry, 0x40)
        self.assertEqual(self.img.sdk_version, 0x04508101)
        self.assertEqual([m.tls_modid for m in self.img.modules], [1, 2])

    def test_cross_module_links(self):
        libc = self.img.modules[1]
        # eboot -> libc by (library, NID)
        self.assertEqual(self.relocs_at(0, 0x10008), [(0x10008, sotcconv.REL_IMAGE, libc.base + 0x20, 0)])
        # libSceLibcInternal -> libc by NID alone
        self.assertEqual(self.relocs_at(0, 0x10018), [(0x10018, sotcconv.REL_IMAGE, libc.base + 0x30, 0)])
        self.assertEqual(self.img.cross_links["eboot -> libc (libc)"], 1)
        self.assertEqual(self.img.cross_links["eboot -> libc (libSceLibcInternal)"], 1)

    def test_local_and_relative(self):
        self.assertEqual(self.relocs_at(0, 0x10000), [(0x10000, sotcconv.REL_IMAGE, 0x40, 0)])
        self.assertEqual(self.relocs_at(0, 0x10028), [(0x10028, sotcconv.REL_IMAGE, 0x10030, 8)])
        libc = self.img.modules[1]
        self.assertEqual(self.relocs_at(1, 0x10010), [(libc.base + 0x10010, sotcconv.REL_RAW, 2, 0)])

    def test_system_imports(self):
        index = {i.name: n for n, i in enumerate(self.img.imports)}
        kinds = {i.name: (i.library, i.kind) for i in self.img.imports}
        self.assertEqual(kinds, {"sceKernelUsleep": ("libkernel", sotcconv.IMP_FUNC),
                                 "__stack_chk_guard": ("libkernel", sotcconv.IMP_DATA)})
        self.assertEqual(self.relocs_at(0, 0x10010),
                         [(0x10010, sotcconv.REL_IMPORT, index["sceKernelUsleep"], 0)])
        self.assertEqual(self.relocs_at(0, 0x10020),
                         [(0x10020, sotcconv.REL_IMPORT, index["__stack_chk_guard"], 0)])

    def test_patch_sites_only_in_functions(self):
        sites = [(p.vaddr, p.kind) for p in self.img.patches]
        self.assertEqual(sites, [(0x40, codescan.PATCH_FS), (0x49, codescan.PATCH_EXTRQ_REG)])
        fs = self.img.patches[0]
        self.assertEqual((fs.a, fs.b, fs.c, fs.disp, fs.length), (0, 5, codescan.FS_ABS, 0, 9))
        ex = self.img.patches[1]
        self.assertEqual((ex.a, ex.b, ex.length), (0, 1, 4))

    def test_serialize_round_trip(self):
        blob = sotcconv.serialize(self.img)
        h = sotcconv.HEADER.unpack_from(blob, 0)
        (magic, version, nmod, size, entry, procparam, sdk, strings, nseg, nrel, nimp, npatch,
         _, _, data_off) = h
        self.assertEqual((magic, version, nmod), (sotcconv.MAGIC, sotcconv.VERSION, 2))
        self.assertEqual((size, entry, sdk), (self.img.size, 0x40, 0x04508101))
        self.assertEqual((nseg, nrel, nimp, npatch),
                         (len(self.img.segments), len(self.img.relocs), 2, 2))
        self.assertEqual(data_off % 16, 0)

        off = sotcconv.HEADER.size
        mods = [sotcconv.MODULE.unpack_from(blob, off + i * sotcconv.MODULE.size) for i in range(2)]
        libc = mods[1]
        self.assertEqual(libc[4], self.img.modules[1].base + 0x10)    # init
        self.assertEqual((libc[6], libc[7]), (4, 16))                  # tls filesz, memsz
        self.assertEqual(mods[0][9] != 0, True)                         # eh_frame_hdr present

        off += 2 * sotcconv.MODULE.size
        seg = sotcconv.SEGMENT.unpack_from(blob, off)
        img_off, file_off, filesz, memsz, prot = seg
        self.assertEqual(blob[file_off:file_off + len(EBOOT_CODE)], EBOOT_CODE)
        self.assertEqual(prot, 5)

    def test_cli(self):
        out = Path(self.tmp.name) / "boot.img"
        rc = sotcconv.main(["--game", str(self.game), "--module", "sce_module/libc.prx",
                            "-o", str(out)])
        self.assertEqual(rc, 0)
        self.assertEqual(out.read_bytes()[:8], sotcconv.MAGIC)


# A stand-in for the user's firmware libSceNgs2.sprx (never committed).
NGS2 = dataclasses.replace(LIBC, name="libSceNgs2", export_libs=[("libSceNgs2", 0)])


class FirmwareFolderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.game = root / "game"
        (cls.game / "sce_module").mkdir(parents=True)
        (cls.game / "eboot.bin").write_bytes(build_module(EBOOT))
        (cls.game / "sce_module" / "libc.prx").write_bytes(build_module(LIBC))
        cls.fw = root / "fw"
        cls.fw.mkdir()
        (cls.fw / "libSceNgs2.sprx").write_bytes(build_module(NGS2))
        cls.empty = root / "empty"
        cls.empty.mkdir()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def convert(self, *extra: str) -> tuple[str, str]:
        out = Path(self.tmp.name) / "boot.img"
        rep = Path(self.tmp.name) / "boot.txt"
        buf = io.StringIO()
        buf.reconfigure = lambda **_: None
        with contextlib.redirect_stdout(buf):
            rc = sotcconv.main(["--game", str(self.game), "--module", "sce_module/libc.prx",
                                *extra, "-o", str(out), "--report", str(rep)])
        self.assertEqual(rc, 0)
        return buf.getvalue(), rep.read_text(encoding="utf-8")

    def modules(self, report: str) -> list[str]:
        return [line.split()[1] for line in report.splitlines() if line.startswith("  module ")]

    def test_absolute_module_path(self):
        _, rep = self.convert("--module", str((self.fw / "libSceNgs2.sprx").resolve()))
        self.assertEqual(self.modules(rep), ["eboot", "libc", "libSceNgs2"])

    def test_fw_dir_links_ngs2(self):
        out, rep = self.convert("--fw-dir", str(self.fw))
        self.assertEqual(self.modules(rep), ["eboot", "libc", "libSceNgs2"])
        self.assertIn("firmware: linking libSceNgs2.sprx", out)

    def test_fw_dir_and_explicit_module_link_once(self):
        _, rep = self.convert("--module", str(self.fw / "libSceNgs2.sprx"), "--fw-dir", str(self.fw))
        self.assertEqual(self.modules(rep), ["eboot", "libc", "libSceNgs2"])

    def test_fw_dir_without_module(self):
        out, rep = self.convert("--fw-dir", str(self.empty))
        self.assertEqual(self.modules(rep), ["eboot", "libc"])
        self.assertIn("libSceNgs2.sprx not in", out)
        self.assertEqual(out.count("firmware:"), 1)

    def test_no_fw_dir(self):
        out, rep = self.convert()
        self.assertEqual(self.modules(rep), ["eboot", "libc"])
        self.assertIn("SOTC_FW_DIR unset", out)


if __name__ == "__main__":
    unittest.main()
