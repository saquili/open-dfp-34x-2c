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

``2026-10-03-run04``
    The same cases and the new VLAN translation case on the RAM image built
    from ``run04``, after the GPON state machine's O6, O7 and TO2, the
    standard counters, a receive ring of OMCI's own, pause on the SerDes port,
    BER_Interval and Ranging_Adjustment, the key exchange of encryption, and
    the fix that makes a VLAN translation translate. 28 of 28 passed.

``2026-10-03-run05``
    The same cases and the new ACL case on the RAM image built from
    ``run05``, after the fixes to the bridge's upstream stream when a
    GEM interface leaves, to the keys of the address entries, to the PHC's
    frequency adjustment, to the PTP core's range check and to the conduit's
    handling of frames that look tagged, and with the PPPoE case waiting for
    the bench's end of the session. 30 of 30 passed.

``2026-10-03-run06``
    The same cases and the new multicast case on the RAM image built from
    ``run06``, after the MTU of 1508 for PPPoE on the conduit, the GEM
    interfaces and the PPPoE client (RFC 4638), and the switch's part in the
    bridge's multicast snooping: IGMP and MLD copied to the CPU, the PON port
    in every group, the multicast that no host asked for dropped while a
    router sits behind the PON, and the IPv6 protocol of a flower rule. 31 of
    31 passed.

``2026-10-03-run07``
    The same cases and the new VLAN tag case on the RAM image built from
    ``run07``, after the switch's tag operations through tc flower: a
    C-tag pushed onto untagged frames and popped, an S-tag pushed in front of
    a C-tag or onto an untagged frame, the VID and priority of a tag changed
    and its priority matched; omcid's extended VLAN rules that push a C-tag
    onto untagged frames and set tag priorities; and one MIB upload for both
    OMCI priorities. 32 of 32 passed.

``2026-10-03-run08``
    The same cases on the RAM image built from ``run08``, the VLAN tag
    case now also taking an S-tag off downstream, after the switch's
    recognition of S-tags on the SFP and PON ports, the VLAN rules that match,
    rewrite and take off an S-tag, and omcid's extended VLAN rules that put an
    S-tag in front of a C-tag upstream and take it off downstream. 32 of 32
    passed.

``2026-10-03-run09``
    The same cases and the new mirror case on the RAM image built from
    ``run09``, the kernel log now read after the other cases and failing
    on a switchdev object that a driver failed, after port mirroring in both
    directions at once and its reset at setup, the host's own multicast groups
    stopped at an offloaded GEM interface, and the optics' I2C bus at 100 kHz
    with each diagnostic word read in one transfer. 33 of 33 passed.

``2026-10-03-run10``
    The same cases and the new cases of the hardware clock, the address table
    and the shaper, on the RAM image built from ``run10``, after the fixes
    of the open issues: the PON class's devices, dumps, OMCI reads and
    unregistration, the conduit's transmit test, counters and dsts, the switch
    driver's table reads, rule removal, apptrust, clock reads and devlink
    teardown, the signal-detect limit, the parked upstream streams, the l3flow
    view, the order of the optics table's checks, the early console and the
    GPIO bank's compatible. 36 of 36 passed.

The series was rebased after the first, the second and the fourth run, before
any of it was published, so the commits they name are not in the history. The
same code, as far as the stick and the bench host run it, is at
``run01``, ``run02`` and ``run04``: the rebase added
documentation, and for the first run changed test data of the unit tests
alone.
