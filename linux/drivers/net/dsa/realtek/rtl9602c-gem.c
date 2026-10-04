// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: GEM ports as network interfaces, through the PON
 * class.
 *
 * The switch registers its data path with the PON class as the ONU behind
 * its PON port, and management creates one gem interface per GEM port on the
 * pon interface. A GEM port is a flow of the GPON MAC: the flow holds the
 * port's Port-ID in the GPON MAC's Port-ID table and the type of its
 * downstream traffic, and it is the port's upstream stream in the PON MAC,
 * which takes the port's frames to a queue of its T-CONT. The downstream half
 * is programmed when the port is created, the upstream half once the OLT has
 * given the T-CONT an Alloc-ID, which may come before or after the port.
 *
 * A GEM interface's frames go to the conduit with the flow in a port-mux dst,
 * and received frames come back from the conduit with the flow they arrived
 * on. In a bridge, the switch forwards between the GEM ports and the bridge's
 * other ports itself, treating every GEM port as its one PON port.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/bitmap.h>
#include <linux/if_link.h>
#include <linux/netdevice.h>
#include <linux/pon.h>
#include <linux/dsa/rtl9602c.h>
#include <net/dsa.h>
#include <net/switchdev.h>

#include "rtl9602c.h"

/*
 * Queue 63, where the build points every GEM stream, not admitted, and where
 * deleting a GEM port points its stream again. It is also the highest queue
 * of the last T-CONT the GEM ports use (rtl9602c_gem_tcont_qbit()): what keeps
 * a parked stream's frames off the fibre is that the stream is not admitted,
 * since a stream that is not admitted sends nothing even when its queue is
 * scheduled (measured on this board).
 */
#define RTL9602C_GEM_PARKING_QUEUE	RTL9602C_PON_QUEUE(15, 31)

/* GEM interfaces stack on pon, the DSA user port of the PON port. */
static bool rtl9602c_gem_owns_netdev(struct pon_device *pon,
				     const struct net_device *dev)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);
	struct dsa_port *dp;

	if (!dsa_user_dev_check(dev))
		return false;

	dp = dsa_port_from_netdev((struct net_device *)dev);
	return !IS_ERR(dp) && dp->ds == &p->ds &&
	       dp->index == RTL9602C_PORT_PON;
}

/*
 * Whether the GPON MAC holds an Alloc-ID for the PON class's T-CONT @tcont.
 * It holds one once the OLT has assigned it with Assign_Alloc-ID, a PLOAM
 * message, which may come before or after the OMCI that creates a GEM port
 * on the T-CONT.
 */
static bool rtl9602c_gem_tcont_bound(struct rtl9602c_priv *p, u32 tcont)
{
	lockdep_assert_held(&p->gpon_lock);

	return tcont < RTL9602C_GEM_NUM_TCONT &&
	       (p->gpon_tcont_used & BIT(rtl9602c_gem_tcont(tcont)));
}

/*
 * Program flow @f for the GEM port @cfg describes. The downstream half does
 * not depend on the T-CONT: the Port-ID goes into the flow's entry of the
 * Port-ID table when the port carries downstream traffic, and the flow is
 * typed as Ethernet, with the multicast bit for a multicast port, whatever
 * the direction. What the multicast bit does on its own is not verified.
 *
 * The upstream half waits for the T-CONT to hold an Alloc-ID: a T-CONT
 * without one is granted nothing, and the switch would discard at the ingress
 * port what the stream's queue cannot take. Until then the port is logged as
 * "upstream parked" and its stream is parked, not admitted and on the parking
 * queue, whether the port is new or programmed again after a bring-up
 * (rtl9602c_gem_restore()). rtl9602c_gem_tcont_ready() programs the upstream
 * half once the Alloc-ID comes.
 *
 * With the Alloc-ID, the T-CONT's queue of the highest priority is attached
 * (rtl9602c_gem_tcont_qbit()), the flow is given the Port-ID its upstream
 * GEM headers carry, and the stream is pointed at that queue and admitted:
 * every GEM port of a T-CONT sends through that one queue.
 */
static void rtl9602c_gem_park(struct rtl9602c_priv *p, unsigned int f);

static int rtl9602c_gem_program(struct rtl9602c_priv *p, unsigned int f,
				const struct pon_gem_config *cfg)
{
	u32 type = RTL9602C_GEM_DS_FLOW_CFG_TYPE_ETH;
	unsigned int t, q;
	int err;

	if (cfg->multicast)
		type |= RTL9602C_GEM_DS_FLOW_CFG_TYPE_MC;
	type |= rtl9602c_gpon_port_aes(p, cfg->port_id);

	if (cfg->direction & IFLA_GEM_DIR_DOWNSTREAM) {
		err = rtl9602c_gpon_port_bind(p, f, cfg->port_id);
		if (err)
			return err;
	}
	rtl9602c_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
		     RTL9602C_GEM_DS_FLOW_CFG_TYPE, type);

	if (cfg->direction & IFLA_GEM_DIR_UPSTREAM) {
		if (!rtl9602c_gem_tcont_bound(p, cfg->tcont)) {
			rtl9602c_gem_park(p, f);
			pr_warn("<GEM> port %u: T-CONT %u has no Alloc-ID; upstream parked\n",
				cfg->port_id, cfg->tcont);
			return 0;
		}

		t = rtl9602c_gem_tcont(cfg->tcont);
		q = rtl9602c_gem_tcont_qbit(cfg->tcont, 0);
		err = rtl9602c_pon_queue_add(p, t, q);
		if (err)
			return err;

		rtl9602c_rmw(p, RTL9602C_GEM_US_FLOW_CFG(f),
			     RTL9602C_GEM_US_FLOW_CFG_PORTID,
			     FIELD_PREP(RTL9602C_GEM_US_FLOW_CFG_PORTID,
					cfg->port_id));
		rtl9602c_ponq_stream_queue_set(p, f, RTL9602C_PON_QUEUE(t, q));
		rtl9602c_ponmac_sid_valid(p, f, true);
	}

	return 0;
}

/*
 * Undo rtl9602c_gem_program() for flow @f: the stream, when the flow has
 * one, is no longer admitted and goes back to the parking queue, the
 * upstream Port-ID and the downstream type are zeroed, and the flow's entry
 * in the Port-ID table is cleared. The queue stays in its T-CONT's map,
 * since queues are never detached.
 */
/* Park the stream of flow @f: not admitted, on the parking queue. */
static void rtl9602c_gem_park(struct rtl9602c_priv *p, unsigned int f)
{
	if (f < RTL9602C_PON_NUM_SID) {
		rtl9602c_ponmac_sid_valid(p, f, false);
		rtl9602c_ponq_stream_queue_set(p, f,
					       RTL9602C_GEM_PARKING_QUEUE);
	}
}

static void rtl9602c_gem_unprogram(struct rtl9602c_priv *p, unsigned int f)
{
	rtl9602c_gem_park(p, f);
	rtl9602c_rmw(p, RTL9602C_GEM_US_FLOW_CFG(f),
		     RTL9602C_GEM_US_FLOW_CFG_PORTID, 0);
	rtl9602c_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
		     RTL9602C_GEM_DS_FLOW_CFG_TYPE, 0);
	rtl9602c_gpon_port_unbind(p, f);
}

static void rtl9602c_gem_cntr_sync(struct rtl9602c_priv *p, unsigned int f);

static int rtl9602c_gem_add(struct pon_device *pon,
			    const struct pon_gem_config *cfg,
			    unsigned int *flow, struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);
	unsigned int f, last;
	int err = 0;

	if ((cfg->direction & IFLA_GEM_DIR_UPSTREAM) &&
	    cfg->tcont >= RTL9602C_GEM_NUM_TCONT) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "No T-CONT %u: traffic uses T-CONTs 0 to %u",
				       cfg->tcont, RTL9602C_GEM_NUM_TCONT - 1);
		return -ERANGE;
	}

	/* Every queue is strict and unshaped (rtl9602c_pon_queue_set()). */
	if (cfg->weight || cfg->cir || cfg->pir) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Upstream weights and rates are not supported");
		return -EOPNOTSUPP;
	}

	mutex_lock(&p->gpon_lock);

	/* GEM ports need the PON MAC, which a bring-up of the PON builds. */
	if (!p->ponmac_up) {
		NL_SET_ERR_MSG_MOD(extack, "The PON MAC is not up yet");
		err = -ENETDOWN;
		goto out;
	}

	/*
	 * A GEM Port-ID identifies one logical connection: the OMCC's is
	 * assigned by Configure_Port-ID and every other through the OMCC
	 * (ITU-T G.984.3 clause 5.5.5). So neither the OMCC's Port-ID, while it
	 * is bound, nor one that already has a GEM interface is taken.
	 */
	if (READ_ONCE(p->gpon_omcc_bound) &&
	    p->gpon_omcc_port == cfg->port_id) {
		NL_SET_ERR_MSG_FMT_MOD(extack, "Port-ID %u is the OMCC's",
				       cfg->port_id);
		err = -EEXIST;
		goto out;
	}

	for_each_set_bit(f, p->gem_flows, RTL9602C_GPON_NUM_FLOW) {
		if (p->gem_cfg[f].port_id == cfg->port_id) {
			NL_SET_ERR_MSG_FMT_MOD(extack,
					       "Port-ID %u has a GEM interface already",
					       cfg->port_id);
			err = -EEXIST;
			goto out;
		}
	}

	/*
	 * The lowest free flow, never the OMCC's. The GPON MAC has
	 * RTL9602C_GPON_NUM_FLOW flows, but the PON MAC has upstream streams
	 * for flows 0 to RTL9602C_PON_NUM_SID - 1 only, the OMCC's among them:
	 * a port that sends upstream takes one of those or none.
	 */
	last = cfg->direction & IFLA_GEM_DIR_UPSTREAM ? RTL9602C_PON_NUM_SID :
							RTL9602C_GPON_NUM_FLOW;
	for (f = 0; f < last; f++)
		if (f != RTL9602C_GPON_OMCC_FLOW && !test_bit(f, p->gem_flows))
			break;
	if (f == last) {
		if (last == RTL9602C_PON_NUM_SID)
			NL_SET_ERR_MSG_MOD(extack,
					   "Every GEM flow with an upstream stream is in use");
		else
			NL_SET_ERR_MSG_MOD(extack, "Every GEM flow is in use");
		err = -ENOSPC;
		goto out;
	}

	err = rtl9602c_gem_program(p, f, cfg);
	if (err) {
		rtl9602c_gem_unprogram(p, f);
		NL_SET_ERR_MSG_MOD(extack, "The MAC did not take the GEM port");
		goto out;
	}

	/*
	 * The flow's upstream octet counter keeps its count across a warm reset
	 * (measured on this board): this read empties it, and the port's total
	 * starts at zero.
	 */
	rtl9602c_gem_cntr_sync(p, f);
	p->gem_tx_octets[f] = 0;

	p->gem_cfg[f] = *cfg;
	set_bit(f, p->gem_flows);
	*flow = f;
	pr_debug("GEM port %u in flow %u\n", cfg->port_id, f);
out:
	mutex_unlock(&p->gpon_lock);
	return err;
}

/*
 * The switch keeps isolation, learning and VLAN membership per port and sees
 * every GEM port as its one PON port, so all GEM interfaces share one bridge.
 * rtl9602c_bridge_refresh() then makes the PON port a member of that bridge
 * and rebuilds the classifier entries that give the frames the switch
 * forwards to the PON port their stream; where no VLAN claims a stream, the
 * entries name the flow of the last upstream GEM interface to join, or, once
 * that one has left, of another (rtl9602c_gem_unbridge()).
 */
static int rtl9602c_gem_bridge_join(struct pon_device *pon, unsigned int flow,
				    struct net_device *br,
				    struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);
	bool upstream = p->gem_cfg[flow].direction & IFLA_GEM_DIR_UPSTREAM;

	if (p->pon_br && p->pon_br != br) {
		NL_SET_ERR_MSG_MOD(extack,
				   "the GEM interfaces can be in one bridge only");
		return -EBUSY;
	}
	set_bit(flow, p->gem_bridged);
	p->pon_br = br;
	p->pon_br_members++;

	if (upstream)
		p->pon_br_us_flow = flow;
	rtl9602c_bridge_refresh(&p->ds);
	return 0;
}

/*
 * Take @flow out of the bridge. Both gem_bridge_leave and gem_del come here,
 * in either order, and the second finds nothing to do. If @flow was the one
 * the bridge's stream entries name, the upstream GEM interface of the lowest
 * flow left in the bridge takes its place, so that the bridge's frames keep a
 * stream while it has one. The flow gives up its VLAN claims here, since the
 * bridge reports a port's VLANs going away only while it is still a bridge
 * port. When the last GEM interface leaves, the addresses the PON port
 * learned are flushed.
 */
static void rtl9602c_gem_unbridge(struct rtl9602c_priv *p, unsigned int flow)
{
	unsigned int f;

	if (!test_and_clear_bit(flow, p->gem_bridged))
		return;

	if (p->pon_br_us_flow == flow) {
		p->pon_br_us_flow = -1;
		for_each_set_bit(f, p->gem_bridged, RTL9602C_GPON_NUM_FLOW) {
			if (p->gem_cfg[f].direction & IFLA_GEM_DIR_UPSTREAM) {
				p->pon_br_us_flow = f;
				break;
			}
		}
	}

	for (unsigned int vid = 0; vid < VLAN_N_VID; vid++)
		if (p->gem_vid_flow[vid] == flow + 1)
			p->gem_vid_flow[vid] = 0;
	if (!--p->pon_br_members)
		p->pon_br = NULL;
	rtl9602c_bridge_refresh(&p->ds);

	if (!p->pon_br)
		rtl9602c_port_fast_age(&p->ds, RTL9602C_PORT_PON);
}

static void rtl9602c_gem_bridge_leave(struct pon_device *pon,
				      unsigned int flow, struct net_device *br)
{
	rtl9602c_gem_unbridge(pon_device_get_drvdata(pon), flow);
}

/*
 * The switch keeps VLAN membership per port, so a bridged GEM interface's
 * VLANs are the PON port's. An upstream GEM interface also claims each of its
 * VLANs for its flow (gem_vid_flow holds the flow plus one, 0 for none): once
 * VLANs claim flows, the classifier entries that give forwarded frames their
 * stream match the ingress port and the C-VID, so a VLAN can travel up one
 * GEM interface only, and a second upstream GEM interface in a claimed VLAN
 * is refused.
 */
static int rtl9602c_gem_vlan_add(struct pon_device *pon, unsigned int flow,
				 u16 vid, u16 flags,
				 struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);
	struct switchdev_obj_port_vlan vlan = { .vid = vid, .flags = flags };
	bool upstream = p->gem_cfg[flow].direction & IFLA_GEM_DIR_UPSTREAM;
	int err;

	if (upstream && vid < VLAN_N_VID && p->gem_vid_flow[vid] &&
	    p->gem_vid_flow[vid] != flow + 1) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "VLAN %u already travels up another GEM interface",
				       vid);
		return -EBUSY;
	}

	err = rtl9602c_port_vlan_add(&p->ds, RTL9602C_PORT_PON, &vlan, extack);
	if (err)
		return err;

	if (upstream && vid < VLAN_N_VID) {
		p->gem_vid_flow[vid] = (u8)(flow + 1);
		rtl9602c_cf_sid_refresh(p);
	}
	return 0;
}

/*
 * The PON class calls this only once no other GEM interface of the bridge
 * has @vid, so the PON port leaves the VLAN.
 */
static void rtl9602c_gem_vlan_del(struct pon_device *pon, unsigned int flow,
				  u16 vid)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);
	struct switchdev_obj_port_vlan vlan = { .vid = vid };

	if (vid < VLAN_N_VID && p->gem_vid_flow[vid] == flow + 1) {
		p->gem_vid_flow[vid] = 0;
		rtl9602c_cf_sid_refresh(p);
	}
	rtl9602c_port_vlan_del(&p->ds, RTL9602C_PORT_PON, &vlan);
}

/*
 * Whether the bridge sees a multicast router behind a GEM port decides what
 * the PON port does with unregistered multicast (rtl9602c_mcast_refresh()).
 */
static void rtl9602c_gem_mrouter(struct pon_device *pon, unsigned int flow,
				 bool mrouter)
{
	rtl9602c_mcast_refresh(pon_device_get_drvdata(pon));
}

/**
 * rtl9602c_gem_us_ready() - whether a GEM port's upstream is programmed
 * @p: the switch
 * @flow: the flow of a GEM interface
 *
 * The flowtable offload asks this before it lets the switch carry a flow out
 * of a GEM interface: the PON MAC must be built, and @flow must be a GEM port
 * that carries upstream traffic on a T-CONT that holds an Alloc-ID. Such a
 * port has a stream, since rtl9602c_gem_add() gives it a flow that has one.
 *
 * Context: process context; takes gpon_lock.
 * Return: true when all of that holds.
 */
bool rtl9602c_gem_us_ready(struct rtl9602c_priv *p, unsigned int flow)
{
	bool ready;

	if (flow >= RTL9602C_PON_NUM_SID)
		return false;

	mutex_lock(&p->gpon_lock);
	ready = p->ponmac_up && test_bit(flow, p->gem_flows) &&
		(p->gem_cfg[flow].direction & IFLA_GEM_DIR_UPSTREAM) &&
		rtl9602c_gem_tcont_bound(p, p->gem_cfg[flow].tcont);
	mutex_unlock(&p->gpon_lock);
	return ready;
}

/*
 * The port leaves the bridge first; the PON class may report that only after
 * this call. A routed flow that names the port is forgotten with the
 * references of the devices over it, and the stream entries are rebuilt
 * without it. Then, if the PON MAC is built, the port's hardware is undone.
 */
static void rtl9602c_gem_del(struct pon_device *pon, unsigned int flow)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);

	rtl9602c_gem_unbridge(p, flow);

	if (p->l34_us_flow == (int)flow) {
		p->l34_us_flow = -1;
		p->l34_us_refs = 0;
		rtl9602c_cf_sid_refresh(p);
	}

	mutex_lock(&p->gpon_lock);
	if (test_and_clear_bit(flow, p->gem_flows) && p->ponmac_up)
		rtl9602c_gem_unprogram(p, flow);
	mutex_unlock(&p->gpon_lock);
}

/**
 * rtl9602c_gem_restore() - program every GEM port again after a bring-up
 * @p: the switch
 *
 * Bring-up empties the GPON MAC's tables, which the GEM ports live in, while
 * their interfaces remain; this programs each port again. None of T-CONTs 0
 * to 15 holds an Alloc-ID at that point, so a port that sends upstream gets
 * only its downstream half, the "upstream parked" warning and its stream
 * parked, and its upstream half follows when the Alloc-ID is assigned again
 * (rtl9602c_gem_tcont_ready()). The PON MAC is not rebuilt, so without the
 * parking the stream would keep its old queue and admission meanwhile.
 */
void rtl9602c_gem_restore(struct rtl9602c_priv *p)
{
	unsigned int f;

	for_each_set_bit(f, p->gem_flows, RTL9602C_GPON_NUM_FLOW)
		if (rtl9602c_gem_program(p, f, &p->gem_cfg[f]))
			pr_warn("GEM port %u was not restored\n",
				p->gem_cfg[f].port_id);
}

/**
 * rtl9602c_gem_tcont_ready() - start the upstream of the GEM ports on a T-CONT
 * @p: the switch
 * @tcont: the PON class's T-CONT that has just been given an Alloc-ID
 *
 * Called when Assign_Alloc-ID binds an Alloc-ID to @tcont. Programs every
 * upstream GEM port on @tcont again, now with its upstream half, and logs
 * whether each one's upstream started. Nothing is done before the PON MAC is
 * built.
 *
 * Context: gpon_lock held.
 */
void rtl9602c_gem_tcont_ready(struct rtl9602c_priv *p, u32 tcont)
{
	unsigned int f;

	lockdep_assert_held(&p->gpon_lock);

	if (!p->ponmac_up)
		return;

	for_each_set_bit(f, p->gem_flows, RTL9602C_GPON_NUM_FLOW) {
		if (p->gem_cfg[f].tcont != tcont ||
		    !(p->gem_cfg[f].direction & IFLA_GEM_DIR_UPSTREAM))
			continue;
		if (rtl9602c_gem_program(p, f, &p->gem_cfg[f]))
			pr_warn("<GEM> port %u: upstream did not start on T-CONT %u\n",
				p->gem_cfg[f].port_id, tcont);
		else
			pr_info("<GEM> port %u: upstream started, T-CONT %u assigned\n",
				p->gem_cfg[f].port_id, tcont);
	}
}

/*
 * Straight to the conduit, on its queue for the PON port, which names the
 * port the frame leaves by. The conduit writes the flow from the frame's
 * port-mux dst into the transmit descriptor, and that gives the frame its
 * stream. Sent through the pon interface instead, the frame would lose the
 * dst, and the flow with it.
 */
static int rtl9602c_gem_xmit(struct pon_device *pon, struct sk_buff *skb)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);
	struct dsa_port *cpu_dp = dsa_to_port(&p->ds, RTL9602C_PORT_CPU);

	skb->dev = cpu_dp->conduit;
	skb_set_queue_mapping(skb, RTL9602C_PORT_PON);
	return dev_queue_xmit(skb);
}

/*
 * Filled under gpon_lock, so that the report is one moment of the activation
 * state machine.
 */
static int rtl9602c_pon_dev_status(struct pon_device *pon,
				   struct pon_dev_status *st)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);

	mutex_lock(&p->gpon_lock);
	rtl9602c_gpon_fill_status(p, st);
	mutex_unlock(&p->gpon_lock);

	return 0;
}

/*
 * Add flow @f's upstream octets to its GEM port's total. The counter is 64
 * bits, the low word at the lower address; it counts every GEM frame whole,
 * the 5-byte GEM header included, and clears when read (all measured on this
 * board). So the total is kept here, and nothing else may read the counter
 * of a GEM port's flow: any other read would take octets out of the total.
 */
static void rtl9602c_gem_cntr_sync(struct rtl9602c_priv *p, unsigned int f)
{
	u32 lo, hi;

	lockdep_assert_held(&p->gpon_lock);

	if (!p->ponmac_up)
		return;

	lo = rtl9602c_r(p, RTL9602C_GEM_US_FLOW_BYTES(f));
	hi = rtl9602c_r(p, RTL9602C_GEM_US_FLOW_BYTES(f) + 4);
	p->gem_tx_octets[f] += (u64)hi << 32 | lo;
}

static int rtl9602c_gem_stats(struct pon_device *pon, unsigned int flow,
			      struct pon_gem_stats *st)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);

	if (flow >= RTL9602C_GPON_NUM_FLOW)
		return -ERANGE;

	mutex_lock(&p->gpon_lock);
	rtl9602c_gem_cntr_sync(p, flow);
	st->tx_octets = p->gem_tx_octets[flow];
	mutex_unlock(&p->gpon_lock);

	return 0;
}

/*
 * Answered from the driver's record of the Alloc-IDs it bound, not from the
 * GPON MAC's Alloc-ID table: the table keeps its entries through a reset, so
 * it tells what was written, not what the OLT still assigns.
 */
static int rtl9602c_tcont_get(struct pon_device *pon, unsigned int index,
			      u16 *alloc_id)
{
	struct rtl9602c_priv *p = pon_device_get_drvdata(pon);
	unsigned int t = rtl9602c_gem_tcont(index);
	int err = -ENOENT;

	if (index >= RTL9602C_GEM_NUM_TCONT)
		return -ERANGE;

	mutex_lock(&p->gpon_lock);
	if (p->gpon_tcont_used & BIT(t)) {
		*alloc_id = p->gpon_alloc[t];
		err = 0;
	}
	mutex_unlock(&p->gpon_lock);

	return err;
}

static const struct pon_device_ops rtl9602c_pon_ops = {
	.owns_netdev		= rtl9602c_gem_owns_netdev,
	.gem_add		= rtl9602c_gem_add,
	.gem_del		= rtl9602c_gem_del,
	.gem_xmit		= rtl9602c_gem_xmit,
	.gem_bridge_join	= rtl9602c_gem_bridge_join,
	.gem_bridge_leave	= rtl9602c_gem_bridge_leave,
	.gem_vlan_add		= rtl9602c_gem_vlan_add,
	.gem_vlan_del		= rtl9602c_gem_vlan_del,
	.gem_mrouter		= rtl9602c_gem_mrouter,
	.dev_status		= rtl9602c_pon_dev_status,
	.gem_stats		= rtl9602c_gem_stats,
	.tcont_get		= rtl9602c_tcont_get,
};

/*
 * The receive handler for data frames: a frame from the PON port in any flow
 * but the OMCC's goes to the GEM interface of its flow, if there is one. A
 * frame not taken goes on to its user port.
 */
static bool rtl9602c_gem_rx(void *ctx, struct sk_buff *skb,
			    unsigned int port, unsigned int flow)
{
	struct rtl9602c_priv *p = ctx;

	if (port != RTL9602C_PORT_PON || flow == RTL9602C_GPON_OMCC_FLOW)
		return false;

	return pon_gem_rx(p->pon, flow, skb);
}

/**
 * rtl9602c_gem_setup() - register the switch's data path with the PON class
 * @p: the switch
 *
 * Registers the switch as the ONU behind its PON port, with all
 * RTL9602C_GPON_NUM_FLOW flows of the GPON MAC, although the OMCC's is never
 * given to a GEM port, and the RTL9602C_GEM_NUM_TCONT T-CONTs the GEM ports
 * use, each with the one queue a GEM port can name: the scheduler's T-CONT 16
 * is the OMCC's. Then attaches the receive handler for data frames to the
 * conduit. Nothing is registered without the
 * PON MAC's system controller and buffers, or without a conduit.
 *
 * Return: 0, also when nothing is registered, or a negative error code, with
 * nothing registered.
 */
int rtl9602c_gem_setup(struct rtl9602c_priv *p)
{
	struct dsa_port *cpu_dp = dsa_to_port(&p->ds, RTL9602C_PORT_CPU);
	struct pon_device *pon;
	int err;

	if (!p->sysctl || !cpu_dp || !cpu_dp->conduit)
		return 0;

	pon = pon_device_register(p->dev, &rtl9602c_pon_ops,
				  RTL9602C_GPON_NUM_FLOW,
				  RTL9602C_GEM_NUM_TCONT, 1, p);
	if (IS_ERR(pon))
		return PTR_ERR(pon);
	/* The handler delivers through p->pon, so it is set first. */
	p->pon = pon;

	err = rtl9602c_conduit_flow_attach(cpu_dp->conduit, rtl9602c_gem_rx,
					   p);
	if (err) {
		pon_device_unregister(pon);
		p->pon = NULL;
	}

	return err;
}

/**
 * rtl9602c_gem_teardown() - unregister the switch's data path
 * @p: the switch
 *
 * Detaches the receive handler for data frames, so that no frame reaches
 * pon_gem_rx() any more, then unregisters the data path, which deletes the
 * GEM interfaces. Runs before rtl9602c_omci_teardown(), since the PON class
 * needs the data path unregistered before the OMCI port goes.
 */
void rtl9602c_gem_teardown(struct rtl9602c_priv *p)
{
	struct dsa_port *cpu_dp = dsa_to_port(&p->ds, RTL9602C_PORT_CPU);

	if (!p->pon)
		return;

	rtl9602c_conduit_flow_detach(cpu_dp->conduit);
	pon_device_unregister(p->pon);
	p->pon = NULL;
}
