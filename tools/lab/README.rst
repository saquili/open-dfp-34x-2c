.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
tools/lab
=========

Small tools for the host that stands on the other side of the stick in a lab:
the one that makes the traffic a test needs and that no other program makes as
steadily.

``bsend``
=========

UDP broadcasts out of one interface at a fixed rate::

    sudo tools/lab/bsend [--port PORT] IFACE PPS SECONDS [SIZE]

It sends PPS datagrams a second for SECONDS seconds, each of SIZE payload bytes
(1400 by default), to the limited broadcast address 255.255.255.255 and UDP
port 9, out of IFACE whatever the routing table says, and prints what it sent
and the rate it kept::

    sent 5000 frames of 1400 B in 5.00 s: 1000 pps, 11.20 Mbit/s of payload

It is the load of the broadcast and storm-control measurements: a bridge floods
the frames, the stick's switch counts and limits them, and the rate is known to
the frame. ``--port`` names another UDP port.

The interface is named with ``SO_BINDTODEVICE``, which needs ``CAP_NET_RAW``
(root on Linux), and is not routed: the frames leave the interface and nothing
else. The pace is one frame at each ``1/PPS`` from the start, and the run lasts
out the last interval, so the time and the rate are the ones asked for. At a
few thousand frames a second the pace is that of the host's timer: the printed
rate is the rate that was kept, not the rate asked for.

Exit status: 0 when it sent, 2 on a usage error or when the interface cannot be
used.

Tests
=====

``tests/run`` checks the arguments and the pace on a socket and a clock that
the test makes up, so that it needs no root, no interface and no time. It is
part of ``make check``.
