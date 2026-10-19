// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9602C switch: the optics' I2C master, who may write the optics, and the
 * SFP slave
 *
 * The transmitter and the receiver are run by a laser driver IC that serves
 * the SFF-8472 management interface, A0h at 0x50 and A2h at 0x51, and has a
 * driver of its own (drivers/net/pon/gn25l95.c), described in the switch
 * node's i2c node. The SoC has two I2C masters with the same set of
 * registers; the part answers on master 1 only (through master 0 its
 * addresses read zeros). The master has a driver of its own, i2c-rtl9602c,
 * which registers it as an SMBus adapter once the switch driver has made its
 * device (rtl9602c_optics_i2c_add()), with the children of the i2c node.
 *
 * What stays here is who may write the part. A0h and A2h bytes 0-127 take
 * writes without a password and keep them across a power cycle of the
 * module, so a write there is a permanent change to it. The adapter passes a
 * write on only once this file has identified the module as one the laser
 * driver's code was measured on (rtl9602c_optics_identify()), and never with
 * CONFIG_RTL9602C_OPTICS_RO; every other write is refused with -EPERM, from
 * the laser driver and from userspace through i2c-dev alike, and the laser
 * driver then only reads the part. A header that cannot be read is read
 * again every RTL9602C_OPTICS_ID_RETRY_S seconds, the writes refused until it
 * reads.
 *
 * The file also offers the debugfs view "sfpslave" of the SoC's SFP slave.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "rtl9602c.h"

/* How often a header that could not be read is read again. */
#define RTL9602C_OPTICS_ID_RETRY_S	5

/*
 * The optics' bus is the adapter that i2c-rtl9602c registers, reached by the
 * number it is registered under, since it may come after this driver and go
 * before it. The identification holds the optics lock for its reads and the
 * adapter's bus lock inside it. Without the adapter every read fails with
 * -ENODEV.
 */
static void rtl9602c_i2c_lock(struct rtl9602c_priv *p)
{
	struct i2c_adapter *adap = NULL;

	mutex_lock(&p->optics_lock);
	if (p->i2c_nr >= 0)
		adap = i2c_get_adapter(p->i2c_nr);
	if (adap)
		i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	p->i2c_adap = adap;
}

static void rtl9602c_i2c_unlock(struct rtl9602c_priv *p)
{
	struct i2c_adapter *adap = p->i2c_adap;

	p->i2c_adap = NULL;
	if (adap) {
		i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
		i2c_put_adapter(adap);
	}
	mutex_unlock(&p->optics_lock);
}

/* Read A0h byte @reg, under rtl9602c_i2c_lock(). */
static int rtl9602c_optics_read(struct rtl9602c_priv *p, u8 reg, u8 *val)
{
	union i2c_smbus_data data;
	int err;

	lockdep_assert_held(&p->optics_lock);
	if (!p->i2c_adap)
		return -ENODEV;
	err = __i2c_smbus_xfer(p->i2c_adap, 0x50, 0, I2C_SMBUS_READ, reg,
			       I2C_SMBUS_BYTE_DATA, &data);
	if (!err)
		*val = data.byte;
	return err;
}

/* Whether the adapter may pass a write on: the module is one the switch knows. */
static bool rtl9602c_optics_writable(void *priv)
{
	struct rtl9602c_priv *p = priv;

	return !IS_ENABLED(CONFIG_RTL9602C_OPTICS_RO) && READ_ONCE(p->optics_known);
}

/*
 * The modules whose laser driver may be written: those the laser driver's
 * code was measured on, by the vendor name (A0h bytes 20-35) and the start of
 * the vendor part number (A0h bytes 40-55) they declare, the header the host
 * reads through the SFP cage.  The header describes the module, not its laser
 * driver, for which no identification register is known; the module it names
 * was measured with the laser driver the code describes.  The header can be
 * rewritten: a module whose header was changed is taken for an unknown one.
 */
static const struct {
	const char *name;
	const char *part;
} rtl9602c_optics_modules[] = {
	{ "ODI", "DFP-34X-2C" },
};

/* Read the @len-byte space-padded ASCII field at A0h byte @reg into @out. */
static int rtl9602c_optics_field(struct rtl9602c_priv *p, u8 reg, char *out,
				 size_t len)
{
	size_t i;
	u8 c;
	int err;

	for (i = 0; i < len; i++) {
		err = rtl9602c_optics_read(p, reg + i, &c);
		if (err)
			return err;
		out[i] = c >= 0x20 && c < 0x7f ? c : '?';
	}
	out[len] = '\0';
	strim(out);
	return 0;
}

/**
 * rtl9602c_optics_identify() - tell whether the laser driver may be written
 * @p: driver state
 *
 * Reads the vendor name and part number the module declares and sets
 * &rtl9602c_priv.optics_known when they are those of a module in
 * rtl9602c_optics_modules.  For any other module, and while the header
 * cannot be read, the adapter refuses writes, as with
 * CONFIG_RTL9602C_OPTICS_RO.  A failed read is logged once.
 *
 * Return: true when the header was read, whatever it names.
 */
static bool rtl9602c_optics_identify(struct rtl9602c_priv *p)
{
	char name[17], part[17];
	unsigned int i;
	int err;

	rtl9602c_i2c_lock(p);
	err = rtl9602c_optics_field(p, 20, name, 16) ?:
	      rtl9602c_optics_field(p, 40, part, 16);
	if (err) {
		if (!p->optics_unread)
			pr_warn("optics: the module's header could not be read (%pe); writes to the laser driver are refused until it reads\n",
				ERR_PTR(err));
		p->optics_unread = true;
	} else {
		p->optics_unread = false;
	}
	rtl9602c_i2c_unlock(p);
	if (err)
		return false;

	for (i = 0; i < ARRAY_SIZE(rtl9602c_optics_modules); i++) {
		if (!strcmp(name, rtl9602c_optics_modules[i].name) &&
		    strstarts(part, rtl9602c_optics_modules[i].part)) {
			WRITE_ONCE(p->optics_known, true);
			return true;
		}
	}
	pr_info("optics: module \"%s\" \"%s\" is not one the laser driver was measured on; writes to it are refused\n",
		name, part);
	return true;
}

/* Read a header that could not be read, until it reads. */
static void rtl9602c_optics_id_work(struct work_struct *work)
{
	struct rtl9602c_priv *p = container_of(to_delayed_work(work),
					       struct rtl9602c_priv,
					       optics_id_work);

	if (!rtl9602c_optics_identify(p))
		schedule_delayed_work(&p->optics_id_work,
				      RTL9602C_OPTICS_ID_RETRY_S * HZ);
}

/*
 * The master's driver registers its adapter, or is about to take it away.
 * The module is identified from the first moment the adapter is there.
 */
static void rtl9602c_optics_adapter(void *priv, struct i2c_adapter *adap)
{
	struct rtl9602c_priv *p = priv;

	if (!adap)
		cancel_delayed_work_sync(&p->optics_id_work);

	mutex_lock(&p->optics_lock);
	p->i2c_nr = adap ? adap->nr : -1;
	mutex_unlock(&p->optics_lock);

	if (adap && !rtl9602c_optics_identify(p))
		schedule_delayed_work(&p->optics_id_work,
				      RTL9602C_OPTICS_ID_RETRY_S * HZ);
}

/**
 * rtl9602c_optics_i2c_add() - hand the optics' I2C master to its driver
 * @p: driver state
 *
 * Makes the auxiliary device "rtl9602c.i2c" that i2c-rtl9602c binds to, with
 * the switch's register window, which holds the master's registers. The
 * adapter comes when that driver is there, which may be after this call
 * (rtl9602c_optics_adapter()).
 *
 * Then gives the TX_DISABLE pad to its GPIO function, when the optics the
 * switch node names have a tx-disable GPIO: gpiolib owns the line's direction
 * and level but not the pad's function, which is in the switch register
 * window. The optics' driver has requested the line asserted by then, if the
 * master's driver was there to register its device.  With the bit set the
 * line acts (measured); whether the pad needs it is not verified.
 *
 * Return: 0 or a negative error code.
 */
int rtl9602c_optics_i2c_add(struct rtl9602c_priv *p)
{
	struct auxiliary_device *adev;
	struct device_node *np;

	INIT_DELAYED_WORK(&p->optics_id_work, rtl9602c_optics_id_work);
	p->i2c.reg = p->reg;
	p->i2c.priv = p;
	p->i2c.writable = rtl9602c_optics_writable;
	p->i2c.adapter = rtl9602c_optics_adapter;
	adev = devm_auxiliary_device_create(p->dev, "i2c", &p->i2c);
	if (!adev)
		return -ENODEV;

	np = of_parse_phandle(p->dev->of_node, "realtek,pon-optics", 0);
	if (np && of_property_present(np, "tx-disable-gpios"))
		rtl9602c_rmw(p, RTL9602C_PADMUX_GPIO_EN, 0,
			     BIT(RTL9602C_PADMUX_GPIO_EN_TX_DISABLE));
	of_node_put(np);
	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * The SoC's SFP slave, read and nothing else: whether the slave is enabled
 * and how, and its RAM, A0h then A2h, as the host would be served it. Whether
 * the host's 0x50/0x51 is this RAM or a separate EEPROM is not known; this
 * view and the host's own read of the cage tell. A byte whose read does not
 * complete ends the dump.
 */
static int rtl9602c_sfpslave_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 aux = rtl9602c_r(p, RTL9602C_SFPSLV_AUX);
	u32 ctl = rtl9602c_r(p, RTL9602C_SFPSLV_CTL);
	u8 ram[RTL9602C_SFPSLV_RAM_SIZE];
	unsigned int a, n;
	u32 v = 0;

	seq_printf(s, "aux %08x  ctl %08x\n", aux, ctl);
	seq_printf(s, "slave %s, standard mode %lu, byte mode %lu, burst read %lu, endian %lu, open drain %lu, SDA delay %lu, TX_DISABLE control %lu (inverted %lu)\n",
		   ctl & RTL9602C_SFPSLV_CTL_ENABLE ? "enabled" : "disabled",
		   FIELD_GET(RTL9602C_SFPSLV_CTL_STANDARD, ctl),
		   FIELD_GET(RTL9602C_SFPSLV_CTL_BYTES, ctl),
		   FIELD_GET(RTL9602C_SFPSLV_CTL_BURST, ctl),
		   FIELD_GET(RTL9602C_SFPSLV_CTL_BYTE_ORDER, ctl),
		   FIELD_GET(RTL9602C_SFPSLV_CTL_OPEN_DRAIN, ctl),
		   FIELD_GET(RTL9602C_SFPSLV_CTL_SDA_DELAY, ctl),
		   FIELD_GET(RTL9602C_SFPSLV_CTL_TXDIS, ctl),
		   FIELD_GET(RTL9602C_SFPSLV_CTL_TXDIS_INVERT, ctl));

	for (a = 0; a < RTL9602C_SFPSLV_RAM_SIZE; a++) {
		rtl9602c_w(p, RTL9602C_SFPSLV_RAM_ACC, RTL9602C_SFPSLV_RAM_ACC_GO |
			   FIELD_PREP(RTL9602C_SFPSLV_RAM_ACC_ADDR, a));
		for (n = 0; n < 1000; n++) {
			v = rtl9602c_r(p, RTL9602C_SFPSLV_RAM_ACC);
			if (!(v & RTL9602C_SFPSLV_RAM_ACC_GO))
				break;
			udelay(1);
		}
		if (v & RTL9602C_SFPSLV_RAM_ACC_GO) {
			seq_printf(s, "byte %u: the read did not complete (%08x)\n", a, v);
			break;
		}
		ram[a] = FIELD_GET(RTL9602C_SFPSLV_RAM_ACC_DATA, v);
	}

	seq_puts(s, "A0h\n");
	seq_hex_dump(s, "", DUMP_PREFIX_OFFSET, 16, 1, ram, min(a, 256u), false);
	if (a > 256) {
		seq_puts(s, "A2h\n");
		seq_hex_dump(s, "", DUMP_PREFIX_OFFSET, 16, 1, ram + 256, a - 256, false);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_sfpslave);

/**
 * rtl9602c_optics_debugfs_init() - create the SFP slave's debugfs view
 * @p: driver state
 *
 * Creates "sfpslave", mode 0400, in the switch's debugfs directory. It only
 * reads, so a CONFIG_RTL9602C_OPTICS_RO build has it too. The laser driver's
 * views are in its own driver's directory.
 */
void rtl9602c_optics_debugfs_init(struct rtl9602c_priv *p)
{
	debugfs_create_file("sfpslave", 0400, p->debugfs, p,
			    &rtl9602c_sfpslave_fops);
}

#endif
