// SPDX-License-Identifier: GPL-2.0-only
/*
 * The gem link kind: one network interface per GEM port of an ONU.
 *
 * A GEM port carries traffic and is a bridge and tc endpoint, so it is a
 * network interface; a T-CONT only schedules upstream traffic, so it is not.
 * GEM interfaces stack, like VLANs, on the interface that carries the ONU's
 * traffic, the one the MAC's owns_netdev claims. Management creates them
 * from the OLT's GEM port network CTPs, with an RTM_NEWLINK of kind "gem"
 * that names that interface in IFLA_LINK and the GEM port in the IFLA_GEM_*
 * attributes.
 *
 * Transmitted frames go to the MAC's gem_xmit with the flow in a
 * METADATA_HW_PORT_MUX dst; the MAC hands received frames to pon_gem_rx() by
 * flow. When the MAC can forward a GEM port in hardware, a GEM interface
 * that joins a bridge is offloaded to the switch through switchdev.
 */

#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/dst_metadata.h>
#include <net/rtnetlink.h>
#include <net/switchdev.h>

#include "pon_core.h"

/**
 * struct pon_gem - a GEM interface
 * @lower: the interface it stacks on, the one that carries the ONU's traffic
 * @pon: the ONU, referenced until the interface is freed
 * @md: the dst every transmitted frame carries: its port_id is @flow and its
 *	lower_dev @lower
 * @cfg: the GEM port as management created it
 * @flow: the MAC's flow for the GEM port
 * @offloaded: the GEM interface is a bridge port offloaded to the switch
 */
struct pon_gem {
	struct net_device *lower;
	struct pon_device *pon;
	struct metadata_dst *md;
	struct pon_gem_config cfg;
	unsigned int flow;
	bool offloaded;
};

static netdev_tx_t pon_gem_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct pon_gem *gem = netdev_priv(dev);
	unsigned int len = skb->len;
	int ret;

	/* A downstream-only GEM port sends nothing. */
	if (!(gem->cfg.direction & IFLA_GEM_DIR_UPSTREAM)) {
		dev_core_stats_tx_dropped_inc(dev);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	/*
	 * The MAC takes the flow from the port-mux dst. The frame goes to the
	 * MAC directly and not through the lower's transmit: the lower is a
	 * switch port, and a frame sent there would lose its dst before the MAC
	 * saw it.
	 */
	skb_dst_drop(skb);
	dst_hold(&gem->md->dst);
	skb_dst_set(skb, &gem->md->dst);

	/*
	 * net_xmit_eval() evaluates its argument twice, so it is given the
	 * stored result and never the call. NET_XMIT_SUCCESS and NET_XMIT_CN
	 * count as sent.
	 */
	ret = gem->pon->ops->gem_xmit(gem->pon, skb);
	if (likely(net_xmit_eval(ret) == 0))
		dev_sw_netstats_tx_add(dev, 1, len);
	else
		dev_core_stats_tx_dropped_inc(dev);

	return NETDEV_TX_OK;
}

bool pon_gem_rx(struct pon_device *pon, unsigned int flow, struct sk_buff *skb)
{
	struct net_device *dev;
	struct pon_gem *gem;

	if (flow >= pon->num_flows)
		return false;

	/* The flow table is published under RCU (pon_gem_newlink()). */
	rcu_read_lock();
	dev = rcu_dereference(pon->gem[flow]);
	if (!dev) {
		rcu_read_unlock();
		return false;
	}

	/* A frame for a GEM interface that is down is dropped here. */
	if (unlikely(!(dev->flags & IFF_UP))) {
		dev_core_stats_rx_dropped_inc(dev);
		kfree_skb(skb);
		rcu_read_unlock();
		return true;
	}

	gem = netdev_priv(dev);
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
#ifdef CONFIG_NET_SWITCHDEV
	/*
	 * An offloaded GEM interface is a port of the switch's hardware domain.
	 * The mark tells the bridge that the switch has already forwarded the
	 * frame to the other ports of that domain, so that the bridge forwards
	 * it only to ports outside it. That the switch has done so for every
	 * such frame, so that none arrives twice, is an assumption, not
	 * verified. A frame that an ACL rule trapped is one that the switch
	 * has not forwarded, and with the mark the bridge does not either, so
	 * a trapped frame reaches the host alone.
	 */
	skb->offload_fwd_mark = READ_ONCE(gem->offloaded);
#endif
	dev_sw_netstats_rx_add(dev, skb->len);
	netif_receive_skb(skb);
	rcu_read_unlock();
	return true;
}
EXPORT_SYMBOL_GPL(pon_gem_rx);

/*
 * A GEM interface does not open while its lower is administratively down. So
 * one created up on a lower that is down fails to open, and rtnetlink deletes
 * it again.
 */
static int pon_gem_open(struct net_device *dev)
{
	struct pon_gem *gem = netdev_priv(dev);

	if (!(gem->lower->flags & IFF_UP))
		return -ENETDOWN;

	netif_stacked_transfer_operstate(gem->lower, dev);
	return 0;
}

static int pon_gem_get_iflink(const struct net_device *dev)
{
	const struct pon_gem *gem = netdev_priv(dev);

	return READ_ONCE(gem->lower->ifindex);
}

/*
 * The interface statistics are the software counters: what the CPU sent and
 * received, which for an offloaded GEM interface is a small part of its
 * traffic. They stay software on purpose, since hardware numbers there would
 * leave the drop counters beside them counting different traffic. The MAC's
 * own counters of the GEM port are ethtool statistics under their own names,
 * present only when the MAC has gem_stats:
 *
 * pon_tx_octets: the octets the MAC sent upstream on the GEM port.
 */
static const char pon_gem_stat_strings[][ETH_GSTRING_LEN] = {
	"pon_tx_octets",
};

static int pon_gem_get_sset_count(struct net_device *dev, int sset)
{
	const struct pon_gem *gem = netdev_priv(dev);

	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	if (!gem->pon->ops->gem_stats)
		return 0;

	return ARRAY_SIZE(pon_gem_stat_strings);
}

static void pon_gem_get_strings(struct net_device *dev, u32 sset, u8 *data)
{
	const struct pon_gem *gem = netdev_priv(dev);

	if (sset != ETH_SS_STATS || !gem->pon->ops->gem_stats)
		return;

	memcpy(data, pon_gem_stat_strings, sizeof(pon_gem_stat_strings));
}

static void pon_gem_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct pon_gem *gem = netdev_priv(dev);
	struct pon_gem_stats st = {};

	if (!gem->pon->ops->gem_stats)
		return;

	if (gem->pon->ops->gem_stats(gem->pon, gem->flow, &st))
		return;

	data[0] = st.tx_octets;
}

static const struct ethtool_ops pon_gem_ethtool_ops = {
	.get_sset_count		= pon_gem_get_sset_count,
	.get_strings		= pon_gem_get_strings,
	.get_ethtool_stats	= pon_gem_get_ethtool_stats,
};

/*
 * Only flowtable offload (TC_SETUP_FT) is passed down to the lower. An
 * offloaded flowtable needs every device in its set to accept it, and the
 * device of a flow that leaves by a GEM port has to be the GEM interface,
 * since what the CPU sends on the lower carries no GEM flow. Every other
 * type gets -EOPNOTSUPP and stays in software: a tc block belongs to one
 * interface, and passed down, a GEM interface's block would be bound to the
 * switch port's single ingress block, so that the GEM interface and the lower
 * could not both have one.
 */
static int pon_gem_setup_tc(struct net_device *dev, enum tc_setup_type type,
			    void *type_data)
{
	struct pon_gem *gem = netdev_priv(dev);
	struct net_device *lower = gem->lower;

	if (type != TC_SETUP_FT)
		return -EOPNOTSUPP;
	if (!lower || !lower->netdev_ops->ndo_setup_tc)
		return -EOPNOTSUPP;

	return lower->netdev_ops->ndo_setup_tc(lower, type, type_data);
}

static const struct net_device_ops pon_gem_netdev_ops = {
	.ndo_open		= pon_gem_open,
	.ndo_start_xmit		= pon_gem_xmit,
	.ndo_get_stats64	= dev_get_tstats64,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_iflink		= pon_gem_get_iflink,
	.ndo_setup_tc		= pon_gem_setup_tc,
};

bool pon_gem_netdev_flow(const struct net_device *dev, unsigned int *flow)
{
	const struct pon_gem *gem;

	if (dev->netdev_ops != &pon_gem_netdev_ops)
		return false;

	gem = netdev_priv(dev);
	*flow = gem->flow;
	return true;
}
EXPORT_SYMBOL_GPL(pon_gem_netdev_flow);

/*
 * Releases the dst and the ONU: the destructor, once registration has
 * succeeded, and before that called by pon_gem_newlink() itself.
 */
static void pon_gem_free(struct net_device *dev)
{
	struct pon_gem *gem = netdev_priv(dev);

	if (gem->md) {
		dst_release(&gem->md->dst);
		gem->md = NULL;
	}
	if (gem->pon) {
		pon_device_put(gem->pon);
		gem->pon = NULL;
	}
}

/*
 * An Ethernet interface without a transmit queue, with lockless transmit
 * (gem_xmit may run on several CPUs at once) and per-CPU software counters,
 * freed on unregistration.
 */
static void pon_gem_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &pon_gem_netdev_ops;
	dev->ethtool_ops = &pon_gem_ethtool_ops;
	dev->needs_free_netdev = true;

	dev->priv_flags |= IFF_NO_QUEUE;
	dev->lltx = true;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
}

/*
 * IFLA_GEM_PORT_ID: the Port-ID field of the GEM header has 12 bits
 * (ITU-T G.984.3 clause 8.3.1). IFLA_GEM_DIRECTION: the direction attribute
 * of the GEM port network CTP (ITU-T G.988 clause 9.2.3), 1 upstream (UNI to
 * ANI), 2 downstream (ANI to UNI) or 3 both. IFLA_GEM_TCONT: a T-CONT index,
 * from 0 and below the ONU's num-tconts; the MAC refuses one it does not
 * have.
 */
static const struct nla_policy pon_gem_policy[IFLA_GEM_MAX + 1] = {
	[IFLA_GEM_PORT_ID]	= NLA_POLICY_MAX(NLA_U16, 4095),
	[IFLA_GEM_DIRECTION]	= NLA_POLICY_RANGE(NLA_U8,
						   IFLA_GEM_DIR_UPSTREAM,
						   IFLA_GEM_DIR_BOTH),
	[IFLA_GEM_TCONT]	= { .type = NLA_U32 },
	[IFLA_GEM_MULTICAST]	= { .type = NLA_FLAG },
};

static int pon_gem_parse(struct pon_gem_config *cfg, struct nlattr **data,
			 struct netlink_ext_ack *extack)
{
	if (!data || !data[IFLA_GEM_PORT_ID]) {
		NL_SET_ERR_MSG(extack, "A GEM port needs its Port-ID");
		return -EINVAL;
	}

	cfg->port_id = nla_get_u16(data[IFLA_GEM_PORT_ID]);
	cfg->direction = nla_get_u8_default(data[IFLA_GEM_DIRECTION],
					    IFLA_GEM_DIR_BOTH);
	cfg->multicast = nla_get_flag(data[IFLA_GEM_MULTICAST]);

	/*
	 * A multicast GEM port carries downstream traffic only: the ONU
	 * reconstructs a multicast stream from the GEM packets it receives on
	 * it (ITU-T G.988 clause 9.2.5).
	 */
	if (cfg->multicast && cfg->direction != IFLA_GEM_DIR_DOWNSTREAM) {
		NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GEM_MULTICAST],
				    "A multicast GEM port carries downstream traffic only");
		return -EINVAL;
	}

	/*
	 * A GEM port that carries upstream traffic needs the T-CONT that
	 * schedules it; several GEM ports may share one.
	 */
	if (cfg->direction & IFLA_GEM_DIR_UPSTREAM) {
		if (!data[IFLA_GEM_TCONT]) {
			NL_SET_ERR_MSG(extack,
				       "An upstream GEM port needs its T-CONT");
			return -EINVAL;
		}
		cfg->tcont = nla_get_u32(data[IFLA_GEM_TCONT]);
	}

	return 0;
}

static int pon_gem_newlink(struct net_device *dev,
			   struct rtnl_newlink_params *params,
			   struct netlink_ext_ack *extack)
{
	struct net *link_net = rtnl_newlink_link_net(params);
	struct pon_gem *gem = netdev_priv(dev);
	struct nlattr **tb = params->tb;
	struct net_device *lower;
	int err;

	if (!tb[IFLA_LINK]) {
		NL_SET_ERR_MSG(extack,
			       "A GEM port needs the ONU's interface as its link");
		return -EINVAL;
	}

	lower = __dev_get_by_index(link_net, nla_get_u32(tb[IFLA_LINK]));
	if (!lower) {
		NL_SET_ERR_MSG_ATTR(extack, tb[IFLA_LINK], "Unknown link");
		return -ENODEV;
	}

	err = pon_gem_parse(&gem->cfg, params->data, extack);
	if (err)
		return err;

	/* The ONU whose traffic the link carries, with a reference. */
	gem->pon = pon_device_get_by_netdev(lower);
	if (!gem->pon) {
		NL_SET_ERR_MSG_ATTR(extack, tb[IFLA_LINK],
				    "Link does not carry an ONU's traffic");
		return -EOPNOTSUPP;
	}

	gem->md = metadata_dst_alloc(0, METADATA_HW_PORT_MUX, GFP_KERNEL);
	if (!gem->md) {
		err = -ENOMEM;
		goto err_free;
	}

	/*
	 * The MAC creates its GEM port and chooses the flow. A flow outside
	 * its flow space, or one that already has a GEM interface, is refused,
	 * and the MAC's GEM port removed again.
	 */
	err = gem->pon->ops->gem_add(gem->pon, &gem->cfg, &gem->flow, extack);
	if (err)
		goto err_free;

	if (WARN_ON(gem->flow >= gem->pon->num_flows) ||
	    rtnl_dereference(gem->pon->gem[gem->flow])) {
		err = -EEXIST;
		goto err_del;
	}

	gem->md->u.port_info.port_id = gem->flow;
	gem->md->u.port_info.lower_dev = lower;
	gem->lower = lower;

	/*
	 * The lower's MAC address, which is the ONU's, and its MTU, both taken
	 * once: later changes of the lower's MTU are not followed.
	 */
	eth_hw_addr_inherit(dev, lower);
	dev->mtu = lower->mtu;

	err = register_netdevice(dev);
	if (err)
		goto err_del;

	/*
	 * register_netdevice() runs the destructor on some of its failures and
	 * not on others, so the destructor is installed only now; until here
	 * the error paths release the dst and the ONU themselves.
	 */
	dev->priv_destructor = pon_gem_free;

	err = netdev_upper_dev_link(lower, dev, extack);
	if (err) {
		unregister_netdevice(dev);
		gem->pon->ops->gem_del(gem->pon, gem->flow);
		return err;
	}

	/* Receive reaches the interface once it is published here. */
	netif_stacked_transfer_operstate(lower, dev);
	rcu_assign_pointer(gem->pon->gem[gem->flow], dev);
	return 0;

err_del:
	gem->pon->ops->gem_del(gem->pon, gem->flow);
err_free:
	pon_gem_free(dev);
	return err;
}

/*
 * The interface is unpublished first, so that receive stops reaching it. A
 * bridge lets go of the interface only during the unregistration queued
 * here, so for a bridged GEM interface gem_bridge_leave comes after gem_del.
 */
static void pon_gem_dellink(struct net_device *dev, struct list_head *head)
{
	struct pon_gem *gem = netdev_priv(dev);

	RCU_INIT_POINTER(gem->pon->gem[gem->flow], NULL);
	netdev_upper_dev_unlink(gem->lower, dev);
	unregister_netdevice_queue(dev, head);
	gem->pon->ops->gem_del(gem->pon, gem->flow);
}

static size_t pon_gem_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(u16)) +
	       nla_total_size(sizeof(u8)) +
	       nla_total_size(sizeof(u32)) +
	       nla_total_size(0);
}

static int pon_gem_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	const struct pon_gem *gem = netdev_priv(dev);

	if (nla_put_u16(skb, IFLA_GEM_PORT_ID, gem->cfg.port_id) ||
	    nla_put_u8(skb, IFLA_GEM_DIRECTION, gem->cfg.direction))
		return -EMSGSIZE;

	if ((gem->cfg.direction & IFLA_GEM_DIR_UPSTREAM) &&
	    nla_put_u32(skb, IFLA_GEM_TCONT, gem->cfg.tcont))
		return -EMSGSIZE;

	if (gem->cfg.multicast && nla_put_flag(skb, IFLA_GEM_MULTICAST))
		return -EMSGSIZE;

	return 0;
}

static struct net *pon_gem_get_link_net(const struct net_device *dev)
{
	const struct pon_gem *gem = netdev_priv(dev);

	return dev_net(gem->lower);
}

/*
 * No changelink: a Port-ID or T-CONT changed under traffic would move a
 * subscriber's frames to another service with nothing torn down. A changed
 * GEM port is deleted and created again.
 */
static struct rtnl_link_ops pon_gem_link_ops __read_mostly = {
	.kind		= "gem",
	.priv_size	= sizeof(struct pon_gem),
	.setup		= pon_gem_setup,
	.maxtype	= IFLA_GEM_MAX,
	.policy		= pon_gem_policy,
	.newlink	= pon_gem_newlink,
	.dellink	= pon_gem_dellink,
	.get_size	= pon_gem_get_size,
	.fill_info	= pon_gem_fill_info,
	.get_link_net	= pon_gem_get_link_net,
};

/**
 * pon_gem_remove_all() - delete every GEM interface of an ONU
 * @pon: the PON device
 *
 * Context: RTNL held.
 */
void pon_gem_remove_all(struct pon_device *pon)
{
	struct net_device *dev;
	unsigned int flow;
	LIST_HEAD(kill);

	ASSERT_RTNL();

	for (flow = 0; flow < pon->num_flows; flow++) {
		dev = rtnl_dereference(pon->gem[flow]);
		if (dev)
			pon_gem_dellink(dev, &kill);
	}

	unregister_netdevice_many(&kill);
}

/*
 * Joining a bridge: the MAC is asked to forward in hardware between the GEM
 * port and its other ports in the bridge, and a refusal fails the join with
 * the MAC's message. The GEM interface is then offloaded to switchdev with
 * the lower as the hardware device, which puts it in the switch's hardware
 * domain: the bridge forwards in software only what the hardware did not.
 * If that fails, the MAC's join is undone and the join fails.
 *
 * Leaving undoes the offload on NETDEV_PRECHANGEUPPER, while the interface
 * is still a bridge port, and calls gem_bridge_leave on NETDEV_CHANGEUPPER.
 * Without gem_bridge_join, or for an upper that is not a bridge, nothing is
 * done, and the bridge forwards the GEM interface's traffic in software.
 */
static int pon_gem_changeupper(struct net_device *dev, unsigned long event,
			       struct netdev_notifier_changeupper_info *info)
{
	struct netlink_ext_ack *extack =
		netdev_notifier_info_to_extack(&info->info);
	struct pon_gem *gem = netdev_priv(dev);
	const struct pon_device_ops *ops = gem->pon->ops;
	int err;

	if (!netif_is_bridge_master(info->upper_dev) || !ops->gem_bridge_join)
		return NOTIFY_DONE;

	if (event == NETDEV_PRECHANGEUPPER) {
		if (!info->linking && gem->offloaded) {
			WRITE_ONCE(gem->offloaded, false);
			switchdev_bridge_port_unoffload(dev, NULL, NULL, NULL);
		}
		return NOTIFY_DONE;
	}

	if (!info->linking) {
		ops->gem_bridge_leave(gem->pon, gem->flow, info->upper_dev);
		return NOTIFY_DONE;
	}

	err = ops->gem_bridge_join(gem->pon, gem->flow, info->upper_dev,
				   extack);
	if (err)
		return notifier_from_errno(err);

	err = switchdev_bridge_port_offload(dev, gem->lower, NULL, NULL, NULL,
					    false, extack);
	if (err) {
		ops->gem_bridge_leave(gem->pon, gem->flow, info->upper_dev);
		return notifier_from_errno(err);
	}

	WRITE_ONCE(gem->offloaded, true);
	return NOTIFY_OK;
}

static bool pon_gem_dev_check(const struct net_device *dev)
{
	return dev->rtnl_link_ops == &pon_gem_link_ops;
}

/*
 * Whether another GEM interface of bridge @br has VLAN @vid. A MAC may hold
 * VLAN membership once for all of its GEM ports, as a switch with a single
 * PON port does, so a VLAN is taken out of the MAC only when no GEM interface
 * of the bridge has it any more.
 */
static bool pon_gem_vlan_shared(const struct pon_gem *gem,
				const struct net_device *br, u16 vid)
{
	struct bridge_vlan_info info;
	unsigned int flow;

	for (flow = 0; flow < gem->pon->num_flows; flow++) {
		struct net_device *other = rtnl_dereference(gem->pon->gem[flow]);

		if (!other || flow == gem->flow ||
		    netdev_master_upper_dev_get(other) != br)
			continue;
		if (!br_vlan_get_info(other, vid, &info))
			return true;
	}
	return false;
}

/*
 * The bridge VLANs of an offloaded GEM interface go to the MAC. They have to
 * reach the hardware: even a bridge without VLAN filtering gives each port
 * its PVID, and a switch that learns per VLAN files every learned address
 * under it, so without them the addresses learned on the PON side would sit
 * in another VLAN than the one lookups from the subscriber side use, and
 * frames towards the PON would be flooded. Other objects, and every object
 * of a GEM interface that is not offloaded, get -EOPNOTSUPP and stay in
 * software.
 */
static int pon_gem_port_obj_add(struct net_device *dev, const void *ctx,
				const struct switchdev_obj *obj,
				struct netlink_ext_ack *extack)
{
	struct pon_gem *gem = netdev_priv(dev);
	const struct switchdev_obj_port_vlan *vlan;

	if (obj->id != SWITCHDEV_OBJ_ID_PORT_VLAN || !gem->offloaded ||
	    !gem->pon->ops->gem_vlan_add)
		return -EOPNOTSUPP;

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);
	return gem->pon->ops->gem_vlan_add(gem->pon, gem->flow, vlan->vid,
					   vlan->flags, extack);
}

static int pon_gem_port_obj_del(struct net_device *dev, const void *ctx,
				const struct switchdev_obj *obj)
{
	struct pon_gem *gem = netdev_priv(dev);
	const struct switchdev_obj_port_vlan *vlan;
	struct net_device *br;

	if (obj->id != SWITCHDEV_OBJ_ID_PORT_VLAN || !gem->offloaded ||
	    !gem->pon->ops->gem_vlan_del)
		return -EOPNOTSUPP;

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);
	br = netdev_master_upper_dev_get(dev);
	if (!br || !pon_gem_vlan_shared(gem, br, vlan->vid))
		gem->pon->ops->gem_vlan_del(gem->pon, gem->flow, vlan->vid);
	return 0;
}

static int pon_gem_switchdev_blocking_event(struct notifier_block *nb,
					    unsigned long event, void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	int err;

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD:
		err = switchdev_handle_port_obj_add(dev, ptr, pon_gem_dev_check,
						    pon_gem_port_obj_add);
		return notifier_from_errno(err);
	case SWITCHDEV_PORT_OBJ_DEL:
		err = switchdev_handle_port_obj_del(dev, ptr, pon_gem_dev_check,
						    pon_gem_port_obj_del);
		return notifier_from_errno(err);
	}

	return NOTIFY_DONE;
}

static struct notifier_block pon_gem_switchdev_blocking_notifier = {
	.notifier_call = pon_gem_switchdev_blocking_event,
};

/*
 * GEM interfaces follow their lower. They copy its operational state on its
 * CHANGE, UP and DOWN events, so a lower that stays dormant until the ONU
 * reaches O5 keeps them dormant too, and they are deleted when the lower is
 * unregistered. The upper changes of a GEM interface itself go to
 * pon_gem_changeupper().
 */
static int pon_gem_netdev_event(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct net_device *lower = netdev_notifier_info_to_dev(ptr);
	struct pon_device *pon;
	struct net_device *dev;
	struct pon_gem *gem;
	unsigned int flow;
	LIST_HEAD(kill);

	if ((event == NETDEV_PRECHANGEUPPER || event == NETDEV_CHANGEUPPER) &&
	    pon_gem_dev_check(lower))
		return pon_gem_changeupper(lower, event, ptr);

	if (event != NETDEV_CHANGE && event != NETDEV_UP &&
	    event != NETDEV_DOWN && event != NETDEV_UNREGISTER)
		return NOTIFY_DONE;

	pon = pon_device_get_by_netdev(lower);
	if (!pon)
		return NOTIFY_DONE;

	for (flow = 0; flow < pon->num_flows; flow++) {
		dev = rtnl_dereference(pon->gem[flow]);
		if (!dev)
			continue;
		gem = netdev_priv(dev);
		if (gem->lower != lower)
			continue;

		if (event == NETDEV_UNREGISTER)
			pon_gem_dellink(dev, &kill);
		else
			netif_stacked_transfer_operstate(lower, dev);
	}

	unregister_netdevice_many(&kill);
	pon_device_put(pon);
	return NOTIFY_DONE;
}

static struct notifier_block pon_gem_notifier = {
	.notifier_call = pon_gem_netdev_event,
};

/**
 * pon_gem_init() - register the gem link kind and its notifiers
 *
 * Return: 0 or a negative error code.
 */
int pon_gem_init(void)
{
	int err;

	err = register_netdevice_notifier(&pon_gem_notifier);
	if (err)
		return err;

	err = register_switchdev_blocking_notifier(&pon_gem_switchdev_blocking_notifier);
	if (err)
		goto err_netdev;

	err = rtnl_link_register(&pon_gem_link_ops);
	if (err)
		goto err_switchdev;

	return 0;

err_switchdev:
	unregister_switchdev_blocking_notifier(&pon_gem_switchdev_blocking_notifier);
err_netdev:
	unregister_netdevice_notifier(&pon_gem_notifier);
	return err;
}

/**
 * pon_gem_exit() - unregister the gem link kind and its notifiers
 */
void pon_gem_exit(void)
{
	rtnl_link_unregister(&pon_gem_link_ops);
	unregister_switchdev_blocking_notifier(&pon_gem_switchdev_blocking_notifier);
	unregister_netdevice_notifier(&pon_gem_notifier);
}

/*
 * Creating a link of kind "gem" loads a modular class. The pon family has no
 * such alias: a request to it alone does not load the module.
 */
MODULE_ALIAS_RTNL_LINK("gem");
