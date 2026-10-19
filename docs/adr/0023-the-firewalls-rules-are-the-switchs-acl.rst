.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============================================================================
ADR 0023: The firewall's rules are the switch's ACL; its defaults netfilter's
=============================================================================

:Status: Accepted
:Date: 2026-10-19
:Decided-by: the owner, 2026-10-18; recorded 2026-10-19

.. note::

   In plain words: the rules a person writes on the firewall's page go into
   the switch chip, which checks every frame as it arrives and costs the CPU
   nothing. What happens where no rule decides -- nothing comes in from the
   internet, the networks may go out, they do not reach each other -- stays
   with Linux, the one part that remembers connections, so that answers come
   back. A rule that lets something through opens Linux's default too.

Context
=======

The stick routes up to eight local networks out by its connections (stage
1 to 3 of the router: networks, DHCP, NAT, forwards and DMZ). The panel
has a page of the firewall: up to 96 rules in order, the first that traffic
matches deciding, each allowing or blocking traffic from somewhere to
somewhere else (the internet or a part of it, every network, one, or a
device of one) by protocol, ports and family; and what it does where no
rule decides: from the internet in, blocked but for NAT's forwards and the
DMZ; from the networks out to the internet, allowed unless the person
blocks it; between the networks, blocked unless the person allows it.

Two parts of the stick can judge traffic:

* **netfilter**, whose connection tracking is the only part that knows a
  connection: it lets the answers of an allowed connection back and drops
  the new ones a default refuses. Its flowtable hands each flow to the
  switch once it is set up, so netfilter sees a flow's first packets, not
  its traffic, and a rule there costs the CPU each new connection.
* **the switch's ACL** (``linux/Documentation/networking/dsa/
  rtl9602c-acl-qos.rst``, "The ACL" and "What the driver does with tc
  flower"): 96 ternary entries, judged for every frame as it enters a
  port, before the CPU or netfilter sees it, at no cost to either; the
  driver makes one of each ``tc`` flower rule whose action is ``drop``,
  ``trap`` or ``pass``, in the order of the rules' priorities. It keeps no
  state: it sees a frame, not a connection.

Decision
========

* The firewall's **rules live in the switch's ACL only**: each rule in
  force becomes ``tc`` flower rules on ingress, a block a ``drop`` and an
  allow a ``pass``, in the rules' order. The compiler from the settings to
  those rules is the second part of this stage (part 2).
* The **defaults stay stateful, in netfilter** (``onu-fw``'s table, ADR
  0014): a new connection from the internet into a network is dropped but
  what a forward or the DMZ takes; one from a network to the internet when
  ``FW_OUT`` is ``block``; one between two networks when ``FW_BETWEEN`` is
  ``block``, its default. They are told apart by the interfaces, and hold
  for every family.
* **A rule that allows opens netfilter too**, where the default of its way
  would drop the new connection: an ACL ``pass`` is of no use when
  netfilter drops the connection after it. Each is a rule of netfilter's
  chain for its way (``allow_in``, ``allow_out``, ``allow_between``), which
  a new connection that way jumps to and which drops what none of them
  takes. **A rule that blocks is never written to netfilter**: the ACL drops
  its frames before netfilter sees them.
* netfilter's part of a rule is **IPv4 alone** until the stick routes IPv6
  to the networks' devices (stage 5). A rule of IPv6 is kept in the
  settings and shown, and has no effect until then.

The settings (``onu-utils/src/settings.h``) are ``RULEn``, n the place in
the order, ``"ID_ACTION_FROM_TO_PROTO_PORTS_FAMILY_ON"``, ``RULEn_NAME``,
``FW_OUT`` and ``FW_BETWEEN``.

Consequences
============

* The rules cost the CPU nothing per frame, and netfilter's chains only the
  first packet of a connection; the defaults keep answers coming back, which
  a stateless ACL alone could not do without opening every port back in.
* Known limits, which the owner was told when deciding:

  - **An IPv6 rule to a device cannot be made in the ACL**: the driver
    refuses IPv6 addresses, and a frame from the internet carries the
    stick's MAC address, not the device's.
  - **A rule to "the internet" costs extra entries**: the ACL judges a
    frame as it enters, with no route, so "the internet" is "no local
    network", which takes entries that pass the networks' own addresses
    ahead of the rule.
  - **Fewer than 96 rules in practice**: four of the 96 entries are the
    driver's own (IGMP and MLD for snooping), a rule of both TCP and UDP
    takes two, and a rule to the internet more.
  - The ACL's IPv4 fields do not reach into a PPPoE session's frames
    (measured 2026-10-19, the table's rows for tagged, GEM and PPPoE
    frames): a rule on the addresses of what comes in from the internet by
    a session cannot be judged at the PON's port; frames of the networks on
    the host link, tagged or not, and of an IPoE connection's GEM port are.
  - What comes in from the internet carries the stick's address until
    netfilter translates it for a forward or the DMZ, after the ACL judged
    the frame.

  How part 2 meets the last two is its own to decide.
* Between the networks the default is now to block: networks that reached
  each other through the stick before no longer do, unless a rule or
  ``FW_BETWEEN=allow`` lets them.
* Part 2, the compiler, is still to come: until it is in, a rule that
  blocks has no effect, and a rule that allows works through netfilter
  alone.
