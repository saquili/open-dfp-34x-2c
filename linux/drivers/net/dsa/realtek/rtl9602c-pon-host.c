// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: the switch's side of the contract with the driver
 * of its GPON MAC (<linux/platform_data/rtl9602c-pon.h>).
 *
 * The host operations are what the switch does for the MAC: the interrupt's
 * sources, the laser's transmit-disable line and power level, and the
 * switch's side of the GEM ports. A GEM interface of the PON class is a flow
 * of the MAC, but the switch sees every GEM port as its one PON port, keeps
 * isolation, learning and VLAN membership for that port, and forwards between
 * the GEM ports and the bridge's other ports itself. What it keeps for them
 * is here: the bridge the GEM interfaces are in, the VLANs and priorities
 * each flow claims, the addresses the CPU takes, and the routed flows that
 * name a GEM port.
 *
 * The PON operations are what the MAC does for the switch. They are called
 * with the contract's semaphore held for reading, so that the MAC's driver can
 * unbind between two calls but not during one.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/auxiliary_bus.h>
#include <linux/bitmap.h>
#include <linux/if_link.h>
#include <linux/if_vlan.h>
#include <linux/pon.h>
#include <net/dsa.h>
#include <net/switchdev.h>

#include "rtl9602c.h"

/* The host operations */

static void rtl9602c_host_irq_enable(void *host, u32 clr, u32 set)
{
	rtl9602c_irq_enable(host, clr, set);
}

static int rtl9602c_host_tx_disable(void *host, bool disable)
{
	return rtl9602c_l_txdis_gpio(host, disable);
}

static int rtl9602c_host_hold_modulation(void *host)
{
	return rtl9602c_optics_hold_modulation(host);
}

static void rtl9602c_host_power_level_changed(void *host)
{
	struct rtl9602c_priv *p = host;

	queue_work(system_wq, &p->optics_level_work);
}

static void rtl9602c_host_gem_added(void *host, unsigned int flow)
{
	struct rtl9602c_priv *p = host;

	memset(&p->gem_rx[flow], 0, sizeof(p->gem_rx[flow]));
	set_bit(flow, p->gem_flows);
}

/*
 * Claim the frames of VID @vid with the IEEE 802.1p priorities in @pbits for
 * @flow, and give up the flow's claims on the VID's other priorities; refuse
 * when another flow holds one of @pbits. gem_vid_flow holds a claim as the
 * flow plus one, 0 for none.
 */
static int rtl9602c_gem_vid_claim(struct rtl9602c_priv *p, unsigned int flow,
				  u16 vid, u8 pbits,
				  struct netlink_ext_ack *extack)
{
	u8 *row = p->gem_vid_flow[vid];
	unsigned int pcp;

	for (pcp = 0; pcp < 8; pcp++) {
		if (!(pbits & BIT(pcp)) || !row[pcp] || row[pcp] == flow + 1)
			continue;
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "VLAN %u priority %u already travels up another GEM interface",
				       vid, pcp);
		return -EBUSY;
	}
	for (pcp = 0; pcp < 8; pcp++) {
		if (pbits & BIT(pcp))
			row[pcp] = flow + 1;
		else if (row[pcp] == flow + 1)
			row[pcp] = 0;
	}
	return 0;
}

/* Give up @flow's claims on VID @vid; return whether it had any. */
static bool rtl9602c_gem_vid_release(struct rtl9602c_priv *p,
				     unsigned int flow, u16 vid)
{
	u8 *row = p->gem_vid_flow[vid];
	bool had = false;
	unsigned int pcp;

	for (pcp = 0; pcp < 8; pcp++) {
		if (row[pcp] != flow + 1)
			continue;
		row[pcp] = 0;
		had = true;
	}
	return had;
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
static int rtl9602c_host_gem_bridge_join(void *host, unsigned int flow,
					 bool upstream, struct net_device *br,
					 struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = host;

	if (p->pon_br && p->pon_br != br) {
		NL_SET_ERR_MSG_MOD(extack,
				   "the GEM interfaces can be in one bridge only");
		return -EBUSY;
	}
	set_bit(flow, p->gem_bridged);
	if (upstream)
		set_bit(flow, p->gem_bridged_up);
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
	clear_bit(flow, p->gem_bridged_up);

	if (p->pon_br_us_flow == flow) {
		p->pon_br_us_flow = -1;
		f = find_first_bit(p->gem_bridged_up, RTL9602C_GPON_NUM_FLOW);
		if (f < RTL9602C_GPON_NUM_FLOW)
			p->pon_br_us_flow = f;
	}

	for (unsigned int vid = 0; vid < VLAN_N_VID; vid++)
		rtl9602c_gem_vid_release(p, flow, vid);
	if (!--p->pon_br_members)
		p->pon_br = NULL;
	rtl9602c_bridge_refresh(&p->ds);

	if (!p->pon_br)
		rtl9602c_port_fast_age(&p->ds, RTL9602C_PORT_PON);
}

static void rtl9602c_host_gem_bridge_leave(void *host, unsigned int flow)
{
	rtl9602c_gem_unbridge(host, flow);
}

/*
 * The switch keeps VLAN membership per port, so a bridged GEM interface's
 * VLANs are the PON port's. An upstream GEM interface also claims the frames
 * of each of its VLANs with its IEEE 802.1p priorities for its flow
 * (rtl9602c_gem_vid_claim()): once VLANs claim flows, the classifier entries
 * that give forwarded frames their stream match the ingress port, the C-VID
 * and, where the GEM interfaces of a VLAN divide its priorities, the C-tag's
 * priority. So upstream GEM interfaces may share a VLAN only with disjoint
 * priorities, and one that would take a priority another holds is refused.
 * The claim is made first, so that a refusal leaves the PON port's VLANs as
 * they were.
 */
static int rtl9602c_host_gem_vlan_add(void *host, unsigned int flow,
				      bool upstream, u8 pbits, u16 vid,
				      u16 flags, struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = host;
	struct switchdev_obj_port_vlan vlan = { .vid = vid, .flags = flags };
	u8 had[8];
	int err;

	if (upstream && vid < VLAN_N_VID) {
		memcpy(had, p->gem_vid_flow[vid], sizeof(had));
		err = rtl9602c_gem_vid_claim(p, flow, vid, pbits, extack);
		if (err)
			return err;
	}

	err = rtl9602c_port_vlan_add(&p->ds, RTL9602C_PORT_PON, &vlan, extack);
	if (err) {
		if (upstream && vid < VLAN_N_VID)
			memcpy(p->gem_vid_flow[vid], had, sizeof(had));
		return err;
	}

	if (upstream && vid < VLAN_N_VID)
		rtl9602c_cf_sid_refresh(p);
	return 0;
}

/*
 * The flow gives up its claims on @vid; the PON port leaves the VLAN once no
 * GEM interface of the bridge has it (@last).
 */
static void rtl9602c_host_gem_vlan_del(void *host, unsigned int flow, u16 vid,
				       bool last)
{
	struct rtl9602c_priv *p = host;
	struct switchdev_obj_port_vlan vlan = { .vid = vid };

	if (vid < VLAN_N_VID && rtl9602c_gem_vid_release(p, flow, vid))
		rtl9602c_cf_sid_refresh(p);
	if (last)
		rtl9602c_port_vlan_del(&p->ds, RTL9602C_PORT_PON, &vlan);
}

/*
 * The flow's claims on every VID it claims, taken again with new priorities
 * @pbits; refused, leaving every claim as it was, when another flow holds one
 * of them in one of those VLANs.
 */
static int rtl9602c_host_gem_pbits_change(void *host, unsigned int flow,
					  u8 pbits,
					  struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = host;
	unsigned int vid, pcp;

	for (vid = 0; vid < VLAN_N_VID; vid++) {
		const u8 *row = p->gem_vid_flow[vid];

		if (!memchr(row, flow + 1, 8))
			continue;
		for (pcp = 0; pcp < 8; pcp++) {
			if (!(pbits & BIT(pcp)) || !row[pcp] ||
			    row[pcp] == flow + 1)
				continue;
			NL_SET_ERR_MSG_FMT_MOD(extack,
					       "VLAN %u priority %u already travels up another GEM interface",
					       vid, pcp);
			return -EBUSY;
		}
	}
	for (vid = 0; vid < VLAN_N_VID; vid++)
		if (memchr(p->gem_vid_flow[vid], flow + 1, 8))
			rtl9602c_gem_vid_claim(p, flow, vid, pbits, NULL);
	rtl9602c_cf_sid_refresh(p);
	return 0;
}

/*
 * Whether the bridge sees a multicast router behind a GEM port decides what
 * the PON port does with unregistered multicast (rtl9602c_mcast_refresh()).
 */
static void rtl9602c_host_gem_mrouter(void *host)
{
	rtl9602c_mcast_refresh(host);
}

/*
 * The port leaves the bridge first; the PON class may report that only after
 * the call. A routed flow that names the port is forgotten with the
 * references of the devices over it, and the stream entries are rebuilt
 * without it.
 */
static void rtl9602c_host_gem_unbind(void *host, unsigned int flow)
{
	struct rtl9602c_priv *p = host;

	rtl9602c_gem_unbridge(p, flow);

	if (p->l34_us_flow == (int)flow) {
		p->l34_us_flow = -1;
		p->l34_us_refs = 0;
		rtl9602c_cf_sid_refresh(p);
	}
}

/* Its addresses leave the table, and its say on unknown unicast. */
static void rtl9602c_host_gem_forget(void *host, unsigned int flow)
{
	struct rtl9602c_priv *p = host;

	clear_bit(flow, p->gem_flows);
	memset(&p->gem_rx[flow], 0, sizeof(p->gem_rx[flow]));
	rtl9602c_gem_hosts_sync(p);
}

/*
 * The unicast frames the GEM interface of @flow takes, as the class hands
 * them on (pon_device_ops.gem_rx_filter): kept per flow, and the static
 * entries and the PON port's unknown unicast follow
 * (rtl9602c_gem_hosts_sync()). More addresses or VLANs than are kept count
 * as promiscuous.
 */
static void rtl9602c_host_gem_rx_filter(void *host, unsigned int flow,
					const struct pon_gem_rx_filter *f)
{
	struct rtl9602c_priv *p = host;
	struct rtl9602c_gem_rx *r = &p->gem_rx[flow];
	unsigned int i;

	r->known = true;
	r->promisc = f->promisc || f->n_addrs > RTL9602C_GEM_RX_ADDRS ||
		     f->n_vids > RTL9602C_GEM_RX_VIDS;
	r->n_addrs = r->promisc ? 0 : f->n_addrs;
	r->n_vids = r->promisc ? 0 : f->n_vids;
	for (i = 0; i < r->n_addrs; i++)
		ether_addr_copy(r->addrs[i], f->addrs[i]);
	for (i = 0; i < r->n_vids; i++)
		r->vids[i] = f->vids[i];

	rtl9602c_gem_hosts_sync(p);
}

static const struct rtl9602c_pon_host_ops rtl9602c_pon_host_ops = {
	.irq_enable		= rtl9602c_host_irq_enable,
	.tx_disable		= rtl9602c_host_tx_disable,
	.hold_modulation	= rtl9602c_host_hold_modulation,
	.power_level_changed	= rtl9602c_host_power_level_changed,
	.gem_added		= rtl9602c_host_gem_added,
	.gem_unbind		= rtl9602c_host_gem_unbind,
	.gem_forget		= rtl9602c_host_gem_forget,
	.gem_bridge_join	= rtl9602c_host_gem_bridge_join,
	.gem_bridge_leave	= rtl9602c_host_gem_bridge_leave,
	.gem_vlan_add		= rtl9602c_host_gem_vlan_add,
	.gem_vlan_del		= rtl9602c_host_gem_vlan_del,
	.gem_pbits_change	= rtl9602c_host_gem_pbits_change,
	.gem_mrouter		= rtl9602c_host_gem_mrouter,
	.gem_rx_filter		= rtl9602c_host_gem_rx_filter,
};

/**
 * rtl9602c_pon_data_init() - fill in the platform data of the PON device
 * @p: the switch
 *
 * Called when the register window and the debugfs directory exist and before
 * the PON driver can bind.
 */
static void rtl9602c_pon_data_init(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;

	pd->ds = &p->ds;
	pd->reg = p->reg;
	pd->debugfs = p->debugfs;
	pd->host = p;
	pd->host_ops = &rtl9602c_pon_host_ops;
}

/**
 * rtl9602c_pon_attach() - make the device of the GPON MAC's driver
 * @p: the switch
 *
 * The driver, PON_RTL9602C, binds to it when it is there, now or when it
 * loads. It is destroyed with rtl9602c_pon_detach() and not with the
 * switch's device, so that the driver unbinds before the switch is torn down.
 *
 * Return: 0, or a negative error code, with nothing made.
 */
int rtl9602c_pon_attach(struct rtl9602c_priv *p)
{
	rtl9602c_pon_data_init(p);
	p->pon_adev = auxiliary_device_create(p->dev, KBUILD_MODNAME, "pon",
					      &p->pon, 0);
	return p->pon_adev ? 0 : -ENODEV;
}

/**
 * rtl9602c_pon_detach() - destroy the device of the GPON MAC's driver
 * @p: the switch
 *
 * The driver unbinds, which takes its services away from the switch and
 * unregisters its PON device.
 */
void rtl9602c_pon_detach(struct rtl9602c_priv *p)
{
	if (p->pon_adev)
		auxiliary_device_destroy(p->pon_adev);
	p->pon_adev = NULL;
}

/**
 * rtl9602c_pon_irq_note() - note the time of a GPON interrupt
 * @p: the switch
 *
 * Called in the first half of the interrupt, for the PON driver's timing.
 */
void rtl9602c_pon_irq_note(struct rtl9602c_priv *p)
{
	atomic64_set(&p->pon.irq_ns, ktime_get_ns());
}

/* The PON operations: calls to the MAC's driver, if one is bound. */

int rtl9602c_pon_start(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	int err = -ENODEV;

	down_read(&pd->sem);
	if (pd->pon_ops)
		err = pd->pon_ops->start(pd->pon);
	up_read(&pd->sem);
	return err;
}

void rtl9602c_pon_stop(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;

	down_read(&pd->sem);
	if (pd->pon_ops)
		pd->pon_ops->stop(pd->pon);
	up_read(&pd->sem);
}

void rtl9602c_pon_operstate(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;

	down_read(&pd->sem);
	if (pd->pon_ops)
		pd->pon_ops->operstate(pd->pon);
	up_read(&pd->sem);
}

/**
 * rtl9602c_pon_irq() - run the thread of the GPON interrupt
 * @p: the switch
 *
 * Return: whether a PON driver was bound to run it.
 */
bool rtl9602c_pon_irq(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	bool handled = false;

	down_read(&pd->sem);
	if (pd->pon_ops) {
		pd->pon_ops->irq(pd->pon);
		handled = true;
	}
	up_read(&pd->sem);
	return handled;
}

bool rtl9602c_pon_is_up(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	bool up = false;

	down_read(&pd->sem);
	if (pd->pon_ops)
		up = pd->pon_ops->is_up(pd->pon);
	up_read(&pd->sem);
	return up;
}

int rtl9602c_pon_sn_set(struct rtl9602c_priv *p, const char *s)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	int err = -ENODEV;

	down_read(&pd->sem);
	if (pd->pon_ops)
		err = pd->pon_ops->sn_set(pd->pon, s);
	up_read(&pd->sem);
	return err;
}

int rtl9602c_pon_sn_get(struct rtl9602c_priv *p, char *buf, size_t len)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	int err = -ENODATA;

	down_read(&pd->sem);
	if (pd->pon_ops)
		err = pd->pon_ops->sn_get(pd->pon, buf, len);
	up_read(&pd->sem);
	return err;
}

int rtl9602c_pon_password_set(struct rtl9602c_priv *p, const char *s)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	int err = -ENODEV;

	down_read(&pd->sem);
	if (pd->pon_ops)
		err = pd->pon_ops->password_set(pd->pon, s);
	up_read(&pd->sem);
	return err;
}

bool rtl9602c_pon_password_is_set(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	bool set = false;

	down_read(&pd->sem);
	if (pd->pon_ops)
		set = pd->pon_ops->password_is_set(pd->pon);
	up_read(&pd->sem);
	return set;
}

/**
 * rtl9602c_pon_us_ready() - whether a GEM port's upstream is programmed
 * @p: the switch
 * @flow: the flow of a GEM interface
 *
 * The flowtable offload asks this before it lets the switch carry a flow out
 * of a GEM interface.
 *
 * Context: process context.
 * Return: true when the PON driver is bound and says so.
 */
bool rtl9602c_pon_us_ready(struct rtl9602c_priv *p, unsigned int flow)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	bool ready = false;

	down_read(&pd->sem);
	if (pd->pon_ops)
		ready = pd->pon_ops->us_ready(pd->pon, flow);
	up_read(&pd->sem);
	return ready;
}

/* The transmit power level mode the MAC asks for; 0, the normal one, if none. */
u8 rtl9602c_pon_power_mode(struct rtl9602c_priv *p)
{
	struct rtl9602c_pon_data *pd = &p->pon;
	u8 mode = 0;

	down_read(&pd->sem);
	if (pd->pon_ops)
		mode = pd->pon_ops->power_mode(pd->pon);
	up_read(&pd->sem);
	return mode;
}

void rtl9602c_pon_power_applied(struct rtl9602c_priv *p, u8 asked, u8 mode,
				int err)
{
	struct rtl9602c_pon_data *pd = &p->pon;

	down_read(&pd->sem);
	if (pd->pon_ops)
		pd->pon_ops->power_applied(pd->pon, asked, mode, err);
	up_read(&pd->sem);
}
