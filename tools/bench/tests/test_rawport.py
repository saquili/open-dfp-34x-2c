# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The raw ports and the console contract over one."""

from __future__ import annotations

import os
import socket
import termios
import unittest
from unittest import mock

from bench import console as con
from bench.console import ConsoleUnusable, ToolLog
from bench.rawport import RawConsole, SerialPort, TcpPort

from .simclock import VirtualClock


class FakePort:
    """A port with a queue of what the board says."""

    def __init__(self) -> None:
        self.incoming: list[bytes] = []
        self.written: list[bytes] = []
        self.closed = False

    def read(self, count: int) -> bytes:
        return self.incoming.pop(0)[:count] if self.incoming else b""

    def write(self, data: bytes) -> None:
        self.written.append(data)

    def close(self) -> None:
        self.closed = True


class RawConsoleContract(unittest.TestCase):

    def setUp(self) -> None:
        self.port = FakePort()
        self.clock = VirtualClock()
        self.lines: list[str] = []
        self.console = RawConsole(self.port, self.clock, ToolLog(self.lines.append))

    def test_mark_and_since_work_on_what_has_been_heard(self) -> None:
        self.port.incoming.append(b"old\r\r\n9601D# ")
        mark = self.console.mark()
        self.assertEqual(mark, 13)
        self.assertEqual(self.console.since(mark), "")
        self.port.incoming.append(b"new\r\r\nmore")
        self.assertEqual(self.console.since(mark), "new\nmore")

    def test_a_wait_runs_over_it_without_a_pause_of_its_own(self) -> None:
        self.assertEqual(self.console.poll, 0.0)
        mark = self.console.mark()
        self.port.incoming.append(b"\r\r\n9601D# ")
        seen = con.wait(self.console, self.clock, mark, [con.pattern("p", r"9601D# $")], 1.0)
        self.assertEqual(seen.label, "p")

    def test_typing_goes_to_the_port_as_bytes(self) -> None:
        self.console.type_text("loady 80000000")
        self.console.press("Enter")
        self.console.press("C-c", count=2)
        self.console.press("Enter", count=0)
        self.assertEqual(self.port.written, [b"loady 80000000", b"\r", b"\x03\x03"])

    def test_the_same_restrictions_as_the_tmux_console(self) -> None:
        for text in ("a\nb", "café", "\x03"):
            with self.subTest(text=text), self.assertRaises(ConsoleUnusable):
                self.console.type_text(text)
        for key in ("Tab", "C-d", "enter"):
            with self.subTest(key=key), self.assertRaises(ConsoleUnusable):
                self.console.press(key)
        self.console.type_text("")
        self.assertEqual(self.port.written, [])

    def test_the_echo_check_works_over_it(self) -> None:
        class Echo(FakePort):
            def write(inner, data: bytes) -> None:  # noqa: N805
                super().write(data)
                inner.incoming.append(data if data != b"\r" else b"\r\r\n9601D# ")

        port = Echo()
        console = RawConsole(port, self.clock)
        mark = con.type_line(console, self.clock, "printenv")
        self.assertEqual(port.written, [b"printenv", b"\r"])
        self.assertEqual(console.since(mark), "\n9601D# ")

    def test_what_is_heard_is_logged_without_a_mac_address(self) -> None:
        self.port.incoming.append(b"ethaddr=00:00:5e:00:53:01\r\r\nhello 00:00:5E:00:53:02\r\r\n")
        self.console.mark()
        text = "\n".join(self.lines)
        self.assertIn("hello", text)
        self.assertNotIn("00:00:5e", text.lower())
        self.assertNotIn("ethaddr", text)

    def test_close_closes_the_port_once(self) -> None:
        self.assertTrue(self.console.alive())
        self.console.close()
        self.console.close()
        self.assertFalse(self.console.alive())
        self.assertTrue(self.port.closed)


class Tcp(unittest.TestCase):

    def pair(self) -> tuple[socket.socket, socket.socket]:
        """A connected pair of loopback TCP sockets: the tool's end and the bridge's."""
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        self.addCleanup(listener.close)
        client = socket.create_connection(listener.getsockname())
        server, _ = listener.accept()
        self.addCleanup(client.close)
        self.addCleanup(server.close)
        return client, server

    def open_port(self, client: socket.socket) -> TcpPort:
        with mock.patch("socket.create_connection", return_value=client):
            return TcpPort("127.0.0.1", 2001)

    def test_pending_input_is_discarded_on_opening(self) -> None:
        client, server = self.pair()
        server.sendall(b"stale bytes")
        port = self.open_port(client)
        self.assertEqual(port.read(100), b"")

    def test_data_arrives_and_a_quiet_line_gives_nothing(self) -> None:
        client, server = self.pair()
        port = self.open_port(client)
        server.sendall(b"C")
        self.assertEqual(port.read(10), b"C")
        self.assertEqual(port.read(10), b"")

    def test_a_write_reaches_the_bridge_in_one_piece(self) -> None:
        client, server = self.pair()
        port = self.open_port(client)
        port.write(b"x" * 1029)
        server.settimeout(2.0)
        got = b""
        while len(got) < 1029:
            got += server.recv(4096)
        self.assertEqual(got, b"x" * 1029)

    def test_nodelay_is_set(self) -> None:
        client, _server = self.pair()
        port = self.open_port(client)
        self.assertTrue(port._socket.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY))

    def test_a_closed_connection_is_an_error_not_silence(self) -> None:
        client, server = self.pair()
        port = self.open_port(client)
        server.close()
        with self.assertRaises(ConsoleUnusable):
            port.read(10)

    def test_a_refused_connection_is_reported(self) -> None:
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        free = sock.getsockname()[1]
        sock.close()
        with self.assertRaises(ConsoleUnusable) as caught:
            TcpPort("127.0.0.1", free)
        self.assertIn(str(free), str(caught.exception))


class Serial(unittest.TestCase):

    def setUp(self) -> None:
        try:
            self.master, self.slave = os.openpty()
        except OSError:
            self.skipTest("no pseudo-terminals here")
        self.addCleanup(os.close, self.master)
        self.addCleanup(os.close, self.slave)
        self.path = os.ttyname(self.slave)

    def test_the_port_is_raw_8n1_with_a_read_timeout_of_a_tenth_of_a_second(self) -> None:
        port = SerialPort(self.path)
        self.addCleanup(port.close)
        attrs = termios.tcgetattr(self.slave)
        self.assertFalse(attrs[3] & (termios.ECHO | termios.ICANON | termios.ISIG))
        self.assertFalse(attrs[0] & (termios.IXON | termios.IXOFF | termios.ICRNL))
        self.assertFalse(attrs[1] & termios.OPOST)
        self.assertEqual(attrs[2] & termios.CSIZE, termios.CS8)
        self.assertFalse(attrs[2] & (termios.PARENB | termios.CSTOPB))
        self.assertEqual(attrs[6][termios.VMIN], 0)
        self.assertEqual(attrs[6][termios.VTIME], 1)

    def test_bytes_pass_both_ways_untranslated(self) -> None:
        port = SerialPort(self.path)
        self.addCleanup(port.close)
        port.write(b"\x03\r\n\x1a\xff")
        self.assertEqual(os.read(self.master, 16), b"\x03\r\n\x1a\xff")
        os.write(self.master, b"C\r")
        self.assertEqual(port.read(16), b"C\r")
        self.assertEqual(port.read(16), b"")

    def test_a_device_that_cannot_be_opened_is_reported(self) -> None:
        with self.assertRaises(ConsoleUnusable):
            SerialPort("/dev/no-such-serial-device")


if __name__ == "__main__":
    unittest.main()
