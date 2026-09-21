# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tokeniser and hashing for the deny-list gate.

The same functions produce the deny-list (on the laboratory side) and check
against it (here), so the normalisation below is part of the deny-list
format: changing it means regenerating the list.

A token is one of:

* an identifier -- ``[A-Za-z_][A-Za-z0-9_]*`` -- which covers C identifiers,
  shell and make variables, file-name stems and words in prose;
* a hexadecimal literal such as ``0x1234abcdu``, without its C suffix;
* a bare run of exactly eight hexadecimal digits that starts with ``8``,
  which is how a 32-bit kernel-segment address is often written without its
  ``0x``.

Identifiers are compared in lower case. Hexadecimal tokens are normalised to
``0x`` followed by the lower-case digits.
"""

from __future__ import annotations

import hashlib
import re
from typing import Iterator, NamedTuple

_TOKEN_RE = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"(?:"
    r"(?P<hex>0[xX][0-9A-Fa-f]+)(?:[uU][lL]{0,2}|[lL]{1,2}[uU]?)?"
    r"|(?P<bare>8[0-9A-Fa-f]{7})"
    r"|(?P<ident>[A-Za-z_][A-Za-z0-9_]*)"
    r")"
    r"(?![A-Za-z0-9_])"
)

# Identifiers split into components at underscores. A "part" entry of the
# deny-list matches any run of whole components; runs longer than this many
# components are not formed (the list header may lower it).
DEFAULT_MAX_PART_COMPONENTS = 8


class Token(NamedTuple):
    """One token: its span in the text, its spelling and its normal form."""

    start: int
    end: int
    text: str
    norm: str


def normalize_ident(text: str) -> str:
    """Return the normal form of an identifier."""
    return text.lower()


def normalize_hex(digits: str) -> str:
    """Return the normal form of a hexadecimal number given as its digits."""
    return "0x" + digits.lower()


def tokenize(text: str) -> Iterator[Token]:
    """Yield every token of ``text`` with its span and normal form."""
    for m in _TOKEN_RE.finditer(text):
        if m.group("ident") is not None:
            norm = normalize_ident(m.group("ident"))
            yield Token(m.start("ident"), m.end("ident"), m.group("ident"), norm)
        elif m.group("hex") is not None:
            norm = normalize_hex(m.group("hex")[2:])
            yield Token(m.start(), m.end(), m.group(0), norm)
        else:
            norm = normalize_hex(m.group("bare"))
            yield Token(m.start(), m.end(), m.group(0), norm)


def components(norm: str) -> list[str]:
    """Split a normalised identifier into its non-empty components."""
    return [part for part in norm.split("_") if part]


def component_runs(norm: str, max_len: int = DEFAULT_MAX_PART_COMPONENTS) -> Iterator[str]:
    """Yield every run of whole components of an identifier, joined by ``_``.

    ``chip_block_reg_bit`` yields ``chip``, ``chip_block``, ...,
    ``block_reg``, ``reg_bit`` and so on, up to ``max_len`` components per run.
    Hexadecimal tokens have no components and yield nothing.
    """
    if norm.startswith("0x"):
        return
    parts = components(norm)
    count = len(parts)
    for first in range(count):
        for last in range(first + 1, min(count, first + max_len) + 1):
            yield "_".join(parts[first:last])


class Hasher:
    """Salted SHA-256 over the UTF-8 encoding of a normalised token."""

    def __init__(self, salt: bytes) -> None:
        self._base = hashlib.sha256(salt)

    def __call__(self, norm: str) -> str:
        digest = self._base.copy()
        digest.update(norm.encode("utf-8"))
        return digest.hexdigest()
