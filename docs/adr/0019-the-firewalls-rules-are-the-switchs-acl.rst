.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=======================================================================
ADR 0019: Put firewall rules in the switch's ACL, defaults in netfilter
=======================================================================

:Status: Accepted
:Date: 2026-10-19
:Deciders: the maintainer

Summary
=======

The rules a person writes on the firewall's page go into the switch chip,
which checks every frame as it arrives and costs the CPU nothing. What
happens where no rule decides (nothing comes in from the internet, the
networks may go out, they do not reach each other) stays with Linux, the
one part that tracks connections, so that answers come back. A rule that
lets something through opens Linux's default for it too.

Context
=======

The stick routes up to eight local networks out through its connections
(stages 1 to 3 of the router: networks, DHCP, NAT, port forwards and a
DMZ). The panel has a firewall page: up to 96 rules in order, the first
match deciding, each allowing or blocking traffic from somewhere to
somewhere else (the internet or a part of it, every network, one network,
or one device of a network) by protocol, ports and address family. It also
sets what happens where no rule decides: from the internet in, blocked
except for NAT's forwards and the DMZ; from the networks out to the
internet, allowed unless the person blocks it; between the networks,
blocked unless the person allows it.

Two parts of the stick can judge traffic:

netfilter
    Its connection tracking is the only part that knows a connection: it
    lets the answers of an allowed connection back and drops new
    connections that a default refuses. Its flowtable hands each flow to the
    switch once it is set up, so netfilter sees a flow's first packets, not
    its traffic, and a rule there costs the CPU each new connection.
The switch's ACL
    96 ternary entries, judged for every frame as it enters a port, before
    the CPU or netfilter sees it, at no cost to either
    (``linux/Documentation/networking/dsa/rtl9602c-acl-qos.rst``, "The ACL"
    and "What the driver does with tc flower"). The driver makes one entry
    for each ``tc`` flower rule whose action is ``drop``, ``trap`` or
    ``pass``, in the order of the rules' priorities. It keeps no state: it
    sees a frame, not a connection.

Decision
========

* **The firewall's rules live in the switch's ACL only.** Each rule in force
  becomes ``tc`` flower rules on ingress, ``drop`` for a block and ``pass``
  for an allow, in the rules' order. The compiler from the settings to those
  rules is ``onu-fw acl``, described in ``onu-utils/src/fwacl.h`` and
  ``onu-utils/README.rst``.
* **The defaults stay stateful, in netfilter** (``onu-fw``'s table,
  ADR 0011). A new connection is dropped from the internet into a network,
  except what a forward or the DMZ takes; from a network to the internet
  when ``FW_OUT`` is ``block``; and between two networks when
  ``FW_BETWEEN`` is ``block``, its default. The ways are told apart by the
  interfaces, and the defaults hold for every address family.
* **A rule that allows opens netfilter too**, where the default for its way
  would drop the new connection: an ACL ``pass`` is useless if netfilter
  drops the connection after it. Each such rule is a rule in netfilter's
  chain for its way (``allow_in``, ``allow_out``, ``allow_between``), to
  which a new connection that way jumps, and which drops what none of its
  rules takes. **A rule that blocks is never written to netfilter**: the ACL
  drops its frames before netfilter sees them.
* netfilter's part of a rule is **IPv4 only** until the stick routes IPv6 to
  the networks' devices (stage 5). An IPv6 rule is kept in the settings and
  shown, and has no effect until then.

The settings (``onu-utils/src/settings.h``) are ``RULEn``, where n is the
place in the order, with the value
``"ID_ACTION_FROM_TO_PROTO_PORTS_FAMILY_ON"``, ``RULEn_NAME``, ``FW_OUT`` and
``FW_BETWEEN``.

How the compiler places the rules
=================================

``onu-fw acl`` puts every rule in force into the ACL at priorities 400 to
499 on sfp and pon (``omcid/target/tcprio.h``) whenever the settings or a
connection's address change, and writes what became of each rule to
``/run/onu-fw.acl``. Its choices:

* **"The internet" means no local address.** On the host link the rules to
  a network or a device come first, then a pass for each network's
  addresses, for 255.255.255.255 and for multicast UDP, then the rules to
  the internet, which then need to name no destination. A rule of one kind
  and a rule of the other never match the same frame, so the order the
  person gave holds. IGMP is not passed, as a pass would stop the driver's
  own rule from copying it to the bridge's snooping.
* **A rule that allows also passes the answers**, from its ports, placed
  with it, since a later blocking rule would drop them otherwise; the far
  end may then start from those ports as well. Between the networks a rule
  without ports passes all of its protocol back, which undoes a later
  blocking rule in the other direction in the ACL; ``FW_BETWEEN=block``
  keeps that direction shut in netfilter, which follows the connection. To
  the internet, only a rule that names ports has its answers passed, on the
  PON's port, because passing every frame to the connection's address would
  undo every later rule from the internet.
* **Rules from the internet go through NAT.** A rule from the internet to a
  device or a network becomes the external ports of the forwards that reach
  it, its own ports mapped through each forward, and, for a DMZ that reaches
  it, the ports no forward of the connection takes plus an echo request,
  each at the connection's own address, which keeps out what the OLT
  bridges for other services. A rule through a DMZ that names no port is
  left out there: the DMZ also takes the answers to the networks' own
  traffic, which the ACL cannot tell from a new connection.
* **A session's entries match inside it**, by its address and not by its
  number, which, with a part of the internet named, would need a ninth
  slot.
* **The rules go in, in order, as far as the free entries hold them whole**,
  after the driver's own four and those of the other writers; the rules
  after the first that does not fit stay out too, as they may depend on it.
  The old set goes before the new one comes, a gap of milliseconds.

Consequences
============

* The rules cost the CPU nothing per frame, and netfilter's chains cost only
  the first packet of a connection. The defaults keep answers coming back,
  which a stateless ACL alone could not do without opening every port in
  the other direction.
* Between the networks the default is to block: networks do not reach each
  other through the stick unless a rule or ``FW_BETWEEN=allow`` lets them.
* Known limits, which were weighed when deciding:

  - **An IPv6 rule to a device cannot be made in the ACL**: the driver
    refuses IPv6 addresses, and a frame from the internet carries the
    stick's MAC address, not the device's.
  - **A rule to "the internet" costs extra entries**: the ACL judges a frame
    as it enters, with no route, so "the internet" is "no local network",
    which takes entries that pass the networks' own addresses ahead of the
    rule.
  - **Fewer than 96 rules in practice**: four of the 96 entries are the
    driver's own (IGMP and MLD for snooping), a rule for both TCP and UDP
    takes two, and a rule to the internet more.
  - **The ACL's IPv4 fields do not reach into a PPPoE session's frames**
    (measured 2026-10-19). The driver compares the IPv4 that a session
    carries through field selectors instead: a flower rule on
    ``protocol ppp_ses`` with ``ppp_proto ip`` names its addresses, protocol
    and ports. So a rule on what comes in from the internet through a session
    is judged there too, as are the frames of the networks on the host link,
    tagged or not, and those of an IPoE connection's GEM port.
  - **What comes in from the internet carries the stick's address** until
    netfilter translates it for a forward or the DMZ, which happens after
    the ACL judged the frame. The compiler meets this through NAT, as above.

* ``docs/handbook/system.rst``, "The firewall, as measured", has the trial
  of 2026-10-19 on the stick and what it has not covered yet.
