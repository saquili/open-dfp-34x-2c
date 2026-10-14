// SPDX-License-Identifier: GPL-2.0-only
/*
 * PON class core: the PON devices and the OMCI control port.
 *
 * An optical network unit (ONU) is represented by one PON device, ponN, a
 * child of its MAC driver's device. The MAC registers two things against its
 * device, and the first of them creates ponN: its OMCI control port, here,
 * and its data path, on which management creates GEM interfaces (pon_gem.c).
 * The pon generic netlink family (pon_netlink.c) reports on ponN.
 *
 * The OMCI control port is the character device ponNomci0, a child of ponN.
 * Through it a daemon in userspace runs the ONU management and control
 * interface (OMCI, ITU-T G.988): a read returns one message from the OLT, a
 * write sends one message to it, and the class does not look inside either.
 * The port is modelled on the control ports of the WWAN subsystem, without
 * what OMCI does not need: one port type, no terminal handling, and no
 * splitting of writes. It is a character device and not a network interface
 * because OMCI is not Ethernet.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/poll.h>
#include <linux/pon.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/rtnetlink.h>
#include <linux/wait.h>

#include "pon_core.h"

/* Minors of the pon_omci region: one per OMCI port, and so per MAC. */
#define PON_MAX_MINORS		256

/*
 * Received messages waiting for the reader; further ones are dropped and
 * counted. The limit assumes that an OLT waits for each answer before it
 * sends its next request, so that a few messages in flight are normal and
 * 64 is plenty; that assumption is not verified.
 */
#define PON_OMCI_RXQ_LEN	64

/*
 * The registration lock. It serialises the creation and removal of PON
 * devices, OMCI ports and data paths with each other and with the calls of
 * owns_netdev and dev_status. Where RTNL is held as well, RTNL is taken
 * first, so owns_netdev and dev_status must not take RTNL.
 */
static DEFINE_MUTEX(pon_register_lock);
#if IS_ENABLED(CONFIG_PON_OMCI_INJECT)
/* The debugfs directory "pon", with a directory for each OMCI port. */
static struct dentry *pon_debugfs;
#endif
/* The N of each ponN, and the minor of each OMCI port. */
static DEFINE_IDA(pon_ids);
static DEFINE_IDA(pon_minors);
static int pon_major;

/* The PON devices, ponN, and their OMCI ports, ponNomci0. */
static const struct class pon_class = {
	.name = "pon",
};

/**
 * struct pon_omci_port - an OMCI control port, the character device ponNomci0
 * @dev: the port's device: a child of ponN, with the port's device number
 * @ops: the MAC's operations; NULL once the MAC has removed the port
 * @opened: a file holds the port open; there is at most one
 * @link: the MAC has the OMCI channel (pon_omci_port_link_set())
 * @rxq: the received messages waiting for the reader
 * @waitq: where readers and poll wait for a message, a change of @link or
 *	   the removal of the port
 * @rx_messages: messages queued for the reader
 * @rx_dropped: messages dropped because nobody held the port open or the
 *		reader was too far behind
 * @rx_discarded: messages still waiting for the reader when the OMCI channel
 *		  went down
 * @tx_messages: messages the MAC accepted
 * @tx_errors: messages the MAC refused
 * @debugfs: the port's debugfs directory, which holds its inject file
 */
struct pon_omci_port {
	struct device dev;
	const struct pon_omci_port_ops *ops;
	/**
	 * @ops_lock: serialises open, close and the removal of the port with
	 * each other and with the calls into the MAC
	 */
	struct mutex ops_lock;
	bool opened;
	bool link;
	struct sk_buff_head rxq;
	wait_queue_head_t waitq;
	atomic_long_t rx_messages;
	atomic_long_t rx_dropped;
	atomic_long_t rx_discarded;
	atomic_long_t tx_messages;
	atomic_long_t tx_errors;
	struct dentry *debugfs;
};

static void pon_omci_debugfs_init(struct pon_omci_port *port);
static void pon_omci_debugfs_exit(struct pon_omci_port *port);

#define to_pon_device(d)	container_of(d, struct pon_device, dev)
#define to_pon_omci_port(d)	container_of(d, struct pon_omci_port, dev)

static void pon_dev_release(struct device *dev)
{
	struct pon_device *pon = to_pon_device(dev);

	ida_free(&pon_ids, pon->id);
	kfree(pon);
}

static const struct device_type pon_dev_type = {
	.name = "pon",
	.release = pon_dev_release,
};

static int pon_dev_parent_match(struct device *dev, const void *parent)
{
	return dev->type == &pon_dev_type && dev->parent == parent;
}

/*
 * The PON device of @parent, with a reference for pon_dev_put(). The MAC's
 * first registration creates it, as ponN with the lowest N free, whichever of
 * the OMCI port and the data path comes first; the second finds it.
 */
static struct pon_device *pon_dev_get(struct device *parent)
{
	struct pon_device *pon;
	struct device *dev;
	int id, err;

	lockdep_assert_held(&pon_register_lock);

	dev = class_find_device(&pon_class, NULL, parent, pon_dev_parent_match);
	if (dev)
		return to_pon_device(dev);

	id = ida_alloc(&pon_ids, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(id);

	pon = kzalloc(sizeof(*pon), GFP_KERNEL);
	if (!pon) {
		ida_free(&pon_ids, id);
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * Initialised before anything can fail, so that put_device() runs the
	 * release, which frees the id and the device, on every error path.
	 */
	pon->id = id;
	device_initialize(&pon->dev);
	pon->dev.parent = parent;
	pon->dev.class = &pon_class;
	pon->dev.type = &pon_dev_type;
	err = dev_set_name(&pon->dev, "pon%u", id);
	if (!err)
		err = device_add(&pon->dev);
	if (err) {
		put_device(&pon->dev);
		return ERR_PTR(err);
	}

	get_device(&pon->dev);
	return pon;
}

/*
 * Drop the reference pon_dev_get() gave, and unregister ponN when neither of
 * the MAC's registrations, its OMCI port and its data path, is left. The two
 * may go in either order.
 */
static void pon_dev_put(struct pon_device *pon)
{
	lockdep_assert_held(&pon_register_lock);

	if (!pon->registrations)
		device_unregister(&pon->dev);
	put_device(&pon->dev);
}

/*
 * Only a PON device with a registered data path that is not going can own a
 * netdev. Called under the registration lock, which keeps pon->ops and
 * pon->unregistering from changing.
 */
static int pon_dev_netdev_match(struct device *dev, const void *netdev)
{
	struct pon_device *pon;

	if (dev->type != &pon_dev_type)
		return 0;

	pon = to_pon_device(dev);
	return pon->ops && !pon->unregistering &&
	       pon->ops->owns_netdev(pon, netdev);
}

/**
 * pon_device_get_by_netdev() - find the ONU whose traffic a netdev carries
 * @dev: the network device
 *
 * Asks the owns_netdev operation of each registered data path, under the
 * registration lock.
 *
 * Context: process context; RTNL may be held.
 * Return: the PON device with a reference held, for pon_device_put(), or
 * NULL.
 */
struct pon_device *pon_device_get_by_netdev(const struct net_device *dev)
{
	struct device *d;

	mutex_lock(&pon_register_lock);
	d = class_find_device(&pon_class, NULL, dev, pon_dev_netdev_match);
	mutex_unlock(&pon_register_lock);

	return d ? to_pon_device(d) : NULL;
}

/**
 * pon_device_put() - drop the reference a lookup returned
 * @pon: the PON device
 */
void pon_device_put(struct pon_device *pon)
{
	put_device(&pon->dev);
}

static int pon_dev_id_match(struct device *dev, const void *data)
{
	const u32 *id = data;

	return dev->type == &pon_dev_type && to_pon_device(dev)->id == *id;
}

/**
 * pon_device_get_by_id() - find a PON device by its number
 * @id: the N of ponN
 *
 * Return: the PON device with a reference held, for pon_device_put(), or
 * NULL.
 */
struct pon_device *pon_device_get_by_id(u32 id)
{
	struct device *d;

	d = class_find_device(&pon_class, NULL, &id, pon_dev_id_match);

	return d ? to_pon_device(d) : NULL;
}

/*
 * The cursor lookup of dumps. The class keeps its devices in registration
 * order, and a dump needs them in the order of their ids, so every device is
 * looked at, and the one with the lowest id at or above the cursor is kept,
 * with a reference.
 */
struct pon_dev_next {
	u32 from;
	struct pon_device *found;
};

static int pon_dev_next_lowest(struct device *dev, void *data)
{
	struct pon_dev_next *next = data;
	struct pon_device *pon;

	if (dev->type != &pon_dev_type)
		return 0;

	pon = to_pon_device(dev);
	if (pon->id < next->from)
		return 0;
	if (next->found && next->found->id <= pon->id)
		return 0;

	if (next->found)
		put_device(&next->found->dev);
	get_device(&pon->dev);
	next->found = pon;
	return 0;
}

/**
 * pon_device_get_next() - the PON device with the lowest id from a cursor on
 * @from: the lowest id to consider
 *
 * Return: the PON device with a reference held, for pon_device_put(), or
 * NULL when no PON device has an id of @from or above.
 */
struct pon_device *pon_device_get_next(u32 from)
{
	struct pon_dev_next next = { .from = from };

	class_for_each_device(&pon_class, NULL, &next, pon_dev_next_lowest);

	return next.found;
}

/**
 * pon_device_status() - ask the MAC for the status of its ONU
 * @pon: the PON device
 * @st: the report, which the caller has set to "nothing known"
 *
 * The MAC's dev_status runs under the registration lock, so that the data
 * path cannot unregister meanwhile.
 *
 * Context: process context; may sleep.
 * Return: 0; -EOPNOTSUPP when no data path is registered or the MAC has no
 * dev_status; or the MAC's error.
 */
int pon_device_status(struct pon_device *pon, struct pon_dev_status *st)
{
	int err = -EOPNOTSUPP;

	mutex_lock(&pon_register_lock);
	if (pon->ops && pon->ops->dev_status)
		err = pon->ops->dev_status(pon, st);
	mutex_unlock(&pon_register_lock);

	return err;
}

/**
 * pon_device_tcont() - ask the MAC for the Alloc-ID of one of its T-CONTs
 * @pon: the PON device
 * @index: the T-CONT
 * @alloc_id: the Alloc-ID, when there is one
 *
 * The MAC's tcont_get runs under the registration lock, as dev_status does
 * in pon_device_status().
 *
 * Context: process context; may sleep.
 * Return: 0; -ENOENT when the T-CONT holds no Alloc-ID; -ENODEV when no data
 * path is registered, the MAC has no tcont_get, or @index is not below the
 * T-CONTs it registered; or the MAC's error.
 */
int pon_device_tcont(struct pon_device *pon, unsigned int index, u16 *alloc_id)
{
	int err = -ENODEV;

	mutex_lock(&pon_register_lock);
	if (pon->ops && pon->ops->tcont_get && index < pon->num_tconts)
		err = pon->ops->tcont_get(pon, index, alloc_id);
	mutex_unlock(&pon_register_lock);

	return err;
}

/**
 * pon_device_tcont_set() - map one of the MAC's T-CONTs to an Alloc-ID
 * @pon: the PON device
 * @index: the T-CONT
 * @alloc_id: the Alloc-ID, 0 to 4095, or a negative number to unmap it
 * @extack: where a refusal is explained
 *
 * The MAC's tcont_set runs under the registration lock, as tcont_get does in
 * pon_device_tcont().
 *
 * Context: process context; may sleep.
 * Return: 0; -ENODEV when no data path is registered; -EOPNOTSUPP when the
 * MAC has no tcont_set; -ENOENT when @index is not below the T-CONTs it
 * registered; or the MAC's error.
 */
int pon_device_tcont_set(struct pon_device *pon, unsigned int index,
			 int alloc_id, struct netlink_ext_ack *extack)
{
	int err;

	mutex_lock(&pon_register_lock);
	if (!pon->ops) {
		NL_SET_ERR_MSG(extack, "the ONU has no data path");
		err = -ENODEV;
	} else if (!pon->ops->tcont_set) {
		NL_SET_ERR_MSG(extack, "the MAC does not map T-CONTs");
		err = -EOPNOTSUPP;
	} else if (index >= pon->num_tconts) {
		NL_SET_ERR_MSG(extack, "no such T-CONT on this ONU");
		err = -ENOENT;
	} else {
		err = pon->ops->tcont_set(pon, index, alloc_id, extack);
	}
	mutex_unlock(&pon_register_lock);

	return err;
}

/**
 * pon_device_tod_set() - set the ONU's clock at a downstream frame
 * @pon: the PON device
 * @superframe: the superframe counter of the frame
 * @tstamp: the OLT's TstampN for that frame
 * @extack: where a refusal is explained
 *
 * The MAC's tod_set runs under the registration lock, as tcont_set does in
 * pon_device_tcont_set().
 *
 * Context: process context; may sleep.
 * Return: 0; -ENODEV when no data path is registered; -EOPNOTSUPP when the
 * MAC has no tod_set; or the MAC's error.
 */
int pon_device_tod_set(struct pon_device *pon, u32 superframe,
		       const struct timespec64 *tstamp,
		       struct netlink_ext_ack *extack)
{
	int err;

	mutex_lock(&pon_register_lock);
	if (!pon->ops) {
		NL_SET_ERR_MSG(extack, "the ONU has no data path");
		err = -ENODEV;
	} else if (!pon->ops->tod_set) {
		NL_SET_ERR_MSG(extack, "the MAC cannot set a clock at a frame");
		err = -EOPNOTSUPP;
	} else {
		err = pon->ops->tod_set(pon, superframe, tstamp, extack);
	}
	mutex_unlock(&pon_register_lock);

	return err;
}

struct pon_device *pon_device_register(struct device *parent,
				       const struct pon_device_ops *ops,
				       unsigned int num_flows,
				       unsigned int num_tconts,
				       unsigned int num_queues, void *drvdata)
{
	struct net_device __rcu **gem;
	struct pon_device *pon;

	if (!ops || !ops->owns_netdev || !ops->gem_add || !ops->gem_del ||
	    !ops->gem_xmit || !num_flows || !num_queues)
		return ERR_PTR(-EINVAL);

	/*
	 * The optional operations come in pairs: a MAC that joins a bridge has
	 * to be able to leave it, and one that takes a VLAN has to be able to
	 * drop it.
	 */
	if (!ops->gem_bridge_join != !ops->gem_bridge_leave ||
	    !ops->gem_vlan_add != !ops->gem_vlan_del)
		return ERR_PTR(-EINVAL);

	gem = kcalloc(num_flows, sizeof(*gem), GFP_KERNEL);
	if (!gem)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&pon_register_lock);
	pon = pon_dev_get(parent);
	if (IS_ERR(pon))
		goto out;

	if (pon->ops) {
		pon_dev_put(pon);
		pon = ERR_PTR(-EBUSY);
		goto out;
	}

	/*
	 * The data path keeps the reference pon_dev_get() gave until it
	 * unregisters. With its operations set, lookups by netdev find it.
	 */
	pon->gem = gem;
	pon->num_flows = num_flows;
	pon->num_tconts = num_tconts;
	pon->num_queues = num_queues;
	pon->drvdata = drvdata;
	pon->ops = ops;
	pon->registrations++;
	gem = NULL;
out:
	mutex_unlock(&pon_register_lock);
	kfree(gem);
	return pon;
}
EXPORT_SYMBOL_GPL(pon_device_register);

void pon_device_unregister(struct pon_device *pon)
{
	/*
	 * The GEM interfaces go first, under RTNL, which is not taken inside
	 * the registration lock; the operations are cleared after them. A GEM
	 * interface is created under RTNL too, and finds its ONU by the
	 * lookup that the mark set here fails, so none can be created between
	 * the removal and the clearing.
	 */
	rtnl_lock();
	mutex_lock(&pon_register_lock);
	pon->unregistering = true;
	mutex_unlock(&pon_register_lock);
	pon_gem_remove_all(pon);
	rtnl_unlock();

	mutex_lock(&pon_register_lock);
	pon->unregistering = false;
	pon->ops = NULL;
	kfree(pon->gem);
	pon->gem = NULL;
	pon->num_flows = 0;
	pon->registrations--;
	pon_dev_put(pon);
	mutex_unlock(&pon_register_lock);
}
EXPORT_SYMBOL_GPL(pon_device_unregister);

void *pon_device_get_drvdata(struct pon_device *pon)
{
	return pon->drvdata;
}
EXPORT_SYMBOL_GPL(pon_device_get_drvdata);

/*
 * The counters of struct pon_omci_port, read-only in the port's sysfs group
 * "statistics".
 */
static ssize_t rx_messages_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%ld\n",
			  atomic_long_read(&to_pon_omci_port(dev)->rx_messages));
}
static DEVICE_ATTR_RO(rx_messages);

static ssize_t rx_dropped_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%ld\n",
			  atomic_long_read(&to_pon_omci_port(dev)->rx_dropped));
}
static DEVICE_ATTR_RO(rx_dropped);

static ssize_t rx_discarded_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%ld\n",
			  atomic_long_read(&to_pon_omci_port(dev)->rx_discarded));
}
static DEVICE_ATTR_RO(rx_discarded);

static ssize_t tx_messages_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%ld\n",
			  atomic_long_read(&to_pon_omci_port(dev)->tx_messages));
}
static DEVICE_ATTR_RO(tx_messages);

static ssize_t tx_errors_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%ld\n",
			  atomic_long_read(&to_pon_omci_port(dev)->tx_errors));
}
static DEVICE_ATTR_RO(tx_errors);

static struct attribute *pon_omci_port_stat_attrs[] = {
	&dev_attr_rx_messages.attr,
	&dev_attr_rx_dropped.attr,
	&dev_attr_rx_discarded.attr,
	&dev_attr_tx_messages.attr,
	&dev_attr_tx_errors.attr,
	NULL
};

static const struct attribute_group pon_omci_port_stat_group = {
	.name = "statistics",
	.attrs = pon_omci_port_stat_attrs,
};

static const struct attribute_group *pon_omci_port_groups[] = {
	&pon_omci_port_stat_group,
	NULL
};

static void pon_omci_port_release(struct device *dev)
{
	struct pon_omci_port *port = to_pon_omci_port(dev);

	ida_free(&pon_minors, MINOR(dev->devt));
	skb_queue_purge(&port->rxq);
	mutex_destroy(&port->ops_lock);
	kfree(port);
}

static const struct device_type pon_omci_port_type = {
	.name = "pon_omci",
	.release = pon_omci_port_release,
	.groups = pon_omci_port_groups,
};

struct pon_omci_port *pon_omci_port_create(struct device *parent,
					   const struct pon_omci_port_ops *ops,
					   void *drvdata)
{
	struct pon_omci_port *port;
	struct pon_device *pon;
	int minor, err;

	if (!ops || !ops->tx)
		return ERR_PTR(-EINVAL);

	mutex_lock(&pon_register_lock);

	pon = pon_dev_get(parent);
	if (IS_ERR(pon)) {
		err = PTR_ERR(pon);
		goto out_unlock;
	}

	minor = ida_alloc_max(&pon_minors, PON_MAX_MINORS - 1, GFP_KERNEL);
	if (minor < 0) {
		err = minor;
		goto out_put;
	}

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port) {
		ida_free(&pon_minors, minor);
		err = -ENOMEM;
		goto out_put;
	}

	port->ops = ops;
	mutex_init(&port->ops_lock);
	skb_queue_head_init(&port->rxq);
	init_waitqueue_head(&port->waitq);

	device_initialize(&port->dev);
	port->dev.parent = &pon->dev;
	port->dev.class = &pon_class;
	port->dev.type = &pon_omci_port_type;
	port->dev.devt = MKDEV(pon_major, minor);
	dev_set_drvdata(&port->dev, drvdata);

	/*
	 * Port number 0, the only one: an ONU has at most one OMCI connection
	 * (ITU-T G.984.3 clause 9.2.3.14).
	 */
	err = dev_set_name(&port->dev, "pon%uomci0", pon->id);
	if (!err)
		err = device_add(&port->dev);
	if (err) {
		/*
		 * The device is initialised, so the release function frees the
		 * minor and the port, whichever step failed.
		 */
		put_device(&port->dev);
		goto out_put;
	}

	/* The port keeps the reference pon_dev_get() gave until it goes. */
	pon->registrations++;
	mutex_unlock(&pon_register_lock);

	pon_omci_debugfs_init(port);

	dev_info(&pon->dev, "OMCI port %s attached\n", dev_name(&port->dev));
	return port;

out_put:
	pon_dev_put(pon);
out_unlock:
	mutex_unlock(&pon_register_lock);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(pon_omci_port_create);

void pon_omci_port_remove(struct pon_omci_port *port)
{
	struct device *parent = port->dev.parent;

	pon_omci_debugfs_exit(port);

	/*
	 * Stop an open port and detach the operations. From then on a read
	 * fails with -ENODEV, a write fails as well, and poll reports a
	 * hang-up. A file that holds the port open keeps a reference on it
	 * until it is closed.
	 */
	mutex_lock(&port->ops_lock);
	if (port->opened && port->ops->stop)
		port->ops->stop(port);
	port->ops = NULL;
	mutex_unlock(&port->ops_lock);

	wake_up_interruptible_all(&port->waitq);
	skb_queue_purge(&port->rxq);

	/*
	 * Drop the port's registration; pon_dev_put() unregisters ponN when
	 * the data path has gone as well.
	 */
	mutex_lock(&pon_register_lock);
	device_unregister(&port->dev);
	to_pon_device(parent)->registrations--;
	pon_dev_put(to_pon_device(parent));
	mutex_unlock(&pon_register_lock);
}
EXPORT_SYMBOL_GPL(pon_omci_port_remove);

void pon_omci_port_rx(struct pon_omci_port *port, struct sk_buff *skb)
{
	/*
	 * The message is dropped when nobody holds the port open, since a new
	 * opener must not be handed requests the OLT has long since given up
	 * on, and when the reader has fallen PON_OMCI_RXQ_LEN messages behind.
	 */
	if (!READ_ONCE(port->opened) ||
	    skb_queue_len(&port->rxq) >= PON_OMCI_RXQ_LEN) {
		atomic_long_inc(&port->rx_dropped);
		dev_kfree_skb_any(skb);
		return;
	}

	skb_queue_tail(&port->rxq, skb);
	atomic_long_inc(&port->rx_messages);
	wake_up_interruptible(&port->waitq);
}
EXPORT_SYMBOL_GPL(pon_omci_port_rx);

#if IS_ENABLED(CONFIG_PON_OMCI_INJECT)

/*
 * The inject file, pon/ponNomci0/inject in debugfs. A write puts one message
 * into the port as if the MAC had received it: same queue, same counters,
 * same wake-up. The daemon's answers still leave through the MAC, so the
 * stack above the port can be exercised against a real OLT, including the
 * paths of provisioning that an OLT which provisions little never reaches.
 *
 * A write holds pairs of hex digits, with whitespace allowed between pairs,
 * for a message of 1 to PON_OMCI_MAX_MSG bytes. An odd digit, a character
 * that is not a hex digit, or no byte at all is -EINVAL, and nothing is
 * injected; a successful write returns its whole count. An injected message
 * counts as received, also when it is dropped for want of a reader. It
 * bypasses whatever the MAC gates reception on, such as the activation state
 * or the OMCI channel, so the daemon's answers may still fail with -ENOLINK.
 * The file exists only in a kernel built with CONFIG_PON_OMCI_INJECT.
 */
static ssize_t pon_omci_inject_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct pon_omci_port *port = file->private_data;
	struct sk_buff *skb;
	char *text;
	u8 *msg;
	size_t n = 0;
	size_t i;
	int err = 0;

	/* The digits of the largest message, and some whitespace. */
	if (count < 2 || count > 2 * PON_OMCI_MAX_MSG + 64)
		return -EINVAL;

	text = memdup_user_nul(buf, count);
	if (IS_ERR(text))
		return PTR_ERR(text);

	msg = kzalloc(PON_OMCI_MAX_MSG, GFP_KERNEL);
	if (!msg) {
		kfree(text);
		return -ENOMEM;
	}

	for (i = 0; text[i]; i++) {
		int hi, lo;

		if (isspace(text[i]))
			continue;
		hi = hex_to_bin(text[i]);
		lo = text[i + 1] ? hex_to_bin(text[i + 1]) : -1;
		if (hi < 0 || lo < 0 || n >= PON_OMCI_MAX_MSG) {
			err = -EINVAL;
			goto out;
		}
		msg[n++] = (u8)(hi << 4 | lo);
		i++;
	}

	if (!n) {
		err = -EINVAL;
		goto out;
	}

	skb = alloc_skb(n, GFP_KERNEL);
	if (!skb) {
		err = -ENOMEM;
		goto out;
	}
	skb_put_data(skb, msg, n);

	pon_omci_port_rx(port, skb);
out:
	kfree(msg);
	kfree(text);
	return err ? err : (ssize_t)count;
}

static const struct file_operations pon_omci_inject_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= pon_omci_inject_write,
	.llseek	= default_llseek,
};

static void pon_omci_debugfs_init(struct pon_omci_port *port)
{
	port->debugfs = debugfs_create_dir(dev_name(&port->dev), pon_debugfs);
	debugfs_create_file("inject", 0200, port->debugfs, port,
			    &pon_omci_inject_fops);
}

static void pon_omci_debugfs_exit(struct pon_omci_port *port)
{
	debugfs_remove_recursive(port->debugfs);
	port->debugfs = NULL;
}
#else
static void pon_omci_debugfs_init(struct pon_omci_port *port) { }
static void pon_omci_debugfs_exit(struct pon_omci_port *port) { }
#endif

void pon_omci_port_link_set(struct pon_omci_port *port, bool up)
{
	struct sk_buff *skb;

	if (READ_ONCE(port->link) == up)
		return;

	WRITE_ONCE(port->link, up);
	/*
	 * The requests still queued belong to an OMCI channel that has ended,
	 * as Deactivate_ONU-ID discards the OMCI Port-ID (ITU-T G.984.3
	 * clause 9.2.1): discard them, counted apart from the messages lost to
	 * a missing or slow reader.
	 */
	if (!up) {
		while ((skb = skb_dequeue(&port->rxq))) {
			atomic_long_inc(&port->rx_discarded);
			kfree_skb(skb);
		}
	}
	/* Both edges wake poll, whose EPOLLOUT follows the channel. */
	wake_up_interruptible(&port->waitq);
}
EXPORT_SYMBOL_GPL(pon_omci_port_link_set);

void *pon_omci_port_get_drvdata(struct pon_omci_port *port)
{
	return dev_get_drvdata(&port->dev);
}
EXPORT_SYMBOL_GPL(pon_omci_port_get_drvdata);

static int pon_omci_port_minor_match(struct device *dev, const void *minor)
{
	return dev->type == &pon_omci_port_type &&
	       MINOR(dev->devt) == *(const unsigned int *)minor;
}

static int pon_omci_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct pon_omci_port *port;
	struct device *dev;
	int err = 0;

	/*
	 * Whoever holds the port answers the OLT for the ONU, and can
	 * reconfigure its data path through OMCI.
	 */
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	dev = class_find_device(&pon_class, NULL, &minor,
				pon_omci_port_minor_match);
	if (!dev)
		return -ENODEV;
	port = to_pon_omci_port(dev);

	/*
	 * One opener at a time: OMCI is request and response between one OLT
	 * and one management entity, and two readers would each see half of
	 * the conversation. Opening empties the queue, so that the new opener
	 * is not handed requests the OLT has long since given up on.
	 */
	mutex_lock(&port->ops_lock);
	if (!port->ops)
		err = -ENODEV;
	else if (port->opened)
		err = -EBUSY;
	else if (port->ops->start)
		err = port->ops->start(port);
	if (!err) {
		skb_queue_purge(&port->rxq);
		WRITE_ONCE(port->opened, true);
	}
	mutex_unlock(&port->ops_lock);

	if (err) {
		put_device(dev);
		return err;
	}

	/*
	 * The file keeps the reference class_find_device() took until it is
	 * closed. It is a stream: it has no position.
	 */
	file->private_data = port;
	return stream_open(inode, file);
}

static int pon_omci_release(struct inode *inode, struct file *file)
{
	struct pon_omci_port *port = file->private_data;

	mutex_lock(&port->ops_lock);
	WRITE_ONCE(port->opened, false);
	if (port->ops && port->ops->stop)
		port->ops->stop(port);
	mutex_unlock(&port->ops_lock);

	skb_queue_purge(&port->rxq);
	put_device(&port->dev);
	return 0;
}

/* A reader wakes for a message or for the removal of the port. */
static bool pon_omci_readable(struct pon_omci_port *port)
{
	return !skb_queue_empty(&port->rxq) || !READ_ONCE(port->ops);
}

/*
 * A read returns exactly one message. A buffer smaller than the message fails
 * with -EMSGSIZE and leaves the message queued, so that half a message is
 * never delivered; a failed copy leaves it queued too. On an empty queue a
 * non-blocking read fails with -EAGAIN, and a blocking one waits for a
 * message or for the removal of the port, after which reads fail with
 * -ENODEV. A queue that the loss of the OMCI channel empties between the
 * wake-up and the dequeue is an empty queue again, not a removed port.
 */
static ssize_t pon_omci_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct pon_omci_port *port = file->private_data;
	struct sk_buff *skb;
	ssize_t len;

	while (!(skb = skb_dequeue(&port->rxq))) {
		if (!READ_ONCE(port->ops))
			return -ENODEV;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(port->waitq,
					     pon_omci_readable(port)))
			return -ERESTARTSYS;
	}

	if (count < skb->len) {
		skb_queue_head(&port->rxq, skb);
		return -EMSGSIZE;
	}

	len = skb->len;
	if (copy_to_user(buf, skb->data, len)) {
		skb_queue_head(&port->rxq, skb);
		return -EFAULT;
	}

	consume_skb(skb);
	return len;
}

/*
 * A write sends exactly one message and is never split: the largest OMCI
 * message fits one GEM frame, whose 12-bit length field allows 4095 bytes
 * (ITU-T G.984.3 clause 8.3.1). The class does not look inside; the daemon
 * builds the whole message, the integrity check in its last four bytes
 * included.
 *
 * Without the OMCI channel there is nobody to send to, since OMCI travels
 * only on the Port-ID the OLT configured for it (G.984.3 clause 14.1): the
 * write fails with -ENOLINK. A message the MAC refuses fails with the MAC's
 * error and counts in tx_errors; -EAGAIN means that its transmit path is
 * momentarily full.
 */
static ssize_t pon_omci_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct pon_omci_port *port = file->private_data;
	struct sk_buff *skb;
	int err;

	if (!count)
		return -EINVAL;
	if (count > PON_OMCI_MAX_MSG)
		return -EMSGSIZE;
	if (!READ_ONCE(port->link))
		return -ENOLINK;

	skb = alloc_skb(count, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	if (copy_from_user(skb_put(skb, count), buf, count)) {
		kfree_skb(skb);
		return -EFAULT;
	}

	mutex_lock(&port->ops_lock);
	err = port->ops ? port->ops->tx(port, skb) : -ENODEV;
	mutex_unlock(&port->ops_lock);

	if (err) {
		atomic_long_inc(&port->tx_errors);
		kfree_skb(skb);
		return err;
	}

	atomic_long_inc(&port->tx_messages);
	return count;
}

/*
 * EPOLLIN while a message is queued, EPOLLOUT while the OMCI channel exists,
 * and EPOLLHUP | EPOLLERR once the MAC has removed the port. EPOLLOUT does not
 * promise room in the MAC: a write can still fail with -EAGAIN.
 */
static __poll_t pon_omci_poll(struct file *file, poll_table *wait)
{
	struct pon_omci_port *port = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &port->waitq, wait);

	if (!READ_ONCE(port->ops))
		return EPOLLHUP | EPOLLERR;
	if (READ_ONCE(port->link))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (!skb_queue_empty(&port->rxq))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static const struct file_operations pon_omci_fops = {
	.owner		= THIS_MODULE,
	.open		= pon_omci_open,
	.release	= pon_omci_release,
	.read		= pon_omci_read,
	.write		= pon_omci_write,
	.poll		= pon_omci_poll,
	.llseek		= noop_llseek,
};

/*
 * The class, the character region, the family and the gem link kind have to
 * exist before MAC drivers probe, because the drivers create their OMCI ports
 * and data paths at probe: hence subsys_initcall. A failure unwinds the steps
 * before it, except the debugfs directory, which stays.
 */
static int __init pon_init(void)
{
	int err;

	err = class_register(&pon_class);
	if (err)
		return err;

#if IS_ENABLED(CONFIG_PON_OMCI_INJECT)
	pon_debugfs = debugfs_create_dir("pon", NULL);
#endif

	pon_major = __register_chrdev(0, 0, PON_MAX_MINORS, "pon_omci",
				      &pon_omci_fops);
	if (pon_major < 0) {
		err = pon_major;
		goto err_class;
	}

	err = pon_netlink_init();
	if (err)
		goto err_chrdev;

	err = pon_gem_init();
	if (err)
		goto err_netlink;

	return 0;

err_netlink:
	pon_netlink_exit();
err_chrdev:
	__unregister_chrdev(pon_major, 0, PON_MAX_MINORS, "pon_omci");
err_class:
	class_unregister(&pon_class);
	return err;
}

static void __exit pon_exit(void)
{
	pon_gem_exit();
#if IS_ENABLED(CONFIG_PON_OMCI_INJECT)
	debugfs_remove_recursive(pon_debugfs);
#endif
	pon_netlink_exit();
	__unregister_chrdev(pon_major, 0, PON_MAX_MINORS, "pon_omci");
	class_unregister(&pon_class);
	ida_destroy(&pon_minors);
	ida_destroy(&pon_ids);
}

subsys_initcall(pon_init);
module_exit(pon_exit);

MODULE_DESCRIPTION("PON ONU core: the OMCI control port and GEM interfaces");
MODULE_LICENSE("GPL");
