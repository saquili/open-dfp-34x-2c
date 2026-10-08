# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of tools/lib-trim.

The parsers are tested on text that readelf prints. The end-to-end tests build
a small stand-in for uClibc-ng's build directory and a root filesystem with a
MIPS cross compiler (gcc-mips-linux-gnu on Debian, or another named by
CROSS_COMPILE) and run the tool as a program; without one they are skipped.
"""

import filecmp
import hashlib
import importlib.machinery
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "lib-trim")
CROSS = os.environ.get("CROSS_COMPILE", "mips-linux-gnu-")
HAVE_CROSS = all(shutil.which(CROSS + tool) for tool in ("gcc", "readelf", "strip", "ar"))


def load_tool():
    loader = importlib.machinery.SourceFileLoader("lib_trim", TOOL)
    spec = importlib.util.spec_from_loader("lib_trim", loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules["lib_trim"] = module  # the dataclasses look their module up there
    loader.exec_module(module)
    return module


lt = load_tool()

READELF_PROGRAM = """\
ELF Header:
  Class:                             ELF32
  Type:                              EXEC (Executable file)
  Flags:                             0x1007, noreorder, pic, cpic, o32, mips1

Program Headers:
  Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
  INTERP         0x000174 0x00400174 0x00400174 0x00014 0x00014 R   0x1
      [Requesting program interpreter: /lib/ld-uClibc.so.0]
  LOAD           0x000000 0x00400000 0x00400000 0x0a0c0 0x0a0c0 R E 0x10000

Dynamic section at offset 0x18c contains 4 entries:
  Tag        Type                         Name/Value
 0x00000001 (NEEDED)                     Shared library: [libmnl.so.0]
 0x00000001 (NEEDED)                     Shared library: [libc.so.0]
 0x70000013 (MIPS_GOTSYM)                0x4
 0x00000000 (NULL)                       0x0

Relocation section '.rel.dyn' at offset 0x3d0 contains 2 entries:
 Offset     Info    Type                Sym.Value  Sym. Name
00000000  00000000 R_MIPS_NONE
0041a010  00000103 R_MIPS_REL32           00000000   environ

Symbol table '.dynsym' contains 7 entries:
   Num:    Value  Size Type    Bind   Vis      Ndx Name
     0: 00000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 00000000     0 OBJECT  GLOBAL DEFAULT  UND environ
     2: 00000000     0 OBJECT  GLOBAL DEFAULT  UND __gnu_local_gp
     3: 00400800    28 FUNC    GLOBAL DEFAULT   11 main
     4: 00400900     0 FUNC    GLOBAL DEFAULT  UND mnl_socket_open@LIBMNL_1.0 (2)
     5: 00000000     0 NOTYPE  WEAK   DEFAULT  UND __gmon_start__
     6: 00400a00     0 FUNC    GLOBAL DEFAULT  UND printf
"""

READELF_LIBRARY = """\
ELF Header:
  Type:                              DYN (Shared object file)

Dynamic section at offset 0x18c contains 3 entries:
 0x00000001 (NEEDED)                     Shared library: [libc.so.0]
 0x0000000e (SONAME)                     Library soname: [libmnl.so.0]
 0x00000000 (NULL)                       0x0

Symbol table '.dynsym' contains 3 entries:
   Num:    Value  Size Type    Bind   Vis      Ndx Name
     0: 00000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 00001234    96 FUNC    GLOBAL DEFAULT   10 mnl_socket_open@@LIBMNL_1.0
     2: 00002000     4 OBJECT  WEAK   DEFAULT   15 mnl_weak_thing
"""


class Parsing(unittest.TestCase):
    def test_a_program_gives_its_interpreter_its_libraries_and_its_symbols(self):
        elf = lt.parse_readelf(READELF_PROGRAM)
        self.assertEqual(elf.kind, "EXEC")
        self.assertEqual(elf.interp, "/lib/ld-uClibc.so.0")
        self.assertEqual(elf.needed, ["libmnl.so.0", "libc.so.0"])
        self.assertIsNone(elf.soname)
        self.assertEqual(elf.defined, {"main"})
        self.assertEqual(elf.undefined, {"environ", "__gnu_local_gp", "mnl_socket_open", "printf"})
        self.assertEqual(elf.weak_undefined, {"__gmon_start__"})

    def test_the_loader_resolves_what_a_relocation_or_the_global_got_names(self):
        # environ by its relocation; mnl_socket_open and printf from MIPS_GOTSYM (4) on;
        # __gnu_local_gp (2) neither, so it is never looked up.
        self.assertEqual(lt.parse_readelf(READELF_PROGRAM).resolved,
                         {"environ", "mnl_socket_open", "printf"})

    def test_a_library_gives_its_soname_and_its_versioned_names_plainly(self):
        elf = lt.parse_readelf(READELF_LIBRARY)
        self.assertEqual(elf.kind, "DYN")
        self.assertEqual(elf.soname, "libmnl.so.0")
        self.assertEqual(elf.needed, ["libc.so.0"])
        self.assertEqual(elf.defined, {"mnl_socket_open", "mnl_weak_thing"})
        self.assertFalse(elf.is_program)

    def test_the_link_follows_the_c_librarys_configuration(self):
        flags = lt.link_flags({"UCLIBC_BUILD_RELRO": "y", "UCLIBC_BUILD_NOW": "y"})
        self.assertIn("-Wl,-z,relro", flags)
        self.assertIn("-Wl,-z,now", flags)
        self.assertIn("-Wl,-z,defs", flags)
        self.assertNotIn("-Wl,--hash-style=gnu", flags)
        flags = lt.link_flags({"LDSO_GNU_HASH_SUPPORT": "y"})
        self.assertNotIn("-Wl,-z,relro", flags)
        self.assertIn("-Wl,--hash-style=gnu", flags)


class Paths(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name
        os.makedirs(os.path.join(self.root, "lib"))
        os.makedirs(os.path.join(self.root, "usr", "lib"))
        with open(os.path.join(self.root, "lib", "libuClibc-1.0.0.so"), "w") as handle:
            handle.write("x")

    def test_links_are_followed_inside_the_root_absolute_ones_from_the_root(self):
        os.symlink("libuClibc-1.0.0.so", os.path.join(self.root, "lib", "libc.so.0"))
        os.symlink("/lib/libc.so.0", os.path.join(self.root, "usr", "lib", "libc.so"))
        self.assertEqual(lt.resolve(self.root, "lib/libc.so.0"), "lib/libuClibc-1.0.0.so")
        self.assertEqual(lt.resolve(self.root, "usr/lib/libc.so"), "lib/libuClibc-1.0.0.so")
        self.assertEqual(lt.find_library(self.root, "libc.so"), "lib/libuClibc-1.0.0.so")
        self.assertIsNone(lt.find_library(self.root, "libnothere.so.1"))

    def test_a_loop_of_links_is_an_error(self):
        os.symlink("b", os.path.join(self.root, "lib", "a"))
        os.symlink("a", os.path.join(self.root, "lib", "b"))
        with self.assertRaises(lt.TrimError):
            lt.resolve(self.root, "lib/a")


class Usage(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)

    def test_the_options_are_required(self):
        self.assertEqual(self.run_tool().returncode, 2)
        self.assertEqual(self.run_tool("--cross", "x-", "/").returncode, 2)

    def test_missing_cross_tools_are_an_error(self):
        done = self.run_tool("--cross", "/nonexistent/x-", "--uclibc", "/nonexistent", "/")
        self.assertEqual(done.returncode, 2)
        self.assertIn("no /nonexistent/x-gcc", done.stderr)


# --------------------------------------------------------------------------
# End to end, with a cross compiler
# --------------------------------------------------------------------------

LIBC_SOURCES = {
    # uClibc-ng's initializer, which its link names with -init.
    "init.c": "int __libc_ready; void __uClibc_init(void) { __libc_ready = 1; }\n",
    "fa.c": "int fb(void); int fa(void) { return fb() + 1; }\n",
    "fb.c": "int fb(void) { return 2; }\n",
    "fc.c": "int fc(void) { return 3; }\n",
    "fz.c": "int fz(int x) { return x * 2; }\n",
}


def sha(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


@unittest.skipUnless(HAVE_CROSS, "no MIPS cross compiler (gcc-mips-linux-gnu, or CROSS_COMPILE)")
class EndToEnd(unittest.TestCase):
    """A stand-in uClibc-ng build directory and a root filesystem that uses it.

    The C library exports fa, fb, fc and fz. The program imports fa (which needs
    fb) and loads libz.so.1, which imports fz; nothing imports fc. libunused.so.1
    is loaded by nothing.
    """

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        base = cls._tmp.name
        cls.uclibc = os.path.join(base, "uclibc")
        cls.pristine = os.path.join(base, "root")
        work = os.path.join(base, "work")
        lib = os.path.join(cls.uclibc, "lib")
        for directory in (lib, os.path.join(cls.uclibc, "libc"), work,
                          os.path.join(cls.pristine, "lib"), os.path.join(cls.pristine, "usr", "lib"),
                          os.path.join(cls.pristine, "usr", "bin")):
            os.makedirs(directory)
        with open(os.path.join(cls.uclibc, ".config"), "w", encoding="utf-8") as handle:
            handle.write("UCLIBC_BUILD_RELRO=y\nUCLIBC_BUILD_NOW=y\n# DODEBUG is not set\n")

        def source(name, text):
            path = os.path.join(work, name)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
            return path

        def cc(*args):
            subprocess.run([CROSS + "gcc", "-O2", *args], check=True, capture_output=True)

        # The pieces of uClibc-ng's link.
        members = []
        for name, text in sorted(LIBC_SOURCES.items()):
            out = os.path.join(work, name[:-2] + ".os")
            cc("-fPIC", "-c", "-o", out, source(name, text))
            members.append(out)
        archive = os.path.join(cls.uclibc, "libc", "libc_so.a")
        subprocess.run([CROSS + "ar", "rcs", archive, *members], check=True)
        cc("-fPIC", "-c", "-o", os.path.join(lib, "interp.os"),
           source("interp.c", 'const char __dl_interp[] = "/lib/ld-uClibc.so.0";\n'))
        cc("-fPIC", "-c", "-o", os.path.join(lib, "ehframe-term.o"), source("term.c", ""))
        cc("-fPIC", "-c", "-o", os.path.join(work, "nonshared.os"),
           source("nonshared.c", "int __nonshared_helper(void) { return 0; }\n"))
        subprocess.run([CROSS + "ar", "rcs", os.path.join(lib, "uclibc_nonshared.a"),
                        os.path.join(work, "nonshared.os")], check=True)
        loader = os.path.join(lib, "ld-uClibc-1.0.0.so")
        cc("-shared", "-fPIC", "-nostdlib", "-Wl,-soname=ld-uClibc.so.1", "-o", loader,
           source("ld.c", "int _dl_ready(void) { return 1; }\n"))
        os.symlink("ld-uClibc-1.0.0.so", os.path.join(lib, "ld-uClibc.so.1"))
        full = os.path.join(lib, "libuClibc-1.0.0.so")
        tools = lt.Tools(CROSS)
        cc(*lt.link_flags(lt.read_config(os.path.join(cls.uclibc, ".config"))),
           "-Wl,-init,__uClibc_init", "-Wl,-soname=libc.so.0", "-nostdlib", "-nostartfiles",
           "-o", full, "-Wl,--whole-archive", archive, "-Wl,--no-whole-archive",
           os.path.join(lib, "interp.os"), loader, os.path.join(lib, "uclibc_nonshared.a"),
           tools.run("gcc", "-print-libgcc-file-name").strip(), os.path.join(lib, "ehframe-term.o"))

        # The root filesystem: the libraries stripped, as the build system leaves them.
        root = cls.pristine
        rootlib = os.path.join(root, "lib")
        tools.run("strip", "--remove-section=.comment", "--remove-section=.note", "-o",
                  os.path.join(rootlib, "libuClibc-1.0.0.so"), full)
        shutil.copy(loader, rootlib)
        os.symlink("libuClibc-1.0.0.so", os.path.join(rootlib, "libc.so.0"))
        os.symlink("ld-uClibc-1.0.0.so", os.path.join(rootlib, "ld-uClibc.so.0"))
        os.symlink("ld-uClibc-1.0.0.so", os.path.join(rootlib, "ld-uClibc.so.1"))
        usrlib = os.path.join(root, "usr", "lib")
        libz = os.path.join(usrlib, "libz.so.1.0")
        cc("-shared", "-fPIC", "-nostdlib", "-Wl,-soname=libz.so.1", "-o", libz,
           source("z.c", "int fz(int); int zip(int x) { return fz(x); }\n"), full)
        os.symlink("libz.so.1.0", os.path.join(usrlib, "libz.so.1"))
        cc("-shared", "-fPIC", "-nostdlib", "-Wl,-soname=libunused.so.1",
           "-o", os.path.join(usrlib, "libunused.so.1"),
           source("unused.c", "int fc(void); int unused(void) { return fc(); }\n"), full)
        os.symlink("libunused.so.1", os.path.join(usrlib, "libunused.so"))
        cls.program_source = source(
            "prog.c", "int fa(void); int zip(int);\n"
                      "void __start(void) { zip(fa()); for (;;); }\n")
        cls.link_program(os.path.join(root, "usr", "bin", "prog"), cls.program_source,
                         full, libz)

    @classmethod
    def link_program(cls, out, src, *libs):
        subprocess.run([CROSS + "gcc", "-O2", "-fno-pie", "-no-pie", "-nostdlib",
                        "-Wl,-dynamic-linker,/lib/ld-uClibc.so.0", "-o", out, src, *libs],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def setUp(self):
        self._root = tempfile.TemporaryDirectory()
        self.addCleanup(self._root.cleanup)
        self.root = os.path.join(self._root.name, "root")
        shutil.copytree(self.pristine, self.root, symlinks=True)

    def trim(self, *extra):
        return subprocess.run([sys.executable, TOOL, "--cross", CROSS, "--uclibc", self.uclibc,
                               *extra, self.root], capture_output=True, text=True)

    def exports(self, rel):
        return lt.read_elf(lt.Tools(CROSS), self.root, rel).defined

    def test_the_c_library_keeps_what_is_imported_and_what_that_needs(self):
        done = self.trim()
        self.assertEqual(done.returncode, 0, done.stderr)
        exported = self.exports("lib/libuClibc-1.0.0.so")
        self.assertTrue({"fa", "fb", "fz", "__uClibc_init"} <= exported, exported)
        self.assertNotIn("fc", exported)
        self.assertIn("cut lib/libuClibc-1.0.0.so", done.stdout)
        self.assertIn("1 programs checked", done.stdout)

    def test_the_new_c_library_keeps_the_initializer_and_the_soname(self):
        self.assertEqual(self.trim().returncode, 0)
        out = subprocess.run([CROSS + "readelf", "-dW",
                              os.path.join(self.root, "lib", "libuClibc-1.0.0.so")],
                             capture_output=True, text=True, check=True).stdout
        self.assertIn("(INIT)", out)
        self.assertIn("Library soname: [libc.so.0]", out)
        self.assertIn("Flags: NOW", out)

    def test_a_library_nothing_loads_goes_with_its_links_and_the_rest_stays(self):
        done = self.trim()
        self.assertEqual(done.returncode, 0, done.stderr)
        usrlib = os.path.join(self.root, "usr", "lib")
        self.assertFalse(os.path.lexists(os.path.join(usrlib, "libunused.so.1")))
        self.assertFalse(os.path.lexists(os.path.join(usrlib, "libunused.so")))
        self.assertTrue(os.path.isfile(os.path.join(usrlib, "libz.so.1.0")))
        self.assertTrue(os.path.islink(os.path.join(usrlib, "libz.so.1")))
        self.assertTrue(os.path.isfile(os.path.join(self.root, "lib", "ld-uClibc-1.0.0.so")))
        self.assertIn("removed usr/lib/libunused.so.1 and usr/lib/libunused.so", done.stdout)

    def test_a_second_run_changes_nothing(self):
        self.assertEqual(self.trim().returncode, 0)
        first = sha(os.path.join(self.root, "lib", "libuClibc-1.0.0.so"))
        self.assertEqual(self.trim().returncode, 0)
        self.assertEqual(sha(os.path.join(self.root, "lib", "libuClibc-1.0.0.so")), first)

    def test_a_dry_run_reports_and_changes_nothing(self):
        done = self.trim("-n")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("would remove usr/lib/libunused.so.1", done.stdout)
        self.assertIn("would cut lib/libuClibc-1.0.0.so", done.stdout)
        comparison = filecmp.dircmp(self.pristine, self.root)
        self.assertEqual(comparison.diff_files + comparison.left_only + comparison.right_only, [])
        self.assertTrue(filecmp.cmp(os.path.join(self.pristine, "lib", "libuClibc-1.0.0.so"),
                                    os.path.join(self.root, "lib", "libuClibc-1.0.0.so"),
                                    shallow=False))

    def test_quiet_prints_nothing_when_all_is_well(self):
        done = self.trim("-q")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout + done.stderr, "")

    def test_a_library_a_program_needs_and_the_tree_lacks_fails_and_changes_nothing(self):
        os.remove(os.path.join(self.root, "usr", "lib", "libz.so.1"))
        done = self.trim()
        self.assertEqual(done.returncode, 1)
        self.assertIn("usr/bin/prog: needs libz.so.1", done.stderr)
        self.assertTrue(os.path.lexists(os.path.join(self.root, "usr", "lib", "libunused.so.1")))

    def test_a_symbol_no_loaded_object_defines_fails(self):
        # libz.so.1 replaced by one that no longer defines zip.
        libz = os.path.join(self.root, "usr", "lib", "libz.so.1.0")
        subprocess.run([CROSS + "gcc", "-shared", "-fPIC", "-nostdlib", "-Wl,-soname=libz.so.1",
                        "-o", libz, "-x", "c", "-", "-x", "none",
                        os.path.join(self.root, "lib", "libc.so.0")],
                       input="int fz(int); int unzip(int x) { return fz(x); }\n", text=True,
                       check=True, capture_output=True)
        done = self.trim()
        self.assertEqual(done.returncode, 1)
        self.assertIn("usr/bin/prog: zip, which usr/bin/prog imports, is defined by none",
                      done.stderr)

    def test_a_build_directory_of_another_c_library_is_an_error(self):
        os.rename(os.path.join(self.root, "lib", "libuClibc-1.0.0.so"),
                  os.path.join(self.root, "lib", "libuClibc-1.0.1.so"))
        os.remove(os.path.join(self.root, "lib", "libc.so.0"))
        os.symlink("libuClibc-1.0.1.so", os.path.join(self.root, "lib", "libc.so.0"))
        done = self.trim()
        self.assertEqual(done.returncode, 2)
        self.assertIn("libuClibc-1.0.1.so", done.stderr)


if __name__ == "__main__":
    unittest.main()
