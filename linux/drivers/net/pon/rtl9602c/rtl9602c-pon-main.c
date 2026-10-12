// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C GPON MAC: the driver's start and end, and its operations
 * for the switch driver (<linux/platform_data/rtl9602c-pon.h>).
 *
 * The driver gets the switch's register window, its interrupt and the
 * services of the host operations through the platform data, and registers
 * the ONU with the PON class: its OMCI port, and its data path with the GEM
 * ports.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/auxiliary_bus.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/pon.h>
#include <net/dsa.h>

#include "rtl9602c-pon.h"

static int rtl9602c_pon_op_start(void *pon)
{
	return rtl9602c_gpon_start(pon);
}

static void rtl9602c_pon_op_stop(void *pon)
{
	rtl9602c_gpon_stop(pon);
}

static void rtl9602c_pon_op_operstate(void *pon)
{
	rtl9602c_gpon_operstate(pon);
}

static void rtl9602c_pon_op_irq(void *pon)
{
	rtl9602c_gpon_irq(pon);
}

static bool rtl9602c_pon_op_is_up(void *pon)
{
	struct rtl9602c_pon *p = pon;

	return p->gpon_up;
}

static int rtl9602c_pon_op_sn_set(void *pon, const char *s)
{
	return rtl9602c_gpon_sn_set(pon, s);
}

/*
 * The serial number: empty until set, then its first four octets as
 * characters and the other four in hex (ITU-T G.984.3 clause 9.2.4.1).
 */
static int rtl9602c_pon_op_sn_get(void *pon, char *buf, size_t len)
{
	struct rtl9602c_pon *p = pon;

	if (!p->gpon_have_sn)
		return -ENODATA;
	snprintf(buf, len, "%.4s%02X%02X%02X%02X", p->gpon_sn, p->gpon_sn[4],
		 p->gpon_sn[5], p->gpon_sn[6], p->gpon_sn[7]);
	return 0;
}

static int rtl9602c_pon_op_password_set(void *pon, const char *s)
{
	return rtl9602c_gpon_password_set(pon, s);
}

static bool rtl9602c_pon_op_password_is_set(void *pon)
{
	struct rtl9602c_pon *p = pon;

	return p->gpon_have_password;
}

static bool rtl9602c_pon_op_us_ready(void *pon, unsigned int flow)
{
	return rtl9602c_gem_us_ready(pon, flow);
}

static u8 rtl9602c_pon_op_power_mode(void *pon)
{
	struct rtl9602c_pon *p = pon;
	u8 mode;

	mutex_lock(&p->gpon_lock);
	mode = p->gpon_power_mode;
	mutex_unlock(&p->gpon_lock);
	return mode;
}

static void rtl9602c_pon_op_power_applied(void *pon, u8 asked, u8 mode, int err)
{
	rtl9602c_gpon_power_applied(pon, asked, mode, err);
}

static const struct rtl9602c_pon_ops rtl9602c_pon_ops = {
	.start		= rtl9602c_pon_op_start,
	.stop		= rtl9602c_pon_op_stop,
	.operstate	= rtl9602c_pon_op_operstate,
	.irq		= rtl9602c_pon_op_irq,
	.is_up		= rtl9602c_pon_op_is_up,
	.sn_set		= rtl9602c_pon_op_sn_set,
	.sn_get		= rtl9602c_pon_op_sn_get,
	.password_set	= rtl9602c_pon_op_password_set,
	.password_is_set = rtl9602c_pon_op_password_is_set,
	.us_ready	= rtl9602c_pon_op_us_ready,
	.power_mode	= rtl9602c_pon_op_power_mode,
	.power_applied	= rtl9602c_pon_op_power_applied,
};

/**
 * rtl9602c_pon_probe() - start the driver of the GPON MAC
 * @pd: the platform data
 * @dev: the device the driver is bound to
 *
 * Finds what the PON MAC needs outside the switch, arranges the dying gasp
 * before a restart, and registers the OMCI port and the data path with the PON
 * class; neither is registered without the PON MAC's system controller and
 * buffers, or without the CPU port's conduit. Last, makes the driver's
 * operations available to the switch driver.
 *
 * Return: 0, or a negative error code, with nothing started.
 */
int rtl9602c_pon_probe(struct rtl9602c_pon_data *pd, struct device *dev)
{
	struct rtl9602c_pon *p;
	int err;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = dev;
	p->pd = pd;
	p->ds = pd->ds;
	p->reg = pd->reg;
	memset(p->gpon_tcont_map, 0xff, sizeof(p->gpon_tcont_map));
	INIT_DELAYED_WORK(&p->gpon_work, rtl9602c_gpon_work);
	mutex_init(&p->ploam_lock);
	spin_lock_init(&p->ploam_log_lock);
	mutex_init(&p->gpon_lock);

	err = rtl9602c_ponmac_probe(p);
	if (err)
		return dev_err_probe(dev, err,
				     "cannot reach the system controller\n");

	err = rtl9602c_gpon_reboot_register(p);
	if (err)
		return dev_err_probe(dev, err,
				     "cannot register the dying gasp\n");

	rtl9602c_gpon_report(p);
	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		rtl9602c_gpon_debugfs_init(p);
		rtl9602c_ponmac_debugfs_init(p);
	}

	/*
	 * The OMCI port first, then the GEM data path, which registers on the
	 * same PON device.
	 */
	err = rtl9602c_omci_setup(p);
	if (err)
		pr_warn("no OMCI channel: %pe\n", ERR_PTR(err));

	err = rtl9602c_gem_setup(p);
	if (err)
		pr_warn("no GEM data path: %pe\n", ERR_PTR(err));

	down_write(&pd->sem);
	pd->pon = p;
	pd->pon_ops = &rtl9602c_pon_ops;
	up_write(&pd->sem);
	return 0;
}

/**
 * rtl9602c_pon_remove() - stop the driver of the GPON MAC
 * @pd: the platform data
 *
 * Takes the driver's operations away from the switch driver, which waits for
 * a call in progress, takes the MAC down if it is up, so that the laser is
 * dark and the PON interface has no carrier, then cancels the worker and
 * unregisters the data path before the OMCI port: the PON device is
 * unregistered with its last child, the OMCI port, and the data path must not
 * outlive it. The ONU's serial number and password go with the driver's
 * state: after a bind they are set again, and the PON interface, which stays
 * up, is cycled to bring the MAC up.
 */
void rtl9602c_pon_remove(struct rtl9602c_pon_data *pd)
{
	struct rtl9602c_pon *p = pd->pon;

	if (!p)
		return;

	down_write(&pd->sem);
	pd->pon = NULL;
	pd->pon_ops = NULL;
	up_write(&pd->sem);

	rtl9602c_pon_debugfs_exit(p);

	rtl9602c_gpon_stop(p);
	rtl9602c_gpon_operstate(p);

	/*
	 * Nothing reads the GTC latches any more, so the source must not
	 * assert: the switch driver masks it too if it fires meanwhile.
	 */
	RTL9602C_HOST(p, irq_enable, RTL9602C_IRQ_SRC_GPON, 0);

	/* With no interrupt the worker is not cancelled by the bring-down. */
	cancel_delayed_work_sync(&p->gpon_work);
	rtl9602c_gem_teardown(p);
	rtl9602c_omci_teardown(p);
}

static int rtl9602c_pon_aux_probe(struct auxiliary_device *adev,
				  const struct auxiliary_device_id *id)
{
	return rtl9602c_pon_probe(dev_get_platdata(&adev->dev), &adev->dev);
}

static void rtl9602c_pon_aux_remove(struct auxiliary_device *adev)
{
	rtl9602c_pon_remove(dev_get_platdata(&adev->dev));
}

/* The switch driver makes the device, whose module is rtl9602c. */
static const struct auxiliary_device_id rtl9602c_pon_id_table[] = {
	{ .name = "rtl9602c.pon" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, rtl9602c_pon_id_table);

static struct auxiliary_driver rtl9602c_pon_driver = {
	.probe		= rtl9602c_pon_aux_probe,
	.remove		= rtl9602c_pon_aux_remove,
	.id_table	= rtl9602c_pon_id_table,
};
module_auxiliary_driver(rtl9602c_pon_driver);

MODULE_DESCRIPTION("GPON MAC of the Realtek RTL9602C");
MODULE_AUTHOR("Paweł Phan <szop.hektar_1w@icloud.com>");
MODULE_LICENSE("GPL");
