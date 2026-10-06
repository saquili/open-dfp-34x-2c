# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The scripted board's flash, YMODEM receiver and flash commands.

The simulated flash is 8 MiB. Each region starts with a distinct repeating
pattern, so that a test can tell which region a stray byte came from. An erase
sets bytes to 0xff in 4 KiB units and a write can only clear bits (new = old
AND data), so a missing erase shows as wrong data. Every erase and write is
logged with its offset and length, and the memory-mapped windows read the same
bytes. The environment is kept apart from the flash bytes: a save is counted,
not played into the ``env`` regions, so that those regions can be asserted
unchanged after any scenario.

The commands print the loader's progress displays over virtual time: an erase
of 1328 KiB takes about 15 seconds, a write about 100 kB a second, and a
checksum a few hundred milliseconds. Behaviours a test can switch on: a write
that stops part way and leaves the loader hung; a write that stores a wrong
byte; a YMODEM transfer with faults; the lane that drops after a long write.
"""

from __future__ import annotations

import zlib
from dataclasses import dataclass

from .loadermodel import number
from .simboard import EOL
from .simnet import NetBoard, NetModel, physical
from .simymodem import Faults, YmodemReceiver

SIZE = 0x800000
UNIT = 4096
REGIONS = (("boot", 0x000000, 0x040000), ("env", 0x040000, 0x002000),
           ("env2", 0x042000, 0x002000), ("config", 0x044000, 0x03C000),
           ("k0", 0x080000, 0x1D0000), ("r0", 0x250000, 0x1F0000),
           ("k1", 0x440000, 0x14C000), ("r1", 0x58C000, 0x274000))
ERASE_SECONDS_PER_UNIT = 0.045
WRITE_BYTES_PER_SECOND = 100_000.0
CRC_BYTES_PER_SECOND = 4_000_000.0


class SimFlash:
    """The 8 MiB chip."""

    def __init__(self) -> None:
        self.data = bytearray(SIZE)
        for index, (_name, start, size) in enumerate(REGIONS):
            pattern = bytes((index * 29 + step * 7 + 1) & 0xFF for step in range(251))
            self.data[start:start + size] = (pattern * (size // len(pattern) + 1))[:size]
        self.log: list[tuple[str, int, int]] = []

    def region(self, name: str) -> bytes:
        for label, start, size in REGIONS:
            if label == name:
                return bytes(self.data[start:start + size])
        raise KeyError(name)

    def preload(self, name: str, content: bytes) -> None:
        """Put a content into a region, as the factory would: erased, then written."""
        for label, start, size in REGIONS:
            if label == name:
                assert len(content) <= size
                self.data[start:start + size] = b"\xff" * size
                self.data[start:start + len(content)] = content

    def erase(self, offset: int, length: int) -> None:
        self.log.append(("erase", offset, length))
        self.erase_units(offset, length)

    def erase_units(self, offset: int, length: int) -> None:
        """Clear the 4 KiB units that the range touches, without logging."""
        first = offset // UNIT * UNIT
        last = -(-(offset + length) // UNIT) * UNIT
        self.data[first:last] = b"\xff" * (last - first)

    def write(self, offset: int, content: bytes) -> None:
        self.log.append(("write", offset, len(content)))
        self.program(offset, content)

    def program(self, offset: int, content: bytes) -> None:
        """Clear the bits that the content has clear, without logging."""
        for index, byte in enumerate(content):
            self.data[offset + index] &= byte

    def read(self, offset: int, length: int) -> bytes:
        return bytes(self.data[offset:offset + length])

    def snapshot(self) -> dict[str, bytes]:
        return {name: self.region(name) for name, _s, _z in REGIONS}

    def protected_unchanged(self, before: dict[str, bytes]) -> list[str]:
        """The protected regions that differ from a snapshot."""
        return [name for name in ("boot", "env", "env2", "config", "k1", "r1")
                if self.region(name) != before[name]]

    def operations_outside_bank_0(self) -> list[tuple[str, int, int]]:
        k0, r0 = REGIONS[4], REGIONS[5]
        return [op for op in self.log
                if not any(start <= op[1] and op[1] + op[2] <= start + size
                           for _n, start, size in (k0, r0))]


def window_offset(address: int) -> int | None:
    """The flash offset behind a read window address, in any spelling."""
    base = physical(address)
    if 0x14000000 <= base < 0x14000000 + SIZE:
        return base - 0x14000000
    return None


@dataclass
class WriteFaults:
    stall_at: float | None = None  # fraction of an sf write after which the loader hangs
    wrong_byte: bool = False  # the write stores one wrong byte


class FlashModel(NetModel):

    def on_flash(self, kind: str, offset: int, length: int, memory: int = 0) -> None:
        board = self.board
        flash = board.flash
        if kind == "erase":
            seconds = max(-(-length // UNIT), 1) * ERASE_SECONDS_PER_UNIT
            flash.log.append(("erase", offset, length))
            self._timed("Erasing %d B from %08x..." % (length, offset), length,
                        offset + length - 1, seconds,
                        lambda start, size: flash.erase_units(offset + start, size))
            return
        content = board.read_memory(memory, length)
        if board.write_faults.wrong_byte:
            content = bytes([content[0] ^ 0x01]) + content[1:]
        seconds = length / WRITE_BYTES_PER_SECOND
        stall = board.write_faults.stall_at
        flash.log.append(("write", offset, length))
        self._timed("Writing %d B from %08x to %08x..." % (length, memory, offset), length,
                    offset + length - 1, seconds,
                    lambda start, size: flash.program(offset + start, content[start:start + size]),
                    stall_at=stall)
        self.defer(lambda: setattr(board, "lane_up", False))  # the lane drops after a long write

    def _timed(self, prefix: str, length: int, last: int, seconds: float, apply,
               stall_at: float | None = None) -> None:
        steps = 20
        self.say(prefix)
        done = 0
        for step in range(steps + 1):
            if stall_at is not None and step / steps > stall_at:
                self.terminal = "hang"
                return
            self.wait(seconds / steps)
            self.say("\x08\x08\x08\x08%3d%%" % (100 * step // steps))
            upto = length * (step + 1) // steps if step < steps else length
            if upto > done:
                chunk = (done, upto - done)
                self.defer(lambda c=chunk: apply(c[0], c[1]))
                done = upto
        self.say(" ~ %08x/%d B" % (last, length) + EOL)

    def cmd_crc32(self, args, suffix, depth):
        if len(args) >= 2:
            self.wait(number(args[1]) / CRC_BYTES_PER_SECOND)
        return super().cmd_crc32(args, suffix, depth)

    def checksum(self, address: int, length: int) -> int:
        return zlib.crc32(self.board.read_memory(address, length))

    def cmd_loady(self, args, suffix, depth):
        board = self.board
        super().cmd_loady(args, suffix, depth)
        address = number(args[0]) if args else 0x82000000
        self.print("## Ready for binary (ymodem) download to 0x%08X at 115200 bps..." % address)
        self.block(lambda: board.start_ymodem(address))
        return 0


class FlashBoard(NetBoard):
    """A board with a flash chip and a YMODEM receiver."""

    model_class = FlashModel

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.flash = SimFlash()
        self.write_faults = WriteFaults()
        self.ymodem: YmodemReceiver | None = None
        self.ymodem_faults = Faults()
        self.ymodem_received: list[tuple[str, int, bytes]] = []

    # -- memory ----------------------------------------------------------------

    def read_memory(self, address: int, length: int) -> bytes:
        offset = window_offset(address)
        if offset is not None:
            return self.flash.read(offset, length)
        base = physical(address)
        for start, content in self.ram.items():
            if start <= base < start + len(content):
                return content[base - start:base - start + length]
        return bytes(length)

    def memory_image(self, address: int) -> bytes | None:
        offset = window_offset(address)
        if offset is not None:
            return self.flash.read(offset, SIZE - offset)
        return super().memory_image(address)

    # -- YMODEM ----------------------------------------------------------------

    def start_ymodem(self, address: int) -> None:
        def done(name: str, length: int, data: bytes) -> None:
            self.ymodem = None
            self.ymodem_received.append((name, length, data))
            self.ram_store(address, data)
            self.model.env["filesize"] = "%x" % length
            self.resume()

        def cancelled() -> None:
            self.ymodem = None
            self.resume()

        self.ymodem = YmodemReceiver(
            self.clock, lambda data: self.emit(data.decode("latin-1")), self.ymodem_faults,
            on_done=done, on_cancel=cancelled, on_reset=lambda: self.restart("test"))
        self.ymodem.start()

    def on_busy_input(self, byte: int) -> None:
        if self.ymodem is not None:
            self.ymodem.feed(bytes([byte]))
        else:
            super().on_busy_input(byte)

    def set_pane_attached(self, attached: bool) -> None:
        """The pane's client was stopped or restarted; a restart prints a marker."""
        self.pane_attached = attached
        if attached:
            self.marker("connected")


class BoardPort:
    """A raw port to the board's UART."""

    def __init__(self, board: FlashBoard, clock) -> None:
        self.board = board
        self.clock = clock
        self.closed = False
        board.raw_output.clear()  # opening the port discards pending input

    def read(self, count: int) -> bytes:
        if not self.board.raw_output:
            self.clock.advance(0.1)
        data = bytes(self.board.raw_output[:count])
        del self.board.raw_output[:count]
        return data

    def write(self, data: bytes) -> None:
        self.board.receive(data)

    def close(self) -> None:
        self.closed = True

