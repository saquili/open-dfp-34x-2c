# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Findings and the report that collects them."""

from __future__ import annotations

import collections
import json
from dataclasses import asdict, dataclass, field
from typing import Optional

from . import GATES

# Where a finding sits when it is not in a file.
KIND_MESSAGE = "message"
KIND_COMMIT = "commit"


@dataclass
class Finding:
    """One violation of one gate."""

    gate: str
    rule: str
    message: str
    path: Optional[str] = None       # repository path; None for a commit message
    line: Optional[int] = None
    col: Optional[int] = None
    text: str = ""                   # the offending text, as found
    value: str = ""                  # normalised value, for allowlist "values"
    digest: str = ""                 # deny-list digest, for allowlist "hashes"
    kind: str = ""                   # code, comment, string, text, message, commit
    commit: Optional[str] = None     # full commit id, in range mode
    line_text: str = field(default="", repr=False)   # for allowlist "match"
    span: tuple[int, int] = field(default=(0, 0), repr=False)

    def where(self) -> str:
        if self.path is None:
            place = "commit %s" % (self.commit[:12] if self.commit else "<message>")
            place += " message" if self.kind == KIND_MESSAGE else ""
        else:
            place = self.path
            if self.commit:
                place = "%s:%s" % (self.commit[:12], place)
        if self.line is not None:
            place += ":%d" % self.line
            if self.col is not None:
                place += ":%d" % self.col
        return place

    def render(self) -> str:
        kind = " [%s]" % self.kind if self.kind and self.kind not in (KIND_COMMIT,) else ""
        shown = ""
        if self.text:
            text = self.text if len(self.text) <= 60 else self.text[:57] + "..."
            shown = ' "%s"' % text.replace("\n", " ")
        return "%s%s: %s:%s %s" % (self.where(), kind, self.rule, shown, self.message)


class Report:
    """Collects findings, applies the allowlist and summarises."""

    def __init__(self, allowlist=None) -> None:
        self.allowlist = allowlist
        self.findings: list[Finding] = []
        self.allowed: list[Finding] = []
        self.notes: list[str] = []
        self.scanned_files = 0
        self.scanned_commits = 0

    def add(self, finding: Finding) -> None:
        if self.allowlist is not None and self.allowlist.allows(finding):
            self.allowed.append(finding)
        else:
            self.findings.append(finding)

    def extend(self, findings) -> None:
        for finding in findings:
            self.add(finding)

    def counts(self) -> dict[str, int]:
        counter = collections.Counter(f.gate for f in self.findings)
        return {gate: counter.get(gate, 0) for gate in GATES}

    def ok(self) -> bool:
        return not self.findings

    def render_text(self, verbose: bool = False) -> str:
        out = []
        by_gate = collections.defaultdict(list)
        for finding in self.findings:
            by_gate[finding.gate].append(finding)
        for gate in GATES:
            items = by_gate.get(gate)
            if not items:
                continue
            out.append("== %s: %d finding%s" % (gate, len(items), "" if len(items) == 1 else "s"))
            out.extend("  " + f.render() for f in items)
        if verbose and self.allowed:
            out.append("== allowed by the allowlist: %d" % len(self.allowed))
            out.extend("  " + f.render() for f in self.allowed)
        if verbose and self.allowlist is not None:
            for entry in self.allowlist.unused():
                out.append("note: allowlist entry %s matched nothing in this run" % entry.label())
        out.extend("note: " + note for note in self.notes)
        counts = self.counts()
        summary = ", ".join("%s %d" % (gate, counts[gate]) for gate in GATES)
        out.append("tools/provenance: %s -- %d file%s, %d commit%s scanned; %s; %d allowed"
                   % ("FAIL" if self.findings else "ok",
                      self.scanned_files, "" if self.scanned_files == 1 else "s",
                      self.scanned_commits, "" if self.scanned_commits == 1 else "s",
                      summary, len(self.allowed)))
        return "\n".join(out)

    def render_json(self) -> str:
        def export(f: Finding) -> dict:
            data = asdict(f)
            data.pop("line_text", None)
            data.pop("span", None)
            return data
        return json.dumps({
            "ok": self.ok(),
            "counts": self.counts(),
            "scanned_files": self.scanned_files,
            "scanned_commits": self.scanned_commits,
            "findings": [export(f) for f in self.findings],
            "allowed": [export(f) for f in self.allowed],
            "notes": self.notes,
        }, indent=2)
