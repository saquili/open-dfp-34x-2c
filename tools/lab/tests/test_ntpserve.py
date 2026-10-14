# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of ntpserve: the answer it builds (RFC 5905, 7.3), its options, and one exchange on loopback."""

import importlib.machinery
import importlib.util
import io
import os
import socket
import struct
import threading
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "ntpserve")

_loader = importlib.machinery.SourceFileLoader("ntpserve", TOOL)
_spec = importlib.util.spec_from_loader("ntpserve", _loader)
ntpserve = importlib.util.module_from_spec(_spec)
_loader.exec_module(ntpserve)

# 2026-10-13 12:00:00 UTC.
T0 = 1_791_892_800 * 1_000_000_000


def request(version=4, mode=3, transmit=b"\x01\x02\x03\x04\x05\x06\x07\x08"):
    return bytes([version << 3 | mode, 0, 6, 0]) + bytes(36) + transmit


def ntp_ns(field):
    sec, frac = struct.unpack(">II", field)
    return (sec - ntpserve.NTP_UNIX_EPOCH) * 1_000_000_000 + (frac * 1_000_000_000 >> 32)


class Answer(unittest.TestCase):
    def test_server_answer(self):
        a = ntpserve.answer(request(), T0, T0 + 1500, 2)
        self.assertEqual(len(a), 48)
        self.assertEqual(a[0] >> 6, 0)          # no leap warning
        self.assertEqual((a[0] >> 3) & 7, 4)    # the client's version
        self.assertEqual(a[0] & 7, 4)           # server
        self.assertEqual(a[1], 2)
        # The client's transmit time comes back as the origin.
        self.assertEqual(a[24:32], b"\x01\x02\x03\x04\x05\x06\x07\x08")
        # Receive and transmit, to within the 2^-32 s of the format.
        self.assertLessEqual(abs(ntp_ns(a[32:40]) - T0), 1)
        self.assertLessEqual(abs(ntp_ns(a[40:48]) - (T0 + 1500)), 1)
        # A root distance far below what a client refuses (16 s).
        delay, disp = struct.unpack(">II", a[4:12])
        self.assertLess(delay / 65536 / 2 + disp / 65536, 0.01)

    def test_version_followed(self):
        self.assertEqual((ntpserve.answer(request(version=3), T0, T0, 2)[0] >> 3) & 7, 3)

    def test_no_time_to_give(self):
        a = ntpserve.answer(request(), T0, T0, 16)
        self.assertEqual(a[0] >> 6, 3)
        self.assertEqual(a[1], 16)

    def test_not_a_request(self):
        self.assertIsNone(ntpserve.answer(request(mode=4), T0, T0, 2))
        self.assertIsNone(ntpserve.answer(request(version=0), T0, T0, 2))
        self.assertIsNone(ntpserve.answer(request()[:47], T0, T0, 2))


class Options(unittest.TestCase):
    def test_defaults(self):
        self.assertEqual(ntpserve.parse([]),
                         {"address": "", "port": 123, "seconds": 60, "skew_ms": 0, "stratum": 2})

    def test_values(self):
        opt = ntpserve.parse(["--address", "192.0.2.1", "--port", "1123", "--skew-ms", "-40",
                              "--seconds", "5", "--stratum", "3"])
        self.assertEqual(opt, {"address": "192.0.2.1", "port": 1123, "seconds": 5, "skew_ms": -40,
                               "stratum": 3})

    def test_errors(self):
        for argv in (["--port"], ["--port", "x"], ["--port", "0"], ["--stratum", "17"], ["eth0"],
                     ["--bogus", "1"]):
            self.assertIsInstance(ntpserve.parse(argv), str, argv)

    def test_usage_error_status(self):
        err = io.StringIO()
        old, ntpserve.sys.stderr = ntpserve.sys.stderr, err
        try:
            self.assertEqual(ntpserve.main(["--port", "x"]), 2)
        finally:
            ntpserve.sys.stderr = old
        self.assertIn("--port takes a number", err.getvalue())


class Loopback(unittest.TestCase):
    def test_one_exchange(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        opt = {"address": "127.0.0.1", "port": port, "seconds": 2, "skew_ms": 250, "stratum": 2}
        out = io.StringIO()
        server = threading.Thread(target=ntpserve.run, args=(opt, out, lambda: T0))
        server.start()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(2)
                for _ in range(20):
                    client.sendto(request(), ("127.0.0.1", port))
                    try:
                        reply, _ = client.recvfrom(512)
                        break
                    except socket.timeout:
                        continue
        finally:
            server.join()
        self.assertEqual(reply[0] & 7, 4)
        self.assertLessEqual(abs(ntp_ns(reply[40:48]) - (T0 + 250_000_000)), 1)
        self.assertIn("ntpserve done:", out.getvalue())


if __name__ == "__main__":
    unittest.main()
