# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The allowlist: justified exceptions to the gates.

The allowlist is a TOML file holding an array of ``[[allow]]`` tables. Each
entry names the gate it relaxes and carries a written ``reason``; the other
keys narrow where it applies, and every key given must match:

``gate``        required; one of the gate names.
``rules``       rule ids within the gate (default: every rule of the gate).
``paths``       glob patterns of repository paths (``*`` crosses ``/``);
                an entry with ``paths`` never applies to a commit message.
``match``       a regular expression searched in the finding's line; the
                entry applies only where one of its matches covers the
                finding.
``values``      exact normalised values (an address, a MAC, a serial).
``hashes``      deny-list digests (the gate reports the digest it hit).
``commits``     full commit ids; the entry applies to findings of those
                commits only (their messages and their changes).
``provenance``  required for the binaries gate: where the file comes from.
``reason``      required: why the exception is right.
"""

from __future__ import annotations

import fnmatch
import re
import tomllib
from dataclasses import dataclass, field
from typing import Optional

from . import GATE_BINARIES, GATES

_KEYS = {"gate", "rules", "paths", "match", "values", "hashes", "commits", "provenance", "reason"}
_MIN_REASON_WORDS = 5


class AllowlistError(ValueError):
    """The allowlist file is malformed."""


@dataclass
class Entry:
    index: int
    gate: str
    reason: str
    rules: Optional[frozenset[str]] = None
    paths: Optional[tuple[str, ...]] = None
    match: Optional[re.Pattern] = None
    values: Optional[frozenset[str]] = None
    hashes: Optional[frozenset[str]] = None
    commits: Optional[frozenset[str]] = None
    provenance: Optional[str] = None
    used: int = field(default=0)

    def label(self) -> str:
        return "#%d (%s)" % (self.index, self.gate)

    def allows(self, finding) -> bool:
        if finding.gate != self.gate:
            return False
        if self.rules is not None and finding.rule not in self.rules:
            return False
        if self.commits is not None and finding.commit not in self.commits:
            return False
        if self.paths is not None:
            if finding.path is None:
                return False
            if not any(fnmatch.fnmatchcase(finding.path, pat) for pat in self.paths):
                return False
        if self.values is not None and finding.value.lower() not in self.values:
            return False
        if self.hashes is not None and finding.digest not in self.hashes:
            return False
        if self.match is not None:
            start, end = finding.span
            if not any(m.start() <= start and end <= m.end()
                       for m in self.match.finditer(finding.line_text)):
                return False
        return True


def _strings(value, key: str, where: str) -> tuple[str, ...]:
    if isinstance(value, str):
        value = [value]
    if not isinstance(value, list) or not value or not all(isinstance(v, str) and v for v in value):
        raise AllowlistError("%s: %s must be a string or a non-empty list of strings" % (where, key))
    return tuple(value)


class Allowlist:
    """A parsed allowlist."""

    def __init__(self, entries: list[Entry], origin: str = "<allowlist>") -> None:
        self.entries = entries
        self.origin = origin

    @classmethod
    def parse(cls, text: str, origin: str = "<allowlist>") -> "Allowlist":
        try:
            data = tomllib.loads(text)
        except tomllib.TOMLDecodeError as exc:
            raise AllowlistError("%s: %s" % (origin, exc)) from exc
        unknown_top = set(data) - {"allow"}
        if unknown_top:
            raise AllowlistError("%s: unknown top-level keys %s" % (origin, sorted(unknown_top)))
        raw_entries = data.get("allow", [])
        if not isinstance(raw_entries, list):
            raise AllowlistError("%s: 'allow' must be an array of tables" % origin)
        entries = []
        for index, raw in enumerate(raw_entries, 1):
            where = "%s: entry %d" % (origin, index)
            if not isinstance(raw, dict):
                raise AllowlistError("%s: not a table" % where)
            unknown = set(raw) - _KEYS
            if unknown:
                raise AllowlistError("%s: unknown keys %s" % (where, sorted(unknown)))
            gate = raw.get("gate")
            if gate not in GATES:
                raise AllowlistError("%s: gate must be one of %s" % (where, ", ".join(GATES)))
            reason = raw.get("reason")
            if not isinstance(reason, str) or len(reason.split()) < _MIN_REASON_WORDS:
                raise AllowlistError("%s: every entry needs a written reason (at least %d words)"
                                     % (where, _MIN_REASON_WORDS))
            entry = Entry(index=index, gate=gate, reason=" ".join(reason.split()))
            if "rules" in raw:
                entry.rules = frozenset(_strings(raw["rules"], "rules", where))
            if "paths" in raw:
                entry.paths = _strings(raw["paths"], "paths", where)
            if "match" in raw:
                if not isinstance(raw["match"], str):
                    raise AllowlistError("%s: match must be a string" % where)
                try:
                    entry.match = re.compile(raw["match"])
                except re.error as exc:
                    raise AllowlistError("%s: bad match regex: %s" % (where, exc)) from exc
            if "values" in raw:
                entry.values = frozenset(v.lower() for v in _strings(raw["values"], "values", where))
            if "hashes" in raw:
                hashes = _strings(raw["hashes"], "hashes", where)
                if not all(re.fullmatch(r"[0-9a-f]{64}", h) for h in hashes):
                    raise AllowlistError("%s: hashes must be 64 lower-case hex digits" % where)
                entry.hashes = frozenset(hashes)
            if "commits" in raw:
                commits = _strings(raw["commits"], "commits", where)
                if not all(re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", c) for c in commits):
                    raise AllowlistError("%s: commits must be full commit ids" % where)
                entry.commits = frozenset(commits)
            if "provenance" in raw:
                if not isinstance(raw["provenance"], str) or len(raw["provenance"].split()) < 3:
                    raise AllowlistError("%s: provenance must say where the file comes from" % where)
                entry.provenance = raw["provenance"]
            if gate == GATE_BINARIES and entry.provenance is None:
                raise AllowlistError("%s: a binaries entry needs a provenance line" % where)
            if gate == GATE_BINARIES and entry.paths is None:
                raise AllowlistError("%s: a binaries entry must name its paths" % where)
            if entry.paths is None and entry.commits is None and entry.values is None \
                    and entry.hashes is None and entry.match is None:
                raise AllowlistError("%s: an entry must narrow its scope (paths, commits, values, "
                                     "hashes or match); a whole gate cannot be switched off" % where)
            entries.append(entry)
        return cls(entries, origin)

    @classmethod
    def load(cls, path: str) -> "Allowlist":
        with open(path, encoding="utf-8") as handle:
            return cls.parse(handle.read(), origin=path)

    def allows(self, finding) -> bool:
        for entry in self.entries:
            if entry.allows(finding):
                entry.used += 1
                return True
        return False

    def unused(self) -> list[Entry]:
        return [entry for entry in self.entries if not entry.used]
