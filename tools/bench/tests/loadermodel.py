# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A deliberately permissive model of the loader's command interpreter.

The model reads a typed line the way the real loader might, which is wider
than what the guard accepts: quotes and backslashes, ``;``, ``&&``, ``||``,
``if ... fi``, ``$name`` and ``${name}`` expanded before the line is split,
command substitution, command names that are any unambiguous prefix, numbers
that are the longest run of hexadecimal digits read modulo 2**32, ``+length``
for ``sf erase``, and the commands ``sf``, ``mw``, ``cp``, ``loady``,
``tftpboot``, ``erase``, ``protect``, ``setenv``, ``saveenv``, ``run``,
``boot``, ``bootm``, ``reset`` and a few more. It also lets an address that
lies beyond the chip wrap onto it. The guard is only worth anything if the
model can be fooled by what the guard lets through.

Parentheses are ordinary characters inside the words of a ``setenv``, which
takes a partition table such as ``256K(boot)`` as data. Anywhere else ``(``
starts a group whose content runs as commands, when the command it belongs to
runs, so a guard that lets a parenthesis through outside ``setenv`` is caught.

Every command that changes anything records what it changed and where, in
``ops``; the invariant test reads that record and never the guard's opinion.
The model does not keep flash contents; a subclass that does (the simulated
board) hooks ``flash_erase`` and ``flash_write``.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

MASK32 = 0xFFFFFFFF
FLASH_SIZE = 0x800000
ADDRESS_SPACE = 1 << 32
ENV_RANGES = ((0x40000, 0x42000), (0x42000, 0x44000))
DEFAULT_LOAD_ADDRESS = 0x82000000

COMMANDS = (
    "base", "bdinfo", "boot", "bootd", "bootm", "cmp", "coninfo", "cp", "crc32", "echo",
    "erase", "false", "go", "help", "itest", "loady", "loop", "md", "mm", "mtest", "mw",
    "nm", "printenv", "protect", "reset", "run", "saveenv", "setenv", "sf", "sleep",
    "tftpboot", "true", "version",
)
SF_COMMANDS = ("erase", "probe", "read", "update", "write")
_SIZES = {"": 4, "l": 4, "w": 2, "b": 1}
_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_HEX_PREFIX = re.compile(r"[0-9A-Fa-f]*")


@dataclass
class Op:
    """One recorded effect: ``kind`` is erase, write, store, saveenv or exec.

    ``ranges`` are flash offsets for erase, write and saveenv, and bus
    addresses for store and exec.
    """

    kind: str
    ranges: list[tuple[int, int]]
    detail: str = ""


def span(start: int, length: int, modulus: int = ADDRESS_SPACE) -> list[tuple[int, int]]:
    """The intervals ``length`` bytes from ``start`` cover when addresses wrap."""
    if length <= 0:
        return []
    if length >= modulus:
        return [(0, modulus)]
    start %= modulus
    end = start + length
    if end <= modulus:
        return [(start, end)]
    return [(start, modulus), (0, end - modulus)]


def number(word: str) -> int:
    """Read a number as the loader might: leading hex digits, modulo 2**32."""
    text = word[2:] if word[:2] in ("0x", "0X") else word
    digits = _HEX_PREFIX.match(text).group(0)  # type: ignore[union-attr]
    return int(digits, 16) & MASK32 if digits else 0


@dataclass
class _Frame:
    parent_enabled: bool
    condition: bool
    enabled: bool = False


@dataclass
class _Group:
    """The text between ``(`` and its ``)``: it runs as commands with its command."""

    text: str


@dataclass
class LoaderModel:
    """The interpreter: an environment, a record of effects and an output log."""

    env: dict[str, str] = field(default_factory=dict)
    ops: list[Op] = field(default_factory=list)
    output: list[str] = field(default_factory=list)
    terminal: str | None = None  # set when a command resets or leaves the loader
    max_depth: int = 12

    # -- running a line --------------------------------------------------------

    def run(self, text: str, depth: int = 0) -> int:
        """Execute a whole line, as the loader would; return the last status."""
        if depth > self.max_depth:
            self.print("macro nesting too deep")
            return 1
        expanded = self._expand(text, depth)
        status = 0
        frames: list[_Frame] = []
        for separator, words in self._parse(expanded):
            if not words:
                continue
            enabled = frames[-1].enabled if frames else True
            if separator == "&&" and status != 0:
                continue
            if separator == "||" and status == 0:
                continue
            first = words[0]
            if first == "if":
                result = self._command(words[1:], depth) if enabled and words[1:] else 1
                frames.append(_Frame(enabled, result == 0))
                if enabled:
                    status = result
                continue
            if first in ("then", "else") and frames:
                frame = frames[-1]
                frame.enabled = frame.parent_enabled and (
                    frame.condition if first == "then" else not frame.condition)
                words = words[1:]
                if not words or not frame.enabled:
                    continue
            elif first == "fi":
                if frames:
                    frames.pop()
                continue
            elif not enabled:
                continue
            status = self._command(words, depth)
            if self.terminal is not None:
                break
        return status

    def print(self, text: str) -> None:
        """Append a line of output."""
        self.output.append(text)

    # -- expansion and parsing -------------------------------------------------

    def _expand(self, text: str, depth: int) -> str:
        """Expand variables and substitutions textually, before the line is split."""
        out: list[str] = []
        pos, single = 0, False
        while pos < len(text):
            ch = text[pos]
            if ch == "'":
                single = not single
                out.append(ch)
            elif single or ch not in "$`":
                out.append(ch)
            elif ch == "`":
                end = text.find("`", pos + 1)
                inner = text[pos + 1:end if end >= 0 else len(text)]
                out.append(self._capture(inner, depth))
                pos = len(text) if end < 0 else end
            elif text.startswith("$(", pos):
                level, scan = 1, pos + 2
                while scan < len(text) and level:
                    level += {"(": 1, ")": -1}.get(text[scan], 0)
                    scan += 1
                out.append(self._capture(text[pos + 2:scan - 1 if level == 0 else scan], depth))
                pos = scan - 1
            elif text.startswith("${", pos):
                end = text.find("}", pos)
                name = text[pos + 2:end if end >= 0 else len(text)]
                out.append(self.env.get(name, ""))
                pos = len(text) if end < 0 else end
            else:
                match = _NAME.match(text, pos + 1)
                if match is None:
                    out.append("$")
                else:
                    out.append(self.env.get(match.group(0), ""))
                    pos = match.end() - 1
            pos += 1
        return "".join(out)

    def _capture(self, command: str, depth: int) -> str:
        saved, self.output = self.output, []
        self.run(command, depth + 1)
        captured, self.output = " ".join(self.output), saved
        return captured

    @staticmethod
    def _names_setenv(words: list) -> bool:
        """Does the part read so far name ``setenv`` (after a leading if, then or else)?"""
        index = 1 if words and isinstance(words[0], str) \
            and words[0] in ("if", "then", "else") else 0
        return index < len(words) and isinstance(words[index], str) \
            and LoaderModel._find(words[index].partition(".")[0]) == "setenv"

    @staticmethod
    def _group_end(text: str, start: int) -> int:
        """The index of the ``)`` that closes the ``(`` at ``start``, or the end of the text."""
        level = 0
        for pos in range(start, len(text)):
            level += {"(": 1, ")": -1}.get(text[pos], 0)
            if level == 0:
                return pos
        return len(text)

    @staticmethod
    def _parse(text: str) -> list[tuple[str, list]]:
        """Split into (separator, words) pairs, honouring quotes and backslashes.

        A word is text or a ``_Group``; groups are made only outside ``setenv``.
        """
        parts: list[tuple[str, list]] = []
        words: list = []
        word: str | None = None
        quote = ""
        separator = ""
        pos = 0
        while pos < len(text):
            ch = text[pos]
            if quote == "'":
                if ch == "'":
                    quote = ""
                else:
                    word = (word or "") + ch
            elif quote == '"':
                if ch == '"':
                    quote = ""
                elif ch == "\\" and pos + 1 < len(text):
                    pos += 1
                    word = (word or "") + text[pos]
                else:
                    word = (word or "") + ch
            elif ch in " \t":
                if word is not None:
                    words.append(word)
                    word = None
            elif ch == "\\" and pos + 1 < len(text):
                pos += 1
                word = (word or "") + text[pos]
            elif ch in "'\"":
                quote = ch
                word = word or ""
            elif ch in ";\n\r" or text.startswith(("&&", "||"), pos):
                if word is not None:
                    words.append(word)
                    word = None
                parts.append((separator, words))
                words = []
                if ch in ";\n\r":
                    separator = ";"
                else:
                    separator = text[pos:pos + 2]
                    pos += 1
            elif ch == "(" and not LoaderModel._names_setenv(words):
                if word is not None:
                    words.append(word)
                    word = None
                end = LoaderModel._group_end(text, pos)
                words.append(_Group(text[pos + 1:end]))
                pos = end
            else:
                word = (word or "") + ch
            pos += 1
        if word is not None:
            words.append(word)
        parts.append((separator, words))
        return parts

    # -- commands --------------------------------------------------------------

    @staticmethod
    def _find(name: str, table: tuple[str, ...] = COMMANDS) -> str | None:
        """An exact name, or the one command that the text abbreviates."""
        if name in table:
            return name
        matches = [candidate for candidate in table if candidate.startswith(name) and name]
        return matches[0] if len(matches) == 1 else None

    def _command(self, words: list, depth: int) -> int:
        status = 0
        plain: list[str] = []
        for word in words:
            if isinstance(word, _Group):
                status = self.run(word.text, depth + 1)
            else:
                plain.append(word)
        if not plain:
            return status
        words = plain
        base, _, suffix = words[0].partition(".")
        name = self._find(base)
        if name is None:
            self.print("Unknown command '%s' - try 'help'" % words[0])
            return 1
        handler = getattr(self, "cmd_" + name)
        result = handler(words[1:], suffix, depth)
        return 0 if result is None else result

    def cmd_base(self, args, suffix, depth):
        return None

    cmd_bdinfo = cmd_base
    cmd_coninfo = cmd_base
    cmd_help = cmd_base
    cmd_loop = cmd_base
    cmd_mtest = cmd_base
    cmd_cmp = cmd_base
    cmd_md = cmd_base
    cmd_sleep = cmd_base
    cmd_bootd = cmd_base
    cmd_protect = cmd_base
    cmd_true = cmd_base

    def cmd_false(self, args, suffix, depth):
        return 1

    def cmd_echo(self, args, suffix, depth):
        self.print(" ".join(args))

    def cmd_version(self, args, suffix, depth):
        self.print("U-Boot 2011.12.NA")

    def cmd_reset(self, args, suffix, depth):
        self.terminal = "reset"

    def cmd_printenv(self, args, suffix, depth):
        names = args or sorted(self.env)
        status = 0
        for name in names:
            if name in self.env:
                self.print("%s=%s" % (name, self.env[name]))
            else:
                self.print('## Error: "%s" not defined' % name)
                status = 1
        return status

    def cmd_setenv(self, args, suffix, depth):
        if not args:
            return 1
        if len(args) == 1:
            self.env.pop(args[0], None)
        else:
            self.env[args[0]] = " ".join(args[1:])

    def cmd_saveenv(self, args, suffix, depth):
        self.ops.append(Op("saveenv", [ENV_RANGES[0], ENV_RANGES[1]]))
        self.on_saveenv()

    def on_saveenv(self) -> None:
        """Hook: the environment was saved."""

    def cmd_run(self, args, suffix, depth):
        status = 0
        for name in args:
            if name not in self.env:
                self.print('## Error: "%s" not defined' % name)
                status = 1
                continue
            status = self.run(self.env[name], depth + 1)
        return status

    def cmd_boot(self, args, suffix, depth):
        return self.run(self.env.get("bootcmd", ""), depth + 1)

    def cmd_bootm(self, args, suffix, depth):
        address = number(args[0]) if args else DEFAULT_LOAD_ADDRESS
        self.ops.append(Op("exec", [(address, address + 1)], "bootm"))
        self.on_bootm(address)

    def on_bootm(self, address: int) -> None:
        """Hook: a kernel image at ``address`` is started."""

    def cmd_go(self, args, suffix, depth):
        address = number(args[0]) if args else DEFAULT_LOAD_ADDRESS
        self.ops.append(Op("exec", [(address, address + 1)], "go"))

    def cmd_itest(self, args, suffix, depth):
        if len(args) != 3:
            return 1
        left, operator, right = args
        if suffix != "s":
            left, right = number(left), number(right)  # type: ignore[assignment]
        outcomes = {"==": left == right, "!=": left != right, "<": left < right,
                    ">": left > right, "<=": left <= right, ">=": left >= right}
        return 0 if outcomes.get(operator, False) else 1

    # memory

    def cmd_mw(self, args, suffix, depth):
        if len(args) < 2:
            return 1
        size = _SIZES.get(suffix, 4)
        count = number(args[2]) if len(args) > 2 else 1
        address = number(args[0])
        self.ops.append(Op("store", span(address, count * size), "mw"))
        self.on_store(address, number(args[1]), size)

    def cmd_mm(self, args, suffix, depth):
        if not args:
            return 1
        size = _SIZES.get(suffix, 4)
        self.ops.append(Op("store", span(number(args[0]), size), "mm"))

    cmd_nm = cmd_mm

    def cmd_cp(self, args, suffix, depth):
        if len(args) < 3:
            return 1
        size = _SIZES.get(suffix, 4)
        self.ops.append(Op("store", span(number(args[1]), number(args[2]) * size), "cp"))

    def on_store(self, address: int, value: int, size: int) -> None:
        """Hook: ``size`` bytes of ``value`` were stored at ``address``."""

    def cmd_loady(self, args, suffix, depth):
        address = number(args[0]) if args else DEFAULT_LOAD_ADDRESS
        self.ops.append(Op("store", span(address, 1), "loady"))
        self.env["fileaddr"] = "%x" % address
        self.on_load(address, "loady")

    def cmd_tftpboot(self, args, suffix, depth):
        address = number(args[0]) if args else DEFAULT_LOAD_ADDRESS
        self.ops.append(Op("store", span(address, 1), "tftpboot"))
        self.env["fileaddr"] = "%x" % address
        self.on_load(address, "tftpboot", args[1] if len(args) > 1 else "")

    def on_load(self, address: int, how: str, name: str = "") -> None:
        """Hook: data is received into RAM at ``address``."""

    def cmd_crc32(self, args, suffix, depth):
        if len(args) < 2:
            return 1
        address, length = number(args[0]), number(args[1])
        if len(args) > 2:
            self.ops.append(Op("store", span(number(args[2]), 4), "crc32"))
        self.print("CRC32 for %08x ... %08x ==> %08x" % (
            address, (address + length - 1) & MASK32, self.checksum(address, length)))

    def checksum(self, address: int, length: int) -> int:
        """The CRC-32 of memory; the model has none and answers 0."""
        return 0

    # flash

    def cmd_sf(self, args, suffix, depth):
        sub = self._find(args[0], SF_COMMANDS) if args else None
        if sub is None:
            self.print("sf: unknown or missing subcommand")
            return 1
        if sub == "probe":
            return 0
        if sub == "erase" and len(args) >= 3:
            length = args[2][1:] if args[2].startswith("+") else args[2]
            self.flash_erase(number(args[1]), number(length), "sf erase")
            return 0
        if sub in ("write", "update") and len(args) >= 4:
            memory, offset, length = number(args[1]), number(args[2]), number(args[3])
            if sub == "update":
                self.flash_erase(offset, length, "sf update")
            self.flash_write(offset, length, memory, "sf " + sub)
            return 0
        if sub == "read":
            return 0
        self.print("sf: missing arguments")
        return 1

    def cmd_erase(self, args, suffix, depth):
        if len(args) < 2:
            return 1
        start = number(args[0])
        length = number(args[1][1:]) if args[1].startswith("+") else number(args[1]) - start + 1
        offset = self.window_offset(start)
        if offset is None:
            self.print("Error: start address not on flash")
            return 1
        self.flash_erase(offset, length, "erase")

    @staticmethod
    def window_offset(address: int) -> int | None:
        """The flash offset behind a window address, in any spelling; None if none."""
        physical = address & 0x1FFFFFFF if address >= 0x80000000 else address
        if 0x14000000 <= physical < 0x15000000:
            return (physical & 0xFFFFFF) % FLASH_SIZE
        if 0x1FC00000 <= physical < 0x20000000:
            return physical & 0x3FFFFF
        return None

    def flash_erase(self, offset: int, length: int, how: str) -> None:
        self.ops.append(Op("erase", span(offset, length, FLASH_SIZE), how))
        self.on_flash("erase", offset, length)

    def flash_write(self, offset: int, length: int, memory: int, how: str) -> None:
        self.ops.append(Op("write", span(offset, length, FLASH_SIZE), how))
        self.on_flash("write", offset, length, memory)

    def on_flash(self, kind: str, offset: int, length: int, memory: int = 0) -> None:
        """Hook: the flash is erased or written (offsets as the loader was given them)."""
