# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The hashed deny-list: file format, loading and lookup.

The list is kept in the project's local lab, not in this repository; lab.py
says where the gates find it. This module and its tests are all that live
here.

The file is plain text. Comment lines start with ``#``. The directives are::

    format rtl9601d-provenance-denylist 1
    salt <hex>
    parts-max <n>

followed by one entry per line::

    <sha256-hex> <match> <category>

``<sha256-hex>`` is SHA-256 over the salt bytes followed by the UTF-8
normalised token (see tokens.py). ``<match>`` is ``token`` -- the entry
matches a whole token -- or ``part`` -- it also matches any run of whole
underscore-separated components inside an identifier. ``<category>`` says
what kind of name the entry stands for, so that a report can say why a token
was refused without the list ever holding the name itself.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from typing import Iterable, NamedTuple

from .tokens import DEFAULT_MAX_PART_COMPONENTS, Hasher, component_runs

FORMAT_LINE = "format rtl9601d-provenance-denylist 1"

MATCH_TOKEN = "token"
MATCH_PART = "part"
MATCHES = (MATCH_TOKEN, MATCH_PART)

# What each category stands for, as printed in a report.
CATEGORIES = {
    "symbol": "a function or data symbol of other software",
    "source-file": "a source-file name of other software",
    "module": "a module, library or program name of other software",
    "address": "a code address inside other software",
    "me-table": "a managed-entity name of another implementation",
    "register": "a register name that is not of this project's naming",
    "field": "a register-field name that is not of this project's naming",
    "internal": "an internal name of the lab",
    "test": "a test entry",
}

# Every generated list carries one entry of this category: the canary.
CANARY_CATEGORY = "test"

_HEX64 = re.compile(r"[0-9a-f]{64}")


def canary(salt: bytes) -> str:
    """The test token every generated list carries, derived from its salt.

    It names nothing. The self-test seeds it to prove that the lab's list,
    its salt and the committed tokeniser agree end to end.
    """
    return "denylist_canary_" + hashlib.sha256(b"canary:" + salt).hexdigest()[:16]
_CATEGORY = re.compile(r"[a-z][a-z0-9-]*")


class DenyListError(ValueError):
    """The deny-list file is malformed."""


class Hit(NamedTuple):
    """A deny-list match: the matching run of the token and its entry."""

    run: str
    digest: str
    match: str
    category: str


@dataclass
class DenyList:
    """A loaded deny-list."""

    salt: bytes
    entries: dict[str, tuple[str, str]]
    parts_max: int = DEFAULT_MAX_PART_COMPONENTS

    def __post_init__(self) -> None:
        self._hash = Hasher(self.salt)
        self._has_parts = any(m == MATCH_PART for m, _ in self.entries.values())
        self._cache: dict[str, tuple[Hit, ...]] = {}

    @classmethod
    def parse(cls, text: str, origin: str = "<deny-list>") -> "DenyList":
        """Parse the text of a deny-list file."""
        salt = None
        parts_max = DEFAULT_MAX_PART_COMPONENTS
        seen_format = False
        entries: dict[str, tuple[str, str]] = {}
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            where = "%s:%d" % (origin, lineno)
            if line.startswith("format "):
                if line != FORMAT_LINE:
                    raise DenyListError("%s: unknown format %r" % (where, line))
                seen_format = True
                continue
            if not seen_format:
                raise DenyListError("%s: the format line must come first" % where)
            fields = line.split()
            if fields[0] == "salt" and len(fields) == 2:
                try:
                    salt = bytes.fromhex(fields[1])
                except ValueError as exc:
                    raise DenyListError("%s: bad salt" % where) from exc
                continue
            if fields[0] == "parts-max" and len(fields) == 2 and fields[1].isdigit():
                parts_max = int(fields[1])
                continue
            if len(fields) != 3 or not _HEX64.fullmatch(fields[0]):
                raise DenyListError("%s: expected '<sha256> <match> <category>'" % where)
            digest, match, category = fields
            if match not in MATCHES:
                raise DenyListError("%s: unknown match kind %r" % (where, match))
            if not _CATEGORY.fullmatch(category):
                raise DenyListError("%s: bad category %r" % (where, category))
            if digest in entries:
                raise DenyListError("%s: duplicate entry" % where)
            entries[digest] = (match, category)
        if not seen_format:
            raise DenyListError("%s: missing format line" % origin)
        if salt is None:
            raise DenyListError("%s: missing salt" % origin)
        return cls(salt=salt, entries=entries, parts_max=parts_max)

    @classmethod
    def load(cls, path: str) -> "DenyList":
        """Read and parse a deny-list file."""
        with open(path, encoding="utf-8") as handle:
            return cls.parse(handle.read(), origin=path)

    def lookup(self, norm: str) -> tuple[Hit, ...]:
        """Return the entries a normalised token hits (cached)."""
        cached = self._cache.get(norm)
        if cached is not None:
            return cached
        hits: list[Hit] = []
        digest = self._hash(norm)
        entry = self.entries.get(digest)
        if entry is not None:
            hits.append(Hit(norm, digest, entry[0], entry[1]))
        if self._has_parts:
            seen = {norm}
            for run in component_runs(norm, self.parts_max):
                if run in seen:
                    continue
                seen.add(run)
                digest = self._hash(run)
                entry = self.entries.get(digest)
                if entry is not None and entry[0] == MATCH_PART:
                    hits.append(Hit(run, digest, entry[0], entry[1]))
        result = tuple(hits)
        self._cache[norm] = result
        return result

    def __len__(self) -> int:
        return len(self.entries)


def format_denylist(
    salt: bytes,
    entries: Iterable[tuple[str, str, str]],
    header: Iterable[str],
    parts_max: int = DEFAULT_MAX_PART_COMPONENTS,
) -> str:
    """Render a deny-list file: header comment lines, directives, entries.

    ``entries`` are ``(digest, match, category)`` triples; they are written
    sorted by digest, so that the file reveals nothing about their order of
    origin and regenerations diff cleanly.
    """
    lines = ["# " + line if line else "#" for line in header]
    lines.append(FORMAT_LINE)
    lines.append("salt " + salt.hex())
    lines.append("parts-max %d" % parts_max)
    for digest, match, category in sorted(set(entries)):
        if match not in MATCHES or not _HEX64.fullmatch(digest):
            raise DenyListError("bad entry %r" % ((digest, match, category),))
        lines.append("%s %s %s" % (digest, match, category))
    return "\n".join(lines) + "\n"
