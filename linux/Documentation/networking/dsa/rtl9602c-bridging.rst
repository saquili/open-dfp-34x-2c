.. SPDX-License-Identifier: GPL-2.0-only

==============================================================
Realtek RTL9602C switch: bridging, VLANs and the address table
==============================================================

Overview
========

The switch core of the RTL9602C forwards frames between its four ports. It looks
the destination address up in an address table keyed by address and VLAN, keeps
a table of VLAN members and forwards by it when VLAN filtering is on, restricts
each ingress port to a set of egress ports (port isolation), obeys a
spanning-tree state for each port, and floods a frame whose destination it does
not know to the ports that may receive it. The CPU takes no part in forwarding
between bridged ports. This page describes how the driver sets these functions
up and what the hardware does with what the driver writes: the port masks, the
indirect table engine, the address table, flushing, learning, ageing, the
spanning-tree state, isolation, flooding, bridge membership, VLANs and the PVID,
port mirroring, the S-VLAN and reserved-address controls, the classifier entries
that translate VLANs and give bridged ports a GEM stream, and the address-table
entries on which the L3 engine depends.

The other pages of the port are named by short names on this page:

- *main*, Documentation/networking/dsa/rtl9602c.rst: the ports, the DSA
  integration and the tag protocol, the CPU tag, the frame length limit, the
  switch interrupt and a code map of the whole driver.
- *acl-qos*, Documentation/networking/dsa/rtl9602c-acl-qos.rst: the ACL, the
  classifier's entry format, meters, storm control and the quality of service.
- *l3-nat*, Documentation/networking/dsa/rtl9602c-l3-nat.rst: the L3/NAT engine
  and its tables.
- *PON*, Documentation/networking/device_drivers/pon/rtl9602c.rst: the GPON MAC
  and the PON MAC, with its GEM streams.
- *PON class*, Documentation/networking/pon.rst: the GEM interfaces as network
  interfaces.
- *devlink*, Documentation/networking/devlink/rtl9602c.rst: the frame capture
  region.

How the driver bridges
----------------------

The driver bridges with the switch's own tables and masks. Six things carry the
function:

- Port isolation carries bridge membership. The egress mask of each user port
  names the CPU port together with the first extension port, and the other user
  ports of its bridge. ``rtl9602c_bridge_refresh()`` computes the masks again on
  every membership change (see "Bridge membership").
- The three flood masks follow membership. A user port receives flooded frames
  only while it shares a bridge with another port of the switch (see
  "Flooding").
- The address table is written through the hash mode of the indirect table
  engine only. The switch learns source addresses itself, the driver adds static
  entries and multicast groups, and setup empties the table (see "The address
  table (FDB)").
- VLAN filtering is one bit for the whole switch. The VLAN table has 4096
  entries indexed by VID, and each port has a PVID (see "VLAN membership,
  filtering and PVID").
- The PON port is bridged through its GEM interfaces and not itself. All GEM
  interfaces share one bridge, and classifier entries give the frames that the
  switch forwards to the PON port their GEM stream (see "Bridging the GEM
  ports").
- The L3 engine borrows the address table: a next hop names an entry of it (see
  "The layer-2 side of routing").

Operations and files
--------------------

The driver implements these operations of ``struct dsa_switch_ops`` for the
function, all in ``rtl9602c-l2.c`` except where the table says otherwise, and
the host operations that the PON driver's bridge operations of the PON class
(in its ``rtl9602c-gem.c``) are passed on to, in ``rtl9602c-pon-host.c``. Every
name is prefixed ``rtl9602c_``.

.. list-table::
   :header-rows: 1
   :widths: 46 54

   * - Operations
     - Described under
   * - ``port_bridge_join``, ``port_bridge_leave``
     - "Bridge membership".
   * - ``port_pre_bridge_flags``, ``port_bridge_flags``
     - "Learning and the learning limit" and "Flooding".
   * - ``port_stp_state_set``
     - "Spanning-tree state".
   * - ``set_ageing_time``
     - "Ageing".
   * - ``port_fdb_add``, ``port_fdb_del``, ``port_fdb_dump``, ``port_mdb_add``,
       ``port_mdb_del``
     - "The address table (FDB)".
   * - ``port_fast_age``
     - "Flush and fast ageing".
   * - ``port_vlan_filtering``, ``port_vlan_add``, ``port_vlan_del``
     - "VLAN membership, filtering and PVID".
   * - ``port_change_mtu``, ``port_max_mtu``
     - "Frame length".
   * - ``port_mirror_add``, ``port_mirror_del``
     - "Mirroring".
   * - ``cls_flower_add``, ``cls_flower_del`` (``rtl9602c-offload.c``; the rules
       that drop, trap or police are on the *acl-qos* page)
     - "VLAN tag operations".
   * - ``gem_bridge_join``, ``gem_bridge_leave``, ``gem_vlan_add``,
       ``gem_vlan_del`` (host operations, ``rtl9602c-pon-host.c``, called by the
       operations of the PON class of the same names)
     - "Bridging the GEM ports".
   * - ``gem_mrouter`` (a host operation, ``rtl9602c-pon-host.c``, called by the
       operation of the PON class)
     - "Multicast snooping".

The files that hold the rest of this page:

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - File
     - What of this page it holds
   * - ``drivers/net/dsa/realtek/rtl9602c-l2.c``
     - The address table, flush, learning, ageing, spanning-tree state,
       isolation, flooding, unregistered multicast (``rtl9602c_mcast_reset()``,
       ``rtl9602c_mcast_refresh()``), bridge membership, VLANs and the PVID,
       frame length, mirroring, the address-table entries that the L3 engine
       depends on, and the debugfs view ``fdb``.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - The indirect table engine (``rtl9602c_table_op()``,
       ``rtl9602c_table_read()``, ``rtl9602c_table_write()``) and its self-test,
       the subscription to the learning-limit alarm
       (``rtl9602c_lrn_over_intr_sync()``), and the parts of
       ``rtl9602c_setup()`` that empty the address table, clear port
       mirroring, exempt reserved addresses from storm control and set the
       standalone forwarding state.
   * - ``drivers/net/dsa/realtek/rtl9602c-offload.c``
     - The VLAN tag operations (``rtl9602c_flower_vlan()``,
       ``rtl9602c_cf_write_xlate()``) and the S-VLAN block they need
       (``rtl9602c_svlan_setup()``), the classifier entries that give bridged
       ports a GEM stream (``rtl9602c_cf_sid_refresh()``), the ACL rules that copy
       IGMP and MLD to the CPU (``rtl9602c_acl_snoop_setup()``), and the
       debugfs views ``l2learn``, ``svlan`` and ``rma``.
   * - ``drivers/net/dsa/realtek/rtl9602c-pon-host.c``
     - The bridge half of the GEM interfaces: the one bridge they share, the
       VLANs and priorities each flow claims, and the addresses the CPU takes
       for them, kept for the PON driver.
   * - ``drivers/net/pon/rtl9602c/rtl9602c-gem.c``
     - The GEM interfaces' bridge operations of the PON class, which pass on
       to the host what the switch keeps.
   * - ``drivers/net/dsa/realtek/rtl9602c.h``
     - The registers and entry layouts of this page, ``struct rtl9602c_lookup``
       and the state of the driver that it uses (``flood_req``, ``mc_drop``,
       ``pon_br``, ``l34_nh_ports``).

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Symbol
     - Effect
   * - ``CONFIG_NET_DSA_RTL9602C``
     - Builds everything on this page, as described on the *main* page.
   * - ``CONFIG_BRIDGE``
     - The bridge. Bridged forwarding, the learning and flood flags and the
       spanning-tree state come from it. ``rtl9602c_defconfig`` sets it.
   * - ``CONFIG_BRIDGE_VLAN_FILTERING``
     - VLAN filtering and the bridge's VLANs. ``rtl9602c_defconfig`` sets it.
   * - ``CONFIG_BRIDGE_IGMP_SNOOPING``
     - Multicast snooping and groups: ``bridge mdb``, the calls to
       ``port_mdb_add`` and ``port_mdb_del`` and the ACL rules of "Multicast
       snooping" need it. ``rtl9602c_defconfig`` sets it.
   * - ``CONFIG_VLAN_8021Q``
     - VLAN devices. The 8021q core adds VID 0 to the receive filter of a device
       that has VLAN filtering (see "Known limitations").
   * - ``CONFIG_NET_SCH_INGRESS``, ``CONFIG_NET_CLS_FLOWER``,
       ``CONFIG_NET_CLS_ACT``, ``CONFIG_NET_ACT_VLAN``
     - The VLAN tag operations: a flower rule with ``vlan modify``, ``push``
       or ``pop`` on the ingress of a user port.
   * - ``CONFIG_NET_CLS_MATCHALL``, ``CONFIG_NET_ACT_MIRRED``
     - Port mirroring: a matchall rule with a mirred action.
   * - ``CONFIG_PON``
     - The GEM interfaces, which are the bridge ports of the PON port.
       ``CONFIG_NET_DSA_RTL9602C`` selects it.
   * - ``CONFIG_DEBUG_FS``
     - The views ``fdb``, ``l2learn``, ``svlan`` and ``rma``.

Device tree
-----------

Nothing on this page is described in the device tree beyond the ``switch`` node
and its ports, with the compatible ``realtek,rtl9602c-switch``, which the *main*
page describes. No property configures bridging, VLANs or the address table.

Interfaces to user space
------------------------

- ``ip link set sfp master br0`` puts a user port in a bridge. The GEM
  interfaces of the PON class are put in a bridge the same way. ``pon`` itself
  cannot be offloaded into a bridge (see "Bridge membership").
- ``bridge link set dev sfp`` with ``learning``, ``flood``, ``mcast_flood``,
  ``bcast_flood`` and ``state`` sets the learning limit, the three flood masks
  and the spanning-tree state of a port. ``ip link set br0 type bridge`` with
  ``ageing_time`` and ``vlan_filtering`` sets the ageing time and the VLAN
  awareness of the whole switch.
- ``bridge vlan`` adds and removes VLAN members and sets the PVID.
  ``bridge fdb`` adds and removes static entries, and ``bridge fdb show`` lists
  the switch's own entries as ``self`` entries. ``bridge mdb`` manages multicast
  groups.
- ``tc`` rules on ``sfp`` or ``pon``: a flower rule with ``vlan modify``,
  ``push`` or ``pop`` changes the VLAN tags of the frames it matches, and a
  matchall rule with a mirred action mirrors a port (see "VLAN tag operations"
  and "Mirroring").
- Four read-only debugfs views in ``/sys/kernel/debug/rtl9602c/``, ``fdb``,
  ``l2learn``, ``svlan`` and ``rma`` (see "The read-only views").
- ``ethtool -S`` reports ``rx_forward_discards`` for a port: the frames that the
  switch dropped in forwarding.
- The kernel log lines of setup that begin ``rtl9602c-switch:``, among them the
  result of the table self-test and the number of addresses left in the table by
  the previous kernel.

How to read the fact tables
===========================

Each block below has a short account of what the hardware does and what the
driver does with it, followed by fact tables with three columns: *Fact*, *Tag*
and *Checked by*. The tag says how the fact is established. **M**, measured on
the board (the DFP-34X-2C3): *Checked by* gives the command or observation,
through the driver's own interfaces (the kernel log, ``ip``, ``bridge``,
``ethtool``, debugfs), that shows it. **T**, tested by function: the driver
depends on the fact, and the named behaviour fails if it is wrong. **P**, public
standard: *Checked by* names the document and the clause. **I**, inferred, not
verified: the fact is an assumption, and *Checked by* says what would confirm or
refute it. The last chapter collects these facts and says which of them the
driver depends on.

A row with two tags is established in two ways, or in two parts; *Checked by*
says which part carries which tag. Register offsets are from the base of the
switch's register window, physical ``0x1b000000``. Values are 32-bit words as
``readl()`` and ``writel()`` see them on this big-endian CPU. The bits of a
table entry are numbered from bit 0 of word 0, so bit 0 of word 2 is entry bit
64. Port numbers are the hardware's (0 ``sfp``, 1 not wired, 2 ``pon``, 3 the
CPU port; see the *main* page), and "the host" is the equipment at the far end
of the SFP lane. The behaviours that a tested fact depends on are named in
words: bridged traffic passes both ways, sourced from the host; VLAN filtering,
FDB learning and ageing work; a routing test sees a translated source address on
the wire. Addresses in the examples are from the documentation range
``00:00:5E:00:53:xx``.

Port masks: four ports and seven extensions
===========================================

Several forwarding blocks describe a set of ports as an eleven-bit mask: the
port isolation masks, the VLAN entries and the multicast entries of the address
table. The first extension port matters on this board even though nothing is
wired to it. Not every mask has the extension half: the three flood masks and
the flush's port set have one bit per port, and the mirror's source set names
the four ports only.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The port masks of the forwarding blocks are eleven bits: bits 3:0 are the
       four ports, bits 10:4 seven extension ports. No extension port is wired
       on this board, but the first one (bit 4) lies on the path to the CPU: a
       frame reaches the CPU port only if a mask names the CPU port and the
       first extension port together. This holds in the isolation mask (see
       "Port isolation") and in VLAN membership (see "VLAN membership, filtering
       and PVID"). The driver writes both bits wherever it names the CPU.
     - M
     - One bit at a time, with pings from the host: isolation with bits 3 and 4
       passed 6 of 6 and either alone 0 of 6; a VLAN entry with the CPU member
       bit and extension 0 passed 8 of 8 and either alone 0 of 8.

The indirect table engine
=========================

The address table, the VLAN table and the rule and action tables of the ACL and
of the classifier are not memory mapped. One engine reaches all six: the driver
loads the data registers, writes a command that names the table, the entry and
how the entry is found, waits until the engine is no longer busy, and reads the
status and, for a read, the data registers. The driver serialises every access
with one lock, ``table_lock``.

.. list-table::
   :header-rows: 1
   :widths: 16 50 34

   * - Table
     - Macro
     - Holds
   * - 0
     - ``RTL9602C_TBLACC_CMD_TABLE_FDB``
     - the address table
   * - 1
     - ``RTL9602C_TBLACC_CMD_TABLE_VLAN``
     - the VLAN table
   * - 2
     - ``RTL9602C_TBLACC_CMD_TABLE_ACL_RULE``
     - the ACL rules
   * - 3
     - ``RTL9602C_TBLACC_CMD_TABLE_ACL_ACT``
     - the ACL actions
   * - 4
     - ``RTL9602C_TBLACC_CMD_TABLE_CLS_RULE``
     - the classifier rules
   * - 5
     - ``RTL9602C_TBLACC_CMD_TABLE_CLS_ACT``
     - the classifier actions

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The six tables listed above sit behind one access engine. They share one
       command register, one status register and one set of data registers, so
       the driver runs every access under one lock, ``table_lock``: a command
       written while another caller reads its result would hand that caller the
       wrong entry. The ACL and classifier entry formats are on the *acl-qos*
       page.
     - T
     - VLAN filtering, FDB learning and ageing work (the address and VLAN
       tables); the ACL and classifier functions of the *acl-qos* page.
   * - ``RTL9602C_TBLACC_CMD`` (``0x012000``): writing it starts an operation.
       ``RTL9602C_TBLACC_CMD_TABLE`` (bits 2:0) selects the table,
       ``RTL9602C_TBLACC_CMD_WRITE`` (bit 3) is set for a write and clear for a
       read, ``RTL9602C_TBLACC_CMD_MODE`` (bits 6:4) says how the entry is
       found, and ``RTL9602C_TBLACC_CMD_INDEX`` (bits 20:9, twelve bits) is the
       entry index.
     - M
     - The self-test at every setup (see "The self-test at every setup") logs
       ``indirect table port ok`` when the index field is where the driver
       expects it. Negative control: a test build with the index field at bits
       23:12 made the same test report aliasing and name the index field.
   * - ``RTL9602C_TBLACC_CMD_MODE_INDEX`` (1): the engine goes to the entry that
       the index names. The VLAN, ACL and classifier tables are accessed only
       this way. The driver reads the address table this way when it walks it,
       and never writes it this way (see "Placing, changing and deleting through
       the hash").
     - T
     - VLAN filtering, FDB learning and ageing work; the table self-test.
   * - ``RTL9602C_TBLACC_CMD_MODE_HASH`` (0): the index is ignored. The engine
       hashes the key held in the data registers and goes to the slot that the
       key belongs in. A read returns the entry found there; a write puts the
       entry there or, with ``RTL9602C_FDB_W2_VALID`` clear, deletes the entry
       that the key names. The driver adds, changes and deletes address-table
       entries only this way.
     - M
     - A static entry added and deleted this way stopped and restored the
       flooding of frames from the host to the CPU (5, 0, 0, 0, then 5 of 5
       received); a multicast group's member set read and rewritten this way
       stayed in one slot throughout.
   * - ``RTL9602C_TBLACC_STAT`` (``0x012004``): ``RTL9602C_TBLACC_STAT_BUSY``
       (bit 13) is set while the engine works; ``RTL9602C_TBLACC_STAT_HIT`` (bit
       12) says that the slot the operation reached holds an entry (a read) or
       took the entry (a hashed write); ``RTL9602C_TBLACC_STAT_INDEX`` (bits
       9:0) is the index the operation settled on. The driver hands hit, CAM
       flag and index back to its caller in ``struct rtl9602c_lookup``.
     - M
     - In a by-index scan of the whole address table only the three slots
       holding real addresses set HIT; a hashed add, update and delete of one
       address reported the same index each time. ``bridge fdb show`` lists only
       real hardware entries, because the dump skips a slot without HIT.
   * - ``RTL9602C_TBLACC_STAT_CAM`` (bit 10) is set when the entry that a
       hashed operation reached is in the 64-entry CAM and not in a hash
       bucket; ``RTL9602C_TBLACC_STAT_INDEX`` then holds n for entry 1024 + n.
       The driver records it and decides nothing on it.
     - M, I
     - M: with the CAM on (see "Structure"), a hashed lookup and a hashed
       delete of each of two entries in the CAM, entries 1024 and 1025, set it
       with index 0 and 1. I: that a hashed placement into the CAM sets it.
   * - ``RTL9602C_TBLACC_WDATA(i)`` (``0x012008`` + 4i) and
       ``RTL9602C_TBLACC_RDATA(i)`` (``0x01201c`` + 4i), i = 0 to 4, carry the
       entry in and out in natural order: word 0 holds entry bits 31:0, word 1
       bits 63:32, word 2 bits 95:64. A table uses as many words as its entries
       have: the address table 3, the VLAN table 1, an ACL rule 5. On a hashed
       operation the write registers carry the key (a read) or the whole entry
       (a write).
     - M
     - Learned entries read by index decode, with word 0 as the low address
       bits, to the right address, VLAN and port (see "Entry layout").
       ``bridge fdb show`` shows them.
   * - The driver polls ``RTL9602C_TBLACC_STAT_BUSY`` every 10 microseconds and
       gives up after 1 ms with ``-ETIMEDOUT`` and the message ``table <t> addr
       <i>: engine still busy after 1 ms``: an engine that is still busy then is
       not answering, and waiting for ever would take the console with it.
     - T
     - VLAN filtering, FDB learning and ageing work, so every access completes;
       a timed-out access fails with the message.

One table access
----------------

``rtl9602c_table_op()``, in order:

1. For a write, or for any hashed access, load ``RTL9602C_TBLACC_WDATA(0..n-1)``
   with the entry or the key. Purpose: the engine reads the entry, or the key
   that it hashes, from there. (T)
2. Write ``RTL9602C_TBLACC_CMD`` with the index, the mode, the table and, for a
   write, ``RTL9602C_TBLACC_CMD_WRITE``. Purpose: writing the command starts the
   operation. (T)
3. Poll ``RTL9602C_TBLACC_STAT_BUSY`` until it is clear (see the last row of the
   table above). Purpose: the result is not valid before. (T)
4. Take hit, CAM flag and index from ``RTL9602C_TBLACC_STAT``. Purpose: whether
   a slot is occupied is told only here (see "Entry layout"). (M)
5. For a read, read ``RTL9602C_TBLACC_RDATA(0..n-1)``. Purpose: the entry. (M)

The self-test at every setup
----------------------------

``rtl9602c_table_selftest()`` runs in setup, after the address table has been
emptied, before any bridge exists and with VLAN filtering off, so that no VLAN
entry is in use. It checks that the engine reaches the VLAN entries where the
driver expects them. With the index field misplaced two indices alias, which
only a pattern written to one and read back from the other shows. Its result is
one line in the kernel log, and setup goes on whatever the result is, because
the address and VLAN tables depend on the engine. Steps:

1. Save 13 VLAN entries: VIDs 2, 3, 4, 8, 16 and each power of two up to 2048,
   and 4094. Purpose: together they set every one of the twelve index bits. VIDs
   0, 1 and 4095 are never touched (see "VLAN membership, filtering and PVID").
   (T)
2. Write all ones to the first entry and read it back; what survives is the
   writable mask. Purpose: a VLAN entry is narrower than 32 bits, so a fixed
   pattern could not tell a narrow entry from a wrong index. It reads
   ``0x0003ffff``: 18 bits. (M)
3. Refuse with ``-EIO`` if fewer than 12 bits are writable. Purpose: too few
   bits to tell the entries apart. (T)
4. Derive one pattern for each entry inside the mask and check that the patterns
   are pairwise different. Purpose: an alias between two equal patterns would go
   unseen. (T)
5. Write every pattern before reading any, then read each one back. Purpose:
   with the index field misplaced two indices alias and the second read returns
   the first's pattern, which a write followed by its own read-back cannot show.
   (M)
6. Restore every saved entry. Purpose: the test leaves the table as it found it.
   (T)
7. Log the result. Purpose: the address and VLAN tables depend on the engine.
   (M)

The log line of a passing test is ``indirect table port ok: 13 VLAN entries
addressed distinctly, entry is 18 bits wide (0003ffff)``. After a failing test
setup logs ``the indirect table port did not answer as expected; the FDB and
VLAN table cannot be trusted``.

The address table (FDB)
=======================

The address table maps an address and a VLAN to a port (a unicast entry) or to a
set of ports (a multicast group). The switch learns the source addresses of the
frames that it sees and enters them itself; those entries age out (see
"Ageing"). The driver adds static entries for ``bridge fdb add`` and for the
addresses that the L3 engine needs, and groups for ``bridge mdb``. The table has
no plain memory interface. The driver reads it slot by slot by index, for a dump
and to empty it, and it adds, changes and deletes entries only through the
engine's hash mode, where the engine finds the slot that the key belongs in.

Structure
---------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The address table is table 0 of the engine: 1088 entries of
       ``RTL9602C_FDB_ENTRY_WORDS`` (3) words. Entries 0 to 1023 are hashed: 256
       buckets of four slots each, ``slot = 4 * bucket + way``. An empty hashed
       slot reads back with its own bucket number in its address words. Entries
       1024 to 1087 are 64 more entries, a CAM.
     - M
     - A by-index scan of all 1088 entries: empty slots 0 to 3 read address 0,
       slots 4 to 7 address 1, and so on.
   * - With the CAM off, as out of reset (next row), a CAM entry cannot be
       maintained: a by-index write to it reports HIT and changes nothing, the
       hashed search never matches it, and no flush mode reaches it, so it
       stays until power is removed; a warm restart keeps it. Entries do land
       there: multicast groups written while their VLAN was in shared learning
       did. Setup therefore turns the CAM on before it empties the table (see
       "Emptying the table at setup"), and every walk reads all
       ``RTL9602C_FDB_ENTRIES`` (1088).
     - M
     - Groups written in shared learning, then targeted by an index write, by
       the hashed search and by all three flush modes. Entries put in the CAM
       by the test of the next row, still there after three RAM boots.
   * - ``RTL9602C_FDB_CFG_CAM_OFF``, bit 21 of ``RTL9602C_FDB_CFG``, turns the
       CAM off; it reads 1 out of reset (the register reads ``0x00600bb8``),
       and setup clears it. While it is set, a hashed write to a full bucket is
       not taken, and a hashed lookup does not find an entry in the CAM. With
       it clear, such a write lands in the CAM and the switch forwards frames
       by the entry; a hashed lookup with an entry's own words finds it there,
       and a hashed delete with them removes it. A by-index write is taken
       under neither setting.
     - M, I
     - M: the reset value read after a watchdog reset with the switch driver
       not loaded, and on a running board. Five static entries whose keys share
       a bucket: with the bit set the fifth was not placed, with it clear it
       was in the CAM. Frames from the PON side to it, for 1 s, reached the
       SFP port with 20 copies at the CPU, where an unknown address put 43,000
       there. An entry's words read by index and written back through the
       hash: with the bit set the lookup missed; with it clear it found the
       entry and, with VALID clear, deleted it (``RTL9602C_TBLACC_STAT_CAM``
       under "The indirect table engine"). With the bit clear, index writes of
       a deletion and of a new entry left the CAM as it was.
   * - With the CAM on, an address learned while its bucket is full is learned
       into the CAM, and the CAM's entries, static or learned, work as the
       hashed ones do: a static one steers frames, a learned one ages out at
       the bridge's ageing time and goes with a flush of its port's learned
       entries, and setup's emptying deletes what a warm restart left there.
     - M
     - Addresses built to share a bucket (see "Placing, changing and deleting
       through the hash"). Five static: slots 12 to 15 and entry 1024, no
       full-bucket log line; 300 frames from the network side to the fifth all
       reached the host side, and the CPU took 18 frames meanwhile, where a
       flood would have given it the 300. Five learned from the host side:
       slots 0 to 3 and entry 1024; at an ageing time of 10 s all five were gone
       35 s later; learned again, a flush of the SFP port's learned entries
       (learning off and on) took all five in three runs out of four. The
       fourth, the first, left the four hashed entries and took the one in the
       CAM (see "Open questions"). Five static addresses left at a warm restart,
       the fifth in the CAM: setup logged ``cleared 11 address(es) ...`` and
       the CAM was empty after it.
   * - ``RTL9602C_FDB_HASH_WAYS`` (4), the number of slots in a bucket, is also
       the number of hashed deletes that the driver issues to empty one key out
       of the table.
     - M
     - The bucket layout of the first row.

Entry layout
------------

An entry is 78 bits in three words. The table shows where the fields of a
unicast entry and of a multicast entry lie; the rows after it give what was
measured about them.

.. list-table::
   :header-rows: 1
   :widths: 8 10 12 30 40

   * - Word
     - Bits
     - Entry bits
     - Macro
     - Meaning, and the kind of entry
   * - 0
     - 31:0
     - 31:0
     - (none)
     - Address bytes 2 to 5, byte 2 in bits 31:24. Both kinds.
   * - 1
     - 15:0
     - 47:32
     - ``RTL9602C_FDB_W1_MAC_HI``
     - Address bytes 0 and 1. Both kinds.
   * - 1
     - 27:16
     - 59:48
     - ``RTL9602C_FDB_W1_VID``
     - The VLAN that the entry is keyed by. Both kinds.
   * - 1
     - 29
     - 61
     - ``RTL9602C_FDB_W1_ROUTER_MAC``
     - One of this device's own addresses: hand frames to the L3 engine.
       Unicast.
   * - 1
     - 30
     - 62
     - ``RTL9602C_FDB_W1_STATIC``
     - Static: not aged, not flushed by a dynamic flush. Both kinds.
   * - 1
     - 31
     - 63
     - ``RTL9602C_FDB_W1_IVL``
     - Key the entry by its VID (independent learning). Both kinds.
   * - 2
     - 2:1
     - 66:65
     - ``RTL9602C_FDB_W2_PORT``
     - The port the address lives behind. Unicast.
   * - 2
     - 5:2
     - 69:66
     - ``RTL9602C_FDB_W2_MC_PORTS``
     - The member set of the four ports. Multicast; overlaps the port and age
       fields.
   * - 2
     - 5:3
     - 69:67
     - ``RTL9602C_FDB_W2_AGE``
     - The age, counting down. Unicast.
   * - 2
     - 12:6
     - 76:70
     - ``RTL9602C_FDB_W2_MC_EXT_PORTS``
     - The member set of the extension ports; bit 0 of the field,
       ``RTL9602C_FDB_W2_MC_EXT_PORTS_CPU``, is the first. Multicast.
   * - 2
     - 9
     - 73
     - ``RTL9602C_FDB_W2_NEXTHOP``
     - An entry that a next hop may name. Unicast.
   * - 2
     - 13
     - 77
     - ``RTL9602C_FDB_W2_VALID``
     - Valid: cleared in a hashed write, it deletes the entry that the key
       names. Both kinds.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Word 0 holds address bits 31:0 (address bytes 2 to 5, byte 2 highest).
       ``RTL9602C_FDB_W1_MAC_HI`` holds address bits 47:32 (bytes 0 and 1).
       ``RTL9602C_FDB_W1_VID`` is the VLAN that the entry is keyed by. In a
       unicast entry ``RTL9602C_FDB_W2_PORT`` is the port that the address lives
       behind.
     - M
     - Three entries that the switch learned by itself, read by index and
       decoded with these fields, gave this device's own address behind port 3
       and the host's behind port 0, in the VLAN that the bridge had given it.
       ``bridge fdb show`` shows the ``self`` entries.
   * - ``RTL9602C_FDB_W1_IVL`` keys the entry by its VID (independent learning).
       It must agree with the VLAN entry's ``RTL9602C_VLAN_ENTRY_IVL``: with
       both set, a hashed add, update, find and delete of one address land on
       one slot; with the VLAN in shared learning a second hashed write of the
       same address adds a second entry; with the bit on the entry alone the
       hashed search finds it but frames stop matching it. With the VLAN in
       independent learning the switch learns entries with this bit set. The
       driver sets it on the entries of a VLAN in independent learning and on
       every next-hop entry, and leaves it clear on its other entries of a VLAN
       in shared learning (``rtl9602c_l2_ivl()``).
     - M
     - At one address: VLAN shared, add to slot ``0x15c`` and update to
       ``0x15d``; VLAN independent, add, update and delete all ``0x274``; with
       the bit on the entry only, a static entry stopped steering frames (5 of 5
       flooded again).
   * - In shared learning an entry is keyed by its address alone. The VID field
       of a learned entry holds the VLAN of the frame that it was learned from;
       frames from the same address in another VLAN in shared learning neither
       add an entry nor change that one.
     - M
     - Frames from one address in VLANs 3003 and 3004, neither of them written,
       left its single entry, learned in VLAN 4094, as it was.
   * - A static entry keyed by address steers the frames of a VLAN-unaware
       bridge whose ports have PVID 0, VID 0 being in shared learning: the
       bridge of the OMCI daemon. Keyed by VID, the same entry steered nothing.
     - M
     - A static entry for an address on ``sfp``: 200 frames from the host to
       that address were dropped as frames to their own ingress port, none
       reaching the OLT and none the CPU, and 200 from the network side
       reached the host with none at the CPU. Keyed by VID, as the driver
       wrote every entry before, the frames from the host were flooded to the
       PON and the CPU, 200 of 200, as with no entry.
   * - Independent learning keys the filtering database by VID; shared learning
       keys it by a filtering identifier that several VLANs may share.
     - P
     - IEEE 802.1Q-2022, clause 8.8 (the filtering database; allocation of VIDs
       to FIDs).
   * - ``RTL9602C_FDB_W1_STATIC`` marks a static entry: the dynamic flush leaves
       it and it does not age. The switch never sets it on an entry that it
       learns. The driver sets it on every entry that it writes, and
       ``port_fdb_dump`` reports it as the entry's static flag.
     - M
     - Through a spanning-tree transition (a dynamic flush) static entries
       stayed and learned ones went; at a 10 s ageing time, watched for 35 s,
       the static entries stayed and the learned ones aged out.
   * - A static entry is also not moved by source learning when its address is
       seen arriving on another port.
     - M
     - The static entry of the row above: 200 frames from its address through
       the PON left it behind ``sfp``, with no second entry, and frames from the
       host to the address were still dropped. Keyed by VID, the entry stayed
       too, but the switch learned the address beside it at a second slot.
   * - ``RTL9602C_FDB_W2_AGE`` counts down; zero is aged out. The driver writes
       ``RTL9602C_FDB_W2_AGE_MAX`` (7). A freshly learned entry also starts at 7
       and a written entry counts down like any other, so the age says nothing
       about whether an entry is static.
     - M
     - A written entry went from 7 to 6 in four seconds at a 10 s ageing time;
       fresh learned entries read 7.
   * - ``RTL9602C_FDB_W2_VALID`` reads 1 in every hashed slot, occupied or not,
       so it is no test of occupancy (``RTL9602C_TBLACC_STAT_HIT`` is). Written,
       it matters: a hashed write with it clear deletes the entry that the key
       names. The driver sets it on entries that it places and clears it to
       delete.
     - M
     - All 1024 hashed slots read word 2 = ``0x00002000``; ``bridge fdb del`` of
       a static entry restored the flooding that it had stopped.
   * - ``RTL9602C_FDB_W1_ROUTER_MAC`` is not what admits a frame to the L3
       engine: the engine takes the frames for this device's own addresses
       with no entry for them in the table. What the bit does is not known.
       The driver sets it on the entries behind the CPU port while the engine
       is up (see "The layer-2 side of routing"). A frame that is not IP, to
       an address behind the CPU port that is not one of this device's own, is
       flooded to every port while its entry carries the bit.
     - M
     - Booted straight into router mode, a kernel that places no entry for the
       stick's own address had none in the ``fdb`` view, and the routing test
       still found its flow in the switch, with 20 of 20,000 datagrams through
       the CPU; the same as with the entry and its bit. A host address added
       with ``bridge fdb add ... dev omcibr0 self local``, its entry keyed as
       its VLAN learns and carrying the bit: 200 frames of EtherType ``0x88b5``
       from the host to it reached the OLT as well as the CPU, while 200 to the
       stick's own address reached the CPU alone.
   * - ``RTL9602C_FDB_W2_NEXTHOP`` marks an entry that the L3 engine may use as
       a next hop's destination. Without it the engine resolves a routed frame
       completely and the switch then discards the frame at its ingress port,
       with an empty destination mask and forwarding reason 83.
     - M
     - One routed burst each way: with the bit clear, 287,400 frames were
       counted in the ingress port's ``rx_forward_discards``
       (``RTL9602C_MIB_TX_FWD_DISCARDS``) and none were sent by the PON port;
       with it set, 289,581 were sent and the translated frames were seen on the
       far side of the OLT. The reason and the empty mask were read from the
       switch's frame capture (devlink region ``frame_capture``, see the
       *devlink* page).
   * - A by-index write to the address table is accepted and discarded: the
       status reports HIT and the entry reads back unchanged. The driver never
       writes this table by index. A bit such as ``RTL9602C_FDB_W2_NEXTHOP`` can
       therefore be given to an entry only by placing the entry with it.
     - M
     - Word 1 with bit 29 set (``0xe00000e0``) written to an occupied slot by
       index read back without it (``0xc00000e0``); the same for the next-hop
       bit.
   * - A multicast entry is the same 78 bits with the upper fields read
       differently. ``RTL9602C_FDB_W2_MC_PORTS`` is the member set of the four
       ports and ``RTL9602C_FDB_W2_MC_EXT_PORTS`` the member set of the
       extension ports, of which ``RTL9602C_FDB_W2_MC_EXT_PORTS_CPU`` is the
       first. These fields overlap the unicast port and age fields, so the
       driver clears word 2 before it builds a member set.
     - M
     - One group looked up by hash after each step: the SFP port joins, word 2 =
       ``0x00002004`` (member set 0001, port 0); the PON port joins,
       ``0x00002014`` (0101, ports 0 and 2); the PON port leaves,
       ``0x00002004``; the SFP port leaves, not found. The group was looked
       up by hash rather than read with ``bridge mdb``.
   * - A group with the CPU as a member needs
       ``RTL9602C_FDB_W2_MC_EXT_PORTS_CPU`` as well as the CPU port's member
       bit, the same pair as in "Port masks: four ports and seven extensions".
       The driver sets and clears both together.
     - I
     - Untested for multicast entries: the bridge itself joining a group (a host
       entry, which DSA puts on the CPU port), then that group sent from the
       host with and without the extension bit, would settle it.

Two worked examples of what the driver writes, from ``rtl9602c_l2_pack()`` and
``rtl9602c_mdb_members()``. A static entry for ``00:00:5E:00:53:01`` in VLAN 1
behind port 0, with the age at its maximum, is these three words::

    word 0  0x5e005301   address bytes 2 to 5
    word 1  0xc0010000   IVL, STATIC, VID 1, address bytes 0 and 1 = 0
    word 2  0x00002038   VALID, AGE 7, PORT 0

A group for ``01:00:5E:01:02:03`` in VLAN 1 with the SFP port and the PON port
as members has word 0 = ``0x5e010203``, word 1 = ``0xc0010100`` (address bytes 0
and 1 are ``0x0100``) and word 2 = ``0x00002014``: VALID and the member set 0101
in bits 5:2. A group whose only member is the CPU port would be written with
word 2 = ``0x00002060``, the CPU port's member bit (bit 5) and the first
extension bit (bit 6) together.

Placing, changing and deleting through the hash
-----------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A hashed write of a key that is already in the table does not always
       replace it. In a VLAN in independent learning it updates the one slot.
       Where two entries for the same address already existed, a static one and
       one learned from the CPU's own transmissions, a hashed write of that
       address with one bit changed put a third copy at another slot and left
       both originals as they were. The driver therefore deletes every entry of
       the key first, each by its own words (see the rows below), and then adds
       the key once. It does so in ``port_fdb_add``, for the next-hop entries
       and for the entries behind the CPU port (see "The layer-2 side of
       routing").
     - M
     - This device's own address held slots 364 (learned) and 365 (static), both
       without the bit; the changed copy went to slot 1004; after
       delete-then-add a fresh boot showed one entry, carrying the bit.
   * - A successful hashed write reports ``RTL9602C_TBLACC_STAT_HIT`` set and
       the index that it used.
     - T
     - ``port_fdb_add`` succeeds and its entry steers frames; the next hops of
       the L3 engine point at the index that the placement returns.
   * - A hashed write that finds its bucket full lands in the CAM and reports
       HIT set. One that finds the CAM full too reports HIT clear; the driver
       then returns ``-ENOSPC`` and logs the address and VID (``<address> vlan
       <v>: its hash bucket and the CAM are full``).
     - M, I
     - M: with the CAM off, five static entries whose addresses hash to bucket
       77 (next row): the first four took slots 308 to 311, the fifth was
       refused with the log line, and once one of the four was deleted the
       fifth took its slot; with the CAM on, the fifth of five took entry 1024
       with no log line. I: HIT clear with the CAM full too, which no test
       has filled.
   * - An entry keyed by address alone goes to the bucket that is the XOR of
       the six bytes of its address.
     - M
     - 110 static entries at VID 0, keyed by address, with random last three
       bytes: the bucket of each, its slot divided by four, was that XOR for
       all of them, and the five addresses of the row above, built to share
       one bucket, landed in it.
   * - A hashed delete removes an entry only when the rest of the written word
       agrees with it: a delete built from the address and VID with port 0 left
       an entry behind the CPU port in place. The driver therefore deletes an
       entry by its own words, as the emptying at setup does: it reads the
       hashed slots by index and writes each entry of the key back through the
       hash with ``RTL9602C_FDB_W2_VALID`` clear (``rtl9602c_l2_purge()``). A
       walk of all 1024 slots takes well under 10 ms.
     - M
     - A host address added with ``bridge fdb add ... dev omcibr0 self local``
       went behind the CPU port; ``bridge fdb del`` returned success and the
       ``fdb`` view still showed the entry while the delete carried port 0, and
       showed it gone with the delete by the entry's own words. The bridges of
       earlier runs had left their addresses behind the CPU port the same way.
   * - A VLAN that no ``port_vlan_add`` wrote, such as a VLAN that only an
       upper of a port in a VLAN-unaware bridge uses, is learned in shared
       learning, and the driver keys its entries there by address too, except a
       next hop's. The switch then keeps a learned entry of a next hop's address
       beside the next-hop entry, at another slot, because the two are keyed
       differently.
     - M
     - The ``fdb`` view showed the router's address in the management VLAN
       twice: ``ivl static`` with the next-hop bit at slot 488, and ``svl
       learned`` at slot 556.

Adding, deleting and dumping entries
------------------------------------

``rtl9602c_port_fdb_add()``, in order, steps 3 to 8 under ``fdb_lock``:

1. Refuse a multicast address with ``-EINVAL``. Purpose: a group is a member set
   and goes through ``port_mdb_add``. (T)
2. Replace VID 0 with the port's PVID. Purpose: DSA passes 0 for a VLAN-unaware
   bridge, and the switch puts an untagged frame into its ingress port's PVID
   before the lookup (see "VLAN membership, filtering and PVID"). (M)
3. Read the VLAN's learning mode, ``RTL9602C_VLAN_ENTRY_IVL`` of its entry
   (``rtl9602c_l2_ivl()``); VID 0 is shared learning without a read, as the
   driver never touches its entry. Purpose: an entry matches frames only when
   it is keyed as its VLAN learns (see "Entry layout"). (M)
4. If a routed next hop's entry holds the key: behind this port, return 0, as
   that entry is static and sends the address there already; behind another
   port, return ``-EBUSY`` with the log line ``<address> vlan <v>: a routed
   next hop holds it behind port <n>``. Purpose: a next hop names its entry by
   slot, so the entry stays as placed (see "The layer-2 side of routing"). (T)
5. Delete every other entry of the key, learned or static, behind any port,
   each by its own words; for an entry keyed by address, every entry of the
   address keyed by address, whatever VLAN its VID field names. Purpose: a
   write of a key already in the table does not always replace it. (M)
6. Build the entry: address, VID, ``RTL9602C_FDB_W1_IVL`` if the VLAN is in
   independent learning, ``RTL9602C_FDB_W1_STATIC``, the port,
   ``RTL9602C_FDB_W2_AGE_MAX`` and ``RTL9602C_FDB_W2_VALID``. Purpose: see
   "Entry layout". (M)
7. While the L3 engine is up, set ``RTL9602C_FDB_W1_ROUTER_MAC`` on an entry for
   the CPU port. Purpose: such an entry is one of this device's own addresses
   (see "The layer-2 side of routing"), and stamping it here keeps an address
   added later routed. (I)
8. Write the entry through the hash. A write with HIT clear returns ``-ENOSPC``.
   Purpose: the result is only in the status. (T)

Step 5 is measured on an address learned through the PON in VLAN 3003 and then
made static on ``sfp`` at VID 0, which the bridge offers as ``bridge fdb
replace``: the learned entry went and the static one took its slot.

``rtl9602c_port_fdb_del()``, in order:

1. Replace VID 0 with the port's PVID, as for the add. (M)
2. Behind the CPU port, leave an address that a user port carries at its PVID.
   Purpose: it is this device's own address, which the L3 engine goes on
   using; DSA deletes a bridge's host addresses when the bridge goes, as router
   mode does. (T)
3. Under ``fdb_lock``, delete the entries of the address behind the port whose
   VID field is the VID, keyed either way, each by its own words, but not a
   next hop's. Purpose: a hashed delete built from the key alone does not reach
   every entry (see "Placing, changing and deleting through the hash"), an
   entry placed while its VLAN learned the other way is deleted too, and a
   next hop still names its entry. (M)

``rtl9602c_port_fdb_dump()``, in order:

1. Read every entry by index, the CAM's included. Purpose: the dump shows an
   address that the CAM holds. (M)
2. Skip a slot without HIT. Purpose: the valid bit is set in every slot. (M)
3. Decode address, VID and port, and report the unicast entries behind the port
   asked about, with the static flag from ``RTL9602C_FDB_W1_STATIC``, and the
   groups whose member set holds the port. Purpose: a group's member set lies
   where a unicast entry keeps its port. (M)

``rtl9602c_port_mdb_add()`` and ``rtl9602c_port_mdb_del()`` run the same steps
under ``fdb_lock``, which makes the lookup, the change and the write one
operation: two ports joining at once would otherwise each write back the member
set that they read before the other. In order:

1. Refuse a unicast address with ``-EINVAL``, replace VID 0 with the port's
   PVID and read the VLAN's learning mode, as for ``port_fdb_add``. (T)
2. Look the key up through the hash (address, VID, ``RTL9602C_FDB_W1_IVL`` if
   the VLAN is in independent learning, ``RTL9602C_FDB_W1_STATIC``, word 2
   zero). Purpose: the group's current member set. (M; a group keyed by address
   is I, since a bridge of this configuration, without multicast snooping,
   adds no group)
3. Not found: a delete has nothing to do, and an add starts from an empty set
   and the rebuilt key. Purpose: the data read on a miss are no entry. (T)
4. Set or clear the port's member bit and, for the CPU port,
   ``RTL9602C_FDB_W2_MC_EXT_PORTS_CPU`` with it. Purpose: see the rows of "Entry
   layout" on groups. (M, I)
5. Keep ``RTL9602C_FDB_W2_VALID`` while any member remains and clear it with the
   last. Purpose: the entry is retired with its last member. (M)
6. Write the entry through the hash. A write with HIT clear returns ``-ENOSPC``
   with the log line ``<address> vlan <v>: nowhere left to put it``. (M)

In step 4 the member bits are M and the CPU port's extension bit is I.

Emptying the table at setup
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The switch keeps its address table across a restart of the SoC, the CAM
       included. At setup the driver turns the CAM on, so that the hash reaches
       it, walks all 1088 entries by index and writes every entry that has HIT
       back through the hash with ``RTL9602C_FDB_W2_VALID`` clear
       (``rtl9602c_l2_clear()``), which deletes exactly that entry, unicast or
       multicast, and logs ``cleared <n> address(es) left in the table by the
       previous kernel``.
     - M
     - A fresh boot from RAM found six entries of the previous kernel:
       ``cleared 6 address(es) ...``. With an entry in the CAM before the
       restart, none was there after setup (see "Structure").
   * - The flush cannot do this: it selects entries by source port, and a
       multicast entry holds a member set where the source port would be.
     - I
     - A group whose only member is one port, a port flush of that port, then a
       hashed lookup of the group would settle it. Nothing depends on it.

Flush and fast ageing
=====================

The switch can remove the entries of chosen ports from the address table in one
operation, either all of them or only the learned ones. The driver uses it for
fast ageing: when a port stops forwarding or leaves a bridge, the entries that
it learned are stale.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_FDB_FLUSH_CTRL`` (``0x017024``):
       ``RTL9602C_FDB_FLUSH_CTRL_START`` (bit 0) starts a flush and reads 0 once
       it has finished; ``RTL9602C_FDB_FLUSH_CTRL_MODE`` (bits 2:1) selects
       which entries, ``RTL9602C_FDB_FLUSH_CTRL_MODE_PORT`` (0) being those of
       the ports set in ``RTL9602C_FDB_FLUSH_PORTS``;
       ``RTL9602C_FDB_FLUSH_CTRL_DYNAMIC`` (bit 4) limits the flush to learned
       entries. ``RTL9602C_FDB_FLUSH_PORTS`` (``0x017028``) has one bit per
       port; the port is never written into the control register.
     - M
     - ``bridge link set dev sfp state disabled`` took the SFP port's hardware
       entries from 2 to 0 and left the static one (``bridge fdb show`` around a
       spanning-tree transition).
   * - ``port_fast_age`` flushes the learned entries of one port under
       ``fdb_lock``. DSA calls it when a port stops forwarding or leaves a
       bridge, so that the switch does not keep forwarding to a port whose
       topology has just changed. The driver also calls it for the PON port when
       the last GEM interface leaves the bridge (see "Bridging the GEM ports").
     - T
     - VLAN filtering, FDB learning and ageing work.

The flush, ``rtl9602c_l2_flush()``, in order:

1. Write ``RTL9602C_FDB_FLUSH_PORTS`` with the port's bit. Purpose: the ports
   are a separate register. (M)
2. Write ``RTL9602C_FDB_FLUSH_CTRL`` with ``RTL9602C_FDB_FLUSH_CTRL_MODE_PORT``,
   ``RTL9602C_FDB_FLUSH_CTRL_DYNAMIC`` and ``RTL9602C_FDB_FLUSH_CTRL_START``.
   Purpose: flush that port's learned entries and keep the static ones. (M)
3. Poll ``RTL9602C_FDB_FLUSH_CTRL_START`` every 100 microseconds, for up to 100
   ms, else log ``the L2 flush never finished``. Purpose: the bit clears when
   the flush is done. (M)

Learning and the learning limit
===============================

Learning is a limit, not a switch. Each port has a limit on the number of
addresses that it may learn: the maximum stands for unlimited and 0 for learning
off. A second register says what happens to a frame from a new source once the
limit is reached, and the driver keeps it at "forward without learning", so that
a limit of 0 means "learn nothing" and not "forward nothing". A count for each
port shows how many addresses it has learned.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_FDB_LEARN_LIMIT(p)`` (``0x017008`` + ``4 * (p / 2)``) holds
       how many addresses port p may learn: eleven bits
       (``RTL9602C_FDB_LEARN_MASK``) at bit ``RTL9602C_FDB_LEARN_SHIFT(p)`` =
       ``11 * (p mod 2)``, two ports to a word. ``RTL9602C_FDB_LEARN_LIMIT_MAX``
       (``0x7ff``), every port's reset value, serves as unlimited, and 0 as
       learning off.
     - M
     - Learning off on port 0 took the word from ``0x003fffff`` to
       ``0x003ff800``, only port 0's field moving; after a reset both words read
       ``0x003fffff``. Through the driver: debugfs ``rtl9602c/l2learn``, column
       ``limit``.
   * - ``RTL9602C_FDB_LEARN_CNT(p)`` (``0x017010`` + ``4 * (p / 2)``), packed
       the same way, counts the addresses that port p has learned. With learning
       off it stays at 0 while traffic passes.
     - M
     - Learning off: six pings from the host all answered, count 0 and no
       hardware entry; learning on: count 2 and the host's address in
       ``bridge fdb show``. Through the driver: ``l2learn``, column ``count``.
   * - ``RTL9602C_FDB_LEARN_LIMIT_ACT`` (``0x01c00c``) holds, per port, what
       happens to a frame from a new source once the port's limit is reached:
       ``RTL9602C_FDB_LEARN_LIMIT_ACT_MASK`` (two bits) at
       ``RTL9602C_FDB_LEARN_LIMIT_ACT_SHIFT(p)`` = ``2 * p``.
       ``RTL9602C_FDB_LEARN_LIMIT_ACT_FORWARD`` (0), the reset value, forwards
       the frame without learning it, which is what makes a limit of 0 mean
       "learn nothing" and not "forward nothing".
     - M
     - Pings pass with learning off and the register at 0; the register reads 0
       after a reset, and through the driver every port shows ``0=fw``
       (``l2learn``, column ``over``).
   * - The three other action values are taken to drop or trap such frames, and
       the two-bits-per-port packing is untested: every field reads 0, and 0 is
       all the driver writes. The driver writes
       ``RTL9602C_FDB_LEARN_LIMIT_ACT_FORWARD`` before every limit change, so
       that learning off never rests on a value that it did not write;
       ``l2learn`` prints any other value raw.
     - I
     - A limit of 0 with the action at 1 and frames from unknown sources would
       show both. Not load-bearing while the driver writes only 0.
   * - A port's limit is set from the bridge's learning flag (``BR_LEARNING``,
       in ``port_bridge_flags``); for the PON port, on while its GEM interfaces
       are bridged and off otherwise (see "Bridging the GEM ports"); and for the
       CPU port, off once the L3 engine is up (see "The layer-2 side of
       routing").
     - T
     - Bridged traffic passes both ways, sourced from the host; VLAN filtering,
       FDB learning and ageing work.
   * - The learning-limit latch is one bit for the whole switch: which port
       reached its limit is read from the limits and the counts (``l2learn``).
     - I
     - A deliberate limit exceeded on each of two ports in turn, the latch read
       after each, would confirm it. Only the log wording depends on it.

Because learning off is a limit of 0, a frame from an unknown source on such a
port counts as reaching the limit and can raise the switch's learning-limit
alarm; the CPU port, with learning off, raises it on an idle board as the
learned entries of the CPU's own sources age out and are offered again. The
driver therefore enables the alarm only while some port's limit is neither 0 nor
the maximum, a limit that somebody set, and evaluates that again after every
write of a limit. The alarm, its self-masking and its re-arm are described under
"The switch interrupt" on the *main* page.

Setting learning on a port, ``rtl9602c_port_learning()``, in order:

1. Set the port's field of ``RTL9602C_FDB_LEARN_LIMIT_ACT`` to
   ``RTL9602C_FDB_LEARN_LIMIT_ACT_FORWARD`` by read-modify-write. Purpose: a
   limit of 0 must not become a drop. (M)
2. Set the port's field of ``RTL9602C_FDB_LEARN_LIMIT(p)`` to
   ``RTL9602C_FDB_LEARN_LIMIT_MAX`` (on) or 0 (off) by read-modify-write. (M)
3. Evaluate the alarm subscription again (``rtl9602c_lrn_over_intr_sync()``).
   Purpose: the alarm is on only while a port has a limit that somebody set. (M)

Addresses that move
===================

A frame whose source address the table holds for another port moves the entry
to the port it came in by, by default. A register says, per port the frame came
in by, what happens instead, and the driver keeps the default except on the PON
port, where such a frame is dropped and nothing is learned from it
(``rtl9602c_l2_moves_init()``, in setup). An OLT may send the stick's upstream
frames back down its GEM port; learned from them, the subscriber's addresses
would move to the PON port, and the switch would drop the downstream frames to
them as frames to the port they came in by. A host that does move from the SFP
side to the PON side is reached again once its entry ages out. ``l2learn``
prints the action of each port (column ``moved``).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_SA_MOVED`` (``0x01c000``) holds two bits per port,
       ``RTL9602C_SA_MOVED_ACT(p)`` at bit ``2 * p``, for the frames that come
       in by port p. ``RTL9602C_SA_MOVED_ACT_MOVE`` (0), every port's value
       after the boot loader, moves the entry to that port;
       ``RTL9602C_SA_MOVED_ACT_DROP`` (1) drops the frame and leaves the entry
       where it is.
     - M
     - The lab OLT sends every frame from the stick back down its GEM port: of
       20 frames sent upstream, 20 reached the router and 20 came back. With
       0 on the PON port, a ping from the SFP side moved the host's address to
       the PON port (``fdb``) and pings failed both ways; with 1, the address
       stayed on the SFP port and 10 of 10 pings passed both ways, while the
       frames sent back still arrived at the GEM port. After setup the
       register reads ``0x00000010``.
   * - The values 2 and 3 are taken to trap such a frame to the CPU and to copy
       it there.
     - I
     - Not load-bearing: the driver writes 1, on the PON port only.

Ageing
======

One ageing time serves the whole switch. A learned entry that no frame refreshes
for that long is removed; a static entry is not aged. With several bridges on
the switch DSA programs the shortest of their ageing times.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_FDB_CFG`` (``0x017000``), field
       ``RTL9602C_FDB_CFG_AGEING_TIME`` (bits 20:0): how long a learned entry
       lives unrefreshed, one value for the whole switch, in units of
       ``RTL9602C_FDB_CFG_AGEING_TIME_UNIT_MS`` (100 ms). Out of reset it holds
       3000 (300 s).
     - M
     - With the bridge's ageing time at 10 s the field read 100 and an
       unrefreshed address was gone after 9.2 to 10.2 s; at 120 s the field read
       1200 and the address was gone after 111 to 121 s. Read the register only
       after the bridge has applied the change: a read within milliseconds of
       the netlink call can still show the old value.
   * - ``set_ageing_time`` writes msecs / 100 into the field by
       read-modify-write. DSA refuses a request outside the range that the
       driver declares at probe, ``ageing_time_min`` = 100 ms (one unit) and
       ``ageing_time_max`` = ``RTL9602C_FDB_CFG_AGEING_TIME_MAX`` (``0x1fffff``)
       units of 100 ms, with ``-ERANGE``, so the field cannot overflow.
     - T
     - VLAN filtering, FDB learning and ageing work.
   * - Ageing is enabled per port by the register at ``0x017004``, one bit per
       port, which reads ``0x0000000f`` out of reset: all four ports age. The
       driver has no name for this register and never writes it.
     - M
     - Read at bring-up and after a reset; ageing works (the first row).

For example, a ten-second ageing time, which the bridge takes in hundredths of a
second::

    ip link set br0 type bridge ageing_time 1000

Spanning-tree state
===================

Each port has a spanning-tree state for instance 0 in a register of its own; the
register has room for a second instance, which the driver does not write. The
bridge has five port states and the switch four. The driver maps them as follows
and writes the result by read-modify-write.

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Bridge state
     - Switch state
     - Value
   * - ``BR_STATE_DISABLED``
     - ``RTL9602C_STP_PORT_STATE_DISABLED``
     - 0
   * - ``BR_STATE_BLOCKING``
     - ``RTL9602C_STP_PORT_STATE_BLOCKING``
     - 1
   * - ``BR_STATE_LISTENING``
     - ``RTL9602C_STP_PORT_STATE_BLOCKING``
     - 1
   * - ``BR_STATE_LEARNING``
     - ``RTL9602C_STP_PORT_STATE_LEARNING``
     - 2
   * - ``BR_STATE_FORWARDING``
     - ``RTL9602C_STP_PORT_STATE_FORWARDING``
     - 3

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_STP_PORT_STATE(p)`` (``0x01702c`` + 4p): one word per port,
       two bits per spanning-tree instance, ``RTL9602C_STP_PORT_STATE_MSTI0``
       (bits 1:0) being instance 0. Out of reset every port's word reads
       ``0x0000000f``: both instances forwarding.
     - M
     - Read at bring-up for ports 0 and 3, and after a reset for all four.
   * - ``RTL9602C_STP_PORT_STATE_FORWARDING`` (3) forwards and learns. It is the
       reset state, and all traffic on this board has run in it.
     - M
     - Every boot; bridged traffic passes both ways, sourced from the host.
   * - ``RTL9602C_STP_PORT_STATE_DISABLED`` (0),
       ``RTL9602C_STP_PORT_STATE_BLOCKING`` (1) and
       ``RTL9602C_STP_PORT_STATE_LEARNING`` (2) are the other three states:
       none of them forwards, and of the three only learning learns.
     - M
     - ``bridge link set dev sfp state`` disabled, listening (which the driver
       writes as blocking) and learning, each for 14 s with 200 frames from the
       host from a source address of their own: none of the frames reached the
       OLT in any of the three, and only the address sent in learning was in
       the address table afterwards. In forwarding, before and after, all 200
       arrived and the address was learned. The bridge's blocking state cannot
       be held while spanning tree is off, since the bridge moves a blocking
       port on to forwarding at once (``br_make_forwarding()``), and the frames
       sent then passed.
   * - The bridge's five states map to the switch's four: disabled to disabled;
       blocking and listening to blocking, since in both a port discards frames
       and learns nothing; learning to learning; forwarding to forwarding. The
       driver writes instance 0 only, by read-modify-write.
     - P
     - IEEE 802.1D-1998, clause 8.4 (port states).

Port isolation
==============

Port isolation says, for each ingress port, which ports a frame may leave by.
The driver expresses bridging with it: the mask of a user port names the CPU
port with the first extension port, and the other user ports of its bridge. The
masks are in the shape of "Port masks: four ports and seven extensions".

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_ISOLATE_EGRESS(p)`` (``0x027000`` + ``4 * (p / 2)``) holds the
       ports that a frame arriving on port p may leave by:
       ``RTL9602C_ISOLATE_EGRESS_WIDTH`` (11) bits
       (``RTL9602C_ISOLATE_EGRESS_MASK``) at
       ``RTL9602C_ISOLATE_EGRESS_SHIFT(p)`` = ``11 * (p mod 2)``, two ports to a
       word. Out of reset every field is all ones.
     - M
     - Both words read ``0x003fffff`` at bring-up and after a reset; with the
       SFP port bridged the first word read ``0x003ff818`` (port 0's field
       ``0x018``, port 1's untouched).
   * - ``RTL9602C_ISOLATE_EGRESS_EXT0`` (bit 4) is required, together with the
       CPU port's bit 3, for anything to reach the CPU; bits 0 and 5 to 10 are
       not needed.
     - M
     - Port 0's field swept with pings from the host: all ones 6/6; without bit
       3 0/6; without bit 4 0/6; without bit 5, 6 or 7 6/6; without bits 8 to 10
       8/8; without bit 0 8/8; bit 3 alone 0/6; bit 4 alone 0/6; bits 3 and 4
       6/6.
   * - At reset every port may reach every other, so until the driver wrote the
       masks a broadcast arriving on the SFP port also left by the PON port. The
       driver writes them in setup, before any port has an interface.
     - M
     - Observed with the masks at their reset value: a broadcast from the host
       also left by the PON port. After setup the words read as in the next row.
   * - The driver gives each user port the CPU port,
       ``RTL9602C_ISOLATE_EGRESS_EXT0``, every other user port in the same
       bridge (the PON port counts as a member of its GEM interfaces' bridge)
       and, while the L3 engine is up, every port with an L3 next hop behind it
       other than itself. The CPU port's own field keeps its reset value.
     - M
     - Routed, nothing bridged, next hops on both ports: port 0's field read
       ``0x01c`` (may reach 2, 3 and extension 0) and port 2's ``0x019`` (0, 3
       and extension 0); after a cold boot the first word read ``0x003ff81c``
       with nothing written by hand and the second ``0x003ff819`` with the CPU
       port's field all ones; with the SFP port bridged alone, ``0x018``.
   * - Routed frames obey isolation: with the PON port missing from the SFP
       port's field, every frame that the L3 engine routed from the SFP port to
       the PON port was discarded at its ingress port and counted in that port's
       ``rx_forward_discards`` (``RTL9602C_MIB_TX_FWD_DISCARDS``).
     - M
     - After a cold boot with the next-hop entry correct: 305,000 discards and
       nothing sent by the PON port; with the PON port permitted, the translated
       frames reached the far side of the OLT. ``ethtool -S sfp``.
   * - Isolation cannot keep the two sides of a routed stick apart at layer 2: a
       flooded frame passes through the opening that routed frames need. The
       flood masks do it instead (see "Flooding").
     - M
     - Routed and nothing bridged, before the flood masks took over: 2,000
       broadcasts a second crossed each way, all of them.
   * - ``RTL9602C_ISOLATE_EXT_HAIRPIN`` (``0x01c08c``) reads 0. The driver never
       writes it and ``l2learn`` prints it raw (``src ext perm``). It is taken
       to say, per extension port, whether a frame may leave by the port that it
       entered.
     - I
     - The reset value was read after a reset and through ``l2learn``. Its
       meaning is unverified: nothing on this board reaches an extension port,
       and nothing depends on it.

By the rule of the fourth row, a bridge of ``sfp`` and the GEM interfaces gives
port 0 the field ``0x01c`` (the PON port, the CPU port and the first extension
port) and port 2 the field ``0x019`` (the SFP port, the CPU port and the first
extension port), the values of the routed measurement.

Flooding
========

Three masks, one for broadcasts, one for multicast frames that no table entry
knows and one for unicast frames that no table entry knows, say which ports such
a frame may leave by. They map one to one onto three bridge port flags:

.. list-table::
   :header-rows: 1
   :widths: 30 40 30

   * - Bridge flag
     - Register
     - Frames
   * - ``BR_FLOOD``
     - ``RTL9602C_FLOOD_UNKNOWN_UCAST``
     - unknown unicast
   * - ``BR_MCAST_FLOOD``
     - ``RTL9602C_FLOOD_UNKNOWN_MCAST``
     - unknown multicast
   * - ``BR_BCAST_FLOOD``
     - ``RTL9602C_FLOOD_BCAST``
     - broadcast

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Three masks, one bit per port: ``RTL9602C_FLOOD_BCAST`` (``0x01c020``),
       ``RTL9602C_FLOOD_UNKNOWN_MCAST`` (``0x01c024``) and
       ``RTL9602C_FLOOD_UNKNOWN_UCAST`` (``0x01c028``). A bit is an egress
       permission: it says whether a broadcast, a multicast frame with no table
       entry, or a unicast frame with no table entry may leave by that port. Out
       of reset all three read ``0x0000000f``.
     - M
     - Read at bring-up and after a reset; unknown unicast from the host reached
       the CPU 5 of 5 times with the CPU port's bit set, 0 of 5 with it clear,
       and 5 of 5 again when it was set back.
   * - They map one to one onto the bridge's flags, ``BR_BCAST_FLOOD`` to
       ``RTL9602C_FLOOD_BCAST``, ``BR_MCAST_FLOOD`` to
       ``RTL9602C_FLOOD_UNKNOWN_MCAST`` and ``BR_FLOOD`` to
       ``RTL9602C_FLOOD_UNKNOWN_UCAST``, and each flag moves only its own mask.
     - M
     - ``bridge link set dev sfp flood off``, ``mcast_flood off`` and
       ``bcast_flood off`` each cleared bit 0 of its own mask only, and ``on``
       set it back.
   * - What reaches the hardware is the flag that the bridge asked for and
       whether the port shares a bridge with another switch port (for the PON
       port, its GEM interfaces' bridge). A standalone port needs no flooding:
       what leaves by it comes from the CPU with an explicit port mask, or from
       the L3 engine as a routed unicast frame. The driver keeps each port's
       requested flags (``flood_req``, starting with all three set, the reset
       state) and applies them again on every membership change.
     - T
     - Checked with routed traffic on the board: at most 20 of 2,000 broadcasts
       may cross in either direction. Bridged traffic passes both ways, floods
       included.
   * - Routed, the three masks read ``0x0000000a`` (the SFP and PON ports clear,
       port 1 and the CPU port at their reset value); bridged, ``0x0000000f``.
       Forcing the broadcast mask back to ``0x0f`` in the routed state let 2,000
       of 2,000 broadcasts cross again.
     - M
     - Routed, 0 of 2,000 broadcasts and 0 of 2,000 unknown unicast frames
       crossed in either direction; bridged, 2,002 of 2,000 each way.
       ``l2learn``, first line.
   * - The driver never writes the CPU port's bits, which stay set: that is how
       unknown traffic reaches the host. DSA would drive them through
       ``port_set_host_flood``, but calls it only on a switch that supports host
       unicast and multicast filtering, which needs VLAN filtering for each
       port, and this switch has one filtering bit (see "VLAN membership,
       filtering and PVID"). The operation is not implemented.
     - M
     - With the operation registered in a test build, the bridge took the SFP
       port in and out of promiscuous and all-multicast mode and no mask moved.
   * - The driver accepts ``BR_LEARNING``, ``BR_FLOOD``, ``BR_MCAST_FLOOD`` and
       ``BR_BCAST_FLOOD`` (``RTL9602C_BRIDGE_FLAGS``) and refuses any other
       bridge port flag in ``port_pre_bridge_flags`` with ``-EINVAL`` and the
       message ``this switch has learning and the three flood masks, and nothing
       else``.
     - T
     - VLAN filtering, FDB learning and ageing work: the bridge sets these four
       flags on a port that joins.

Multicast snooping
==================

The bridge snoops IGMP and MLD and hands the groups that its ports join to the
switch as MDB objects, which become multicast entries of the address table (see
"Adding, deleting and dumping entries"). Three things make the switch forward
multicast as the bridge would:

- Every IGMP message, and every MLD query and version 1 report, that enters the
  SFP or the PON port reaches the bridge. The switch forwards the message as
  usual, so it keeps its VLAN translation and the GEM stream its VLAN takes,
  and an ACL rule sends a copy to the CPU (``rtl9602c_acl_snoop_setup()``, see
  the *acl-qos* page, "Rules of the driver's own"). The copy arrives marked as
  forwarded, so the bridge snoops it and forwards nothing again. The other MLD
  messages go to ff02::1, ff02::2 or ff02::16, which reach the CPU unasked.
- The PON port is a member of every group in the table, as the side the
  multicast router of a PON sits on: a host's IGMPv1, IGMPv2 or MLDv1 report
  goes to the group's own address.
- While the bridge sees a multicast router behind a GEM port, the PON port drops
  the multicast that no host asked for, unregistered multicast, IPv4 and IPv6
  each by its own routers (``rtl9602c_mcast_refresh()``): once it has seen a
  querier, a bridge sends such frames only towards router ports. The reserved
  groups are flooded all the same, as the bridge floods the link-local groups,
  which it does not snoop. The PON class passes the router state of a GEM port
  to the driver (``gem_mrouter``, see Documentation/networking/pon.rst).
- While no bridge joins the SFP port to the GEM interfaces, as where the stick
  routes or holds the sessions itself, the multicast and the unicast from the
  PON side that no entry knows would reach only the CPU, which has no use for
  them. The PON port then drops unregistered IPv4 and IPv6 multicast
  (``rtl9602c_mcast_refresh()``), and unicast that no entry knows
  (``rtl9602c_pon_ucast_refresh()``). The unicast is dropped only while every
  GEM interface has said which addresses it takes and none is promiscuous: the
  PON class hands the driver each GEM interface's addresses and the VLANs of
  the VLAN devices over it (``gem_rx_filter``), and the driver places a static
  entry behind the CPU port for each (``rtl9602c_gem_hosts_sync()``), as a
  session's address is not one the switch learns.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A multicast entry is found by the destination MAC address for IP
       multicast too. ``RTL9602C_FDB_CFG`` bit 23 reads 0 at reset, and the
       driver leaves it.
     - M
     - With the SFP port's unknown-multicast flood bit clear, a static group for
       239.2.2.9 on the SFP port let 20 of 20 frames from the PON through, and
       0 of 20 frames to 239.2.2.1.
   * - Without the PON port among a group's members, a report to the group from
       the SFP port goes nowhere but back. With it, and the ACL copy, each
       report reaches both the router and the bridge.
     - M
     - Of three IGMPv2 reports for a new group, the first reached the OLT side,
       and the CPU by flooding, which made the bridge add the group; the second
       and third reached neither, and the bridge's group timer ran down. With
       the PON port a member and the ACL rule, 3 of 3 reached the OLT side and 3
       copies the CPU, and the timer started again at 256 s; 3 of 3 MLDv1
       reports did the same.
   * - ``RTL9602C_MC_UNKNOWN_IP4`` (``0x01c014``) and ``RTL9602C_MC_UNKNOWN_IP6``
       (``0x01c018``) hold two bits per ingress port: 0 floods unregistered
       frames by the unknown-multicast flood mask, 1 drops them. Both read 0 at
       reset, as does ``RTL9602C_MC_UNKNOWN_L2`` (``0x01c010``), which is only
       written back at setup.
     - M
     - With 1 for the PON port, 0 of 20 frames from the PON to 239.2.2.1 and to
       ff3e::1234 passed, while 20 of 20 to a group in the table and to
       224.0.0.251 did; 1 for the SFP port changed nothing for frames from the
       PON.
   * - ``RTL9602C_UC_UNKNOWN`` (``0x01c008``) holds two bits per ingress port:
       0 floods unicast that no entry knows by the unknown-unicast flood mask,
       1 drops it, a frame that the L3 engine would route included. It reads 0
       at reset, and setup writes 0 back.
     - M
     - With 1 for the PON port and no entry for a session's address, a routed
       TCP transfer through the stick and a ping across it stopped at once;
       with 0 they went on.
   * - A static entry behind the CPU port lets the frames for a session's
       address through a drop of unknown unicast on the PON port. In a VLAN
       that no ``port_vlan_add`` wrote, keyed by address alone, the entry the
       lookup finds is the one at the PON port's PVID, 0. With the router bit
       set on it, the frames do not reach the CPU.
     - M
     - With entries at VID 0 and 410 that carried the router bit, the PPPoE
       discovery offers to the session's address never reached ``wan410`` (0
       frames in 30 s) and no session came up; with the entry at VID 0 without
       the bit, both sessions came up, the entries at 410 still carrying it.
   * - Where the stick holds the sessions, the frames that an OLT sent back
       down the stick's own GEM port, for another address, no longer reach
       the CPU, and routed traffic keeps its rate.
     - M
     - Four TCP streams through the stick: with unknown unicast flooded, 932
       Mb/s up and 5000 frames a second to the CPU (the CPU limit); with it
       dropped, 934 up and 936 down and 5 to 10 frames to the CPU in 6 s.
   * - While no bridge joins the SFP port to the GEM interfaces, the PON port
       drops unregistered IPv4 and IPv6 multicast, and floods it again while
       one does. The kernel log says each change.
     - M
     - At boot ``unregistered multicast from the PON: IPv4 dropped, IPv6
       dropped``; ``flooded`` while the OMCI bridge held the SFP port and a
       GEM interface, and ``dropped`` again once it let go of them. That an
       operator's multicast GEM port then no longer loads the CPU is not yet
       measured.
   * - ``RTL9602C_MC_UNKNOWN_CFG`` (``0x01c01c``) reads ``0x28`` at reset: the
       reserved IPv4 groups, 224.0.0.0/24, flooded over a drop, and the reserved
       IPv6 groups as well, of which no class is selected. The driver selects
       ``RTL9602C_MC_UNKNOWN_CFG_RSV6_LINK`` (bit 8) and
       ``RTL9602C_MC_UNKNOWN_CFG_RSV6_SOLICITED`` (bit 12).
     - M
     - Dropping unregistered IPv6 from the PON: with both classes, frames to
       ff02::1, ff02::1:ff00:1 and ff02::fb passed, 20 of 20 each, and frames
       to ff3e::1234, ff05::2 and ff0e::101 did not; with neither class, none
       of the six passed.
   * - A reserved group passes a drop, not a clear bit of the unknown-multicast
       flood mask, as the bridge floods no link-local group to a port with
       ``mcast_flood off`` either.
     - M
     - With ``mcast_flood off`` on the SFP port, 0 of 20 frames from the PON to
       224.0.0.251 and to 224.0.0.1 passed.
   * - The CPU port's bit of the unknown-multicast flood mask stays set while
       the SFP port and the GEM interfaces share a bridge. With it clear, no
       frame to a link-local IPv6 group reaches the CPU, those to groups that
       the CPU has joined included (ff02::1, ff02::2 and a solicited-node
       group): the host's router solicitations and neighbour solicitations are
       never seen, so it gets no router advertisement and no DHCPv6 answer
       and loses its neighbours' entries as they expire. Unicast to the CPU
       is not affected.
     - M
     - On a production unit, whose bridge the OLT built, with the bit clear:
       2 router solicitations from the host and pings of ff02::1, ff02::2 and
       the unit's solicited-node group, none answered, the VLAN device's
       multicast and router-solicitation counters unchanged. On the bench
       with the SFP port and a GEM interface put in a bridge by hand
       (``ip link add omcibr0 type bridge``, then ``ip link set sfp master
       omcibr0`` and the same for a GEM interface), the same: 2 of 2
       solicitations unseen and a ping of ff02::1 unanswered by the unit;
       with the bit set, the ping was answered by the unit and 4 of 4
       solicitations, 10 s apart, drew a router advertisement.
   * - The driver follows the bridge's routers: before any query, unregistered
       multicast is flooded; a query from the OLT side makes the GEM port it
       came by a router port, and the PON port drops unregistered multicast of
       that protocol; with snooping turned off, the bridge forgets its routers
       and the PON port floods again. The kernel log says each change:
       ``rtl9602c-switch: unregistered multicast from the PON: IPv4 dropped,
       IPv6 flooded``.
     - M
     - An IGMPv2 general query from the OLT side made ``gem4095`` a router
       port; frames to 239.5.0.1 then passed 0 of 20 times, to 224.0.0.251 and
       ff02::fb 20 of 20, to a joined group 20 of 20; ``ip link set omcibr0
       type bridge mcast_snooping 0`` brought 239.5.0.1 back to 20 of 20.
   * - The bridge passes a port's router state for IPv4 and IPv6 together: it
       says so when a port becomes a router port for either protocol, and when
       it stops being one for both. The driver reads the two from the bridge's
       router lists (``br_multicast_has_router_adjacent()``) on each such
       report and on each group change, so a router of the second protocol
       behind a port that is a router port already counts from the next group
       change.
     - M
     - After the IGMPv2 query, an MLD query from the OLT side left IPv6
       flooded until the MLDv2 join that followed it.
   * - The switch's per-port IGMP and MLD control (``0x01105c`` + 4p, reading
       ``0x400`` at reset) is not used: its five 2-bit fields, written with each
       value, changed nothing measured. Reports of each IGMP version, with and
       without router alert, went on to the OLT side and reached the CPU only
       by flooding. Clearing bit 10 dropped the port's IGMP reports and
       multicast data.
     - M
     - Three IGMPv2 reports per setting, counted at the OLT side, the trap
       reasons of the CPU port and the bridge's group timer.

Bridge membership
=================

The switch has no notion of a bridge. It has a set of egress ports for each
ingress port (isolation), a set of flood ports for each kind of unknown frame,
and a learning limit for each port. The driver derives the first two from DSA's
view of which user ports share a bridge, with the PON port counted as a member
of the bridge that its GEM interfaces share, and it sets the PON port's learning
from whether its GEM interfaces are bridged. ``rtl9602c_bridge_refresh()`` does
the derivation; every join and leave calls it.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``port_bridge_join`` refuses the PON port with ``-EOPNOTSUPP`` and the
       message ``bridge the GEM interfaces, not pon itself``: a frame that the
       switch forwards to the PON port must name a GEM port, and ``pon`` names
       none. DSA then bridges ``pon`` in software, where the frames sent to it
       are dropped and counted in the interface's ``tx_dropped`` by the tag
       driver. Every other join and every leave runs
       ``rtl9602c_bridge_refresh()``.
     - T
     - Bridged traffic passes both ways, sourced from the host, through the GEM
       interfaces and not through ``pon``.

``rtl9602c_bridge_refresh()`` is called with RTNL held on every membership
change of a switch port or of a GEM interface, and once in setup, before any
port has an interface. In order:

1. For each user port, compute its isolation field and write it by
   read-modify-write (see "Port isolation"). Purpose: bridging is expressed as
   isolation. (M)
2. Apply the port's flooding (see "Flooding"). Purpose: a flood mask must follow
   the bridge peers. (M)
3. Set the PON port's learning: on while any GEM interface is bridged (see
   "Bridging the GEM ports"). (M)
4. Rebuild the classifier entries that give bridged ports a GEM stream (see
   "Bridging the GEM ports"). (M)

VLAN membership, filtering and PVID
===================================

The VLAN table has one entry for each VID, holding the member ports, the ports
that send the VLAN untagged, the learning mode and the membership of the first
extension port. Each port has a PVID, the VID that the switch gives to an
untagged frame arriving on the port. One bit for the whole switch turns VLAN
filtering on: the switch then forwards every frame by VLAN table membership. The
bridge gives each of its ports a PVID whether or not it filters, and DSA
programs the bridge's VLANs into the table even while the bridge is not
filtering, so the entries are in place when filtering starts.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_VLAN_CFG`` (``0x013008``), ``RTL9602C_VLAN_CFG_AWARE`` (bit
       0): one bit for the whole switch, so the driver declares
       ``vlan_filtering_is_global`` to DSA, which refuses to give two bridges of
       the switch different settings. It is not the per-port ingress check (that
       register, at ``0x013004``, reads 0 and the driver never writes it): it
       makes the switch forward every frame by VLAN table membership, the frames
       of standalone ports included. Setting it changes no other register. Reset
       value 0. ``port_vlan_filtering`` writes it by read-modify-write.
     - M
     - A diff of the whole register file around setting the bit showed only this
       register changing; with no suitable entry for the frames' VLAN, setting
       it stopped all traffic (0 of 8 pings) and clearing it restored it at once
       (8 of 8); with a correct table, ``vlan_filtering`` toggled four times on
       a live bridge passed 5 of 5 pings each time.
   * - The VLAN table is table 1: ``RTL9602C_VLAN_TABLE_ENTRIES`` (4096) entries
       of one word, indexed directly by VID. An entry has 18 writable bits.
     - M
     - The second step of the table self-test at every setup.
   * - ``RTL9602C_VLAN_ENTRY_MEMBER_SET`` (bits 3:0) is the set of member ports;
       ``RTL9602C_VLAN_ENTRY_UNTAGGED_SET`` (bits 7:4) the member ports that
       send the VLAN untagged; ``RTL9602C_VLAN_ENTRY_IVL`` (bit 10) sets
       independent learning, clear being shared learning (see "Entry layout");
       ``RTL9602C_VLAN_ENTRY_EXT0_MEMBER`` (bit 11) is membership of the first
       extension port, the lowest bit of the extension member set in bits 17:11.
     - M
     - A bridge VLAN on the SFP port read back as members and untagged {0, 3},
       and removing the port cleared both; the learning bit as in "Entry
       layout".
   * - With filtering on, a frame reaches the CPU only if its VLAN's entry has
       the CPU port's member bit and ``RTL9602C_VLAN_ENTRY_EXT0_MEMBER``. The
       port that the frame enters by need not be a member.
     - M
     - Entries written by hand, four pings each: members and untagged {0, 3}
       0/4; the same with extension 0 4/4; with extension 1 instead 0/4; member
       {0} with extension 0 0/4; member {3} with extension 0 4/4 (the minimal
       working entry); member {3} alone 0/4; extension 0 alone 0/4.
   * - With filtering on, a frame goes to the member ports of its VLAN whatever
       port it enters by, a port outside the VLAN included. So the subscriber's
       frames tagged with the VLAN of a PPPoE session that the stick holds,
       whose members are the PON port and the CPU, go out on the fibre unless a
       rule drops them as they enter, which omcid writes.
     - M
     - 100 frames tagged 410 sent into the SFP port, not a member of VLAN 410,
       while the PON port and the CPU were: 100 arrived beyond the OLT; with a
       flower rule dropping VID 410 on the SFP port's ingress, 0.
   * - With filtering on, a frame that the L3 engine routes out of the PON port
       leaves only if the PON port is a member of the VLAN it leaves in. A
       PPPoE session over a VLAN device on a GEM interface therefore needs the
       PON port in the session's VLAN, which the GEM interface asks for by
       handing the VID of its VLAN device on to the PON port's VLAN filter (see
       the *GEM interfaces* section of ``pon.rst``).
     - M
     - With a bridge filtering VLANs 3001 and 3002 beside two PPPoE sessions in
       VLAN 410, the SFP port counted the routed upstream frames as forward
       discards (50 of 50) and none left; with the PON port added to VLAN 410,
       20 of 20 pings crossed, and the sessions carried 933 Mb/s down and 928
       up in hardware with the CPU 95 % idle or more.
   * - ``port_vlan_add`` also writes the PVID when the VLAN carries the PVID
       flag; ``port_vlan_del`` clears membership. A user port whose PVID VLAN is
       removed, or re-added without the flag, gets back the PVID of reset, 0,
       which no VLAN entry holds; the CPU port's PVID changes only with the
       flag. A failed table access returns the message
       ``the VLAN table did not answer``. DSA calls both for the CPU port for
       every VLAN that the host must see, so the driver infers nothing about the
       CPU's membership.
     - T, I
     - VLAN filtering, FDB learning and ageing work. That a user port with PVID
       0 drops untagged frames while filtering is on is not measured.
   * - VID 0 is never written to the table: ``port_vlan_add`` and
       ``port_vlan_del`` take it as done. DSA calls them with VID 0 when the
       8021q core adds VID 0 to the receive filter of a user port, which it
       does when a port that has receive VLAN filtering comes up; every user
       port of the switch has it while a bridge on the switch filters VLANs.
     - T
     - What a write of entry 0 does is the last fact of this table; that the
       driver no longer makes one is read from the code.
   * - ``RTL9602C_VLAN_PVID(p)`` (``0x01300c`` + ``4 * (p / 2)``),
       ``RTL9602C_VLAN_PVID_MASK`` (bits 11:0) at
       ``RTL9602C_VLAN_PVID_SHIFT(p)`` = ``12 * (p mod 2)``: the VID given to an
       untagged frame arriving on port p. Reset value 0.
     - M
     - A default PVID of 200 on the SFP port read the first word ``0x000000c8``;
       after a reset both words read 0.
   * - The switch puts an untagged frame into its ingress port's PVID before the
       address lookup, and with independent learning the lookup is keyed by that
       VID. The driver therefore replaces a VID 0 from DSA with the port's PVID
       in address-table and multicast operations, and a bridged port whose PVID
       differs from the VLAN that its peers learn in misses every lookup and
       floods.
     - M
     - GEM bridge: before the PON port had its GEM interfaces' VLANs, frames to
       the network side were flooded and every one reached the CPU; after, 75 of
       150,000 did.
   * - Writing VLAN table entry 0 stops the switch forwarding anything to the
       CPU port, not only VID 0's traffic, and writing the old word back does
       not undo it.
     - M
     - With the table self-test probing entry 0 the CPU received nothing for
       five minutes; with entry 0 skipped, 20 of 20 pings passed.
   * - VID 0 (priority-tagged frames) and VID 4095 are reserved and name no
       VLAN. The table self-test also leaves VID 1 alone: it is the bridge's
       default VLAN and so the entry most likely in use.
     - P
     - IEEE 802.1Q-2022, clause 9.6, Table 9-2 (reserved VID values).

``rtl9602c_vlan_set_member()``, which ``port_vlan_add`` and ``port_vlan_del``
call, in order:

1. Read the entry by index. Purpose: the other ports' bits must survive. (M)
2. For a member, set the port's member bit and set or clear its untagged bit;
   for a port that is not a member, clear both. Purpose: the two operations. (M)
3. For the CPU port, set or clear ``RTL9602C_VLAN_ENTRY_EXT0_MEMBER`` with the
   member bit. Purpose: a frame reaches the CPU only with both bits. (M)
4. When adding, set ``RTL9602C_VLAN_ENTRY_IVL``; it is never cleared again.
   Purpose: one learning mode for every VLAN that the bridge uses, which the
   address-table entries of the VLAN follow (``rtl9602c_l2_ivl()``); a VLAN
   left unwritten, VID 0 among them, stays in shared learning. (M)
5. Write the entry back by index. (M)

The steps are measured together: a bridge with ``vlan_filtering 1`` and a
default PVID of 200 read VID 200 back as members and untagged {0, 3} with
extension 0, and passed 10 of 10 pings. The word that the driver writes for that
entry is ``0x00000c99``: members 1001 in bits 3:0, untagged 1001 in bits 7:4,
``RTL9602C_VLAN_ENTRY_IVL`` and ``RTL9602C_VLAN_ENTRY_EXT0_MEMBER``.

The bridge of that measurement, as commands::

    ip link add br0 type bridge vlan_filtering 1 vlan_default_pvid 200
    ip link set sfp master br0

Frame length
============

The frame length limit, its two registers and the selector of each port, and the
operations ``port_change_mtu`` and ``port_max_mtu``, are described under
"Maximum frame length" on the *main* page; nothing about them is repeated here.

Mirroring
=========

A matchall rule with a mirred action on the ingress or the egress of a user
port asks DSA for port mirroring, and DSA calls ``port_mirror_add``. The switch
has one monitor port and one set of source ports, with an enable bit for each
direction, so it mirrors both directions together only for the same ports.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_MIRROR_CTRL`` (``0x01c094``):
       ``RTL9602C_MIRROR_CTRL_SRC_PORTS`` (bits 19:16) are the ports whose
       traffic is copied, one bit per port; ``RTL9602C_MIRROR_CTRL_INGRESS``
       (bit 4) copies what they receive and ``RTL9602C_MIRROR_CTRL_EGRESS``
       (bit 5) what they send, each for the whole set;
       ``RTL9602C_MIRROR_CTRL_DEST_PORT`` (bits 1:0) is the port that sends the
       copies. The frame itself still goes where it would have gone.
     - M
     - ``pon`` mirrored to ``sfp``, frames sent for 1 s and counted by the
       receive counter of the host's interface. From the network side to this
       device, which go to the CPU alone: 235,047 received for 235,000 sent
       with ``INGRESS``, and the CPU's count the same as without the rule.
       From the host to the network side: 217,044 came back for 217,000 sent
       with ``EGRESS``. With neither, 36 to 63, the other traffic of the link.
       Mirroring the SFP port's ingress to ``pon`` read ``0x00010012``.
   * - ``port_mirror_add`` keeps one monitor port and, for each direction, the
       ports that it mirrors; the source set is their union, and a direction's
       bit is set while it has a port. A request that names another monitor
       port is refused with ``-EBUSY``, and one after which the two directions
       would mirror different ports with ``-EOPNOTSUPP``, as the one source set
       cannot express that; source ports accumulate. ``port_mirror_del`` takes
       its port out of its rule's direction; the monitor field keeps its
       value, which is inert with no source.
     - M
     - Both directions of ``pon`` mirrored to ``sfp`` at once, as in the row
       above; with the egress rule removed, the copies of what ``pon`` sends
       stopped (39 frames received) and those of what it receives went on
       (239,038). An egress rule on ``sfp`` while ``pon``'s ingress was
       mirrored was refused.
   * - The mirror word survives a warm restart, so setup writes it 0: no source
       and no direction.
     - M
     - ``pon``'s ingress mirrored to ``sfp``, then a RAM boot of a kernel
       without the write: with no rule in place, 95,900 copies were captured
       at the host in 1 s. With the write, none: 37 frames received, the
       other traffic of the link.

For example, copy to the host what the PON port receives::

    tc qdisc add dev pon clsact
    tc filter add dev pon ingress matchall skip_sw \
            action mirred egress mirror dev sfp

and what it sends as well::

    tc filter add dev pon egress matchall skip_sw \
            action mirred egress mirror dev sfp

The S-VLAN block
================

The switch has registers for S-VLANs (IEEE 802.1ad service tags): the recognised
tag types, a policy word, an S-VID and two flags for each port, and a table of
sixteen translations from an S-VID and a port to a C-VID and a priority. An
S-VLAN's members are those of the VLAN table entry of its VID. Linux 6.18 gives
a DSA driver no S-VLAN request: a switchdev VLAN object carries no protocol, and
DSA does not pass a bridge's ``vlan_protocol`` on. The driver uses the block for
one thing, that the classifier sees S-tags, so that the VLAN rules of tc flower
can match and change them (see "VLAN tag operations"); it forwards no frame by
S-VLAN. ``rtl9602c_svlan_setup()``, at setup, puts the SFP and PON ports in
S-VLAN uplink mode and makes every frame without an S-tag join S-VLAN
``RTL9602C_SVLAN_PVID_NONE`` (4095), of which every port is an untagged member,
so that such a frame leaves as it came. The ``svlan`` view reads the block back.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_SVLAN_TPID_CFG`` (``0x0230c8``) is 64 bits with the high word
       at the lower address (``0x0230c8``) and the low word at ``0x0230cc``. Out
       of reset the low word reads ``0x810088a8``:
       ``RTL9602C_SVLAN_TPID_CFG_PRIMARY`` (bits 15:0) = ``0x88a8`` and
       ``RTL9602C_SVLAN_TPID_CFG_SECOND`` (bits 31:16) = ``0x8100``, and
       ``RTL9602C_SVLAN_TPID_CFG_SECOND_EN`` (bit 0 of the high word) is clear.
     - M
     - Register reads after a reset; through the driver, ``svlan`` prints
       ``tpid 88a8 second 8100 disabled``.
   * - 0x88a8 is the S-tag EtherType and 0x8100 the C-tag's.
     - P
     - IEEE 802.1Q-2022, clause 9, Table 9-1 (802.1Q EtherType allocations).
   * - ``RTL9602C_SVLAN_POLICY`` (``0x01400c``) with
       ``RTL9602C_SVLAN_POLICY_INGRESS_FILTER`` (bit 27),
       ``RTL9602C_SVLAN_POLICY_KEEP_DEI`` (bit 21),
       ``RTL9602C_SVLAN_POLICY_PCP`` (bits 20:18),
       ``RTL9602C_SVLAN_POLICY_UNTAGGED_ACT`` (bits 3:2) and
       ``RTL9602C_SVLAN_POLICY_PCP_SRC`` (bits 1:0); ``RTL9602C_SVLAN_PVID(p)``
       (``0x014004`` + ``4 * (p / 2)``), twelve bits
       (``RTL9602C_SVLAN_PVID_MASK``) at ``RTL9602C_SVLAN_PVID_SHIFT(p)`` =
       ``12 * (p mod 2)``; ``RTL9602C_SVLAN_UPLINK_PORTS`` (``0x0230c4``) and
       ``RTL9602C_SVLAN_DMAC_VID_SEL`` (``0x014000``), one bit per port. All
       read 0 out of reset.
     - M
     - Register reads after a reset; through the driver, the ``policy`` line and
       the per-port table of ``svlan``.
   * - ``RTL9602C_SVLAN_XLATE(i)`` (``0x02a018`` + 4i),
       ``RTL9602C_SVLAN_XLATE_ENTRIES`` (16): ``RTL9602C_SVLAN_XLATE_VALID``
       (bit 0), ``RTL9602C_SVLAN_XLATE_SVID`` (bits 12:1),
       ``RTL9602C_SVLAN_XLATE_PORT`` (bits 14:13), ``RTL9602C_SVLAN_XLATE_CVID``
       (bits 26:15) and ``RTL9602C_SVLAN_XLATE_PRIO`` (bits 29:27). None is
       valid out of reset; the view prints valid entries only.
     - M
     - Register reads after a reset; ``svlan`` lists no entry.
   * - A port in ``RTL9602C_SVLAN_UPLINK_PORTS`` recognises the S-tag of a
       frame coming in: the frame is in the S-VLAN of the tag, and the
       classifier sees the tag (``RTL9602C_CLS_RULE_STAG``; see the *acl-qos*
       page). Of a frame without one, ``RTL9602C_SVLAN_POLICY_UNTAGGED_ACT``
       decides: 0, the value out of reset, and 1 drop it; 2
       (``RTL9602C_SVLAN_POLICY_UNTAGGED_JOIN``) puts it in the S-VLAN of the
       port's ``RTL9602C_SVLAN_PVID``, and so did 3. A frame from a port not in
       the mode is in the S-VLAN of its port's ``RTL9602C_SVLAN_PVID``, an
       802.1ad tag at its front counting as data.
     - M
     - With the PON port alone in the mode, 100 frames each from the network
       side: under 0 and 1, untagged and C-tagged frames were lost and frames
       of S-VID 200, alone or in front of C-VID 100, passed; under 2 and 3 all
       of them passed. Rules that match an S-tag act on frames taken in by
       such a port and on no others ("VLAN tag operations").
   * - A frame leaves a port with an S-tag of the S-VLAN it is in unless the
       port is in the untagged set of that VID's VLAN table entry, whether or
       not the port is in S-VLAN uplink mode; the tag a frame came with is kept
       or taken off the same way.
     - M
     - With the PON port alone in the mode and every port's S-VLAN PVID 0,
       whose VLAN entry the driver never writes: every frame from the SFP port
       left by the PON port with an S-tag of VID 0 in front of it, an 802.1ad
       tag it came with included, and every frame without an S-tag from the
       PON port, under action 2 or 3, left by the SFP port with one; frames of
       S-VID 200 from the PON port left with their tag. With every PVID 4095
       and the entry of 4095 untagged on every port, all of them left as they
       came.
   * - ``rtl9602c_svlan_setup()`` writes the VLAN entry of
       ``RTL9602C_SVLAN_PVID_NONE`` with every port a member and untagged
       (``rtl9602c_vlan_set_member()``), every port's
       ``RTL9602C_SVLAN_PVID`` to that VID, the action
       ``RTL9602C_SVLAN_POLICY_UNTAGGED_JOIN``, and the SFP and PON ports
       into ``RTL9602C_SVLAN_UPLINK_PORTS``. So a frame without an S-tag is in
       S-VLAN 4095 and leaves as it came, and a frame with one keeps its tag
       unless a rule changes it. VID 4095 is reserved by IEEE 802.1Q, so no
       bridge VLAN shares the entry, and the table self-test leaves it alone.
     - M
     - Read back through ``svlan``: ``untag 2``, S-VID 4095 on every port and
       uplink on ports 0 and 2. With no rule, 100 frames each untagged,
       C-tagged, with an S-tag alone and with one in front of a C-tag crossed
       both ways unchanged; management over VLAN 4094 on the SFP port, the
       data path and the device tests ran as before (``docs/testing``).
   * - VID 4095 is reserved for implementation use and is never assigned to a
       VLAN.
     - P
     - IEEE 802.1Q-2022, clause 9.6, Table 9-2 (reserved VID values).
   * - The S-VLAN and C-VLAN of a VID share its VLAN table entry, so a bridge
       VLAN that is untagged on a port also takes the S-tag of that S-VID off
       frames leaving by that port.
     - I
     - Follows from the row before the driver's setup; not tested with a
       bridge VLAN.
   * - What the other fields do, as their names say: the second TPID and its
       enable; ``RTL9602C_SVLAN_POLICY_INGRESS_FILTER``, keeping the received
       DEI, the fixed S-tag priority and the source of an egress S-tag's
       priority; each port's ``RTL9602C_SVLAN_DMAC_VID_SEL``; and the
       translation table. The driver writes none of them, and they keep their
       values out of reset.
     - I
     - None of it is driven.

Reserved multicast addresses
============================

The switch has a control for each of nineteen well-known destination addresses,
and one global word. A control says what the switch does with frames to its
address before it forwards them. The driver uses one bit of five of them, the
exemption from storm control, and the action of one: frames to 01-80-C2-00-00-0E
go to the CPU only, as no bridge forwards them (IEEE 802.1Q 8.6.3) and the Linux
bridge, which receives them, does not either. The ``rma`` view displays the
rest. Storm control
itself is on the *acl-qos* page. The registers are named after the last byte of
the address 01:80:c2:00:00:xx that they are taken to control; the suffixes skip,
so an address is not the register's position in the block.

.. list-table::
   :header-rows: 1
   :widths: 26 40 14 20

   * - Address
     - Register
     - Offset
     - ``rma`` label
   * - 01:80:c2:00:00:00
     - ``RTL9602C_RSVDMC_CTRL_00``
     - ``0x01c038``
     - stp
   * - 01:80:c2:00:00:01
     - ``RTL9602C_RSVDMC_CTRL_01``
     - ``0x01c03c``
     - pause
   * - 01:80:c2:00:00:02
     - ``RTL9602C_RSVDMC_CTRL_02``
     - ``0x01c040``
     - slow
   * - 01:80:c2:00:00:03
     - ``RTL9602C_RSVDMC_CTRL_03``
     - ``0x01c044``
     - dot1x
   * - 01:80:c2:00:00:04
     - ``RTL9602C_RSVDMC_CTRL_04``
     - ``0x01c048``
     -
   * - 01:80:c2:00:00:08
     - ``RTL9602C_RSVDMC_CTRL_08``
     - ``0x01c04c``
     - provider
   * - 01:80:c2:00:00:0d
     - ``RTL9602C_RSVDMC_CTRL_0D``
     - ``0x01c050``
     -
   * - 01:80:c2:00:00:0e
     - ``RTL9602C_RSVDMC_CTRL_0E``
     - ``0x01c054``
     - lldp
   * - 01:80:c2:00:00:10
     - ``RTL9602C_RSVDMC_CTRL_10``
     - ``0x01c058``
     -
   * - 01:80:c2:00:00:11
     - ``RTL9602C_RSVDMC_CTRL_11``
     - ``0x01c05c``
     -
   * - 01:80:c2:00:00:12
     - ``RTL9602C_RSVDMC_CTRL_12``
     - ``0x01c060``
     -
   * - 01:80:c2:00:00:13
     - ``RTL9602C_RSVDMC_CTRL_13``
     - ``0x01c064``
     -
   * - 01:80:c2:00:00:18
     - ``RTL9602C_RSVDMC_CTRL_18``
     - ``0x01c068``
     -
   * - 01:80:c2:00:00:1a
     - ``RTL9602C_RSVDMC_CTRL_1A``
     - ``0x01c06c``
     -
   * - 01:80:c2:00:00:20
     - ``RTL9602C_RSVDMC_CTRL_20``
     - ``0x01c070``
     -
   * - 01:80:c2:00:00:21
     - ``RTL9602C_RSVDMC_CTRL_21``
     - ``0x01c074``
     -
   * - 01:80:c2:00:00:22
     - ``RTL9602C_RSVDMC_CTRL_22``
     - ``0x01c078``
     -
   * - a Cisco protocol address
     - ``RTL9602C_RSVDMC_CTRL_CDP``
     - ``0x01c07c``
     - cisco cdp
   * - a Cisco protocol address
     - ``RTL9602C_RSVDMC_CTRL_SSTP``
     - ``0x01c080``
     - cisco sstp
   * - (all)
     - ``RTL9602C_RSVDMC_GLOBAL_CFG``
     - ``0x01c084``
     - ``cfg`` line

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Twenty registers, ``0x01c038`` to ``0x01c084``, say what the switch does
       with frames to well-known destinations before it forwards them: nineteen
       per-address controls four bytes apart, then
       ``RTL9602C_RSVDMC_GLOBAL_CFG``. The suffix of a control is the last byte
       of 01:80:c2:00:00:xx, apart from ``RTL9602C_RSVDMC_CTRL_CDP`` and
       ``RTL9602C_RSVDMC_CTRL_SSTP``, two Cisco protocol addresses. The suffixes
       skip, so base + 4n names the wrong address from the sixth register on;
       the driver names every register. Out of reset every word reads 0 except
       ``_01`` and ``_02``, which read ``0x00000020``
       (``RTL9602C_RSVDMC_CTRL_ACTION`` = 2).
     - M
     - Register reads after a reset. Through the driver, ``rma``, column ``raw``
       (after setup, bit 3 is also set on the five entries of the fourth row).
   * - ``RTL9602C_RSVDMC_CTRL_00`` controls frames to 01:80:c2:00:00:00.
     - M
     - The measurement of the next row.
   * - ``RTL9602C_RSVDMC_CTRL_STORM_EXEMPT`` (bit 3) set: frames to that address
       are not policed by storm control, which stays in force for everything
       else.
     - M
     - Unknown-multicast storm control at 1 packet a second on port 0, with the
       frame capture armed in drop mode: with the bit clear a frame to
       01:80:c2:00:00:00 was captured as dropped; with it set none was in 14 s;
       with it set a frame to 01:00:5e:01:02:03 was still captured as dropped.
   * - In setup the driver sets ``RTL9602C_RSVDMC_CTRL_STORM_EXEMPT`` on
       ``_00``, ``_01``, ``_02``, ``_03`` and ``_0E``, by read-modify-write
       because ``_01`` and ``_02`` carry an action out of reset. Spanning tree,
       pause, slow protocols, 802.1X and LLDP arrive as multicast that no table
       knows, and a broadcast or unknown-multicast limit on a port would
       otherwise police them along with the flood.
     - M
     - Storm control as in the previous row, after the change: to
       01:80:c2:00:00:00 nothing was dropped in 14 s, to 01:00:5e:01:02:03
       frames were. ``rma``, column ``storm`` reads 1 on those five.
   * - The destinations: 01-80-C2-00-00-00 the bridge group address (spanning
       tree), -01 MAC control (PAUSE), -02 Slow Protocols (LACP), -03 the 802.1X
       port access entity, -0E the nearest-bridge address (LLDP).
     - P
     - IEEE 802.1Q-2022, clause 8.6.3, Table 8-1.
   * - ``RTL9602C_RSVDMC_CTRL_0E`` controls frames to 01-80-C2-00-00-0E, and
       its ``RTL9602C_RSVDMC_CTRL_ACTION`` (bits 5:4) says where they go: 0
       forwards them as other frames, the CPU included; 1
       (``RTL9602C_RSVDMC_CTRL_ACTION_TRAP``) takes them to the CPU only; 2
       drops them; 3 forwards them but not to the CPU. A PTP message trapped
       so keeps its timestamp tag (reason 240). In setup the driver writes 1.
     - M
     - Three Pdelay_Reqs to 01-80-C2-00-00-0E from a host on the SFP side, in
       a VLAN bridged to the PON port, for each value: 0, all three reached
       the lab OLT's side and the CPU; 1, the CPU only, a packet socket on
       ``sfp`` receiving them with hardware stamps; 2, neither; 3, the lab
       OLT's side only.
   * - The registers other than ``_00`` and ``_0E`` control the address their
       suffix names. The two non-zero reset values falling on the pause and
       slow-protocol entries fit.
     - I
     - An exemption test as in the third row, for each address. The driver
       depends on it for ``_01``, ``_02`` and ``_03``.
   * - The action values mean the same on the other controls, whose actions
       the driver leaves as reset leaves them.
       ``RTL9602C_RSVDMC_CTRL_KEEP_TAG`` (bit 2),
       ``RTL9602C_RSVDMC_CTRL_VLAN_BYPASS`` (bit 1),
       ``RTL9602C_RSVDMC_CTRL_ISOLATE_BYPASS`` (bit 0) and
       ``RTL9602C_RSVDMC_GLOBAL_CFG`` are only displayed (``rma`` columns
       ``keep``, ``vleak``, ``isoleak``, and the ``cfg`` line); what the three
       flags and the global word mean is unverified.
     - I
     - The test of ``_0E`` for each address. Nothing depends on them.

VLAN tag operations
===================

The service that the stick exists for is a single-tag VID translation: in ITU-T
G.988 terms (clause 9.3.13, the extended VLAN tagging operation configuration
data), a rule that matches one C-VID and assigns another. The same managed
entity also gives an untagged frame a tag and takes it off again in the other
direction, puts an S-tag in front of a C-tag and takes it off again (IEEE
802.1ad), rewrites a tag, and sets a tag's priority, and a rule may take only
the untagged frames of one EtherType. The driver offers these
operations as tc flower rules on the ingress of a user port, one vlan action
each or a few that together make one tag operation, which the OMCI daemon
installs, and turns each rule into one entry of the classifier, or into one per
priority where GEM interfaces divide the VLAN that the rule leads into (below).
The classifier's entry format and the sequence that programs an entry are on
the *acl-qos* page; VLAN membership (``port_vlan_add``) is a different block.

A translation from VID 410 to VID 1410, a C-tag of VID 410 and priority 5 given
to untagged frames from the subscriber side, and that tag taken off frames from
the network side::

    tc qdisc add dev sfp clsact
    tc qdisc add dev pon clsact
    tc filter add dev sfp ingress protocol 802.1Q flower skip_sw \
            vlan_id 410 action vlan modify id 1410
    tc filter add dev sfp ingress protocol all flower skip_sw \
            num_of_vlans 0 action vlan push id 410 priority 5
    tc filter add dev pon ingress protocol 802.1Q flower skip_sw \
            vlan_id 410 action vlan pop

An S-tag of S-VID 200 and priority 3 put in front of C-VID 100 upstream, and
taken off again downstream::

    tc filter add dev sfp ingress protocol 802.1Q flower skip_sw \
            vlan_id 100 action vlan push id 200 priority 3 protocol 802.1ad
    tc filter add dev pon ingress protocol 802.1ad flower skip_sw \
            vlan_id 200 action vlan pop

A translation from VID 410 to VID 1410 that gives the tag priority 0, which a
``vlan modify`` cannot give (see below), takes the tag off and pushes another::

    tc filter add dev sfp ingress protocol 802.1Q flower skip_sw \
            vlan_id 410 action vlan pop action vlan push id 1410 priority 0

An untagged frame given C-VID 410 with priority 5 behind S-VID 200 with
priority 3 upstream, and both tags taken off again downstream::

    tc filter add dev sfp ingress protocol all flower skip_sw \
            num_of_vlans 0 action vlan push id 410 priority 5 \
            action vlan push id 200 priority 3 protocol 802.1ad
    tc filter add dev pon ingress protocol 802.1ad flower skip_sw \
            vlan_id 200 vlan_ethtype 802.1Q action vlan pop action vlan pop

Untagged PPPoE frames, discovery and session, given a C-tag of VID 35 ahead
of every other untagged frame, given VID 10; one rule for each EtherType,
ahead of the other by tc priority::

    tc filter add dev sfp ingress pref 10 protocol 0x8863 flower skip_sw \
            num_of_vlans 0 action vlan push id 35
    tc filter add dev sfp ingress pref 11 protocol 0x8864 flower skip_sw \
            num_of_vlans 0 action vlan push id 35
    tc filter add dev sfp ingress pref 20 protocol all flower skip_sw \
            num_of_vlans 0 action vlan push id 10

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A rule matches an untagged frame (``num_of_vlans 0``); an outer C-tag
       (``protocol 802.1Q``) with its VID and its priority if the rule names
       them; or an outer S-tag (``protocol 802.1ad``) with the S-tag's VID and
       priority, or with those of the C-tag behind it (``cvlan_id``,
       ``cvlan_prio``), and with ``vlan_ethtype 802.1Q`` only if a C-tag is
       behind it. It becomes one classifier entry that compares the C-tag and
       S-tag bits, the VID and the priority it names, taken from the tag they
       belong to by the entry's template, the direction, and for an upstream
       rule the port it is on.
     - M
     - A translation's entry read back from the classifier's table: action word
       1 = ``0x00042c12`` (C-VID assign, 1410, tag), rule word 0 =
       ``0x000cd008`` on ``sfp`` (C-tag present, VID 410 at bit 11) with bit 31
       also set for the same rule on ``pon``; ``tc filter del`` zeroed the rule.
       With ``vlan_prio 5`` in the match, 100 frames of 410 with priority 5
       were translated and 100 with priority 2 were not. Without ``skip_sw``,
       tc installs a rule that no driver took in software and reports success.
       Through the driver: debugfs ``rtl9602c/cf``. Of 100 frames each, a pop
       on S-VID 200 on ``pon`` took the S-tag off 200 in front of C-VID 100
       and off 200 alone, and left 201 in front of 100 and C-VID 200 as they
       were; with ``vlan_prio 3`` it took priority 3 and not 5; a pop on
       ``cvlan_id 100`` took the S-tag off 200 in front of 100 and left 200 in
       front of 101, and 200 alone; a pop on S-VID 200 with ``vlan_ethtype
       802.1Q`` took 200 in front of 100 and left 200 alone. The same on
       ``sfp`` upstream.
   * - The actions of a rule: ``vlan modify`` of the outer tag gives it a new
       VID, and a new priority unless the action gives 0, a C-tag staying a
       C-tag and an S-tag staying an S-tag with ``protocol 802.1ad`` or
       becoming a C-tag with ``protocol 802.1Q``; ``vlan push`` of a C-tag
       (802.1Q) gives an untagged frame a C-tag; ``vlan push`` of an S-tag
       (802.1ad) puts an S-tag, with the TPID of
       ``RTL9602C_SVLAN_TPID_CFG_PRIMARY`` (``0x88a8``), in front of a frame
       without one; ``vlan pop`` takes the outer tag off, and two of them on
       an S-tag the S-tag and the C-tag behind it. A push gives the tag the
       action's VID and priority.
     - M
     - 100 frames each, through an OLT that is a transparent bridge: untagged
       frames upstream left as C-VID 410 with priority 5, and downstream as 410
       with priority 4, while C-tagged frames passed those rules unchanged;
       C-VID 410 downstream, and upstream, lost its tag; C-VID 100 with
       priority 1 upstream left as S-VID 200 with priority 3 in front of it,
       and untagged frames as S-VID 300 with priority 2; ``vlan modify id 1410
       priority 6`` turned 410 with priority 5 into 1410 with priority 6. S-VID
       200 with priority 3 in front of C-VID 100 left downstream as S-VID 300
       with priority 4 under a modify with ``protocol 802.1ad``, and as C-VID
       300 with priority 4 in front of 100 with ``protocol 802.1Q``; two pops
       left it, and 200 alone, untagged; upstream a modify turned S-VID 200
       with priority 3 into 400 with priority 1.
   * - A ``vlan modify`` that gives priority 0 keeps the frame's priority
       instead: the action reaches the driver without saying whether it gives
       a priority at all, and keeping it is what a modify without one does.
       A rule that gives priority 0 pops the tag and pushes another (next
       row).
     - I
     - The flow action's VLAN fields (``struct flow_action_entry``): a VID, a
       protocol and a priority, and no flag for a priority given.
   * - A ``vlan pop`` of the outer tag followed by a ``vlan push`` of a tag of
       the same protocol becomes the entry of a ``vlan modify`` with the
       priority assigned: the push's VID and its priority, which a push always
       names, so priority 0 is written rather than kept. A pop and a push of
       the other protocol are refused.
     - T, I
     - T: the KUnit case ``rtl9602c_test_vlan_rewrite`` reads back the
       modify's action word, ``0x00042c12``, with
       ``RTL9602C_CLS_ACT_CPRI_SET`` added and the priority 0. I: not
       measured on the board; 100 frames of C-VID 410 and priority 5 under the
       rule of the example above, leaving the far side of the OLT as 1410 with
       priority 0, would settle it.
   * - A C-tag that the actions write, pushed onto an untagged frame or put
       over the outer C-tag by ``vlan modify`` or by a pop and a push,
       followed by a ``vlan push`` of an S-tag, becomes one entry that acts on
       both tags: its C-tag plane writes the C-tag and its S-tag plane puts
       the S-tag in front of it, each as it does alone (see the *acl-qos*
       page, "Entries and rules"). An upstream entry takes the stream of the
       GEM interface of the S-VID, and where the GEM interfaces of that VLAN
       divide its priorities, of the S-tag's priority: the outer tag's, as for
       every other rule. As the entry writes that priority, it stays one entry
       (the per-priority row below).
     - T, I
     - T: the KUnit cases ``rtl9602c_test_vlan_two_tags`` and
       ``rtl9602c_test_vlan_two_tags_flow``. I: the two planes together in
       one entry are not measured; untagged frames under the rule of the
       example above leaving the far side of the OLT as S-VID 200 with
       priority 3 in front of C-VID 410 with priority 5, and frames of S-VID
       200 in front of C-VID 410 sent from there arriving untagged, would
       settle it.
   * - The priority fields of the two planes hold more than an assignment:
       ``RTL9602C_CLS_ACT_SPRI_SET`` is the low bit of a field at bits 20:18
       of action word 0, and ``RTL9602C_CLS_ACT_CPRI_SET`` of one at bits
       23:21 of word 1. Their other values are taken to copy the priority of
       the frame's outer or inner tag, to give the switch's internal
       priority, or to derive one from the frame's DSCP. The driver writes 1,
       the assignment, or 0. A tc vlan action cannot ask for a copy, as a push
       names its priority: an S-tag that takes the priority of the C-tag
       behind it is pushed by one rule for each priority.
     - I
     - Not measured, and not driven. Each value under frames of two
       priorities in two tags, both ways, read at the far side of the OLT,
       would settle it.
   * - A rule on an untagged frame may name its EtherType, in full:
       ``protocol`` with the EtherType, which flower carries in its basic key,
       or ``protocol 0x8864``, a PPPoE session, which it carries in its PPPoE
       key and which the rule then matches by the EtherType alone. The rule's
       entry, of the classifier's pattern 1, compares it in rule word 1 (see
       the *acl-qos* page, "Entries and rules"). A rule on a tag names none:
       which EtherType the classifier compares on a tagged frame, the one
       behind the tags or the outer tag's TPID, is not known yet. The rules
       keep tc's order, so a rule on an EtherType ahead of one on every
       untagged frame takes the frames of its EtherType.
     - T, I
     - T: the KUnit cases ``rtl9602c_test_vlan_etype``, which reads the
       EtherType out of the keys, and ``rtl9602c_test_cf_planes``, which
       reads the planes. I: not measured; PPPoE discovery and session frames
       and IPv4 frames from the host under the rules of the example above,
       arriving at the far side of the OLT in VLANs 35, 35 and 10, 100 of each,
       would settle it, and all three in VLAN 10 with the last rule at
       ``pref 5`` instead.
   * - Refused with ``-EOPNOTSUPP`` and a message: egress rules (the classifier
       acts on ingress); any key but the VLAN tags, with the basic and control
       keys, and the EtherType of an untagged frame, with the PPPoE key; an
       EtherType behind a tag, one in part, a length (below ``0x0600``), a PPPoE
       session's number or PPP protocol, and an IP protocol; a VID or priority
       with a partial mask; a DEI match; ``num_of_vlans`` other than 0, or with
       a VLAN key; an outer tag that is neither 802.1Q nor 802.1ad, or with no
       protocol; a second tag behind an outer C-tag, or anything but a C-tag
       behind an S-tag; the VID or priority of both an S-tag and the C-tag
       behind it; more than one action, but two pops on an S-tag, a pop and a
       push of the outer tag's protocol, and a C-tag written with an S-tag
       pushed in front of it (the rows above); any action but ``vlan modify``,
       ``push`` and ``pop`` (a drop, trap or pass goes to the ACL, and
       ``police`` on a destination-address match to storm control, both on the
       *acl-qos* page); a modify of an untagged frame, or of a C-tag into
       anything but a C-tag; a C-tag pushed onto a tagged frame, or an S-tag
       onto a frame with one; a pop of an untagged frame. With no free entry:
       ``-ENOSPC`` and the message ``the classifier is full``.
     - M
     - ``num_of_vlans 1`` with a VID, a pop with ``num_of_vlans 0``, a C-tag
       pushed onto a C-tag, an S-tag pushed onto an S-tag, two pops on a
       C-tag, an S-VID with a C-VID, and an IPv4 EtherType behind an S-tag
       were each refused with their message; the other refusals take the
       same path.
   * - The direction is the port that the rule is on: a rule on the PON port
       acts on downstream frames, on any other port on upstream frames. The
       direction bit is compared in every entry, so an upstream rule does not
       also act on downstream frames that carry the same VID, and an upstream
       rule compares the ingress port, so it acts on no other port's frames.
     - M
     - In the test of the next row, with both rules in place, frames of 1410
       sent upstream and of 410 sent downstream crossed unchanged: neither rule
       acted in the other direction.
   * - The translation itself: with the entry as the first row writes it, a
       frame with C-VID 410 entering the SFP port and leaving by the PON port
       leaves as 1410, and the reverse for a rule on ``pon``. It needs
       ``RTL9602C_CLS_CTRL_PON_EN``, which setup sets (see the *acl-qos*
       page).
     - M
     - Through an OLT that is a transparent bridge, 200 frames of C-VID 410
       and PCP 5 sent into the SFP port from the host arrived at the OLT's
       uplink as 1410 with PCP 5, and 200 of 1410 sent from there arrived at
       the host as 410; frames of 1410 upstream, of 410 downstream and untagged
       ones crossed unchanged, and with the rules deleted every frame crossed
       as sent. Before the driver set the tag action and the PON enable, the
       same entries changed nothing.
   * - The switch recognises an S-tag only on a port in S-VLAN uplink mode
       (``RTL9602C_SVLAN_UPLINK_PORTS``), and the driver puts the SFP and PON
       ports in it, with every frame without an S-tag passing as it came (see
       "The S-VLAN block"). A rule on an untagged frame or on an outer C-tag
       compares the S-tag bit as 0, so it leaves a frame with an S-tag alone.
     - M
     - With no rule, 100 frames each untagged, C-tagged, with S-VID 200 alone
       and with 200 in front of C-VID 100 crossed both ways unchanged. A push
       onto untagged frames left S-VID 200 as it was, both ways, and a pop of
       C-VID 410 downstream left 200 in front of 410. Before the driver used the
       mode, no rule matched an S-tag, and such a push turned S-VID 200 in
       front of C-VID 100 into C-VID 410 in front of both.
   * - Every rule on a C-tag, and every stream entry that matches a VID (see
       "Bridging the GEM ports"), requires the frame to carry a C-tag at its
       front, so a frame with an outer 802.1ad tag matches none of them.
     - M
     - Under a translation of 3001 to 3002 on ``sfp`` and of 3002 to 3001 on
       ``pon``, 200 frames with an 802.1ad tag (TPID ``0x88a8``, VID 100) in
       front of a C-tag of 3001 crossed upstream untranslated, as did 200 with
       3002 inside downstream; upstream they had their port's stream, whose
       entry compares no tag. That was before the driver recognised S-tags,
       when the switch saw no tag at all on such a frame; now it sees the
       S-tag, which those entries compare as absent.
   * - Of a frame with two C-tags, a translation rewrites the outer one and
       leaves the inner one as it is.
     - M
     - Under the same translations, 200 frames tagged 3001 outside 3001 sent
       upstream arrived as 3002 outside 3001, and 200 tagged 3002 outside 3002
       sent downstream as 3001 outside 3002.
   * - The classifier applies the lowest-numbered entry that a frame matches.
     - M
     - Against an operator's OLT, a pass-through entry for the same VID at a
       lower index applied instead of the translation, until it was removed,
       after which PPPoE over the translated VLAN came up within 15 s.
   * - The stream entries of "Bridging the GEM ports" take the top of the
       classifier's pattern 1, downward, and the tc rules the entries below
       them: in pattern 1 a rule on a C-tag or an untagged frame, in pattern 0,
       below pattern 1, a rule on an S-tag (see the *acl-qos* page, "The
       classifier"). So with the previous row a user rule takes precedence
       over its port's stream entry. The rules of a pattern keep the order of
       their tc priorities, so of two rules that match one frame the one tc
       takes first applies.
     - T, I
     - T: with an earlier driver, which took the rules from the bottom of its
       128 entries, the stream entry read back at the top index (see "Bridging
       the GEM ports"), and routing tests in which the routed port's stream
       entry is found where the driver put it; the KUnit cases
       ``rtl9602c_test_cf_order`` and ``rtl9602c_test_cf_patterns`` for the
       order and the patterns. I: the entries of pattern 1, and the order
       across the two patterns, are not yet on the board (see the *acl-qos*
       page, "Open questions").
   * - An upstream rule therefore carries the stream itself: when an upstream
       GEM interface claims the VID that the rule's modify or push writes (see
       "Bridging the GEM ports"), the entry also sets
       ``RTL9602C_CLS_ACT_FLOW_SET`` with that interface's flow in
       ``RTL9602C_CLS_ACT_FLOW``; otherwise, and for a pop, it sets the flow
       that its port's stream entry gives, which the frame would have had
       without the rule. A frame without a stream reaches the PON MAC without a
       GEM port and is lost. The upstream rules are written again whenever the
       stream entries change.
     - M
     - With no VLAN claimed, an entry without a stream lost all 200 frames of
       its VID, and with the port's stream they arrived translated. With the
       two upstream GEM interfaces of "Bridging the GEM ports" claiming 3001
       and 3002, a translation of 3005 to 3002 named the second interface's
       flow and one of 3006 to 3001 the first's, and the 200 frames of each
       were counted in that port's upstream octets.
   * - Where the upstream GEM interfaces of the VLAN that a rule writes divide
       its priorities (see "Bridging the GEM ports"), the stream depends on the
       priority a frame leaves with. A rule that writes a priority, a push
       among them, or that matches one keeps its one entry, with the stream of
       that priority. A rule that keeps any priority, a ``vlan modify`` with
       priority 0 that matches none, becomes eight entries instead, one for
       each priority of the tag it matches, the C-tag or the S-tag. Each has
       the stream of its priority, or none for a priority that no GEM
       interface takes, whose frames are then lost, as a mapper discards them.
       The entries follow the GEM interfaces: a rule into a VLAN that comes to
       be divided takes the eight, and gives up seven when one interface takes
       the VLAN whole again. A rule that rewrites an S-tag and matches the
       C-tag behind it stays one entry, with the stream of priority 0 and a
       warning; so does a rule already in place whose VLAN comes to be divided
       when the classifier has no room for seven more. A new rule that finds
       fewer than eight entries free is refused with ``-ENOSPC`` and the
       message ``the classifier has no room for this rule's eight entries, one
       for each priority of the VLAN it leads into``. Downstream nothing
       changes: a downstream rule names no stream, and the frames of all the
       GEM ports of a VLAN reach it alike, as a mapper sends them all to its
       root.
     - P, I
     - P for the mapper, ITU-T G.988 clause 9.3.10: a priority whose pointer
       is null is discarded, and downstream every GEM port leads to the root.
       I for the entries, as the *acl-qos* page describes them ("What the
       driver writes"), checked by the driver's KUnit cases; not yet on the
       board (see "Open questions").

Bridging the GEM ports
======================

A GEM interface is a network interface of the PON class, stacked on ``pon``; it
is not a DSA port. The switch sees every GEM port as its one PON port, and the
driver keeps the PON port's isolation, learning and VLAN membership as those of
the GEM interfaces' bridge. The PON class calls ``gem_bridge_join`` and
``gem_bridge_leave`` when a GEM interface joins or leaves a bridge, and
``gem_vlan_add`` and ``gem_vlan_del`` for the bridge's VLANs on it (see the *PON
class* page).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A GEM interface (a ``gem`` netdev of the PON class, not a DSA port) put
       in a bridge takes the PON port into that bridge: the switch then forwards
       between the PON port and the bridge's other ports without the CPU.
     - M
     - On an OLT: downstream 1,271,133 frames at 1 Gb/s line rate, none lost, 65
       reaching the CPU; upstream about 885 Mb/s against a grant of about 990
       Mb/s. A throughput test measured 910 Mb/s down and 988 Mb/s up bridged;
       bridged traffic passes both ways.
   * - All GEM interfaces share one bridge: the switch isolates by port, and the
       PON port is one port whatever GEM port a frame is in. Joining a second
       bridge is refused with ``-EBUSY`` and the message ``the GEM interfaces
       can be in one bridge only``. The driver counts the members and forgets
       the bridge when the last one leaves.
     - T
     - Bridged traffic passes both ways, sourced from the host.
   * - The PON port learns while GEM interfaces are bridged and not otherwise:
       DSA turns learning on for the ports that it bridges and never bridges the
       PON port itself. Without it every frame toward the network side was
       flooded, a copy to the CPU included. When the last GEM interface leaves,
       the PON port's learned entries are flushed as stale.
     - M
     - Before and after: without learning every frame was flooded; with it, in a
       line-rate burst the CPU sees fewer than 500 frames.
   * - A bridged GEM interface's VLANs are the PON port's: the driver passes
       every VLAN that the bridge gives a GEM interface to ``port_vlan_add`` for
       the PON port, and to ``port_vlan_del`` when no GEM interface of the
       bridge has the VLAN any more, so that the port's PVID and membership
       match its bridge peers (see "VLAN membership, filtering and PVID").
     - M
     - Frames to the CPU during an upstream burst fell from all of them to 75 of
       150,000.
   * - A frame that the switch forwards to the PON port leaves in the GEM port
       of its stream, and the PON MAC drops a frame that has none (see the *PON*
       page). A frame that the switch forwards, unlike one that the CPU sends,
       has a stream only if a classifier entry gave it one.
       ``rtl9602c_cf_sid_refresh()`` therefore gives every bridged subscriber
       port a stream, and builds all its entries again on each membership or
       VLAN change: while no VLAN is claimed by an upstream GEM interface, one
       entry for each port that matches the ingress port
       (``RTL9602C_CLS_RULE_UNI``) and assigns the bridge's default upstream
       flow, that of the last upstream GEM interface to join. When that
       interface leaves the bridge, the upstream GEM interface of the lowest
       flow still in it takes its place. The entries come from the top of the
       classifier, downward.
     - M
     - On a board provisioned by an OLT, the one entry read back at index 127,
       the top of the 128 entries an earlier driver used (the driver now takes
       255, the top of pattern 1), matching ingress port 0 and assigning the
       upstream flow (debugfs ``rtl9602c/cf``); upstream frames from the host
       arrive at the OLT. A second upstream GEM interface (Port-ID 300, made by
       hand on the same T-CONT) that joined took the entry, and 200 untagged
       frames from the host were counted in its upstream octets; when it left
       the bridge, and again when it was deleted while in it, the entry named
       the first interface's flow again and the frames arrived at the OLT.
       Before the driver chose another interface, no entry remained once it had
       left, and every upstream frame from the host was lost.
   * - Once VLANs are claimed, one entry for each port and claimed VLAN instead,
       matching the ingress port and the C-tag's VID and assigning that VLAN's
       flow, with no entry for a VLAN that no upstream GEM interface claims. A
       priority of a VLAN travels up one GEM interface only: upstream GEM
       interfaces may share a VLAN with disjoint priorities
       (``IFLA_GEM_PBITS``), and the VLAN then gets one entry per claimed
       priority, which also matches the C-tag's priority (see the *PON* page,
       "Bridges and VLANs"); a GEM interface that would take a priority
       another holds is refused (``-EBUSY``, ``VLAN <v> priority <p> already
       travels up another GEM interface``), and a GEM interface that leaves
       the bridge gives up the VLANs that it claimed.
     - M
     - The OLT's GEM interface (Port-ID 256) claiming VLAN 3001 and the second
       interface of the row above 3002: entries 127 and 126 matched the two
       VIDs and assigned the two flows. Of 200 frames of each VLAN from the
       host, those of 3001 were counted in the first port's upstream octets
       (``pon_tx_octets``, 14,600 more) and arrived at the OLT, those of 3002
       in the second's (14,600 more) and none in the first's; untagged frames
       and frames of VLAN 3003 were counted in neither. The OLT forwarded no
       frame of Port-ID 300, which it had not provisioned. A claim of 3001 for
       the second interface was refused with the message; when it left the
       bridge, its entry went and the frames of 3002 got no stream. The
       division of a VLAN by priority is measured on the *PON* page.
   * - A subscriber port outside the GEM bridge (a routed port) gets one entry
       that matches its ingress port and assigns the flow that its routed
       interface is sent in, when an address on a GEM interface makes one (see
       the *l3-nat* page).
     - T
     - A routing test asserts the entry; PPPoE over a routed port passes
       traffic.

The layer-2 side of routing
===========================

A next hop of the L3 engine does not hold the destination address. It stores an
index into the address table, and the destination address and the egress port of
a routed frame come from the entry at that index. The driver therefore places
address-table entries for the engine. It also files this device's own
addresses behind the CPU port, with the router bit while the engine is up. The
engine itself is on the *l3-nat* page.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - DSA installs no FDB entry for a standalone port's own address, because
       host unicast filtering needs per-port VLAN filtering (see "Flooding"); so
       nothing puts this device's own addresses behind the CPU port unless the
       driver does.
     - M
     - On a freshly booted board the table held two learned entries and none of
       this device's own.
   * - ``RTL9602C_FDB_W1_ROUTER_MAC`` is set only once the L3 engine is up, and
       ``port_fdb_add`` stamps later CPU-port entries itself while it is, in
       case the bit sends a frame to the engine: set while the engine is down
       it would then take this device's own traffic, management included, into
       a block that is not running.
     - I
     - Stamping with the engine's master enable clear while watching management
       traffic would settle it; the ordering costs nothing if it is false.
   * - ``rtl9602c_l2_nexthop_place()`` places the entry that a next hop points
       at: a next hop stores an index into this table, and the destination
       address and egress port come from the entry at that index. The far end of
       a routed port is never learned (the port is standalone) and a static
       entry cannot be added there from user space, so the driver places it. A
       next-hop entry of the key already behind the port is used as it is,
       since another next hop may name its slot; otherwise the driver deletes
       every entry of the key, each by its own words, places a static entry
       with ``RTL9602C_FDB_W2_NEXTHOP``, returns ``RTL9602C_TBLACC_STAT_INDEX``
       as the slot and logs it at debug level (a line that begins ``<L2 next
       hop>``, which ``echo 'file rtl9602c-l2.c +p' >
       /sys/kernel/debug/dynamic_debug/control`` shows in a kernel that has
       dynamic debug); HIT clear returns ``-ENOSPC``. A multicast address is
       refused with ``-EINVAL``.
     - T
     - A routing test sees translated frames on the wire at the far side of the
       OLT; PPPoE over a routed port passes traffic.
   * - The L3 code records which entry each next-hop row names, and
       ``rtl9602c_l2_nexthop_drop()`` deletes an entry once no row names it:
       when the neighbour goes, when a PPPoE WAN is released or fails to come
       up, when a row is given another address, and at teardown.
     - M
     - The ``fdb`` view showed the next-hop entry of a neighbour on ``sfp``,
       and none once ``ip neigh del`` had deleted the neighbour, while the
       entry of the management neighbour, still in use, stayed.
   * - The entry is filed under the VID that the routed frame leaves in, the
       destination VLAN that the engine resolves from the next hop's interface,
       and not under the port's PVID: with independent learning a lookup at one
       VID cannot see an entry filed under another, and with the port's
       unknown-unicast flooding off the miss is silent. ``RTL9602C_L2_VID_PVID``
       (``0xffff``) asks for the port's PVID instead; the VID used goes back to
       the caller.
     - T
     - A routing test sees translated frames on the wire at the far side of the
       OLT.
   * - The first next hop on a port records the port (``l34_nh_ports``) and
       opens it in every other user port's isolation field;
       ``rtl9602c_bridge_refresh()`` applies the opening again while the engine
       is up, so that a later membership change cannot undo it (see "Port
       isolation").
     - M
     - After a cold boot, port 0's field read ``0x01c``, able to reach port 2,
       with nothing written by hand; routed frames then reached the far side of
       the OLT.

``rtl9602c_l2_l3lookup_cpu()``, which the driver calls once the L3 engine's
master enable reads back set (see the *l3-nat* page), in order:

1. Turn learning off on the CPU port (see "Learning and the learning limit").
   Purpose: the switch learns this device's own address from the CPU's own
   transmissions and keeps rewriting a learned entry without the router bit
   beside the static one; its age changed between two reads a second apart.
   Every address behind the CPU port is this device's own, so nothing is lost.
   (M)
2. Delete every learned entry behind the CPU port by its own words, logging
   ``<L2> dropped a learned duplicate of <address> on the CPU port``. Purpose:
   switching learning off stops the next duplicate, not the one already there
   (see "Placing, changing and deleting through the hash"). (M)
3. For each user port that has an interface, ``rtl9602c_l2_own_place()``:
   delete the other entries of its own address at its PVID, each by its own
   words, then place one static entry behind the CPU port with
   ``RTL9602C_FDB_W1_ROUTER_MAC``, logging at debug level ``<L2> <address>
   vlan <v> is <port>'s own address: it gets routed, not trapped to the CPU``
   or, with HIT clear, an error that frames for it are flooded. Purpose: DSA
   installs no entry for a standalone port's address, whose frames would
   otherwise be flooded as unknown unicast. (M)
4. Walk the hashed slots: delete every CPU-port entry whose router bit
   disagrees, by its own words, and place it once with the bit; log ``<L2> <n>
   CPU-port entries now routed by the L3 engine``. Purpose: entries that existed
   before the engine came up. (M)

DSA runs setup before it creates the interfaces, so step 3 finds no port at
boot. ``port_enable`` therefore runs ``rtl9602c_l2_own_place()`` for a user
port when the port is opened, and the netdevice notifier runs it when a user
port's address changes, after ``rtl9602c_l2_own_drop()`` has deleted the old
address unless another user port carries it. In router mode, booted straight
into it, the ``fdb`` view showed the stick's own address behind the CPU port
with the router bit. The routed flow does not depend on the entry: a kernel
that placed none carried it all the same (see "Entry layout").

After a fresh boot the table held one static entry for this device's address
behind the CPU port, carrying the router bit, and no other. That the bit is what
admits the frame to the engine is an assumption (see "Entry layout").

The read-only views
===================

The instruments that the checks above name. All four views only read, and they
exist with ``CONFIG_DEBUG_FS`` in ``/sys/kernel/debug/rtl9602c/``, mode 0400.

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - View
     - Shows
   * - ``fdb``
     - Every occupied slot of the hashed part of the address table, by index:
       the address, the VID, ``ivl`` or ``svl`` (``RTL9602C_FDB_W1_IVL``),
       ``static`` or ``learned``, then for a unicast entry ``port`` and ``age``
       with ``router`` and ``nexthop`` when those bits are set, and for a group
       ``members`` and ``ext``, its member sets raw. The last line counts the
       slots in use.
   * - ``l2learn``
     - The first line shows the three flood masks raw (``unknown-uc``,
       ``unknown-mc``, ``bc``), the second ``src ext perm``:
       ``RTL9602C_ISOLATE_EXT_HAIRPIN`` raw. Then a line for each port with the
       columns ``limit`` (the learning limit), ``count`` (the addresses
       learned), ``over`` (the over-limit action raw, ``=fw`` for 0 and ``=?``
       otherwise), ``moved`` (the action on an address held for another port
       raw, ``=move`` for 0, ``=drop`` for 1 and ``=?`` otherwise; see
       "Addresses that move"), ``flood(uc mc bc)`` (the port's three flood
       bits) and a note:
       ``learning OFF, overflows per frame`` when the limit is 0, and ``(cpu)``
       for the CPU port.
   * - ``svlan``
     - The two TPIDs and the second one's enable (``tpid``); the policy fields
       (``policy``: ``filtering``, ``keep-dei``, ``pcp``, ``untag``,
       ``pcp-sel``); for each port the S-VID, the uplink bit and the
       destination-VID bit (``svid``, ``uplink``, ``dmac-vid``); and the valid
       translation entries (``s2c``: ``svid``, ``port``, ``cvid``, ``pcp``).
   * - ``rma``
     - The global word raw (``cfg``), then for each reserved address its raw
       word (``raw``), the action (``oper``), the storm exemption (``storm``),
       and the keep-tag, VLAN-bypass and isolation-bypass bits (``keep``,
       ``vleak``, ``isoleak``).

Also used above: ``bridge fdb show`` (``port_fdb_dump``), ``bridge mdb``, the
kernel log lines of setup (the table self-test, the emptying of the address
table and the ``<L2>`` lines of the previous section), and ``ethtool -S`` for
``rx_forward_discards`` (``RTL9602C_MIB_TX_FWD_DISCARDS``, see the *main* page).

Known limitations
=================

- The switch has one address table, and its entries are keyed by address and VID
  alone: the database that DSA passes to ``port_fdb_add``, ``port_fdb_del``,
  ``port_mdb_add`` and ``port_mdb_del`` is not used. Of the user ports only
  ``sfp`` can join a bridge (see "Bridge membership"), so two bridges share the
  table only when the GEM interfaces are bridged apart from ``sfp`` and both
  bridges use a VID: an address then has one entry for both.
- The address table holds at most 1088 addresses, and fewer in practice: an
  address whose bucket of four slots is full takes one of the CAM's 64
  entries, and once those are taken too it is refused with ``-ENOSPC`` while
  other buckets are empty.
- DSA writes the address entries that the bridge asks for from a work queue,
  so ``bridge fdb add`` has returned success by the time an entry is refused,
  for instance for a full bucket; the refusal is the kernel log line alone.
- While the L3 engine is up, which is whenever the driver is bound, a frame
  that is not IP, to a host address behind the CPU port other than this
  device's own, is flooded to every port, the PON port included: its entry
  carries ``RTL9602C_FDB_W1_ROUTER_MAC`` (see "Entry layout").
- A multicast router behind the SFP port is not looked for, as DSA passes no
  user port's router state: while a router sits behind a GEM port, the PON
  port drops the unregistered multicast that the bridge would send to it (see
  "Multicast snooping").
- Every group in the table has the PON port, so what a host on the SFP side
  sends to a group goes towards the OLT even while the bridge sees no router
  there.
- A router of the second protocol behind a GEM port that is a router port
  already takes effect at the next group change (see "Multicast snooping").
- A rule matches the EtherType of an untagged frame only (see "VLAN tag
  operations"); one behind a tag is refused until it is known which EtherType
  the classifier compares on a tagged frame. A rule names one EtherType, so
  PPPoE, discovery and session, takes two rules.
- The tc rules on an S-tag have 32 classifier entries to themselves, eight
  for a rule written per priority, and the other rules 224, shared with the
  stream entries (see the *acl-qos* page, "The classifier"): a rule on an
  S-tag finds no room once the 32 are taken, however many the others leave
  free.

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
   * - A multicast group reaches the CPU only with the first extension bit as
       well as the CPU port's member bit ("Entry layout").
     - The bridge itself joining a group, then that group sent from the host
       with and without the extension bit.
   * - The reserved-address registers other than ``_00`` control the addresses
       their suffixes name; the exemptions from storm control rely on it
       ("Reserved multicast addresses").
     - An exemption test as for ``_00``, for each of ``_01``, ``_02``, ``_03``
       and ``_0E``.
   * - A flush of a port's learned entries removes all of them; once in four
       runs, one left four learned hashed entries in place and took only the
       one in the CAM ("Structure").
     - The same steps repeated until it happens again, with the flush's
       registers read around it.
   * - A rule that keeps any priority, into a VLAN whose priorities GEM
       interfaces divide, sends each priority up its own GEM port through its
       eight entries, and loses a priority no GEM interface takes ("VLAN tag
       operations").
     - Two GEM interfaces dividing a VLAN's priorities and a translation into
       it that keeps the priority; frames of each priority from the host,
       counted in each port's upstream octets; the eight entries in ``cf``,
       and back to one when one interface takes every priority.
   * - The rules on a C-tag or an untagged frame, in entries of the
       classifier's pattern 1, match and act as the measured ones did in
       pattern 0, and a rule on an S-tag in pattern 0 takes precedence over
       its port's stream entry at the top of pattern 1 ("VLAN tag operations";
       the *acl-qos* page, "The classifier").
     - The frame tests of "VLAN tag operations" and of "Bridging the GEM
       ports" again, with ``cf`` showing the rules at 32 upwards and the
       stream entries at 255 downwards.
   * - An entry of pattern 1 compares the EtherType of an untagged frame in
       rule word 1, bits 15:0 ("VLAN tag operations").
     - The PPPoE and IPv4 frames of the example of "VLAN tag operations"; and
       downstream, untagged frames of ``0x88b5`` and ``0x88b6`` under a push
       of VID 777 on ``0x88b5``, only the first arriving tagged.
   * - ``RTL9602C_CLS_ACT_CPRI_SET`` with the priority 0 writes priority 0, so
       a pop and a push give a translated tag priority 0 ("VLAN tag
       operations").
     - Frames of C-VID 410 and priority 5 under a pop and a push of 1410 with
       priority 0, read at the far side of the OLT.
   * - One classifier entry acts on both tags, its C-tag plane and its S-tag
       plane, and leaves the S-tag in front of the C-tag ("VLAN tag
       operations").
     - Untagged frames under a push of C-VID 410 and of S-VID 200, read at the
       far side of the OLT, and the two pops of the way back.

Questions the code does not depend on
-------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether the status CAM flag is set when a hashed placement lands in the
       CAM ("The indirect table engine").
     - A hashed placement that lands in the CAM.
   * - What ``RTL9602C_FDB_W1_ROUTER_MAC`` does, since it is not what admits a
       frame to the L3 engine ("Entry layout").
     - A frame to this device's address in a VLAN in independent learning, with
       and without the bit, followed in the frame capture.
   * - Whether the flush modes other than that of a port's learned entries
       reach the CAM ("Structure").
     - Each of them over an entry in the CAM that it should remove.
   * - Whether a port flush reaches a multicast entry ("Emptying the table at
       setup").
     - A group whose only member is one port, a port flush of that port, then a
       hashed lookup of the group.
   * - What the three other over-limit actions do, and whether the
       two-bits-per-port packing of ``RTL9602C_FDB_LEARN_LIMIT_ACT`` is right
       ("Learning and the learning limit").
     - A limit of 0 with the action at 1 and frames from unknown sources.
   * - Whether the learning-limit latch is one bit for the whole switch
       ("Learning and the learning limit").
     - A deliberate limit exceeded on each of two ports in turn, the latch read
       after each.
   * - What ``RTL9602C_ISOLATE_EXT_HAIRPIN`` does ("Port isolation").
     - Nothing on this board reaches an extension port, so nothing settles it
       here.
   * - What the other values of the classifier's priority fields do: copies of
       the outer or the inner tag's priority, the internal priority, a
       priority from the DSCP ("VLAN tag operations").
     - Each value under frames of two priorities in two tags, both ways.
   * - Which EtherType an entry of pattern 1 compares on a frame with a C-tag:
       the one behind the tag, or the tag's TPID ("VLAN tag operations"; the
       driver refuses an EtherType behind a tag).
     - In a test build that takes such a rule, a translation of C-VID 410 on
       ``vlan_ethtype 0x8864``, with frames of VLAN 410 of ``0x8864`` and of
       IPv4: only the first translated says the EtherType behind the tag.
   * - What the S-VLAN block's other fields do, and whether a bridge VLAN takes
       the S-tag of its VID off frames leaving by its untagged ports ("The
       S-VLAN block").
     - Each field set in turn under frames with and without S-tags, both ways;
       a bridge VLAN untagged on ``sfp`` and frames of that S-VID downstream.
   * - What the switch's per-port IGMP and MLD control does ("Multicast
       snooping").
     - Its fields set to each value with IGMP and MLD messages from each port,
       followed in the frame capture.
   * - What the four actions and the three flags of the reserved-address
       controls, and the global word, mean ("Reserved multicast addresses").
     - Frames to 01:80:c2:00:00:00 with the action at each value, counted at the
       CPU and at the host.
   * - Whether the router bit should be set only once the L3 engine is up ("The
       layer-2 side of routing").
     - Stamping it with the engine's master enable clear while watching
       management traffic.
