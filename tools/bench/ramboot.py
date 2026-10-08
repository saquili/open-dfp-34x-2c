# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""RAM boot: start a trial kernel from RAM, over TFTP, writing nothing to flash.

A kernel that fails costs one reset. The sequence is: check the image on the
tool host; take the console and check that it is usable; make the image, the
server and the firewall allowance ready on the image host; bring the board to
the loader prompt (restarting it from a running kernel if need be); read its
environment and check that the macros and addresses are there and agree with
the configuration; type ``run ramboot`` and watch the transfer; compare what
the loader prints about the image with the file; and wait for the kernel's
banner and then for the image's own ready marker.

The board is restarted from a running kernel with SysRq ``b`` over ssh, which
restarts the board at once. A current kernel restarts by a chip reset, which
leaves no count running. An older kernel's restart, through the CPU reset
vector, leaves the watchdog counting, and about 47 seconds later it resets the
chip unless a kernel clears it first. After such a restart the first transfer
sometimes shows only ``T`` marks (one per timeout) and never recovers, so an
attempt is declared failed when two ``T`` marks follow ``Loading: *`` with no
``#``. Ctrl-C is pressed, and the default way out is to wait for that watchdog
reset and break in again; typing ``reset`` at the prompt is expected to do the
same sooner but is not established, so it is only used when asked for, or when
the watchdog reset does not come. One recovery is allowed per run; a second
failure ends it with the console's tail. The restart call over ssh is not
waited for to the end: the board dies under the session, so the tool watches
the console for the first-stage banner and ends the client after at most ten
seconds.

Success is judged only from output after the step that provokes it, never
from the ready marker alone: the loader's report of the image (name, creation
time, size, addresses) must be that of the file that was sent.
"""

from __future__ import annotations

import os
import re
from collections.abc import Callable
from contextlib import nullcontext
from dataclasses import dataclass, field

from . import image
from .console import (PROMPT, BenchError, BoardState, Clock, ConsoleInterrupted, ConsoleLike,
                      ToolLog, UnexpectedState, WaitTimeout, display, interruption, pattern,
                      wait, wait_quiet)
from .guard import Guard
from .host import SYSRQ_RESTART, BoardAccess, ImageHost, Runner, board_command
from .uboot import BreakInFailed, Loader

SERVER_SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tftp.py")
KERNEL_ADDRESS = 0x81000000
_PRELOADER = re.compile(r"(?m)^PRELOADER")
_DONE = re.compile(r"Bytes transferred = (\d+) \(([0-9a-fA-F]+) hex\)")
_TFTP_ERROR = re.compile(r"TFTP error: '([^']*)' \((\d+)\)")
# The loader backs over its "*" before the first mark.
_LOADING = re.compile(r"Loading: \*([#T \r\n\x08]*)")
_STARTED = re.compile(r"Starting kernel \.\.\.")
_BANNER = re.compile(r"Linux version")
_BOOTM_FAILED = re.compile(r"ERROR: can't get kernel image|Wrong Image Format|Bad Data CRC"
                           r"|Bad Magic Number|Bad Header Checksum")
REQUIRED_VARIABLES = ("ramboot", "sdsup", "ipaddr", "serverip", "netmask")


class RamBootError(BenchError):
    """The RAM boot did not succeed."""


class MissingVariable(UnexpectedState):
    """The loader lacks a variable that a step needs; a person sets it once."""


class WrongKernel(UnexpectedState):
    """The loader started something other than the image that was sent."""


class AttemptFailed(Exception):
    """An attempt to load the image failed in a way a restart can cure."""

    def __init__(self, why: str, reset_seen: bool) -> None:
        super().__init__(why)
        self.why = why
        self.reset_seen = reset_seen


@dataclass
class RamBootConfig:
    """What the run needs to know about the board and the image host."""

    image_name: str = "uImage"  # the name served to the loader
    expect_ipaddr: str = "192.0.2.2"
    expect_serverip: str = "192.0.2.1"
    expect_netmask: str = "255.255.255.252"
    ready_pattern: str = "login:"
    ready_limit: float = 60.0  # from the typed ``run ramboot``
    recovery: str = "fuse"  # "fuse": wait for the watchdog reset; "reset": type reset
    recoveries: int = 1
    fuse_wait: float = 60.0  # for the watchdog reset after a failed attempt, before ``reset``
    loader_wait: float = 120.0  # for the countdown or prompt after a restart
    transfer_limit: float = 30.0
    stall_marks: int = 2  # T marks without a # that fail an attempt
    ssh_limit: float = 10.0  # for the restart call
    person_wait: float = 600.0  # for a person's power cycle


@dataclass
class Context:
    """The pieces a run is made of."""

    console: ConsoleLike
    loader: Loader
    guard: Guard
    clock: Clock
    runner: Runner
    access: BoardAccess
    host: ImageHost | None = None
    log: ToolLog = field(default_factory=ToolLog)
    say: Callable[[str], None] = lambda text: None


@dataclass
class RamBootResult:
    """What a successful run reports."""

    image: image.UImage
    transferred: int
    attempts: int
    elapsed: float
    notes: list[str] = field(default_factory=list)


# --------------------------------------------------------------------------
# Environment
# --------------------------------------------------------------------------

def check_environment(env: dict[str, str], required: tuple[str, ...],
                      expect: dict[str, str]) -> None:
    """Stop, naming the variable, if one is missing or the addresses disagree.

    A fresh board needs a person to set the macros and the addresses once; the
    tool never does that. A wrong server address would otherwise fail as a hang
    or a run of ``T`` marks, far from the cause.
    """
    for name in required:
        if name not in env:
            raise MissingVariable("the loader has no variable %r; a person must set it once "
                                  "(see the page on flashing in the handbook)" % name)
    for name, wanted in expect.items():
        if env.get(name) != wanted:
            raise UnexpectedState("the loader's %s is %r but the configuration says %r; "
                                  "nothing dependent on it was typed"
                                  % (name, env.get(name), wanted))


def expected_addresses(config: RamBootConfig) -> dict[str, str]:
    return {"ipaddr": config.expect_ipaddr, "serverip": config.expect_serverip,
            "netmask": config.expect_netmask}


# --------------------------------------------------------------------------
# Getting to the loader prompt
# --------------------------------------------------------------------------

def request_restart(ctx: Context, mark: int, config: RamBootConfig) -> bool:
    """Restart a running board through SysRq over ssh; True once the banner appears.

    The ssh session never ends from the far end (the board dies under it), so
    the tool starts it, watches the console for the first-stage banner, and
    ends the client. The control address is tried first: it answers whether or
    not the board's daemon has bridged the subscriber port.
    """
    for address in ctx.access.addresses():
        ctx.log.event("restart", "sysrq over ssh to %s" % address)
        process = ctx.runner.spawn(board_command(ctx.access, address, SYSRQ_RESTART))
        try:
            deadline = ctx.clock.now() + config.ssh_limit
            while ctx.clock.now() < deadline:
                if _PRELOADER.search(ctx.console.since(mark)):
                    return True
                if process.poll() is not None:
                    break
                ctx.clock.sleep(0.1)
            if _PRELOADER.search(ctx.console.since(mark)):
                return True
        finally:
            process.terminate()
    return False


def catch_power_cycle(ctx: Context, mark: int, config: RamBootConfig) -> None:
    """Break in after a person has power-cycled the board.

    The tool watches for the first-stage banner and from then sends Enter every
    100 ms until the loader prompt is verified, or 30 seconds have passed.
    """
    wait(ctx.console, ctx.clock, mark, [pattern("banner", _PRELOADER)], config.person_wait,
         what="the board to be power-cycled")
    ctx.say("the board restarted; breaking in")
    started = ctx.clock.now()
    while ctx.clock.now() - started < 30.0:
        text = ctx.console.since(mark)
        cut = interruption(text)
        if cut is not None:
            raise ConsoleInterrupted("the console connection broke (%s) during break-in" % cut)
        banner = [m.start() for m in _PRELOADER.finditer(text)][-1]
        if text.endswith(PROMPT) and text.rfind("Hit any key", banner) >= 0:
            return
        if "Starting kernel" in text[banner:]:
            raise BreakInFailed("the countdown ran out before the tool could break in")
        ctx.console.press("Enter")
        ctx.clock.sleep(0.1)
    raise BreakInFailed("no loader prompt 30 s after the board restarted; the console shows: %s"
                        % display(ctx.console.since(mark), 300))


def check_ready_pattern(pattern_text: str) -> None:
    """Refuse a ready marker that is not a regular expression, before anything is typed."""
    try:
        re.compile(pattern_text)
    except re.error as exc:
        raise RamBootError("the ready marker %r is not a regular expression: %s"
                           % (pattern_text, exc)) from exc


def reach_loader_prompt(ctx: Context, config: RamBootConfig) -> str:
    """Bring the board to a verified loader prompt; say how.

    A kernel that is still starting cannot be restarted over ssh yet, so it is
    given time to settle (a quiet console) and the console is checked again.
    """
    check = ctx.console.check_usable()
    if check.state is BoardState.KERNEL_STARTING:
        ctx.say("a kernel is still starting; waiting for the console to go quiet")
        wait_quiet(ctx.console, ctx.clock, 3.0, 90.0)
        check = ctx.console.check_usable()
    state, mark = check.state, check.mark
    if state is BoardState.LOADER_PROMPT:
        return "already at the loader prompt"
    if state is BoardState.COUNTDOWN:
        ctx.loader.break_in()
        return "broke into the countdown"
    if state is not BoardState.RESET:
        mark = ctx.console.mark()
        ctx.say("the board is running (%s); restarting it" % state.value)
        if not request_restart(ctx, mark, config):
            ctx.say("ssh cannot reach the board. Power-cycle it now; the tool will break in "
                    "when it restarts.")
            catch_power_cycle(ctx, mark, config)
            return "broke in after a power cycle"
    ctx.loader.wait_for_loader(mark, config.loader_wait)
    ctx.loader.break_in()
    return "restarted and broke in"


# --------------------------------------------------------------------------
# One attempt
# --------------------------------------------------------------------------

def _stalled(count: int) -> Callable[[str], str | None]:
    def match(text: str) -> str | None:
        found = _LOADING.search(text)
        if found and "#" not in found.group(1) and found.group(1).count("T") >= count:
            return "stalled"
        return None
    return match


def watch_transfer(ctx: Context, mark: int, size: int, config: RamBootConfig,
                   what: str = "the TFTP transfer") -> int:
    """Wait for the loader's ``Bytes transferred`` line; return the byte count.

    Raises ``AttemptFailed`` for a stall (two ``T`` marks and no ``#``) or a
    reset the tool did not ask for, and ``RamBootError`` for a refusal by the
    server or a transfer of the wrong size.
    """
    seen = wait(ctx.console, ctx.clock, mark,
                [pattern("done", _DONE), pattern("error", _TFTP_ERROR),
                 pattern("unknown", r"Unknown command '"), _stalled(config.stall_marks),
                 pattern("reset", _PRELOADER)], config.transfer_limit, what=what)
    if seen.label == "done":
        count = int(_DONE.search(seen.text).group(1))  # type: ignore[union-attr]
        if count != size:
            raise RamBootError("the loader received %d bytes, the file has %d" % (count, size))
        return count
    if seen.label == "error":
        found = _TFTP_ERROR.search(seen.text)
        raise RamBootError("the TFTP server answered %r (%s); check that the file is in the "
                           "served directory of the image host"
                           % (found.group(1), found.group(2)))  # type: ignore[union-attr]
    if seen.label == "unknown":
        raise RamBootError("the loader did not understand a typed line: %s"
                           % display(seen.text, 300))
    if seen.label == "reset":
        raise AttemptFailed("the board reset during the transfer", reset_seen=True)
    raise AttemptFailed("the transfer showed only T marks (nothing arrived)", reset_seen=False)


def check_started_image(ctx: Context, mark: int, img: image.UImage | None,
                        address: int) -> None:
    """Wait for ``Starting kernel ...`` and compare the loader's report with the file."""
    seen = wait(ctx.console, ctx.clock, mark,
                [pattern("started", _STARTED), pattern("failed", _BOOTM_FAILED)], 20.0,
                what="the loader to start the kernel")
    if seen.label == "failed":
        raise RamBootError("the loader could not start the image: %s" % display(seen.text, 300))
    problems = image.compare_boot_report(image.parse_boot_report(seen.text), img, address)
    if problems:
        raise WrongKernel("the loader did not start the image that was sent: %s"
                          % "; ".join(problems))


def wait_for_kernel(ctx: Context, mark: int, deadline: float, ready: str) -> float:
    """Wait for the kernel's banner and then for the image's ready marker.

    ``deadline`` is an absolute time on the tool's clock. The marker is looked
    for only after the banner. Returns the seconds the wait took.
    """
    started = ctx.clock.now()
    banner = pattern("banner", _BANNER)
    ready_re = re.compile(ready)

    def after_banner(text: str) -> str | None:
        found = _BANNER.search(text)
        return "ready" if found and ready_re.search(text, found.end()) else None

    remaining = max(deadline - ctx.clock.now(), 0.0)
    try:
        wait(ctx.console, ctx.clock, mark, [banner], remaining, what="the kernel's banner")
    except WaitTimeout as exc:
        raise RamBootError("nothing was printed after 'Starting kernel ...': the kernel "
                           "stopped before its console came up; the console shows: %s"
                           % exc.tail) from exc
    remaining = max(deadline - ctx.clock.now(), 0.0)
    try:
        wait(ctx.console, ctx.clock, mark, [after_banner], remaining,
             what="the image's ready marker %r" % ready)
    except WaitTimeout as exc:
        raise RamBootError("the kernel started but the ready marker %r did not appear in time; "
                           "the console shows: %s" % (ready, exc.tail)) from exc
    return ctx.clock.now() - started


def _reset_from_the_prompt(ctx: Context, mark: int) -> bool:
    """Type ``reset`` at the loader prompt and wait for the first-stage banner.

    False if the prompt is not there, in which case nothing was typed.
    """
    try:
        wait(ctx.console, ctx.clock, mark,
             [lambda text: "prompt" if text.endswith(PROMPT) else None], 15.0,
             what="the loader prompt after Ctrl-C")
    except WaitTimeout:
        return False
    ctx.say("Ctrl-C stopped the transfer; resetting from the loader prompt")
    _approval, reset_mark = ctx.loader.start("reset")
    wait(ctx.console, ctx.clock, reset_mark, [pattern("banner", _PRELOADER)], 10.0,
         what="the board to restart")
    return True


def recover(ctx: Context, mark: int, failure: AttemptFailed, config: RamBootConfig) -> None:
    """After a failed attempt, bring about a reset; the caller then breaks in again.

    Ctrl-C is pressed first. The default recovery waits for the watchdog reset
    the failing restart caused, and types ``reset`` at the loader prompt only
    if that reset does not come within ``fuse_wait``. ``reset`` first is the
    faster way that is only expected to work, so it is used first when asked
    for, and the watchdog reset is waited for if it does not work.
    """
    if failure.reset_seen:
        return
    ctx.console.press("C-c")
    if config.recovery == "reset":
        try:
            if _reset_from_the_prompt(ctx, mark):
                return
        except WaitTimeout:
            pass
        ctx.say("the prompt did not come back; waiting for the watchdog reset instead")
    ctx.say("waiting for the watchdog reset")
    try:
        wait(ctx.console, ctx.clock, mark, [pattern("banner", _PRELOADER)], config.fuse_wait,
             what="the watchdog reset after a failed transfer")
    except WaitTimeout:
        if config.recovery == "reset":
            raise
        ctx.say("the watchdog reset did not come; resetting from the loader prompt")
        if not _reset_from_the_prompt(ctx, mark):
            raise


# --------------------------------------------------------------------------
# The run
# --------------------------------------------------------------------------

def console_lock(console: ConsoleLike, who: str):
    """The console's exclusive lock, when the console has one."""
    lock = getattr(console, "exclusive", None)
    return lock(who) if lock is not None else nullcontext()


def read_cmdline(ctx: Context) -> str | None:
    """The running kernel's command line over ssh, or None if the board cannot be reached."""
    for address in ctx.access.addresses():
        result = ctx.runner.run(board_command(ctx.access, address, "cat /proc/cmdline"),
                                timeout=15.0)
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    return None


def ramboot(ctx: Context, image_path: str, config: RamBootConfig,
            server_source: str = SERVER_SOURCE) -> RamBootResult:
    """Boot the image from RAM; return what was seen, or raise why not."""
    started = ctx.clock.now()
    check_ready_pattern(config.ready_pattern)
    img = image.check_uimage(image.read_file(image_path), image.RAM_BOOT_LIMIT,
                             what=os.path.basename(image_path))
    ctx.say("image %s: %d bytes, name %r" % (os.path.basename(image_path), img.length, img.name))
    notes: list[str] = []
    attempts = 0
    interrupted = False
    resume_mark: int | None = None
    with console_lock(ctx.console, "ramboot"):
        if ctx.host is not None:
            notes += ctx.host.prepare(image_path, config.image_name, server_source)
            ctx.say("image host ready: %s served, server listening" % config.image_name)
        while True:
            try:
                if resume_mark is None:
                    ctx.say(reach_loader_prompt(ctx, config))
                else:  # the board reset on its own: wait for it and break in again
                    ctx.loader.wait_for_loader(resume_mark, config.loader_wait)
                    ctx.loader.break_in()
                    resume_mark = None
                env = ctx.loader.begin_session()
                check_environment(env, REQUIRED_VARIABLES, expected_addresses(config))
                attempts += 1
                begun = ctx.clock.now()
                _approval, mark = ctx.loader.start("run ramboot")
                ctx.log.event("ramboot", "attempt %d" % attempts)
                try:
                    count = watch_transfer(ctx, mark, img.length, config)
                    ctx.say("transferred %d bytes" % count)
                    check_started_image(ctx, mark, img, KERNEL_ADDRESS)
                    ctx.say("the loader started %r" % img.name)
                    wait_for_kernel(ctx, mark, begun + config.ready_limit, config.ready_pattern)
                except AttemptFailed as failure:
                    if attempts > config.recoveries:
                        raise RamBootError("%s again after a recovery; the console shows: %s"
                                           % (failure.why, display(ctx.console.since(mark), 400))
                                           ) from failure
                    ctx.say("attempt %d failed: %s; recovering" % (attempts, failure.why))
                    recover(ctx, mark, failure, config)
                    resume_mark = mark
                    continue
                return RamBootResult(img, count, attempts, ctx.clock.now() - started, notes)
            except ConsoleInterrupted:
                if interrupted:
                    raise
                interrupted = True  # nothing was written: start again from the console check
                resume_mark = None
                ctx.say("the console connection broke; waiting for the board to settle, then "
                        "checking the console and starting again")
                wait_quiet(ctx.console, ctx.clock, 2.0, 180.0)
