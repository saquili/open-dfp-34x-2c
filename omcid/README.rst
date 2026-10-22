.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====
omcid
=====

The OMCI daemon of the ONU: the side of ITU-T G.988 that answers the OLT.

It holds the management information base, answers each request of the OLT
in the baseline message format, and programs the data path from what the
OLT configures: GEM ports, bridges and the VLAN rules of the extended VLAN
tagging operation, the rate at which the UNI sends to the subscriber, from
the outbound traffic descriptor of its bridge port, and the most each GEM
port takes from the OLT, from the traffic descriptor its downstream pointer
names (the peak rate, with the peak burst size, which the switch holds to with
a meter). It notifies the OLT of alarms, among them those it
judges itself: signal fail and degrade from the BIP errors of the
downstream, and the optical alarms from the readings of the transceiver,
whose levels it keeps in the ANI-G. It counts the performance monitoring
of the Ethernet UNI, the bridge ports, the GEM ports and the downstream FEC
in fifteen-minute intervals, which Synchronize time places, and raises the
threshold crossing alerts that the threshold data ask for. The performance
monitoring of the IP hosts, of the TCP/UDP ports, of calls, RTP sessions and
the SIP agent, and of the GAL Ethernet profile is kept the same way and
reads 0, as the device counts none of what it names. The IPv6 hosts the
device has are told to the OLT with their addresses, routers, DNS servers and
prefixes as tables; a change the ONU makes to a table is announced by the
table's name alone, as G.988 A.3.20 has it, and the OLT reads the rows
again. It gives the
kernel the time of day that the OLT sets in the OLT-G, the frame it names and
the time of that frame, and the switch's PTP clock takes the time at that
frame; once the kernel took it, omcid tells the time daemon in
``/run/omcid/tod``. It reports the
result of the optical line supervision test, the addresses the switch
forwards by in the bridge table of each bridge port, and through the VEIP
whether the stick routes. It keeps the encryption state of each GEM port
network CTP in step with the Port-IDs that the OLT encrypts with the PLOAM
message Encrypted_Port-ID, which the kernel reports and announces: 1, AES-128,
for an encrypted one and 0 for another (G.988 9.2.3), read again when the
kernel announces a change, when the GEM ports are programmed, and before the
OLT reads the state; G.988 has no attribute value change for it. It has the
switch carry out the filters of the bridge ports, the groups of addresses and EtherTypes each port filters and
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
stopped, with the lost multicast group alarm. With the profile's IGMP
function at 1 or 2 it speaks for the subscribers to the OLT instead
(`The multicast proxy`_). A local control socket lets tools read the MIB,
follow the frames and see what became of each VLAN rule.

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
   the data path (GEM ports, bridges, VLAN rules and the rules of the
   services, performance monitoring)
``src/mcast/``
   multicast: the tables of the multicast operations profile and of the
   multicast subscriber config info, read into whole rows; the IGMP and MLD
   messages of the subscriber and the OLT's queries, taken apart; the
   messages the ONU makes itself, and the proxy that tells the OLT what the
   subscribers hold; and the agent that holds the bridge's snooped groups to
   the OLT's policy, keeps the multicast subscriber monitor and counts the
   groups' downstream frames to tell the lost ones
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

The ports told to the OLT
=========================

The stick tells the OLT of the subscriber ports the ONT it stands in for
had, as the identity file says (``target/hw_kernel.c``,
``identity_ports()``): ``uni_ports``, 1 to 8 Ethernet UNIs of slot 1, one
cardholder and circuit pack of that many ports, each UNI the one SFP lane;
and ``uni_report``, ``pptp`` for the Ethernet UNIs alone, without the VEIP,
``veip`` for the VEIP alone, without the UNIs, their slot and their
downstream queues, or both; and ``pots_ports``, 0 to 2 phone sockets of
slot 2, whatever ``uni_report`` says. A change of them is a change of the
identity: the stick registers again and builds its MIB anew. The ubus
method ``login`` gives the PPPoE login an OLT sent in an instance of the
Authentication security method (class 148): its usernames together, its
password and realm (``omci_publish_login()``), of an instance that no SIP
user data points at.

The phone sockets
=================

The stick has no phone socket that takes a wire. It tells the OLT of the
sockets of the ONT it stands in for, so that the OLT sets the phone up, and
a person's VoIP device behind the stick uses what the OLT sent. A socket is
a PPTP POTS UNI (class 53, G.988 9.9.1) with its UNI-G and VoIP line status
(9.9.11), in a slot whose circuit pack is of type 32, POTS; VoIP config
data (9.9.18) offers SIP, set up through OMCI. The OLT's classes of SIP
VoIP are served as G.988 defines them (9.9.2 to 9.9.10): SIP user data and
agent config data, the VoIP voice CTP, media, voice service and RTP
profiles, the application service profile, the feature access codes and
the dial plan table. Their tables are known by their leading index, a zero
one clearing them (the tone, ringing and SIP response tables), or by their
number with a removal action (the dial plan).

The voice reaches the device through the IP host the OLT points it at: a
bridge port of an IP host (type 4) is the SFP lane, as a UNI's is, and the
VLAN rules of an IP host (association 3, or such a bridge port) that tag
its untagged frames would take the untagged frames of every service on the
one lane, so they are given a host VLAN of their own: the fibre's when free,
else the first free one, a move the stick made that the service rules list
(``omci_vlan_phone()``), or a person's move. The ubus method ``phone``
gives what the panel shows (``omci_publish_phone()``): how the OLT sets the
phone up, each socket's number, display name and SIP user, the SIP servers,
the codecs, how DTMF goes and the voice's VLAN, never a password; and
``phone_secrets`` the SIP passwords, which pond shows with root's password.

Not measured on the board, nor with an operator's OLT: the lab OLT sets up
no phone.

The rules of the services
=========================

The SFP lane is the one way to the host of every service the OLT sets up, on
any Ethernet UNI or on the VEIP. A person's rules (``src/me/svcrule.h``),
which onu-apply writes from the settings (onu-config's ``SVCn``) into
``/run/omcid/rules``, change what the hardware is given of the VLAN rules,
never the MIB: a service, known by its VLAN on the fibre, goes to the host in
another VLAN, stays off the host, or keeps the rules of the Ethernet UNIs or
of the VEIP alone. When the rules of two different outputs would take one
host VLAN for two services, the programming moves the later service to its
own VLAN on the fibre, or to the first free one, where it left the later rule
out before; without rules and without such a clash the hardware is given
what it was before. A new file of rules programs the data path again (the
backend's ``reprogram_due``). ``omcivlan`` names a rule that a person's rule
keeps out ``local``, and the ubus method ``rules`` gives the rules in force, a
person's and the moves made, which pond serves as the value
``service_rules``.

The multicast proxy
===================

The IGMP function of the multicast operations profile (G.988 9.3.27) has
three values. At 0 the agent only snoops, and passes each message of the
subscribers on to the OLT as it came, under the OLT's policy. At 1,
snooping with proxy reporting, and at 2, IGMP proxy (RFC 4605 is the
model), the OLT hears of the groups from the ONU itself:

* a join goes up once, for the first subscriber of a group, and a leave once,
  for the last; more subscribers, and the subscribers' repeated reports, tell
  the OLT nothing;
* a general query of the OLT is answered with the groups held, after a time
  within the response time that the query allows, and a query of one group
  at once if it is held; the messages are IGMP version 1 to 3 or MLD version
  1 and 2, as the profile's version says, and for the protocol the profile is
  not of, the highest;
* when a subscriber leaves a group and the profile has no immediate leave,
  the agent asks the subscribers with queries of the group (the profile's
  robustness and last member interval), in the VLAN the group was joined in,
  and lets the group go from the bridge when none answers. The bridge alone
  shortens the group's timer at a leave and lets the group go, without a
  query, while another subscriber of the port still holds it;
* at 2 the ONU is also the querier of the subscribers: it sends general
  queries, IGMP and MLD, at the profile's interval and response time.

The messages are made in ``src/mcast/message.c``, whose source address is the
profile's querier address (attribute 11) for IGMP and the link-local address
of the ONU's Ethernet address for MLD. A message for a group carries the VLAN
and priority of the joins of it, and goes up with the profile's upstream tag
control, the VLAN rules, GEM port and rate as the subscribers' own do. ``src/mcast/proxy.c``
keeps what the OLT was told, so that a message that could not be sent is
made again a second later.

What it does not do: the switch also forwards the OLT's queries to the
subscribers, so that the subscribers answer them, and the agent takes in
what they answer without telling the OLT; a state change is sent once, not
robustness times, and the next query of the OLT repairs a loss; at most 64
groups and 8 sources of one group are told; and the profile's upstream IGMP
rate holds the proxy's messages back, which are sent again, as it holds the
subscribers' back. The kernel bridge snoops the group of a join that names
sources without the sources, so such a join is told to the OLT as a join of
the group from every source. Where the agent cannot hold the subscribers' messages
back for itself (a backend that cannot send, an Ethernet address that cannot
be read, no subscriber side to hold), the function is carried out as 0, and
the log says so.

Where the VLAN rules translate the subscriber's VLAN to the one the OLT sends
the group in, the bridge holds a snooped group under the first, the VLAN of
the join, and the switch looks the group's downstream frames up by the VID
they arrive with, the second. The agent therefore files each group it allows
under the VLAN of the row that allowed it as well (``alias_sync()``), and
takes that entry away with the group; a permanent group of the bridge that is
no static group and no such entry is a leftover of an agent that ended, and
goes. Measured on an operator's line, 2026-10-13: with the group under the
subscriber's VLAN alone, the PON port of the switch received 5,800 multicast
frames a second after a join and discarded all of them in forwarding; with
one entry added by hand under the OLT's VLAN, the frames of that group, 1,290
a second, left by the SFP port and reached the host.

What was measured on the board (the ODI DFP-34X-2C3, 2026-10-13): the lab OLT
provisions nothing, so the recorded provisioning of an operator's OLT, with
its multicast operations profile, was written into the OMCI port's inject
file (``CONFIG_PON_OMCI_INJECT``, a test image) and the profile's function
was changed with further Set messages. Subscribers were frames sent into
``sfp`` and the frames of the stick were read on every interface.

* Function 0: a join in the subscriber VLAN went up one GEM port as it came,
  its VLAN translated by the rules.
* Function 1: the first join of a group went up as a message of the stick,
  from its Ethernet address and the profile's querier address, in the VLAN
  the rules give the join's, and a second subscriber's join and repeated
  reports sent nothing. With immediate leave a leave went up at once. Without
  it, the stick queried the group in the subscribers' VLAN, twice a second
  apart when none answered and then told the OLT, and a single answer of
  another subscriber kept the group and told the OLT nothing. With the
  profile at version 3 the messages were IGMPv3 reports.
* Function 2: the stick sent IGMP and MLD general queries at the profile's
  interval, the IGMP ones in the VLAN of the groups held, and a subscriber
  that did not answer lost its group after the profile's membership time,
  with a leave to the OLT. The MLD queries and their checksums were read by
  the router's kernel, which answered them.
* A query that arrived on a GEM interface (made by a tc rule on ``sfp``
  that redirects a VLAN of the test to ``gem257``) was answered with the
  groups held, after a time within the response time it allowed, and a query
  of one group was answered at once when the group was held and not when it
  was not.

The packet socket now reads every interface, to tell the OLT's side from the
subscriber's. Two PPPoE sessions at once, TCP from the host behind the stick
to the router's side: 905, 899, 912, 912 and 902 Mb/s up and 881 and 881
down with it, against 905, 908, 898, 896 and 894 up and 880 and 882 down
with the daemon before, each run 15 s, the stick's CPU at 5 per cent.

Not measured: queries that come from the OLT itself, since the lab OLT passes
only the GEM ports it knows and the stick had none of those in the
provisioning; and the MLD proxy, the recorded policy having no IPv6 group.
The proxy's messages are tested on the host for both.

Television on the stick's panel
===============================

**The priority of IGMP.** At the profile's upstream IGMP tag control 1 and 2
the OLT's tag gives the subscriber's messages their priority. At 0 (the VLAN
rules) and 3 (the VID replaced) the subscriber's own goes up, and where the
operator's rules take the VLAN at one priority alone, or the GEM ports of the
VLAN carry some priorities alone, a message of another one is lost on the
way. The agent gives such a message, one with a C-tag, its own priority when
the rules and a GEM port take that, and otherwise the first they take of the
priorities a rule names for its VID and then 0 to 7 (``upstream_frame()``):
the router behind the stick needs no setting for it. A person can have the
subscriber's own priority kept, IGMP_PRIORITY=host of onu-config, which
onu-apply writes into ``/run/omcid/igmp-priority`` and the agent reads at each
message (the backend's ``igmp_host_priority``); a message the rules would have
taken at another priority is then counted as dropped.

**What the panel shows.** The ubus method ``iptv`` (``omci_mcast_publish()``)
gives the OLT's policy, the groups the bridge forwards with who joined them
and the stream's bits a second, the last 16 joins the policy refused with
why, the last 32 events (a join, a leave, a refusal, a query of the OLT, a
stream lost and back), the counts since the start, the OLT's latest query,
and who gives IGMP its priority; pond serves it as the value ``iptv``. The
rate is that of the bytes the switch counts for each group: the counting
rule of a group passes its frames, and the switch's driver gives such a rule
a counter of bytes, where a rule that drops or traps frames counts frames.
A group whose bytes do not grow for 10 s is lost, as before.

**A test of a group.** The ubus method ``tv_test`` (``omci_mcast_test_start()``)
joins a group from the stick, as a set-top box's join would go up, in its
VLAN and judged by the policy, from the stick's Ethernet address and the
profile's querier address, and counts the group's bytes. The bridge holds no
entry of the group for it, so the switch does not forward the stream to the
host. Its arguments are numbers, as pond's calls carry them: ``group``, the
IPv4 address, ``vlan``, 0 for untagged, and ``seconds``, 1 to 30; a call with
none reads the count, and one with ``stop`` ends the test with a leave. The
agent ends a test itself 5 s after its time when nobody did. pond's
``/api/iptv/test`` reads the count every 50 ms for the time to the first
bytes.

Not measured on the board yet: the priority given to a real operator's
messages, the bytes the switch counts, and a test of a group of the lab's
OLT; all of it is tested on the host backend.

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
left, and the tbf that limits the UNI. It also sends IGMP queries over pairs
of virtual Ethernet interfaces to see that a message of ``sfp`` is the
subscriber's, one of a GEM interface the OLT's and one of any other none, and
that a message goes out of ``sfp`` or of a GEM interface. It builds the
backend with ``OMCID_TC_FLAGS=0``, so that the filters, which on the board go
to the switch alone, go into software. It needs root
and tc, and says that it skipped without them; on a kernel without the vlan
action it skips the VLAN rules and the tags, and without the tbf qdisc the
UNI's rate. Before its namespace, where the kernel announces, it reads the
encrypted Port-IDs from the "pon" family and follows their announcements over
the PON class's stand-in MAC, ``pon_sim``, when the kernel has it.
``tools/vm/vm omcid-kernel`` runs it on a kernel of this repository's tree in
qemu, which has every action it uses and the stand-in
(``tools/vm/README.rst``). The other parts that need the stick's PON family
are tried on the board.

``make board`` builds the daemon for the board, with the backend of the
board's kernel interfaces (``target/``): the OMCI port ``/dev/pon0omci0``, the
"pon" generic netlink family for the ONU's state, T-CONTs, FEC counts and
encrypted Port-IDs, and its group "monitor" for their changes, the
"gem" link kind for the GEM ports, the ethtool statistics of the switch ports
and the GEM ports for the performance monitoring, the bridge ``omcibr0`` with
the host link ``sfp``, tc flower rules for the VLAN rules, the multicast
traps, tags and counts and the bridge port filters, and a tbf at the root of
``sfp`` for the UNI's rate, which the switch carries out::

   omcid [-d DEVICE] [-m FILE] [-r] [-x PROGRAM] [-v]...

``-r`` is router mode: the stick routes the subscriber's traffic itself. The
daemon answers the OLT as usual, but leaves the bridge and the VLAN rules that
the OLT provisions out of the hardware, so the GEM interfaces stay out of any
bridge and the stick's router uses them.

Beside the stick's own PPPoE sessions the daemon bridges what the OLT
provisions but the sessions' VLANs, which ``onu-apply`` writes, by their VID on
the PON side, one per line, to ``/run/onu-held-vlans`` before it sends SIGUSR2,
and waits for the daemon to say in ``/run/omcid/applied``, ``bridge`` or
``router``, that it programmed the data path again for the mode the signal
asked for.
The VLAN rules into those VLANs stay out of the hardware, and so does a bridge
port that carries nothing else, such as the GEM ports of an Internet service
whose PPPoE the stick holds; the subscriber's frames of those VLANs are
dropped as they come in on ``sfp``. Television and telephone go on crossing
the bridge. A bridge that filters no VLAN would carry the sessions' VLANs too,
and the stick then leaves it out altogether, as in router mode.

The daemon also writes the OLT's VLAN translations to ``/run/omcid/xlate``: a
``SUB NET`` line for each VID it translates from the subscriber's side, with the
VID it leads into on the fibre. ``onu-apply`` runs a session that was given the
subscriber's VID, the one a router behind the stick would use, in the VID it
leads into. And it writes the VLANs the OLT lets through each GEM port to
``/run/omcid/gem-vlans``: a ``VID PORT-ID`` line for each VID a GEM port's
bridge port carries. ``onu-apply`` runs a session whose GEM port is ``auto`` on
the GEM port that carries the session's VID on the fibre. Both files are
replaced whole, and only when what they say changes, so that a session follows
its service when the OLT moves it to another VID or another GEM port.

``-x PROGRAM`` tells a program of each such change: the daemon runs it with
the argument ``omcid`` after it replaces either file and after it makes or
takes away a GEM interface, and does not wait for it. On the stick the
program is ``onu-apply``, which follows the change at once.

With ``make board UBUS=1`` the daemon is on ubus (ADR 0011), as the object
``omcid``. Its method ``status`` gives the number of instances and the counts
of the frames; ``mib`` gives the MIB: every instance of the classes of its
argument ``classes``, an array of class numbers, in that order, or of every
class without it, each attribute in hexadecimal and a table's rows end to end,
and the counts of the frames, in at most ``max`` bytes of JSON text when
given; an instance that does not fit is left out with the rest, and
``truncated`` says so. The answers are blobmsg, ubus's own binary form,
written straight from the MIB with no text in between: a reader that needs
JSON, such as the panel's, makes it once, at the end. The ONU-G's logical password is never given, only whether one is set
(``logical_password_set``). ``frames`` gives the conversation with the OLT:
the daemon keeps the last 1024 frames it received and sent, numbered from its
start, each with the wall clock when it passed (the second ``-t`` holds the
clock at, under ``-t``), what it did with the frame (the verdicts of the
control socket's tap) and its message type, and gives them from the number
``since`` on, at most ``max`` and never more than 200 at a time, with the next
frame's number (``head``) and the oldest one kept (``first``); a ``since``
older than the oldest, or past the head, which a reader of a daemon that has
since started again holds, gives the oldest. The logical password is zeroed
in a frame before it is kept, wherever a frame carries the ONU-G's values (a
Set, a Get's answer, an attribute change, a step of the MIB upload), and
``masked`` says so. It sends no event: a change the OLT makes is
told to the program of ``-x``. A daemon that finds no ubusd, or loses it,
tries again every ten seconds, and serves the OLT meanwhile.

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
at the next change. On the stick the file is ``/run/omcid/mib``, which goes
with a restart of the stick, as the OLT's set-up does.
