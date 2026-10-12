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

# Root's line of each file of the accounts, as the build system writes them.
ROOT_LINES = dict(zip(("passwd", "group", "shadow"),
                      ("root:x:0:0:root:/root:/bin/sh\n", "root:x:0:\n",
                       "root::19000:0:99999:7:::\n")))

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
        full = {key: value for key, value in os.environ.items() if key != "BASE_DIR"}
        full.update(GIT=self.git, **env)
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

    IPROUTE2 = ("ip", "tc", "bridge", "devlink", "ctstat", "dcb", "genl", "ifstat", "lnstat",
                "nstat", "rdma", "routel", "rtacct", "rtmon", "rtstat", "ss", "tipc", "vdpa")

    def make_iproute2(self, mark="1"):
        """iproute2's files as its package installs them, and BusyBox."""
        for directory in ("sbin", "bin", "usr/lib/tc", "usr/share/iproute2"):
            os.makedirs(os.path.join(self.target, directory), exist_ok=True)
        files = ["sbin/" + prog for prog in self.IPROUTE2]
        files += ["usr/lib/tc/normal.dist", "usr/share/iproute2/rt_tables", "bin/busybox"]
        for name in files:
            with open(os.path.join(self.target, name), "w", encoding="utf-8") as handle:
                handle.write("%s %s\n" % (name, mark))

    def busybox_ip(self):
        ip = os.path.join(self.target, "sbin", "ip")
        return os.path.islink(ip) and os.readlink(ip) == "../bin/busybox"

    def test_iproute2_goes_and_ip_is_busyboxs(self):
        self.make_iproute2()
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(os.listdir(os.path.join(self.target, "sbin")), ["ip"])
        self.assertTrue(self.busybox_ip())
        self.assertEqual(os.listdir(os.path.join(self.target, "usr", "lib")), [])
        self.assertEqual(os.listdir(os.path.join(self.target, "usr", "share")), [])

    def test_iproute2_is_set_aside_for_the_ram_image_with_its_ip_as_ip_full(self):
        base = os.path.join(self._tmp.name, "base")
        self.make_iproute2()
        done = self.run_script(BASE_DIR=base)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(os.listdir(os.path.join(self.target, "sbin")), ["ip"])
        self.assertTrue(self.busybox_ip())
        diag = os.path.join(base, "diag")
        self.assertEqual(sorted(os.listdir(os.path.join(diag, "sbin"))),
                         ["bridge", "devlink", "tc"])
        for name, was in (("usr/sbin/ip-full", "sbin/ip"),
                          ("usr/lib/tc/normal.dist", "usr/lib/tc/normal.dist"),
                          ("usr/share/iproute2/rt_tables", "usr/share/iproute2/rt_tables")):
            with open(os.path.join(diag, name), encoding="utf-8") as handle:
                self.assertEqual(handle.read(), "%s 1\n" % was)

    def test_a_second_run_keeps_iproute2_set_aside_and_a_new_install_replaces_it(self):
        base = os.path.join(self._tmp.name, "base")
        diag = os.path.join(base, "diag")
        self.make_iproute2()
        self.assertEqual(self.run_script(BASE_DIR=base).returncode, 0)
        self.assertEqual(self.run_script(BASE_DIR=base).returncode, 0)
        self.assertTrue(self.busybox_ip())
        with open(os.path.join(diag, "usr", "sbin", "ip-full"), encoding="utf-8") as handle:
            self.assertEqual(handle.read(), "sbin/ip 1\n")

        os.remove(os.path.join(self.target, "sbin", "ip"))
        self.make_iproute2(mark="2")
        self.assertEqual(self.run_script(BASE_DIR=base).returncode, 0)
        self.assertTrue(self.busybox_ip())
        with open(os.path.join(diag, "usr", "sbin", "ip-full"), encoding="utf-8") as handle:
            self.assertEqual(handle.read(), "sbin/ip 2\n")
        self.assertEqual(os.listdir(os.path.join(diag, "usr", "lib", "tc")), ["normal.dist"])
        with open(os.path.join(diag, "usr", "lib", "tc", "normal.dist"), encoding="utf-8") as handle:
            self.assertEqual(handle.read(), "usr/lib/tc/normal.dist 2\n")

    def make_tools(self):
        tools = os.path.join(self.target, "usr", "bin")
        sbin = os.path.join(self.target, "usr", "sbin")
        shell = os.path.join(self.target, "usr", "share", "libubox")
        for directory in (tools, sbin, shell):
            os.makedirs(directory, exist_ok=True)
        for prog in ("ubus", "uci", "jshn", "omcitap", "omcivlan", "passwd"):
            with open(os.path.join(tools, prog), "w", encoding="utf-8") as handle:
                handle.write("#!/bin/sh\n# %s\n" % prog)
        for prog in ("flashcp", "flash_erase", "mtd_debug", "pond"):
            with open(os.path.join(sbin, prog), "w", encoding="utf-8") as handle:
                handle.write("#!/bin/sh\n# %s\n" % prog)
        with open(os.path.join(shell, "jshn.sh"), "w", encoding="utf-8") as handle:
            handle.write("# jshn\n")
        return tools, shell

    def test_the_command_line_tools_of_the_network_layer_go(self):
        tools, shell = self.make_tools()
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(os.listdir(tools), ["passwd"])
        self.assertFalse(os.path.exists(shell))

    def test_the_tools_of_diagnosis_are_set_aside_for_the_ram_image(self):
        base = os.path.join(self._tmp.name, "base")
        tools, shell = self.make_tools()
        done = self.run_script(BASE_DIR=base)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(os.listdir(tools), ["passwd"])
        self.assertFalse(os.path.exists(shell))
        aside = os.path.join(base, "diag", "usr", "bin")
        self.assertEqual(sorted(os.listdir(aside)), ["omcitap", "omcivlan", "ubus"])
        with open(os.path.join(aside, "ubus"), encoding="utf-8") as handle:
            self.assertEqual(handle.read(), "#!/bin/sh\n# ubus\n")
        self.assertEqual(sorted(os.listdir(os.path.join(base, "diag", "usr", "sbin"))),
                         ["flash_erase", "flashcp", "mtd_debug"])
        self.assertEqual(os.listdir(os.path.join(self.target, "usr", "sbin")), ["pond"])

    def test_a_second_run_keeps_the_tools_set_aside(self):
        base = os.path.join(self._tmp.name, "base")
        self.make_tools()
        self.assertEqual(self.run_script(BASE_DIR=base).returncode, 0)
        self.assertEqual(self.run_script(BASE_DIR=base).returncode, 0)
        aside = os.path.join(base, "diag", "usr", "bin")
        self.assertEqual(sorted(os.listdir(aside)), ["omcitap", "omcivlan", "ubus"])

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

    def test_the_accounts_of_the_daemons_are_written_once(self):
        etc = os.path.join(self.target, "etc")
        for name in ("passwd", "group", "shadow"):
            with open(os.path.join(etc, name), "w", encoding="utf-8") as handle:
                handle.write(ROOT_LINES[name])
        for _ in range(2):
            self.assertEqual(self.run_script().returncode, 0)
        with open(os.path.join(etc, "passwd"), encoding="utf-8") as handle:
            passwd = handle.read()
        with open(os.path.join(etc, "group"), encoding="utf-8") as handle:
            group = handle.read()
        with open(os.path.join(etc, "shadow"), encoding="utf-8") as handle:
            shadow = handle.read()
        self.assertEqual(passwd.count("\npond:"), 1)
        self.assertIn("\npond:x:200:200:The status daemon:/var/empty:/bin/false\n", passwd)
        self.assertIn("\npond:x:200:\n", group)
        # No password: no one logs in with it.
        self.assertIn("\npond:*:::::::\n", shadow)
        self.assertEqual(passwd.count("\nntp:"), 1)
        self.assertIn("\nntp:x:201:201:The time daemon:/var/empty:/bin/false\n", passwd)
        self.assertIn("\nntp:x:201:\n", group)
        self.assertIn("\nntp:*:::::::\n", shadow)

    def test_without_the_file_it_fails(self):
        os.remove(self.release)
        done = self.run_script()
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("no ", done.stderr)


if __name__ == "__main__":
    unittest.main()
