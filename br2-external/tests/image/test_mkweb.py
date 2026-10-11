# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of board/dfp-34x-2c3/mkweb.py and the fwu.sh it packs.

The package is made from small stand-in images and taken apart again; then
fwu.sh runs as the shipped firmware runs it, on files that stand in for its
partitions, with stand-ins for nv, flash_eraseall and md5sum.
"""

import hashlib
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
BOARD = os.path.join(os.path.dirname(os.path.dirname(HERE)), "board", "dfp-34x-2c3")
SCRIPT = os.path.join(BOARD, "mkweb.py")

KERNEL_PART, SHIPPED_KERNEL, SHIPPED_ROOTFS = 0x1D0000, 0x14C000, 0x274000

# The shipped firmware's partitions, as its /proc/mtd lists them.
MTD = """dev:    size   erasesize  name
mtd0: 00040000 00001000 "boot"
mtd1: 00002000 00001000 "env"
mtd2: 00002000 00001000 "env2"
mtd3: 0003c000 00001000 "config"
mtd4: 0014c000 00001000 "k0"
mtd5: 00274000 00001000 "r0"
mtd6: 0014c000 00001000 "k1"
mtd7: 00274000 00001000 "r1"
"""

# nv logs what it is told; flash_eraseall fills its device with 0xff, or,
# while $T/erase.fail is there, does nothing; md5sum prints as BusyBox does.
STANDINS = {
    "nv": '#!/bin/sh\necho "$*" >>"$T/nv.log"\n',
    "flash_eraseall": ('#!/bin/sh\n[ -e "$T/erase.fail" ] && exit 0\n'
                       'exec python3 -c "import os,sys; n = os.path.getsize(sys.argv[1]); '
                       'open(sys.argv[1], \'r+b\').write(b\'\\\\xff\' * n)" "$1"\n'),
    "md5sum": ('#!/bin/sh\nexec python3 -c "import hashlib,sys; '
               'print(hashlib.md5(sys.stdin.buffer.read()).hexdigest() + \'  -\')"\n'),
}


class Scratch(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name
        self.kernel = b"\x27\x05\x19\x56" + bytes(range(256)) * 6000
        self.rootfs = b"hsqs" + bytes(range(255, -1, -1)) * 5000
        self.out = os.path.join(self.dir, "web-install.tar")

    def file(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as handle:
            handle.write(data)
        return path

    def pack(self, kernel=None, rootfs=None, version="0123456789ab"):
        argv = [sys.executable, SCRIPT, "--version", version, "--time", "1700000000",
                self.file("kernel", self.kernel if kernel is None else kernel),
                self.file("rootfs", self.rootfs if rootfs is None else rootfs), self.out]
        return subprocess.run(argv, capture_output=True, text=True)

    def members(self):
        with tarfile.open(self.out) as tar:
            return {m.name: (m, tar.extractfile(m).read()) for m in tar.getmembers()}


class Package(Scratch):
    def test_it_holds_fwu_sh_md5_txt_k_and_r_in_that_order(self):
        self.assertEqual(self.pack().returncode, 0)
        with tarfile.open(self.out) as tar:
            self.assertEqual(tar.getnames(), ["fwu.sh", "md5.txt", "k", "r"])
            for member in tar.getmembers():
                self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 1700000000))
                self.assertEqual(member.mode, 0o755 if member.name == "fwu.sh" else 0o644)

    def test_k_and_r_are_this_systems_bank_cut_where_the_shipped_firmware_cuts(self):
        self.pack()
        m = self.members()
        k, r = m["k"][1], m["r"][1]
        self.assertEqual(len(k), SHIPPED_KERNEL)
        self.assertEqual(k + r, self.kernel + b"\xff" * (KERNEL_PART - len(self.kernel)) + self.rootfs)

    def test_md5_txt_is_what_md5sum_prints_and_gives_fwu_sh_as_fwu_starter_checks_it(self):
        self.pack()
        m = self.members()
        lines = m["md5.txt"][1].decode().splitlines()
        self.assertEqual(lines, ["%s  %s" % (hashlib.md5(m[name][1]).hexdigest(), name)
                                 for name in ("fwu.sh", "k", "r")])
        self.assertEqual([line for line in lines if "fwu.sh" in line],
                         ["%s  fwu.sh" % hashlib.md5(m["fwu.sh"][1]).hexdigest()])

    def test_fwu_sh_carries_the_digests_of_the_partitions_and_the_version(self):
        self.pack()
        m = self.members()
        script = m["fwu.sh"][1].decode()
        r = m["r"][1] + b"\xff" * (SHIPPED_ROOTFS - len(m["r"][1]))
        self.assertIn("K_MD5=%s\n" % hashlib.md5(m["k"][1]).hexdigest(), script)
        self.assertIn("R_MD5=%s\n" % hashlib.md5(r).hexdigest(), script)
        self.assertIn("VERSION=0123456789ab\n", script)
        self.assertNotIn("@", script.split("VERSION=")[1].splitlines()[0])

    def test_the_same_images_give_the_same_package(self):
        self.pack()
        with open(self.out, "rb") as handle:
            first = handle.read()
        self.pack()
        with open(self.out, "rb") as handle:
            self.assertEqual(handle.read(), first)

    def test_what_it_refuses(self):
        for kernel, rootfs, version in ((b"not a uImage", None, "v"),
                                        (None, b"not squashfs", "v"),
                                        (b"\x27\x05\x19\x56" + bytes(KERNEL_PART), None, "v"),
                                        (None, None, "a b"), (None, None, "")):
            done = self.pack(kernel, rootfs, version)
            self.assertNotEqual(done.returncode, 0, (kernel and kernel[:12], version))
            self.assertFalse(os.path.exists(self.out))

    def test_a_package_the_page_would_refuse_is_not_made(self):
        # Both partitions full: the bank alone is the page's limit.
        kernel = b"\x27\x05\x19\x56" + bytes(KERNEL_PART - 4)
        rootfs = b"hsqs" + bytes(0x1F0000 - 4)
        done = self.pack(kernel, rootfs)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("fewer than 3932160", done.stderr)
        self.assertFalse(os.path.exists(self.out))


class FwuSh(Scratch):
    def setUp(self):
        super().setUp()
        self.pack()
        self.root = os.path.join(self.dir, "root")
        os.makedirs(os.path.join(self.root, "proc"))
        os.makedirs(os.path.join(self.root, "dev"))
        with open(os.path.join(self.root, "proc", "mtd"), "w") as handle:
            handle.write(MTD)
        for dev, size in (("mtd4", SHIPPED_KERNEL), ("mtd5", SHIPPED_ROOTFS),
                          ("mtd6", SHIPPED_KERNEL), ("mtd7", SHIPPED_ROOTFS)):
            with open(os.path.join(self.root, "dev", dev), "wb") as handle:
                handle.write(b"\x5a" * size)
        self.bin = os.path.join(self.dir, "bin")
        os.makedirs(self.bin)
        for name, text in STANDINS.items():
            path = os.path.join(self.bin, name)
            with open(path, "w") as handle:
                handle.write(text)
            os.chmod(path, 0o755)
        # fwu_starter.sh runs fwu.sh from the package, unpacked in a directory.
        self.work = os.path.join(self.dir, "fwu")
        os.makedirs(self.work)
        with open(os.path.join(self.work, "fwu.sh"), "wb") as handle:
            handle.write(self.members()["fwu.sh"][1])

    def run_fwu(self, bank="1", package=None):
        env = dict(os.environ, FWU_ROOT=self.root, T=self.dir,
                   PATH=self.bin + os.pathsep + os.environ["PATH"])
        return subprocess.run(["sh", "fwu.sh", bank, package or self.out], cwd=self.work,
                              env=env, capture_output=True, text=True)

    def dev(self, name):
        with open(os.path.join(self.root, "dev", name), "rb") as handle:
            return handle.read()

    def nv(self):
        path = os.path.join(self.dir, "nv.log")
        if not os.path.exists(path):
            return []
        with open(path) as handle:
            return handle.read().splitlines()

    def test_it_writes_the_other_bank_and_has_it_tried_and_exits_1(self):
        done = self.run_fwu("1")
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        m = self.members()
        self.assertEqual(self.dev("mtd6"), m["k"][1])
        r = m["r"][1]
        self.assertEqual(self.dev("mtd7"), r + b"\xff" * (SHIPPED_ROOTFS - len(r)))
        self.assertEqual(self.dev("mtd4"), b"\x5a" * SHIPPED_KERNEL)
        self.assertEqual(self.nv(), ["setenv sw_valid1 0", "setenv sw_version1 0123456789ab",
                                     "setenv sw_valid1 1", "setenv onu_trial 1",
                                     "setenv sw_tryactive 1"])

    def test_bank_0_is_k0_and_r0(self):
        self.assertEqual(self.run_fwu("0").returncode, 1)
        self.assertEqual(self.dev("mtd4"), self.members()["k"][1])
        self.assertEqual(self.dev("mtd6"), b"\x5a" * SHIPPED_KERNEL)
        self.assertEqual(self.nv()[-1], "setenv sw_tryactive 0")

    def test_a_write_that_does_not_read_back_leaves_no_trial(self):
        open(os.path.join(self.dir, "erase.fail"), "w").close()
        done = self.run_fwu("1")
        self.assertEqual(done.returncode, 3)
        self.assertIn("does not read back", done.stdout)
        self.assertEqual(self.nv(), ["setenv sw_valid1 0"])

    def test_what_it_refuses_before_it_writes(self):
        with open(os.path.join(self.root, "proc", "mtd"), "w") as handle:
            handle.write(MTD.replace('0014c000 00001000 "k1"', '001d0000 00001000 "k1"'))
        for bank, package in (("1", None), ("2", None), ("1", os.path.join(self.dir, "none"))):
            done = self.run_fwu(bank, package)
            self.assertEqual(done.returncode, 2, (bank, package))
        self.assertEqual(self.dev("mtd6"), b"\x5a" * SHIPPED_KERNEL)
        self.assertEqual(self.nv(), [])


if __name__ == "__main__":
    unittest.main()
