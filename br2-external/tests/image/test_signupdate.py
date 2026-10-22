# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of board/dfp-34x-2c3/signupdate.py, which signs an update file.

The files are packed by mkupdate.py, and the keys made by openssl, which the
script signs with; onu-utils' tests check the same signatures with the
stick's onu-update. The tests are skipped without openssl.
"""

import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.realpath(__file__))
BOARD = os.path.join(os.path.dirname(os.path.dirname(HERE)), "board", "dfp-34x-2c3")
MKUPDATE = os.path.join(BOARD, "mkupdate.py")
SCRIPT = os.path.join(BOARD, "signupdate.py")
KERNEL_TIME = 1760000000


@unittest.skipUnless(shutil.which("openssl"), "openssl is not installed")
class Signatures(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name
        kernel = b"\x27\x05\x19\x56" + bytes(4) + struct.pack(">I", KERNEL_TIME) + bytes(500)
        squash = bytearray(b"hsqs" + bytes(3000))
        squash[40:48] = struct.pack("<Q", len(squash))
        self.squash = bytes(squash)
        k, r = self.file("kernel", kernel), self.file("rootfs", self.squash)
        self.unsigned = os.path.join(self.dir, "update.bin")
        subprocess.run([sys.executable, MKUPDATE, "--board", "odi,dfp-34x-2c3", "--version",
                        "2026.10.22-1200", k, r, self.unsigned], check=True)
        self.key = self.make_key("release")
        self.keys = self.file("keys", self.run_script("key", self.key, "the release key").stdout)

    def file(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb" if isinstance(data, bytes) else "w") as handle:
            handle.write(data)
        return path

    def make_key(self, name):
        path = os.path.join(self.dir, name + ".pem")
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", path], check=True,
                       capture_output=True)
        return path

    def run_script(self, *args):
        return subprocess.run([sys.executable, SCRIPT, *args], capture_output=True, text=True)

    def sign(self, key=None, src=None, name="signed.bin"):
        out = os.path.join(self.dir, name)
        done = self.run_script("sign", key or self.key, src or self.unsigned, out)
        self.assertEqual(done.returncode, 0, done.stderr)
        return out

    def said(self, path):
        done = self.run_script("check", path, self.keys)
        return done.returncode, dict(line.split("=", 1) for line in done.stdout.splitlines())

    def read(self, path):
        with open(path, "rb") as handle:
            return handle.read()

    def test_the_key_line_is_the_stick_s_form(self):
        line = self.read(self.keys).decode()
        self.assertRegex(line, r"^ed25519 [A-Za-z0-9+/]{43}= the release key\n$")

    def test_an_unsigned_file_has_no_signature(self):
        rc, said = self.said(self.unsigned)
        self.assertEqual((rc, said["SIGNATURE"]), (0, "none"))
        self.assertEqual(said["SHA256"], hashlib.sha256(self.read(self.unsigned)).hexdigest())

    def test_a_signed_file_is_valid_with_its_key(self):
        rc, said = self.said(self.sign())
        self.assertEqual((rc, said["SIGNATURE"], said["SIGNER"]), (0, "valid", "the release key"))

    def test_the_layout_is_that_of_update_h(self):
        data = self.read(self.sign())
        head = data[:256]
        klen, rlen = struct.unpack(">II", head[12:20])
        rootfs = data[256 + klen:256 + klen + rlen]
        self.assertEqual(rootfs[:len(self.squash)], self.squash)
        block = rootfs[len(self.squash):]
        self.assertEqual(len(block), 128)
        self.assertEqual(block[:8], b"ONUS\0\0\0\1")
        pub = subprocess.run(["openssl", "pkey", "-in", self.key, "-pubout", "-outform", "DER"],
                             check=True, capture_output=True).stdout[-32:]
        self.assertEqual(block[8:40], hashlib.sha256(pub).digest())
        self.assertEqual(block[104:], bytes(24))
        self.assertEqual(struct.unpack(">I", head[252:])[0], zlib.crc32(head[:252]))
        self.assertEqual(data[-32:], hashlib.sha256(data[:-32]).digest())
        self.assertEqual(struct.unpack(">Q", head[24:32])[0], KERNEL_TIME)

    def test_signing_twice_gives_the_same_file(self):
        once = self.sign()
        again = self.sign(src=once, name="again.bin")
        self.assertEqual(self.read(once), self.read(again))

    def test_another_key_is_unknown(self):
        rc, said = self.said(self.sign(key=self.make_key("other")))
        self.assertEqual((rc, said["SIGNATURE"]), (0, "unknown"))

    def test_a_changed_file_is_bad(self):
        data = bytearray(self.read(self.sign()))
        data[256 + 512 + 1000] ^= 1     # inside the SquashFS image
        body = bytes(data[:-32])
        changed = self.file("changed.bin", body + hashlib.sha256(body).digest())
        rc, said = self.said(changed)
        self.assertEqual((rc, said["SIGNATURE"]), (1, "bad"))

    def test_the_root_filesystem_part_carries_the_signature(self):
        signed = self.sign()
        out = os.path.join(self.dir, "r0")
        done = self.run_script("rootfs", signed, out)
        self.assertEqual(done.returncode, 0, done.stderr)
        part = self.read(out)
        self.assertEqual(len(part), len(self.squash) + 128)
        self.assertEqual(part[:len(self.squash)], self.squash)
        self.assertEqual(part[len(self.squash):][:4], b"ONUS")

    def test_a_damaged_file_is_not_signed(self):
        data = bytearray(self.read(self.unsigned))
        data[300] ^= 1
        bad = self.file("bad.bin", bytes(data))
        out = os.path.join(self.dir, "out.bin")
        done = self.run_script("sign", self.key, bad, out)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("damaged", done.stderr)
        self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
