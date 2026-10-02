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
