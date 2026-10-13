# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of tools/vm that need no docker and no qemu.

The fragments are checked against the kernel's own Kconfig, so that a typo or
an option the kernel dropped shows here and not after a kernel build that
silently ignored it; the scripts are parsed by the shell; and the launcher's
usage and the guest's refusals are exercised.
"""

import os
import re
import shutil
import subprocess
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
VM = os.path.dirname(HERE)
TOP = os.path.dirname(os.path.dirname(VM))
KERNEL = os.path.join(TOP, "linux")

FRAGMENTS = ("base.config", "omcid.config", "pon.config", "rtl9602c.kunitconfig")


def symbols():
    """Every symbol that a Kconfig file of the kernel defines."""
    found = set()
    pattern = re.compile(r"^\s*(?:menu)?config\s+([A-Z0-9_]+)\s*$")
    for root, _dirs, files in os.walk(KERNEL):
        if "/Documentation" in root or "/tools/" in root:
            continue
        for name in files:
            if not name.startswith("Kconfig"):
                continue
            with open(os.path.join(root, name), errors="replace") as f:
                for line in f:
                    m = pattern.match(line)
                    if m:
                        found.add(m.group(1))
    return found


def options(path):
    """The (option, value) pairs of a fragment, and its comment lines kept out."""
    out = []
    with open(os.path.join(VM, path)) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.fullmatch(r"CONFIG_([A-Z0-9_]+)=(.*)", line)
            if not m:
                raise AssertionError(f"{path}: not an option line: {line!r}")
            out.append((m.group(1), m.group(2)))
    return out


class Fragments(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.known = symbols()

    def test_the_kernel_has_kconfig_files(self):
        self.assertGreater(len(self.known), 5000)

    def test_every_option_exists_in_the_kernel(self):
        for path in FRAGMENTS:
            for name, _value in options(path):
                self.assertIn(name, self.known, f"{path}: CONFIG_{name} is no symbol of linux/")

    def test_no_option_is_asked_twice_in_a_fragment_with_two_values(self):
        for path in FRAGMENTS:
            seen = {}
            for name, value in options(path):
                self.assertEqual(seen.setdefault(name, value), value, f"{path}: CONFIG_{name}")

    def test_the_fragments_that_merge_do_not_contradict_each_other(self):
        merged = {}
        for path in ("base.config", "omcid.config", "pon.config"):
            for name, value in options(path):
                if value != "n" and name in merged and merged[name] == "n":
                    self.fail(f"{path}: CONFIG_{name} is asked on after another fragment turned it off")
                merged[name] = value

    def test_the_guest_needs_what_it_is_asked_to_boot_with(self):
        names = {n for n, v in options("base.config") if v == "y"}
        for needed in ("SERIAL_AMBA_PL011_CONSOLE", "DEVTMPFS", "PROC_FS", "SYSFS", "NET", "BRIDGE"):
            self.assertIn(needed, names)
        # 9p is what the kvm_guest.config of the kernel gives; it must exist.
        self.assertTrue(os.path.exists(os.path.join(KERNEL, "kernel", "configs", "kvm_guest.config")))


class Scripts(unittest.TestCase):
    def test_the_scripts_parse(self):
        sh = shutil.which("sh")
        for name in ("vm", "guest"):
            r = subprocess.run([sh, "-n", os.path.join(VM, name)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"{name}: {r.stderr}")

    def test_the_launcher_says_what_it_does_and_refuses_the_rest(self):
        for args in ([], ["nonsense"], ["omcid-kernel", "more"]):
            r = subprocess.run([os.path.join(VM, "vm")] + args, capture_output=True, text=True)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn("tools/vm/vm omcid-kernel", r.stderr)
            self.assertIn("tools/vm/vm kunit", r.stderr)

    def test_every_test_the_launcher_names_is_one_the_guest_runs(self):
        with open(os.path.join(VM, "vm")) as f:
            launcher = f.read()
        with open(os.path.join(VM, "guest")) as f:
            guest = f.read()
        for name in ("omcid-kernel", "pon-selftest", "kunit"):
            self.assertIn(name, launcher)
            self.assertRegex(guest, rf"(?m)^{name}\)")

    @unittest.skipIf(os.uname().machine == "aarch64", "the host is aarch64, which the guest accepts")
    def test_the_guest_refuses_a_host_that_is_not_aarch64(self):
        r = subprocess.run([shutil.which("sh"), os.path.join(VM, "guest"), "kunit"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("aarch64", r.stderr)


class Container(unittest.TestCase):
    def test_the_container_is_the_pinned_debian_of_the_build(self):
        with open(os.path.join(VM, "Dockerfile")) as f:
            dockerfile = f.read()
        with open(os.path.join(TOP, "docs", "handbook", "building.rst")) as f:
            building = f.read()
        digest = re.search(r"^FROM debian:trixie@(sha256:[0-9a-f]{64})$", dockerfile, re.M)
        self.assertIsNotNone(digest)
        self.assertIn(digest.group(1), building)

    def test_the_container_has_what_the_guest_calls(self):
        with open(os.path.join(VM, "Dockerfile")) as f:
            dockerfile = f.read()
        # ipxe-qemu holds the ROM of the virtual network card, which qemu
        # refuses to start without and which the package does not pull in.
        for package in ("qemu-system-arm", "ipxe-qemu", "iproute2", "python3-yaml", "bison", "flex", "bc", "rsync"):
            self.assertIn(package, dockerfile)


if __name__ == "__main__":
    unittest.main()
