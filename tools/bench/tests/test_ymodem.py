# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The YMODEM sender against a simulated receiver and its faults."""

from __future__ import annotations

import random
import unittest

from bench import ymodem
from bench.ymodem import (ACK, CAN, CRC_REQUEST, EOT, NAK, SOH, STX, Sender, YmodemAborted,
                          YmodemError, YmodemGaveUp, YmodemTimeout)

from .simclock import VirtualClock
from .simymodem import Faults, LoopbackPort


def bitwise_crc16(data: bytes) -> int:
    """An independent CRC-16/XMODEM, one bit at a time."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Crc(unittest.TestCase):

    def test_crc16_check_values(self) -> None:
        cases = [(b"123456789", 0x31C3), (bytes(128), 0x0000), (bytes([0x1A]) * 1024, 0xFC96),
                 (bytes(range(256)) * 4, 0xC2E0)]
        for data, value in cases:
            with self.subTest(length=len(data)):
                self.assertEqual(ymodem.crc16(data), value)
                self.assertEqual(bitwise_crc16(data), value)

    def test_it_agrees_with_an_independent_implementation(self) -> None:
        rng = random.Random(5)
        for _ in range(50):
            data = rng.randbytes(rng.randrange(0, 300))
            self.assertEqual(ymodem.crc16(data), bitwise_crc16(data))


class Frames(unittest.TestCase):

    def test_ymodem_header(self) -> None:
        block = ymodem.header_block("uImage", 390170)
        self.assertEqual(len(block), 3 + 128 + 2)
        self.assertEqual(block[:3], b"\x01\x00\xff")
        self.assertEqual(block[3:3 + 18], b"uImage\x00390170\x00" + b"\x00" * 4)
        self.assertEqual(block[3 + 14:-2], bytes(128 - 14))
        self.assertEqual(block[-2:], b"\x64\x5f", "CRC 0x645f, high byte first")

    def test_the_header_carries_the_base_name_only(self) -> None:
        self.assertEqual(ymodem.header_block("/a/b/uImage", 1), ymodem.header_block("uImage", 1))

    def test_names_that_cannot_go_in_a_header(self) -> None:
        for name in ("", "café", "x" * 130, "dir/"):
            with self.subTest(name=name), self.assertRaises(YmodemError):
                ymodem.header_block(name, 1)

    def test_ymodem_blocks(self) -> None:
        chunk = bytes(range(256)) * 4
        block = ymodem.data_block(1, chunk)
        self.assertEqual(len(block), 1029)
        self.assertEqual(block[:3], b"\x02\x01\xfe")
        self.assertEqual(block[3:-2], chunk)
        self.assertEqual(block[-2:], bytes([0xC2, 0xE0]))
        self.assertEqual(ymodem.data_block(255, chunk)[:3], b"\x02\xff\x00")
        self.assertEqual(ymodem.data_block(256, chunk)[:3], b"\x02\x00\xff")
        self.assertEqual(ymodem.data_block(257, chunk)[:3], b"\x02\x01\xfe")

    def test_the_last_block_is_padded_with_0x1a(self) -> None:
        block = ymodem.data_block(3, b"abc")
        self.assertEqual(block[3:-2], b"abc" + bytes([0x1A]) * 1021)
        self.assertEqual(ymodem.crc16(block[3:-2]), block[-2] << 8 | block[-1])

    def test_the_empty_header_block_ends_the_batch(self) -> None:
        block = ymodem.end_block()
        self.assertEqual(block[:3], b"\x01\x00\xff")
        self.assertEqual(block[3:-2], bytes(128))
        self.assertEqual(block[-2:], b"\x00\x00")


class Transfers(unittest.TestCase):

    def setUp(self) -> None:
        self.clock = VirtualClock()
        self.rng = random.Random(11)

    def port(self, faults: Faults | None = None, started: bool = True) -> LoopbackPort:
        port = LoopbackPort(self.clock, faults)
        if started:
            port.start()
        return port

    def send(self, port: LoopbackPort, data: bytes, name: str = "uImage", progress=None):
        return Sender(port, self.clock, progress).send(name, data)

    def test_a_clean_transfer_and_the_loaders_summary(self) -> None:
        data = self.rng.randbytes(390170)
        port = self.port()
        report = self.send(port, data)
        self.assertEqual(port.done, [("uImage", 390170, data)])
        self.assertEqual((report.blocks, report.resends, report.length), (382, 0, 390170))
        self.assertTrue(report.end_acknowledged)
        text = bytes(port.heard).decode()
        self.assertIn("xyzModem - CRC mode, 2(SOH)/382(STX)/0(CAN) packets, 2 retries", text)
        self.assertIn("## Total Size      = 0x0005f41a = 390170 Bytes", text)
        self.assertEqual(port.receiver.frames_seen[0], (SOH, 0))
        self.assertEqual(port.receiver.frames_seen[1], (STX, 1))

    def test_the_summary_counts_ceil_of_length_over_1024_blocks(self) -> None:
        for length in (1, 1023, 1024, 1025, 5000):
            with self.subTest(length=length):
                port = self.port()
                self.send(port, (bytes(range(256)) * (length // 256 + 1))[:length])
                self.assertIn("%d(STX)" % -(-length // 1024), bytes(port.heard).decode())

    def test_sequence_numbers_wrap_at_256(self) -> None:
        data = self.rng.randbytes(1024 * 260 + 10)
        port = self.port()
        self.send(port, data)
        self.assertEqual(port.done[0][2], data)
        sequences = [seq for _lead, seq in port.receiver.frames_seen[1:]]
        self.assertEqual(sequences[254:258], [255, 0, 1, 2])

    def test_everything_before_the_first_c_is_discarded(self) -> None:
        port = self.port(started=False)
        port.heard += b"loady 80000000\r\n## Ready for binary (ymodem) download to 0x80000000\r\n"
        self.clock.schedule(1.0, port.start)
        self.send(port, b"x" * 100)
        self.assertEqual(len(port.done[0][2]), 100)

    def test_no_c_in_25_seconds_fails_with_the_last_200_bytes_heard(self) -> None:
        port = self.port(started=False)
        port.heard += b"banner " * 100
        started = self.clock.now()
        with self.assertRaises(YmodemTimeout) as caught:
            self.send(port, b"x")
        self.assertAlmostEqual(self.clock.now() - started, 25.0, delta=0.2)
        self.assertEqual(len(caught.exception.seen), 200)
        self.assertTrue(caught.exception.seen.endswith(b"banner "))

    def test_the_header_waits_for_the_c_that_follows_but_goes_on_without_it(self) -> None:
        port = self.port()
        # Take the C that follows the header away: the sender waits five seconds, then sends.
        original = port.receiver.emit
        port.receiver.emit = lambda data: None if data == bytes([CRC_REQUEST]) and \
            port.receiver.frames_seen else original(data)
        started = self.clock.now()
        self.send(port, b"x" * 2000)
        self.assertGreaterEqual(self.clock.now() - started, 5.0)
        self.assertEqual(len(port.done[0][2]), 2000)

    def test_progress_is_reported_every_64_blocks(self) -> None:
        calls: list[tuple[int, int]] = []
        self.send(self.port(), self.rng.randbytes(1024 * 130), progress=lambda a, b: calls.append((a, b)))
        self.assertEqual(calls, [(64, 130), (128, 130)])

    def test_the_rate_is_that_of_a_115200_baud_line(self) -> None:
        data = self.rng.randbytes(1239420)
        report = self.send(self.port(), data)
        self.assertEqual(report.blocks, 1211)
        self.assertGreater(report.seconds, 100.0)
        self.assertLess(report.seconds, 130.0)
        biggest = self.rng.randbytes(2572288)
        self.assertLess(self.send(self.port(), biggest).seconds, 260.0)

    def test_nothing_to_send_is_an_error(self) -> None:
        with self.assertRaises(YmodemError):
            self.send(self.port(), b"")


class Faulty(unittest.TestCase):
    """One test per behaviour of the simulated receiver."""

    def setUp(self) -> None:
        self.clock = VirtualClock()
        self.data = random.Random(21).randbytes(1024 * 10 + 100)

    def run_with(self, faults: Faults) -> tuple[LoopbackPort, ymodem.SendReport]:
        port = LoopbackPort(self.clock, faults)
        port.start()
        report = Sender(port, self.clock).send("uImage", self.data)
        return port, report

    def test_a_corrupted_byte_is_refused_and_resent_at_once(self) -> None:
        started = self.clock.now()
        port, report = self.run_with(Faults(corrupt={3}))
        self.assertEqual(port.done[0][2], self.data)
        self.assertEqual(report.resends, 1)
        self.assertEqual(port.receiver.naks, 1)
        self.assertLess(self.clock.now() - started, 3.0, "a NAK is an immediate resend")
        self.assertIn("3 retries", bytes(port.heard).decode())

    def test_a_corrupted_header_is_resent(self) -> None:
        port, report = self.run_with(Faults(corrupt={0}))
        self.assertEqual(port.done[0][2], self.data)
        self.assertEqual(report.resends, 1)

    def test_a_lost_byte_makes_the_sender_time_out_and_resend(self) -> None:
        started = self.clock.now()
        port, report = self.run_with(Faults(lose={4}))
        self.assertEqual(port.done[0][2], self.data)
        self.assertEqual(report.resends, 1)
        self.assertGreaterEqual(self.clock.now() - started, 6.0)

    def test_ymodem_duplicate_ack(self) -> None:
        """A lost ACK leads to a repeated block, which is acknowledged but stored once."""
        port, report = self.run_with(Faults(drop_ack={5}))
        self.assertEqual(port.done[0][2], self.data, "stored once")
        self.assertEqual(port.receiver.duplicates, 1)
        self.assertEqual(report.resends, 1)
        self.assertEqual(len([f for f in port.receiver.frames_seen if f[1] == 5]), 1)

    def test_a_late_ack_is_taken_for_the_answer_to_a_resend(self) -> None:
        port, report = self.run_with(Faults(delay_ack={2: 7.0}))
        self.assertEqual(port.done[0][2], self.data, "the data is whole")
        self.assertEqual(port.receiver.duplicates, 1)
        self.assertEqual(report.resends, 1)

    def test_a_lost_header_acknowledgement_is_resent(self) -> None:
        port, report = self.run_with(Faults(drop_ack={0}))
        self.assertEqual(port.done[0][2], self.data)
        self.assertEqual(report.resends, 1)

    def test_cancel_bytes_from_the_receiver_abort(self) -> None:
        port = LoopbackPort(self.clock, Faults(cancel_after=4))
        port.start()
        with self.assertRaises(YmodemAborted):
            Sender(port, self.clock).send("uImage", self.data)
        self.assertEqual(port.done, [])

    def test_a_single_cancel_byte_is_not_an_abort(self) -> None:
        port = LoopbackPort(self.clock)
        port.start()
        port.heard += bytes([CAN])
        report = Sender(port, self.clock).send("uImage", self.data)
        self.assertTrue(report.end_acknowledged)

    def test_ymodem_gives_up(self) -> None:
        """A block that is never acknowledged ends the transfer after six sends."""
        port = LoopbackPort(self.clock, Faults(silence_after=3))
        port.start()
        started = self.clock.now()
        with self.assertRaises(YmodemGaveUp) as caught:
            Sender(port, self.clock).send("uImage", self.data)
        self.assertEqual((caught.exception.block, caught.exception.offset), (4, 3 * 1024))
        self.assertEqual(port.writes[-2:], [bytes([CAN, CAN]), b"\r"], "two cancels, then Enter")
        # six sends of block 4, each waiting six seconds, then the two-second pause
        self.assertAlmostEqual(self.clock.now() - started, 6 * 6.0 + 2.0, delta=3.0)
        sends = [w for w in port.writes if len(w) == 1029 and w[:3] == bytes([STX, 4, 0xFB])]
        self.assertEqual(len(sends), 6)

    def test_a_header_that_is_never_acknowledged_gives_up_after_five_sends(self) -> None:
        port = LoopbackPort(self.clock, Faults(lose={0}))
        port.start()
        port.receiver.faults.lose = set()
        # The receiver hears nothing of the header: lose it every time.
        port.receiver._once = lambda kind, seq, wanted: kind == "lose" and seq == 0  # type: ignore
        with self.assertRaises(YmodemGaveUp) as caught:
            Sender(port, self.clock).send("uImage", self.data)
        self.assertEqual((caught.exception.block, caught.exception.offset), (0, 0))
        headers = [w for w in port.writes if len(w) == 133 and w[:3] == bytes([SOH, 0, 0xFF])]
        self.assertEqual(len(headers), 5)

    def test_a_reset_during_the_transfer_leaves_the_sender_without_answers(self) -> None:
        resets: list[int] = []
        port = LoopbackPort(self.clock, Faults(reset_after=2))
        port.receiver.on_reset = lambda: resets.append(1)
        port.start()
        with self.assertRaises(YmodemGaveUp):
            Sender(port, self.clock).send("uImage", self.data)
        self.assertEqual(resets, [1])
        self.assertEqual(port.done, [])

    def test_ymodem_faults_do_not_lose_data_when_combined(self) -> None:
        faults = Faults(corrupt={1, 6}, drop_ack={2, 8}, lose={9}, delay_ack={4: 7.0})
        port, report = self.run_with(faults)
        self.assertEqual(port.done[0][2], self.data)
        self.assertGreaterEqual(report.resends, 5)

    def test_ymodem_end(self) -> None:
        """The end is EOT (answered with NAK), EOT again (ACK), then the empty header block."""
        port, report = self.run_with(Faults())
        eots = [i for i, w in enumerate(port.writes) if w == bytes([EOT])]
        self.assertEqual(len(eots), 2)
        self.assertEqual(port.writes[eots[1] + 1], ymodem.end_block())
        self.assertEqual(port.writes[-1], ymodem.end_block())
        self.assertTrue(report.end_acknowledged)
        self.assertEqual(report.resends, 0, "the second EOT is the protocol, not a resend")

    def test_a_receiver_that_acknowledges_the_first_eot_at_once_is_accepted(self) -> None:
        port = LoopbackPort(self.clock)
        port.start()
        original = port.receiver._end_of_file

        def at_once() -> None:
            port.receiver.state = "eot"
            original()

        port.receiver._end_of_file = at_once  # type: ignore[method-assign]
        Sender(port, self.clock).send("uImage", self.data)
        self.assertEqual(port.writes.count(bytes([EOT])), 1)
        self.assertEqual(port.done[0][2], self.data)

    def test_no_acknowledgement_of_the_end_batch_is_reported_not_raised(self) -> None:
        port = LoopbackPort(self.clock)
        port.start()
        original = port.receiver._empty_header
        port.receiver._empty_header = lambda seq, payload: None  # type: ignore[method-assign]
        report = Sender(port, self.clock).send("uImage", self.data)
        self.assertFalse(report.end_acknowledged)
        self.assertIs(original is not None, True)

    def test_the_end_of_file_that_is_never_acknowledged_is_a_timeout(self) -> None:
        port = LoopbackPort(self.clock)
        port.start()
        port.receiver._end_of_file = lambda: None  # type: ignore[method-assign]
        with self.assertRaises(YmodemTimeout) as caught:
            Sender(port, self.clock).send("uImage", self.data)
        self.assertIn("end of the file", str(caught.exception))

    def test_a_receiver_that_wants_another_block_number_is_never_satisfied(self) -> None:
        """Every block is answered with NAK and resent at once, six times, then given up."""
        port = LoopbackPort(self.clock)
        port.start()
        port.receiver.expected = 5
        started = self.clock.now()
        with self.assertRaises(YmodemGaveUp) as caught:
            Sender(port, self.clock).send("uImage", self.data)
        self.assertEqual((caught.exception.block, caught.exception.offset), (1, 0))
        self.assertEqual(port.receiver.naks, 6)
        self.assertLess(self.clock.now() - started, 10.0, "NAKs are answered at once")
        self.assertEqual(port.done, [])

    def test_the_control_bytes_are_the_ones_of_the_protocol(self) -> None:
        self.assertEqual((SOH, STX, EOT, ACK, NAK, CAN, CRC_REQUEST),
                         (0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43))
        self.assertEqual(ymodem.PAD, 0x1A)


if __name__ == "__main__":
    unittest.main()
