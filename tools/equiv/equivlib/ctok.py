# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""C, assembly and device-tree source to a canonical token stream.

The stream holds every token the compiler would see -- identifiers, numbers,
punctuators, string and character literals, all byte-exact -- and drops
comments and the amount of whitespace between tokens.  Two files whose
streams are equal differ only in comments and whitespace.

Three rules keep that claim honest:

* **Never split finer than the compiler.**  Removing whitespace between two
  tokens may change how the compiler splits them (``a - -b`` against
  ``a --b``).  Punctuators are therefore matched longest-first, as the C
  lexer does, and wherever this lexer cannot follow the real one exactly it
  keeps characters together rather than apart.  An error in that direction
  reports a difference that is not there; it never hides one.
* **Line structure survives where it is syntax.**  A newline ends a
  preprocessor directive, so every directive ends in an ``eol`` token.  In
  assembly a newline also ends a statement, so there every line that holds
  a token ends in one -- a line of comments or blanks holds none.
  Whether a macro is function-like depends on whether ``(`` follows its name
  without a space, so the name gets the kind ``fmacro`` or ``macro``.
* **Comments are removed the way the preprocessor removes them**: after line
  splicing, so a comment may continue a directive onto the next line, and
  never inside string or character literals.  A quote with no closing quote
  on its line starts one token that runs to the end of the line, as in the
  GCC preprocessor; nothing inside that token is a comment.

Assembly (``.S``) is preprocessed like C and then assembled.  On MIPS (and
x86) the assembler treats ``#`` as a comment character, so outside directives
the rest of such a line is a comment; a line whose first token is ``#`` but
which is not a known directive is one as well, on every architecture.  The
line ends where the preprocessor's output line ends: a C comment inside the
assembler comment is replaced by a space even when it spans lines, so the
text after it still belongs to the assembler comment.

Device-tree source is preprocessed too, then read by the device-tree compiler,
whose names may contain ``#`` (``#address-cells``); a ``#`` line is a
directive only when it names one.
"""

import bisect
import re

from .tok import (CHR, CSTR, EOL, FMACRO, HDR, ID, MACRO, MSG, NUM, OP,
                  OTHER, PP, STR, LexError, Token)

# Translation phase 2: a backslash immediately before a newline joins the two
# lines.  GCC also joins when only horizontal whitespace separates them (with
# a warning), so this does too.
_SPLICE = re.compile(r'\\[ \t\f\v]*\r?\n')

_NA = '\x80-\U0010ffff'   # non-ASCII characters stick to identifiers

_C_RE = re.compile(r'''
    (?P<ws>[ \t\f\v\r]+)
  | (?P<nl>\n)
  | (?P<bcom>/\*)
  | (?P<lcom>//)
  | (?P<squote>(?:u8|[uUL])?")
  | (?P<cquote>(?:u8|[uUL])?')
  | (?P<num>\.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.$NA])*)
  | (?P<ident>[A-Za-z_$NA][A-Za-z0-9_$NA]*)
  | (?P<punct>%:%:|\.\.\.|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\|
      |[*/%+\-&^|]=|\#\#|<:|:>|<%|%>|%:|[\s\S])
'''.replace('NA', _NA), re.X)

# Assembler names may start with and contain '.' (.section, .Lfoo), and the
# assembler-with-cpp mode of the preprocessor has no digraphs.
_ASM_RE = re.compile(r'''
    (?P<ws>[ \t\f\v\r]+)
  | (?P<nl>\n)
  | (?P<bcom>/\*)
  | (?P<lcom>//)
  | (?P<squote>")
  | (?P<cquote>')
  | (?P<num>\.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.$NA])*)
  | (?P<ident>[A-Za-z_.$NA][A-Za-z0-9_.$NA]*)
  | (?P<punct>\.\.\.|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\|
      |[*/%+\-&^|]=|\#\#|[\s\S])
'''.replace('NA', _NA), re.X)

# Device-tree names take letters, digits and ,._+*#?@- (node and property
# names, labels, numbers and macro names all fall into this one class, which
# is coarser than the real lexer and therefore safe).  /word/ is a keyword.
_DTS_RE = re.compile(r'''
    (?P<ws>[ \t\f\v\r]+)
  | (?P<nl>\n)
  | (?P<bcom>/\*)
  | (?P<lcom>//)
  | (?P<kw>/[a-z][a-z0-9-]*/)
  | (?P<squote>")
  | (?P<cquote>')
  | (?P<ident>[A-Za-z0-9_,.+*\#?@$NA-]+)
  | (?P<punct><<|>>|<=|>=|==|!=|&&|\|\||\#\#|[\s\S])
'''.replace('NA', _NA), re.X)

_STR_BODY = re.compile(r'(?:[^"\\\n]|\\[\s\S])*"')
_CHR_BODY = re.compile(r"(?:[^'\\\n]|\\[\s\S])*'")
_HSPACE = re.compile(r'[ \t\f\v\r]*')
_IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')

# The directive names the preprocessor knows.  In assembler and device-tree
# source any other word after a line-initial '#' is not a directive.
DIRECTIVES = frozenset((
    'define', 'undef', 'include', 'include_next', 'import', 'if', 'ifdef',
    'ifndef', 'elif', 'elifdef', 'elifndef', 'else', 'endif', 'error',
    'warning', 'pragma', 'line', 'ident', 'sccs', 'assert', 'unassert',
))
_INCLUDES = frozenset(('include', 'include_next', 'import'))
_MESSAGES = frozenset(('error', 'warning'))

# Strings inside these constructs are code, not data: an asm template, a
# pragma, an attribute argument such as a section or alias name.
_ASM_WORDS = frozenset(('asm', '__asm', '__asm__', 'asm_inline',
                        'asm_goto_output'))
_ASM_QUALIFIERS = frozenset(('volatile', '__volatile', '__volatile__',
                             'goto', 'inline', '__inline', '__inline__'))
_GROUP_WORDS = frozenset(('_Pragma', '__attribute__', '__attribute',
                          '__section'))

LANGS = ('c', 'asm', 'dts')


def splice(text):
    """Join backslash-continued lines.

    Returns the spliced text and the sorted positions (in the spliced text)
    at which a line break was removed, so that line numbers can still be
    given in terms of the original file.
    """
    pieces = []
    removed = []
    last = 0
    length = 0
    for m in _SPLICE.finditer(text):
        pieces.append(text[last:m.start()])
        length += m.start() - last
        removed.append(length)
        last = m.end()
    pieces.append(text[last:])
    return ''.join(pieces), removed


class _Scanner:
    def __init__(self, text, lang, hash_comments):
        if lang not in LANGS:
            raise ValueError('unknown language %r' % (lang,))
        self.s, self.removed = splice(text)
        self.lang = lang
        self.hash_comments = hash_comments and lang == 'asm'
        self.rx = {'c': _C_RE, 'asm': _ASM_RE, 'dts': _DTS_RE}[lang]
        self.newlines = [m.start() for m in re.finditer('\n', self.s)]
        self.toks = []

    def line_of(self, pos):
        return (bisect.bisect_left(self.newlines, pos)
                + bisect.bisect_right(self.removed, pos) + 1)

    # -- directives ----------------------------------------------------------

    def _skip_space_and_block_comments(self, k):
        s = self.s
        while True:
            k = _HSPACE.match(s, k).end()
            if s.startswith('/*', k):
                end = s.find('*/', k + 2)
                if end < 0:
                    raise LexError('line %d: unterminated comment'
                                   % self.line_of(k))
                k = end + 2
                continue
            return k

    def _directive(self, i):
        """Look at a line-initial '#' (or '%:') at i.

        Returns (what, name, end): what is 'directive', 'null', 'unknown'
        or 'junk'; name is the directive name ('' for a line marker); end is
        where scanning continues if the line is treated as a directive.
        """
        s = self.s
        k = i + (1 if s[i] == '#' else 2)
        k = self._skip_space_and_block_comments(k)
        if k >= len(s) or s[k] == '\n' or s.startswith('//', k):
            return 'null', '', k
        m = _IDENT.match(s, k)
        if m:
            name = m.group()
            what = 'directive' if name in DIRECTIVES else 'unknown'
            return what, name, m.end()
        if s[k].isdigit():
            return 'directive', '', k
        return 'junk', '', k

    # -- the scan ------------------------------------------------------------

    def scan(self):
        s = self.s
        n = len(s)
        rx = self.rx
        toks = self.toks
        lang = self.lang
        i = 0
        bol = True            # only whitespace and comments since the newline
        pp = None             # name of the directive being scanned
        drop = False          # in an assembler '#' comment
        want_macro = False    # next identifier is the name of a #define
        want_header = False   # next token may be a <header> or "header"
        want_dts_header = False
        msg_start = None      # first token of an #error/#warning message
        line_start = 0        # len(toks) when the current line began
        white = False         # whitespace or a comment before this token
        self.msg_white = []

        while i < n:
            c = s[i]
            if (bol and pp is None and not drop
                    and (c == '#' or (lang == 'c' and s.startswith('%:', i)))):
                what, name, k = self._directive(i)
                if lang == 'c' or what == 'directive':
                    line = self.line_of(i)
                    toks.append(Token(PP, name, line))
                    pp = name
                    bol = False
                    want_macro = name == 'define'
                    want_header = name in _INCLUDES
                    msg_start = len(toks) if name in _MESSAGES else None
                    self.msg_white = []
                    white = False
                    i = k
                    continue
                if what == 'null':
                    i = k
                    continue
                if lang == 'asm':
                    # Not a directive: the assembler reads it as a comment.
                    drop = True
                    bol = False
                    i += 1
                    continue
                # Device tree: '#address-cells' and friends are names.

            if want_header and pp is not None and c in '<"':
                close = '>' if c == '<' else '"'
                end = s.find(close, i + 1)
                eol = s.find('\n', i + 1)
                if end >= 0 and (eol < 0 or end < eol):
                    toks.append(Token(HDR, s[i:end + 1], self.line_of(i)))
                    want_header = False
                    i = end + 1
                    continue

            m = rx.match(s, i)
            kind = m.lastgroup
            j = m.end()

            if kind == 'ws':
                white = True
                i = j
                continue
            if kind == 'nl':
                if pp is not None:
                    if msg_start is not None:
                        self._collapse_message(msg_start)
                        msg_start = None
                    toks.append(Token(EOL, '', self.line_of(i)))
                    pp = None
                    want_macro = want_header = False
                elif lang == 'asm' and len(toks) > line_start:
                    toks.append(Token(EOL, '', self.line_of(i)))
                line_start = len(toks)
                bol = True
                drop = False
                white = True
                i = j
                continue
            if kind == 'bcom':
                end = s.find('*/', j)
                if end < 0:
                    raise LexError('line %d: unterminated comment'
                                   % self.line_of(i))
                white = True
                i = end + 2
                continue
            if kind == 'lcom':
                end = s.find('\n', j)
                white = True
                i = n if end < 0 else end
                continue

            bol = False
            line = self.line_of(i)
            if msg_start is not None and not drop:
                self.msg_white.append(white)
            white = False

            if kind in ('squote', 'cquote'):
                body = _STR_BODY if kind == 'squote' else _CHR_BODY
                bm = body.match(s, j)
                if bm is None:
                    # No closing quote on this line: like the preprocessor,
                    # take the rest of the line as one token (which the
                    # compiler rejects outside skipped code).
                    end = s.find('\n', j)
                    end = n if end < 0 else end
                    if not drop:
                        toks.append(Token(OTHER, s[i:end], line))
                    i = end
                    continue
                text = s[i:bm.end()]
                i = bm.end()
                if drop:
                    continue
                if kind == 'cquote':
                    toks.append(Token(CHR, text, line))
                elif want_dts_header:
                    toks.append(Token(HDR, text, line))
                elif lang == 'asm' or (pp is not None and pp != 'define'):
                    toks.append(Token(CSTR, text, line))
                else:
                    toks.append(Token(STR, text, line))
                want_dts_header = False
                want_header = False
                continue

            text = s[i:j]
            i = j
            if drop:
                continue
            if kind == 'ident':
                if want_macro:
                    tk = FMACRO if s.startswith('(', j) else MACRO
                    want_macro = False
                else:
                    tk = ID
                toks.append(Token(tk, text, line))
            elif kind == 'num':
                toks.append(Token(NUM, text, line))
            elif kind == 'kw':
                toks.append(Token(OP, text, line))
                want_dts_header = text == '/include/'
                continue
            else:
                if (lang == 'asm' and self.hash_comments and pp is None
                        and text in ('#', '##')):
                    drop = True
                    continue
                toks.append(Token(OP, text, line))
            want_header = False
            want_dts_header = False

        if pp is not None:
            if msg_start is not None:
                self._collapse_message(msg_start)
            toks.append(Token(EOL, '', self.line_of(n)))
        elif lang == 'asm' and len(toks) > line_start:
            toks.append(Token(EOL, '', self.line_of(n)))
        if lang == 'c':
            _mark_code_strings(toks)
        return toks

    def _collapse_message(self, start):
        """Replace the tokens of an #error/#warning message by one token,
        spelt as the preprocessor spells it: one space wherever whitespace
        or a comment separated two tokens, none where nothing did."""
        toks = self.toks
        if start < len(toks):
            parts = []
            for k, t in enumerate(toks[start:]):
                if k and self.msg_white[k]:
                    parts.append(' ')
                parts.append(t.text)
            line = toks[start].line
            del toks[start:]
            toks.append(Token(MSG, ''.join(parts), line))


def _mark_code_strings(toks):
    """Give strings inside asm statements and attribute-like groups the
    kind CSTR: their text is code even though it is spelt as a string."""
    n = len(toks)
    # Which tokens are inside a directive.  A group that opens in a directive
    # ends with it; one that opens in code runs through any directives in
    # between (an #ifdef inside an asm statement).
    in_dir = [False] * n
    inside = False
    for idx, t in enumerate(toks):
        if t.kind == PP:
            inside = True
        in_dir[idx] = inside
        if t.kind == EOL:
            inside = False
    i = 0
    while i < n:
        t = toks[i]
        if t.kind == ID and (t.text in _ASM_WORDS or t.text in _GROUP_WORDS):
            j = i + 1
            if t.text in _ASM_WORDS:
                while (j < n and toks[j].kind == ID
                       and toks[j].text in _ASM_QUALIFIERS):
                    j += 1
            if j < n and toks[j].kind == OP and toks[j].text == '(':
                bounded = in_dir[i]
                depth = 0
                k = j
                while k < n:
                    tk = toks[k]
                    if bounded and tk.kind == EOL:
                        break
                    if tk.kind == OP and tk.text == '(':
                        depth += 1
                    elif tk.kind == OP and tk.text == ')':
                        depth -= 1
                        if depth == 0:
                            break
                    elif tk.kind == STR:
                        toks[k] = tk._replace(kind=CSTR)
                    k += 1
                i = k
        i += 1


def tokenize(text, lang='c', hash_comments=False):
    """Return the canonical token list of *text*.

    *lang* is 'c', 'asm' or 'dts'.  *hash_comments* makes '#' outside
    directives start a comment in assembly, as it does for the MIPS and x86
    assemblers.  Raises LexError on input that cannot be tokenised.
    """
    return _Scanner(text, lang, hash_comments).scan()


# -- string values --------------------------------------------------------------

_ESCAPES = {
    'a': 7, 'b': 8, 'f': 12, 'n': 10, 'r': 13, 't': 9, 'v': 11, 'e': 27,
    'E': 27, '\\': 92, "'": 39, '"': 34, '?': 63,
}
_PREFIX = re.compile(r'(u8|[uUL])?"')


def string_value(literals):
    """The value of adjacent string literals after concatenation.

    Returns (prefixes, code units) or None if a literal cannot be decoded.
    Used only to say whether a changed run of literals kept its value.
    """
    prefixes = set()
    units = []
    for lit in literals:
        m = _PREFIX.match(lit)
        if m is None or not lit.endswith('"'):
            return None
        prefixes.add(m.group(1) or '')
        body = lit[m.end():-1]
        k = 0
        while k < len(body):
            ch = body[k]
            if ch != '\\':
                units.extend(ch.encode('utf-8', 'surrogateescape'))
                k += 1
                continue
            k += 1
            if k >= len(body):
                return None
            e = body[k]
            if e in '01234567':
                m2 = re.match(r'[0-7]{1,3}', body[k:])
                units.append(int(m2.group(), 8))
                k += m2.end()
            elif e == 'x':
                m2 = re.match(r'[0-9a-fA-F]+', body[k + 1:])
                if m2 is None:
                    return None
                units.append(int(m2.group(), 16))
                k += 1 + m2.end()
            elif e in 'uU':
                width = 4 if e == 'u' else 8
                digits = body[k + 1:k + 1 + width]
                if not re.fullmatch(r'[0-9a-fA-F]{%d}' % width, digits):
                    return None
                units.extend(chr(int(digits, 16)).encode('utf-8',
                                                         'surrogatepass'))
                k += 1 + width
            else:
                units.append(_ESCAPES.get(e, ord(e)))
                k += 1
    prefixes.discard('')
    return (tuple(sorted(prefixes)), tuple(units))
