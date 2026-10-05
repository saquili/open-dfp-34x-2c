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

``2026-10-04-run11``
    The same cases and the new cases of the limit on the frames handed to the
    CPU, the order of storm-control rules and the PCP rewrite, with the ACL
    case now checking the rules' hit counts and the hardware clock's case
    reading both pulses in one command, on the RAM image built from
    ``run11``: the CPU limit with NAPI deferral on the conduit, storm
    control policed by the rule of the lowest priority value, drops and traps
    of broadcast, the group bit and ARP as ACL rules, the ACL's hit counts, the
    CAM of the address table, the half PTP tag of a message to the stick's own
    address and the PCP rewrite from the DCB rewrite table. 39 of 39 passed.

``2026-10-04-run12``
    The same cases and the new case of the host link's search, which takes the
    bench host's port to 1000BaseX with in-band negotiation and back and runs
    before the watchdog's reset, on the RAM image built from
    ``run12``: the SerDes lane to the host finding the host's setting
    itself. 40 of 40 passed.

``2026-10-04-run13``
    The same cases on the RAM image built from ``run13``:
    Extended_Burst_Length taken only before the first serial-number request,
    the burst overhead clamped at 128 bytes with the type 1 and type 2
    preambles, the PEE alarm, the PST answer, the declared transmit power
    level, power levelling and Dying_Gasp before a restart. The watchdog
    reset is back in 83 s, the loader now counting 5 s. 39 of 40 passed:
    ``omci.channel`` found one OMCI message dropped, which went with the
    OMCI channel when the OLT deactivated the ONU 18 s into its first O5
    session, before the run began; the kernel log shows the deactivation and
    the ONU back in O5 2 s later.

``2026-10-04-run14``
    The same cases on the RAM image built from ``run14``: the OMCI
    messages a channel's end discards counted apart, each GEM T-CONT with
    eight queues of its own, and the upstream attributes of a GEM link
    (priorities, queue, weight and rates), which omcid now takes from the
    priority queues, traffic schedulers, traffic descriptors and IEEE 802.1p
    mappers of the MIB. 40 of 40 passed; the upstream burst left at 963
    Mbit/s through the T-CONT's queue of the highest priority.

``2026-10-04-run15``
    The same cases and the new ``omci.tcont-map`` on the RAM image built from
    ``run15``: the T-CONTs bound to the Alloc-IDs that the OLT maps
    through OMCI, which omcid passes on with tcont-set, rather than in the
    order of the PLOAM messages. 41 of 41 passed; T-CONT 0 held the OLT's
    Alloc-ID, mapped to it.

``2026-10-04-run16``
    The same cases and the new ``omci.alarms`` on the RAM image built from
    ``run16``: omcid notifies the OLT of the alarms of the ANI-G,
    signal fail and degrade judged from the BIP errors and the optical alarms
    from the readings of the transceiver, and keeps its optical levels; the
    kernel reports the thresholds of the optics and the transmit power level,
    and ``optics.diagnostics`` judges each reading against the module's own
    warning thresholds. 42 of 42 passed; omcid raised no alarm on the clean
    line, at power level high.

``2026-10-04-run17``
    The same cases and the new ``data.vlan-priority`` on the RAM image built
    from ``run17``: omcid matches the filter priority of the rules of
    an extended VLAN tagging operation, takes the rules in the order of their
    filters and lets a later rule that gives the same tag to other frames
    work upstream only. 43 of 43 passed; of two priorities in one VLAN, the
    translation of one rewrote that priority's frames alone.

``2026-10-04-run18``
    The same cases on the RAM image built from ``run18``, after the
    driver's count of BIP errors in bits, its ACL rules that match an ICMP
    type and those that only count, and the multicast of omcid: the OLT's
    multicast policy over the groups the bridge snoops, the subscriber's IGMP
    and MLD sent upstream under it, the profile's downstream tag, the further
    multicast GEM ports and the lost groups. The lab OLT provisions no
    multicast policy, so the cases ran without one. 43 of 43 passed.

``2026-10-05-run19``
    The same cases on the RAM image built from ``run19``, after the
    driver's counts of each GEM port's frames and octets and of the downstream
    FEC corrections, the performance monitoring of omcid (the history data of
    the Ethernet UNI, the bridge ports, the GEM ports and FEC, the extended
    PM, threshold crossing alerts and Synchronize time), the standard classes
    the stick lacked (the VEIP, the LCT UNI, and the filter and bridge tables
    of each bridge port), and the driver's hold of the key switch for a new
    key and clearing of the keys when an activation ends. 43 of 43 passed.

``2026-10-05-run20``
    The same cases and the new case of the management VLAN on the RAM image
    built from ``run20``, after the driver's ACL binds each rule to its
    own port, matches a C-tag and the EtherType of a PPPoE session and lets a
    passing rule decide its frames, the boot script keeps the management VLAN
    off the PON, and omcid carries out the filters of the bridge ports. The lab
    OLT sets no filter, so the cases ran without one. 44 of 44 passed.

``2026-10-05-run21``
    The same cases and the new case of the PON side's silence on the RAM image
    built from ``run21``, after the boot script keeps the stick quiet
    towards the PON, omcid takes the versions a bench names and reports an
    unmapped T-CONT as the original did, gives the bridge ports the VLANs a
    translation and a port without a filter need, and discards the tagged
    frames that no VLAN rule takes. The lab OLT sets no VLAN filter, so the
    last two had nothing to do. 43 of 45 passed, and both failures lay with
    the bench: its ssh client shared the connection of the earlier cases with
    the probe that offers no key, which so got in; and since the bench host's
    restart its sender reaches about 420 Mb/s, under the 900 Mb/s the shaper
    case wants of the stream without a shaper, while every frame passed and
    the shaper held 200 Mb/s at 205. The reset case passed after the loader's
    countdown was stopped at the console and the image was RAM-booted by hand.

The series was rebased after the first, the second and the fourth run, before
any of it was published, so the commits they name are not in the history. The
same code, as far as the stick and the bench host run it, is at
``run01``, ``run02`` and ``run04``: the rebase added
documentation, and for the first run changed test data of the unit tests
alone.
