# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Writing bank 0: the kernel or the root filesystem, and the one-shot trial.

Only the two partitions of bank 0 are ever written, each by an image made for
it: a kernel image to ``k0`` and a SquashFS image to ``r0``. Bank 1 holds the
shipped firmware, the fallback, and is never touched; the tool records its
checksums before it writes and fails if they differ afterwards. Every line is
typed by the tool itself, with literal numbers from its own table, after the
guard has approved it; the board's own flashing macros are never run.

The procedure is the same for both partitions. The file is checked on the tool
host; the console is checked and the loader reached; the environment is read
and the layout variables must equal the tool's table, and no trial may be
pending; the image reaches RAM at 0x80000000 by TFTP (the default) or by
YMODEM, and its CRC-32 there must equal the host's; the partition is erased
and written, and the flash itself, read through the loader's window, must give
the host's CRC-32 again. Only then, and only when asked, is a one-shot trial
armed and the board booted to watch it; the committed bank is changed only on
an explicit request.

The watchdog that an older kernel's restart, through the CPU reset vector,
leaves counting would reset the chip in the middle of a transfer or a write, so
its control register is cleared: before the transfer for YMODEM, and for TFTP
after the RAM copy has been checked and before the erase, because the reset
heals a first TFTP attempt that fails after a restart. A write must not be
interrupted: nothing is typed while ``loady``, ``sf erase`` or ``sf write``
runs, and a signal that arrives then is held until the command has finished and
the prompt is back. If the console stalls in the middle of a write, the tool
stops, says that the partition is invalid, and arms nothing.

A one-shot trial is a good test only while the committed bank is another one:
when the committed bank is the one under test, a failed trial is followed by
the same bank. The tool therefore says which bank the fallback is, and arms in
that case only when told so explicitly.

The trial is started by typing ``boot``, which runs the same boot command as
the end of the countdown does, rather than by waiting for a countdown of
unknown length to run out; the guard checks the boot chain first.

YMODEM through a bridge needs the pane's client stopped and a second
connection to the bridge, which has not been validated; until it has, YMODEM is
for a direct serial port, and the bridge needs an explicit opt-in.
"""

from __future__ import annotations

import re
import zlib
from collections.abc import Callable, Iterator
from contextlib import AbstractContextManager, contextmanager
from dataclasses import dataclass, field

from . import image
from .console import (PROMPT, BenchError, Console, ConsoleInterrupted, UnexpectedState,
                      WaitTimeout, display, wait, wait_quiet)
from .guard import (BY_NAME, Partition, check_bank, check_layout, check_target, window_address)
from .ramboot import (SERVER_SOURCE, AttemptFailed, Context, RamBootConfig, RamBootError,
                      check_environment, check_ready_pattern, check_started_image, console_lock,
                      expected_addresses, read_cmdline, reach_loader_prompt, recover,
                      wait_for_kernel, watch_transfer)
from .rawport import RawConsole, TcpPort
from .uboot import Loader, UnexpectedReset
from .ymodem import Sender, YmodemError

IMAGE_ADDRESS = 0x80000000
WATCHDOG_LINE = "mw.l b8003268 0"
STALL = 30.0  # seconds without a new byte
TRIAL_ADDRESS = window_address(BY_NAME["k0"].start)  # 0x94080000


class FlashError(BenchError):
    """The write did not succeed."""


class Stalled(FlashError):
    """The console stopped answering in the middle of a write."""

    exit_status = 6


class Interrupted(FlashError):
    """A signal arrived during a write; the tool stopped after the command ended."""


class Bank1Changed(UnexpectedState):
    """Bank 1's checksums differ from before the write."""


class SignalGate:
    """Holds a stop request until a command that must not be interrupted has finished."""

    def __init__(self) -> None:
        self.stop_requested = False
        self.reason = ""

    def request_stop(self, reason: str = "a signal") -> None:
        self.stop_requested = True
        self.reason = reason

    def check(self, finished: str) -> None:
        if self.stop_requested:
            raise Interrupted("stopped on request (%s) after %s had finished" % (self.reason,
                                                                                  finished))


@dataclass
class FlashOptions:
    """What the person asked for."""

    target: str = "kernel"
    bank: str = "0"
    via: str = "tftp"  # "tftp" or "ymodem"
    arm: bool = False
    commit: bool = False
    accept_same_bank_fallback: bool = False


@dataclass
class FlashConfig:
    """What the run needs to know, apart from the request."""

    kernel_name: str = "uImage"  # served names
    rootfs_name: str = "rootfs"
    squashfs_compressions: tuple[int, ...] = (image.SQUASHFS_XZ,)
    stall_limit: float = STALL
    boot: RamBootConfig = field(default_factory=RamBootConfig)
    ymodem_over_bridge: bool = False  # an explicit opt-in: not validated on a bridge
    bridge_host: str = ""
    bridge_port: int = 2001


@dataclass
class FlashResult:
    """What a finished run reports."""

    partition: str
    length: int
    crc32: int
    armed: bool
    committed: bool
    booted: bool
    fallback_bank: str
    bank1: tuple[int, int]
    notes: list[str] = field(default_factory=list)


# --------------------------------------------------------------------------
# The lines, from the tool's own table
# --------------------------------------------------------------------------

def erase_line(part: Partition) -> str:
    return "sf erase %x +%x" % (part.start, part.size)


def write_line(part: Partition, length: int) -> str:
    return "sf write %x %x %x" % (IMAGE_ADDRESS, part.start, length)


def ram_crc_line(length: int) -> str:
    return "crc32 %x %x" % (IMAGE_ADDRESS, length)


def flash_crc_line(part: Partition, length: int) -> str:
    return "crc32 %x %x" % (window_address(part.start), length)


def bank1_lines() -> list[str]:
    return [flash_crc_line(BY_NAME["k1"], BY_NAME["k1"].size),
            flash_crc_line(BY_NAME["r1"], BY_NAME["r1"].size)]


@dataclass(frozen=True)
class Step:
    """A step of the procedure and the lines it types."""

    name: str
    lines: tuple[str, ...]


def plan(target: str, length: int, via: str, *, arm: bool, commit: bool,
         config: FlashConfig | None = None) -> list[Step]:
    """The lines a successful run types, in order, as the procedure's steps.

    The guard's own reads of single variables are not part of the plan: they
    depend on what the board's macros name. A dry run prints exactly this.
    """
    config = config or FlashConfig()
    part = BY_NAME[check_target(target)]
    name = config.kernel_name if part.name == "k0" else config.rootfs_name
    steps = [Step("read the environment", ("printenv",)),
             Step("record the checksums of bank 1", tuple(bank1_lines()))]
    if via == "ymodem":
        steps += [Step("stop the watchdog", (WATCHDOG_LINE,)),
                  Step("receive by YMODEM", ("loady %x" % IMAGE_ADDRESS,)),
                  Step("check the RAM copy", (ram_crc_line(length),))]
    else:
        steps += [Step("receive by TFTP", ("run sdsup", "tftpboot %x %s" % (IMAGE_ADDRESS, name))),
                  Step("check the RAM copy", (ram_crc_line(length),)),
                  Step("stop the watchdog", (WATCHDOG_LINE,))]
    steps += [Step("erase the partition", (erase_line(part),)),
              Step("write the partition", (write_line(part, length),)),
              Step("check the flash", (flash_crc_line(part, length),)),
              Step("check bank 1 again", tuple(bank1_lines()))]
    pairs: list[str] = []
    if commit:
        pairs.append("sw_commit")
    if arm:
        pairs.append("sw_tryactive")
    if pairs:
        values = {"sw_commit": "0", "sw_tryactive": "0"}
        lines = ["setenv %s %s" % (n, values[n]) for n in pairs] + ["saveenv"]
        lines += ["printenv %s" % n for n in pairs]
        steps.append(Step("set the variables", tuple(lines)))
    if arm:
        steps.append(Step("boot the trial", ("boot",)))
    return steps


def plan_lines(steps: list[Step]) -> list[str]:
    return [line for step in steps for line in step.lines]


# --------------------------------------------------------------------------
# Receiving the image
# --------------------------------------------------------------------------

def receive_by_tftp(ctx: Context, name: str, length: int, config: FlashConfig) -> None:
    """Bring the image into RAM over TFTP; one recovery from the failing first attempt."""
    boot = config.boot
    attempts = 0
    resume: int | None = None
    while True:
        if resume is not None:
            ctx.loader.wait_for_loader(resume, boot.loader_wait)
            ctx.loader.break_in()
            ctx.loader.begin_session()
            resume = None
        ctx.loader.command("run sdsup", timeout=15.0)
        attempts += 1
        approval, mark = ctx.loader.start("tftpboot %x %s" % (IMAGE_ADDRESS, name))
        try:
            watch_transfer(ctx, mark, length, boot)
        except AttemptFailed as failure:
            if attempts > boot.recoveries:
                raise FlashError("%s again after a recovery; nothing was written"
                                 % failure.why) from failure
            ctx.say("the transfer failed (%s); recovering" % failure.why)
            recover(ctx, mark, failure, boot)
            resume = mark
            continue
        ctx.loader.finish(approval.text, mark, timeout=15.0)
        return


@contextmanager
def bridge_connection(console: Console, host: str, port: int, ctx: Context) -> Iterator[RawConsole]:
    """Stop the pane's client and open the one raw connection to the bridge."""
    with console.released_pane():
        raw = RawConsole(TcpPort(host, port), ctx.clock, ctx.log)
        try:
            yield raw
        finally:
            raw.close()


_SUMMARY = re.compile(r"xyzModem - CRC mode, (\d+)\(SOH\)/(\d+)\(STX\)/(\d+)\(CAN\) packets, "
                      r"(\d+) retries")
_TOTAL = re.compile(r"## Total Size\s+=\s+0x([0-9a-fA-F]+) = (\d+) Bytes")


def receive_by_ymodem(ctx: Context, name: str, data: bytes,
                      connect: Callable[[], AbstractContextManager[RawConsole]]) -> None:
    """Send the image with YMODEM through a raw connection, then check the loader's summary.

    The raw connection gets its own loader driver, behind the same guard; the
    guard's live reads go through it for the length of the transfer.
    """
    previous = ctx.guard.fetch
    with connect() as raw:
        loader = Loader(raw, ctx.guard, ctx.clock, ctx.log)  # reads variables through ``raw``
        try:
            mark = raw.mark()
            raw.press("Enter")  # the raw connection has heard nothing yet: ask for a prompt
            wait(raw, ctx.clock, mark, [lambda text: "prompt" if text.endswith(PROMPT) else None],
                 5.0, what="the loader prompt on the raw connection")
            approval, mark = loader.start("loady %x" % IMAGE_ADDRESS)
            try:
                report = Sender(raw.port, ctx.clock, lambda done, total: ctx.say(
                    "YMODEM: %d of %d blocks sent" % (done, total))).send(name, data)
            except YmodemError as exc:
                _recover_prompt(raw, ctx)
                failure = FlashError("the YMODEM transfer failed: %s; nothing was written to "
                                     "flash" % exc)
                failure.__cause__ = exc
                raise failure from exc
            result = loader.finish(approval.text, mark, timeout=60.0, idle=30.0)
        finally:
            ctx.guard.fetch = previous
            ctx.loader.typed.extend(loader.typed)
    ctx.loader.resync()
    _check_summary(result.output, len(data), report.end_acknowledged)


def _recover_prompt(raw: RawConsole, ctx: Context) -> None:
    try:
        wait_quiet(raw, ctx.clock, 1.0, 15.0)
    except WaitTimeout:
        pass


def _check_summary(output: str, length: int, acknowledged: bool) -> None:
    summary = _SUMMARY.search(output)
    total = _TOTAL.search(output)
    if summary is None or total is None:
        raise FlashError("the loader printed no YMODEM summary after the transfer (the end of "
                         "the batch %s acknowledged); the console shows: %s"
                         % ("was" if acknowledged else "was not", display(output, 300)))
    soh, stx, can = (int(summary.group(i)) for i in (1, 2, 3))
    blocks = -(-length // 1024)
    if (soh, stx, can) != (2, blocks, 0):
        raise FlashError("the loader counted %d header, %d data and %d cancel packets; "
                         "%d, %d and 0 were expected" % (soh, stx, can, 2, blocks))
    if int(total.group(2)) != length:
        raise FlashError("the loader received %s bytes, the file has %d" % (total.group(2), length))


# --------------------------------------------------------------------------
# The run
# --------------------------------------------------------------------------

def _written(output: str, size: int, what: str) -> None:
    """The progress display must have reached its last line: ``100% ~ <end>/<size> B``."""
    match = re.search(r"100% ~ ([0-9a-f]{8})/(\d+) B", output)
    if match is None or int(match.group(2)) != size:
        raise FlashError("%s did not report finishing %d bytes; the partition may be wrong"
                         % (what, size))


def bank1_checksums(loader: Loader) -> tuple[int, int]:
    """The loader's CRC-32 of bank 1's kernel and root filesystem."""
    k1, r1 = BY_NAME["k1"], BY_NAME["r1"]
    return (loader.crc32(window_address(k1.start), k1.size),
            loader.crc32(window_address(r1.start), r1.size))


def fallback_bank(env: dict[str, str]) -> str:
    """The bank a plain reset boots, which is where a failed trial ends up."""
    return env.get("sw_commit", "?")


def flash_image(ctx: Context, path: str, options: FlashOptions, config: FlashConfig, *,
                gate: SignalGate | None = None,
                ymodem_connect: Callable[[], AbstractContextManager[RawConsole]] | None = None,
                server_source: str = SERVER_SOURCE) -> FlashResult:
    """Write ``path`` to its partition of bank 0, verify it, and optionally arm a trial."""
    gate = gate or SignalGate()
    check_bank(options.bank)  # a bank other than 0 is refused before any work
    part = BY_NAME[check_target(options.target)]
    if options.via not in ("tftp", "ymodem"):
        raise FlashError("the transport is tftp or ymodem, not %r" % options.via)
    if options.arm:
        check_ready_pattern(config.boot.ready_pattern)
    data = image.read_file(path)
    kernel: image.UImage | None = None
    if part.name == "k0":
        kernel = image.check_uimage(data, part.size, what=path)
    else:
        image.check_squashfs(data, config.squashfs_compressions, part.size, what=path)
    length, crc = len(data), zlib.crc32(data)
    served = config.kernel_name if part.name == "k0" else config.rootfs_name
    connect = ymodem_connect
    if options.via == "ymodem" and connect is None:
        if not config.ymodem_over_bridge or not config.bridge_host \
                or not isinstance(ctx.console, Console):
            raise UnexpectedState(
                "YMODEM needs a direct serial port: through the bridge it has not been "
                "validated. Use TFTP, or a serial device, or pass --ymodem-over-bridge and "
                "the bridge's address to try it")
        console = ctx.console

        def connect() -> AbstractContextManager[RawConsole]:  # type: ignore[misc]
            return bridge_connection(console, config.bridge_host, config.bridge_port, ctx)

    ctx.say("%s: %d bytes, CRC-32 %08x, for %s" % (path, length, crc, part.name))
    notes: list[str] = []
    ctx.guard.session.commit = options.commit
    with console_lock(ctx.console, "flash"):
        if options.via == "tftp" and ctx.host is not None:
            notes += ctx.host.prepare(path, served, server_source)
        ctx.say(reach_loader_prompt(ctx, config.boot))
        env = ctx.loader.begin_session()
        check_layout(env)
        if env.get("sw_tryactive") != "2":
            raise UnexpectedState("sw_tryactive reads %r, not 2: a trial is already pending, "
                                  "so the board is not in a state to flash"
                                  % env.get("sw_tryactive"))
        fallback = fallback_bank(env)
        if options.arm:
            ctx.say("a failed trial falls back to bank %s%s" % (
                fallback, " (the shipped firmware)" if fallback == "1" else ""))
            if fallback != "1" and not options.accept_same_bank_fallback:
                raise UnexpectedState(
                    "sw_commit is %s, so a failed trial of bank 0 is not followed by the "
                    "shipped firmware: the way back would be the loader prompt and 'run b1'. "
                    "Nothing was written. Pass --accept-same-bank-fallback to arm anyway"
                    % fallback)
        required = ("sdsup", "ipaddr", "serverip", "netmask") if options.via == "tftp" else ()
        check_environment(env, required, expected_addresses(config.boot)
                          if options.via == "tftp" else {})
        before = bank1_checksums(ctx.loader)
        ctx.say("bank 1 checksums recorded: %08x %08x" % before)

        # the image into RAM, and a check of the RAM copy
        if options.via == "ymodem":
            ctx.loader.command(WATCHDOG_LINE)
            assert connect is not None
            receive_by_ymodem(ctx, served, data, connect)
            gate.check("the YMODEM transfer")
        else:
            receive_by_tftp(ctx, served, length, config)
        got = ctx.loader.crc32(IMAGE_ADDRESS, length)
        if got != crc:
            raise FlashError("the RAM copy has CRC-32 %08x, the file %08x; nothing was written"
                             % (got, crc))
        ctx.say("the RAM copy matches")
        if options.via == "tftp":
            ctx.loader.command(WATCHDOG_LINE)

        # the write: nothing is typed while it runs, and a signal waits
        gate.check("the checks before the erase")
        try:
            ctx.say("erasing %s" % part.name)
            result = ctx.loader.command(erase_line(part), timeout=900.0, idle=config.stall_limit)
            _written(result.output, part.size, "the erase")
            gate.check("the erase of %s" % part.name)
            ctx.say("writing %s" % part.name)
            result = ctx.loader.command(write_line(part, length), timeout=900.0,
                                        idle=config.stall_limit)
            _written(result.output, length, "the write")
            gate.check("the write of %s" % part.name)
        except WaitTimeout as exc:
            raise Stalled("the console stalled while %s was being written: %s. The partition is "
                          "invalid; the trial was not armed" % (part.name, exc.tail)) from exc
        except (ConsoleInterrupted, UnexpectedReset) as exc:
            raise type(exc)("%s. %s was being written: the partition may be invalid and the "
                            "trial was not armed" % (exc, part.name)) from exc
        stored = ctx.loader.crc32(window_address(part.start), length)
        if stored != crc:
            raise FlashError("%s reads back with CRC-32 %08x, the file has %08x; the partition "
                             "is wrong and the trial was not armed" % (part.name, stored, crc))
        ctx.say("%s verified through the flash window" % part.name)
        after = bank1_checksums(ctx.loader)
        if after != before:
            raise Bank1Changed("bank 1's checksums changed (%08x %08x before, %08x %08x after); "
                               "nothing was armed" % (before + after))
        gate.check("the verification of %s" % part.name)

        armed = committed = booted = False
        pairs: list[tuple[str, str]] = []
        if options.commit and env.get("sw_commit") != "0":
            pairs.append(("sw_commit", "0"))
        if options.arm:
            pairs.append(("sw_tryactive", "0"))
        if pairs:
            ctx.loader.set_saved(*pairs)
            committed = any(n == "sw_commit" for n, _v in pairs)
            armed = options.arm
            ctx.say("environment saved: %s" % ", ".join("%s=%s" % p for p in pairs))
        if armed:
            try:
                booted = _boot_trial(ctx, part, kernel, config)
            except BenchError:
                _cancel_trial(ctx)
                raise
        return FlashResult(part.name, length, crc, armed, committed, booted, fallback, after,
                           notes)


def _cancel_trial(ctx: Context) -> None:
    """Best effort: if a trial is still armed and the loader prompt is there, cancel it.

    The boot chain spends the flag before it starts the kernel, so a trial that
    did start leaves nothing to cancel; one that never started would otherwise
    run at the next reset.
    """
    try:
        if ctx.loader.at_prompt() and ctx.loader.getenv("sw_tryactive") == "0":
            ctx.loader.set_saved(("sw_tryactive", "2"))
            ctx.say("the trial that did not start was cancelled (sw_tryactive 2)")
    except BenchError:
        ctx.say("the trial may still be armed: check sw_tryactive at the loader prompt")


def _boot_trial(ctx: Context, part: Partition, kernel: image.UImage | None,
                config: FlashConfig) -> bool:
    """Boot the armed trial and watch it: the right image, the banner, the ready marker."""
    begun = ctx.clock.now()
    _approval, mark = ctx.loader.start("boot")
    check_started_image(ctx, mark, kernel, TRIAL_ADDRESS)
    ctx.say("the loader started the trial of %s" % ("the new kernel" if kernel else "bank 0"))
    wait_for_kernel(ctx, mark, begun + config.boot.ready_limit, config.boot.ready_pattern)
    cmdline = read_cmdline(ctx)
    if cmdline is not None and "root=31:5" not in cmdline:
        raise RamBootError("the trial kernel's command line has no root=31:5 (bank 0's root "
                           "filesystem): %s" % display(cmdline, 200))
    if cmdline is not None:
        ctx.say("the command line confirms bank 0's root filesystem")
    return True
