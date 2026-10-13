# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of tools/kexec that need no board and no cross compiler.

The build script runs against a made-up image tree, in dry-run mode and for its
initramfs list; kexecboot is compiled for the host, where there is one with
the ELF header, and run on files that are not what it loads, which it refuses
before it asks the kernel for anything.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
KEXEC = os.path.dirname(HERE)
BUILD = os.path.join(KEXEC, "build")
SOURCE = os.path.join(KEXEC, "kexecboot.c")


def image_tree(root, libs=("ld-uClibc-1.0.99.so", "libuClibc-1.0.99.so"), busybox=True):
    """A made-up image build: a host toolchain and a target tree."""
    os.makedirs(os.path.join(root, "host", "bin"))
    os.makedirs(os.path.join(root, "target", "bin"))
    os.makedirs(os.path.join(root, "target", "lib"))
    os.makedirs(os.path.join(root, "images"))
    tool = os.path.join(root, "host", "bin", "mips-buildroot-linux-uclibc-gcc")
    with open(tool, "w") as f:
        f.write("#!/bin/sh\n")
    os.chmod(tool, 0o755)
    if busybox:
        path = os.path.join(root, "target", "bin", "busybox")
        with open(path, "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(path, 0o755)
    for lib in libs:
        with open(os.path.join(root, "target", "lib", lib), "w") as f:
            f.write("x")


def run_build(*args, **env):
    environ = dict(os.environ)
    environ.update(env)
    return subprocess.run(["sh", BUILD, *args], capture_output=True, text=True, env=environ)


class Build(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.image = os.path.join(self.tmp, "image")
        self.out = os.path.join(self.tmp, "out")

    def test_the_script_parses(self):
        r = subprocess.run(["sh", "-n", BUILD], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_the_list_names_busybox_and_the_libc_the_tree_has(self):
        image_tree(self.image)
        r = run_build("--list", os.path.join(self.image, "target"), KEXEC_OUT=self.out)
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = r.stdout.splitlines()
        target = os.path.join(self.image, "target")
        self.assertIn(f"file /bin/busybox {target}/bin/busybox 0755 0 0", lines)
        self.assertIn(f"file /lib/ld-uClibc-1.0.99.so {target}/lib/ld-uClibc-1.0.99.so 0755 0 0", lines)
        self.assertIn("slink /lib/ld-uClibc.so.1 ld-uClibc-1.0.99.so 0777 0 0", lines)
        self.assertIn("slink /lib/libc.so.0 libuClibc-1.0.99.so 0777 0 0", lines)
        self.assertIn(f"file /init {self.out}/init 0755 0 0", lines)
        self.assertIn("nod /dev/console 0600 0 0 c 5 1", lines)

    def test_the_list_refuses_a_tree_without_busybox_or_without_a_libc(self):
        image_tree(self.image, busybox=False)
        r = run_build("--list", os.path.join(self.image, "target"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("busybox", r.stderr)
        other = os.path.join(self.tmp, "other")
        image_tree(other, libs=("ld-uClibc-1.0.99.so",))
        r = run_build("--list", os.path.join(other, "target"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("libuClibc", r.stderr)

    def test_a_dry_run_names_the_kernel_options_and_links_statically(self):
        image_tree(self.image)
        r = run_build("--dry-run", IMAGE_OUT=self.image, KEXEC_OUT=self.out)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout
        self.assertIn("--enable KEXEC --disable CRASH_DUMP", out)
        self.assertIn("RAM_DIAG=no", out)
        self.assertIn("make ram-image", out)
        self.assertIn("ARCH=mips", out)
        self.assertIn("--enable BLK_DEV_INITRD", out)
        self.assertIn(f"--set-str INITRAMFS_SOURCE {self.out}/tiny.list", out)
        self.assertIn("--set-str CMDLINE console=ttyS0,115200 panic=10", out)
        self.assertIn("-static", out)
        self.assertIn("kexecboot.c", out)
        self.assertFalse(os.path.exists(self.out), "a dry run writes nothing")

    def test_without_an_image_build_it_says_to_make_one(self):
        r = run_build("--dry-run", IMAGE_OUT=self.image)
        self.assertEqual(r.returncode, 2)
        self.assertIn("make image", r.stderr)

    def test_unknown_arguments_are_refused(self):
        r = run_build("--nonsense")
        self.assertEqual(r.returncode, 2)
        self.assertIn("usage", r.stderr)


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("cc"),
                     "kexecboot needs <elf.h> and a Linux host compiler")
class Kexecboot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.exe = os.path.join(cls.tmp, "kexecboot")
        r = subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-o", cls.exe, SOURCE],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def refused(self, content, text):
        path = os.path.join(self.tmp, "image")
        with open(path, "wb") as f:
            f.write(content)
        r = subprocess.run([self.exe, path], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(text, r.stderr)

    def header(self, cls=1, data=2, machine=8, phoff=52, phnum=1, phentsize=32):
        # The program reads the header's fields in the byte order of the machine
        # it runs on, which on the board is the big-endian order of the ELF; the
        # test writes them in the order of the host it runs on, whose ident
        # still says big-endian, so that the checks after the ident can be seen.
        ident = b"\x7fELF" + bytes([cls, data, 1]) + bytes(9)
        return ident + struct.pack("=HHIIIIIHHHHHH", 2, machine, 1, 0x80100000, phoff, 0, 0,
                                   52, phentsize, phnum, 40, 0, 0)

    def test_it_compiles_without_a_warning(self):
        self.assertTrue(os.path.exists(self.exe))

    def test_usage_without_an_argument(self):
        r = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("usage", r.stderr)

    def test_a_file_that_is_not_an_elf_is_refused(self):
        self.refused(b"x" * 100, "no ELF magic")
        self.refused(b"short", "not an ELF")

    def test_only_a_32_bit_big_endian_mips_elf_is_taken(self):
        self.refused(self.header(cls=2) + bytes(64), "32-bit big-endian")
        self.refused(self.header(data=1) + bytes(64), "32-bit big-endian")
        self.refused(self.header(machine=40) + bytes(64), "not MIPS")

    def test_program_headers_that_cannot_be_trusted_are_refused(self):
        self.refused(self.header(phnum=0) + bytes(64), "no program headers")
        self.refused(self.header(phentsize=20) + bytes(64), "program headers")
        self.refused(self.header(phoff=4000) + bytes(64), "past the end")

    def test_an_image_with_nothing_to_load_is_refused(self):
        # One program header of type PT_NULL: there is no PT_LOAD to load.
        phdr = struct.pack("=IIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0)
        self.refused(self.header() + phdr, "no PT_LOAD")


if __name__ == "__main__":
    unittest.main()
