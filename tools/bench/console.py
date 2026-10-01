# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The shared console of the board.

The board's UART is reached through a network bridge that serves one client
per port. That client runs in a tmux pane on the tool host, where a person
watches and types as well. A tool therefore shares the pane: it types keys
into it and reads what the board printed from a transcript, a file that
tmux appends every byte of the pane's output to. It never reads the screen
(the screen is not a log and ``capture-pane`` is not used), never opens a
second connection to the bridge, never truncates the transcript, and never
replaces the pane's process, except for the one transfer that needs the raw
port (see ``Console.released_pane``).

Reading is by byte offset: ``mark()`` is the transcript's size now and
``since(mark)`` every byte after it, taken immediately before the keys that
provoke the output, so that output printed earlier can never satisfy a wait.
The transcript holds raw bytes (lines end CR CR LF, countdowns and percentage
displays use backspaces, the client prints marker lines when the bridge
drops), so a reader decodes it as Latin-1, deletes the carriage returns and
splits on backspaces itself when it needs the fields; it never tries to
rebuild the screen.

A silent console proves nothing, since a loader at its prompt prints nothing
until a key arrives. ``Console.check_usable`` therefore types one empty line
and classifies what comes back before anything else is typed.

Where behaviour is assumed rather than known, the module takes the cautious
side: a console is used only when tmux reports the pane alive and mirrored, a
bridge marker in a waited-for stream aborts the step instead of being waited
out (keys typed while the bridge is down may arrive later, in another state of
the board), and a second connection to the bridge is never opened, not even to
look: a second client either replaces the first or has its input merged onto
the UART.
"""

from __future__ import annotations

import datetime as _datetime
import enum
import fcntl
import os
import re
import subprocess
import time
from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager
from dataclasses import dataclass
from typing import IO, Protocol

# --------------------------------------------------------------------------
# Errors
# --------------------------------------------------------------------------


class BenchError(Exception):
    """Base of the errors the command line turns into an exit status."""

    exit_status = 1


class ConsoleUnusable(BenchError):
    """The console cannot be used: no session, a dead pane, no transcript, no echo."""

    exit_status = 4


class ConsoleBusy(ConsoleUnusable):
    """Another instance of the tool holds the console."""


class ConsoleInterrupted(BenchError):
    """The bridge dropped the connection while a step waited for the board."""

    exit_status = 5


class UnexpectedState(BenchError):
    """The board is in a state the step does not expect."""

    exit_status = 5


class WaitTimeout(BenchError):
    """What a step waited for did not appear in time."""

    exit_status = 6

    def __init__(self, what: str, waited: float, tail: str) -> None:
        super().__init__("timed out after %.1f s waiting for %s; the console shows: %s"
                         % (waited, what, tail or "(nothing)"))
        self.what = what
        self.waited = waited
        self.tail = tail


class ForbiddenTmuxCall(BenchError):
    """A tmux command the tool never runs against a console session."""


class EchoMismatch(BenchError):
    """The echo of a typed line was not the line: nothing was executed."""

    def __init__(self, typed: str, echoed: str) -> None:
        super().__init__("the echo %r is not the typed line %r; the line was cancelled"
                         % (echoed, typed))
        self.typed = typed
        self.echoed = echoed


# --------------------------------------------------------------------------
# Time
# --------------------------------------------------------------------------


class Clock:
    """The clock and the pause that every wait uses, so that tests can replace both."""

    def now(self) -> float:
        raise NotImplementedError

    def sleep(self, seconds: float) -> None:
        raise NotImplementedError


class SystemClock(Clock):
    """Monotonic time and real sleeps."""

    def now(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


# --------------------------------------------------------------------------
# What the console prints
# --------------------------------------------------------------------------

PROMPT = "9601D# "
COUNTDOWN = "Hit any key to stop autoboot:"
INTERRUPT = "<INTERRUPT>"

_MARKER = re.compile(r"--- console: (.+?) at \d\d:\d\d:\d\d ---")
_MAC = re.compile(r"(?<![0-9A-Fa-f:-])[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}(?![0-9A-Fa-f:-])")
_PRELOADER_LINE = re.compile(r"^PRELOADER", re.M)


class BoardState(enum.Enum):
    """What the console shows, as far as the tool can tell."""

    LOADER_PROMPT = "loader prompt"
    COUNTDOWN = "autoboot countdown"
    RESET = "reset in progress"
    KERNEL_STARTING = "kernel starting"
    LOGIN = "login prompt"
    OTHER = "other"


def classify(text: str) -> BoardState:
    """Classify the tail of the console since a mark.

    The loader prompt is a text that ends with it. Otherwise the state is the
    one whose marker is the latest in the text: a first-stage banner line
    (reset in progress), the countdown line when no prompt follows it, the
    kernel's start, or a login prompt.
    """
    if text.endswith(PROMPT):
        return BoardState.LOADER_PROMPT
    found: list[tuple[int, BoardState]] = []
    countdown = text.rfind(COUNTDOWN)
    if countdown >= 0 and PROMPT not in text[countdown:]:
        found.append((countdown, BoardState.COUNTDOWN))
    resets = [m.start() for m in _PRELOADER_LINE.finditer(text)]
    if resets:
        found.append((resets[-1], BoardState.RESET))
    kernel = max(text.rfind("Starting kernel ..."), text.rfind("Linux version"))
    if kernel >= 0:
        found.append((kernel, BoardState.KERNEL_STARTING))
    login = max(text.rfind("login:"), text.rfind("Password:"))
    if login >= 0:
        found.append((login, BoardState.LOGIN))
    return max(found)[1] if found else BoardState.OTHER


def interruption(text: str) -> str | None:
    """The first bridge marker in the text that is not ``connected``, if any."""
    for match in _MARKER.finditer(text):
        if match.group(1) != "connected":
            return match.group(1)
    return None


def redact(text: str) -> str:
    """The text without the loader's ``ethaddr`` line or any MAC address.

    The environment dump holds a MAC address; neither it nor a console tail
    that may hold it is ever printed or logged.
    """
    kept = [line for line in text.split("\n") if not line.lstrip().startswith("ethaddr=")]
    return _MAC.sub("xx:xx:xx:xx:xx:xx", "\n".join(kept))


def display(text: str, limit: int = 600) -> str:
    """Console text made fit to print: redacted, backspaces dropped, the end kept.

    Dropping the backspaces turns a countdown into the numbers it counted;
    it does not rebuild what the screen would have shown.
    """
    return redact(text).replace("\x08", "")[-limit:]


def fields(text: str) -> list[str]:
    """Split a countdown or percentage display on its runs of backspaces."""
    return [part for part in re.split(r"\x08+", text) if part]


# --------------------------------------------------------------------------
# The tool's own log
# --------------------------------------------------------------------------


class ToolLog:
    """A timestamped log of what the tool typed and what it waited for.

    The transcript has no timestamps and the board has no clock; only the
    tool host knows when. The log never holds the environment dump: every
    text goes through ``redact``, and the callers log lines and outcomes, not
    console output.
    """

    def __init__(self, sink: IO[str] | Callable[[str], None] | None = None,
                 wall: Callable[[], _datetime.datetime] | None = None) -> None:
        self._sink = sink
        self._wall = wall or (lambda: _datetime.datetime.now().astimezone())

    def event(self, kind: str, text: str = "") -> None:
        if self._sink is None:
            return
        stamp = self._wall().isoformat(timespec="milliseconds")
        line = ("%s %s %s" % (stamp, kind, redact(text))).rstrip()
        if callable(self._sink):
            self._sink(line)
        else:
            self._sink.write(line + "\n")
            self._sink.flush()


# --------------------------------------------------------------------------
# tmux
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class TmuxResult:
    returncode: int
    stdout: str = ""
    stderr: str = ""


TmuxRunner = Callable[[Sequence[str]], TmuxResult]


def system_tmux(args: Sequence[str]) -> TmuxResult:
    """Run the tmux program on the default server."""
    try:
        proc = subprocess.run(["tmux", *args], capture_output=True, text=True, timeout=15,
                              check=False)
    except FileNotFoundError:
        return TmuxResult(127, "", "tmux is not installed")
    except subprocess.TimeoutExpired:
        return TmuxResult(124, "", "tmux did not answer")
    return TmuxResult(proc.returncode, proc.stdout, proc.stderr)


# The only tmux subcommands the tool runs on a console session.
_ALLOWED_TMUX = frozenset({"has-session", "send-keys", "display-message", "pipe-pane",
                           "respawn-pane", "set-option"})
_KEYS = frozenset({"Enter", "C-c"})
_PLACEHOLDER = "sleep 31536000"


def default_transcript(session: str) -> str:
    """A per-user transcript path named after the session."""
    state = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(state, "bench", session + ".log")


@dataclass
class ConsoleConfig:
    """Where the console is and how it may be used."""

    session: str = "uart-wifi-1"
    transcript: str = ""
    tmux_socket: str = ""  # empty: the default tmux server, where a person's attach finds it
    read_only: bool = False
    poll: float = 0.05

    def __post_init__(self) -> None:
        if not self.transcript:
            self.transcript = default_transcript(self.session)


class ConsoleLike(Protocol):
    """What the tool needs of a console; a raw port has a second implementation."""

    poll: float

    def mark(self) -> int: ...

    def since(self, mark: int) -> str: ...

    def type_text(self, text: str) -> None: ...

    def press(self, key: str, count: int = 1) -> None: ...

    def alive(self) -> bool: ...


@dataclass(frozen=True)
class ConsoleCheck:
    """The result of ``check_usable``: what the board shows after an empty line."""

    state: BoardState
    text: str  # everything printed since the empty line
    mark: int  # the transcript offset the empty line was typed at


class Console:
    """The console of one board, through its tmux session and its transcript."""

    def __init__(self, config: ConsoleConfig, tmux: TmuxRunner = system_tmux,
                 clock: Clock | None = None, log: ToolLog | None = None) -> None:
        self.config = config
        self._tmux_runner = tmux
        self.clock = clock or SystemClock()
        self.log = log or ToolLog()
        self.poll = config.poll

    # -- tmux ------------------------------------------------------------------

    def _tmux(self, *args: str) -> TmuxResult:
        """Run an allowed tmux command on this console's session."""
        subcommand = args[0]
        if subcommand not in _ALLOWED_TMUX:
            raise ForbiddenTmuxCall("tmux %s is never run against a console session" % subcommand)
        if subcommand == "pipe-pane" and "-o" in args:
            raise ForbiddenTmuxCall("pipe-pane -o toggles the pipe; the tool never uses it")
        prefix = ["-L", self.config.tmux_socket] if self.config.tmux_socket else []
        return self._tmux_runner([*prefix, *args])

    def _target(self) -> list[str]:
        return ["-t", self.config.session]

    def has_session(self) -> bool:
        return self._tmux("has-session", *self._target()).returncode == 0

    def _display(self, fmt: str) -> str:
        result = self._tmux("display-message", "-p", *self._target(), fmt)
        if result.returncode != 0:
            raise ConsoleUnusable("tmux cannot describe the pane of session %s: %s"
                                  % (self.config.session, result.stderr.strip()))
        return result.stdout.rstrip("\n")

    def pane_flags(self) -> tuple[bool, bool]:
        """Whether the pane's process has exited, and whether its output is mirrored."""
        words = self._display("#{pane_dead} #{pane_pipe}").split()
        if len(words) != 2:
            raise ConsoleUnusable("unexpected pane description %r" % " ".join(words))
        return words[0] == "1", words[1] == "1"

    # -- the transcript --------------------------------------------------------

    def mark(self) -> int:
        """The transcript's size now."""
        try:
            return os.stat(self.config.transcript).st_size
        except OSError as exc:
            raise ConsoleUnusable("the transcript %s cannot be read: %s"
                                  % (self.config.transcript, exc.strerror)) from exc

    def since(self, mark: int) -> str:
        """Everything printed after the mark, decoded as Latin-1 without carriage returns."""
        try:
            with open(self.config.transcript, "rb") as handle:
                handle.seek(0, os.SEEK_END)
                if handle.tell() < mark:
                    raise ConsoleUnusable("the transcript is shorter than it was: it was "
                                          "truncated or rotated while the tool used it")
                handle.seek(mark)
                data = handle.read()
        except OSError as exc:
            raise ConsoleUnusable("the transcript %s cannot be read: %s"
                                  % (self.config.transcript, exc.strerror)) from exc
        return data.decode("latin-1").replace("\r", "")

    # -- typing ----------------------------------------------------------------

    def type_text(self, text: str) -> None:
        """Type literal text, without the end of the line.

        The text goes through ``send-keys -l --``: literal, and a leading dash
        is text. tmux reads a trailing semicolon as a command separator and
        would drop it, so such a text is refused; so is anything that is not
        printable ASCII.
        """
        if not text:
            return
        if text.endswith(";"):
            raise ConsoleUnusable("tmux would swallow the trailing semicolon of %r" % text)
        if any(not 0x20 <= ord(ch) < 0x7F for ch in text):
            raise ConsoleUnusable("only printable ASCII may be typed")
        self._send("-l", "--", text)

    def press(self, key: str, count: int = 1) -> None:
        """Press Enter or Ctrl-C ``count`` times, in one tmux call."""
        if key not in _KEYS:
            raise ConsoleUnusable("the tool presses only Enter and Ctrl-C, not %r" % key)
        if count < 1:
            return
        self._send(*([key] * count))

    def _send(self, *keys: str) -> None:
        result = self._tmux("send-keys", *self._target(), *keys)
        if result.returncode != 0:
            raise ConsoleUnusable(
                "tmux could not type into session %s (%s). If the pane's client has "
                "exited, a person can restart it with respawn-pane -k -t %s and "
                "set-option -t %s remain-on-exit on"
                % (self.config.session, result.stderr.strip() or "no reason given",
                   self.config.session, self.config.session))

    def alive(self) -> bool:
        """Is the session there and its pane's process running?"""
        if not self.has_session():
            return False
        dead, _pipe = self.pane_flags()
        return not dead

    # -- exclusive use ---------------------------------------------------------

    @contextmanager
    def exclusive(self, who: str) -> Iterator[None]:
        """Hold the console for one operation; a second operation fails at once.

        The lock is an exclusive ``flock`` on ``<transcript>.lock``. A second
        holder fails naming the first instead of interleaving keystrokes with
        it, which would put stray characters in the board's input.
        """
        path = self.config.transcript + ".lock"
        try:
            fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
        except OSError as exc:
            raise ConsoleUnusable("the lock file %s cannot be opened: %s"
                                  % (path, exc.strerror)) from exc
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                holder = os.pread(fd, 200, 0).decode("utf-8", "replace").strip()
                raise ConsoleBusy("the console is in use by %s" % (holder or "another operation")) from None
            os.ftruncate(fd, 0)
            os.pwrite(fd, ("pid %d: %s" % (os.getpid(), who)).encode("utf-8"), 0)
            yield
        finally:
            os.close(fd)

    # -- checking --------------------------------------------------------------

    def check_usable(self, settle: float = 0.2) -> ConsoleCheck:
        """Decide that the console is usable, and say what the board shows.

        The session must exist, its pane must be alive and mirrored into the
        transcript, the transcript must be readable, and one empty line must
        draw at least one byte within two seconds. What came back is then
        classified. Only a loader prompt means a command may be typed; in any
        other state at most an empty line is sent.
        """
        if not self.has_session():
            raise ConsoleUnusable("there is no tmux session %s on the default tmux server"
                                  % self.config.session)
        dead, piped = self.pane_flags()
        if dead:
            raise ConsoleUnusable("the pane of session %s has no running client "
                                  "(pane_dead is 1)" % self.config.session)
        if not piped:
            raise ConsoleUnusable("the pane of session %s is not mirrored into the "
                                  "transcript (pane_pipe is 0)" % self.config.session)
        mark = self.mark()
        self.press("Enter")
        self.log.event("probe", "empty line")
        try:
            wait(self, self.clock, mark, [lambda _text: "byte" if self.mark() > mark else None],
                 2.0, what="any byte after an empty line")
        except WaitTimeout as exc:
            raise ConsoleUnusable("the console is silent: no byte within 2 s of an empty "
                                  "line; the bridge or the board may be down") from exc
        wait_quiet(self, self.clock, settle, 5.0)
        text = self.since(mark)
        state = classify(text)
        self.log.event("probe", "state %s" % state.value)
        return ConsoleCheck(state, text, mark)

    def tail(self, limit: int = 600) -> str:
        """The last characters of the console, safe to print."""
        return display(self.since(max(self.mark() - 4096, 0)), limit)

    # -- giving the pane's client up for one raw transfer ----------------------

    @contextmanager
    def released_pane(self) -> Iterator[str]:
        """Stop the pane's client so that one raw connection can own the UART.

        The pane's process is replaced by an idle placeholder after its start
        command is read; on leaving, the original command is started again and
        the pane keeps its remain-on-exit setting. The transcript holds nothing
        printed in between. Yields the start command that is restored.
        """
        start = self._display("#{pane_start_command}")
        if not start:
            raise ConsoleUnusable("tmux reports no start command for the pane; "
                                  "the client cannot be restored")
        self.log.event("release", "pane client stopped")
        self._must("respawn-pane", "-k", *self._target(), _PLACEHOLDER)
        try:
            yield start
        finally:
            self._must("respawn-pane", "-k", *self._target(), start)
            self._must("set-option", *self._target(), "remain-on-exit", "on")
            if not self.pane_flags()[1]:
                self._must("pipe-pane", *self._target(), "cat >> " + self.config.transcript)
            self.log.event("release", "pane client restored")

    def _must(self, *args: str) -> None:
        result = self._tmux(*args)
        if result.returncode != 0:
            raise ConsoleUnusable("tmux %s failed: %s" % (args[0], result.stderr.strip()))


# --------------------------------------------------------------------------
# Waiting
# --------------------------------------------------------------------------

Matcher = Callable[[str], "str | None"]


def pattern(label: str, regex: str | re.Pattern[str], flags: int = 0) -> Matcher:
    """A matcher that answers ``label`` when the regular expression is found."""
    compiled = re.compile(regex, flags) if isinstance(regex, str) else regex
    return lambda text: label if compiled.search(text) else None


@dataclass(frozen=True)
class Seen:
    """What a wait found: which matcher answered, and the text since the mark."""

    label: str
    text: str
    elapsed: float


def wait(console: ConsoleLike, clock: Clock, mark: int, matchers: Sequence[Matcher],
         timeout: float, *, idle: float | None = None, what: str = "") -> Seen:
    """Wait until a matcher answers on the text since ``mark``.

    The matchers are tried in order. The wait ends with ``WaitTimeout`` after
    ``timeout`` seconds, or after ``idle`` seconds without a new character
    (a stalled command). A bridge marker that is not ``connected`` ends it with
    ``ConsoleInterrupted``: the stream was cut, so what the board did is
    unknown.
    """
    started = clock.now()
    deadline = started + timeout
    last_size = -1
    last_growth = started
    while True:
        text = console.since(mark)
        cut = interruption(text)
        if cut is not None:
            raise ConsoleInterrupted("the console connection broke (%s) while waiting for %s"
                                     % (cut, what or "the board"))
        for matcher in matchers:
            label = matcher(text)
            if label is not None:
                return Seen(label, text, clock.now() - started)
        now = clock.now()
        if len(text) != last_size:
            last_size, last_growth = len(text), now
        if idle is not None and now - last_growth >= idle:
            raise WaitTimeout(what or "output", now - started, display(text))
        if now >= deadline:
            raise WaitTimeout(what or "output", now - started, display(text))
        clock.sleep(min(console.poll, max(deadline - now, 0.0)) if console.poll else 0.0)


def wait_quiet(console: ConsoleLike, clock: Clock, quiet: float, timeout: float) -> None:
    """Wait until nothing has been printed for ``quiet`` seconds."""
    started = clock.now()
    last_mark = console.mark()
    last_change = started
    while True:
        now = clock.now()
        current = console.mark()
        if current != last_mark:
            last_mark, last_change = current, now
        if now - last_change >= quiet:
            return
        if now - started >= timeout:
            raise WaitTimeout("the console to go quiet", now - started,
                              display(console.since(max(current - 300, 0))))
        clock.sleep(console.poll or 0.01)


def type_line(console: ConsoleLike, clock: Clock, text: str, *,
              echo_timeout: float = 2.0) -> int:
    """Type a line with the echo check; return the mark that output is read from.

    The line goes in without its Enter and the echo is compared with the text;
    only an equal echo is followed by Enter. A dropped, doubled or substituted
    character would otherwise be executed as a different command, so on a
    difference Ctrl-C cancels the line and ``EchoMismatch`` is raised.
    """
    if text:
        mark = console.mark()
        console.type_text(text)
        deadline = clock.now() + echo_timeout
        while True:
            echoed = console.since(mark)
            if len(echoed) >= len(text) or clock.now() >= deadline:
                break
            clock.sleep(console.poll or 0.01)
        cut = interruption(echoed)
        if cut is not None:
            raise ConsoleInterrupted("the console connection broke (%s) while typing" % cut)
        if echoed != text:
            console.press("C-c")
            raise EchoMismatch(text, echoed)
    after = console.mark()
    console.press("Enter")
    return after
