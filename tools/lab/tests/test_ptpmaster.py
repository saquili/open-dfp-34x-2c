# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of ptpmaster: the messages it builds, byte by byte (IEEE 1588-2008, clause 13)."""

import importlib.machinery
import importlib.util
import os
import socket
import struct
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "ptpmaster")

_loader = importlib.machinery.SourceFileLoader("ptpmaster", TOOL)
_spec = importlib.util.spec_from_loader("ptpmaster", _loader)
ptpmaster = importlib.util.module_from_spec(_spec)
_loader.exec_module(ptpmaster)

IDENT = bytes.fromhex("001b19fffe000001")


def fields(msg):
    """The header: type, version, length, domain, flags, correction, source port, sequence id."""
    return {
        "type": msg[0] & 0x0F,
        "version": msg[1] & 0x0F,
        "length": struct.unpack(">H", msg[2:4])[0],
        "domain": msg[4],
        "flags": struct.unpack(">H", msg[6:8])[0],
        "correction": struct.unpack(">q", msg[8:16])[0] >> 16,
        "source": bytes(msg[20:28]),
        "port": struct.unpack(">H", msg[28:30])[0],
        "seq": struct.unpack(">H", msg[30:32])[0],
    }


def stamp(msg, at=34):
    hi, lo, nsec = struct.unpack(">HII", msg[at:at + 10])
    return ((hi << 32 | lo) * 1_000_000_000) + nsec


class Messages(unittest.TestCase):
    def test_a_timestamp_is_48_bits_of_seconds_and_32_of_nanoseconds(self):
        t = 1_791_892_800 * 1_000_000_000 + 123_456_789
        raw = ptpmaster.timestamp(t)
        self.assertEqual(len(raw), 10)
        self.assertEqual(raw[:6], (1_791_892_800).to_bytes(6, "big"))
        self.assertEqual(struct.unpack(">I", raw[6:])[0], 123_456_789)

    def test_announce(self):
        msg = ptpmaster.build_announce(4, 9, IDENT, 37)
        f = fields(msg)
        self.assertEqual((len(msg), f["length"], f["type"], f["version"], f["domain"], f["seq"]), (64, 64, 0xB, 2, 4, 9))
        self.assertEqual(f["source"], IDENT)
        self.assertTrue(f["flags"] & 0x0008)  # PTP timescale
        self.assertTrue(f["flags"] & 0x0004)  # current UTC offset valid
        self.assertEqual(struct.unpack(">h", msg[44:46])[0], 37)
        self.assertEqual(msg[47], 128)  # priority 1
        self.assertEqual(msg[48], 6)  # clock class
        self.assertEqual(msg[52], 128)  # priority 2
        self.assertEqual(msg[53:61], IDENT)  # grandmaster identity
        self.assertEqual(struct.unpack(">H", msg[61:63])[0], 0)  # steps removed

    def test_sync_two_step_carries_no_time_and_its_follow_up_does(self):
        t = 1_791_892_837 * 1_000_000_000 + 5
        sync = ptpmaster.build_sync(0, 3, IDENT, t, True)
        follow = ptpmaster.build_follow_up(0, 3, IDENT, t)
        self.assertEqual((fields(sync)["type"], len(sync)), (0x0, 44))
        self.assertTrue(fields(sync)["flags"] & 0x0200)  # two-step
        self.assertEqual(stamp(sync), 0)
        self.assertEqual((fields(follow)["type"], fields(follow)["seq"], len(follow)), (0x8, 3, 44))
        self.assertEqual(stamp(follow), t)

    def test_sync_one_step_carries_the_time(self):
        t = 1_791_892_837 * 1_000_000_000 + 5
        sync = ptpmaster.build_sync(0, 1, IDENT, t, False)
        self.assertFalse(fields(sync)["flags"] & 0x0200)
        self.assertEqual(stamp(sync), t)

    def test_delay_resp_names_the_requester(self):
        requester = bytes.fromhex("0200 00fffe000002 0001".replace(" ", ""))
        msg = ptpmaster.build_delay_resp(0, 77, IDENT, 42_000_000_123, requester)
        f = fields(msg)
        self.assertEqual((f["type"], f["seq"], f["length"], len(msg)), (0x9, 77, 54, 54))
        self.assertEqual(stamp(msg), 42_000_000_123)
        self.assertEqual(msg[44:54], requester)

    def test_a_delay_req_is_read_by_its_domain(self):
        req = bytearray(44)
        req[0], req[1] = 0x01, 0x02
        req[4] = 0
        req[20:30] = bytes.fromhex("0200 00fffe000002 0001".replace(" ", ""))
        req[30:32] = b"\x00\x2a"
        self.assertEqual(ptpmaster.parse_delay_req(bytes(req), 0), (42, bytes(req[20:30])))
        self.assertIsNone(ptpmaster.parse_delay_req(bytes(req), 1))
        req[0] = 0x00  # a Sync
        self.assertIsNone(ptpmaster.parse_delay_req(bytes(req), 0))
        self.assertIsNone(ptpmaster.parse_delay_req(bytes(req)[:30], 0))

    def test_the_kernel_stamp_of_a_message_sent(self):
        three = struct.pack("@ll", 1791892800, 123456789) + struct.pack("@ll", 0, 0) * 2
        stamp = (socket.SOL_SOCKET, ptpmaster.SO_TIMESTAMPING, three)
        t = 1791892800 * 1_000_000_000 + 123456789

        def err(info, level=ptpmaster.SOL_PACKET, ctype=ptpmaster.PACKET_TX_TIMESTAMP):
            return (level, ctype, struct.pack("@IBBBBII", 42, 4, 0, 0, 0, info, 0))

        # The driver's stamp and the queue's, by the extended error beside them, over layer 2 and UDP.
        self.assertEqual(ptpmaster.tx_stamp_of([stamp, err(ptpmaster.SCM_TSTAMP_SND)]), (t, ptpmaster.SCM_TSTAMP_SND))
        self.assertEqual(ptpmaster.tx_stamp_of([stamp, err(ptpmaster.SCM_TSTAMP_SCHED)]),
                         (t, ptpmaster.SCM_TSTAMP_SCHED))
        self.assertEqual(ptpmaster.tx_stamp_of([err(ptpmaster.SCM_TSTAMP_SCHED, ptpmaster.SOL_IP, ptpmaster.IP_RECVERR),
                                                stamp]), (t, ptpmaster.SCM_TSTAMP_SCHED))
        # No software stamp, another message, or too short: none.
        zero = struct.pack("@ll", 0, 0) * 3
        self.assertEqual(ptpmaster.tx_stamp_of([(socket.SOL_SOCKET, ptpmaster.SO_TIMESTAMPING, zero)]), (None, None))
        self.assertEqual(ptpmaster.tx_stamp_of([(socket.SOL_SOCKET, ptpmaster.SO_TIMESTAMPNS, three)]), (None, None))
        self.assertEqual(ptpmaster.tx_stamp_of([(socket.SOL_SOCKET, ptpmaster.SO_TIMESTAMPING, three[:8])]),
                         (None, None))

    def test_clock_identity_from_a_mac(self):
        self.assertEqual(ptpmaster.clock_identity(bytes.fromhex("40ed0081 5bfb".replace(" ", ""))),
                         bytes.fromhex("40ed00fffe815bfb"))


class Options(unittest.TestCase):
    def test_the_defaults(self):
        opt = ptpmaster.parse(["eth0"])
        self.assertEqual((opt["transport"], opt["domain"], opt["seconds"], opt["utc_offset"], opt["two_step"]),
                         ("l2", 0, 60, 37, True))

    def test_the_options(self):
        opt = ptpmaster.parse(["--transport", "udp", "--domain", "4", "--seconds", "5", "--skew-ms", "-250",
                               "--utc-offset", "36", "--one-step", "eth0"])
        self.assertEqual((opt["transport"], opt["domain"], opt["seconds"], opt["skew_ms"], opt["utc_offset"],
                          opt["two_step"], opt["iface"]), ("udp", 4, 5, -250, 36, False, "eth0"))

    def test_errors(self):
        for argv in ([], ["a", "b"], ["--transport", "tcp", "eth0"], ["--domain", "300", "eth0"], ["--bogus", "eth0"],
                     ["--seconds", "x", "eth0"], ["--domain"]):
            self.assertIsInstance(ptpmaster.parse(argv), str, argv)


if __name__ == "__main__":
    unittest.main()
