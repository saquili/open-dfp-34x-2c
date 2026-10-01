# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A YMODEM receiver as the loader behaves, with faults that can be injected.

The receiver asks for CRC mode with ``C`` at once and every three seconds
until a block starts; checks each block's complement, CRC and sequence; answers
a bad block with NAK and a repeated block with ACK without storing it twice;
answers the first end-of-file with NAK and the second with ACK; asks for
``C`` again after the header block and after the end of the file; acknowledges
the empty header block; and prints the loader's summary lines.

Faults: a corrupted byte (the CRC fails), a lost block (the sender times out),
a lost acknowledgement, an acknowledgement that arrives late, cancel bytes
from the receiver, silence after a block, and a reset during the transfer.
Time is virtual: the line is slow (11 520 bytes a second), the receiver's
timer is the clock's.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field

from bench.ymodem import ACK, BLOCK, CAN, CRC_REQUEST, EOT, NAK, SOH, STX, crc16

from .simclock import VirtualClock

BYTES_PER_SECOND = 11520.0


@dataclass
class Faults:
    """Faults, each keyed by the data block's sequence number (block 0 is the header)."""

    corrupt: set[int] = field(default_factory=set)  # one bit flipped, once
    lose: set[int] = field(default_factory=set)  # the block is never seen, once
    drop_ack: set[int] = field(default_factory=set)  # the first ACK is not sent
    delay_ack: dict[int, float] = field(default_factory=dict)  # the first ACK comes late
    cancel_after: int | None = None
    silence_after: int | None = None
    reset_after: int | None = None


class YmodemReceiver:
    """The receiving end. ``emit`` carries what it says to the sender."""

    def __init__(self, clock: VirtualClock, emit: Callable[[bytes], None],
                 faults: Faults | None = None, *,
                 on_done: Callable[[str, int, bytes], None] | None = None,
                 on_cancel: Callable[[], None] | None = None,
                 on_reset: Callable[[], None] | None = None) -> None:
        self.clock = clock
        self.emit = emit
        self.faults = faults or Faults()
        self.on_done = on_done
        self.on_cancel = on_cancel
        self.on_reset = on_reset
        self.state = "idle"  # idle, header, data, eot, end, done, silent, cancelled
        self.epoch = 0
        self._buffer = bytearray()
        self._cancels = 0
        self.name = ""
        self.length: int | None = None
        self.stored = bytearray()
        self.expected = 1
        self.packets = {"soh": 0, "stx": 0, "can": 0}
        self.retries = 2
        self.acks_sent = 0
        self.frames_seen: list[tuple[int, int]] = []  # (lead, sequence) of every good frame
        self.duplicates = 0
        self.naks = 0
        self._used: set[tuple[str, int]] = set()

    # -- starting --------------------------------------------------------------

    def start(self) -> None:
        self.state = "header"
        self._ask_for_crc(self.epoch)

    def _ask_for_crc(self, epoch: int) -> None:
        if epoch != self.epoch or self.state != "header" or self.frames_seen:
            return
        self.emit(bytes([CRC_REQUEST]))
        self.clock.schedule(3.0, lambda: self._ask_for_crc(epoch))

    # -- input -----------------------------------------------------------------

    def feed(self, data: bytes) -> None:
        """Bytes from the sender, which takes time on a 115200-baud line."""
        self.clock.advance(len(data) / BYTES_PER_SECOND)
        for byte in data:
            self._byte(byte)

    def _byte(self, byte: int) -> None:
        if self.state in ("idle", "done", "silent", "cancelled"):
            return
        if not self._buffer:  # between frames
            if byte == CAN:
                self._cancels += 1
                if self._cancels >= 2:
                    self._cancel()
                return
            self._cancels = 0
            if byte == EOT:
                self._end_of_file()
                return
            if byte not in (SOH, STX):
                return  # noise
        self._buffer.append(byte)
        size = 3 + (128 if self._buffer[0] == SOH else BLOCK) + 2
        if len(self._buffer) == size:
            block = bytes(self._buffer)
            self._buffer.clear()
            self._block(block)

    def _once(self, kind: str, sequence: int, wanted: set[int] | dict[int, float]) -> bool:
        """Is this the first time a fault of this kind applies to this block?"""
        key = (kind, sequence)
        if sequence in wanted and key not in self._used:
            self._used.add(key)
            return True
        return False

    def _block(self, block: bytes) -> None:
        sequence, complement = block[1], block[2]
        payload = block[3:-2]
        if self._once("lose", sequence, self.faults.lose):
            return
        if self._once("corrupt", sequence, self.faults.corrupt):
            payload = bytes([payload[0] ^ 0x01]) + payload[1:]
        crc_ok = crc16(payload) == (block[-2] << 8 | block[-1])
        if sequence ^ complement != 0xFF or not crc_ok:
            self.retries += 1
            self.naks += 1
            self.emit(bytes([NAK]))
            return
        if self.state == "header":
            self._header(block[0], sequence, payload)
        elif self.state == "end":
            self._empty_header(sequence, payload)
        elif self.state == "data":
            self._data(block[0], sequence, payload)
        else:
            self.emit(bytes([NAK]))

    def _acknowledge(self, sequence: int) -> None:
        if self.state == "silent":
            return
        if self._once("drop", sequence, self.faults.drop_ack):
            return
        if self._once("delay", sequence, self.faults.delay_ack):
            delay = self.faults.delay_ack[sequence]
            self.clock.schedule(delay, lambda: self.emit(bytes([ACK])))
            return
        self.acks_sent += 1
        self.emit(bytes([ACK]))

    def _header(self, lead: int, sequence: int, payload: bytes) -> None:
        if sequence != 0:
            self.emit(bytes([NAK]))
            return
        name, _sep, rest = payload.partition(b"\0")
        digits = rest.split(b"\0", 1)[0]
        self.name = name.decode("latin-1")
        self.length = int(digits) if digits.isdigit() else None
        self.packets["soh"] += 1
        self.frames_seen.append((lead, 0))
        self.state = "data"
        self._acknowledge(0)
        self.emit(bytes([CRC_REQUEST]))
        self._after(0)

    def _data(self, lead: int, sequence: int, payload: bytes) -> None:
        if lead == SOH and sequence == 0 and self.expected == 1:
            self.duplicates += 1  # the header again: its acknowledgement was lost
            self._acknowledge(0)
            return
        if sequence == (self.expected - 1) & 0xFF and self.expected > 1:
            self.duplicates += 1  # a repeat of the block just stored
            self._acknowledge(self.expected - 1)
            return
        if sequence != self.expected & 0xFF:
            self.retries += 1
            self.naks += 1
            self.emit(bytes([NAK]))
            return
        self.stored += payload
        self.packets["stx"] += 1
        self.frames_seen.append((lead, sequence))
        number = self.expected
        self.expected += 1
        self._acknowledge(number)
        self._after(number)

    def _after(self, number: int) -> None:
        if self.faults.cancel_after == number:
            self.emit(bytes([CAN, CAN]))
            self._cancel()
        elif self.faults.silence_after == number:
            self.state = "silent"
        elif self.faults.reset_after == number:
            self.state = "silent"
            if self.on_reset is not None:
                self.on_reset()

    def _end_of_file(self) -> None:
        if self.state == "data":
            self.state = "eot"
            self.emit(bytes([NAK]))
        elif self.state == "eot":
            self.state = "end"
            self.emit(bytes([ACK]))
            self.emit(bytes([CRC_REQUEST]))

    def _empty_header(self, sequence: int, payload: bytes) -> None:
        if sequence != 0 or any(payload):
            self.emit(bytes([NAK]))
            return
        self.packets["soh"] += 1
        self.emit(bytes([ACK]))
        self.state = "done"
        data = bytes(self.stored)
        length = len(data) if self.length is None else self.length
        self.emit(("xyzModem - CRC mode, %d(SOH)/%d(STX)/%d(CAN) packets, %d retries\r\r\n"
                   % (self.packets["soh"], self.packets["stx"], self.packets["can"],
                      self.retries)).encode())
        self.emit(("## Total Size      = 0x%08x = %d Bytes\r\r\n" % (length, length)).encode())
        if self.on_done is not None:
            self.on_done(self.name, length, data[:length])

    def _cancel(self) -> None:
        self.packets["can"] += 2
        self.state = "cancelled"
        if self.on_cancel is not None:
            self.on_cancel()


class LoopbackPort:
    """A raw port wired straight to a receiver, for the sender's own tests."""

    def __init__(self, clock: VirtualClock, faults: Faults | None = None) -> None:
        self.clock = clock
        self.heard = bytearray()  # what the receiver said, waiting to be read
        self.sent = bytearray()
        self.writes: list[bytes] = []  # each write, as the sender made it
        self.done: list[tuple[str, int, bytes]] = []
        self.receiver = YmodemReceiver(
            clock, self.heard.extend, faults,
            on_done=lambda name, length, data: self.done.append((name, length, data)))
        self.closed = False

    def start(self) -> None:
        self.receiver.start()

    def read(self, count: int) -> bytes:
        if not self.heard:
            self.clock.advance(0.1)
        data = bytes(self.heard[:count])
        del self.heard[:count]
        return data

    def write(self, data: bytes) -> None:
        self.sent += data
        self.writes.append(bytes(data))
        self.receiver.feed(data)

    def close(self) -> None:
        self.closed = True
