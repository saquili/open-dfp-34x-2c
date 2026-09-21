# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Kconfig files and configuration fragments.

Kconfig
-------
A Kconfig file is read line by line, as the kconfig lexer reads it.  Dropped:
``#`` comments, indentation and the amount of whitespace, and **help text**
together with its ``help`` keyword -- help is prose, like a comment.  Kept:
every keyword, symbol, operator and string, and the end of every line (a line
is a statement).

A help block ends where the kconfig lexer ends it: at the first non-blank line
indented less than the first line of the help text (a tab counts to the next
multiple of eight columns), or at a line that starts in column zero.

The first string on a line that starts with ``bool``, ``tristate``, ``int``,
``hex``, ``string``, ``prompt``, ``menu``, ``comment`` or ``mainmenu`` is a
prompt: a literal.  Every other string -- a ``default`` value, a ``source``
path, an operand in an expression -- is code.  A macro call ``$(...)`` is kept
as one token, whitespace included.

Configuration fragments
-----------------------
In a ``.config``-style fragment, ``# CONFIG_FOO is not set`` is not a comment:
it switches an option off.  Such lines and ``CONFIG_FOO=value`` lines are
code; other ``#`` lines are comments.
"""

import re

from .tok import CSTR, EOL, LINE, OP, STR, WORD, LexError, Token

_TOKEN = re.compile(r'''
    (?P<ws>[ \t\f\v\r]+)
  | (?P<comment>\#.*)
  | (?P<str>"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')
  | (?P<macro>\$\()
  | (?P<word>[A-Za-z0-9_-]+)
  | (?P<op>&&|\|\||!=|<=|>=|:=|\+=|[\s\S])
''', re.X)

_PROMPT_WORDS = frozenset(('bool', 'tristate', 'int', 'hex', 'string',
                           'prompt', 'menu', 'comment', 'mainmenu'))
_HELP_WORDS = frozenset(('help', '---help---'))


def _indent_width(line):
    width = 0
    for ch in line:
        if ch == '\t':
            width = (width & ~7) + 8
        elif ch == ' ':
            width += 1
        else:
            break
    return width


def _macro_end(s, i):
    """End of the $( ... ) starting at i, parentheses balanced."""
    depth = 0
    k = i
    while k < len(s):
        ch = s[k]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                return k + 1
        k += 1
    return -1


def _line_tokens(text, lineno):
    """Tokens of one logical Kconfig line (comments dropped)."""
    toks = []
    i = 0
    first_word = None
    seen_string = False
    while i < len(text):
        m = _TOKEN.match(text, i)
        kind = m.lastgroup
        j = m.end()
        if kind == 'ws':
            i = j
            continue
        if kind == 'comment':
            break
        if kind == 'macro':
            end = _macro_end(text, i + 1)
            if end < 0:
                raise LexError('line %d: unterminated $(' % lineno)
            toks.append(Token(WORD, text[i:end], lineno))
            i = end
            continue
        if kind == 'str':
            literal = first_word in _PROMPT_WORDS and not seen_string
            toks.append(Token(STR if literal else CSTR, text[i:j], lineno))
            seen_string = True
        elif kind == 'word':
            toks.append(Token(WORD, text[i:j], lineno))
        else:
            toks.append(Token(OP, text[i:j], lineno))
        if first_word is None:
            first_word = text[i:j] if kind == 'word' else ''
        i = j
    return toks


def tokenize(text):
    """Return the canonical token list of a Kconfig file."""
    lines = text.split('\n')
    toks = []
    i = 0
    n = len(lines)
    while i < n:
        lineno = i + 1
        line = lines[i]
        # Join continuation lines.
        while line.endswith('\\') and i + 1 < n:
            i += 1
            line = line[:-1] + ' ' + lines[i]
        i += 1
        line_toks = _line_tokens(line, lineno)
        if not line_toks:
            continue
        if (len(line_toks) == 1 and line_toks[0].kind == WORD
                and line_toks[0].text in _HELP_WORDS):
            i = _skip_help(lines, i)
            continue
        toks.extend(line_toks)
        toks.append(Token(EOL, '', lineno))
    return toks


def _skip_help(lines, i):
    """Skip the help text that starts at lines[i]; return the next line."""
    n = len(lines)
    first = None
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        width = _indent_width(line)
        if width == 0:
            return i
        if first is None:
            first = width
        elif width < first:
            return i
        i += 1
    return i


def strip(text):
    """Kconfig text with comments and help removed, one statement per line,
    single spaces between tokens."""
    out = []
    cur = []
    for t in tokenize(text):
        if t.kind == EOL:
            out.append(' '.join(cur))
            cur = []
        else:
            cur.append(t.text)
    if cur:
        out.append(' '.join(cur))
    return '\n'.join(out) + ('\n' if out else '')


# -- configuration fragments --------------------------------------------------------

_SET = re.compile(r'CONFIG_[A-Za-z0-9_]+=')
_UNSET = re.compile(r'#\s*(CONFIG_[A-Za-z0-9_]+)\s+is not set\s*$')


def fragment_tokens(text):
    """Return the canonical token list of a configuration fragment."""
    toks = []
    for lineno, raw in enumerate(text.split('\n'), 1):
        line = raw.strip()
        if not line:
            continue
        m = _UNSET.match(line)
        if m:
            toks.append(Token(LINE, '# %s is not set' % m.group(1), lineno))
        elif line.startswith('#'):
            continue
        else:
            toks.append(Token(LINE, line, lineno))
    return toks
