# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Split a file into code, comment, string and prose segments.

The gates scan whole files; the segments only say *where* a finding sits, so
that a report can tell a comment from a string literal from code. The lexers
are deliberately small: they know the comment and string syntax of the file
types this repository holds and nothing else.
"""

from __future__ import annotations

import bisect
import os
import re

CODE = "code"
COMMENT = "comment"
STRING = "string"
TEXT = "text"

_C_SUFFIXES = {".c", ".h", ".S", ".s", ".dts", ".dtsi", ".lds", ".cc", ".cpp", ".hpp", ".y", ".l"}
_HASH_SUFFIXES = {".sh", ".py", ".mk", ".pl", ".awk", ".sed", ".yaml", ".yml", ".toml", ".cfg",
                  ".conf", ".ini", ".cmake", ".config", ".tcl", ".bash", ".mak"}
_HASH_NAMES = {"Makefile", "Kbuild", "Platform", "Dockerfile", "GNUmakefile", "makefile",
               "Containerfile", "check"}

_C_RE = re.compile(
    r"/\*.*?(?:\*/|\Z)"          # block comment (unterminated runs to the end)
    r"|//[^\n]*"                  # line comment
    r'|"(?:\\.|[^"\\\n])*"?'      # string literal
    r"|'(?:\\.|[^'\\\n])*'?",     # character literal
    re.S,
)
_HASH_RE = re.compile(
    r'"""[\s\S]*?(?:"""|\Z)'
    r"|'''[\s\S]*?(?:'''|\Z)"
    r"|#[^\n]*"
    r'|"(?:\\.|[^"\\\n])*"?'
    r"|'(?:\\.|[^'\\\n])*'?",
)
_KCONFIG_HELP = re.compile(r"^([ \t]*)(?:help|---help---)[ \t]*$")


def language(path: str) -> str:
    """Return the lexer family for a path: ``c``, ``hash``, ``kconfig`` or ``text``."""
    name = os.path.basename(path)
    suffix = os.path.splitext(name)[1]
    if suffix in _C_SUFFIXES:
        return "c"
    if name.startswith("Kconfig"):
        return "kconfig"
    if suffix in _HASH_SUFFIXES or name in _HASH_NAMES or name.startswith("Makefile"):
        return "hash"
    return "text"


def _from_regex(text: str, regex: re.Pattern) -> list[tuple[int, int, str]]:
    segments = []
    pos = 0
    for m in regex.finditer(text):
        if m.start() > pos:
            segments.append((pos, m.start(), CODE))
        lead = m.group(0)[:1]
        kind = COMMENT if lead in "/#" else STRING
        segments.append((m.start(), m.end(), kind))
        pos = m.end()
    if pos < len(text):
        segments.append((pos, len(text), CODE))
    return segments


def _kconfig(text: str) -> list[tuple[int, int, str]]:
    """Kconfig: '#' comments, quoted prompts, and help text as prose."""
    segments = []
    offset = 0
    help_indent = None
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\n").expandtabs()
        width = len(body) - len(body.lstrip())
        if help_indent is not None:
            if not body.strip() or width > help_indent:
                segments.append((offset, offset + len(line), COMMENT))
                offset += len(line)
                continue
            help_indent = None
        m = _KCONFIG_HELP.match(body)
        if m:
            help_indent = len(m.group(1))
            segments.append((offset, offset + len(line), CODE))
        else:
            for start, end, kind in _from_regex(line, _HASH_RE):
                segments.append((offset + start, offset + end, kind))
        offset += len(line)
    return segments


def segments(path: str, text: str) -> list[tuple[int, int, str]]:
    """Return ``(start, end, kind)`` spans covering ``text`` in order."""
    lang = language(path)
    if lang == "c":
        return _from_regex(text, _C_RE)
    if lang == "hash":
        return _from_regex(text, _HASH_RE)
    if lang == "kconfig":
        return _kconfig(text)
    return [(0, len(text), TEXT)] if text else []


class SegmentMap:
    """Answer "which kind of segment holds this offset" quickly."""

    def __init__(self, path: str, text: str) -> None:
        self._spans = segments(path, text)
        self._starts = [start for start, _end, _kind in self._spans]

    def kind_at(self, offset: int) -> str:
        index = bisect.bisect_right(self._starts, offset) - 1
        if index < 0:
            return CODE
        return self._spans[index][2]
