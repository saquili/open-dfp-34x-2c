.. SPDX-License-Identifier: GPL-2.0-only

============================================================
Realtek RTL9602C switch: ACL, classifier, QoS and protection
============================================================

Overview
========

Besides forwarding frames, the switch core of the RTL9602C judges, marks,
counts, queues and paces them. This page describes those blocks and how the
driver sets each of them up: the ACL and the parser's field selectors; the
classifier, which translates VLANs and gives frames their GEM stream; the
sixteen shared meters and the storm control that uses them; the ingress
policer of each port; the malformed-frame (DoS) checks and the loop detection;
the choice of a frame's internal priority and the DCB maps; the egress queues,
the scheduler and the shaper of each port; the buffer page accounting and the
flow-control thresholds of the PON port; and the two instruments that capture
a frame, the header capture and the egress descriptor capture. The address
table, the VLANs and flooding, which decide where a frame goes, are on the
*bridging* page.

The other pages of the port are named by short names on this page:

- *main*, Documentation/networking/dsa/rtl9602c.rst: the ports, the DSA
  integration, the switch interrupt, the order of setup and a code map of the
  whole driver.
- *bridging*, Documentation/networking/dsa/rtl9602c-bridging.rst: the indirect
  table engine, the address table, flooding, VLANs, and the VLAN translation
  and the GEM stream entries as the bridge uses them.
- *l3-nat*, Documentation/networking/dsa/rtl9602c-l3-nat.rst: the L3/NAT engine
  and the flowtable offload, whose routed flows use the classifier's stream
  entries.
- *ptp*, Documentation/networking/dsa/rtl9602c-ptp.rst: the PTP clock and the
  timestamps, which the egress descriptor capture shows.
- *devlink*, Documentation/networking/devlink/rtl9602c.rst: the devlink
  parameters and the region as the user sees them. This page describes the
  hardware behind those that belong to its blocks and names each one; it does
  not repeat their table.
- *CPU port*,
  Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst: the
  CPU port's DMA engine, which carries the priority and the PON flow of a frame
  that the CPU sends.
- *PON*, Documentation/networking/device_drivers/pon/rtl9602c.rst: the PON MAC
  and its GEM flows.

The blocks
----------

The blocks down to the priority selection act on frames as they enter a port
(loop detection sends probes of its own as well). The queues, the scheduler and
the shaper work on the egress side of a port, and the last rows are accounting,
thresholds and instruments.

.. list-table::
   :header-rows: 1
   :widths: 20 24 38 18

   * - Block
     - Acts on
     - Set through
     - Described under
   * - ACL
     - Frames entering a port.
     - A ``tc`` flower rule with ``drop`` or ``trap``.
     - "The ACL"
   * - Classifier
     - Frames entering a port, by direction.
     - A ``tc`` flower rule with ``vlan modify``; the stream entries that the
       driver writes itself.
     - "The classifier"
   * - Storm control
     - Seven classes of frames entering a port.
     - A ``tc`` flower rule with ``police pkts_rate`` for broadcast, multicast
       and ARP; devlink parameters for the other four classes.
     - "Meters", "Storm control"
   * - Ingress policer
     - All frames entering a port.
     - A ``tc`` matchall rule with ``police rate``.
     - "The ingress policer"
   * - Malformed-frame checks
     - Frames entering a port.
     - The devlink parameters ``dos_*``.
     - "Malformed-frame (DoS) checks"
   * - Loop detection
     - The probes that ports send and receive.
     - The devlink parameters ``loop_detect*``.
     - "Loop detection"
   * - Priority selection
     - The internal priority of each frame.
     - ``dcb app`` and ``dcb apptrust``.
     - "Priority selection and the DCB maps"
   * - Queues and scheduler
     - The eight egress queues of a port.
     - A ``tc`` ets qdisc.
     - "Egress queues and the scheduler"
   * - Egress shaper
     - The egress rate of a port.
     - A ``tc`` tbf qdisc.
     - "The egress shaper"
   * - Buffer pages, flow control
     - The pages that each queue holds; the pause thresholds of the PON port.
     - The debugfs view ``qdepth``; none.
     - "Buffer pages and qdepth", "Flow-control thresholds"
   * - Header capture
     - One frame, as parsed and as decided.
     - The devlink parameters ``pktdbg_*`` and the region ``frame_capture``.
     - "Frame capture"
   * - Egress descriptor capture
     - The last descriptor that left by a port.
     - The debugfs view ``hsd``.
     - "Egress descriptor capture"

How the driver reaches the blocks
---------------------------------

Four things carry the function:

- A flower rule on the ingress of a user port goes to one block, by what it
  matches and what it does. One ``drop`` or ``trap`` action makes an ACL rule.
  A destination address of all ones or of the group bit alone, or an exact ARP
  EtherType with no VLAN match, together with one ``police`` action, makes a
  storm-control rule. A VLAN match with one ``vlan modify`` action makes a
  classifier entry (see "The ACL", "Storm control" and "The classifier").
- A matchall rule with ``police`` is the ingress policer of the port, a tbf or
  ets qdisc sets the shaper or the scheduler, and DCB sets the priority maps.
  DSA hands all of these to the driver as calls of ``struct dsa_switch_ops``.
- The blocks that ``tc`` and DCB have no words for are reached through devlink
  parameters and a region: the DoS checks, the four storm-control classes that
  a flower rule cannot describe, loop detection and the frame capture.
- The driver keeps the bookkeeping of the rules and qdiscs that it installed
  and nothing else; the hardware holds the state. A warm reboot and a driver
  reload leave the switch as it is, so setup writes some of the blocks to a
  known state and leaves the others as they were (see "Reach and state" and
  "Known limitations").

Operations and files
--------------------

The driver implements these operations of ``struct dsa_switch_ops`` for the
function; every name is prefixed ``rtl9602c_``.

.. list-table::
   :header-rows: 1
   :widths: 46 54

   * - Operations
     - Described under
   * - ``cls_flower_add``, ``cls_flower_del``
     - "The ACL", "Storm control" and "The classifier".
   * - ``port_setup_tc`` (tbf and ets; its flowtable offload is the *l3-nat*
       page's)
     - "Egress queues and the scheduler" and "The egress shaper".
   * - ``port_policer_add``, ``port_policer_del``
     - "The ingress policer".
   * - ``port_get_default_prio``, ``port_set_default_prio``,
       ``port_get_dscp_prio``, ``port_add_dscp_prio``,
       ``port_del_dscp_prio``, ``port_set_apptrust``, ``port_get_apptrust``
     - "Priority selection and the DCB maps".
   * - ``devlink_param_get``, ``devlink_param_set``
     - "Malformed-frame (DoS) checks", "Storm control", "Loop detection" and
       "Frame capture".
   * - ``setup``
     - "Setup for these blocks".

The files that hold this page:

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - File
     - What of this page it holds
   * - ``drivers/net/dsa/realtek/rtl9602c-offload.c``
     - The ACL, the classifier entries, the meters, storm control, the ingress
       policer, the scheduler and the shaper, the priority maps, and the debugfs
       views ``cf``, ``qos``, ``qdepth`` and ``hsd``. The L3/NAT engine of the
       same file is the *l3-nat* page's, and its views ``rma``, ``svlan`` and
       ``l2learn`` are the *bridging* page's.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - The steps of ``rtl9602c_setup()`` that reset or arm these blocks, the
       parts of the interrupt handler for the meters, the ACL and loop
       detection, the loop detection's magic, the devlink parameters and the
       ``frame_capture`` region.
   * - ``drivers/net/dsa/realtek/rtl9602c-ponmac.c``
     - ``rtl9602c_pon_fc_thresholds()``, the flow-control thresholds of the PON
       port.
   * - ``drivers/net/dsa/realtek/rtl9602c.h``
     - The registers and fields of these blocks, the tables of storm classes
       and devlink parameters, and the driver's state for them (``acl``,
       ``storm``, ``egress``, ``hsb``, ``dos_flood_th``, ``cf_used``).

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Symbol
     - Effect
   * - ``CONFIG_NET_DSA_RTL9602C``
     - Builds everything on this page, as described on the *main* page.
   * - ``CONFIG_NET_SCH_INGRESS``, ``CONFIG_NET_CLS_ACT``
     - The ingress hook (``clsact``) that the filters attach to.
   * - ``CONFIG_NET_CLS_FLOWER``, ``CONFIG_NET_ACT_GACT``
     - ACL rules: a flower rule with ``drop`` or ``trap``. The classifier's
       VLAN translation additionally needs ``CONFIG_NET_ACT_VLAN``.
   * - ``CONFIG_NET_ACT_POLICE``
     - Storm-control rules and the ingress policer: the ``police`` action.
   * - ``CONFIG_NET_CLS_MATCHALL``
     - The ingress policer: a matchall rule with ``police``.
   * - ``CONFIG_NET_SCH_TBF``, ``CONFIG_NET_SCH_ETS``
     - The egress shaper and the scheduler.
   * - ``CONFIG_DCB``
     - ``dcb``: the default priority, the DSCP map and the trust order. DSA
       reads the switch's current maps into the DCB tables when it creates the
       user interfaces.
   * - ``CONFIG_DEBUG_FS``
     - The views ``cf``, ``qos``, ``qdepth`` and ``hsd``.

``rtl9602c_defconfig`` sets all of them.

Device tree
-----------

No property configures these blocks. Two are used. The ``interrupts`` of the
``switch`` node carries the meter, ACL and loop-detection sources (see the
*main* page, "The switch interrupt"), and the ``ethernet`` property of
``port@3`` names the conduit, whose MAC address loop detection takes as its
magic (see "Loop detection"). The compatible is ``realtek,rtl9602c-switch``, as
described on the *main* page.

Interfaces to user space
------------------------

- ``tc`` on ``sfp`` and ``pon``, through the ingress hook. Every example uses
  ``skip_sw``: without it a rule that the switch refuses stays installed in
  software and ``tc`` reports success::

      tc qdisc add dev sfp clsact
      tc filter add dev sfp ingress protocol ip flower skip_sw \
              ip_proto udp dst_port 4444 action drop
      tc filter add dev sfp ingress flower skip_sw \
              dst_mac ff:ff:ff:ff:ff:ff \
              action police pkts_rate 100 pkts_burst 200 conform-exceed drop
      tc filter add dev sfp ingress matchall skip_sw \
              action police rate 1mbit burst 10k conform-exceed drop

  The first is an ACL rule, the second a storm-control rule and the third the
  ingress policer of the port. A qdisc is added to the port itself, with the
  commands of "Egress queues and the scheduler" and "The egress shaper".
- ``dcb app`` and ``dcb apptrust`` for the default priority, the DSCP map and
  the order in which PCP and DSCP are trusted (see "Priority selection and the
  DCB maps").
- ``devlink`` on the handle ``platform/1b000000.switch``: the parameters
  ``dos_*``, ``storm_*``, ``loop_detect*`` and ``pktdbg_*``, and the region
  ``frame_capture``. The *devlink* page lists the parameters.
- Four debugfs views in ``/sys/kernel/debug/rtl9602c/``: ``cf`` and ``qos``
  (mode 0400), ``qdepth`` and ``hsd`` (mode 0600, because a write changes the
  hardware). ``qos`` shows, raw, the words that setup returns to their reset
  values: per port the priority-to-queue word, the scheduler's weighted-queue
  bits, the queue weights, the egress shaper and the ingress policer; then the
  ACL's port enables and default permits, the classifier's control word, port
  0's 2.5G override, each port's per-queue packet-rate enables and meter
  indices, the DSCP map, the port-priority word and the priority-selection
  weights; last, each storm-control class's enable and meter-select words and
  the rate word of each of the sixteen meters.
- The kernel log lines of these blocks. At setup ``rtl9602c-switch: classifier:
  128 entries usable of 256``; the loop lines ``rtl9602c-switch: port N: loop
  detected, closing through port M`` and ``rtl9602c-switch: port N: loop
  cleared``; and, rate-limited and carrying the device's name instead,
  ``meters 0x... exceeded their rate`` and ``acl templates 0x... matched``. The
  messages that refuse a ``tc`` rule are returned to ``tc`` and begin
  ``rtl9602c:``.

How to read the fact tables
===========================

Each block below has a short account of what the hardware does and what the
driver does with it, followed by fact tables with three columns: *Fact*, *Tag*
and *Checked by*. The tag says how the fact is established. **M**, measured on
the board (the DFP-34X-2C3): *Checked by* gives the command or observation,
through the driver's own interfaces (the kernel log, ``tc``, ``dcb``,
``devlink``, debugfs), that shows it. **T**, tested by function: the driver
depends on the fact, and the named behaviour fails if it is wrong. **P**, public
standard: *Checked by* names the document and the clause. **I**, inferred, not
verified: the fact is an assumption, and *Checked by* says what would confirm or
refute it. The last chapter collects these facts and says which of them the
driver depends on.

A row with two tags is established in two ways, or in two parts; *Checked by*
says which part carries which tag. Register offsets are from the base of the
switch's register window, physical ``0x1b000000``. Values are 32-bit words as
``readl()`` and ``writel()`` see them on this big-endian CPU. The bits of a
table entry are numbered from bit 0 of word 0: word 0 holds entry bits 31:0,
word 1 bits 63:32, and so on, and an entry has two to five words. A range of
bits such as 20:13 runs from the high bit to the low one. A port mask has four
bits, bit p for port p. Port numbers are the hardware's (0 ``sfp``, 1 not wired,
2 ``pon``, 3 the CPU port; see the *main* page), and "the host" is the
equipment at the far end of the SFP lane. Traffic measurements used frames sent
from the host, or from the far side of an OLT that bridges transparently,
through the PON, and counted them at the receiving end. "A full chip reset" is
one that returns the switch to its reset values. The behaviours that a tested
fact depends on are named in words: the board boots to a shell; bridged traffic
from the host reaches the far side of an OLT; a routing test and a PPPoE test
over a routed port pass traffic; the storm-control rates match their settings;
ETS and TBF behave as configured.

Reach and state
===============

Two properties hold for every block on this page. The blocks that judge frames
see only the frames that enter a port, so a frame that the CPU sends is outside
their reach. And the switch keeps the state of every block across a warm reboot
and a driver reload, so a block holds what the previous kernel or driver left in
it until something writes it. Setup writes each block that acts on a frame as a
full chip reset leaves it (see "Setup for these blocks").

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The ingress blocks (the ACL, the classifier, the priority selection,
       storm control, the policer and the malformed-frame checks) judge frames
       as they enter a port. A frame that the CPU transmits leaves by its port
       without entering it, and none of them sees it.
     - M
     - An ACL rule enabled on port 0 did not act on frames that the CPU sent out
       of port 0, including its positive control (an EtherType match on exactly
       those frames); the same rule acted on frames arriving from the host.
   * - A warm reboot or a driver reload leaves the switch running: its
       registers, tables and latched status bits keep what the previous kernel
       or driver left in them, and only a full chip reset returns them to their
       reset values (see the *main* page, "Restart, reload and teardown"). So
       setup writes the blocks that it resets to a known state instead of
       assuming reset values.
     - M
     - Malformed-frame checks enabled through devlink stayed enabled, and
       dropping, across ``rmmod`` and ``insmod``; the per-queue page high-water
       marks of ``qdepth`` survived a warm reboot.
   * - Setup resets the malformed-frame checks, storm control and the meters,
       loop detection and the header capture, and, with
       ``rtl9602c_filter_qos_reset()``, the ACL, the classifier, the priority
       maps, the ingress policer and the egress scheduler and shaper, to what
       a full chip reset leaves: every ACL rule retired with its action
       classes at ``RTL9602C_ACL_ACT_EN_DEFAULT``, ACL lookup and the default
       permits off on every port; every classifier entry retired, the
       verdict for unmatched frames (``RTL9602C_CLS_CTRL`` bits 1:0) at 0 and
       ``RTL9602C_CLS_CTRL_PON_EN`` set (which the reset leaves clear);
       per port the reset's priority-to-queue word (port 0 ``0x00000000``,
       port 1 ``0x00249000``, port 2 ``0x006d2240``, port 3 ``0x00fac688``),
       every queue strict at weight 1 and under no packet rate, the shaper and
       the policer at their highest rate; the DSCP map, the port-priority word
       and the priority-selection weights at 0. The ACL templates are left: no
       rule names one until the driver writes it again.
     - M
     - The ``qos`` view read these values after a reset by the watchdog, which
       is a full chip reset, on a kernel that writes none of them; ``cf`` then
       listed all 256 entries, the 255 that the driver had not written holding
       arbitrary words with the valid bit clear. After a warm reboot from a
       kernel with a tbf and an ets on ``sfp``, a policer on ``pon``, an ACL
       rule, a VLAN translation and DCB maps in force, ``qos`` read the same
       values and ``cf`` listed only the entry that the driver wrote after the reboot.
       Before the reset was added, port 0's ``RTL9602C_QOS_PRIO_TO_QUEUE`` read
       ``0x00248000`` after a warm reboot, the value that an ets configured
       before the reboot had left.

The ACL
=======

The ACL holds 96 ternary rules. A rule compares the ingress port, a template
number and up to eight 16-bit slots of the frame. The template, one of seven,
says which field of the frame each of its eight slots holds, so a rule that
matches an EtherType uses one slot, one that matches a MAC address uses three,
and rules that need the same fields share a template. A rule is two entries of
one table, one holding the bits that must be 0 and one the bits that must be 1,
and its action is an entry of a second table. Lookup is enabled for each port,
with a default verdict for the frames that no rule matches. Both tables are
reached through the indirect table engine (see the *bridging* page, "The
indirect table engine").

The driver makes an ACL rule of each ``tc`` flower rule on ingress whose one
action is ``drop`` or ``trap``. It applies only the forwarding class of the
action: a rule drops a frame or sends it to the CPU. The rules judge frames as
they enter a port (see "Reach and state").

Rules, templates and lookup
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The ACL holds ``RTL9602C_ACL_ENTRIES`` (96) ternary rules in the table
       engine: the rule planes as table type
       ``RTL9602C_TBLACC_CMD_TABLE_ACL_RULE`` (2), ``RTL9602C_ACL_RULE_WORDS``
       (5) words per entry; the actions as table type
       ``RTL9602C_TBLACC_CMD_TABLE_ACL_ACT`` (3), ``RTL9602C_ACL_ACT_WORDS`` (3)
       words per entry.
     - M
     - 96 drop rules, each on a distinct EtherType, through ``tc filter add dev
       sfp ingress protocol <type> flower skip_sw action drop``: of 960 target
       frames 2 arrived, of 960 control frames 966 (background traffic
       included); the 97th rule is refused.
   * - A rule is two entries of the same table. Index i holds the bits that
       must be 0 (care and not value); index i +
       ``RTL9602C_ACL_RULE_ONES_PLANE`` (128) holds the bits that must be 1
       (care and value). A bit clear in both is not compared. Indices 0-95 and
       128-223 hold entries, 96-127 and 224-255 read zero, and index 256 reads
       as index 0.
     - M
     - The whole index space read through the table engine; a rule written as a
       value plane plus a mask plane matches nothing, and one written as above
       drops its traffic (see the field codes below).
   * - Rule entry layout, bit positions in the 160-bit entry: the template
       number in bits 2:0; ingress port p at bit 10 + p; eight 16-bit compare
       slots from ``RTL9602C_ACL_RULE_SLOT0_LSB`` (16), slot k at bits 16k + 31
       to 16k + 16; the valid bit at ``RTL9602C_ACL_RULE_VALID_BIT`` (144), set
       in the ones plane. All eight slots compare. The driver compares all three
       template bits and, of the port bits, only its rule's own port.
     - M
     - After a full chip reset, rules on port 0: an EtherType match placed alone
       in slot k drops its target and passes a control frame for every k from 0
       to 7; clearing the ones plane stops the rule.
   * - The port bit of the rule entry selects ports 1 to 3 the same way as port
       0.
     - M
     - For port 2: a drop rule and a trap rule on ``pon`` for EtherType
       ``0x88b5`` each kept all 200 frames of it that came through the PON for
       the host side, while the same frames reached the host side with no rule.
       Ports 1 and 3 carry no traffic that could test theirs.
   * - ``RTL9602C_ACL_TMPL_FIELD`` (``0x015008``) holds the templates: slot k of
       template t is a 7-bit field code at bit 7(n mod 4) of word n/4, where n =
       8t + k; bits 31:28 of each word are unused. ``RTL9602C_ACL_TMPL_COUNT``
       (7) templates of ``RTL9602C_ACL_TMPL_SLOTS`` (8) slots fill the 14 words
       that end where ``RTL9602C_ACL_PORT_EN`` begins. All 56 slots read
       ``RTL9602C_ACL_TMPL_FIELD_NONE`` on a chip that nothing has programmed.
     - M
     - The slots read before the driver programs them; rules on templates built
       from six different field sets each matched their traffic (see the field
       codes below).
   * - A rule that names template 7, which the 3-bit field can express and no
       register holds, matches no frame at all. The driver uses templates 0
       to 6.
     - M
     - A template-7 rule comparing 0xffff in the slot that would hold the
       destination address passed every broadcast frame; the same comparison in
       a template of the driver's dropped them.

The field codes that the driver writes into the template slots:

.. list-table::
   :header-rows: 1
   :widths: 14 46 40

   * - Code
     - Macro
     - Compares
   * - ``0x00``
     - ``RTL9602C_ACL_TMPL_FIELD_NONE``
     - Nothing.
   * - ``0x01`` to ``0x03``
     - ``RTL9602C_ACL_TMPL_FIELD_DMAC0`` + 0 to 2
     - The destination address in three parts: bits 15:0, 31:16 and 47:32.
   * - ``0x04`` to ``0x06``
     - ``RTL9602C_ACL_TMPL_FIELD_SMAC0`` + 0 to 2
     - The source address in the same three parts.
   * - ``0x07``
     - ``RTL9602C_ACL_TMPL_FIELD_ETHERTYPE``
     - The EtherType.
   * - ``0x10``, ``0x11``
     - ``RTL9602C_ACL_TMPL_FIELD_IPV4_SRC0`` + 0 and 1
     - The IPv4 source address, bits 15:0 and 31:16.
   * - ``0x12``, ``0x13``
     - ``RTL9602C_ACL_TMPL_FIELD_IPV4_DST0`` + 0 and 1
     - The IPv4 destination address, bits 15:0 and 31:16.
   * - ``0x14``
     - ``RTL9602C_ACL_TMPL_FIELD_IPV4_TOS_PROTO``
     - The IP protocol in bits 7:0. Bits 15:8, the TOS octet, are never
       compared by the driver.
   * - ``0x40`` + n
     - ``RTL9602C_ACL_TMPL_FIELD_USER0`` + n
     - The window of user field selector n (see "The parser's field
       selectors").

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The field codes of the table above each compare the field they name, and
       the rule's ternary mask applies to the 16-bit slot.
     - M
     - 100 frames per case against a control that differed only in the matched
       field: ``protocol``, ``ip_proto``, ``dst_mac``, ``src_mac``, ``dst_ip``
       and ``src_ip`` each dropped the target and passed the control, and so did
       ``dst_port`` and ``src_port`` through the field selectors.
   * - ``RTL9602C_ACL_PORT_EN`` (``0x015040``): one bit per port turns ACL
       lookup on for frames entering that port. Every bit is clear out of reset.
     - M
     - With the bit clear a rule's target arrives; removing the last filter on
       port 0 clears the bit and the traffic returns.
   * - ``RTL9602C_ACL_DEFAULT_PERMIT`` (``0x015044``): one bit per port, the
       verdict for a frame that no rule matches: set permits it, clear drops it.
       Every bit is clear out of reset, so turning lookup on without it drops
       every frame entering the port. The driver sets and clears it together
       with the port's ``RTL9602C_ACL_PORT_EN`` bit: it sets the permit before
       the lookup and clears it after, so no frame meets the lookup without
       it.
     - M
     - One EtherType drop rule on port 0: with the bit clear 0 of 100
       non-matching frames arrived (103 with no rule); with it set, 103 arrived
       and the target was dropped.

Actions
-------

A rule's action is an entry of the second table plus a word of action classes.
The classes say which parts of the action entry apply, and the forwarding
decision is one of them.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_ACL_ACT_EN`` (``0x015048`` + 4i): one word per rule, the
       action classes that the rule's action entry applies;
       ``RTL9602C_ACL_ACT_EN_FWD`` (bit 4) is the forwarding decision. Every
       word reads ``RTL9602C_ACL_ACT_EN_DEFAULT`` (``0x7f``) out of reset. The
       driver writes ``RTL9602C_ACL_ACT_EN_FWD`` alone for a rule, so no class
       that the rule did not program is applied.
     - M
     - Drop rules written with this word at ``0x10`` drop their traffic.
   * - A zero in ``RTL9602C_ACL_ACT_EN`` chains the entry to the one before it:
       entries n and n + 1 act as one rule that matches only when both patterns
       match, and only entry n's action applies. Any value from ``0x01`` to
       ``0x7f`` leaves the entry standalone. So a released entry gets
       ``RTL9602C_ACL_ACT_EN_DEFAULT`` back, never zero.
     - M
     - After a full chip reset, a rule in entry 0 and one in entry 5: zeroing
       the next entry's word made the rule pass everything, zeroing the
       previous one changed nothing, each of ``0x01`` to ``0x7f`` in the next
       entry kept it dropping, and a zeroed next entry given the same pattern
       made it drop again.
   * - Action entry layout: the forwarding decision in bits 42:41
       (``RTL9602C_ACL_ACT_FWD_LSB``) and its port mask in bits 40:37
       (``RTL9602C_ACL_ACT_FWD_PORTS_LSB``).
       ``RTL9602C_ACL_ACT_FWD_REDIRECT`` (1) with an empty port mask drops the
       frame; there is no separate drop code.
     - M
     - Every drop measurement above uses this encoding.
   * - ``RTL9602C_ACL_ACT_FWD_TRAP`` (3) sends a matching frame to the CPU
       instead of forwarding it. A frame from the PON arrives on the GEM
       interface of its port, which marks it as forwarded by the switch, as it
       does every frame while it is bridged, so the bridge does not forward it
       either: a trapped frame reaches the host alone.
     - M
     - A trap rule on ``pon`` for EtherType ``0x88b5``: of 200 such frames that
       came through the PON for the host side none reached it, and all 200
       reached the CPU, on the GEM interface of the port they came in on, where
       a software ``tc`` filter counted them. The ``data.acl`` case of
       ``tests/device`` repeats this, the drop and the ordering below.

The ACL can raise an interrupt: the source ``RTL9602C_IRQ_SRC_ACL`` of the
switch interrupt, which setup unmasks, and a latch ``RTL9602C_ACL_HIT_STAT``
whose field ``RTL9602C_ACL_HIT_STAT_TMPL`` (bits 7:0) has a bit for each
template. The handler and the latch are described on the *main* page, "The
switch interrupt". The class word of a rule has a class for the interrupt as
well as the one for forwarding; the driver enables only the forwarding class.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - With the interrupt source unmasked and a matching drop rule whose class
       word reads ``0x30`` (the forwarding class and the interrupt class), no
       ACL interrupt arrives and ``RTL9602C_ACL_HIT_STAT`` (``0x0151c8``) reads
       0 throughout. No rule that the driver writes enables the interrupt class,
       so the source has not been seen to fire.
     - M
     - 300 matching frames, the rule's action confirmed by the frame capture in
       drop mode; the switch interrupt count in ``/proc/interrupts`` rose only
       by the learning-limit alarms.

What the driver does with tc flower
-----------------------------------

A rule has eight slots. With full masks, a MAC address needs three of them, an
IPv4 address two, and the EtherType and the IP protocol one each, so a rule that
matches both MAC addresses, the EtherType and the IP protocol fills all eight
and one more field is refused. The rows below are the mapping from a flower rule
to an ACL rule.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A flower rule on ingress whose single action is ``drop`` or ``trap``
       becomes one ACL rule. Accepted matches: EtherType, IP protocol,
       destination and source MAC and IPv4 addresses (with masks), and L4 source
       and destination ports (see "The parser's field selectors"). Each field is
       cut into 16-bit slots and a slot whose mask is zero is left out.
     - M
     - The matches of the field codes above, through ``tc filter ... flower
       skip_sw ... action drop``.
   * - Refused with ``-EOPNOTSUPP`` and a message: egress; IPv6 addresses; any
       other key; a rule with no field left (it would drop every frame on the
       port); more than eight slots; an L4 port without an exact IPv4 EtherType
       and an exact IP protocol. Refused with ``-ENOSPC``: no template covers
       the fields and none is free; all 96 entries are in use.
     - M
     - ``tc filter add dev sfp ingress protocol ip flower skip_sw dst_port 4444
       action drop`` without ``ip_proto`` was refused; the 97th rule was refused
       with ``all 96 ACL rules are in use``.
   * - Templates are shared: a rule takes the first template in use whose slots
       include each of its fields, in any slot (the rule masks the others), else
       the first unused template, whose eight slots it writes (the unused ones
       ``RTL9602C_ACL_TMPL_FIELD_NONE``). A template is counted by the rules
       that use it and is free when none does.
     - M
     - 96 EtherType rules on one shared template, each dropping only its own
       EtherType.
   * - Of two rules that match a frame, the one in the lower entry decides.
       The driver keeps the rules in entries ordered by the priority of their
       ``tc`` filter, a lower value lower, and a new rule after those of its own
       value (``rtl9602c_acl_place()``): when no free entry lies where the new
       rule belongs, the rules above that point move up one entry each into the
       first free entry above them, the topmost first, so that every rule stays
       in at least one entry while they move.
     - M
     - A drop and a trap rule for the same frames on ``pon``: with entries
       taken first-free, the rule added first decided whatever its priority;
       with the entries ordered, the one of priority 10 decided over the one of
       20 in both orders of adding, for drop over trap and for trap over
       drop.

Writing and removing a rule
---------------------------

Writing one rule, ``rtl9602c_acl_replace()``, ``rtl9602c_acl_place()`` and
``rtl9602c_acl_put()``, in order:

1. Find or build a template. Purpose: rules that need the same fields share
   one. A new template's eight slots are written one at a time by
   read-modify-write, because a word holds four slots. (M)
2. Choose the entry i by the rule's priority, moving the rules of larger
   priority values up one entry each when no free entry lies where it belongs
   (see "Rules, templates and lookup"). Each move writes the rule into the
   entry above as below, and the entry left over is retired. Purpose: the
   lower entry decides, and tc's lower priority value goes first. (M)
3. Write zeros to the ones plane at i + 128, then the zeros plane at i (five
   words). Purpose: the entry cannot match yet, because its valid bit is in
   the ones plane. (M)
4. Write the action entry at i (three words): the decision and its port mask.
   (M)
5. Write ``RTL9602C_ACL_ACT_EN(i)`` = ``RTL9602C_ACL_ACT_EN_FWD``. Purpose:
   only the forwarding class applies. (M)
6. Write the ones plane at i + 128, which holds the valid bit. Purpose: it goes
   last, so that no frame meets a half-written rule. (M)
7. Set the port's ``RTL9602C_ACL_DEFAULT_PERMIT`` bit, then its
   ``RTL9602C_ACL_PORT_EN`` bit. Purpose: lookup without the permit drops every
   frame that no rule matches. (M)

Removing one, ``rtl9602c_acl_del()``, in order:

1. Write zeros to the ones plane at i + 128. Purpose: the valid bit goes and
   the rule stops. (M)
2. Write ``RTL9602C_ACL_ACT_EN_DEFAULT`` to ``RTL9602C_ACL_ACT_EN(i)``, and
   never 0. Purpose: a zero would chain the next entry to this one. (M)
3. Drop the template's use count. Purpose: a template that no rule uses is free
   again. (M)
4. If the port has no rule left, clear its ``RTL9602C_ACL_PORT_EN`` and
   ``RTL9602C_ACL_DEFAULT_PERMIT`` bits, in that order. Purpose: a port without
   rules needs no lookup, and the lookup is off before the permit goes. (M)

The steps are checked as follows. Adding: step 1 by the rules on templates built
from six different field sets that each matched their traffic; step 2 by the
whole index space read through the table engine; steps 3 and 5 by the drop
measurements, which use this action encoding and this valid bit; step 4 by the
drop rules that dropped with the class word at ``0x10``; step 6 by the frames
that arrived with the permit bit set and none with it clear. Removing: step 1 by
clearing the ones plane stopping a rule; step 2 by the chaining measurement;
step 3 by 96 rules sharing one template; step 4 by the traffic returning when
the last filter on port 0 was removed.

The parser's field selectors
============================

No fixed field code of the ACL finds the Layer 4 ports. The parser offers user
field selectors instead: each cuts a 16-bit window out of the frame, at a byte
offset from a chosen origin, and hands it to the ACL under a field code of its
own. The driver points two of them at the source port and the destination port
of a UDP or TCP header, at the start of the IP payload, and never changes them.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_FIELDSEL_CFG`` (``0x023144`` + 4n) configures user field
       selector n: a 16-bit window cut out of the frame at byte offset
       ``RTL9602C_FIELDSEL_CFG_OFFSET`` (bits 10:3) from the origin
       ``RTL9602C_FIELDSEL_CFG_ORIGIN`` (bits 2:0).
       ``RTL9602C_FIELDSEL_CFG_ORIGIN_IP_PAYLOAD`` (6) is the start of the IP
       payload, the L4 header. A template slot compares selector n's window with
       field code ``RTL9602C_ACL_TMPL_FIELD_USER0`` + n, and the rule's ternary
       mask applies to it.
     - M
     - A selector at IP-payload offset 2 asked for 4444 dropped UDP frames to
       port 4444 and passed port 5555, and the reverse when asked for 5555;
       offset 0 matched the source port; the value ``0x1100`` with the mask
       ``0xff00`` matched port 4444 (``0x115c``) and not 5555.
   * - At setup the driver points selector ``RTL9602C_FSEL_SPORT`` (0) at
       IP-payload offset 0 and ``RTL9602C_FSEL_DPORT`` (1) at offset 2, and
       never changes them. A flower ``src_port`` or ``dst_port`` becomes field
       code ``0x40`` or ``0x41`` with the port's value and mask.
     - M
     - ``tc filter add dev sfp ingress protocol ip flower skip_sw ip_proto udp
       dst_port 4444 action drop`` dropped port 4444 and passed 5555;
       ``src_port 33333`` dropped the frames carrying it.
   * - UDP and TCP headers carry the source port in octets 0-1 and the
       destination port in octets 2-3.
     - P
     - RFC 768, "Format"; RFC 9293, section 3.1.
   * - A selector cuts its window whatever the frame carries, including a frame
       with no L4 header, which is why the driver accepts a port match only
       together with an exact IPv4 EtherType and an exact IP protocol.
     - I
     - Not load-bearing: the driver only refuses more. A port-only rule compared
       against ARP frames with a chosen value at that offset would confirm it.

The classifier
==============

The classifier has 256 entries, of which the driver uses the lower part (see
the depth row below). An entry is a rule, an action and a set of action classes.
A rule keys on the direction of the frame, upstream when it enters by a user
port and downstream when it enters by the PON port, and, if it asks, on the
ingress port and on the frame's C-tag and VID. An action assigns the frame a GEM
stream, the flow number of a GEM port, or a C-VID, and the classes say which
parts of the action apply. When several entries match a frame the
lowest-numbered applies.

The driver uses the classifier for two things. The first is to give the frames
that the switch forwards to the PON port their GEM stream, without which they do
not leave (see the last row of "Entries and rules"). The second is the VLAN
translation that the stick exists for, a rule that matches one C-VID and assigns
another. As services they are described on the *bridging* page ("Bridging the
GEM ports" and "VLAN translation"); this page describes the entries.

Entries and rules
-----------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The classifier holds ``RTL9602C_CLS_ENTRIES`` (256) entries in the table
       engine: the rule planes as table type
       ``RTL9602C_TBLACC_CMD_TABLE_CLS_RULE`` (4), two words; the actions as
       ``RTL9602C_TBLACC_CMD_TABLE_CLS_ACT`` (5), three words. As in the ACL,
       index i holds the must-be-0 plane (care and not value) and index i +
       ``RTL9602C_CLS_RULE_ONES_PLANE`` (``0x100``) the must-be-1 plane (value).
     - T
     - Frames that the switch forwards to the PON port reach the far side of an
       OLT only through the stream entries: bridged traffic from the host, and a
       routing test.
   * - ``RTL9602C_CLS_CTRL`` (``0x01600c``) bits 12:5,
       ``RTL9602C_CLS_CTRL_LAYOUT1_START``, is the first entry matched with a
       second rule layout; the entries below it take the layout that the driver
       writes. It reads 128 on this board. The driver reads it at setup and uses
       only the entries below it (all 256 if it reads 0).
     - M
     - The boot log line ``classifier: 128 entries usable of 256``; the driver's
       stream entries sit at 127 downwards.
   * - The entries from ``RTL9602C_CLS_CTRL_LAYOUT1_START`` upward interpret a
       rule in a different layout, so a rule of the driver's layout written
       there does not match as written.
     - I
     - The driver never writes there, so this is load-bearing only as its depth
       limit. A frame test of an entry above the boundary would settle it.
   * - ``RTL9602C_CLS_CTRL`` bits 1:0 are one field that decides what happens to
       a frame that matches no entry: the value 2 drops it (forwarding reason
       239), in whichever direction has no matching entry; 0, 1 and 3 let it
       through. Bit 2 changes nothing observed. Setup writes bits 1:0 to 0 and
       the driver writes them nowhere else; it never writes bit 2. The ``cf``
       view prints bits 1:0.
     - M
     - One upstream stream entry installed and the eight values of bits 2:0
       swept under traffic both ways through the PON: the values 2 and 6
       discarded every downstream frame at the PON port (reason 239 in the frame
       capture) and the others passed them; upstream frames passed under all
       eight.
   * - ``RTL9602C_CLS_CTRL_PON_EN`` (bit 3 of ``RTL9602C_CLS_CTRL``) makes the
       C-tag actions of the entries apply to the frames that cross the PON
       port. With it clear an entry still matches and gives its stream, but
       the frame leaves with the tag it came with. Setup sets it, and nothing
       clears it; the ``cf`` view prints the word (``cls cfg``).
     - M
     - With the bit clear, an upstream translation entry without a stream
       lost all 200 frames of its VID, and with the port's stream all 200
       arrived untranslated; with the bit set they arrived translated. Frames
       of other VIDs, and untagged ones, crossed both ways unchanged with the
       bit set.
   * - A full chip reset leaves ``RTL9602C_CLS_CTRL`` bits 1:0 at 0, which lets
       unmatched frames through, and setup writes 0 there too. The driver
       installs no downstream entries of its own, so it depends on the value
       0, not on what power-on or an earlier kernel left.
     - M
     - ``cf`` read ``upstream permit 0`` after a reset by the watchdog and
       after a warm reboot; the sweep in the row above passed frames both ways
       with the value 0.
   * - Rule word 0: ``RTL9602C_CLS_RULE_UNI`` (bits 2:0) is the port that an
       upstream frame entered by.
     - T
     - Entries keyed on the ingress port alone give that port's frames their
       stream: bridged traffic from the host, a routing test and PPPoE over a
       routed port reach the far side.
   * - Rule word 0: ``RTL9602C_CLS_RULE_DS`` (bit 31) is set for a frame
       travelling downstream (entering by the PON port). The driver always
       compares it: set in a downstream entry, clear in an upstream one.
     - M
     - In the unmatched-frame sweep above, an upstream entry keyed on port 0
       with this bit compared as 0 matched no downstream frame.
   * - Rule word 1: ``RTL9602C_CLS_RULE_VALID`` (bit 16), written in the ones
       plane; zeroing both words of the ones plane retires the entry.
     - T
     - As for the ingress-port key; after ``tc filter del`` of a translation the
       entry no longer shows ``VALID`` in the ``cf`` view.
   * - Rule word 0: ``RTL9602C_CLS_RULE_CTAG`` (bit 3) is set when the frame
       carries a C-tag and ``RTL9602C_CLS_RULE_VID`` (bits 22:11) holds that
       tag's VLAN ID.
     - M
     - Under a translation of 410 to 1410 on ``sfp`` and of 1410 to 410 on
       ``pon``, 200 frames of C-VID 410 sent into the SFP port left the far side
       of the OLT as 1410, and 200 of C-VID 1410 sent from there left the SFP
       port as 410; frames of the other VID in each direction, and untagged
       ones, passed unchanged.
   * - Rule word 0: ``RTL9602C_CLS_RULE_STAG`` (bit 4) is set when the frame
       carries an S-tag. Every VLAN entry that the driver writes compares it as
       0 (it is in ``RTL9602C_CLS_RULE_VLAN_CARE``), so an S-tagged frame
       matches none of them.
     - I
     - An S-tagged frame through a translation entry would show whether it is
       refused.
   * - Action word 0 in an upstream entry: ``RTL9602C_CLS_ACT_FLOW_SET``
       (bit 23) gives the frame a stream and ``RTL9602C_CLS_ACT_FLOW`` (bits
       30:24) is the stream, the GEM port's flow number (see the *PON* page).
       The driver writes these bits in upstream entries only.
     - T
     - Bridged traffic from the host, a routing test and PPPoE over a routed
       port reach the far side of an OLT.
   * - Action word 1, the C-tag plane: ``RTL9602C_CLS_ACT_CVID_SET`` (bit 18)
       assigns the C-VID held in ``RTL9602C_CLS_ACT_CVID`` (bits 14:3), and
       ``RTL9602C_CLS_ACT_CTAG`` (bits 2:1) says what becomes of the C-tag:
       ``RTL9602C_CLS_ACT_CTAG_TAG`` (1) gives the frame a C-tag with the
       assigned VID and keeps its priority bits, ``RTL9602C_CLS_ACT_CTAG_UNTAG``
       (2) takes the tag off, and with 0 or 3 the frame keeps its tag and VID,
       the assignment notwithstanding. A translation writes the assignment with
       ``RTL9602C_CLS_ACT_CTAG_TAG``. Action word 0's S-tag plane stays zero for
       a single-tag translation.
     - M
     - Frames of C-VID 410 and PCP 5 under an upstream entry assigning 1410,
       and of 1410 under a downstream entry assigning 410, with each value of
       the field: 1 delivered 1410/5 and 410/5, 200 of 200; 2 delivered the
       downstream frames untagged; 0 and 3 delivered them as sent.
   * - ``RTL9602C_CLS_ACT_EN(i)`` (``0x016038`` + 4i): which of entry i's action
       classes apply. The driver writes ``RTL9602C_CLS_ACT_EN_OTHERS`` and
       ``RTL9602C_CLS_ACT_EN_FLOW`` together (``0x3f``) for an upstream entry,
       and its stream is applied.
     - T
     - As for the stream in action word 0.
   * - ``RTL9602C_CLS_ACT_EN_FLOW`` (bit 5) is stream assignment and
       ``RTL9602C_CLS_ACT_EN_OTHERS`` (bits 4:0) are forwarding, DSCP, priority,
       C-tag and S-tag; the value ``0x1f`` that the driver writes for a
       downstream entry applies its C-tag action.
     - M
     - The downstream translation of the C-tag row above applies with
       ``0x1f``.
   * - ``RTL9602C_CLS_ENTRY_TMPL`` (``0x016010`` + 4(i/32), bit i mod 32): the
       template that entry i is matched with. The driver clears the bit,
       selecting template 0, the layout of the rule and action words above.
     - T
     - As for the ingress-port key.
   * - When a frame matches several entries, the classifier acts on the
       lowest-numbered one. The driver depends on it: stream entries are taken
       from the top of the usable range and translations from the bottom, so a
       translation must name the stream itself.
     - M
     - Against an operator's OLT, an entry that passed the same VID through at a
       lower index applied instead of a translation at a higher index; once the
       lower entries were removed the translation applied and the PPPoE link
       behind it came up within 15 s.
   * - A frame that the switch forwards to the PON port leaves on the fibre only
       if a classifier entry gave it a stream; without one the switch counts it
       transmitted on the PON port and it never arrives. A frame that the CPU
       sends names its stream in its transmit descriptor instead (see the *CPU
       port* page).
     - M
     - A routed flow with no stream entry: the PON port counted 100 frames sent
       and 0 discarded while the far side received none of them; with the
       driver's entry the flow arrives.

What the driver writes
----------------------

The entries that the driver composes, all in ``rtl9602c-offload.c``:

.. list-table::
   :header-rows: 1
   :widths: 22 30 34 14

   * - Entry
     - Rule compares
     - Action, and action classes
     - Taken from
   * - Stream entry of a routed port, or of a port in the GEM bridge while no
       VLAN claims a GEM flow
     - Upstream; the ingress port (``RTL9602C_CLS_RULE_UNI``).
     - ``RTL9602C_CLS_ACT_FLOW_SET`` and ``RTL9602C_CLS_ACT_FLOW``: the routed
       flow, or the bridge's upstream flow. Classes ``0x3f``.
     - The top of the usable range, downwards.
   * - Stream entry of a bridged port for one claimed VLAN
     - Upstream; the ingress port; a C-tag with that VID; no S-tag.
     - The stream of that VLAN's GEM interface. Classes ``0x3f``.
     - The top of the usable range, downwards.
   * - Upstream translation of VID A to B
     - Upstream; a C-tag with VID A; no S-tag; any ingress port.
     - ``RTL9602C_CLS_ACT_CVID_SET`` and ``RTL9602C_CLS_ACT_CVID`` = B with
       ``RTL9602C_CLS_ACT_CTAG_TAG``, and the stream of the GEM interface that
       claims B, or else the one the rule's port's stream entry gives. Classes
       ``0x3f``.
     - The bottom of the usable range, upwards.
   * - Downstream translation of VID A to B
     - Downstream; a C-tag with VID A; no S-tag.
     - ``RTL9602C_CLS_ACT_CVID_SET`` and ``RTL9602C_CLS_ACT_CVID`` = B with
       ``RTL9602C_CLS_ACT_CTAG_TAG``. Classes ``0x1f``.
     - The bottom of the usable range, upwards.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Stream entries are rebuilt whole, every entry of the mechanism retired
       and then the set computed again, on each bridge membership change, each
       VLAN change of an upstream GEM interface, each change of the routed
       upstream flow, and at setup. For each user port other than the PON port:
       a port outside the GEM interfaces' bridge gets one entry giving its
       frames the routed upstream flow, if one is registered; a port inside that
       bridge gets one entry giving its frames the bridge's upstream flow while
       no VLAN names a flow. Entries are allocated from the top of the usable
       range downwards. A port for which the classifier has no room is left
       without a stream, with a warning in the kernel log. When upstream GEM
       interfaces claim VLANs, a bridged port gets one entry for each claimed
       VLAN instead, and a frame in no claimed VLAN gets no stream; that part is
       not verified (see the *bridging* page, "Bridging the GEM ports").
     - T, I
     - T: bridged upstream traffic from the host reaches the far side of an OLT;
       in a routing test the routed port's entry is a ``VALID`` line in ``cf``
       and the flow is forwarded. I: the per-VLAN entries; two upstream GEM
       interfaces in two VLANs, each VLAN's frames arriving in its own GEM port
       at the far side of an OLT, would settle them.
   * - A translation, ``tc filter add dev port ingress protocol 802.1Q flower
       vlan_id A action vlan modify id B``, becomes one entry: C-tag present and
       VID A compared (the priority is not compared, so one entry covers every
       PCP), the direction taken from the port (the PON port is downstream,
       every other port upstream), C-VID B assigned, the action classes as in
       the table above, and the entry taken from the bottom of the usable range.
       What the driver refuses is listed on the *bridging* page. An upstream
       translation also gives its frames a stream: the flow of the upstream GEM
       interface that is a member of the assigned VLAN B, or else the flow
       that the stream entry of the rule's port gives, which the frame would
       have had untranslated. Each change of the stream entries writes the
       upstream translations again with the stream that now applies.
     - M
     - The entry read back exactly as the driver composed it; ``vlan_prio`` in
       the match was refused; ``tc filter del`` zeroed the ones plane. With
       the GEM interfaces bridged and no VLAN claimed, an upstream translation
       without a stream lost all 200 frames of its VID, and with the port's
       stream delivered them translated (see the C-tag rows above).
   * - The upstream and downstream directions are the only ones the classifier
       sees: a frame from the SFP port to the CPU crosses neither, so a
       translation cannot be observed on it.
     - I
     - A translation entry on ``sfp`` did not change the VLAN that a frame was
       learned in on its way to the CPU; a frame across the PON would settle
       it.

Writing one entry, ``rtl9602c_cf_program()``, in order:

1. Write the zeros plane at index i: word 0 = care and not value, with
   ``RTL9602C_CLS_RULE_DS`` always in the care set; word 1 = 0. Purpose: the
   bits that must be 0 are in place first. (T)
2. Write the three action words at i. Purpose: they go before the rule is
   valid, so that no frame meets half of an entry. (T)
3. Write ``RTL9602C_CLS_ACT_EN(i)``. Purpose: the action classes that apply.
   (T)
4. Clear entry i's ``RTL9602C_CLS_ENTRY_TMPL`` bit. Purpose: template 0, the
   layout of the words above. (T)
5. Write the ones plane at i + ``0x100``: word 0 = value, word 1 =
   ``RTL9602C_CLS_RULE_VALID``. Purpose: the valid bit goes last. (T)

Retiring entry i writes both words of its ones plane as 0
(``rtl9602c_cf_invalidate()``). Purpose: the valid bit goes and the entry stops.
(T)

The steps are checked by the stream entries doing their work: bridged traffic
from the host reaches the far side of an OLT, a routing test and PPPoE over a
routed port pass traffic, and a removed translation no longer shows ``VALID``.

The cf view
-----------

``/sys/kernel/debug/rtl9602c/cf`` (mode 0400) prints the classifier as the
hardware holds it. Each read goes through the table engine, which takes the
engine's lock itself, so the view does not hold it.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Output
     - Shows
   * - ``N classifier entries, pattern-1 boundary B``
     - The number of entries the driver uses, and
       ``RTL9602C_CLS_CTRL_LAYOUT1_START``.
   * - ``cls cfg W  upstream permit V``
     - The whole ``RTL9602C_CLS_CTRL`` word, and its bits 1:0, the field that
       decides the fate of a frame that no entry matches.
   * - one line per entry whose ones plane has ``RTL9602C_CLS_RULE_VALID`` or a
       non-zero word 0
     - The index; ``rule``, the ones plane (two words); ``mask``, the zeros
       plane; ``act``, the three action words; ``en``,
       ``RTL9602C_CLS_ACT_EN(i)`` followed by ``+sid`` when
       ``RTL9602C_CLS_ACT_EN_FLOW`` is set; ``tmpl``, the template bit; and
       ``VALID`` when the valid bit is set.
   * - ``(no entry has a rule)``
     - Printed when no entry qualifies.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The view reads each entry through the table engine, which takes the
       engine's lock itself, so the view does not hold it. The tests that count
       the ``VALID`` lines depend on the form above.
     - T
     - A routing test and a PPPoE test count the ``VALID`` lines of ``cf``.

Meters
======

Sixteen meters are shared by everything that limits a rate by packets: the
storm control of ``tc`` and of devlink. A meter has a rate, a bucket size and a
unit, and it sets a latching bit when it drops a frame. The driver programs
every meter that it hands out in packet mode, with the default bucket, because
byte mode does not police on this chip.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_METER_COUNT`` (16) shared meters, 64 bits each in two words
       with the high half at the lower address: ``RTL9602C_METER_CFG_HI(i)``
       (``0x025004`` + 8i) holds bits 63:32 and ``RTL9602C_METER_CFG_LO(i)``
       (``0x025008`` + 8i) bits 31:0. ``RTL9602C_METER_CFG_HI_RATE`` (bits 18:0
       of the high word) is the rate, ``RTL9602C_METER_CFG_LO_BURST`` (bits 16:1
       of the low word) the bucket size and ``RTL9602C_METER_CFG_LO_MODE`` (bit
       17 of the low word) the unit: ``RTL9602C_METER_CFG_LO_MODE_PACKET`` (1)
       counts packets.
     - M
     - A packet-mode meter at rate 0 exceeded under background traffic of a few
       broadcasts a second; the same meter at rate 100000 did not.
   * - In packet mode one rate unit is one packet per second.
     - M
     - Broadcast offered at 322 frames/s against limits of 50, 150, 300, 600 and
       1200 set with ``tc ... police pkts_rate``: 51.7, 151.7 and 301.8 frames/s
       arrived, and everything at the two limits above the offered rate.
   * - Byte mode (mode bit 0) does not police on this chip, and bit 0 of the low
       word, which would count the preamble and inter-frame gap in a byte rate,
       reads back 0 whatever is written. The driver therefore programs every
       meter that it hands out in packet mode.
     - M
     - A broadcast class bound to a byte-mode meter at its lowest rate passed
       thirty times that rate for 15 s without setting its exceeded bit;
       changing only the mode bit made it exceed at once.
   * - Every meter reads rate ``RTL9602C_METER_CFG_HI_RATE_MAX`` (``0x7ffff``),
       which never limits, and bucket ``RTL9602C_METER_CFG_LO_BURST_DEFAULT``
       (``0x3fff``) out of reset. The driver always writes that bucket; a bucket
       of one token exceeds at any rate.
     - M
     - The meters read at probe; a packet-mode meter with bucket 1 exceeded at
       rate 100000, with bucket ``0x3fff`` it did not.
   * - ``RTL9602C_METER_EXCEEDED`` (``0x025084``): one latching bit per meter,
       set when the meter drops a frame and cleared by writing a one. The driver
       clears all sixteen at setup. The interrupt source
       ``RTL9602C_IRQ_SRC_METER`` that a set bit raises, and its handler, are
       described on the *main* page ("The switch interrupt").
     - M
     - A limit of 1 packet/s on unknown multicast set the bit of its meter
       (``meters 0x1 exceeded their rate`` in the kernel log). A reset that
       cleared the classes and the meters but not the latches left meter 15
       reading exceeded with nothing bound to it.
   * - A meter handed back is first reset (rate
       ``RTL9602C_METER_CFG_HI_RATE_MAX``, the default bucket, byte mode) so
       that it limits nothing while it has no owner. The sixteen meters are one
       pool for the storm control of ``tc`` and of devlink.
     - M
     - An ARP limit from ``tc`` on meter 0 and an unknown-unicast limit from
       devlink on meter 1 held at the same time; deleting the rules left no
       class enabled and no meter bound.

Storm control
=============

Storm control limits how fast frames of a class enter a port. There are seven
classes: unknown unicast, unknown multicast, multicast, broadcast, ARP, DHCP,
and IGMP with MLD. Each class has an enable register, with one bit for each
port, and a meter-select register, with four bits for each port that name one
of the sixteen meters. A port polices a class when its bit is set: the frames of
that class entering the port count against the selected meter, and the meter
drops those beyond its rate. Both ways of setting storm control take their
meters from the same pool of sixteen.

.. list-table::
   :header-rows: 1
   :widths: 20 28 32 20

   * - Class (driver index)
     - Enable register
     - Meter-select register
     - Set through
   * - Unknown multicast (``RTL9602C_STORM_UM``)
     - ``RTL9602C_STORM_UNKNOWN_MCAST_EN`` (``0x01703c``)
     - ``RTL9602C_STORM_UNKNOWN_MCAST_METER_SEL`` (``0x01704c``)
     - devlink
   * - Unknown unicast (``RTL9602C_STORM_UC``)
     - ``RTL9602C_STORM_UNKNOWN_UCAST_EN`` (``0x017040``)
     - ``RTL9602C_STORM_UNKNOWN_UCAST_METER_SEL`` (``0x017050``)
     - devlink
   * - Multicast (``RTL9602C_STORM_MC``)
     - ``RTL9602C_STORM_MCAST_EN`` (``0x017044``)
     - ``RTL9602C_STORM_MCAST_METER_SEL`` (``0x017054``)
     - ``tc``, a destination with the group bit
   * - Broadcast (``RTL9602C_STORM_BC``)
     - ``RTL9602C_STORM_BCAST_EN`` (``0x017048``)
     - ``RTL9602C_STORM_BCAST_METER_SEL`` (``0x017058``)
     - ``tc``, a destination of all ones
   * - ARP (``RTL9602C_STORM_ARP``)
     - ``RTL9602C_STORM_ARP_EN`` (``0x01705c``)
     - ``RTL9602C_STORM_ARP_METER_SEL`` (``0x017068``)
     - ``tc``, an ARP EtherType
   * - DHCP (``RTL9602C_STORM_DHCP``)
     - ``RTL9602C_STORM_DHCP_EN`` (``0x017060``)
     - ``RTL9602C_STORM_DHCP_METER_SEL`` (``0x01706c``)
     - devlink
   * - IGMP and MLD (``RTL9602C_STORM_IGMP``)
     - ``RTL9602C_STORM_IGMP_EN`` (``0x017064``)
     - ``RTL9602C_STORM_IGMP_METER_SEL`` (``0x017070``)
     - devlink

The driver's table ``rtl9602c_storm_class[]`` pairs each class with its two
registers; ``RTL9602C_STORM_NCLASS`` (7) is its length, and its numbering is the
table's own, not a hardware value.

A flower rule with ``police pkts_rate`` reaches three classes: broadcast,
multicast and ARP. Each such rule takes a meter of its own, programmed at the
rule's rate, and binds it to its class on the rule's port. The other four
classes are reached through devlink, one packet rate and one port mask for each:
``storm_unknown_unicast_pps`` and ``_ports``, ``storm_unknown_multicast_pps``
and ``_ports``, ``storm_dhcp_pps`` and ``_ports``, and ``storm_igmp_mld_pps``
and ``_ports``. The unknown classes are defined by what the address tables hold
and not by a field of the frame, and the DHCP and IGMP classes are taken to be
wider than one flower rule (see the rows below). Setting the rate takes a meter
and programs it; setting the ports enables the class on them::

    devlink dev param set platform/1b000000.switch \
            name storm_unknown_unicast_pps value 200 cmode runtime
    devlink dev param set platform/1b000000.switch \
            name storm_unknown_unicast_ports value 1 cmode runtime

The unknown-multicast class includes the reserved group addresses, the
destinations of the bridge's own control protocols, unless they are exempted.
Setup sets ``RTL9602C_RSVDMC_CTRL_STORM_EXEMPT`` (bit 3) on the controls of the
five addresses of spanning tree, MAC Control pause, the Slow Protocols, 802.1X
and LLDP, so that a flood limit does not police a port's own control protocols
while storm control stays in force for other traffic. It sets the bit by
read-modify-write, because ``RTL9602C_RSVDMC_CTRL_ACTION`` (bits 5:4) reads 2 on
two of the controls out of reset. The controls, the measurement and the
assumption about the controls after the first are on the *bridging* page,
"Reserved multicast addresses".

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Each of the seven classes has an enable register and a meter-select
       register (``RTL9602C_STORM_METER_SEL_PORT(p)``, four bits per port at bit
       4p), the registers of the table above. All enables and selectors are 0
       out of reset. The unknown-multicast, unknown-unicast and broadcast
       classes police as set.
     - M
     - Broadcast, through ``tc``: 267 of 800 frames arrived at ``pkts_rate
       100``, 801 with no rule. Unknown unicast, through devlink: offered at 322
       frames/s against 200, 202 frames/s arrived. Unknown multicast, at 1
       packet/s: the frames were caught dropped by the frame capture.
   * - The multicast, ARP, DHCP and IGMP classes police their own frames at the
       rate set, as the first three do.
     - I
     - They have been configured through ``tc`` or devlink and read back, and
       never offered matching traffic. Each class policing its own frames at a
       set rate would confirm it.
   * - The unknown-unicast class is unicast to an address that the switch has
       not learned; the unknown-multicast class is multicast to a group that no
       table holds, the reserved addresses included unless they are exempted.
     - M
     - The measurements of the first row.
   * - The driver writes the meter selector before it sets the class's port bit,
       so that a newly enabled class is never live against another class's meter
       (a selector reads 0 at reset, naming meter 0).
     - T
     - The storm-control rates match their settings.
   * - Through ``tc``: ``flower dst_mac ff:ff:ff:ff:ff:ff`` (full mask) is the
       broadcast class, ``flower dst_mac 01:00:00:00:00:00/01:00:00:00:00:00``
       the multicast class, and ``flower protocol arp`` without a VLAN match the
       ARP class. The rule matches its class and nothing else, since the class
       is policed whole: any other key, such as an EtherType beside a
       destination, an IP protocol, a port or a VLAN, is refused, as are a
       source-address match and any other destination or mask. The one action
       must be ``police pkts_rate N``, passing what conforms and dropping what
       exceeds, with no byte rate, peak rate, average rate or overhead. Each
       rule takes its own meter, programmed in packet mode at N (at most
       ``RTL9602C_METER_CFG_HI_RATE_MAX``) and bound to its class on its port.
       The burst of ``police`` is not programmed: every meter gets the default
       bucket.
     - M
     - ``tc filter add dev sfp ingress flower skip_sw dst_mac ff:ff:ff:ff:ff:ff
       action police pkts_rate 100 pkts_burst 200 conform-exceed drop`` limited
       broadcast; ``police rate 1mbit`` was refused with ``these meters count
       packets``. The same rule with ``protocol ip`` and ``ip_proto udp``, an
       ARP rule with ``arp_op request``, and one with ``conform-exceed
       drop/continue`` were refused, and ``qos`` showed no class enabled.
   * - A class has one meter selector per port, so of several rules for one
       class and port the one added last polices. Removing it hands the class
       to the rule added before it, or switches the class off on the port when
       none is left; removing any other rule leaves the binding alone. ``tc
       filter replace``, which adds the new rule before it removes the old,
       therefore leaves the class on, on the new rule's meter.
     - M
     - In ``qos``, the broadcast class's selector followed a second rule on
       ``sfp`` and went back to the first when the second was removed; after a
       ``tc filter replace`` the class was still enabled, on the meter
       programmed at the new rate, and the old meter was back at its maximum.
   * - The destination-address shapes of the previous row: the group bit is the
       least significant bit of the first octet, and all ones is the broadcast
       address.
     - P
     - IEEE 802.3, clause 3.2.3.
   * - Through devlink, one packet rate per class and a port mask for the ports
       it polices. A non-zero rate takes a meter and programs it at once; rate 0
       unbinds every port and returns the meter; a rate of
       ``RTL9602C_METER_CFG_HI_RATE_MAX`` or more, and a mask with bits beyond
       the four ports, are refused as out of range. A mask for a class that has
       no rate is refused, and so is a rate when all sixteen meters are in use.
       Reads come from the hardware: the ports from the enable register, the
       rate from the meter that the class owns, a maximum reading 0. The driver
       keeps only which meter a class owns, because a selector of 0 cannot tell
       meter 0 from none.
     - M, T
     - M: setting ``storm_unknown_unicast_pps`` to 200 left the class off until
       ``storm_unknown_unicast_ports`` was set to 1; after the registers were
       cleared from outside the driver, both parameters read 0. T: the
       storm-control rates match their settings.
   * - The DHCP class takes UDP ports 67 and 68 in either direction and the IGMP
       class takes MLD as well, which no single flower rule expresses; that is
       why those two classes are reached through devlink only.
     - I
     - Not load-bearing: it only decides the interface. The classes offered DHCP
       server, DHCP client and MLD frames would confirm it.
   * - DHCP uses UDP port 67 at the server and 68 at the client; MLD messages
       are ICMPv6 messages.
     - P
     - RFC 2131, section 4.1; RFC 2710, section 3.

Binding a class to a meter on a port, in order (``rtl9602c_storm_bind()`` and
its callers):

1. Program the meter: the rate in ``RTL9602C_METER_CFG_HI``, then the default
   bucket and packet mode in ``RTL9602C_METER_CFG_LO``. Purpose: the meter
   limits before any class points at it. (M)
2. Write the port's nibble of the class's meter-select register. Purpose: the
   class never goes live on another class's meter. (T)
3. Set the port's bit of the class's enable register. Purpose: the class
   polices. (T)

Removing the rule that polices its class and port binds the rule added before
it by steps 2 and 3, the bit staying set; with no rule left it clears the bit,
then writes the nibble to 0, so that the class is never live on meter 0. The
removed rule's meter is reset and returned last. Steps 1 to 3 are checked by the
measurements of the meters and of the classes above, and by the storm-control
rates matching their settings.

The ingress policer
===================

Each port has a rate limiter for what enters it. The rate is the only setting
that the driver has for it: no bucket size is known for this limiter. The driver
offers it as the one matchall ``police`` rule that DSA allows on the ingress of
a port.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_POLICE_PORT(p)`` (``0x020010`` + ``0x400`` p, in the per-port
       MAC window): ``RTL9602C_POLICE_PORT_RATE`` (bits 20:2) limits what enters
       the port, in units of 8 kbit/s (1000 bytes/s).
       ``RTL9602C_POLICE_PORT_RATE_MAX`` (``0x7ffff``) is the reset value and
       never limits, so the rate is also the enable.
     - M
     - ``tc filter add dev sfp ingress matchall skip_sw action police rate 1mbit
       burst 10k conform-exceed drop`` wrote 125; a 300-frame flood ping from
       the host lost 34.3% of its frames with it and none after ``tc qdisc del
       dev sfp clsact``, which wrote ``0x7ffff`` back.
   * - ``port_policer_add`` (one matchall ``police`` rule per port, ingress)
       writes the rate in bytes per second divided by 1000, refusing 0 and
       cutting a larger value to the maximum; the burst is not programmed.
       ``port_policer_del`` writes ``RTL9602C_POLICE_PORT_RATE_MAX``.
     - M
     - As for the register: the value written for ``rate 1mbit``, and the
       maximum written back on removal.

Malformed-frame (DoS) checks
============================

The switch can recognise sixteen shapes of malformed or hostile frames and drop
them or send them to the CPU. Each check, numbered 0 to 15, has an enable and an
action, drop or trap; a per-port register chooses the ports whose entering
frames are judged, and the three flood checks have a threshold each. A frame
that a check stops carries the forwarding reason 64 + n, which the frame
capture shows (see "Frame capture"). The driver switches every check off at
setup and offers them through six devlink parameters: ``dos_ports``, which is
``RTL9602C_DOS_PORT_EN``; ``dos_checks`` and ``dos_actions``, the two halves of
``RTL9602C_DOS_CHECK_CTRL``; and ``dos_synflood_thresh``,
``dos_finflood_thresh`` and ``dos_icmpflood_thresh``, the three thresholds::

    devlink dev param set platform/1b000000.switch \
            name dos_ports value 1 cmode runtime
    devlink dev param set platform/1b000000.switch \
            name dos_checks value 2 cmode runtime

The checks, in bit order, with the forwarding reason of each:

.. list-table::
   :header-rows: 1
   :widths: 8 62 20 10

   * - Bit n
     - Check
     - Forwarding reason
     - Tag
   * - 0
     - Destination address equal to source address.
     - 64
     - I
   * - 1
     - Land: the IP source equal to the IP destination.
     - 65
     - M
   * - 2
     - Blat: the L4 source port equal to the destination port.
     - 66
     - M
   * - 3
     - SYN and FIN both set.
     - 67
     - M
   * - 4
     - Xmas scan.
     - 68
     - M
   * - 5
     - Null scan.
     - 69
     - M
   * - 6
     - SYN from a source port below 1024.
     - 70
     - M
   * - 7
     - TCP header too short.
     - 71
     - M
   * - 8
     - TCP fragment offset.
     - 72
     - M
   * - 9
     - ICMP fragment.
     - 73
     - M
   * - 10
     - Ping of death: a fragment whose offset plus length goes beyond 65535.
     - 74
     - M
   * - 11
     - UDP bomb: a UDP length larger than the datagram.
     - 75
     - M
   * - 12
     - SYN carrying data.
     - 76
     - M
   * - 13
     - SYN flood, counted against ``RTL9602C_DOS_SYN_FLOOD_THR``.
     - 77
     - M
   * - 14
     - FIN flood, counted against ``RTL9602C_DOS_FIN_FLOOD_THR``.
     - 78
     - M
   * - 15
     - ICMP flood, counted against ``RTL9602C_DOS_ICMP_FLOOD_THR``.
     - 79
     - M

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_DOS_PORT_EN`` (``0x026000``): one bit per port
       (``RTL9602C_DOS_PORT_EN_MASK``, bits 3:0) applies the checks to frames
       entering that port. A check acts only with both its port bit and its
       enable set.
     - M
     - ``dos_ports`` 1 with ``dos_checks`` 2 catches land frames arriving on
       port 0; land with its enable off and on.
   * - ``RTL9602C_DOS_CHECK_CTRL`` (``0x026004``):
       ``RTL9602C_DOS_CHECK_CTRL_EN`` (bits 15:0), bit n enables check n;
       ``RTL9602C_DOS_CHECK_CTRL_TRAP`` (bits 31:16), bit 16 + n, is its action:
       0 drops the frame, 1 sends it to the CPU instead of forwarding it.
     - M
     - One TCP-short-header frame offered twice through devlink, changing only
       ``dos_actions``: at 65535 the frame capture caught it in the
       trapped-frame mode with a non-empty destination mask (``0xc0``), at 0 in
       the dropped-frame mode with an empty one; forwarding reason 71 both
       times.
   * - The checks in bit order are those of the table above. A frame that a
       check stops carries forwarding reason 64 + n. A frame that matches
       several enabled checks is reported under one of them: the ping-of-death
       shape reported 73 or 79 until checks 9 and 15 were disabled, so each
       check was isolated with the others off.
     - M
     - Checks 1 to 15 each enabled through devlink, fired by a frame of its
       shape and caught by the frame capture with reason 64 + n.
   * - ``RTL9602C_DOS_SYN_FLOOD_THR`` (``0x026008``),
       ``RTL9602C_DOS_FIN_FLOOD_THR`` (``0x02600c``) and
       ``RTL9602C_DOS_ICMP_FLOOD_THR`` (``0x026010``) are 8-bit thresholds of
       checks 13 to 15. At 0 a flood check fires on the first matching frame.
     - M
     - ``sfp`` bridged, 200 SYN frames a run: they were dropped at
       ``dos_synflood_thresh`` 0, passed at 255 and dropped again at 0; the
       frame capture on reason 77 caught one at 0, nothing in 15 s at 255, and
       one again at 0.
   * - A threshold counts frames per a fixed hardware window of unknown length.
     - I
     - Not load-bearing. Sweeping the offered rate against a fixed threshold on
       a quiet link would settle it.
   * - A read of a threshold register can stall the CPU on the register bus
       until the soft-lockup detector resets the board; a write works. Whether
       every read does is not settled, because one read of all three registers
       returned 0. The driver never reads them: it keeps the last
       value written, in an array indexed by ``RTL9602C_DOS_FLOOD_SYN`` (0),
       ``RTL9602C_DOS_FLOOD_FIN`` (1) and ``RTL9602C_DOS_FLOOD_ICMP`` (2), which
       are not hardware values, and answers devlink from that copy.
     - M, I
     - M: one 32-bit read of ``0x026008`` through a user-space mapping of the
       window never returned and the board reset about 32 s later;
       ``devlink dev param show`` returns with the board alive and
       ``dos_synflood_thresh`` reads back what was set. I: that every read is
       unsafe; a single read through a driver debug path after a write would
       show it. Not load-bearing, since nothing reads them.
   * - Check 0 cannot be exercised with frames whose destination equals their
       source: with the check off they are already dropped, with forwarding
       reason 228, because the destination resolves to the port they entered by.
       So check 0 has not been seen to fire.
     - M, I
     - M: 200 such frames were dropped with the check off, and a frame to a
       different address that was learned on its ingress port also got the
       reason 228. I: that the check fires at all; no frame can show it. Not
       load-bearing.
   * - The devlink parameters ``dos_ports``, ``dos_checks`` and ``dos_actions``
       write ``RTL9602C_DOS_PORT_EN`` and the two halves of
       ``RTL9602C_DOS_CHECK_CTRL`` and are read back from the registers; the
       three thresholds are written to their registers and read back from the
       copy above. A value wider than its field is refused, not cut.
     - M
     - ``dos_checks`` 65536 and ``dos_ports`` 255 answered "Numerical result out
       of range"; ``dos_checks`` 2 read back 2 and the register agreed; two
       checks and a port written and ``dos_checks`` read back as 6.
   * - A full chip reset clears all five registers; a driver reload does not.
       Setup writes all five to 0 and zeroes the driver's copy of the thresholds
       with them.
     - M
     - The five read 0 after a full chip reset with the driver not loaded; land
       and blat enabled before a reload read 0 after it.

Loop detection
==============

Loop detection sends probes out of chosen ports and watches for one of its own
to come back. A probe is a broadcast frame with EtherType ``0x8899`` and an
all-zero source address. Its payload carries a 48-bit identifier that the
hardware generates and refreshes, and a 48-bit magic. When a probe with the
switch's magic returns on a port, the switch latches a loop for that port and
raises the switch interrupt source ``RTL9602C_IRQ_SRC_LOOP``; it latches the end
of a loop as well. The driver reports the loops in the kernel log and does
nothing else about them.

The magic is what the switch recognises when a probe returns, so it has to be
unique to the switch. The driver writes the MAC address of the CPU port's
conduit, the interface ``cpu``, into it. The probes use the EtherType of the
PTP timestamp tag; the conduit's handling of a frame that carries it is
described on the *CPU port* page.

.. list-table::
   :header-rows: 1
   :widths: 40 16 44

   * - Register
     - Offset
     - Holds
   * - ``RTL9602C_LOOPDET_CTRL``
     - ``0x01a000``
     - ``RTL9602C_LOOPDET_CTRL_EN`` (bit 0) turns the block on.
   * - ``RTL9602C_LOOPDET_PROBE_CFG``
     - ``0x01a004``
     - ``RTL9602C_LOOPDET_PROBE_CFG_PERIOD`` (bits 23:8): the interval between
       probes in milliseconds, 2000 after reset.
   * - ``RTL9602C_LOOPDET_MAGIC_HI``
     - ``0x01a014``
     - Magic bits 47:32.
   * - ``RTL9602C_LOOPDET_MAGIC_LO``
     - ``0x01a018``
     - Magic bits 31:0.
   * - ``RTL9602C_LOOPDET_TX_PORTS``
     - ``0x01a01c``
     - One bit per port that sends probes.
   * - ``RTL9602C_LOOPDET_DETECTED``
     - ``0x01a020``
     - One latched bit per port for a loop found, cleared by writing 1.
   * - ``RTL9602C_LOOPDET_CLEARED``
     - ``0x01a024``
     - One latched bit per port for a loop gone, cleared by writing 1.
   * - ``RTL9602C_LOOPDET_PEER_PORT``
     - ``0x01a02c``
     - ``RTL9602C_LOOPDET_PEER_PORT_BITS`` (3) bits per port: the port that the
       loop closes through.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The registers of the table above behave as listed.
     - M
     - Probes captured on the host 2.000 s apart at period 2000; a loop found
       and cleared through the interrupt; the probe word read ``0x0007d008``
       after a reset.
   * - The probe is a broadcast frame with EtherType ``0x8899`` and an all-zero
       source address; its payload carries a 48-bit identifier that the hardware
       generates and refreshes, and the 48-bit magic at offset 7 of the payload.
     - M
     - A capture on the host.
   * - On enable the driver writes the CPU port conduit's MAC address into the
       magic (the first two octets in ``RTL9602C_LOOPDET_MAGIC_HI``, the last
       four in ``RTL9602C_LOOPDET_MAGIC_LO``) and refuses to enable when there
       is no conduit.
     - M
     - The magic read back equal to the conduit's address.
   * - The devlink parameter ``loop_detect`` (0 or 1) unmasks
       ``RTL9602C_IRQ_SRC_LOOP`` and sets ``RTL9602C_LOOPDET_CTRL_EN``, or masks
       the source and clears it; ``loop_detect_ports`` is
       ``RTL9602C_LOOPDET_TX_PORTS`` (four bits); ``loop_detect_period`` is the
       period, up to ``0xffff`` ms. The last two and the enable are read back
       from the registers.
     - M
     - Set and shown through devlink with a reflector on the host.
   * - The handler logs ``port N: loop detected, closing through port M`` and
       ``port N: loop cleared``, and acknowledges by writing back exactly the
       bits it read, so that an event arriving between the read and the write is
       kept. It reports and does not act: deciding what to do about a loop is
       policy, and the bridge has a protocol for it, spanning tree.
     - M
     - A reflector on the host returned EtherType ``0x8899`` frames and produced
       both lines through the interrupt.
   * - ``RTL9602C_LOOPDET_PEER_PORT`` has only been seen for a loop that closes
       through the port it left (port 0 through port 0); a loop between two
       different ports has not been produced.
     - I
     - Loop two ports to each other. Load-bearing for the far-end port in the
       log line only.
   * - Whether a returning probe with the same magic but another switch's
       identifier counts as a loop is unknown; the driver makes the magic unique
       so that it does not matter.
     - I
     - Replaying a probe with a changed identifier would show it. Not
       load-bearing.
   * - Setup turns the block off (``RTL9602C_LOOPDET_CTRL`` = 0,
       ``RTL9602C_LOOPDET_TX_PORTS`` = 0) and clears both latches, all four bits
       of each: a loaded driver must not start sending probes onto a network by
       itself, and a latch left from before would report a loop that ended
       before this driver existed.
     - T
     - The board boots to a shell with the block off.

Priority selection and the DCB maps
===================================

A frame gets an internal priority, one of eight, from sources such as the
ingress port, the frame's PCP and its DSCP. Each source has a weight, and maps
turn a source's value into a priority. The priority then chooses the frame's
egress queue through a map of each egress port (see "Egress queues and the
scheduler").

The driver sets these through DCB. ``dcb app add ... dscp-prio`` writes an entry
of the DSCP map, ``dcb app add ... default-prio`` sets the default priority of
the port, and ``dcb apptrust`` sets the weights of PCP and DSCP, which fixes the
order in which they are trusted::

    dcb apptrust set dev sfp order pcp dscp
    dcb app add dev sfp dscp-prio 46:5

The DSCP map is switch-wide, and setup tells DSA so
(``dscp_prio_mapping_is_global``); it serves frames that enter by the PON port
as well, and the weights are switch-wide too. A frame that the CPU sends carries
its priority in its transmit descriptor instead (see the *CPU port* page), and
the priority-to-queue map turns it into a queue.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The switch gives each frame an internal priority, one of
       ``RTL9602C_QOS_PRIO_COUNT`` (8), taken from sources (the ingress port,
       the 802.1Q PCP, the DSCP and others that the driver does not use), each
       with a 3-bit weight; weight 0 means the source is not consulted. All
       weights read 0 out of reset, so every frame is priority 0 until a weight
       is set.
     - M
     - After ``dcb apptrust set dev sfp order pcp dscp`` and ``dcb app add dev
       sfp dscp-prio 46:5``, frames with DSCP 46 were captured with internal
       priority 5 and the same frames with DSCP 0 with priority 0.
   * - Of several sources that apply to a frame, the one with the heaviest
       weight decides its priority.
     - I
     - The driver depends on it for the order in ``dcb apptrust``. A tagged
       frame with a PCP and a DSCP that map to different priorities, captured
       under both orders, would confirm it.
   * - ``RTL9602C_QOS_PRIO_WEIGHT`` (``0x01c0f4``) is two weight tables, one
       word each; ``RTL9602C_QOS_PRIO_WEIGHT_SEL`` (``0x01c0fc``) bit 0,
       ``RTL9602C_QOS_PRIO_WEIGHT_SEL_IDX``, names the one in force. The driver
       reads the index and writes only that table. Source s has its weight at
       bits 3s + 2 to 3s; ``RTL9602C_QOS_PRIO_WEIGHT_DSCP`` is s = 2.
     - T
     - The scheduling checks, whose two traffic classes differ only in DSCP,
       share the port as the priorities of the DSCP map ask.
   * - ``RTL9602C_QOS_PRIO_WEIGHT_PCP`` is s = 1 and
       ``RTL9602C_QOS_PRIO_WEIGHT_PORT`` s = 0.
     - I
     - They have been written and read back only. A tagged frame classified by
       PCP under ``dcb apptrust ... order pcp``, and an untagged one by the port
       default priority, would confirm them.
   * - The priority maps pack ten 3-bit entries into a word
       (``RTL9602C_QOS_MAP_PER_WORD``, ``RTL9602C_QOS_MAP_ENTRY_BITS``): entry i
       is in word i/10 at bits 3(i mod 10) + 2 to 3(i mod 10).
     - T
     - The scheduling checks: DSCP 10 and 46 are entries in words 1 and 4.
   * - ``RTL9602C_QOS_DSCP_TO_PRIO`` (``0x01c0d4``): ``RTL9602C_QOS_DSCP_COUNT``
       (64) entries mapping DSCP to internal priority, one table for the whole
       switch, all 0 out of reset. ``dcb app add ... dscp-prio`` writes an entry
       and ``del`` writes it back to 0. Frames entering by the PON port are
       classified by the same table.
     - T
     - ``dcb app add dev sfp dscp-prio 46:5 10:2`` classifies frames that enter
       by the PON port.
   * - The DS field carries a 6-bit DSCP, 64 codepoints; the 802.1Q tag carries
       a 3-bit PCP and a 12-bit VID.
     - P
     - RFC 2474, section 3; IEEE 802.1Q, clause 9.6.
   * - ``RTL9602C_QOS_PORT_PRIO`` (``0x01c0f0``): each port's default priority,
       3 bits per port in one word, all 0 out of reset. ``dcb`` default priority
       writes the port's entry and, if the port source's weight is 0, sets it to
       1, the lightest, so that the default applies and any trusted source
       outranks it.
     - I
     - They have been written and read back only. An untagged frame without DSCP
       trust on a port with default priority 5, captured with priority 5, would
       confirm them.
   * - ``dcb apptrust``: the weight table is switch-wide, so the first port to
       set an order owns it and another port asking for a different order is
       refused (``-EOPNOTSUPP``). PCP and DSCP only, at most
       ``RTL9602C_APPTRUST_MAX`` (2): the first gets
       ``RTL9602C_QOS_PRIO_WEIGHT_MAX`` (7), the second 6. Reading answers from
       the weights, heavier first.
     - M
     - ``dcb apptrust set dev sfp order pcp dscp`` read back as ``pcp dscp``,
       the two weights as 7 and 6.
   * - The PCP-to-priority map is the identity out of reset; the driver does not
       write it.
     - M
     - A register read after a reset.
   * - ``RTL9602C_QOS_PRIO_TO_QUEUE`` (``0x01c0b0`` + 4p): per port, eight 3-bit
       entries (priority i at bits 3i + 2 to 3i) naming the egress queue of each
       internal priority. Out of reset port 0 sends every priority to queue 0
       (``0x00000000``), port 1 priorities 0 to 3 and 4 to 7 to queues 0 and 1
       (``0x00249000``), port 2 pairs of priorities to queues 0 to 3
       (``0x006d2240``), and port 3 each priority to its own queue
       (``0x00fac688``). The register keeps any value written.
     - M
     - Read after a full chip reset; swapping two priorities in the priomap of
       an ets swaps which class wins in the scheduling checks.

Egress queues and the scheduler
===============================

Each port has eight egress queues. The scheduler serves the strict queues
first, the highest-numbered first, and shares what is left among the weighted
queues by weight. The ets qdisc of ``tc`` maps onto it. An ets band is a queue,
band b being queue 7 - b, so that band 0, which ``tc`` serves first, is the
queue that the switch serves first. A band with a quantum of 0 is a strict
queue; any other band is a weighted queue whose weight is its quantum in
scheduler tokens. The priomap of the ets becomes the port's priority-to-queue
map. An ets is accepted at the root of a port, or under the port's tbf, by the
tbf's class (``parent 1:1`` under a tbf ``1:``) or by the tbf itself (``parent
1:``); a qdisc in a band is not offloaded::

    tc qdisc add dev sfp root handle 1: ets bands 2 quanta 660 1980 \
            priomap 0 0 0 0 1 1 1 1

This makes priorities 0 to 3 go to band 0, which is queue 7, and priorities 4 to
7 to band 1, queue 6, and the two queues share the port in the proportion 1:3 of
their quanta.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Each port has ``RTL9602C_QUEUES`` (8) egress queues.
       ``RTL9602C_SCHED_WFQ_QUEUES`` (``0x02d85c`` + 4p): bit q set makes queue
       q weighted, clear strict; 0 on every port out of reset. Strict queues are
       served highest number first; every strict queue is served before any
       weighted one; weighted queues share the rest by weight. Out of reset
       every weight is 1.
     - M
     - Frames that the switch forwards from the PON to the SFP port under a 50
       Mbit/s shaper: with two strict classes the higher queue took 49 988
       kbit/s and the lower none; with strict queue 0 against weighted queue 1,
       50 008 against 157 kbit/s.
   * - Ports 1 to 3 have eight working queues like port 0, where queues 0, 2, 3,
       5, 6 and 7 have been seen to hold frames.
     - I
     - The driver accepts an ets on any port. The scheduling traffic on a second
       port would confirm it.
   * - Weights: queue 0's is 16 bits in ``RTL9602C_SCHED_WEIGHT_Q0``
       (``0x02d824`` + 4(p/2), at bit 16(p mod 2), two ports to a word, at most
       ``RTL9602C_SCHED_WEIGHT_Q0_MAX`` ``0xffff``); queues 1 to 7 are 10 bits
       in ``RTL9602C_SCHED_WEIGHT_Q1_7`` (``0x02d82c`` + 12p + 4((q - 1)/3), at
       bit 10((q - 1) mod 3), three to a word and three words to a port, at most
       ``RTL9602C_SCHED_WEIGHT_Q1_7_MAX`` ``0x3ff``).
     - M
     - A dump shows three weights of 1, three more, and the seventh alone,
       repeating every three words per port, and queue 0's word reading
       ``0x00010001``; two weighted queues split the bytes as their weights
       say.
   * - ``RTL9602C_SCHED_CTRL`` (``0x02d800``) bit 17,
       ``RTL9602C_SCHED_CTRL_WEIGHT_PKTS``: set, weights count packets; clear,
       bytes; weighted queues are served by weight either way. Clear out of
       reset. The driver clears it whenever it programs weights, because an ets
       quantum is in bytes.
     - M
     - Weights 1:1, 300-byte against 1514-byte frames: set gave equal frame
       rates (29 006 and 27 794 frames), clear equal bandwidth (23 670 and 25
       055 kbit/s); weights 1:3 with it clear split the bytes 1:3 at either
       size.
   * - ``RTL9602C_SCHED_TOKEN_SIZE`` (``0x02d890``) bits 7:0,
       ``RTL9602C_SCHED_TOKEN_SIZE_BYTES``: the bytes that one weight unit
       stands for; it reads 66. The driver divides each quantum by the value it
       reads and refuses to use a 0.
     - M
     - A register read; quanta of 660 and 1980 split the bytes 1:3.
   * - tc band b is queue 7 - b: ``tc`` serves band 0 first and the switch
       serves its highest queue first. A band with quantum 0 is strict at weight
       1; any other is weighted at quantum / token size, at least 1, and a
       weight that does not fit its queue's field is refused as out of range.
       The priomap entry of priority i writes queue 7 - band into
       ``RTL9602C_QOS_PRIO_TO_QUEUE``; entries that name a band beyond ``bands``
       are left alone.
     - T
     - ETS behaves as configured: strict and weighted classes share the port as
       the bands and quanta ask.
   * - An ets is accepted at the root, or as the child of the port's followed
       root tbf, whose class or handle may name it as the parent; more than
       eight bands is refused as out of range; a qdisc in a band is not
       offloaded. The first ets on a port saves the port's
       ``RTL9602C_QOS_PRIO_TO_QUEUE`` word.
     - T
     - The ETS checks that replace an ets at the root and under a tbf. Under a
       tbf ``1:`` on ``sfp``, ``tc`` reports an ets added with ``parent 1:1``
       or ``parent 1:`` as offloaded, and ``qos`` shows its map and weights.
   * - Removing the ets that the port follows sets every queue strict at weight
       1, writes 0 to ``RTL9602C_SCHED_WFQ_QUEUES`` and restores the saved
       priority-to-queue word.
     - T
     - After the removal the port's shaper, weights, queue types and map read as
       before the first qdisc.
   * - Each port follows one tbf and one ets, by handle. A replace programs the
       port and makes its handle the followed one; a destroy, graft or
       statistics request that names any other handle changes nothing and
       answers ``-EOPNOTSUPP``, because ``tc qdisc replace`` under a new handle
       creates the new qdisc before it destroys the old one, and the old one's
       destroy must not undo the new one. A statistics request succeeds only for
       a followed qdisc, which is what makes ``tc`` print ``offloaded``; the
       counters stay the software qdisc's.
     - T
     - ETS and TBF behave as configured when each check replaces its qdisc under
       a new handle, and when the last one is removed.

Programming an ets, ``rtl9602c_setup_ets()``, in order:

1. Check the parent, the band count and the token size; compute every queue's
   weight and the weighted-queue mask. Purpose: nothing is written for a request
   that is refused. A refused replace of the followed ets resets the port as in
   step 4. (T)
2. Save the port's priority-to-queue word the first time. Purpose: it is put
   back when the ets goes; setup does not reset the word. (T)
3. Write all eight weights (1 for strict queues) and
   ``RTL9602C_SCHED_WFQ_QUEUES``, clear ``RTL9602C_SCHED_CTRL_WEIGHT_PKTS``,
   then write the priomap into ``RTL9602C_QOS_PRIO_TO_QUEUE`` entry by entry,
   and remember the handle. Purpose: the queues, their types and the map follow
   the qdisc. (T)
4. On the destroy of the followed ets: weights 1, ``RTL9602C_SCHED_WFQ_QUEUES``
   0, the saved priority-to-queue word back, and forget the handle. Purpose: the
   port is as before the ets. (T)

The steps are checked by the ETS behaviour: classes that differ only in DSCP
share a port as the strict and weighted queues, the quanta and the priomap ask,
and after the removal the port reads as before the first qdisc.

The egress shaper
=================

Each port has an egress rate limit, the shaper. Its rate is a count of units of
8 kbit/s, and its reset value does not limit. The tbf qdisc of ``tc`` at the
root of a port sets it: the rate of the tbf becomes the rate of the shaper. The
``burst`` and ``latency`` are not programmed, because no bucket depth is known
for the shaper. Port 0 enforces its rate only when a second register holds it
too, in units of twice the size; that register also holds the SerDes lane's 2.5G
enable, ``RTL9602C_SHAPE_2G5_CFG_EN`` (bit 19), which the *main* page describes
("Forced MAC ability")::

    tc qdisc add dev sfp root handle 1: tbf rate 50mbit burst 64k latency 50ms

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_SHAPE_PORT_CFG`` (``0x02d804`` + 4p):
       ``RTL9602C_SHAPE_PORT_CFG_RATE`` (bits 19:1) is the port's egress rate in
       units of 8 kbit/s, ``RTL9602C_SHAPE_PORT_CFG_RATE_STEP`` (1000) bytes/s
       each; ``RTL9602C_SHAPE_PORT_CFG_RATE_MAX`` (``0x7ffff``) is the reset
       value and does not limit. The driver changes only the rate field.
     - M
     - ``tc qdisc add dev sfp root handle 1: tbf rate 50mbit burst 64k latency
       50ms`` wrote ``0x000030d4`` (6250 at bit 1) and frames forwarded from the
       PON arrived at 50 444 kbit/s, the software bucket having seen 25 of them.
   * - On port 0 the rate is enforced only when
       ``RTL9602C_SHAPE_2G5_CFG_RATE`` (bits 18:0 of ``RTL9602C_SHAPE_2G5_CFG``,
       ``0x02d8b8``) holds it too, in units of 16 kbit/s (half the value). The
       driver writes both for port 0 and only the first for other ports.
     - M
     - ``RTL9602C_SHAPE_PORT_CFG`` alone at six values from 8 kbit/s to 20
       Mbit/s left the port at its full rate; with both written, 625 (5 Mbit/s)
       gave 5044 kbit/s over 30 s.
   * - A SerDes bring-up at 2500BASE-X writes half the rate of port 0's
       ``RTL9602C_SHAPE_PORT_CFG_RATE`` into ``RTL9602C_SHAPE_2G5_CFG_RATE``,
       so a tbf that holds port 0 stays in force; with no tbf that rate is the
       maximum, whose half is ``RTL9602C_SHAPE_2G5_CFG_RATE_FULL``.
     - M, I
     - M: after setup the ``qos`` view read the override as ``000bffff``; with
       a tbf on ``sfp`` it held half the tbf's rate, and ``000bffff`` again
       after the tbf was removed. I: the bring-up runs again only when phylink
       changes the interface mode, which no measurement has done under a
       tbf.
   * - tbf is accepted at the root only. The rate is the rate in bytes per
       second divided by 1000; 0, or more than the maximum, is refused as out of
       range, and if the qdisc being replaced was followed the port goes back to
       unlimited. ``burst`` and ``latency`` are not programmed: no per-port
       bucket depth is known. A tbf in an ets band is refused, because a rate
       for one queue cannot be expressed (see "Measured, not driven").
     - T
     - TBF behaves as configured: the rate of a port under a tbf is the rate
       that was set.
   * - The shaped rate reads about 1% above the requested rate when counted in
       IP length at 1514-byte frames; what the shaper counts beyond the IP
       packet (header, FCS, preamble, gap) is not established.
     - M
     - 50 Mbit/s asked, 50 444 kbit/s counted; 25 Mbit/s asked, 25 292 kbit/s.
   * - A zero in ``RTL9602C_SHAPE_PORT_CFG_RATE`` passes nothing.
     - I
     - Not load-bearing: the driver never writes 0. Writing 0 and watching the
       port stop would confirm it.

Programming a tbf, ``rtl9602c_setup_tbf()``, in order:

1. Replace at the root: the rate divided by 1000 into
   ``RTL9602C_SHAPE_PORT_CFG_RATE`` and, for port 0, half of it into
   ``RTL9602C_SHAPE_2G5_CFG_RATE``; remember the handle. Purpose: the port is
   limited. (T)
2. On the destroy of the followed tbf: ``RTL9602C_SHAPE_PORT_CFG_RATE_MAX`` into
   ``RTL9602C_SHAPE_PORT_CFG_RATE`` and, for port 0, half of it
   (``RTL9602C_SHAPE_2G5_CFG_RATE_FULL``, ``0x3ffff``) into
   ``RTL9602C_SHAPE_2G5_CFG_RATE``; forget the handle. Purpose: the port is
   unlimited again. (T)

The steps are checked by the TBF behaviour: the rate of a port under a tbf, and
the port unlimited again after the removal.

Buffer pages and qdepth
=======================

The switch counts the buffer pages that each egress queue holds, now and at the
most. The debugfs view ``qdepth`` shows the count of every queue, which is how
the queue that a frame took is seen.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_BUF_QUEUE_PAGES`` (``0x02d038`` + 4(8p + q)), 32 words,
       port-major: ``RTL9602C_BUF_QUEUE_PAGES_CUR`` (bits 9:0) is the number of
       buffer pages that queue q of port p holds now,
       ``RTL9602C_BUF_QUEUE_PAGES_PEAK`` (bits 25:16) the most it has held.
     - M
     - An idle board shows only port 0 queue 0 and port 3 queue 0 non-zero;
       frames sent at priority 3, 5 and 7 each raise exactly their own queue's
       peak on port 0 (``cat /sys/kernel/debug/rtl9602c/qdepth``).
   * - ``RTL9602C_BUF_PEAK_CLR`` (``0x0230fc``) bit 0,
       ``RTL9602C_BUF_PEAK_CLR_PORT``, clears the per-port page high-water marks
       and leaves the per-queue ones. Only a full chip reset is known to clear
       the per-queue marks.
     - M
     - Holding the bit took port 0's port-level peak from 3 pages to its live 2
       and left every per-queue word unchanged, whether the bit was pulsed or
       held.
   * - ``qdepth`` (debugfs, mode 0600): reading prints port, queue, current and
       peak pages for all 32 queues; writing ``clear`` sets
       ``RTL9602C_BUF_PEAK_CLR_PORT``, waits 1 to 2 ms and clears it again,
       which clears the per-port marks only and changes nothing the view shows.
     - M
     - ``cat`` and ``echo clear >`` on ``/sys/kernel/debug/rtl9602c/qdepth``.

Flow-control thresholds
=======================

The switch has flow-control thresholds, in buffer pages: a pair for the whole
switch and a pair of the PON port's own. A pair is two words, and each word has
an on level and an off level. The driver gives the PON port its own thresholds
when the PON MAC is built (see the *PON* page): each PON level is the
switch-wide level plus ``RTL9602C_FC_PON_EXTRA`` (``0x30``) pages, clamped to
its field. That the registers are what their names say, and that the PON port
needs the margin, are not verified.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PAUSE_THR_GLOBAL_HIGH`` (``0x0230e4``) and
       ``RTL9602C_PAUSE_THR_GLOBAL_LOW`` (``0x0230e8``) are the switch-wide
       flow-control threshold pairs, and ``RTL9602C_PAUSE_THR_PON_HIGH``
       (``0x02311c``) and ``RTL9602C_PAUSE_THR_PON_LOW`` (``0x023120``) the PON
       port's own; each word has an on level, ``RTL9602C_PAUSE_THR_ON`` (bits
       25:16), and an off level, ``RTL9602C_PAUSE_THR_OFF`` (bits 9:0), in
       buffer pages.
     - I
     - The registers have been written only; see the next row.
   * - When the PON MAC is built the driver sets both PON pairs to the global
       pairs plus ``RTL9602C_FC_PON_EXTRA`` (``0x30``), each level clamped to
       its field.
     - I
     - The driver depends on it, and whether the PON port needs it is not
       verified. Reading the global and PON words after bring-up, then repeating
       the bridged-traffic check with the write removed, would show whether it
       matters.

The one step, ``rtl9602c_pon_fc_thresholds()``: for the high and then the low
pair, read the global word, add ``RTL9602C_FC_PON_EXTRA`` to its on and off
levels, clamp each to its field, and write both into the PON word by
read-modify-write. Purpose: the PON port has its own margin of pages. (I)

Frame capture
=============

The header capture freezes one frame: the parser's view of it and the switch's
decision about it. It is armed with a mode, which chooses the frame; two port
masks, which restrict it by ingress and by egress port; and a forwarding reason,
which mode 7 waits for. When a frame is held, a valid bit is set, and the frame
stays until the capture is armed again. The capture is how the forwarding reason
of a dropped or trapped frame is seen, and most of the measurements on this page
use it.

The driver offers it through four devlink parameters, ``pktdbg_mode``,
``pktdbg_ingress_ports``, ``pktdbg_egress_ports`` and ``pktdbg_reason``, which
together make one setting, and through the region ``frame_capture``, which holds
a snapshot of the frame. The region keeps at most one snapshot, so a snapshot is
deleted before the next one is taken::

    devlink dev param set platform/1b000000.switch \
            name pktdbg_mode value 0 cmode runtime
    devlink region new platform/1b000000.switch/frame_capture
    devlink region read platform/1b000000.switch/frame_capture \
            snapshot <id> address 0 length 168
    devlink region del platform/1b000000.switch/frame_capture snapshot <id>

The capture modes:

.. list-table::
   :header-rows: 1
   :widths: 10 70 20

   * - Mode
     - Captures
     - Tag
   * - 0
     - Any frame.
     - M
   * - 1
     - None: it never captures (``RTL9602C_PKTDBG_LATCH_MODE_NONE``).
     - M
   * - 2
     - The first dropped frame.
     - M
   * - 3
     - The first forwarded frame.
     - I
   * - 4
     - The first frame sent to the CPU.
     - I
   * - 5
     - Dropped frames.
     - M
   * - 6
     - Frames sent to the CPU.
     - M
   * - 7
     - Frames whose forwarding reason is in ``RTL9602C_PKTDBG_LATCH_REASON``;
       ``RTL9602C_PKTDBG_LATCH_MODE_MAX`` is 7.
     - M

The forwarding reasons seen in captures:

.. list-table::
   :header-rows: 1
   :widths: 20 70 10

   * - Reason
     - Meaning
     - Tag
   * - 0
     - Forwarded.
     - M
   * - 64 + n
     - Stopped by malformed-frame check n (see "Malformed-frame (DoS) checks").
     - M
   * - 88
     - Dropped by storm control.
     - M
   * - 228
     - The destination resolves to the ingress port.
     - M
   * - 239
     - No classifier entry matched while ``RTL9602C_CLS_CTRL`` bits 1:0 were 2
       (see "The classifier").
     - M

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PKTDBG_LATCH`` (``0x028000``) arms a capture of one frame:
       ``RTL9602C_PKTDBG_LATCH_MODE`` (bits 4:2) chooses which frame,
       ``RTL9602C_PKTDBG_LATCH_INGRESS_PORTS`` (bits 8:5) and
       ``RTL9602C_PKTDBG_LATCH_EGRESS_PORTS`` (bits 12:9) restrict it by port,
       one bit per port, ``RTL9602C_PKTDBG_LATCH_REASON`` (bits 20:13) is the
       forwarding reason for mode 7, and ``RTL9602C_PKTDBG_LATCH_VALID``
       (bit 21) is set once a frame is held. The frame is
       ``RTL9602C_PKTDBG_PARSE(i)`` (``0x028040`` + 4i,
       ``RTL9602C_PKTDBG_PARSE_WORDS`` 28 words), the parser's view of it, and
       ``RTL9602C_PKTDBG_VERDICT(i)`` (``0x028148`` + 4i,
       ``RTL9602C_PKTDBG_VERDICT_WORDS`` 13 words), the forwarding decision; bit
       b of either is bit b mod 32 of word b/32.
     - M
     - A captured frame's source address decoded to its sender's in all 48 bits,
       and the VLAN ID in the parse and in the verdict agreed; the control word
       read back in each capture as armed, with VALID set.
   * - In the parse, bits 13:0 are the frame length. In the verdict, the
       destination port mask (bits 331:324) is 0 for a dropped frame, and the
       forwarding reason (bits 346:339) is the field that mode 7 compares.
     - M
     - A parse's length decoded to 102 for a ping crossing a tagged port; a
       storm drop was captured with mask 0 and reason 88, and latched by reason
       88.
   * - Mode ``RTL9602C_PKTDBG_LATCH_MODE_NONE`` (1) never captures.
     - M
     - Ten pings across the switch with the capture in mode none; the snapshot
       was refused before and after.
   * - The modes of the table above. Modes 0, 2, 5, 6 and 7 each held a frame of
       their kind; modes 3 and 4 have not been exercised.
     - M, I
     - M: captures in modes 0, 2, 5, 6 and 7: mode 0 latched within 12 s, storm
       and DoS drops in 5, a DoS trap in 6, reason 88 in 7 while reason 77
       caught nothing under the same traffic, a classifier drop in 2. I: modes 3
       and 4; a capture in each against traffic of one kind would confirm them.
       Not load-bearing.
   * - Writing mode none clears VALID. Arming therefore writes the word twice,
       mode none alone and then the whole setting, so that no capture predates
       the new setting. That a capture armed without the first write can return
       the previous frame is an assumption.
     - M, I
     - M: after valid captures, arming mode none made the next snapshot refuse.
       I: arming twice without mode none in between, with distinguishable
       frames, would show whether the second capture can hold the first frame.
   * - After a full chip reset and the boot loader the latch reads
       ``0x00201fe0``: mode 0, both port masks ``0xf``, VALID set. Setup
       therefore arms mode none with both masks ``0xf`` and reason 0, so that no
       capture predates the driver.
     - M
     - A register read after a reset.
   * - The forwarding reasons of the table above.
     - M
     - Each produced by its cause with a control: a frame of the shape of check
       n, a storm drop, a frame to the port it came from, an unmatched
       downstream frame under ``RTL9602C_CLS_CTRL`` bits 1:0 of 2.
   * - ``pktdbg_mode`` (at most 7), ``pktdbg_ingress_ports`` and
       ``pktdbg_egress_ports`` (four port bits, else refused as out of range)
       and ``pktdbg_reason`` are answered from the driver's copy of the armed
       setting, and any change re-arms with all four, because arming is one
       action with four inputs. Setup arms mode none, all four ports both ways,
       reason 0. The region ``frame_capture`` holds one snapshot of
       ``RTL9602C_PKTDBG_SNAPSHOT_WORDS`` (42) words, 168 bytes: the control
       word, the 28 parse words and the 13 verdict words, in CPU byte order. A
       snapshot is refused with ``-EAGAIN`` while VALID is clear, so a snapshot
       always describes a frame.
     - M, T
     - M: the commands above; on an empty capture ``devlink region new`` answers
       "Resource temporarily unavailable". T: the captures of the rows above
       were armed this way.

Arming the capture, ``rtl9602c_hsb_arm()``, in order:

1. Write ``RTL9602C_PKTDBG_LATCH`` with mode none. Purpose: it clears VALID, so
   that nothing captured before the new setting is kept. (M)
2. Write ``RTL9602C_PKTDBG_LATCH`` with the mode, both port masks and the reason
   at once. Purpose: arming is one action. (M)

The steps are checked by the captures of the rows above.

Egress descriptor capture
=========================

A second instrument holds the last frame descriptor on the egress side that
matches a configuration: the ports and, if set, a mode and a forwarding reason.
The descriptor of a PTP frame carries the frame's PTP fields. The instrument is
read and armed through the debugfs view ``hsd``. The PTP clock and the
timestamps are the *ptp* page's.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PKTDBG_DATA(i)`` (``0x02a070`` + 4i),
       ``RTL9602C_PKTDBG_DATA_WORDS`` (15) words, hold the last frame descriptor
       on the egress side that matches ``RTL9602C_PKTDBG_CFG`` (``0x02a0b4``):
       ``RTL9602C_PKTDBG_CFG_EGRESS_PORT`` (bits 2:0) and
       ``RTL9602C_PKTDBG_CFG_INGRESS_PORT`` (bits 5:3) name the ports,
       ``RTL9602C_PKTDBG_CFG_VALID`` (bit 16) is set while one is held. It holds
       one descriptor, the latest. Writing the configuration word clears VALID.
     - M
     - ``echo <word> > /sys/kernel/debug/rtl9602c/hsd`` naming each egress port
       in turn while PTP frames arrived: only port 3 held a PTP descriptor; an
       ingress port of 6, which does not exist, made it match nothing; a read
       after a fresh write held a fresh frame.
   * - Bit b of the captured descriptor is bit b mod 32 of word b/32, word 0 at
       the lowest address. The PTP fields: seconds at
       ``RTL9602C_PKTDBG_PTP_SEC_LSB`` (92), ``RTL9602C_PKTDBG_PTP_SEC_BITS``
       (48) wide; nanoseconds at ``RTL9602C_PKTDBG_PTP_NSEC_LSB`` (140),
       ``RTL9602C_PKTDBG_PTP_NSEC_BITS`` (32) wide; the PTP flag at
       ``RTL9602C_PKTDBG_PTP_FLAG_LSB`` (181), set for a PTP frame.
     - M
     - A PTP frame that the switch sent to the CPU read 20323.098509680 in the
       capture and the same time, to the nanosecond, in its receive timestamp
       through ``SO_TIMESTAMPING``; the reverse word order gives 0.000004096.
   * - ``RTL9602C_PKTDBG_PTP_ID_LSB`` (172, ``RTL9602C_PKTDBG_PTP_ID_BITS`` 7)
       and ``RTL9602C_PKTDBG_PTP_ACTION_LSB`` (179,
       ``RTL9602C_PKTDBG_PTP_ACTION_BITS`` 2) are the frame's PTP message
       identifier and egress PTP action.
     - I
     - The identifier read 1 and the action 0 under all four egress actions. A
       PTP frame forwarded out of a wire port under a known action would settle
       them. Not load-bearing: they are only printed.
   * - ``RTL9602C_PKTDBG_CFG_MODE`` (bits 7:6) and
       ``RTL9602C_PKTDBG_CFG_REASON`` (bits 15:8) narrow the capture by mode and
       forwarding reason.
     - I
     - They have not been exercised apart. Capturing with a reason that only
       some of the traffic carries would confirm the reason field. Not
       load-bearing: they are written as given.
   * - For a PTP frame that the CPU transmits, the descriptor carries the PTP
       flag and an identifier but a time of 0 under every egress action; for a
       PTP frame that the switch sends to the CPU it carries the time (see the
       *ptp* page).
     - M
     - All four egress actions.
   * - ``hsd`` (debugfs, mode 0600): reading prints the 15 raw words, the
       configuration word decoded, the PTP fields, and the switch clock read at
       the same moment (see the *ptp* page); writing a number (decimal,
       hexadecimal with ``0x``, octal with ``0``) stores it in
       ``RTL9602C_PKTDBG_CFG``, which re-arms the capture and changes no
       forwarding.
     - M
     - ``echo 0x2 > /sys/kernel/debug/rtl9602c/hsd; cat
       /sys/kernel/debug/rtl9602c/hsd``.

Setup for these blocks
======================

``rtl9602c_setup()`` writes these blocks in the order below; the whole of setup
is on the *main* page ("Setup, in order"), which names each of these steps. A
warm reboot or a reload leaves the switch as it was, so each step writes the
state that the driver depends on.

1. Unmask the meter and ACL sources of the switch interrupt with the link
   source (``RTL9602C_IRQ_SRC_METER`` and ``RTL9602C_IRQ_SRC_ACL``, in the
   interrupt setup). Purpose: an exceeded meter and an ACL hit are events. (T)
2. Mark the DSCP table switch-wide (``dscp_prio_mapping_is_global``) and give
   apptrust no owner. Purpose: one map serves the switch, and the first port to
   set a trust order becomes the owner of the weights. (T)
3. Read ``RTL9602C_CLS_CTRL_LAYOUT1_START`` as the usable classifier depth and
   log it (``classifier: 128 entries usable of 256``). Purpose: only the entries
   below the boundary are written. (M)
4. Write ``RTL9602C_DOS_CHECK_CTRL``, ``RTL9602C_DOS_PORT_EN`` and the three
   flood thresholds to 0 and zero the driver's copy of the thresholds. Purpose:
   the checks start off, whatever the chip held before the driver started. (M)
5. Write all seven storm-control enables and meter selectors to 0, reset every
   meter and mark it free, forget every class's meter, and write all sixteen
   bits of ``RTL9602C_METER_EXCEEDED``. Purpose: no limit or latched exceed the
   chip held before the driver started survives; a latch left from before would
   read as a policed drop that never happened. (M)
6. Write the ACL, the classifier, the priority maps, the policer, the scheduler
   and the shaper as a full chip reset leaves them
   (``rtl9602c_filter_qos_reset()``, with the values of "Reach and state"):
   every ACL rule and classifier entry retired, ACL lookup and its default
   permits off, unmatched frames let through, and no limit on any port.
   Purpose: no rule, entry, map or limit that an earlier kernel wrote stays in
   force with no record of it in the driver. (M)
7. Point field selectors 0 and 1 at the L4 source and destination ports.
   Purpose: ACL rules can match a port. (M)
8. Set ``RTL9602C_RSVDMC_CTRL_STORM_EXEMPT`` on the five control-protocol
   addresses, by read-modify-write. Purpose: a flood limit does not police them.
   (M)
9. Switch loop detection off and clear its latches. Purpose: a loaded driver
   must not start sending probes by itself. (T)
10. Arm the frame capture in mode none, all four ports both ways, reason 0.
    Purpose: no capture predates the driver. (M)
11. Register the devlink parameters and create the ``frame_capture`` region. A
    failure is logged (``devlink parameters unavailable``, ``no devlink
    region``) and setup carries on. (M)
12. Clear the per-port stream entries and refresh the bridge state, which
    rebuilds the stream entries; with no port bridged yet it installs none.
    Purpose: the entries follow the bridge (see "The classifier"). (T)

The steps are checked as follows. Step 1 by the meter interrupt: an exceeded
meter is logged and counted in ``/proc/interrupts``. Step 2 by the scheduling
checks, whose two classes differ only in DSCP. Step 3 by the boot log line. Step
4 by a check enabled before a reload reading off after it, and step 5 by the
classes, meters and latches read after a reload and after a full chip reset.
Step 6 by ``qos`` and ``cf`` after a warm reboot from a kernel that had left
rules, entries, maps and limits in force (see "Reach and state"). Step 7 by the
port matches that drop their traffic, and step 8 by the frame capture in drop
mode. Step 9 by the board booting with the block off, step 10 by the latch
register read after a reset and armed in mode none, and step 11 by the
parameters and the region being there again after a reload. Step 12 by bridged
upstream traffic and a routing test, which find the entries in ``cf``.

Measured, not driven
====================

The driver does not program these; they are recorded because its choices rest
on them. They were measured with register writes made outside the driver.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A per-queue egress rate exists: per port, a word at ``0x02d86c`` + 4p
       (``RTL9602C_QUEUE_RATE_EN``) with one enable bit per queue and a word at
       ``0x02d87c`` + 4p (``RTL9602C_QUEUE_RATE_METER``) with a 3-bit meter
       index per queue, binding a queue to one of meters 0 to 7. It limits
       packets per second, per queue, so it has no form in bytes for ``tc``,
       and a tbf in an ets band is refused rather than applied to the whole
       port. Setup writes both words 0 on every port.
     - M
     - Frames from the CPU at the priority of queue 5: 500 packets/s asked, 511
       measured; queue 2 of the same port unaffected at the same time; clearing
       the enable restored the rate.
   * - The CPU port's queues are not limited by it, so it cannot keep a flood
       to the CPU below what the CPU's receive engine takes.
     - M
     - Queues 0 to 6 of port 3 bound to a meter at 100 packets/s, with the
       enables set: a flood of 272,000 frames/s to the stick's address put
       324,000 frames into the CPU in 10 s and 2,398,000 into its engine's
       missed count, as many as with no limit; the flood went through queue 0
       (``qdepth``).
   * - The range-checker banks of the ACL: 8 VLAN-ID checkers at ``0x0151cc``
       (one word each: type bits 25:24, upper bound 23:12, lower 11:0), 32
       IP-address checkers at ``0x0151ec`` (five words each), 32 L4-port
       checkers at ``0x01546c`` (two words each, type at +0 and both bounds at
       +4), 8 packet-length checkers at ``0x01556c`` (one word each: type bit
       28, upper bound 27:14, lower 13:0). The VLAN-ID bank (type 1: inside the
       range) and the packet-length bank (type 0: inside, 1: outside) deliver
       their bitmap to a rule; the L4-port and IP-address banks deliver a bitmap
       of 0 at every type value and slot code tried. So the driver matches L4
       ports through the field selectors.
     - M
     - After a full chip reset, 20 frames per case from the host, in and out of
       range, each measurement with a control rule that dropped.
   * - An egress PCP remark exists: a per-port enable at ``0x02a058`` and eight
       3-bit priority-to-PCP entries at ``0x02a05c``. On this board its effect
       cannot be observed: a frame that the CPU sends carries its own tag
       decision in its transmit descriptor, and frames that the switch sends to
       the CPU arrive untagged.
     - M
     - Enabled and mapped (priority 5 to PCP 7): the registers read back as
       written and no frame changed.

Known limitations
=================

- A flower rule whose destination address is all ones or the group bit alone, or
  whose EtherType is exactly ARP with no VLAN match, is a storm-control rule, and
  with any action but ``police`` it is refused, so the broadcast address, the
  group bit and ARP cannot be dropped or trapped by an ACL rule.
- Of several storm-control rules for one class and port only the one added last
  polices, although ``tc`` lists every one of them as offloaded.
- The driver reports no statistics for offloaded flower rules (it has no
  ``cls_flower_stats`` operation), so ``tc -s filter show`` counts no hits for a
  rule that the switch applies. The effect of a rule is seen in the frames that
  arrive and in the frame capture.
- A read of the flood-threshold registers of the DoS checks, ``0x026008``,
  ``0x02600c`` and ``0x026010``, by any means other than the driver's, for
  instance through a mapping of the register window, has stalled the board until
  the soft-lockup detector reset it. The driver never reads them.

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
   * - The S-tag bit of a classifier rule is the S-tag flag, so the VLAN entries
       that compare it as 0, which match single-tagged frames, refuse S-tagged
       ones ("Entries and rules").
     - An S-tagged frame through a translation entry.
   * - Per-VLAN stream entries work for more than one upstream GEM interface,
       and an upstream translation carries the stream of the GEM interface that
       claims its VLAN ("What the driver writes").
     - Two upstream GEM interfaces in two VLANs, each VLAN's frames arriving in
       its own GEM port at the far side of an OLT: the test of the *bridging*
       page.
   * - The entries from ``RTL9602C_CLS_CTRL_LAYOUT1_START`` up use another rule
       layout, and the driver's depth limit is right ("Entries and rules").
     - A frame test of an entry above the boundary.
   * - The multicast, ARP, DHCP and IGMP storm-control classes police their own
       frames ("Storm control").
     - Each class offered its own frames at a set rate.
   * - Of several sources that apply to a frame, the one with the heaviest
       weight decides its priority ("Priority selection and the DCB maps").
     - A tagged frame with a PCP and a DSCP that map to different priorities,
       captured under both orders of ``dcb apptrust``.
   * - The weight slots of PCP and of the port are the ones named
       ("Priority selection and the DCB maps").
     - A tagged frame classified by PCP under ``dcb apptrust ... order pcp``,
       and an untagged one by the port default priority, captured with their
       priorities.
   * - The default priority of a port applies as described, the port source's
       weight being set to 1 when it is 0 ("Priority selection and the DCB
       maps").
     - An untagged frame without DSCP trust on a port with default priority 5,
       captured with priority 5.
   * - Ports 1 to 3 have eight working queues ("Egress queues and the
       scheduler").
     - The scheduling traffic on a second port.
   * - The flow-control threshold registers are laid out as named
       ("Flow-control thresholds").
     - Reading the global and PON words after bring-up.
   * - The PON port needs its thresholds at the global ones plus ``0x30``
       ("Flow-control thresholds").
     - Repeating the bridged-traffic check with the write removed.

Questions the code does not depend on
-------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether an ACL rule can raise the ACL interrupt, and whether
       ``RTL9602C_ACL_HIT_STAT_TMPL`` latches which template's rule raised it
       and is cleared by writing ones ("Actions"; the interrupt is also a
       question on the *main* page).
     - A rule whose action raises the interrupt.
   * - Whether a field selector cuts its window from a frame that has no L4
       header ("The parser's field selectors").
     - A port-only rule compared against ARP frames with a chosen value at that
       offset.
   * - Whether a frame from the SFP port to the CPU crosses neither classifier
       direction ("What the driver writes").
     - A frame across the PON, translated by an entry.
   * - Whether the DHCP and IGMP storm-control classes are wider than one flower
       rule ("Storm control").
     - The classes offered DHCP server, DHCP client and MLD frames.
   * - Whether a zero in ``RTL9602C_SHAPE_PORT_CFG_RATE`` passes nothing ("The
       egress shaper").
     - Writing 0 and watching the port stop.
   * - Whether the DoS flood thresholds count frames per a hardware window, and
       of what length ("Malformed-frame (DoS) checks").
     - Sweeping the offered rate against a fixed threshold on a quiet link.
   * - Whether every read of a flood-threshold register is unsafe
       ("Malformed-frame (DoS) checks").
     - A single read through a driver debug path after a write.
   * - Whether check 0 of the DoS checks fires at all ("Malformed-frame (DoS)
       checks").
     - No frame can show it, because forwarding drops such a frame first.
   * - Whether a returning loop-detection probe with the same magic and another
       switch's identifier counts as a loop ("Loop detection").
     - Replaying a probe with a changed identifier.
   * - Whether ``RTL9602C_LOOPDET_PEER_PORT`` names the right port for a loop
       between two different ports ("Loop detection").
     - Looping two ports to each other.
   * - Whether capture modes 3 and 4 capture the first forwarded and the first
       trapped frame ("Frame capture").
     - A capture in each mode against traffic of one kind.
   * - Whether a capture armed without mode none first can return the previous
       frame ("Frame capture").
     - Arming twice without mode none in between, with distinguishable frames.
   * - Whether the PTP identifier and action fields of the egress descriptor are
       where they are taken to be ("Egress descriptor capture").
     - A PTP frame forwarded out of a wire port under a known action.
   * - What the mode and reason fields of the egress capture's configuration do
       ("Egress descriptor capture").
     - Capturing with a reason that only some of the traffic carries.
