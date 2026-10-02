# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of tree-digest."""

import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(HERE, "tree-digest")


def digest(path):
    done = subprocess.run([sys.executable, TOOL, path], capture_output=True, text=True)
    return done.returncode, done.stdout, done.stderr


def make(top, order):
    os.makedirs(os.path.join(top, "etc"), exist_ok=True)
    for name in order:
        with open(os.path.join(top, "etc", name), "w") as handle:
            handle.write("content of %s\n" % name)
    os.symlink("a", os.path.join(top, "etc", "link"))
    os.chmod(os.path.join(top, "etc", "a"), 0o644)
    os.chmod(os.path.join(top, "etc", "b"), 0o755)


class TreeDigest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name

    def tree(self, name, order=("a", "b")):
        top = os.path.join(self.root, name)
        os.makedirs(top)
        make(top, order)
        return top

    def test_it_lists_files_directories_and_links_sorted_by_path(self):
        status, out, err = digest(self.tree("t"))
        self.assertEqual(status, 0, err)
        lines = out.splitlines()
        self.assertEqual([l.split()[-1] if l[0] != "l" else l.split()[4] for l in lines],
                         ["etc", "etc/a", "etc/b", "etc/link"])
        want = hashlib.sha256(b"content of a\n").hexdigest()
        self.assertIn("f 0644 13 %s etc/a" % want, lines)
        self.assertIn("l ", out)
        self.assertIn("etc/link -> a", out)

    def test_the_order_files_were_made_in_does_not_matter(self):
        a = digest(self.tree("a", ("a", "b")))[1]
        b = digest(self.tree("b", ("b", "a")))[1]
        self.assertEqual(a, b)

    def test_times_and_owners_are_left_out(self):
        top = self.tree("t")
        before = digest(top)[1]
        os.utime(os.path.join(top, "etc", "a"), (1, 1))
        self.assertEqual(digest(top)[1], before)

    def test_a_changed_byte_a_changed_mode_and_a_changed_link_each_show(self):
        top = self.tree("t")
        before = digest(top)[1]
        with open(os.path.join(top, "etc", "a"), "a") as handle:
            handle.write("x")
        changed = digest(top)[1]
        self.assertNotEqual(changed, before)
        os.chmod(os.path.join(top, "etc", "b"), 0o700)
        self.assertNotEqual(digest(top)[1], changed)
        os.remove(os.path.join(top, "etc", "link"))
        os.symlink("b", os.path.join(top, "etc", "link"))
        self.assertIn("etc/link -> b", digest(top)[1])

    def test_a_file_that_is_not_a_directory_is_refused(self):
        path = os.path.join(self.root, "file")
        open(path, "w").close()
        self.assertEqual(digest(path)[0], 2)
        self.assertEqual(digest(os.path.join(self.root, "none"))[0], 2)


if __name__ == "__main__":
    unittest.main()
