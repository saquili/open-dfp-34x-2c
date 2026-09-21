# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Makefiles (Makefile, Kbuild, Platform, *.mk) as token streams.

GNU make is line oriented and much of its whitespace is syntax, so the rules
here are narrower than for C:

* Every non-empty logical line becomes its words followed by an ``eol``.
  A word boundary is kept; the amount of whitespace at a boundary, and the
  indentation of a line that does not start with a tab, are not.
* A line that ends in a backslash continues on the next one; make turns the
  break and the whitespace around it into one space, so re-wrapping such a
  line changes nothing.
* A comment starts at a ``#`` that is not escaped as ``\\#`` and is not inside
  a ``$(...)`` or ``${...}`` reference (GNU make 4.3 and later keep those).
* A line that starts with a tab may be a recipe, which make hands to the shell.
  It keeps a ``recipe`` marker, its quoted text exactly, and loses a ``#``
  comment only where the shell would see one -- at the start of a word,
  outside quotes and references -- and only if the comment holds no ``$``,
  since make would expand a reference even there.
* The body of a ``define`` is kept line by line, verbatim apart from trailing
  whitespace: it is expanded later, in a context this reader cannot know.

Not detected: make keeps the exact whitespace inside function arguments
(``$(subst a, b,...)``) and at the end of a value (the blanks before a
trailing comment belong to the value).  Changing only the amount of such
whitespace is reported as no change.
"""

from .tok import EOL, LINE, WORD, Token

_RECIPE = 'recipe'


def _odd_backslashes_before(s, pos):
    count = 0
    k = pos - 1
    while k >= 0 and s[k] == '\\':
        count += 1
        k -= 1
    return count % 2 == 1


def _strip_make_comment(line):
    """Remove a make comment from a (non-recipe) logical line."""
    depth = 0
    k = 0
    while k < len(line):
        ch = line[k]
        if ch == '$' and k + 1 < len(line) and line[k + 1] in '({':
            depth += 1
            k += 2
            continue
        if depth and ch in ')}':
            depth -= 1
        elif ch == '#' and depth == 0 and not _odd_backslashes_before(line, k):
            return line[:k]
        k += 1
    return line


def _recipe_words(text):
    """Split a recipe line into words the way the shell would, keeping quoted
    text exactly and dropping a shell comment where that is safe."""
    words = []
    cur = []
    k = 0
    quote = None
    depth = 0
    n = len(text)
    while k < n:
        ch = text[k]
        if quote:
            cur.append(ch)
            if ch == '\\' and quote == '"' and k + 1 < n:
                cur.append(text[k + 1])
                k += 2
                continue
            if ch == quote:
                quote = None
            k += 1
            continue
        if ch == '$' and k + 1 < n and text[k + 1] in '({':
            depth += 1
            cur.append(text[k:k + 2])
            k += 2
            continue
        if depth and ch in ')}':
            depth -= 1
            cur.append(ch)
            k += 1
            continue
        if ch == '\\' and k + 1 < n:
            if text[k + 1] == '\n':
                # A continued recipe line: a word boundary for the shell.
                if cur:
                    words.append(''.join(cur))
                    cur = []
                k += 2
                continue
            cur.append(text[k:k + 2])
            k += 2
            continue
        if ch in ' \t\n':
            if cur:
                words.append(''.join(cur))
                cur = []
            k += 1
            continue
        if ch == '#' and not cur and depth == 0:
            comment = text[k:]
            if '$' not in comment:
                break
        if ch in '\'"':
            quote = ch
        cur.append(ch)
        k += 1
    if cur:
        words.append(''.join(cur))
    return words


def tokenize(text):
    """Return the canonical token list of a makefile."""
    lines = text.split('\n')
    toks = []
    n = len(lines)
    i = 0
    define_depth = 0
    while i < n:
        lineno = i + 1
        line = lines[i]

        if define_depth:
            first = line.split()[:1]
            word = first[0] if first else ''
            if word == 'endef':
                define_depth -= 1
            elif word == 'define' or _is_define(line):
                define_depth += 1
            if define_depth:
                i += 1
                body = line.rstrip()
                if body:
                    toks.append(Token(LINE, body, lineno))
                continue
            # The endef line itself is ordinary syntax.

        if line.startswith('\t'):
            # A recipe line and its continuations, passed on together.
            text = line[1:]
            while _odd_backslashes_before(text, len(text)) and i + 1 < n:
                i += 1
                nxt = lines[i]
                text = text + '\n' + (nxt[1:] if nxt.startswith('\t') else nxt)
            i += 1
            words = _recipe_words(text)
            if words:
                toks.append(Token(_RECIPE, '', lineno))
                toks.extend(Token(WORD, w, lineno) for w in words)
                toks.append(Token(EOL, '', lineno))
            continue

        # Ordinary makefile syntax: join continuations into one space.
        while _odd_backslashes_before(line, len(line)) and i + 1 < n:
            i += 1
            line = line[:-1] + ' ' + lines[i]
        i += 1
        line = _strip_make_comment(line)
        words = line.split()
        if not words:
            continue
        if _is_define(line):
            define_depth += 1
        toks.extend(Token(WORD, w, lineno) for w in words)
        toks.append(Token(EOL, '', lineno))
    return toks


_DEFINE_PREFIXES = ('override', 'export', 'private', 'unexport')


def _is_define(line):
    words = line.split()
    while words and words[0] in _DEFINE_PREFIXES:
        words = words[1:]
    return bool(words) and words[0] == 'define'
