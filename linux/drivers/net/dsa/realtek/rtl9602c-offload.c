// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: filtering, QoS and the L3/NAT engine.
 *
 * The switch's filtering and QoS, as tc and DCB offloads. A flower rule that
 * drops or traps becomes an ACL rule, and one that rewrites a C-VID a
 * classifier entry; one that polices broadcast, multicast or ARP by packet rate
 * becomes storm control, on one of the sixteen meters the devlink storm-control
 * parameters also draw from; a matchall police is the port's ingress policer.
 * The classifier also holds the entries that give each frame the switch
 * forwards to the PON port its GEM stream. DCB sets the default priority, the
 * DSCP map and whether PCP or DSCP is trusted first; tc tbf and ets set each
 * port's egress shaper and scheduler.
 *
 * The L3/NAT engine translates the source address and port of IPv4 TCP and UDP
 * flows (NAPT, RFC 2663) and forwards them, and their replies, without the CPU,
 * over Ethernet or a PPPoE session. Its interface, neighbour, next-hop, route
 * and external-address tables follow the kernel's address, neighbour and route
 * notifiers, and each flow that netfilter's flowtable offload hands over gets
 * one outbound and one inbound NAPT entry.
 *
 * Debugfs views at the end show these blocks, the queue depths, address
 * learning, the reserved multicast addresses, the S-VLAN registers and the
 * egress descriptor capture.
 *
 * Setup resets storm control and the meters and empties the L3/NAT tables. It
 * does not reset the ACL, the classifier, the priority weights and maps, the
 * policer, the scheduler or the shaper: after a warm restart or a reload these
 * keep what was programmed before the driver started, while the bookkeeping here starts
 * empty.
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
#include <linux/if_pppox.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <net/netevent.h>
#include <net/arp.h>
#include <net/fib_notifier.h>
#include <net/ip_fib.h>
#include <net/nexthop.h>
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
 * The storm-control classes, indexed by RTL9602C_STORM_UM to
 * RTL9602C_STORM_IGMP: each has an enable register, one bit per port, and a
 * meter selector, four bits per port naming one of the sixteen meters. The
 * unknown-unicast, unknown-multicast and broadcast classes police as set
 * (measured on this board); that the multicast, ARP, DHCP and IGMP classes do
 * is not verified.
 */
const struct rtl9602c_storm_class_row rtl9602c_storm_class[] = {
	{ "unknown-multicast",	RTL9602C_STORM_UNKNOWN_MCAST_EN,
				RTL9602C_STORM_UNKNOWN_MCAST_METER_SEL },
	{ "unknown-unicast",	RTL9602C_STORM_UNKNOWN_UCAST_EN,
				RTL9602C_STORM_UNKNOWN_UCAST_METER_SEL },
	{ "multicast",		RTL9602C_STORM_MCAST_EN,   RTL9602C_STORM_MCAST_METER_SEL },
	{ "broadcast",		RTL9602C_STORM_BCAST_EN,   RTL9602C_STORM_BCAST_METER_SEL },
	{ "arp",		RTL9602C_STORM_ARP_EN,  RTL9602C_STORM_ARP_METER_SEL },
	{ "dhcp",		RTL9602C_STORM_DHCP_EN, RTL9602C_STORM_DHCP_METER_SEL },
	{ "igmp",		RTL9602C_STORM_IGMP_EN, RTL9602C_STORM_IGMP_METER_SEL },
};

/*
 * The L3/NAT engine's tables, by selector: the data words of an entry and the
 * rows the driver uses. A selector without rows names no table: an access to
 * selector 8 never completes and leaves the engine's table port stuck until a
 * chip reset. So every access is checked against these first, and an index past
 * the last row is refused as well, on the assumption, not verified, that it
 * would do the same. The word counts of the IPv6 route, binding and WAN-type
 * tables, which only the clear at probe touches, are not verified.
 */
static const u8 rtl9602c_nat_tbl_words[16] = {
	[RTL9602C_L3TBL_CTRL_TABLE_ROUTE]	= 2,
	[RTL9602C_L3TBL_CTRL_TABLE_PPPOE]	= 1,
	[RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP]	= 1,
	[RTL9602C_L3TBL_CTRL_TABLE_L3IF]	= 4,
	[RTL9602C_L3TBL_CTRL_TABLE_EXTADDR]	= 3,
	[RTL9602C_L3TBL_CTRL_TABLE_ROUTE6]	= 5,
	[RTL9602C_L3TBL_CTRL_TABLE_BIND]	= 1,
	[RTL9602C_L3TBL_CTRL_TABLE_WANTYPE]	= 1,
	[RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN]	= 3,
	[RTL9602C_L3TBL_CTRL_TABLE_NAPT]		= 1,
	[RTL9602C_L3TBL_CTRL_TABLE_NEIGH6]	= 3,
	[RTL9602C_L3TBL_CTRL_TABLE_FLOW]	= 4,
	[RTL9602C_L3TBL_CTRL_TABLE_NEIGH]	= 2,
};

static const u16 rtl9602c_nat_tbl_entries[16] = {
	[RTL9602C_L3TBL_CTRL_TABLE_ROUTE]	= 16,
	[RTL9602C_L3TBL_CTRL_TABLE_PPPOE]	= 8,
	[RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP]	= 16,
	[RTL9602C_L3TBL_CTRL_TABLE_L3IF]	= 16,
	[RTL9602C_L3TBL_CTRL_TABLE_EXTADDR]	= 8,
	[RTL9602C_L3TBL_CTRL_TABLE_ROUTE6]	= 8,
	[RTL9602C_L3TBL_CTRL_TABLE_BIND]	= 32,
	[RTL9602C_L3TBL_CTRL_TABLE_WANTYPE]	= 16,
	[RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN]	= 4096,
	[RTL9602C_L3TBL_CTRL_TABLE_NAPT]		= 4096,
	[RTL9602C_L3TBL_CTRL_TABLE_NEIGH6]	= 128,
	[RTL9602C_L3TBL_CTRL_TABLE_FLOW]	= 256,
	[RTL9602C_L3TBL_CTRL_TABLE_NEIGH]	= 128,
};

/*
 * The engine's own table port, apart from the switch's table engine: writing
 * the command starts an access, and its READ or WRITE bit clears itself once
 * the access is done; there is no status register. table_lock serializes this
 * port as it does the switch's engine.
 */
static int rtl9602c_nat_tbl_read(struct rtl9602c_priv *p, unsigned int table,
				 unsigned int index, u32 *data,
				 unsigned int words)
{
	unsigned int i;
	u32 cmd;
	int err;

	if (table >= ARRAY_SIZE(rtl9602c_nat_tbl_entries) ||
	    !rtl9602c_nat_tbl_entries[table] ||
	    index >= rtl9602c_nat_tbl_entries[table] ||
	    words != rtl9602c_nat_tbl_words[table])
		return -EINVAL;

	mutex_lock(&p->table_lock);

	cmd = FIELD_PREP(RTL9602C_L3TBL_CTRL_INDEX, index) |
	      FIELD_PREP(RTL9602C_L3TBL_CTRL_TABLE, table) |
	      RTL9602C_L3TBL_CTRL_READ;
	rtl9602c_w(p, RTL9602C_L3TBL_CTRL, cmd);

	err = read_poll_timeout(rtl9602c_r, cmd,
				!(cmd & (RTL9602C_L3TBL_CTRL_WRITE |
					 RTL9602C_L3TBL_CTRL_READ)),
				10, 1000, false, p, RTL9602C_L3TBL_CTRL);
	if (err) {
		pr_err("L3/NAT table %u entry %u: engine still busy after 1 ms\n",
		       table, index);
		goto out;
	}

	for (i = 0; i < words; i++)
		data[i] = rtl9602c_r(p, RTL9602C_L3TBL_READ_DATA(i));
out:
	mutex_unlock(&p->table_lock);
	return err;
}

/*
 * All five write-data registers are loaded, zeros past the table's words, so
 * that no word of an earlier write can reach the entry. Whether a write commits
 * all five whatever the table's width is not verified; the padding is a
 * precaution.
 */
static int rtl9602c_nat_tbl_write(struct rtl9602c_priv *p, unsigned int table,
				  unsigned int index, const u32 *data,
				  unsigned int words)
{
	unsigned int i;
	u32 cmd;
	int err;

	if (table >= ARRAY_SIZE(rtl9602c_nat_tbl_entries) ||
	    !rtl9602c_nat_tbl_entries[table] ||
	    index >= rtl9602c_nat_tbl_entries[table] ||
	    words != rtl9602c_nat_tbl_words[table])
		return -EINVAL;

	mutex_lock(&p->table_lock);

	for (i = 0; i < RTL9602C_L3TBL_DATA_WORDS; i++)
		rtl9602c_w(p, RTL9602C_L3TBL_WRITE_DATA(i),
			   i < words ? data[i] : 0);

	cmd = FIELD_PREP(RTL9602C_L3TBL_CTRL_INDEX, index) |
	      FIELD_PREP(RTL9602C_L3TBL_CTRL_TABLE, table) |
	      RTL9602C_L3TBL_CTRL_WRITE;
	rtl9602c_w(p, RTL9602C_L3TBL_CTRL, cmd);

	err = read_poll_timeout(rtl9602c_r, cmd,
				!(cmd & (RTL9602C_L3TBL_CTRL_WRITE |
					 RTL9602C_L3TBL_CTRL_READ)),
				10, 1000, false, p, RTL9602C_L3TBL_CTRL);
	if (err)
		pr_err("L3/NAT table %u entry %u: write still busy after 1 ms\n",
		       table, index);

	mutex_unlock(&p->table_lock);
	return err;
}

/*
 * A classifier entry as composed here: the bits of rule word 0 to compare and
 * their values, the direction, and the three action words.
 */
struct rtl9602c_cf_entry {
	u32 value;
	u32 care;
	bool downstream;
	u32 act[3];
	u8 tmpl;
};

/*
 * A classifier rule is two entries of the rule table: @idx holds the bits that
 * must be 0 (care and not value), @idx + RTL9602C_CLS_RULE_ONES_PLANE those
 * that must be 1; a bit clear in both is not compared. The direction bit is
 * always compared, so an entry matches frames of one direction only. The ones
 * plane carries the valid bit and goes last, so that no frame meets a
 * half-written entry.
 */
static int rtl9602c_cf_program(struct rtl9602c_priv *p, unsigned int idx,
			       const struct rtl9602c_cf_entry *e)
{
	u32 care = e->care | RTL9602C_CLS_RULE_DS;
	u32 value = (e->value & e->care) |
		    (e->downstream ? RTL9602C_CLS_RULE_DS : 0);
	u32 plane[3];
	int err;

	plane[0] = care & ~value;
	plane[1] = 0;
	err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_CLS_RULE, idx, plane, 2);
	if (err)
		return err;

	memcpy(plane, e->act, sizeof(plane));
	err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_CLS_ACT, idx, plane, 3);
	if (err)
		return err;
	/*
	 * The action classes that apply: upstream all of them, stream
	 * assignment included; downstream all but that one. That these others
	 * include the C-tag action is not verified.
	 */
	rtl9602c_w(p, RTL9602C_CLS_ACT_EN(idx),
		   RTL9602C_CLS_ACT_EN_OTHERS |
		   (e->downstream ? 0 : RTL9602C_CLS_ACT_EN_FLOW));

	/* The template that VID and PRI are matched with. */
	rtl9602c_rmw(p, RTL9602C_CLS_ENTRY_TMPL + 4 * (idx / 32),
		     BIT(idx % 32),
		     e->tmpl == RTL9602C_CLS_TMPL_S ? BIT(idx % 32) : 0);

	plane[0] = value;
	plane[1] = RTL9602C_CLS_RULE_VALID;
	return rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_CLS_RULE,
				    idx + RTL9602C_CLS_RULE_ONES_PLANE, plane, 2);
}

static bool rtl9602c_port_is_downstream(int port)
{
	return port == RTL9602C_PORT_PON;
}

/*
 * Write VLAN rule @idx as p->cf_xlate[@idx] records it (see
 * rtl9602c_flower_vlan()), with the stream of an upstream rule. Of several
 * matching entries the lowest applies, and the VLAN rules sit below the stream
 * entries, so an upstream rule names its stream itself: the GEM flow of the
 * upstream GEM interface that claims the VLAN the frame leaves in, if any, and
 * otherwise the flow its port's own stream entry gives, which the frame would
 * have taken without the rule. Without a stream the frame reaches the PON MAC
 * without a GEM port and is lost (measured on this board).
 */
static int rtl9602c_cf_write_xlate(struct rtl9602c_priv *p, unsigned int idx)
{
	const struct rtl9602c_cf_xlate *x = &p->cf_xlate[idx];
	struct rtl9602c_cf_entry e = {
		.value		= x->value,
		.care		= x->care,
		.downstream	= rtl9602c_port_is_downstream(x->port),
		.act		= { x->act[0], x->act[1], x->act[2] },
		.tmpl		= x->tmpl,
	};
	int flow = -1;

	if (!e.downstream && x->out_vid < VLAN_N_VID &&
	    p->gem_vid_flow[x->out_vid])
		flow = p->gem_vid_flow[x->out_vid] - 1u;
	else if (!e.downstream && x->port < RTL9602C_NUM_PORTS)
		flow = p->cf_sid_flow[x->port];
	if (flow >= 0)
		e.act[0] |= RTL9602C_CLS_ACT_FLOW_SET |
			    FIELD_PREP(RTL9602C_CLS_ACT_FLOW, flow);

	return rtl9602c_cf_program(p, idx, &e);
}

/* Zeroing the ones plane clears the valid bit and retires the entry. */
static int rtl9602c_cf_invalidate(struct rtl9602c_priv *p, unsigned int idx)
{
	u32 zero[2] = { 0, 0 };

	return rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_CLS_RULE,
				    idx + RTL9602C_CLS_RULE_ONES_PLANE, zero, 2);
}

/*
 * Stream entries. A frame the switch forwards to the PON port leaves on the
 * fibre only if a classifier entry gave it a stream, the GEM flow of a GEM
 * port: the PON MAC drops a frame without one while the switch counts it as
 * sent. A frame the CPU sends names its stream in its descriptor instead. A
 * stream entry keys on the ingress port, or on the port and a C-VID, and
 * assigns a GEM flow.
 */
static int rtl9602c_cf_write_sid(struct rtl9602c_priv *p, unsigned int idx,
				 int port, unsigned int flow)
{
	struct rtl9602c_cf_entry e = {
		.value	= FIELD_PREP(RTL9602C_CLS_RULE_UNI, port),
		.care	= RTL9602C_CLS_RULE_UNI,
		.act	= { RTL9602C_CLS_ACT_FLOW_SET |
			    FIELD_PREP(RTL9602C_CLS_ACT_FLOW, flow), 0, 0 },
	};

	return rtl9602c_cf_program(p, idx, &e);
}

/*
 * Frames entering by @port with a C-tag of VID @vid; matching on the VID is not
 * verified.
 */
static int rtl9602c_cf_write_sid_vlan(struct rtl9602c_priv *p, unsigned int idx,
				      int port, u16 vid, unsigned int flow)
{
	struct rtl9602c_cf_entry e = {
		.value	= FIELD_PREP(RTL9602C_CLS_RULE_UNI, port) |
			  RTL9602C_CLS_RULE_CTAG |
			  FIELD_PREP(RTL9602C_CLS_RULE_VID, vid),
		.care	= RTL9602C_CLS_RULE_UNI | RTL9602C_CLS_RULE_VLAN_CARE,
		.act	= { RTL9602C_CLS_ACT_FLOW_SET |
			    FIELD_PREP(RTL9602C_CLS_ACT_FLOW, flow), 0, 0 },
	};

	return rtl9602c_cf_program(p, idx, &e);
}

/*
 * Stream entries are taken from the top of the usable range downwards and
 * translations from the bottom: the lowest matching entry applies, so a
 * translation overrides its port's stream entry.
 */
static int rtl9602c_cf_sid_alloc(struct rtl9602c_priv *p)
{
	int idx;

	for (idx = p->cf_entries - 1; idx >= 0; idx--)
		if (!test_bit(idx, p->cf_used))
			break;
	if (idx < 0)
		return -ENOSPC;

	set_bit(idx, p->cf_used);
	set_bit(idx, p->cf_sid_owned);
	p->cf_cookie[idx] = 0;
	return idx;
}

static void rtl9602c_cf_sid_release(struct rtl9602c_priv *p)
{
	unsigned int idx;

	for_each_set_bit(idx, p->cf_sid_owned, p->cf_entries) {
		rtl9602c_cf_invalidate(p, idx);
		clear_bit(idx, p->cf_used);
	}
	bitmap_zero(p->cf_sid_owned, RTL9602C_CLS_ENTRIES);

	for (unsigned int i = 0; i < RTL9602C_NUM_PORTS; i++) {
		p->cf_sid[i] = -1;
		p->cf_sid_flow[i] = -1;
	}
}

/**
 * rtl9602c_cf_sid_refresh() - rebuild the classifier's stream entries
 * @p: the switch
 *
 * Every stream entry is retired and the set is computed again, for each user
 * port but the PON port. A port outside the GEM interfaces' bridge gets one
 * entry for the routed flow, the GEM flow routed frames leave in, if one is
 * claimed. A port in that bridge gets one entry for the bridge's upstream flow,
 * that of the last upstream GEM interface to join (or of another, once that
 * one has left), while no VLAN claims a GEM flow; otherwise one entry per
 * claimed VLAN, keyed on the C-VID, so that a frame in no claimed VLAN gets
 * no stream. A port the classifier has no room for is left without a stream,
 * with a warning.
 *
 * Called at setup, on each bridge membership change, on each VLAN change of an
 * upstream GEM interface and on each change of the routed flow.
 */
void rtl9602c_cf_sid_refresh(struct rtl9602c_priv *p)
{
	unsigned int claimed = 0;
	struct dsa_port *dp;
	unsigned int vid;

	rtl9602c_cf_sid_release(p);

	for (vid = 0; vid < VLAN_N_VID; vid++)
		if (p->gem_vid_flow[vid])
			claimed++;

	dsa_switch_for_each_user_port(dp, &p->ds) {
		int port = dp->index;
		bool bridged = p->pon_br && port != RTL9602C_PORT_PON &&
			       dp->bridge && dp->bridge->dev == p->pon_br;
		int idx;

		if (!bridged) {
			/*
			 * Outside the GEM bridge: the routed flow, if one is
			 * claimed, for every port but the PON port.
			 */
			if (port == RTL9602C_PORT_PON || p->l34_us_flow < 0)
				continue;

			idx = rtl9602c_cf_sid_alloc(p);
			if (idx < 0) {
				pr_warn("classifier full: routed port %d's frames get no GEM port upstream\n",
					port);
				continue;
			}
			p->cf_sid[port] = idx;
			p->cf_sid_flow[port] = p->l34_us_flow;
			if (rtl9602c_cf_write_sid(p, idx, port, p->l34_us_flow))
				pr_warn("classifier entry %d was not written\n",
					idx);
			else
				pr_debug("port %d routed upstream: stream %d, entry %d\n",
					 port, p->l34_us_flow, idx);
			continue;
		}

		/* In the GEM bridge, while no VLAN claims a GEM flow. */
		if (!claimed) {
			if (p->pon_br_us_flow < 0)
				continue;

			idx = rtl9602c_cf_sid_alloc(p);
			if (idx < 0) {
				pr_warn("classifier full: port %d's frames get no GEM port upstream\n",
					port);
				continue;
			}
			p->cf_sid[port] = idx;
			p->cf_sid_flow[port] = p->pon_br_us_flow;
			if (rtl9602c_cf_write_sid(p, idx, port,
						  p->pon_br_us_flow))
				pr_warn("classifier entry %d was not written\n",
					idx);
			else
				pr_debug("port %d upstream: stream %d, entry %d\n",
					 port, p->pon_br_us_flow, idx);
			continue;
		}

		/*
		 * Otherwise one entry per claimed VLAN; gem_vid_flow[] holds a
		 * VLAN's GEM flow plus one.
		 */
		for (vid = 0; vid < VLAN_N_VID; vid++) {
			unsigned int flow;

			if (!p->gem_vid_flow[vid])
				continue;
			flow = p->gem_vid_flow[vid] - 1u;

			idx = rtl9602c_cf_sid_alloc(p);
			if (idx < 0) {
				pr_warn("classifier full: port %d VLAN %u gets no GEM port upstream\n",
					port, vid);
				break;
			}
			if (rtl9602c_cf_write_sid_vlan(p, idx, port, (u16)vid,
						       flow))
				pr_warn("classifier entry %d was not written\n",
					idx);
			else
				pr_debug("port %d VLAN %u upstream: stream %u, entry %d\n",
					 port, vid, flow, idx);
		}
	}

	/*
	 * An upstream VLAN rule names its stream itself (see
	 * rtl9602c_cf_write_xlate()), which the streams just computed may have
	 * changed: write each one again.
	 */
	for_each_set_bit(vid, p->cf_used, p->cf_entries) {
		if (!p->cf_cookie[vid] ||
		    rtl9602c_port_is_downstream(p->cf_xlate[vid].port))
			continue;
		if (rtl9602c_cf_write_xlate(p, vid))
			pr_warn("classifier entry %u was not written\n", vid);
	}
}

/*
 * Meters. Sixteen shared meters serve storm control, from tc and devlink alike.
 * A meter is 64 bits in two words, the high half, which holds the rate, at the
 * lower address. Only packet mode is used: in byte mode a meter does not police
 * (measured on this board).
 */
static int rtl9602c_meter_alloc(struct rtl9602c_priv *p)
{
	unsigned int i = find_first_zero_bit(p->meter_used, RTL9602C_METER_COUNT);

	if (i >= RTL9602C_METER_COUNT)
		return -ENOSPC;
	set_bit(i, p->meter_used);
	return (int)i;
}

/**
 * rtl9602c_meter_free() - reset a meter and give it back
 * @p: the switch
 * @meter: the meter, 0 to 15
 *
 * The meter is written back to the maximum rate, which never limits, the
 * default bucket and byte mode, so that a meter with no owner limits nothing.
 */
void rtl9602c_meter_free(struct rtl9602c_priv *p, unsigned int meter)
{
	rtl9602c_w(p, RTL9602C_METER_CFG_HI(meter), RTL9602C_METER_CFG_HI_RATE_MAX);
	rtl9602c_w(p, RTL9602C_METER_CFG_LO(meter),
		   FIELD_PREP(RTL9602C_METER_CFG_LO_BURST,
			      RTL9602C_METER_CFG_LO_BURST_DEFAULT));
	clear_bit(meter, p->meter_used);
}

/*
 * Program @meter in packet mode, where one rate unit is one packet a second, at
 * @units, with the default bucket: a bucket of one token exceeds at any rate.
 */
static void rtl9602c_meter_program(struct rtl9602c_priv *p, unsigned int meter,
				   u32 units)
{
	rtl9602c_w(p, RTL9602C_METER_CFG_HI(meter),
		   FIELD_PREP(RTL9602C_METER_CFG_HI_RATE, units));
	rtl9602c_w(p, RTL9602C_METER_CFG_LO(meter),
		   FIELD_PREP(RTL9602C_METER_CFG_LO_BURST,
			      RTL9602C_METER_CFG_LO_BURST_DEFAULT) |
		   FIELD_PREP(RTL9602C_METER_CFG_LO_MODE,
			      RTL9602C_METER_CFG_LO_MODE_PACKET));
}

/*
 * Point @port's selector for class @cls at @meter and set the port's bit in the
 * class's enable register, or clear the bit and point the selector at @meter.
 * The bit is set last and cleared first, so that the class is never live on a
 * meter it was not given, meter 0 included.
 */
static void rtl9602c_storm_bind(struct rtl9602c_priv *p, unsigned int cls,
				int port, unsigned int meter, bool on)
{
	u32 sel = rtl9602c_storm_class[cls].meter;
	u32 en = rtl9602c_storm_class[cls].ctrl;

	if (!on)
		rtl9602c_rmw(p, en, BIT(port), 0);
	rtl9602c_rmw(p, sel, RTL9602C_STORM_METER_SEL_PORT(port),
		     (u32)meter << (4 * port));
	if (on)
		rtl9602c_rmw(p, en, BIT(port), BIT(port));
}

/**
 * rtl9602c_storm_ports_get() - read the ports a storm-control class polices
 * @p: the switch
 * @cls: the class, an RTL9602C_STORM_* index into rtl9602c_storm_class[]
 *
 * Read from the class's enable register, so that a change made outside the
 * driver shows.
 *
 * Return: the port mask, bit n for port n.
 */
u32 rtl9602c_storm_ports_get(struct rtl9602c_priv *p, unsigned int cls)
{
	return rtl9602c_r(p, rtl9602c_storm_class[cls].ctrl) &
	       RTL9602C_DOS_PORT_EN_MASK;
}

/**
 * rtl9602c_storm_rate_get() - read a storm-control class's packet rate
 * @p: the switch
 * @cls: the class, as in rtl9602c_storm_ports_get()
 *
 * Read from the class's meter. The driver records only which meter each class
 * owns: a selector of 0 cannot tell meter 0 from none.
 *
 * Return: packets a second, or 0 when the class owns no meter or its meter is
 * at the maximum, which does not limit.
 */
u32 rtl9602c_storm_rate_get(struct rtl9602c_priv *p, unsigned int cls)
{
	int meter = p->storm_meter[cls];
	u32 rate;

	if (meter < 0)
		return 0;
	rate = FIELD_GET(RTL9602C_METER_CFG_HI_RATE,
			 rtl9602c_r(p, RTL9602C_METER_CFG_HI(meter)));
	return rate == RTL9602C_METER_CFG_HI_RATE_MAX ? 0 : rate;
}

/**
 * rtl9602c_storm_rate_set() - set a storm-control class's packet rate
 * @p: the switch
 * @cls: the class, as in rtl9602c_storm_ports_get()
 * @rate: packets a second, below %RTL9602C_METER_CFG_HI_RATE_MAX, which the
 *	caller checks; 0 switches the class off
 *
 * A class without a meter takes one. The meter is programmed at once and every
 * port's selector pointed at it; the enable bits stay as they are. Rate 0
 * unbinds every port and gives the meter back.
 *
 * Return: 0, or -ENOSPC when all sixteen meters are taken.
 */
int rtl9602c_storm_rate_set(struct rtl9602c_priv *p, unsigned int cls,
			    u32 rate)
{
	int meter = p->storm_meter[cls];
	u32 ports;
	unsigned int port;

	if (!rate) {
		if (meter < 0)
			return 0;
		for (port = 0; port < RTL9602C_NUM_PORTS; port++)
			rtl9602c_storm_bind(p, cls, port, 0, false);
		rtl9602c_meter_free(p, (unsigned int)meter);
		p->storm_meter[cls] = -1;
		return 0;
	}

	if (meter < 0) {
		meter = rtl9602c_meter_alloc(p);
		if (meter < 0)
			return meter;
		p->storm_meter[cls] = meter;
	}

	rtl9602c_meter_program(p, (unsigned int)meter, rate);

	ports = rtl9602c_storm_ports_get(p, cls);
	for (port = 0; port < RTL9602C_NUM_PORTS; port++)
		rtl9602c_storm_bind(p, cls, port, (unsigned int)meter,
				    ports & BIT(port));
	return 0;
}

/**
 * rtl9602c_storm_ports_set() - choose the ports a storm-control class polices
 * @p: the switch
 * @cls: the class, as in rtl9602c_storm_ports_get()
 * @ports: the port mask, bit n for port n; the caller refuses bits beyond the
 *	four ports
 *
 * Return: 0, or -EINVAL when @ports is not empty and the class has no rate.
 */
int rtl9602c_storm_ports_set(struct rtl9602c_priv *p, unsigned int cls,
			     u32 ports)
{
	int meter = p->storm_meter[cls];
	unsigned int port;

	if (ports && meter < 0)
		return -EINVAL;

	for (port = 0; port < RTL9602C_NUM_PORTS; port++)
		rtl9602c_storm_bind(p, cls, port,
				    meter < 0 ? 0 : (unsigned int)meter,
				    ports & BIT(port));
	return 0;
}

/*
 * Storm control from tc: each flower rule takes a meter of its own, programmed
 * at the rule's packet rate, and enables its class on its port. A class has one
 * meter selector per port, so of several rules for one class and port the one
 * added last polices. This is what keeps the class on through `tc filter
 * replace`, which adds the new rule before it removes the old.
 */
static int rtl9602c_storm_add(struct rtl9602c_priv *p, int port,
			      unsigned int cls, u64 rate_pkt_ps,
			      unsigned long cookie,
			      struct netlink_ext_ack *extack)
{
	u64 units = rate_pkt_ps;
	int meter, slot;

	/* A rate beyond the field is no limit, as the maximum is. */
	if (units > RTL9602C_METER_CFG_HI_RATE_MAX)
		units = RTL9602C_METER_CFG_HI_RATE_MAX;

	for (slot = 0; slot < RTL9602C_METER_COUNT; slot++)
		if (!p->storm[slot].used)
			break;
	if (slot == RTL9602C_METER_COUNT) {
		NL_SET_ERR_MSG_MOD(extack, "no room for another storm rule");
		return -ENOSPC;
	}

	meter = rtl9602c_meter_alloc(p);
	if (meter < 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "every one of the sixteen shared meters is in use");
		return meter;
	}

	rtl9602c_meter_program(p, (unsigned int)meter, (u32)units);
	rtl9602c_storm_bind(p, cls, port, (unsigned int)meter, true);

	p->storm[slot].cookie = cookie;
	p->storm[slot].cls = cls;
	p->storm[slot].port = port;
	p->storm[slot].meter = meter;
	p->storm[slot].used = true;
	p->storm[slot].seq = ++p->storm_seq;
	return 0;
}

/* The rule for @cls on @port added last, leaving out slot @skip, or -1. */
static int rtl9602c_storm_latest(struct rtl9602c_priv *p, unsigned int cls,
				 int port, int skip)
{
	int slot, latest = -1;

	for (slot = 0; slot < RTL9602C_METER_COUNT; slot++) {
		if (slot == skip || !p->storm[slot].used ||
		    p->storm[slot].cls != cls || p->storm[slot].port != port)
			continue;
		if (latest < 0 ||
		    (s32)(p->storm[slot].seq - p->storm[latest].seq) > 0)
			latest = slot;
	}
	return latest;
}

/*
 * Removing the rule that polices its class and port hands the class to the
 * rule for them added last before it, or switches the class off on the port if
 * there is none; removing any other rule leaves the binding alone. The meter is
 * freed after the selector has left it.
 */
static int rtl9602c_storm_del(struct rtl9602c_priv *p, unsigned long cookie)
{
	unsigned int cls;
	int slot, next, port;

	for (slot = 0; slot < RTL9602C_METER_COUNT; slot++)
		if (p->storm[slot].used && p->storm[slot].cookie == cookie)
			break;
	if (slot == RTL9602C_METER_COUNT)
		return -ENOENT;

	cls = p->storm[slot].cls;
	port = p->storm[slot].port;
	if (rtl9602c_storm_latest(p, cls, port, -1) == slot) {
		next = rtl9602c_storm_latest(p, cls, port, slot);
		if (next >= 0)
			rtl9602c_storm_bind(p, cls, port, p->storm[next].meter,
					    true);
		else
			rtl9602c_storm_bind(p, cls, port, 0, false);
	}
	rtl9602c_meter_free(p, p->storm[slot].meter);
	p->storm[slot].used = false;
	return 0;
}

/*
 * The storm class a flower rule's destination match names: the broadcast
 * address, all ones, or the multicast group bit alone, the least significant
 * bit of the first octet (IEEE 802.3, 3.2.3). A rule that matches the source
 * address names none. The rule's other keys are checked by
 * rtl9602c_storm_rule_check().
 */
static int rtl9602c_storm_class_of(struct flow_rule *rule)
{
	struct flow_match_eth_addrs eth;
	static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const u8 mcbit[ETH_ALEN] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS))
		return -1;

	flow_rule_match_eth_addrs(rule, &eth);
	if (!is_zero_ether_addr(eth.mask->src))
		return -1;

	if (ether_addr_equal(eth.mask->dst, bcast) &&
	    ether_addr_equal(eth.key->dst, bcast))
		return RTL9602C_STORM_BC;
	if (ether_addr_equal(eth.mask->dst, mcbit) &&
	    ether_addr_equal(eth.key->dst, mcbit))
		return RTL9602C_STORM_MC;
	return -1;
}

/*
 * Egress scheduling and shaping, from tc ets and tbf. Each port has eight
 * queues. Strict queues are served first, the highest-numbered first, and
 * weighted queues share what is left by weight.
 */

/*
 * Queue 0's weight is 16 bits, two ports to a word; the weights of queues 1 to
 * 7 are 10 bits, three to a word and three words to a port.
 */
static void rtl9602c_wfq_weight(struct rtl9602c_priv *p, int port,
				unsigned int q, u32 w)
{
	u32 off, mask;
	unsigned int sh;

	if (q == 0) {
		off = RTL9602C_SCHED_WEIGHT_Q0 + 4 * (port / 2);
		sh = 16 * (port % 2);
		mask = (u32)RTL9602C_SCHED_WEIGHT_Q0_MAX << sh;
	} else {
		off = RTL9602C_SCHED_WEIGHT_Q1_7 + 12 * port + 4 * ((q - 1) / 3);
		sh = 10 * ((q - 1) % 3);
		mask = (u32)RTL9602C_SCHED_WEIGHT_Q1_7_MAX << sh;
	}
	rtl9602c_rmw(p, off, mask, (w << sh) & mask);
}

/*
 * The port's egress rate, in steps of 1000 bytes a second (8 kbit/s). Port 0
 * enforces it only when RTL9602C_SHAPE_2G5_CFG holds it too, in steps of twice
 * that; that word also holds the SerDes lane's 2.5G enable, hence the
 * read-modify-write. A SerDes bring-up at 2500BASE-X copies the rate of
 * RTL9602C_SHAPE_PORT_CFG there, so it keeps a limit on port 0.
 */
static void rtl9602c_egress_shaper(struct rtl9602c_priv *p, int port, u32 rate)
{
	rtl9602c_rmw(p, RTL9602C_SHAPE_PORT_CFG + 4 * port,
		     RTL9602C_SHAPE_PORT_CFG_RATE,
		     FIELD_PREP(RTL9602C_SHAPE_PORT_CFG_RATE, rate));

	if (port == 0)
		rtl9602c_rmw(p, RTL9602C_SHAPE_2G5_CFG, RTL9602C_SHAPE_2G5_CFG_RATE,
			     FIELD_PREP(RTL9602C_SHAPE_2G5_CFG_RATE, rate / 2));
}

/*
 * The priority-to-queue word of each port as a full chip reset leaves it,
 * read in the qos view after a reset by the watchdog: port 0's sends every
 * priority to queue 0, port 1's priorities 4 to 7 to queue 1, the PON port's
 * priorities 2n and 2n + 1 to queue n, and the CPU port's priority n to
 * queue n.
 */
static const u32 rtl9602c_prio_queue_reset[RTL9602C_NUM_PORTS] = {
	0x00000000, 0x00249000, 0x006d2240, 0x00fac688,
};

/**
 * rtl9602c_filter_qos_reset() - the ACL, the classifier and QoS as at reset
 * @p: the switch
 *
 * A warm reboot and a driver reload leave these blocks as the previous kernel
 * wrote them while this driver's bookkeeping starts empty, so setup writes
 * them as a full chip reset leaves them, which the qos view showed after a
 * reset by the watchdog: every ACL rule retired with its action classes back
 * at their default, ACL lookup off on every port with no default permit;
 * every classifier entry retired and the verdict for a frame no entry
 * matches back to letting it through; and per port the priority-to-queue
 * word of the reset, every queue strict at weight 1, no queue under a packet
 * rate, and the egress shaper and the ingress policer at their highest rate.
 * The DSCP map, the port-priority word and the priority-selection weights
 * read 0 after the reset and are written 0.
 *
 * Context: setup, before the driver writes classifier entries of its own.
 */
void rtl9602c_filter_qos_reset(struct rtl9602c_priv *p)
{
	u32 zero[RTL9602C_ACL_RULE_WORDS] = {};
	unsigned int i, port, q;

	for (i = 0; i < RTL9602C_ACL_ENTRIES; i++) {
		rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_ACL_RULE,
				     i + RTL9602C_ACL_RULE_ONES_PLANE, zero,
				     RTL9602C_ACL_RULE_WORDS);
		rtl9602c_w(p, RTL9602C_ACL_ACT_EN + 4 * i,
			   RTL9602C_ACL_ACT_EN_DEFAULT);
	}
	rtl9602c_w(p, RTL9602C_ACL_PORT_EN, 0);
	rtl9602c_w(p, RTL9602C_ACL_DEFAULT_PERMIT, 0);

	for (i = 0; i < RTL9602C_CLS_ENTRIES; i++)
		rtl9602c_cf_invalidate(p, i);
	rtl9602c_rmw(p, RTL9602C_CLS_CTRL,
		     GENMASK(1, 0) | RTL9602C_CLS_CTRL_PON_EN,
		     RTL9602C_CLS_CTRL_PON_EN);

	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		rtl9602c_w(p, RTL9602C_QUEUE_RATE_EN(port), 0);
		rtl9602c_w(p, RTL9602C_QUEUE_RATE_METER(port), 0);
		rtl9602c_w(p, RTL9602C_QOS_PRIO_TO_QUEUE + 4 * port,
			   rtl9602c_prio_queue_reset[port]);
		rtl9602c_w(p, RTL9602C_SCHED_WFQ_QUEUES + 4 * port, 0);
		for (q = 0; q < RTL9602C_QUEUES; q++)
			rtl9602c_wfq_weight(p, port, q, 1);
		rtl9602c_egress_shaper(p, port, RTL9602C_SHAPE_PORT_CFG_RATE_MAX);
		rtl9602c_rmw(p, RTL9602C_POLICE_PORT(port),
			     RTL9602C_POLICE_PORT_RATE,
			     FIELD_PREP(RTL9602C_POLICE_PORT_RATE,
					RTL9602C_POLICE_PORT_RATE_MAX));
	}

	for (i = 0; i < DIV_ROUND_UP(RTL9602C_QOS_DSCP_COUNT,
				     RTL9602C_QOS_MAP_PER_WORD); i++)
		rtl9602c_w(p, RTL9602C_QOS_DSCP_TO_PRIO + 4 * i, 0);
	rtl9602c_w(p, RTL9602C_QOS_PORT_PRIO, 0);
	rtl9602c_w(p, RTL9602C_QOS_PRIO_WEIGHT, 0);
	rtl9602c_w(p, RTL9602C_QOS_PRIO_WEIGHT + 4, 0);
	rtl9602c_w(p, RTL9602C_QOS_PRIO_WEIGHT_SEL, 0);
}

/**
 * rtl9602c_svlan_setup() - recognise S-tags on the SFP and PON ports
 * @p: the switch
 *
 * The SFP and PON ports become S-VLAN uplink ports, so that the classifier
 * sees the S-tag of a frame either takes in and a VLAN rule can match and
 * change it. Every frame without an S-tag joins S-VLAN
 * RTL9602C_SVLAN_PVID_NONE, of which every port is an untagged member: by an
 * uplink port through RTL9602C_SVLAN_POLICY_UNTAGGED_JOIN, by the others
 * through their S-VLAN PVID. So such a frame leaves as it came, and a frame
 * with an S-tag leaves with it unless a rule changes it, both ways (all
 * measured). Template RTL9602C_CLS_TMPL_C matches the VID and priority of
 * the C-tag, as out of reset, and RTL9602C_CLS_TMPL_S those of the S-tag.
 *
 * Context: setup, after the table self-test, which leaves the VLAN entry of
 * RTL9602C_SVLAN_PVID_NONE alone.
 */
void rtl9602c_svlan_setup(struct rtl9602c_priv *p)
{
	unsigned int port;

	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		rtl9602c_vlan_set_member(p, RTL9602C_SVLAN_PVID_NONE, port,
					 true, true);
		rtl9602c_rmw(p, RTL9602C_SVLAN_PVID(port),
			     RTL9602C_SVLAN_PVID_MASK <<
			     RTL9602C_SVLAN_PVID_SHIFT(port),
			     RTL9602C_SVLAN_PVID_NONE <<
			     RTL9602C_SVLAN_PVID_SHIFT(port));
	}
	rtl9602c_rmw(p, RTL9602C_SVLAN_POLICY, RTL9602C_SVLAN_POLICY_UNTAGGED_ACT,
		     FIELD_PREP(RTL9602C_SVLAN_POLICY_UNTAGGED_ACT,
				RTL9602C_SVLAN_POLICY_UNTAGGED_JOIN));
	rtl9602c_w(p, RTL9602C_SVLAN_UPLINK_PORTS,
		   BIT(RTL9602C_PORT_SFP) | BIT(RTL9602C_PORT_PON));

	rtl9602c_w(p, RTL9602C_CLS_TMPL_CFG(RTL9602C_CLS_TMPL_C),
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_US_VID, RTL9602C_CLS_TMPL_CFG_TAG_C) |
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_US_PRI, RTL9602C_CLS_TMPL_CFG_TAG_C) |
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_DS_VID, RTL9602C_CLS_TMPL_CFG_TAG_C) |
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_DS_PRI, RTL9602C_CLS_TMPL_CFG_TAG_C));
	rtl9602c_w(p, RTL9602C_CLS_TMPL_CFG(RTL9602C_CLS_TMPL_S),
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_US_VID, RTL9602C_CLS_TMPL_CFG_TAG_S) |
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_US_PRI, RTL9602C_CLS_TMPL_CFG_TAG_S) |
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_DS_VID, RTL9602C_CLS_TMPL_CFG_TAG_S) |
		   FIELD_PREP(RTL9602C_CLS_TMPL_CFG_DS_PRI, RTL9602C_CLS_TMPL_CFG_TAG_S));
}

/*
 * Each port follows one tbf and one ets, by handle. tc replaces a qdisc under a
 * new handle by creating the new one before destroying the old, so a request
 * naming any other handle than the followed one must change nothing.
 */
static bool rtl9602c_qdisc_followed(u32 held, u32 handle)
{
	return held && held == handle;
}

/*
 * tbf at the root sets the port's egress rate; burst and latency are not
 * programmed, no bucket depth per port being known. The per-queue limiter
 * counts packets, so a byte rate for one queue cannot be expressed, and a tbf
 * in an ets band is refused rather than applied to the whole port. Statistics
 * succeed for the followed tbf only, which is what makes tc report it
 * offloaded; the counters stay the software qdisc's.
 */
static int rtl9602c_setup_tbf(struct rtl9602c_priv *p, int port,
			      struct tc_tbf_qopt_offload *qopt)
{
	struct rtl9602c_egress *e = &p->egress[port];
	u64 rate;

	switch (qopt->command) {
	case TC_TBF_REPLACE:
		if (qopt->parent != TC_H_ROOT)
			return -EOPNOTSUPP;
		rate = div_u64(qopt->replace_params.rate.rate_bytes_ps,
			       RTL9602C_SHAPE_PORT_CFG_RATE_STEP);
		if (!rate || rate > RTL9602C_SHAPE_PORT_CFG_RATE_MAX) {
			pr_err("port %d: tbf rate out of range; this shaper covers %u..%u bytes/s in steps of %u\n",
			       port, RTL9602C_SHAPE_PORT_CFG_RATE_STEP,
			       RTL9602C_SHAPE_PORT_CFG_RATE_MAX * RTL9602C_SHAPE_PORT_CFG_RATE_STEP,
			       RTL9602C_SHAPE_PORT_CFG_RATE_STEP);

			/*
			 * A failed replace of the followed tbf leaves the port
			 * unlimited.
			 */
			if (rtl9602c_qdisc_followed(e->tbf, qopt->handle)) {
				rtl9602c_egress_shaper(p, port,
						       RTL9602C_SHAPE_PORT_CFG_RATE_MAX);
				e->tbf = 0;
			}
			return -ERANGE;
		}
		rtl9602c_egress_shaper(p, port, (u32)rate);
		e->tbf = qopt->handle;
		return 0;
	case TC_TBF_DESTROY:
		if (!rtl9602c_qdisc_followed(e->tbf, qopt->handle))
			return -EOPNOTSUPP;
		rtl9602c_egress_shaper(p, port, RTL9602C_SHAPE_PORT_CFG_RATE_MAX);
		e->tbf = 0;
		return 0;
	case TC_TBF_STATS:
		return rtl9602c_qdisc_followed(e->tbf, qopt->handle) ?
		       0 : -EOPNOTSUPP;
	case TC_TBF_GRAFT:
		/* The followed ets, grafted under the followed tbf. */
		return rtl9602c_qdisc_followed(e->tbf, qopt->handle) &&
		       rtl9602c_qdisc_followed(e->ets, qopt->child_handle) ?
		       0 : -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * tc band b is queue 7 - b: tc serves band 0 first, the switch its highest
 * queue first.
 */
static unsigned int rtl9602c_ets_queue(unsigned int band)
{
	return RTL9602C_QUEUES - 1 - band;
}

/*
 * Every queue strict at weight 1, and the priority-to-queue word saved at the
 * first ets put back.
 */
static void rtl9602c_ets_reset(struct rtl9602c_priv *p, int port)
{
	struct rtl9602c_egress *e = &p->egress[port];
	unsigned int q;

	for (q = 0; q < RTL9602C_QUEUES; q++)
		rtl9602c_wfq_weight(p, port, q, 1);
	rtl9602c_w(p, RTL9602C_SCHED_WFQ_QUEUES + 4 * port, 0);
	if (e->qid_saved)
		rtl9602c_w(p, RTL9602C_QOS_PRIO_TO_QUEUE + 4 * port,
			   e->qid_reset);
	e->ets = 0;
}

/*
 * ets at the root, or under the followed tbf. A band with quantum 0 is a strict
 * queue; any other is weighted, at its quantum in scheduler tokens, at least
 * one. The priomap is written into the port's priority-to-queue word; an entry
 * naming a band beyond those asked for is left alone. A qdisc inside a band is
 * not offloaded.
 */
static int rtl9602c_setup_ets(struct rtl9602c_priv *p, int port,
			      struct tc_ets_qopt_offload *qopt)
{
	struct tc_ets_qopt_offload_replace_params *r = &qopt->replace_params;
	struct rtl9602c_egress *e = &p->egress[port];
	u32 weight[RTL9602C_QUEUES] = {};
	unsigned int i, q, bytes_per_token;
	u32 type = 0;
	int err;

	switch (qopt->command) {
	case TC_ETS_REPLACE:
		break;
	case TC_ETS_DESTROY:
		if (!rtl9602c_qdisc_followed(e->ets, qopt->handle))
			return -EOPNOTSUPP;
		rtl9602c_ets_reset(p, port);
		return 0;
	case TC_ETS_STATS:
		return rtl9602c_qdisc_followed(e->ets, qopt->handle) ?
		       0 : -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}

	/*
	 * Under the tbf, the parent is the tbf's one class (1:1 under a tbf
	 * 1:), the usual form, or the tbf itself (1:).
	 */
	if (qopt->parent != TC_H_ROOT &&
	    !rtl9602c_qdisc_followed(e->tbf, TC_H_MAJ(qopt->parent)))
		return -EOPNOTSUPP;

	if (r->bands > RTL9602C_QUEUES) {
		pr_err("port %d: %u bands asked for, this port has %u queues\n",
		       port, r->bands, RTL9602C_QUEUES);
		err = -ERANGE;
		goto refuse;
	}

	/* The bytes one weight unit stands for; it reads 66 on this chip. */
	bytes_per_token = FIELD_GET(RTL9602C_SCHED_TOKEN_SIZE_BYTES,
				    rtl9602c_r(p, RTL9602C_SCHED_TOKEN_SIZE));
	if (!bytes_per_token) {
		pr_err("port %d: the scheduler reports zero bytes per token; refusing to divide by it\n",
		       port);
		err = -EIO;
		goto refuse;
	}

	for (i = 0; i < r->bands; i++) {
		u32 limit;

		q = rtl9602c_ets_queue(i);
		limit = q == 0 ? RTL9602C_SCHED_WEIGHT_Q0_MAX : RTL9602C_SCHED_WEIGHT_Q1_7_MAX;
		if (!r->quanta[i]) {
			weight[q] = 1;
			continue;
		}
		weight[q] = max(r->quanta[i] / bytes_per_token, 1u);
		if (weight[q] > limit) {
			pr_err("port %d: band %u wants quantum %u, which is %u tokens of %u bytes and the field holds %u\n",
			       port, i, r->quanta[i], weight[q],
			       bytes_per_token, limit);
			err = -ERANGE;
			goto refuse;
		}
		type |= BIT(q);
	}

	/*
	 * Saved the first time, to be put back when the ets goes. Setup does
	 * not reset the word, so after a warm restart it can be one an earlier
	 * ets left.
	 */
	if (!e->qid_saved) {
		e->qid_reset = rtl9602c_r(p, RTL9602C_QOS_PRIO_TO_QUEUE +
					  4 * port);
		e->qid_saved = true;
	}

	for (q = 0; q < RTL9602C_QUEUES; q++)
		rtl9602c_wfq_weight(p, port, q, weight[q] ?: 1);
	rtl9602c_w(p, RTL9602C_SCHED_WFQ_QUEUES + 4 * port, type);

	/* Weights count bytes, as a quantum does. */
	rtl9602c_rmw(p, RTL9602C_SCHED_CTRL, RTL9602C_SCHED_CTRL_WEIGHT_PKTS, 0);

	for (i = 0; i < RTL9602C_QUEUES; i++) {
		u32 off = RTL9602C_QOS_PRIO_TO_QUEUE + 4 * port;

		if (r->priomap[i] >= r->bands)
			continue;
		rtl9602c_rmw(p, off, 7u << (3 * i),
			     (u32)rtl9602c_ets_queue(r->priomap[i]) << (3 * i));
	}
	e->ets = qopt->handle;
	return 0;

refuse:
	/* A failed replace of the followed ets resets the port. */
	if (rtl9602c_qdisc_followed(e->ets, qopt->handle))
		rtl9602c_ets_reset(p, port);
	return err;
}

/*
 * netfilter's flowtable offload. netfilter offers each direction of a flow as a
 * flower rule, once to each device of the flowtable. The L3/NAT engine carries
 * the source-NAT direction and translates the replies back from its entries, so
 * the reply direction's rule, which carries no source translation, is declined
 * with nothing left to program.
 */
static void rtl9602c_flow_dump_actions(struct rtl9602c_priv *p,
				       struct flow_rule *rule)
{
	struct flow_action_entry *act;
	int i;

	if (!rule->action.num_entries) {
		pr_debug("<flow> no actions\n");
		return;
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			pr_debug("<flow> act[%d] MANGLE htype %u offset %u mask %08x val %08x\n",
				 i, act->mangle.htype, act->mangle.offset,
				 act->mangle.mask, act->mangle.val);
			break;
		case FLOW_ACTION_REDIRECT:
			pr_debug("<flow> act[%d] REDIRECT dev %s\n", i,
				 act->dev ? netdev_name(act->dev) : "(null)");
			break;
		case FLOW_ACTION_CSUM:
			pr_debug("<flow> act[%d] CSUM flags %x\n", i,
				 act->csum_flags);
			break;
		case FLOW_ACTION_VLAN_PUSH:
			pr_debug("<flow> act[%d] VLAN_PUSH vid %u proto %04x\n",
				 i, act->vlan.vid, ntohs(act->vlan.proto));
			break;
		default:
			pr_debug("<flow> act[%d] id %u (unhandled)\n", i,
				 act->id);
			break;
		}
	}
}

/*
 * A flow's PPPoE egress, from its rule: the session, -1 for none; the
 * concentrator's and the egress port's Ethernet addresses; and the device the
 * flow leaves by.
 */
struct rtl9602c_flow_pppoe {
	int			sid;
	u8			dmac[ETH_ALEN];
	u8			smac[ETH_ALEN];
	struct net_device	*dev;
};

/* The L3/NAT engine's side, below. */
static int rtl9602c_l34_flow_add(struct rtl9602c_priv *p, unsigned long cookie,
				 u32 lip, u16 lport, u32 rip, u16 rport,
				 u32 extip, u16 extport, bool tcp,
				 const struct rtl9602c_flow_pppoe *ppp,
				 int port);
static int rtl9602c_l34_flow_del(struct rtl9602c_priv *p, unsigned long cookie);
static int rtl9602c_l34_flow_stats(struct rtl9602c_priv *p,
				   struct flow_cls_offload *cls);
static void rtl9602c_l34_flows_release_port(struct rtl9602c_priv *p, int port);
static void rtl9602c_l34_nh_state(struct rtl9602c_priv *p, int ifindex,
				  __be32 gw, bool down);
static int rtl9602c_flow_pppoe_of(struct flow_rule *rule);
static int rtl9602c_flow_eth_of(struct flow_rule *rule, u8 *dmac, u8 *smac);
static bool rtl9602c_flow_has(struct flow_rule *rule, enum flow_action_id id);
static bool rtl9602c_l34_egress_ok(struct rtl9602c_priv *p,
				   struct net_device *dev);
static struct net_device *rtl9602c_flow_dev_of(struct flow_rule *rule);
static int rtl9602c_extip_get(struct rtl9602c_priv *p, u32 addr);
static void rtl9602c_extip_put(struct rtl9602c_priv *p, u32 addr);

/*
 * The source translation of a flowtable rule: the IPv4 source address as a
 * mangle at offsetof(struct iphdr, saddr), and the TCP or UDP source port as a
 * mangle at offset 0 with mask 0x0000ffff, the port in the value's high half.
 * The values are header bytes in network order, which is host order on this
 * big-endian CPU. Returns 0 with both, 1 with the address alone, and
 * -EOPNOTSUPP with no address: not a source-NAT flow, but the reply direction
 * or plain routing.
 */
static int rtl9602c_flow_snat_of(struct flow_rule *rule, u8 ip_proto,
				 u32 *extip, u16 *extport)
{
	bool have_ip = false, have_port = false;
	struct flow_action_entry *act;
	int i;

	flow_action_for_each(i, act, &rule->action) {
		if (act->id != FLOW_ACTION_MANGLE)
			continue;

		if (act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_IP4 &&
		    act->mangle.offset == offsetof(struct iphdr, saddr)) {
			*extip = act->mangle.val;
			have_ip = true;
		} else if ((act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_TCP ||
			    act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_UDP) &&
			   act->mangle.offset == 0 &&
			   act->mangle.mask == 0x0000ffff) {
			*extport = act->mangle.val >> 16;
			have_port = true;
		}
	}

	if (!have_ip)
		return -EOPNOTSUPP;

	if (!have_port)
		*extport = 0;
	return have_port ? 0 : 1;
}

/*
 * One flowtable command. A flow is programmed only if the engine can carry it
 * whole: IPv4 TCP or UDP with ports; a source translation; over PPPoE, the
 * egress device and both Ethernet addresses; out of a GEM interface, the routed
 * flow with its upstream ready. A PPPoE flow that also pushes a VLAN is
 * declined, that case not being verified. Whatever is declined answers
 * -EOPNOTSUPP and stays in software: a 0 would make netfilter stop forwarding
 * the flow there.
 */
static int rtl9602c_flow_offload_cmd(struct dsa_port *dp,
				     struct flow_cls_offload *cls)
{
	struct rtl9602c_priv *p = dp->ds->priv;
	struct flow_rule *rule;
	struct flow_match_ipv4_addrs ip;
	struct flow_match_ports ports;
	struct flow_match_basic basic;
	struct rtl9602c_flow_pppoe ppp = { .sid = -1 };
	struct net_device *egress;
	u32 extip = 0;
	u16 extport = 0;
	int err;

	switch (cls->command) {
	case FLOW_CLS_DESTROY:
		pr_debug("<flow> destroy cookie %lx on port %d\n", cls->cookie,
			 dp->index);
		return rtl9602c_l34_flow_del(p, cls->cookie);
	case FLOW_CLS_STATS:
		return rtl9602c_l34_flow_stats(p, cls);
	case FLOW_CLS_REPLACE:
		break;
	default:
		return -EOPNOTSUPP;
	}

	rule = flow_cls_offload_flow_rule(cls);

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		pr_debug("<flow> replace cookie %lx, no basic key\n",
			 cls->cookie);
		return -EOPNOTSUPP;
	}
	flow_rule_match_basic(rule, &basic);

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		pr_debug("<flow> replace cookie %lx port %d proto %u, not IPv4\n",
			 cls->cookie, dp->index, basic.key->ip_proto);
		return -EOPNOTSUPP;
	}
	flow_rule_match_ipv4_addrs(rule, &ip);

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		pr_debug("<flow> replace cookie %lx port %d proto %u %pI4 -> %pI4, no L4 ports\n",
			 cls->cookie, dp->index, basic.key->ip_proto,
			 &ip.key->src, &ip.key->dst);
		return -EOPNOTSUPP;
	}
	flow_rule_match_ports(rule, &ports);

	rtl9602c_flow_dump_actions(p, rule);

	if (!p->l34_up) {
		pr_debug("<flow> cookie %lx declined, the L3 engine is not up\n",
			 cls->cookie);
		return -EOPNOTSUPP;
	}

	if (basic.key->ip_proto != IPPROTO_TCP &&
	    basic.key->ip_proto != IPPROTO_UDP)
		return -EOPNOTSUPP;

	err = rtl9602c_flow_snat_of(rule, basic.key->ip_proto, &extip,
				    &extport);
	if (err < 0) {
		pr_debug("<flow> cookie %lx has no source translation this engine can express\n",
			 cls->cookie);
		return err;
	}
	/* The address alone is translated: the source port is kept. */
	if (err == 1)
		extport = ntohs(ports.key->src);

	ppp.sid = rtl9602c_flow_pppoe_of(rule);
	if (ppp.sid >= 0) {
		pr_debug("<flow> cookie %lx leaves over PPPoE session %d\n",
			 cls->cookie, ppp.sid);

		ppp.dev = rtl9602c_flow_dev_of(rule);
		if (!ppp.dev ||
		    rtl9602c_flow_eth_of(rule, ppp.dmac, ppp.smac)) {
			pr_debug("<flow> cookie %lx: session %d has no Ethernet egress this driver can read, declining\n",
				 cls->cookie, ppp.sid);
			return -EOPNOTSUPP;
		}
		if (rtl9602c_flow_has(rule, FLOW_ACTION_VLAN_PUSH)) {
			pr_debug("<flow> cookie %lx: PPPoE inside a VLAN is not measured, declining\n",
				 cls->cookie);
			return -EOPNOTSUPP;
		}
	}

	egress = rtl9602c_flow_dev_of(rule);
	if (egress && !rtl9602c_l34_egress_ok(p, egress)) {
		pr_debug("<flow> cookie %lx: %s has no upstream stream for routed frames, declining\n",
			 cls->cookie, egress->name);
		return -EOPNOTSUPP;
	}

	err = rtl9602c_l34_flow_add(p, cls->cookie,
				    ntohl(ip.key->src), ntohs(ports.key->src),
				    ntohl(ip.key->dst), ntohs(ports.key->dst),
				    extip, extport,
				    basic.key->ip_proto == IPPROTO_TCP, &ppp,
				    dp->index);
	if (err) {
		pr_debug("<flow> cookie %lx not programmed: %d\n", cls->cookie,
			 err);
		return err;
	}

	pr_debug("<flow> cookie %lx port %d proto %u %pI4:%u -> %pI4:%u, source now %pI4:%u, IN HARDWARE\n",
		 cls->cookie, dp->index, basic.key->ip_proto, &ip.key->src,
		 ntohs(ports.key->src), &ip.key->dst, ntohs(ports.key->dst),
		 &extip, extport);
	return 0;
}

static int rtl9602c_setup_ft_block_cb(enum tc_setup_type type, void *type_data,
				      void *cb_priv)
{
	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return rtl9602c_flow_offload_cmd(cb_priv, type_data);
}

static LIST_HEAD(rtl9602c_ft_block_cb_list);

/*
 * A block going away frees the flows no remaining block owns: netfilter unbinds
 * a flowtable's blocks before it tears its flows down, so no destroy comes for
 * flows still in hardware when a ruleset is flushed.
 */
static void rtl9602c_ft_block_release(void *cb_priv)
{
	struct dsa_port *dp = cb_priv;
	struct rtl9602c_priv *p = dp->ds->priv;

	rtl9602c_l34_flows_release_port(p, dp->index);
}

/*
 * One flow block per user port, keyed on its dsa_port and counted across binds.
 * A GEM interface passes its flowtable to the PON port below it.
 */
static int rtl9602c_setup_ft_block(struct dsa_switch *ds, int port,
				   struct flow_block_offload *f)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	flow_setup_cb_t *cb = rtl9602c_setup_ft_block_cb;
	struct flow_block_cb *block_cb;

	f->driver_block_list = &rtl9602c_ft_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dp);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			pr_debug("<flow> block for port %d bound again\n", port);
			return 0;
		}
		block_cb = flow_block_cb_alloc(cb, dp, dp,
					       rtl9602c_ft_block_release);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &rtl9602c_ft_block_cb_list);
		pr_debug("<flow> block for port %d bound\n", port);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dp);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
			pr_debug("<flow> block for port %d unbound\n", port);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * rtl9602c_port_setup_tc() - offload a qdisc or a flowtable
 * @ds: the switch
 * @port: the port
 * @type: %TC_SETUP_QDISC_TBF, %TC_SETUP_QDISC_ETS or %TC_SETUP_FT
 * @type_data: the request
 *
 * DSA offers %TC_SETUP_FT to the switch driver before the conduit, which is how
 * netfilter's flowtable reaches the L3/NAT engine.
 *
 * Return: 0, or a negative error code; -EOPNOTSUPP for another type or a
 * request that is not offloaded.
 */
int rtl9602c_port_setup_tc(struct dsa_switch *ds, int port,
			   enum tc_setup_type type, void *type_data)
{
	struct rtl9602c_priv *p = ds->priv;

	switch (type) {
	case TC_SETUP_QDISC_TBF:
		return rtl9602c_setup_tbf(p, port, type_data);
	case TC_SETUP_QDISC_ETS:
		return rtl9602c_setup_ets(p, port, type_data);
	case TC_SETUP_FT:
		return rtl9602c_setup_ft_block(ds, port, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * The ACL: 96 ternary rules in the table engine. A rule compares its template
 * number, the ingress port and eight 16-bit slots, and its template says which
 * field each slot holds. Seven templates are used: a rule naming the eighth,
 * which no register holds, matches nothing. The only action class applied is
 * the forwarding decision, drop or trap. Rules judge the frames entering a
 * port, not those the CPU sends out of it; rules on ports 0 and 2 are
 * verified. Of two rules that match a frame the lower entry decides, so the
 * rules are kept in entries ordered by tc's priority (rtl9602c_acl_place()).
 * Setup clears the ACL (rtl9602c_filter_qos_reset()).
 */

/*
 * Slot @slot of template @tmpl: a 7-bit field code, four codes to a word, at
 * bit 7(n mod 4) of word n/4 for n = 8 * @tmpl + @slot. One slot is written at
 * a time, read-modify-write.
 */
static void rtl9602c_acl_slot_write(struct rtl9602c_priv *p, unsigned int tmpl,
				    unsigned int slot, u8 sel)
{
	unsigned int lin = tmpl * RTL9602C_ACL_TMPL_SLOTS + slot;
	u32 off = RTL9602C_ACL_TMPL_FIELD + 4 * (lin / 4);
	unsigned int sh = 7 * (lin % 4);

	rtl9602c_rmw(p, off, 0x7fu << sh, (u32)sel << sh);
}

/*
 * Find a template in use with a slot for each field of @want, in any order, the
 * rule masking the rest; or else fill the first unused template, its spare
 * slots comparing nothing. @slot_of gets the slot of each field. A template is
 * counted by the rules using it. Returns the template, or -ENOSPC.
 */
static int rtl9602c_acl_template(struct rtl9602c_priv *p, const u8 *want,
				 unsigned int n, u8 *slot_of)
{
	unsigned int t, i, j, free_t = RTL9602C_ACL_TMPL_COUNT;

	lockdep_assert_held(&p->acl_lock);

	for (t = 0; t < RTL9602C_ACL_TMPL_COUNT; t++) {
		bool all = true;

		if (!p->acl_tmpl_refs[t]) {
			if (free_t == RTL9602C_ACL_TMPL_COUNT)
				free_t = t;
			continue;
		}
		for (i = 0; i < n && all; i++) {
			all = false;
			for (j = 0; j < RTL9602C_ACL_TMPL_SLOTS; j++)
				if (p->acl_tmpl[t][j] == want[i]) {
					slot_of[i] = j;
					all = true;
					break;
				}
		}
		if (all)
			return t;
	}

	if (free_t == RTL9602C_ACL_TMPL_COUNT)
		return -ENOSPC;

	for (j = 0; j < RTL9602C_ACL_TMPL_SLOTS; j++) {
		u8 sel = j < n ? want[j] : RTL9602C_ACL_TMPL_FIELD_NONE;

		p->acl_tmpl[free_t][j] = sel;
		rtl9602c_acl_slot_write(p, free_t, j, sel);
	}
	for (i = 0; i < n; i++)
		slot_of[i] = i;
	return free_t;
}

static void rtl9602c_acl_bits(u32 *w, unsigned int lsb, unsigned int len,
			      u32 val)
{
	unsigned int word = lsb / 32, sh = lsb % 32;

	w[word] |= (val & (len == 32 ? ~0u : (BIT(len) - 1))) << sh;
	if (sh + len > 32)
		w[word + 1] |= val >> (32 - sh);
}

/* The ones plane takes care and value, the zeros plane care and not value. */
static void rtl9602c_acl_pair(u32 *ones, u32 *zeroes, unsigned int lsb,
			      unsigned int len, u32 val, u32 care)
{
	rtl9602c_acl_bits(ones, lsb, len, care & val);
	rtl9602c_acl_bits(zeroes, lsb, len, care & ~val);
}

/*
 * Turn ACL lookup on @port on or off, together with its default verdict of
 * permit for frames no rule matches: lookup without that bit drops every such
 * frame. So the permit goes on before the lookup and off after it, and no
 * frame meets a lookup without it.
 */
static void rtl9602c_acl_port_enable(struct rtl9602c_priv *p, int port,
				     bool on)
{
	if (on) {
		rtl9602c_rmw(p, RTL9602C_ACL_DEFAULT_PERMIT, 0, BIT(port));
		rtl9602c_rmw(p, RTL9602C_ACL_PORT_EN, 0, BIT(port));
	} else {
		rtl9602c_rmw(p, RTL9602C_ACL_PORT_EN, BIT(port), 0);
		rtl9602c_rmw(p, RTL9602C_ACL_DEFAULT_PERMIT, BIT(port), 0);
	}
}

/* The slots a rule asks for: field code, value and care of each. */
struct rtl9602c_acl_match {
	u8	sel[RTL9602C_ACL_TMPL_SLOTS];
	u16	val[RTL9602C_ACL_TMPL_SLOTS];
	u16	care[RTL9602C_ACL_TMPL_SLOTS];
	unsigned int n;
};

/* @m->n counts on past the slots, so that an oversized rule shows. */
static void rtl9602c_acl_push(struct rtl9602c_acl_match *m, u8 sel, u16 val,
			      u16 care)
{
	if (m->n < RTL9602C_ACL_TMPL_SLOTS) {
		m->sel[m->n] = sel;
		m->val[m->n] = val;
		m->care[m->n] = care;
	}
	m->n++;
}

/* Where each part of an entry sits in the words of struct rtl9602c_priv acl[]. */
#define RTL9602C_ACL_W_MASK	0
#define RTL9602C_ACL_W_DATA	RTL9602C_ACL_RULE_WORDS
#define RTL9602C_ACL_W_ACT	(2 * RTL9602C_ACL_RULE_WORDS)

/*
 * Compose rule @w: the template, compared whole; the ingress port bits, of
 * which only @port's is compared; the slots; the valid bit, in the ones plane;
 * and the action. Only the forwarding class is applied:
 * RTL9602C_ACL_ACT_FWD_REDIRECT with no port drops the frame,
 * RTL9602C_ACL_ACT_FWD_TRAP sends it to the CPU, and
 * RTL9602C_ACL_ACT_FWD_MIRROR to the CPU port copies it there.
 */
static void rtl9602c_acl_compose(u32 *w, int port, unsigned int tmpl,
				 const u8 *slot_of,
				 const struct rtl9602c_acl_match *m,
				 unsigned int fwdact, unsigned int fwd_pmsk)
{
	u32 *mask = &w[RTL9602C_ACL_W_MASK], *data = &w[RTL9602C_ACL_W_DATA];
	u32 *act = &w[RTL9602C_ACL_W_ACT];
	unsigned int i;

	rtl9602c_acl_pair(data, mask, 0, 3, tmpl, 7);
	rtl9602c_acl_pair(data, mask, 10, 6, BIT(port), BIT(port));

	for (i = 0; i < m->n; i++) {
		unsigned int lsb = RTL9602C_ACL_RULE_SLOT0_LSB + 16 * slot_of[i];

		rtl9602c_acl_pair(data, mask, lsb, 16, m->val[i], m->care[i]);
	}
	rtl9602c_acl_bits(data, RTL9602C_ACL_RULE_VALID_BIT, 1, 1);

	rtl9602c_acl_bits(act, RTL9602C_ACL_ACT_FWD_LSB, 2, fwdact);
	rtl9602c_acl_bits(act, RTL9602C_ACL_ACT_FWD_PORTS_LSB, 4, fwd_pmsk);
}

/*
 * Write entry @idx from words @w. The zeros plane goes first, then the action,
 * the action classes and last the ones plane, so that no frame meets a
 * half-written rule; an entry in use is retired first.
 */
static int rtl9602c_acl_put(struct rtl9602c_priv *p, unsigned int idx,
			    u32 *w)
{
	u32 zero[RTL9602C_ACL_RULE_WORDS] = {};
	int err;

	err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_ACL_RULE,
				   idx + RTL9602C_ACL_RULE_ONES_PLANE, zero,
				   RTL9602C_ACL_RULE_WORDS);
	if (err)
		return err;
	err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_ACL_RULE, idx,
				   &w[RTL9602C_ACL_W_MASK], RTL9602C_ACL_RULE_WORDS);
	if (err)
		return err;
	err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_ACL_ACT, idx,
				   &w[RTL9602C_ACL_W_ACT], RTL9602C_ACL_ACT_WORDS);
	if (err)
		return err;

	rtl9602c_w(p, RTL9602C_ACL_ACT_EN + 4 * idx, RTL9602C_ACL_ACT_EN_FWD);

	return rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_ACL_RULE,
				    idx + RTL9602C_ACL_RULE_ONES_PLANE,
				    &w[RTL9602C_ACL_W_DATA], RTL9602C_ACL_RULE_WORDS);
}

/*
 * Retire entry @idx: its ones plane zeroed, which clears the valid bit, and
 * its action classes back to their reset value, never 0: a zero chains an
 * entry to the one before it, so that a rule there would match only together
 * with this entry's pattern.
 */
static int rtl9602c_acl_retire(struct rtl9602c_priv *p, unsigned int idx)
{
	u32 zero[RTL9602C_ACL_RULE_WORDS] = {};
	int err;

	err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_ACL_RULE,
				   idx + RTL9602C_ACL_RULE_ONES_PLANE, zero,
				   RTL9602C_ACL_RULE_WORDS);
	rtl9602c_w(p, RTL9602C_ACL_ACT_EN + 4 * idx, RTL9602C_ACL_ACT_EN_DEFAULT);
	return err;
}

/*
 * The entry for a new rule of tc priority @prio. Of several matching rules the
 * lowest entry applies (measured on this board), and tc evaluates the lower
 * priority value first, so the rules are kept in entries ordered by priority,
 * a new one after those of its own value: below every rule of a larger value
 * and above every other. When no free entry lies between them, the rules of
 * larger values move up one entry each into the first free entry above them,
 * the topmost first, so that every rule stays in at least one entry
 * throughout; the entry left over is retired. Returns the entry, -ENOSPC when
 * the ACL is full, or the table's error.
 */
static int rtl9602c_acl_place(struct rtl9602c_priv *p, u16 prio)
{
	unsigned int hi = RTL9602C_ACL_ENTRIES, idx, free;
	int lo = -1, err;

	for_each_set_bit(idx, p->acl_used, RTL9602C_ACL_ENTRIES) {
		if (p->acl[idx].prio <= prio)
			lo = idx;
		else if (hi == RTL9602C_ACL_ENTRIES)
			hi = idx;
	}

	idx = find_next_zero_bit(p->acl_used, RTL9602C_ACL_ENTRIES, lo + 1);
	if (idx < hi)
		return idx;

	free = find_next_zero_bit(p->acl_used, RTL9602C_ACL_ENTRIES, hi);
	if (free >= RTL9602C_ACL_ENTRIES)
		return -ENOSPC;

	for (idx = free; idx > hi; idx--) {
		err = rtl9602c_acl_put(p, idx, p->acl[idx - 1].words);
		if (err)
			return err;
		p->acl[idx] = p->acl[idx - 1];
		set_bit(idx, p->acl_used);
	}
	clear_bit(hi, p->acl_used);
	err = rtl9602c_acl_retire(p, hi);
	return err ? err : hi;
}

/*
 * Write the rule of @m on @port into a new entry, kept in priority order
 * (rtl9602c_acl_place()), and turn on the port's lookup. @extack, if not NULL,
 * gets the reason of a failure.
 */
static int rtl9602c_acl_insert(struct rtl9602c_priv *p, int port,
			       const struct rtl9602c_acl_match *m, u16 prio,
			       unsigned long cookie, unsigned int fwdact,
			       unsigned int fwd_pmsk,
			       struct netlink_ext_ack *extack)
{
	u8 slot_of[RTL9602C_ACL_TMPL_SLOTS];
	unsigned int idx;
	int tmpl, err;

	lockdep_assert_held(&p->acl_lock);

	tmpl = rtl9602c_acl_template(p, m->sel, m->n, slot_of);
	if (tmpl < 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "no template covers these fields and all seven are in use");
		return tmpl;
	}

	err = rtl9602c_acl_place(p, prio);
	if (err == -ENOSPC) {
		NL_SET_ERR_MSG_MOD(extack, "all 96 ACL rules are in use");
		return err;
	}
	if (err < 0) {
		NL_SET_ERR_MSG_MOD(extack, "the ACL table did not answer");
		return err;
	}
	idx = err;

	memset(p->acl[idx].words, 0, sizeof(p->acl[idx].words));
	rtl9602c_acl_compose(p->acl[idx].words, port, tmpl, slot_of, m,
			     fwdact, fwd_pmsk);
	err = rtl9602c_acl_put(p, idx, p->acl[idx].words);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "the ACL table did not answer");
		return err;
	}

	p->acl[idx].cookie = cookie;
	p->acl[idx].tmpl = tmpl;
	p->acl[idx].port = port;
	p->acl[idx].prio = prio;
	p->acl_tmpl_refs[tmpl]++;
	set_bit(idx, p->acl_used);
	rtl9602c_acl_port_enable(p, port, true);
	return 0;
}

/*
 * A flower rule whose one action is drop or trap, as one ACL rule. Each field
 * it matches is cut into 16-bit slots, and a slot with a zero mask is left out.
 * L4 ports are compared through the field selectors setup points at the start
 * of the IP payload, which are taken to cut their window whatever the frame
 * carries (not verified): a port match is taken only with an exact IPv4
 * ethertype and IP protocol.
 */
static int rtl9602c_acl_replace(struct rtl9602c_priv *p, int port,
				struct flow_cls_offload *cls,
				unsigned int fwdact, unsigned int fwd_pmsk)
{
	struct netlink_ext_ack *extack = cls->common.extack;
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	struct rtl9602c_acl_match m = {};
	bool ipv4 = false, ipv6 = false, ipproto = false;
	int err;

	if (rule->match.dissector->used_keys &
	    ~(BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_BASIC) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_PORTS))) {
		NL_SET_ERR_MSG_MOD(extack,
				   "this ACL matches addresses, ethertype, IP protocol, IPv4 addresses and L4 ports");
		return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs eth;
		unsigned int i;

		flow_rule_match_eth_addrs(rule, &eth);
		/* Slot i holds address bits 16i + 15 to 16i. */
		for (i = 0; i < 3; i++) {
			u16 v = get_unaligned_be16(eth.key->dst + 4 - 2 * i);
			u16 c = get_unaligned_be16(eth.mask->dst + 4 - 2 * i);

			if (c)
				rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_DMAC0 + i,
						  v, c);
		}
		for (i = 0; i < 3; i++) {
			u16 v = get_unaligned_be16(eth.key->src + 4 - 2 * i);
			u16 c = get_unaligned_be16(eth.mask->src + 4 - 2 * i);

			if (c)
				rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_SMAC0 + i,
						  v, c);
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic basic;

		flow_rule_match_basic(rule, &basic);
		if (basic.mask->n_proto) {
			rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_ETHERTYPE,
					  ntohs(basic.key->n_proto),
					  ntohs(basic.mask->n_proto));
			ipv4 = basic.mask->n_proto == 0xffff &&
			       basic.key->n_proto == htons(ETH_P_IP);
		}

		/*
		 * The IP protocol is the field's low octet; the TOS or traffic
		 * class octet above it is not compared. For IPv6 the field
		 * holds the protocol behind any extension headers, as the
		 * flower match does.
		 */
		if (basic.mask->ip_proto) {
			ipv6 = basic.mask->n_proto == htons(0xffff) &&
			       basic.key->n_proto == htons(ETH_P_IPV6);
			rtl9602c_acl_push(&m, ipv6 ?
					  RTL9602C_ACL_TMPL_FIELD_IPV6_TC_NH :
					  RTL9602C_ACL_TMPL_FIELD_IPV4_TOS_PROTO,
					  basic.key->ip_proto,
					  basic.mask->ip_proto);
			ipproto = basic.mask->ip_proto == 0xff;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control ctrl;

		flow_rule_match_control(rule, &ctrl);
		if (ctrl.key->addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
			NL_SET_ERR_MSG_MOD(extack,
					   "IPv6 addresses are not offloaded; the chip has eight field selectors for them and this driver drives none");
			return -EOPNOTSUPP;
		}
		if (ctrl.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
			goto no_addrs;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs ip;
		u32 v, c;

		flow_rule_match_ipv4_addrs(rule, &ip);
		v = ntohl(ip.key->src);
		c = ntohl(ip.mask->src);
		if (c & 0xffffu)
			rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_IPV4_SRC0, v, c);
		if (c >> 16)
			rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_IPV4_SRC0 + 1,
					  v >> 16, c >> 16);
		v = ntohl(ip.key->dst);
		c = ntohl(ip.mask->dst);
		if (c & 0xffffu)
			rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_IPV4_DST0, v, c);
		if (c >> 16)
			rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_IPV4_DST0 + 1,
					  v >> 16, c >> 16);
	}
no_addrs:
	mutex_lock(&p->acl_lock);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports ports;

		if (!(ipv4 || ipv6) || !ipproto) {
			NL_SET_ERR_MSG_MOD(extack,
					   "an L4 port match needs an exact ethertype of ip or ipv6 and an exact ip_proto: the port window is cut from a fixed offset and would otherwise match bytes in frames that carry no L4 header");
			err = -EOPNOTSUPP;
			goto out;
		}

		flow_rule_match_ports(rule, &ports);
		/* Through the L4 port field selectors set up at probe. */
		if (ports.mask->src)
			rtl9602c_acl_push(&m,
					  RTL9602C_ACL_TMPL_FIELD_USER0 +
					  RTL9602C_FSEL_SPORT,
					  ntohs(ports.key->src),
					  ntohs(ports.mask->src));
		if (ports.mask->dst)
			rtl9602c_acl_push(&m,
					  RTL9602C_ACL_TMPL_FIELD_USER0 +
					  RTL9602C_FSEL_DPORT,
					  ntohs(ports.key->dst),
					  ntohs(ports.mask->dst));
	}

	if (!m.n) {
		NL_SET_ERR_MSG_MOD(extack,
				   "a rule that matches nothing would drop every frame on the port");
		err = -EOPNOTSUPP;
		goto out;
	}

	if (m.n > RTL9602C_ACL_TMPL_SLOTS) {
		pr_debug("<ACL> rule wants %u slots, %u available\n", m.n,
			 RTL9602C_ACL_TMPL_SLOTS);
		NL_SET_ERR_MSG_MOD(extack,
				   "too many fields: a template matches eight 16-bit slots and this rule needs more");
		err = -EOPNOTSUPP;
		goto out;
	}

	err = rtl9602c_acl_insert(p, port, &m, cls->common.prio, cls->cookie,
				  fwdact, fwd_pmsk, extack);
out:
	mutex_unlock(&p->acl_lock);
	return err;
}

/*
 * Multicast snooping. The bridge has to see every IGMP and MLD message to
 * snoop, and a report to a group already in the table goes only to the
 * group's members. So an IGMP or MLD message entering the SFP or the PON port
 * goes on as the switch forwards it, with its VLAN translation and in the GEM
 * stream its VLAN takes, and a copy goes to the CPU
 * (RTL9602C_ACL_ACT_FWD_MIRROR, measured with IGMPv2 and MLDv1 reports). DSA
 * and the PON class mark the copy as forwarded, so the bridge snoops it and
 * forwards nothing again. IGMP is found by its IP protocol. MLD is ICMPv6, of
 * which the rule takes the two types sent to a group's own address, the query
 * (130) and the version 1 report (131), through the field selector at the
 * start of the IP payload; the rest of MLD goes to ff02::1, ff02::2 or ff02::16,
 * which reach the CPU unasked, and no ping or neighbour discovery message is
 * copied. The rules take the largest priority, after every tc rule, so that a
 * tc rule matching these messages decides for them; their cookies are no kernel
 * address, so no tc rule has one.
 */
#define RTL9602C_ACL_SNOOP_PRIO		U16_MAX
#define RTL9602C_ACL_SNOOP_COOKIE(i)	(1UL + (i))

/**
 * rtl9602c_acl_snoop_setup() - copy IGMP and MLD to the CPU for snooping
 * @p: the switch
 *
 * Writes the four rules, IGMP and MLD on the SFP and the PON port, with a
 * kernel that has bridge multicast snooping. Each rule names the EtherType,
 * the IPv4 protocol, the IPv6 protocol and the ICMPv6 type, ignoring what it
 * does not compare, so that the four share one template.
 *
 * Context: setup, after rtl9602c_filter_qos_reset().
 */
void rtl9602c_acl_snoop_setup(struct rtl9602c_priv *p)
{
	static const int ports[] = { RTL9602C_PORT_SFP, RTL9602C_PORT_PON };
	unsigned int i, n = 0;

	if (!IS_ENABLED(CONFIG_BRIDGE_IGMP_SNOOPING))
		return;

	mutex_lock(&p->acl_lock);
	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		struct rtl9602c_acl_match igmp = {}, mld = {};

		rtl9602c_acl_push(&igmp, RTL9602C_ACL_TMPL_FIELD_ETHERTYPE,
				  ETH_P_IP, 0xffff);
		rtl9602c_acl_push(&igmp, RTL9602C_ACL_TMPL_FIELD_IPV4_TOS_PROTO,
				  IPPROTO_IGMP, 0x00ff);
		rtl9602c_acl_push(&igmp, RTL9602C_ACL_TMPL_FIELD_IPV6_TC_NH, 0, 0);
		rtl9602c_acl_push(&igmp, RTL9602C_ACL_TMPL_FIELD_USER0 +
				  RTL9602C_FSEL_SPORT, 0, 0);

		rtl9602c_acl_push(&mld, RTL9602C_ACL_TMPL_FIELD_ETHERTYPE,
				  ETH_P_IPV6, 0xffff);
		rtl9602c_acl_push(&mld, RTL9602C_ACL_TMPL_FIELD_IPV4_TOS_PROTO, 0, 0);
		rtl9602c_acl_push(&mld, RTL9602C_ACL_TMPL_FIELD_IPV6_TC_NH,
				  IPPROTO_ICMPV6, 0x00ff);
		rtl9602c_acl_push(&mld, RTL9602C_ACL_TMPL_FIELD_USER0 +
				  RTL9602C_FSEL_SPORT,
				  ICMPV6_MGM_QUERY << 8, 0xfe00);

		if (rtl9602c_acl_insert(p, ports[i], &igmp,
					RTL9602C_ACL_SNOOP_PRIO,
					RTL9602C_ACL_SNOOP_COOKIE(n++),
					RTL9602C_ACL_ACT_FWD_MIRROR,
					BIT(RTL9602C_PORT_CPU), NULL) ||
		    rtl9602c_acl_insert(p, ports[i], &mld,
					RTL9602C_ACL_SNOOP_PRIO,
					RTL9602C_ACL_SNOOP_COOKIE(n++),
					RTL9602C_ACL_ACT_FWD_MIRROR,
					BIT(RTL9602C_PORT_CPU), NULL))
			pr_err("port %d: no ACL rule for IGMP and MLD; the bridge misses reports to groups in the table\n",
			       ports[i]);
	}
	mutex_unlock(&p->acl_lock);
}

/*
 * Remove the rule with @cookie (see rtl9602c_acl_retire()). The port's lookup
 * is switched off with its last rule.
 */
static int rtl9602c_acl_del(struct rtl9602c_priv *p, unsigned long cookie)
{
	unsigned int idx, other;
	bool last = true;
	int err;

	mutex_lock(&p->acl_lock);
	for_each_set_bit(idx, p->acl_used, RTL9602C_ACL_ENTRIES) {
		if (p->acl[idx].cookie != cookie)
			continue;

		err = rtl9602c_acl_retire(p, idx);
		if (p->acl_tmpl_refs[p->acl[idx].tmpl])
			p->acl_tmpl_refs[p->acl[idx].tmpl]--;
		clear_bit(idx, p->acl_used);

		for_each_set_bit(other, p->acl_used, RTL9602C_ACL_ENTRIES)
			if (p->acl[other].port == p->acl[idx].port)
				last = false;
		if (last)
			rtl9602c_acl_port_enable(p, p->acl[idx].port, false);

		mutex_unlock(&p->acl_lock);
		return err;
	}
	mutex_unlock(&p->acl_lock);
	return -ENOENT;
}

/*
 * An exact ARP ethertype and no VLAN match names the ARP class; the rule's other
 * keys are checked by rtl9602c_storm_rule_check().
 */
static bool rtl9602c_flower_is_arp(struct flow_rule *rule)
{
	struct flow_match_basic basic;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN))
		return false;
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return false;

	flow_rule_match_basic(rule, &basic);
	return basic.mask->n_proto == htons(0xffff) &&
	       basic.key->n_proto == htons(ETH_P_ARP);
}

/*
 * A storm-control rule polices its whole class on its port, so it matches the
 * class and nothing else: the destination address of the broadcast and
 * multicast classes, the EtherType of the ARP class. Its one action polices in
 * packets a second at one rate, passing what conforms and dropping the rest,
 * as the meter does.
 */
static int rtl9602c_storm_rule_check(struct flow_rule *rule, unsigned int cls,
				     struct netlink_ext_ack *extack)
{
	u64 keys = BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL) |
		   BIT_ULL(FLOW_DISSECTOR_KEY_BASIC);
	const struct flow_action_entry *act;
	struct flow_match_control control;
	struct flow_match_basic basic;

	if (cls != RTL9602C_STORM_ARP)
		keys |= BIT_ULL(FLOW_DISSECTOR_KEY_ETH_ADDRS);
	if (rule->match.dissector->used_keys & ~keys)
		goto wider;
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		flow_rule_match_control(rule, &control);
		if (control.mask->flags)
			goto wider;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		flow_rule_match_basic(rule, &basic);
		if (basic.mask->ip_proto ||
		    (cls != RTL9602C_STORM_ARP && basic.mask->n_proto))
			goto wider;
	}

	if (!flow_offload_has_one_action(&rule->action)) {
		NL_SET_ERR_MSG_MOD(extack, "exactly one action, please");
		return -EOPNOTSUPP;
	}
	act = &rule->action.entries[0];
	if (act->id != FLOW_ACTION_POLICE) {
		NL_SET_ERR_MSG_MOD(extack,
				   "a destination-address match is offloaded as storm control, whose only action is `police`");
		return -EOPNOTSUPP;
	}
	if (act->police.rate_bytes_ps) {
		NL_SET_ERR_MSG_MOD(extack,
				   "these meters count packets; use `police pkts_rate` rather than `rate`");
		return -EOPNOTSUPP;
	}
	if (!act->police.rate_pkt_ps) {
		NL_SET_ERR_MSG_MOD(extack,
				   "storm control needs a packet rate: `police pkts_rate N pkts_burst M`");
		return -EOPNOTSUPP;
	}
	if (act->police.exceed.act_id != FLOW_ACTION_DROP ||
	    (act->police.notexceed.act_id != FLOW_ACTION_PIPE &&
	     act->police.notexceed.act_id != FLOW_ACTION_ACCEPT)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "a meter passes what conforms and drops what exceeds: `conform-exceed drop`");
		return -EOPNOTSUPP;
	}
	if (act->police.peakrate_bytes_ps || act->police.avrate ||
	    act->police.overhead) {
		NL_SET_ERR_MSG_MOD(extack,
				   "a meter has one rate, and no peakrate, avrate or overhead");
		return -EOPNOTSUPP;
	}
	return 0;

wider:
	NL_SET_ERR_MSG_MOD(extack,
			   "storm control polices a whole class: match the destination address or the ARP EtherType and nothing else");
	return -EOPNOTSUPP;
}

/*
 * Each flower rule goes to one block: a broadcast or multicast destination, or
 * ARP, with one police action to storm control; one drop or trap action to the
 * ACL; a VLAN match with its vlan actions to the classifier.
 */
/*
 * A tag a VLAN rule names: its DEI is not matched, and its VID and priority
 * each in full or not at all.
 */
static int rtl9602c_flower_tag_check(const struct flow_match_vlan *m,
				     struct netlink_ext_ack *extack)
{
	if (m->mask->vlan_dei) {
		NL_SET_ERR_MSG_MOD(extack, "the DEI is not matched");
		return -EOPNOTSUPP;
	}
	if (m->mask->vlan_id && m->mask->vlan_id != VLAN_VID_MASK) {
		NL_SET_ERR_MSG_MOD(extack, "a VLAN id is matched in full or not at all");
		return -EOPNOTSUPP;
	}
	if (m->mask->vlan_priority &&
	    m->mask->vlan_priority != VLAN_PRIO_MASK >> VLAN_PRIO_SHIFT) {
		NL_SET_ERR_MSG_MOD(extack, "a priority is matched in full or not at all");
		return -EOPNOTSUPP;
	}
	return 0;
}

/* The VID and priority of @m that the rule names, into @x's match. */
static void rtl9602c_flower_tag_fill(const struct flow_match_vlan *m,
				     struct rtl9602c_cf_xlate *x)
{
	if (m->mask->vlan_id) {
		x->value |= FIELD_PREP(RTL9602C_CLS_RULE_VID, m->key->vlan_id);
		x->care |= RTL9602C_CLS_RULE_VID;
	}
	if (m->mask->vlan_priority) {
		x->value |= FIELD_PREP(RTL9602C_CLS_RULE_PRI,
				       m->key->vlan_priority);
		x->care |= RTL9602C_CLS_RULE_PRI;
	}
}

/*
 * The tags a VLAN rule matches, from its flower keys:
 *
 * - an untagged frame, num_of_vlans 0: neither tag;
 * - an outer C-tag (802.1Q): a C-tag and no S-tag, with the tag's VID and
 *   priority if the rule names them, from template RTL9602C_CLS_TMPL_C;
 * - an outer S-tag (802.1ad), which the SFP and PON ports recognise
 *   (rtl9602c_svlan_setup()): with the S-tag's VID and priority, from
 *   template RTL9602C_CLS_TMPL_S, or with those of the C-tag behind it,
 *   cvlan_id and cvlan_prio, from template RTL9602C_CLS_TMPL_C; one entry
 *   matches the VID and priority of one tag.
 *
 * The switch counts no tags, so a rule on a tag takes no number of tags, and
 * an S-tag rule matches a frame with a C-tag behind the S-tag or without one.
 * Fills @x's match and template, and @outer with the protocol of the outer
 * tag, 0 for an untagged frame.
 */
static int rtl9602c_flower_vlan_match(struct flow_rule *rule,
				      struct rtl9602c_cf_xlate *x, u16 *outer,
				      struct netlink_ext_ack *extack)
{
	struct flow_match_vlan vlan, cvlan;
	struct flow_match_basic basic;
	int err;

	if (rule->match.dissector->used_keys &
	    ~(BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_BASIC) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_VLAN) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_CVLAN) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_NUM_OF_VLANS))) {
		NL_SET_ERR_MSG_MOD(extack,
				   "this classifier matches the VLAN tags and nothing else");
		return -EOPNOTSUPP;
	}

	flow_rule_match_basic(rule, &basic);
	if (basic.mask->n_proto || basic.mask->ip_proto) {
		NL_SET_ERR_MSG_MOD(extack,
				   "what follows the tags is not matched; drop the EtherType behind them");
		return -EOPNOTSUPP;
	}

	*outer = 0;
	x->tmpl = RTL9602C_CLS_TMPL_C;
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_NUM_OF_VLANS)) {
		struct flow_match_num_of_vlans n;

		flow_rule_match_num_of_vlans(rule, &n);
		if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN) ||
		    n.mask->num_of_vlans != 0xff || n.key->num_of_vlans) {
			NL_SET_ERR_MSG_MOD(extack,
					   "the switch counts no tags; num_of_vlans 0, alone, matches an untagged frame");
			return -EOPNOTSUPP;
		}
		/* Untagged: neither tag. */
		x->care |= RTL9602C_CLS_RULE_CTAG | RTL9602C_CLS_RULE_STAG;
		return 0;
	}
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "name the outer tag, or match an untagged frame with num_of_vlans 0");
		return -EOPNOTSUPP;
	}

	flow_rule_match_vlan(rule, &vlan);
	err = rtl9602c_flower_tag_check(&vlan, extack);
	if (err)
		return err;
	if (vlan.mask->vlan_tpid != htons(0xffff)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "name the protocol of the outer tag: 802.1Q or 802.1ad");
		return -EOPNOTSUPP;
	}

	if (vlan.key->vlan_tpid == htons(ETH_P_8021Q)) {
		if (vlan.mask->vlan_eth_type ||
		    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "what follows an outer C-tag is not matched; the switch recognises a second tag only behind an S-tag");
			return -EOPNOTSUPP;
		}
		/* An outer C-tag: a C-tag and no S-tag. */
		*outer = ETH_P_8021Q;
		x->value |= RTL9602C_CLS_RULE_CTAG;
		x->care |= RTL9602C_CLS_RULE_CTAG | RTL9602C_CLS_RULE_STAG;
		rtl9602c_flower_tag_fill(&vlan, x);
		return 0;
	}
	if (vlan.key->vlan_tpid != htons(ETH_P_8021AD)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "the outer tag is a C-tag (802.1Q) or an S-tag (802.1ad)");
		return -EOPNOTSUPP;
	}

	/* An outer S-tag. */
	*outer = ETH_P_8021AD;
	x->value |= RTL9602C_CLS_RULE_STAG;
	x->care |= RTL9602C_CLS_RULE_STAG;
	if (vlan.mask->vlan_eth_type &&
	    vlan.key->vlan_eth_type != htons(ETH_P_8021Q)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "behind an S-tag only a C-tag is matched, not an EtherType");
		return -EOPNOTSUPP;
	}
	/* A C-tag behind the S-tag. */
	if (vlan.mask->vlan_eth_type) {
		x->value |= RTL9602C_CLS_RULE_CTAG;
		x->care |= RTL9602C_CLS_RULE_CTAG;
	}
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		x->tmpl = RTL9602C_CLS_TMPL_S;
		rtl9602c_flower_tag_fill(&vlan, x);
		return 0;
	}

	flow_rule_match_cvlan(rule, &cvlan);
	err = rtl9602c_flower_tag_check(&cvlan, extack);
	if (err)
		return err;
	if (cvlan.mask->vlan_eth_type ||
	    (cvlan.mask->vlan_tpid &&
	     (cvlan.mask->vlan_tpid != htons(0xffff) ||
	      cvlan.key->vlan_tpid != htons(ETH_P_8021Q)))) {
		NL_SET_ERR_MSG_MOD(extack,
				   "behind an S-tag a C-tag (802.1Q) is matched, and nothing behind that");
		return -EOPNOTSUPP;
	}
	if ((cvlan.mask->vlan_id || cvlan.mask->vlan_priority) &&
	    (vlan.mask->vlan_id || vlan.mask->vlan_priority)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "a rule matches the VID and priority of one tag: the S-tag's or those of the C-tag behind it");
		return -EOPNOTSUPP;
	}
	if (cvlan.mask->vlan_id || cvlan.mask->vlan_priority) {
		rtl9602c_flower_tag_fill(&cvlan, x);
	} else {
		x->tmpl = RTL9602C_CLS_TMPL_S;
		rtl9602c_flower_tag_fill(&vlan, x);
	}
	return 0;
}

/*
 * A VLAN rule of tc flower as one classifier entry, on the ingress of the SFP
 * port for upstream frames, of its own port only, or of the PON port for
 * downstream ones. The match is that of rtl9602c_flower_vlan_match(); the
 * actions are:
 *
 * - vlan modify of the outer tag: of a C-tag, its VID, and its priority
 *   unless the action gives 0, as the action cannot say whether it gives one;
 *   of an S-tag the same, the tag keeping the S-tag's TPID with protocol
 *   802.1ad or taking the C-tag's with 802.1Q;
 * - vlan push of a C-tag onto an untagged frame, or of an S-tag (802.1ad, with
 *   the TPID of RTL9602C_SVLAN_TPID_CFG_PRIMARY) in front of a frame without
 *   one, with the action's VID and priority;
 * - vlan pop of the outer tag, and two vlan pops of an S-tag and the C-tag
 *   behind it.
 */
static int rtl9602c_flower_vlan(struct rtl9602c_priv *p, int port,
				struct flow_cls_offload *cls)
{
	struct netlink_ext_ack *extack = cls->common.extack;
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	struct rtl9602c_cf_xlate x = { .out_vid = VLAN_N_VID, .port = port };
	const struct flow_action_entry *act;
	unsigned int idx;
	u16 outer;
	int err;

	err = rtl9602c_flower_vlan_match(rule, &x, &outer, extack);
	if (err)
		return err;
	if (!rtl9602c_port_is_downstream(port)) {
		x.value |= FIELD_PREP(RTL9602C_CLS_RULE_UNI, port);
		x.care |= RTL9602C_CLS_RULE_UNI;
	}

	if (outer == ETH_P_8021AD && rule->action.num_entries == 2 &&
	    rule->action.entries[0].id == FLOW_ACTION_VLAN_POP &&
	    rule->action.entries[1].id == FLOW_ACTION_VLAN_POP) {
		/* Both tags off: the S-tag, and the C-tag if there is one. */
		x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG,
				      RTL9602C_CLS_ACT_STAG_POP);
		x.act[1] = FIELD_PREP(RTL9602C_CLS_ACT_CTAG,
				      RTL9602C_CLS_ACT_CTAG_UNTAG);
		goto place;
	}
	if (!flow_offload_has_one_action(&rule->action)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "one action, or two vlan pops of an S-tag and the C-tag behind it");
		return -EOPNOTSUPP;
	}
	act = &rule->action.entries[0];

	switch (act->id) {
	case FLOW_ACTION_VLAN_MANGLE:
		if (outer == ETH_P_8021Q &&
		    act->vlan.proto == htons(ETH_P_8021Q)) {
			x.act[1] = RTL9602C_CLS_ACT_CVID_SET |
				   FIELD_PREP(RTL9602C_CLS_ACT_CVID,
					      act->vlan.vid) |
				   FIELD_PREP(RTL9602C_CLS_ACT_CTAG,
					      RTL9602C_CLS_ACT_CTAG_TAG);
			if (act->vlan.prio)
				x.act[1] |= RTL9602C_CLS_ACT_CPRI_SET |
					    FIELD_PREP(RTL9602C_CLS_ACT_CPRI,
						       act->vlan.prio);
		} else if (outer == ETH_P_8021AD &&
			   (act->vlan.proto == htons(ETH_P_8021AD) ||
			    act->vlan.proto == htons(ETH_P_8021Q))) {
			x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG,
					      act->vlan.proto == htons(ETH_P_8021AD) ?
					      RTL9602C_CLS_ACT_STAG_ADD :
					      RTL9602C_CLS_ACT_STAG_CTPID) |
				   RTL9602C_CLS_ACT_SVID_SET |
				   FIELD_PREP(RTL9602C_CLS_ACT_SVID,
					      act->vlan.vid);
			if (act->vlan.prio)
				x.act[0] |= RTL9602C_CLS_ACT_SPRI_SET |
					    FIELD_PREP(RTL9602C_CLS_ACT_SPRI,
						       act->vlan.prio);
		} else {
			NL_SET_ERR_MSG_MOD(extack,
					   "vlan modify rewrites the outer tag: a C-tag into a C-tag, an S-tag into an S-tag or a C-tag");
			return -EOPNOTSUPP;
		}
		x.out_vid = act->vlan.vid;
		break;
	case FLOW_ACTION_VLAN_PUSH:
		if (act->vlan.proto == htons(ETH_P_8021Q) && !outer) {
			x.act[1] = RTL9602C_CLS_ACT_CVID_SET |
				   FIELD_PREP(RTL9602C_CLS_ACT_CVID,
					      act->vlan.vid) |
				   RTL9602C_CLS_ACT_CPRI_SET |
				   FIELD_PREP(RTL9602C_CLS_ACT_CPRI,
					      act->vlan.prio) |
				   FIELD_PREP(RTL9602C_CLS_ACT_CTAG,
					      RTL9602C_CLS_ACT_CTAG_TAG);
		} else if (act->vlan.proto == htons(ETH_P_8021AD) &&
			   outer != ETH_P_8021AD) {
			x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG,
					      RTL9602C_CLS_ACT_STAG_ADD) |
				   RTL9602C_CLS_ACT_SVID_SET |
				   FIELD_PREP(RTL9602C_CLS_ACT_SVID,
					      act->vlan.vid) |
				   RTL9602C_CLS_ACT_SPRI_SET |
				   FIELD_PREP(RTL9602C_CLS_ACT_SPRI,
					      act->vlan.prio);
		} else {
			NL_SET_ERR_MSG_MOD(extack,
					   "a C-tag is pushed onto an untagged frame, an S-tag onto a frame without one");
			return -EOPNOTSUPP;
		}
		x.out_vid = act->vlan.vid;
		break;
	case FLOW_ACTION_VLAN_POP:
		if (outer == ETH_P_8021AD) {
			x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG,
					      RTL9602C_CLS_ACT_STAG_POP);
		} else if (outer == ETH_P_8021Q) {
			x.act[1] = FIELD_PREP(RTL9602C_CLS_ACT_CTAG,
					      RTL9602C_CLS_ACT_CTAG_UNTAG);
		} else {
			NL_SET_ERR_MSG_MOD(extack,
					   "an untagged frame has no tag to pop");
			return -EOPNOTSUPP;
		}
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack,
				   "the offloadable actions are vlan modify, push and pop");
		return -EOPNOTSUPP;
	}

place:
	/* From the bottom of the usable range, below the stream entries. */
	idx = find_first_zero_bit(p->cf_used, p->cf_entries);
	if (idx >= p->cf_entries) {
		NL_SET_ERR_MSG_MOD(extack, "the classifier is full");
		return -ENOSPC;
	}

	p->cf_xlate[idx] = x;
	err = rtl9602c_cf_write_xlate(p, idx);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "the classifier did not answer");
		return err;
	}

	p->cf_cookie[idx] = cls->cookie;
	set_bit(idx, p->cf_used);
	return 0;
}

static int rtl9602c_flower_replace(struct rtl9602c_priv *p, int port,
				   struct flow_cls_offload *cls)
{
	struct netlink_ext_ack *extack = cls->common.extack;
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	const struct flow_action_entry *act;
	int cls_id, err;

	cls_id = rtl9602c_storm_class_of(rule);
	if (cls_id < 0 && rtl9602c_flower_is_arp(rule))
		cls_id = RTL9602C_STORM_ARP;
	if (cls_id >= 0) {
		err = rtl9602c_storm_rule_check(rule, (unsigned int)cls_id,
						extack);
		if (err)
			return err;
		act = &rule->action.entries[0];
		return rtl9602c_storm_add(p, port, (unsigned int)cls_id,
					  act->police.rate_pkt_ps,
					  cls->cookie, extack);
	}

	if (flow_offload_has_one_action(&rule->action)) {
		act = &rule->action.entries[0];
		if (act->id == FLOW_ACTION_DROP)
			return rtl9602c_acl_replace(p, port, cls,
						    RTL9602C_ACL_ACT_FWD_REDIRECT,
						    0);
		if (act->id == FLOW_ACTION_TRAP)
			return rtl9602c_acl_replace(p, port, cls,
						    RTL9602C_ACL_ACT_FWD_TRAP,
						    0);
	}

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN) &&
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_NUM_OF_VLANS)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "only a match on the VLAN tags can be offloaded here");
		return -EOPNOTSUPP;
	}
	return rtl9602c_flower_vlan(p, port, cls);
}

static int rtl9602c_flower_destroy(struct rtl9602c_priv *p,
				   struct flow_cls_offload *cls)
{
	unsigned int idx;

	if (rtl9602c_storm_del(p, cls->cookie) == 0)
		return 0;
	if (rtl9602c_acl_del(p, cls->cookie) == 0)
		return 0;

	for_each_set_bit(idx, p->cf_used, RTL9602C_CLS_ENTRIES) {
		if (p->cf_cookie[idx] != cls->cookie)
			continue;
		clear_bit(idx, p->cf_used);
		return rtl9602c_cf_invalidate(p, idx);
	}
	return -ENOENT;
}

/**
 * rtl9602c_cls_flower_add() - offload a flower rule
 * @ds: the switch
 * @port: the port the rule is on
 * @cls: the rule
 * @ingress: whether the rule is on ingress; egress is refused, as every block a
 *	rule can reach judges the frames entering a port
 *
 * Return: 0, or a negative error code with a message in the extack.
 */
int rtl9602c_cls_flower_add(struct dsa_switch *ds, int port,
			    struct flow_cls_offload *cls, bool ingress)
{
	struct rtl9602c_priv *p = ds->priv;

	if (!ingress) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "the classifier acts on ingress only");
		return -EOPNOTSUPP;
	}
	return rtl9602c_flower_replace(p, port, cls);
}

/**
 * rtl9602c_cls_flower_del() - remove an offloaded flower rule
 * @ds: the switch
 * @port: unused; the cookie names the rule
 * @cls: the rule
 * @ingress: unused
 *
 * Return: 0, -ENOENT when no rule has the cookie, or a negative error code from
 * the table access.
 */
int rtl9602c_cls_flower_del(struct dsa_switch *ds, int port,
			    struct flow_cls_offload *cls, bool ingress)
{
	return rtl9602c_flower_destroy(ds->priv, cls);
}

/*
 * The devlink storm-control parameters, a packet rate and a port mask for each
 * class the flower rules do not reach: unknown unicast, unknown multicast,
 * DHCP, and IGMP with MLD.
 */
const struct rtl9602c_storm_param_row rtl9602c_storm_param[] = {
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_UC_PORTS,   RTL9602C_STORM_UC,   false },
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_UC_RATE,    RTL9602C_STORM_UC,   true  },
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_UM_PORTS,   RTL9602C_STORM_UM,   false },
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_UM_RATE,    RTL9602C_STORM_UM,   true  },
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_PORTS, RTL9602C_STORM_DHCP, false },
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_RATE,  RTL9602C_STORM_DHCP, true  },
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_PORTS, RTL9602C_STORM_IGMP, false },
	{ RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_RATE,  RTL9602C_STORM_IGMP, true  },
};

/**
 * rtl9602c_storm_param_find() - look up a storm-control parameter
 * @id: the devlink parameter's ID
 *
 * Return: its index in rtl9602c_storm_param[], or -1 when @id is not one.
 */
int rtl9602c_storm_param_find(u32 id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rtl9602c_storm_param); i++)
		if (rtl9602c_storm_param[i].id == id)
			return (int)i;
	return -1;
}

/*
 * Priority selection. The switch gives each frame one of eight internal
 * priorities, from sources such as the ingress port, the PCP and the DSCP, each
 * with a 3-bit weight; weight 0 leaves a source out, and of the sources that
 * apply the heaviest is taken to decide (not verified). Every weight reads 0
 * out of reset, so every frame has priority 0 until one is set. The maps hold
 * ten 3-bit entries to a word.
 */
static unsigned int rtl9602c_packed3_get(struct rtl9602c_priv *p, u32 base,
					 unsigned int idx)
{
	u32 w = rtl9602c_r(p, base + 4 * (idx / RTL9602C_QOS_MAP_PER_WORD));

	return (w >> (RTL9602C_QOS_MAP_ENTRY_BITS *
		      (idx % RTL9602C_QOS_MAP_PER_WORD))) & 7;
}

static void rtl9602c_packed3_set(struct rtl9602c_priv *p, u32 base,
				 unsigned int idx, u8 val)
{
	unsigned int shift = RTL9602C_QOS_MAP_ENTRY_BITS *
			     (idx % RTL9602C_QOS_MAP_PER_WORD);

	rtl9602c_rmw(p, base + 4 * (idx / RTL9602C_QOS_MAP_PER_WORD),
		     0x7u << shift, (u32)(val & 7) << shift);
}

/*
 * There are two weight tables; the index bit names the one in force, and only
 * that one is read or written.
 */
static u32 rtl9602c_pri_sel_reg(struct rtl9602c_priv *p)
{
	u32 sel = rtl9602c_r(p, RTL9602C_QOS_PRIO_WEIGHT_SEL) &
		  RTL9602C_QOS_PRIO_WEIGHT_SEL_IDX;

	return RTL9602C_QOS_PRIO_WEIGHT + 4 * sel;
}

/*
 * Source @src's weight is 3 bits at bit 3 * @src. That the PCP and port sources
 * sit at 1 and 0 is not verified.
 */
static void rtl9602c_pri_weight_set(struct rtl9602c_priv *p, unsigned int src,
				    u8 weight)
{
	unsigned int shift = 3 * src;

	rtl9602c_rmw(p, rtl9602c_pri_sel_reg(p), 0x7u << shift,
		     (u32)(weight & 7) << shift);
}

static u8 rtl9602c_pri_weight_get(struct rtl9602c_priv *p, unsigned int src)
{
	return (rtl9602c_r(p, rtl9602c_pri_sel_reg(p)) >> (3 * src)) & 7;
}

/**
 * rtl9602c_port_get_default_prio() - read a port's default priority
 * @ds: the switch
 * @port: the port
 *
 * Return: the priority, 0 to 7.
 */
int rtl9602c_port_get_default_prio(struct dsa_switch *ds, int port)
{
	struct rtl9602c_priv *p = ds->priv;

	return rtl9602c_packed3_get(p, RTL9602C_QOS_PORT_PRIO, port);
}

/**
 * rtl9602c_port_set_default_prio() - set a port's default priority
 * @ds: the switch
 * @port: the port
 * @prio: the priority
 *
 * A port source weighing 0 is given 1, the lightest, so that the default
 * applies and any trusted source outranks it; not verified.
 *
 * Return: 0, or -ERANGE for a priority above 7.
 */
int rtl9602c_port_set_default_prio(struct dsa_switch *ds, int port,
				   u8 prio)
{
	struct rtl9602c_priv *p = ds->priv;

	if (prio >= RTL9602C_QOS_PRIO_COUNT)
		return -ERANGE;

	rtl9602c_packed3_set(p, RTL9602C_QOS_PORT_PRIO, port, prio);
	if (!rtl9602c_pri_weight_get(p, RTL9602C_QOS_PRIO_WEIGHT_PORT))
		rtl9602c_pri_weight_set(p, RTL9602C_QOS_PRIO_WEIGHT_PORT, 1);
	return 0;
}

/**
 * rtl9602c_port_get_dscp_prio() - read the priority a DSCP maps to
 * @ds: the switch
 * @port: unused: one DSCP map serves the whole switch, frames entering by the
 *	PON port included
 * @dscp: the DSCP
 *
 * Return: the priority, or -EINVAL for a DSCP above 63.
 */
int rtl9602c_port_get_dscp_prio(struct dsa_switch *ds, int port,
				u8 dscp)
{
	struct rtl9602c_priv *p = ds->priv;

	if (dscp >= RTL9602C_QOS_DSCP_COUNT)
		return -EINVAL;
	return rtl9602c_packed3_get(p, RTL9602C_QOS_DSCP_TO_PRIO, dscp);
}

/**
 * rtl9602c_port_add_dscp_prio() - map a DSCP to a priority
 * @ds: the switch
 * @port: unused, as in rtl9602c_port_get_dscp_prio()
 * @dscp: the DSCP
 * @prio: the priority
 *
 * Return: 0, or -ERANGE for a DSCP above 63 or a priority above 7.
 */
int rtl9602c_port_add_dscp_prio(struct dsa_switch *ds, int port,
				u8 dscp, u8 prio)
{
	struct rtl9602c_priv *p = ds->priv;

	if (dscp >= RTL9602C_QOS_DSCP_COUNT || prio >= RTL9602C_QOS_PRIO_COUNT)
		return -ERANGE;
	rtl9602c_packed3_set(p, RTL9602C_QOS_DSCP_TO_PRIO, dscp, prio);
	return 0;
}

/**
 * rtl9602c_port_del_dscp_prio() - map a DSCP back to priority 0
 * @ds: the switch
 * @port: unused, as in rtl9602c_port_get_dscp_prio()
 * @dscp: the DSCP
 * @prio: unused
 *
 * Return: 0, or -EINVAL for a DSCP above 63.
 */
int rtl9602c_port_del_dscp_prio(struct dsa_switch *ds, int port,
				u8 dscp, u8 prio)
{
	struct rtl9602c_priv *p = ds->priv;

	if (dscp >= RTL9602C_QOS_DSCP_COUNT)
		return -EINVAL;
	rtl9602c_packed3_set(p, RTL9602C_QOS_DSCP_TO_PRIO, dscp, 0);
	return 0;
}

/**
 * rtl9602c_port_set_apptrust() - set which sources are trusted, in order
 * @ds: the switch
 * @port: the port
 * @sel: the selectors, most trusted first; PCP and DSCP only
 * @nsel: how many, at most two
 *
 * The weights are switch-wide: once a port has set an order, another port may
 * only set the same one. The first source gets weight 7, the second 6, a source
 * left out 0. An empty order releases the weights.
 *
 * Return: 0, or -EOPNOTSUPP for another selector, more than two, or an order
 * differing from another port's.
 */
int rtl9602c_port_set_apptrust(struct dsa_switch *ds, int port,
			       const u8 *sel, int nsel)
{
	struct rtl9602c_priv *p = ds->priv;
	int i;

	if (nsel > RTL9602C_APPTRUST_MAX)
		return -EOPNOTSUPP;

	if (p->apptrust_port >= 0 && p->apptrust_port != port &&
	    (p->apptrust_n != nsel || memcmp(p->apptrust, sel, nsel)))
		return -EOPNOTSUPP;

	rtl9602c_pri_weight_set(p, RTL9602C_QOS_PRIO_WEIGHT_PCP, 0);
	rtl9602c_pri_weight_set(p, RTL9602C_QOS_PRIO_WEIGHT_DSCP, 0);

	for (i = 0; i < nsel; i++) {
		u8 weight = RTL9602C_QOS_PRIO_WEIGHT_MAX - i;

		switch (sel[i]) {
		case DCB_APP_SEL_PCP:
			rtl9602c_pri_weight_set(p, RTL9602C_QOS_PRIO_WEIGHT_PCP,
						weight);
			break;
		case IEEE_8021QAZ_APP_SEL_DSCP:
			rtl9602c_pri_weight_set(p, RTL9602C_QOS_PRIO_WEIGHT_DSCP,
						weight);
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	memset(p->apptrust, 0, sizeof(p->apptrust));
	memcpy(p->apptrust, sel, nsel);
	p->apptrust_n = nsel;
	p->apptrust_port = nsel ? port : -1;
	return 0;
}

/**
 * rtl9602c_port_get_apptrust() - read which sources are trusted, in order
 * @ds: the switch
 * @port: unused; the weights are switch-wide
 * @sel: receives the selectors, heavier first
 * @nsel: receives how many
 *
 * Answered from the weights, as the hardware holds them.
 *
 * Return: 0.
 */
int rtl9602c_port_get_apptrust(struct dsa_switch *ds, int port, u8 *sel,
			       int *nsel)
{
	struct rtl9602c_priv *p = ds->priv;
	u8 dot1q, dscp;
	int n = 0;

	dot1q = rtl9602c_pri_weight_get(p, RTL9602C_QOS_PRIO_WEIGHT_PCP);
	dscp = rtl9602c_pri_weight_get(p, RTL9602C_QOS_PRIO_WEIGHT_DSCP);

	if (dot1q && dot1q >= dscp)
		sel[n++] = DCB_APP_SEL_PCP;
	if (dscp)
		sel[n++] = IEEE_8021QAZ_APP_SEL_DSCP;
	if (dot1q && dot1q < dscp)
		sel[n++] = DCB_APP_SEL_PCP;

	*nsel = n;
	return 0;
}

/**
 * rtl9602c_port_policer_add() - limit the traffic entering a port
 * @ds: the switch
 * @port: the port
 * @policer: the rate; the burst is not programmed, as no bucket size is known
 *	for this limiter
 *
 * The rate is in steps of 1000 bytes a second (8 kbit/s); one beyond the field
 * is cut to its maximum, which does not limit.
 *
 * Return: 0, or -ERANGE for a rate below one step.
 */
int rtl9602c_port_policer_add(struct dsa_switch *ds, int port,
			      struct dsa_mall_policer_tc_entry *policer)
{
	struct rtl9602c_priv *p = ds->priv;
	u64 units = div_u64(policer->rate_bytes_per_sec, 1000);

	if (!units)
		return -ERANGE;
	if (units > RTL9602C_POLICE_PORT_RATE_MAX)
		units = RTL9602C_POLICE_PORT_RATE_MAX;

	rtl9602c_rmw(p, RTL9602C_POLICE_PORT(port), RTL9602C_POLICE_PORT_RATE,
		     FIELD_PREP(RTL9602C_POLICE_PORT_RATE, units));
	return 0;
}

/**
 * rtl9602c_port_policer_del() - lift a port's ingress limit
 * @ds: the switch
 * @port: the port
 *
 * The rate field's maximum, its reset value, does not limit.
 */
void rtl9602c_port_policer_del(struct dsa_switch *ds, int port)
{
	struct rtl9602c_priv *p = ds->priv;

	rtl9602c_rmw(p, RTL9602C_POLICE_PORT(port), RTL9602C_POLICE_PORT_RATE,
		     FIELD_PREP(RTL9602C_POLICE_PORT_RATE, RTL9602C_POLICE_PORT_RATE_MAX));
}

/**
 * rtl9602c_nat_clear() - empty the L3/NAT engine's tables
 * @p: the switch
 *
 * The tables keep their contents across a warm restart and hold arbitrary words
 * on a board nothing has programmed, and the engine's reset strobes do not
 * empty them. So every row of every table is read, and each that is not all
 * zero is overwritten with zeros; an all-zero row is inert, its valid bits
 * clear. Stops at the first failed access. The NAPT hit bitmap is not cleared.
 */
void rtl9602c_nat_clear(struct rtl9602c_priv *p)
{
	unsigned int table, index, w, words, dirty = 0;
	u32 e[RTL9602C_L3TBL_DATA_WORDS];
	static const u32 zero[RTL9602C_L3TBL_DATA_WORDS];
	ktime_t t0 = ktime_get();

	for (table = 0; table < ARRAY_SIZE(rtl9602c_nat_tbl_entries); table++) {
		words = rtl9602c_nat_tbl_words[table];
		if (!rtl9602c_nat_tbl_entries[table] || !words)
			continue;

		for (index = 0; index < rtl9602c_nat_tbl_entries[table];
		     index++) {
			if (rtl9602c_nat_tbl_read(p, table, index, e, words))
				return;
			for (w = 0; w < words; w++)
				if (e[w])
					break;
			if (w == words)
				continue;
			if (rtl9602c_nat_tbl_write(p, table, index, zero,
						   words))
				return;
			dirty++;
		}
	}

	if (dirty)
		pr_info("cleared %u L3/NAT table entr%s, in %lld ms\n",
			dirty, dirty == 1 ? "y" : "ies",
			ktime_ms_delta(ktime_get(), t0));
}

/*
 * qdepth: for each port and queue, the buffer pages it holds now and the most
 * it has held. Writing "clear" pulses the clear of the per-port high-water
 * marks, which leaves these per-queue ones as they are: nothing but a full chip
 * reset is known to clear them.
 */
static int rtl9602c_qdepth_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int port, q;

	seq_puts(s, "port queue   now    peak\n");
	for (port = 0; port < RTL9602C_NUM_PORTS; port++)
		for (q = 0; q < RTL9602C_QUEUES; q++) {
			u32 v = rtl9602c_r(p, RTL9602C_BUF_QUEUE_PAGES +
					   4 * (RTL9602C_QUEUES * port + q));

			seq_printf(s, "%4u %5u %5lu %7lu\n", port, q,
				   FIELD_GET(RTL9602C_BUF_QUEUE_PAGES_CUR, v),
				   FIELD_GET(RTL9602C_BUF_QUEUE_PAGES_PEAK, v));
		}
	return 0;
}

static int rtl9602c_qdepth_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_qdepth_show, inode->i_private);
}

static ssize_t rtl9602c_qdepth_write(struct file *file, const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	struct rtl9602c_priv *p = file_inode(file)->i_private;
	char buf[16], *s;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	s = strim(buf);

	if (strcmp(s, "clear"))
		return -EINVAL;

	rtl9602c_w(p, RTL9602C_BUF_PEAK_CLR,
		   RTL9602C_BUF_PEAK_CLR_PORT);
	usleep_range(1000, 2000);
	rtl9602c_w(p, RTL9602C_BUF_PEAK_CLR, 0);
	return len;
}

/**
 * rtl9602c_l34_init() - bring up the L3/NAT engine
 * @p: the switch
 *
 * Strobes the reset of each table engine, configures the engine and turns it
 * on, then records whether it came on as l34_up, which gates everything that
 * hands the engine frames or rows. Only then are this device's own addresses
 * handed to it. Which table each strobe resets, and that the strobes are needed
 * before the enable, are not verified; a strobe does not empty its table,
 * rtl9602c_nat_clear() does.
 */
/*
 * Strobe the reset of each table engine and return how many there are. A strobe
 * clears itself; one still set is left and counted in @busy, the others in
 * @done. Nothing waits for the strobes to finish: that they have by the first
 * table access is not verified.
 */
static unsigned int rtl9602c_l34_tables_reset(struct rtl9602c_priv *p,
					      unsigned int *done,
					      unsigned int *busy)
{
	static const u32 rst[] = {
		RTL9602C_L3TBL_RESET_ROUTE, RTL9602C_L3TBL_RESET_PPPOE,
		RTL9602C_L3TBL_RESET_NEXTHOP, RTL9602C_L3TBL_RESET_L3IF,
		RTL9602C_L3TBL_RESET_EXTADDR, RTL9602C_L3TBL_RESET_NEIGH,
		RTL9602C_L3TBL_RESET_NAPT_IN, RTL9602C_L3TBL_RESET_NAPT,
		RTL9602C_L3TBL_RESET_ROUTE6, RTL9602C_L3TBL_RESET_BIND,
		RTL9602C_L3TBL_RESET_NEIGH6, RTL9602C_L3TBL_RESET_WANTYPE,
		RTL9602C_L3TBL_RESET_FLOW,
	};
	unsigned int i;

	*done = 0;
	*busy = 0;
	for (i = 0; i < ARRAY_SIZE(rst); i++) {
		if (rtl9602c_r(p, RTL9602C_L3TBL_RESET) & rst[i]) {
			(*busy)++;
			continue;
		}
		rtl9602c_rmw(p, RTL9602C_L3TBL_RESET, 0, rst[i]);
		(*done)++;
	}
	return ARRAY_SIZE(rst);
}

void rtl9602c_l34_init(struct rtl9602c_priv *p)
{
	unsigned int done, busy, tables;
	u32 v;

	tables = rtl9602c_l34_tables_reset(p, &done, &busy);

	/*
	 * The per-L3-interface counters, which nothing reads; that the bit
	 * enables them and nothing else is not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_MIB_L3IF_CTRL, 0, RTL9602C_MIB_L3IF_CTRL_EN);

	/*
	 * The actions for frames whose WAN binding disagrees with the lookup;
	 * the binding table stays empty. Whether the word is needed is not
	 * verified.
	 */
	rtl9602c_w(p, RTL9602C_L3_BIND_CFG, RTL9602C_L3_BIND_CFG_INIT);
	pr_info("L3: binding actions %08x\n", rtl9602c_r(p, RTL9602C_L3_BIND_CFG));

	/*
	 * NAPT, and frames matched to interface rows by MAC, taken to be their
	 * destination MAC (not verified). Only these two fields are changed:
	 * changing others as well turns a translated flow's verdict into one
	 * for the CPU (measured on this board).
	 */
	rtl9602c_rmw(p, RTL9602C_L3_CFG,
		     RTL9602C_L3_CFG_NAT_MODE | RTL9602C_L3_CFG_L3IF_LOOKUP,
		     FIELD_PREP(RTL9602C_L3_CFG_NAT_MODE,
				RTL9602C_L3_CFG_NAT_MODE_NAPT) |
		     FIELD_PREP(RTL9602C_L3_CFG_L3IF_LOOKUP,
				RTL9602C_L3_CFG_L3IF_LOOKUP_MAC));

	/*
	 * The engine's master enable, read-modify-write to keep bit 2. With it
	 * clear the engine still computes a verdict for every frame, and the
	 * switch ignores it.
	 */
	rtl9602c_rmw(p, RTL9602C_L3_GLOBAL_CTRL, 0, RTL9602C_L3_GLOBAL_CTRL_EN);
	v = rtl9602c_r(p, RTL9602C_L3_GLOBAL_CTRL);

	/*
	 * What the bits of RTL9602C_L3_AUX_CFG_ON enable, and whether the write
	 * is needed, is not verified.
	 */
	rtl9602c_w(p, RTL9602C_L3_AUX_CFG, RTL9602C_L3_AUX_CFG_ON);
	pr_info("L3: auxiliary control %08x\n",
		rtl9602c_r(p, RTL9602C_L3_AUX_CFG));

	/*
	 * Only once the engine runs: from now on, frames to this device's own
	 * addresses, management included, go to the engine instead of the CPU.
	 */
	p->l34_up = !!(v & RTL9602C_L3_GLOBAL_CTRL_EN);
	if (p->l34_up)
		rtl9602c_l2_l3lookup_cpu(p, true);

	pr_info("L3: reset %u of %u table engine(s)%s, engine %s (control %08x, reset %08x)\n",
		done, tables,
		busy ? ", some already resetting" : "",
		v & RTL9602C_L3_GLOBAL_CTRL_EN ? "ON" : "off", v,
		rtl9602c_r(p, RTL9602C_L3TBL_RESET));
}

/*
 * Record that next-hop row @row names the address-table entry of @mac at @vid,
 * or, with @mac NULL, that it names none. The entry the row named before is
 * deleted when no other row names it: rtl9602c_l34_nh_l2_put() deletes an
 * entry that no row names.
 */
static void rtl9602c_l34_nh_l2_put(struct rtl9602c_priv *p,
				   const unsigned char *mac, u16 vid)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->l34_nh_mac); i++)
		if (ether_addr_equal(p->l34_nh_mac[i], mac) &&
		    p->l34_nh_vid[i] == vid)
			return;
	if (rtl9602c_l2_nexthop_drop(p, mac, vid))
		pr_warn("<l3> %pM vlan %u: the next hop's address-table entry was not deleted\n",
			mac, vid);
}

static void rtl9602c_l34_nh_l2_set(struct rtl9602c_priv *p, unsigned int row,
				   const unsigned char *mac, u16 vid)
{
	u16 old_vid = p->l34_nh_vid[row];
	u8 old[ETH_ALEN];

	ether_addr_copy(old, p->l34_nh_mac[row]);
	if (mac && ether_addr_equal(old, mac) && old_vid == vid)
		return;

	if (mac)
		ether_addr_copy(p->l34_nh_mac[row], mac);
	else
		eth_zero_addr(p->l34_nh_mac[row]);
	p->l34_nh_vid[row] = mac ? vid : 0;
	if (!is_zero_ether_addr(old))
		rtl9602c_l34_nh_l2_put(p, old, old_vid);
}

/**
 * rtl9602c_l34_fini() - stop the L3/NAT engine and empty its tables
 * @p: the switch, its L3 notifiers unregistered
 *
 * The undoing of rtl9602c_l34_init() at teardown: this device's own addresses
 * are trapped to the CPU again, the address-table entries of the next hops are
 * deleted, the engine's master enable is cleared, and its tables are reset and
 * emptied, so that no flow is forwarded by rows that no driver keeps any more,
 * and a driver bound again finds the engine as its bookkeeping starts.
 */
void rtl9602c_l34_fini(struct rtl9602c_priv *p)
{
	unsigned int done, busy, i;

	mutex_lock(&p->flow_lock);
	if (p->l34_up)
		rtl9602c_l2_l3lookup_cpu(p, false);
	p->l34_up = false;
	for (i = 0; i < ARRAY_SIZE(p->l34_nh_mac); i++)
		rtl9602c_l34_nh_l2_set(p, i, NULL, 0);
	rtl9602c_rmw(p, RTL9602C_L3_GLOBAL_CTRL, RTL9602C_L3_GLOBAL_CTRL_EN, 0);
	rtl9602c_l34_tables_reset(p, &done, &busy);
	rtl9602c_nat_clear(p);
	mutex_unlock(&p->flow_lock);
	pr_info("L3: engine off, %u table engine(s) reset\n", done);
}

/*
 * Entry fields are set and read bit by bit across the entry's words, so a field
 * may cross a word; every writer builds its entry from zero.
 */
static void rtl9602c_flow_put(u32 *e, struct rtl9602c_field f, u64 val)
{
	unsigned int i;

	for (i = 0; i < f.len; i++)
		if (val & BIT_ULL(i))
			e[(f.lsb + i) / 32] |= BIT((f.lsb + i) % 32);
}

static u64 rtl9602c_flow_get(const u32 *e, struct rtl9602c_field f)
{
	u64 v = 0;
	unsigned int i;

	for (i = 0; i < f.len; i++)
		if (e[(f.lsb + i) / 32] & BIT((f.lsb + i) % 32))
			v |= BIT_ULL(i);
	return v;
}

/*
 * The NAPT hashes. Both NAPT tables are 1024 buckets of four ways, and the
 * engine looks a flow up only in the bucket its hash gives. Inputs are in host
 * order. The TCP term is not verified, no TCP flow having been carried.
 */

/* Outbound: from the five-tuple as the LAN host sent it. */
static unsigned int rtl9602c_napt_bucket(u32 lip, u16 lport, u32 rip, u16 rport,
					 bool tcp)
{
	u32 u = (lport & 0xffff) + (lip & 0xffff) + (rip & 0xffff) + rport;

	return ((((u32)tcp << 9) | (lip >> 26)) ^ ((lip >> 16) & 0x3ff) ^
		((rip >> 16) & 0x3ff) ^ ((rip >> 22) & 0x3f0) ^
		(u + ((u >> 10) & 0xff))) & 0x3ff;
}

/*
 * A 16-bit digest of the remote end, kept in the inbound entry. The engine does
 * not check it on replies; the function, and whether the outbound lookup
 * compares the field, are not verified.
 */
static u16 rtl9602c_napt_rem_hash(u32 rip, u16 rport)
{
	return (rip >> 16) ^ (rip & 0xffff) ^ rport;
}

/*
 * Inbound: from the external address and port a reply is sent to. The remote
 * end is not an input.
 */
static unsigned int rtl9602c_napt_in_bucket(u32 extip, u16 extport, bool tcp)
{
	u32 u = extport + (extip & 0xffff);

	return ((((u32)tcp << 9) | (extip >> 26)) ^ ((extip >> 16) & 0x3ff) ^
		(u + (u >> 10))) & 0x3ff;
}

/*
 * L3-interface row @idx. Arriving frames are matched to it by @mac, exactly,
 * taken to be their destination address (not verified); frames routed out of it
 * take @mac as their source and @vlan as their VLAN. How the engine applies
 * @mtu is not verified.
 */
static int rtl9602c_l34_netif_set(struct rtl9602c_priv *p, unsigned int idx,
				  const u8 *mac, u16 vlan, __be32 addr,
				  unsigned int mtu)
{
	unsigned int words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_L3IF];
	u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};
	u64 gmac = 0;
	unsigned int i;

	if (idx >= rtl9602c_nat_tbl_entries[RTL9602C_L3TBL_CTRL_TABLE_L3IF])
		return -ERANGE;

	for (i = 0; i < ETH_ALEN; i++)
		gmac = gmac << 8 | mac[i];

	rtl9602c_flow_put(e, RTL9602C_L3IF_VALID, 1);
	rtl9602c_flow_put(e, RTL9602C_L3IF_VID, vlan);
	rtl9602c_flow_put(e, RTL9602C_L3IF_MAC, gmac);
	rtl9602c_flow_put(e, RTL9602C_L3IF_MAC_MASK,
			  RTL9602C_L3IF_MAC_MASK_EXACT);
	rtl9602c_flow_put(e, RTL9602C_L3IF_ROUTE_EN, 1);
	rtl9602c_flow_put(e, RTL9602C_L3IF_MTU, mtu);
	rtl9602c_flow_put(e, RTL9602C_L3IF_OFFLOAD_EN, 1);
	rtl9602c_flow_put(e, RTL9602C_L3IF_IP, be32_to_cpu(addr));

	return rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_L3IF, idx, e, words);
}

static int rtl9602c_l34_netif_clear(struct rtl9602c_priv *p, unsigned int idx)
{
	unsigned int words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_L3IF];
	u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};

	return rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_L3IF, idx, e, words);
}

/*
 * External-address row @idx: @addr, shared by NAPT flows, whose frames leave by
 * next-hop row @nh.
 */
static int rtl9602c_l34_extip_set(struct rtl9602c_priv *p, unsigned int idx,
				  __be32 addr, unsigned int nh)
{
	unsigned int extip_words =
		rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_EXTADDR];
	u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};

	if (idx >= ARRAY_SIZE(p->l34_extip))
		return -ERANGE;

	rtl9602c_flow_put(e, RTL9602C_EXTADDR_IP, be32_to_cpu(addr));
	rtl9602c_flow_put(e, RTL9602C_EXTADDR_VALID, 1);
	rtl9602c_flow_put(e, RTL9602C_EXTADDR_MODE, RTL9602C_EXTADDR_MODE_NAPT);
	rtl9602c_flow_put(e, RTL9602C_EXTADDR_NEXTHOP_IDX, nh);
	return rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_EXTADDR, idx, e,
				      extip_words);
}

/*
 * The switch port @dev is, or runs over: a GEM interface or a VLAN upper names
 * its lower device as its iflink. -1 for any other device.
 */
static int rtl9602c_l34_port_of(struct rtl9602c_priv *p, struct net_device *dev)
{
	struct net_device *lower;
	struct dsa_port *dp;
	int iflink;

	if (dsa_user_dev_check(dev)) {
		dp = dsa_port_from_netdev(dev);
		if (!IS_ERR(dp) && dp->ds == &p->ds)
			return dp->index;
		return -1;
	}

	iflink = dev_get_iflink(dev);
	if (!iflink || iflink == dev->ifindex)
		return -1;

	lower = __dev_get_by_index(dev_net(dev), iflink);
	if (!lower || !dsa_user_dev_check(lower))
		return -1;

	dp = dsa_port_from_netdev(lower);
	if (IS_ERR(dp) || dp->ds != &p->ds)
		return -1;

	return dp->index;
}

static int rtl9602c_l34_netif_row(struct rtl9602c_priv *p, int ifindex)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->l34_netif_ifindex); i++)
		if (p->l34_netif_ifindex[i] == ifindex)
			return i;
	return -1;
}

/*
 * Routes. Of the frames addressed to this device the engine forwards those of
 * translated flows only, and hands the rest to the CPU whatever the table says;
 * but a translated flow needs an on-link row for the subnet on each side, and a
 * row with action 0 hands its frames to the CPU. The table is rebuilt from a
 * shadow of the main routing table on every change. A route the engine cannot
 * follow becomes an action-0 row rather than none, as an absent row would not
 * shadow a shorter prefix.
 */

/*
 * Encode route @r for @dst/@prefix. The length field is the prefix length minus
 * one, so a /0 has no encoding of its own.
 */
static void rtl9602c_l34_route_encode(struct rtl9602c_priv *p,
				      const struct rtl9602c_l34_route *r,
				      __be32 dst, u8 prefix, u32 *e)
{
	unsigned int i;
	int netif, nh;

	rtl9602c_flow_put(e, RTL9602C_ROUTE_DST, be32_to_cpu(dst));
	rtl9602c_flow_put(e, RTL9602C_ROUTE_DST_LEN, prefix - 1);
	rtl9602c_flow_put(e, RTL9602C_ROUTE_VALID, 1);

	/* Blackhole: action 1, taken to drop in hardware (not verified). */
	if (r->flags & RTL9602C_L34_RT_DROP) {
		rtl9602c_flow_put(e, RTL9602C_ROUTE_ACTION,
				  RTL9602C_ROUTE_ACTION_DROP);
		return;
	}

	/*
	 * Action 0, for the CPU: a route Linux must decide, one whose nexthop
	 * is down, one on a device with no interface row.
	 */
	if (r->flags & RTL9602C_L34_RT_CPU)
		return;

	if (r->linkdown)
		return;

	netif = rtl9602c_l34_netif_row(p, r->ifindex);
	if (netif < 0)
		return;

	if (!r->gw) {
		/* On-link: the prefix is attached to the interface row. */
		rtl9602c_flow_put(e, RTL9602C_ROUTE_ACTION,
				  RTL9602C_ROUTE_ACTION_NEIGH);
		rtl9602c_flow_put(e, RTL9602C_ROUTE_L3IF_IDX, netif);

		/*
		 * A LAN-side route: on a switch port other than the PON port,
		 * which is the WAN side of a stick with one uplink. What the
		 * engine does with the bit is not verified.
		 */
		if (r->port >= 0 && r->port != RTL9602C_PORT_PON)
			rtl9602c_flow_put(e, RTL9602C_ROUTE_INTERNAL, 1);
		return;
	}

	/*
	 * Via a gateway: the next-hop row holding its address, else action 0
	 * until the neighbour resolves. The engine does not follow this row for
	 * translated frames, which leave by their external address's next hop.
	 */
	nh = -1;
	for (i = 0; i < ARRAY_SIZE(p->l34_nh_addr); i++) {
		if (p->l34_nh_addr[i] == r->gw) {
			nh = i;
			break;
		}
	}
	if (nh < 0)
		return;

	rtl9602c_flow_put(e, RTL9602C_ROUTE_ACTION, RTL9602C_ROUTE_ACTION_NEXTHOP);
	rtl9602c_flow_put(e, RTL9602C_ROUTE_NEXTHOP_IDX, nh);
}

/*
 * The next-hop row by which translated frames leave: the row of the gateway of
 * the default route, once its neighbour holds one; else row 0, which the first
 * neighbour on the PON port takes (see rtl9602c_l34_neigh_set()).
 */
static unsigned int rtl9602c_l34_uplink_nh(struct rtl9602c_priv *p)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(p->l34_route); i++) {
		const struct rtl9602c_l34_route *r = &p->l34_route[i];

		if (!r->used || r->prefix || !r->gw || r->linkdown)
			continue;
		for (j = 0; j < ARRAY_SIZE(p->l34_nh_addr); j++)
			if (p->l34_nh_addr[j] == r->gw)
				return j;
	}
	return 0;
}

static bool rtl9602c_l34_extip_of_wan(struct rtl9602c_priv *p, unsigned int idx)
{
	unsigned int w;

	for (w = 1; w < ARRAY_SIZE(p->l34_pppoe); w++)
		if (p->l34_pppoe[w].extip && p->l34_pppoe[w].extidx == (int)idx)
			return true;
	return false;
}

/*
 * Point the external-address rows of the addresses and of the flows at the
 * uplink's next hop, when it changed; a PPPoE WAN's row keeps its own.
 */
static void rtl9602c_l34_extip_repoint(struct rtl9602c_priv *p)
{
	unsigned int nh = rtl9602c_l34_uplink_nh(p);
	unsigned int i;

	if (nh == p->l34_extip_nh)
		return;
	p->l34_extip_nh = nh;
	for (i = 0; i < ARRAY_SIZE(p->l34_extip); i++)
		if (p->l34_extip[i] && !rtl9602c_l34_extip_of_wan(p, i))
			rtl9602c_l34_extip_set(p, i, p->l34_extip[i], nh);
	pr_debug("<l3> translated frames leave by next hop %u\n", nh);
}

/*
 * Rewrite the hardware table, longest prefix first, which is right whether the
 * engine takes the longest matching row or the first valid one; which it takes
 * is not verified. If the routes need more rows than the table has, the shadow
 * has overflowed or the namespace has FIB rules, which the table cannot
 * express, the table is written empty, every destination to the CPU, with one
 * warning. Rows that did not change are not written. The external-address
 * rows then follow the uplink's next hop, which a route or a neighbour may
 * have changed.
 */
static void rtl9602c_l34_route_commit(struct rtl9602c_priv *p)
{
	unsigned int rows = rtl9602c_nat_tbl_entries[RTL9602C_L3TBL_CTRL_TABLE_ROUTE];
	unsigned int words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_ROUTE];
	u32 hw[ARRAY_SIZE(p->l34_route_hw)][2] = {};
	unsigned int n = 0, i;
	bool bail;
	int prefix;

	if (WARN_ON(rows > ARRAY_SIZE(hw)))
		rows = ARRAY_SIZE(hw);

	bail = p->l34_route_over || fib4_has_custom_rules(&init_net);

	for (prefix = 32; prefix >= 0 && !bail; prefix--) {
		for (i = 0; i < ARRAY_SIZE(p->l34_route); i++) {
			const struct rtl9602c_l34_route *r = &p->l34_route[i];

			if (!r->used || r->prefix != prefix)
				continue;

			if (n + (prefix ? 1 : 2) > rows) {
				bail = true;
				break;
			}
			if (prefix) {
				rtl9602c_l34_route_encode(p, r, r->dst, prefix,
							  hw[n++]);
				continue;
			}
			/* A /0 as two /1 rows. */
			rtl9602c_l34_route_encode(p, r, 0, 1, hw[n++]);
			rtl9602c_l34_route_encode(p, r, htonl(0x80000000), 1,
						  hw[n++]);
		}
	}

	if (bail) {
		memset(hw, 0, sizeof(hw));
		if (!p->l34_route_warned)
			pr_warn("<l3> this routing table cannot be mirrored faithfully; every destination goes to the CPU\n");
		p->l34_route_warned = true;
	} else {
		p->l34_route_warned = false;
	}

	for (i = 0; i < rows; i++) {
		if (!memcmp(p->l34_route_hw[i], hw[i], words * sizeof(u32)))
			continue;
		if (rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_ROUTE, i,
					   hw[i], words)) {
			pr_warn("<l3> route[%u] did not take\n", i);
			continue;
		}
		memcpy(p->l34_route_hw[i], hw[i], words * sizeof(u32));
		pr_debug("<l3> route[%u] = %08x %08x\n", i, hw[i][0],
			 hw[i][1]);
	}

	/* The routes and the neighbours change the uplink's gateway too. */
	rtl9602c_l34_extip_repoint(p);
}

/*
 * Record @ev in the shadow, keyed by destination and prefix, and commit. A full
 * shadow stops hardware routing until the driver is reloaded, since the table
 * could no longer follow the kernel's.
 */
static void rtl9602c_l34_route_set(struct rtl9602c_priv *p,
				   const struct rtl9602c_l34_route *ev, bool add)
{
	struct rtl9602c_l34_route *free = NULL, *r;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->l34_route); i++) {
		r = &p->l34_route[i];
		if (r->used && r->dst == ev->dst && r->prefix == ev->prefix) {
			if (add)
				*r = *ev;
			r->used = add;
			goto commit;
		}
		if (!r->used && !free)
			free = r;
	}

	if (!add)
		return;

	if (!free) {
		p->l34_route_over = true;
		pr_warn("<l3> more than %zu routes; %pI4/%u was not recorded and hardware routing is off until this driver is reloaded\n",
			ARRAY_SIZE(p->l34_route), &ev->dst, ev->prefix);
		goto commit;
	}

	*free = *ev;
	free->used = true;
commit:
	rtl9602c_l34_route_commit(p);
}

/*
 * The route notifier can be called under rcu_read_lock(), and the table writes
 * sleep: the callback copies out what a row needs and queues it, and a work
 * item applies the queue under flow_lock.
 */
static void rtl9602c_l34_fib_work(struct work_struct *work)
{
	struct rtl9602c_priv *p =
		container_of(work, struct rtl9602c_priv, l34_fib_work);
	struct rtl9602c_l34_fib_ev *ev;

	for (;;) {
		spin_lock_bh(&p->l34_fib_lock);
		ev = list_first_entry_or_null(&p->l34_fib_q,
					      struct rtl9602c_l34_fib_ev, list);
		if (ev)
			list_del(&ev->list);
		spin_unlock_bh(&p->l34_fib_lock);
		if (!ev)
			return;

		mutex_lock(&p->flow_lock);
		if (ev->nh)
			rtl9602c_l34_nh_state(p, ev->route.ifindex,
					      ev->route.gw, !ev->add);
		else
			rtl9602c_l34_route_set(p, &ev->route, ev->add);
		mutex_unlock(&p->flow_lock);
		kfree(ev);
	}
}

static void rtl9602c_l34_fib_queue(struct rtl9602c_priv *p,
				   struct rtl9602c_l34_fib_ev *ev)
{
	spin_lock_bh(&p->l34_fib_lock);
	list_add_tail(&ev->list, &p->l34_fib_q);
	spin_unlock_bh(&p->l34_fib_lock);
	schedule_work(&p->l34_fib_work);
}

/*
 * A nexthop that becomes usable or unusable: the routes through it are written
 * again, as full rows or as CPU rows.
 */
static int rtl9602c_l34_fib_nh_event(struct rtl9602c_priv *p,
				     unsigned long event,
				     struct fib_notifier_info *info)
{
	struct fib_nh_notifier_info *fnh =
		container_of(info, struct fib_nh_notifier_info, info);
	const struct fib_nh *nh = fnh->fib_nh;
	struct rtl9602c_l34_fib_ev *ev;

	if (!nh || !nh->fib_nh_dev)
		return NOTIFY_DONE;

	ev = kzalloc(sizeof(*ev), GFP_ATOMIC);
	if (!ev)
		return NOTIFY_DONE;

	ev->nh = true;
	ev->add = event == FIB_EVENT_NH_ADD;
	ev->route.ifindex = nh->fib_nh_dev->ifindex;
	if (nh->fib_nh_gw_family == AF_INET)
		ev->route.gw = nh->fib_nh_gw4;

	rtl9602c_l34_fib_queue(p, ev);
	return NOTIFY_DONE;
}

/*
 * Mark the routes through @gw on @ifindex down or up. A route keeps its
 * interface, port and gateway while down, so that it becomes a full row again
 * once its nexthop is usable, a connected route learnt before its link came up
 * included.
 */
static void rtl9602c_l34_nh_state(struct rtl9602c_priv *p, int ifindex,
				  __be32 gw, bool down)
{
	bool changed = false;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->l34_route); i++) {
		struct rtl9602c_l34_route *r = &p->l34_route[i];

		if (!r->used || r->ifindex != ifindex || r->gw != gw ||
		    r->linkdown == down)
			continue;
		r->linkdown = down;
		changed = true;
	}
	if (changed)
		rtl9602c_l34_route_commit(p);
}

/*
 * IPv4 routes of the main table. A unicast route with one IPv4 or direct
 * nexthop, no nexthop object and no DSCP can be followed; a blackhole route
 * becomes a drop row; any other a CPU row.
 */
static int rtl9602c_l34_fib_event(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct rtl9602c_priv *p =
		container_of(nb, struct rtl9602c_priv, l34_fib_nb);
	struct fib_entry_notifier_info *fen;
	struct fib_notifier_info *info = ptr;
	struct rtl9602c_l34_fib_ev *ev;
	const struct fib_nh_common *nhc;
	struct fib_info *fi;

	if (info->family != AF_INET || !p->l34_up)
		return NOTIFY_DONE;

	switch (event) {
	case FIB_EVENT_ENTRY_REPLACE:
	case FIB_EVENT_ENTRY_APPEND:
	case FIB_EVENT_ENTRY_DEL:
		break;
	case FIB_EVENT_NH_ADD:
	case FIB_EVENT_NH_DEL:
		return rtl9602c_l34_fib_nh_event(p, event, info);
	default:
		return NOTIFY_DONE;
	}

	fen = container_of(info, struct fib_entry_notifier_info, info);
	fi = fen->fi;
	if (!fi || fen->tb_id != RT_TABLE_MAIN)
		return NOTIFY_DONE;
	if (fen->dst_len > 32)
		return NOTIFY_DONE;

	ev = kzalloc(sizeof(*ev), GFP_ATOMIC);
	if (!ev)
		return NOTIFY_DONE;

	ev->add = event != FIB_EVENT_ENTRY_DEL;
	ev->route.dst = htonl(fen->dst);
	ev->route.prefix = fen->dst_len;
	ev->route.port = -1;

	switch (fen->type) {
	case RTN_UNICAST:
		break;
	case RTN_BLACKHOLE:
		ev->route.flags = RTL9602C_L34_RT_DROP;
		goto queue;
	default:
		ev->route.flags = RTL9602C_L34_RT_CPU;
		goto queue;
	}

	/* A nexthop object, several nexthops or a DSCP: Linux decides. */
	if (fi->nh || fi->fib_nhs != 1 || fen->dscp) {
		ev->route.flags = RTL9602C_L34_RT_CPU;
		goto queue;
	}

	nhc = &fib_info_nh(fi, 0)->nh_common;
	if (!nhc->nhc_dev ||
	    (nhc->nhc_gw_family && nhc->nhc_gw_family != AF_INET)) {
		ev->route.flags = RTL9602C_L34_RT_CPU;
		goto queue;
	}

	/* Dead or without carrier: a CPU row until FIB_EVENT_NH_ADD. */
	ev->route.linkdown = !!(nhc->nhc_flags & (RTNH_F_DEAD | RTNH_F_LINKDOWN));

	ev->route.ifindex = nhc->nhc_dev->ifindex;
	ev->route.port = rtl9602c_l34_port_of(p, nhc->nhc_dev);
	if (nhc->nhc_gw_family == AF_INET)
		ev->route.gw = nhc->nhc_gw.ipv4;

queue:
	rtl9602c_l34_fib_queue(p, ev);
	return NOTIFY_DONE;
}

/*
 * Whether @dev is a GEM interface or runs over one, a ppp device on a GEM
 * interface for instance, and if so its GEM flow: the last device on its
 * forward path is asked.
 */
static bool rtl9602c_l34_gem_under(struct net_device *dev, unsigned int *flow)
{
	static const u8 nowhere[ETH_ALEN];
	struct net_device_path_stack stack;
	const struct net_device *last;

	if (pon_gem_netdev_flow(dev, flow))
		return true;

	if (dev_fill_forward_path(dev, nowhere, &stack) < 0 || !stack.num_paths)
		return false;

	last = stack.path[stack.num_paths - 1].dev;
	return last && pon_gem_netdev_flow(last, flow);
}

/* Whether @dev has an IPv4 address other than @ifa. Context: RTNL held. */
static bool rtl9602c_l34_other_addr(struct net_device *dev,
				    struct in_ifaddr *ifa)
{
	struct in_device *in_dev = __in_dev_get_rtnl(dev);
	struct in_ifaddr *other;

	if (in_dev)
		in_dev_for_each_ifa_rtnl(other, in_dev)
			if (other != ifa)
				return true;
	return false;
}

/*
 * Claim the GEM flow a routed interface sends in as the routed flow, which the
 * ports outside the GEM bridge are given as their stream. One routed flow at a
 * time: an address on a device over another GEM flow claims nothing, with a
 * warning. Each device over the flow holds one reference, taken with its first
 * address and given back with its last; the last reference gives the flow up.
 */
static void rtl9602c_l34_us_update(struct rtl9602c_priv *p,
				   struct net_device *dev, unsigned long event,
				   struct in_ifaddr *ifa)
{
	unsigned int flow;
	bool gem;

	rcu_read_lock();
	gem = rtl9602c_l34_gem_under(dev, &flow);
	rcu_read_unlock();
	if (!gem)
		return;

	if (event == NETDEV_UP) {
		if (p->l34_us_flow >= 0 && p->l34_us_flow != (int)flow) {
			pr_warn("<l3> %s is routed out of GEM flow %u, but flow %d already is; only one routed GEM port can be offloaded\n",
				dev->name, flow, p->l34_us_flow);
			return;
		}
		if (rtl9602c_l34_other_addr(dev, ifa))
			return;
		p->l34_us_refs++;
		if (p->l34_us_flow == (int)flow)
			return;
		p->l34_us_flow = flow;
		pr_debug("<l3> %s is routed out of GEM flow %u\n", dev->name,
			 flow);
		rtl9602c_cf_sid_refresh(p);
		return;
	}

	if (p->l34_us_flow != (int)flow || !p->l34_us_refs)
		return;
	if (rtl9602c_l34_other_addr(dev, ifa))
		return;
	if (--p->l34_us_refs)
		return;
	p->l34_us_flow = -1;
	pr_debug("<l3> %s no longer routes out of GEM flow %u\n", dev->name,
		 flow);
	rtl9602c_cf_sid_refresh(p);
}

/*
 * A flow may leave by a GEM interface only if that interface's GEM flow is the
 * routed flow and its upstream is ready. Other devices pass.
 */
static bool rtl9602c_l34_egress_ok(struct rtl9602c_priv *p,
				   struct net_device *dev)
{
	unsigned int flow;

	if (!pon_gem_netdev_flow(dev, &flow))
		return true;

	return READ_ONCE(p->l34_us_flow) == (int)flow &&
	       rtl9602c_gem_us_ready(p, flow);
}

/*
 * The address notifier: the routed flow first; then, for a device that is or
 * runs over a switch port, its L3-interface row, one per device. An address
 * added writes the row with the device's MAC, VLAN, address and MTU, replacing
 * an earlier address, and becomes an external address with the uplink's next
 * hop (rtl9602c_l34_uplink_nh()), where a translated source is looked up. An
 * address removed gives its external-address row back, and rewrites the
 * interface row with one that remains, or clears it.
 * A later change of the MAC address or the MTU is written by
 * rtl9602c_l34_netdev_event().
 */
static int rtl9602c_l34_inetaddr_event(struct notifier_block *nb,
				       unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = ptr;
	struct rtl9602c_priv *p;
	struct net_device *dev;
	unsigned int i, free = UINT_MAX;
	int port, row = -1;

	p = container_of(nb, struct rtl9602c_priv, l34_inetaddr_nb);
	dev = ifa->ifa_dev->dev;

	if (event != NETDEV_UP && event != NETDEV_DOWN)
		return NOTIFY_DONE;
	if (!p->l34_up)
		return NOTIFY_DONE;

	rtl9602c_l34_us_update(p, dev, event, ifa);

	port = rtl9602c_l34_port_of(p, dev);
	if (port < 0)
		return NOTIFY_DONE;

	mutex_lock(&p->flow_lock);

	for (i = 0; i < ARRAY_SIZE(p->l34_netif_ifindex); i++) {
		if (p->l34_netif_ifindex[i] == dev->ifindex)
			row = i;
		else if (!p->l34_netif_ifindex[i] && free == UINT_MAX)
			free = i;
	}

	if (event == NETDEV_DOWN) {
		struct in_device *in_dev = __in_dev_get_rtnl(dev);
		struct in_ifaddr *other, *keep = NULL;

		rtl9602c_extip_put(p, ntohl(ifa->ifa_address));
		if (row < 0)
			goto out;

		if (in_dev) {
			in_dev_for_each_ifa_rtnl(other, in_dev) {
				if (other != ifa) {
					keep = other;
					break;
				}
			}
		}

		if (keep) {
			rtl9602c_l34_netif_set(p, row, dev->dev_addr,
					       is_vlan_dev(dev) ?
					       vlan_dev_vlan_id(dev) : 0,
					       keep->ifa_address, dev->mtu);
			pr_debug("<l3> %s lost %pI4, l3if[%d] now %pI4\n",
				 dev->name, &ifa->ifa_address, row,
				 &keep->ifa_address);
			goto out;
		}

		rtl9602c_l34_netif_clear(p, row);
		p->l34_netif_ifindex[row] = 0;
		pr_debug("<l3> %s lost %pI4, l3if[%d] cleared\n", dev->name,
			 &ifa->ifa_address, row);
		goto out;
	}

	if (row < 0) {
		if (free == UINT_MAX) {
			pr_warn("<l3> no L3 interface row left for %s; it will not be routed in hardware\n",
				dev->name);
			goto out;
		}
		row = free;
	}

	if (rtl9602c_l34_netif_set(p, row, dev->dev_addr,
				   is_vlan_dev(dev) ? vlan_dev_vlan_id(dev) : 0,
				   ifa->ifa_address, dev->mtu)) {
		pr_warn("<l3> l3if[%d] for %s did not take\n", row, dev->name);
		goto out;
	}
	p->l34_netif_ifindex[row] = dev->ifindex;

	rtl9602c_extip_get(p, ntohl(ifa->ifa_address));

	pr_debug("<l3> %s has %pI4, l3if[%d] on port %d\n", dev->name,
		 &ifa->ifa_address, row, port);
out:
	/* A connected route names the row: commit the routes again. */
	rtl9602c_l34_route_commit(p);
	mutex_unlock(&p->flow_lock);
	return NOTIFY_DONE;
}

static int rtl9602c_extip_find(struct rtl9602c_priv *p, u32 addr)
{
	unsigned int i;

	for (i = 0; i < rtl9602c_nat_tbl_entries[RTL9602C_L3TBL_CTRL_TABLE_EXTADDR]; i++) {
		u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};

		if (rtl9602c_nat_tbl_read(p, RTL9602C_L3TBL_CTRL_TABLE_EXTADDR, i, e,
					  rtl9602c_nat_tbl_words
						[RTL9602C_L3TBL_CTRL_TABLE_EXTADDR]))
			return -EIO;
		if (rtl9602c_flow_get(e, RTL9602C_EXTADDR_VALID) &&
		    rtl9602c_flow_get(e, RTL9602C_EXTADDR_IP) == addr)
			return i;
	}
	return -ENOENT;
}

static int rtl9602c_extip_get(struct rtl9602c_priv *p, u32 addr)
{
	unsigned int i;
	int idx;

	idx = rtl9602c_extip_find(p, addr);
	if (idx >= 0)
		return idx;

	for (i = 0; i < ARRAY_SIZE(p->l34_extip); i++)
		if (!p->l34_extip[i])
			break;
	if (i == ARRAY_SIZE(p->l34_extip))
		return -ENOSPC;

	if (rtl9602c_l34_extip_set(p, i, cpu_to_be32(addr), p->l34_extip_nh))
		return -EIO;
	p->l34_extip[i] = cpu_to_be32(addr);
	pr_debug("<flow> extaddr[%u] := %pI4, taken for a flow\n", i, &addr);
	return (int)i;
}

/*
 * Give back the external-address row the driver took for @addr, if it took
 * one: the address is gone, and a flow translated to it is dead with it.
 */
static void rtl9602c_extip_put(struct rtl9602c_priv *p, u32 addr)
{
	u32 zero[RTL9602C_L3TBL_DATA_WORDS] = {};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->l34_extip); i++) {
		if (p->l34_extip[i] != cpu_to_be32(addr))
			continue;
		rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_EXTADDR, i,
				       zero,
				       rtl9602c_nat_tbl_words
					       [RTL9602C_L3TBL_CTRL_TABLE_EXTADDR]);
		p->l34_extip[i] = 0;
		pr_debug("<flow> extaddr[%u] := free, %pI4 is gone\n", i,
			 &addr);
	}
}

/* The session a flow is pushed into, or -1; only the upstream rule has one. */
static int rtl9602c_flow_pppoe_of(struct flow_rule *rule)
{
	struct flow_action_entry *act;
	int i;

	flow_action_for_each(i, act, &rule->action)
		if (act->id == FLOW_ACTION_PPPOE_PUSH)
			return (int)act->pppoe.sid;
	return -1;
}

/*
 * The two Ethernet addresses of a flow leaving over PPPoE, from four mangles of
 * the Ethernet header: at offset 0 with mask 0, destination bytes 0 to 3; at 4
 * with mask 0xffff0000, destination bytes 4 and 5, in the value's low half; at
 * 4 with mask 0x0000ffff, source bytes 0 and 1, in its high half; at 8 with
 * mask 0, source bytes 2 to 5. The values hold the bytes in memory order, which
 * the big-endian stores put back. -EOPNOTSUPP unless all four are there.
 */
static int rtl9602c_flow_eth_of(struct flow_rule *rule, u8 *dmac, u8 *smac)
{
	struct flow_action_entry *act;
	unsigned int have = 0;
	int i;

	flow_action_for_each(i, act, &rule->action) {
		if (act->id != FLOW_ACTION_MANGLE ||
		    act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_ETH)
			continue;

		if (act->mangle.offset == 0 && act->mangle.mask == 0) {
			put_unaligned_be32(act->mangle.val, dmac);
			have |= BIT(0);
		} else if (act->mangle.offset == 4 &&
			   act->mangle.mask == 0xffff0000) {
			put_unaligned_be16(act->mangle.val & 0xffff, dmac + 4);
			have |= BIT(1);
		} else if (act->mangle.offset == 4 &&
			   act->mangle.mask == 0x0000ffff) {
			put_unaligned_be16(act->mangle.val >> 16, smac);
			have |= BIT(2);
		} else if (act->mangle.offset == 8 && act->mangle.mask == 0) {
			put_unaligned_be32(act->mangle.val, smac + 2);
			have |= BIT(3);
		}
	}

	return have == (BIT(0) | BIT(1) | BIT(2) | BIT(3)) ? 0 : -EOPNOTSUPP;
}

static bool rtl9602c_flow_has(struct flow_rule *rule, enum flow_action_id id)
{
	struct flow_action_entry *act;
	int i;

	flow_action_for_each(i, act, &rule->action)
		if (act->id == id)
			return true;
	return false;
}

static struct net_device *rtl9602c_flow_dev_of(struct flow_rule *rule)
{
	struct flow_action_entry *act;
	int i;

	flow_action_for_each(i, act, &rule->action)
		if (act->id == FLOW_ACTION_REDIRECT)
			return act->dev;
	return NULL;
}

/*
 * PPPoE WANs. The engine only encapsulates: a session is a 16-bit ID in a row,
 * and discovery, LCP, authentication and IPCP run in software. A WAN takes one
 * index w, 1 to 7, in the L3-interface, next-hop and session tables, row 0
 * staying the Ethernet uplink's, and its external address is pointed at next
 * hop w. A WAN is keyed by that address and outlives its flows; it is recycled
 * only when a new address needs its index.
 */
static void rtl9602c_l34_wan_forget_extip(struct rtl9602c_priv *p, int idx)
{
	u32 zero[RTL9602C_L3TBL_DATA_WORDS] = {};

	rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_EXTADDR, idx, zero,
			       rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_EXTADDR]);
	p->l34_extip[idx] = 0;
}

/*
 * Session row @idx: the ID the engine writes into the PPPoE header. A row has
 * no valid bit; a free row holds session 0, the SESSION_ID of the discovery
 * stage, which never names an established session (RFC 2516, section 4).
 */
static int rtl9602c_l34_pppoe_set(struct rtl9602c_priv *p, unsigned int idx,
				  u16 sid)
{
	unsigned int words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_PPPOE];
	u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};

	rtl9602c_flow_put(e, RTL9602C_PPPOE_ENTRY_SESSION, sid);
	return rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_PPPOE, idx, e, words);
}

/*
 * The next-hop address marking a row held by a PPPoE WAN, one no neighbour can
 * have; the WAN's L3-interface row is marked with ifindex -1.
 */
#define RTL9602C_NH_HELD_BY_PPPOE	cpu_to_be32(INADDR_BROADCAST)

static bool rtl9602c_l34_wan_row_free(struct rtl9602c_priv *p, unsigned int w)
{
	return !p->l34_pppoe[w].extip && !p->l34_netif_ifindex[w] &&
	       !p->l34_nh_addr[w];
}

/*
 * Undo WAN @w: its external-address row is freed if this path took it and
 * otherwise given the uplink's next hop again; its next-hop, L3-interface and
 * session rows are zeroed.
 */
static void rtl9602c_l34_wan_release(struct rtl9602c_priv *p, unsigned int w)
{
	unsigned int nh_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP];
	unsigned int ext_words =
		rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_EXTADDR];
	struct rtl9602c_pppoe_wan *r = &p->l34_pppoe[w];
	u32 zero[RTL9602C_L3TBL_DATA_WORDS] = {};

	if (r->extip && r->extidx >= 0) {
		if (r->took_extip) {
			rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_EXTADDR,
					       r->extidx, zero, ext_words);
			p->l34_extip[r->extidx] = 0;
		} else {
			rtl9602c_l34_extip_set(p, r->extidx, r->extip,
					       p->l34_extip_nh);
		}
	}
	rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP, w, zero, nh_words);
	rtl9602c_l34_netif_clear(p, w);
	rtl9602c_l34_pppoe_set(p, w, 0);

	p->l34_netif_ifindex[w] = 0;
	p->l34_nh_addr[w] = 0;
	rtl9602c_l34_nh_l2_set(p, w, NULL, 0);
	pr_debug("<flow> PPPoE WAN %u released\n", w);
	memset(r, 0, sizeof(*r));
}

static int rtl9602c_l34_wan_alloc(struct rtl9602c_priv *p)
{
	unsigned int w;

	for (w = 1; w < ARRAY_SIZE(p->l34_pppoe); w++)
		if (rtl9602c_l34_wan_row_free(p, w))
			return w;

	for (w = 1; w < ARRAY_SIZE(p->l34_pppoe); w++) {
		if (!p->l34_pppoe[w].extip || p->l34_pppoe[w].refs)
			continue;
		rtl9602c_l34_wan_release(p, w);
		if (rtl9602c_l34_wan_row_free(p, w))
			return w;
	}
	return -ENOSPC;
}

/*
 * The WAN for a flow's session. A WAN already on @extip is kept when session,
 * port and concentrator agree, and rewritten in place when they do not. Its
 * rows: the session; an address-table entry for the concentrator's MAC on the
 * egress port, under VID 0, since PPP has no neighbour to put one there; the
 * L3-interface row, with the egress port's MAC; the next hop, encapsulating,
 * naming that row, the session and the entry; and the external address, pointed
 * at the next hop. A failure undoes all of it. Returns w, or a negative error
 * code.
 */
static int rtl9602c_l34_wan_get(struct rtl9602c_priv *p,
				const struct rtl9602c_flow_pppoe *ppp,
				u32 extip, int extidx, bool took_extip)
{
	unsigned int nh_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP];
	u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};
	__be32 addr = cpu_to_be32(extip);
	struct rtl9602c_pppoe_wan *r;
	unsigned int slot, mtu;
	u16 vid = 0;
	int port, w, err;

	port = rtl9602c_l34_port_of(p, ppp->dev);
	if (port < 0)
		return -ENODEV;

	for (w = 1; w < (int)ARRAY_SIZE(p->l34_pppoe); w++)
		if (p->l34_pppoe[w].extip == addr)
			break;
	if (w < (int)ARRAY_SIZE(p->l34_pppoe)) {
		r = &p->l34_pppoe[w];
		if (r->sid == ppp->sid && r->port == port &&
		    ether_addr_equal(r->dmac, ppp->dmac))
			return w;

		took_extip = r->took_extip;
	} else {
		w = rtl9602c_l34_wan_alloc(p);
		if (w < 0)
			return w;
		r = &p->l34_pppoe[w];
	}

	err = rtl9602c_l34_pppoe_set(p, w, ppp->sid);
	if (err)
		goto fail;

	err = rtl9602c_l2_nexthop_place(p, ppp->dmac, port, &vid, &slot);
	if (err)
		goto fail;
	rtl9602c_l34_nh_l2_set(p, w, ppp->dmac, vid);

	/*
	 * The PPPoE header and the PPP protocol field take 8 octets: at most
	 * 1492 over a 1500-octet payload (RFC 2516, section 7).
	 */
	mtu = ppp->dev->mtu > PPPOE_SES_HLEN ? ppp->dev->mtu - PPPOE_SES_HLEN
					     : ppp->dev->mtu;
	err = rtl9602c_l34_netif_set(p, w, ppp->smac, vid, addr, mtu);
	if (err)
		goto fail;

	rtl9602c_flow_put(e, RTL9602C_NEXTHOP_ENCAP, RTL9602C_NEXTHOP_ENCAP_PPPOE);
	rtl9602c_flow_put(e, RTL9602C_NEXTHOP_L3IF_IDX, w);
	rtl9602c_flow_put(e, RTL9602C_NEXTHOP_PPPOE_IDX, w);
	rtl9602c_flow_put(e, RTL9602C_NEXTHOP_L2_IDX, slot);
	err = rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP, w, e,
				     nh_words);
	if (err)
		goto fail;

	err = rtl9602c_l34_extip_set(p, extidx, addr, w);
	if (err)
		goto fail;

	p->l34_netif_ifindex[w] = -1;
	p->l34_nh_addr[w] = RTL9602C_NH_HELD_BY_PPPOE;
	r->extip = addr;
	r->sid = ppp->sid;
	r->port = port;
	r->extidx = extidx;
	r->took_extip = took_extip;
	ether_addr_copy(r->dmac, ppp->dmac);

	pr_debug("<flow> PPPoE WAN %d: session %d to %pM on port %d, l3if/nexthop/pppoe[%d], extaddr[%d] nexthop %d, %pI4 mtu %u, L2 slot %u\n",
		 w, ppp->sid, ppp->dmac, port, w, extidx, w, &extip, mtu, slot);
	return w;

fail:
	pr_debug("<flow> PPPoE WAN %d did not come up: %d\n", w, err);
	rtl9602c_l34_wan_release(p, w);
	return err;
}

/*
 * Program one flow: an inbound entry, which translates the replies back, and an
 * outbound entry pointing at it, each in a free way of the bucket its hash
 * gives. The inbound entry is written first: the engine reads the tables all
 * the time, and a pointer written first could name an entry that still holds
 * another flow. Free ways are read from the hardware, since entries survive a
 * warm restart. That the engine searches every way of a bucket is not verified.
 * A cookie already held reuses its slot, its old entries zeroed first:
 * netfilter offers a flow once to each device of its flowtable.
 */
static int rtl9602c_l34_flow_add(struct rtl9602c_priv *p, unsigned long cookie,
				 u32 lip, u16 lport, u32 rip, u16 rport,
				 u32 extip, u16 extport, bool tcp,
				 const struct rtl9602c_flow_pppoe *ppp,
				 int port)
{
	u32 napt[RTL9602C_L3TBL_DATA_WORDS] = {};
	u32 naptr[RTL9602C_L3TBL_DATA_WORDS] = {};
	u32 zero[RTL9602C_L3TBL_DATA_WORDS] = {};
	unsigned int naptr_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN];
	unsigned int bucket, way, slot, i, inbucket, rway, free_way;
	unsigned int naptr_idx = 0;
	unsigned int wan = 0, old_wan = 0, old_ports = 0;
	int extidx, err = -ENOSPC;

	bucket = rtl9602c_napt_bucket(lip, lport, rip, rport, tcp);
	inbucket = rtl9602c_napt_in_bucket(extip, extport, tcp);

	mutex_lock(&p->flow_lock);

	/* PPPoE: an external-address row for the address, then the WAN. */
	if (ppp->sid >= 0) {
		bool took = rtl9602c_extip_find(p, extip) < 0;
		int w;

		extidx = rtl9602c_extip_get(p, extip);
		if (extidx < 0) {
			err = extidx;
			goto out;
		}
		w = rtl9602c_l34_wan_get(p, ppp, extip, extidx, took);
		if (w < 0) {
			if (took)
				rtl9602c_l34_wan_forget_extip(p, extidx);
			err = w;
			goto out;
		}
		wan = w;
	} else {
		/* Ethernet: the address must already have a row. */
		extidx = rtl9602c_extip_find(p, extip);
		if (extidx < 0) {
			pr_debug("<flow> %pI4 is not an external address, declining\n",
				 &extip);
			mutex_unlock(&p->flow_lock);
			return extidx;
		}
	}

	/* The cookie's slot, its old entries zeroed, or a free one. */
	slot = UINT_MAX;
	for (i = 0; i < ARRAY_SIZE(p->l34_flow); i++)
		if (p->l34_flow[i].used && p->l34_flow[i].cookie == cookie) {
			slot = i;
			break;
		}
	if (slot != UINT_MAX) {
		old_wan = p->l34_flow[slot].wan;
		old_ports = p->l34_flow[slot].ports;
		rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT,
				       p->l34_flow[slot].napt, zero, 1);
		rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN,
				       p->l34_flow[slot].naptr, zero,
				       naptr_words);
	} else {
		for (i = 0; i < ARRAY_SIZE(p->l34_flow); i++)
			if (!p->l34_flow[i].used)
				break;
		if (i == ARRAY_SIZE(p->l34_flow))
			goto out;
		slot = i;
	}

	for (way = 0; way < RTL9602C_NAPT_WAYS; way++) {
		u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};
		unsigned int idx = bucket * RTL9602C_NAPT_WAYS + way;

		if (rtl9602c_nat_tbl_read(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT, idx, e, 1)) {
			err = -EIO;
			goto out;
		}
		if (!rtl9602c_flow_get(e, RTL9602C_NAPT_VALID))
			break;
	}
	if (way == RTL9602C_NAPT_WAYS) {
		pr_debug("<flow> bucket %u is full\n", bucket);
		goto out;
	}

	free_way = RTL9602C_NAPT_WAYS;
	for (rway = 0; rway < RTL9602C_NAPT_WAYS; rway++) {
		u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};
		unsigned int idx = inbucket * RTL9602C_NAPT_WAYS + rway;

		if (rtl9602c_nat_tbl_read(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN, idx, e,
					  naptr_words)) {
			err = -EIO;
			goto out;
		}
		if (!rtl9602c_flow_get(e, RTL9602C_NAPT_IN_VALID)) {
			if (free_way == RTL9602C_NAPT_WAYS)
				free_way = rway;
			continue;
		}
		/*
		 * The remote end is no input of the inbound hash and the engine
		 * ignores the digest, so replies to two flows sharing external
		 * port, address row and protocol could not be told apart: this
		 * flow stays in software. That entries differing in these are
		 * told apart is not verified.
		 */
		if (rtl9602c_flow_get(e, RTL9602C_NAPT_IN_EXTERNAL_PORT) == extport &&
		    rtl9602c_flow_get(e, RTL9602C_NAPT_IN_EXTADDR_IDX) == extidx &&
		    rtl9602c_flow_get(e, RTL9602C_NAPT_IN_TCP) == tcp) {
			pr_debug("<flow> %pI4:%u is napt-in %u's external port too; declining, the engine could not tell their replies apart\n",
				 &extip, extport, idx);
			goto out;
		}
	}
	if (free_way == RTL9602C_NAPT_WAYS) {
		pr_debug("<flow> inbound bucket %u is full\n", inbucket);
		goto out;
	}
	naptr_idx = inbucket * RTL9602C_NAPT_WAYS + free_way;

	rtl9602c_flow_put(naptr, RTL9602C_NAPT_IN_INTERNAL_ADDR, lip);
	rtl9602c_flow_put(naptr, RTL9602C_NAPT_IN_INTERNAL_PORT, lport);
	rtl9602c_flow_put(naptr, RTL9602C_NAPT_IN_REMOTE_HASH,
			  rtl9602c_napt_rem_hash(rip, rport));
	rtl9602c_flow_put(naptr, RTL9602C_NAPT_IN_EXTADDR_IDX, extidx);
	rtl9602c_flow_put(naptr, RTL9602C_NAPT_IN_EXTERNAL_PORT, extport);
	rtl9602c_flow_put(naptr, RTL9602C_NAPT_IN_TCP, tcp);
	rtl9602c_flow_put(naptr, RTL9602C_NAPT_IN_VALID, RTL9602C_NAPT_IN_VALID_ACTIVE);

	err = rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN, naptr_idx, naptr,
				     naptr_words);
	if (err)
		goto out;

	rtl9602c_flow_put(napt, RTL9602C_NAPT_INBOUND_IDX, naptr_idx);
	rtl9602c_flow_put(napt, RTL9602C_NAPT_VALID, 1);
	err = rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT,
				     bucket * RTL9602C_NAPT_WAYS + way, napt, 1);
	if (err)
		goto out;

	if (wan)
		p->l34_pppoe[wan].refs++;
	if (old_wan && p->l34_pppoe[old_wan].refs)
		p->l34_pppoe[old_wan].refs--;

	p->l34_flow[slot].cookie = cookie;
	p->l34_flow[slot].napt = bucket * RTL9602C_NAPT_WAYS + way;
	p->l34_flow[slot].naptr = naptr_idx;
	p->l34_flow[slot].wan = wan;

	/* Owned by every port whose block offered it. */
	p->l34_flow[slot].ports = old_ports | (port >= 0 ? BIT(port) : 0);
	p->l34_flow[slot].used = true;

	pr_debug("<flow> %pI4:%u -> %pI4:%u as %pI4:%u, slot %u, bucket %u way %u, napt-in %u (inbound bucket %u), digest %04x\n",
		 &lip, lport, &rip, rport, &extip, extport, slot, bucket, way,
		 naptr_idx, inbucket, rtl9602c_napt_rem_hash(rip, rport));
out:
	mutex_unlock(&p->flow_lock);
	return err;
}

/*
 * The NAPT hit bitmap has one bit per outbound entry, set by a frame of the
 * flow in either direction. Reading a word clears all 32 of its bits, so every
 * word read is ORed into l34_trf and a flow's bit is taken from there. Returns
 * whether @napt was hit since its bit was last taken.
 */
static bool rtl9602c_l34_trf_take(struct rtl9602c_priv *p, unsigned int napt)
{
	unsigned int w = napt / 32;
	u32 bit = BIT(napt % 32);
	bool used;

	p->l34_trf[w] |= rtl9602c_r(p, RTL9602C_NAPT_HIT + 4 * w);
	used = p->l34_trf[w] & bit;
	p->l34_trf[w] &= ~bit;
	return used;
}

/*
 * Free slot @i: the outbound entry zeroed, then the inbound one; the flow's hit
 * bit taken; the PPPoE WAN's reference dropped, its rows kept for the next
 * flow.
 */
static int rtl9602c_l34_flow_free(struct rtl9602c_priv *p, unsigned int i)
{
	unsigned int naptr_words =
		rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN];
	u32 zero[RTL9602C_L3TBL_DATA_WORDS] = {};
	unsigned int wan = p->l34_flow[i].wan;
	int e;

	e = rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT,
				   p->l34_flow[i].napt, zero, 1);
	if (!e)
		e = rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN,
					   p->l34_flow[i].naptr, zero,
					   naptr_words);
	if (wan && p->l34_pppoe[wan].refs)
		p->l34_pppoe[wan].refs--;
	rtl9602c_l34_trf_take(p, p->l34_flow[i].napt);
	p->l34_flow[i].wan = 0;
	p->l34_flow[i].ports = 0;
	p->l34_flow[i].used = false;
	return e;
}

/*
 * netfilter tears an offloaded flow down 30 s after its last reported use. A
 * flow whose hit bit is set was used, and lastused becomes now; otherwise
 * lastused is left alone, since both blocks of a flow ask in turn and the
 * second finds the bit taken. A bit left set before the driver loaded counts as
 * one use. That the engine never removes an entry by itself is assumed, not
 * verified.
 */
static int rtl9602c_l34_flow_stats(struct rtl9602c_priv *p,
				   struct flow_cls_offload *cls)
{
	bool held = false, used = false;
	unsigned int i;

	mutex_lock(&p->flow_lock);
	for (i = 0; i < ARRAY_SIZE(p->l34_flow); i++) {
		if (!p->l34_flow[i].used || p->l34_flow[i].cookie != cls->cookie)
			continue;
		held = true;
		used |= rtl9602c_l34_trf_take(p, p->l34_flow[i].napt);
	}
	mutex_unlock(&p->flow_lock);

	if (!held)
		return -EOPNOTSUPP;

	if (used)
		cls->stats.lastused = jiffies;
	return 0;
}

static int rtl9602c_l34_flow_del(struct rtl9602c_priv *p, unsigned long cookie)
{
	unsigned int i;
	int err = -ENOENT;

	mutex_lock(&p->flow_lock);
	for (i = 0; i < ARRAY_SIZE(p->l34_flow); i++) {
		int e;

		if (!p->l34_flow[i].used || p->l34_flow[i].cookie != cookie)
			continue;
		e = rtl9602c_l34_flow_free(p, i);
		if (err == -ENOENT || !err)
			err = e;
	}
	mutex_unlock(&p->flow_lock);
	return err;
}

/*
 * The flow block of @port is gone: the port no longer owns its flows, and a
 * flow no remaining block owns is freed.
 */
static void rtl9602c_l34_flows_release_port(struct rtl9602c_priv *p, int port)
{
	unsigned int i, freed = 0;

	mutex_lock(&p->flow_lock);
	for (i = 0; i < ARRAY_SIZE(p->l34_flow); i++) {
		if (!p->l34_flow[i].used || !(p->l34_flow[i].ports & BIT(port)))
			continue;
		p->l34_flow[i].ports &= ~BIT(port);
		if (p->l34_flow[i].ports)
			continue;
		rtl9602c_l34_flow_free(p, i);
		freed++;
	}
	mutex_unlock(&p->flow_lock);
	if (freed)
		pr_debug("<flow> block for port %d freed, %u flow(s) released with it\n",
			 port, freed);
}

/*
 * qos: the words of the blocks that setup returns to their reset values, raw.
 * Per port: the priority-to-queue word, the scheduler's weighted-queue bits,
 * the word that holds queue 0's weight (two ports share it), the three that
 * hold the weights of queues 1 to 7, the egress shaper and the ingress
 * policer. Then the ACL's port enables and default permits, the classifier's
 * control word, port 0's 2.5G override, which holds its shaper's rate as
 * well, each port's per-queue packet-rate enables and meter indices, the
 * DSCP-to-priority map, the port-priority word and the priority-selection
 * weights. Last, storm control: each class's enable and meter-select words,
 * and the rate word of each of the sixteen meters.
 */
static int rtl9602c_qos_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int port, i;

	seq_puts(s, "port  prio>queue  wfq       q0 weight q1-7 weights             shaper    policer\n");
	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		u32 w = RTL9602C_SCHED_WEIGHT_Q1_7 + 12 * port;

		seq_printf(s, "%4u  %08x    %08x  %08x  %08x %08x %08x  %08x  %08x\n",
			   port,
			   rtl9602c_r(p, RTL9602C_QOS_PRIO_TO_QUEUE + 4 * port),
			   rtl9602c_r(p, RTL9602C_SCHED_WFQ_QUEUES + 4 * port),
			   rtl9602c_r(p, RTL9602C_SCHED_WEIGHT_Q0 + 4 * (port / 2)),
			   rtl9602c_r(p, w), rtl9602c_r(p, w + 4),
			   rtl9602c_r(p, w + 8),
			   rtl9602c_r(p, RTL9602C_SHAPE_PORT_CFG + 4 * port),
			   rtl9602c_r(p, RTL9602C_POLICE_PORT(port)));
	}
	seq_printf(s, "acl port enable %08x  default permit %08x\n",
		   rtl9602c_r(p, RTL9602C_ACL_PORT_EN),
		   rtl9602c_r(p, RTL9602C_ACL_DEFAULT_PERMIT));
	seq_printf(s, "cls control %08x\n", rtl9602c_r(p, RTL9602C_CLS_CTRL));
	seq_printf(s, "port 0 2.5G override %08x\n",
		   rtl9602c_r(p, RTL9602C_SHAPE_2G5_CFG));
	for (i = 0; i < RTL9602C_NUM_PORTS; i++)
		seq_printf(s, "port %u queue rate enable %08x  meters %08x\n", i,
			   rtl9602c_r(p, RTL9602C_QUEUE_RATE_EN(i)),
			   rtl9602c_r(p, RTL9602C_QUEUE_RATE_METER(i)));
	seq_puts(s, "dscp>prio");
	for (i = 0; i < DIV_ROUND_UP(RTL9602C_QOS_DSCP_COUNT,
				     RTL9602C_QOS_MAP_PER_WORD); i++)
		seq_printf(s, " %08x",
			   rtl9602c_r(p, RTL9602C_QOS_DSCP_TO_PRIO + 4 * i));
	seq_printf(s, "\nport prio %08x  prio weights %08x %08x  select %08x\n",
		   rtl9602c_r(p, RTL9602C_QOS_PORT_PRIO),
		   rtl9602c_r(p, RTL9602C_QOS_PRIO_WEIGHT),
		   rtl9602c_r(p, RTL9602C_QOS_PRIO_WEIGHT + 4),
		   rtl9602c_r(p, RTL9602C_QOS_PRIO_WEIGHT_SEL));
	for (i = 0; i < RTL9602C_STORM_NCLASS; i++)
		seq_printf(s, "storm %-17s  enable %08x  meters %08x\n",
			   rtl9602c_storm_class[i].name,
			   rtl9602c_r(p, rtl9602c_storm_class[i].ctrl),
			   rtl9602c_r(p, rtl9602c_storm_class[i].meter));
	for (i = 0; i < RTL9602C_METER_COUNT; i++)
		seq_printf(s, "%s%08x%s", i % 8 ? " " : "meter rates ",
			   rtl9602c_r(p, RTL9602C_METER_CFG_HI(i)),
			   i % 8 == 7 ? "\n" : "");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_qos);

/*
 * cf: the usable entry count and the layout boundary; the classifier's control
 * word, with its bits 1:0, which decide the fate of a frame no entry matches (2
 * drops it, the other values let it through); the two templates' sources of
 * VID and priority (RTL9602C_CLS_TMPL_CFG); then every entry whose ones plane
 * is valid or has a non-zero word 0: the ones plane, the zeros plane, the three
 * action words, the action classes, "+sid" when stream assignment is among
 * them, the template bit and VALID. Each read goes through the table engine,
 * which takes table_lock itself.
 */
static void rtl9602c_cf_one(struct rtl9602c_priv *p, struct seq_file *s,
			    unsigned int idx)
{
	u32 mask[2] = {}, rule[2] = {}, act[3] = {}, en, tmpl;

	if (rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_CLS_RULE, idx, mask, 2,
				NULL) ||
	    rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_CLS_RULE,
				idx + RTL9602C_CLS_RULE_ONES_PLANE, rule, 2, NULL) ||
	    rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_CLS_ACT, idx, act, 3,
				NULL))
		return;

	en = rtl9602c_r(p, RTL9602C_CLS_ACT_EN(idx));
	tmpl = rtl9602c_r(p, RTL9602C_CLS_ENTRY_TMPL + 4 * (idx / 32));

	seq_printf(s,
		   "%3u  rule %08x %08x  mask %08x %08x  act %08x %08x %08x  en %02x%s  tmpl %u%s\n",
		   idx, rule[0], rule[1], mask[0], mask[1], act[0], act[1],
		   act[2], en, en & RTL9602C_CLS_ACT_EN_FLOW ? "+sid" : "    ",
		   !!(tmpl & BIT(idx % 32)),
		   rule[1] & RTL9602C_CLS_RULE_VALID ? "  VALID" : "");
}

static int rtl9602c_cf_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int idx, shown = 0;

	seq_printf(s, "%u classifier entries, pattern-1 boundary %lu\n",
		   p->cf_entries,
		   FIELD_GET(RTL9602C_CLS_CTRL_LAYOUT1_START,
			     rtl9602c_r(p, RTL9602C_CLS_CTRL)));
	seq_printf(s, "cls cfg %08x  upstream permit %lu\n",
		   rtl9602c_r(p, RTL9602C_CLS_CTRL),
		   rtl9602c_r(p, RTL9602C_CLS_CTRL) & GENMASK(1, 0));
	seq_printf(s, "templates %08x %08x\n\n",
		   rtl9602c_r(p, RTL9602C_CLS_TMPL_CFG(RTL9602C_CLS_TMPL_C)),
		   rtl9602c_r(p, RTL9602C_CLS_TMPL_CFG(RTL9602C_CLS_TMPL_S)));

	for (idx = 0; idx < RTL9602C_CLS_ENTRIES; idx++) {
		u32 rule[2] = {};

		if (rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_CLS_RULE,
					idx + RTL9602C_CLS_RULE_ONES_PLANE, rule, 2,
					NULL))
			break;
		if (!(rule[1] & RTL9602C_CLS_RULE_VALID) && !rule[0])
			continue;
		rtl9602c_cf_one(p, s, idx);
		shown++;
	}
	if (!shown)
		seq_puts(s, "(no entry has a rule)\n");
	return 0;
}

static int rtl9602c_cf_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_cf_show, inode->i_private);
}

static const struct file_operations rtl9602c_cf_fops = {
	.owner	= THIS_MODULE,
	.open	= rtl9602c_cf_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release = single_release,
};

/*
 * l2learn: the three flood masks and RTL9602C_ISOLATE_EXT_HAIRPIN, raw; then
 * per port the learning limit, the count learned, the action once the limit is
 * reached, "=fw" for forwarding without learning, the port's flood bits for
 * unknown unicast, unknown multicast and broadcast, and a note when the limit
 * is 0: learning is off, and every frame from an unknown source counts as an
 * overflow.
 */
static int rtl9602c_l2learn_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 over, uc, mc, bc, ext;
	unsigned int port;

	over = rtl9602c_r(p, RTL9602C_FDB_LEARN_LIMIT_ACT);
	uc = rtl9602c_r(p, RTL9602C_FLOOD_UNKNOWN_UCAST);
	mc = rtl9602c_r(p, RTL9602C_FLOOD_UNKNOWN_MCAST);
	bc = rtl9602c_r(p, RTL9602C_FLOOD_BCAST);
	ext = rtl9602c_r(p, RTL9602C_ISOLATE_EXT_HAIRPIN);

	seq_printf(s, "flood masks   unknown-uc %08x  unknown-mc %08x  bc %08x\n",
		   uc, mc, bc);
	seq_printf(s, "src ext perm  %08x\n\n", ext);
	seq_puts(s, "port  limit  count  over  flood(uc mc bc)  note\n");

	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		unsigned int limit, count, act;

		limit = (rtl9602c_r(p, RTL9602C_FDB_LEARN_LIMIT(port)) >>
			 RTL9602C_FDB_LEARN_SHIFT(port)) & RTL9602C_FDB_LEARN_MASK;
		count = (rtl9602c_r(p, RTL9602C_FDB_LEARN_CNT(port)) >>
			 RTL9602C_FDB_LEARN_SHIFT(port)) & RTL9602C_FDB_LEARN_MASK;
		act = (over >> RTL9602C_FDB_LEARN_LIMIT_ACT_SHIFT(port)) &
		      RTL9602C_FDB_LEARN_LIMIT_ACT_MASK;

		seq_printf(s, "%4u  %5u  %5u  %u%-3s  %u  %u  %u          %s%s\n",
			   port, limit, count, act,
			   act == RTL9602C_FDB_LEARN_LIMIT_ACT_FORWARD ? "=fw" : "=?",
			   !!(uc & BIT(port)), !!(mc & BIT(port)),
			   !!(bc & BIT(port)),
			   limit ? "" : "learning OFF, overflows per frame",
			   port == RTL9602C_PORT_CPU ? "  (cpu)" : "");
	}
	return 0;
}

static int rtl9602c_l2learn_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_l2learn_show, inode->i_private);
}

static const struct file_operations rtl9602c_l2learn_fops = {
	.owner	= THIS_MODULE,
	.open	= rtl9602c_l2learn_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release = single_release,
};

/*
 * @len bits from @lsb up of the egress descriptor, bit b being bit b % 32 of
 * word b / 32.
 */
static u64 rtl9602c_hsd_field(const u32 *w, unsigned int lsb, unsigned int len)
{
	u64 v = 0;
	unsigned int i;

	for (i = 0; i < len; i++) {
		unsigned int bit = lsb + i;

		if (bit / 32 < RTL9602C_PKTDBG_DATA_WORDS &&
		    (w[bit / 32] & BIT(bit % 32)))
			v |= BIT_ULL(i);
	}
	return v;
}

/*
 * The reserved-multicast controls, one by one: the suffixes skip, so base + 4n
 * would name the wrong address from the sixth register on.
 */
static const struct {
	const char	*name;
	u32		reg;
} rtl9602c_rma[] = {
	{ "01:80:c2:00:00:00 stp",	RTL9602C_RSVDMC_CTRL_00 },
	{ "01:80:c2:00:00:01 pause",	RTL9602C_RSVDMC_CTRL_01 },
	{ "01:80:c2:00:00:02 slow",	RTL9602C_RSVDMC_CTRL_02 },
	{ "01:80:c2:00:00:03 dot1x",	RTL9602C_RSVDMC_CTRL_03 },
	{ "01:80:c2:00:00:04",		RTL9602C_RSVDMC_CTRL_04 },
	{ "01:80:c2:00:00:08 provider",	RTL9602C_RSVDMC_CTRL_08 },
	{ "01:80:c2:00:00:0d",		RTL9602C_RSVDMC_CTRL_0D },
	{ "01:80:c2:00:00:0e lldp",	RTL9602C_RSVDMC_CTRL_0E },
	{ "01:80:c2:00:00:10",		RTL9602C_RSVDMC_CTRL_10 },
	{ "01:80:c2:00:00:11",		RTL9602C_RSVDMC_CTRL_11 },
	{ "01:80:c2:00:00:12",		RTL9602C_RSVDMC_CTRL_12 },
	{ "01:80:c2:00:00:13",		RTL9602C_RSVDMC_CTRL_13 },
	{ "01:80:c2:00:00:18",		RTL9602C_RSVDMC_CTRL_18 },
	{ "01:80:c2:00:00:1a",		RTL9602C_RSVDMC_CTRL_1A },
	{ "01:80:c2:00:00:20",		RTL9602C_RSVDMC_CTRL_20 },
	{ "01:80:c2:00:00:21",		RTL9602C_RSVDMC_CTRL_21 },
	{ "01:80:c2:00:00:22",		RTL9602C_RSVDMC_CTRL_22 },
	{ "cisco cdp",			RTL9602C_RSVDMC_CTRL_CDP },
	{ "cisco sstp",			RTL9602C_RSVDMC_CTRL_SSTP },
};

/*
 * rma: the global word raw; per reserved address its word raw, the action, and
 * the storm-exemption, keep-tag, VLAN-bypass and isolation-bypass bits. Only
 * the storm exemption is verified; what the actions and the other bits do is
 * not.
 */
static int rtl9602c_rma_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int i;

	seq_printf(s, "cfg %08x\n\n", rtl9602c_r(p, RTL9602C_RSVDMC_GLOBAL_CFG));
	seq_puts(s, "address                     raw  oper  storm  keep  vleak  isoleak\n");
	for (i = 0; i < ARRAY_SIZE(rtl9602c_rma); i++) {
		u32 v = rtl9602c_r(p, rtl9602c_rma[i].reg);

		seq_printf(s, "%-26s %04x %5lu %6u %5u %6u %8u\n",
			   rtl9602c_rma[i].name, v,
			   FIELD_GET(RTL9602C_RSVDMC_CTRL_ACTION, v),
			   !!(v & RTL9602C_RSVDMC_CTRL_STORM_EXEMPT),
			   !!(v & RTL9602C_RSVDMC_CTRL_KEEP_TAG),
			   !!(v & RTL9602C_RSVDMC_CTRL_VLAN_BYPASS),
			   !!(v & RTL9602C_RSVDMC_CTRL_ISOLATE_BYPASS));
	}
	return 0;
}

static int rtl9602c_rma_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_rma_show, inode->i_private);
}

/*
 * svlan: the S-VLAN registers, as rtl9602c_svlan_setup() wrote them: the two
 * TPIDs and the second's enable, the policy fields, each port's S-VID and its
 * uplink and destination-VID bits, and the valid translation entries. The TPID
 * word is 64 bits, its high half at the lower address.
 */
static int rtl9602c_svlan_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 hi = rtl9602c_r(p, RTL9602C_SVLAN_TPID_CFG);
	u32 lo = rtl9602c_r(p, RTL9602C_SVLAN_TPID_CFG + 4);
	u32 ctrl = rtl9602c_r(p, RTL9602C_SVLAN_POLICY);
	u32 up = rtl9602c_r(p, RTL9602C_SVLAN_UPLINK_PORTS);
	u32 dmac = rtl9602c_r(p, RTL9602C_SVLAN_DMAC_VID_SEL);
	unsigned int i;

	seq_printf(s, "tpid      %04lx  second %04lx %s\n",
		   FIELD_GET(RTL9602C_SVLAN_TPID_CFG_PRIMARY, lo),
		   FIELD_GET(RTL9602C_SVLAN_TPID_CFG_SECOND, lo),
		   (hi & RTL9602C_SVLAN_TPID_CFG_SECOND_EN) ? "enabled" : "disabled");
	seq_printf(s, "policy    filtering %u  keep-dei %u  pcp %lu  untag %lu  pcp-sel %lu\n",
		   !!(ctrl & RTL9602C_SVLAN_POLICY_INGRESS_FILTER),
		   !!(ctrl & RTL9602C_SVLAN_POLICY_KEEP_DEI),
		   FIELD_GET(RTL9602C_SVLAN_POLICY_PCP, ctrl),
		   FIELD_GET(RTL9602C_SVLAN_POLICY_UNTAGGED_ACT, ctrl),
		   FIELD_GET(RTL9602C_SVLAN_POLICY_PCP_SRC, ctrl));

	seq_puts(s, "port  svid  uplink  dmac-vid\n");
	for (i = 0; i < RTL9602C_NUM_PORTS; i++) {
		u32 w = rtl9602c_r(p, RTL9602C_SVLAN_PVID(i));

		seq_printf(s, "%4u %5lu %7u %8u\n", i,
			   (unsigned long)((w >> RTL9602C_SVLAN_PVID_SHIFT(i)) &
					   RTL9602C_SVLAN_PVID_MASK),
			   !!(up & BIT(i)), !!(dmac & BIT(i)));
	}

	seq_puts(s, "\ns2c   svid  port  cvid  pcp\n");
	for (i = 0; i < RTL9602C_SVLAN_XLATE_ENTRIES; i++) {
		u32 e = rtl9602c_r(p, RTL9602C_SVLAN_XLATE(i));

		if (!(e & RTL9602C_SVLAN_XLATE_VALID))
			continue;
		seq_printf(s, "%4u %8lu %5lu %9lu %4lu\n", i,
			   FIELD_GET(RTL9602C_SVLAN_XLATE_SVID, e),
			   FIELD_GET(RTL9602C_SVLAN_XLATE_PORT, e),
			   FIELD_GET(RTL9602C_SVLAN_XLATE_CVID, e),
			   FIELD_GET(RTL9602C_SVLAN_XLATE_PRIO, e));
	}
	return 0;
}

static int rtl9602c_svlan_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_svlan_show, inode->i_private);
}

/*
 * hsd: the egress descriptor capture, which holds the last frame to leave by
 * the ports its configuration word names: the 15 words raw, the configuration
 * decoded, the PTP fields and the switch's clock at the same moment. The PTP
 * identifier and action, and the mode and reason fields, are printed but not
 * verified. A write stores a number, 0x for hexadecimal and 0 for octal, as the
 * configuration word, which re-arms the capture and changes no forwarding.
 */
static int rtl9602c_hsd_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 w[RTL9602C_PKTDBG_DATA_WORDS];
	unsigned int i;
	u32 cfg, nsec;
	u64 sec;

	for (i = 0; i < RTL9602C_PKTDBG_DATA_WORDS; i++)
		w[i] = rtl9602c_r(p, RTL9602C_PKTDBG_DATA(i));
	cfg = rtl9602c_r(p, RTL9602C_PKTDBG_CFG);

	seq_puts(s, "raw");
	for (i = 0; i < RTL9602C_PKTDBG_DATA_WORDS; i++)
		seq_printf(s, " %08x", w[i]);
	seq_printf(s, "\ncfg %08x  valid %u  reason %lu  mode %lu  igr %lu  egr %lu\n",
		   cfg, !!(cfg & RTL9602C_PKTDBG_CFG_VALID),
		   FIELD_GET(RTL9602C_PKTDBG_CFG_REASON, cfg),
		   FIELD_GET(RTL9602C_PKTDBG_CFG_MODE, cfg),
		   FIELD_GET(RTL9602C_PKTDBG_CFG_INGRESS_PORT, cfg),
		   FIELD_GET(RTL9602C_PKTDBG_CFG_EGRESS_PORT, cfg));

	seq_printf(s, "ptp %llu  act %llu  id %llu  stamp %llu.%09llu\n",
		   rtl9602c_hsd_field(w, RTL9602C_PKTDBG_PTP_FLAG_LSB, 1),
		   rtl9602c_hsd_field(w, RTL9602C_PKTDBG_PTP_ACTION_LSB,
				      RTL9602C_PKTDBG_PTP_ACTION_BITS),
		   rtl9602c_hsd_field(w, RTL9602C_PKTDBG_PTP_ID_LSB,
				      RTL9602C_PKTDBG_PTP_ID_BITS),
		   rtl9602c_hsd_field(w, RTL9602C_PKTDBG_PTP_SEC_LSB,
				      RTL9602C_PKTDBG_PTP_SEC_BITS),
		   rtl9602c_hsd_field(w, RTL9602C_PKTDBG_PTP_NSEC_LSB,
				      RTL9602C_PKTDBG_PTP_NSEC_BITS));

	/* Latch the clock and read it, under ptp_lock like every clock read. */
	mutex_lock(&p->ptp_lock);
	rtl9602c_rmw(p, RTL9602C_PTP_CLK_CTRL, 0, RTL9602C_PTP_CLK_CTRL_LATCH);
	sec = rtl9602c_ptp_rd_sec(p, RTL9602C_PTP_CLK_SEC);
	nsec = rtl9602c_r(p, RTL9602C_PTP_CLK_NSEC);
	mutex_unlock(&p->ptp_lock);
	seq_printf(s, "clock    %llu.%09lu\n", sec,
		   FIELD_GET(RTL9602C_PTP_NSEC_TICKS, nsec) *
		   RTL9602C_PTP_NSEC_TICK_NS);
	return 0;
}

static int rtl9602c_hsd_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_hsd_show, inode->i_private);
}

static ssize_t rtl9602c_hsd_write(struct file *file, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct rtl9602c_priv *p = file_inode(file)->i_private;
	char buf[24], *s;
	u32 cfg;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	s = strim(buf);

	if (kstrtou32(s, 0, &cfg))
		return -EINVAL;

	rtl9602c_w(p, RTL9602C_PKTDBG_CFG, cfg);
	return len;
}

/*
 * The tables the l3nat view prints: name, selector, words, and the rows shown,
 * the first four of each large table and every row of the small ones.
 */
static const struct {
	const char	*name;
	u8		table;
	u8		words;
	u8		show;
} rtl9602c_l3nat_tables[] = {
	{ "flow", RTL9602C_L3TBL_CTRL_TABLE_FLOW, 4, 4 },
	{ "napt",	  RTL9602C_L3TBL_CTRL_TABLE_NAPT,	 1, 4 },
	{ "neigh",	  RTL9602C_L3TBL_CTRL_TABLE_NEIGH,	 2, 4 },
	{ "neigh6",	  RTL9602C_L3TBL_CTRL_TABLE_NEIGH6,	 3, 4 },
	/* The small tables, whole. */
	{ "route",	  RTL9602C_L3TBL_CTRL_TABLE_ROUTE,	 2, 16 },
	{ "l3if",	  RTL9602C_L3TBL_CTRL_TABLE_L3IF,	 4, 16 },
	{ "nexthop",	  RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP,	 1, 16 },
	{ "extaddr",  RTL9602C_L3TBL_CTRL_TABLE_EXTADDR,	 3, 8 },
	{ "pppoe",	  RTL9602C_L3TBL_CTRL_TABLE_PPPOE,	 1, 8 },
};

/*
 * l3nat: the auxiliary and configuration words, then the rows above, raw.
 * Reading a table has no side effect. The last line reads row 9999 of the
 * five-tuple table, which the geometry check must refuse with -EINVAL.
 */
static int rtl9602c_l3nat_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int t, e, w;
	u32 l3cfg;

	seq_printf(s, "aux ctrl %08x\n",
		   rtl9602c_r(p, RTL9602C_L3_AUX_CFG));
	l3cfg = rtl9602c_r(p, RTL9602C_L3_CFG);
	seq_printf(s, "l3 cfg   %08x  flow4 %u  flow6 %u\n\n", l3cfg,
		   !!(l3cfg & RTL9602C_L3_CFG_FLOW4_EN),
		   !!(l3cfg & RTL9602C_L3_CFG_FLOW6_EN));

	for (t = 0; t < ARRAY_SIZE(rtl9602c_l3nat_tables); t++) {
		seq_printf(s, "%s (table %u, %u words)\n",
			   rtl9602c_l3nat_tables[t].name,
			   rtl9602c_l3nat_tables[t].table,
			   rtl9602c_l3nat_tables[t].words);
		for (e = 0; e < rtl9602c_l3nat_tables[t].show; e++) {
			u32 data[RTL9602C_L3TBL_DATA_WORDS] = {};
			int err;

			err = rtl9602c_nat_tbl_read(p, rtl9602c_l3nat_tables[t].table,
						    e, data,
						    rtl9602c_l3nat_tables[t].words);
			if (err) {
				seq_printf(s, "  [%u] read error %d\n", e, err);
				break;
			}
			seq_printf(s, "  [%u]", e);
			for (w = 0; w < rtl9602c_l3nat_tables[t].words; w++)
				seq_printf(s, " %08x", data[w]);
			seq_puts(s, "\n");
		}
		seq_puts(s, "\n");
	}

	{
		u32 data[RTL9602C_L3TBL_DATA_WORDS] = {};
		int err = rtl9602c_nat_tbl_read(p, RTL9602C_L3TBL_CTRL_TABLE_FLOW,
						9999, data, 4);

		seq_printf(s, "guard: flow-routing[9999] -> %d (want %d)\n",
			   err, -EINVAL);
	}
	return 0;
}

static int rtl9602c_l3nat_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_l3nat_show, inode->i_private);
}

static const struct file_operations rtl9602c_rma_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_rma_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations rtl9602c_svlan_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_svlan_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations rtl9602c_hsd_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_hsd_open,
	.read		= seq_read,
	.write		= rtl9602c_hsd_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * l3flow: the engine's enable and configuration words, the PPPoE hit bits, word
 * 0 of the NAPT hit bitmap, the hit bitmaps of the five-tuple, IPv6 neighbour
 * ("neighbour") and neighbour ("arp") tables, then the per-frame capture's
 * control word, decoded, and its input and output words, raw. Reading word 0 of
 * the NAPT hit bitmap clears it, so a flow in outbound entries 0 to 31 can read
 * as unused at netfilter's next statistics query. Whether reading the other
 * bitmaps clears them is not verified.
 */
static int rtl9602c_l3flow_show(struct seq_file *m, void *unused)
{
	struct rtl9602c_priv *p = m->private;
	unsigned int i;
	u32 v;

	v = rtl9602c_r(p, RTL9602C_L3_GLOBAL_CTRL);
	seq_printf(m, "global     %08x  engine %s\n", v,
		   v & RTL9602C_L3_GLOBAL_CTRL_EN ? "ON" : "off");
	v = rtl9602c_r(p, RTL9602C_L3_CFG);
	seq_printf(m, "l3 cfg     %08x  flow4 %u  flow6 %u\n", v,
		   !!(v & RTL9602C_L3_CFG_FLOW4_EN),
		   !!(v & RTL9602C_L3_CFG_FLOW6_EN));
	seq_printf(m, "pppoe hit  %08x\n", rtl9602c_r(p, RTL9602C_PPPOE_HIT));
	seq_printf(m, "napt hit   %08x\n", rtl9602c_r(p, RTL9602C_NAPT_HIT));

	seq_puts(m, "\nhit bitmaps, one bit per entry\n");
	seq_puts(m, "  flow     ");
	for (i = 0; i < 8; i++)
		seq_printf(m, "%08x ", rtl9602c_r(p, RTL9602C_L3_FLOW_HIT(i)));
	seq_puts(m, "\n  neighbour");
	for (i = 0; i < 4; i++)
		seq_printf(m, " %08x", rtl9602c_r(p, RTL9602C_NEIGH6_HIT(i)));
	seq_puts(m, "\n  arp      ");
	for (i = 0; i < 4; i++)
		seq_printf(m, " %08x", rtl9602c_r(p, RTL9602C_NEIGH_HIT(i)));

	v = rtl9602c_r(p, RTL9602C_L3_CAPTURE_CTRL);
	seq_printf(m, "\n\ncapture    %08x  log mode %lu  capture %s  engine %s\n",
		   v, FIELD_GET(RTL9602C_L3_CAPTURE_CTRL_MODE, v),
		   v & RTL9602C_L3_CAPTURE_CTRL_EN ? "active" : "idle",
		   v & RTL9602C_L3_CAPTURE_CTRL_BUSY ? "busy" : "free");
	seq_puts(m, "  in     ");
	for (i = 0; i < RTL9602C_L3_CAPTURE_IN_WORDS; i++)
		seq_printf(m, " %08x", rtl9602c_r(p, RTL9602C_L3_CAPTURE_IN(i)));
	seq_puts(m, "\n  out    ");
	for (i = 0; i < RTL9602C_L3_CAPTURE_OUT_WORDS; i++)
		seq_printf(m, " %08x", rtl9602c_r(p, RTL9602C_L3_CAPTURE_OUT(i)));
	seq_putc(m, '\n');
	return 0;
}

static int rtl9602c_l3flow_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_l3flow_show, inode->i_private);
}

static const struct file_operations rtl9602c_l3flow_fops = {
	.owner	 = THIS_MODULE,
	.open	 = rtl9602c_l3flow_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static const struct file_operations rtl9602c_l3nat_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_l3nat_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations rtl9602c_qdepth_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_qdepth_open,
	.read		= seq_read,
	.write		= rtl9602c_qdepth_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * l3capture: the engine's per-frame capture. A read gives the control word
 * decoded (mode, armed, busy), the 14 input words, the engine's parse of the
 * frame, and the 5 output words, its verdict, raw. The words latch separately,
 * so one reading can mix two frames. Writing "<mode> [<armed>]" sets the mode
 * and arms the latch, or disarms it with an armed of 0; that this leaves
 * forwarding unchanged is not verified.
 */
static int rtl9602c_l3capture_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 ctrl;
	unsigned int i;

	ctrl = rtl9602c_r(p, RTL9602C_L3_CAPTURE_CTRL);
	seq_printf(s, "capture %08x  mode %lu  armed %u  busy %u\n",
		   ctrl, FIELD_GET(RTL9602C_L3_CAPTURE_CTRL_MODE, ctrl),
		   !!(ctrl & RTL9602C_L3_CAPTURE_CTRL_EN),
		   !!(ctrl & RTL9602C_L3_CAPTURE_CTRL_BUSY));

	seq_puts(s, "in: ");
	for (i = 0; i < RTL9602C_L3_CAPTURE_IN_WORDS; i++)
		seq_printf(s, " %08x", rtl9602c_r(p, RTL9602C_L3_CAPTURE_IN(i)));
	seq_puts(s, "\nout:");
	for (i = 0; i < RTL9602C_L3_CAPTURE_OUT_WORDS; i++)
		seq_printf(s, " %08x", rtl9602c_r(p, RTL9602C_L3_CAPTURE_OUT(i)));
	seq_puts(s, "\n");
	return 0;
}

static int rtl9602c_l3capture_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_l3capture_show, inode->i_private);
}

static ssize_t rtl9602c_l3capture_write(struct file *file, const char __user *ubuf,
					size_t len, loff_t *ppos)
{
	struct rtl9602c_priv *p = file_inode(file)->i_private;
	unsigned int mode, atv = 1;
	char buf[32];
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	n = sscanf(strim(buf), "%u %u", &mode, &atv);
	if (n < 1 || mode > FIELD_MAX(RTL9602C_L3_CAPTURE_CTRL_MODE) || atv > 1)
		return -EINVAL;

	rtl9602c_rmw(p, RTL9602C_L3_CAPTURE_CTRL,
		     RTL9602C_L3_CAPTURE_CTRL_MODE | RTL9602C_L3_CAPTURE_CTRL_EN,
		     FIELD_PREP(RTL9602C_L3_CAPTURE_CTRL_MODE, mode) |
		     (atv ? RTL9602C_L3_CAPTURE_CTRL_EN : 0));
	pr_info("l3capture: mode %u, capture %s, control now %08x\n", mode,
		atv ? "on" : "off", rtl9602c_r(p, RTL9602C_L3_CAPTURE_CTRL));
	return len;
}

static const struct file_operations rtl9602c_l3capture_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_l3capture_open,
	.read		= seq_read,
	.write		= rtl9602c_l3capture_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * A resolved ARP neighbour as a next hop. Its device needs an L3-interface row
 * and must be, or run over, a switch port. Its address-table entry is placed
 * under the VLAN of that row, as the hardware holds it; the next-hop row names
 * the interface row and the entry; the neighbour table's row at the same index
 * holds the address. The routed frame's destination MAC comes from the entry,
 * not from the neighbour table, which is taken to be searched by address; what
 * the engine uses it for is not verified.
 */
static int rtl9602c_l34_neigh_set(struct rtl9602c_priv *p, struct neighbour *n)
{
	unsigned int nif_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_L3IF];
	unsigned int nh_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP];
	unsigned int arp_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NEIGH];
	u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};
	unsigned int slot, row, first;
	__be32 addr;
	int netif, i, port;
	u16 vid;
	int err;

	if (n->tbl->family != AF_INET || !n->dev)
		return -EOPNOTSUPP;

	netif = rtl9602c_l34_netif_row(p, n->dev->ifindex);
	if (netif < 0)
		return -ENODEV;
	port = rtl9602c_l34_port_of(p, n->dev);
	if (port < 0)
		return -ENODEV;

	memcpy(&addr, n->primary_key, sizeof(addr));

	if (rtl9602c_nat_tbl_read(p, RTL9602C_L3TBL_CTRL_TABLE_L3IF, netif, e,
				  nif_words))
		return -EIO;
	vid = rtl9602c_flow_get(e, RTL9602C_L3IF_VID);

	err = rtl9602c_l2_nexthop_place(p, n->ha, port, &vid, &slot);
	if (err)
		return err;

	/*
	 * The row already holding the address, else the first free one: from
	 * row 0 on the PON port, from row 1 elsewhere. The external addresses
	 * send their frames to the default route's gateway, and to row 0 until
	 * that is known, so row 0 is kept for the PON side: with a LAN-side
	 * neighbour there the switch drops the translated frames at their
	 * ingress port (measured on this board).
	 */
	first = port == RTL9602C_PORT_PON ? 0 : 1;
	row = UINT_MAX;
	for (i = 0; i < (int)ARRAY_SIZE(p->l34_nh_addr); i++) {
		if (p->l34_nh_addr[i] == addr) {
			row = i;
			break;
		}
		if (i >= (int)first && !p->l34_nh_addr[i] && row == UINT_MAX)
			row = i;
	}
	if (row == UINT_MAX) {
		rtl9602c_l34_nh_l2_put(p, n->ha, vid);
		return -ENOSPC;
	}

	memset(e, 0, sizeof(e));
	rtl9602c_flow_put(e, RTL9602C_NEXTHOP_L3IF_IDX, netif);
	rtl9602c_flow_put(e, RTL9602C_NEXTHOP_L2_IDX, slot);
	err = rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP, row, e,
				     nh_words);
	if (err)
		return err;

	memset(e, 0, sizeof(e));
	rtl9602c_flow_put(e, RTL9602C_NEIGH_IP, be32_to_cpu(addr));
	rtl9602c_flow_put(e, RTL9602C_NEIGH_L2_IDX, slot);
	rtl9602c_flow_put(e, RTL9602C_NEIGH_VALID, 1);
	err = rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NEIGH, row, e,
				     arp_words);
	if (err)
		return err;

	p->l34_nh_addr[row] = addr;
	rtl9602c_l34_nh_l2_set(p, row, n->ha, vid);
	pr_debug("<l3> %pI4 at %pM on %s is nexthop[%u] -> l3if[%d], L2 slot %u vlan %u\n",
		 &addr, n->ha, n->dev->name, row, netif, slot, vid);
	return 0;
}

/*
 * Zero the next-hop and neighbour rows of @addr, and delete the address-table
 * entry placed for it unless another next hop names that entry.
 */
static void rtl9602c_l34_neigh_del(struct rtl9602c_priv *p, __be32 addr)
{
	unsigned int nh_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP];
	unsigned int arp_words = rtl9602c_nat_tbl_words[RTL9602C_L3TBL_CTRL_TABLE_NEIGH];
	u32 e[RTL9602C_L3TBL_DATA_WORDS] = {};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->l34_nh_addr); i++) {
		if (p->l34_nh_addr[i] != addr)
			continue;
		rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NEIGH, i, e,
				       arp_words);
		rtl9602c_nat_tbl_write(p, RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP, i, e,
				       nh_words);
		p->l34_nh_addr[i] = 0;
		rtl9602c_l34_nh_l2_set(p, i, NULL, 0);
		return;
	}
}

/*
 * The neighbour notifier can run in softirq, and the table writes sleep: the
 * event (address, device held) is queued and a work item applies the queue
 * under flow_lock, looking the neighbour up again so that its state and address
 * are current. A neighbour in a valid state becomes a next hop, any other is
 * removed. The routes are committed after each event, since a gateway route
 * resolves only once its neighbour has a row.
 */
static void rtl9602c_l34_neigh_work(struct work_struct *work)
{
	struct rtl9602c_priv *p =
		container_of(work, struct rtl9602c_priv, l34_neigh_work);
	struct rtl9602c_l34_neigh_ev *ev;

	for (;;) {
		spin_lock_bh(&p->l34_neigh_lock);
		ev = list_first_entry_or_null(&p->l34_neigh_q,
					      struct rtl9602c_l34_neigh_ev,
					      list);
		if (ev)
			list_del(&ev->list);
		spin_unlock_bh(&p->l34_neigh_lock);
		if (!ev)
			return;

		mutex_lock(&p->flow_lock);
		if (ev->add) {
			struct neighbour *n = neigh_lookup(&arp_tbl, &ev->addr,
							   ev->dev);

			if (n) {
				if (n->nud_state & NUD_VALID)
					rtl9602c_l34_neigh_set(p, n);
				neigh_release(n);
			}
		} else {
			rtl9602c_l34_neigh_del(p, ev->addr);
		}

		rtl9602c_l34_route_commit(p);
		mutex_unlock(&p->flow_lock);
		dev_put(ev->dev);
		kfree(ev);
	}
}

static int rtl9602c_l34_netevent(struct notifier_block *nb, unsigned long event,
				 void *ptr)
{
	struct rtl9602c_priv *p =
		container_of(nb, struct rtl9602c_priv, l34_netevent_nb);
	struct rtl9602c_l34_neigh_ev *ev;
	struct neighbour *n = ptr;

	if (event != NETEVENT_NEIGH_UPDATE || !p->l34_up)
		return NOTIFY_DONE;
	if (n->tbl != &arp_tbl || !n->dev)
		return NOTIFY_DONE;

	ev = kzalloc(sizeof(*ev), GFP_ATOMIC);
	if (!ev)
		return NOTIFY_DONE;

	memcpy(&ev->addr, n->primary_key, sizeof(ev->addr));
	ev->dev = n->dev;
	ev->add = !!(n->nud_state & NUD_VALID);
	dev_hold(ev->dev);

	spin_lock_bh(&p->l34_neigh_lock);
	list_add_tail(&ev->list, &p->l34_neigh_q);
	spin_unlock_bh(&p->l34_neigh_lock);
	schedule_work(&p->l34_neigh_work);
	return NOTIFY_DONE;
}

static void rtl9602c_l34_q_drain(struct rtl9602c_priv *p)
{
	struct rtl9602c_l34_neigh_ev *nev, *ntmp;
	struct rtl9602c_l34_fib_ev *fev, *ftmp;

	list_for_each_entry_safe(nev, ntmp, &p->l34_neigh_q, list) {
		list_del(&nev->list);
		dev_put(nev->dev);
		kfree(nev);
	}
	list_for_each_entry_safe(fev, ftmp, &p->l34_fib_q, list) {
		list_del(&fev->list);
		kfree(fev);
	}
}

/*
 * The netdevice notifier: a device that holds an L3-interface row has the row
 * written again when its MAC address or its MTU changes, with its first IPv4
 * address, so that the engine rewrites routed frames with the device's
 * current MAC address and keeps to its current MTU. A user port whose address
 * changes has its own-address entry moved with it.
 */
static int rtl9602c_l34_netdev_event(struct notifier_block *nb,
				     unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct rtl9602c_priv *p;
	const struct in_ifaddr *ifa;
	struct in_device *in_dev;
	unsigned int i;

	p = container_of(nb, struct rtl9602c_priv, l34_netdev_nb);

	/* A user port's own address behind the CPU port follows the port. */
	if (event == NETDEV_PRE_CHANGEADDR || event == NETDEV_CHANGEADDR) {
		struct dsa_port *dp;

		dsa_switch_for_each_user_port(dp, &p->ds) {
			if (dp->user != dev)
				continue;
			if (event == NETDEV_PRE_CHANGEADDR)
				rtl9602c_l2_own_drop(p, dp->index);
			else
				rtl9602c_l2_own_place(p, dp->index);
		}
	}

	if (event != NETDEV_CHANGEMTU && event != NETDEV_CHANGEADDR)
		return NOTIFY_DONE;
	if (!p->l34_up)
		return NOTIFY_DONE;

	in_dev = __in_dev_get_rtnl(dev);
	ifa = in_dev ? rtnl_dereference(in_dev->ifa_list) : NULL;
	if (!ifa)
		return NOTIFY_DONE;

	mutex_lock(&p->flow_lock);
	for (i = 0; i < ARRAY_SIZE(p->l34_netif_ifindex); i++) {
		if (p->l34_netif_ifindex[i] != dev->ifindex)
			continue;
		if (rtl9602c_l34_netif_set(p, i, dev->dev_addr,
					   is_vlan_dev(dev) ?
					   vlan_dev_vlan_id(dev) : 0,
					   ifa->ifa_address, dev->mtu))
			pr_warn("<l3> l3if[%u] for %s did not take its new %s\n",
				i, dev->name,
				event == NETDEV_CHANGEMTU ? "MTU" : "address");
		else
			pr_debug("<l3> %s: l3if[%u] now %pM mtu %u\n",
				 dev->name, i, dev->dev_addr, dev->mtu);
	}
	mutex_unlock(&p->flow_lock);
	return NOTIFY_DONE;
}

/**
 * rtl9602c_l34_notifiers_register() - follow addresses, neighbours and routes
 * @p: the switch
 *
 * Registers the address notifier and the netdevice notifier, then the
 * neighbour notifier, then the route notifier: registering that one replays
 * every route the kernel holds, and a route resolves against the rows the
 * others write. A notifier that fails to register is reported, and the switch
 * comes up without it.
 */
void rtl9602c_l34_notifiers_register(struct rtl9602c_priv *p)
{
	INIT_WORK(&p->l34_neigh_work, rtl9602c_l34_neigh_work);
	spin_lock_init(&p->l34_neigh_lock);
	INIT_LIST_HEAD(&p->l34_neigh_q);

	p->l34_inetaddr_nb.notifier_call = rtl9602c_l34_inetaddr_event;
	if (register_inetaddr_notifier(&p->l34_inetaddr_nb)) {
		pr_err("no address notifier; L3 interfaces must be written by hand\n");
		p->l34_inetaddr_nb.notifier_call = NULL;
	}

	p->l34_netdev_nb.notifier_call = rtl9602c_l34_netdev_event;
	if (register_netdevice_notifier(&p->l34_netdev_nb)) {
		pr_err("no netdevice notifier; a changed MAC address or MTU is not written\n");
		p->l34_netdev_nb.notifier_call = NULL;
	}

	p->l34_netevent_nb.notifier_call = rtl9602c_l34_netevent;
	if (register_netevent_notifier(&p->l34_netevent_nb)) {
		pr_err("no neighbour notifier; next hops must be written by hand\n");
		p->l34_netevent_nb.notifier_call = NULL;
	}

	INIT_WORK(&p->l34_fib_work, rtl9602c_l34_fib_work);
	spin_lock_init(&p->l34_fib_lock);
	INIT_LIST_HEAD(&p->l34_fib_q);

	p->l34_fib_nb.notifier_call = rtl9602c_l34_fib_event;
	if (register_fib_notifier(&init_net, &p->l34_fib_nb, NULL, NULL)) {
		pr_err("no route notifier; the routing table must be written by hand\n");
		p->l34_fib_nb.notifier_call = NULL;
	}
}

/**
 * rtl9602c_l34_notifiers_unregister() - stop following the kernel
 * @p: the switch
 *
 * The notifiers go first, since they can fire on any CPU and touch the tables;
 * then both work items are cancelled and the events still queued are freed.
 */
void rtl9602c_l34_notifiers_unregister(struct rtl9602c_priv *p)
{
	if (p->l34_inetaddr_nb.notifier_call)
		unregister_inetaddr_notifier(&p->l34_inetaddr_nb);
	if (p->l34_netdev_nb.notifier_call)
		unregister_netdevice_notifier(&p->l34_netdev_nb);
	if (p->l34_netevent_nb.notifier_call)
		unregister_netevent_notifier(&p->l34_netevent_nb);
	if (p->l34_fib_nb.notifier_call)
		unregister_fib_notifier(&init_net, &p->l34_fib_nb);

	cancel_work_sync(&p->l34_neigh_work);
	cancel_work_sync(&p->l34_fib_work);
	rtl9602c_l34_q_drain(p);
}

/**
 * rtl9602c_offload_debugfs_init() - create this file's debugfs views
 * @p: the switch, with its debugfs directory created
 *
 * Creates rma, svlan, l3nat, l3flow, l2learn, qos and cf, mode 0400, and
 * qdepth, hsd and l3capture, mode 0600, since a write to them changes the
 * hardware's state.
 */
void rtl9602c_offload_debugfs_init(struct rtl9602c_priv *p)
{
	debugfs_create_file("qdepth", 0600, p->debugfs, p,
			    &rtl9602c_qdepth_fops);
	debugfs_create_file("rma", 0400, p->debugfs, p,
			    &rtl9602c_rma_fops);
	debugfs_create_file("svlan", 0400, p->debugfs, p,
			    &rtl9602c_svlan_fops);
	debugfs_create_file("hsd", 0600, p->debugfs, p,
			    &rtl9602c_hsd_fops);
	debugfs_create_file("l3nat", 0400, p->debugfs, p,
			    &rtl9602c_l3nat_fops);
	debugfs_create_file("l3flow", 0400, p->debugfs, p,
			    &rtl9602c_l3flow_fops);
	debugfs_create_file("l2learn", 0400, p->debugfs, p,
			    &rtl9602c_l2learn_fops);
	debugfs_create_file("qos", 0400, p->debugfs, p, &rtl9602c_qos_fops);
	debugfs_create_file("cf", 0400, p->debugfs, p,
			    &rtl9602c_cf_fops);
	debugfs_create_file("l3capture", 0600, p->debugfs, p,
			    &rtl9602c_l3capture_fops);
}
