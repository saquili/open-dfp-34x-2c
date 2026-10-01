# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of kernel-archive, on small repositories made for each test.

The script runs as a program inside a repository whose linux/ directory is a
few files with a Makefile that names a kernel version. The archive is then read
back with tarfile. Git is run with no user or system configuration, so the
tests do not depend on, or sign anything with, the configuration of whoever runs
them.
"""

import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "kernel-archive")

NEEDS_TOOLS = unittest.skipUnless(shutil.which("git") and shutil.which("xz"),
                                  "git and xz are needed")

MAKEFILE = """\
VERSION = 6
PATCHLEVEL = 18
SUBLEVEL = 48
EXTRAVERSION =%s
NAME = Test Release
"""

WHEN = "2026-01-02T03:04:05+00:00"


class Repo:
    """A scratch git repository; files go in by blob and index, never by checkout."""

    def __init__(self, root):
        self.path = os.path.join(root, "repo")
        os.makedirs(self.path)
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
                        GIT_AUTHOR_NAME="Test", GIT_AUTHOR_EMAIL="test@example.org",
                        GIT_COMMITTER_NAME="Test", GIT_COMMITTER_EMAIL="test@example.org",
                        GIT_AUTHOR_DATE=WHEN, GIT_COMMITTER_DATE=WHEN)
        self.git("init", "-q", "-b", "main")

    def git(self, *args, stdin=None):
        done = subprocess.run(["git", "-C", self.path, "-c", "commit.gpgsign=false", *args],
                              input=stdin, capture_output=True, text=True, env=self.env)
        if done.returncode:
            raise RuntimeError("git %s: %s" % (" ".join(args), done.stderr))
        return done.stdout.strip()

    def add(self, name, text, mode="100644"):
        blob = self.git("hash-object", "-w", "--stdin", stdin=text)
        self.git("update-index", "--add", "--cacheinfo", "%s,%s,%s" % (mode, blob, name))

    def commit(self, message="commit", when=None):
        if when:
            self.env.update(GIT_AUTHOR_DATE=when, GIT_COMMITTER_DATE=when)
        self.git("commit", "-q", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD")

    def tree(self, rev="HEAD"):
        return self.git("rev-parse", rev + ":linux")


def members(path):
    with tarfile.open(path, "r:xz") as archive:
        return {member.name: member for member in archive.getmembers()}


class KernelArchive(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kernel-archive-test-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "out")
        self.repo = Repo(self.tmp)
        self.repo.add("linux/Makefile", MAKEFILE % "")
        self.repo.add("linux/include/uapi/linux/pon.h", "/* uapi */\n")
        self.repo.add("linux/scripts/run.sh", "#!/bin/sh\n", mode="100755")
        self.repo.add("README.rst", "not the kernel\n")
        self.repo.add("tools/other", "not the kernel either\n")
        self.first = self.repo.commit("first")

    def run_script(self, *args, cwd=None, env=None):
        return subprocess.run([SCRIPT, *args], capture_output=True, text=True,
                              cwd=cwd or self.repo.path, env=env or self.repo.env)

    def archive(self, *args):
        done = self.run_script("-o", self.out, *args)
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout.strip()

    def tree_id(self, rev="HEAD"):
        return self.repo.tree(rev)[:12]

    @NEEDS_TOOLS
    def test_the_name_holds_the_release_and_the_tree(self):
        path = self.archive()
        self.assertEqual(os.path.realpath(path),
                         os.path.join(os.path.realpath(self.out),
                                      "linux-6.18.48-%s.tar.xz" % self.tree_id()))
        self.assertTrue(os.path.isfile(path))

    @NEEDS_TOOLS
    def test_the_archive_holds_the_kernel_tree_and_nothing_else(self):
        found = members(self.archive())
        self.assertEqual(sorted(name for name, member in found.items() if member.isfile()),
                         ["linux-6.18.48/Makefile", "linux-6.18.48/include/uapi/linux/pon.h",
                          "linux-6.18.48/scripts/run.sh"])

    @NEEDS_TOOLS
    def test_owners_modes_and_times_are_fixed(self):
        found = members(self.archive())
        header = found["linux-6.18.48/include/uapi/linux/pon.h"]
        script = found["linux-6.18.48/scripts/run.sh"]
        self.assertEqual((header.mode, script.mode), (0o644, 0o755))
        self.assertEqual((header.uid, header.gid), (0, 0))
        self.assertEqual(header.mtime, 1767323045)   # WHEN
        directory = found["linux-6.18.48/include"]
        self.assertEqual(directory.mode, 0o755)

    @NEEDS_TOOLS
    def test_both_files_of_paths_that_differ_only_in_case_are_kept(self):
        self.repo.add("linux/include/uapi/linux/netfilter/xt_DSCP.h", "/* upper */\n")
        self.repo.add("linux/include/uapi/linux/netfilter/xt_dscp.h", "/* lower */\n")
        self.repo.commit("a pair of paths that differ in case")
        found = members(self.archive())
        self.assertIn("linux-6.18.48/include/uapi/linux/netfilter/xt_DSCP.h", found)
        self.assertIn("linux-6.18.48/include/uapi/linux/netfilter/xt_dscp.h", found)

    @NEEDS_TOOLS
    def test_the_extra_version_is_part_of_the_release(self):
        self.repo.add("linux/Makefile", MAKEFILE % " -rc1")
        self.repo.commit("a release candidate")
        path = self.archive()
        self.assertTrue(os.path.basename(path).startswith("linux-6.18.48-rc1-"), path)
        self.assertIn("linux-6.18.48-rc1/Makefile", members(path))

    @NEEDS_TOOLS
    def test_the_same_tree_gives_the_same_name_and_the_file_is_kept(self):
        path = self.archive()
        before = os.stat(path)
        self.repo.add("README.rst", "changed outside the kernel\n")
        self.repo.commit("not the kernel", when="2026-02-03T04:05:06+00:00")
        done = self.run_script("-o", self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), path)
        self.assertIn("exists", done.stderr)
        after = os.stat(path)
        self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))

    @NEEDS_TOOLS
    def test_a_changed_kernel_file_gives_another_name(self):
        first = self.archive()
        self.repo.add("linux/include/uapi/linux/pon.h", "/* changed */\n")
        self.repo.commit("change the kernel")
        second = self.archive()
        self.assertNotEqual(first, second)
        self.assertTrue(os.path.isfile(first) and os.path.isfile(second))
        with tarfile.open(second, "r:xz") as archive:
            data = archive.extractfile("linux-6.18.48/include/uapi/linux/pon.h").read()
        self.assertEqual(data, b"/* changed */\n")

    @NEEDS_TOOLS
    def test_a_revision_names_the_commit_to_archive(self):
        self.repo.add("linux/include/uapi/linux/pon.h", "/* changed */\n")
        self.repo.commit("change the kernel")
        path = self.archive(self.first)
        self.assertIn(self.tree_id(self.first), path)
        with tarfile.open(path, "r:xz") as archive:
            data = archive.extractfile("linux-6.18.48/include/uapi/linux/pon.h").read()
        self.assertEqual(data, b"/* uapi */\n")

    @NEEDS_TOOLS
    def test_the_same_commit_gives_the_same_bytes_in_another_directory(self):
        first = self.archive()
        other = os.path.join(self.tmp, "other")
        done = self.run_script("-o", other)
        self.assertEqual(done.returncode, 0, done.stderr)
        with open(first, "rb") as a, open(done.stdout.strip(), "rb") as b:
            self.assertEqual(a.read(), b.read())

    @NEEDS_TOOLS
    def test_the_default_directory_is_build_kernel_in_the_repository(self):
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        want = os.path.join(os.path.realpath(self.repo.path), "build", "kernel")
        self.assertEqual(os.path.dirname(done.stdout.strip()), want)

    @NEEDS_TOOLS
    def test_no_partial_file_is_left_beside_the_archive(self):
        self.archive()
        self.assertEqual([n for n in os.listdir(self.out) if ".part" in n], [])

    def test_a_commit_without_linux_is_an_error(self):
        other = Repo(os.path.join(self.tmp, "none"))
        other.add("README.rst", "no kernel\n")
        other.commit("no kernel")
        done = self.run_script("-o", self.out, cwd=other.path, env=other.env)
        self.assertEqual(done.returncode, 1)
        self.assertIn("has no linux/ directory", done.stderr)
        self.assertFalse(os.path.exists(self.out))

    def test_a_makefile_without_a_version_is_an_error(self):
        self.repo.add("linux/Makefile", "all:\n")
        self.repo.commit("a Makefile without a version")
        done = self.run_script("-o", self.out)
        self.assertEqual(done.returncode, 1)
        self.assertIn("cannot read the kernel version", done.stderr)

    def test_an_unknown_revision_is_an_error(self):
        done = self.run_script("-o", self.out, "no-such-revision")
        self.assertEqual(done.returncode, 1)
        self.assertIn("is not a commit", done.stderr)

    def test_outside_a_repository_is_an_error(self):
        outside = os.path.join(self.tmp, "outside")
        os.makedirs(outside)
        env = dict(self.repo.env, GIT_CEILING_DIRECTORIES=self.tmp)
        done = self.run_script("-o", self.out, cwd=outside, env=env)
        self.assertEqual(done.returncode, 1)
        self.assertIn("not inside a git repository", done.stderr)

    def test_a_missing_xz_is_reported(self):
        bin_dir = os.path.join(self.tmp, "bin")
        os.makedirs(bin_dir)
        os.symlink(shutil.which("git") or "/usr/bin/git", os.path.join(bin_dir, "git"))
        done = self.run_script("-o", self.out, env=dict(self.repo.env, PATH=bin_dir))
        self.assertEqual(done.returncode, 1)
        self.assertIn("xz is not installed", done.stderr)

    def test_too_many_arguments_are_a_usage_error(self):
        done = self.run_script("one", "two")
        self.assertEqual(done.returncode, 2)
        self.assertIn("usage", done.stderr)


if __name__ == "__main__":
    unittest.main()
