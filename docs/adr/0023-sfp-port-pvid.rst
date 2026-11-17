.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========================================================
ADR 0023: Give the SFP port a PVID, as on an 802.1Q switch
==========================================================

:Status: Accepted
:Date: 2026-11-17
:Deciders: the maintainer

Summary
=======

The stick behaves towards the router as a port of an 802.1Q switch: the SFP
port has a PVID, the VLAN the router's untagged frames get, whose frames go
back to the router untagged, and the PON side's PVID is 1, the operator's
untagged traffic. A stick installed from the shipped firmware's page starts
with the PVID of its management VLAN, so that its panel answers untagged at
the address that page had, without a VLAN set on the router; one updated from
an earlier build keeps 1, and the untagged frames go to the operator as they
did. A PVID of a service's VLAN towards the router gives that service the
untagged frames. The panel shows the stick's ports and VLANs as a switch's
page does, and a change of the PVID runs on trial.

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

omcid makes that bridge, ``omcibr0``, with ``sfp`` and the GEM ports, and
turns its VLAN filtering on only when a port's filter lists VIDs; without a
list the switch forwards every frame. The rules of ME 171 become rules of
the switch's classifier, which adds, removes and translates tags on the way
to and from the GEM ports.

Measured on the board on 2026-11-17, with the lab's OLT, which provisions no
VLAN filter (M):

* frames without a tag from the router went out on the PON (12 ARP and
  neighbour discovery frames on GEM 257), as nothing filters them;
* a rule of the classifier that adds a tag to untagged frames is the upstream
  rule of a GEM flow: the frames still went to the PON, and the management
  interface got none;
* with the bridge's VLAN filtering on, ``sfp`` given a VLAN for its untagged
  frames and the bridge itself a member of it, an interface on that VLAN of
  the bridge got the router's untagged frames and nothing went to the PON;
* the switch has one bit for VLAN awareness, for all of its ports
  (``rtl9602c_port_vlan_filtering()``).

The C-tag field of the switch's ACL templates reads 0 for a frame without a
C-tag, as for one with a priority tag (VID 0), which IEEE 802.1Q-2022 clause
6.9.1 classifies as untagged. Linux runs the filters of an interface's
ingress before it hands a tagged frame to the interface's VLAN device and an
untagged one to the bridge the interface is a port of. Measured on the board
on 2026-11-17 with the rules of the decision below in place (M): 300 frames
without a tag from the router reached the management interface and none the
PON; 300 frames tagged with VLAN 35 went to the PON; from a router port
without a VLAN, management answered ping, ssh and the panel over HTTPS, and
no longer answered on VLAN 4094; the kernel's slab stayed at 2.9 MB.

Options considered
==================

* **A bridge that always filters VLANs.** The host link a port of one bridge
  with VLAN filtering always on, the management VLAN its own, the untagged
  frames given that VLAN, and a port of an OLT's bridge without a filter
  every VLAN but the stick's own. It isolates the untagged frames in the
  switch, but the kernel keeps an entry, with its multicast context, for each
  VLAN of each port: with 4093 VLANs on ``sfp`` and a GEM port, its slab held
  some 10.6 MB of the 24 MB, and the out-of-memory killer took omcid,
  onu-apply, pond and netifd within 90 s of the start (the board, RAM image,
  2026-11-17). It would also have changed the data path the OLT provisions,
  and moved the stick's other VLAN devices on ``sfp`` onto the bridge, as the
  kernel refuses a bridge VLAN on a port whose VLAN device has its VID. Not
  taken.
* **A flag of management's: untagged frames to management or not.** It
  covers the one case, and a person who knows switches finds no PVID to set
  and no way to give the untagged frames to a service. Not taken.
* **A PVID, by the switch's trap and the classifier's rules.** An ACL rule on
  ``sfp``'s ingress traps the frames whose C-tag field reads 0 to the CPU
  alone for a VLAN of the stick's own; a rule of the classifier gives them a
  service's VLAN on the fibre; the switch's tables hold nothing more. Taken.

Decision
========

* **The SFP port has a PVID, the PON side 1.** The router's untagged frames,
  and those with a priority tag, get the SFP port's PVID; the frames of that
  VLAN leave the SFP port untagged. 1 is the PON side's: with the PVID 1 the
  untagged frames go to the OLT as they are, VLAN 1 untagged on both sides.
  ``onu-pvid`` owns the PVID (``/run/onu-pvid``, ``SFP_PVID`` of the store).
* **A new install starts with the management VLAN's.** On a stick whose
  first start follows an install from the shipped firmware's page,
  ``onu-config`` stores ``SFP_PVID=4094``; a stick updated from an earlier
  build of this system keeps its store, without the key, and the PVID 1.
* **A VLAN of the stick's own: the switch traps, the software tags.** When
  the PVID is the management VLAN or a VLAN device on ``sfp``, the switch
  traps the untagged and priority-tagged frames to the CPU alone, and in
  software they are given that VLAN's tag on ``sfp``'s ingress, which its
  frames lose on ``sfp``'s egress; the software's rules go in before the trap
  and out after it. That VLAN then answers untagged alone.
* **A service's VLAN: a rule of the classifier.** While the PVID is not 1,
  omcid gives a service the OLT set up on untagged frames a host VLAN of its
  own, the fibre's when free, as it does an IP host's; a PVID that is a
  service's host VLAN puts a rule ahead of the others that gives untagged
  frames the service's VLAN on the fibre and takes it away on the way down,
  the service's translation then working upstream alone. A PVID that leads
  nowhere drops the untagged frames, as a switch does.
* **The data path stays as the OLT provisions it** but for these rules.
* **A change runs on trial**, ten minutes, kept from the panel or taken back
  by itself, as the lane's mode is. A new management pair in another VLAN,
  confirmed through that VLAN, sets a PVID of the old VLAN back to 1.
* **The panel shows the stick as a switch.** The SFP page lists the ports,
  SFP and PON, with their PVIDs, and the VLANs with their members, untagged,
  tagged or not, and what each VLAN is: management, the operator's services,
  the service the OLT set up untagged, VLAN 1. It says when the panel would
  answer tagged alone, and when the OLT's untagged service or its bridge
  would want the untagged frames while the PVID leads them elsewhere: omcid
  reports it in ``service_rules``.

Consequences
============

* While the PVID is a VLAN of the stick's own, every frame the router sends
  untagged reaches the CPU, within the CPU port's limit of frames a second, as
  the management VLAN's do.
* The PVID 4094 of a new stick leaves its management VLAN untagged on the
  SFP port: a router that reaches it tagged sets the PVID to 1 first.
* What was measured with a service's VLAN as the PVID is the classifier's
  rule of an OLT that tags untagged frames; the lab's OLT sets up none, so
  it is tested on the build machine alone until an OLT that does is at
  hand.
