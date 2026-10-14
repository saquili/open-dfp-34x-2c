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

#include "rtl9602c-pon.h"

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
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
	struct dsa_port *dp;

	if (!dsa_user_dev_check(dev))
		return false;

	dp = dsa_port_from_netdev((struct net_device *)dev);
	return !IS_ERR(dp) && dp->ds == p->ds &&
	       dp->index == RTL9602C_PORT_PON;
}

/*
 * Whether the GPON MAC holds an Alloc-ID for the PON class's T-CONT @tcont.
 * It holds one once the OLT has assigned it with Assign_Alloc-ID, a PLOAM
 * message, which may come before or after the OMCI that creates a GEM port
 * on the T-CONT.
 */
static bool rtl9602c_gem_tcont_bound(struct rtl9602c_pon *p, u32 tcont)
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
 * With the Alloc-ID, the upstream half follows (rtl9602c_gem_us_program()).
 */
static void rtl9602c_gem_park(struct rtl9602c_pon *p, unsigned int f);

/*
 * A GEM port's weight w goes into the queue's 10-bit field as 4w: two queues
 * weighted 1 and 3 shared 29 % to 71 %, while 4 and 12 shared 25.4 % to
 * 74.6 % (measured on this board).
 */
#define RTL9602C_GEM_WEIGHT_SCALE	4

/*
 * Set the scheduling of the queue of priority @queue of GEM T-CONT @tcont
 * from every upstream GEM port that sends through it: weighted, by the
 * largest weight among them, if one of them has a weight; their committed
 * rates added up; their peak rates added up, or no limit if one of them has
 * none. A committed rate is rounded up to whole units and a peak rate down,
 * to one unit at least; both are kept within the field, and the committed
 * rate within the peak rate. A queue that no GEM port sends through is made
 * strict and unshaped again.
 */
static void rtl9602c_gem_queue_sched(struct rtl9602c_pon *p, u32 tcont,
				     u8 queue)
{
	unsigned int f, weight = 0;
	u64 cir = 0, pir = 0;
	bool limited = true;
	u32 cir_u, pir_u;

	lockdep_assert_held(&p->gpon_lock);

	for_each_set_bit(f, p->gem_flows, RTL9602C_PON_NUM_SID) {
		const struct pon_gem_config *c = &p->gem_cfg[f];

		if (!(c->direction & IFLA_GEM_DIR_UPSTREAM) ||
		    c->tcont != tcont || c->queue != queue)
			continue;
		weight = max_t(unsigned int, weight, c->weight);
		if (check_add_overflow(cir, c->cir, &cir))
			cir = U64_MAX;
		if (!c->pir)
			limited = false;
		else if (check_add_overflow(pir, c->pir, &pir))
			pir = U64_MAX;
	}

	rtl9602c_gem_rate_units(cir, pir, limited, &cir_u, &pir_u);

	rtl9602c_pon_queue_set(p, RTL9602C_PON_QUEUE(rtl9602c_gem_tcont(tcont),
						     rtl9602c_gem_tcont_qbit(tcont, queue)),
			       weight * RTL9602C_GEM_WEIGHT_SCALE, cir_u, pir_u);
}

/*
 * The upstream half of flow @f, for a GEM port on a T-CONT that holds an
 * Alloc-ID: the port's queue of its T-CONT is attached
 * (rtl9602c_gem_tcont_qbit()), the flow is given the Port-ID its upstream
 * GEM headers carry, the stream is pointed at the queue and admitted, and
 * the queue is scheduled for the GEM ports that send through it, @cfg's
 * port among them (rtl9602c_gem_queue_sched()): the caller has recorded
 * @cfg as the flow's configuration.
 */
static int rtl9602c_gem_us_program(struct rtl9602c_pon *p, unsigned int f,
				   const struct pon_gem_config *cfg)
{
	unsigned int t = rtl9602c_gem_tcont(cfg->tcont);
	unsigned int q = rtl9602c_gem_tcont_qbit(cfg->tcont, cfg->queue);
	int err;

	err = rtl9602c_pon_queue_add(p, t, q);
	if (err)
		return err;

	rtl9602c_pon_rmw(p, RTL9602C_GEM_US_FLOW_CFG(f),
			 RTL9602C_GEM_US_FLOW_CFG_PORTID,
		     FIELD_PREP(RTL9602C_GEM_US_FLOW_CFG_PORTID, cfg->port_id));
	rtl9602c_ponq_stream_queue_set(p, f, RTL9602C_PON_QUEUE(t, q));
	rtl9602c_ponmac_sid_valid(p, f, true);
	rtl9602c_gem_queue_sched(p, cfg->tcont, cfg->queue);
	return 0;
}

static int rtl9602c_gem_program(struct rtl9602c_pon *p, unsigned int f,
				const struct pon_gem_config *cfg)
{
	u32 type = RTL9602C_GEM_DS_FLOW_CFG_TYPE_ETH;
	int err;

	if (cfg->multicast)
		type |= RTL9602C_GEM_DS_FLOW_CFG_TYPE_MC;
	type |= rtl9602c_gpon_port_aes(p, cfg->port_id);

	if (cfg->direction & IFLA_GEM_DIR_DOWNSTREAM) {
		err = rtl9602c_gpon_port_bind(p, f, cfg->port_id);
		if (err)
			return err;
	}
	rtl9602c_pon_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
			 RTL9602C_GEM_DS_FLOW_CFG_TYPE, type);

	if (cfg->direction & IFLA_GEM_DIR_UPSTREAM) {
		if (!rtl9602c_gem_tcont_bound(p, cfg->tcont)) {
			rtl9602c_gem_park(p, f);
			pr_warn("<GEM> port %u: T-CONT %u has no Alloc-ID; upstream parked\n",
				cfg->port_id, cfg->tcont);
			return 0;
		}

		return rtl9602c_gem_us_program(p, f, cfg);
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
static void rtl9602c_gem_park(struct rtl9602c_pon *p, unsigned int f)
{
	if (f < RTL9602C_PON_NUM_SID) {
		rtl9602c_ponmac_sid_valid(p, f, false);
		rtl9602c_ponq_stream_queue_set(p, f,
					       RTL9602C_GEM_PARKING_QUEUE);
	}
}

static void rtl9602c_gem_unprogram(struct rtl9602c_pon *p, unsigned int f)
{
	rtl9602c_gem_park(p, f);
	rtl9602c_pon_rmw(p, RTL9602C_GEM_US_FLOW_CFG(f),
			 RTL9602C_GEM_US_FLOW_CFG_PORTID, 0);
	rtl9602c_pon_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
			 RTL9602C_GEM_DS_FLOW_CFG_TYPE, 0);
	rtl9602c_gpon_port_unbind(p, f);
}

static void rtl9602c_gem_cntr_sync(struct rtl9602c_pon *p, unsigned int f);
static void rtl9602c_gem_del(struct pon_device *pon, unsigned int flow);

static int rtl9602c_gem_add(struct pon_device *pon,
			    const struct pon_gem_config *cfg,
			    unsigned int *flow, struct netlink_ext_ack *extack)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
	unsigned int f, last;
	int err = 0;

	if ((cfg->direction & IFLA_GEM_DIR_UPSTREAM) &&
	    cfg->tcont >= RTL9602C_GEM_NUM_TCONT) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "No T-CONT %u: traffic uses T-CONTs 0 to %u",
				       cfg->tcont, RTL9602C_GEM_NUM_TCONT - 1);
		return -ERANGE;
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

	/*
	 * Recorded first, so that the scheduling of the port's queue counts
	 * the port (rtl9602c_gem_queue_sched()).
	 */
	p->gem_cfg[f] = *cfg;
	set_bit(f, p->gem_flows);
	err = rtl9602c_gem_program(p, f, cfg);
	if (err) {
		clear_bit(f, p->gem_flows);
		rtl9602c_gem_unprogram(p, f);
		NL_SET_ERR_MSG_MOD(extack, "The MAC did not take the GEM port");
		goto out;
	}

	/*
	 * The flow's counters keep their counts across a warm reset and hold
	 * noise after power-up (measured on this board): this read empties
	 * them, and the port's totals start at zero.
	 */
	rtl9602c_gem_cntr_sync(p, f);
	p->gem_tx_octets[f] = 0;
	p->gem_tx_frames[f] = 0;
	p->gem_rx_frames[f] = 0;
	p->gem_rx_octets[f] = 0;
	RTL9602C_HOST(p, gem_added, f);
	*flow = f;
	pr_debug("GEM port %u in flow %u\n", cfg->port_id, f);
out:
	mutex_unlock(&p->gpon_lock);

	/*
	 * The switch limits what the OLT sends the port. It does so outside
	 * the lock, as it does for a change; a port it cannot limit is
	 * taken away again, as the class expects of a refusal.
	 */
	if (!err && cfg->ds_rate) {
		err = RTL9602C_HOST(p, gem_ds_limit, *flow, cfg->ds_rate,
				    cfg->ds_burst, extack);
		if (err)
			rtl9602c_gem_del(pon, *flow);
	}
	return err;
}

/*
 * The switch keeps isolation, learning and VLAN membership per port and sees
 * every GEM port as its one PON port, so all GEM interfaces share one bridge.
 * The switch driver keeps the bridge, makes its PON port a member and gives
 * the frames it forwards to that port their stream (rtl9602c-pon-host.c);
 * here the flow says only whether it carries upstream traffic.
 */
static int rtl9602c_gem_bridge_join(struct pon_device *pon, unsigned int flow,
				    struct net_device *br,
				    struct netlink_ext_ack *extack)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
	bool upstream = p->gem_cfg[flow].direction & IFLA_GEM_DIR_UPSTREAM;

	return RTL9602C_HOST(p, gem_bridge_join, flow, upstream, br, extack);
}

static void rtl9602c_gem_bridge_leave(struct pon_device *pon,
				      unsigned int flow, struct net_device *br)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);

	RTL9602C_HOST(p, gem_bridge_leave, flow);
}

/*
 * The switch keeps VLAN membership per port, so a bridged GEM interface's
 * VLANs are the PON port's. An upstream GEM interface also claims the frames
 * of each of its VLANs with its IEEE 802.1p priorities for its flow: once
 * VLANs claim flows, the classifier entries that give forwarded frames their
 * stream match the ingress port, the C-VID and, where the GEM interfaces of a
 * VLAN divide its priorities, the C-tag's priority. So upstream GEM
 * interfaces may share a VLAN only with disjoint priorities, and one that
 * would take a priority another holds is refused; the switch driver keeps
 * the claims (rtl9602c-pon-host.c).
 */
static int rtl9602c_gem_vlan_add(struct pon_device *pon, unsigned int flow,
				 u16 vid, u16 flags,
				 struct netlink_ext_ack *extack)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
	const struct pon_gem_config *cfg = &p->gem_cfg[flow];

	return RTL9602C_HOST(p, gem_vlan_add, flow,
			     cfg->direction & IFLA_GEM_DIR_UPSTREAM, cfg->pbits,
			     vid, flags, extack);
}

/*
 * The flow gives up its claims on @vid; the PON port leaves the VLAN once no
 * GEM interface of the bridge has it (@last).
 */
static void rtl9602c_gem_vlan_del(struct pon_device *pon, unsigned int flow,
				  u16 vid, bool last)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);

	RTL9602C_HOST(p, gem_vlan_del, flow, vid, last);
}

/*
 * Whether the bridge sees a multicast router behind a GEM port decides what
 * the PON port does with unregistered multicast, which the switch driver
 * works out.
 */
static void rtl9602c_gem_mrouter(struct pon_device *pon, unsigned int flow,
				 bool mrouter)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);

	RTL9602C_HOST(p, gem_mrouter);
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
bool rtl9602c_gem_us_ready(struct rtl9602c_pon *p, unsigned int flow)
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
 * without it. Then, if the PON MAC is built, the port's hardware is undone,
 * and the queue it sent through is scheduled for the GEM ports left in it.
 */
static void rtl9602c_gem_del(struct pon_device *pon, unsigned int flow)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
	const struct pon_gem_config *cfg = &p->gem_cfg[flow];

	RTL9602C_HOST(p, gem_unbind, flow);

	mutex_lock(&p->gpon_lock);
	if (test_and_clear_bit(flow, p->gem_flows) && p->ponmac_up) {
		rtl9602c_gem_unprogram(p, flow);
		if ((cfg->direction & IFLA_GEM_DIR_UPSTREAM) &&
		    rtl9602c_gem_tcont_bound(p, cfg->tcont))
			rtl9602c_gem_queue_sched(p, cfg->tcont, cfg->queue);
	}
	mutex_unlock(&p->gpon_lock);

	RTL9602C_HOST(p, gem_forget, flow);
}

/*
 * The unicast frames the GEM interface of @flow takes, as the class hands
 * them on (pon_device_ops.gem_rx_filter). The switch driver keeps them per
 * flow, and the static entries and the PON port's unknown unicast follow.
 */
static void rtl9602c_gem_rx_filter(struct pon_device *pon, unsigned int flow,
				   const struct pon_gem_rx_filter *f)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);

	if (flow >= RTL9602C_GPON_NUM_FLOW || !test_bit(flow, p->gem_flows))
		return;

	RTL9602C_HOST(p, gem_rx_filter, flow, f);
}

/*
 * New attributes for a GEM port. A new downstream limit goes to the switch
 * first (the host's gem_ds_limit()). New priorities take the flow's
 * VLAN claims again (the host's gem_pbits_change()), under RTNL as the
 * bridge's VLAN changes are. Then the configuration is recorded, and while
 * the port's upstream is programmed, the upstream half is programmed again
 * for it, which moves the stream to a new queue when the queue changes, and
 * the old queue is scheduled for the ports left in it. Frames already in the
 * old queue still leave in the T-CONT's grants. When the new queue cannot be
 * attached, the old configuration, claims and limit are put back.
 */
static int rtl9602c_gem_change(struct pon_device *pon, unsigned int flow,
			       const struct pon_gem_config *cfg,
			       struct netlink_ext_ack *extack)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
	struct pon_gem_config old = p->gem_cfg[flow];
	const bool ds_changed = cfg->ds_rate != old.ds_rate ||
				cfg->ds_burst != old.ds_burst;
	int err = 0;

	if (WARN_ON(!test_bit(flow, p->gem_flows)))
		return -ENOENT;

	if (ds_changed) {
		err = RTL9602C_HOST(p, gem_ds_limit, flow, cfg->ds_rate,
				    cfg->ds_burst, extack);
		if (err)
			return err;
	}

	if (cfg->pbits != old.pbits) {
		err = RTL9602C_HOST(p, gem_pbits_change, flow, cfg->pbits, extack);
		if (err)
			goto undo;
	}

	mutex_lock(&p->gpon_lock);
	p->gem_cfg[flow] = *cfg;
	if (!p->ponmac_up || !rtl9602c_gem_tcont_bound(p, cfg->tcont))
		goto out;

	err = rtl9602c_gem_us_program(p, flow, cfg);
	if (err) {
		p->gem_cfg[flow] = old;
		NL_SET_ERR_MSG_MOD(extack,
				   "The MAC could not attach the new queue");
		goto out;
	}
	if (old.queue != cfg->queue)
		rtl9602c_gem_queue_sched(p, old.tcont, old.queue);
out:
	mutex_unlock(&p->gpon_lock);
	if (err && cfg->pbits != old.pbits)
		RTL9602C_HOST(p, gem_pbits_change, flow, old.pbits, NULL);
undo:
	if (err && ds_changed)
		RTL9602C_HOST(p, gem_ds_limit, flow, old.ds_rate, old.ds_burst,
			      NULL);
	return err;
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
void rtl9602c_gem_restore(struct rtl9602c_pon *p)
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
void rtl9602c_gem_tcont_ready(struct rtl9602c_pon *p, u32 tcont)
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
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
	struct dsa_port *cpu_dp = dsa_to_port(p->ds, RTL9602C_PORT_CPU);

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
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);

	mutex_lock(&p->gpon_lock);
	rtl9602c_gpon_fill_status(p, st);
	mutex_unlock(&p->gpon_lock);

	return 0;
}

/* Read flow @f's upstream GEM frame count, -ETIMEDOUT after 1 ms without it. */
static int rtl9602c_gem_us_frames(struct rtl9602c_pon *p, unsigned int f,
				  u32 *count)
{
	u32 v;

	rtl9602c_pon_w(p, RTL9602C_GEM_US_FLOW_CNT_SEL,
		       FIELD_PREP(RTL9602C_GEM_US_FLOW_CNT_SEL_FLOW, f));
	if (read_poll_timeout(rtl9602c_pon_r, v,
			      v & RTL9602C_GEM_US_FLOW_CNT_SEL_DONE, 0, 1000,
			      false, p, RTL9602C_GEM_US_FLOW_CNT_SEL))
		return -ETIMEDOUT;
	*count = rtl9602c_pon_r(p, RTL9602C_GEM_US_FLOW_CNT_DATA);
	return 0;
}

/*
 * Add flow @f's counters to its GEM port's totals. Each counter clears when
 * read (all measured on this board), so the totals are kept here, and nothing
 * else may read the counters of a GEM port's flow: any other read would take
 * counts out of the totals. The upstream octet counter is 64 bits, the low
 * word at the lower address, and counts every GEM frame whole, the 5-byte GEM
 * header included; the others are 32 bits, and the downstream octets are
 * payload, the Ethernet frames with their FCS. A count the MAC does not give
 * within 1 ms is left for the next time.
 */
static void rtl9602c_gem_cntr_sync(struct rtl9602c_pon *p, unsigned int f)
{
	u32 lo, hi, v;

	lockdep_assert_held(&p->gpon_lock);

	if (!p->ponmac_up)
		return;

	lo = rtl9602c_pon_r(p, RTL9602C_GEM_US_FLOW_BYTES(f));
	hi = rtl9602c_pon_r(p, RTL9602C_GEM_US_FLOW_BYTES(f) + 4);
	p->gem_tx_octets[f] += (u64)hi << 32 | lo;

	if (!rtl9602c_gem_us_frames(p, f, &v))
		p->gem_tx_frames[f] += v;
	v = rtl9602c_gpon_flow_cntr(p, f, false);
	if (v != U32_MAX)
		p->gem_rx_frames[f] += v;
	v = rtl9602c_gpon_flow_cntr(p, f, true);
	if (v != U32_MAX)
		p->gem_rx_octets[f] += v;
}

/**
 * rtl9602c_gem_cntr_sync_all() - add every GEM port's counters to its totals
 * @p: the switch
 *
 * The GPON worker calls this every RTL9602C_GEM_CNTR_POLLS runs, before a
 * counter can wrap, and the gpontbl view before it shows the totals.
 *
 * Context: gpon_lock held.
 */
void rtl9602c_gem_cntr_sync_all(struct rtl9602c_pon *p)
{
	unsigned int f;

	for_each_set_bit(f, p->gem_flows, RTL9602C_GPON_NUM_FLOW)
		rtl9602c_gem_cntr_sync(p, f);
}

static int rtl9602c_gem_stats(struct pon_device *pon, unsigned int flow,
			      struct pon_gem_stats *st)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);

	if (flow >= RTL9602C_GPON_NUM_FLOW)
		return -ERANGE;

	mutex_lock(&p->gpon_lock);
	rtl9602c_gem_cntr_sync(p, flow);
	st->tx_octets = p->gem_tx_octets[flow];
	st->tx_frames = p->gem_tx_frames[flow];
	st->rx_frames = p->gem_rx_frames[flow];
	st->rx_octets = p->gem_rx_octets[flow];
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
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);
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

/* The OMCC's mapping of a T-CONT to an Alloc-ID (rtl9602c_gpon_tcont_map()). */
static int rtl9602c_tcont_set(struct pon_device *pon, unsigned int index,
			      int alloc_id, struct netlink_ext_ack *extack)
{
	struct rtl9602c_pon *p = pon_device_get_drvdata(pon);

	if (index >= RTL9602C_GEM_NUM_TCONT)
		return -ENOENT;
	return rtl9602c_gpon_tcont_map(p, index,
				       alloc_id < 0 ? RTL9602C_GPON_ALLOC_NONE :
						      (u16)alloc_id,
				       extack);
}

/* The OLT's time of day (rtl9602c_gpon_tod_set()). */
static int rtl9602c_tod_set(struct pon_device *pon, u32 superframe,
			    const struct timespec64 *tstamp,
			    struct netlink_ext_ack *extack)
{
	return rtl9602c_gpon_tod_set(pon_device_get_drvdata(pon), superframe,
				     tstamp, extack);
}

static const struct pon_device_ops rtl9602c_pon_ops = {
	.owns_netdev		= rtl9602c_gem_owns_netdev,
	.gem_add		= rtl9602c_gem_add,
	.gem_change		= rtl9602c_gem_change,
	.gem_del		= rtl9602c_gem_del,
	.gem_xmit		= rtl9602c_gem_xmit,
	.gem_bridge_join	= rtl9602c_gem_bridge_join,
	.gem_bridge_leave	= rtl9602c_gem_bridge_leave,
	.gem_vlan_add		= rtl9602c_gem_vlan_add,
	.gem_vlan_del		= rtl9602c_gem_vlan_del,
	.gem_mrouter		= rtl9602c_gem_mrouter,
	.gem_rx_filter		= rtl9602c_gem_rx_filter,
	.dev_status		= rtl9602c_pon_dev_status,
	.gem_stats		= rtl9602c_gem_stats,
	.tcont_get		= rtl9602c_tcont_get,
	.tcont_set		= rtl9602c_tcont_set,
	.tod_set		= rtl9602c_tod_set,
};

/*
 * The receive handler for data frames: a frame from the PON port in any flow
 * but the OMCC's goes to the GEM interface of its flow, if there is one. A
 * frame not taken goes on to its user port.
 */
static bool rtl9602c_gem_rx(void *ctx, struct sk_buff *skb,
			    unsigned int port, unsigned int flow)
{
	struct rtl9602c_pon *p = ctx;

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
 * use, with RTL9602C_GEM_TCONT_QUEUES queues each: the scheduler's T-CONT 16
 * is the OMCC's. Then attaches the receive handler for data frames to the
 * conduit. Nothing is registered without the
 * PON MAC's system controller and buffers, or without a conduit.
 *
 * Return: 0, also when nothing is registered, or a negative error code, with
 * nothing registered.
 */
int rtl9602c_gem_setup(struct rtl9602c_pon *p)
{
	struct dsa_port *cpu_dp = dsa_to_port(p->ds, RTL9602C_PORT_CPU);
	struct pon_device *pon;
	int err;

	if (!p->sysctl || !cpu_dp || !cpu_dp->conduit)
		return 0;

	pon = pon_device_register(p->dev, &rtl9602c_pon_ops,
				  RTL9602C_GPON_NUM_FLOW,
				  RTL9602C_GEM_NUM_TCONT,
				  RTL9602C_GEM_TCONT_QUEUES, p);
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
void rtl9602c_gem_teardown(struct rtl9602c_pon *p)
{
	struct dsa_port *cpu_dp = dsa_to_port(p->ds, RTL9602C_PORT_CPU);

	if (!p->pon)
		return;

	rtl9602c_conduit_flow_detach(cpu_dp->conduit);
	pon_device_unregister(p->pon);
	p->pon = NULL;
}
