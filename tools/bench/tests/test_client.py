# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The pane's console client: its reconnect schedule and its raw pass-through."""

from __future__ import annotations

import ast
import os
import re
import select
import socket
import termios
import threading
import time
import unittest
from unittest import mock

from bench import client
from bench.client import Client, retry_delay
from bench.console import Clock

from .simclock import VirtualClock


class Schedule(unittest.TestCase):

    def test_client_reconnect_schedule(self) -> None:
        """min(20, max(2, 2 x F)) seconds, F counting the quick failures in a row."""
        table = {0: 2, 1: 2, 2: 4, 3: 6, 4: 8, 5: 10, 9: 18, 10: 20, 11: 20, 500: 20}
        for failures, delay in table.items():
            self.assertEqual(retry_delay(failures), delay, failures)


class FakeSocket:
    """A connection that lives ``lifetime`` virtual seconds and then closes."""

    def __init__(self, clock: VirtualClock, lifetime: float) -> None:
        self.clock = clock
        self.lifetime = lifetime
        self.options: list[tuple[int, int, int]] = []
        self.timeout: object = "unset"
        self.closed = False

    def setsockopt(self, level: int, name: int, value: int) -> None:
        self.options.append((level, name, value))

    def settimeout(self, value: object) -> None:
        self.timeout = value

    def recv(self, count: int) -> bytes:
        self.clock.advance(self.lifetime)
        return b""

    def sendall(self, data: bytes) -> None:
        raise AssertionError("nothing was typed")

    def close(self) -> None:
        self.closed = True


class Loop(unittest.TestCase):

    def setUp(self) -> None:
        self.clock = VirtualClock()
        self.read, self.write = os.pipe()
        self.addCleanup(os.close, self.read)
        self.addCleanup(os.close, self.write)
        self.sockets: list[FakeSocket] = []
        self.connect_args: list[tuple] = []

    def markers(self) -> list[str]:
        os.set_blocking(self.read, False)
        try:
            text = os.read(self.read, 65536).decode("latin-1")
        except BlockingIOError:
            text = ""
        return re.findall(r"--- console: (.+?) at \d\d:\d\d:\d\d ---", text)

    def run_client(self, plan: list[float | None], count: int | None = None) -> Client:
        """Run until every step of the plan has been used: a number is a connection that lasts
        that many seconds, ``None`` a connection that fails at once."""
        steps = list(plan)

        def connect(address, timeout):
            self.connect_args.append((address, timeout))
            step = steps.pop(0)
            if step is None:
                raise ConnectionRefusedError("no bridge")
            sock = FakeSocket(self.clock, step)
            self.sockets.append(sock)
            return sock

        instance = Client("192.0.2.9", 2001, tty_in=self.read, tty_out=self.write,
                          clock=self.clock, connect=connect,
                          readable=lambda fds, timeout: [fds[1]], wall=lambda: 0.0)
        instance.run(stop=lambda: not steps)
        return instance

    def test_the_wait_grows_with_quick_failures_and_resets_after_a_connection_that_lasted(self) -> None:
        self.run_client([1, 1, 1, 5, 1])
        self.assertEqual(self.clock.slept, 2 + 4 + 6 + 2 + 2)
        names = self.markers()
        self.assertEqual(names[:3], ["connected", "the bridge closed the connection",
                                     "reconnecting in 2 s"])
        self.assertEqual([n for n in names if n.startswith("reconnecting")],
                         ["reconnecting in 2 s", "reconnecting in 4 s", "reconnecting in 6 s",
                          "reconnecting in 2 s", "reconnecting in 2 s"])

    def test_failed_connections_back_off_to_twenty_seconds(self) -> None:
        self.run_client([None] * 12)
        names = self.markers()
        self.assertEqual(names.count("link failed"), 12)
        delays = [int(n.split()[2]) for n in names if n.startswith("reconnecting")]
        self.assertEqual(delays, [2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 20])
        self.assertNotIn("connected", names)

    def test_the_connection_has_a_ten_second_timeout_and_nodelay(self) -> None:
        self.run_client([1])
        self.assertEqual(self.connect_args, [(("192.0.2.9", 2001), 10.0)])
        sock = self.sockets[0]
        self.assertEqual(sock.options, [(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)])
        self.assertIsNone(sock.timeout, "blocking once connected")
        self.assertTrue(sock.closed)

    def test_a_connection_that_fails_slowly_is_not_a_quick_failure(self) -> None:
        def slow(address, timeout):
            self.clock.advance(10.0)
            raise TimeoutError("timed out")

        steps = [1, 2, 3]
        instance = Client("192.0.2.9", 2001, tty_in=self.read, tty_out=self.write,
                          clock=self.clock, connect=slow, readable=lambda f, t: [f[1]],
                          wall=lambda: 0.0)
        instance.run(stop=lambda: not steps.pop() if steps else True)
        delays = [int(n.split()[2]) for n in self.markers() if n.startswith("reconnecting")]
        self.assertTrue(delays and set(delays) == {2}, delays)

    def test_the_client_runs_no_threads_and_has_no_exit_key(self) -> None:
        with open(client.__file__, encoding="utf-8") as handle:
            tree = ast.parse(handle.read())
        imported = {alias.name for node in ast.walk(tree) if isinstance(node, ast.Import)
                    for alias in node.names}
        self.assertNotIn("threading", imported)
        self.assertNotIn("subprocess", imported)

    def test_main_checks_its_arguments(self) -> None:
        with mock.patch("sys.stderr"):
            self.assertEqual(client.main([]), 2)
            self.assertEqual(client.main(["host", "not-a-port"]), 2)
        with mock.patch.object(Client, "run") as run:
            self.assertEqual(client.main(["192.0.2.9", "2001"]), 0)
        run.assert_called_once()


class QuickClock(Clock):
    """Real time, but a wait of any length takes a few milliseconds."""

    def __init__(self) -> None:
        self.waits: list[float] = []

    def now(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        self.waits.append(seconds)
        time.sleep(0.01)


class PassThrough(unittest.TestCase):
    """A real pseudo-terminal and a real loopback listener, with the client in a thread."""

    def setUp(self) -> None:
        try:
            self.master, self.slave = os.openpty()
        except OSError:
            self.skipTest("no pseudo-terminals here")
        self.master_closed = False
        self.addCleanup(os.close, self.slave)
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.addCleanup(self.listener.close)
        self.saved = termios.tcgetattr(self.slave)
        self.clock = QuickClock()
        self.client = Client("127.0.0.1", self.listener.getsockname()[1], tty_in=self.slave,
                             tty_out=self.slave, clock=self.clock)
        self.result: list[BaseException | None] = []
        self.stop = False
        self.thread = threading.Thread(target=self.run_client)
        self.thread.start()
        self.addCleanup(self.finish)
        self.conn, _ = self.listener.accept()
        self.conn.settimeout(3.0)
        self.addCleanup(self.conn.close)

    def run_client(self) -> None:
        try:
            self.client.run(lambda: self.stop)
            self.result.append(None)
        except BaseException as exc:  # noqa: BLE001
            self.result.append(exc)

    def close_master(self) -> None:
        if not self.master_closed:
            self.master_closed = True
            os.close(self.master)

    def finish(self) -> None:
        """End the client: closing its terminal makes it return."""
        self.close_master()
        self.thread.join(5)

    def read_master(self, until: bytes, limit: float = 3.0) -> bytes:
        data = b""
        deadline = time.monotonic() + limit
        while until not in data and time.monotonic() < deadline:
            if select.select([self.master], [], [], 0.1)[0]:
                data += os.read(self.master, 4096)
        return data

    def test_client_raw_passthrough(self) -> None:
        """Every byte typed reaches the bridge as itself, and the board's bytes come back."""
        self.assertIn(b"--- console: connected at", self.read_master(b"connected at"))
        typed = b"\x03\x1d\r\x7f\x04 hello\n\x1b[6n"
        os.write(self.master, typed)
        got = b""
        while len(got) < len(typed):
            got += self.conn.recv(100)
        self.assertEqual(got, typed, "Ctrl-C, Ctrl-], Enter as CR, DEL, Ctrl-D, ESC: all untouched")
        self.conn.sendall(b"U-Boot 2011.12\r\n9601D# ")
        printed = self.read_master(b"9601D# ")
        self.assertIn(b"U-Boot 2011.12\r\r\n9601D# ", printed,
                      "the pane's terminal adds a carriage return to each line feed")
        self.assertNotIn(b"hello", printed, "typed keys are not echoed locally")

    def test_the_terminal_is_raw_for_input_only_while_the_client_runs(self) -> None:
        self.read_master(b"connected at")
        running = termios.tcgetattr(self.slave)
        self.assertFalse(running[3] & (termios.ECHO | termios.ICANON | termios.ISIG))
        self.assertFalse(running[0] & (termios.ICRNL | termios.IXON))
        self.assertTrue(running[1] & termios.OPOST, "output processing is left alone")
        self.assertEqual(running[6][termios.VMIN], 1)
        self.stop = True  # end the client while its terminal still exists, so that the
        self.conn.close()  # restored settings can be read back on every system
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        changed_lflag = termios.ECHO | termios.ICANON | termios.ISIG | termios.IEXTEN
        changed_iflag = (termios.ICRNL | termios.INLCR | termios.IGNCR | termios.IXON
                         | termios.IXOFF | termios.BRKINT | termios.INPCK | termios.ISTRIP
                         | termios.PARMRK)
        restored = termios.tcgetattr(self.slave)
        self.assertEqual(restored[3] & changed_lflag, self.saved[3] & changed_lflag)
        self.assertEqual(restored[0] & changed_iflag, self.saved[0] & changed_iflag)
        self.assertTrue(restored[3] & termios.ICANON, "canonical mode is back")

    def test_a_dropped_connection_is_reported_and_the_client_reconnects(self) -> None:
        self.read_master(b"connected at")
        self.conn.close()
        text = self.read_master(b"reconnecting in")
        self.assertIn(b"--- console: the bridge closed the connection at", text)
        self.assertIn(b"--- console: reconnecting in 2 s at", text)
        self.listener.settimeout(5.0)
        again, _ = self.listener.accept()
        self.addCleanup(again.close)
        self.assertEqual(self.clock.waits[:1], [2.0])
        os.write(self.master, b"after")
        again.settimeout(3.0)
        self.assertEqual(again.recv(10), b"after")

    def test_closing_the_terminal_ends_the_client(self) -> None:
        self.read_master(b"connected at")
        self.close_master()
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.result, [None])


if __name__ == "__main__":
    unittest.main()
