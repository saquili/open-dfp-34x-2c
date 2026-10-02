# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The boot loader as the tool drives it.

The loader prints ``9601D# `` (without a newline) as its prompt, echoes every
character it receives, and executes whatever arrives, so this module is
careful in three ways. Nothing is typed unless the guard has approved the
line and the console tail shows the prompt; a line is typed without its
Enter and sent only when the echo equals it; and a countdown is only ever
sent Enter.

A reset costs under two seconds to the countdown, which the loader prints as
``Hit any key to stop autoboot:`` and a number that counts down once a
second. The first byte received stops the countdown and is consumed, so a
command typed into it loses its first character. Break-in therefore sends
Enter at once: a burst, a short pause, then a trickle, and verifies that the
prompt came back. It does not depend on how long a tick is, and a window of
five ticks must work as well as one of six hundred.

The environment is read once at the start of a session, and the guard expands
the variables of a typed line from that copy. It judges the macros that ``run``
and ``boot`` would execute by their text at that moment, not by the copy, so
the loader gives it live reads of single variables (``printenv name``).
Reading the environment never keeps or prints the MAC address that it holds.

This module assumes that the loader aborts nothing on its own while a
command runs and that ``crc32`` reports what a host CRC-32 would; both are
checked by the flashing procedure against the host's value rather than
trusted.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from .console import (COUNTDOWN, INTERRUPT, PROMPT, BenchError, Clock, ConsoleInterrupted,
                      ConsoleLike, ToolLog, UnexpectedState, classify, display, interruption,
                      pattern, shows_prompt, type_line, wait, wait_quiet)
from .guard import Approval, Guard

BURST = 20  # Enters sent with no gap
TRICKLE = 20  # Enters sent 0.2 s apart after a pause of 0.5 s
VERIFY_AFTER = 1.0  # seconds after the last Enter before the prompt is checked
QUIET = 0.3  # seconds without output before the first command of a session

_PRELOADER = re.compile(r"^PRELOADER", re.M)
_ENV_LINE = re.compile(r"^([^\s=]+)=(.*)$")


class LoaderError(BenchError):
    """The loader did not do what a step needed."""


class NotAtPrompt(UnexpectedState):
    """A command was to be typed, but the console does not show the loader prompt."""


class UnexpectedReset(UnexpectedState):
    """The board restarted while a step waited for it."""


class BreakInFailed(UnexpectedState):
    """The prompt could not be reached before the countdown ran out."""


class CommandFailed(LoaderError):
    """The loader answered a line with ``Unknown command``."""


@dataclass(frozen=True)
class CommandResult:
    """What a command printed, and how long it took."""

    line: str
    text: str  # everything after the echo, as printed
    output: str  # the same without the echoed line end and the final prompt
    elapsed: float


def parse_environment(text: str) -> dict[str, str]:
    """The ``name=value`` lines of a ``printenv`` dump, up to its size line.

    A line is split at its first ``=``. The loader's ``ethaddr`` is dropped:
    the dump holds a MAC address, which the tool never keeps.
    """
    env: dict[str, str] = {}
    for line in text.split("\n"):
        if line.startswith("Environment size:"):
            break
        match = _ENV_LINE.match(line.rstrip())
        if match:
            env[match.group(1)] = match.group(2)
    env.pop("ethaddr", None)
    return env


def _output_of(text: str) -> str:
    body = text[:-len(PROMPT)] if text.endswith(PROMPT) else text
    if body.startswith("\n"):
        body = body[1:]
    return body.rstrip("\n")


def _prompt_matcher(text: str) -> str | None:
    return "prompt" if text.endswith(PROMPT) else None


class Loader:
    """The loader prompt of one console, behind the guard."""

    def __init__(self, console: ConsoleLike, guard: Guard, clock: Clock,
                 log: ToolLog | None = None) -> None:
        self.console = console
        self.guard = guard
        self.clock = clock
        self.log = log or ToolLog()
        self.environment: dict[str, str] = {}
        self.typed: list[tuple[str, str]] = []  # (kind, line): "step" or "read"
        self._kind = "step"
        self.use_for_guard()

    def use_for_guard(self) -> None:
        """Let the guard read the board's variables through this loader."""
        self.guard.fetch = self.getenv

    # -- the prompt ------------------------------------------------------------

    def at_prompt(self) -> bool:
        """Does the end of the console show the loader prompt?"""
        return shows_prompt(self.console)

    def _require_prompt(self) -> None:
        if not self.at_prompt():
            state = classify(self.console.since(max(self.console.mark() - 400, 0)))
            raise NotAtPrompt("the console does not show the loader prompt (it shows: %s); "
                              "nothing was typed" % state.value)

    # -- commands --------------------------------------------------------------

    def start(self, line: str) -> tuple[Approval, int]:
        """Type a line, once the guard and the prompt allow it, and press Enter.

        Returns the approval and the mark that the line's output is read from.
        A refusal raises before anything is typed.
        """
        approval = self.guard.check(line)
        self._require_prompt()
        self.log.event("type", approval.text)
        self.typed.append((self._kind, approval.text))
        mark = type_line(self.console, self.clock, approval.text)
        return approval, mark

    def finish(self, line: str, mark: int, *, timeout: float,
               idle: float | None = None) -> CommandResult:
        """Wait for the prompt after a line and collect what it printed."""
        started = self.clock.now()
        seen = wait(self.console, self.clock, mark,
                    [_prompt_matcher, pattern("reset", _PRELOADER)], timeout, idle=idle,
                    what="the loader prompt after %r" % line)
        if seen.label == "reset":
            raise UnexpectedReset("the board restarted while %r ran; the console shows: %s"
                                  % (line, display(seen.text, 300)))
        output = _output_of(seen.text)
        elapsed = self.clock.now() - started
        self.log.event("done", "%s in %.1f s" % (line, elapsed))
        if re.search(r"^Unknown command", output, re.M):
            raise CommandFailed("the loader did not understand %r: %s"
                                % (line, display(output, 200)))
        return CommandResult(line, seen.text, output, elapsed)

    def command(self, line: str, *, timeout: float = 10.0,
                idle: float | None = None) -> CommandResult:
        """Type a line and wait for the prompt that follows its output."""
        approval, mark = self.start(line)
        return self.finish(approval.text, mark, timeout=timeout, idle=idle)

    # -- the environment -------------------------------------------------------

    def read_environment(self) -> dict[str, str]:
        """Read the whole environment once; the guard keeps it for its expansions."""
        result = self.command("printenv", timeout=15.0)
        if "Environment size:" not in result.output:
            raise LoaderError("the environment dump ended without its size line")
        self.environment = parse_environment(result.output)
        self.guard.variables = dict(self.environment)
        return self.environment

    def getenv(self, name: str) -> str | None:
        """The value of one variable as the board has it now, or None if undefined."""
        if name == "ethaddr":
            return None
        outer, self._kind = self._kind, "read"
        try:
            result = self.command("printenv " + name, timeout=5.0)
        finally:
            self._kind = outer
        for line in result.output.split("\n"):
            match = _ENV_LINE.match(line.rstrip())
            if match and match.group(1) == name:
                return match.group(2)
        return None

    def save_environment(self) -> int:
        """Run ``saveenv`` and return the number of the environment copy it wrote."""
        result = self.command("saveenv", timeout=15.0)
        match = re.search(r"Valid environment: (\d)", result.output)
        if match is None:
            raise LoaderError("saveenv did not report a valid environment: %s"
                              % display(result.output, 200))
        return int(match.group(1))

    def set_saved(self, *pairs: tuple[str, str]) -> None:
        """Set variables the guard allows, save the environment once, and read them back."""
        for name, value in pairs:
            self.command("setenv %s %s" % (name, value))
        self.save_environment()
        for name, value in pairs:
            got = self._read_back(name)
            if got != value:
                raise LoaderError("%s reads %r after it was set to %r and saved"
                                  % (name, got, value))
            self.guard.variables[name] = value

    def _read_back(self, name: str) -> str | None:
        """Read a variable as a step of the procedure, not as a read for the guard."""
        result = self.command("printenv " + name, timeout=5.0)
        for line in result.output.split("\n"):
            match = _ENV_LINE.match(line.rstrip())
            if match and match.group(1) == name:
                return match.group(2)
        return None

    def crc32(self, address: int, length: int) -> int:
        """The loader's CRC-32 of memory; it prints nothing until it is done."""
        result = self.command("crc32 %x %x" % (address, length), timeout=30.0)
        match = re.search(r"==> ([0-9A-Fa-f]{8})", result.output)
        if match is None:
            raise LoaderError("crc32 gave no checksum: %s" % display(result.output, 200))
        return int(match.group(1), 16)

    # -- reaching the prompt ---------------------------------------------------

    def wait_for_loader(self, mark: int, timeout: float = 120.0) -> str:
        """After a reset, wait for the countdown or the prompt, whichever is first.

        Only what follows the last first-stage banner counts, so a second
        reset restarts the wait. Returns ``"countdown"`` or ``"prompt"``; a
        kernel that started on its own raises ``UnexpectedState``.
        """
        def matcher(text: str) -> str | None:
            banners = [m.start() for m in _PRELOADER.finditer(text)]
            tail = text[banners[-1]:] if banners else text
            if "Starting kernel" in tail:
                return "kernel"
            if COUNTDOWN in tail and PROMPT not in tail[tail.rfind(COUNTDOWN):]:
                return "countdown"
            if tail.endswith(PROMPT):
                return "prompt"
            return None

        seen = wait(self.console, self.clock, mark, [matcher], timeout,
                    what="the loader's countdown or prompt after a reset")
        if seen.label == "kernel":
            raise UnexpectedState("the board's autoboot started a kernel before the tool "
                                  "could break in")
        return seen.label

    def _breakin_state(self, mark: int) -> str:
        text = self.console.since(mark)
        cut = interruption(text)
        if cut is not None:
            raise ConsoleInterrupted("the console connection broke (%s) during break-in" % cut)
        if "Starting kernel" in text or _PRELOADER.search(text):
            return "gone"
        if text.endswith(PROMPT):
            return "landed"
        return "waiting"

    def break_in(self, rounds: int = 3) -> None:
        """Stop the countdown and verify that the loader prompt is up.

        A round is a burst of Enters with no gap, a pause of half a second and
        a trickle of more at 0.2 s; the first byte stops the countdown and the
        rest print empty prompts. A burst that has landed ends the round early.
        One second after the last Enter the console must end with the prompt.
        After the last round that fails, ``BreakInFailed`` reports the console.
        """
        last = ""
        first = self.console.mark()
        for number in range(1, rounds + 1):
            mark = self.console.mark()
            self.log.event("break-in", "round %d" % number)
            self.console.press("Enter", count=BURST)
            self.clock.sleep(0.5)
            state = self._breakin_state(mark)
            for _ in range(TRICKLE):
                if state != "waiting":
                    break
                self.console.press("Enter")
                self.clock.sleep(0.2)
                state = self._breakin_state(mark)
            if state == "gone":
                raise BreakInFailed("the board restarted or booted during break-in; the "
                                    "console shows: %s" % display(self.console.since(mark), 300))
            self.clock.sleep(VERIFY_AFTER)
            last = self.console.since(mark)
            if last.endswith(PROMPT):
                return
        raise BreakInFailed("no loader prompt after %d break-in rounds; the console is in "
                            "state '%s' and shows: %s"
                            % (rounds,
                               classify(self.console.since(max(first - 800, 0))).value,
                               display(last, 300)))

    def resync(self) -> None:
        """Ask for a fresh prompt, after the console was used by another channel.

        After a raw transfer the shared console has not seen the prompt that
        the board printed meanwhile; an Enter makes it print another.
        """
        mark = self.console.mark()
        self.console.press("Enter")
        wait(self.console, self.clock, mark, [_prompt_matcher], 5.0,
             what="a loader prompt after the raw connection")

    def settle(self) -> None:
        """Clear the line before the first command of a session.

        Enters that are still printing prompts would interleave with a command,
        so the console is first left to go quiet; Ctrl-C then clears a line a
        person left half typed, and the loader must answer with a fresh prompt.
        """
        wait_quiet(self.console, self.clock, QUIET, 15.0)
        mark = self.console.mark()
        self.console.press("C-c")
        wait(self.console, self.clock, mark,
             [lambda text: "fresh" if INTERRUPT in text and text.endswith(PROMPT) else None],
             3.0, what="a fresh prompt after Ctrl-C")

    def begin_session(self) -> dict[str, str]:
        """Clean the line, then read the environment for the guard."""
        self.settle()
        return self.read_environment()
