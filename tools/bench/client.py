# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The console client: the process that a tmux pane runs.

The board's UART is served by a network bridge, one raw TCP connection per
port and no line-parameter control. The pane's process connects its terminal to
that connection, so that a person types into the board and watches it, and
tmux mirrors the pane's output into the transcript the tool reads.

It is one process with one select loop and no threads. Its terminal is raw for
input, so that a key reaches the board as itself (Enter stays a carriage
return, Ctrl-C and Ctrl-] are bytes like any other); output is left alone, so
the pane's terminal still adds the carriage return to each line feed. It has no
exit key: one leaves by detaching from the pane. The connection has a ten-second
connect timeout and ``TCP_NODELAY``; a receive that returns nothing means the
connection is gone.

When the bridge drops the connection the client says so on its own line (a
marker, ``--- console: <text> at HH:MM:SS ---``), waits, and connects again.
The wait is ``min(20, max(2, 2 x F))`` seconds, F counting the consecutive
connections that lasted less than three seconds, so a bridge that flaps
quickly is not hammered. Keys typed during the wait stay in the terminal's
input and are delivered after the reconnect, which may be into another state
of the board; bytes the board prints during the gap are lost. A client that is
a terminal program in a restart loop looks alive after a dropped connection,
because only the program's reader dies, so this one does not run another
program: when it fails, the pane's process is gone, which the tool can see.
"""

from __future__ import annotations

import os
import select
import socket
import sys
import time
from collections.abc import Callable
from typing import Any

from .console import Clock, SystemClock

CONNECT_TIMEOUT = 10.0
FAST = 3.0  # a connection shorter than this is a failure for the retry schedule
POLL = 1.0


def retry_delay(failures: int) -> float:
    """Seconds to wait before connecting again, after ``failures`` quick failures in a row."""
    return min(20.0, max(2.0, 2.0 * failures))


def _readable(fds: list[int], timeout: float) -> list[int]:
    return select.select(fds, [], [], timeout)[0]


class Client:
    """Connects the terminal ``tty_in``/``tty_out`` to the bridge at ``host:port``."""

    def __init__(self, host: str, port: int, *, tty_in: int = 0, tty_out: int = 1,
                 clock: Clock | None = None,
                 connect: Callable[..., Any] = socket.create_connection,
                 readable: Callable[[list[int], float], list[int]] = _readable,
                 wall: Callable[[], float] = time.time) -> None:
        self.host = host
        self.port = port
        self.tty_in = tty_in
        self.tty_out = tty_out
        self.clock = clock or SystemClock()
        self._connect = connect
        self._readable = readable
        self._wall = wall
        self.failures = 0

    # -- the terminal ----------------------------------------------------------

    def _marker(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S", time.localtime(self._wall()))
        os.write(self.tty_out, ("\n--- console: %s at %s ---\n" % (text, stamp)).encode("latin-1"))

    def _raw_input(self) -> Callable[[], None]:
        """Make the terminal raw for input; return the function that restores it."""
        if not os.isatty(self.tty_in):
            return lambda: None
        import termios
        saved = termios.tcgetattr(self.tty_in)
        raw = termios.tcgetattr(self.tty_in)
        raw[0] &= ~(termios.ICRNL | termios.INLCR | termios.IGNCR | termios.IXON | termios.IXOFF
                    | termios.BRKINT | termios.INPCK | termios.ISTRIP | termios.PARMRK)
        raw[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG | termios.IEXTEN)
        raw[6][termios.VMIN] = 1
        raw[6][termios.VTIME] = 0
        termios.tcsetattr(self.tty_in, termios.TCSANOW, raw)

        def restore() -> None:
            try:
                termios.tcsetattr(self.tty_in, termios.TCSANOW, saved)
            except (termios.error, OSError):
                pass  # the terminal is gone, so there is nothing left to restore

        return restore

    # -- the loop --------------------------------------------------------------

    def _pump(self, sock: Any) -> bool:
        """Move bytes both ways until the connection ends; False if the terminal closed."""
        while True:
            for fd in self._readable([self.tty_in, sock], POLL):
                if fd == self.tty_in:
                    try:
                        data = os.read(self.tty_in, 4096)
                    except OSError:
                        return False  # a hung-up terminal reads as an error on some systems
                    if not data:
                        return False
                    sock.sendall(data)
                else:
                    try:
                        data = sock.recv(4096)
                    except OSError:
                        return True
                    if not data:
                        return True
                    try:
                        os.write(self.tty_out, data)
                    except OSError:
                        return False

    def run(self, stop: Callable[[], bool] = lambda: False) -> None:
        """Connect, pump, and reconnect until the terminal closes or ``stop`` says so."""
        restore = self._raw_input()
        try:
            while not stop():
                started = self.clock.now()
                try:
                    sock = self._connect((self.host, self.port), timeout=CONNECT_TIMEOUT)
                except OSError:
                    self._marker("link failed")
                    sock = None
                if sock is not None:
                    try:
                        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        sock.settimeout(None)
                        self._marker("connected")
                        started = self.clock.now()
                        if not self._pump(sock):
                            return
                    finally:
                        sock.close()
                    self._marker("the bridge closed the connection")
                self.failures = self.failures + 1 if self.clock.now() - started < FAST else 0
                delay = retry_delay(self.failures)
                self._marker("reconnecting in %d s" % delay)
                self.clock.sleep(delay)
        finally:
            restore()


def main(argv: list[str] | None = None) -> int:
    """``python3 -m bench console client HOST PORT``: the process for a tmux pane."""
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 2 or not args[1].isdigit():
        sys.stderr.write("usage: console client HOST PORT\n")
        return 2
    Client(args[0], int(args[1])).run()
    return 0
