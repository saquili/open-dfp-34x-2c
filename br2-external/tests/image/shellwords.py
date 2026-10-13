# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""shellwords: the commands a POSIX shell script runs, by reading it as a shell does.

``bootscript.commands`` reads one script of the image by a rule of thumb. The
image has a dozen, with here-documents, functions, ``case`` statements and
command substitutions inside quotes, so this module reads them as a shell does,
as far as the question needs: which words stand in command position, and with
which words after them.

What it understands: comments; single and double quotes, escapes and line
continuations; ``$(...)``, ``$((...))``, backticks and ``${...}``, in a word and
inside double quotes, the substitutions read again as scripts of their own;
here-documents (``<<``, ``<<-``, a quoted or unquoted delimiter), whose bodies
are not commands but whose substitutions are; redirections and the word that
follows each; the operators ``; & && || |`` and parentheses; the reserved words
(``if then else elif fi while until do done for in case esac { } !``) and
``case`` patterns; functions (``name() {`` and ``function name``); assignments in
front of a command; and the commands that take a command for an argument
(``exec``, ``command``, ``env``, ``nohup``, ``xargs``, ``timeout``, ``time``, ``nice``,
``setsid``).

What it does not: expansion. A command word that holds ``$`` or a backtick is
reported as dynamic, with its text, and left to whoever reads the report.
Options are collected as they stand, the words after the command that begin
with a dash, until the first that does not.
"""

import re
from collections import namedtuple

Command = namedtuple("Command", "name args line dynamic")

START_WORDS = {"if", "then", "else", "elif", "while", "until", "do", "!", "{"}
END_WORDS = {"fi", "done", "esac", "}"}
RESERVED = START_WORDS | END_WORDS | {"for", "in", "case", "function", "select"}
# Commands whose first word that is not an option is a command too.
WRAPPERS = {"exec", "command", "env", "nohup", "xargs", "timeout", "time", "nice", "setsid"}
ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\[[^\]]*\])?\+?=")
OPERATORS = (";;", "&&", "||", ";", "&", "|", "(", ")")
REDIRECT = re.compile(r"([0-9]*)(<<-|<<|<>|<&|>&|>>|>\||<|>)")


class Word:
    """A word as written, with the scripts of the substitutions inside it."""

    def __init__(self, text, subs, line):
        self.text = text
        self.subs = subs
        self.line = line

    @property
    def dynamic(self):
        return "$" in self.text.replace("\\$", "") or "`" in self.text

    @property
    def plain(self):
        """The word without its quotes, where it has no expansion."""
        return re.sub(r"""['"\\]""", "", self.text)


def matching(text, pos, open_char, close_char):
    """The index of the character that closes the group opened before pos."""
    depth = 1
    n = len(text)
    while pos < n:
        c = text[pos]
        if c == "\\":
            pos += 2
            continue
        if c == "'":
            pos = text.find("'", pos + 1)
            if pos < 0:
                return n
        elif c == '"':
            pos = skip_double(text, pos + 1)
            continue
        elif c == "#" and (pos == 0 or text[pos - 1] in " \t\n;&|("):
            pos = text.find("\n", pos)
            if pos < 0:
                return n
        elif c == open_char:
            depth += 1
        elif c == close_char:
            depth -= 1
            if depth == 0:
                return pos
        pos += 1
    return n


def skip_double(text, pos):
    """The index after the double quote that closes the one before pos."""
    n = len(text)
    while pos < n:
        c = text[pos]
        if c == "\\":
            pos += 2
            continue
        if c == '"':
            return pos + 1
        if c == "$" and text[pos + 1:pos + 3] == "((":
            pos = matching(text, pos + 3, "(", ")") + 2
            continue
        if c == "$" and text[pos + 1:pos + 2] == "(":
            pos = matching(text, pos + 2, "(", ")") + 1
            continue
        if c == "$" and text[pos + 1:pos + 2] == "{":
            pos = matching(text, pos + 2, "{", "}") + 1
            continue
        if c == "`":
            pos = text.find("`", pos + 1) + 1 or n
            continue
        pos += 1
    return n


def scan_word(text, pos, line):
    """The word that starts at pos: (Word, the index after it, the lines it spans)."""
    start = pos
    n = len(text)
    subs = []
    while pos < n:
        c = text[pos]
        if c in " \t\n;&|()<>":
            break
        if c == "\\":
            pos += 2
            continue
        if c == "'":
            end = text.find("'", pos + 1)
            pos = n if end < 0 else end + 1
            continue
        if c == '"':
            end = skip_double(text, pos + 1)
            inner = text[pos + 1:end - 1]
            subs.extend(substitutions(inner))
            pos = end
            continue
        if c == "$" and text[pos + 1:pos + 3] == "((":
            pos = matching(text, pos + 3, "(", ")") + 2
            continue
        if c == "$" and text[pos + 1:pos + 2] == "(":
            end = matching(text, pos + 2, "(", ")")
            subs.append(text[pos + 2:end])
            pos = end + 1
            continue
        if c == "$" and text[pos + 1:pos + 2] == "{":
            pos = matching(text, pos + 2, "{", "}") + 1
            continue
        if c == "`":
            end = text.find("`", pos + 1)
            end = n if end < 0 else end
            subs.append(text[pos + 1:end])
            pos = end + 1
            continue
        pos += 1
    word = text[start:pos]
    return Word(word, subs, line + text[:start].count("\n")), pos


def substitutions(text):
    """The scripts of the command substitutions in the text of a double-quoted string."""
    found = []
    pos = 0
    n = len(text)
    while pos < n:
        c = text[pos]
        if c == "\\":
            pos += 2
            continue
        if c == "$" and text[pos + 1:pos + 3] == "((":
            pos = matching(text, pos + 3, "(", ")") + 2
            continue
        if c == "$" and text[pos + 1:pos + 2] == "(":
            end = matching(text, pos + 2, "(", ")")
            found.append(text[pos + 2:end])
            pos = end + 1
            continue
        if c == "$" and text[pos + 1:pos + 2] == "{":
            pos = matching(text, pos + 2, "{", "}") + 1
            continue
        if c == "`":
            end = text.find("`", pos + 1)
            end = n if end < 0 else end
            found.append(text[pos + 1:end])
            pos = end + 1
            continue
        pos += 1
    return found


def tokens(text, first_line=1):
    """The tokens of a script: ("word", Word), ("op", text) and ("redir", text)."""
    out = []
    pos = 0
    n = len(text)
    pending = []  # here-documents whose bodies follow the next newline
    while pos < n:
        c = text[pos]
        if c == "\n":
            out.append(("op", "\n", first_line + text[:pos].count("\n")))
            pos += 1
            for delimiter, strip, quoted in pending:
                body_start = pos
                while pos < n:
                    end = text.find("\n", pos)
                    end = n if end < 0 else end
                    row = text[pos:end]
                    if (row.lstrip("\t") if strip else row) == delimiter:
                        break
                    pos = end + 1
                body = text[body_start:pos]
                if not quoted:
                    for sub in substitutions(body):
                        out.append(("sub", sub, first_line + text[:body_start].count("\n")))
                pos = min(n, text.find("\n", pos) + 1 if text.find("\n", pos) >= 0 else n)
            pending = []
            continue
        if c in " \t":
            pos += 1
            continue
        if c == "\\" and text[pos + 1:pos + 2] == "\n":
            pos += 2
            continue
        if c == "#":
            end = text.find("\n", pos)
            pos = n if end < 0 else end
            continue
        # A number glued to a redirection is its descriptor.
        match = REDIRECT.match(text, pos) if c in "<>0123456789" else None
        if match is not None:
            operator = match.group(2)
            pos = match.end()
            out.append(("redir", operator, first_line + text[:pos].count("\n")))
            if operator in ("<<", "<<-"):
                while pos < n and text[pos] in " \t":
                    pos += 1
                word, pos = scan_word(text, pos, first_line)
                delimiter = re.sub(r"""['"\\]""", "", word.text)
                quoted = any(q in word.text for q in "'\"\\")
                pending.append((delimiter, operator == "<<-", quoted))
                out.append(("word", Word("", [], word.line), word.line))
            continue
        two = text[pos:pos + 2]
        if two in OPERATORS:
            out.append(("op", two, first_line + text[:pos].count("\n")))
            pos += 2
            continue
        if c in ";&|()":
            out.append(("op", c, first_line + text[:pos].count("\n")))
            pos += 1
            continue
        word, pos = scan_word(text, pos, first_line)
        if word.text == "" and pos < n and text[pos] not in " \t\n;&|()<>":
            pos += 1
            continue
        out.append(("word", word, word.line))
    return out


def commands(text, first_line=1):
    """The commands of a script: (list of Command, set of the functions it defines)."""
    found = []
    functions = set()
    toks = tokens(text, first_line)
    i = 0
    cmd_pos = True
    case_state = None   # None, "subject", "in", "pattern", "body"
    case_depth = []
    skip_list = False   # the words of "for x in WORDS" up to its terminator
    current = None
    wrapper = None      # a wrapper whose own command word is still to come

    def add_subs(word):
        for sub in word.subs:
            inner, inner_fns = commands(sub, word.line)
            found.extend(inner)
            functions.update(inner_fns)

    def finish(command):
        found.append(command)

    while i < len(toks):
        kind, value, line = toks[i]
        i += 1
        if kind == "sub":
            inner, inner_fns = commands(value, line)
            found.extend(inner)
            functions.update(inner_fns)
            continue
        if kind == "redir":
            # The word after it is a file or a descriptor.
            if i < len(toks) and toks[i][0] == "word":
                add_subs(toks[i][1])
                i += 1
            continue
        if kind == "op":
            if skip_list and value in (";", "\n"):
                skip_list = False
                cmd_pos = True
                continue
            if case_state == "pattern":
                if value == ")":
                    case_state = "body"
                    cmd_pos = True
                continue
            if value == ";;":
                if case_depth:
                    case_state = "pattern"
                cmd_pos = False
                continue
            cmd_pos = True
            current = None
            wrapper = None
            continue
        word = value
        add_subs(word)
        if skip_list:
            continue
        if case_state == "subject":
            case_state = "in"
            continue
        if case_state == "in":
            case_state = "pattern" if word.text == "in" else None
            continue
        if case_state == "pattern":
            if word.text == "esac":
                case_state = case_depth.pop() if case_depth else None
                cmd_pos = False
            continue
        if not cmd_pos:
            if current is not None:
                if wrapper is not None and not word.text.startswith("-") and not ASSIGNMENT.match(word.text) \
                        and not re.fullmatch(r"[0-9.]+[smhd]?", word.text):
                    # The command of a wrapper.
                    finish(Command(word.plain if not word.dynamic else word.text, [], word.line, word.dynamic))
                    current = found[-1]
                    wrapper = current.name if current.name in WRAPPERS else None
                else:
                    current.args.append(word.text)
            continue
        text = word.text
        if text in RESERVED:
            if text == "for" or text == "select":
                skip_list = False
                # "for NAME in WORDS ;": the name, then the words up to the terminator.
                if i < len(toks) and toks[i][0] == "word":
                    i += 1
                if i < len(toks) and toks[i][0] == "word" and toks[i][1].text == "in":
                    i += 1
                    skip_list = True
                cmd_pos = False
                continue
            if text == "case":
                case_depth.append(case_state)
                case_state = "subject"
                cmd_pos = False
                continue
            if text == "function":
                if i < len(toks) and toks[i][0] == "word":
                    functions.add(toks[i][1].text)
                    i += 1
                cmd_pos = True
                continue
            if text in START_WORDS:
                cmd_pos = True
                continue
            if text == "esac":
                case_state = case_depth.pop() if case_depth else None
            cmd_pos = False
            continue
        if ASSIGNMENT.match(text):
            continue
        if i + 1 < len(toks) and toks[i][0] == "op" and toks[i][1] == "(" and \
                toks[i + 1][0] == "op" and toks[i + 1][1] == ")":
            functions.add(text)
            i += 2
            cmd_pos = True
            continue
        command = Command(word.plain if not word.dynamic else word.text, [], word.line, word.dynamic)
        finish(command)
        current = command
        cmd_pos = False
        wrapper = command.name if command.name in WRAPPERS else None
    return found, functions


def options(command):
    """The options of a command as written: the words that begin with a dash, to the first that does not."""
    out = []
    for arg in command.args:
        if arg.startswith("-") and len(arg) > 1:
            out.append(arg)
        else:
            break
    return out
