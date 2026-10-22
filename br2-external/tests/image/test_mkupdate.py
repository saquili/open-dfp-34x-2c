# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of board/dfp-34x-2c3/mkupdate.py, which packs an update file.

The layout itself is checked against the post-image script's output in
test_post_image.py, and against the stick's onu-update by the tests of
onu-utils; these are the refusals, and the time it takes.
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
BOARD = os.path.join(os.path.dirname(os.path.dirname(HERE)), "board", "dfp-34x-2c3")
SCRIPT = os.path.join(BOARD, "mkupdate.py")


class Refusals(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name
        self.kernel = self.file("kernel", b"\x27\x05\x19\x56 a kernel\n")
        self.rootfs = self.file("rootfs", b"hsqs a root filesystem\n")
        self.out = os.path.join(self.dir, "update.bin")

    def file(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as handle:
            handle.write(data)
        return path

    def pack(self, kernel=None, rootfs=None, version="0123456789ab", board="odi,dfp-34x-2c3",
             time=("--time", "1")):
        argv = [sys.executable, SCRIPT, "--board", board, "--version", version, *time,
                kernel or self.kernel, rootfs or self.rootfs, self.out]
        return subprocess.run(argv, capture_output=True, text=True)

    def test_the_time_is_the_kernel_image_s_unless_named(self):
        kernel = self.file("timed", b"\x27\x05\x19\x56" + bytes(4) + struct.pack(">I", 1760000000))
        done = self.pack(kernel=kernel, time=())
        self.assertEqual(done.returncode, 0, done.stderr)
        with open(self.out, "rb") as handle:
            self.assertEqual(struct.unpack(">Q", handle.read(32)[24:32])[0], 1760000000)

    def test_a_good_pair_packs(self):
        done = self.pack()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(os.path.exists(self.out))

    def test_a_kernel_that_is_no_uimage_is_refused(self):
        done = self.pack(kernel=self.file("bad", b"not a kernel"))
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("not a uImage", done.stderr)
        self.assertFalse(os.path.exists(self.out))

    def test_a_root_filesystem_that_is_no_squashfs_is_refused(self):
        done = self.pack(rootfs=self.file("bad", b"not a filesystem"))
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("not a SquashFS image", done.stderr)

    def test_texts_the_header_cannot_hold_are_refused(self):
        for version in ("", "x" * 64, "bad\tversion", "wersja-żółta"):
            with self.subTest(version=version):
                done = self.pack(version=version)
                self.assertNotEqual(done.returncode, 0)
                self.assertFalse(os.path.exists(self.out))
                self.assertFalse(os.path.exists(self.out + ".part"))


if __name__ == "__main__":
    unittest.main()
