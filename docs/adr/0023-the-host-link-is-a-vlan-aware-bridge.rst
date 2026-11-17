.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================================================
ADR 0023: Make the host link a port of a bridge that always filters VLANs
=========================================================================

:Status: Accepted
:Date: 2026-11-17
:Deciders: the maintainer

Summary
=======

The stick's untagged traffic on the host link can go to its management, so
that a stick just installed from the shipped firmware's page answers at the
address that page had, without a VLAN set on the router. For that the host
link, ``sfp``, is a port of one bridge that always filters VLANs and that
exists from the start: the switch then gives a frame without a tag the VLAN
that the port's untagged frames are given, and forwards it only to that
VLAN's members. The stick's own interfaces on the host link, its management
and those of its sessions and local networks, become VLANs of that bridge,
no longer VLAN devices on ``sfp``. What the OLT provisions through OMCI goes
into the same bridge, as now. When the OLT gives the subscriber's untagged
frames a service of its own while they go to management, the two claim the
same frames: the stick says so, as it says of two services that would take
one VLAN towards the host, and the person decides.

Context
=======

The stick is managed on VLAN 4094 of the host link (``onu-mgmt``), a VLAN
device on ``sfp``, which the switch keeps from the PON by two rules: frames
of the VLAN coming in on ``sfp`` go to the CPU alone, and those coming in
from the PON are dropped. A stick installed from the shipped firmware's page
answers there at the address the page had, but only once the router has
moved that address onto VLAN 4094, within the five minutes the trial of the
new system waits for a sign-in. GitHub issue 2 asks for management without
a VLAN.

What the OLT provisions through OMCI (G.988) decides what the subscriber's
untagged frames are for. The bridge (ME 45) has ports (ME 47): GEM ports on
the network's side, alone or behind an 802.1p mapper (ME 130), and the
Ethernet UNI (ME 11) or the VEIP (ME 329) on the subscriber's, which on this
stick are both the host link. A bridge port's VLAN filter (ME 84) lists VIDs
and says what becomes of tagged frames and of untagged ones; a port without
one passes everything. The UNI's VLAN rules (ME 171) take a frame by its
outer and inner tags, a priority of 15 standing for no tag and 14 for a
default rule, and remove up to two tags or discard the frame, then add up to
two; the other way they are undone. An operator's service on untagged
frames is a rule of ME 171 that takes them and does not discard them, such
as one that adds the tag of VLAN 1501, or, without such a rule, bridge ports
that pass untagged frames.

omcid makes that bridge, ``omcibr0``, when the OLT has provisioned one, and
turns its VLAN filtering on only when a port's filter lists VIDs; without a
list the switch forwards every frame. It takes the bridge away while the
stick's own sessions hold VLANs that a bridge filtering none would carry, and
leaves it out while the stick routes the UNI. The rules of ME 171 become
rules of the switch's classifier, which adds, removes and translates tags on
the way to and from the GEM ports. A person's rules of the services
(``SVCn``) move a service, known by its VLAN on the fibre, to another VLAN
towards the host, and omcid moves a service whose VLAN towards the host
another service takes already.

Measured on ONT-UART on 2026-11-17, with the lab's OLT, which provisions no
VLAN filter (M):

* frames without a tag from the router went out on the PON (12 ARP and
  neighbour discovery frames on GEM 257), as nothing filters them;
* a rule of the classifier that adds a tag to untagged frames is the upstream
  rule of a GEM flow, not the VLAN a port gives its untagged frames: the
  frames still went to the PON, and the management interface got none; and
  the switch's ACL cannot tell an untagged frame from a tagged one;
* with the bridge's VLAN filtering on, ``sfp`` given VLAN 4000 for its
  untagged frames, sending it untagged, and the bridge itself a member of the
  VLAN, an interface on the bridge's VLAN 4000 got the router's untagged ARP
  requests, and its answers left ``sfp`` untagged: the router resolved the
  address, and nothing went to the PON. With ``sfp`` and GEM 257 given VLAN 1
  for their untagged frames instead, the router's untagged frames went to the
  PON and the interface got none;
* the kernel refuses a VLAN of the bridge on ``sfp`` whose VID a VLAN device
  on ``sfp`` has already ("Port already has a VLAN upper with this VID"): the
  two ways of putting the CPU on a VLAN of the host link cannot be mixed.

The switch has one bit for VLAN awareness, for all of its ports
(``rtl9602c_port_vlan_filtering()``); only with it set does it forward by the
members of a VLAN, and only then does the VLAN of a port's untagged frames
keep anything from the PON.

Decision
========

* **One bridge, from the start, filtering VLANs.** The bridge exists before
  the OLT provisions anything and stays while the stick runs, with ``sfp``
  as a port and VLAN filtering always on. omcid puts the GEM ports into it
  and gives them their VLANs as it does now; a port's missing VLAN filter
  becomes every VLAN but the ones the stick keeps for itself, and the
  untagged frames of the subscriber the VLAN the untagged frames of the host
  link are given. The bridge is no longer taken away; what it should not
  carry, it is not given the VLANs of.
* **The stick's own interfaces are VLANs of the bridge.** Management and the
  interfaces of the sessions and local networks on the host link move from
  VLAN devices on ``sfp`` to VLANs of the bridge, of which the bridge itself
  is a member. A management VLAN is a member of the bridge and of ``sfp``
  alone, never of a GEM port, so the switch never sends it to the PON.
* **The host link's untagged frames are one VLAN of the bridge.** What the
  stick used ``sfp`` itself for, untagged, uses the bridge itself instead:
  the address of a bench (``DATA_ADDRESS``), the link a session hands its
  address down on, a local network without a VLAN, and now management. The
  untagged frames of ``sfp`` are given one VLAN of the bridge's own, which
  the bridge is a member of while the stick uses them, and the GEM ports while
  the OLT gives them a service. Both at once is the clash below; the stick's
  own use goes first, as omcid's bridge stood aside for the sessions before
  (``g_held_unbridged``, router mode).
* **Untagged frames from the router go to management, on a new install.**
  The host link's untagged frames are given the management VLAN on a stick
  whose first start follows an install from the shipped firmware's page, and
  management answers untagged; a stick updated from an earlier build of this
  system keeps them where they went before, and management on VLAN 4094.
  A person changes it on the panel, on trial, as a change of the management
  address is: the panel then answers through the new way, from which it is
  confirmed, or the old one comes back.
* **A claim of the untagged frames by the OLT is a clash.** When the OLT
  gives the subscriber's untagged frames a service while they go to
  management, the service gets none of them, and omcid reports it beside the
  services it moved for other clashes. The panel shows it with the two ways
  out: a rule of the services that moves the OLT's service to a tagged VLAN
  towards the host, which leaves untagged frames to management, or
  management on its VLAN alone, which gives them to the service.

Consequences
============

* The data path changes where omcid builds it: the bridge that it made and
  took away becomes one it only fills, and every change is measured on the
  lab's OLT with VLAN filters and without, with an untagged service and
  without, before anything reaches a stick on an operator's line.
* netifd, onu-mgmt and onu-apply put the stick's interfaces on the bridge's
  VLANs; the guard of the stick's own programs, which answers on the
  management interfaces alone, follows them.
* A VLAN filter missing on an OLT's bridge port means every VLAN but the
  stick's own, written to the switch's table: some 4,000 entries.
* The steps, each its own change and test: the bridge and management on it;
  the interfaces of the sessions and local networks; the clash of an
  untagged service with management, in omcid, pond and the panel; the
  management gateway of issue 1.
