# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A YMODEM sender with CRC-16, for the loader's ``loady``.

The sender follows the public protocol description (Forsberg, "XMODEM/YMODEM
Protocol Reference", 1988) and works on any object with ``read`` and ``write``,
so a simulated port can drive it. It sends a single file: after ``loady`` is
accepted the receiver asks for CRC mode with ``C`` bytes; the sender discards
everything else it hears and answers with a header block that carries the
file's name and length, then the data in 1024-byte blocks, stop-and-wait, then
the end of the file and the empty header block that ends the batch. The loader
reports completion itself, in its own summary lines, which the caller reads.

The port is raw: eight bits, no parity, one stop bit, no echo, no newline
translation, no flow control; ``read`` returns within about 0.1 s with what
there is, or nothing. There is no flow control, so stop-and-wait is what keeps
the receiver ahead of the sender.

Where the protocol cannot tell an answer from a late answer to an earlier
send, the sender accepts the first acknowledgement it sees, and the receiver
is expected to acknowledge a repeated block number without storing it twice.
"""

from __future__ import annotations

import binascii
import os
from collections.abc import Callable
from dataclasses import dataclass
from typing import Protocol

from .console import BenchError, Clock

SOH, STX, EOT, ACK, NAK, CAN, CRC_REQUEST = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43
PAD = 0x1A
BLOCK = 1024
HEADER_BLOCK = 128

HANDSHAKE_WAIT = 25.0
HEADER_SENDS, HEADER_WAIT = 5, 5.0
AFTER_HEADER_WAIT = 5.0
DATA_SENDS, DATA_WAIT = 6, 6.0
END_WAIT = 5.0
GIVE_UP_PAUSE = 2.0
PROGRESS_EVERY = 64
SEEN_BYTES = 200


class YmodemError(BenchError):
    """The transfer did not complete."""


class YmodemTimeout(YmodemError):
    """The receiver did not answer; ``seen`` is the last it said."""

    def __init__(self, message: str, seen: bytes = b"") -> None:
        super().__init__("%s; the last bytes heard: %r" % (message, seen))
        self.seen = seen


class YmodemAborted(YmodemError):
    """The receiver cancelled the transfer."""


class YmodemGaveUp(YmodemError):
    """A block was sent the allowed number of times without being acknowledged."""

    def __init__(self, block: int, offset: int) -> None:
        super().__init__("block %d (at offset %d) was not acknowledged; the transfer was "
                         "cancelled" % (block, offset))
        self.block = block
        self.offset = offset


class Port(Protocol):
    """The raw byte stream to the loader."""

    def read(self, count: int) -> bytes: ...

    def write(self, data: bytes) -> None: ...


def crc16(data: bytes, crc: int = 0) -> int:
    """CRC-16/XMODEM: polynomial 0x1021, initial value 0, no reflection, no final xor."""
    return binascii.crc_hqx(data, crc)


def frame(lead: int, sequence: int, payload: bytes) -> bytes:
    """A block: the lead byte, the sequence and its complement, the data, the CRC high byte first."""
    crc = crc16(payload)
    return (bytes([lead, sequence & 0xFF, 0xFF - (sequence & 0xFF)]) + payload
            + bytes([crc >> 8, crc & 0xFF]))


def header_block(name: str, length: int) -> bytes:
    """Block 0: the file's base name, NUL, its length in decimal, NUL, padding to 128 bytes."""
    base = os.path.basename(name)
    try:
        encoded = base.encode("ascii")
    except UnicodeEncodeError as exc:
        raise YmodemError("the file name %r is not ASCII" % base) from exc
    body = encoded + b"\0" + str(length).encode("ascii") + b"\0"
    if not base or len(body) > HEADER_BLOCK:
        raise YmodemError("the file name %r does not fit a header block" % base)
    return frame(SOH, 0, body.ljust(HEADER_BLOCK, b"\0"))


def data_block(sequence: int, chunk: bytes) -> bytes:
    """A data block of 1024 bytes; a short last chunk is padded with 0x1a."""
    return frame(STX, sequence, chunk.ljust(BLOCK, bytes([PAD])))


def end_block() -> bytes:
    """The empty header block that ends the batch."""
    return frame(SOH, 0, bytes(HEADER_BLOCK))


@dataclass(frozen=True)
class SendReport:
    """What the sender did."""

    blocks: int  # data blocks
    resends: int
    seconds: float
    length: int
    end_acknowledged: bool


class Sender:
    """Sends one file."""

    def __init__(self, port: Port, clock: Clock,
                 progress: Callable[[int, int], None] | None = None) -> None:
        self.port = port
        self.clock = clock
        self.progress = progress
        self._seen = bytearray()
        self._cancels = 0

    # -- reading -----------------------------------------------------------------

    def _byte(self, deadline: float) -> int | None:
        """One byte from the port, or None at the deadline.

        One byte at a time, so that nothing the receiver says after the last
        acknowledgement (its summary lines) is taken from the stream.
        """
        while True:
            data = self.port.read(1)
            if data:
                return data[0]
            if self.clock.now() >= deadline:
                return None

    def _await(self, wanted: tuple[int, ...], timeout: float) -> int | None:
        """The first wanted byte within ``timeout`` seconds; everything else is discarded.

        Two cancel bytes in a row abort the transfer.
        """
        deadline = self.clock.now() + timeout
        while True:
            byte = self._byte(deadline)
            if byte is None:
                return None
            self._seen.append(byte)
            del self._seen[:-SEEN_BYTES]
            if byte == CAN:
                self._cancels += 1
                if self._cancels >= 2:
                    raise YmodemAborted("the receiver cancelled the transfer")
                continue
            self._cancels = 0
            if byte in wanted:
                return byte

    # -- sending -----------------------------------------------------------------

    def _send_acknowledged(self, packet: bytes, sends: int, wait: float) -> tuple[bool, int]:
        """Send until acknowledged: a NAK resends at once, silence resends after ``wait``."""
        resends = -1
        for _ in range(sends):
            resends += 1
            self.port.write(packet)
            if self._await((ACK, NAK), wait) == ACK:
                return True, resends
        return False, resends

    def send(self, name: str, data: bytes) -> SendReport:
        """Send ``data`` as the file ``name``; raise ``YmodemError`` if it cannot be done."""
        if not data:
            raise YmodemError("there is nothing to send")
        started = self.clock.now()
        if self._await((CRC_REQUEST,), HANDSHAKE_WAIT) is None:
            raise YmodemTimeout("the receiver did not ask for CRC mode (no 'C') within %d s"
                                % HANDSHAKE_WAIT, bytes(self._seen))
        resends = 0
        done, extra = self._send_acknowledged(header_block(name, len(data)), HEADER_SENDS,
                                              HEADER_WAIT)
        resends += extra
        if not done:
            self._give_up(0, 0)
        self._await((CRC_REQUEST,), AFTER_HEADER_WAIT)  # goes on without it
        total = -(-len(data) // BLOCK)
        for number in range(1, total + 1):
            offset = (number - 1) * BLOCK
            done, extra = self._send_acknowledged(
                data_block(number, data[offset:offset + BLOCK]), DATA_SENDS, DATA_WAIT)
            resends += extra
            if not done:
                self._give_up(number, offset)
            if self.progress is not None and number % PROGRESS_EVERY == 0:
                self.progress(number, total)
        self._end_of_file()
        acknowledged = self._end_batch()
        return SendReport(total, resends, self.clock.now() - started, len(data), acknowledged)

    def _give_up(self, block: int, offset: int) -> None:
        """Cancel: two cancel bytes, a pause of two seconds, an Enter; then report where."""
        self.port.write(bytes([CAN, CAN]))
        self.clock.sleep(GIVE_UP_PAUSE)
        self.port.write(b"\r")
        raise YmodemGaveUp(block, offset)

    def _end_of_file(self) -> None:
        """End of file: the first EOT is answered with NAK, the second with ACK."""
        for _ in range(DATA_SENDS):
            self.port.write(bytes([EOT]))
            if self._await((ACK, NAK), END_WAIT) == ACK:
                return
        raise YmodemTimeout("the receiver did not acknowledge the end of the file",
                            bytes(self._seen))

    def _end_batch(self) -> bool:
        self._await((CRC_REQUEST,), END_WAIT)
        self.port.write(end_block())
        return self._await((ACK,), END_WAIT) == ACK
