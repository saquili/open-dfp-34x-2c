# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of kernel-series, on small repositories made for each test.

Each repository has the import commit of a kernel, a few commits of a series on
top of it, and the page the check reads. Git is run with no user or system
configuration, so the tests do not depend on the configuration of whoever runs
them.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "kernel-series")
DOC = "docs/handbook/kernel-series.rst"

NEEDS_GIT = unittest.skipUnless(shutil.which("git"), "git is needed")

WHEN = "2026-01-02T03:04:05+00:00"


class Repo:
    """A scratch git repository, committed to through its working tree."""

    def __init__(self, root):
        self.path = os.path.join(root, "repo")
        os.makedirs(self.path)
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
                        GIT_AUTHOR_NAME="Test", GIT_AUTHOR_EMAIL="test@example.org",
                        GIT_COMMITTER_NAME="Test", GIT_COMMITTER_EMAIL="test@example.org",
                        GIT_AUTHOR_DATE=WHEN, GIT_COMMITTER_DATE=WHEN)
        self.git("init", "-q", "-b", "main")

    def git(self, *args):
        done = subprocess.run(["git", "-C", self.path, "-c", "commit.gpgsign=false", *args],
                              capture_output=True, text=True, env=self.env)
        if done.returncode:
            raise RuntimeError("git %s: %s" % (" ".join(args), done.stderr))
        return done.stdout.strip()

    def write(self, name, text):
        full = os.path.join(self.path, name)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(text)

    def commit(self, subject, files):
        for name, text in files.items():
            self.write(name, text)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", subject)
        return self.git("rev-parse", "HEAD")

    def remove(self, name, subject):
        self.git("rm", "-q", name)
        self.git("commit", "-q", "-m", subject)


def run(repo, *args):
    return subprocess.run([SCRIPT, *args], cwd=repo.path, capture_output=True, text=True,
                          env=repo.env)


def page(*paths):
    """A page that names each path as the real one does."""
    return "The series.\n\n" + "".join("* ``%s``\n" % path for path in paths)


@NEEDS_GIT
class KernelSeries(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.repo = Repo(self.tmp)
        # Something before the import, as the real repository has.
        self.repo.commit("docs: start", {"README": "hello\n"})
        self.repo.commit("linux: import Linux 6.18.48",
                         {"linux/net/dsa/user.c": "user\n",
                          "linux/drivers/ptp/ptp_clock.c": "ptp\n",
                          "linux/arch/mips/Kconfig": "mips\n",
                          "linux/Makefile": "VERSION = 6\n"})

    def series(self):
        """The usual series: a core change, a platform change, a driver."""
        self.repo.commit("net: dsa: offer flow blocks first", {"linux/net/dsa/user.c": "user 2\n"})
        self.repo.commit("MIPS: platform", {"linux/arch/mips/Kconfig": "mips 2\n"})
        self.repo.commit("net: dsa: tag driver", {"linux/net/dsa/tag_x.c": "tag\n",
                                                  "linux/net/dsa/user.c": "user 3\n"})
        self.repo.commit("net: new driver", {"linux/drivers/net/x.c": "x\n"})

    def test_list_names_the_files_the_series_changes_and_not_the_ones_it_adds(self):
        self.series()
        done = run(self.repo, "list")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.split(), ["arch/mips/Kconfig", "net/dsa/user.c"])

    def test_list_is_empty_for_a_series_that_only_adds(self):
        self.repo.commit("net: new driver", {"linux/drivers/net/x.c": "x\n"})
        done = run(self.repo, "list")
        self.assertEqual((done.returncode, done.stdout), (0, ""))

    def test_list_names_a_deleted_file(self):
        self.repo.remove("linux/drivers/ptp/ptp_clock.c", "ptp: remove")
        done = run(self.repo, "list")
        self.assertEqual(done.stdout.split(), ["drivers/ptp/ptp_clock.c"])

    def test_list_of_an_older_revision_leaves_out_what_came_after(self):
        self.series()
        done = run(self.repo, "list", "HEAD~3")
        self.assertEqual(done.stdout.split(), ["net/dsa/user.c"])

    def test_check_passes_when_the_page_names_every_file(self):
        self.series()
        self.repo.write(DOC, page("arch/mips/Kconfig", "net/dsa/user.c"))
        done = run(self.repo, "check")
        self.assertEqual((done.returncode, done.stderr), (0, ""))

    def test_check_fails_and_names_the_file_the_page_leaves_out(self):
        self.series()
        self.repo.write(DOC, page("net/dsa/user.c"))
        done = run(self.repo, "check")
        self.assertEqual(done.returncode, 1)
        self.assertIn("arch/mips/Kconfig", done.stderr)
        self.assertNotIn("net/dsa/user.c", done.stderr)

    def test_check_does_not_count_a_path_that_is_part_of_another(self):
        self.series()
        self.repo.write(DOC, page("linux/arch/mips/Kconfig.platforms", "net/dsa/user.c"))
        done = run(self.repo, "check")
        self.assertEqual(done.returncode, 1)
        self.assertIn("arch/mips/Kconfig", done.stderr)

    def test_check_fails_without_the_page(self):
        self.series()
        done = run(self.repo, "check")
        self.assertEqual(done.returncode, 1)
        self.assertIn(DOC, done.stderr)

    def test_check_with_a_revision_reads_the_page_of_that_commit(self):
        self.series()
        self.repo.commit("docs: the page",
                         {DOC: page("arch/mips/Kconfig", "net/dsa/user.c")})
        self.repo.write(DOC, "emptied in the working tree\n")
        self.assertEqual(run(self.repo, "check", "HEAD").returncode, 0)
        self.assertEqual(run(self.repo, "check").returncode, 1)

    def test_export_writes_the_core_commits_in_order_and_leaves_the_rest(self):
        self.series()
        out = os.path.join(self.tmp, "out")
        done = run(self.repo, "export", "-o", out)
        self.assertEqual(done.returncode, 0, done.stderr)
        names = sorted(os.listdir(out))
        self.assertEqual(names, ["0001-net-dsa-offer-flow-blocks-first.patch",
                                 "0002-net-dsa-tag-driver.patch"])
        self.assertEqual(sorted(os.path.basename(line) for line in done.stdout.split()), names)
        with open(os.path.join(out, names[1]), encoding="utf-8") as handle:
            text = handle.read()
        # The commit that adds a driver beside a core change goes out whole.
        self.assertIn("linux/net/dsa/tag_x.c", text)
        self.assertIn("linux/net/dsa/user.c", text)

    def test_export_fails_when_no_commit_touches_the_core(self):
        self.repo.commit("MIPS: platform", {"linux/arch/mips/Kconfig": "mips 2\n"})
        done = run(self.repo, "export", "-o", os.path.join(self.tmp, "out"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("networking core", done.stderr)

    def test_a_repository_without_the_import_is_refused(self):
        other = Repo(os.path.join(self.tmp, "other"))
        other.commit("just a commit", {"README": "x\n"})
        done = run(other, "list")
        self.assertEqual(done.returncode, 1)
        self.assertIn("import", done.stderr)

    def test_a_bad_command_or_revision_is_refused(self):
        self.assertEqual(run(self.repo, "nonsense").returncode, 2)
        self.assertEqual(run(self.repo).returncode, 2)
        done = run(self.repo, "list", "no-such-rev")
        self.assertEqual(done.returncode, 1)
        self.assertIn("not a commit", done.stderr)


if __name__ == "__main__":
    unittest.main()
