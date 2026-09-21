# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The allowlist: every entry is reasoned and narrow, and it matches exactly."""

from __future__ import annotations

import unittest

from gates.allowlist import Allowlist, AllowlistError
from gates.findings import Finding, Report

REASON = 'reason = "a reason of at least five words"\n'


def finding(**kwargs) -> Finding:
    values = dict(gate="forbidden-words", rule="code-address", message="m", path="linux/a.c",
                  line=1, col=5, text="x", value="v", digest="", kind="code", commit=None,
                  line_text="int x = 0;", span=(4, 5))
    values.update(kwargs)
    return Finding(**values)


def entry(body: str) -> Allowlist:
    return Allowlist.parse("[[allow]]\n" + body + REASON)


class Parsing(unittest.TestCase):

    def refused(self, text: str, why: str) -> None:
        with self.assertRaises(AllowlistError) as ctx:
            Allowlist.parse(text)
        self.assertIn(why, str(ctx.exception))

    def test_an_empty_file_is_an_empty_list(self) -> None:
        self.assertEqual(Allowlist.parse("").entries, [])

    def test_malformed_files_are_refused(self) -> None:
        self.refused("[[allow]\n", "<allowlist>")
        self.refused("[other]\nx = 1\n", "unknown top-level keys")
        self.refused("allow = 1\n", "array of tables")
        self.refused("allow = [1]\n", "not a table")

    def test_each_entry_is_checked(self) -> None:
        self.refused('[[allow]]\ngate = "nope"\n' + REASON, "gate must be one of")
        self.refused('[[allow]]\ngate = "binaries"\nsize = 1\n' + REASON, "unknown keys")
        self.refused('[[allow]]\ngate = "unit-ids"\npaths = ["a"]\n', "written reason")
        self.refused('[[allow]]\ngate = "unit-ids"\npaths = ["a"]\nreason = "too short"\n',
                     "written reason")
        self.refused('[[allow]]\ngate = "unit-ids"\n' + REASON, "narrow its scope")
        self.refused('[[allow]]\ngate = "unit-ids"\npaths = []\n' + REASON, "non-empty list")
        self.refused('[[allow]]\ngate = "unit-ids"\nmatch = 1\n' + REASON, "match must be")
        self.refused('[[allow]]\ngate = "unit-ids"\nmatch = "("\n' + REASON, "bad match regex")
        self.refused('[[allow]]\ngate = "denylist"\nhashes = ["ab"]\n' + REASON, "hashes must be")
        self.refused('[[allow]]\ngate = "commit-message"\ncommits = ["abc"]\n' + REASON,
                     "full commit ids")
        self.refused('[[allow]]\ngate = "binaries"\n' + REASON, "provenance line")
        self.refused('[[allow]]\ngate = "binaries"\nprovenance = "made by us here"\n'
                     'commits = ["' + "a" * 40 + '"]\n' + REASON, "must name its paths")
        self.refused('[[allow]]\ngate = "binaries"\npaths = ["a"]\nprovenance = "ours"\n'
                     + REASON, "where the file comes from")


class Matching(unittest.TestCase):

    def test_gate_and_rules(self) -> None:
        allow = entry('gate = "forbidden-words"\nrules = ["code-address"]\nvalues = ["v"]\n')
        self.assertTrue(allow.allows(finding()))
        self.assertFalse(allow.allows(finding(gate="unit-ids")))
        self.assertFalse(allow.allows(finding(rule="re-tool")))

    def test_paths_glob_across_directories_and_never_match_a_message(self) -> None:
        allow = entry('gate = "forbidden-words"\npaths = ["linux/*.c"]\n')
        self.assertTrue(allow.allows(finding(path="linux/drivers/net/a.c")))
        self.assertFalse(allow.allows(finding(path="omcid/a.c")))
        self.assertFalse(allow.allows(finding(path=None, kind="message")))

    def test_a_single_string_is_a_list_of_one(self) -> None:
        allow = entry('gate = "forbidden-words"\npaths = "linux/a.c"\n')
        self.assertEqual(allow.entries[0].paths, ("linux/a.c",))
        self.assertTrue(allow.allows(finding()))

    def test_values_ignore_case(self) -> None:
        allow = entry('gate = "forbidden-words"\nvalues = ["AbC"]\n')
        self.assertTrue(allow.allows(finding(value="abc")))
        self.assertTrue(allow.allows(finding(value="ABC")))
        self.assertFalse(allow.allows(finding(value="abd")))

    def test_hashes(self) -> None:
        digest = "ab" * 32
        allow = entry('gate = "denylist"\nhashes = ["%s"]\n' % digest)
        self.assertTrue(allow.allows(finding(gate="denylist", digest=digest)))
        self.assertFalse(allow.allows(finding(gate="denylist", digest="cd" * 32)))

    def test_commits(self) -> None:
        sha = "a" * 40
        allow = entry('gate = "commit-message"\ncommits = ["%s"]\n' % sha)
        self.assertTrue(allow.allows(finding(gate="commit-message", commit=sha)))
        self.assertFalse(allow.allows(finding(gate="commit-message", commit="b" * 40)))
        self.assertFalse(allow.allows(finding(gate="commit-message", commit=None)))

    def test_match_must_cover_the_finding(self) -> None:
        allow = entry('gate = "forbidden-words"\nmatch = "x = 0"\n')
        self.assertTrue(allow.allows(finding()))
        self.assertFalse(allow.allows(finding(span=(0, 5))))
        self.assertFalse(allow.allows(finding(line_text="int y = 0;")))

    def test_the_report_applies_the_list_and_counts_use(self) -> None:
        allow = Allowlist.parse(
            '[[allow]]\ngate = "forbidden-words"\nvalues = ["v"]\n' + REASON
            + '[[allow]]\ngate = "unit-ids"\nvalues = ["w"]\n' + REASON)
        report = Report(allow)
        report.extend([finding(), finding(value="other")])
        self.assertEqual(len(report.allowed), 1)
        self.assertEqual(len(report.findings), 1)
        self.assertFalse(report.ok())
        self.assertEqual([e.label() for e in allow.unused()], ["#2 (unit-ids)"])
        text = report.render_text(verbose=True)
        self.assertIn("allowed by the allowlist: 1", text)
        self.assertIn("#2 (unit-ids) matched nothing", text)


if __name__ == "__main__":
    unittest.main()
