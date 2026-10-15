# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The programs for the stick (mcsniff, mdbadd, sstats) compile cleanly.

They run on the stick, so here they are only compiled, with the host's
compiler and every warning an error. A host without a compiler, or without the
headers of Linux, skips them.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

LAB = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
PROGRAMS = ("mcsniff", "mdbadd", "sstats")
FLAGS = ("-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror")


def compiler():
    """The host's C compiler, if it has the headers of Linux."""
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return None
    probe = subprocess.run([cc, "-x", "c", "-fsyntax-only", "-"], input=b"#include <linux/if_packet.h>\n",
                           capture_output=True)
    return cc if probe.returncode == 0 else None


class Compile(unittest.TestCase):
    def setUp(self):
        self.cc = compiler()
        if not self.cc:
            self.skipTest("no C compiler with the headers of Linux")

    def test_each_compiles_without_a_warning(self):
        with tempfile.TemporaryDirectory() as out:
            for name in PROGRAMS:
                with self.subTest(program=name):
                    run = subprocess.run([self.cc, *FLAGS, "-o", os.path.join(out, name),
                                          os.path.join(LAB, name + ".c")], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)

    def test_each_says_its_usage(self):
        with tempfile.TemporaryDirectory() as out:
            for name in PROGRAMS:
                exe = os.path.join(out, name)
                subprocess.run([self.cc, *FLAGS, "-o", exe, os.path.join(LAB, name + ".c")], check=True)
                with self.subTest(program=name):
                    run = subprocess.run([exe], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 2)
                    self.assertIn("usage: " + name, run.stderr)


if __name__ == "__main__":
    unittest.main()
