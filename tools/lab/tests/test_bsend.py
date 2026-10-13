# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of bsend on a socket and a clock that are made up for the test."""

import importlib.machinery
import importlib.util
import io
import os
import socket
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "bsend")

_loader = importlib.machinery.SourceFileLoader("bsend", TOOL)
_spec = importlib.util.spec_from_loader("bsend", _loader)
bsend = importlib.util.module_from_spec(_spec)
_loader.exec_module(bsend)


class Clock:
    """A clock that only the sleeps move, so that a run takes no time."""

    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class Socket:
    def __init__(self, *args):
        self.args = args
        self.options = []
        self.sent = []

    def setsockopt(self, level, name, value):
        self.options.append((level, name, value))

    def sendto(self, data, address):
        self.sent.append((len(data), address))


def run(argv, fail_option=False):
    clock = Clock()
    made = []

    def make(*args):
        s = Socket(*args)
        if fail_option:
            def refuse(level, name, value):
                raise PermissionError("Operation not permitted")
            s.setsockopt = refuse
        made.append(s)
        return s

    out, err = io.StringIO(), io.StringIO()
    rc = bsend.main(argv, make_socket=make, clock=clock, sleep=clock.sleep, out=out, err=err)
    return rc, out.getvalue(), err.getvalue(), made, clock


class Arguments(unittest.TestCase):
    def test_the_defaults(self):
        self.assertEqual(bsend.parse(["eth0", "1000", "5"]), ("eth0", 1000, 5.0, 1400, 9))

    def test_a_size_and_a_port(self):
        self.assertEqual(bsend.parse(["--port", "6000", "eth0", "10", "2", "60"]),
                         ("eth0", 10, 2.0, 60, 6000))

    def test_what_is_refused(self):
        for argv in (["eth0"], ["eth0", "1000"], ["eth0", "0", "5"], ["eth0", "10", "0"],
                     ["eth0", "10", "-1"], ["eth0", "10", "5", "0"], ["eth0", "10", "5", "65508"],
                     ["eth0", "ten", "5"], ["--port"], ["--port", "x", "eth0", "1", "1"],
                     ["--port", "0", "eth0", "1", "1"], ["eth0", "1", "1", "1", "1"]):
            self.assertIsInstance(bsend.parse(argv), str, argv)


class Sending(unittest.TestCase):
    def test_it_sends_the_rate_for_the_time_in_the_size(self):
        rc, out, err, made, clock = run(["eth0", "1000", "2", "1400"])
        self.assertEqual(rc, 0, err)
        (sock,) = made
        # One frame at every millisecond of two seconds.
        self.assertEqual(len(sock.sent), 2000)
        self.assertEqual({s for s, _a in sock.sent}, {1400})
        self.assertEqual({a for _s, a in sock.sent}, {("255.255.255.255", 9)})
        self.assertIn("sent 2000 frames of 1400 B in 2.00 s: 1000 pps, 11.20 Mbit/s", out)

    def test_it_names_the_interface_and_allows_broadcast(self):
        rc, _out, _err, made, _clock = run(["eth0", "10", "1"])
        self.assertEqual(rc, 0)
        options = made[0].options
        self.assertIn((socket.SOL_SOCKET, socket.SO_BROADCAST, 1), options)
        if hasattr(socket, "SO_BINDTODEVICE"):
            self.assertIn((socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b"eth0"), options)

    def test_a_slow_rate_keeps_its_pace(self):
        _rc, _out, _err, made, clock = run(["eth0", "4", "3", "64"])
        self.assertEqual(len(made[0].sent), 12)
        self.assertAlmostEqual(clock.now - 100.0, 3.0, places=6)

    def test_the_port_goes_with_the_datagram(self):
        _rc, _out, _err, made, _clock = run(["--port", "7777", "eth0", "10", "1", "10"])
        self.assertEqual({a for _s, a in made[0].sent}, {("255.255.255.255", 7777)})

    def test_an_interface_it_cannot_use_is_a_usage_error(self):
        rc, out, err, _made, _clock = run(["eth0", "10", "1"], fail_option=True)
        self.assertEqual(rc, 2)
        self.assertEqual(out, "")
        self.assertIn("cannot send out of eth0", err)

    def test_a_bad_argument_says_what_and_exits_2(self):
        rc, _out, err, made, _clock = run(["eth0", "0", "1"])
        self.assertEqual(rc, 2)
        self.assertIn("PPS is at least 1", err)
        self.assertEqual(made, [])

    def test_help_and_no_arguments(self):
        rc, out, _err, _made, _clock = run(["--help"])
        self.assertEqual(rc, 0)
        self.assertIn("usage: bsend", out)
        rc, _out, err, _made, _clock = run([])
        self.assertEqual(rc, 2)
        self.assertIn("usage: bsend", err)


if __name__ == "__main__":
    unittest.main()
