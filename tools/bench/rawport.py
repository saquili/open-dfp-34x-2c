# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Raw ports to the board's UART, and a console over one.

A YMODEM transfer needs the UART's byte stream to itself, so for its length
the tool reads and writes the port directly instead of through the shared
pane. A port is eight bits, no parity, one stop bit, no echo, no newline
translation, no flow control and no signals; ``read`` returns within about a
tenth of a second with what there is, or nothing; opening it discards pending
input. Two kinds are provided: a TCP connection to the bridge that serves the
UART, and a serial device that nothing else holds.

``RawConsole`` gives such a port the same small contract as the tmux console,
so that the loader driver, with its guard and echo check, runs unchanged over
it. Nothing the board prints in between reaches the shared transcript, so the
console keeps the bytes itself and logs what it hears, redacted, in the tool's
own log.

Over a bridge, the raw port is used only after the pane's client has been
stopped (``Console.released_pane``): a second connection to a bridge port while
the first is up either kills the first or merges its input onto the UART.
"""

from __future__ import annotations

import os
import socket
from typing import Protocol

from .console import (Clock, ConsoleCheck, ConsoleUnusable, ToolLog, classify, wait,
                      wait_quiet)

CONNECT_TIMEOUT = 10.0
READ_TIMEOUT = 0.1


class RawPort(Protocol):
    def read(self, count: int) -> bytes: ...

    def write(self, data: bytes) -> None: ...

    def close(self) -> None: ...


class TcpPort:
    """A TCP connection to a bridge port."""

    def __init__(self, host: str, port: int) -> None:
        try:
            self._socket = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
        except OSError as exc:
            raise ConsoleUnusable("cannot connect to the bridge port %s:%d: %s"
                                  % (host, port, exc)) from exc
        self._socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._socket.settimeout(READ_TIMEOUT)
        try:  # opening the port discards what is pending
            while self._socket.recv(4096):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def read(self, count: int) -> bytes:
        try:
            data = self._socket.recv(count)
        except (socket.timeout, BlockingIOError):
            return b""
        if not data:
            raise ConsoleUnusable("the bridge closed the connection")
        return data

    def write(self, data: bytes) -> None:
        self._socket.settimeout(10.0)
        try:
            self._socket.sendall(data)
        finally:
            self._socket.settimeout(READ_TIMEOUT)

    def close(self) -> None:
        self._socket.close()


class SerialPort:
    """A serial device at 115200 baud, 8N1, raw."""

    def __init__(self, device: str) -> None:
        import termios
        try:
            self._fd = os.open(device, os.O_RDWR | os.O_NOCTTY)
        except OSError as exc:
            raise ConsoleUnusable("cannot open the serial device %s: %s"
                                  % (device, exc.strerror)) from exc
        try:
            attrs = termios.tcgetattr(self._fd)
            attrs[0] = 0  # input flags: nothing translated, no flow control
            attrs[1] = 0  # output flags: no processing
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[3] = 0  # local flags: no echo, not canonical, no signals
            attrs[4] = attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 1  # tenths of a second
            termios.tcsetattr(self._fd, termios.TCSANOW, attrs)
            termios.tcflush(self._fd, termios.TCIFLUSH)
        except BaseException:
            os.close(self._fd)
            raise

    def read(self, count: int) -> bytes:
        return os.read(self._fd, count)

    def write(self, data: bytes) -> None:
        view = memoryview(data)
        while view:
            view = view[os.write(self._fd, view):]

    def close(self) -> None:
        os.close(self._fd)


_KEYS = {"Enter": b"\r", "C-c": b"\x03"}


class RawConsole:
    """The console contract over a raw port.

    ``mark`` and ``since`` read from the bytes heard so far; each call first
    takes in whatever the port has. The port's own read timeout is the pause
    of a wait loop, so ``poll`` is zero.
    """

    poll = 0.0

    def __init__(self, port: RawPort, clock: Clock, log: ToolLog | None = None) -> None:
        self.port = port
        self.clock = clock
        self.log = log or ToolLog()
        self._heard = bytearray()
        self._open = True

    def _pump(self) -> None:
        data = self.port.read(4096)
        if data:
            self._heard += data
            self.log.event("heard", data.decode("latin-1").replace("\r", ""))

    def mark(self) -> int:
        self._pump()
        return len(self._heard)

    def since(self, mark: int) -> str:
        self._pump()
        return bytes(self._heard[mark:]).decode("latin-1").replace("\r", "")

    def type_text(self, text: str) -> None:
        if not text:
            return
        if any(not 0x20 <= ord(ch) < 0x7F for ch in text):
            raise ConsoleUnusable("only printable ASCII may be typed")
        self.port.write(text.encode("latin-1"))

    def press(self, key: str, count: int = 1) -> None:
        if key not in _KEYS:
            raise ConsoleUnusable("the tool presses only Enter and Ctrl-C, not %r" % key)
        if count >= 1:
            self.port.write(_KEYS[key] * count)

    def alive(self) -> bool:
        return self._open

    def check_usable(self, settle: float = 0.2) -> ConsoleCheck:
        """The same check as the tmux console's: an empty line must draw a byte."""
        mark = self.mark()
        self.press("Enter")
        try:
            wait(self, self.clock, mark, [lambda _text: "byte" if self.mark() > mark else None],
                 2.0, what="any byte after an empty line")
        except Exception as exc:
            raise ConsoleUnusable("the serial console is silent: no byte within 2 s of an "
                                  "empty line") from exc
        wait_quiet(self, self.clock, settle, 5.0)
        return ConsoleCheck(classify(self.since(mark)), self.since(mark), mark)

    def close(self) -> None:
        if self._open:
            self._open = False
            self.port.close()
