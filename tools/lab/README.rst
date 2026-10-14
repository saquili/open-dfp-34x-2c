.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
tools/lab
=========

Small tools for the host that stands on the other side of the stick in a lab:
the ones that make the traffic or give the time a test needs, which no other
program on that host gives as a test wants it.

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

``ptpmaster`` and ``ntpserve``
==============================

A PTP master and an NTP server on the clock of the host, for the tests of the
stick's ``timed`` (``timed/README.rst``) where the host has neither::

    sudo tools/lab/ptpmaster [--transport l2|udp] [--domain N] [--seconds N]
                             [--skew-ms MS] [--utc-offset S] [--one-step] IFACE
    sudo tools/lab/ntpserve [--address ADDR] [--port N] [--seconds N]
                            [--skew-ms MS] [--stratum N]

``ptpmaster`` announces itself on IFACE, sends a Sync a second, two-step unless
``--one-step``, and answers each Delay_Req, over layer 2 or UDP. Its times are
the kernel's software stamps, as ``ptp4l -S`` takes them: the Follow_Up carries
the time the driver sent the Sync or, from a driver that stamps nothing, the
time it entered the device's queue (``SO_TIMESTAMPING``), and a Delay_Resp the
time the Delay_Req arrived. A time read before sending was some tens of
microseconds late on the lab's router, and the stick's PHC followed that; the
summary at the end counts the Syncs the kernel stamped. ``ntpserve``
answers the NTP requests that come to ADDR (every address of the host by
default) on port 123. Each gives the host's ``CLOCK_REALTIME`` plus
``--skew-ms``, so that the stick has a clock to move to, and neither sets a
clock. Each ends after ``--seconds`` (60 by default), so that nothing is left
running in a namespace. They need root for the raw socket and the ports below
1024.

Tests
=====

``tests/run`` checks the arguments and the pace of ``bsend`` on a socket and a
clock that the test makes up, the messages ``ptpmaster`` builds, byte by byte,
the stamps it reads from the error queue,
and the answer ``ntpserve`` builds and one exchange of it on loopback, so that
it needs no root, no interface and no time. It is part of ``make check``.
