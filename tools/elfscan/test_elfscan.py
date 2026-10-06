# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for elfscan, using a synthetic PS4-style ELF built in memory."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).parent.parent / "common")]
import elfscan  # noqa: E402
from testelf import StrTab, modval, wrap_fself  # noqa: E402

# NIDs as registered by shadPS4 (src/core/libraries/kernel/*.cpp).
KNOWN_NIDS = {
    "__stack_chk_guard": "f7uOxY9mM1U",
    "sceKernelUsleep": "1jfXLRVzisc",
}


def build_elf(path: Path) -> None:
    usleep, guard = KNOWN_NIDS["sceKernelUsleep"], KNOWN_NIDS["__stack_chk_guard"]

    st = StrTab()
    mod_kernel = st.add("libkernel")
    mod_self = st.add("eboot")
    lib_kernel = st.add("libkernel")
    lib_gnm = st.add("libSceGnmDriver")
    needed = st.add("libkernel.prx")
    sym_names = [
        st.add(f"{usleep}#B#B"),       # libkernel / libkernel, function
        st.add(f"{guard}#B#B"),        # libkernel / libkernel, object
        st.add("xbxNatawohc#C#B"),     # libSceGnmDriver, not in our names list
        st.add("main"),                # defined export
    ]

    # Symbol table: null, 3 imports, 1 definition.
    symtab = bytearray(24)
    for i, (name, info, shndx) in enumerate(
            [(sym_names[0], 0x12, 0), (sym_names[1], 0x11, 0), (sym_names[2], 0x12, 0),
             (sym_names[3], 0x12, 1)]):
        symtab += struct.pack("<IBBHQQ", name, info, 0, shndx, 0x1000 if shndx else 0, 0)

    rela = struct.pack("<QQq", 0x2000, (2 << 32) | 6, 0) + struct.pack("<QQq", 0x2008, 8, 0x1000)
    jmprel = struct.pack("<QQq", 0x2010, (1 << 32) | 7, 0) + struct.pack("<QQq", 0x2018, (3 << 32) | 7, 0)

    dynlib = bytearray(st.blob)
    sym_off = len(dynlib); dynlib += symtab
    rela_off = len(dynlib); dynlib += rela
    jmp_off = len(dynlib); dynlib += jmprel

    dyn_entries = [
        (elfscan.DT_NEEDED, needed),
        (elfscan.DT_SCE_MODULE_INFO, modval(mod_self, 1, 0, 0)),
        (elfscan.DT_SCE_NEEDED_MODULE, modval(mod_kernel, 1, 1, 1)),
        (elfscan.DT_SCE_IMPORT_LIB, modval(lib_kernel, 1, 0, 1)),
        (elfscan.DT_SCE_IMPORT_LIB, modval(lib_gnm, 1, 0, 2)),
        (elfscan.DT_SCE_STRTAB, 0), (elfscan.DT_SCE_STRSZ, len(st.blob)),
        (elfscan.DT_SCE_SYMTAB, sym_off), (elfscan.DT_SCE_SYMTABSZ, len(symtab)),
        (elfscan.DT_SCE_RELA, rela_off), (elfscan.DT_SCE_RELASZ, len(rela)),
        (elfscan.DT_SCE_JMPREL, jmp_off), (elfscan.DT_SCE_PLTRELSZ, len(jmprel)),
        (elfscan.DT_NULL, 0),
    ]
    dynamic = b"".join(struct.pack("<qQ", t, v) for t, v in dyn_entries)

    # Code: one EXTRQ, one INSERTQ, one mov rax, fs:[0], padded with NOPs.
    code = (b"\x90" * 4 + b"\x66\x0f\x78\xc0\x08\x00" + b"\xf2\x0f\x79\xc1"
            + b"\x64\x48\x8b\x04\x25\x00\x00\x00\x00" + b"\xc3")
    procparam = struct.pack("<QIIQ", 24, 0x4942524F, 1, 0x05050001)

    phnum = 4
    code_off = 64 + phnum * 56
    dyn_off = code_off + len(code)
    dynlib_off = dyn_off + len(dynamic)
    pp_off = dynlib_off + len(dynlib)

    phdrs = b"".join([
        struct.pack("<IIQQQQQQ", elfscan.PT_LOAD, 5, code_off, 0x400000, 0, len(code), len(code), 0x4000),
        struct.pack("<IIQQQQQQ", elfscan.PT_DYNAMIC, 4, dyn_off, 0, 0, len(dynamic), len(dynamic), 8),
        struct.pack("<IIQQQQQQ", elfscan.PT_SCE_DYNLIBDATA, 4, dynlib_off, 0, 0, len(dynlib), 0, 16),
        struct.pack("<IIQQQQQQ", elfscan.PT_SCE_PROCPARAM, 4, pp_off, 0x500000, 0, len(procparam),
                    len(procparam), 8),
    ])
    ident = b"\x7fELF" + bytes([2, 1, 1, 9]) + bytes(8)
    header = ident + struct.pack("<HHIQQQIHHHHHH", 0xFE10, 0x3E, 1, 0x400004, 64, 0, 0, 64, 56,
                                 phnum, 64, 0, 0)
    path.write_bytes(header + phdrs + code + dynamic + bytes(dynlib) + procparam)


class NidTests(unittest.TestCase):
    def test_known_nids(self):
        for name, nid in KNOWN_NIDS.items():
            self.assertEqual(elfscan.name_to_nid(name), nid, name)

    def test_decode_id(self):
        self.assertEqual(elfscan.decode_id("A"), 0)
        self.assertEqual(elfscan.decode_id("B"), 1)
        self.assertEqual(elfscan.decode_id("BA"), 64)


class ParseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.elf = Path(cls.tmp.name) / "eboot.bin"
        build_elf(cls.elf)
        cls.info = elfscan.parse(cls.elf)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_header(self):
        self.assertEqual(self.info.elf_type, "ET_SCE_EXEC_ASLR")
        self.assertEqual(self.info.entry, 0x400004)
        self.assertEqual(self.info.sdk_version, 0x05050001)

    def test_modules_and_libs(self):
        self.assertEqual(self.info.needed_files, ["libkernel.prx"])
        self.assertEqual([(m.name, m.version, m.id) for m in self.info.needed_modules],
                         [("libkernel", "1.1", 1)])
        self.assertEqual([lib.name for lib in self.info.import_libs], ["libkernel", "libSceGnmDriver"])
        self.assertEqual(self.info.module_info.name, "eboot")

    def test_imports(self):
        imports = {s.nid: s for s in self.info.imports}
        self.assertEqual(set(imports), {"1jfXLRVzisc", "f7uOxY9mM1U", "xbxNatawohc"})
        self.assertEqual(imports["1jfXLRVzisc"].library, "libkernel")
        self.assertEqual(imports["1jfXLRVzisc"].module, "libkernel")
        self.assertEqual(imports["f7uOxY9mM1U"].type, "OBJECT")
        self.assertEqual(imports["xbxNatawohc"].library, "libSceGnmDriver")
        self.assertEqual([s.raw_name for s in self.info.exports], ["main"])

    def test_relocations(self):
        self.assertEqual(self.info.relocations["R_X86_64_JUMP_SLOT"], 2)
        self.assertEqual(self.info.relocations["R_X86_64_GLOB_DAT"], 1)
        self.assertEqual(self.info.relocations["R_X86_64_RELATIVE"], 1)

    def test_patterns(self):
        self.assertEqual(self.info.patterns["sse4a_extrq"], [0x400004])
        self.assertEqual(self.info.patterns["sse4a_insertq"], [0x40000A])
        self.assertEqual(self.info.patterns["fs_tls_base_read"], [0x40000E])

    def test_report_names_and_shadps4(self):
        names = {elfscan.name_to_nid("sceKernelUsleep"): "sceKernelUsleep"}
        shad = {"f7uOxY9mM1U": ("libkernel", "g_stack_chk_guard")}
        report = elfscan.build_report(self.info, names, shad)
        self.assertEqual(report["summary"], {"imports": 3, "named": 2, "in_shadps4": 1, "exports": 1})
        for fmt in ("text", "md"):
            self.assertIn("sceKernelUsleep", elfscan.render_text(report, markdown=fmt == "md"))

    def test_fake_self_matches_plain_elf(self):
        path = Path(self.tmp.name) / "eboot_fself.bin"
        path.write_bytes(wrap_fself(self.elf.read_bytes()))
        info = elfscan.parse(path)
        self.assertEqual(info.sdk_version, self.info.sdk_version)
        self.assertEqual([s.raw_name for s in info.symbols], [s.raw_name for s in self.info.symbols])
        self.assertEqual(info.relocations, self.info.relocations)
        self.assertEqual(info.patterns, self.info.patterns)

    def test_rejects_encrypted_self(self):
        path = Path(self.tmp.name) / "encrypted.bin"
        path.write_bytes(wrap_fself(self.elf.read_bytes(), extra_flags=elfscan.SELF_SEG_ENCRYPTED))
        with self.assertRaisesRegex(elfscan.ElfError, "encrypted retail SELF"):
            elfscan.parse(path)

    def test_rejects_garbage_self(self):
        path = Path(self.tmp.name) / "garbage.bin"
        path.write_bytes(elfscan.SELF_MAGIC + bytes(60))
        with self.assertRaises(elfscan.ElfError):
            elfscan.parse(path)


class ShadPs4ScrapeTests(unittest.TestCase):
    def test_lib_function_formats(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "src"
            src.mkdir()
            (src / "kernel.cpp").write_text(
                'LIB_FUNCTION("1jfXLRVzisc", "libkernel", 1, "libkernel", 1, 1, sceKernelUsleep);\n'
                'LIB_OBJ("f7uOxY9mM1U", "libkernel", 1, "libkernel", &g_stack_chk_guard);\n'
                'LIB_FUNCTION("xbxNatawohc", "libSceGnmDriver", 1, "libSceGnmDriver",\n'
                '             sceGnmSubmitDone);\n')
            known = elfscan.load_shadps4(Path(tmp))
        self.assertEqual(known["1jfXLRVzisc"], ("libkernel", "sceKernelUsleep"))
        self.assertEqual(known["f7uOxY9mM1U"], ("libkernel", "g_stack_chk_guard"))
        self.assertEqual(known["xbxNatawohc"], ("libSceGnmDriver", "sceGnmSubmitDone"))


if __name__ == "__main__":
    unittest.main()
