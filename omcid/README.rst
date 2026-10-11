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
   logging, the transport of frames, the control socket and what the daemon
   publishes of the MIB
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
   make check-kernel   # the board's backend against this host's kernel, as root

This build runs on the host backend. Frames come on the standard input and
the answers go to the standard output, so recorded frames can be played
through the daemon::

   build/omcid -p < frames.bin > answers.bin

The daemon reads the clock between the frames, so a second that passes there
has it judge the alarms again, sample the counters and advance its timers,
and the answers can depend on how fast the frames come. ``-t N`` holds the
clock at second N, and the same frames then give the same answers::

   build/omcid -p -t 1 < frames.bin > answers.bin

``make check-kernel`` runs the board's backend, ``target/hw_kernel.c``, in a
network namespace of its own, with dummy interfaces named ``sfp`` and ``pon``,
and reads back with tc the filters it adds: the VLAN rules, the bridge port
filters, the multicast tags and the drops at the UNI that an earlier run
left. It builds the backend with ``OMCID_TC_FLAGS=0``, so that the filters,
which on the board go to the switch alone, go into software. It needs root
and tc, and says that it skipped without them; on a kernel without the vlan
action it skips the VLAN rules and the tags. The parts that need the stick's
PON family are tried on the board.

``make board`` builds the daemon for the board, with the backend of the
board's kernel interfaces (``target/``): the OMCI port ``/dev/pon0omci0``, the
"pon" generic netlink family for the ONU's state, T-CONTs and FEC counts, the
"gem" link kind for the GEM ports, the ethtool statistics of the switch ports
and the GEM ports for the performance monitoring, the bridge ``omcibr0`` with
the host link ``sfp``, and tc flower rules for the VLAN rules, the
multicast traps, tags and counts and the bridge port filters, which the
switch carries out::

   omcid [-d DEVICE] [-m FILE] [-r] [-x PROGRAM] [-v]...

``-r`` is router mode: the stick routes the subscriber's traffic itself. The
daemon answers the OLT as usual, but leaves the bridge and the VLAN rules that
the OLT provisions out of the hardware, so the GEM interfaces stay out of any
bridge and the stick's router uses them.

Beside the stick's own PPPoE sessions the daemon bridges what the OLT
provisions but the sessions' VLANs, which ``onu-apply`` writes, by their VID on
the PON side, one per line, to ``/run/onu-held-vlans`` before it sends SIGUSR2.
The VLAN rules into those VLANs stay out of the hardware, and so does a bridge
port that carries nothing else, such as the GEM ports of an Internet service
whose PPPoE the stick holds; the subscriber's frames of those VLANs are
dropped as they come in on ``sfp``. Television and telephone go on crossing
the bridge. A bridge that filters no VLAN would carry the sessions' VLANs too,
and the stick then leaves it out altogether, as in router mode.

The daemon also writes the OLT's VLAN translations to ``/run/omcid-xlate``: a
``SUB NET`` line for each VID it translates from the subscriber's side, with the
VID it leads into on the fibre. ``onu-apply`` runs a session that was given the
subscriber's VID, the one a router behind the stick would use, in the VID it
leads into. And it writes the VLANs the OLT lets through each GEM port to
``/run/omcid-gem-vlans``: a ``VID PORT-ID`` line for each VID a GEM port's
bridge port carries. ``onu-apply`` runs a session whose GEM port is ``auto`` on
the GEM port that carries the session's VID on the fibre. Both files are
replaced whole, and only when what they say changes, so that a session follows
its service when the OLT moves it to another VID or another GEM port.

``-x PROGRAM`` tells a program of each such change: the daemon runs it with
the argument ``omcid`` after it replaces either file and after it makes or
takes away a GEM interface, and does not wait for it. On the stick the
program is ``onu-apply``, which follows the change at once.

With ``make board UBUS=1`` the daemon is on ubus (ADR 0014), as the object
``omcid``. Its method ``status`` gives the number of instances and the counts
of the frames; ``mib`` gives the MIB: every instance of the classes of its
argument ``classes``, an array of class numbers, in that order, or of every
class without it, each attribute in hexadecimal and a table's rows end to end,
and the counts of the frames, in at most ``max`` bytes of JSON text when
given; an instance that does not fit is left out with the rest, and
``truncated`` says so. The answers are blobmsg, ubus's own binary form,
written straight from the MIB with no text in between: a reader that needs
JSON, such as the panel's, makes it once, at the end. The ONU-G's logical password is never given, only whether one is set
(``logical_password_set``). Each change the OLT makes sends the event
``omcid``, with the status. A daemon that finds no ubusd, or loses it, tries
again every ten seconds, and serves the OLT meanwhile.

``-m FILE`` keeps the MIB in a file, so that a daemon started again goes on
as the OLT set it up, and the OLT sees no new ONU. The daemon replaces the
file whole after each request that may change what the OLT set up (MIB reset,
Create, Delete, Set and Synchronize time) and when it stops. One that starts
and finds the file takes up from it the MIB, the time of day and where the
performance monitoring intervals stand, and programs the data path again
from the MIB: what it finds in the kernel as the MIB has it, the GEM
interfaces, the T-CONTs' Alloc-IDs and the bridge, stays as it is. What a
start again loses is the counts of the interval in progress and the alarms
judged so far, which it judges anew. Traffic does not notice: on the stick holding
two PPPoE sessions, with a TCP upload of 905 Mb/s through them, the frames
that left by the PON port, read every 0.1 s, went on at their rate through
nine restarts, five after SIGTERM and four after SIGKILL, each back in about
2.5 s, procd's wait of 2 s with it. The upload's stalls of 0.1 to 0.3 s came
as often without a restart: they are TCP's, after a burst of frames that the
switch dropped at the SFP port, whose 2.5 Gb/s the PON's upstream does not
drain. A file of another layout, such as one
of another build, it does not take: it starts afresh and replaces the file
at the next change. On the stick the file is ``/run/omcid.mib``, which goes
with a restart of the stick, as the OLT's set-up does.
