// SPDX-License-Identifier: GPL-2.0-only
/*
 * A stand-in ONU MAC for testing the PON class.
 *
 * It registers an OMCI port and a data path with the class as a MAC does, on a
 * platform device of its own and a network interface, ponsim0, that carries
 * the traffic of its GEM interfaces, and it has no hardware: a frame sent on a
 * GEM interface is counted and dropped, an OMCI message written to the port
 * comes back to be read, and everything the class asks of a MAC is answered
 * from a few arrays. The state it reports is set through debugfs, in the
 * directory pon_sim: write a number to "state" to change the activation state,
 * which sends a notification, and a flow to "rx" to receive a frame in it.
 */

#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/pon.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/uaccess.h>
#include <net/dst_metadata.h>

#define PON_SIM_FLOWS		16
#define PON_SIM_TCONTS		4
#define PON_SIM_QUEUES		4
#define PON_SIM_ONU_ID		7
#define PON_SIM_OMCC_PORT_ID	4095
#define PON_SIM_EQD		12345
#define PON_SIM_RX_LEN		64

/**
 * struct pon_sim_stats - a GEM port's counters, which gem_xmit and the receive
 *	path bump with bottom halves off
 * @tx_octets: octets sent
 * @tx_frames: frames sent
 * @rx_frames: frames received
 * @rx_octets: octets received
 */
struct pon_sim_stats {
	atomic64_t tx_octets;
	atomic64_t tx_frames;
	atomic64_t rx_frames;
	atomic64_t rx_octets;
};

/**
 * struct pon_sim - the stand-in MAC
 * @pdev: its platform device
 * @netdev: ponsim0
 * @pon: its PON device
 * @omci: its OMCI port
 * @lock: protects the rest
 * @state: the activation state, a PON_ONU_STATE_* value
 * @omci_up: the OMCI channel exists
 * @flows: the flows with a GEM port
 * @cfg: the configuration of each, by flow
 * @stats: the counters of each, by flow
 * @alloc_id: the Alloc-ID of each T-CONT; @bound says which are mapped
 * @bound: the T-CONTs that have an Alloc-ID
 * @calls: how often each operation was called, for the "calls" file
 * @dbg: the debugfs directory
 */
struct pon_sim {
	struct platform_device *pdev;
	struct net_device *netdev;
	struct pon_device *pon;
	struct pon_omci_port *omci;
	/* protects the members below it */
	struct mutex lock;
	u32 state;
	/* when @state was entered, in jiffies */
	u64 since;
	bool omci_up;
	DECLARE_BITMAP(flows, PON_SIM_FLOWS);
	struct pon_gem_config cfg[PON_SIM_FLOWS];
	struct pon_sim_stats stats[PON_SIM_FLOWS];
	u16 alloc_id[PON_SIM_TCONTS];
	DECLARE_BITMAP(bound, PON_SIM_TCONTS);
	struct {
		unsigned int bridge_join;
		unsigned int bridge_leave;
		unsigned int vlan_add;
		unsigned int vlan_del;
		unsigned int mrouter;
		unsigned int rx_filter;
		unsigned int change;
		unsigned int omci_start;
		unsigned int omci_stop;
		unsigned int omci_tx;
	} calls;
	struct dentry *dbg;
};

static struct pon_sim *pon_sim;

static netdev_tx_t pon_sim_xmit(struct sk_buff *skb, struct net_device *dev)
{
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/*
 * The bridge offloads a port to switchdev only when its lower device says
 * which hardware it is a port of, as a DSA user port does for the real MAC.
 */
static int pon_sim_port_parent_id(struct net_device *dev,
				  struct netdev_phys_item_id *ppid)
{
	ppid->id_len = ETH_ALEN;
	ether_addr_copy(ppid->id, dev->dev_addr);
	return 0;
}

static const struct net_device_ops pon_sim_netdev_ops = {
	.ndo_start_xmit		= pon_sim_xmit,
	.ndo_get_port_parent_id	= pon_sim_port_parent_id,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static bool pon_sim_owns_netdev(struct pon_device *pon,
				const struct net_device *dev)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	return dev == sim->netdev;
}

static int pon_sim_gem_add(struct pon_device *pon,
			   const struct pon_gem_config *cfg,
			   unsigned int *flow, struct netlink_ext_ack *extack)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);
	unsigned int f;
	int err = 0;

	mutex_lock(&sim->lock);
	if ((cfg->direction & IFLA_GEM_DIR_UPSTREAM) &&
	    cfg->tcont >= PON_SIM_TCONTS) {
		NL_SET_ERR_MSG_FMT_MOD(extack, "No T-CONT %u: the stand-in has %u",
				       cfg->tcont, PON_SIM_TCONTS);
		err = -ERANGE;
		goto out;
	}
	if (cfg->port_id == PON_SIM_OMCC_PORT_ID) {
		NL_SET_ERR_MSG_MOD(extack, "Port-ID 4095 is the OMCC's");
		err = -EEXIST;
		goto out;
	}
	for_each_set_bit(f, sim->flows, PON_SIM_FLOWS) {
		if (sim->cfg[f].port_id == cfg->port_id) {
			NL_SET_ERR_MSG_MOD(extack, "The Port-ID has a GEM port");
			err = -EEXIST;
			goto out;
		}
	}
	f = find_first_zero_bit(sim->flows, PON_SIM_FLOWS);
	if (f == PON_SIM_FLOWS) {
		NL_SET_ERR_MSG_MOD(extack, "Every flow is in use");
		err = -ENOSPC;
		goto out;
	}
	set_bit(f, sim->flows);
	sim->cfg[f] = *cfg;
	atomic64_set(&sim->stats[f].tx_octets, 0);
	atomic64_set(&sim->stats[f].tx_frames, 0);
	atomic64_set(&sim->stats[f].rx_frames, 0);
	atomic64_set(&sim->stats[f].rx_octets, 0);
	*flow = f;
out:
	mutex_unlock(&sim->lock);
	return err;
}

static int pon_sim_gem_change(struct pon_device *pon, unsigned int flow,
			      const struct pon_gem_config *cfg,
			      struct netlink_ext_ack *extack)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	sim->cfg[flow] = *cfg;
	sim->calls.change++;
	mutex_unlock(&sim->lock);
	return 0;
}

static void pon_sim_gem_del(struct pon_device *pon, unsigned int flow)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	clear_bit(flow, sim->flows);
	mutex_unlock(&sim->lock);
}

static int pon_sim_gem_xmit(struct pon_device *pon, struct sk_buff *skb)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);
	struct metadata_dst *md = skb_metadata_dst(skb);
	unsigned int flow = md ? md->u.port_info.port_id : PON_SIM_FLOWS;

	if (flow < PON_SIM_FLOWS) {
		atomic64_inc(&sim->stats[flow].tx_frames);
		atomic64_add(skb->len, &sim->stats[flow].tx_octets);
	}
	dev_kfree_skb_any(skb);
	return NET_XMIT_SUCCESS;
}

static int pon_sim_bridge_join(struct pon_device *pon, unsigned int flow,
			       struct net_device *br,
			       struct netlink_ext_ack *extack)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	sim->calls.bridge_join++;
	mutex_unlock(&sim->lock);
	return 0;
}

static void pon_sim_bridge_leave(struct pon_device *pon, unsigned int flow,
				 struct net_device *br)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	sim->calls.bridge_leave++;
	mutex_unlock(&sim->lock);
}

static int pon_sim_vlan_add(struct pon_device *pon, unsigned int flow, u16 vid,
			    u16 flags, struct netlink_ext_ack *extack)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	sim->calls.vlan_add++;
	mutex_unlock(&sim->lock);
	return 0;
}

static void pon_sim_vlan_del(struct pon_device *pon, unsigned int flow,
			     u16 vid, bool last)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	sim->calls.vlan_del++;
	mutex_unlock(&sim->lock);
}

static void pon_sim_mrouter(struct pon_device *pon, unsigned int flow,
			    bool mrouter)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	sim->calls.mrouter++;
	mutex_unlock(&sim->lock);
}

static void pon_sim_rx_filter(struct pon_device *pon, unsigned int flow,
			      const struct pon_gem_rx_filter *f)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	sim->calls.rx_filter++;
	mutex_unlock(&sim->lock);
}

/* What the class asks for: the state the debugfs file last set. */
static void pon_sim_fill_status(struct pon_sim *sim, struct pon_dev_status *st)
{
	unsigned int t, n = 0;

	st->type = PON_TYPE_GPON;
	st->onu_state = sim->state;
	st->have_alarms = true;
	st->alarms = sim->state < PON_ONU_STATE_O2 ? PON_ALARM_LOS : 0;
	st->ifindex = sim->netdev->ifindex;
	if (sim->state >= PON_ONU_STATE_O4)
		st->onu_id = PON_SIM_ONU_ID;
	if (sim->state >= PON_ONU_STATE_O5 && sim->omci_up)
		st->omcc_port_id = PON_SIM_OMCC_PORT_ID;
	memcpy(st->serial_number, "SIMU\x00\x00\x00\x01", PON_SERIAL_NUMBER_LEN);
	st->have_serial = true;
	for_each_set_bit(t, sim->bound, PON_SIM_TCONTS)
		st->alloc_id[n++] = sim->alloc_id[t];
	st->num_alloc_ids = n;
	st->have_stats = true;
	st->stats.gem_frames = 42;
	st->stats.password_requests = 3;
	st->stats.password_replies = 2;
	st->power_level = PON_POWER_LEVEL_HIGH;
	st->have_power_level = true;
	if (sim->state == PON_ONU_STATE_O5) {
		st->eqd = PON_SIM_EQD;
		st->have_eqd = true;
	}
	st->onu_state_ms = jiffies64_to_msecs(get_jiffies_64() - sim->since);
	st->have_onu_state_time = true;
}

static int pon_sim_dev_status(struct pon_device *pon, struct pon_dev_status *st)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	mutex_lock(&sim->lock);
	pon_sim_fill_status(sim, st);
	mutex_unlock(&sim->lock);
	return 0;
}

static int pon_sim_gem_stats(struct pon_device *pon, unsigned int flow,
			     struct pon_gem_stats *st)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);

	if (flow >= PON_SIM_FLOWS)
		return -ERANGE;
	st->tx_octets = atomic64_read(&sim->stats[flow].tx_octets);
	st->tx_frames = atomic64_read(&sim->stats[flow].tx_frames);
	st->rx_frames = atomic64_read(&sim->stats[flow].rx_frames);
	st->rx_octets = atomic64_read(&sim->stats[flow].rx_octets);
	return 0;
}

static int pon_sim_tcont_get(struct pon_device *pon, unsigned int index,
			     u16 *alloc_id)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);
	int err = -ENOENT;

	mutex_lock(&sim->lock);
	if (test_bit(index, sim->bound)) {
		*alloc_id = sim->alloc_id[index];
		err = 0;
	}
	mutex_unlock(&sim->lock);
	return err;
}

static int pon_sim_tcont_set(struct pon_device *pon, unsigned int index,
			     int alloc_id, struct netlink_ext_ack *extack)
{
	struct pon_sim *sim = pon_device_get_drvdata(pon);
	unsigned int t;
	int err = 0;

	mutex_lock(&sim->lock);
	if (alloc_id < 0) {
		clear_bit(index, sim->bound);
		goto out;
	}
	for_each_set_bit(t, sim->bound, PON_SIM_TCONTS) {
		if (t != index && sim->alloc_id[t] == alloc_id) {
			NL_SET_ERR_MSG_MOD(extack, "The Alloc-ID serves another T-CONT");
			err = -EBUSY;
			goto out;
		}
	}
	sim->alloc_id[index] = alloc_id;
	set_bit(index, sim->bound);
out:
	mutex_unlock(&sim->lock);
	return err;
}

static const struct pon_device_ops pon_sim_ops = {
	.owns_netdev		= pon_sim_owns_netdev,
	.gem_add		= pon_sim_gem_add,
	.gem_change		= pon_sim_gem_change,
	.gem_del		= pon_sim_gem_del,
	.gem_xmit		= pon_sim_gem_xmit,
	.gem_bridge_join	= pon_sim_bridge_join,
	.gem_bridge_leave	= pon_sim_bridge_leave,
	.gem_vlan_add		= pon_sim_vlan_add,
	.gem_vlan_del		= pon_sim_vlan_del,
	.gem_mrouter		= pon_sim_mrouter,
	.gem_rx_filter		= pon_sim_rx_filter,
	.dev_status		= pon_sim_dev_status,
	.gem_stats		= pon_sim_gem_stats,
	.tcont_get		= pon_sim_tcont_get,
	.tcont_set		= pon_sim_tcont_set,
};

/* An OMCI message written to the port comes back to be read. */
static int pon_sim_omci_start(struct pon_omci_port *port)
{
	struct pon_sim *sim = pon_omci_port_get_drvdata(port);

	sim->calls.omci_start++;
	return 0;
}

static void pon_sim_omci_stop(struct pon_omci_port *port)
{
	struct pon_sim *sim = pon_omci_port_get_drvdata(port);

	sim->calls.omci_stop++;
}

static int pon_sim_omci_tx(struct pon_omci_port *port, struct sk_buff *skb)
{
	struct pon_sim *sim = pon_omci_port_get_drvdata(port);

	sim->calls.omci_tx++;
	pon_omci_port_rx(port, skb);
	return 0;
}

static const struct pon_omci_port_ops pon_sim_omci_ops = {
	.start	= pon_sim_omci_start,
	.stop	= pon_sim_omci_stop,
	.tx	= pon_sim_omci_tx,
};

/* debugfs: state (rw) */
static ssize_t pon_sim_state_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct pon_sim *sim = file->private_data;
	struct pon_dev_status st = { .onu_id = -1, .omcc_port_id = -1 };
	u32 state;
	int err;

	err = kstrtou32_from_user(ubuf, count, 10, &state);
	if (err)
		return err;
	if (state < PON_ONU_STATE_O1 || state > PON_ONU_STATE_O7)
		return -EINVAL;

	mutex_lock(&sim->lock);
	if (sim->state != state)
		sim->since = get_jiffies_64();
	sim->state = state;
	pon_sim_fill_status(sim, &st);
	pon_device_notify(sim->pon, &st);
	mutex_unlock(&sim->lock);
	return count;
}

static ssize_t pon_sim_state_read(struct file *file, char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct pon_sim *sim = file->private_data;
	char buf[8];
	int len = scnprintf(buf, sizeof(buf), "%u\n", sim->state);

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations pon_sim_state_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= pon_sim_state_read,
	.write	= pon_sim_state_write,
};

/* debugfs: omci_link (w): 0 or 1, the OMCI channel's existence */
static ssize_t pon_sim_omci_link_write(struct file *file,
				       const char __user *ubuf, size_t count,
				       loff_t *ppos)
{
	struct pon_sim *sim = file->private_data;
	bool up;
	int err;

	err = kstrtobool_from_user(ubuf, count, &up);
	if (err)
		return err;
	mutex_lock(&sim->lock);
	sim->omci_up = up;
	mutex_unlock(&sim->lock);
	pon_omci_port_link_set(sim->omci, up);
	return count;
}

static const struct file_operations pon_sim_omci_link_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= pon_sim_omci_link_write,
};

/* debugfs: rx (w): "FLOW": receive a frame in that flow */
static ssize_t pon_sim_rx_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct pon_sim *sim = file->private_data;
	struct sk_buff *skb;
	unsigned int flow;
	int err;

	err = kstrtouint_from_user(ubuf, count, 10, &flow);
	if (err)
		return err;
	if (flow >= PON_SIM_FLOWS)
		return -ERANGE;

	skb = alloc_skb(PON_SIM_RX_LEN + NET_IP_ALIGN, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, NET_IP_ALIGN);
	/* A broadcast frame from an address of the documentation range. */
	eth_broadcast_addr(skb_put(skb, ETH_ALEN));
	memcpy(skb_put(skb, ETH_ALEN), "\x00\x00\x5e\x00\x53\x01", ETH_ALEN);
	put_unaligned_be16(ETH_P_802_EX1, skb_put(skb, 2));
	memset(skb_put(skb, PON_SIM_RX_LEN - ETH_HLEN), 0, PON_SIM_RX_LEN - ETH_HLEN);

	if (test_bit(flow, sim->flows)) {
		atomic64_inc(&sim->stats[flow].rx_frames);
		atomic64_add(skb->len, &sim->stats[flow].rx_octets);
	}

	/* The class owns the frame when the flow has a GEM interface. */
	if (!pon_gem_rx(sim->pon, flow, skb)) {
		kfree_skb(skb);
		return -ENOENT;
	}
	return count;
}

static const struct file_operations pon_sim_rx_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= pon_sim_rx_write,
};

/* debugfs: flows (r): "FLOW PORT-ID TCONT DIRECTION DS-RATE DS-BURST" a line for each */
static int pon_sim_flows_show(struct seq_file *m, void *unused)
{
	struct pon_sim *sim = m->private;
	unsigned int f;

	mutex_lock(&sim->lock);
	for_each_set_bit(f, sim->flows, PON_SIM_FLOWS)
		seq_printf(m, "%u %u %u %u %llu %u\n", f, sim->cfg[f].port_id,
			   sim->cfg[f].tcont, sim->cfg[f].direction,
			   sim->cfg[f].ds_rate, sim->cfg[f].ds_burst);
	mutex_unlock(&sim->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_sim_flows);

/* debugfs: calls (r): "NAME COUNT" a line for each operation of interest */
static int pon_sim_calls_show(struct seq_file *m, void *unused)
{
	struct pon_sim *sim = m->private;

	mutex_lock(&sim->lock);
	seq_printf(m, "bridge_join %u\nbridge_leave %u\nvlan_add %u\nvlan_del %u\n"
		   "mrouter %u\nrx_filter %u\nchange %u\nomci_start %u\n"
		   "omci_stop %u\nomci_tx %u\n",
		   sim->calls.bridge_join, sim->calls.bridge_leave,
		   sim->calls.vlan_add, sim->calls.vlan_del, sim->calls.mrouter,
		   sim->calls.rx_filter, sim->calls.change,
		   sim->calls.omci_start, sim->calls.omci_stop,
		   sim->calls.omci_tx);
	mutex_unlock(&sim->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_sim_calls);

static int __init pon_sim_init(void)
{
	struct pon_sim *sim;
	int err;

	sim = kzalloc(sizeof(*sim), GFP_KERNEL);
	if (!sim)
		return -ENOMEM;
	mutex_init(&sim->lock);
	sim->state = PON_ONU_STATE_O5;
	sim->since = get_jiffies_64();
	sim->omci_up = true;

	sim->pdev = platform_device_register_simple("pon-sim", PLATFORM_DEVID_NONE,
						    NULL, 0);
	if (IS_ERR(sim->pdev)) {
		err = PTR_ERR(sim->pdev);
		goto free;
	}

	sim->netdev = alloc_netdev(0, "ponsim%d", NET_NAME_UNKNOWN, ether_setup);
	if (!sim->netdev) {
		err = -ENOMEM;
		goto pdev;
	}
	sim->netdev->netdev_ops = &pon_sim_netdev_ops;
	sim->netdev->max_mtu = ETH_DATA_LEN + 8;
	eth_hw_addr_random(sim->netdev);
	err = register_netdev(sim->netdev);
	if (err)
		goto free_netdev;

	sim->omci = pon_omci_port_create(&sim->pdev->dev, &pon_sim_omci_ops, sim);
	if (IS_ERR(sim->omci)) {
		err = PTR_ERR(sim->omci);
		goto unregister;
	}
	sim->pon = pon_device_register(&sim->pdev->dev, &pon_sim_ops,
				       PON_SIM_FLOWS, PON_SIM_TCONTS,
				       PON_SIM_QUEUES, sim);
	if (IS_ERR(sim->pon)) {
		err = PTR_ERR(sim->pon);
		goto omci;
	}
	pon_omci_port_link_set(sim->omci, true);

	sim->dbg = debugfs_create_dir("pon_sim", NULL);
	debugfs_create_file("state", 0600, sim->dbg, sim, &pon_sim_state_fops);
	debugfs_create_file("omci_link", 0200, sim->dbg, sim, &pon_sim_omci_link_fops);
	debugfs_create_file("rx", 0200, sim->dbg, sim, &pon_sim_rx_fops);
	debugfs_create_file("flows", 0400, sim->dbg, sim, &pon_sim_flows_fops);
	debugfs_create_file("calls", 0400, sim->dbg, sim, &pon_sim_calls_fops);

	pon_sim = sim;
	return 0;

omci:
	pon_omci_port_remove(sim->omci);
unregister:
	unregister_netdev(sim->netdev);
free_netdev:
	free_netdev(sim->netdev);
pdev:
	platform_device_unregister(sim->pdev);
free:
	kfree(sim);
	return err;
}

static void __exit pon_sim_exit(void)
{
	struct pon_sim *sim = pon_sim;

	debugfs_remove_recursive(sim->dbg);
	/* The data path first: it deletes the GEM interfaces over ponsim0. */
	pon_device_unregister(sim->pon);
	pon_omci_port_remove(sim->omci);
	unregister_netdev(sim->netdev);
	free_netdev(sim->netdev);
	platform_device_unregister(sim->pdev);
	kfree(sim);
}

module_init(pon_sim_init);
module_exit(pon_sim_exit);

MODULE_DESCRIPTION("A stand-in ONU MAC for testing the PON class");
MODULE_AUTHOR("Paweł Phan <szop.hektar_1w@icloud.com>");
MODULE_LICENSE("GPL");
