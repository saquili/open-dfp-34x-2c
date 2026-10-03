.. SPDX-License-Identifier: GPL-2.0-only

================================================================
Realtek RTL9602C switch: the L3/NAT engine and flowtable offload
================================================================

Overview
========

Beside the switch core the RTL9602C carries a routing and address-translation
engine: a second table engine that recognises a frame addressed to one of the
router's own interfaces, translates the source address and port of an IPv4 TCP
or UDP flow (network address and port translation, NAPT, RFC 2663), and
forwards the frame to its next hop without the CPU. Replies are translated back
the same way. The engine speeds up translated flows and nothing else: a routed
frame that has no NAPT entry goes to the CPU whatever the route table says, and
IPv6 traffic stays on the software path.

The engine sits between the two sides of the stick. The SFP port (port 0) faces
the subscriber's network and the PON port (port 2) the operator's, and the
driver takes the PON port to be the WAN side: the WAN interface of a router on
this stick is a GEM interface of the PON class, or a PPP interface over one.

The driver brings the engine up at setup, fills its interface, next-hop,
neighbour, route and external-address tables from the kernel's address,
neighbour and route notifiers, and writes one outbound and one inbound NAPT
entry for each flow that netfilter's flowtable offload hands it. A PPPoE WAN
adds a session table and an encapsulating next hop. This page describes the
engine's registers and tables, the order of its bring-up, what each notifier
writes, the flowtable offload, the PPPoE path, the hit bitmaps that keep an
offloaded flow alive, and the debugfs views that show the engine. The
address-table entries that the engine depends on are described from the
engine's side only; the entries themselves are on the *bridging* page.

The other pages of the port are named by short names on this page:

- *main*, Documentation/networking/dsa/rtl9602c.rst: the ports, the DSA
  integration, the order of setup and a code map of the whole driver.
- *bridging*, Documentation/networking/dsa/rtl9602c-bridging.rst: the address
  table, port isolation and flooding, which a routed frame passes through, and
  the address-table entries that the engine depends on.
- *acl-qos*, Documentation/networking/dsa/rtl9602c-acl-qos.rst: the
  classifier, whose stream entries give routed frames their GEM port, and the
  switch's frame capture.
- *devlink*, Documentation/networking/devlink/rtl9602c.rst: the devlink
  parameters and the ``frame_capture`` region as the user sees them.
- *PON class*, Documentation/networking/pon.rst: the GEM interfaces, which pass
  a flowtable down to the switch port below them.
- *CPU port*,
  Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst: the
  CPU port's receive descriptor, which carries the reason a frame was sent up.

The tables and what points to what
----------------------------------

The engine is a set of small tables joined by row indices. For a translated
frame the engine follows the pointers below. The route table decides only
whether the engine accepts the flow, and the neighbour CAM is followed by no
pointer at all (see "How a translated frame goes through the engine")::

    frame ----> L3 interface   (matched by the destination MAC)
                route rows     (an on-link row for the subnet on each side)
                     |
        outbound NAPT   (entry = 4 * bucket + way; holds no key)
                     |  RTL9602C_NAPT_INBOUND_IDX
                     v
        inbound NAPT    (bucket from the reply's address and port)
                     |  RTL9602C_NAPT_IN_EXTADDR_IDX
                     v
        external address   (the source address on the wire)
                     |  RTL9602C_EXTADDR_NEXTHOP_IDX
                     v
        next hop ---- RTL9602C_NEXTHOP_L3IF_IDX ----> L3 interface
             |   \                                    (source MAC, VLAN)
             |    `- RTL9602C_NEXTHOP_PPPOE_IDX ----> PPPoE session
             |  RTL9602C_NEXTHOP_L2_IDX
             v
        address-table entry   (destination MAC, egress port)

How the driver runs the engine
------------------------------

Six things carry the function:

- One condition for everything. ``l34_up``, the engine's enable read back set,
  is what the three notifiers, the flowtable offer and the router bit on the
  address-table entries behind the CPU port wait for (see "Global enable and
  configuration").
- The tables follow the kernel. An IPv4 address on a switch port, on a GEM
  interface or on a VLAN device above one becomes an L3-interface row and an
  external-address row. A resolved ARP neighbour on such a device becomes a
  next-hop row, a neighbour-CAM row and an address-table entry. The main
  routing table is mirrored into the route table, longest prefix first. A PPPoE
  WAN gets its rows from the flow rule that first uses it (see "The notifiers
  that fill the tables" and "The PPPoE encapsulation path").
- A flow is two entries. netfilter's flowtable offers each direction of a flow
  as a flower rule. The driver programs the source-NAT direction as one
  inbound and one outbound NAPT entry, each in the bucket that the engine's
  hash of the flow selects, and declines the rest, which then stays in
  software (see "The flowtable offload").
- The engine keeps one hit bit per outbound entry, which the driver reads so
  that netfilter keeps an offloaded flow alive while it carries traffic (see
  "Hit bitmaps, flow statistics and ageing").
- Setup empties the tables. They keep their contents across a warm restart,
  and the engine's reset strobes do not empty them, so setup writes zeros over
  every row that is not zero (see "Reset strobes, table contents and the clear
  at setup").
- One table port and two locks. The engine has a table port of its own; every
  access goes through ``rtl9602c_nat_tbl_read()`` or
  ``rtl9602c_nat_tbl_write()`` under ``table_lock``, and the bookkeeping above
  it, the rows, routes and flows, is under ``flow_lock`` (see "The table access
  port").

Operations and files
--------------------

The driver implements one operation of ``struct dsa_switch_ops`` for the
engine, registers three kernel notifiers, and calls the engine from setup and
teardown; a second operation, ``port_fdb_add``, carries the router bit of the
address-table entries. Every name is prefixed ``rtl9602c_``.

.. list-table::
   :header-rows: 1
   :widths: 56 44

   * - Entry point
     - Described under
   * - ``nat_clear()`` and ``l34_init()``, called from ``setup()``
     - "Reset strobes, table contents and the clear at setup" and "Bring-up
       sequence".
   * - ``l34_inetaddr_event()``, ``l34_netevent()`` and ``l34_fib_event()``,
       registered by ``l34_notifiers_register()`` and unregistered by
       ``l34_notifiers_unregister()``, which ``teardown()`` calls
     - "The notifiers that fill the tables".
   * - ``l34_fini()``, called from ``teardown()`` after the notifiers
     - "Bring-up sequence" (its end).
   * - ``port_setup_tc``, its ``TC_SETUP_FT`` case (``setup_ft_block()``); the
       tbf and ets cases are on the *acl-qos* page
     - "The flowtable offload".
   * - ``port_fdb_add`` (the router bit), ``l2_l3lookup_cpu()``,
       ``l2_nexthop_place()`` and ``bridge_refresh()`` (the isolation masks)
     - "The address-table entries the engine uses"; the *bridging* page.

The files that hold this page:

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - File
     - What of this page it holds
   * - ``drivers/net/dsa/realtek/rtl9602c-offload.c``
     - The engine: its table port, the clear at setup and the bring-up, the
       entry fields and hashes, the three notifiers, the PPPoE WANs, the
       flowtable offload, the hit bitmaps and the debugfs views ``l3nat``,
       ``l3flow`` and ``l3capture``. The ACL, the classifier, storm control and
       the scheduler of the same file are on the *acl-qos* page.
   * - ``drivers/net/dsa/realtek/rtl9602c-l2.c``
     - The address-table helpers of the engine: ``rtl9602c_l2_l3lookup_cpu()``,
       ``rtl9602c_l2_nexthop_place()``, the router bit that
       ``rtl9602c_port_fdb_add()`` stamps, and the opening of the isolation
       masks in ``rtl9602c_bridge_refresh()``.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - The steps of ``rtl9602c_setup()`` and ``rtl9602c_teardown()`` that call
       the engine, and the initialisation of ``flow_lock``.
   * - ``drivers/net/dsa/realtek/rtl9602c-gem.c``
     - ``rtl9602c_gem_us_ready()``, which says whether a GEM interface can
       carry a routed flow, and the release of the routed flow when its GEM
       port is removed.
   * - ``drivers/net/dsa/realtek/rtl9602c.h``
     - The registers of the engine, the entry fields (``RTL9602C_FIELD()``),
       the shadows in ``struct rtl9602c_priv`` (``l34_*``, ``flow_lock``),
       ``struct rtl9602c_l34_route`` and ``struct rtl9602c_pppoe_wan``.
   * - ``net/dsa/user.c``
     - ``dsa_user_setup_ft_block()``, which offers ``TC_SETUP_FT`` to the
       switch driver before the conduit.
   * - ``drivers/net/pon/pon_gem.c``
     - ``pon_gem_setup_tc()``, which passes ``TC_SETUP_FT`` from a GEM
       interface down to the switch port below it.

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Symbol
     - Effect
   * - ``CONFIG_NET_DSA_RTL9602C``
     - Builds everything on this page, as described on the *main* page: the
       engine and its views are in ``rtl9602c-offload.o``, the address-table
       helpers in ``rtl9602c-l2.o``.
   * - ``CONFIG_NF_FLOW_TABLE``, ``CONFIG_NF_FLOW_TABLE_INET``,
       ``CONFIG_NFT_FLOW_OFFLOAD``
     - netfilter's flowtable, and the nftables statement and flag (``flow
       add``, ``flags offload``) that offer a flow to the driver. Without them
       nothing reaches the driver.
   * - ``CONFIG_NF_CONNTRACK``, ``CONFIG_NF_TABLES``, ``CONFIG_NFT_NAT``,
       ``CONFIG_NFT_MASQ``
     - Source NAT, which the engine carries: only a flow with a source
       translation is programmed (see "The flowtable offload").
   * - ``CONFIG_PON``
     - The GEM interfaces, the WAN side of a stick that routes over the PON.
       ``CONFIG_NET_DSA_RTL9602C`` selects it.
   * - ``CONFIG_PPP``, ``CONFIG_PPPOE``
     - A PPPoE WAN.
   * - ``CONFIG_VLAN_8021Q``
     - VLAN devices above a switch port or a GEM interface; the VLAN ID goes
       into the L3-interface row.
   * - ``CONFIG_DEBUG_FS``
     - The views ``l3nat``, ``l3flow`` and ``l3capture``.

``rtl9602c_defconfig`` sets all of them except ``CONFIG_PON``, which the driver
selects.

Device tree
-----------

The engine's registers are inside the register window that the ``switch`` node
maps (``reg = <0x1b000000 0xf10000>``, see the *main* page): its own registers
are the part of it from offset ``0x800000`` to ``0x800fff``, physical
``0x1b800000``. No property of the node, and no other node, configures the
engine. The compatible is ``realtek,rtl9602c-switch``, as described on the
*main* page.

Interfaces to user space
------------------------

- ``nft`` with a flowtable that has ``flags offload`` and names ``sfp`` and the
  GEM interface of the WAN, together with a source-NAT rule for that WAN. A
  flowtable that names the bare ``pon`` never offloads a flow (see the *PON
  class* page)::

      table inet x {
              flowtable f {
                      hook ingress priority 0; devices = { sfp, gem0 };
                      flags offload;
              }
              chain forward {
                      type filter hook forward priority 0; policy accept;
                      ip protocol { tcp, udp } flow add @f
              }
      }

  The connection tracking table shows an offloaded flow with ``[HW_OFFLOAD]``,
  and a flow that the driver declined and the software fast path carries with
  ``[OFFLOAD]`` (Documentation/networking/nf_flowtable.rst).
- ``ip address``, ``ip route`` and ``ip neigh``: the engine's tables follow
  them through the notifiers. A VLAN device above ``sfp`` or a GEM interface
  gets an L3-interface row with its VLAN ID, and a flow that leaves by the ppp
  device of a PPPoE client on a GEM interface gets a PPPoE WAN.
- Three debugfs views in ``/sys/kernel/debug/rtl9602c/``: ``l3nat`` and
  ``l3flow`` (mode 0400), which show the engine's registers and tables, and
  ``l3capture`` (mode 0600), which reads and arms the engine's per-frame
  capture (see "The debugfs views" and "The per-frame capture").
- The kernel log lines that begin ``rtl9602c-switch:``. Setup logs ``L3:
  binding actions``, ``L3: auxiliary control`` and ``L3: reset 13 of 13 table
  engine(s), engine ON`` with the control and reset words, and, when it had to
  empty rows, ``cleared N L3/NAT table entries left by the previous kernel``.
  A row write that failed, a route table that cannot be mirrored, an interface
  that finds no free L3-interface row and a second routed GEM port are logged
  as warnings that begin ``<l3>``, and a table access that does not complete as
  ``L3/NAT table <t> entry <i>: engine still busy after 1 ms``.
- ``echo 'file rtl9602c-offload.c +p' >
  /sys/kernel/debug/dynamic_debug/control`` enables the debug lines of the file,
  those of its ACL code included. Those that begin ``<flow>`` follow the
  flowtable offload and the PPPoE WANs, and those that begin ``<l3>`` the
  notifiers and the route table.
- ``ethtool -S sfp`` and ``ethtool -S pon``: ``rx_forward_discards`` counts the
  frames that the switch dropped in forwarding, which is how several of the
  measurements below saw a routed frame discarded (see the *bridging* page).

How to read the fact tables
===========================

Each block below has a short account of what the hardware does and what the
driver does with it, followed by fact tables with three columns: *Fact*, *Tag*
and *Checked by*. The tag says how the fact is established. **M**, measured on
the board (the DFP-34X-2C3): *Checked by* gives the command or observation,
through the driver's own interfaces (the kernel log, the debugfs views,
``ethtool``, the connection tracking table, devlink), or the access made by
hand, that shows it. **T**, tested by function: the driver depends on the fact,
and the named behaviour fails if it is wrong. **P**, public standard:
*Checked by* names the document and the clause. **I**, inferred, not verified:
the fact is an assumption, and *Checked by* says what would confirm or refute
it. The last chapter collects these facts and says which of them the driver
depends on.

Register offsets are from the base of the switch's register window, physical
``0x1b000000``; the engine's own registers are the part of it from ``0x800000``.
Values are 32-bit words as ``readl()`` and ``writel()`` see them on this
big-endian CPU. Some facts were measured with register reads and writes made by
hand, outside the driver, and the *Checked by* column says so.

An *entry* is one row of a table behind the engine's table port. Entry bit n is
bit n mod 32 of data word n/32, so word 0 holds entry bits 31:0, and a field is
given as its high and low entry bits, high first. Addresses and ports inside
entries are in host order: the first octet of an address is in its highest
bits. The *reset state* of a register is what a read shows after a full chip
reset, with the boot loader run and this driver not loaded.

The *forwarding reason* is the number that the switch's frame capture reports
for a frame (the devlink region ``frame_capture``, armed with the ``pktdbg_*``
parameters; see the *acl-qos* page). The *CPU reason* is the number that the
receive descriptor of the CPU port carries (``rxreason`` in the debugfs
directory ``rtl9602c-eth``; see the *CPU port* page). The engine has a reason of
its own in its output descriptor (see "The per-frame capture"). Port numbers are
the hardware's (0 ``sfp``, 1 not wired, 2 ``pon``, 3 the CPU port). Addresses in
the examples are from the documentation ranges ``192.0.2.0/24``,
``198.51.100.0/24`` and ``203.0.113.0/24``, and ``00:00:5E:00:53:xx``.

The behaviours that a tested fact depends on are named in words. A *routing
test* is traffic through the board with NAT: a host behind the SFP port sends
UDP to a remote end beyond the PON, over a flowtable with ``flags offload``. It
passes when the translated source address is seen on the wire at the far side
of an OLT that bridges transparently, the connection tracking table shows
``[HW_OFFLOAD]`` for the flow, and the CPU port counts only the first frames of
it. A *PPPoE test* is the same over a PPPoE WAN whose ppp device runs on a GEM
interface. It passes when the concentrator receives PPPoE frames with the
session ID that the driver wrote and UDP checksums that verify. A *throughput
test* sends a flow through the engine at the rate of the test setup, NAT and NAT
over PPPoE, each way, and reads the CPU port's counter beside the rate at the
far side.

The engine's registers
======================

The engine's own registers are the 4 KiB of the switch's window from
``0x800000``. Three registers that the bring-up writes lie outside it, in
blocks of the switch: the engine's master enable in the switch's lookup block,
an auxiliary word, and the control of the per-L3-interface counters. The block
needs nothing opened before it answers, unlike the PON MAC behind its enable in
the system controller (see the *main* page), so the driver reads and writes it
from setup on.

.. list-table::
   :header-rows: 1
   :widths: 40 24 36

   * - Register
     - Offset
     - What it is
   * - ``RTL9602C_L3_GLOBAL_CTRL``
     - ``0x01106c``
     - The master enable (outside the engine's window).
   * - ``RTL9602C_L3_AUX_CFG``
     - ``0x02a104``
     - An auxiliary word (outside the engine's window).
   * - ``RTL9602C_MIB_L3IF_CTRL``
     - ``0x03401c``
     - The control of the per-L3-interface counters (outside the engine's
       window).
   * - ``RTL9602C_L3_CFG``
     - ``0x800010``
     - The configuration word.
   * - ``RTL9602C_PPPOE_HIT``
     - ``0x800014``
     - The PPPoE hit bitmap, one word.
   * - ``RTL9602C_NEIGH6_HIT(i)``
     - ``0x800018`` + 4i
     - The hit bitmap named for the IPv6 neighbours, 4 words.
   * - ``RTL9602C_L3_BIND_CFG``
     - ``0x80002c``
     - The actions for a WAN binding that disagrees with the lookup.
   * - ``RTL9602C_L3_FLOW_HIT(i)``
     - ``0x800030`` + 4i
     - The five-tuple hit bitmap, 8 words.
   * - ``RTL9602C_NEIGH_HIT(i)``
     - ``0x800050`` + 4i
     - The hit bitmap named for the IPv4 neighbours, 4 words.
   * - ``RTL9602C_L3TBL_CTRL``
     - ``0x800100``
     - The table port's command register.
   * - ``RTL9602C_L3TBL_RESET``
     - ``0x800104``
     - The reset strobes, one per table engine.
   * - ``RTL9602C_L3TBL_READ_DATA(i)``
     - ``0x800108`` + 4i
     - The entry after a read, 5 words.
   * - ``RTL9602C_L3TBL_WRITE_DATA(i)``
     - ``0x80011c`` + 4i
     - The entry that the next write commits, 5 words.
   * - ``RTL9602C_L3_CAPTURE_CTRL``
     - ``0x800200``
     - The control of the per-frame capture.
   * - ``RTL9602C_L3_CAPTURE_IN(i)``
     - ``0x800204`` + 4i
     - The capture's input descriptor, 14 words.
   * - ``RTL9602C_L3_CAPTURE_OUT(i)``
     - ``0x800280`` + 4i
     - The capture's output descriptor, 5 words.
   * - ``RTL9602C_NAPT_HIT``
     - ``0x800400``
     - The NAPT hit bitmap, 128 words, to ``0x8005fc``.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The engine's registers lie at ``0x800000`` to ``0x800fff``:
       configuration ``0x800010``, PPPoE hit bitmap ``0x800014``, IPv6-neighbour
       hit bitmap ``0x800018`` to ``0x800024``, binding actions ``0x80002c``,
       five-tuple hit bitmap ``0x800030`` to ``0x80004c``, neighbour hit bitmap
       ``0x800050`` to ``0x80005c``, table port ``0x800100`` to ``0x80012c``,
       frame capture ``0x800200`` to ``0x800293``, NAPT hit bitmap ``0x800400``
       to ``0x8005fc``. Three registers that the bring-up writes lie outside
       it: the global enable at ``0x01106c`` (in the switch's lookup block), an
       auxiliary word at ``0x02a104``, and the per-L3-interface counter control
       at ``0x03401c``.
     - M
     - A read of every register after a full chip reset; the ``l3nat`` and
       ``l3flow`` views read them on every use.
   * - The block needs nothing opened before it answers: all 180 of its words
       read without a hang, the same on two scans, and none returns the pattern
       that an unimplemented register gives. The driver reads and writes it from
       setup on.
     - M
     - Two full scans of the block; at every setup ``rtl9602c_nat_clear()``
       reads every table row (see "Reset strobes, table contents and the clear
       at setup").

Global enable and configuration
===============================

Four words configure the engine and a fifth turns on counters. The master
enable is outside the engine's window. The configuration word has two fields
that the driver changes; the driver changes nothing else in it, because
changing further fields turned a translated flow's verdict into one for the
CPU. The binding-action word and the auxiliary word are written with values
that changed nothing measurable, and whether they are needed is not
established.

The master enable
-----------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_L3_GLOBAL_CTRL`` (``0x01106c``) reads ``0x00000004`` in the
       reset state. Its bit 0, ``RTL9602C_L3_GLOBAL_CTRL_EN``, is the master
       switch of the whole engine. With it clear the engine still parses every
       frame and computes a complete verdict, visible in the output descriptor
       of the per-frame capture, and the switch acts on none of it: the frames
       go on as ordinary layer-2 traffic. With it set the switch acts on the
       verdict.
     - M
     - The one bit changed by hand while a flow ran for 14 s and the CPU port's
       frame counter compared: 270,000 frames reached the CPU with the bit
       clear, 8 with it set. After setup ``l3flow`` prints ``global     00000005
       engine ON``.
   * - The driver sets ``EN`` with a read-modify-write (bit 2, set in the reset
       state, is kept), reads the register back and records the result as
       ``l34_up``. Everything that hands frames or rows to the engine waits for
       ``l34_up``: the router bit on the address-table entries behind the CPU
       port, the three notifiers and the flowtable offer.
     - T
     - A routing test: the engine is on after setup with nothing written to it
       from outside; the boot log line ``L3: reset 13 of 13 table engine(s),
       engine ON (control 00000005, ...)``.

The configuration word
----------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_L3_CFG`` (``0x800010``) reads ``0x64000020`` in the reset
       state: ``RTL9602C_L3_CFG_FLOW4_EN`` (bit 30) and
       ``RTL9602C_L3_CFG_FLOW6_EN`` (bit 29) set, ``RTL9602C_L3_CFG_NAT_MODE``
       (bits 11:10) 0, ``RTL9602C_L3_CFG_L3IF_LOOKUP`` (bits 9:8) 0.
     - M
     - A read after a full chip reset with the driver not loaded.
   * - Setup sets ``RTL9602C_L3_CFG_NAT_MODE`` to
       ``RTL9602C_L3_CFG_NAT_MODE_NAPT`` (3) and ``RTL9602C_L3_CFG_L3IF_LOOKUP``
       to ``RTL9602C_L3_CFG_L3IF_LOOKUP_MAC`` (2) in one read-modify-write,
       before the global enable; the register then reads ``0x64000e20``. With
       both at 0 the engine gave up frames addressed to the router (CPU reason
       5); with both set (changed together) and the tables in place it
       translated them.
     - T
     - A routing test reads both values back after setup and needs them for
       the flow to be carried. By hand: CPU reason 5 with both at 0, and the
       translated verdict in the output descriptor of the per-frame capture with
       the values set. ``l3flow`` prints ``l3 cfg     64000e20``.
   * - With ``L3IF_LOOKUP`` set to 2 the engine matches arriving frames to the
       L3-interface rows that the driver writes, each built around a netdev's
       MAC with an exact mask (see "L3 interface (selector 3)").
     - T
     - A routing test.
   * - That the value 2 selects matching on the frame's destination MAC, rather
       than on another key that those rows also happen to satisfy, is inferred,
       not measured.
     - I
     - A row whose MAC differs from the frames' destination address, other
       fields equal, would show whether the frames still match it.
   * - No value of ``NAT_MODE`` makes the engine forward a routed frame that has
       no NAPT entry: with netfilter's flowtable removed, the CPU port counted
       230,220 frames for a routed flow of 230,217, and the same held at
       ``NAT_MODE`` 0, 1, 2 and 3; the switch's capture gave forwarding reason
       34. The engine accelerates translated flows, not routes.
     - M
     - The four values written by hand in turn; the CPU port's frame counter and
       the switch's frame capture (first-trap mode) read for each.
   * - The driver changes only those two fields. Writing the word with further
       fields changed (``0x84801e10``) turned the engine's verdict for a
       translated flow from forward (output action 2) into to-CPU (3) and its
       reason from 23 to 27.
     - M
     - The whole word written by hand and the output descriptor of the per-frame
       capture read before and after.
   * - ``FLOW4_EN`` and ``FLOW6_EN`` enable lookups in the five-tuple flow
       tables (selector 12 for IPv4). The driver keeps them as it finds them
       (set), never writes a five-tuple entry and zeroes that table at setup;
       translated flows are carried with it empty. The views print both bits.
     - T
     - A routing test with the five-tuple table all zero; ``l3nat`` and
       ``l3flow`` print ``flow4 1  flow6 1``.

The binding, auxiliary and counter words
----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_L3_BIND_CFG`` (``0x80002c``) holds the actions for frames
       whose WAN binding disagrees with the lookup, ten actions in one word; it
       reads 0 in the reset state. Setup writes ``RTL9602C_L3_BIND_CFG_INIT``
       (``0xa2400000``) before the global enable and logs the read-back; the
       value lands. The binding table itself stays empty.
     - M
     - A read in the reset state; the boot log line ``L3: binding actions
       a2400000``.
   * - Whether the binding word is needed is not established: writing it changed
       nothing measurable, and frames arriving on the subscriber port carry "not
       from a WAN port" in the input descriptor of the per-frame capture, so
       binding rules may never apply to them.
     - I
     - Omitting the write and repeating the routing test and the PPPoE test
       would settle it.
   * - ``RTL9602C_L3_AUX_CFG`` (``0x02a104``) is a plain read/write word:
       ``0xf5``, 0, ``0xffffffff`` and ``0xaaaaaaaa`` each read back as written;
       it reads ``0x55555555`` in the reset state. Setup writes
       ``RTL9602C_L3_AUX_CFG_ON`` (``0xf5``) after the global enable and logs
       it; ``l3nat`` prints it.
     - M
     - Four patterns written by hand and read back; a read in the reset state;
       the boot log line ``L3: auxiliary control 000000f5``; the first line of
       ``l3nat``, ``aux ctrl 000000f5``.
   * - What the bits of ``0xf5`` enable, and whether the write is needed, is not
       established: ``0xf5`` and 0 gave the same result in a configuration that
       did not yet forward.
     - I
     - Omitting the write and repeating the routing test and the PPPoE test
       would settle it.
   * - ``RTL9602C_MIB_L3IF_CTRL`` (``0x03401c``) reads 0 in the reset state.
       Setup sets ``RTL9602C_MIB_L3IF_CTRL_EN`` so that the per-L3-interface
       packet and octet counters count; nothing in the driver reads those
       counters. That the bit enables them and affects nothing else is
       inferred, not measured.
     - I
     - Reading the per-interface counters around a flow, with the bit set and
       clear, would confirm it.

The table access port
=====================

Every table of the engine is reached through a table port of its own, apart
from the switch's table engine (see the *bridging* page). A command register
names a table by a selector and an entry by an index and starts a read or a
write; the entry moves through five read-data and five write-data registers.
The port has no status register: an access is done when its command bit clears.
The driver makes every access, that is the command, the wait for its completion
and the transfer of the data, under ``table_lock``, the mutex that also
serialises the switch's own table engine, so that no caller reads the entry
that another one asked for. The bookkeeping above the accesses, the rows,
routes and flows, is under ``flow_lock``.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The engine has its own indirect table port, separate from the switch's
       table engine. ``RTL9602C_L3TBL_CTRL`` (``0x800100``) is its command
       register: ``RTL9602C_L3TBL_CTRL_INDEX`` (bits 15:0) the entry index,
       ``RTL9602C_L3TBL_CTRL_TABLE`` (bits 19:16) the table selector,
       ``RTL9602C_L3TBL_CTRL_WRITE`` (bit 24) and ``RTL9602C_L3TBL_CTRL_READ``
       (bit 25) start an access. It reads 0 in the reset state; after an access
       it keeps the last index and selector.
     - M
     - The port driven by hand and through the driver agreed word for word;
       ``cat /sys/kernel/debug/rtl9602c/l3nat`` reads nine tables through it.
   * - ``READ`` and ``WRITE`` clear themselves when the access completes, and
       the port has no separate status register: completion is polled on the
       command register. A read command ``0x020c0001`` (index 1, selector 12,
       ``READ``) read back ``0x000c0001`` with the entry in the data registers.
       The driver polls every 10 microseconds for at most 1 ms and logs ``L3/NAT
       table <t> entry <i>: engine still busy after 1 ms`` (read) or ``... write
       still busy after 1 ms`` (write) on a timeout.
     - M
     - A read driven by hand; every read of ``l3nat`` completes.
   * - ``RTL9602C_L3TBL_READ_DATA(i)`` (``0x800108`` + 4i) return the entry
       after a read; ``RTL9602C_L3TBL_WRITE_DATA(i)`` (``0x80011c`` + 4i) stage
       the entry that the next write commits; ``RTL9602C_L3TBL_DATA_WORDS`` is 5
       each way.
     - M
     - The next row; the register addresses read in the reset state.
   * - Word order is identity: data register i carries entry bits 32i+31 to
       32i. Four distinct words written to a five-tuple entry came back in the
       same four registers; a second visit to the entry found the restored
       original, so the round trip went through the table and not through the
       port's own registers; a walking one over every bit of nine tables came
       back where it was put every time.
     - M
     - Write, read and restore of two five-tuple entries, twice each, by hand;
       a walking one over nine tables, 693 round trips, no bit seen anywhere
       else.
   * - Reading an entry has no side effect: the same entry read twice, by the
       driver and by hand, returned the same words.
     - M
     - A double read of one five-tuple entry; two ``cat`` of ``l3nat`` in a row
       print the same table words.
   * - A selector that is not a table never completes. Selector 8 left ``READ``
       set, and every later access to any table then timed out as well. Writing
       0 to the command register did not recover the port, setting every reset
       strobe did not, a warm restart did not; only a chip reset did.
     - M
     - One read of selector 8 by hand, then reads of a known entry, the recovery
       attempts and a chip reset.
   * - So the driver checks every access before it issues a command: a selector
       with no entry in its geometry table (8, 14, 15), an index at or beyond
       the table's row count, or a word count different from the table's is
       refused with ``-EINVAL`` and nothing is written to the port. ``l3nat``
       ends with a line that exercises the check on index 9999.
     - T
     - ``cat /sys/kernel/debug/rtl9602c/l3nat``: the last line reads ``guard:
       flow-routing[9999] -> -22 (want -22)``, the tables having printed
       normally before it.
   * - That an index at or beyond a table's last entry wedges the port as an
       unlisted selector does is inferred from the row above, not measured.
     - I
     - One such access on a board that can be power-cycled would settle it; the
       driver never issues one.
   * - Before every write command the driver writes all five write-data
       registers, the table's words and zeros after them, so that no register
       can carry an earlier write's word into the entry. Whether one write
       command commits all five registers regardless of the table's width is
       not established; with the word counts enforced the padding is a
       precaution.
     - I
     - Writing a one-word table after a three-word write without the padding,
       and reading both back, would settle it.

``rtl9602c_nat_tbl_read()`` and ``rtl9602c_nat_tbl_write()``, in order:

1. Check the access against the geometry table (see "Tables, selectors and
   geometry"): the selector must have rows, the index must be below the row
   count and the word count must be the table's. Otherwise return ``-EINVAL``
   and write nothing. Purpose: an access to a selector that is not a table
   wedges the port. (T)
2. Take ``table_lock``. Purpose: the port is one set of registers shared by all
   callers. (T)
3. For a write, load all five ``RTL9602C_L3TBL_WRITE_DATA`` registers, the
   table's words first and zeros after them. Purpose: no register can carry a
   word of an earlier write into the entry. (I)
4. Write ``RTL9602C_L3TBL_CTRL`` with the index, the selector and
   ``RTL9602C_L3TBL_CTRL_READ`` or ``RTL9602C_L3TBL_CTRL_WRITE``. Purpose: this
   starts the access. (T)
5. Poll ``RTL9602C_L3TBL_CTRL`` every 10 microseconds until both command bits
   are clear, for at most 1 ms; on a timeout log the message of the table above
   and return the error. Purpose: the data are valid only after the bit clears.
   (M)
6. For a read, read ``RTL9602C_L3TBL_READ_DATA(0)`` to the last word of the
   table. Purpose: the entry. (M)
7. Release ``table_lock``.

The steps are checked as follows. Steps 1, 2 and 4 by every table access of a
routing test completing and by the last line of ``l3nat``. Step 5 by the reads
of ``l3nat`` completing, and step 6 by the round trips of the table above. Step
3 is the precaution of the last row of that table.

Tables, selectors and geometry
==============================

The engine has thirteen tables. The driver fills eight of them; the other five,
the IPv6 routes, the bindings, the WAN types, the IPv6 neighbours and the
five-tuple flows, it only clears at setup. The selectors are the
``RTL9602C_L3TBL_CTRL_TABLE_*`` macros. *Rows* is what the driver uses and
bounds its accesses by, *words* is the entry's size in data registers, and *top
bit* is the highest entry bit that stores (see the last row of the facts).

.. list-table::
   :header-rows: 1
   :widths: 10 18 36 10 10 16

   * - Selector
     - Suffix
     - Holds
     - Rows
     - Words
     - Top bit
   * - 0
     - ``_ROUTE``
     - IPv4 routes
     - 16
     - 2
     - not probed
   * - 1
     - ``_PPPOE``
     - PPPoE sessions
     - 8
     - 1
     - 15
   * - 2
     - ``_NEXTHOP``
     - Next hops
     - 16
     - 1
     - 20
   * - 3
     - ``_L3IF``
     - L3 interfaces
     - 16
     - 4
     - 116
   * - 4
     - ``_EXTADDR``
     - External addresses
     - 8
     - 3
     - 74
   * - 5
     - ``_ROUTE6``
     - IPv6 routes; cleared at setup, otherwise unused
     - 8
     - 5
     - not probed
   * - 6
     - ``_BIND``
     - WAN bindings; cleared at setup, otherwise unused
     - 32
     - 1
     - not probed
   * - 7
     - ``_WANTYPE``
     - WAN types; cleared at setup, otherwise unused
     - 16
     - 1
     - not probed
   * - 8
     - none
     - An access never completes
     - none
     - none
     - none
   * - 9
     - ``_NAPT_IN``
     - Inbound NAPT entries
     - 4096
     - 3
     - 89
   * - 10
     - ``_NAPT``
     - Outbound NAPT entries
     - 4096
     - 1
     - 16
   * - 11
     - ``_NEIGH6``
     - IPv6 neighbours; cleared at setup, otherwise unused
     - 128
     - 3
     - 79
   * - 12
     - ``_FLOW``
     - Five-tuple flows; cleared at setup, otherwise unused
     - 256
     - 4
     - 127
   * - 13
     - ``_NEIGH``
     - The IPv4 neighbour CAM
     - 128
     - 2
     - 43
   * - 14, 15
     - none
     - No table
     - none
     - none
     - none

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``_ROUTE`` (0) holds IPv4 routes, 16 rows of 2 words. It is written from
       the route notifier (see "The route notifier").
     - T
     - A routing test with the routes of the board: the route rows follow ``ip
       route`` for a /32, a /30, a /24 and a default route.
   * - ``_PPPOE`` (1) holds PPPoE sessions, 8 rows of 1 word, top bit 15. It is
       written for a PPPoE WAN (see "The PPPoE encapsulation path").
     - T
     - A PPPoE test.
   * - ``_NEXTHOP`` (2) holds next hops, 16 rows of 1 word, top bit 20. It is
       written from the neighbour notifier and for a PPPoE WAN.
     - T
     - A routing test and a PPPoE test.
   * - ``_L3IF`` (3) holds L3 interfaces, 16 rows of 4 words, top bit 116. It is
       written from the address notifier and for a PPPoE WAN.
     - T
     - A routing test.
   * - ``_EXTADDR`` (4) holds external addresses, 8 rows of 3 words, top bit 74.
       It is written from the address notifier and for a PPPoE WAN.
     - T
     - A routing test and a PPPoE test.
   * - ``_ROUTE6`` (5, IPv6 routes, 8 rows of 5 words), ``_BIND`` (6, WAN
       bindings, 32 rows of 1 word) and ``_WANTYPE`` (7, WAN types, 16 rows of 1
       word) are reset and zeroed at setup and otherwise untouched. Their word
       counts are not measured.
     - I
     - A walking one over one entry of each would measure the widths; only the
       clear at setup depends on them.
   * - There is no table at selector 8 (see "The table access port"). The driver
       has none at 14 and 15 either and refuses an access to them.
     - M
     - A read of selector 8 by hand never completed.
   * - ``_NAPT_IN`` (9) holds inbound NAPT entries, 4096 rows of 3 words, top
       bit 89. ``_NAPT`` (10) holds outbound NAPT entries, 4096 rows of 1 word,
       top bit 16. Both are 1024 buckets of ``RTL9602C_NAPT_WAYS`` (4) ways,
       entry = bucket * 4 + way (see "Outbound NAPT (selector 10)" and "Inbound
       NAPT (selector 9)"). They are written per flow.
     - T, I
     - T: a routing test reads the outbound entry of the reference flow back at
       the expected word through ``l3nat``. I: no view prints an inbound
       entry, so its word is not checked on a running board.
   * - ``_NEIGH6`` (11) holds IPv6 neighbours, 128 rows of 3 words, top bit 79;
       ``_FLOW`` (12) holds five-tuple flows, 256 rows of 4 words, top bit 127.
       Both are reset and zeroed at setup; ``l3nat`` shows the first four rows
       of each.
     - M
     - The walking one of the last row of this table; ``cat
       /sys/kernel/debug/rtl9602c/l3nat``.
   * - ``_NEIGH`` (13) is the IPv4 neighbour CAM, 128 rows of 2 words, top bit
       43. It is written from the neighbour notifier.
     - T
     - A routing test.
   * - Every table holds at least the rows the driver uses: at every setup
       ``rtl9602c_nat_clear()`` reads every row of every table up to these
       counts, and every access completes.
     - M
     - The boot log has no ``engine still busy`` line, and ``cat l3nat`` answers
       afterwards.
   * - The highest entry bit that stores, measured: outbound NAPT 16, inbound
       NAPT 89, neighbour CAM 43, IPv6 neighbour 79, PPPoE 15, next hop 20, L3
       interface 116, external address 74, five-tuple 127. Every bit below it
       stores (bit 0 of each entry was not probed). A table does not keep every
       bit it is given: of the 31 probed bits of the one-word outbound entry, 16
       store.
     - M
     - A walking one over entry 0 of each table, by hand, on an idle engine.

Reset strobes, table contents and the clear at setup
====================================================

Each table engine has a reset strobe, one bit of ``RTL9602C_L3TBL_RESET``. A
strobe does not empty its table. The tables keep their contents across a warm
restart of the CPU, because the switch core is not reset by it, and on a board
that nothing has programmed they hold arbitrary words. So setup strobes every
table engine, and then writes zeros over every row that is not zero.

.. list-table::
   :header-rows: 1
   :widths: 10 54 36

   * - Bit
     - Macro
     - Table engine, by the macro's name
   * - 15
     - ``RTL9602C_L3TBL_RESET_NEIGH``
     - IPv4 neighbour CAM
   * - 14
     - ``RTL9602C_L3TBL_RESET_FLOW``
     - Five-tuple flows
   * - 13
     - ``RTL9602C_L3TBL_RESET_ROUTE6``
     - IPv6 routes
   * - 12
     - ``RTL9602C_L3TBL_RESET_NEIGH6``
     - IPv6 neighbours
   * - 11
     - ``RTL9602C_L3TBL_RESET_BIND``
     - WAN bindings
   * - 10
     - ``RTL9602C_L3TBL_RESET_WANTYPE``
     - WAN types
   * - 9
     - ``RTL9602C_L3TBL_RESET_ROUTE``
     - IPv4 routes
   * - 8
     - ``RTL9602C_L3TBL_RESET_PPPOE``
     - PPPoE sessions
   * - 7
     - ``RTL9602C_L3TBL_RESET_NEXTHOP``
     - Next hops
   * - 6
     - ``RTL9602C_L3TBL_RESET_L3IF``
     - L3 interfaces
   * - 5
     - ``RTL9602C_L3TBL_RESET_EXTADDR``
     - External addresses
   * - 3
     - ``RTL9602C_L3TBL_RESET_NAPT_IN``
     - Inbound NAPT entries
   * - 2
     - ``RTL9602C_L3TBL_RESET_NAPT``
     - Outbound NAPT entries

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_L3TBL_RESET`` (``0x800104``) holds one reset strobe per table
       engine, in the bits of the table above. It reads 0 in the reset state. A
       strobe reads back non-zero on the read right after it is written and
       zero on the next: it clears itself.
     - M
     - Strobes written by hand and read back twice; a read in the reset state.
   * - A strobe does not empty its table. A PPPoE entry and a five-tuple entry
       kept their contents across their own strobes, with the engine off and on,
       and an outbound NAPT entry kept its contents across all thirteen.
     - M
     - Entries written by hand, strobed and read back.
   * - Which table each bit resets is inferred, not measured; so is the premise
       that the table engines must be reset before the global enable. Setup
       strobes all thirteen before it sets ``EN``.
     - I
     - Bringing the engine up without the strobes and repeating the routing test
       would show whether they are needed.
   * - Bring-up does not wait for the strobes to clear before the table accesses
       that follow it. That every strobe has finished by then is not measured.
     - I
     - The boot log line ``L3: reset 13 of 13 table engine(s), engine ON
       (control ..., reset ...)`` prints the register at the end of bring-up;
       ``reset 00000000`` at every boot would settle it.
   * - Table contents survive a warm restart, since the switch core is not reset
       by a CPU restart: the neighbour CAM held the same words across three
       boots, and a configuration value written in one boot was still there in
       the next. A table that nothing has written holds arbitrary words on an
       unprogrammed board (the five-tuple, outbound NAPT, neighbour CAM and
       IPv6-neighbour tables). After a chip reset the five-tuple, outbound NAPT,
       IPv6-neighbour and PPPoE tables read zero while the neighbour CAM still
       held its earlier words.
     - M
     - Reads across boots and before and after a chip reset; the configuration
       word read across a warm restart.
   * - So at setup ``rtl9602c_nat_clear()`` reads every row of every table and
       writes zeros over any row that is not all zero, and logs ``cleared N
       L3/NAT table entries left by the previous kernel, in T ms`` when it wrote
       any. It runs in ``rtl9602c_setup()`` right after the address table is
       emptied (``rtl9602c_l2_clear()``) and before the engine is brought up.
     - M
     - The boot log; ``cat l3nat`` after boot shows zeros except the rows that
       the notifiers wrote.
   * - An all-zero row is inert: every valid bit is clear, and the engine
       carries translated flows with the unused tables all zero.
     - T
     - A routing test.
   * - The NAPT hit bitmap is not cleared at setup. On a board after power-up it
       held arbitrary bits, different from boot to boot; after a chip reset it
       reads zero. A stale bit makes the first statistics query of the first
       flow in that entry report one use (see "Hit bitmaps, flow statistics and
       ageing").
     - M
     - Words 0 and 1 read on two boots; a read in the reset state.

Bring-up sequence
=================

The steps of ``rtl9602c_setup()`` that concern the engine, in order; the whole
of setup is on the *main* page ("Setup, in order").

1. Empty the address table (``rtl9602c_l2_clear()``) and then the engine's
   tables (``rtl9602c_nat_clear()``). Purpose: start from empty tables, since
   neither a warm restart nor the table strobes empty them. (M)
2. Refresh the bridge state (``rtl9602c_bridge_refresh()``), which isolates the
   standalone ports (see the *bridging* page), and initialise ``flow_lock``.
   Purpose: a routed egress port is opened later, when a next hop is placed on
   it (see "The address-table entries the engine uses"). (T)
3. In ``rtl9602c_l34_init()``, write the reset strobe of each of the thirteen
   table engines, each only if its bit reads 0 (a bit already set is counted as
   busy and left), in the fixed order route, PPPoE, next hop, L3 interface,
   external address, neighbour CAM, inbound NAPT, outbound NAPT, IPv6 route,
   binding, IPv6 neighbour, WAN type, five-tuple. Purpose: reset every table
   engine before the engine is enabled. Whether the order matters is not
   established. (I)
4. Set ``RTL9602C_MIB_L3IF_CTRL_EN``. Purpose: the per-interface counters count.
   (I)
5. Write ``RTL9602C_L3_BIND_CFG_INIT`` to ``RTL9602C_L3_BIND_CFG`` and log it.
   Purpose: defined actions for a binding mismatch. (I)
6. Read-modify-write ``RTL9602C_L3_CFG``: NAPT mode, and interface lookup by
   MAC. Purpose: the engine translates the frames addressed to the router (see
   "Global enable and configuration"). (T)
7. Read-modify-write ``RTL9602C_L3_GLOBAL_CTRL_EN``, then read the register
   back. Purpose: turn the engine on, and learn whether it is on. (T)
8. Write ``RTL9602C_L3_AUX_CFG_ON`` and log it. Purpose: it is written on the
   assumption, not verified, that the engine needs it. (I)
9. Record ``l34_up`` as ``EN`` read back and, if it is set, call
   ``rtl9602c_l2_l3lookup_cpu(p, true)`` (see "The address-table entries the
   engine uses"). Purpose of the order: that call sends frames addressed to this
   device, management included, to the engine instead of the CPU, so it may only
   happen once the engine runs. (T)
10. Log ``L3: reset N of 13 table engine(s), engine ON (control 00000005,
    reset ...)``. (M)
11. Register the notifiers, ``rtl9602c_l34_notifiers_register()``: the address
    notifier, then the neighbour (netevent) notifier, then the route (FIB)
    notifier. Purpose: the route notifier comes last because registering it
    replays every route the kernel already holds, and a route resolves against
    the rows that the other two write. A notifier that fails to register is
    reported and the switch still comes up. (T)

The steps are checked as follows. Step 1 by the boot log and by ``l3nat`` after
boot showing zeros except the rows that the notifiers wrote. Step 2 by a routing
test and by the flooding check of "The address-table entries the engine uses".
Steps 6, 7 and 9 by a routing test; step 9 also by the board answering after
bring-up, its management traffic still reaching the CPU. Step 10 by the boot
log. Step 11 by the route rows following ``ip route`` (see "The route
notifier"). Steps 3, 4, 5 and 8 have no test: the driver takes them on the
assumption that they are needed (see "Open questions").

At teardown the driver unregisters the three notifiers first, since they can
fire on any CPU and touch these tables. Then it cancels both work items and
frees the events still queued, which releases the netdev reference that each
queued neighbour event holds. Last, ``rtl9602c_l34_fini()`` undoes the
bring-up: it traps this device's own addresses to the CPU again
(``rtl9602c_l2_l3lookup_cpu()`` with ``false``), clears the engine's master
enable, strobes the table resets and empties the tables with
``rtl9602c_nat_clear()``, so that no flow is forwarded by rows that no driver
keeps.

Entry layouts
=============

Each field below is a ``RTL9602C_FIELD(lsb, len)``, a position and a width in
entry bits. ``rtl9602c_flow_put()`` and ``rtl9602c_flow_get()`` set and read one
bit at a time across the entry's words, so a field may cross a word boundary,
and every write builds the whole entry from zero. Bits that no macro names are
listed as *none*, with what the driver writes there.

Route (selector 0)
------------------

A route row says what the engine does with a frame whose destination lies in a
prefix. It has 2 words and the table 16 rows. The driver writes the whole table
from a shadow of the main routing table, longest prefix first (see "The route
notifier").

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Entry bits
     - Macro
     - Meaning
   * - 31:0
     - ``RTL9602C_ROUTE_DST``
     - The destination prefix.
   * - 36:32
     - ``RTL9602C_ROUTE_DST_LEN``
     - The prefix length minus one.
   * - 37
     - ``RTL9602C_ROUTE_VALID``
     - The row is valid.
   * - 39:38
     - ``RTL9602C_ROUTE_ACTION``
     - 0 hands the frame to the CPU;
       ``RTL9602C_ROUTE_ACTION_DROP`` (1); ``RTL9602C_ROUTE_ACTION_NEIGH`` (2);
       ``RTL9602C_ROUTE_ACTION_NEXTHOP`` (3).
   * - 40
     - ``RTL9602C_ROUTE_INTERNAL``
     - Marks a LAN-side route.
   * - 44:41
     - ``RTL9602C_ROUTE_L3IF_IDX`` and ``RTL9602C_ROUTE_NEXTHOP_IDX``
     - The same four bits: the L3-interface row for action 2, the next-hop row
       for action 3.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_ROUTE_DST`` (31:0) is the destination prefix.
       ``RTL9602C_ROUTE_DST_LEN`` (36:32) is the prefix length minus one, so 0
       is a /1 and 31 a /32, and a /0 has no encoding. ``RTL9602C_ROUTE_VALID``
       (37). Read back: a /30 carries 29, a /24 23, a /32 31, a /1 0.
     - T
     - A routing test: the route rows follow ``ip route``; the ``l3nat`` route
       rows for a /32, a /30, a /24 and a default route.
   * - ``RTL9602C_ROUTE_ACTION`` (39:38) value 0 hands the frame to the CPU. A
       valid row with action 0 for a LAN prefix made the engine hand every frame
       of an accepted flow to the CPU.
     - M
     - The second word ``0x0000003d`` on the LAN rows: 301 of 300 frames went
       through the CPU. The PPPoE test asserts that no LAN row has action 0.
   * - Action 2, ``RTL9602C_ROUTE_ACTION_NEIGH``: the prefix is on-link on the
       L3 interface in bits 44:41 (``RTL9602C_ROUTE_L3IF_IDX``). With an
       Ethernet WAN the engine needs an action-2 row for the subnet on each side
       of a translation (the LAN host's and the WAN interface's): zeroing either
       one sent every frame of an offloaded flow to the CPU (192,219 of 192,214;
       190,022 of 190,016).
     - M
     - One row zeroed at a time by hand and restored through the stack, the CPU
       port's frame counter read; the routing test with the route rows in
       place.
   * - Action 3, ``RTL9602C_ROUTE_ACTION_NEXTHOP``: via the next-hop row in bits
       44:41 (``RTL9602C_ROUTE_NEXTHOP_IDX``, the same four bits as the
       interface index of action 2). The engine does not consult this row for
       translated frames: zeroing the /32 action-3 row of an offloaded flow's
       destination changed nothing (349 of 191,417 frames to the CPU, as in the
       control).
     - M
     - As the row above.
   * - Action 1, ``RTL9602C_ROUTE_ACTION_DROP``: discard in hardware; the driver
       writes it for a blackhole route. That 1 drops is inferred, not measured.
     - I
     - A blackhole route over a prefix that carries an offloaded flow's traffic,
       with the drop counters, would settle it.
   * - ``RTL9602C_ROUTE_INTERNAL`` (40) marks a LAN-side route. The driver sets
       it on a directly attached route whose egress port is a switch port other
       than the PON port, since on a stick with one uplink the PON port is the
       WAN side. Read back: the LAN /30 second word ``0x000001bd`` (length 29,
       valid, action 2, internal, interface 0) and the WAN /24 ``0x000002b7``
       (length 23, valid, action 2, interface 1, not internal).
     - M
     - The ``l3nat`` route rows; the routing test asserts ``0x1bd`` in the low
       nine bits of the LAN row.
   * - What the engine does with ``INTERNAL`` is not established.
     - I
     - Clearing it on the LAN row and repeating the routing test would show it.
   * - More rows read back: ``0x000002ff`` (/32, valid, action 3, next hop 1)
       and ``0x000000e0`` (/1, valid, action 3, next hop 0), twice, the two
       halves of a default route.
     - M
     - The ``l3nat`` route rows with a host route and a default route; the
       routing test.
   * - Whether the engine takes the longest matching prefix or the first valid
       row is not established. The driver writes the rows longest prefix first,
       which is right either way (see "The route notifier").
     - I
     - A /24 row placed after a /1 that covers it, with a flow into the /24,
       would settle it.

L3 interface (selector 3)
-------------------------

An L3-interface row describes one interface of the router: the MAC that frames
addressed to it carry, the MAC and VLAN that frames leave it with, and its
address. It has 4 words and the table 16 rows. The address notifier writes one
row for each netdev, and a PPPoE WAN takes one (see "The address notifier" and
"The PPPoE encapsulation path").

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Entry bits
     - Macro
     - Meaning
   * - 0
     - ``RTL9602C_L3IF_VALID``
     - The row is valid; set in every row the driver writes.
   * - 12:1
     - ``RTL9602C_L3IF_VID``
     - The interface's VLAN.
   * - 60:13
     - ``RTL9602C_L3IF_MAC``
     - The interface's MAC, first octet in the highest bits.
   * - 63:61
     - ``RTL9602C_L3IF_MAC_MASK``
     - How much of the MAC an arriving frame must match; the driver writes
       ``RTL9602C_L3IF_MAC_MASK_EXACT`` (7).
   * - 64
     - ``RTL9602C_L3IF_ROUTE_EN``
     - The interface may route; written 1.
   * - 78:65
     - ``RTL9602C_L3IF_MTU``
     - An MTU for the interface, 14 bits.
   * - 81:79
     - none
     - Stores; written 0.
   * - 82
     - ``RTL9602C_L3IF_OFFLOAD_EN``
     - The interface takes part in L3 and L4 processing; written 1.
   * - 114:83
     - ``RTL9602C_L3IF_IP``
     - The interface's own IPv4 address.
   * - 116:115
     - none
     - Stores; written 0.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_L3IF_VALID`` (0) is set in every row the driver writes. A row
       built by the driver's encoder for a given MAC, VLAN, MTU and address
       matched, bit for bit, a working row read back for the same parameters,
       once bit 0 was set in the read-back row.
     - M
     - The encoder's output compared with the read-back row; a routing test.
   * - ``RTL9602C_L3IF_VID`` (12:1) is the interface's VLAN. The engine gives a
       frame routed out of the interface this VLAN as its destination VLAN:
       changing the row's VID from 0 to 100 moved the output descriptor's
       destination VLAN from 0 to 100. The driver writes the VLAN ID of a VLAN
       device, otherwise 0, and 0 works.
     - M
     - The output descriptor of the per-frame capture read before and after the
       change; a routing test with VID 0.
   * - ``RTL9602C_L3IF_MAC`` (60:13) is the interface's MAC, first octet in the
       highest bits. The destination MAC of an arriving frame is matched against
       it (see "Global enable and configuration"), and it is the source MAC of
       frames routed out of the interface.
     - M
     - A capture at the far side of the translated frames: the source MAC equal
       to the row's MAC.
   * - ``RTL9602C_L3IF_MAC_MASK`` (63:61) says how much of the MAC an arriving
       frame must match. The driver writes ``RTL9602C_L3IF_MAC_MASK_EXACT`` (7),
       with which a row matches frames addressed to its own MAC; the other
       values are not used.
     - T
     - A routing test.
   * - ``RTL9602C_L3IF_ROUTE_EN`` (64): the interface may route; written 1.
       ``RTL9602C_L3IF_OFFLOAD_EN`` (82): the interface takes part in L3 and L4
       processing; written 1.
     - T
     - A routing test and a PPPoE test.
   * - ``RTL9602C_L3IF_MTU`` (78:65, 14 bits) is an MTU for the interface. The
       driver writes the netdev's MTU when the address is added, and again when
       the MTU changes; for a PPPoE WAN, the egress device's MTU less 8 (see
       "The PPPoE encapsulation path").
     - M
     - The ``l3nat`` L3-interface row of a PPPoE WAN reads MTU 1492.
   * - The engine enforces the row's MTU: a packet of an offloaded flow that
       does not fit it goes to the CPU, whose stack deals with it as with any
       packet too large for the route (a datagram that allows fragmentation is
       fragmented), and packets that fit stay in the engine.
     - M
     - Over a PPPoE WAN (row MTU 1492), with the flow marked ``OFFLOAD``: 2000
       UDP datagrams of 1500 bytes, fragmentation allowed, reached the
       concentrator as 4000 fragments of 1492 and 28 bytes, with 2041 frames
       reaching the CPU; 2000 of 1028 bytes arrived whole with 21 frames
       reaching the CPU.
   * - ``RTL9602C_L3IF_IP`` (114:83) is the interface's own IPv4 address. Bits
       81:79 and 116:115 store but are written 0.
     - T
     - A routing test; the storage from the walking one of "Tables, selectors
       and geometry".

Next hop (selector 2)
---------------------

A next-hop row says how a frame leaves: through which L3 interface (its source
MAC and VLAN), with or without a PPPoE header, and to which address-table entry
(its destination MAC and egress port). It is 1 word and the table has 16 rows.
The neighbour notifier writes the rows of Ethernet next hops, and a PPPoE WAN
writes its own.

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Entry bits
     - Macro
     - Meaning
   * - 0
     - ``RTL9602C_NEXTHOP_ENCAP``
     - 0 sends the frame as Ethernet; ``RTL9602C_NEXTHOP_ENCAP_PPPOE`` (1) adds
       a PPPoE session header on egress.
   * - 4:1
     - ``RTL9602C_NEXTHOP_L3IF_IDX``
     - The L3-interface row the frame leaves by.
   * - 7:5
     - ``RTL9602C_NEXTHOP_PPPOE_IDX``
     - The PPPoE row whose session goes into the header.
   * - 18:8
     - ``RTL9602C_NEXTHOP_L2_IDX``
     - An index into the address table.
   * - 20:19
     - none
     - A two-bit field that stores; written 0.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_NEXTHOP_ENCAP`` (0): 0 sends the frame as Ethernet;
       ``RTL9602C_NEXTHOP_ENCAP_PPPOE`` (1) adds a PPPoE session header on
       egress. Clearing only this bit on a PPPoE WAN's next hop took the
       engine's PPPoE frames from 296 to 0 (they left as plain IP), and setting
       it again brought them back.
     - M
     - The bit cleared and set by hand while a PPPoE flow ran, the frames
       counted at the concentrator; a PPPoE test.
   * - ``RTL9602C_NEXTHOP_L3IF_IDX`` (4:1) is the L3-interface row the frame
       leaves by, which gives its source MAC and VLAN.
     - T
     - A routing test; the egress interface in the engine's output descriptor
       equals it (see "The per-frame capture").
   * - ``RTL9602C_NEXTHOP_PPPOE_IDX`` (7:5) is the PPPoE row whose session goes
       into the header; the engine's output descriptor named the same row.
     - M
     - A capture of the output descriptor; a PPPoE test.
   * - ``RTL9602C_NEXTHOP_L2_IDX`` (18:8) is an index into the address table;
       the routed frame's destination MAC and egress port come from that row.
       Pointing a next hop at another address-table row changed the destination
       MAC of the engine's frames (350 of 400; the other 50 were the CPU's, sent
       before the flow was offloaded).
     - M
     - The destination MACs counted at the far side.
   * - Bits 20:19 are a two-bit field that stores (3 written reads back 3); the
       driver writes 0, and with 0 the frames are encapsulated exactly as
       ``ENCAP`` says. The other three values are not used.
     - M
     - A read-back after a write by hand; a PPPoE test.
   * - The word stores exactly bits 20:0: ``0xffffffff`` reads back
       ``0x001fffff``. Rows read back: ``0x00023002`` (Ethernet, interface 1,
       address-table row 560) and ``0x00023167`` (PPPoE, interface 3, session
       row 3, address-table row 561).
     - M
     - The ``l3nat`` next-hop rows.

IPv4 neighbour CAM (selector 13)
--------------------------------

The CAM holds one row per resolved neighbour, written by the neighbour notifier.
A row is 2 words and the table has 128 rows. What the engine uses the CAM for is
not established; the rows below are what the driver writes and what was read
back.

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Entry bits
     - Macro
     - Meaning
   * - 31:0
     - ``RTL9602C_NEIGH_IP``
     - The neighbour's address.
   * - 32
     - ``RTL9602C_NEIGH_VALID``
     - The row is valid.
   * - 43:33
     - ``RTL9602C_NEIGH_L2_IDX``
     - The address-table row that holds the neighbour's MAC.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_NEIGH_IP`` (31:0) is the neighbour's address,
       ``RTL9602C_NEIGH_VALID`` (32) the valid bit and
       ``RTL9602C_NEIGH_L2_IDX`` (43:33) the address-table row that holds the
       neighbour's MAC. A row's second word read back ``0x00000109``: valid,
       address-table row 132.
     - M
     - The ``l3nat`` neighbour rows (``neigh``); the storage from the walking
       one of "Tables, selectors and geometry".
   * - The table is content-addressed: the engine matches an address against
       ``NEIGH_IP`` and no other table points into it by index. The driver
       writes a neighbour's CAM row at the same index as its next-hop row for
       its own bookkeeping only.
     - I
     - Inferred from the layouts: no index field anywhere names a CAM row.
   * - The destination MAC of a routed frame does not come from the CAM:
       pointing a CAM row's address-table index at another row changed none of
       400 frames.
     - M
     - The far-side MACs counted.
   * - What the engine uses the CAM for (recognising the far end of a directly
       attached route is the reading), and whether the rows are needed at all,
       is not established.
     - I
     - Zeroing the CAM rows and repeating the routing test would show it.

External address (selector 4)
-----------------------------

An external-address row is an address that a translated source can become, with
the next hop that frames translated to it leave by. It has 3 words and the table
8 rows. The address notifier writes a row for each address of a router
interface, and a PPPoE WAN points its row at its own next hop.

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Entry bits
     - Macro
     - Meaning
   * - 31:0
     - none
     - Stores; written 0.
   * - 63:32
     - ``RTL9602C_EXTADDR_IP``
     - The address that a translated source becomes.
   * - 64
     - ``RTL9602C_EXTADDR_VALID``
     - The row is valid.
   * - 66:65
     - ``RTL9602C_EXTADDR_MODE``
     - The kind of translation; the driver writes
       ``RTL9602C_EXTADDR_MODE_NAPT`` (0), an address shared by NAPT flows.
   * - 70:67
     - ``RTL9602C_EXTADDR_NEXTHOP_IDX``
     - The next-hop row that a frame translated to this address leaves by.
   * - 74:71
     - none
     - Stores; written 0.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_EXTADDR_IP`` (63:32) is the address that a translated source
       becomes; ``RTL9602C_EXTADDR_VALID`` (64). The source address on the wire
       of an offloaded flow is this row's address.
     - M
     - A capture at the far side; the routing test sees the translated source
       address on the wire.
   * - ``RTL9602C_EXTADDR_MODE`` (66:65) is the kind of translation the address
       serves; the driver writes ``RTL9602C_EXTADDR_MODE_NAPT`` (0), an address
       shared by NAPT flows. The other values are not used.
     - T
     - A routing test.
   * - ``RTL9602C_EXTADDR_NEXTHOP_IDX`` (70:67) is the next-hop row that a frame
       translated to this address leaves by. A row with next hop 3 sent its
       flows out through next-hop row 3, encapsulated, and nowhere else; its
       third word reads back ``0x00000019`` (valid, NAPT, next hop 3).
     - M
     - A PPPoE test; the ``l3nat`` external-address rows.
   * - For an address learnt from the address notifier the driver writes next
       hop 0, so next-hop row 0 must hold the uplink's gateway. With a LAN-side
       neighbour in row 0 the switch discarded 15,700 of 15,769 translated
       frames at the ingress port (forwarding reason 228); with the gateway
       there it discarded none and sent 15,200. Hence the neighbour notifier
       keeps row 0 for a next hop on the PON port (see "The neighbour
       notifier").
     - M
     - Five runs alternating the occupant of row 0 on one boot; the switch's
       frame capture latching reason 228 in the runs with the LAN-side
       neighbour.
   * - Bits 31:0 and 74:71 store but are written 0.
     - M
     - The walking one of "Tables, selectors and geometry".

PPPoE session (selector 1)
--------------------------

A PPPoE row holds the session ID that the engine writes into the header of a
frame it encapsulates. It is 1 word and the table has 8 rows. Its one field is
``RTL9602C_PPPOE_ENTRY_SESSION`` (15:0).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PPPOE_ENTRY_SESSION`` (15:0) is the session ID that the engine
       writes into the PPPoE header. The word stores exactly 16 bits
       (``0xffffffff`` reads back ``0x0000ffff``), rows are independent, row 7
       takes a value, and all eight rows read 0 after reset. The concentrator
       received the session ID that the driver wrote, on frames that the engine
       produced.
     - M
     - Writes and read-backs by hand; a capture at the concentrator; the
       ``l3nat`` PPPoE rows.
   * - The entry has no valid bit; the driver treats session 0 as a free row.
       0x0000 is the SESSION_ID of the discovery packets and of a PADS that
       refuses the request, so it never names an established session; 0xffff is
       reserved.
     - P
     - RFC 2516 clause 4 (SESSION_ID; 0xffff reserved) and clauses 5.1 to 5.4
       (discovery packets, PADS).

Outbound NAPT (selector 10)
---------------------------

An outbound NAPT entry is a pointer: it holds the index of the flow's inbound
entry and carries no key of its own. It is 1 word and the table has 4096 rows,
1024 buckets of four ways; the bucket is given by the outbound hash (see "The
NAPT hashes").

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Entry bits
     - Macro
     - Meaning
   * - 11:0
     - ``RTL9602C_NAPT_INBOUND_IDX``
     - The index of the flow's inbound entry.
   * - 12
     - ``RTL9602C_NAPT_VALID``
     - The entry is valid.
   * - 16:13
     - none
     - Stores; written 0.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The table is 1024 buckets of four ways: entry = bucket * 4 + way, the
       bucket given by the outbound hash. The entry carries no key of its own.
     - T
     - A routing test and a PPPoE test with flows of different five-tuples
       carried by the engine; the placement measured under "The NAPT hashes".
   * - ``RTL9602C_NAPT_INBOUND_IDX`` (11:0) is the index of the flow's inbound
       entry; ``RTL9602C_NAPT_VALID`` (12). A live flow's entry read
       ``0x00001128``: valid, inbound entry 296. Bits 16:13 store and are
       written 0.
     - M
     - The entry read while the flow ran; the walking one of "Tables, selectors
       and geometry".
   * - That the engine searches all four ways of a bucket is inferred from the
       table's shape; only way 0 has been seen in use. The driver places a
       second flow of the same bucket in way 1.
     - I
     - Two flows forced into one bucket (source ports chosen with the outbound
       hash), both carried by the engine, would settle it.

Inbound NAPT (selector 9)
-------------------------

An inbound NAPT entry holds the translation of a flow: the LAN host's address
and port, the external address row and port the flow leaves as, and a digest of
the remote end. It is 3 words and the table has 4096 rows, 1024 buckets of four
ways; the bucket is given by the inbound hash, from the address and port a reply
is sent to.

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Entry bits
     - Macro
     - Meaning
   * - 31:0
     - ``RTL9602C_NAPT_IN_INTERNAL_ADDR``
     - The LAN host's address.
   * - 47:32
     - ``RTL9602C_NAPT_IN_INTERNAL_PORT``
     - The LAN host's port.
   * - 63:48
     - ``RTL9602C_NAPT_IN_REMOTE_HASH``
     - A 16-bit digest of the remote end.
   * - 66:64
     - ``RTL9602C_NAPT_IN_EXTADDR_IDX``
     - The external-address row of the translated address.
   * - 82:67
     - ``RTL9602C_NAPT_IN_EXTERNAL_PORT``
     - The translated port.
   * - 83
     - ``RTL9602C_NAPT_IN_TCP``
     - 0 for UDP; the driver writes 1 for TCP.
   * - 85:84
     - ``RTL9602C_NAPT_IN_VALID``
     - Two bits; ``RTL9602C_NAPT_IN_VALID_ACTIVE`` (2) marks an entry in use.
   * - 89:86
     - none
     - Stores; written 0.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The table is 1024 buckets of four ways: entry = bucket * 4 + way, the
       bucket given by the inbound hash. An inbound frame finds its entry only
       in that bucket; the engine follows no pointer to it.
     - M
     - Placement measured with entries at the hash's slots and at other slots
       (see "The NAPT hashes").
   * - ``RTL9602C_NAPT_IN_INTERNAL_ADDR`` (31:0) and
       ``RTL9602C_NAPT_IN_INTERNAL_PORT`` (47:32) are the LAN host's address and
       port. ``RTL9602C_NAPT_IN_EXTADDR_IDX`` (66:64) is the external-address
       row of the translated address. ``RTL9602C_NAPT_IN_EXTERNAL_PORT`` (82:67)
       is the translated port. A live entry decoded to exactly the flow captured
       on both sides of the translation.
     - M
     - The entry read while the flow ran, compared with captures on both sides.
   * - ``RTL9602C_NAPT_IN_REMOTE_HASH`` (63:48) is a 16-bit digest of the remote
       end (see "The NAPT hashes"). The engine does not check it on replies:
       with all sixteen bits of a live entry's digest inverted, 40,018 of 40,000
       replies were still carried by the engine.
     - M
     - The digest inverted and restored by hand; the LAN-side count and the CPU
       port's counter.
   * - ``RTL9602C_NAPT_IN_TCP`` (83) is 0 for UDP, as read in a live UDP entry.
     - M
     - A live UDP entry read while the flow ran.
   * - ``RTL9602C_NAPT_IN_VALID`` (85:84) is two bits wide;
       ``RTL9602C_NAPT_IN_VALID_ACTIVE`` (2) marks an entry in use (a live entry
       read 2). The driver treats any non-zero value as occupied. Bits 89:86
       store and are written 0.
     - M
     - A live entry read while the flow ran; the walking one of "Tables,
       selectors and geometry".
   * - For a UDP flow with internal port 5000, external port 5000 and
       external-address row 0, sent to 203.0.113.9 port 9999, words 1 and 2 of
       the entry are ``0x9d061388`` and ``0x00209c40``.
     - T
     - The routing test asserts the encoder's words for that flow.
   * - TCP flows: the protocol bit is 1 for TCP, and with it and the protocol
       term of the two hashes the engine carries a TCP connection both ways.
     - M
     - The device suite's ``router.tcp``: 100,000,000 bytes from a LAN host to
       the far end of a static WAN at 964 Mb/s, received in full from the WAN
       address, with 60 frames reaching the CPU port against some 69,000
       segments and the connection marked ``HW_OFFLOAD`` while it was up.

The NAPT hashes
===============

Both NAPT tables are 1024 buckets of four ways, and the engine looks a flow up
only in the bucket that its hash gives, so the driver computes both hashes and
places the entries itself. The inputs are host-order integers: A_i and P_i the
LAN host's address and port, A_r and P_r the remote end's, A_e and P_e the
translated (external) address and port, and t, 1 for TCP and 0 for UDP. A[j:k]
is bits j to k of A, ``^`` is exclusive or, and each bucket function gives 10
bits, 0 to 1023::

    outbound, rtl9602c_napt_bucket():
        s = P_i + A_i[15:0] + A_r[15:0] + P_r          (an 18-bit sum)
        f = s + s[17:10]
        bucket = ((t << 9) | A_i[31:26]) ^ A_i[25:16] ^ A_r[25:16]
                 ^ (A_r[31:26] << 4) ^ f               (bits 9:0)

    inbound, rtl9602c_napt_in_bucket():
        s = P_e + A_e[15:0]                            (a 17-bit sum)
        f = s + s[16:10]
        bucket = ((t << 9) | A_e[31:26]) ^ A_e[25:16] ^ f    (bits 9:0)

    remote digest, rtl9602c_napt_rem_hash():
        digest = A_r[31:16] ^ A_r[15:0] ^ P_r          (16 bits)

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The outbound bucket comes from the five-tuple as the LAN host sent it, by
       the first function above. The engine finds a flow's outbound entry only
       in this bucket.
     - T
     - A routing test and a PPPoE test: flows of different five-tuples carried
       by the engine; the placement measured below.
   * - The inbound bucket comes from the address and port a reply is sent to, by
       the second function above. The remote end is not an input.
     - T
     - A throughput test, NAT and NAT over PPPoE downstream: replies carried by
       the engine only with the entries placed by this function. The routing
       test asserts the reference flow's inbound bucket.
   * - The remote digest is written into ``RTL9602C_NAPT_IN_REMOTE_HASH``. It
       rests on one entry only: for 203.0.113.9 port 9999 it gives ``0xcb00`` ^
       ``0x7109`` ^ ``0x270f`` = ``0x9d06``, the value read in a live entry. The
       engine does not check the field on replies; whether the outbound lookup
       compares it is not established.
     - I
     - A second live entry with a different remote end, read back, would settle
       the function; the ``<flow>`` debug line prints the digest that the driver
       computed, for that check. Two flows in one outbound bucket would show
       whether the outbound lookup compares it.
   * - Placement, measured: a live flow's outbound entry sat in way 0 of the
       bucket that the outbound hash gives, and changing only the source port by
       one moved it to the bucket that the hash gives for the new port; the
       inbound entries of the same two flows sat in way 0 of the buckets that
       the inbound hash gives (74 and 75, entries 296 and 300). With inbound
       entries placed at the driver's own slot numbers instead, every reply went
       to the CPU (244,889 frames in 12 s); placed by the inbound hash, replies
       ran through the engine at the line rate of the test setup with the CPU
       port moving by 21 frames in 12 s.
     - M
     - Entries read while the flows ran; a throughput test, NAT downstream, with
       the inbound entries at the driver's slots and at the hash's.
   * - Because the remote end is not an input of the inbound hash and the engine
       ignores the digest, it cannot tell apart two inbound entries that share
       external-address row, external port and protocol, and those always share
       a bucket. So the driver declines a flow whose external port,
       external-address row and protocol are already in its inbound bucket; that
       flow stays in software.
     - M
     - A second flow from the same source to another remote port was declined
       and ran as ``[OFFLOAD]`` in the connection tracking table while the first
       stayed ``[HW_OFFLOAD]``.
   * - That the engine tells apart two inbound entries of one bucket by their
       external port (with row and protocol) is inferred; only one flow per
       bucket has been measured.
     - I
     - Two flows whose external ports fall in one inbound bucket, replies to
       both, each counted at its own LAN host, would settle it.

A worked example, computed from the field definitions and the two hash functions
for documentation addresses. The LAN host 192.0.2.2 sends UDP from port 5000 to
203.0.113.9 port 9999 through ``sfp`` (MAC ``00:00:5E:00:53:01``, address
192.0.2.1, MTU 1500), and the flow leaves translated to 198.51.100.7 port 5000
through a GEM interface (MAC ``00:00:5E:00:53:02``, address 198.51.100.7, MTU
1500) towards the gateway 198.51.100.1, whose address-table entry is row 560.
The outbound hash gives bucket 477 and the inbound hash bucket 430; with way 0
free in both, the outbound entry is entry 1908 and the inbound entry is entry
1720. The words that the driver writes, word 0 first::

    L3 interface 0     0a602001 e0000bc0 100c0bb9 00060000   sfp
    L3 interface 1     0a604001 e0000bc0 203c0bb9 0006319b   the GEM interface
    route 0            c0000200 000001b7                     192.0.2.0/24
    route 1            c6336400 000002b7                     198.51.100.0/24
    route 2            00000000 000000e0                     0.0.0.0/1
    route 3            80000000 000000e0                     128.0.0.0/1
    next hop 0         00023002                              the gateway
    neighbour CAM 0    c6336401 00000461                     the gateway
    external address 0 00000000 c6336407 00000001            198.51.100.7
    outbound NAPT 1908 000016b8                              the flow
    inbound NAPT 1720  c0000202 9d061388 00209c40            the flow

The ``l3nat`` view shows the first four outbound entries and no inbound entry,
so the two NAPT entries of a flow are located only by the ``<flow>`` debug line
that names them (see "The debugfs views").

How a translated frame goes through the engine
==============================================

The path of an upstream frame, from the LAN host to the wire, is set by the
tables above, and every step needs something of the tables and of the switch
around them. A reply takes the mirror path through the inbound entry. The rows
below give the steps in order.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - An upstream frame (LAN host to remote end) reaches the engine addressed
       to one of this device's MACs, the LAN host's gateway, whether or not the
       address table holds an entry for that MAC (see "The address-table
       entries the engine uses").
     - M
     - A routing test, also from a boot straight into router mode with no entry
       for the stick's MAC in the table.
   * - The engine finds the ingress L3 interface by the destination MAC, needs
       action-2 route rows for the subnets on both sides, finds the flow in the
       outbound bucket, follows the way to the inbound entry, and rewrites the
       source address (from the external-address row), the source port and the
       UDP checksum to match.
     - M
     - 300 UDP checksums verified at the concentrator, none wrong (a PPPoE
       test).
   * - The frame leaves by the next hop that the external-address row names, not
       by the route table's next hop: source MAC and VLAN from that next hop's
       L3-interface row, destination MAC and egress port from its address-table
       row, and for a PPPoE WAN the session from its PPPoE row.
     - M
     - The next-hop and external-address rows of "Entry layouts": the
       destination MACs counted at the far side after a next hop was pointed at
       another row, and the PPPoE frames carrying the session of the next-hop
       row.
   * - The switch then forwards it only if the address-table row that the next
       hop names carries ``RTL9602C_FDB_W2_NEXTHOP``; without it every routed
       frame was discarded at the ingress port with an empty destination mask
       and forwarding reason 83.
     - M
     - The switch's frame capture and ``rx_forward_discards`` (see "The
       address-table entries the engine uses").
   * - And only if the ingress port's isolation mask lets it reach the egress
       port.
     - M
     - As for the next-hop flag: the frames discarded with the mask closed (see
       "The address-table entries the engine uses").
   * - A frame that is not part of a translated flow is handed to the CPU
       whatever the route rows say (forwarding reason 34).
     - M
     - The routed flow without a flowtable, the switch's frame capture (see
       "Global enable and configuration").
   * - A next hop whose address-table row is on the frame's own ingress port
       gets the frame discarded at ingress with forwarding reason 228: the frame
       capture armed to latch only reason 228 latched a frame when next-hop row
       0 held a LAN-side neighbour, and none with the gateway there or when the
       upstream queue had no bandwidth grant.
     - M
     - The frame capture latching one chosen reason.
   * - A reply (remote end to the external address and port) is found by the
       inbound hash, translated back to the internal address and port, and, for
       a PPPoE WAN, stripped of its PPPoE header, all by the engine.
     - M
     - A throughput test, NAT and NAT over PPPoE downstream, the CPU port moving
       by 21 and 24 frames.
   * - With the tables complete the engine carries a translated flow at the
       limits of the test setup: 981.9 Mbit/s up and 980.9 down for NAT, 978.9
       up and 875.8 down for NAT over PPPoE (the last limited by the traffic
       generator). A routing test counted 236,050 of 236,419 frames forwarded by
       the engine, none discarded, 369 to the CPU.
     - M
     - A throughput test; a routing test.
   * - The first frames of a flow are the CPU's: netfilter offers the flow on
       its second packet and the driver's entries land some milliseconds
       later, so of 300 frames sent 0.3 ms apart 12 to 57 went through the
       CPU.
     - M
     - The CPU port's counter against the far-side count (a PPPoE test).

The address-table entries the engine uses
=========================================

The engine takes the destination MAC and the egress port of a routed frame from
the address table, and the switch acts on the engine's verdict only for rows
that carry two flags. This section states what the engine needs of the table.
How the driver places the entries, through the hash of the table engine, is on
the *bridging* page ("The layer-2 side of routing"), which also has the steps of
``rtl9602c_l2_l3lookup_cpu()`` and ``rtl9602c_l2_nexthop_place()``.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_FDB_W1_ROUTER_MAC`` (word 1 bit 29, entry bit 61) is not what
       admits frames to the engine: the engine takes the frames for this
       device's MACs with no entry for them in the address table. What the bit
       does is not known. The driver sets it on the entries behind the CPU port
       when the engine comes up, and ``rtl9602c_port_fdb_add()`` sets it on
       every static CPU-port entry that it adds while ``l34_up``.
     - M
     - Booted straight into router mode, a kernel that places no entry for the
       stick's own MAC had none in the *bridging* page's ``fdb`` view, and a
       routing test found its flow in the switch with 20 of 20,000 datagrams
       through the CPU.
   * - Turning the engine on calls ``rtl9602c_l2_l3lookup_cpu(p, true)`` once,
       from the bring-up: learning off on the CPU port, the learned entries
       behind it deleted, and the router bit set on the entries behind the CPU
       port. The reasons, measured: the switch learns this device's own MAC from
       the frames the CPU sends, as a dynamic entry without the flag that is
       rewritten continuously and wins the lookup; and an existing entry cannot
       be changed in place, since a write by index is discarded and an add by
       MAC places a second copy.
     - M
     - Address-table read-backs showing the learned duplicate and the second
       copy: three slots for one address (see the *bridging* page).
   * - DSA does not install this device's own addresses on the CPU port on this
       part: the switch has one VLAN-filtering setting for all ports, so DSA
       does no unicast filtering on the host's behalf. Nothing puts the
       addresses there unless the driver does.
     - M
     - The address table read on a freshly booted board: two learned entries,
       none for this device.
   * - ``RTL9602C_FDB_W2_NEXTHOP`` (word 2 bit 9, entry bit 73) marks an
       address-table row as usable as a next hop's destination. Without it every
       routed frame was discarded at the ingress port (287,400 discards, nothing
       out of the PON port, forwarding reason 83, empty destination mask); with
       it 289,581 frames left the PON port. The bit takes only when the entry is
       placed by MAC (hashed); the same word written by index reads back without
       it.
     - M
     - The per-port counters before and after (``ethtool -S``:
       ``rx_forward_discards`` on the ingress port and ``tx_unicast`` on the PON
       port).
   * - ``rtl9602c_l2_nexthop_place()`` places the entry that a next hop names
       and returns its slot, which becomes the next hop's
       ``RTL9602C_NEXTHOP_L2_IDX``. It deletes every way that holds the key,
       places the entry by MAC with ``RTL9602C_FDB_W2_NEXTHOP`` set, and returns
       ``-ENOSPC`` for a full bucket. The VID is the caller's: the L3-interface
       row's VID, which is the routed frame's destination VLAN, and not the
       port's PVID. ``RTL9602C_L2_VID_PVID`` (``0xffff``) would ask for the
       PVID; no caller uses it.
     - T
     - A routing test, and a PPPoE test, whose WAN has no ARP neighbour and
       files the concentrator's MAC under VID 0.
   * - The first time a port becomes a next hop's egress it is recorded in
       ``l34_nh_ports`` and every other user port's isolation mask is opened to
       it; ``rtl9602c_bridge_refresh()`` applies the opening again while
       ``l34_up``, so a bridge change cannot undo it. Without it every routed
       frame was discarded at the ingress port (305,000 discards, nothing out).
     - M
     - A cold boot with the isolation left closed and then opened: the ingress
       port's ``rx_forward_discards`` and the frames out.
   * - That opening also let flooded frames cross between the routed ports
       (2,000 broadcasts a second, all of them, both ways), so flooding is
       honoured only on a port with a bridge peer (see the *bridging* page);
       routed ports then pass 0 of 2,000.
     - M
     - Broadcasts offered to the routed ports before and after; the flooding
       check of a routing test.
   * - That a next hop's address-table row has to be filed under the destination
       VLAN is inferred; only VID 0 on both sides has been measured working.
     - I
     - A VLAN device as the WAN interface, its next hop filed under its VID and
       under another VID in turn, with a routing test, would settle it.

The address-table row placed for a next hop is not removed when the neighbour
goes; it stays, with ``RTL9602C_FDB_W2_NEXTHOP`` set, until the table is
emptied (see "Known limitations").

The notifiers that fill the tables
==================================

Three kernel notifiers fill the tables, and all of them wait for ``l34_up``. The
address notifier writes the L3-interface and external-address rows, the
neighbour notifier the next-hop and neighbour-CAM rows and the address-table
entry that a next hop names, and the route notifier the route rows. The
neighbour and route notifiers can be called where a table write, which sleeps,
is not allowed, so each copies out what it needs and queues an event, and a work
item applies the queue under ``flow_lock``.

The address notifier
--------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The address notifier, ``rtl9602c_l34_inetaddr_event()``, handles IPv4
       ``NETDEV_UP`` and ``NETDEV_DOWN`` while ``l34_up``. First the routed GEM
       stream is claimed or given back (see the rows below); then only a netdev
       that is a user port of this switch, or whose ``ndo_get_iflink`` names one
       (a GEM interface, a VLAN device), is handled. It keeps one L3-interface
       row per netdev, by ifindex, in the first free of the 16 rows. ``UP``
       writes the netdev's MAC, VLAN (see "L3 interface (selector 3)"), the
       address and the MTU, and puts the address in the external-address table
       with the uplink's next hop (see the row below), where netfilter's
       translated source will be looked up; a second address replaces the
       first in the row. ``DOWN`` gives back the
       address's external-address row, then rewrites the interface row with an
       address that remains, or clears it. Then the route table is committed
       again, since a directly attached route names the row. A netdevice
       notifier writes the row again, with the device's first address, when
       its MAC address or its MTU changes (``NETDEV_CHANGEADDR``,
       ``NETDEV_CHANGEMTU``).
     - T, I
     - A routing test, and the route rows following ``ip route``: ``ip address
       add`` and ``ip address del`` change the ``l3nat`` L3-interface row of the
       interface. That ``ip address del`` frees the external-address row is read
       from the code; the ``l3nat`` view after it would show the row empty.
   * - The external-address rows of the addresses and of the flows name the
       uplink's next hop: the next-hop row of the default route's gateway,
       once its neighbour holds one, else row 0
       (``rtl9602c_l34_uplink_nh()``). Each commit of the route table, after
       every route, neighbour and address event, points them again when it
       changed (``rtl9602c_l34_extip_repoint()``); a PPPoE WAN's row keeps its
       own next hop.
     - T
     - The router-mode cases of ``tests/device``: a static WAN after a PPPoE
       session, whose concentrator the translated frames went to before.
   * - A frame that the switch forwards to the PON port leaves only if a
       classifier entry gave it a GEM stream; the PON MAC drops one without,
       while the switch's own counters count it as sent.
     - M
     - The PON port's egress counter showed 100 frames out while the far side
       saw the 10 that the CPU sent; a routing test and a PPPoE test assert the
       classifier entry (``cf``, see the *acl-qos* page).
   * - The first address on a routed GEM interface (or on a ppp device running
       over one) claims that GEM flow as the routed-frame stream:
       ``rtl9602c_cf_sid_refresh()`` gives each subscriber port outside the GEM
       bridge a classifier entry that assigns the flow. One routed GEM flow at a
       time; a second is refused with a warning. Each device over the flow
       holds one reference, taken with its first address and given back with
       its last, whatever number of addresses it had; the last reference gives
       the flow back, and removing the GEM port forgets the flow with its
       references.
     - T
     - A routing test and a PPPoE test assert the classifier entry of the
       routed port in ``cf``.

The neighbour notifier
----------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The neighbour notifier, ``rtl9602c_l34_netevent()``, handles
       ``NETEVENT_NEIGH_UPDATE`` for ARP neighbours while ``l34_up``. It can run
       in softirq and the table writes sleep, so the event (address, netdev
       held) is queued and a work item handles it: a neighbour in a
       ``NUD_VALID`` state becomes a next hop, any other is removed (its
       next-hop and CAM rows zeroed). Then the route table is committed again,
       since a gateway route resolves only once its neighbour has a row.
     - T
     - A routing test, and the route rows following ``ip route``.

``rtl9602c_l34_neigh_set()``, for a neighbour that became valid, in order:

1. Require that the neighbour's netdev has an L3-interface row and is, or runs
   over, a switch port. Purpose: the next hop names an interface row and an
   egress port. (T)
2. Read the VID of that L3-interface row back from the hardware. Purpose: the
   address-table entry is filed under the VLAN the routed frame leaves in. (T)
3. Place the neighbour's address-table entry with
   ``rtl9602c_l2_nexthop_place()`` (see "The address-table entries the engine
   uses"). Purpose: the next hop names its slot. (T)
4. Choose the next-hop row: the row that already holds this address, else the
   first free one, from row 0 for a neighbour on the PON port and from row 1
   otherwise. Purpose: the external addresses that the address notifier writes
   send their frames to next hop 0, so row 0 must hold the uplink's gateway. (M)
5. Write the next-hop row (Ethernet, the L3-interface row, the address-table
   slot) and the CAM row at the same index (address, slot, valid). Purpose: the
   route rows resolve a gateway against the address that the driver records for
   each next-hop row; whether the engine needs the CAM row is not established
   (see "IPv4 neighbour CAM (selector 13)"). (T)

Steps 1 to 3 and 5 are checked by a routing test. Step 4 is checked by the
occupant of next-hop row 0 (see "External address (selector 4)").

The route notifier
------------------

The route notifier is registered for the initial network namespace and follows
its main routing table. What it records goes into a shadow of up to 32 routes,
and the hardware table of 16 rows is rebuilt from the shadow on every change.
The encoding of a route into a row:

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Route
     - Row
   * - Directly attached, on a device with an L3-interface row
     - Action 2 with that interface row, and ``RTL9602C_ROUTE_INTERNAL`` when
       the egress port is a switch port other than the PON port.
   * - Through a gateway that a next-hop row holds
     - Action 3 with that next-hop row.
   * - Through a gateway that no next-hop row holds yet
     - Action 0 (the CPU) until the neighbour resolves.
   * - On a device with no L3-interface row
     - Action 0.
   * - Whose nexthop is dead or has no carrier
     - Action 0 until ``FIB_EVENT_NH_ADD`` reports the nexthop usable.
   * - Blackhole
     - Action 1 (drop).
   * - Unreachable, prohibit, multipath, nexthop object, IPv6 gateway or DSCP
     - Action 0.
   * - Prefix length 0
     - Two rows, 0.0.0.0/1 and 128.0.0.0/1, with the encoding of the route.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The route notifier, ``rtl9602c_l34_fib_event()``, handles IPv4 only,
       while ``l34_up``: ``FIB_EVENT_ENTRY_REPLACE``, ``_APPEND`` and ``_DEL``
       in the main table, and ``FIB_EVENT_NH_ADD`` and ``_NH_DEL``. What a row
       needs is copied out in the callback, which can run under
       ``rcu_read_lock()``, and a work item applies it. A unicast route with one
       IPv4 or direct nexthop, no nexthop object and no DSCP becomes a full row;
       a blackhole route a drop row; every other route (unreachable, prohibit,
       multipath, nexthop object, IPv6 gateway, DSCP) a CPU row, because an
       absent row would not shadow a shorter prefix.
     - T
     - A routing test: the route rows for a host route, a connected subnet and a
       default route follow ``ip route``.
   * - A route whose nexthop is dead or has no carrier keeps its interface, port
       and gateway, is written as a CPU row, and becomes a full row when
       ``FIB_EVENT_NH_ADD`` reports the nexthop usable. Without this, a
       connected route learnt before the SerDes link came up stayed a CPU row
       (second word ``0x0000003d``) and every flow was accepted and then carried
       by the CPU; with it, five fresh boots read ``0x000001bd`` and
       ``0x000003bd``.
     - M
     - Five fresh boots, the ``l3nat`` route rows; the PPPoE test asserts that
       no LAN row has action 0.
   * - The commit, ``rtl9602c_l34_route_commit()``: the whole table is rebuilt
       from a shadow of up to 32 routes on every change, longest prefix first; a
       /0 becomes 0.0.0.0/1 and 128.0.0.0/1 (two rows); a directly attached
       route is action 2 with its netdev's interface row; a gateway route is
       action 3 with the next-hop row holding the gateway's address, or a CPU
       row until that neighbour resolves; rows whose words did not change are
       not written.
     - M
     - A /32, a /30 and a /24 at rows 0, 1 and 2; a default route as two rows
       below the /30, gone with the route; the routes of a routing test.
   * - If the routes need more than 16 rows, or the namespace has FIB rules, the
       table is written empty (every destination to the CPU) and a warning is
       logged once; the table comes back when the routes fit again. If the
       32-route shadow overflows, hardware routing stays off until the driver is
       reloaded, and the warning names the route that was not recorded.
     - M
     - 19 rows needed gave 0 rows and recovered when the routes went; 40 routes
       gave 0 rows and stayed so. The branch for FIB rules is not exercised: the
       board's kernel has no multiple routing tables.

The shadows
-----------

The driver keeps a shadow of each table it fills, to find a free row, to find
the row that holds an address, and to skip an unchanged write. The hardware is
read for the NAPT ways and the external addresses, because entries survive a
warm restart.

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Shadow
     - What it mirrors
   * - ``l34_route[32]``
     - Up to 32 routes of the main table (``struct rtl9602c_l34_route``), with
       the software flags ``RTL9602C_L34_RT_CPU`` (cannot be represented, Linux
       decides) and ``RTL9602C_L34_RT_DROP`` (blackhole);
       ``l34_route_hw[16][2]`` holds the rows last written.
   * - ``l34_netif_ifindex[16]``
     - The netdev of each L3-interface row, by ifindex; 0 for a free row and -1
       for a row that a PPPoE WAN holds.
   * - ``l34_extip[8]``
     - Each external-address row's address, 0 when free.
   * - ``l34_nh_addr[16]``
     - The neighbour address of each next-hop row, 0 for a free row. A row held
       for a PPPoE WAN has the address ``RTL9602C_NH_HELD_BY_PPPOE``
       (255.255.255.255), which no neighbour can have.
   * - ``l34_pppoe[8]``
     - The PPPoE WANs by row; row 0 is the Ethernet uplink's
       (``struct rtl9602c_pppoe_wan``).
   * - ``l34_us_flow``, ``l34_us_refs``
     - The GEM flow that routed frames leave in, -1 for none, and the references
       on it that the addresses take.
   * - ``l34_nh_ports``
     - The ports a next hop lives behind, which every other user port may reach
       (see "The address-table entries the engine uses").

An external-address row is freed when its address is removed, so an interface
that takes many addresses over time, a DHCP client for instance, does not use
the eight rows up.

The flowtable offload
=====================

netfilter's flowtable offers each direction of a flow to the driver as a flower
rule, once to each device of the flowtable. DSA offers ``TC_SETUP_FT`` to the
switch driver before the conduit, and the driver binds one flow block for each
user port. The engine carries the source-NAT direction of a flow and translates
the replies back from its entries, so the driver programs the upstream rule
only. It declines every other rule with ``-EOPNOTSUPP``, and the flow then stays
in software: returning 0 would make netfilter stop forwarding the flow there.
What the driver takes from a rule:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - In the flower rule
     - In the engine
   * - The IPv4 source address and TCP or UDP source port of the match
     - The LAN host's address and port: ``RTL9602C_NAPT_IN_INTERNAL_ADDR`` and
       ``RTL9602C_NAPT_IN_INTERNAL_PORT``, and the inputs A_i and P_i of the
       outbound hash.
   * - The destination address and port of the match
     - The remote end: the inputs A_r and P_r of the outbound hash, and the
       digest in ``RTL9602C_NAPT_IN_REMOTE_HASH``.
   * - The protocol
     - ``RTL9602C_NAPT_IN_TCP`` and the input t of both hashes.
   * - The mangle of the IPv4 source address
     - The external address (an external-address row) and the input A_e of the
       inbound hash.
   * - The mangle of the source port
     - ``RTL9602C_NAPT_IN_EXTERNAL_PORT`` and the input P_e of the inbound hash.
   * - The redirect device and, for PPPoE, the push of a session and the
       Ethernet addresses
     - The egress: whether the GEM interface may carry a routed flow, and the
       rows of a PPPoE WAN.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - DSA offers ``TC_SETUP_FT`` to the switch driver before the conduit
       (``dsa_user_setup_ft_block()`` in ``net/dsa/user.c`` of this tree), and
       the driver binds one flow block for each user port
       (``rtl9602c_port_setup_tc()``, ``rtl9602c_setup_ft_block()``), keyed on
       the ``dsa_port`` and reference-counted across binds. A GEM interface
       passes ``TC_SETUP_FT`` to the PON port below it. Per-flow commands arrive
       as ``FLOW_CLS_REPLACE``, ``FLOW_CLS_DESTROY`` and ``FLOW_CLS_STATS``.
     - T
     - A routing test: the connection tracking table shows ``[HW_OFFLOAD]``.
   * - netfilter unbinds a flowtable's blocks before it tears its flows down, so
       ``FLOW_CLS_DESTROY`` never arrives for flows still in hardware when a
       ruleset is flushed. The block's release callback frees every flow that no
       remaining block owns (a flow is marked with the ports whose blocks
       offered it).
     - M
     - Fourteen rounds of load, offer and flush, each taking slot 0 again and
       logging ``released with it`` in the ``<flow>`` debug line; the flush of a
       PPPoE test.
   * - netfilter offers the same flow once per device in the flowtable (one
       cookie, two blocks). A ``REPLACE`` for a cookie already held reuses its
       slot and its bucket ways (its old entries zeroed first) and adds the
       offering port to its owners; ``DESTROY`` frees every slot with the
       cookie. Without the reuse four flows filled the eight slots and later
       offers failed with ``-ENOSPC``.
     - M
     - The ``<flow>`` debug line ``not programmed: -28`` on a board that had
       been offloading.
   * - A ``REPLACE`` is programmed only if all of these hold:

       - the rule has basic, IPv4-address and port keys;
       - the engine is up;
       - the protocol is TCP or UDP;
       - a source-address rewrite is present, which makes it a source-NAT flow
         and excludes the reply direction's rule and plain routing;
       - for a PPPoE push, the egress device and both Ethernet addresses are
         present and no VLAN is pushed;
       - a GEM egress has the routed-frame stream and its upstream is ready;
       - the external address is an external-address row (Ethernet) or can be
         given one (PPPoE);
       - a slot (of 8), an outbound way and an inbound way are free, and the
         inbound-bucket rule of "The NAPT hashes" does not apply.

       Otherwise it is declined. IPv6 flows are declined (``not IPv4``) and stay
       on the software fast path.
     - T
     - A routing test; an IPv6 flow logged ``not IPv4`` and stayed
       ``[OFFLOAD]``.
   * - The translated source arrives as two mangle actions: the IPv4 source at
       ``offsetof(struct iphdr, saddr)`` (12) with the new address as the value,
       and the TCP or UDP source port at offset 0 of the transport header with
       mask ``0x0000ffff`` and the port in the high half of the value (value >>
       16). On this big-endian CPU both arrive in the order the tables use. A
       rule that rewrites only the address keeps the original source port as the
       external port.
     - M
     - The driver's action dump in the ``<flow>`` debug lines: ``MANGLE htype 2
       offset 12 mask 00000000`` and ``MANGLE htype 5 offset 0 mask 0000ffff val
       13880000`` for port 5000.
   * - For a PPPoE flow the two Ethernet addresses arrive as four Ethernet
       mangle actions: offset 0 mask 0 (destination bytes 0 to 3), offset 4 mask
       ``0xffff0000`` (destination bytes 4 and 5, low half), offset 4 mask
       ``0x0000ffff`` (source bytes 0 and 1, high half), offset 8 mask 0 (source
       bytes 2 to 5); the values are bytes in memory order. The session comes
       from ``FLOW_ACTION_PPPOE_PUSH``, present on the upstream direction only,
       and the hardware egress device from ``FLOW_ACTION_REDIRECT``.
     - T
     - A PPPoE test.
   * - Three conditions outside the driver stop netfilter offering flows at all:
       a bridge over the ingress or egress port (0 driver calls and
       ``[OFFLOAD]`` with one standing, 38 calls and ``[HW_OFFLOAD]``
       without), a flowtable naming the bare PON port instead of the GEM
       interface (the CPU cannot send on the bare port, the neighbour never
       resolves and no rule is built), and a host behind the stick that is not
       a valid neighbour (the flow stays ``[OFFLOAD]`` and the CPU forwards it
       until the neighbour is valid: 161,185 of 440,712 frames in one run).
     - M
     - Both conditions set and removed in turn, the calls counted in the
       ``<flow>`` debug lines and the state read in the connection tracking
       table.

``rtl9602c_l34_flow_add()``, for a rule that passed the conditions, in order:

1. Compute both buckets, the outbound from the five-tuple and the inbound from
   the translated address and port (see "The NAPT hashes"), and take
   ``flow_lock``. Purpose: the ways are searched in these buckets only. (T)
2. Find the external-address row of the translated address; for PPPoE, take one
   and set up the WAN (see "The PPPoE encapsulation path"). Purpose: the inbound
   entry names it. (T)
3. Take the flow's slot by its cookie, or a free one. Purpose: netfilter offers
   a flow once per device (see the table above). (T)
4. Find a free outbound way, reading ``RTL9602C_NAPT_VALID`` from the hardware.
   Purpose: entries survive a warm restart, so the driver's records do not say
   which ways are free. (T)
5. Find a free inbound way, reading the hardware, and check that no entry of the
   bucket shares the external port, the external-address row and the protocol.
   Purpose: the engine could not tell the replies to two such flows apart. (T)
6. Write the inbound entry: the internal address and port, the remote digest,
   the external-address row and port, the protocol and
   ``RTL9602C_NAPT_IN_VALID_ACTIVE``. (T)
7. Write the outbound entry, pointing at it. Purpose: the inbound entry goes
   first because the engine reads the tables continuously, and a pointer written
   first could name an entry that still holds another flow. (T)
8. Take the WAN reference and record the slot. Purpose: the driver's record of
   the flow for statistics, removal and the release of a block. (T)

The steps are checked by a routing test, in which the ``<flow>`` debug line
names the slot, the outbound bucket and way, the inbound entry and bucket and
the remote digest.

Removal, for ``FLOW_CLS_DESTROY`` and for the release of a block, is
``rtl9602c_l34_flow_free()``: the outbound entry is zeroed first, then the
inbound entry; the flow's hit bit is consumed; and the PPPoE WAN's reference is
dropped, its rows staying for the next flow. It is checked by the flush of a
PPPoE test.

The driver keeps eight flow slots, ``l34_flow[8]``, a limit of the driver and
not of the engine. Each slot records the cookie, the outbound and inbound
entries, the PPPoE WAN and the owning ports. A PPPoE flow that also pushes a
VLAN is declined without having been measured.

The PPPoE encapsulation path
============================

A PPPoE WAN is a PPP interface that runs over a switch port or a GEM interface.
The engine only encapsulates: a session is a 16-bit ID in a row, and discovery,
LCP, authentication and IPCP run in software on the CPU. PPP has no ARP
neighbour, so the rows of a WAN are written from the flow rule that first uses
it.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The engine does PPPoE encapsulation only: a session is a 16-bit ID in a
       row, and discovery, LCP, authentication and IPCP run in software on the
       CPU.
     - M
     - The table measurement of "PPPoE session (selector 1)"; a session set up
       by the PPPoE client, the frames encapsulated by the engine only
       afterwards.
   * - A PPPoE WAN takes one index w (1 to 7) in the L3-interface, next-hop and
       PPPoE tables, and points its external-address row at it; row 0 stays the
       Ethernet uplink's. The five writes of steps 2 to 6 below are all made
       from the flow rule. Read back: PPPoE ``0x00000001``, next hop
       ``0x00023167``, external address third word ``0x00000019``.
     - T
     - A PPPoE test; the ``l3nat`` rows of the WAN.
   * - The row MTU is the egress device's MTU less 8: the PPPoE header is 6
       octets and the PPP protocol field 2, so the MRU over a 1500-octet
       Ethernet payload is at most 1492.
     - P
     - RFC 2516 clause 4 (packet format) and clause 7 (MRU at most 1492).
   * - The encapsulated frames are whole: of 300 frames 300 left the board, 296
       carried by the engine and 4 by the CPU, and all 300 UDP checksums
       verified at the concentrator.
     - M
     - A PPPoE test: the frames counted and their checksums verified at the
       concentrator.
   * - The engine translates and decapsulates the replies from the inbound entry
       that the upstream rule wrote; netfilter's reply-direction rule carries no
       source translation and is declined, with nothing left to program.
     - M
     - A throughput test, NAT over PPPoE downstream, 875.8 Mbit/s with the CPU
       port moving by 24 frames.

``rtl9602c_l34_wan_get()``, for a flow that leaves over PPPoE, in order:

1. Find the WAN that already has the flow's external address. If its session,
   port and concentrator agree, use it as it is. If they differ, write its rows
   again in place (steps 2 to 6). If there is none, take a free index w,
   recycling a WAN that has no flows when none is free, and write its rows
   (steps 2 to 6). Purpose: one index w ties the rows together. (T)
2. Write PPPoE row w: the session. (T)
3. Place an address-table entry for the concentrator's MAC on the egress port,
   VID 0, with ``RTL9602C_FDB_W2_NEXTHOP`` set. Purpose: PPP has no ARP
   neighbour that would put it there. (T)
4. Write L3-interface row w: the egress MAC (from the source-MAC mangle), VID 0,
   the PPP address, and the egress device's MTU less 8. (T)
5. Write next-hop row w: PPPoE, interface w, session row w and the address-table
   slot. (T)
6. Point the external-address row of the WAN at next hop w. (T)

If a step fails, everything written is undone and the flow is declined. The
steps are checked by a PPPoE test.

A WAN record is keyed by the PPP interface's address. A new session on a known
address rewrites the rows in place. A WAN with no flows is kept, and recycled
only when a new address needs its index; ``-ENOSPC`` when all seven carry flows.
On release the external-address row is freed if this path allocated it, and
otherwise restored with the uplink's next hop.

Hit bitmaps, flow statistics and ageing
=======================================

The engine sets a bit in a hit bitmap when a frame uses an entry. The NAPT hit
bitmap has one bit per outbound entry, set by a frame of the flow in either
direction, and reading a word of it clears all 32 bits of the word. netfilter
tears an offloaded flow down 30 s after its last reported use, so the driver
answers the statistics command of a flow from these bits. The other bitmaps are
only printed.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_NAPT_HIT`` (``0x800400``, ``RTL9602C_NAPT_HIT_WORDS`` = 128
       words): one bit per outbound NAPT entry (entry n is bit n mod 32 of word
       n/32), set by a frame of the flow in either direction, and cleared by
       reading its word, all 32 bits of it. The predicted bit of one flow came
       up after upstream frames and again after downstream frames alone, and a
       second read returned zero.
     - M
     - One flow, the word read after each direction.
   * - Because a read clears 32 entries, the driver ORs every word it reads into
       a shadow (``l34_trf``) and takes a flow's bit from the shadow.
     - T
     - A routing test, and the statistics row below.
   * - The ``l3flow`` view reads word 0 of this bitmap, which clears the hit
       bits of outbound entries 0 to 31 (buckets 0 to 7); a flow there can then
       read as unused at netfilter's next statistics query.
     - M
     - It follows from the first row: the view's read is an ordinary read of
       word 0.
   * - ``RTL9602C_PPPOE_HIT`` (``0x800014``): bits 7:0, one per PPPoE row; bit 3
       came up when frames went through PPPoE row 3.
     - M
     - The ``l3flow`` line ``pppoe hit`` during a PPPoE test.
   * - ``RTL9602C_L3_FLOW_HIT(i)`` (``0x800030`` + 4i, 8 words): one bit per
       five-tuple entry; entry 240, word 7 bit 16, came up when that entry
       matched. This bitmap, ``RTL9602C_NEIGH_HIT(i)`` (``0x800050`` + 4i) and
       ``RTL9602C_NEIGH6_HIT(i)`` (``0x800018`` + 4i), 4 words each, read 0 in
       the reset state. The driver only prints them.
     - M
     - A five-tuple entry written by hand and matched; reads in the reset state;
       the ``l3flow`` view.
   * - Flow statistics (``FLOW_CLS_STATS``): the flow's hit bit is taken from
       the shadow; if it is set, ``lastused`` becomes now; if not, ``lastused``
       is left alone, because both blocks of a flow ask in turn and the second
       finds the bit already taken. netfilter tears an offloaded flow down 30 s
       after its last reported use: without the answer a reply stream at 31,000
       frames/s stopped 31 s after the flow was added; with it a reply stream
       ran 54 s untouched and an idle flow went 31 s after its traffic stopped.
     - M
     - A reply stream with and without the answer, watched in the connection
       tracking table.
   * - That the two neighbour bitmaps hold one bit per CAM and IPv6-neighbour
       entry is inferred from their width (neither has been seen set), and
       whether reading the PPPoE, five-tuple or neighbour bitmaps clears them is
       not established.
     - I
     - A neighbour's CAM row hit by traffic, with two reads of the bitmaps,
       would settle both. Nothing in the driver depends on them.
   * - The engine does not remove a NAPT entry by itself (no hardware ageing):
       an entry stays until the driver removes it, as the driver's slot
       records and its free-way scan assume.
     - M
     - A UDP flow kept offloaded with netfilter's UDP flowtable timeout raised
       to 600 s for the test, then idle for 300 s: 2000 datagrams of the flow
       sent after that passed with 25 frames reaching the CPU, as before the
       pause.

The debugfs views
=================

Three views in ``/sys/kernel/debug/rtl9602c/`` show the engine. The tables that
the views print are the small ones whole and the large ones by their first four
rows; no view prints an inbound NAPT entry.

``l3nat`` (mode 0400) prints the auxiliary word, the configuration word and the
tables, and ends with a check of the table port's guard. Its top and its last
line::

    # cat /sys/kernel/debug/rtl9602c/l3nat
    aux ctrl 000000f5
    l3 cfg   64000e20  flow4 1  flow6 1

    flow (table 12, 4 words)
      [0] 00000000 00000000 00000000 00000000
    ...
    guard: flow-routing[9999] -> -22 (want -22)

``l3flow`` (mode 0400) prints the engine's state words and the hit bitmaps::

    # cat /sys/kernel/debug/rtl9602c/l3flow
    global     00000005  engine ON
    l3 cfg     64000e20  flow4 1  flow6 1
    ...

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``l3nat`` (mode 0400) prints the auxiliary word (``aux ctrl``), the
       configuration word with its two flow-lookup enables (``l3 cfg``), then
       for each table a heading ``<name> (table <selector>, <words> words)`` and
       the raw words of its rows, ``[<row>]``: ``flow`` (five-tuple) rows 0 to
       3, ``napt`` (outbound NAPT) 0 to 3, ``neigh`` (neighbour CAM) 0 to 3,
       ``neigh6`` (IPv6 neighbour) 0 to 3, ``route`` 0 to 15, ``l3if`` 0 to 15,
       ``nexthop`` 0 to 15, ``extaddr`` 0 to 7 and ``pppoe`` 0 to 7. A row that
       cannot be read prints ``read error`` and ends its table. The last line,
       ``guard:``, is the check of the table access port (see "The table access
       port"). Its table reads have no side effect.
     - M
     - ``cat /sys/kernel/debug/rtl9602c/l3nat``.
   * - ``l3flow`` (mode 0400) prints the global control word with the engine's
       state (``global``), the configuration word with the flow-lookup enables
       (``l3 cfg``), the PPPoE hit bits (``pppoe hit``), word 0 of the NAPT hit
       bitmap (``napt hit``, which the read clears, see "Hit bitmaps, flow
       statistics and ageing"), the hit bitmaps of the five-tuple (``flow``, 8
       words), IPv6-neighbour (``neighbour``, 4 words) and neighbour (``arp``, 4
       words) tables, the capture control word (``capture``, with ``log mode``,
       ``capture active`` or ``idle``, and ``engine busy`` or ``free``), and the
       14 input words (``in``) and 5 output words (``out``) of the capture, raw.
     - M
     - ``cat /sys/kernel/debug/rtl9602c/l3flow``.
   * - ``l3capture`` (mode 0600): a read prints the capture control word decoded
       (``mode``, ``armed``, ``busy``), then the input words (``in:``) and the
       output words (``out:``) of the capture, raw. A write ``<mode> [<armed>]``
       sets ``RTL9602C_L3_CAPTURE_CTRL_MODE`` and
       ``RTL9602C_L3_CAPTURE_CTRL_EN`` (armed defaults to 1; 0 disarms) and logs
       ``l3capture: mode <m>, capture on|off, control now <word>``. A mode above
       7 or an armed value above 1 is refused with ``-EINVAL``.
     - M
     - ``echo 2 > /sys/kernel/debug/rtl9602c/l3capture``, then ``cat`` it.

The per-frame capture
=====================

The engine can latch, for one frame at a time, its parse of the frame (the
*input descriptor*, 14 words) and its verdict (the *output descriptor*, 5
words). The capture is how the translated verdict, the engine's reason and the
egress interface of a frame are seen, and several measurements on this page use
it. The latch captures bridged frames as well as frames that the engine routes.
Bit b of a descriptor is bit b mod 32 of word b/32, word 0 being the first word
the views print. The views print the words raw; the tables below give the
positions that were matched against known frames and those that were not.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_L3_CAPTURE_CTRL`` (``0x800200``):
       ``RTL9602C_L3_CAPTURE_CTRL_MODE`` (bits 4:2) selects which frames are
       latched, ``RTL9602C_L3_CAPTURE_CTRL_EN`` (bit 1) arms the latch,
       ``RTL9602C_L3_CAPTURE_CTRL_BUSY`` (bit 0) is read-only (an all-ones write
       reads back ``0x1e``). It reads ``0x00000004`` (mode 1, not armed) in the
       reset state, and bring-up does not write it. Unarmed, the latch keeps
       what it last captured.
     - M
     - Writes and read-backs by hand; a read in the reset state; ``cat
       /sys/kernel/debug/rtl9602c/l3capture``.
   * - ``RTL9602C_L3_CAPTURE_IN(i)`` (``0x800204`` + 4i,
       ``RTL9602C_L3_CAPTURE_IN_WORDS`` = 14) hold the engine's parse of a
       frame; ``RTL9602C_L3_CAPTURE_OUT(i)`` (``0x800280`` + 4i,
       ``RTL9602C_L3_CAPTURE_OUT_WORDS`` = 5) hold its verdict. Descriptor bit 0
       is the lowest bit of word 0. The latch captures bridged frames as well as
       frames that the engine routes.
     - M
     - Frames bridged across the switch latched; the field positions of the
       tables below.
   * - The capture is not atomic: words latch separately, and one reading can
       mix two frames (two of five shots held a broadcast in words 0 to 3 while
       the port fields followed the frame under test). A reading has to be
       repeated before anything is concluded from it.
     - M
     - Repeated readings while known frames passed.
   * - Modes, measured on bridged traffic: 2 and 3 latched the unicast test
       frame, 4 and 5 a broadcast, and 0, 1, 6 and 7 nothing; mode 4 latched a
       forwarded frame of an offloaded PPPoE flow. What each mode selects beyond
       that is not established.
     - M
     - 300 frames per mode; mode 4 during a PPPoE test.
   * - That arming the capture or changing its mode leaves forwarding unchanged
       is not established. A sweep of the modes with an incomplete set of tables
       saw frames addressed to this device pass the engine by (CPU reason 0) in
       mode 0 and taken by it in modes 1 to 7.
     - I
     - Writing each mode to ``l3capture`` during a routing test, and comparing
       the CPU port's counter and the discard counters, would settle it.

The input descriptor, 14 words, bits 0 to 447:

.. list-table::
   :header-rows: 1
   :widths: 16 68 16

   * - Bits
     - Field
     - Tag
   * - 47:0
     - The destination MAC.
     - M
   * - 63:48
     - The L4 source port.
     - M
   * - 95:64
     - The source IPv4 address.
     - M
   * - 127:96
     - The destination IPv4 address.
     - M
   * - 239:224
     - The PPPoE session ID.
     - I
   * - 255:240
     - The L4 destination port.
     - M
   * - 269:256
     - The length.
     - I
   * - 281:270
     - The C-VLAN ID.
     - I
   * - 286:282
     - The ingress port (0 for the subscriber port).
     - M
   * - 287
     - C-tag present.
     - I
   * - 295:288
     - The TCP flags.
     - I
   * - 298:296
     - The frame type (6 for UDP).
     - M
   * - 300:299
     - The TTL state.
     - I
   * - 301
     - S-tag present.
     - I
   * - 302
     - PPPoE present.
     - I
   * - 303
     - Parse failure.
     - I
   * - 304
     - UDP without checksum (1 for frames sent without one).
     - M
   * - 305
     - Sent directly by the CPU.
     - I
   * - 306, 307
     - L3 and L4 checksum good (1 on the frames captured).
     - M
   * - 308
     - More fragments.
     - I
   * - 309
     - Non-zero fragment offset.
     - I
   * - 310
     - Bridge only.
     - I
   * - 311
     - From a WAN port (0 for frames from the subscriber port).
     - M
   * - 312
     - Policy route.
     - I
   * - 316:313
     - The policy next hop.
     - I
   * - 413
     - IPv6 extension header.
     - I
   * - 414
     - Source MAC found.
     - M
   * - 425:415
     - The address-table row of the source MAC (the sender's slot).
     - M
   * - 426
     - CFI.
     - I
   * - 427
     - A destination-MAC flag.
     - I

The output descriptor, 5 words:

.. list-table::
   :header-rows: 1
   :widths: 16 68 16

   * - Bits
     - Field
     - Tag
   * - 31:0
     - The translated source address (for a bridged frame, the destination
       address).
     - M
   * - 47:32
     - The translated source port (for a bridged frame, the destination port).
     - M
   * - 63:48
     - The L3 checksum offset.
     - I
   * - 79:64
     - The L4 checksum offset.
     - I
   * - 90:80
     - The address-table row of the next hop.
     - M
   * - 93:91
     - The PPPoE row.
     - M
   * - 95:94
     - The PPPoE action (1 when a header is added).
     - M
   * - 107:96
     - The destination VLAN (the interface row's VID).
     - M
   * - 111:108
     - The egress L3-interface row.
     - M
   * - 112
     - Fragment.
     - I
   * - 115:113
     - The L4 header pointer.
     - I
   * - 116
     - The L4 header pointer is valid.
     - I
   * - 118:117
     - The action (2 forward, 3 to the CPU).
     - M
   * - 119
     - The layer-2 rewrite is done.
     - M
   * - 120
     - The L3 and L4 translation is done.
     - M
   * - 126:121
     - The engine's reason (23 on every translated frame seen).
     - M
   * - 127
     - Internal VLAN.
     - I
   * - 128
     - Binding VLAN translation.
     - I
   * - 129
     - PPPoE keep.
     - I
   * - 130
     - Policy.
     - I
   * - 131, 133:132
     - A downstream output and its index.
     - I

How the positions were established:

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The positions tagged M in the input descriptor matched frames of known
       content: the destination MAC, the two L4 ports and the two IPv4 addresses
       with one field of the five-tuple changed per shot, and the other fields
       with generated frames.
     - M
     - 200 frames for each changed field; a translated flow; captures of
       generated frames.
   * - The positions tagged M in the output descriptor matched known state: the
       translated address and port, the next hop's address-table row, the PPPoE
       row and action, the destination VLAN and the egress L3-interface row
       against the rows that the driver wrote, and the action, the two
       completion bits and the reason against the verdict of each flow.
     - M
     - Captures of Ethernet and PPPoE flows, read against the rows that the
       driver wrote.
   * - The positions tagged I in either descriptor are not verified, and neither
       is that ``RTL9602C_L3_CAPTURE_CTRL_BUSY`` means that the engine is busy;
       it has only been seen at 0.
     - I
     - A capture of a frame that sets each one, read in ``l3capture``, would
       confirm its position.

Known limitations
=================

- Until the default route's gateway has a next-hop row, the external-address
  rows name row 0, which goes to whichever neighbour on the PON port resolves
  first while it is free.
- The route table mirrors the main routing table of the initial network
  namespace in 16 rows, and a /0 takes two of them. When the routes need more
  rows, or the namespace has FIB rules, the table is written empty and every
  destination goes to the CPU, with one warning; it is written again when the
  routes fit. When more than 32 routes exist, hardware routing stays off until
  the driver is reloaded.
- The address-table entries on which the engine depends have limitations of
  their own, described on the *bridging* page.

Open questions
==============

These are the facts tagged **I** in the tables above, each with what would
settle it. The first list holds the assumptions that the driver depends on: the
driver is correct on this board only if they hold, and a board test must settle
them. The second list holds the questions to which the code is indifferent: it
works whichever way they resolve.

Assumptions the driver depends on
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Assumption
     - What would settle it
   * - Every reset strobe has finished by the first table access that follows it
       ("Reset strobes, table contents and the clear at setup"). The driver does
       not wait for the strobes.
     - The boot log line ``L3: reset 13 of 13 table engine(s), engine ON
       (control ..., reset ...)`` reading ``reset 00000000`` at every boot.
   * - The engine searches all four ways of an outbound bucket, so that a second
       flow in one bucket is carried from way 1 ("Outbound NAPT (selector 10)").
     - Two flows forced into one outbound bucket by the choice of source ports,
       both carried by the engine.
   * - The remote digest function is right, and the outbound lookup does not
       compare the field in a way that the function gets wrong ("The NAPT
       hashes").
     - A second live entry with a different remote end, read back; two flows in
       one outbound bucket.
   * - The engine tells apart two inbound entries of one bucket by their
       external port ("The NAPT hashes").
     - Two flows whose external ports fall in one inbound bucket, replies to
       both, each counted at its own LAN host.
   * - Arming the per-frame capture or changing its mode leaves forwarding
       unchanged ("The per-frame capture"). The safety of writing to
       ``l3capture`` rests on it.
     - Each mode written to ``l3capture`` during a routing test, comparing the
       CPU port's counter and the discard counters.

Questions the code does not depend on
-------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether the value 2 of the interface lookup field selects matching on the
       destination MAC ("Global enable and configuration").
     - A row whose MAC differs from the frames' destination address, other
       fields equal, showing whether the frames still match it.
   * - Whether the binding-action word is needed ("Global enable and
       configuration"; "Bring-up sequence" step 5).
     - Omitting the write and repeating the routing test and the PPPoE test.
   * - Whether the auxiliary word ``0xf5`` is needed, and what its bits enable
       ("Global enable and configuration"; step 8).
     - Omitting the write and repeating the routing test and the PPPoE test.
   * - Whether the counter-control bit enables the per-L3-interface counters and
       nothing else ("Global enable and configuration"; step 4).
     - Reading the per-interface counters around a flow, with the bit set and
       clear.
   * - Whether an index at or beyond a table's last entry wedges the table port
       as an unlisted selector does ("The table access port"). The driver never
       issues one.
     - One such access on a board that can be power-cycled.
   * - Whether one write command commits all five write-data registers whatever
       the table's width ("The table access port"). The driver pads the
       registers.
     - Writing a one-word table after a three-word write without the padding,
       and reading both back.
   * - The word counts of the IPv6-route, binding and WAN-type tables ("Tables,
       selectors and geometry"). Only the clear at setup depends on them.
     - A walking one over one entry of each.
   * - Which table each reset strobe resets, and whether the strobes are needed
       before the enable ("Reset strobes, table contents and the clear at
       setup").
     - Bringing the engine up without the strobes and repeating the routing
       test.
   * - Whether the order of the strobes matters ("Bring-up sequence" step 3).
     - Bringing the engine up with the strobes in another order.
   * - Whether route action 1 drops in hardware ("Route (selector 0)").
     - A blackhole route over a prefix that carries an offloaded flow's traffic,
       with the drop counters.
   * - What the engine does with ``RTL9602C_ROUTE_INTERNAL`` ("Route (selector
       0)").
     - Clearing it on the LAN row and repeating the routing test.
   * - Whether the engine takes the longest matching prefix or the first valid
       route row ("Route (selector 0)").
     - A /24 row placed after a /1 that covers it, with a flow into the /24.
   * - Whether the neighbour CAM is content-addressed, what the engine uses it
       for, and whether its rows are needed ("IPv4 neighbour CAM (selector
       13)").
     - Zeroing the CAM rows and repeating the routing test.
   * - What the router bit on this device's own addresses does, since it is
       not what admits frames to the engine ("The address-table entries the
       engine uses"). The driver sets the bit whichever way this resolves.
     - A frame to this device's address in a VLAN in independent learning,
       with and without the bit, followed in the frame capture.
   * - Whether a next hop's address-table row has to be filed under the
       destination VLAN ("The address-table entries the engine uses"). The
       driver files it there either way.
     - A VLAN device as the WAN interface, the next hop filed under its VID and
       under another VID in turn, with a routing test.
   * - Whether the neighbour hit bitmaps hold one bit per entry, and which hit
       bitmaps clear on read ("Hit bitmaps, flow statistics and ageing"). The
       driver only prints them.
     - A neighbour's CAM row hit by traffic, with two reads of the bitmaps.
   * - The positions of the input and output descriptor fields tagged I, and
       what ``RTL9602C_L3_CAPTURE_CTRL_BUSY`` means ("The per-frame capture").
       The views print the words raw.
     - A capture of a frame that sets each field, read in ``l3capture``.
