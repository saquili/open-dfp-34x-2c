# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The TFTP server: packets, refusals, retransmission and real loopback transfers."""

from __future__ import annotations

import ast
import contextlib
import io
import os
import random
import socket
import struct
import threading
import unittest

from bench import tftp
from bench.tftp import Server, Transfer, parse_request

from .support import TempDirTestCase

CLIENT = ("192.0.2.2", 40000)
OTHER = ("192.0.2.3", 40001)


def rrq(name: str, mode: str = "octet", **options: str) -> bytes:
    """A read request packet."""
    body = name.encode() + b"\0" + mode.encode() + b"\0"
    for key, value in options.items():
        body += key.encode() + b"\0" + value.encode() + b"\0"
    return struct.pack(">H", 1) + body


def ack(block: int) -> bytes:
    return struct.pack(">HH", 4, block & 0xFFFF)


def start(data: bytes, packet: bytes, client=CLIENT) -> Transfer:
    return Transfer(io.BytesIO(data), len(data), parse_request(packet), client)


def error_text(packet: bytes) -> tuple[int, str]:
    opcode, code = struct.unpack(">HH", packet[:4])
    assert opcode == 5, packet
    return code, packet[4:-1].decode()


class Packets(unittest.TestCase):
    """The packets of the specification, byte for byte."""

    def test_tftp_options(self) -> None:
        image = bytes(range(256)) * 20
        request = b"\x00\x01uImage\x00octet\x00blksize\x001468\x00"
        self.assertEqual(rrq("uImage", blksize="1468"), request)
        transfer = start(image, request)
        self.assertEqual(transfer.first(), b"\x00\x06blksize\x001468\x00")
        data = transfer.on_packet(b"\x00\x04\x00\x00", CLIENT)
        self.assertEqual(data, b"\x00\x03\x00\x01" + image[:1468])

    def test_the_block_size_is_clamped_to_8_up_to_8192(self) -> None:
        for asked, granted in (("4", "8"), ("99999", "8192"), ("7", "8"), ("8", "8"),
                               ("8192", "8192"), ("8193", "8192"), ("1468", "1468"),
                               ("0", "8")):
            with self.subTest(asked=asked):
                transfer = start(b"x" * 100, rrq("f", blksize=asked))
                self.assertEqual(transfer.first(), b"\x00\x06blksize\x00%s\x00" % granted.encode())

    def test_tsize_is_answered_with_the_file_size_as_decimal_text(self) -> None:
        transfer = start(b"x" * 4395389, rrq("f", tsize="0"))
        self.assertEqual(transfer.first(), b"\x00\x06tsize\x004395389\x00")

    def test_both_options_are_answered_in_the_order_asked(self) -> None:
        transfer = start(b"x" * 99, rrq("f", tsize="0", blksize="1468"))
        self.assertEqual(transfer.first(), b"\x00\x06tsize\x0099\x00blksize\x001468\x00")

    def test_without_options_block_one_follows_directly_at_512_bytes(self) -> None:
        data = os.urandom(1300)
        transfer = start(data, rrq("f"))
        self.assertEqual(transfer.first(), b"\x00\x03\x00\x01" + data[:512])
        self.assertEqual(transfer.on_packet(ack(1), CLIENT), b"\x00\x03\x00\x02" + data[512:1024])
        self.assertEqual(transfer.on_packet(ack(2), CLIENT), b"\x00\x03\x00\x03" + data[1024:])
        self.assertIsNone(transfer.on_packet(ack(3), CLIENT))
        self.assertTrue(transfer.done)

    def test_unknown_options_are_left_out_and_bad_values_ignored(self) -> None:
        self.assertEqual(start(b"abc", rrq("f", windowsize="4")).first(),
                         b"\x00\x03\x00\x01abc")
        self.assertEqual(start(b"abc", rrq("f", blksize="abc")).first(), b"\x00\x03\x00\x01abc")
        self.assertEqual(start(b"abc", rrq("f", blksize="")).first(), b"\x00\x03\x00\x01abc")
        self.assertEqual(start(b"abc", rrq("f", timeout="5", blksize="16")).first(),
                         b"\x00\x06blksize\x0016\x00")

    def test_option_names_and_the_mode_compare_in_lower_case(self) -> None:
        request = parse_request(rrq("uImage", "OcTeT", BLKSIZE="1468"))
        self.assertEqual(request.mode, "octet")
        self.assertEqual(request.options, {"blksize": "1468"})

    def test_tftp_exact_multiple(self) -> None:
        """The last block is shorter than the block size, and empty on an exact multiple."""
        for blksize in (512, 1468):
            with self.subTest(blksize=blksize):
                data = os.urandom(blksize * 3)
                transfer = start(data, rrq("f", blksize=str(blksize)))
                transfer.first()
                blocks = [transfer.on_packet(ack(0), CLIENT)]
                for number in (1, 2, 3):
                    blocks.append(transfer.on_packet(ack(number), CLIENT))
                self.assertEqual([len(b) - 4 for b in blocks[:4]], [blksize] * 3 + [0])
                self.assertIsNone(transfer.on_packet(ack(4), CLIENT))
                self.assertTrue(transfer.done)

    def test_a_short_file_and_an_empty_file(self) -> None:
        short = start(b"abc", rrq("f", blksize="1468"))
        short.first()
        self.assertEqual(short.on_packet(ack(0), CLIENT), b"\x00\x03\x00\x01abc")
        self.assertIsNone(short.on_packet(ack(1), CLIENT))
        self.assertTrue(short.done)
        empty = start(b"", rrq("f"))
        self.assertEqual(empty.first(), b"\x00\x03\x00\x01")
        self.assertIsNone(empty.on_packet(ack(1), CLIENT))
        self.assertTrue(empty.done)

    def test_the_block_number_is_sixteen_bits_and_wraps(self) -> None:
        data = random.Random(7).randbytes(8 * 65540 + 3)
        transfer = start(data, rrq("f", blksize="8"))
        transfer.first()
        packet = transfer.on_packet(ack(0), CLIENT)
        received = bytearray()
        numbers = []
        while packet is not None:
            number = struct.unpack(">H", packet[2:4])[0]
            numbers.append(number)
            received += packet[4:]
            packet = transfer.on_packet(ack(number), CLIENT)
        self.assertTrue(transfer.done)
        self.assertEqual(bytes(received), data)
        self.assertEqual(numbers[65534:65538], [65535, 0, 1, 2])
        self.assertEqual(len(numbers), 65541)

    def test_tftp_retransmit(self) -> None:
        """A block is resent up to 5 times, then the transfer is given up."""
        self.assertEqual(tftp.TIMEOUT, 3.0)
        self.assertEqual(tftp.RETRIES, 5)
        transfer = start(os.urandom(2000), rrq("f"))
        first = transfer.first()
        for _ in range(5):
            self.assertEqual(transfer.on_timeout(), first)
        self.assertIsNone(transfer.on_timeout())
        self.assertEqual(transfer.failed, "no answer from the client")
        self.assertIsNone(transfer.on_packet(ack(1), CLIENT), "nothing more is sent")

    def test_an_acknowledgement_resets_the_resend_count(self) -> None:
        transfer = start(os.urandom(2000), rrq("f"))
        transfer.first()
        for _ in range(4):
            transfer.on_timeout()
        second = transfer.on_packet(ack(1), CLIENT)
        self.assertIsNotNone(second)
        for _ in range(5):
            self.assertEqual(transfer.on_timeout(), second)
        self.assertIsNone(transfer.on_timeout())

    def test_a_late_acknowledgement_of_an_old_block_is_ignored(self) -> None:
        transfer = start(os.urandom(2000), rrq("f"))
        transfer.first()
        second = transfer.on_packet(ack(1), CLIENT)
        self.assertIsNone(transfer.on_packet(ack(1), CLIENT), "no resend: the timer deals with it")
        self.assertEqual(transfer.on_timeout(), second)

    def test_the_option_acknowledgement_is_resent_too(self) -> None:
        transfer = start(os.urandom(100), rrq("f", blksize="16"))
        oack = transfer.first()
        self.assertEqual(transfer.on_timeout(), oack)

    def test_tftp_wrong_source(self) -> None:
        transfer = start(os.urandom(2000), rrq("f"))
        first = transfer.first()
        self.assertIsNone(transfer.on_packet(ack(1), OTHER))
        self.assertIsNone(transfer.on_packet(ack(1), (CLIENT[0], CLIENT[1] + 1)))
        self.assertIsNone(transfer.on_packet(struct.pack(">HH", 5, 0) + b"x\0", OTHER))
        self.assertFalse(transfer.done or transfer.failed)
        self.assertEqual(transfer.on_timeout(), first, "still waiting for the real client")
        self.assertIsNotNone(transfer.on_packet(ack(1), CLIENT))

    def test_an_error_from_the_client_ends_the_transfer(self) -> None:
        transfer = start(os.urandom(2000), rrq("f"))
        transfer.first()
        self.assertIsNone(transfer.on_packet(struct.pack(">HH", 5, 3) + b"disk full\0", CLIENT))
        self.assertEqual(transfer.failed, "the client ended the transfer")
        self.assertIsNone(transfer.on_timeout())

    def test_short_or_foreign_packets_are_ignored(self) -> None:
        transfer = start(os.urandom(2000), rrq("f"))
        transfer.first()
        for packet in (b"", b"\x00", b"\x00\x04\x00", struct.pack(">HH", 3, 1) + b"x"):
            self.assertIsNone(transfer.on_packet(packet, CLIENT))
        self.assertFalse(transfer.done or transfer.failed)


class Refusals(TempDirTestCase):
    """Each refusal is an error packet; one packet per row."""

    def setUp(self) -> None:
        super().setUp()
        self.served = os.path.join(self.tmp, "served")
        os.mkdir(self.served)
        with open(os.path.join(self.served, "uImage"), "wb") as handle:
            handle.write(b"image")

    def answer(self, packet: bytes) -> tuple[int, str]:
        try:
            request = parse_request(packet)
            tftp.open_regular(self.served, request.filename, request.mode)[0].close()
        except tftp.ProtocolError as error:
            return error.code, error.text
        return 0, "served"

    def test_tftp_refusals(self) -> None:
        cases = [
            ("write request", b"\x00\x02x\x00octet\x00", 2, "read-only server"),
            ("parent directory", rrq("../uImage"), 2, None),
            ("subdirectory", rrq("a/b"), 2, None),
            ("hidden file", rrq(".hidden"), 2, None),
            ("trailing space", rrq("uImage "), 2, None),
            ("65 letters", rrq("a" * 65), 2, None),
            ("64 letters, absent", rrq("a" * 64), 1, "no such file"),
            ("dot", rrq("."), 2, None),
            ("dot dot", rrq(".."), 2, None),
            ("empty name", rrq(""), 2, None),
            ("absolute", rrq("/etc/passwd"), 2, None),
            ("backslash", rrq("a\\b"), 2, None),
            ("a missing file", rrq("nothing"), 1, "no such file"),
            ("mail mode", rrq("uImage", "mail"), 4, None),
            ("no mode", b"\x00\x01uImage\x00", 4, None),
            ("no terminator", b"\x00\x01uImage\x00octet", 4, None),
            ("odd option strings", b"\x00\x01uImage\x00octet\x00blksize\x00", 4, None),
            ("too short", b"\x00", 4, None),
            ("a data packet", b"\x00\x03\x00\x01abc", 4, None),
            ("an acknowledgement", ack(1), 4, None),
            ("an unknown opcode", b"\x00\x09uImage\x00octet\x00", 4, None),
        ]
        for label, packet, code, text in cases:
            with self.subTest(label):
                got = self.answer(packet)
                self.assertEqual(got[0], code, label)
                if text is not None:
                    self.assertEqual(got[1], text, label)

    def test_the_error_packet_is_opcode_5_code_text_nul(self) -> None:
        self.assertEqual(tftp.error_packet(2, "read-only server"),
                         b"\x00\x05\x00\x02read-only server\x00")
        self.assertEqual(tftp.error_packet(1, "no such file"), b"\x00\x05\x00\x01no such file\x00")

    def test_a_symbolic_link_is_not_a_regular_file(self) -> None:
        outside = os.path.join(self.tmp, "outside")
        with open(outside, "wb") as handle:
            handle.write(b"secret")
        os.symlink(outside, os.path.join(self.served, "link"))
        os.symlink("uImage", os.path.join(self.served, "alias"))
        for name in ("link", "alias"):
            with self.subTest(name):
                self.assertEqual(self.answer(rrq(name)), (1, "no such file"))

    def test_a_directory_and_a_device_are_not_regular_files(self) -> None:
        os.mkdir(os.path.join(self.served, "sub"))
        self.assertEqual(self.answer(rrq("sub")), (1, "no such file"))
        if os.path.exists("/dev/null"):
            os.symlink("/dev/null", os.path.join(self.served, "null"))
            self.assertEqual(self.answer(rrq("null")), (1, "no such file"))

    def test_a_regular_file_is_served(self) -> None:
        self.assertEqual(self.answer(rrq("uImage")), (0, "served"))
        self.assertEqual(self.answer(rrq("uImage", "NETASCII")), (0, "served"))

    def test_a_file_without_read_permission_is_an_access_error(self) -> None:
        path = os.path.join(self.served, "locked")
        with open(path, "wb") as handle:
            handle.write(b"x")
        os.chmod(path, 0)
        if os.access(path, os.R_OK):
            self.skipTest("the tests run with enough privilege to read anything")
        self.assertEqual(self.answer(rrq("locked"))[0], 2)

    def test_netascii_conversion(self) -> None:
        self.assertEqual(tftp.to_netascii(b"a\nb\rc\r\n"), b"a\r\nb\r\0c\r\0\r\n")
        path = os.path.join(self.served, "text")
        with open(path, "wb") as handle:
            handle.write(b"x\ny")
        handle, size = tftp.open_regular(self.served, "text", "netascii")
        self.assertEqual((handle.read(), size), (b"x\r\ny", 4))
        handle, size = tftp.open_regular(self.served, "text", "octet")
        self.assertEqual((handle.read(), size), (b"x\ny", 3))
        handle.close()


class Loopback(TempDirTestCase):
    """A real server on an ephemeral port of the loopback address."""

    def setUp(self) -> None:
        super().setUp()
        self.served = os.path.join(self.tmp, "served")
        os.mkdir(self.served)
        self.image = random.Random(3).randbytes(1468 * 40 + 77)
        with open(os.path.join(self.served, "uImage"), "wb") as handle:
            handle.write(self.image)
        self.logs: list[str] = []
        self.server = Server(self.served, "127.0.0.1", 0, timeout=0.3, log=self.logs.append)
        self.port = self.server.open()
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.server.serve, args=(self.stopping.is_set,))
        self.thread.start()
        self.addCleanup(self.shutdown)

    def shutdown(self) -> None:
        self.stopping.set()
        self.thread.join(timeout=5)
        self.server.close()

    def client(self) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(2.0)
        self.addCleanup(sock.close)
        return sock

    def fetch(self, sock: socket.socket, request: bytes, *, drop_first_data: bool = False,
              acknowledge: bool = True) -> tuple[bytes, list[int]]:
        """Run a transfer; return the data and the ports the server answered from."""
        sock.sendto(request, ("127.0.0.1", self.port))
        data = bytearray()
        ports: list[int] = []
        dropped = False
        while True:
            packet, peer = sock.recvfrom(65535)
            ports.append(peer[1])
            opcode, number = struct.unpack(">HH", packet[:4])
            if opcode == 6:
                sock.sendto(ack(0), peer)
            elif opcode == 3:
                if drop_first_data and not dropped:
                    dropped = True
                    continue
                if number == (len(data) // 1468 + 1) & 0xFFFF or not drop_first_data:
                    pass
                data += packet[4:]
                sock.sendto(ack(number), peer)
                if len(packet) - 4 < 1468:
                    return bytes(data), ports
            else:
                return packet, ports

    def test_a_whole_image_arrives_from_a_port_other_than_the_listening_one(self) -> None:
        sock = self.client()
        data, ports = self.fetch(sock, rrq("uImage", blksize="1468", tsize="0"))
        self.assertEqual(data, self.image)
        self.assertEqual(len(set(ports)), 1)
        self.assertNotEqual(ports[0], self.port, "never from the listening port")

    def test_each_transfer_gets_its_own_port(self) -> None:
        first = self.fetch(self.client(), rrq("uImage", blksize="1468"))[1][0]
        second = self.fetch(self.client(), rrq("uImage", blksize="1468"))[1][0]
        self.assertNotEqual(first, second)

    def test_a_lost_block_is_resent(self) -> None:
        data, ports = self.fetch(self.client(), rrq("uImage", blksize="1468"),
                                 drop_first_data=True)
        self.assertEqual(data, self.image)

    def test_refusals_come_from_a_fresh_port_too(self) -> None:
        for packet, code in ((b"\x00\x02x\x00octet\x00", 2), (rrq("../x"), 2), (rrq("nope"), 1)):
            with self.subTest(code=code):
                sock = self.client()
                sock.sendto(packet, ("127.0.0.1", self.port))
                reply, peer = sock.recvfrom(1024)
                self.assertEqual(struct.unpack(">HH", reply[:4]), (5, code))
                self.assertNotEqual(peer[1], self.port)

    def test_a_stranger_cannot_disturb_a_transfer(self) -> None:
        sock, stranger = self.client(), self.client()
        sock.sendto(rrq("uImage", blksize="1468"), ("127.0.0.1", self.port))
        packet, peer = sock.recvfrom(65535)
        self.assertEqual(packet[:2], b"\x00\x06")
        stranger.sendto(ack(0), peer)
        stranger.sendto(struct.pack(">HH", 5, 0) + b"stop\0", peer)
        sock.sendto(ack(0), peer)
        data = bytearray()
        while True:
            packet, peer = sock.recvfrom(65535)
            data += packet[4:]
            sock.sendto(ack(struct.unpack(">H", packet[2:4])[0]), peer)
            if len(packet) - 4 < 1468:
                break
        self.assertEqual(bytes(data), self.image)

    def test_the_server_listens_on_the_one_address_it_was_given(self) -> None:
        self.assertEqual(self.server.socket.getsockname()[0], "127.0.0.1")

    def test_requests_are_logged(self) -> None:
        self.fetch(self.client(), rrq("uImage", blksize="1468"))
        sock = self.client()
        sock.sendto(rrq("nope"), ("127.0.0.1", self.port))
        sock.recvfrom(1024)
        self.shutdown()
        text = "\n".join(self.logs)
        self.assertIn("read uImage", text)
        self.assertIn("refused", text)
        self.assertIn("no such file", text)

    def test_a_client_that_gives_up_ends_the_transfer(self) -> None:
        sock = self.client()
        sock.sendto(rrq("uImage"), ("127.0.0.1", self.port))
        packet, peer = sock.recvfrom(1024)
        sock.sendto(struct.pack(">HH", 5, 0) + b"stop\0", peer)
        again = self.client()
        data, _ports = self.fetch(again, rrq("uImage", blksize="1468"))
        self.assertEqual(data, self.image, "the server went on to the next request")


class Standalone(TempDirTestCase):

    def test_the_server_stands_alone_and_is_plain_python(self) -> None:
        path = tftp.__file__
        with open(path, encoding="utf-8") as handle:
            source = handle.read()
        # The image host's Python may be old: nothing newer than 3.8 may be needed to read it.
        tree = ast.parse(source, feature_version=(3, 8))
        newer = tuple(getattr(ast, name) for name in ("Match", "TryStar") if hasattr(ast, name))
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom):
                self.assertEqual(node.level, 0, "a relative import")
                self.assertNotEqual((node.module or "").split(".")[0], "bench")
            if isinstance(node, ast.Import):
                for alias in node.names:
                    self.assertNotEqual(alias.name.split(".")[0], "bench")
            self.assertNotIsInstance(node, newer)

    def test_main_checks_its_arguments(self) -> None:
        quiet = io.StringIO()
        with contextlib.redirect_stderr(quiet):
            self.assertEqual(tftp.main(["--dir", os.path.join(self.tmp, "none"),
                                        "--bind", "127.0.0.1", "--port", "0"]), 2)
            self.assertIn("is not a directory", quiet.getvalue())
            with self.assertRaises(SystemExit):
                tftp.main(["--bind", "127.0.0.1"])
            os.mkdir(os.path.join(self.tmp, "d"))
            taken = Server(self.tmp, "127.0.0.1", 0)
            port = taken.open()
            self.addCleanup(taken.close)
            self.assertEqual(tftp.main(["--dir", os.path.join(self.tmp, "d"), "--bind",
                                        "127.0.0.1", "--port", str(port)]), 1)
            self.assertIn("cannot listen", quiet.getvalue())

    def test_main_serves_writes_its_pid_and_cleans_up(self) -> None:
        served = os.path.join(self.tmp, "d")
        os.mkdir(served)
        pidfile = os.path.join(self.tmp, "pid")
        listening = threading.Event()
        stopping = threading.Event()

        def stop() -> bool:
            listening.set()
            return stopping.is_set()

        result: list[int] = []
        quiet = io.StringIO()

        def serve() -> None:
            with contextlib.redirect_stderr(quiet):
                result.append(tftp.main(
                    ["--dir", served, "--bind", "127.0.0.1", "--port", "0", "--pidfile", pidfile],
                    stop=stop))

        thread = threading.Thread(target=serve)
        thread.start()
        self.assertTrue(listening.wait(5))
        self.assertTrue(os.path.exists(pidfile))
        with open(pidfile) as handle:
            self.assertEqual(int(handle.read()), os.getpid())
        stopping.set()
        thread.join(5)
        self.assertEqual(result, [0])
        self.assertFalse(os.path.exists(pidfile))


if __name__ == "__main__":
    unittest.main()
