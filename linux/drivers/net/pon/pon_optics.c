// SPDX-License-Identifier: GPL-2.0-only
/*
 * PON class: the optics of an ONU, between their driver and the MAC's.
 *
 * The transmitter and the receiver of an ONU are run by a part of their own,
 * a laser driver on a bus of its own, typically I2C, with a driver of its
 * own. The MAC decides when the transmitter may emit and at which power
 * level, from the activation state and the OLT's messages; the optics'
 * driver knows how. The optics' driver registers the optics here, against
 * its device, and the MAC takes them by the firmware node its own firmware
 * names, as a MAC takes an SFP cage. A device link makes the driver core
 * unbind the MAC before the optics go.
 */

#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pon.h>
#include <linux/property.h>

/* The registered optics, by the firmware node of their device. */
static LIST_HEAD(pon_optics_list);
static DEFINE_MUTEX(pon_optics_lock);

static void pon_optics_unregister(void *data)
{
	struct pon_optics *optics = data;

	mutex_lock(&pon_optics_lock);
	list_del(&optics->list);
	mutex_unlock(&pon_optics_lock);
}

/* The optics registered for @fwnode; under pon_optics_lock. */
static struct pon_optics *pon_optics_find(struct fwnode_handle *fwnode)
{
	struct pon_optics *optics;

	lockdep_assert_held(&pon_optics_lock);
	list_for_each_entry(optics, &pon_optics_list, list)
		if (dev_fwnode(optics->dev) == fwnode)
			return optics;
	return NULL;
}

int devm_pon_optics_register(struct device *dev, struct pon_optics *optics)
{
	struct fwnode_handle *fwnode = dev_fwnode(dev);

	if (!optics->ops || !optics->ops->tx_disable || !fwnode)
		return -EINVAL;

	mutex_lock(&pon_optics_lock);
	if (pon_optics_find(fwnode)) {
		mutex_unlock(&pon_optics_lock);
		return -EBUSY;
	}
	optics->dev = dev;
	list_add_tail(&optics->list, &pon_optics_list);
	mutex_unlock(&pon_optics_lock);

	return devm_add_action_or_reset(dev, pon_optics_unregister, optics);
}
EXPORT_SYMBOL_GPL(devm_pon_optics_register);

static void pon_optics_release(void *data)
{
	put_device(data);
}

struct pon_optics *devm_pon_optics_get(struct device *consumer,
				       struct fwnode_handle *fwnode)
{
	struct pon_optics *optics;
	struct device *dev;
	int err;

	if (!fwnode)
		return NULL;

	/*
	 * A driver registers its optics at the end of its probe; one still
	 * probing may yet fail and withdraw them, so it is waited for.
	 */
	mutex_lock(&pon_optics_lock);
	optics = pon_optics_find(fwnode);
	if (!optics || !device_is_bound(optics->dev)) {
		mutex_unlock(&pon_optics_lock);
		return ERR_PTR(-EPROBE_DEFER);
	}
	dev = get_device(optics->dev);
	mutex_unlock(&pon_optics_lock);

	if (!device_link_add(consumer, dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(dev);
		return ERR_PTR(-EINVAL);
	}
	err = devm_add_action_or_reset(consumer, pon_optics_release, dev);
	if (err)
		return ERR_PTR(err);
	return optics;
}
EXPORT_SYMBOL_GPL(devm_pon_optics_get);
