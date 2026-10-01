# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The bank guard: which command lines may reach the boot loader.

The guard decides, before a byte is typed, whether a line may be sent to the
loader's prompt. It is an allowlist with the arguments checked, not a
blocklist: a line that is not one of the listed forms is refused, however
harmless it looks. The loader executes whatever arrives, and a damaged
``boot`` partition is assumed to be beyond repair from the loader, so the tool
never relies on the board to refuse anything.

What the guard protects is the flash map of the board. Only the two
partitions of bank 0 (``k0`` and ``r0``) are ever written; the loader
(``boot``), its two environment copies (``env`` and ``env2``), the device
configuration store (``config``) and the shipped firmware of bank 1 (``k1``
and ``r1``) are protected. The memory-mapped windows through which the loader
can read the flash are never written either.

A line is judged by ``check_line`` from the line itself, the variables read
from the board and a small session state. The macros that ``run`` and
``boot`` would execute are judged separately, by ``preflight``, from the text
the macros have on the board at that moment. Both raise ``Refusal`` with a
code and a detail; nothing has been sent when they do.

Where the loader's behaviour is not known, the guard assumes the worst: the
loader may accept abbreviated command names, read a number modulo 2**32, or
split an expanded variable at a separator, so the guard accepts none of those
spellings. The one parenthesis it accepts, in the value of a variable that a
``setenv bootargs`` part of a macro names, rests on the loader taking a
partition table as data, which has not been checked on the board.
"""

from __future__ import annotations

import re
from collections.abc import Callable, Iterator, Mapping
from dataclasses import dataclass, field

# Refusal codes.
SYNTAX = "syntax"
OVERFLOW = "overflow"
UNKNOWN_VARIABLE = "unknown-variable"
NOT_ALLOWED = "not-allowed"
PROTECTED_RANGE = "protected-range"
PROTECTED_VARIABLE = "protected-variable"
FLASH_APERTURE = "flash-aperture"
BANK1_TRIAL = "bank1-trial"
STATE = "state"
MISSING_MACRO = "missing-macro"
DEPTH = "depth"

CODES = (SYNTAX, OVERFLOW, UNKNOWN_VARIABLE, NOT_ALLOWED, PROTECTED_RANGE,
         PROTECTED_VARIABLE, FLASH_APERTURE, BANK1_TRIAL, STATE, MISSING_MACRO,
         DEPTH)

MAX_LINE = 200  # characters in one raw line
MAX_DEPTH = 4  # nesting of ``run`` inside the macros that are checked
MAX_NAME = 64  # characters in a variable name or a file name


def _printable(text: str) -> str:
    """The text with everything outside printable ASCII written as an escape."""
    out = []
    for ch in text:
        code = ord(ch)
        if 0x20 <= code < 0x7F:
            out.append(ch)
        elif code < 0x100:
            out.append("\\x%02x" % code)
        else:
            out.append("\\u%04x" % code)
    return "".join(out)


class Refusal(Exception):
    """A line, a word or a macro that the guard will not let through."""

    exit_status = 3

    def __init__(self, code: str, detail: str = "", line: str = "") -> None:
        super().__init__(code, detail, line)
        self.code = code
        self.detail = detail
        self.line = line

    def __str__(self) -> str:
        return "refused [%s] %s: %s" % (self.code, self.detail, _printable(self.line))


class LayoutMismatch(Exception):
    """A layout variable of the board differs from the tool's own table."""

    exit_status = 5

    def __init__(self, name: str, expected: int, actual: str | None) -> None:
        shown = "not defined" if actual is None else "reads %r" % _printable(actual)
        super().__init__("%s %s, the tool's table has %x" % (name, shown, expected))
        self.name = name
        self.expected = expected
        self.actual = actual


# --------------------------------------------------------------------------
# The flash map
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class Partition:
    """One partition of the 8 MiB flash, in bytes from its start."""

    name: str
    start: int
    size: int
    writable: bool

    @property
    def end(self) -> int:
        """The offset of the first byte after the partition."""
        return self.start + self.size


FLASH_SIZE = 0x800000

PARTITIONS = (
    Partition("boot", 0x000000, 0x040000, False),
    Partition("env", 0x040000, 0x002000, False),
    Partition("env2", 0x042000, 0x002000, False),
    Partition("config", 0x044000, 0x03C000, False),
    Partition("k0", 0x080000, 0x14C000, True),
    Partition("r0", 0x1CC000, 0x274000, True),
    Partition("k1", 0x440000, 0x14C000, False),
    Partition("r1", 0x58C000, 0x274000, False),
)
BY_NAME = {part.name: part for part in PARTITIONS}
WRITABLE = tuple(part for part in PARTITIONS if part.writable)
# The order in which an overlap with a protected partition is reported.
PROTECTED = tuple(BY_NAME[name] for name in ("boot", "env", "env2", "config", "k1", "r1"))

# The loader reads the chip through this window (cached alias of the physical
# window at 0x14000000); a partition's offset added to it is its address.
FLASH_WINDOW = 0x94000000

# Everything the guard treats as a way to write the flash without ``sf``: the
# 16 MiB window at physical 0x14000000 (wider than the chip, in case it is
# mirrored) and the reset-vector window at physical 0x1fc00000, each in the
# three spellings of the same memory (physical, cached and uncached).
_SEGMENT_BASES = (0x00000000, 0x80000000, 0xA0000000)
_APERTURE_PHYSICAL = ((0x14000000, 0x15000000), (0x1FC00000, 0x20000000))
APERTURE = tuple((low + base, high + base)
                 for base in _SEGMENT_BASES for low, high in _APERTURE_PHYSICAL)
# RAM: 32 MiB at physical 0, in the same three spellings.
_RAM = tuple((base, base + 0x2000000) for base in _SEGMENT_BASES)

# The only control register the tool ever writes, and the macro-window range.
WATCHDOG_CONTROL = 0xB8003268
REGISTER_WINDOW = (0xB8000000, 0xBBFFFFFF)
# Where the tool receives images, and the one other place a kernel is loaded.
IMAGE_ADDRESS = 0x80000000
KERNEL_ADDRESS = 0x81000000
# Kernels the macros may start: RAM copies, and the two banks' flash windows.
RUN_BOOT_ADDRESSES = frozenset({0x80000000, 0x81000000, 0x94080000, 0xB4080000})
CHAIN_BOOT_ADDRESSES = RUN_BOOT_ADDRESSES | {0x94440000, 0xB4440000}

# Layout the board's environment must agree with before any write.
LAYOUT_VARIABLES = {
    "fl_boot_sz": 0x40000,
    "fl_env": 0x40000,
    "fl_env2": 0x42000,
    "fl_env_sz": 0x2000,
    "fl_cfgfs": 0x44000,
    "fl_cfgfs_sz": 0x3C000,
    "fl_kernel1": 0x80000,
    "fl_kernel1_sz": 0x14C000,
    "fl_rootfs1": 0x1CC000,
    "fl_rootfs1_sz": 0x274000,
    "fl_kernel2": 0x440000,
    "fl_kernel2_sz": 0x14C000,
    "fl_rootfs2": 0x58C000,
    "fl_rootfs2_sz": 0x274000,
    "img0_kernel": 0x94080000,
    "img1_kernel": 0x94440000,
}


def _within(value: int, spans: tuple[tuple[int, int], ...]) -> bool:
    return any(low <= value < high for low, high in spans)


def in_aperture(value: int) -> bool:
    """Is the address one of the spellings of a window onto the flash?"""
    return _within(value, APERTURE)


def in_ram(value: int) -> bool:
    """Is the address inside the board's RAM, in any of its spellings?"""
    return _within(value, _RAM)


def window_address(offset: int) -> int:
    """The address at which the loader shows the flash byte at ``offset``."""
    return FLASH_WINDOW + offset


# --------------------------------------------------------------------------
# Words, numbers and variables
# --------------------------------------------------------------------------

_NUMBER = re.compile(r"(?:0[xX])?([0-9A-Fa-f]{1,16})")
_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_REFERENCE = re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*\}|\$[A-Za-z_][A-Za-z0-9_]*")
_PRINTABLE_OR_TAB = re.compile(r"[\t\x20-\x7e]*")
_FILE_NAME = re.compile(r"[A-Za-z0-9._-]{1,%d}" % MAX_NAME)
_BARE_QUOTES = frozenset("`'\"\\")
_SEPARATORS = frozenset(";&|(){}<>$")
_WORD_SPLIT = re.compile(r"[ \t]+")


def parse_number(word: str) -> int:
    """The value of a number as the loader reads it, or a refusal.

    A number is ``0x`` or ``0X`` followed by one to sixteen hexadecimal
    digits, or just those digits: no sign, no suffix. The loader reads 32
    bits, so a value above 0xffffffff is refused rather than wrapped.
    """
    match = _NUMBER.fullmatch(word)
    if match is None:
        raise Refusal(SYNTAX, "not a number: %s" % _printable(word))
    value = int(match.group(1), 16)
    if value > 0xFFFFFFFF:
        raise Refusal(OVERFLOW, "%s is wider than 32 bits" % _printable(word))
    return value


def _split(text: str) -> list[str]:
    """Split on runs of spaces and tabs; an empty text has no words."""
    stripped = text.strip(" \t")
    return _WORD_SPLIT.split(stripped) if stripped else []


def _check_raw(line: str) -> None:
    """Step 1: length, character set and quoting of the line as given."""
    if len(line) > MAX_LINE:
        raise Refusal(SYNTAX, "longer than %d characters" % MAX_LINE)
    if _PRINTABLE_OR_TAB.fullmatch(line) is None:
        raise Refusal(SYNTAX, "a character outside printable ASCII")
    for ch in line:
        if ch in _BARE_QUOTES:
            raise Refusal(SYNTAX, "the character %r" % ch)


def _check_expanded(text: str) -> None:
    """Step 3: no separator, grouping character or quote survives expansion.

    The raw text has been checked for quotes before; a variable's value has not,
    and a quote in a value could hide a command name as well as one typed.
    """
    for ch in text:
        if ch in _SEPARATORS or ch in _BARE_QUOTES:
            raise Refusal(SYNTAX, "the character %r" % ch)


def expand(text: str, variables: Mapping[str, str], *, undefined_empty: bool = False,
           parentheses_in_values: bool = False) -> str:
    """Replace ``${name}`` and ``$name`` by the variable's value.

    A name is a letter or underscore followed by the longest run of letters,
    digits and underscores. An undefined name is refused unless
    ``undefined_empty`` is set, a ``$`` that starts neither form is refused,
    and so is a value that contains a ``$``: values are never expanded again.
    With ``parentheses_in_values`` a value may hold ``(`` and ``)``, and then
    every other character of the separator set is refused here, in the value,
    because the check of the expanded text cannot tell a value from the text
    around it; the caller checks that text itself.
    """
    if "$" not in text:
        return text
    out: list[str] = []
    pos = 0
    while pos < len(text):
        ch = text[pos]
        if ch != "$":
            out.append(ch)
            pos += 1
            continue
        if text.startswith("${", pos):
            match = _NAME.match(text, pos + 2)
            if match is None or text[match.end():match.end() + 1] != "}":
                raise Refusal(SYNTAX, "unterminated or empty ${...}")
            name, pos = match.group(0), match.end() + 1
        else:
            match = _NAME.match(text, pos + 1)
            if match is None:
                raise Refusal(SYNTAX, "a $ that starts no variable name")
            name, pos = match.group(0), match.end()
        if name in variables:
            value = variables[name]
        elif undefined_empty:
            value = ""
        else:
            raise Refusal(UNKNOWN_VARIABLE, name)
        if "$" in value:
            raise Refusal(SYNTAX, "the value of %s holds a $" % name)
        if parentheses_in_values:
            _check_expanded(value.replace("(", "").replace(")", ""))
        out.append(value)
    return "".join(out)


def _memory_argument(words: list[str]) -> str | None:
    """The word of a line that names the memory the command would write."""
    name = words[0]
    if name in _FIRST_MEMORY and len(words) >= 2:
        return words[1]
    if name in _SECOND_MEMORY and len(words) >= 3:
        return words[2]
    if name == "sf" and len(words) >= 3 and words[1] == "write":
        return words[2]
    return None


_FIRST_MEMORY = frozenset({"mw", "mw.b", "mw.w", "mw.l", "loady", "tftpboot"})
_SECOND_MEMORY = frozenset({"cp", "cp.b", "cp.w", "cp.l"})


def _check_aperture(words: list[str]) -> None:
    """Step 4a: refuse a store into a window onto the flash, whatever the command."""
    if not words:
        return
    argument = _memory_argument(words)
    if argument is None:
        return
    match = _NUMBER.fullmatch(argument)
    if match is not None and in_aperture(int(match.group(1), 16)):
        raise Refusal(FLASH_APERTURE, argument)


# --------------------------------------------------------------------------
# The range rule
# --------------------------------------------------------------------------

def check_range(offset: int, length: int) -> Partition:
    """The writable partition that holds ``length`` bytes at ``offset``, or a refusal.

    The checks run in a fixed order: a zero length, an end beyond the chip,
    any overlap with a protected partition (the first in the map's order
    names the refusal), and finally that the range lies wholly inside one of
    the two writable partitions.
    """
    if length < 1:
        raise Refusal(PROTECTED_RANGE, "zero length")
    end = offset + length
    if end > FLASH_SIZE:
        raise Refusal(PROTECTED_RANGE, "beyond")
    for part in PROTECTED:
        if offset < part.end and end > part.start:
            raise Refusal(PROTECTED_RANGE, part.name)
    for part in WRITABLE:
        if part.start <= offset and end <= part.end:
            return part
    raise Refusal(PROTECTED_RANGE, "bank0-span")


# --------------------------------------------------------------------------
# Session state and the verdict
# --------------------------------------------------------------------------

@dataclass
class Session:
    """What the guard remembers between the lines of one loader session.

    ``pending`` holds the ``setenv`` forms it let through since the last
    ``saveenv``; ``saveenv`` is allowed only while one is pending. ``commit``
    is set by the caller for an explicit request to commit bank 0.
    """

    commit: bool = False
    pending: list[tuple[str, str]] = field(default_factory=list)


@dataclass(frozen=True)
class Approval:
    """A line the guard let through, as it may be typed.

    ``text`` is the line after expansion, its words separated by one space:
    what the loader receives, with the numbers the guard checked.
    """

    line: str
    text: str
    name: str


# Macros that may be run, and variables that may only be set in the forms
# the guard allows.
RUN_MACROS = frozenset({"ramboot", "sdsup", "b0"})
_PROTECTED_PREFIXES = ("sw_", "fl_", "img", "boot", "mtdparts")
_PROTECTED_NAMES = frozenset({
    "ramboot", "sdsup", "b0", "b1", "en_wdt", "ipaddr", "serverip", "netmask",
    "ethaddr", "tftp_base", "filesize", "fileaddr", "loadaddr",
})
READ_ONLY_NAMES = frozenset({"", "printenv", "version", "crc32"})


def _count(words: list[str], expected: int) -> None:
    if len(words) != expected:
        raise Refusal(NOT_ALLOWED, "%s takes %d arguments here" % (words[0], expected - 1))


def _reads_as_one(value: str) -> bool:
    """Does a variable's text read as the number 1, however it is spelled?"""
    text = value.strip(" \t")
    match = _NUMBER.fullmatch(text)
    return match is not None and int(match.group(1), 16) == 1


def _form_empty(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    return None


def _form_printenv(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    if len(words) > 2:
        raise Refusal(NOT_ALLOWED, "printenv takes at most one name")
    if len(words) == 2:
        name = words[1]
        if len(name) > MAX_NAME or _NAME.fullmatch(name) is None:
            raise Refusal(NOT_ALLOWED, "not a variable name")


def _form_bare(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 1)


def _form_boot(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 1)
    value = variables.get("sw_tryactive")
    if value is not None and _reads_as_one(value):
        raise Refusal(BANK1_TRIAL, "sw_tryactive reads 1")


def _form_setenv(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    if len(words) < 2:
        raise Refusal(NOT_ALLOWED, "setenv without a name")
    name = words[1]
    allowed = (words[2:] in (["0"], ["2"]) and name == "sw_tryactive") or \
              (session.commit and words[2:] == ["0"] and name == "sw_commit")
    if allowed:
        session.pending.append((name, words[2]))
        return
    if name.startswith(_PROTECTED_PREFIXES) or name in _PROTECTED_NAMES:
        raise Refusal(PROTECTED_VARIABLE, name)
    raise Refusal(NOT_ALLOWED, "setenv of %s" % _printable(name))


def _form_saveenv(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 1)
    if not session.pending:
        raise Refusal(STATE, "saveenv with no setenv pending")
    session.pending.clear()


def _form_run(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 2)
    if words[1] not in RUN_MACROS:
        raise Refusal(NOT_ALLOWED, "macro %s" % _printable(words[1]))


def _form_mw(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 3)
    address = parse_number(words[1])
    value = parse_number(words[2])
    if address != WATCHDOG_CONTROL or value != 0:
        raise Refusal(NOT_ALLOWED, "only the watchdog control register may be written")


def _form_loady(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 2)
    if parse_number(words[1]) != IMAGE_ADDRESS:
        raise Refusal(NOT_ALLOWED, "loady loads at one address only")


def _form_crc32(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 3)
    address = parse_number(words[1])
    length = parse_number(words[2])
    if not (in_ram(address) or in_aperture(address)):
        raise Refusal(NOT_ALLOWED, "crc32 reads RAM or the flash windows")
    if length < 1:
        raise Refusal(NOT_ALLOWED, "crc32 of nothing")
    if address + length > 1 << 32:
        raise Refusal(OVERFLOW, "the range passes the end of the address space")


def _form_tftpboot(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    _count(words, 3)
    address = parse_number(words[1])
    if address not in (IMAGE_ADDRESS, KERNEL_ADDRESS):
        raise Refusal(NOT_ALLOWED, "tftpboot loads at two addresses only")
    if _FILE_NAME.fullmatch(words[2]) is None:
        raise Refusal(NOT_ALLOWED, "not a plain file name")


def _form_sf(words: list[str], variables: Mapping[str, str], session: Session) -> None:
    if len(words) < 2 or words[1] not in ("erase", "write"):
        raise Refusal(NOT_ALLOWED, "sf subcommand")
    if words[1] == "erase":
        _count(words, 4)
        offset = parse_number(words[2])
        if not words[3].startswith("+"):
            raise Refusal(NOT_ALLOWED, "a length needs a leading plus")
        length = parse_number(words[3][1:])
        check_range(offset, length)
        return
    _count(words, 5)
    ram = parse_number(words[2])
    offset = parse_number(words[3])
    length = parse_number(words[4])
    if ram != IMAGE_ADDRESS:
        raise Refusal(NOT_ALLOWED, "sf write reads from one RAM address only")
    check_range(offset, length)


_FORMS: dict[str, Callable[[list[str], Mapping[str, str], Session], None]] = {
    "": _form_empty,
    "printenv": _form_printenv,
    "version": _form_bare,
    "reset": _form_bare,
    "boot": _form_boot,
    "setenv": _form_setenv,
    "saveenv": _form_saveenv,
    "run": _form_run,
    "mw.l": _form_mw,
    "loady": _form_loady,
    "crc32": _form_crc32,
    "tftpboot": _form_tftpboot,
    "sf": _form_sf,
}


def check_line(line: str, variables: Mapping[str, str] | None = None,
               session: Session | None = None, *, read_only: bool = False) -> Approval:
    """Allow a line or raise ``Refusal``.

    The checks run in this order and the first that fails decides: the raw
    line (length, characters, quotes); expansion of ``$name`` and
    ``${name}``; the characters that join or group commands; the first word
    as the command name, compared exactly; a write into the flash windows;
    the allowlist of command names; the number of words; the numbers; and
    the rules of the form. ``variables`` are the variables read from the
    board, and ``session`` records what a ``setenv`` may be followed by.
    With ``read_only`` the console may only be read: nothing but the empty
    line, ``printenv``, ``version`` and ``crc32`` passes.
    """
    variables = {} if variables is None else variables
    session = Session() if session is None else session
    try:
        _check_raw(line)
        text = expand(line, variables)
        _check_expanded(text)
        words = _split(text)
        name = words[0] if words else ""
        _check_aperture(words)
        if read_only and name not in READ_ONLY_NAMES:
            raise Refusal(NOT_ALLOWED, "the console is read-only")
        form = _FORMS.get(name)
        if form is None:
            raise Refusal(NOT_ALLOWED, "command %s" % _printable(name))
        form(words, variables, session)
    except Refusal as refusal:
        refusal.line = line
        raise
    return Approval(line=line, text=" ".join(words), name=name)


# --------------------------------------------------------------------------
# The macro preflight
# --------------------------------------------------------------------------

_LOOP_WORDS = frozenset({"while", "for", "do", "done", "case", "esac"})
_IF_WORDS = frozenset({"if", "then", "else", "fi"})
_LEADING_IF_WORDS = frozenset({"if", "then", "else"})
_PART_SPLIT = re.compile(r"&&|\|\||;")
_REGISTER_WRITERS = frozenset({"mw", "mw.b", "mw.w", "mw.l"})


def _check_part_characters(part: str) -> None:
    """The characters of one part of a macro: printable ASCII, no quote of any kind."""
    if _PRINTABLE_OR_TAB.fullmatch(part) is None:
        raise Refusal(SYNTAX, "a character outside printable ASCII in a macro")
    for ch in part:
        if ch in _BARE_QUOTES:
            raise Refusal(SYNTAX, "the character %r in a macro" % ch)


def _expand_part(text: str, variables: Mapping[str, str], bootargs: bool) -> str:
    """Expand one part of a macro and check the characters that are left.

    A part is judged as a typed line is, with one exception. A ``setenv
    bootargs`` part assembles the kernel arguments from variables, and a
    partition table holds parentheses (``256K(boot)``) that ``setenv`` stores
    as data: those that come from the value of a variable are accepted, while
    the part's own text (everything outside the references) and every other
    character of a value are judged as before. In every other form a
    parenthesis from any source is refused.
    """
    if not bootargs:
        expanded = expand(text, variables)
        _check_expanded(expanded)
        return expanded
    expanded = expand(text, variables, undefined_empty=True, parentheses_in_values=True)
    _check_expanded(_REFERENCE.sub("", text))
    return expanded


def _walk(name: str, variables: Mapping[str, str], *, boot: bool, depth: int) -> None:
    """Check one macro and, through ``run``, the macros it starts."""
    if depth > MAX_DEPTH:
        raise Refusal(DEPTH, "macros nest deeper than %d" % MAX_DEPTH)
    if name not in variables:
        raise Refusal(MISSING_MACRO, name)
    for part in _PART_SPLIT.split(variables[name]):
        try:
            _check_part(part, variables, boot=boot, depth=depth)
        except Refusal as refusal:
            # Say which macro held the part, once: the innermost one.
            if " (macro " not in refusal.detail:
                refusal.detail = "%s (macro %s)" % (refusal.detail, name)
            raise


def _check_part(part: str, variables: Mapping[str, str], *, boot: bool, depth: int) -> None:
    raw = _split(part)
    if boot and raw and raw[0] in _LEADING_IF_WORDS:
        raw = raw[1:]
    elif boot and raw == ["fi"]:
        return
    if not raw:
        return
    for word in raw:
        if word in _LOOP_WORDS or (not boot and word in _IF_WORDS):
            raise Refusal(NOT_ALLOWED, "the word %s in a macro" % word)
    _check_part_characters(part)
    bootargs = raw[:2] == ["setenv", "bootargs"]
    text = _expand_part(" ".join(raw), variables, bootargs)
    words = _split(text)
    if not words:
        return
    _check_aperture(words)
    command = words[0]
    if command == "run":
        _count(words, 2)
        target = words[1]
        if boot:
            if _NAME.fullmatch(target) is None:
                raise Refusal(NOT_ALLOWED, "not a macro name")
        elif target not in RUN_MACROS:
            raise Refusal(NOT_ALLOWED, "macro %s" % _printable(target))
        _walk(target, variables, boot=boot, depth=depth + 1)
    elif command == "tftpboot" and not boot:
        _form_tftpboot(words, variables, Session())
    elif command == "setenv":
        _check_macro_setenv(words, boot=boot)
    elif command == "bootm":
        _count(words, 2)
        allowed = CHAIN_BOOT_ADDRESSES if boot else RUN_BOOT_ADDRESSES
        if parse_number(words[1]) not in allowed:
            raise Refusal(NOT_ALLOWED, "bootm at an address the macros may not use")
    elif command in _REGISTER_WRITERS:
        _count(words, 3)
        destination = parse_number(words[1])
        parse_number(words[2])
        if not REGISTER_WINDOW[0] <= destination <= REGISTER_WINDOW[1]:
            raise Refusal(NOT_ALLOWED, "a store outside the register window")
    elif boot and command == "itest.s":
        _count(words, 4)
        if words[2] not in ("==", "!="):
            raise Refusal(NOT_ALLOWED, "itest.s compares for equality only")
    elif boot and command == "saveenv":
        _count(words, 1)
    else:
        raise Refusal(NOT_ALLOWED, "command %s in a macro" % _printable(command))


def _check_macro_setenv(words: list[str], *, boot: bool) -> None:
    if len(words) >= 3 and words[1] == "bootargs":
        return
    if boot and len(words) == 3:
        if words[1] == "sw_active" and words[2] in ("0", "1"):
            return
        if words[1] == "sw_tryactive" and words[2] in ("0", "1", "2"):
            return
    raise Refusal(NOT_ALLOWED, "setenv of %s in a macro" % _printable(words[1] if words[1:] else ""))


def preflight_macro(name: str, variables: Mapping[str, str]) -> None:
    """Check the macro that ``run <name>`` would execute, and every macro it runs.

    Only ``ramboot``, ``sdsup`` and ``b0`` may be run. Each must consist of
    forms that cannot write the flash: a nested ``run`` of the same macros,
    a TFTP load into RAM, setting the kernel arguments, starting a kernel
    from RAM or from bank 0's window, and stores into the register window. A
    store into a flash window, a variable that is not defined, a macro that
    is not defined, a loop or a conditional all end the check.

    A parenthesis is refused in every form but one: the value of a variable
    that a ``setenv bootargs`` part names may hold parentheses, because a
    partition table does and ``setenv`` takes its words as data.
    """
    if name not in RUN_MACROS:
        raise Refusal(NOT_ALLOWED, "macro %s" % _printable(name))
    _walk(name, variables, boot=False, depth=1)


def preflight_boot(variables: Mapping[str, str]) -> None:
    """Check the boot chain that ``boot`` would execute.

    ``bootcmd`` and every macro it runs must be built from the forms above
    plus the comparison, the environment saves and the bank-selection
    variables that the chain uses. Because the chain starts whichever bank is
    committed, bank 1's window is an allowed kernel address here, and ``if``,
    ``then``, ``else`` and ``fi`` are understood. The chain consumes a
    pending trial, so the trial flag is read once more as the board has it
    now: a pending trial of bank 1 refuses ``boot`` whatever the line check
    saw earlier.
    """
    _walk("bootcmd", variables, boot=True, depth=1)
    _form_boot(["boot"], variables, Session())


def preflight(text: str, variables: Mapping[str, str]) -> None:
    """Run the preflight that a typed line calls for; other lines pass."""
    words = _split(text)
    if words == ["boot"]:
        preflight_boot(variables)
    elif len(words) == 2 and words[0] == "run":
        preflight_macro(words[1], variables)


# --------------------------------------------------------------------------
# Targets, banks and the layout
# --------------------------------------------------------------------------

_TARGETS = {"kernel": "k0", "k0": "k0", "rootfs": "r0", "r0": "r0"}


def check_target(word: str) -> str:
    """The partition that a target word names: ``k0`` or ``r0``.

    The command line takes a word, never an offset. The names of protected
    partitions are refused as such; any other word is not a target.
    """
    if word in _TARGETS:
        return _TARGETS[word]
    if word in BY_NAME:
        raise Refusal(PROTECTED_RANGE, word, word)
    raise Refusal(NOT_ALLOWED, "not a target: %s" % _printable(word), word)


def check_bank(value: str) -> int:
    """The bank number the command line asks for: only ``0`` is allowed."""
    if value == "0":
        return 0
    if value == "1":
        raise Refusal(PROTECTED_RANGE, "bank 1", value)
    raise Refusal(NOT_ALLOWED, "not a bank number: %s" % _printable(value), value)


def check_layout(variables: Mapping[str, str]) -> None:
    """Compare the board's layout variables with the tool's own table.

    Every ``fl_*`` value and the two kernel window addresses must equal the
    table as numbers. The first difference stops the operation, naming the
    variable, before a destructive line is typed: an environment that does
    not match is a different board or a changed one.
    """
    for name, expected in LAYOUT_VARIABLES.items():
        actual = variables.get(name)
        try:
            value = None if actual is None else parse_number(actual)
        except Refusal:
            value = None
        if value != expected:
            raise LayoutMismatch(name, expected, actual)


# --------------------------------------------------------------------------
# The guard of one console
# --------------------------------------------------------------------------

class LiveVariables(Mapping[str, str]):
    """Variable lookups that read the board's current text.

    The macro preflight must judge a macro by what it holds now, so each name
    it touches is read from the board once per check through ``fetch``, which
    returns the value or ``None`` for a name that is not defined.
    """

    def __init__(self, fetch: Callable[[str], str | None]) -> None:
        self._fetch = fetch
        self._cache: dict[str, str | None] = {}

    def __getitem__(self, name: str) -> str:
        if name not in self._cache:
            self._cache[name] = self._fetch(name)
        value = self._cache[name]
        if value is None:
            raise KeyError(name)
        return value

    def __iter__(self) -> Iterator[str]:
        return iter([name for name, value in self._cache.items() if value is not None])

    def __len__(self) -> int:
        return sum(1 for value in self._cache.values() if value is not None)


class Guard:
    """The guard of one console: session state, variables and live reads.

    ``variables`` holds the environment read from the board at the start of
    the session; a typed line is expanded from it. When ``fetch`` is given,
    the macro preflight does not use that copy: each macro and variable it
    names is read from the board at the moment, so a macro is judged by what
    it is now. A line typed on its own, before any copy was read, expands the
    variables it names from the board in the same way.
    """

    def __init__(self, *, commit: bool = False, read_only: bool = False,
                 variables: Mapping[str, str] | None = None,
                 fetch: Callable[[str], str | None] | None = None) -> None:
        self.session = Session(commit=commit)
        self.read_only = read_only
        self.variables: dict[str, str] = dict(variables or {})
        self.fetch = fetch

    def check(self, line: str, *, macros: bool = True) -> Approval:
        """Allow a line, including the preflight of ``run`` and ``boot``, or raise.

        With ``macros`` false only the line itself is judged; a dry run that
        has no board to read the macros from uses that.
        """
        live: Mapping[str, str] = (
            LiveVariables(self.fetch) if self.fetch is not None else self.variables)
        typed = self.variables if self.variables or self.fetch is None else live
        approval = check_line(line, typed, self.session, read_only=self.read_only)
        if macros:
            try:
                preflight(approval.text, live)
            except Refusal as refusal:
                refusal.line = line
                raise
        return approval
