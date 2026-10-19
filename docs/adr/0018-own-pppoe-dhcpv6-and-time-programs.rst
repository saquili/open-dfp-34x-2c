.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=======================================================
ADR 0018: Write our own PPPoE, DHCPv6 and time programs
=======================================================

:Status: Accepted
:Date: 2026-10-15
:Deciders: the maintainer

Summary
=======

The stick has too little flash for the usual PPPoE, DHCPv6 and time
programs, so it has three small ones of its own that do only what the stick
needs.

Context
=======

The root file system of a bank, with the C library, BusyBox, procd, netifd,
the ssh server, omcid, the stick's own programs and the panel, has to fit
the partition ``r0`` of 2,031,616 bytes (``docs/handbook/building.rst``,
"Sizes and budgets"). On 2026-10-11 that left 743,747 bytes for everything
still to come, and getting there took a C library shared and cut to what
the programs use (ADR 0008) and leaving iproute2 out (ADR 0017).

The stick needs three things that general-purpose programs provide:

* a PPPoE client (RFC 2516) with PAP or CHAP, IPCP and IPv6CP, for up to
  seven sessions at once, whose sessions the switch can offload;
* a DHCPv6 client that asks for a delegated prefix (RFC 8415) and, on the
  link to the router, a server for that one prefix with router
  advertisements (RFC 4861), so that the router behind the stick receives
  the session's IPv6;
* a clock: the stick has none that runs while it is off, and the time may
  come from the host by NTP or PTP, or from the OLT's time of day.

Options considered
==================

The usual programs
    pppd with its PPPoE plugin, a DHCPv6 client with a separate relay or
    server, an NTP daemon and a PTP daemon. Each brings much that the stick
    does not use: other link types, plugins, scripting hooks, options, and
    the libraries they link.
Forks of the usual programs, cut until they fit
    A fork keeps the structure, the configuration and the
    dependencies of the whole program around the part that is left, and
    every upstream fix has to be carried over by hand into code that no
    longer looks like upstream's. No general-purpose program reads the
    OLT's time of day either.
Programs of our own, each for its case
    Chosen.

Decision
========

The stick has three programs of its own, written for this system from the
RFCs and from IEEE 1588, each doing only its case: ``pppoed``, ``dhcp6d``
and ``timed``. Each has its own directory, README and tests that run on the
host, and procd and netifd start it like any other program of the image.

Consequences
============

* They share the cut C library, and every build checks that the image still
  fits its partition (``tools/image-budget``).
* They cover the cases of this stick and no more. A line or a server that
  needs what they leave out, such as another authentication method, another
  kind of address assignment or a protocol option an operator insists on,
  needs a change to them; a report from such a line is the way to find it.
* The project maintains them. Each stands on its own behind its command line
  and its use by netifd and procd, so any of them can later be replaced, by
  a cut general-purpose program or something else, without touching the
  others.
