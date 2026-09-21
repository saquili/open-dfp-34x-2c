# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Binary detection, line-restricted scanning and how findings read."""

from __future__ import annotations

import json
import unittest

from gates.content import ContentScanner, is_binary
from gates.findings import Finding, Report

from .support import Seeds


class Binaries(unittest.TestCase):

    def test_what_counts_as_binary(self) -> None:
        self.assertFalse(is_binary(b""))
        self.assertFalse(is_binary("text with tabs\tand\r\nform\x0cfeeds\x1b[0m\n".encode()))
        self.assertTrue(is_binary(b"text\0"))
        self.assertTrue(is_binary(bytes(range(1, 32)) * 4))
        self.assertFalse(is_binary(b"a" * 95 + b"\x01" * 5))


class LineRestriction(unittest.TestCase):

    def test_only_the_given_lines_are_reported(self) -> None:
        address = Seeds().private_ipv4()
        text = "line one\nreach %s\nline three\n" % address
        scanner = ContentScanner()
        self.assertEqual(len(scanner.scan_text("a.txt", text)), 1)
        self.assertEqual(len(scanner.scan_text("a.txt", text, lines={2})), 1)
        self.assertEqual(scanner.scan_text("a.txt", text, lines={1, 3}), [])
        self.assertEqual(scanner.scan_text("a.txt", text, lines=set()), [])
        self.assertEqual(scanner.scan_text("a.txt", ""), [])


class Rendering(unittest.TestCase):

    def test_where_a_finding_sits(self) -> None:
        sha = "c" * 40
        cases = [
            (Finding("g", "r", "m", path="a.c", line=3, col=7), "a.c:3:7"),
            (Finding("g", "r", "m", path="a.c", commit=sha), "cccccccccccc:a.c"),
            (Finding("g", "r", "m", commit=sha, kind="message", line=2), "commit cccccccccccc message:2"),
            (Finding("g", "r", "m", commit=sha, kind="commit"), "commit cccccccccccc"),
            (Finding("g", "r", "m", kind="message", line=1), "commit <message> message:1"),
            (Finding("g", "r", "m", path="a.c", line=4), "a.c:4"),
        ]
        for finding, where in cases:
            self.assertEqual(finding.where(), where)

    def test_render_shortens_long_text(self) -> None:
        finding = Finding("g", "r", "a message", path="a.c", text="x" * 70 + "\ny", kind="code")
        self.assertEqual(finding.render(), 'a.c [code]: r: "%s..." a message' % ("x" * 57))
        commit = Finding("g", "r", "m", commit="d" * 40, kind="commit")
        self.assertEqual(commit.render(), "commit dddddddddddd: r: m")

    def test_json_report(self) -> None:
        report = Report()
        report.add(Finding("unit-ids", "r", "m", path="a.c", line=1, col=1, span=(0, 1),
                           line_text="secret line"))
        report.notes.append("a note")
        data = json.loads(report.render_json())
        self.assertFalse(data["ok"])
        self.assertEqual(data["counts"]["unit-ids"], 1)
        self.assertNotIn("line_text", data["findings"][0])
        self.assertNotIn("span", data["findings"][0])
        self.assertEqual(data["notes"], ["a note"])

    def test_text_report_counts_and_passes(self) -> None:
        report = Report()
        self.assertTrue(report.ok())
        self.assertIn("tools/provenance: ok -- 0 files, 0 commits scanned", report.render_text())
        report.scanned_files = 1
        report.add(Finding("binaries", "binary-content", "binary", path="a.bin", kind="file"))
        report.add(Finding("binaries", "file-size", "big", path="a.bin", kind="file"))
        text = report.render_text()
        self.assertIn("== binaries: 2 findings", text)
        self.assertIn("FAIL -- 1 file, 0 commits scanned", text)


if __name__ == "__main__":
    unittest.main()
