# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The lab directory: where the deny-list and the lab's own paths come from.

The lab's list and patterns never enter this repository, so these tests make
a lab of their own: a small list under a test salt and a synthetic pattern,
in a temporary directory. With a lab every gate runs; without one the
deny-list gate and the lab-path rule are skipped, with one line on stderr,
and every other gate runs as before. Only when the environment names a lab
directory is the real one read: its list must then be well formed and live,
which its canary proves.
"""

from __future__ import annotations

import os
import unittest

from gates import GATES
from gates.denylist import CANARY_CATEGORY, CATEGORIES, canary
from gates.lab import ENV, LAB_PATHS_FILE, LabError, load, parse_lab_paths

from .support import (LAB_PATH, LAB_PATTERN, NO_LAB, Repo, Seeds, TempDirTestCase, make_denylist,
                      make_lab, pairs, run_check)

SKIPPED = ("tools/provenance: %s is not set, so the deny-list gate and the lab-path rule are "
           "skipped here and run on the maintainer's machine before every push\n" % ENV)


class LabPathsFile(unittest.TestCase):

    def test_comments_blank_lines_and_white_space_are_left_out(self) -> None:
        text = "# a comment\n\n  %s  \n\t# another\n(?i:\\bscratch\\b)\n" % LAB_PATTERN
        self.assertEqual(parse_lab_paths(text), [LAB_PATTERN, r"(?i:\bscratch\b)"])

    def test_a_bad_pattern_is_refused_with_its_line(self) -> None:
        with self.assertRaises(LabError) as ctx:
            parse_lab_paths("# fine\n%s\n(unclosed\n" % LAB_PATTERN, origin="lab-paths")
        self.assertIn("lab-paths:3: bad pattern", str(ctx.exception))


class LabDirectory(TempDirTestCase):

    def setUp(self) -> None:
        super().setUp()
        self.seeds = Seeds()
        self.symbol = self.seeds.identifier()
        self.lab = make_lab(os.path.join(self.tmp, "lab"), [(self.symbol, "token", "symbol")],
                            [LAB_PATTERN])
        self.repo = Repo(os.path.join(self.tmp, "repo"))
        self.repo.write("tools/demo/README.rst", "A demonstration tool.\n")
        self.repo.write("tools/demo/notes.rst", "Count with %s, as %s says.\nReach it at %s.\n"
                        % (self.symbol, LAB_PATH, self.seeds.private_ipv4()))
        self.repo.commit("tools/demo: add a demonstration tool", "Count frames with it.")
        self.everything = {("denylist", "symbol"), ("forbidden-words", "lab-path"),
                           ("unit-ids", "ipv4-address")}

    def test_with_a_lab_every_gate_runs(self) -> None:
        code, report = run_check("all", "--history", repo=self.repo.path, lab=self.lab)
        self.assertEqual(code, 1)
        self.assertEqual(pairs(report), self.everything)
        self.assertEqual(report["stderr"], "")

    def test_the_environment_names_the_lab(self) -> None:
        code, report = run_check("tree", repo=self.repo.path, lab=NO_LAB, environ={ENV: self.lab})
        self.assertEqual((code, pairs(report), report["stderr"]), (1, self.everything, ""))

    def test_without_a_lab_the_other_gates_run(self) -> None:
        for environ in ({}, {ENV: ""}):
            code, report = run_check("all", "--history", repo=self.repo.path, lab=NO_LAB,
                                     environ=environ)
            self.assertEqual(code, 1)
            self.assertEqual(pairs(report), {("unit-ids", "ipv4-address")})
            self.assertEqual(report["stderr"], SKIPPED)
            self.assertEqual(report["scanned_commits"], 1)

    def test_a_clean_tree_passes_without_a_lab(self) -> None:
        repo = Repo(os.path.join(self.tmp, "clean"))
        repo.write("tools/demo/README.rst", "A demonstration tool.\n")
        repo.commit("tools/demo: add a demonstration tool", "Count frames with it.")
        code, report = run_check("all", "--history", repo=repo.path, lab=NO_LAB)
        self.assertEqual((code, report["ok"], report["stderr"]), (0, True, SKIPPED))

    def test_only_what_the_selected_gates_need_is_skipped(self) -> None:
        code, report = run_check("--gates", "unit-ids", "tree", repo=self.repo.path, lab=NO_LAB)
        self.assertEqual((code, report["stderr"]), (1, ""))
        code, report = run_check("--gates", "forbidden-words", "tree", repo=self.repo.path,
                                 lab=NO_LAB)
        self.assertEqual(code, 0)
        self.assertIn("so the lab-path rule is skipped here", report["stderr"])
        # A list given on the command line stands in for the lab's.
        denylist = os.path.join(self.tmp, "other.sha256")
        make_denylist(denylist, [(self.symbol, "token", "symbol")])
        code, report = run_check("tree", repo=self.repo.path, lab=NO_LAB, denylist=denylist)
        self.assertEqual(pairs(report), {("denylist", "symbol"), ("unit-ids", "ipv4-address")})
        self.assertIn("so the lab-path rule is skipped here", report["stderr"])
        self.assertEqual(report["stderr"].count("\n"), 1)

    def test_a_lab_that_is_named_must_be_whole(self) -> None:
        missing = os.path.join(self.tmp, "no-such-lab")
        for lab, environ in ((missing, None), (NO_LAB, {ENV: missing})):
            code, report = run_check("tree", repo=self.repo.path, lab=lab, environ=environ)
            self.assertEqual(code, 2)
            self.assertIn("does not exist", report["stderr"])
        os.remove(os.path.join(self.lab, LAB_PATHS_FILE))
        code, report = run_check("tree", repo=self.repo.path, lab=self.lab)
        self.assertEqual(code, 2)
        self.assertIn(LAB_PATHS_FILE, report["stderr"])
        bad = make_lab(os.path.join(self.tmp, "bad"), [], ["(unclosed"])
        code, report = run_check("tree", repo=self.repo.path, lab=bad)
        self.assertEqual(code, 2)
        self.assertIn("bad pattern", report["stderr"])

    def test_the_lab_path_rule_reads_commit_messages_too(self) -> None:
        base = self.repo.head()
        self.repo.write("tools/demo/README.rst", "A demonstration tool, with notes.\n")
        self.repo.commit("tools/demo: mention the notes", "Write them up as %s does." % LAB_PATH)
        code, report = run_check("range", "%s..HEAD" % base, repo=self.repo.path, lab=self.lab)
        found = [(f["rule"], f["kind"]) for f in report["findings"]]
        self.assertEqual((code, found), (1, [("lab-path", "message")]))


@unittest.skipUnless(os.environ.get(ENV), "%s is not set: no lab directory to prove" % ENV)
class TheLabsOwnFiles(TempDirTestCase):
    """The real lab directory, on the maintainer's machine only."""

    def setUp(self) -> None:
        super().setUp()
        self.directory = os.environ[ENV]
        self.lab = load(GATES, self.directory)

    def test_they_load_and_the_list_is_well_formed(self) -> None:
        categories = {category for _match, category in self.lab.denylist.entries.values()}
        self.assertIn(CANARY_CATEGORY, categories)
        self.assertLessEqual(categories, set(CATEGORIES))
        self.assertEqual([rule.id for rule in self.lab.rules], ["lab-path"])

    def test_the_canary_trips_the_lab_list(self) -> None:
        token = canary(self.lab.denylist.salt)
        repo = Repo(os.path.join(self.tmp, "repo"))
        repo.write("tools/demo/demo.c", "int %s;\n" % token.upper())
        repo.commit("tools/demo: add a counter", "Add it.")
        code, report = run_check("tree", repo=repo.path, lab=self.directory)
        self.assertEqual(code, 1)
        self.assertEqual(pairs(report), {("denylist", CANARY_CATEGORY)})

    def test_a_fresh_word_does_not_trip_it(self) -> None:
        self.assertEqual(self.lab.denylist.lookup("frames_per_burst_counter"), ())


if __name__ == "__main__":
    unittest.main()
