.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====
omcid
=====

The OMCI daemon of the ONU: the side of ITU-T G.988 that answers the OLT.

It holds the management information base, answers each request of the OLT
in the baseline message format, and programs the data path from what the
OLT configures: GEM ports, bridges and the VLAN rules of the extended VLAN
tagging operation. It notifies the OLT of alarms, among them those it
judges itself: signal fail and degrade from the BIP errors of the
downstream, and the optical alarms from the readings of the transceiver,
whose levels it keeps in the ANI-G. It counts the performance monitoring
of the Ethernet UNI, the bridge ports, the GEM ports and the downstream FEC
in fifteen-minute intervals, which Synchronize time places, and raises the
threshold crossing alerts that the threshold data ask for. It reports the
result of the optical line supervision test, the addresses the switch
forwards by in the bridge table of each bridge port, and through the VEIP
whether the stick routes. It has the switch carry out the filters of the
bridge ports, the groups of addresses and EtherTypes each port filters and
the addresses it filters or alone admits, and the PPPoE filter of the
Ethernet UNI; a filter the switch cannot carry out, an admitting one on the
UNI side, is kept and said in the log. It holds the
groups that the bridge snoops on the subscriber side to the OLT's multicast
policy: the groups each profile allows, in full or for preview, the most
groups and bandwidth, immediate leave and the IGMP timers, and static
groups; under such a policy it sends the subscriber's IGMP and MLD
upstream itself, a refused join held back, with the profile's tag and
rate, and gives multicast the profile's downstream tag; and it reports
the active groups and the join counts, and the groups whose stream
stopped, with the lost multicast group alarm. A local control socket lets
tools read the MIB, follow the frames and see what became of each VLAN
rule.

Layout
======

``src/omci/``
   the message frame, the engine, alarms and the bit error rate and optical
   readings they are judged by, notifications, the clock and tests
``src/mib/``
   the store of instances and attributes
``src/me/``
   the managed entity classes: the class table, the instances the ONU makes
   itself, MIB upload, table attributes, the values an attribute takes, and
   the data path (GEM ports, bridges, VLAN rules, performance monitoring)
``src/mcast/``
   multicast: the tables of the multicast operations profile and of the
   multicast subscriber config info, read into whole rows; the IGMP and MLD
   messages of the subscriber, taken apart; and the agent that holds the
   bridge's snooped groups to the OLT's policy, keeps the multicast
   subscriber monitor and counts the groups' downstream frames to tell the
   lost ones
``src/hw/``
   the interface to the hardware, and the host backend that stands in for it
``src/sys/``
   logging, the transport of frames and the control socket
``src/libomcid/`` and ``include/omcid/``
   the client library of the control socket
``tools/``
   command-line tools, the table generator and the gates
``tests/``
   the tests
``data/g988-me-min.tsv``
   the managed entity classes, from which ``src/me/table.[ch]`` are made

Building and testing
====================

::

   make                # the daemon and the tools, in build/
   make check          # the tests
   make coverage       # the tests, failing unless every line of src/ ran
   make check-style CLANG_FORMAT=clang-format CHECKPATCH=../linux/scripts/checkpatch.pl

This build runs on the host backend. Frames come on the standard input and
the answers go to the standard output, so recorded frames can be played
through the daemon::

   build/omcid -p < frames.bin > answers.bin

The daemon reads the clock between the frames, so a second that passes there
has it judge the alarms again, sample the counters and advance its timers,
and the answers can depend on how fast the frames come. ``-t N`` holds the
clock at second N, and the same frames then give the same answers::

   build/omcid -p -t 1 < frames.bin > answers.bin

``make board`` builds the daemon for the board, with the backend of the
board's kernel interfaces (``target/``): the OMCI port ``/dev/pon0omci0``, the
"pon" generic netlink family for the ONU's state, T-CONTs and FEC counts, the
"gem" link kind for the GEM ports, the ethtool statistics of the switch ports
and the GEM ports for the performance monitoring, the bridge ``omcibr0`` with
the host link ``sfp``, and tc flower rules for the VLAN rules, the
multicast traps, tags and counts and the bridge port filters, which the
switch carries out::

   omcid [-d DEVICE] [-r] [-v]...

``-r`` is router mode: the stick routes the subscriber's traffic itself. The
daemon answers the OLT as usual, but leaves the bridge and the VLAN rules that
the OLT provisions out of the hardware, so the GEM interfaces stay out of any
bridge and the stick's router uses them.
