# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A scripted board: first-stage loader, loader countdown and prompt.

The board is a state machine behind the console. It prints the byte
sequences of the real thing -- lines that end CR CR LF, the countdown with
its backspaces and two-digit numbers, the echo of every received character,
the prompt without a newline, ``Unknown command``, ``<INTERRUPT>`` -- and
runs on a virtual clock, so a countdown of ten minutes costs nothing. A
command is executed by the permissive model of the loader, which also
records every effect it has; here the command's output and its duration are
scripted on top.

Behaviours a test can switch on: a countdown that consumes its first byte
(always); a board that is deaf to the first bytes of a break-in; characters
that are dropped, doubled or substituted on their way in; a reset in the
middle of a command; a bridge marker line in the stream; a stall; a login
prompt where the loader should be; an empty line that runs the last typed
command again, as the loader does. Input that arrives while the board is
busy is not read: it is recorded in ``lost`` so a test can tell that nothing
was typed during a write.
"""

from __future__ import annotations

import enum
from collections.abc import Callable

from bench.console import PROMPT
from bench.guard import Guard
from bench.uboot import Loader

from . import vectors
from .faketmux import Transcript
from .loadermodel import LoaderModel
from .simclock import VirtualClock
from .support import ConsoleCase

EOL = "\r\r\n"

# The commands the loader does not run again on an empty line; it repeats every
# other one. The list is the part of the loader's table that the tests need.
NON_REPEATABLE = ("loady", "reset", "saveenv", "setenv")

# From a restart to the first-stage banner, the loader's banner and the
# countdown, in seconds.
FIRST_STAGE_BANNER = 0.285
LOADER_BANNER = 1.47
COUNTDOWN_OPENS = 1.81


class Mode(enum.Enum):
    OFF = "off"
    FIRST_STAGE = "first stage"
    LOADER_INIT = "loader starting"
    COUNTDOWN = "countdown"
    PROMPT = "prompt"
    BUSY = "busy"
    KERNEL = "kernel starting"
    LINUX = "linux"
    HUNG = "hung"


def default_env(bootdelay: int = 600, base: dict[str, str] | None = None) -> dict[str, str]:
    """The variables of the board: layout, macros, boot chain and addresses.

    The kernel arguments are the board's shape (``vectors.E6``): partition
    tables that hold parentheses.
    """
    env = dict(vectors.E6 if base is None else base)
    env.update(
        bootdelay=str(bootdelay), ipaddr="192.0.2.2", serverip="192.0.2.1",
        netmask="255.255.255.252", ethaddr="00:00:5E:00:53:01", tftp_base="80000000",
        sw_active="0", fileaddr="", bootargs="")
    return env


def human_size(count: int) -> str:
    """A size as the loader prints it after the byte count."""
    if count >= 1 << 20:
        return "%.1f MiB" % (count / (1 << 20))
    return "%.1f KiB" % (count / 1024)


class BoardModel(LoaderModel):
    """The loader's interpreter with scripted output and timing.

    A command appends to ``script``: lines of output, pauses and calls that
    run when their time comes. The board plays the script on the virtual
    clock and prints the prompt after the last step.
    """

    def __init__(self, board: "SimBoard", env: dict[str, str]) -> None:
        super().__init__(env=env)
        self.board = board
        self.script: list[tuple] = []

    # -- scripting -------------------------------------------------------------

    def print(self, text: str) -> None:
        self.output.append(text)
        self.script.append(("out", text + EOL))

    def say(self, text: str) -> None:
        """Output without a line end."""
        self.script.append(("out", text))

    def wait(self, seconds: float) -> None:
        self.script.append(("wait", seconds))

    def defer(self, call: Callable[[], None]) -> None:
        """Run ``call`` at this point of the script's timeline."""
        self.script.append(("call", call))

    def block(self, start: Callable[[], None]) -> None:
        """Stop the script here; ``start`` begins what holds it, ``board.resume`` goes on."""
        self.script.append(("block", start))

    def progress(self, prefix: str, size: int, last: int, seconds: float, steps: int = 20) -> None:
        """A flash progress display: percentages with backspaces, then the end line."""
        self.say(prefix)
        for step in range(steps + 1):
            self.wait(seconds / steps)
            self.say("\x08\x08\x08\x08%3d%%" % (100 * step // steps))
        self.say(" ~ %08x/%d B" % (last, size) + EOL)

    # -- commands with output --------------------------------------------------

    def cmd_version(self, args, suffix, depth):
        self.print("U-Boot 2011.12.NA (Jan 01 2026 - 00:00:00)")
        self.print("toolchain line one")
        self.print("toolchain line two")

    def cmd_printenv(self, args, suffix, depth):
        status = 0
        if args:
            for name in args:
                if name in self.env:
                    self.print("%s=%s" % (name, self.env[name]))
                else:
                    self.print('## Error: "%s" not defined' % name)
                    status = 1
            return status
        used = 0
        for name in sorted(self.env):
            self.print("%s=%s" % (name, self.env[name]))
            used += len(name) + len(self.env[name]) + 2
        self.print("")
        self.print("Environment size: %d/8187 bytes" % used)
        return 0

    def cmd_saveenv(self, args, suffix, depth):
        copy = 2 if self.board.valid_environment == 1 else 1
        start = 0x40000 if copy == 1 else 0x42000
        other = 0x42000 if copy == 1 else 0x40000
        self.print("Saving Environment to SPI Flash...")
        self.progress("Erasing SPI flash...Erasing 8192 B from %08x..." % start, 8192,
                      start + 0x1FFF, 0.2, 2)
        self.progress("Writing to SPI flash...Writing 8192 B from 81f00000 to %08x..." % start,
                      8192, start + 0x1FFF, 0.2, 2)
        self.progress("Writing 1 B from 81f00000 to %08x..." % (other + 4), 1, other + 4, 0.05, 1)
        self.print("done")
        self.print("Valid environment: %d" % copy)
        self.defer(lambda: setattr(self.board, "valid_environment", copy))
        super().cmd_saveenv(args, suffix, depth)

    def cmd_reset(self, args, suffix, depth):
        self.terminal = "reset"

    def cmd_bootm(self, args, suffix, depth):
        """The plain board starts a kernel that prints nothing more."""
        super().cmd_bootm(args, suffix, depth)
        self.print("Starting kernel ...")
        self.terminal = "kernel"
        self.defer(lambda: setattr(self.board, "mode", Mode.KERNEL))


class SimBoard:
    """The board behind the console."""

    model_class = BoardModel

    def __init__(self, clock: VirtualClock, transcript: Transcript, *, bootdelay: int = 600,
                 env: dict[str, str] | None = None, tick: float = 1.0) -> None:
        self.clock = clock
        self.transcript = transcript
        self.tick = tick
        self.bootdelay = bootdelay
        self.model = self.model_class(self, dict(env) if env is not None
                                      else default_env(bootdelay))
        self.mode = Mode.OFF
        self.epoch = 0
        self.line = ""
        self.countdown = 0
        self.valid_environment = 1
        self.pane_attached = True
        self.raw_output = bytearray()  # what a raw connection would read
        # what happened, for tests
        self.executed: list[str] = []
        self.lost = bytearray()
        self.lost_busy = bytearray()
        self.typed_at_login = bytearray()
        self.restarts: list[str] = []
        self.restart_times: list[float] = []
        # behaviours a test switches on
        self.deaf_bytes = 0
        self.input_fault: Callable[[str], str] | None = None
        self.repeats_last_command = False  # an empty line runs the last typed command again
        self.last_typed = ""  # the line an empty line would repeat
        self.repeated: list[str] = []  # the lines that were run again by an empty line
        self.countdown_started: list[float] = []
        self._pending: list[tuple] = []
        self._terminal: str | None = None

    # -- output ----------------------------------------------------------------

    def emit(self, text: str) -> None:
        """Print to the console: into the transcript, or to a raw connection."""
        data = text.encode("latin-1")
        if self.pane_attached:
            self.transcript.emit(data)
        else:
            self.raw_output += data

    def marker(self, text: str) -> None:
        """A line the pane's client prints when the bridge connection changes."""
        seconds = int(self.clock.now()) % 86400
        stamp = "%02d:%02d:%02d" % (seconds // 3600, seconds // 60 % 60, seconds % 60)
        self.transcript.emit("--- console: %s at %s ---%s" % (text, stamp, EOL))

    def flap(self, text: str = "the bridge closed the connection") -> None:
        self.marker(text)

    def at(self, delay: float, call: Callable[[], None]) -> None:
        """Schedule ``call`` in this life of the board only."""
        epoch = self.epoch
        self.clock.schedule(delay, lambda: call() if epoch == self.epoch else None)

    # -- power and restarts ----------------------------------------------------

    def power_off(self) -> None:
        self.epoch += 1
        self.mode = Mode.OFF
        self.line = ""

    def restart(self, cause: str = "restart") -> None:
        """Start again at the first-stage loader, as a CPU restart or a power-on does."""
        self.epoch += 1
        self.restarts.append(cause)
        self.restart_times.append(self.clock.now())
        self.mode = Mode.FIRST_STAGE
        self.line = ""
        self.last_typed = ""  # a loader that starts again remembers no command
        self.on_restart(cause)
        self.emit(EOL + "9601D" + EOL)
        self.at(FIRST_STAGE_BANNER, self._first_stage)
        self.at(LOADER_BANNER, self._loader_banner)
        self.at(COUNTDOWN_OPENS, self._open_countdown)

    power_on = restart

    def on_restart(self, cause: str) -> None:
        """Hook for state that a restart clears or arms."""

    def _first_stage(self) -> None:
        self.emit("PRELOADER test-stage 0.1" + EOL)
        for line in ("II: calibration 0 0 0", "AK: calibration 1 1 1", "   0x00 0x01 0x02"):
            self.emit(line + EOL)

    def _loader_banner(self) -> None:
        self.mode = Mode.LOADER_INIT
        for line in ("U-Boot 2011.12.NA (Jan 01 2026 - 00:00:00)", "toolchain line one",
                     "toolchain line two",
                     "Board: simulated board",
                     "DRAM: 32 MB", "SPI-F: MXIC/C22017/MMIO16-1/ModeC 1x8 MB (simulated)",
                     "Loading 8192B env. variables from offset 0x40000",
                     "Loading 8192B env. variables from offset 0x42000",
                     "Loaded 8192B env. variables from offset 0x42000",
                     "Net:   eth0", ""):
            self.emit(line + EOL)

    def _open_countdown(self) -> None:
        self.mode = Mode.COUNTDOWN
        self.countdown = int(self.model.env.get("bootdelay", "5"))
        self.countdown_started.append(self.clock.now())
        self.emit("Hit any key to stop autoboot: %2d " % self.countdown)
        self.at(self.tick, self._tick)

    def _tick(self) -> None:
        if self.mode is not Mode.COUNTDOWN:
            return
        self.countdown -= 1
        self.emit("\x08\x08\x08%2d " % self.countdown)
        if self.countdown > 0:
            self.at(self.tick, self._tick)
            return
        self.emit(EOL)
        self.mode = Mode.BUSY
        self.execute("boot", echo=False)

    # -- input -----------------------------------------------------------------

    def receive(self, data: bytes) -> None:
        for byte in data:
            self._receive(byte)

    def _receive(self, byte: int) -> None:
        mode = self.mode
        if mode is Mode.COUNTDOWN:
            if self.deaf_bytes > 0:
                self.deaf_bytes -= 1
                self.lost.append(byte)
                return
            # Any byte stops the countdown and is consumed.
            self.emit("\x08\x08\x08 0 " + EOL + PROMPT)
            self.mode = Mode.PROMPT
        elif mode is Mode.PROMPT:
            self._edit(byte)
        elif mode is Mode.BUSY:
            self.lost_busy.append(byte)
            self.on_busy_input(byte)
        elif mode is Mode.LINUX:
            self.typed_at_login.append(byte)
            if byte == 0x0D:
                self.emit("\r\nlogin: ")
        else:
            self.lost.append(byte)

    def on_busy_input(self, byte: int) -> None:
        """Hook: a byte arrived while a command was running."""

    def _edit(self, byte: int) -> None:
        if byte == 0x0D:
            line, self.line = self.line, ""
            self.emit(EOL)
            if line.strip():
                name = line.split()[0].partition(".")[0]
                self.last_typed = "" if name in NON_REPEATABLE else line
                self.execute(line)
            elif self.repeats_last_command and self.last_typed:
                self.repeated.append(self.last_typed)
                self.execute(self.last_typed)
            else:
                self.emit(PROMPT)
        elif byte == 0x03:
            self.line = ""
            self.emit("<INTERRUPT>" + EOL + PROMPT)
        elif 0x20 <= byte < 0x7F:
            received = chr(byte)
            if self.input_fault is not None:
                received = self.input_fault(received)
            self.line += received
            self.emit(received)

    # -- commands --------------------------------------------------------------

    def execute(self, line: str, echo: bool = True) -> None:
        """Run a line through the interpreter and play what it scripted."""
        self.executed.append(line)
        model = self.model
        model.script = []
        model.terminal = None
        model.run(line)
        self.play(model.script, model.terminal)

    def play(self, script: list[tuple], terminal: str | None) -> None:
        """Play a command's script, step by step, on the virtual clock."""
        self.mode = Mode.BUSY
        self._pending = list(script)
        self._terminal = terminal
        self._advance(self.epoch)

    def _advance(self, epoch: int) -> None:
        """Run steps until one waits, blocks or the script ends; then finish."""
        if epoch != self.epoch:
            return
        while self._pending:
            step = self._pending.pop(0)
            kind = step[0]
            if kind == "out":
                self.emit(step[1])
            elif kind == "wait":
                self.clock.schedule(step[1], lambda e=epoch: self._advance(e))
                return
            elif kind == "call":
                step[1]()
            elif kind == "block":
                step[1]()
                return
            if epoch != self.epoch:
                return
        self.finish(self._terminal)

    def resume(self) -> None:
        """Continue a script that stopped at a block."""
        self._advance(self.epoch)

    def finish(self, terminal: str | None) -> None:
        """The script is over: the prompt, a restart, a hang, or the kernel."""
        if terminal is None:
            self.mode = Mode.PROMPT
            self.emit(PROMPT)
        elif terminal == "reset":
            self.restart("command")
        elif terminal == "hang":
            self.mode = Mode.HUNG
        # "kernel": the kernel's own events take over

    # -- states a test can enter directly --------------------------------------

    def at_prompt(self) -> None:
        """Be at the loader prompt, as after a break-in that has settled."""
        self.epoch += 1
        self.mode = Mode.PROMPT
        self.emit(PROMPT)

    def show_login(self) -> None:
        """Be at a Linux login prompt."""
        self.epoch += 1
        self.mode = Mode.LINUX
        self.emit("\r\nlogin: ")

    def stall(self) -> None:
        """Stop answering without printing anything."""
        self.epoch += 1
        self.mode = Mode.HUNG


class LoaderCase(ConsoleCase):
    """A console over a scripted board, with a loader driver behind a guard."""

    bootdelay = 600
    board_class = SimBoard

    def setUp(self) -> None:
        super().setUp()
        self.board = self.make_board()
        self.tmux.board = self.board
        self.guard = Guard()
        self.loader = Loader(self.console, self.guard, self.clock, self.console.log)

    def make_board(self) -> SimBoard:
        return self.board_class(self.clock, self.transcript, bootdelay=self.bootdelay,
                                env=self.board_environment())

    def board_environment(self) -> dict[str, str] | None:
        """The board's variables; None gives ``default_env``."""
        return None

    def restart_to_countdown(self, cause: str = "power") -> None:
        """Restart the board and wait for its countdown."""
        mark = self.console.mark()
        self.board.restart(cause)
        self.assertEqual(self.loader.wait_for_loader(mark), "countdown")

    def reach_prompt(self) -> None:
        """Restart the board, break in and start a loader session."""
        self.restart_to_countdown()
        self.loader.break_in()
        self.loader.begin_session()
