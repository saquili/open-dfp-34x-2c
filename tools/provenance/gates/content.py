# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Content gates: deny-list, forbidden words, unit identifiers, binaries."""

from __future__ import annotations

import bisect
from typing import Iterable, Optional

from . import GATE_BINARIES, GATE_DENYLIST, GATES
from .denylist import CATEGORIES, DenyList, MATCH_PART
from .findings import Finding
from .lexer import SegmentMap
from .rules import ALL_RULES, Rule
from .tokens import tokenize

# Text files above this size are refused unless the allowlist names them.
DEFAULT_SIZE_CAP = 512 * 1024
# A file is binary if it holds a NUL byte, or if more than this share of its
# first 64 KiB are control characters other than tab, newline, carriage
# return, form feed and escape.
_CONTROL_SHARE = 0.10
_PROBE = 64 * 1024
_TEXT_CONTROLS = {0x09, 0x0A, 0x0C, 0x0D, 0x1B}


def is_binary(data: bytes) -> bool:
    """Decide whether a file's bytes are binary rather than text."""
    if b"\0" in data:
        return True
    probe = data[:_PROBE]
    if not probe:
        return False
    controls = sum(1 for byte in probe if (byte < 0x20 and byte not in _TEXT_CONTROLS) or byte == 0x7F)
    return controls > _CONTROL_SHARE * len(probe)


class _Lines:
    """Offset-to-line arithmetic for one text."""

    def __init__(self, text: str) -> None:
        self.text = text
        self.starts = [0]
        pos = text.find("\n")
        while pos != -1:
            self.starts.append(pos + 1)
            pos = text.find("\n", pos + 1)

    def line_of(self, offset: int) -> int:
        return bisect.bisect_right(self.starts, offset)

    def line_span(self, line: int) -> tuple[int, int]:
        start = self.starts[line - 1]
        end = self.text.find("\n", start)
        return start, (len(self.text) if end == -1 else end)


class ContentScanner:
    """Runs the content gates over one file or one commit message at a time."""

    def __init__(self, denylist: Optional[DenyList] = None, gates: Iterable[str] = GATES,
                 size_cap: int = DEFAULT_SIZE_CAP) -> None:
        self.denylist = denylist
        self.gates = set(gates)
        self.size_cap = size_cap
        self.rules = [rule for rule in ALL_RULES if rule.gate in self.gates]

    # -- bytes ---------------------------------------------------------------

    def check_blob(self, path: str, data: bytes, *, check_size: bool = True,
                   commit: Optional[str] = None) -> tuple[list[Finding], Optional[str]]:
        """Binaries-and-size gate. Returns findings and the text, or None if binary."""
        findings: list[Finding] = []
        binary = is_binary(data)
        if GATE_BINARIES in self.gates:
            if check_size and len(data) > self.size_cap:
                findings.append(Finding(
                    GATE_BINARIES, "file-size",
                    "%d bytes, above the %d-byte cap" % (len(data), self.size_cap),
                    path=path, kind="file", commit=commit, value=str(len(data))))
            if binary:
                findings.append(Finding(
                    GATE_BINARIES, "binary-content",
                    "binary content (NUL or control bytes); binaries need an allowlist entry "
                    "with their provenance", path=path, kind="file", commit=commit))
        if binary:
            return findings, None
        return findings, data.decode("utf-8", errors="replace")

    # -- text ----------------------------------------------------------------

    def scan_text(self, path: Optional[str], text: str, *, lines: Optional[set[int]] = None,
                  commit: Optional[str] = None, kind: Optional[str] = None,
                  extra_rules: Iterable[Rule] = ()) -> list[Finding]:
        """Run the text gates over ``text``.

        ``lines`` restricts findings to those 1-based line numbers (the added
        lines of a change); ``kind`` forces the segment kind (a commit message
        is all "message"); ``extra_rules`` adds rules for this call only.
        """
        if not text or (lines is not None and not lines):
            return []
        index = _Lines(text)
        segments = None if kind else SegmentMap(path or "", text)
        findings: list[Finding] = []

        def wanted(start: int, end: int) -> bool:
            if lines is None:
                return True
            first = index.line_of(start)
            last = index.line_of(max(start, end - 1))
            return any(n in lines for n in range(first, last + 1))

        def make(gate: str, rule: str, message: str, start: int, end: int, **extra) -> Finding:
            line = index.line_of(start)
            line_start, line_end = index.line_span(line)
            return Finding(gate, rule, message, path=path, line=line, col=start - line_start + 1,
                           text=text[start:end], kind=kind or segments.kind_at(start),
                           commit=commit, line_text=text[line_start:line_end],
                           span=(start - line_start, min(end, line_end) - line_start), **extra)

        for rule in list(self.rules) + list(extra_rules):
            for m in rule.pattern.finditer(text):
                start, end = m.span()
                if start == end or not wanted(start, end):
                    continue
                where = kind or segments.kind_at(start)
                detail = rule.check(m, where, path or "") if rule.check else rule.message
                if detail is None:
                    continue
                findings.append(make(rule.gate, rule.id, detail, start, end, value=rule.value(m)))

        if self.denylist is not None and GATE_DENYLIST in self.gates:
            for tok in tokenize(text):
                hits = self.denylist.lookup(tok.norm)
                if not hits or not wanted(tok.start, tok.end):
                    continue
                for hit in hits:
                    what = CATEGORIES.get(hit.category, hit.category)
                    message = "matches the deny-list: %s" % what
                    if hit.match == MATCH_PART and hit.run != tok.norm:
                        message += " (the run \"%s\" inside it)" % hit.run
                    findings.append(make(GATE_DENYLIST, hit.category, message, tok.start, tok.end,
                                         value=hit.run, digest=hit.digest))
        findings.sort(key=lambda f: (f.line or 0, f.col or 0, f.gate, f.rule))
        return findings
