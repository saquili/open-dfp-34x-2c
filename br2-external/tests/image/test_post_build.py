# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of board/dfp-34x-2c3/post-build.sh.

The script runs as a program on a scratch target directory, with a stand-in
for git that answers as a clean checkout, a dirty one or no checkout at all.
"""

import os
import stat
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
SCRIPT = os.path.join(os.path.dirname(os.path.dirname(HERE)), "board", "dfp-34x-2c3",
                      "post-build.sh")

STUB = """\
#!/bin/sh
# Stands in for git: rev-parse prints a hash or fails, status prints changes.
case "$3" in
rev-parse) [ -n "$STUB_NO_REPO" ] && exit 128; echo 0123456789ab ;;
status) [ -n "$STUB_DIRTY" ] && echo " M somefile" ;;
esac
exit 0
"""

RELEASE = 'NAME="open-dfp-34x-2c"\nPRETTY_NAME="open-dfp-34x-2c on the ODI DFP-34X-2C3"\n'


class PostBuild(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.target = os.path.join(self._tmp.name, "target")
        os.makedirs(os.path.join(self.target, "etc"))
        self.release = os.path.join(self.target, "etc", "os-release")
        with open(self.release, "w", encoding="utf-8") as handle:
            handle.write(RELEASE)
        self.git = os.path.join(self._tmp.name, "git")
        with open(self.git, "w", encoding="utf-8") as handle:
            handle.write(STUB)
        os.chmod(self.git, stat.S_IRWXU)

    def run_script(self, **env):
        full = dict(os.environ, GIT=self.git, **env)
        return subprocess.run(["sh", SCRIPT, self.target], env=full, capture_output=True,
                              text=True)

    def read(self):
        with open(self.release, encoding="utf-8") as handle:
            return handle.read()

    def test_a_clean_checkout_gives_its_commit(self):
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(self.read(), RELEASE + "BUILD_ID=0123456789ab\n")

    def test_uncommitted_changes_are_said(self):
        self.assertEqual(self.run_script(STUB_DIRTY="1").returncode, 0)
        self.assertTrue(self.read().endswith("BUILD_ID=0123456789ab-dirty\n"))

    def test_sources_without_git_are_unknown(self):
        self.assertEqual(self.run_script(STUB_NO_REPO="1").returncode, 0)
        self.assertTrue(self.read().endswith("BUILD_ID=unknown\n"))

    def test_a_second_run_replaces_the_line(self):
        self.run_script(STUB_DIRTY="1")
        self.run_script()
        self.assertEqual(self.read().count("BUILD_ID="), 1)
        self.assertTrue(self.read().endswith("BUILD_ID=0123456789ab\n"))

    def test_the_programs_of_iproute2_the_stick_does_not_run_go(self):
        sbin = os.path.join(self.target, "sbin")
        os.makedirs(sbin)
        kept = ("ip", "tc", "bridge", "devlink")
        gone = ("ctstat", "dcb", "genl", "ifstat", "lnstat", "nstat", "rdma", "routel",
                "rtacct", "rtmon", "rtstat", "ss", "tipc", "vdpa")
        for prog in kept + gone:
            with open(os.path.join(sbin, prog), "w", encoding="utf-8") as handle:
                handle.write("#!/bin/sh\n")
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(sorted(os.listdir(sbin)), sorted(kept))

    def test_the_command_line_tools_of_the_network_layer_go(self):
        tools = os.path.join(self.target, "usr", "bin")
        shell = os.path.join(self.target, "usr", "share", "libubox")
        os.makedirs(tools)
        os.makedirs(shell)
        for prog in ("ubus", "uci", "jshn", "omcitap"):
            with open(os.path.join(tools, prog), "w", encoding="utf-8") as handle:
                handle.write("#!/bin/sh\n")
        with open(os.path.join(shell, "jshn.sh"), "w", encoding="utf-8") as handle:
            handle.write("# jshn\n")
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(os.listdir(tools), ["omcitap"])
        self.assertFalse(os.path.exists(shell))

    def test_a_tree_without_the_shared_c_library_needs_no_build_directories(self):
        env = {key: value for key, value in os.environ.items()
               if key not in ("HOST_DIR", "STAGING_DIR", "BUILD_DIR")}
        done = subprocess.run(["sh", SCRIPT, self.target], env=dict(env, GIT=self.git),
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_the_shared_c_library_is_not_left_whole_for_want_of_the_build_directories(self):
        lib = os.path.join(self.target, "lib")
        os.makedirs(lib)
        with open(os.path.join(lib, "libuClibc-1.0.59.so"), "wb") as handle:
            handle.write(b"\x7fELF")
        env = {key: value for key, value in os.environ.items()
               if key not in ("HOST_DIR", "STAGING_DIR", "BUILD_DIR")}
        done = subprocess.run(["sh", SCRIPT, self.target], env=dict(env, GIT=self.git),
                              capture_output=True, text=True)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("HOST_DIR is not set", done.stderr)

    def test_without_the_file_it_fails(self):
        os.remove(self.release)
        done = self.run_script()
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("no ", done.stderr)


if __name__ == "__main__":
    unittest.main()
