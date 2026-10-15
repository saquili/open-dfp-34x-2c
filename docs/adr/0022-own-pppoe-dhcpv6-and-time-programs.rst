.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==============================================================
ADR 0022: Programs of our own for PPPoE, DHCPv6 and the time
==============================================================

:Status: Accepted
:Date: 2026-10-15
:Decided-by: the owner, as each program began (2026-10-02, 2026-10-07 and
  2026-10-14); recorded 2026-10-15

.. note::

   In plain words: the stick has too little flash for the usual PPPoE,
   DHCPv6 and time programs, so it has three small ones of its own that do
   only what the stick needs.

Context
=======

The root filesystem of a bank, with the C library, BusyBox, procd, netifd,
the ssh server, omcid, the stick's own programs and the panel, has to fit
the partition ``r0`` of 2031616 bytes (``docs/handbook/building.rst``,
"Sizes and budgets"). On 2026-10-11 that left 743747 bytes for everything
still to come, and it took a C library shared and cut to what the programs
use (ADR 0011) and the removal of iproute2 (ADR 0020) to get there.

The stick needs three things that general programs do:

* a PPPoE client (RFC 2516) with PAP or CHAP, IPCP and IPv6CP, for up to
  seven sessions at once, whose sessions the switch can offload;
* a DHCPv6 client that asks for a delegated prefix (RFC 8415), and on the
  link to the router a server for that one prefix and router advertisements
  (RFC 4861), so that the router behind the stick gets the session's IPv6;
* a clock: the stick has none that runs while it is off, and the time may
  come from the host by NTP or PTP, or from the OLT's time of day.

The usual programs for these -- pppd with its PPPoE plugin, a DHCPv6 client
and a separate relay or server, an NTP daemon and a PTP daemon -- each bring
much that the stick does not use: other link types, plugins, scripting hooks,
options and the libraries they link. A fork of each, cut until it fits, was
the first idea. It keeps the structure, the configuration and the
dependencies of the whole program around the part that is left, and every
fix upstream has to be carried over by hand into code that no longer looks
like upstream's. And no general program reads the OLT's time of day.

Decision
========

The stick has three programs of its own, written for this system from the
RFCs and IEEE 1588 they implement, each doing only its case: ``pppoed``,
``dhcp6d`` and ``timed``. Each has its own directory, README and tests that
run on the host, and is started by procd and netifd like any other program
of the image.

Consequences
============

* They share the cut C library, and every build checks that the image
  still fits its partition (``tools/image-budget``).
* They cover the cases of this stick and no more. A line or a server that
  needs what they leave out -- another authentication, another kind of
  address assignment, a protocol option an operator insists on -- needs a
  change to them, and a report of such a line is the way to find it.
* The project maintains them. Each stands on its own behind its command
  line and its use by netifd and procd, so any one of them can be replaced
  later, by a cut general program or something else, without touching the
  others.
