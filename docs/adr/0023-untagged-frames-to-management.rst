.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============================================================
ADR 0023: Trap the host link's untagged frames to management
============================================================

:Status: Accepted
:Date: 2026-11-17
:Deciders: the maintainer

Summary
=======

The stick's untagged traffic on the host link can go to its management, so
that a stick just installed from the shipped firmware's page answers at the
address that page had, without a VLAN set on the router. The switch then
traps the host link's frames without a tag to the CPU alone, so that none of
them reaches the PON, and in software they are given the management VLAN on
the way in, whose frames leave the host link without their tag. The data
path stays as the OLT provisions it. When the OLT gives the subscriber's
untagged frames a service of its own while they go to management, the two
claim the same frames: the stick says so, as it says of two services that
would take one VLAN towards the host, and the person decides.

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
* **The switch's trap, and the tag in software.** An ACL rule on ``sfp``'s
  ingress traps the frames whose C-tag field reads 0 to the CPU alone; the
  data path is untouched, and the switch's tables hold nothing more. Taken.

Decision
========

* **Untagged frames from the router go to management, on a new install.**
  On a stick whose first start follows an install from the shipped
  firmware's page, ``onu-config`` stores ``MGMT_UNTAGGED=1``; a stick updated
  from an earlier build of this system keeps its store, without the key, and
  management on VLAN 4094 alone.
* **The switch traps them, the software tags them.** With
  ``MGMT_UNTAGGED=1`` ``onu-mgmt`` puts four rules on ``sfp``: in software,
  on the ingress, the management VLAN's tag pushed onto a frame without a
  tag and set as the VID of a priority tag, and, on the egress, the
  management VLAN's tag taken away; then, in the switch, the trap of the
  frames without a tag or with a priority tag to the CPU alone. The software's
  rules go in before the trap and out after it, so that no trapped frame
  reaches the bridge untagged. Management then answers untagged alone: the
  frames of its VLAN leave ``sfp`` without the tag.
* **The data path stays as the OLT provisions it.** omcid builds the bridge
  as before; while the trap is on, the subscriber's untagged frames reach
  none of its ports.
* **A claim of the untagged frames by the OLT is a clash.** When the OLT
  gives the subscriber's untagged frames a service while they go to
  management, the service gets none of them, and omcid reports it beside the
  services it moved for other clashes. The panel shows it with the two ways
  out: a rule of the services that moves the OLT's service to a tagged VLAN
  towards the host, which leaves untagged frames to management, or
  management on its VLAN alone, which gives them to the service. The
  stick's own uses of the host link without a tag, a bench's
  ``DATA_ADDRESS``, the link a session hands its address down on and a local
  network without a VLAN, get no untagged frames either while management has
  them, which the panel says too.
* **A person changes it on the panel, on trial**, as a change of the
  management address is; a new management pair in another VLAN, confirmed
  through that VLAN, keeps it tagged, and the store then holds
  ``MGMT_UNTAGGED=0``.

Consequences
============

* While management has the untagged frames, every frame the router sends
  without a tag reaches the CPU, within the CPU port's limit of frames a
  second, as the management VLAN's do.
* The steps, each its own change and test: the trap and the tags in
  software; the clash of an untagged service, and of the stick's own
  untagged uses, with management, in omcid, pond and the panel; the change
  on the panel; the management gateway of issue 1.
