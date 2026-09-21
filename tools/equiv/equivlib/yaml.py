# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Device-tree binding schemas (YAML) without their prose.

This is a line-oriented reader for the YAML that binding schemas use, not a
YAML parser; the standard library has none.  It drops:

* comments -- a ``#`` at the start of a line or after a blank, outside quotes
  and outside block scalars;
* every ``description:`` and ``title:`` entry with its whole value: an inline
  value, a multi-line plain or quoted value, or a ``|``/``>`` block.  If
  the entry opens items of a sequence (``- description: ...``, as in the
  ``items`` lists of bindings), the ``-`` markers stay: how many items a
  list has is schema, not prose;
* blank lines and trailing whitespace (a blank line inside a kept block
  scalar is kept when more content follows it, or when the block keeps its
  trailing lines with ``+``).

Everything else is kept exactly, indentation included, because indentation is
YAML syntax.  The text of other block scalars (``examples: |``) is kept
verbatim, comments and all.  A change the reader cannot place is therefore
reported as a difference, never hidden.
"""

import re

from .tok import LINE, Token

_PROSE_KEY = re.compile(r'''(?:description|title|"description"|"title"|'description'|'title')\s*:(?:\s|$)''')
_BLOCK_INDICATOR = re.compile(r'(?:^|:|-)\s*([|>])([-+0-9]*)\s*$')


def _indent(line):
    return len(line) - len(line.lstrip(' '))


def _after_markers(line):
    """(column, rest) past the indentation and any '- ' sequence markers,
    plus the column of the last marker (or the indentation if none)."""
    col = _indent(line)
    last_dash = col
    rest = line[col:]
    while rest.startswith('-') and (len(rest) == 1 or rest[1] in ' \t'):
        last_dash = col
        stripped = rest[1:].lstrip(' \t')
        col += len(rest) - len(stripped)
        rest = stripped
    return col, rest, last_dash


def _strip_comment(line):
    """Remove a trailing comment, respecting quoted scalars."""
    quote = None
    k = 0
    n = len(line)
    while k < n:
        ch = line[k]
        if quote == "'":
            if ch == "'":
                if k + 1 < n and line[k + 1] == "'":
                    k += 2
                    continue
                quote = None
        elif quote == '"':
            if ch == '\\':
                k += 2
                continue
            if ch == '"':
                quote = None
        elif ch in '\'"':
            # A quote opens a quoted scalar only where a value may start.
            before = line[:k].rstrip()
            if not before or before[-1] in ':-[{,?':
                quote = ch
        elif ch == '#' and (k == 0 or line[k - 1] in ' \t'):
            return line[:k]
        k += 1
    return line


def strip_lines(text):
    """Return [(lineno, text)] of the schema without comments and prose."""
    out = []
    skip_col = None       # dropping the value of a prose key at this column
    block_parent = None   # inside a kept block scalar deeper than this column
    block_keep = False    # that block keeps its trailing blank lines
    blanks = []           # blank lines seen inside the block, not yet placed
    for lineno, raw in enumerate(text.split('\n'), 1):
        line = raw.rstrip()
        if skip_col is not None:
            if not line.strip() or _indent(line) > skip_col:
                continue
            skip_col = None
        if block_parent is not None:
            if not line.strip():
                blanks.append(lineno)
                continue
            if _indent(line) > block_parent:
                out.extend((b, '') for b in blanks)
                blanks = []
                out.append((lineno, line))
                continue
            if block_keep:
                out.extend((b, '') for b in blanks)
            blanks = []
            block_parent = None
        code = _strip_comment(line).rstrip()
        if not code.strip():
            continue
        col, rest, last_dash = _after_markers(code)
        if _PROSE_KEY.match(rest):
            skip_col = col
            if col > _indent(code):
                out.append((lineno, code[:col].rstrip()))
            continue
        m = _BLOCK_INDICATOR.search(code)
        if m:
            # The block belongs to the key on this line, or to the sequence
            # entry when the line is just '- |'.
            block_parent = last_dash if rest[:1] in ('|', '>') else col
            block_keep = '+' in m.group(2)
        out.append((lineno, code))
    if block_parent is not None and block_keep:
        out.extend((b, '') for b in blanks)
    return out


def tokenize(text):
    """Return the canonical token list of a YAML schema."""
    return [Token(LINE, line, lineno) for lineno, line in strip_lines(text)]


def strip(text):
    """The schema text without comments and prose, for reading or diffing."""
    return ''.join(line + '\n' for _, line in strip_lines(text))
