.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============
docs/testing
============

The records of the device tests, ``tests/device``, run on the stick. Each run
is three files of one name:

``NAME.rst``
    The page: the source the tests came from, the image the stick ran, and a
    table with every case, its result, the time it took and the measured
    values its verdict rests on.
``NAME.log``
    Every command each case ran on the stick and on the bench host, with what
    it printed.
``NAME.json``
    The results of the table, as data.

A record leaves out what names one unit or one bench: MAC addresses, GPON
serial numbers, the status daemon's token, the bench's own addresses, and the
names of the bench host and of the stick. ``tests/device/README.rst`` says how
to make one.

The runs, the oldest first:

``2026-10-02-run01``
    The first run of every case on the board: bridge mode, the data path both
    ways, router mode with a static WAN and with PPPoE and back, and the reset
    by the watchdog, on the RAM image built from ``run01``, on the lab
    OLT. 27 of 27 passed.

``2026-10-02-run02``
    The same cases on the RAM image built from ``run02``, which has the
    fixes that the first run and the router-mode cases found: the WAN of router
    mode answers the network side, translated frames follow the default
    route's gateway, and the mode changes without restarting omcid. 27 of 27
    passed.

``2026-10-03-run03``
    The same cases on the RAM image built from ``run03``, after the
    driver's resets of the ACL, classifier and QoS at setup, its fixes to
    storm control, the address table and the frame limits, the PTP receive
    filter, the clock on timer 1 and the PON MAC's counters and queues. 27 of
    27 passed.
