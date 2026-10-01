# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The scripted board's network, kernel start and watchdog.

On top of the loader of ``simboard`` this adds what a RAM boot needs: the
TFTP transfer with its progress marks, the failure that shows only ``T``
marks, the header that ``bootm`` prints, the kernel's banner and userspace,
the restart through the CPU reset vector from a running kernel, and the
watchdog that a restart leaves counting. That watchdog resets the chip about
47 seconds after a restart unless the prompt writes zero to its control
register or a kernel starts.

Behaviours a test can switch on: the first transfer after a restart that
fails with ``T`` marks (and a Ctrl-C that does or does not stop it); a
server that is not there; a lane that is down; a missing file; a kernel
that prints nothing after ``Starting kernel ...``.
"""

from __future__ import annotations

import struct
import time
import zlib
from typing import NamedTuple

from bench.host import RunResult

from .loadermodel import LoaderModel, number
from .simboard import EOL, BoardModel, LoaderCase, Mode, SimBoard, human_size

FUSE_SECONDS = 46.9
KERNEL_BANNER_DELAY = 0.4
USERSPACE_AFTER_BANNER = 2.07
BLOCK = 1468


def physical(address: int) -> int:
    return address & 0x1FFFFFFF if address >= 0x80000000 else address


def created_text(seconds: int) -> str:
    """A creation time as the loader prints it: two spaces, then the hour in two columns."""
    moment = time.gmtime(seconds)
    return "%04d-%02d-%02d  %2d:%02d:%02d UTC" % (moment.tm_year, moment.tm_mon, moment.tm_mday,
                                                  moment.tm_hour, moment.tm_min, moment.tm_sec)


class Header(NamedTuple):
    name: str
    created: int
    data_size: int
    load_address: int
    entry_point: int


def read_header(data: bytes | None) -> Header | None:
    """The fields of a legacy image whose magic and both CRCs are right, else None."""
    if data is None or len(data) < 64:
        return None
    magic, header_crc, created, size, load, entry, data_crc = struct.unpack(">IIIIIII", data[:28])
    if magic != 0x27051956 or zlib.crc32(data[:4] + bytes(4) + data[8:64]) != header_crc:
        return None
    if len(data) < 64 + size or zlib.crc32(data[64:64 + size]) != data_crc:
        return None
    return Header(data[32:64].split(b"\0")[0].decode("latin-1"), created, size, load, entry)


class NetModel(BoardModel):

    def cmd_tftpboot(self, args, suffix, depth):
        board = self.board
        super().cmd_tftpboot(args, suffix, depth)
        address = number(args[0]) if args else 0x82000000
        name = args[1] if len(args) > 1 else ""
        if not board.network_started:
            self.print("network start-up")
            board.network_started = True
        self.print("Using eth0 device")
        self.print("TFTP from server %s; our IP address is %s"
                   % (self.env.get("serverip", ""), self.env.get("ipaddr", "")))
        self.print("Filename '%s'." % name)
        self.print("Load address: 0x%x" % address)
        first_attempt = not board.tftp_attempted
        board.tftp_attempted = True
        failing = (not board.tftp_server_ok or not board.lane_up
                   or (first_attempt and board.fail_first_after_sysrq
                       and board.restart_cause == "sysrq"))
        if failing:
            self.say("Loading: *")
            self.block(board.start_tmarks)
            return 1
        data = board.tftp_files.get(name)
        if data is None:
            self.say("Loading: *")
            self.print("TFTP error: 'no such file' (1)")
            self.print("Not retrying...")
            return 1
        marks = -(-(len(data) // BLOCK + 1) // 10)
        self.say("Loading: *")
        remaining, column = marks, 1
        total = 0.3 + len(data) / 1e6
        while remaining > 0:
            step = min(remaining, 10)
            self.wait(total * step / marks)
            for _ in range(step):
                self.say("#")
                column += 1
                if column > 65:
                    self.say(EOL + " " * 9)
                    column = 1
            remaining -= step
        self.say(EOL)
        self.print("done")
        self.print("Bytes transferred = %d (%x hex)" % (len(data), len(data)))
        self.env["filesize"] = "%x" % len(data)
        board.ram_store(address, data)  # the macro that follows sees the image at once
        return 0

    def cmd_bootm(self, args, suffix, depth):
        board = self.board
        LoaderModel.cmd_bootm(self, args, suffix, depth)  # records the start
        address = number(args[0]) if args else 0x82000000
        header = read_header(board.memory_image(address))
        self.print("## Booting kernel from Legacy Image at %08x ..." % address)
        if header is None:
            self.print("Wrong Image Format for bootm command")
            self.print("ERROR: can't get kernel image!")
            return 1
        self.print("   Image Name:   %s" % header.name)
        self.print("   Created:      %s" % created_text(header.created))
        self.print("   Image Type:   MIPS Linux Kernel Image (lzma compressed)")
        self.print("   Data Size:    %d Bytes = %s" % (header.data_size, human_size(header.data_size)))
        self.print("   Load Address: %08x" % header.load_address)
        self.print("   Entry Point:  %08x" % header.entry_point)
        self.print("   Verifying Checksum ... OK")
        self.print("   Uncompressing Kernel Image ... OK")
        self.print("")
        self.print("Starting kernel ...")
        self.terminal = "kernel"
        self.defer(lambda: board.start_kernel(header.name))
        return 0

    def cmd_boot(self, args, suffix, depth):
        if self.env.get("sw_tryactive", "2") != "2":
            self.print("network start-up")
            self.print("")
            self.print("Using eth0 device")
            self.print("listener timeout.")
        return super().cmd_boot(args, suffix, depth)

    def on_store(self, address: int, value: int, size: int) -> None:
        board = self.board
        if address == 0xBB000104 and value == 2:
            board.lane_up = True
        elif address == 0xB8003268:
            if value == 0:
                board.fuse_armed = False
            else:
                board.watchdog_enabled = True


class NetBoard(SimBoard):
    """A board with a network, a kernel and a watchdog."""

    model_class = NetModel

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.tftp_files: dict[str, bytes] = {}
        self.tftp_server_ok = True
        self.lane_up = False
        self.network_started = False
        self.tftp_attempted = False
        self.tftp_failing = False
        self.fail_first_after_sysrq = False
        self.ctrl_c_aborts = False
        self.fuse_armed = False
        self.watchdog_enabled = False
        self.restart_cause = ""
        self.kernel_mode = "ok"  # or "silent": nothing after Starting kernel
        self.ready_text = "Welcome to the bench image" + EOL + "login: "
        self.release = "6.18.48-test"
        self.cmdline = "console=ttyS0,115200 root=31:5 rootfstype=squashfs"
        self.kernels_started: list[str] = []
        self.ram: dict[int, bytes] = {}
        self.tmarks_printed = 0
        self.tmark_times: list[float] = []
        self.busy_input_times: list[float] = []

    # -- restarts and the fuse -------------------------------------------------

    def on_restart(self, cause: str) -> None:
        self.restart_cause = cause
        self.lane_up = False
        self.network_started = False
        self.tftp_attempted = False
        self.tftp_failing = False
        self.watchdog_enabled = False
        self.fuse_armed = cause == "sysrq"
        if self.fuse_armed:
            self.at(FUSE_SECONDS, self._fuse)

    def _fuse(self) -> None:
        if self.fuse_armed:
            self.restart("fuse")

    def sysrq_restart(self) -> bool:
        """The restart a running kernel does on ``echo b > /proc/sysrq-trigger``."""
        if self.mode is not Mode.LINUX:
            return False
        self.emit("sysrq: Resetting" + EOL
                  + "rtl9602c: entering the reset vector; watchdog armed behind it" + EOL)
        self.restart("sysrq")
        return True

    # -- transfers -------------------------------------------------------------

    def start_tmarks(self) -> None:
        """A transfer that never starts: one ``T`` for every timeout of five seconds."""
        self.tftp_failing = True
        self.at(5.0, self._tmark)

    def _tmark(self) -> None:
        if not self.tftp_failing:
            return
        self.tmarks_printed += 1
        self.tmark_times.append(self.clock.now())
        self.emit("T ")
        self.at(5.0, self._tmark)

    def on_busy_input(self, byte: int) -> None:
        self.busy_input_times.append(self.clock.now())
        if byte == 0x03 and self.tftp_failing and self.ctrl_c_aborts:
            self.tftp_failing = False
            self.emit(EOL + "Abort" + EOL)
            self.resume()

    def ram_store(self, address: int, data: bytes) -> None:
        self.ram[physical(address)] = data

    def memory_image(self, address: int) -> bytes | None:
        """The bytes at an address: a loaded image, or a flash window (see subclasses)."""
        return self.ram.get(physical(address))

    # -- the kernel ------------------------------------------------------------

    def start_kernel(self, name: str) -> None:
        self.mode = Mode.KERNEL
        self.fuse_armed = False  # the kernel clears the count at its entry
        self.kernels_started.append(name)
        if self.kernel_mode == "silent":
            return
        self.at(KERNEL_BANNER_DELAY, lambda: self.emit(
            "[    0.000000] Linux version %s (builder@example) SMP" % self.release + EOL))
        self.at(KERNEL_BANNER_DELAY + 0.1, lambda: self.emit(
            "[    0.000000] bootargs: %s" % self.model.env.get("bootargs", "") + EOL))
        self.at(KERNEL_BANNER_DELAY + USERSPACE_AFTER_BANNER, self._userspace)

    def _userspace(self) -> None:
        self.emit(self.ready_text)
        self.mode = Mode.LINUX

    def boot_linux(self) -> None:
        """Be a running kernel at its login prompt, without the history."""
        self.epoch += 1
        self.mode = Mode.LINUX
        self.fuse_armed = False
        self.emit("login: ")


class SshToBoard:
    """The ssh calls the tool starts to restart the board.

    ``reachable`` names the addresses an ssh connection succeeds to; a call to
    any other address ends at once with status 255. A connection that works
    runs the restart half a second later, and the client ends (also with 255)
    when the dead connection is noticed, nine seconds after it began.
    """

    def __init__(self, runner, board: NetBoard, clock, reachable: tuple[str, ...]) -> None:
        self.runner = runner
        self.board = board
        self.clock = clock
        self.reachable = set(reachable)
        self.started: list[str] = []
        runner.on_spawn = self.on_spawn
        runner.board_run = self.on_run

    def on_spawn(self, argv) -> float | None:
        target = next((word.split("@", 1)[1] for word in argv if "@" in word
                       and not word.startswith("-")), "")
        self.started.append(target)
        if target not in self.reachable:
            return 0.4
        if "/proc/sysrq-trigger" in argv[-1]:
            self.clock.schedule(0.5, self.board.sysrq_restart)
        return 9.0


    def on_run(self, argv) -> RunResult | None:
        """A command run on the board over ssh and waited for."""
        target = argv[-2].split("@", 1)[1]
        if target not in self.reachable or self.board.mode is not Mode.LINUX:
            return RunResult(255, "", "ssh: connect to host %s port 22: no route" % target)
        if argv[-1] == "cat /proc/cmdline":
            return RunResult(0, self.board.cmdline + "\n")
        return RunResult(1, "", "unsupported in the simulation")


class NetCase(LoaderCase):
    """A loader case on a board that has a network and a kernel."""

    board_class = NetBoard
    kernel_size = 3000

    def setUp(self) -> None:
        super().setUp()
        from .images import make_uimage
        self.kernel_image = make_uimage(self.kernel_size, name="Linux-6.18.48-test")
        self.board.tftp_files["uImage"] = self.kernel_image


class BenchCase(NetCase):
    """A board running Linux, an image host and the ssh route between them, and a run context."""

    def setUp(self) -> None:
        super().setUp()
        import os
        from bench.host import BoardAccess, HostConfig, ImageHost
        from bench.ramboot import Context, RamBootConfig
        from .simhost import SimImageHost, SimRunner
        self.board.boot_linux()
        self.runner = SimRunner(SimImageHost(), self.clock)
        self.ssh = SshToBoard(self.runner, self.board, self.clock, ("198.51.100.2", "192.0.2.2"))
        self.served = os.path.join(self.tmp, "served")
        self.host = ImageHost(HostConfig(ssh="local", served_dir=self.served,
                                         work_dir=os.path.join(self.tmp, "work")),
                              self.runner, clock=self.clock)
        self.said: list[str] = []
        self.ctx = Context(console=self.console, loader=self.loader, guard=self.guard,
                           clock=self.clock, runner=self.runner, access=BoardAccess(),
                           host=self.host, log=self.console.log, say=self.said.append)
        self.boot_config = RamBootConfig()
        self.server_source = os.path.join(self.tmp, "tftp.py")
        with open(self.server_source, "w", encoding="utf-8") as handle:
            handle.write("# the server\n")
