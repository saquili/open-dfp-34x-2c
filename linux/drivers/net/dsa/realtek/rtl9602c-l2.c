// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9602C switch: bridging. The address table (unicast entries, multicast
 * groups, flush, ageing and learning), flooding, port isolation,
 * spanning-tree state, VLAN membership and filtering, the frame-length limit
 * and port mirroring, as DSA switch operations; and the address-table
 * entries the L3 engine depends on: this device's own addresses behind the
 * CPU port, and the entries the engine's next hops point at.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/unaligned.h>
#include <net/ipv6.h>
#include <linux/dcbnl.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/if_bridge.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "rtl9602c.h"

/*
 * The address table is used through its hash only: the engine hashes the
 * address and VID of the key in @e and goes to the slot of the bucket the
 * key belongs in, and a read returns the entry found there. A write by index
 * would be accepted and discarded. Setup turns the CAM after the hashed slots
 * on (RTL9602C_FDB_CFG_CAM_OFF clear), and a hashed write whose bucket is
 * full then lands there, where a hashed lookup or delete with the entry's own
 * words reaches it; every walk reads it too, up to RTL9602C_FDB_ENTRIES. Every
 * hashed slot reads back with RTL9602C_FDB_W2_VALID set, occupied or not: only
 * the hit flag tells an entry from an empty slot.
 *
 * All six tables share the table engine's registers, hence table_lock.
 */
static int rtl9602c_l2_lookup(struct rtl9602c_priv *p, u32 *e,
			      struct rtl9602c_lookup *out)
{
	int err;

	mutex_lock(&p->table_lock);
	err = rtl9602c_table_op(p, RTL9602C_TBLACC_CMD_TABLE_FDB,
				RTL9602C_TBLACC_CMD_MODE_HASH, 0, e,
				RTL9602C_FDB_ENTRY_WORDS, false, out);
	mutex_unlock(&p->table_lock);
	return err;
}

/*
 * Put @e in the slot its key hashes to, or in the CAM when that bucket is full,
 * or, with RTL9602C_FDB_W2_VALID clear, delete the entry the key names. A
 * write the table does not take (hit clear) is taken to have found the bucket
 * and the CAM full, which is not verified.
 *
 * A write of a key already in the table does not always replace it and can
 * add another copy. An entry is therefore changed by deleting its key
 * RTL9602C_FDB_HASH_WAYS times, once per slot of a bucket, then adding it
 * once.
 */
static int rtl9602c_l2_place(struct rtl9602c_priv *p, u32 *e,
			     struct rtl9602c_lookup *out)
{
	int err;

	mutex_lock(&p->table_lock);
	err = rtl9602c_table_op(p, RTL9602C_TBLACC_CMD_TABLE_FDB,
				RTL9602C_TBLACC_CMD_MODE_HASH, 0, e,
				RTL9602C_FDB_ENTRY_WORDS, true, out);
	mutex_unlock(&p->table_lock);
	return err;
}

/*
 * A unicast entry for @addr behind @port, filed under @vid. With @ivl it is
 * keyed by VID (independent learning), otherwise by address alone (shared
 * learning); frames match it only when that agrees with the learning bit of
 * the VLAN's own entry (rtl9602c_l2_ivl()). A static entry is left alone by
 * the dynamic flush and does not age out, although its age counts down like
 * any other, and source learning does not move it (measured with an entry
 * keyed by address).
 */
static void rtl9602c_l2_pack(u32 *e, const unsigned char *addr, u16 vid,
			     int port, bool is_static, bool ivl)
{
	e[0] = addr[2] << 24 | addr[3] << 16 | addr[4] << 8 | addr[5];
	e[1] = FIELD_PREP(RTL9602C_FDB_W1_MAC_HI, addr[0] << 8 | addr[1]) |
	       FIELD_PREP(RTL9602C_FDB_W1_VID, vid);
	if (ivl)
		e[1] |= RTL9602C_FDB_W1_IVL;
	if (is_static)
		e[1] |= RTL9602C_FDB_W1_STATIC;
	e[2] = FIELD_PREP(RTL9602C_FDB_W2_PORT, port) |
	       FIELD_PREP(RTL9602C_FDB_W2_AGE, RTL9602C_FDB_W2_AGE_MAX) |
	       RTL9602C_FDB_W2_VALID;
}

static u16 rtl9602c_port_pvid(struct rtl9602c_priv *p, int port)
{
	u32 v = rtl9602c_r(p, RTL9602C_VLAN_PVID(port));

	return (v >> RTL9602C_VLAN_PVID_SHIFT(port)) & RTL9602C_VLAN_PVID_MASK;
}

/*
 * How the switch keys the entries of @vid: by VID if the VLAN's own entry is
 * in independent learning, which rtl9602c_vlan_set_member() sets when it adds
 * a member and never clears, and by address alone otherwise, as for every
 * VLAN that no port_vlan_add wrote. VID 0 is one of those, the PVID of the
 * ports of a VLAN-unaware bridge whose default PVID is 0; its entry is not
 * read, as the driver never touches it.
 *
 * Return: 0 with @ivl set, or a negative error code from the table access.
 */
static int rtl9602c_l2_ivl(struct rtl9602c_priv *p, u16 vid, bool *ivl)
{
	u32 entry;
	int err;

	*ivl = false;
	if (!vid)
		return 0;

	err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, vid, &entry,
				  1, NULL);
	if (err)
		return err;
	*ivl = entry & RTL9602C_VLAN_ENTRY_IVL;
	return 0;
}

static void rtl9602c_l2_unpack(const u32 *e, unsigned char *addr, u16 *vid,
			       int *port)
{
	u32 hi = FIELD_GET(RTL9602C_FDB_W1_MAC_HI, e[1]);

	addr[0] = hi >> 8;
	addr[1] = hi;
	addr[2] = e[0] >> 24;
	addr[3] = e[0] >> 16;
	addr[4] = e[0] >> 8;
	addr[5] = e[0];
	*vid = FIELD_GET(RTL9602C_FDB_W1_VID, e[1]);
	*port = FIELD_GET(RTL9602C_FDB_W2_PORT, e[2]);
}

/*
 * Delete the entry @e, as read from its slot: written back through the hash
 * with RTL9602C_FDB_W2_VALID clear, it deletes exactly that entry. A delete
 * built from the address and VID alone does not reach every entry: one with
 * port 0 left an entry behind the CPU port in place (measured), so the hashed
 * delete is taken to match the entry's other fields as well.
 */
static int rtl9602c_l2_entry_del(struct rtl9602c_priv *p, const u32 *e)
{
	u32 k[RTL9602C_FDB_ENTRY_WORDS];
	struct rtl9602c_lookup out;

	memcpy(k, e, sizeof(k));
	k[2] &= ~RTL9602C_FDB_W2_VALID;
	return rtl9602c_l2_place(p, k, &out);
}

/* Which of a key's entries rtl9602c_l2_purge() deletes. */
enum rtl9602c_l2_which {
	RTL9602C_L2_ALL,
	RTL9602C_L2_NOT_NEXTHOP,
	RTL9602C_L2_NEXTHOP,
};

/*
 * Delete the unicast entries of @addr at @vid that @which picks, learned or
 * static, behind @port or, when @port is negative, behind any port; with
 * @shared, also every entry of @addr keyed by address alone, whatever VLAN
 * its VID field names, since those all share one key. The hashed slots are
 * read by index, all 1024 in well under 10 ms, and each entry found is
 * deleted by its own words: neither its port, its router bit nor the way it
 * is keyed follows from the address and VID. Called under fdb_lock.
 *
 * Return: the number of entries deleted, or a negative error code from the
 * table access.
 */
static int rtl9602c_l2_purge(struct rtl9602c_priv *p, const unsigned char *addr,
			     u16 vid, bool shared, int port,
			     enum rtl9602c_l2_which which)
{
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	unsigned char a[ETH_ALEN];
	int eport, err, n = 0;
	unsigned int i;
	bool hit, nh;
	u16 evid;

	for (i = 0; i < RTL9602C_FDB_ENTRIES; i++) {
		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_FDB, i, e,
					  RTL9602C_FDB_ENTRY_WORDS, &hit);
		if (err)
			return err;
		if (!hit)
			continue;

		rtl9602c_l2_unpack(e, a, &evid, &eport);
		if (!ether_addr_equal(a, addr) || (port >= 0 && eport != port))
			continue;
		if (evid != vid && !(shared && !(e[1] & RTL9602C_FDB_W1_IVL)))
			continue;
		nh = e[2] & RTL9602C_FDB_W2_NEXTHOP;
		if ((which == RTL9602C_L2_NOT_NEXTHOP && nh) ||
		    (which == RTL9602C_L2_NEXTHOP && !nh))
			continue;

		err = rtl9602c_l2_entry_del(p, e);
		if (err)
			return err;
		n++;
	}
	return n;
}

/*
 * The slot of the next-hop entry of @addr at @vid, with its port in @port.
 * Called under fdb_lock.
 *
 * Return: the slot, -ENOENT when there is none, or another negative error code
 * from the table access.
 */
static int rtl9602c_l2_nexthop_find(struct rtl9602c_priv *p,
				    const unsigned char *addr, u16 vid,
				    int *port)
{
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	unsigned char a[ETH_ALEN];
	unsigned int i;
	int eport, err;
	bool hit;
	u16 evid;

	for (i = 0; i < RTL9602C_FDB_ENTRIES; i++) {
		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_FDB, i, e,
					  RTL9602C_FDB_ENTRY_WORDS, &hit);
		if (err)
			return err;
		if (!hit || !(e[2] & RTL9602C_FDB_W2_NEXTHOP))
			continue;

		rtl9602c_l2_unpack(e, a, &evid, &eport);
		if (evid == vid && ether_addr_equal(a, addr)) {
			*port = eport;
			return i;
		}
	}
	return -ENOENT;
}

/*
 * Whether @addr at @vid is the own address of one of the user ports, as
 * rtl9602c_l2_own_place() files it.
 */
static bool rtl9602c_l2_is_own(struct rtl9602c_priv *p,
			       const unsigned char *addr, u16 vid)
{
	struct dsa_port *dp;

	dsa_switch_for_each_user_port(dp, &p->ds)
		if (dp->user && ether_addr_equal(dp->user->dev_addr, addr) &&
		    rtl9602c_port_pvid(p, dp->index) == vid)
			return true;
	return false;
}

/* The index of @addr in @vid among @n entries of @h, or -1. */
static int rtl9602c_host_find(const struct rtl9602c_host *h, unsigned int n,
			      const u8 *addr, u16 vid)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (h[i].vid == vid && ether_addr_equal(h[i].addr, addr))
			return i;
	return -1;
}

/*
 * The static entry of rtl9602c_port_fdb_add(); @router lets an entry behind
 * the CPU port take the router bit while the L3 engine is up.
 */
static int rtl9602c_l2_static_add(struct rtl9602c_priv *p, int port,
				  const unsigned char *addr, u16 vid,
				  bool router)
{
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	struct rtl9602c_lookup out;
	int err, nh_port;
	bool ivl;

	if (is_multicast_ether_addr(addr))
		return -EINVAL;

	if (!vid)
		vid = rtl9602c_port_pvid(p, port);

	mutex_lock(&p->fdb_lock);

	err = rtl9602c_l2_ivl(p, vid, &ivl);
	if (err)
		goto out;

	/*
	 * A routed next hop's entry is static already and stays as placed,
	 * since a next hop names it by its slot: behind this port it is the
	 * entry asked for, behind another it is in the way.
	 */
	err = rtl9602c_l2_nexthop_find(p, addr, vid, &nh_port);
	if (err >= 0) {
		if (nh_port == port) {
			err = 0;
		} else {
			pr_warn("%pM vlan %u: a routed next hop holds it behind port %d\n",
				addr, vid, nh_port);
			err = -EBUSY;
		}
		goto out;
	}
	if (err != -ENOENT)
		goto out;

	/*
	 * Every other entry of the key goes first, learned or static, so that
	 * one entry is left: a write of a key already in the table does not
	 * always replace it.
	 */
	err = rtl9602c_l2_purge(p, addr, vid, !ivl, -1, RTL9602C_L2_ALL);
	if (err < 0)
		goto out;

	rtl9602c_l2_pack(e, addr, vid, port, true, ivl);

	/*
	 * An address behind the CPU port is one of this device's own. While
	 * the L3 engine is up it gets the router bit, like the entries that
	 * rtl9602c_l2_own_place() placed. What the bit does is not known: the
	 * engine takes the frames for these addresses without it (measured).
	 */
	if (router && p->l34_up && port == RTL9602C_PORT_CPU)
		e[1] |= RTL9602C_FDB_W1_ROUTER_MAC;

	err = rtl9602c_l2_place(p, e, &out);
	if (!err && !out.hit) {
		pr_err("%pM vlan %u: its hash bucket and the CAM are full\n", addr, vid);
		err = -ENOSPC;
	}
out:
	mutex_unlock(&p->fdb_lock);
	return err;
}

/**
 * rtl9602c_port_fdb_add() - add a static unicast entry
 * @ds: the switch
 * @port: the port the address lives behind
 * @addr: the address; a group goes through rtl9602c_port_mdb_add()
 * @vid: the VLAN the entry is keyed by, 0 standing for the port's PVID
 * @db: the database DSA files the entry in; unused, as entries are keyed by
 *	address and VID alone
 *
 * The switch gives an untagged frame its ingress port's PVID before the
 * address lookup, so a VID 0, which DSA passes for a VLAN-unaware bridge,
 * stands for the port's PVID, and the entry is keyed as that VLAN learns:
 * by VID in independent learning, by address alone in shared learning,
 * which is where a PVID of 0 leaves it.
 *
 * Return: 0 on success, -EINVAL for a multicast address, -EBUSY when a routed
 * next hop holds the address behind another port, -ENOSPC when the address's
 * bucket and the CAM are full, or a negative error code from the table access.
 */
int rtl9602c_port_fdb_add(struct dsa_switch *ds, int port,
			  const unsigned char *addr, u16 vid,
			  struct dsa_db db)
{
	return rtl9602c_l2_static_add(ds->priv, port, addr, vid, true);
}

/**
 * rtl9602c_port_fdb_del() - delete a unicast entry
 * @ds: the switch
 * @port: the port the entry was added on
 * @addr: the address
 * @vid: the VLAN the entry is keyed by, 0 standing for the port's PVID
 * @db: unused, as in rtl9602c_port_fdb_add()
 *
 * The entries of the key behind @port are deleted, each by its own words, but
 * not a routed next hop's, which its next hop still names. Nor is this
 * device's own address deleted from behind the CPU port while a user port
 * carries it: DSA removes a bridge's host addresses when the bridge goes,
 * while the port keeps its address.
 *
 * Return: 0 on success or a negative error code from the table access.
 */
int rtl9602c_port_fdb_del(struct dsa_switch *ds, int port,
			  const unsigned char *addr, u16 vid,
			  struct dsa_db db)
{
	struct rtl9602c_priv *p = ds->priv;
	int err;

	if (!vid)
		vid = rtl9602c_port_pvid(p, port);

	if (port == RTL9602C_PORT_CPU && rtl9602c_l2_is_own(p, addr, vid))
		return 0;

	mutex_lock(&p->fdb_lock);
	err = rtl9602c_l2_purge(p, addr, vid, false, port,
				RTL9602C_L2_NOT_NEXTHOP);
	mutex_unlock(&p->fdb_lock);
	return err < 0 ? err : 0;
}

/*
 * Add @port to a group's member set or take it out. The CPU port is a member
 * only together with the first extension port, as in the isolation and VLAN
 * masks. The entry stays valid while any member remains and is deleted with
 * the last.
 *
 * The PON port is a member of every group in the table, as the side the
 * multicast router of a PON sits on: a host's IGMPv1, IGMPv2 or MLDv1 report
 * goes to the group's own address, and with the group in the table but
 * without the PON port among its members, the switch would send the report
 * back where it came from and never towards the router (measured with
 * IGMPv2). DSA never bridges the PON port, so the bit is the driver's alone,
 * and it never keeps an entry alive by itself.
 */
static void rtl9602c_mdb_members(u32 *e, int port, bool member)
{
	u32 mbr = FIELD_GET(RTL9602C_FDB_W2_MC_PORTS, e[2]);
	u32 ext = FIELD_GET(RTL9602C_FDB_W2_MC_EXT_PORTS, e[2]);

	if (member)
		mbr |= BIT(port);
	else
		mbr &= ~BIT(port);

	if (port == RTL9602C_PORT_CPU) {
		if (member)
			ext |= RTL9602C_FDB_W2_MC_EXT_PORTS_CPU;
		else
			ext &= ~RTL9602C_FDB_W2_MC_EXT_PORTS_CPU;
	}

	mbr &= ~BIT(RTL9602C_PORT_PON);
	if (mbr | ext)
		mbr |= BIT(RTL9602C_PORT_PON);

	e[2] &= ~(RTL9602C_FDB_W2_MC_PORTS | RTL9602C_FDB_W2_MC_EXT_PORTS);
	e[2] |= FIELD_PREP(RTL9602C_FDB_W2_MC_PORTS, mbr) |
		FIELD_PREP(RTL9602C_FDB_W2_MC_EXT_PORTS, ext);

	if (mbr | ext)
		e[2] |= RTL9602C_FDB_W2_VALID;
	else
		e[2] &= ~RTL9602C_FDB_W2_VALID;
}

/*
 * A group is one entry that holds its member sets where a unicast entry
 * holds its port and age, so word 2 is built from zero. The lookup, the
 * change and the write run under fdb_lock as one operation: two ports
 * joining at once would otherwise each write back the set they read before
 * the other.
 */
static int rtl9602c_mdb_edit(struct dsa_switch *ds, int port,
			     const unsigned char *addr, u16 vid, bool member)
{
	struct rtl9602c_priv *p = ds->priv;
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	struct rtl9602c_lookup out;
	bool ivl;
	int err;

	if (!is_multicast_ether_addr(addr))
		return -EINVAL;

	if (!vid)
		vid = rtl9602c_port_pvid(p, port);

	mutex_lock(&p->fdb_lock);

	err = rtl9602c_l2_ivl(p, vid, &ivl);
	if (err)
		goto out;

	rtl9602c_l2_pack(e, addr, vid, 0, true, ivl);
	e[2] = 0;

	err = rtl9602c_l2_lookup(p, e, &out);
	if (err)
		goto out;

	/*
	 * Not in the table: a removal has nothing to do, and an addition
	 * starts again from the key, as the data read on a miss are no entry.
	 */
	if (!out.hit) {
		if (!member)
			goto out;
		rtl9602c_l2_pack(e, addr, vid, 0, true, ivl);
		e[2] = 0;
	}

	rtl9602c_mdb_members(e, port, member);

	err = rtl9602c_l2_place(p, e, &out);
	if (!err && !out.hit) {
		pr_err("%pM vlan %u: nowhere left to put it\n", addr, vid);
		err = -ENOSPC;
	}
out:
	mutex_unlock(&p->fdb_lock);
	return err;
}

/**
 * rtl9602c_port_mdb_add() - add a port to a multicast group
 * @ds: the switch
 * @port: the port
 * @mdb: the group's address and VLAN, VID 0 standing for the port's PVID
 * @db: unused, as in rtl9602c_port_fdb_add()
 *
 * Return: 0 on success, -EINVAL for a unicast address, -ENOSPC when the
 * group's bucket and the CAM are full, or a negative error code from the table
 * access.
 */
int rtl9602c_port_mdb_add(struct dsa_switch *ds, int port,
			  const struct switchdev_obj_port_mdb *mdb,
			  struct dsa_db db)
{
	int err = rtl9602c_mdb_edit(ds, port, mdb->addr, mdb->vid, true);

	rtl9602c_mcast_refresh(ds->priv);
	return err;
}

/**
 * rtl9602c_port_mdb_del() - remove a port from a multicast group
 * @ds: the switch
 * @port: the port
 * @mdb: the group's address and VLAN, VID 0 standing for the port's PVID
 * @db: unused, as in rtl9602c_port_fdb_add()
 *
 * The group's entry is deleted with its last member.
 *
 * Return: 0 on success, also when the group is not in the table, -EINVAL for
 * a unicast address, or another negative error code.
 */
int rtl9602c_port_mdb_del(struct dsa_switch *ds, int port,
			  const struct switchdev_obj_port_mdb *mdb,
			  struct dsa_db db)
{
	int err = rtl9602c_mdb_edit(ds, port, mdb->addr, mdb->vid, false);

	rtl9602c_mcast_refresh(ds->priv);
	return err;
}

/**
 * rtl9602c_l2_clear() - empty the address table
 * @p: driver private data
 *
 * The switch keeps its address table across a restart of the SoC, the CAM
 * included. At setup the CAM is turned on, so that the hash reaches it, every
 * slot and CAM entry is read by index, and every entry found is written back
 * through the hash with %RTL9602C_FDB_W2_VALID clear, which deletes exactly
 * that entry, unicast or multicast. A port flush is taken not to reach a
 * multicast entry, whose member set lies where the port would be; that is
 * not verified. Stops at the first failed table access.
 */
void rtl9602c_l2_clear(struct rtl9602c_priv *p)
{
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	struct rtl9602c_lookup out;
	unsigned int i, gone = 0;
	bool hit;

	rtl9602c_rmw(p, RTL9602C_FDB_CFG, RTL9602C_FDB_CFG_CAM_OFF, 0);

	for (i = 0; i < RTL9602C_FDB_ENTRIES; i++) {
		if (rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_FDB, i, e,
					RTL9602C_FDB_ENTRY_WORDS, &hit))
			return;
		if (!hit)
			continue;

		e[2] &= ~RTL9602C_FDB_W2_VALID;
		if (rtl9602c_l2_place(p, e, &out))
			return;
		gone++;
	}

	if (gone)
		pr_info("cleared %u address(es) left in the table by the previous kernel\n",
			gone);
}

static void rtl9602c_port_learning(struct rtl9602c_priv *p, int port, bool on);

/**
 * rtl9602c_l2_own_place() - place a user port's own address behind the CPU port
 * @p: driver private data
 * @port: the user port
 *
 * The address is filed at the port's PVID, with %RTL9602C_FDB_W1_ROUTER_MAC,
 * once the other entries of that key are gone. DSA installs no host address
 * for a standalone port, as it filters host addresses only on a switch with
 * per-port VLAN filtering, so without this entry the frames for the address
 * would be flooded as unknown unicast. The L3 engine does not need it: it
 * takes the frames for this device's addresses with no entry for them in the
 * table (measured). Nothing is placed while the engine is down or before the
 * port has an interface.
 *
 * Return: 0 on success, -ENOSPC when the address's bucket and the CAM are full,
 * or a negative error code from the table access.
 */
int rtl9602c_l2_own_place(struct rtl9602c_priv *p, int port)
{
	struct dsa_port *dp = dsa_to_port(&p->ds, port);
	u32 k[RTL9602C_FDB_ENTRY_WORDS];
	struct rtl9602c_lookup out;
	bool ivl;
	int err;
	u16 vid;

	if (!p->l34_up || !dp || !dsa_port_is_user(dp) || !dp->user)
		return 0;
	vid = rtl9602c_port_pvid(p, port);

	mutex_lock(&p->fdb_lock);
	err = rtl9602c_l2_ivl(p, vid, &ivl);
	if (err)
		goto out;
	err = rtl9602c_l2_purge(p, dp->user->dev_addr, vid, !ivl, -1,
				RTL9602C_L2_NOT_NEXTHOP);
	if (err < 0)
		goto out;

	rtl9602c_l2_pack(k, dp->user->dev_addr, vid, RTL9602C_PORT_CPU, true,
			 ivl);
	k[1] |= RTL9602C_FDB_W1_ROUTER_MAC;
	err = rtl9602c_l2_place(p, k, &out);
	if (!err && !out.hit) {
		pr_err("%pM vlan %u: its hash bucket and the CAM are full, so frames for this device's own address are flooded\n",
		       dp->user->dev_addr, vid);
		err = -ENOSPC;
	}
	if (!err)
		pr_debug("<L2> %pM vlan %u is %s's own address: it gets routed, not trapped to the CPU\n",
			 dp->user->dev_addr, vid, netdev_name(dp->user));
out:
	mutex_unlock(&p->fdb_lock);
	return err;
}

/**
 * rtl9602c_l2_own_drop() - delete a user port's own address before it changes
 * @p: driver private data
 * @port: the user port, still with its old address
 *
 * The entry goes unless another user port carries the same address at the
 * same PVID.
 *
 * Return: 0 on success or a negative error code from the table access.
 */
int rtl9602c_l2_own_drop(struct rtl9602c_priv *p, int port)
{
	struct dsa_port *dp = dsa_to_port(&p->ds, port), *other;
	int err;
	u16 vid;

	if (!dp || !dsa_port_is_user(dp) || !dp->user)
		return 0;
	vid = rtl9602c_port_pvid(p, port);

	dsa_switch_for_each_user_port(other, &p->ds)
		if (other != dp && other->user &&
		    ether_addr_equal(other->user->dev_addr, dp->user->dev_addr) &&
		    rtl9602c_port_pvid(p, other->index) == vid)
			return 0;

	mutex_lock(&p->fdb_lock);
	err = rtl9602c_l2_purge(p, dp->user->dev_addr, vid, false,
				RTL9602C_PORT_CPU, RTL9602C_L2_NOT_NEXTHOP);
	mutex_unlock(&p->fdb_lock);
	return err < 0 ? err : 0;
}

/**
 * rtl9602c_l2_l3lookup_cpu() - route or trap this device's own addresses
 * @p: driver private data
 * @on: true to hand frames for them to the L3 engine, false to trap them to
 *	the CPU port again
 *
 * Every address behind the CPU port is one of this device's own. With @on,
 * learning is turned off on the CPU port and the learned entries behind it
 * are deleted: the switch learns these addresses from the CPU's own
 * transmissions, as entries without the router bit that are rewritten
 * continuously and win the lookup. Each user port that has an interface gets
 * its own address placed by rtl9602c_l2_own_place(); a port that gets its
 * interface later gets it when it is opened. Last, with @on or without, every
 * CPU-port entry whose router bit disagrees with @on is replaced by one that
 * agrees, but for the GEM interfaces' entries (rtl9602c_gem_hosts_sync()).
 * Each entry is deleted by its own words.
 *
 * What the router bit does is not known. It is not what admits a frame to
 * the L3 engine, which takes the frames for this device's addresses with no
 * entry for them in the table (measured). It is set only while the engine
 * runs, in case it sends frames to the engine, which would take this
 * device's own traffic, management included, into a block that is not
 * running.
 *
 * Return: the number of entries placed or changed, or a negative error code
 * from the table access.
 */
int rtl9602c_l2_l3lookup_cpu(struct rtl9602c_priv *p, bool on)
{
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	struct rtl9602c_lookup out;
	unsigned int i, done = 0;
	struct dsa_port *dp;
	int eport, err;
	bool hit;
	u16 vid;

	/* Stop learning the duplicates; static entries replace them below. */
	if (on)
		rtl9602c_port_learning(p, RTL9602C_PORT_CPU, false);

	/* Learning off stops the next duplicate, not those already learned. */
	mutex_lock(&p->fdb_lock);
	for (i = 0; on && i < RTL9602C_FDB_ENTRIES; i++) {
		unsigned char mac[ETH_ALEN];

		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_FDB, i, e,
					  RTL9602C_FDB_ENTRY_WORDS, &hit);
		if (err)
			goto out;
		rtl9602c_l2_unpack(e, mac, &vid, &eport);
		if (!hit || eport != RTL9602C_PORT_CPU ||
		    is_multicast_ether_addr(mac) ||
		    (e[1] & RTL9602C_FDB_W1_STATIC))
			continue;

		if (rtl9602c_l2_entry_del(p, e) == 0)
			pr_info("<L2> dropped a learned duplicate of %pM on the CPU port\n",
				mac);
	}
	mutex_unlock(&p->fdb_lock);

	/* Place each user port's own address, which DSA does not install. */
	if (on) {
		dsa_switch_for_each_user_port(dp, &p->ds)
			if (dp->user && !rtl9602c_l2_own_place(p, dp->index))
				done++;
	}

	/* Then every CPU-port entry whose router bit disagrees with @on. */
	mutex_lock(&p->fdb_lock);
	for (i = 0; i < RTL9602C_FDB_ENTRIES; i++) {
		unsigned char mac[ETH_ALEN];
		u32 k[RTL9602C_FDB_ENTRY_WORDS];

		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_FDB, i, e,
					  RTL9602C_FDB_ENTRY_WORDS, &hit);
		if (err)
			goto out;
		rtl9602c_l2_unpack(e, mac, &vid, &eport);
		if (!hit || eport != RTL9602C_PORT_CPU ||
		    is_multicast_ether_addr(mac) ||
		    !!(e[1] & RTL9602C_FDB_W1_ROUTER_MAC) == on ||
		    rtl9602c_host_find(p->gem_hosts, p->n_gem_hosts, mac,
				       vid) >= 0)
			continue;

		/* Delete it by its own words, then place it once as asked. */
		err = rtl9602c_l2_entry_del(p, e);
		if (err)
			goto out;
		rtl9602c_l2_pack(k, mac, vid, RTL9602C_PORT_CPU,
				 e[1] & RTL9602C_FDB_W1_STATIC,
				 e[1] & RTL9602C_FDB_W1_IVL);
		if (on)
			k[1] |= RTL9602C_FDB_W1_ROUTER_MAC;
		err = rtl9602c_l2_place(p, k, &out);
		if (err)
			goto out;
		done++;
	}
	err = done;

	if (done)
		pr_info("<L2> %u CPU-port entr%s now %s\n", done,
			done == 1 ? "y" : "ies",
			on ? "routed by the L3 engine" : "trapped as before");
out:
	mutex_unlock(&p->fdb_lock);
	return err;
}

/* Let frames arriving on @port leave by the ports in @add as well. */
static void rtl9602c_isolation_permit(struct rtl9602c_priv *p, int port,
				      u32 add)
{
	int shift = RTL9602C_ISOLATE_EGRESS_SHIFT(port);

	rtl9602c_rmw(p, RTL9602C_ISOLATE_EGRESS(port), 0,
		     (add & RTL9602C_ISOLATE_EGRESS_MASK) << shift);
}

static const char *rtl9602c_l2_portname(struct rtl9602c_priv *p, int port)
{
	struct dsa_port *dp = dsa_to_port(&p->ds, port);

	return dp && dp->user ? netdev_name(dp->user) : "?";
}

/**
 * rtl9602c_l2_nexthop_place() - place the address-table entry of a next hop
 * @p: driver private data
 * @mac: the next hop's address, unicast
 * @port: the port the next hop lives behind
 * @want_vid: the VLAN to file the entry under, or %RTL9602C_L2_VID_PVID for
 *	the port's PVID; set to the VID used
 * @slot: set to the entry's index in the table
 *
 * A next hop names its destination by an index into the address table: the
 * routed frame's destination address and egress port come from the entry
 * there. Nothing else puts that entry in the table, as a routed port is
 * standalone and learns nothing, so the key is emptied and one static entry
 * is placed with %RTL9602C_FDB_W2_NEXTHOP, without which the switch discards
 * the routed frame at its ingress port. The entry is keyed by VID whatever
 * the VLAN's learning mode, so that two next hops of one address in two
 * VLANs get two entries; the engine names it by its slot, not by a lookup.
 * A next-hop entry already there for the same port is used as it is, since
 * another next hop may name its slot.
 * rtl9602c_l2_nexthop_drop() deletes the entry when no next hop names it.
 *
 * The caller files the entry under the VLAN the routed frame leaves in, on
 * the assumption, not verified, that the switch looks the destination up at
 * that VID and would not see an entry filed under another.
 *
 * Routed frames obey port isolation, so the first next hop behind a port
 * opens that port to every other user port; rtl9602c_bridge_refresh() keeps
 * the opening while the engine is up.
 *
 * Return: 0 on success, -EINVAL for a multicast address, -ENOSPC when the
 * address's bucket and the CAM are full, or a negative error code from the
 * table access.
 */
int rtl9602c_l2_nexthop_place(struct rtl9602c_priv *p, const unsigned char *mac,
			      int port, u16 *want_vid, unsigned int *slot)
{
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	struct rtl9602c_lookup out;
	int err, nh_port;
	u16 vid;

	if (is_multicast_ether_addr(mac))
		return -EINVAL;

	vid = *want_vid == RTL9602C_L2_VID_PVID ? rtl9602c_port_pvid(p, port)
						: *want_vid;
	*want_vid = vid;

	mutex_lock(&p->fdb_lock);
	err = rtl9602c_l2_nexthop_find(p, mac, vid, &nh_port);
	if (err >= 0 && nh_port == port) {
		*slot = err;
		err = 0;
		goto placed;
	}
	if (err < 0 && err != -ENOENT)
		goto placed;

	/* Empty the key, then place it once. */
	err = rtl9602c_l2_purge(p, mac, vid, false, -1, RTL9602C_L2_ALL);
	if (err < 0)
		goto placed;

	rtl9602c_l2_pack(e, mac, vid, port, true, true);
	e[2] |= RTL9602C_FDB_W2_NEXTHOP;
	err = rtl9602c_l2_place(p, e, &out);
	if (!err && !out.hit) {
		pr_err("%pM vlan %u: its hash bucket and the CAM are full\n", mac, vid);
		err = -ENOSPC;
	}
	if (!err)
		*slot = out.addr;
placed:
	mutex_unlock(&p->fdb_lock);
	if (err)
		return err;

	if (!(p->l34_nh_ports & BIT(port))) {
		struct dsa_port *dp;

		p->l34_nh_ports |= BIT(port);
		dsa_switch_for_each_user_port(dp, &p->ds)
			if (dp->index != port)
				rtl9602c_isolation_permit(p, dp->index,
							  BIT(port));
	}

	pr_debug("<L2 next hop> %pM %s (port %d), vlan %u; forwarding-table -> %u\n",
		 mac, rtl9602c_l2_portname(p, port), port, vid, *slot);
	return 0;
}

/**
 * rtl9602c_l2_nexthop_drop() - delete the address-table entry of a next hop
 * @p: driver private data
 * @mac: the address rtl9602c_l2_nexthop_place() placed
 * @vid: the VLAN it filed the entry under
 *
 * The caller drops an entry once no next hop names it.
 *
 * Return: 0 on success or a negative error code from the table access.
 */
int rtl9602c_l2_nexthop_drop(struct rtl9602c_priv *p, const unsigned char *mac,
			     u16 vid)
{
	int err;

	mutex_lock(&p->fdb_lock);
	err = rtl9602c_l2_purge(p, mac, vid, false, -1, RTL9602C_L2_NEXTHOP);
	mutex_unlock(&p->fdb_lock);
	if (err > 0)
		pr_debug("<L2 next hop> %pM vlan %u: entry deleted\n", mac, vid);
	return err < 0 ? err : 0;
}

/**
 * rtl9602c_port_fdb_dump() - report the address-table entries behind a port
 * @ds: the switch
 * @port: the port
 * @cb: called for each entry
 * @data: passed to @cb
 *
 * The hashed slots are read by index; only those the table engine reports a
 * hit for hold an entry. A group, whose member set lies where a unicast entry
 * keeps its port, is reported for each port of the set, as static.
 *
 * Return: 0, or the first error from the table access or from @cb.
 */
int rtl9602c_port_fdb_dump(struct dsa_switch *ds, int port,
			   dsa_fdb_dump_cb_t *cb, void *data)
{
	struct rtl9602c_priv *p = ds->priv;
	unsigned char addr[ETH_ALEN];
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	unsigned int i;
	int hport, err;
	bool hit;
	u16 vid;

	for (i = 0; i < RTL9602C_FDB_ENTRIES; i++) {
		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_FDB, i, e,
					  RTL9602C_FDB_ENTRY_WORDS, &hit);
		if (err)
			return err;
		if (!hit)
			continue;

		rtl9602c_l2_unpack(e, addr, &vid, &hport);
		if (is_multicast_ether_addr(addr) ?
		    !(FIELD_GET(RTL9602C_FDB_W2_MC_PORTS, e[2]) & BIT(port)) :
		    hport != port)
			continue;

		err = cb(addr, vid, !!(e[1] & RTL9602C_FDB_W1_STATIC),
			 data);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Flush the entries of @ports that @kinds selects. The ports go in a register
 * of their own; RTL9602C_FDB_FLUSH_CTRL_START reads back clear once the flush
 * is done.
 */
static void rtl9602c_l2_flush(struct rtl9602c_priv *p, u32 ports, u32 kinds)
{
	u32 v;

	rtl9602c_w(p, RTL9602C_FDB_FLUSH_PORTS, ports);
	rtl9602c_w(p, RTL9602C_FDB_FLUSH_CTRL,
		   FIELD_PREP(RTL9602C_FDB_FLUSH_CTRL_MODE,
			      RTL9602C_FDB_FLUSH_CTRL_MODE_PORT) |
		   kinds | RTL9602C_FDB_FLUSH_CTRL_START);

	if (read_poll_timeout(rtl9602c_r, v, !(v & RTL9602C_FDB_FLUSH_CTRL_START),
			      100, 100000, false, p, RTL9602C_FDB_FLUSH_CTRL))
		pr_err("the L2 flush never finished\n");
}

/**
 * rtl9602c_port_fast_age() - flush the entries a port has learned
 * @ds: the switch
 * @port: the port
 *
 * DSA calls this when a port stops forwarding or leaves a bridge, so that
 * the switch does not keep forwarding to a port whose topology has changed.
 * It is also called for the PON port when the last GEM interface leaves its
 * bridge, the port's learned entries being stale then. Static entries stay.
 */
void rtl9602c_port_fast_age(struct dsa_switch *ds, int port)
{
	struct rtl9602c_priv *p = ds->priv;

	mutex_lock(&p->fdb_lock);
	rtl9602c_l2_flush(p, BIT(port), RTL9602C_FDB_FLUSH_CTRL_DYNAMIC);
	mutex_unlock(&p->fdb_lock);
}

/**
 * rtl9602c_port_stp_state_set() - set a port's spanning-tree state
 * @ds: the switch
 * @port: the port
 * @state: the bridge's port state
 *
 * The switch has four states to the bridge's five: listening becomes
 * blocking, as in both a port discards frames and learns nothing
 * (IEEE 802.1D-1998, 8.4). Only spanning-tree instance 0 is written.
 * Forwarding is the reset state; the encodings of the other three states
 * are not verified.
 */
void rtl9602c_port_stp_state_set(struct dsa_switch *ds, int port,
				 u8 state)
{
	struct rtl9602c_priv *p = ds->priv;
	u32 val;

	switch (state) {
	case BR_STATE_DISABLED:
		val = RTL9602C_STP_PORT_STATE_DISABLED;
		break;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		val = RTL9602C_STP_PORT_STATE_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		val = RTL9602C_STP_PORT_STATE_LEARNING;
		break;
	default:
		val = RTL9602C_STP_PORT_STATE_FORWARDING;
		break;
	}

	rtl9602c_rmw(p, RTL9602C_STP_PORT_STATE(port), RTL9602C_STP_PORT_STATE_MSTI0, val);
}

static void rtl9602c_isolation_set(struct rtl9602c_priv *p, int port, u32 mask)
{
	int shift = RTL9602C_ISOLATE_EGRESS_SHIFT(port);

	rtl9602c_rmw(p, RTL9602C_ISOLATE_EGRESS(port),
		     RTL9602C_ISOLATE_EGRESS_MASK << shift,
		     (mask & RTL9602C_ISOLATE_EGRESS_MASK) << shift);
}

static void rtl9602c_port_learning(struct rtl9602c_priv *p, int port, bool on);
static void rtl9602c_port_flood_apply(struct rtl9602c_priv *p,
				      const struct dsa_port *dp);

/*
 * The bridge a port is in. The PON port is in the one its GEM interfaces
 * share, as DSA never bridges the PON port itself.
 */
static struct net_device *rtl9602c_port_bridge_dev(struct rtl9602c_priv *p,
						   const struct dsa_port *dp)
{
	if (dp->index == RTL9602C_PORT_PON)
		return p->pon_br;
	return dp->bridge ? dp->bridge->dev : NULL;
}

/**
 * rtl9602c_bridge_refresh() - apply bridge membership to the switch
 * @ds: the switch
 *
 * Bridging is expressed as port isolation. A user port may send to the CPU
 * port together with the first extension port, without which nothing
 * reaches the CPU; to every other user port in its bridge; and, while the L3
 * engine is up, to every other port a next hop lives behind. The CPU port's
 * own field keeps its reset value, open to every port. The flood masks
 * follow the same membership, the PON port learns only while GEM interfaces
 * are bridged, and the classifier entries that give bridged ports a GEM
 * stream are rebuilt.
 *
 * Called on every membership change of a switch port or a GEM interface,
 * with RTNL held, and once at setup, before any port has an interface.
 */
void rtl9602c_bridge_refresh(struct dsa_switch *ds)
{
	struct rtl9602c_priv *p = ds->priv;
	struct dsa_port *dp;

	dsa_switch_for_each_user_port(dp, ds) {
		struct net_device *br = rtl9602c_port_bridge_dev(p, dp);
		struct dsa_port *other;

		u32 mask = BIT(dsa_upstream_port(ds, dp->index)) |
			   RTL9602C_ISOLATE_EGRESS_EXT0;

		/* Routed frames obey isolation too. */
		if (p->l34_up)
			mask |= p->l34_nh_ports & ~BIT(dp->index);

		dsa_switch_for_each_user_port(other, ds) {
			if (other != dp && br &&
			    br == rtl9602c_port_bridge_dev(p, other))
				mask |= BIT(other->index);
		}
		rtl9602c_isolation_set(p, dp->index, mask);
		rtl9602c_port_flood_apply(p, dp);
	}

	/*
	 * DSA never bridges the PON port, so its learning follows the GEM
	 * interfaces: without it, every frame toward the network side would be
	 * flooded, a copy to the CPU included.
	 */
	rtl9602c_port_learning(p, RTL9602C_PORT_PON, !!p->pon_br);

	/*
	 * A frame the switch forwards to the PON port leaves only in the GEM
	 * stream a classifier entry gave it.
	 */
	rtl9602c_cf_sid_refresh(p);

	rtl9602c_mcast_refresh(p);
}

/**
 * rtl9602c_mcast_reset() - unregistered multicast as a reset leaves it
 * @p: the switch
 *
 * Every port floods the multicast and the unicast that no address-table entry
 * knows, which a warm reboot may have left otherwise, and the reserved groups
 * are flooded even where a port drops the rest. Unlike at reset, the IPv6
 * link-scope and solicited-node groups count among them: neighbour discovery
 * and router advertisements use them (RFC 4861), as link-local IPv4 protocols
 * use 224.0.0.0/24, and a bridge floods both without snooping them (RFC 4541
 * section 2.1.2).
 *
 * Context: setup, before rtl9602c_bridge_refresh().
 */
void rtl9602c_mcast_reset(struct rtl9602c_priv *p)
{
	rtl9602c_w(p, RTL9602C_UC_UNKNOWN, 0);
	rtl9602c_w(p, RTL9602C_MC_UNKNOWN_L2, 0);
	rtl9602c_w(p, RTL9602C_MC_UNKNOWN_IP4, 0);
	rtl9602c_w(p, RTL9602C_MC_UNKNOWN_IP6, 0);
	rtl9602c_w(p, RTL9602C_MC_UNKNOWN_CFG,
		   FIELD_PREP(RTL9602C_MC_UNKNOWN_CFG_RSV4,
			      RTL9602C_MC_UNKNOWN_CFG_RSV_FLOOD) |
		   FIELD_PREP(RTL9602C_MC_UNKNOWN_CFG_RSV6,
			      RTL9602C_MC_UNKNOWN_CFG_RSV_FLOOD) |
		   RTL9602C_MC_UNKNOWN_CFG_RSV6_LINK |
		   RTL9602C_MC_UNKNOWN_CFG_RSV6_SOLICITED);
	p->mc_drop = 0;
	p->uc_drop = false;
}

/**
 * rtl9602c_l2_moves_init() - keep the addresses of the other ports off the PON port
 * @p: the switch
 *
 * A frame whose source address the table holds for another port moves the
 * entry to the port it came in by, except on the PON port: a frame from the
 * PON side with an address of the SFP side or of the CPU is dropped, and
 * nothing is learned from it. An OLT may send the stick's upstream frames
 * back down the stick's GEM port, and the lab OLT sends every one
 * (measured): learned from them, the subscriber's addresses would move to the
 * PON port, and the switch would drop the downstream frames to them as frames
 * to the port they came in by.
 *
 * Context: setup.
 */
void rtl9602c_l2_moves_init(struct rtl9602c_priv *p)
{
	rtl9602c_rmw(p, RTL9602C_SA_MOVED,
		     RTL9602C_SA_MOVED_ACT(RTL9602C_PORT_PON),
		     FIELD_PREP(RTL9602C_SA_MOVED_ACT(RTL9602C_PORT_PON),
				RTL9602C_SA_MOVED_ACT_DROP));
}

/*
 * Whether the PON port drops the unicast that no entry knows: only while
 * nothing needs it. A bridge of the SFP port and the GEM interfaces floods it
 * to the SFP port, and a GEM interface that is promiscuous, has not said what
 * it takes, or has an address the table holds no entry for takes it at the
 * CPU. Otherwise it reaches only the CPU, which has no use for it, as with
 * the stick's own upstream frames that an OLT sent back down the stick's GEM
 * port (measured).
 *
 * Context: RTNL held.
 */
static void rtl9602c_pon_ucast_refresh(struct rtl9602c_priv *p)
{
	struct dsa_port *dp = dsa_to_port(&p->ds, RTL9602C_PORT_SFP);
	bool drop = !p->gem_hosts_full;
	unsigned int flow;

	if (dp && dp->user && p->pon_br &&
	    rtl9602c_port_bridge_dev(p, dp) == p->pon_br)
		drop = false;
	for_each_set_bit(flow, p->gem_flows, RTL9602C_GPON_NUM_FLOW)
		if (!p->gem_rx[flow].known || p->gem_rx[flow].promisc)
			drop = false;

	mutex_lock(&p->fdb_lock);
	if (drop != p->uc_drop) {
		rtl9602c_rmw(p, RTL9602C_UC_UNKNOWN,
			     RTL9602C_UC_UNKNOWN_ACT(RTL9602C_PORT_PON),
			     FIELD_PREP(RTL9602C_UC_UNKNOWN_ACT(RTL9602C_PORT_PON),
					drop ? RTL9602C_UC_UNKNOWN_ACT_DROP :
					RTL9602C_UC_UNKNOWN_ACT_FLOOD));
		pr_info("unknown unicast from the PON: %s\n",
			drop ? "dropped" : "flooded");
		p->uc_drop = drop;
	}
	mutex_unlock(&p->fdb_lock);
}

/**
 * rtl9602c_gem_hosts_sync() - take the GEM interfaces' addresses to the CPU
 * @p: the switch
 *
 * The switch learns no address of the stick's own, and a session's address
 * on a GEM interface is one. Each address of a GEM interface that has said
 * what it takes and is not promiscuous gets a static entry behind the CPU
 * port, in each VLAN it takes it in and in the PON port's PVID, for untagged
 * frames, so that the PON port may drop the unicast that no entry knows
 * (rtl9602c_pon_ucast_refresh()). The entries carry no router bit: with it,
 * the PPPoE discovery frames for a session's address never reached the CPU
 * (measured), and the L3 engine routes the frames for the stick's addresses
 * without it. A user port's own address is left to the entries that
 * rtl9602c_l2_own_place() keeps. The entries no interface takes any more go
 * first; an address the table has no room for keeps the PON port flooding.
 *
 * Context: RTNL held.
 */
/*
 * The VLAN that the switch looks @vid's frames up in: @vid itself if the VLAN
 * is in independent learning, and otherwise, keyed by address alone, the
 * VLAN that @pvid stands for. A frame for a session's address in a VLAN that
 * no port_vlan_add wrote reached the CPU through an entry at the PON port's
 * PVID, 0, and not through one at the VLAN's VID (measured). Sets @ivl.
 */
static u16 rtl9602c_gem_host_vid(struct rtl9602c_priv *p, u16 vid, u16 pvid,
				 bool *ivl)
{
	mutex_lock(&p->fdb_lock);
	if (rtl9602c_l2_ivl(p, vid, ivl))
		*ivl = false;
	mutex_unlock(&p->fdb_lock);
	return *ivl ? vid : pvid;
}

/*
 * Whether @addr is a user port's own address that its entry
 * (rtl9602c_l2_own_place()) takes to the CPU in this VLAN already: in one
 * keyed by address alone, whatever the VLAN, and in one keyed by VID, at the
 * port's PVID (rtl9602c_l2_is_own()).
 */
static bool rtl9602c_gem_host_own(struct rtl9602c_priv *p, const u8 *addr,
				  u16 vid, bool ivl)
{
	struct dsa_port *dp;

	if (ivl)
		return rtl9602c_l2_is_own(p, addr, vid);
	dsa_switch_for_each_user_port(dp, &p->ds)
		if (dp->user && ether_addr_equal(dp->user->dev_addr, addr))
			return true;
	return false;
}

/**
 * rtl9602c_gem_hosts_sync() - take the GEM interfaces' addresses to the CPU
 * @p: the switch
 *
 * The switch learns no address of the stick's own, and a session's address
 * on a GEM interface is one. Each address of a GEM interface that has said
 * what it takes and is not promiscuous gets a static entry behind the CPU
 * port, in each VLAN it takes it in as the switch looks that VLAN up
 * (rtl9602c_gem_host_vid()) and in the PON port's PVID, for untagged frames,
 * so that the PON port may drop the unicast that no entry knows
 * (rtl9602c_pon_ucast_refresh()). The entries carry no router bit: with it,
 * the PPPoE discovery frames for a session's address never reached the CPU
 * (measured), and the L3 engine routes the frames for the stick's addresses
 * without it. A user port's own address is left to its own entry. The
 * entries no interface takes any more go; the others are placed again each
 * time, as DSA deletes a bridge's host addresses, which may share their key,
 * when the bridge goes. An address the table has no room for keeps the PON
 * port flooding.
 *
 * Context: RTNL held.
 */
void rtl9602c_gem_hosts_sync(struct rtl9602c_priv *p)
{
	u16 pvid = rtl9602c_port_pvid(p, RTL9602C_PORT_PON);
	struct rtl9602c_host want[RTL9602C_GEM_HOSTS];
	unsigned int flow, i, j, n = 0;
	struct dsa_db db = {};
	bool full = false;

	for_each_set_bit(flow, p->gem_flows, RTL9602C_GPON_NUM_FLOW) {
		const struct rtl9602c_gem_rx *r = &p->gem_rx[flow];

		if (!r->known || r->promisc)
			continue;
		for (j = 0; j <= r->n_vids; j++) {
			bool ivl = false;
			u16 vid = j < r->n_vids ?
				  rtl9602c_gem_host_vid(p, r->vids[j], pvid,
							&ivl) :
				  pvid;

			for (i = 0; i < r->n_addrs; i++) {
				if (rtl9602c_gem_host_own(p, r->addrs[i], vid,
							  ivl) ||
				    rtl9602c_host_find(want, n, r->addrs[i],
						       vid) >= 0)
					continue;
				if (n == RTL9602C_GEM_HOSTS) {
					full = true;
					continue;
				}
				ether_addr_copy(want[n].addr, r->addrs[i]);
				want[n++].vid = vid;
			}
		}
	}

	for (i = 0; i < p->n_gem_hosts; i++)
		if (rtl9602c_host_find(want, n, p->gem_hosts[i].addr,
				       p->gem_hosts[i].vid) < 0)
			rtl9602c_port_fdb_del(&p->ds, RTL9602C_PORT_CPU,
					      p->gem_hosts[i].addr,
					      p->gem_hosts[i].vid, db);

	p->n_gem_hosts = 0;
	for (i = 0; i < n; i++) {
		if (rtl9602c_l2_static_add(p, RTL9602C_PORT_CPU, want[i].addr,
					   want[i].vid, false)) {
			full = true;
			continue;
		}
		p->gem_hosts[p->n_gem_hosts++] = want[i];
	}
	p->gem_hosts_full = full;

	rtl9602c_pon_ucast_refresh(p);
}

/**
 * rtl9602c_mcast_refresh() - follow the multicast routers the bridge sees
 * @p: the switch
 *
 * Once it has seen a querier, a bridge that snoops sends the multicast no host
 * asked for, unregistered multicast, only towards the ports a multicast router
 * sits behind; without a querier it floods it. In a PON the router sits behind
 * a GEM port, so a bridge would send such a frame from the PON side nowhere.
 * The PON port therefore drops unregistered IPv4 multicast while the bridge of
 * the SFP port and the GEM interfaces sees an IPv4 router behind a port other
 * than the SFP port, and IPv6 multicast by the IPv6 routers alike. The reserved
 * groups are flooded all the same (rtl9602c_mcast_reset()), as the bridge
 * floods the link-local groups, which it does not snoop. A router behind the
 * SFP port itself is not looked for: the bridge would send it the unregistered
 * multicast from the PON side, and the switch then drops it.
 *
 * While no bridge joins the SFP port to the GEM interfaces, as where the stick
 * routes or holds the sessions itself, unregistered multicast from the PON
 * side would reach only the CPU, which has no group to take it to, and the
 * PON port drops it too. An operator's OLT sent a television channel by the
 * multicast GEM port that no one had asked for, 7 Mb/s to the CPU (measured).
 *
 * Context: RTNL held; called when bridge membership changes, when a group
 * changes and when the router state of a GEM port does.
 */
void rtl9602c_mcast_refresh(struct rtl9602c_priv *p)
{
	struct dsa_port *dp = dsa_to_port(&p->ds, RTL9602C_PORT_SFP);
	u8 drop = 0;

	if (!dp || !dp->user || !p->pon_br ||
	    rtl9602c_port_bridge_dev(p, dp) != p->pon_br) {
		drop = BIT(0) | BIT(1);
	} else {
		if (br_multicast_has_router_adjacent(dp->user, ETH_P_IP))
			drop |= BIT(0);
		if (br_multicast_has_router_adjacent(dp->user, ETH_P_IPV6))
			drop |= BIT(1);
	}

	mutex_lock(&p->fdb_lock);
	if (drop != p->mc_drop) {
		rtl9602c_rmw(p, RTL9602C_MC_UNKNOWN_IP4,
			     RTL9602C_MC_UNKNOWN_ACT(RTL9602C_PORT_PON),
			     FIELD_PREP(RTL9602C_MC_UNKNOWN_ACT(RTL9602C_PORT_PON),
					drop & BIT(0) ?
					RTL9602C_MC_UNKNOWN_ACT_DROP :
					RTL9602C_MC_UNKNOWN_ACT_FLOOD));
		rtl9602c_rmw(p, RTL9602C_MC_UNKNOWN_IP6,
			     RTL9602C_MC_UNKNOWN_ACT(RTL9602C_PORT_PON),
			     FIELD_PREP(RTL9602C_MC_UNKNOWN_ACT(RTL9602C_PORT_PON),
					drop & BIT(1) ?
					RTL9602C_MC_UNKNOWN_ACT_DROP :
					RTL9602C_MC_UNKNOWN_ACT_FLOOD));
		pr_info("unregistered multicast from the PON: IPv4 %s, IPv6 %s\n",
			drop & BIT(0) ? "dropped" : "flooded",
			drop & BIT(1) ? "dropped" : "flooded");
		p->mc_drop = drop;
	}
	mutex_unlock(&p->fdb_lock);

	rtl9602c_pon_ucast_refresh(p);
}

/**
 * rtl9602c_port_bridge_join() - put a port in a bridge
 * @ds: the switch
 * @port: the port
 * @bridge: the bridge; unused, membership is read from DSA's ports
 * @tx_fwd_offload: left unset, as transmit forwarding offload is not offered
 * @extack: for the error message
 *
 * The PON port is refused: a frame the switch forwards to it must name a GEM
 * port, and the PON port names none. Its GEM interfaces are bridged instead,
 * and DSA bridges the PON port in software.
 *
 * Return: 0, or -EOPNOTSUPP for the PON port.
 */
int rtl9602c_port_bridge_join(struct dsa_switch *ds, int port,
			      struct dsa_bridge bridge,
			      bool *tx_fwd_offload,
			      struct netlink_ext_ack *extack)
{
	if (port == RTL9602C_PORT_PON) {
		NL_SET_ERR_MSG_MOD(extack,
				   "bridge the GEM interfaces, not pon itself");
		return -EOPNOTSUPP;
	}

	rtl9602c_bridge_refresh(ds);
	return 0;
}

/**
 * rtl9602c_port_bridge_leave() - take a port out of a bridge
 * @ds: the switch
 * @port: the port
 * @bridge: the bridge; unused, as in rtl9602c_port_bridge_join()
 */
void rtl9602c_port_bridge_leave(struct dsa_switch *ds, int port,
				struct dsa_bridge bridge)
{
	rtl9602c_bridge_refresh(ds);
}

/**
 * rtl9602c_vlan_set_member() - add a port to a VLAN or remove it
 * @p: driver private data
 * @vid: the VLAN, which is also its entry's index
 * @port: the port
 * @member: true to add the port, false to remove it
 * @untagged: when adding, whether the port sends the VLAN untagged
 *
 * The entry is read, changed and written back, so the other ports' bits
 * survive. The CPU port is a member only together with the first extension
 * port: with VLAN filtering on, frames in the VLAN do not reach the CPU
 * without it. Adding a port also sets independent learning, never cleared
 * again, so that every VLAN agrees with the address-table entries, which
 * are all keyed by VID; under shared learning a second write of one address
 * would add a second entry.
 *
 * Return: 0 on success or a negative error code from the table access.
 */
int rtl9602c_vlan_set_member(struct rtl9602c_priv *p, u16 vid,
			     int port, bool member, bool untagged)
{
	u32 entry, mbr, untag;
	int err;

	err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, vid, &entry, 1, NULL);
	if (err)
		return err;

	mbr = FIELD_GET(RTL9602C_VLAN_ENTRY_MEMBER_SET, entry);
	untag = FIELD_GET(RTL9602C_VLAN_ENTRY_UNTAGGED_SET, entry);

	if (member) {
		mbr |= BIT(port);
		if (untagged)
			untag |= BIT(port);
		else
			untag &= ~BIT(port);
	} else {
		mbr &= ~BIT(port);
		untag &= ~BIT(port);
	}

	entry &= ~(RTL9602C_VLAN_ENTRY_MEMBER_SET | RTL9602C_VLAN_ENTRY_UNTAGGED_SET);
	entry |= FIELD_PREP(RTL9602C_VLAN_ENTRY_MEMBER_SET, mbr) |
		 FIELD_PREP(RTL9602C_VLAN_ENTRY_UNTAGGED_SET, untag);

	if (port == RTL9602C_PORT_CPU) {
		if (member)
			entry |= RTL9602C_VLAN_ENTRY_EXT0_MEMBER;
		else
			entry &= ~RTL9602C_VLAN_ENTRY_EXT0_MEMBER;
	}

	if (member)
		entry |= RTL9602C_VLAN_ENTRY_IVL;

	return rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, vid, &entry, 1);
}

static void rtl9602c_port_set_pvid(struct rtl9602c_priv *p, int port, u16 vid)
{
	u32 mask = RTL9602C_VLAN_PVID_MASK << RTL9602C_VLAN_PVID_SHIFT(port);

	rtl9602c_rmw(p, RTL9602C_VLAN_PVID(port), mask,
		     (vid & RTL9602C_VLAN_PVID_MASK)
		     << RTL9602C_VLAN_PVID_SHIFT(port));
}

/**
 * rtl9602c_port_vlan_add() - add a port to a VLAN
 * @ds: the switch
 * @port: the port; DSA also adds the CPU port to every VLAN the host must see
 * @vlan: the VLAN and its flags; with %BRIDGE_VLAN_INFO_PVID it also becomes
 *	the port's PVID
 * @extack: for the error message
 *
 * VID 0 is not a VLAN of the table: the 8021q core adds it to the receive
 * filter of a port that filters VLANs, and a write of table entry 0 stops
 * the switch forwarding anything to the CPU port. It is taken as done. A
 * user port whose PVID is @vlan and which loses the PVID flag gets back the
 * PVID of reset, 0, which no VLAN entry holds; the CPU port's PVID changes
 * only with the flag, as DSA gives it.
 *
 * Return: 0 on success or a negative error code from the table access.
 */
int rtl9602c_port_vlan_add(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan,
			   struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = ds->priv;
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	int err;

	if (!vlan->vid)
		return 0;

	err = rtl9602c_vlan_set_member(p, vlan->vid, port, true, untagged);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "the VLAN table did not answer");
		return err;
	}

	if (vlan->flags & BRIDGE_VLAN_INFO_PVID)
		rtl9602c_port_set_pvid(p, port, vlan->vid);
	else if (dsa_is_user_port(ds, port) &&
		 rtl9602c_port_pvid(p, port) == vlan->vid)
		rtl9602c_port_set_pvid(p, port, 0);

	return 0;
}

/**
 * rtl9602c_port_vlan_del() - remove a port from a VLAN
 * @ds: the switch
 * @port: the port
 * @vlan: the VLAN
 *
 * VID 0 is taken as done, as for rtl9602c_port_vlan_add(). A user port whose
 * PVID was @vlan gets back the PVID of reset, 0, so that its untagged frames
 * are no longer given a VLAN it has left.
 *
 * Return: 0 on success or a negative error code from the table access.
 */
int rtl9602c_port_vlan_del(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan)
{
	struct rtl9602c_priv *p = ds->priv;
	int err;

	if (!vlan->vid)
		return 0;

	err = rtl9602c_vlan_set_member(p, vlan->vid, port, false, false);
	if (!err && dsa_is_user_port(ds, port) &&
	    rtl9602c_port_pvid(p, port) == vlan->vid)
		rtl9602c_port_set_pvid(p, port, 0);
	return err;
}

static void rtl9602c_port_flood(struct rtl9602c_priv *p, u32 mask_reg,
				int port, bool on)
{
	rtl9602c_rmw(p, mask_reg, BIT(port), on ? BIT(port) : 0);
}

/*
 * A flood mask bit lets a broadcast, a multicast frame no entry knows or a
 * unicast frame no entry knows leave by that port. A port gets the flags the
 * bridge asked for only while it shares a bridge with another switch port.
 * A standalone port needs no flooding, as what leaves by it comes from the
 * CPU with an explicit port or from the L3 engine as routed unicast; and
 * isolation cannot keep floods between two routed ports apart, since routed
 * frames need the opening. The CPU port's bits keep their reset value, set,
 * which is how unknown traffic reaches the host.
 */
static void rtl9602c_port_flood_apply(struct rtl9602c_priv *p,
				      const struct dsa_port *dp)
{
	struct net_device *br = rtl9602c_port_bridge_dev(p, dp);
	unsigned long req = p->flood_req[dp->index];
	struct dsa_port *other;
	bool peer = false;

	if (br)
		dsa_switch_for_each_user_port(other, &p->ds)
			if (other != dp &&
			    br == rtl9602c_port_bridge_dev(p, other))
				peer = true;

	rtl9602c_port_flood(p, RTL9602C_FLOOD_UNKNOWN_UCAST, dp->index,
			    peer && (req & BR_FLOOD));
	rtl9602c_port_flood(p, RTL9602C_FLOOD_UNKNOWN_MCAST, dp->index,
			    peer && (req & BR_MCAST_FLOOD));
	rtl9602c_port_flood(p, RTL9602C_FLOOD_BCAST, dp->index,
			    peer && (req & BR_BCAST_FLOOD));
}

/*
 * Learning is a limit, not a switch: the maximum stands for unlimited and 0
 * for learning off. The over-limit action is set to forward before every
 * limit change, which makes a limit of 0 mean "learn nothing" rather than
 * "forward nothing"; the per-port layout of that register is not verified,
 * every field holding 0 either way. The learning-limit alarm, armed only
 * while some port has a limit other than 0 or the maximum, is re-evaluated.
 */
static void rtl9602c_port_learning(struct rtl9602c_priv *p, int port, bool on)
{
	u32 limit = on ? RTL9602C_FDB_LEARN_LIMIT_MAX : 0;

	rtl9602c_rmw(p, RTL9602C_FDB_LEARN_LIMIT_ACT,
		     RTL9602C_FDB_LEARN_LIMIT_ACT_MASK <<
			RTL9602C_FDB_LEARN_LIMIT_ACT_SHIFT(port),
		     RTL9602C_FDB_LEARN_LIMIT_ACT_FORWARD <<
			RTL9602C_FDB_LEARN_LIMIT_ACT_SHIFT(port));

	rtl9602c_rmw(p, RTL9602C_FDB_LEARN_LIMIT(port),
		     RTL9602C_FDB_LEARN_MASK << RTL9602C_FDB_LEARN_SHIFT(port),
		     limit << RTL9602C_FDB_LEARN_SHIFT(port));

	rtl9602c_lrn_over_intr_sync(p);
}

/**
 * rtl9602c_port_pre_bridge_flags() - check bridge port flags
 * @ds: the switch
 * @port: the port
 * @flags: the flags to be changed
 * @extack: for the error message
 *
 * The switch offers learning and the three flood masks,
 * %RTL9602C_BRIDGE_FLAGS.
 *
 * Return: 0, or -EINVAL if @flags changes any other flag.
 */
int rtl9602c_port_pre_bridge_flags(struct dsa_switch *ds, int port,
				   struct switchdev_brport_flags flags,
				   struct netlink_ext_ack *extack)
{
	if (flags.mask & ~(unsigned long)RTL9602C_BRIDGE_FLAGS) {
		NL_SET_ERR_MSG_MOD(extack,
				   "this switch has learning and the three flood masks, and nothing else");
		return -EINVAL;
	}
	return 0;
}

/**
 * rtl9602c_port_bridge_flags() - apply bridge port flags
 * @ds: the switch
 * @port: the port
 * @flags: the flags to change and their new values
 * @extack: unused
 *
 * Learning takes effect at once. %BR_FLOOD, %BR_MCAST_FLOOD and
 * %BR_BCAST_FLOOD each drive their own mask, for unknown unicast, unknown
 * multicast and broadcast; they are recorded, and reach the hardware only
 * while the port has a bridge peer.
 *
 * Return: 0.
 */
int rtl9602c_port_bridge_flags(struct dsa_switch *ds, int port,
			       struct switchdev_brport_flags flags,
			       struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = ds->priv;

	if (flags.mask & BR_LEARNING)
		rtl9602c_port_learning(p, port, flags.val & BR_LEARNING);

	flags.mask &= BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD;
	p->flood_req[port] = (p->flood_req[port] & ~flags.mask) |
			     (flags.val & flags.mask);
	rtl9602c_port_flood_apply(p, dsa_to_port(ds, port));

	return 0;
}

/**
 * rtl9602c_set_ageing_time() - set how long a learned entry lives unrefreshed
 * @ds: the switch
 * @msecs: the ageing time in milliseconds
 *
 * One value for the whole switch, in units of
 * %RTL9602C_FDB_CFG_AGEING_TIME_UNIT_MS. The bridge keeps @msecs within the
 * range declared at probe, which the field can hold.
 *
 * Return: 0.
 */
int rtl9602c_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	struct rtl9602c_priv *p = ds->priv;
	u32 units = msecs / RTL9602C_FDB_CFG_AGEING_TIME_UNIT_MS;

	rtl9602c_rmw(p, RTL9602C_FDB_CFG, RTL9602C_FDB_CFG_AGEING_TIME,
		     FIELD_PREP(RTL9602C_FDB_CFG_AGEING_TIME, units));
	return 0;
}

/**
 * rtl9602c_port_vlan_filtering() - turn VLAN awareness on or off
 * @ds: the switch
 * @port: unused: the setting is one bit for the whole switch, hence
 *	vlan_filtering_is_global
 * @on: true to forward by VLAN membership
 * @extack: unused
 *
 * With the bit set the switch forwards every frame by VLAN table membership;
 * it is not a per-port ingress check.
 *
 * Return: 0.
 */
int rtl9602c_port_vlan_filtering(struct dsa_switch *ds, int port,
				 bool on,
				 struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = ds->priv;

	rtl9602c_rmw(p, RTL9602C_VLAN_CFG, RTL9602C_VLAN_CFG_AWARE,
		     on ? RTL9602C_VLAN_CFG_AWARE : 0);
	return 0;
}

/**
 * rtl9602c_port_max_mtu() - the largest MTU a port can take
 * @ds: the switch
 * @port: the user port
 *
 * A change of a user port's MTU changes the conduit's as well, and fails
 * beyond the conduit's limit, which is far below the switch's: the CPU
 * port's DMA engine takes an MTU of 1508, room for PPPoE around a 1500-byte
 * packet. The CPU tag travels in the DMA descriptor and adds nothing to a
 * frame, so a port takes what the conduit takes.
 *
 * Return: the lower of the conduit's largest MTU and the longest frame the
 * switch accepts, less %RTL9602C_FRAME_OVERHEAD.
 */
int rtl9602c_port_max_mtu(struct dsa_switch *ds, int port)
{
	const struct net_device *conduit = dsa_to_port(ds, port)->cpu_dp->conduit;

	return min_t(int, conduit->max_mtu,
		     RTL9602C_FRAMELEN_LIMIT0_BYTES_MAX - RTL9602C_FRAME_OVERHEAD);
}

/**
 * rtl9602c_port_change_mtu() - set a port's MTU
 * @ds: the switch
 * @port: the port
 * @new_mtu: the MTU
 *
 * The frame-length check counts the Ethernet header, the FCS, the VLAN tags
 * and, measured on this board, the CPU tag as well, although that travels in
 * the DMA descriptor: the check is made where the CPU port feeds the switch.
 * %RTL9602C_FRAME_OVERHEAD allows for all of them with two VLAN tags, a C-tag
 * and an S-tag. Two length registers serve four ports:
 * %RTL9602C_FRAMELEN_LIMIT0 takes the largest MTU any port has, and
 * %RTL9602C_FRAMELEN_LIMIT1 the largest below it, and each port is pointed
 * at the lower limit that still takes its MTU. With three different MTUs a
 * port with the smallest gets the middle one's limit. The writes run so that
 * no port is held below its MTU at any moment: %RTL9602C_FRAMELEN_LIMIT0,
 * which takes every MTU, then the ports that stay on it or move to it, then
 * %RTL9602C_FRAMELEN_LIMIT1, which only ports that fit it still select, and
 * last the ports that move to it. That the 10/100 Mb/s selector works like
 * the gigabit one is not verified.
 *
 * Return: 0.
 */
int rtl9602c_port_change_mtu(struct dsa_switch *ds, int port,
			     int new_mtu)
{
	const u32 sel = RTL9602C_FRAMELEN_SEL_1000M | RTL9602C_FRAMELEN_SEL_10_100M;
	struct rtl9602c_priv *p = ds->priv;
	unsigned int i, largest = 0, second = 0;
	u32 low = 0;

	p->mtu[port] = new_mtu;
	for (i = 0; i < RTL9602C_NUM_PORTS; i++)
		largest = max(largest, p->mtu[i]);
	for (i = 0; i < RTL9602C_NUM_PORTS; i++)
		if (p->mtu[i] < largest)
			second = max(second, p->mtu[i]);
	for (i = 0; second && i < RTL9602C_NUM_PORTS; i++)
		if (p->mtu[i] <= second)
			low |= BIT(i);

	rtl9602c_rmw(p, RTL9602C_FRAMELEN_LIMIT0, RTL9602C_FRAMELEN_LIMIT0_BYTES,
		     FIELD_PREP(RTL9602C_FRAMELEN_LIMIT0_BYTES,
				largest + RTL9602C_FRAME_OVERHEAD));
	for (i = 0; i < RTL9602C_NUM_PORTS; i++)
		if (!(low & BIT(i)))
			rtl9602c_rmw(p, RTL9602C_FRAMELEN_SEL(i), sel, 0);

	if (!low)
		return 0;
	rtl9602c_rmw(p, RTL9602C_FRAMELEN_LIMIT1, RTL9602C_FRAMELEN_LIMIT0_BYTES,
		     FIELD_PREP(RTL9602C_FRAMELEN_LIMIT0_BYTES,
				second + RTL9602C_FRAME_OVERHEAD));
	for (i = 0; i < RTL9602C_NUM_PORTS; i++)
		if (low & BIT(i))
			rtl9602c_rmw(p, RTL9602C_FRAMELEN_SEL(i), sel, sel);
	return 0;
}

/*
 * The mirror word from the driver's record: the sources are the ports of
 * either direction, each direction's bit is set while it has a port, and the
 * monitor field keeps its value, which is inert with no source.
 */
static void rtl9602c_mirror_write(struct rtl9602c_priv *p)
{
	u32 v = rtl9602c_r(p, RTL9602C_MIRROR_CTRL);

	v &= ~(RTL9602C_MIRROR_CTRL_SRC_PORTS | RTL9602C_MIRROR_CTRL_INGRESS |
	       RTL9602C_MIRROR_CTRL_EGRESS | RTL9602C_MIRROR_CTRL_DEST_PORT);
	v |= FIELD_PREP(RTL9602C_MIRROR_CTRL_SRC_PORTS,
			p->mirror_rx | p->mirror_tx);
	v |= FIELD_PREP(RTL9602C_MIRROR_CTRL_DEST_PORT, p->mirror_to);
	if (p->mirror_rx)
		v |= RTL9602C_MIRROR_CTRL_INGRESS;
	if (p->mirror_tx)
		v |= RTL9602C_MIRROR_CTRL_EGRESS;
	rtl9602c_w(p, RTL9602C_MIRROR_CTRL, v);
}

/**
 * rtl9602c_port_mirror_add() - mirror a port's traffic
 * @ds: the switch
 * @port: the port whose traffic is copied
 * @mirror: the rule; its monitor port sends the copies
 * @ingress: true to copy what the port receives, false what it sends
 * @extack: for the error message
 *
 * The switch has one monitor port and one set of source ports, with an
 * enable bit for each direction: a port in the set is mirrored in every
 * direction that is on. A rule is taken while that stays exact, that is
 * while one direction alone is in use or both mirror the same ports. A rule
 * naming another monitor port is refused rather than moving the first, and
 * source ports accumulate.
 *
 * Return: 0, -EBUSY if another monitor port is in use, or -EOPNOTSUPP if the
 * two directions would mirror different ports.
 */
int rtl9602c_port_mirror_add(struct dsa_switch *ds, int port,
			     struct dsa_mall_mirror_tc_entry *mirror,
			     bool ingress,
			     struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = ds->priv;
	u8 rx = p->mirror_rx, tx = p->mirror_tx;

	if ((rx | tx) && p->mirror_to != mirror->to_local_port) {
		NL_SET_ERR_MSG_MOD(extack,
				   "this switch has one monitor port and it is already in use");
		return -EBUSY;
	}

	if (ingress)
		rx |= BIT(port);
	else
		tx |= BIT(port);
	if (rx && tx && rx != tx) {
		NL_SET_ERR_MSG_MOD(extack,
				   "the switch mirrors one set of ports in both directions, so ingress and egress cannot mirror different ports");
		return -EOPNOTSUPP;
	}

	p->mirror_to = mirror->to_local_port;
	p->mirror_rx = rx;
	p->mirror_tx = tx;
	rtl9602c_mirror_write(p);

	mirror->ingress = ingress;
	return 0;
}

/**
 * rtl9602c_port_mirror_del() - stop mirroring a port
 * @ds: the switch
 * @port: the port
 * @mirror: the rule, for its direction
 *
 * A direction is switched off with its last port, and the port leaves the
 * source set when neither direction mirrors it any more.
 */
void rtl9602c_port_mirror_del(struct dsa_switch *ds, int port,
			      struct dsa_mall_mirror_tc_entry *mirror)
{
	struct rtl9602c_priv *p = ds->priv;

	if (mirror->ingress)
		p->mirror_rx &= ~BIT(port);
	else
		p->mirror_tx &= ~BIT(port);
	rtl9602c_mirror_write(p);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * fdb: every occupied slot of the hashed part of the address table, by index:
 * the address and VID, how the entry is keyed (ivl or svl) and whether it is
 * static, then for a unicast entry its port, its age and the router and
 * next-hop bits, for a group its member set and its extension members. The
 * slots are read one at a time, so a table that changes during the read shows
 * partly old, partly new.
 */
static int rtl9602c_fdb_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 e[RTL9602C_FDB_ENTRY_WORDS];
	unsigned char addr[ETH_ALEN];
	unsigned int i, used = 0;
	int port;
	bool hit;
	u16 vid;

	seq_puts(s, "slot  address            vid   key  kind     where\n");
	for (i = 0; i < RTL9602C_FDB_ENTRIES; i++) {
		if (rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_FDB, i, e,
					RTL9602C_FDB_ENTRY_WORDS, &hit))
			return -EIO;
		if (!hit)
			continue;

		rtl9602c_l2_unpack(e, addr, &vid, &port);
		seq_printf(s, "%4u  %pM  %4u  %s  %-7s  ", i, addr, vid,
			   e[1] & RTL9602C_FDB_W1_IVL ? "ivl" : "svl",
			   e[1] & RTL9602C_FDB_W1_STATIC ? "static" : "learned");
		if (is_multicast_ether_addr(addr))
			seq_printf(s, "members %lx ext %lx\n",
				   FIELD_GET(RTL9602C_FDB_W2_MC_PORTS, e[2]),
				   FIELD_GET(RTL9602C_FDB_W2_MC_EXT_PORTS, e[2]));
		else
			seq_printf(s, "port %d age %lu%s%s\n", port,
				   FIELD_GET(RTL9602C_FDB_W2_AGE, e[2]),
				   e[1] & RTL9602C_FDB_W1_ROUTER_MAC ?
					" router" : "",
				   e[2] & RTL9602C_FDB_W2_NEXTHOP ?
					" nexthop" : "");
		used++;
	}
	seq_printf(s, "%u of %u entries in use, the CAM from %u\n", used,
		   RTL9602C_FDB_ENTRIES, RTL9602C_FDB_HASH_ENTRIES);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_fdb);

/**
 * rtl9602c_l2_debugfs_init() - create the address table's debugfs view
 * @p: the switch, with its debugfs directory created
 *
 * Creates fdb, mode 0400.
 */
void rtl9602c_l2_debugfs_init(struct rtl9602c_priv *p)
{
	debugfs_create_file("fdb", 0400, p->debugfs, p, &rtl9602c_fdb_fops);
}

#endif
