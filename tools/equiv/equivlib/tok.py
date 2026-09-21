# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The token type shared by every language front end.

A token is a (kind, text) pair plus the line it starts on.  Two streams are
compared on (kind, text) only; the line is there for reports.

Kinds fall into two groups.  **Literal kinds** hold text whose change alters
what a program prints or stores but not how it is built: string literals in
ordinary C code, device-tree string values, Kconfig prompts and the message of
an ``#error`` or ``#warning``.  Every other kind is **code**: a change to it is
a code change.  A string literal can be code too -- an ``asm`` template, a
section name, an included file name -- and then it gets a code kind
(``cstr`` or ``hdr``) instead of ``str``.
"""

import collections

ID = 'id'          # identifier or keyword
NUM = 'num'        # preprocessing number
OP = 'op'          # punctuator, or any other single character
STR = 'str'        # string literal whose text is data (literal kind)
CSTR = 'cstr'      # string literal whose text is code: asm, attributes, paths
CHR = 'chr'        # character constant (an integer: code)
OTHER = 'other'    # an unterminated quote and the rest of its line
HDR = 'hdr'        # name of an included file
PP = 'pp'          # start of a preprocessor directive; text = directive name
EOL = 'eol'        # end of a directive, or of a line where lines are syntax
MACRO = 'macro'    # name of an object-like macro being defined
FMACRO = 'fmacro'  # name of a function-like macro being defined
MSG = 'msg'        # text of an #error or #warning directive (literal kind)
WORD = 'word'      # a word of a line-oriented language (Makefile, Kconfig)
LINE = 'line'      # a whole normalised line (YAML, config fragments, bodies)

LITERAL_KINDS = frozenset((STR, MSG))


class Token(collections.namedtuple('Token', 'kind text line')):
    """One token: its kind, its exact text, and the 1-based line it starts on."""

    __slots__ = ()

    @property
    def key(self):
        """The part of the token that takes part in comparisons."""
        return (self.kind, self.text)

    def is_literal(self):
        return self.kind in LITERAL_KINDS


class LexError(Exception):
    """The input cannot be tokenised (for example an unterminated comment)."""


def keys(tokens):
    """Return the comparison keys of a token list."""
    return [t.key for t in tokens]
