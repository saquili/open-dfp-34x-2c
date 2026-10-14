.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====
timed
=====

The clock of the stick. The stick has no clock that runs while it is off and
keeps no time in the flash, so it starts in 1970. timed sets the clock from the
host, in the two ways a host may give the time, and from the date the OLT gives
when the host gives none::

   timed [-n SERVER] [-p IFACE] [-o FILE] [-d DOMAIN] [-s FILE] [-f] [-v] [-D]

``-n`` names an NTP server (RFC 5905), by its address: the stick runs offline
and resolves no name. ``-p`` names the interface on which timed is a PTP slave
(IEEE 1588-2008: an ordinary clock that is never a master, with the delay
request-response mechanism), over layer 2 (EtherType 0x88f7 to
01:1b:19:00:00:00) and over UDP (224.0.1.129, ports 319 and 320) at once;
``-d`` gives its domain, 0 unless given. ``-o`` names the file in which omcid
writes the date the OLT gave. ``-s`` names the file of the state,
``/run/timed/state`` unless given. ``-f`` keeps the log on stderr rather than
the system log, ``-v`` logs every sample, and ``-D`` sets no clock: timed
measures and chooses, for the tests and for looking at what a host gives.

The sources
===========

One source sets the clock at a time, so that two never pull it two ways: the
best of those that are good, PTP before NTP before the OLT. A source is good
after three Sync messages in a row (PTP), one answer (NTP) or one date (the
OLT), and stays good while its samples keep coming: four Sync intervals and at
least 8 s for PTP, three polls of 64 s for NTP, ten minutes after its date for
the OLT. The source in use gives way to a better one as soon as that one is
good, and to a worse one only when it is not good any more.

PTP
   The master is the best of those whose Announce is heard, by the dataset
   comparison of clause 9.3.2; one of class 255, a slave-only clock, never is.
   A master not heard for three of its Announce intervals, and at least 8 s,
   is lost. Sync is taken one-step or two-step (with its Follow_Up), and the
   path delay comes from a Delay_Req every 2 s until it is known, then every
   16 s. The master's times are on its timescale: TAI less the UTC offset its
   Announce gives, 37 s when it gives none.
NTP
   timed polls the server every 2 s for the first four polls, every 16 s up
   to the twelfth, then every 64 s, and every 16 s again after three polls go
   unanswered. The time an answer came is the kernel's stamp of it, as the
   daemon may come to read it late on the stick's busy processor. Each answer
   is used once, unless its delay exceeds the least of the last eight by more
   than half of that, and at least 100 us: such an answer waited in a queue on
   one leg, and its offset is off by half the wait. An answer is refused when
   it is not a server's, does not answer the request it came for, is a
   kiss-o'-death (timed then waits 64 s), or comes from a server with no time
   to give: of stratum 0 or 16, with the leap alarm, or more than 16 s from
   its source.
The OLT
   An OLT may send the date and time with the OMCI message Synchronize time
   (G.988 A.3.33), to the second. omcid writes it in ``/run/omcid/clock`` with
   the monotonic time it came at (``utc=`` and ``monotonic_ns=`` lines), and
   timed takes the time now as the one given plus the time since; it looks at
   the file once a second. It is one reading, not a clock that runs on, and is
   used only when the host gives nothing. The OLT of the operator this stick
   runs on sends none.

Setting the clock
=================

A clock more than 128 ms from its source is stepped (``clock_settime``); a
nearer one is steered by its frequency alone (``adjtimex``, ``ADJ_FREQUENCY``),
by a proportional-integral servo as ptp4l and phc2sys of linuxptp steer theirs
(``src/servo.h``), with the kernel's own loop off. After a start, a step or a
change of source the servo takes two samples at least 4 s apart, sets the
frequency to the rate the clock drifted at between them, and steps the clock by
what it is out then if that is more than 20 us; from then on each sample
corrects what is left, with gains that follow the time between two samples (a
second for PTP, 2 to 64 s for NTP). The kernel's own loop, which an earlier
version used, could not follow the few parts per million the stick's crystal
is off: on the stick the offset grew by some 1.4 us a second under it. The
frequency of a timed before is taken over when procd starts it again.

The OLT's time is to the second: it steps a clock a second or more away and
does nothing to a nearer one. After a step the samples in flight are dropped,
being of the clock before it. The log tells each step ("clock stepped by
+1791893093.123456 s from ntp"), the first sample within the limit ("clock in
step with ptp, offset -0.000012 s"), each change of source ("time source: ptp
(was ntp)"), and a master found or lost.

The time a frame came is the kernel's stamp of it (``SO_TIMESTAMPNS``), and the
time timed sends a Delay_Req or an NTP request is read just before it sends it.
The switch stamps the PTP frames it gives the CPU on its own clock, but gives no
time of departure for one the CPU sends (``rtl9602c-ptp.c``), and its clock is
not the system clock: the software stamps, tens of microseconds off, are plenty
for the time of day.

The state
=========

timed tells what it does in the file of ``-s``, written whole every 10 s and at
each change of source, as ``KEY=VALUE`` lines. An offset is the stick's clock
less the source's, in nanoseconds.

``source``
   ``ptp``, ``ntp``, ``olt`` or ``none``: the source that sets the clock.
``state``
   ``locked``, a source in use and the clock within 1 ms of it; ``syncing``,
   a source in use and the clock farther; ``set``, the OLT's time in use;
   ``holdover``, no source now, but the clock was set and runs on;
   ``unsynchronized``, no source has set it since timed started.
``offset_ns``, ``updated``, ``steps``, ``freq_ppb``
   The offset of the last sample applied, the time of the writing in seconds
   since 1970 by the stick's clock, the steps since the start, and the
   correction of the clock's frequency in parts per billion, positive to speed
   it up.
``ntp_server``, ``ntp_good``, ``ntp_offset_ns``, ``ntp_delay_ns``, ``ntp_stratum``
   The server and whether it is good (``1`` or ``0``), and its last sample.
``ptp_interface``, ``ptp_good``, ``ptp_master``, ``ptp_via``, ``ptp_delay_ns``, ``ptp_offset_ns``
   The interface and whether PTP is good; the master's clock identity, the way
   it is heard (``l2`` or ``udp``), the path delay and the last offset.
``olt_file``, ``olt_good``, ``olt_utc``, ``olt_offset_ns``
   The file of the OLT's time and whether it is good; the time the OLT gave,
   in seconds since 1970, and its offset.

A source timed does not ask has no lines. The status daemon serves the file as
its value ``clock``, and the panel shows it under System, Settings.

On the stick
============

``onu-run time`` starts timed as the account ``ntp``, with the powers to set
the clock, to listen for PTP over layer 2 and to listen on PTP's UDP ports 319
and 320, below the 1024 an account may take without it (``onu-drop -c
sys_time,net_raw,net_bind_service``): NTP from ``NTP_SERVER`` of the stick's settings or,
without one, from the router at the other end of the management link, the
other address of its /30 or /31; PTP on the management link's interface;
the OLT's time from ``/run/omcid/clock``. When the management pair changes,
``onu-mgmt`` ends timed and procd starts it again for the new link. A host
gives the stick PTP with a master on its interface of the management VLAN, such
as ptp4l of linuxptp, over layer 2 or UDP over IPv4.

Building and testing::

   make           # build/timed
   make check     # the tests of the protocols, on any host

``tests/netns.sh`` runs a host that gives the time (``tools/lab/ntpserve`` and
``tools/lab/ptpmaster``) and timed in two network namespaces, and checks from
the state what timed chose: the OLT's time alone, then NTP, then PTP over layer
2, NTP again when the master goes, and PTP over UDP. timed runs dry there, as
the namespaces share one clock. It needs root, a privileged container for one
(see the script).
