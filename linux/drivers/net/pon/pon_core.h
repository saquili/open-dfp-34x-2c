/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internals of the PON class, shared by its three parts: the core with the
 * PON devices and the OMCI control port (pon_core.c), the gem link kind
 * (pon_gem.c) and the pon generic netlink family (pon_netlink.c).
 */

#ifndef _NET_PON_CORE_H
#define _NET_PON_CORE_H

#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/pon.h>

/**
 * struct pon_device - an ONU MAC, the PON device ponN
 * @id: the N of ponN
 * @dev: the device: class "pon", a child of the MAC driver's device, without
 *	 a device node of its own
 * @ops: the MAC's data path operations; NULL while no data path is
 *	 registered. Set and cleared under the registration lock.
 * @drvdata: the MAC's private data for its data path
 * @num_flows: the size of the MAC's flow space, and so of @gem; 0 while no
 *	       data path is registered
 * @num_tconts: the T-CONTs the ONU can be given for subscriber traffic, as
 *		the data path registered them; kept when it unregisters
 * @gem: the GEM interface of each flow, or NULL. Written under RTNL, read
 *	 under RCU on the receive path.
 * @unregistering: the data path is going: lookups by network device no
 *		   longer find it, so no GEM interface is created on it. Set
 *		   under RTNL and the registration lock.
 * @registrations: how many of the MAC's two registrations, its OMCI port and
 *		   its data path, are in place; each holds a reference to the
 *		   device, which is unregistered when the last goes, in either
 *		   order. Under the registration lock.
 *
 * The MAC's first registration against its device creates the PON device,
 * whichever of its OMCI port and its data path comes first; the second
 * finds it.
 */
struct pon_device {
	unsigned int id;
	struct device dev;
	const struct pon_device_ops *ops;
	void *drvdata;
	unsigned int num_flows;
	unsigned int num_tconts;
	struct net_device __rcu **gem;
	bool unregistering;
	unsigned int registrations;
};

struct pon_device *pon_device_get_by_netdev(const struct net_device *dev);
struct pon_device *pon_device_get_by_id(u32 id);
struct pon_device *pon_device_get_next(u32 from);
void pon_device_put(struct pon_device *pon);
int pon_device_status(struct pon_device *pon, struct pon_dev_status *st);
int pon_device_tcont(struct pon_device *pon, unsigned int index, u16 *alloc_id);

int pon_netlink_init(void);
void pon_netlink_exit(void);

int pon_gem_init(void);
void pon_gem_exit(void);
void pon_gem_remove_all(struct pon_device *pon);

#endif
