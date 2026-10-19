// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9602C switch: the optics' diagnostics, laser levelling and TX_DISABLE
 *
 * The transmitter and the receiver are run by a laser driver IC that
 * serves the SFF-8472 management interface, A0h at 0x50 and A2h at 0x51.
 * The SoC has two I2C masters with the same set of registers; the part
 * answers on master 1 only (through master 0 its addresses read zeros).
 * The master has a driver of its own, i2c-rtl9602c, which registers it as an
 * SMBus adapter once the switch driver has made its device
 * (rtl9602c_optics_i2c_add()). The part runs from the configuration and
 * calibration in its own EEPROM, and the driver writes nothing to it at
 * probe.
 *
 * This file moves bytes over that adapter, one per transfer or the two of a
 * diagnostic word together, publishes the SFF-8472 diagnostics through hwmon,
 * drives the laser's TX_DISABLE line, turns the part's automatic
 * extinction-ratio loop off at every PON bring-up
 * (rtl9602c_optics_hold_modulation()), and offers two debugfs views that
 * only read, "a2" and "eeblocks", a third, "tables", that selects each table
 * to read it, and, with CONFIG_NET_DSA_RTL9602C_OPTICS_STORE, "t2write",
 * which stores table 2 bytes in the module's EEPROM on request.
 *
 * CONFIG_RTL9602C_OPTICS_RO makes the modulation hold, the driver's only
 * write to the part of its own accord, return without writing, and the adapter
 * refuse every write with -EPERM, so that nothing on the board changes the
 * module. The hwmon device and the debugfs views stay as they are, since they
 * only read. A module that does not declare itself as one this file was
 * measured on gets the same treatment, without the build option
 * (rtl9602c_optics_identify()).
 *
 * The hwmon reads, the debugfs views, the modulation hold and clients of
 * the adapter all drive the same registers, and the hwmon core and the I2C
 * core each serialize their own callers only. So every user of this file
 * holds the optics lock for its whole sequence and the adapter's bus lock
 * inside it (rtl9602c_i2c_lock()): the modulation hold switches the page of
 * A2h 128-255 and puts it back, and no other user, a client of the adapter
 * included, sees the switched page.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <net/ipv6.h>
#include <linux/dcbnl.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/if_bridge.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/swab.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "rtl9602c.h"

/*
 * The optics' bus is the adapter that i2c-rtl9602c registers, reached by the
 * number it is registered under, since it may come after this driver and go
 * before it. A user holds the optics lock for a whole sequence, and the
 * adapter's bus lock inside it, so that a client of the adapter, such as
 * i2c-dev, cannot put a transfer between two of the sequence's. Without the
 * adapter the sequence runs all the same, under the optics lock, and every
 * transfer in it fails with -ENODEV.
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

DEFINE_GUARD(rtl9602c_i2c, struct rtl9602c_priv *, rtl9602c_i2c_lock(_T),
	     rtl9602c_i2c_unlock(_T))

/* One SMBus transfer on the optics' bus, in a sequence under rtl9602c_i2c_lock(). */
static int rtl9602c_i2c_xfer(struct rtl9602c_priv *p, u16 slave, char rw, u8 reg,
			     int size, union i2c_smbus_data *data)
{
	lockdep_assert_held(&p->optics_lock);
	if (!p->i2c_adap)
		return -ENODEV;
	return __i2c_smbus_xfer(p->i2c_adap, slave, 0, rw, reg, size, data);
}

/**
 * rtl9602c_i2c_read_byte() - read one register of a device on the optics bus
 * @p: driver state
 * @slave: 7-bit device address
 * @reg: register address inside the device
 * @val: set to the byte read
 *
 * Busy-waits for up to about two milliseconds. Called in a sequence under
 * rtl9602c_i2c_lock(): see the note at the top of this file.
 *
 * Return: 0, -ETIMEDOUT if the transfer does not end, -ENXIO if the device
 * does not acknowledge, or -ENODEV without the adapter.
 */
static int rtl9602c_i2c_read_byte(struct rtl9602c_priv *p, u16 slave, u8 reg,
				  u8 *val)
{
	union i2c_smbus_data data;
	int err;

	err = rtl9602c_i2c_xfer(p, slave, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &data);
	if (!err)
		*val = data.byte;
	return err;
}

static int rtl9602c_i2c_write_byte(struct rtl9602c_priv *p, u16 slave, u8 reg,
				   u8 val)
{
	union i2c_smbus_data data = { .byte = val };

	return rtl9602c_i2c_xfer(p, slave, I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &data);
}

/* Whether the adapter may pass on the write of a client: the module is one it knows. */
static bool rtl9602c_optics_writable(void *priv)
{
	struct rtl9602c_priv *p = priv;

	return !IS_ENABLED(CONFIG_RTL9602C_OPTICS_RO) && READ_ONCE(p->optics_known);
}

static void rtl9602c_optics_identify(struct rtl9602c_priv *p);
static int rtl9602c_optics_hwmon_register(struct rtl9602c_priv *p);

/*
 * The master's driver registers its adapter, or is about to take it away.
 * The module is identified, and the diagnostics published, from the first
 * moment the adapter is there.
 */
static void rtl9602c_optics_adapter(void *priv, struct i2c_adapter *adap)
{
	struct rtl9602c_priv *p = priv;
	int err;

	mutex_lock(&p->optics_lock);
	p->i2c_nr = adap ? adap->nr : -1;
	mutex_unlock(&p->optics_lock);
	if (!adap)
		return;

	rtl9602c_optics_identify(p);
	err = rtl9602c_optics_hwmon_register(p);
	if (err)
		pr_warn("no optical diagnostics: %pe\n", ERR_PTR(err));
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
 * Return: 0 or a negative error code.
 */
int rtl9602c_optics_i2c_add(struct rtl9602c_priv *p)
{
	struct auxiliary_device *adev;

	p->i2c.reg = p->reg;
	p->i2c.priv = p;
	p->i2c.writable = rtl9602c_optics_writable;
	p->i2c.adapter = rtl9602c_optics_adapter;
	adev = devm_auxiliary_device_create(p->dev, "i2c", &p->i2c);
	return adev ? 0 : -ENODEV;
}

/*
 * The rtl9602c_l_*() helpers number the bytes of the part's management
 * interface 0x000-0x1ff: 0x000-0x0ff are A0h bytes 0-255 (device 0x50),
 * 0x100-0x1ff are A2h bytes 0-255 (device 0x51; RTL9602C_OPTICS_A2_BASE).
 */
static int rtl9602c_l_addr(u16 reg, u16 *slave, u8 *sub)
{
	static const u8 dev[2] = { 0x50, 0x51 };

	if (reg >= 0x200)
		return -EINVAL;
	*slave = dev[reg >> 8];
	*sub = reg & 0xff;
	return 0;
}

/**
 * rtl9602c_l_read() - read one byte of the optics management interface
 * @p: driver state
 * @reg: 0x000-0x0ff for A0h bytes 0-255, 0x100-0x1ff for A2h bytes 0-255
 * @val: set to the byte read
 *
 * Return: 0, -EINVAL for a @reg above 0x1ff, or an error from the transfer.
 */
int rtl9602c_l_read(struct rtl9602c_priv *p, u16 reg, u8 *val)
{
	u16 slave;
	u8 sub;
	int err;

	err = rtl9602c_l_addr(reg, &slave, &sub);
	if (err)
		return err;
	return rtl9602c_i2c_read_byte(p, slave, sub, val);
}

/*
 * The modules whose laser driver this file writes: those it was measured on,
 * by the vendor name (A0h bytes 20-35) and the start of the vendor part
 * number (A0h bytes 40-55) they declare, the header the host reads through
 * the SFP cage.  The header describes the module, not its laser driver, for
 * which no identification register is known; the module it names was
 * measured with the laser driver this file describes.  The header can be
 * rewritten: a module whose header was changed is taken for an unknown one.
 */
static const struct {
	const char *name;
	const char *part;
} rtl9602c_optics_modules[] = {
	{ "ODI", "DFP-34X-2C" },
};

/* Read the @len-byte space-padded ASCII field at @reg into @out. */
static int rtl9602c_optics_field(struct rtl9602c_priv *p, u16 reg, char *out,
				 size_t len)
{
	size_t i;
	u8 c;
	int err;

	for (i = 0; i < len; i++) {
		err = rtl9602c_l_read(p, reg + i, &c);
		if (err)
			return err;
		out[i] = c >= 0x20 && c < 0x7f ? c : '?';
	}
	out[len] = '\0';
	strim(out);
	return 0;
}

/**
 * rtl9602c_optics_identify() - tell whether the driver may write the laser driver
 * @p: driver state
 *
 * Reads the vendor name and part number the module declares and sets
 * &rtl9602c_priv.optics_known when they are those of a module in
 * rtl9602c_optics_modules.  For any other module, or when the header
 * cannot be read, the driver only reads the laser driver: the modulation
 * hold writes nothing and the adapter refuses writes, as with
 * CONFIG_RTL9602C_OPTICS_RO.  Called once, after the master is enabled.
 */
static void rtl9602c_optics_identify(struct rtl9602c_priv *p)
{
	char name[17], part[17];
	unsigned int i;
	int err;

	rtl9602c_i2c_lock(p);
	err = rtl9602c_optics_field(p, 20, name, 16) ?:
	      rtl9602c_optics_field(p, 40, part, 16);
	rtl9602c_i2c_unlock(p);
	if (err) {
		pr_warn("optics: the module's header could not be read (%pe); the laser driver is only read\n",
			ERR_PTR(err));
		return;
	}

	for (i = 0; i < ARRAY_SIZE(rtl9602c_optics_modules); i++) {
		if (!strcmp(name, rtl9602c_optics_modules[i].name) &&
		    strstarts(part, rtl9602c_optics_modules[i].part)) {
			WRITE_ONCE(p->optics_known, true);
			return;
		}
	}
	pr_info("optics: module \"%s\" \"%s\" is not one the driver was measured on; the laser driver is only read\n",
		name, part);
}

/*
 * A0h and A2h bytes 0-127 take writes without a password and keep them
 * across a power cycle of the module: a write there is a permanent change
 * to it.  Of those bytes the driver writes the page select, 127, through
 * rtl9602c_l_write_changed(), and only the store view writes the password,
 * 123-126, which reads back 00 whatever is written (measured on this board).
 */
static int rtl9602c_l_write(struct rtl9602c_priv *p, u16 reg, u8 val)
{
	u16 slave;
	u8 sub;
	int err;

	if (!p->optics_known)
		return -EPERM;
	err = rtl9602c_l_addr(reg, &slave, &sub);
	if (err)
		return err;
	return rtl9602c_i2c_write_byte(p, slave, sub, val);
}

/*
 * Write a byte only when it holds another value, so that a byte kept in the
 * module's EEPROM, such as the page select that every bring-up sets and puts
 * back, is written only when it changes.
 */
static int rtl9602c_l_write_changed(struct rtl9602c_priv *p, u16 reg, u8 val)
{
	u8 now;
	int err;

	err = rtl9602c_l_read(p, reg, &now);
	if (err || now == val)
		return err;
	return rtl9602c_l_write(p, reg, val);
}

/**
 * rtl9602c_l_txdis_gpio() - drive the laser's TX_DISABLE line
 * @p: driver state
 * @disable: true to assert TX_DISABLE (no light), false to release it
 *
 * The switch requests the line asserted at probe.  The GPON code asserts
 * it in every activation state below O3 and releases it from O3 on, where
 * an ONU first transmits (ITU-T G.984.3 10.2); the GPON MAC still keys
 * each burst.  The line is optional: without a tx-disable GPIO in the
 * device tree this does nothing.  Asserting it stops the light: in O5 the
 * module's bias and transmitted power read 0 within a second, and the OLT
 * deactivated the ONU (measured on this board).
 *
 * The soft TX disable of SFF-8472 (A2h byte 110, bit 6) is not used: A2h
 * bytes 0-127 are kept in the module's EEPROM, so every change would be an
 * EEPROM write.
 *
 * Context: Process context.  May sleep.
 * Return: 0.
 */
int rtl9602c_l_txdis_gpio(struct rtl9602c_priv *p, bool disable)
{
	if (!p->tx_disable)
		return 0;

	/*
	 * gpiolib owns the line's direction and level but not its pad
	 * function, which is in the switch register window: give the pad to
	 * its GPIO function first.  Bit 13 is also the line's GPIO number;
	 * only that bit changes, and setting it again is harmless.  With the
	 * bit set the line acts (measured); whether the pad needs it is not
	 * verified.
	 */
	rtl9602c_rmw(p, RTL9602C_PADMUX_GPIO_EN, 0, BIT(RTL9602C_PADMUX_GPIO_EN_TX_DISABLE));
	gpiod_set_value_cansleep(p->tx_disable, disable);
	return 0;
}

/*
 * Read the word at A2h byte @a2, most significant byte first, in one
 * transfer of two bytes: SFF-8472 has a diagnostic read in one two-byte
 * sequence, during which the module does not update it.
 */
static int rtl9602c_l_a2_word(struct rtl9602c_priv *p, unsigned int a2, u16 *val)
{
	union i2c_smbus_data data;
	u16 slave;
	u8 sub;
	int err;

	err = rtl9602c_l_addr(RTL9602C_OPTICS_A2_BASE + a2, &slave, &sub);
	if (err)
		return err;
	/* The first byte read, the most significant, is the SMBus word's low byte. */
	err = rtl9602c_i2c_xfer(p, slave, I2C_SMBUS_READ, sub, I2C_SMBUS_WORD_DATA, &data);
	if (err)
		return err;
	*val = swab16(data.word);
	return 0;
}

#if IS_ENABLED(CONFIG_HWMON)

/*
 * A2h 96-105 hold temperature, supply voltage, bias, transmitted power and
 * received power in the units SFF-8472 gives an internally calibrated
 * module (signed 1/256 degC, 100 uV, 2 uA, 0.1 uW), converted as
 * drivers/net/phy/sfp.c converts them and rounded to the nearest.  A0h
 * byte 92, which says how a module is calibrated, is not read and no
 * external calibration is applied: the module fitted here holds in A2h
 * 56-91 the constants SFF-8472 gives an internally calibrated module
 * (measured on this board), so both readings agree.  Bias and transmitted
 * power read 0 while the laser does not emit, as below O3.
 *
 * The thresholds come from A2h 0-39, in the same units, and are reported
 * as sfp.c reports them: the alarm thresholds as crit and lcrit, the
 * warning thresholds as max and min.  The module fitted here holds values
 * there that its own readings sit well within (measured on this board).
 *
 * A value is computed even when its read fails; the error returned keeps
 * the hwmon core from using it.
 */
static int rtl9602c_hwmon_read(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, long *val)
{
	struct rtl9602c_priv *p = dev_get_drvdata(dev);
	enum rtl9602c_optics_ddm ddm;
	unsigned int a2;
	int limit;
	u16 raw;
	int err;

	switch (type) {
	case hwmon_temp:
		ddm = RTL9602C_OPTICS_TEMP;
		limit = attr == hwmon_temp_crit ? RTL9602C_OPTICS_HIGH_ALARM :
			attr == hwmon_temp_lcrit ? RTL9602C_OPTICS_LOW_ALARM :
			attr == hwmon_temp_max ? RTL9602C_OPTICS_HIGH_WARN :
			attr == hwmon_temp_min ? RTL9602C_OPTICS_LOW_WARN : -1;
		break;
	case hwmon_in:
		ddm = RTL9602C_OPTICS_VCC;
		limit = attr == hwmon_in_crit ? RTL9602C_OPTICS_HIGH_ALARM :
			attr == hwmon_in_lcrit ? RTL9602C_OPTICS_LOW_ALARM :
			attr == hwmon_in_max ? RTL9602C_OPTICS_HIGH_WARN :
			attr == hwmon_in_min ? RTL9602C_OPTICS_LOW_WARN : -1;
		break;
	case hwmon_curr:
		ddm = RTL9602C_OPTICS_BIAS;
		limit = attr == hwmon_curr_crit ? RTL9602C_OPTICS_HIGH_ALARM :
			attr == hwmon_curr_lcrit ? RTL9602C_OPTICS_LOW_ALARM :
			attr == hwmon_curr_max ? RTL9602C_OPTICS_HIGH_WARN :
			attr == hwmon_curr_min ? RTL9602C_OPTICS_LOW_WARN : -1;
		break;
	case hwmon_power:
		ddm = channel ? RTL9602C_OPTICS_RX : RTL9602C_OPTICS_TX;
		limit = attr == hwmon_power_crit ? RTL9602C_OPTICS_HIGH_ALARM :
			attr == hwmon_power_lcrit ? RTL9602C_OPTICS_LOW_ALARM :
			attr == hwmon_power_max ? RTL9602C_OPTICS_HIGH_WARN :
			attr == hwmon_power_min ? RTL9602C_OPTICS_LOW_WARN : -1;
		break;
	default:
		return -EOPNOTSUPP;
	}

	a2 = limit < 0 ? RTL9602C_OPTICS_A2_WORD(ddm) :
			 RTL9602C_OPTICS_A2_LIMIT(ddm, limit);
	scoped_guard(rtl9602c_i2c, p)
		err = rtl9602c_l_a2_word(p, a2, &raw);

	switch (ddm) {
	case RTL9602C_OPTICS_TEMP:
		*val = DIV_ROUND_CLOSEST((long)(s16)raw * 1000, 256);
		break;
	case RTL9602C_OPTICS_VCC:
		*val = DIV_ROUND_CLOSEST((long)raw, 10);
		break;
	case RTL9602C_OPTICS_BIAS:
		*val = DIV_ROUND_CLOSEST((long)raw, 500);
		break;
	default:
		*val = DIV_ROUND_CLOSEST((long)raw, 10);
		break;
	}
	return err;
}

/* The labels drivers/net/phy/sfp.c gives the same readings. */
static int rtl9602c_hwmon_read_string(struct device *dev,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = "temperature";
		return 0;
	case hwmon_in:
		*str = "VCC";
		return 0;
	case hwmon_curr:
		*str = "bias";
		return 0;
	case hwmon_power:
		*str = channel ? "RX_power" : "TX_power";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t rtl9602c_hwmon_is_visible(const void *data,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	return 0444;
}

/*
 * temp1, in0, curr1, power1 (transmitted) and power2 (received), each with
 * its four thresholds
 */
static const struct hwmon_channel_info * const rtl9602c_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL |
			   HWMON_T_MIN | HWMON_T_MAX |
			   HWMON_T_LCRIT | HWMON_T_CRIT),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL |
			   HWMON_I_MIN | HWMON_I_MAX |
			   HWMON_I_LCRIT | HWMON_I_CRIT),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL |
			   HWMON_C_MIN | HWMON_C_MAX |
			   HWMON_C_LCRIT | HWMON_C_CRIT),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL |
			   HWMON_P_MIN | HWMON_P_MAX |
			   HWMON_P_LCRIT | HWMON_P_CRIT,
			   HWMON_P_INPUT | HWMON_P_LABEL |
			   HWMON_P_MIN | HWMON_P_MAX |
			   HWMON_P_LCRIT | HWMON_P_CRIT),
	NULL,
};

static const struct hwmon_ops rtl9602c_hwmon_ops = {
	.is_visible	= rtl9602c_hwmon_is_visible,
	.read		= rtl9602c_hwmon_read,
	.read_string	= rtl9602c_hwmon_read_string,
};

static const struct hwmon_chip_info rtl9602c_hwmon_chip = {
	.ops	= &rtl9602c_hwmon_ops,
	.info	= rtl9602c_hwmon_info,
};

/**
 * rtl9602c_optics_hwmon_register() - publish the optics' diagnostics
 * @p: driver state
 *
 * Registers the hwmon device "rtl9602c_optics" once one read of the
 * temperature word has succeeded, so that no device appears when nothing
 * answers at A2h.  It does nothing if the device is there already, as when
 * the master's driver comes back.  Without CONFIG_HWMON this is a stub that
 * returns 0.
 *
 * Return: 0, the error of the test read, or an error from the hwmon core.
 */
static int rtl9602c_optics_hwmon_register(struct rtl9602c_priv *p)
{
	struct device *hwmon;
	u16 probe;
	int err;

	if (p->optics_hwmon)
		return 0;
	scoped_guard(rtl9602c_i2c, p)
		err = rtl9602c_l_a2_word(p, RTL9602C_OPTICS_A2_WORD(RTL9602C_OPTICS_TEMP),
					 &probe);
	if (err)
		return err;

	hwmon = devm_hwmon_device_register_with_info(p->dev, "rtl9602c_optics",
						     p, &rtl9602c_hwmon_chip,
						     NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	p->optics_hwmon = hwmon;
	return 0;
}
#else
static int rtl9602c_optics_hwmon_register(struct rtl9602c_priv *p)
{
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * The "a2" view: A2h bytes 0-127, each read on its own, then the five
 * diagnostic words in SFF-8472 units, the password bytes 123-126 and the
 * page select, byte 127.  A failed read ends the view with its error.  It
 * only reads.
 */
static int rtl9602c_a2_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u8 b[0x80];
	unsigned int i;
	int err = 0;
	u16 w[5];
	int mdeg;

	rtl9602c_i2c_lock(p);
	for (i = 0; i < sizeof(b) && !err; i++)
		err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + i, &b[i]);
	for (i = 0; i < ARRAY_SIZE(w) && !err; i++)
		err = rtl9602c_l_a2_word(p, RTL9602C_OPTICS_A2_WORD(i), &w[i]);
	rtl9602c_i2c_unlock(p);
	if (err) {
		seq_printf(s, "A2h read failed: %d\n", err);
		return 0;
	}

	for (i = 0; i < sizeof(b); i += 16)
		seq_printf(s, "%02x: %*ph\n", i, 16, b + i);

	/*
	 * The five words are read again, each in a way that gives a sample the
	 * module held (rtl9602c_l_a2_word()), so they may be newer than the
	 * bytes above.  The received power shows whether light reaches the
	 * receiver, which the GPON MAC's loss of signal cannot tell: that alarm
	 * is the same for no light and for light the receiver does not recover.
	 */
	mdeg = DIV_ROUND_CLOSEST((int)(s16)w[0] * 1000, 256);
	seq_printf(s, "\ntemperature %s%d.%03d C\n", mdeg < 0 ? "-" : "",
		   abs(mdeg) / 1000, abs(mdeg) % 1000);
	seq_printf(s, "vcc         %u.%03u V\n", w[1] / 10000, (w[1] / 10) % 1000);
	seq_printf(s, "tx bias     %u uA\n", w[2] * 2);
	seq_printf(s, "tx power    %u.%u uW\n", w[3] / 10, w[3] % 10);
	seq_printf(s, "rx power    %u.%u uW   <- light on the fibre\n",
		   w[4] / 10, w[4] % 10);

	/*
	 * SFF-8472 leaves bytes 120-126 to the module maker.  The store view
	 * writes 0xff to 123-126, and on this module they read 00 whatever was
	 * written (measured on this board).
	 */
	seq_printf(s, "\npassword 0x7b-0x7e  %02x %02x %02x %02x\n",
		   b[RTL9602C_OPTICS_A2_PASSWORD], b[RTL9602C_OPTICS_A2_PASSWORD + 1],
		   b[RTL9602C_OPTICS_A2_PASSWORD + 2], b[RTL9602C_OPTICS_A2_PASSWORD + 3]);
	seq_printf(s, "table select 0x7f = 0x%02x\n", b[0x7f]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_a2);

#endif

/*
 * The laser driver's configuration table ("table 2"), shown on this module
 * at A2h 128-255 while the page select, A2h byte 127, holds 02h.  The
 * addresses are A2h byte numbers.  Measured on this board:
 * ER_CTRL_AUTO runs the automatic extinction-ratio loop, and ER_CTRL reads
 * 0xe3 after the module powers up.  MOD_DAC (16 bits, most significant
 * byte first) sets the modulation while the loop is off.  With both
 * PERSIST_OFF bits set, register changes are not copied into the module's
 * EEPROM; with PERSIST at 0x00 every change is copied, the diagnostic
 * words included, and the part does not acknowledge on the bus while a
 * copy runs.  This module reads 0x03 in PERSIST.
 *
 * Not verified: MOD_LIMIT as a limit on the modulation DAC, 0xff meaning
 * none.  MOD_RANGE only decides how the log shows the modulation.  The log
 * lines name the part GN25L95; the driver relies on the behaviour above,
 * not on that name, which is not verified either.
 */
#define RTL9602C_OPTICS_TBL2_ER_CTRL	0xa5
#define RTL9602C_OPTICS_TBL2_ER_CTRL_AUTO		BIT(0)
#define RTL9602C_OPTICS_TBL2_ER_CTRL_MOD_RANGE	GENMASK(3, 2)
#define RTL9602C_OPTICS_TBL2_APCSET		0xa8
#define RTL9602C_OPTICS_TBL2_MOD_DAC		0xab
#define RTL9602C_OPTICS_TBL2_MOD_DAC_FIELD	GENMASK(15, 6)
#define RTL9602C_OPTICS_TBL2_MOD_LIMIT		0xaf
#define RTL9602C_OPTICS_TBL2_PERSIST	0xc7
#define RTL9602C_OPTICS_TBL2_PERSIST_OFF	(BIT(1) | BIT(0))

/*
 * Prepare the shown configuration table for a write: if bit 3 or 2 of
 * table byte 0xcc is set, clear both and wait 400 ms; this module reads
 * 0xff there.  That the table needs this before it takes a write is not
 * verified.
 */
static int rtl9602c_gn_t2_prepare(struct rtl9602c_priv *p)
{
	u8 acc;
	int err;

	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0xcc, &acc);
	if (err || !(acc & (BIT(3) | BIT(2))))
		return err;
	err = rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + 0xcc,
			       acc & ~(BIT(3) | BIT(2)));
	if (!err)
		msleep(400);
	return err;
}

/*
 * Transmit power levelling (ITU-T G.984.2 clause 8.3.2): level mode 0 is the
 * power the module came with, modes 1 and 2 are 3 dB and 6 dB below it.  The
 * power control loop holds the power at which the monitor photodiode reaches
 * the set point, moving the bias, so a lower level takes a lower set point
 * and, for the light to keep its shape, a modulation lower by the same
 * factor; with the modulation held, the bias reaches its floor about 1.5 dB
 * down and the power stops following the set point (measured on this
 * board).  So the modulation is scaled first, by 2^-mode, and the set point
 * is then moved one step at a time, never above the module's own, until the
 * transmitted power has crossed 2^-mode times the power measured at mode 0.
 * The module measures the power of the bursts, which come often only from
 * O4 on, so a lower level is set only there.
 *
 * Going down, the scaled modulation first makes the loop hold the power with
 * more bias, about 21 mA at mode 2 here, which the set point then lowers;
 * going up, the modulation first lets the bias fall.  Either way the set
 * point moves last.
 */
#define RTL9602C_OPTICS_LEVEL_MODES	3
/* The power is read this long after each change. */
#define RTL9602C_OPTICS_LEVEL_SETTLE_MS	20
/* A change that needs more bias than this, in 2 uA, is given up. */
#define RTL9602C_OPTICS_LEVEL_BIAS_MAX	(40000 / 2)

/* The transmitted power and the bias, read RTL9602C_OPTICS_LEVEL_SETTLE_MS on. */
static int rtl9602c_optics_tx_bias(struct rtl9602c_priv *p, u16 *tx, u16 *bias)
{
	msleep(RTL9602C_OPTICS_LEVEL_SETTLE_MS);
	return rtl9602c_l_a2_word(p, RTL9602C_OPTICS_A2_WORD(RTL9602C_OPTICS_TX), tx) ?:
	       rtl9602c_l_a2_word(p, RTL9602C_OPTICS_A2_WORD(RTL9602C_OPTICS_BIAS), bias);
}

/*
 * Write MOD_DAC and read it back.  The register takes its two bytes together
 * when the less significant is written, the more significant one waiting
 * until then: written alone, it reads back unchanged (measured on this
 * board).  So the more significant byte goes first, and the modulation steps
 * from the old value to the new with nothing in between.
 */
static int rtl9602c_optics_put_mod(struct rtl9602c_priv *p, u16 mod)
{
	const u16 reg = RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_MOD_DAC;
	u8 hi, lo;
	int err;

	err = rtl9602c_l_write(p, reg, mod >> 8) ?:
	      rtl9602c_l_write(p, reg + 1, mod & 0xff) ?:
	      rtl9602c_l_read(p, reg, &hi) ?:
	      rtl9602c_l_read(p, reg + 1, &lo);
	if (!err && ((hi << 8) | lo) != mod)
		err = -EIO;
	return err;
}

/*
 * Set level mode @mode, with the configuration table shown and ready for
 * writes.  Mode 0 is the module's own modulation and set point, written in
 * that order.  A lower mode needs the module's settings known and the laser
 * sending bursts: the power at mode 0 is measured when the optics are still
 * at mode 0, and a power of 0 there gives -ENODATA.  A set point that would
 * pass the module's own, a power that falls to 0 or a bias above
 * RTL9602C_OPTICS_LEVEL_BIAS_MAX gives -ERANGE; the caller then puts mode 0
 * back.
 */
static int rtl9602c_optics_level_set(struct rtl9602c_priv *p, unsigned int mode)
{
	const u16 apc_reg = RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_APCSET;
	u16 dac = FIELD_GET(RTL9602C_OPTICS_TBL2_MOD_DAC_FIELD, p->optics_mod0);
	u16 mod, tx, bias, target;
	int apc, step, i, err;
	u8 cur;

	if (!mode)
		return rtl9602c_optics_put_mod(p, p->optics_mod0) ?:
		       rtl9602c_l_write(p, apc_reg, p->optics_apcset0);

	if (!p->optics_mode) {
		err = rtl9602c_optics_tx_bias(p, &tx, &bias);
		if (err)
			return err;
		if (!tx)
			return -ENODATA;
		p->optics_p0 = tx;
	}
	if (!p->optics_p0)
		return -ENODATA;
	target = p->optics_p0 >> mode;

	mod = (p->optics_mod0 & ~RTL9602C_OPTICS_TBL2_MOD_DAC_FIELD) |
	      FIELD_PREP(RTL9602C_OPTICS_TBL2_MOD_DAC_FIELD, dac >> mode);
	err = rtl9602c_optics_put_mod(p, mod) ?:
	      rtl9602c_l_read(p, apc_reg, &cur) ?:
	      rtl9602c_optics_tx_bias(p, &tx, &bias);
	if (err)
		return err;

	apc = cur;
	step = tx > target ? -1 : 1;
	for (i = 0; i <= U8_MAX; i++) {
		if (!tx || bias > RTL9602C_OPTICS_LEVEL_BIAS_MAX)
			return -ERANGE;
		if (step < 0 ? tx <= target : tx >= target)
			return 0;
		apc += step;
		if (apc < 0 || apc > p->optics_apcset0)
			return -ERANGE;
		err = rtl9602c_l_write(p, apc_reg, apc) ?:
		      rtl9602c_optics_tx_bias(p, &tx, &bias);
		if (err)
			return err;
	}
	return -ERANGE;
}

/*
 * Put level mode 0 back before an orderly restart, power-off or halt: the
 * module keeps its registers across a warm reboot of the SoC, and the next
 * kernel takes the settings it finds for the module's own.  A reset that
 * runs no notifier, such as the watchdog's, leaves a lower level in place
 * until the module loses power.
 */
static int rtl9602c_optics_reboot(struct notifier_block *nb, unsigned long action,
				  void *data)
{
	struct rtl9602c_priv *p = container_of(nb, struct rtl9602c_priv,
					       optics_reboot_nb);
	u8 tabsel, eectl;
	int err;

	rtl9602c_i2c_lock(p);
	if (!p->optics_ref || !p->optics_mode) {
		rtl9602c_i2c_unlock(p);
		return NOTIFY_DONE;
	}
	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x7f, &tabsel);
	if (!err) {
		err = rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, 0x02) ?:
		      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_PERSIST,
				      &eectl);
		if (!err && (eectl & RTL9602C_OPTICS_TBL2_PERSIST_OFF) !=
			    RTL9602C_OPTICS_TBL2_PERSIST_OFF)
			err = -EPERM;
		if (!err)
			err = rtl9602c_gn_t2_prepare(p) ?:
			      rtl9602c_optics_level_set(p, 0);
		rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tabsel);
	}
	if (!err)
		p->optics_mode = 0;
	rtl9602c_i2c_unlock(p);
	pr_info("GN25L95: transmit power %s level mode 0 before the restart\n",
		err ? "not put back at" : "back at");
	return NOTIFY_DONE;
}

/**
 * rtl9602c_optics_level_register() - prepare transmit power levelling
 * @p: driver state
 *
 * Sets up the work that sets the levels and the notifier that puts level
 * mode 0 back before a restart.
 *
 * Return: 0, or the error of registering the notifier.
 */
int rtl9602c_optics_level_register(struct rtl9602c_priv *p)
{
	INIT_WORK(&p->optics_level_work, rtl9602c_optics_level_work);
	p->optics_reboot_nb.notifier_call = rtl9602c_optics_reboot;
	return devm_register_reboot_notifier(p->dev, &p->optics_reboot_nb);
}

/**
 * rtl9602c_optics_level_work() - set the optics to the ONU's power level
 * @work: &rtl9602c_priv.optics_level_work
 *
 * Takes the level mode the GPON code asks for and sets it under the optics lock,
 * with the configuration table shown and its writes kept out of the EEPROM
 * as in the modulation hold; a failure puts mode 0 back.  The GPON code is
 * then told what was set.  Nothing is written with CONFIG_RTL9602C_OPTICS_RO
 * or before the first modulation hold has kept the module's own settings.
 *
 * Context: Process context.  May sleep for about a second.
 */
void rtl9602c_optics_level_work(struct work_struct *work)
{
	struct rtl9602c_priv *p = container_of(work, struct rtl9602c_priv,
					       optics_level_work);
	u8 tabsel, eectl, mode, asked;
	u16 tx = 0, bias = 0;
	int err;

	asked = rtl9602c_pon_power_mode(p);
	mode = asked;

	rtl9602c_i2c_lock(p);
	if (mode == p->optics_mode) {
		rtl9602c_i2c_unlock(p);
		return;
	}
	if (IS_ENABLED(CONFIG_RTL9602C_OPTICS_RO) || !p->optics_ref ||
	    mode >= RTL9602C_OPTICS_LEVEL_MODES) {
		err = -EOPNOTSUPP;
		goto told;
	}

	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x7f, &tabsel);
	if (err)
		goto told;
	err = rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, 0x02) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_PERSIST,
			      &eectl);
	if (!err && (eectl & RTL9602C_OPTICS_TBL2_PERSIST_OFF) !=
		    RTL9602C_OPTICS_TBL2_PERSIST_OFF)
		err = -EPERM;
	if (!err)
		err = rtl9602c_gn_t2_prepare(p);
	if (!err) {
		err = rtl9602c_optics_level_set(p, mode);
		if (!err) {
			p->optics_mode = mode;
		} else if (rtl9602c_optics_level_set(p, 0)) {
			/*
			 * Neither the mode nor mode 0: the optics are at no
			 * known level, which the next run and the next hold
			 * both try to leave.
			 */
			pr_err("GN25L95: transmit power level not restored to mode 0\n");
			p->optics_mode = U8_MAX;
		} else {
			p->optics_mode = 0;
		}
	}
	rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tabsel);
	if (!err)
		rtl9602c_optics_tx_bias(p, &tx, &bias);
told:
	mode = p->optics_mode < RTL9602C_OPTICS_LEVEL_MODES ? p->optics_mode : 0;
	rtl9602c_i2c_unlock(p);

	if (!err)
		pr_info("GN25L95: transmit power level mode %u: %u.%u uW, bias %u.%02u mA\n",
			mode, tx / 10, tx % 10, bias / 500, bias % 500 / 5);
	rtl9602c_pon_power_applied(p, asked, mode, err);
}

/**
 * rtl9602c_optics_hold_modulation() - turn off the automatic ER loop
 * @p: driver state
 *
 * The part runs an automatic extinction-ratio loop.  In burst operation on
 * this board the loop takes the modulation current about a quarter below
 * its power-up value while the power-control loop holds the transmitted
 * power, so the extinction ratio falls and the OLT needs a longer preamble
 * to lock onto the ONU's bursts.  The GPON bring-up calls this after the
 * MAC reset and before the first O3 to turn the loop off, so that the
 * modulation runs from the value the module loaded at power-up.  Whether
 * activation needs this is not verified; the bring-up logs a failure and
 * carries on.  With CONFIG_RTL9602C_OPTICS_RO, or for a module the driver
 * does not write (rtl9602c_optics_identify()), this returns 0 at once and
 * writes nothing.
 *
 * The table change stays in registers, kept out of the EEPROM by PERSIST:
 * the loop stays off until the module loses power, across a warm reboot
 * of the SoC too, and a power cycle gives the module its own configuration
 * back.  PERSIST is in the table, which the module shows once the page
 * select holds 02h, and the table needs no password bytes written.  After
 * the part's reset, which loads the tables from the EEPROM as at power-up,
 * the table read and took these writes with nothing written to bytes
 * 123-126 (measured on this board).  So a hold that gives up with -EPERM has written only the page
 * select, when it did not hold 02h, which the module keeps in its EEPROM;
 * table byte 0xcc and the table itself are written only once PERSIST keeps
 * register changes out of the EEPROM.  Once the page select has been set,
 * every path puts it back as found, without checking that write.
 *
 * Context: Process context.  May sleep.
 * Return: 0 when the modulation is held; -EPERM when table writes would
 * reach the EEPROM; -EINVAL when the loop does not turn off or MOD_DAC
 * holds no usable value; otherwise the error of a failed transfer.
 */
static int __rtl9602c_optics_hold_modulation(struct rtl9602c_priv *p)
{
	u8 hi, lo, er, eectl, modmax, tabsel, rb_er;
	unsigned int dac;
	int err;

	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x7f, &tabsel);
	if (err)
		return err;

	/* Show the table; from here every path puts the page select back. */
	err = rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, 0x02) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_PERSIST,
			      &eectl) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_MOD_LIMIT,
			      &modmax) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_ER_CTRL,
			      &er);
	if (err)
		goto out;

	/*
	 * Unless both PERSIST_OFF bits are set, the part could copy a table
	 * write into its EEPROM: nothing in the table is written then.
	 */
	if ((eectl & RTL9602C_OPTICS_TBL2_PERSIST_OFF) != RTL9602C_OPTICS_TBL2_PERSIST_OFF) {
		pr_warn("GN25L95: EEPROM updates are enabled (C7h %02x); modulation left alone rather than written to the EEPROM\n",
			eectl);
		err = -EPERM;
		goto out;
	}

	err = rtl9602c_gn_t2_prepare(p);
	if (err)
		goto out;

	/*
	 * A transmit power level set in an earlier session is undone first,
	 * back to the module's own modulation and set point.
	 */
	if (p->optics_ref && p->optics_mode) {
		err = rtl9602c_optics_level_set(p, 0);
		if (err)
			goto out;
		p->optics_mode = 0;
		pr_info("GN25L95: transmit power back at level mode 0\n");
	}

	/*
	 * Turn the loop off and read ER_CTRL back.  With the loop off, MOD_DAC
	 * reads the value the module loaded at power-up, which the modulation
	 * now runs from (measured on this board).
	 */
	err = rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_ER_CTRL,
			       er & ~RTL9602C_OPTICS_TBL2_ER_CTRL_AUTO) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_ER_CTRL,
			      &rb_er) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_MOD_DAC,
			      &hi) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_MOD_DAC + 1,
			      &lo);
	if (err)
		goto out;

	/*
	 * The loop did not turn off, or MOD_DAC holds nothing usable: write
	 * ER_CTRL back as it was found, leaving the loop as it was.  That write
	 * is not checked.
	 */
	if ((rb_er & RTL9602C_OPTICS_TBL2_ER_CTRL_AUTO) || !(hi | lo) ||
	    (modmax != 0xff && hi > modmax)) {
		rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_ER_CTRL,
				 er);
		pr_warn("GN25L95: modulation DAC seed %02x%02x is unusable (modulation max %02x, ER control %02x); modulation left to the module\n",
			hi, lo, modmax, rb_er);
		err = -EINVAL;
		goto out;
	}

	/*
	 * The first hold since probe keeps the modulation and the power
	 * control's set point as the module has them, the settings of level
	 * mode 0 that every lower level is made from and that every session
	 * starts at.
	 */
	if (!p->optics_ref) {
		u8 apc;

		err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_APCSET,
				      &apc);
		if (err)
			goto out;
		p->optics_apcset0 = apc;
		p->optics_mod0 = (hi << 8) | lo;
		p->optics_ref = true;
	}

	/*
	 * With MOD_RANGE at 0, as on this module, the current is logged as
	 * (DAC + 8) / 10 mA, DAC being bits 15:6 of MOD_DAC; otherwise the raw
	 * register is logged.  The field and the conversion are not verified
	 * and affect only this line.
	 */
	dac = ((hi << 8) | lo) >> 6;
	if (!FIELD_GET(RTL9602C_OPTICS_TBL2_ER_CTRL_MOD_RANGE, rb_er))
		pr_info("GN25L95: modulation held at %u.%u mA (modulation DAC %02x%02x, the module's calibration); automatic ER %02x -> %02x\n",
			(dac + 8) / 10, (dac + 8) % 10, hi, lo, er, rb_er);
	else
		pr_info("GN25L95: modulation held at modulation DAC %02x%02x (the module's calibration); automatic ER %02x -> %02x\n",
			hi, lo, er, rb_er);
out:
	/* Leave the page select as it was found. */
	rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tabsel);
	return err;
}

int rtl9602c_optics_hold_modulation(struct rtl9602c_priv *p)
{
	if (IS_ENABLED(CONFIG_RTL9602C_OPTICS_RO) || !p->optics_known)
		return 0;

	guard(rtl9602c_i2c)(p);
	return __rtl9602c_optics_hold_modulation(p);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * The "eeblocks" view: all 256 bytes at each of 0x50-0x53, placed in a
 * 1 KiB image, with the number of reads that failed at each address
 * ("silent" when all did).  A failed read, not acknowledged or timed out,
 * shows as 00.  On this module 0x52 and 0x53 acknowledge but read 0xff
 * throughout (measured on this board): the module's stored configuration
 * cannot be read through them.  The view only reads, so A2h 128-255 show
 * whichever page is selected at the time.
 */
static int rtl9602c_eeblocks_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int slave, i;
	u8 b[0x100];
	int err, nack;

	for (slave = 0x50; slave <= 0x53; slave++) {
		nack = 0;
		rtl9602c_i2c_lock(p);
		for (i = 0; i < sizeof(b); i++) {
			err = rtl9602c_i2c_read_byte(p, slave, i, &b[i]);
			if (err) {
				b[i] = 0;
				nack++;
			}
		}
		rtl9602c_i2c_unlock(p);
		seq_printf(s, "=== i2c 0x%02x -- image 0x%03x -- %d of 256 did not answer\n",
			   slave, (slave - 0x50) * 0x100, nack);
		if (nack == (int)sizeof(b)) {
			seq_puts(s, "    silent\n\n");
			continue;
		}
		for (i = 0; i < sizeof(b); i += 16)
			seq_printf(s, "%02x: %*ph\n", i, 16, b + i);
		seq_puts(s, "\n");
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_eeblocks);

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

/*
 * The "tables" view: A2h bytes 128-255 for each table the page select can
 * show, 0, 1, 2, 4, 5 and 6.  Table 2 holds the laser driver's
 * configuration, among it the receiver's loss-of-signal control (BBh) and
 * the APD bias control (BDh); table 6 the temperature indexed APD DAC
 * values at C0h-FFh.  The page select is put back as it was, also when a
 * table fails.  The page select is kept in the module's EEPROM, so every
 * read of the view writes it; a CONFIG_RTL9602C_OPTICS_RO build has no
 * such view.
 */
static int rtl9602c_tables_show(struct seq_file *s, void *unused)
{
	static const u8 tables[] = { 0, 1, 2, 4, 5, 6 };
	struct rtl9602c_priv *p = s->private;
	unsigned int t, i;
	u8 b[0x80], was;
	int err;

	rtl9602c_i2c_lock(p);
	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x7f, &was);
	if (err) {
		rtl9602c_i2c_unlock(p);
		seq_printf(s, "the page select could not be read: %d\n", err);
		return 0;
	}
	for (t = 0; t < ARRAY_SIZE(tables); t++) {
		err = rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tables[t]);
		for (i = 0; i < sizeof(b) && !err; i++)
			err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x80 + i, &b[i]);
		if (err) {
			seq_printf(s, "=== table %u: failed: %d\n\n", tables[t], err);
			continue;
		}
		seq_printf(s, "=== table %u\n", tables[t]);
		for (i = 0; i < sizeof(b); i += 16)
			seq_printf(s, "%02x: %*ph\n", 0x80 + i, 16, b + i);
		seq_puts(s, "\n");
	}
	err = rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, was);
	rtl9602c_i2c_unlock(p);
	if (err)
		seq_printf(s, "the page select was not put back to %02x: %d\n", was, err);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_tables);

/*
 * The "t2write" entry stores bytes of the laser driver's configuration table
 * (table 2) in the module's EEPROM, and resets the part.  Root writes one
 * command at a time:
 *
 * "open" writes the password bytes (rtl9602c_gn_t2_show()), shows table 2
 * and clears both bits of EEPROM_CTRL (C7h), so that the part copies the
 * changes of its registers into its EEPROM; C7h and the page select as found
 * are kept for "close".
 *
 * "REG VALUE", both hexadecimal, after "open": writes the password bytes and
 * shows table 2 again, so that nothing else that switched the page in between
 * sends the byte to another table, then writes VALUE at table 2 byte REG.
 * The byte is not read back: while the part copies into its EEPROM it does
 * not acknowledge and reads return a constant (measured on this board), so a
 * read-back then proves nothing.  A refused transfer is retried for a second.
 *
 * "close" puts C7h back as it was found, and the page select.
 *
 * "reset" restarts the part's state machine with its reset key, 5Dh 2Ch 6Ah
 * C9h at A2h 123-126 and 0Bh at the page select, which loads the tables from
 * the EEPROM as at power-up, then writes 0xff to the password bytes, as
 * "open" does, and puts the page select back; a read of the "tables" view
 * after it shows what the EEPROM holds.  The laser goes through its start-up
 * again, so the PON drops for that time.
 *
 * Only the bytes that a repair of a module has needed are taken: the current
 * limits ADh-AFh, B6h-B8h, the receiver's loss-of-signal control BBh, its
 * APD bias control BDh and the ADC control C1h.  The other bytes of B1h-BDh,
 * among which are the transmitter's fault, alarm and rogue ONU controls, are
 * not taken, and neither is SAFE_MODE_STARTUP (A0h), whose wrong value turns
 * the transmitter off for good, nor the calibrated DACs and constants, the
 * status, the part's I2C address, the EEPROM controls, the passwords or the
 * monitors.  Which of B1h-BAh holds which control is not recorded, so that
 * B6h-B8h hold none of the rogue ONU controls is not known.  Table 2 bytes
 * written while C7h was clear kept their values through every power-up of
 * the module since (measured on this board).
 */
#define RTL9602C_OPTICS_RESET_MS	500
#define RTL9602C_OPTICS_BUSY_TRIES	20
#define RTL9602C_OPTICS_BUSY_MS		50

static bool rtl9602c_t2_storable(unsigned int reg)
{
	return (reg >= 0xad && reg <= 0xaf) || (reg >= 0xb6 && reg <= 0xb8) ||
	       reg == 0xbb || reg == 0xbd || reg == 0xc1;
}

/* A write the part may refuse while it copies into its EEPROM. */
static int rtl9602c_l_write_busy(struct rtl9602c_priv *p, u16 reg, u8 val)
{
	int err, i;

	for (i = 0; i < RTL9602C_OPTICS_BUSY_TRIES; i++) {
		err = rtl9602c_l_write(p, reg, val);
		if (!err)
			break;
		msleep(RTL9602C_OPTICS_BUSY_MS);
	}
	return err;
}

/* The password bytes and table 2 shown, each write retried while the part is busy. */
static int rtl9602c_gn_t2_show(struct rtl9602c_priv *p)
{
	int err = 0, i;

	for (i = 0; i < 4 && !err; i++)
		err = rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE +
					    RTL9602C_OPTICS_A2_PASSWORD + i, 0xff);
	return err ?: rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE + 0x7f, 0x02);
}

static int rtl9602c_gn_t2_open(struct rtl9602c_priv *p)
{
	u8 tabsel, eectl;
	int err;

	if (p->optics_t2_open)
		return -EBUSY;
	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x7f, &tabsel);
	if (err)
		return err;
	err = rtl9602c_gn_t2_show(p) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_PERSIST,
			      &eectl) ?:
	      rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_PERSIST,
			       eectl & ~RTL9602C_OPTICS_TBL2_PERSIST_OFF);
	if (err) {
		rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tabsel);
		return err;
	}
	p->optics_t2_tabsel = tabsel;
	p->optics_t2_eectl = eectl;
	p->optics_t2_open = true;
	pr_info("GN25L95: table 2 open for storing (C7h %02x -> %02x)\n",
		eectl, (u8)(eectl & ~RTL9602C_OPTICS_TBL2_PERSIST_OFF));
	return 0;
}

static int rtl9602c_gn_t2_close(struct rtl9602c_priv *p)
{
	int err;

	if (!p->optics_t2_open)
		return -EINVAL;
	err = rtl9602c_gn_t2_show(p) ?:
	      rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_PERSIST,
				    p->optics_t2_eectl) ?:
	      rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE + 0x7f, p->optics_t2_tabsel);
	if (err)
		return err;
	p->optics_t2_open = false;
	pr_info("GN25L95: table 2 closed (C7h %02x)\n", p->optics_t2_eectl);
	return 0;
}

static int rtl9602c_gn_t2_store(struct rtl9602c_priv *p, u8 reg, u8 val)
{
	int err;

	if (!p->optics_t2_open)
		return -EINVAL;
	err = rtl9602c_gn_t2_show(p) ?:
	      rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE + reg, val);
	if (!err)
		pr_info("GN25L95: table 2 %02Xh written %02x\n", reg, val);
	return err;
}

static int rtl9602c_gn_reset(struct rtl9602c_priv *p)
{
	static const u8 key[] = { 0x5d, 0x2c, 0x6a, 0xc9 };
	u8 tabsel;
	int err, i;

	if (p->optics_t2_open)
		return -EBUSY;
	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x7f, &tabsel);
	for (i = 0; i < ARRAY_SIZE(key) && !err; i++)
		err = rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_A2_PASSWORD + i,
				       key[i]);
	if (err)
		return err;
	err = rtl9602c_l_write_changed(p, RTL9602C_OPTICS_A2_BASE + 0x7f, 0x0b);
	msleep(RTL9602C_OPTICS_RESET_MS);
	for (i = 0; i < 4 && !err; i++)
		err = rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE +
					    RTL9602C_OPTICS_A2_PASSWORD + i, 0xff);
	err = err ?: rtl9602c_l_write_busy(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tabsel);
	if (!err)
		pr_info("GN25L95: reset, the tables loaded from the EEPROM\n");
	return err;
}

static ssize_t rtl9602c_t2write_write(struct file *file, const char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct rtl9602c_priv *p = file->private_data;
	unsigned int reg, val;
	char buf[16];
	int err;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	guard(rtl9602c_i2c)(p);
	if (sysfs_streq(buf, "open"))
		err = rtl9602c_gn_t2_open(p);
	else if (sysfs_streq(buf, "close"))
		err = rtl9602c_gn_t2_close(p);
	else if (sysfs_streq(buf, "reset"))
		err = rtl9602c_gn_reset(p);
	else if (sscanf(buf, "%x %x", &reg, &val) == 2 && val <= 0xff &&
		 rtl9602c_t2_storable(reg))
		err = rtl9602c_gn_t2_store(p, reg, val);
	else
		err = -EINVAL;
	return err ?: len;
}

static const struct file_operations rtl9602c_t2write_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = rtl9602c_t2write_write,
	.llseek = noop_llseek,
};

/**
 * rtl9602c_optics_debugfs_init() - create the optics' debugfs views
 * @p: driver state
 *
 * Creates "a2", "eeblocks" and "sfpslave", mode 0400, in the switch's
 * debugfs directory.  They only read, so a CONFIG_RTL9602C_OPTICS_RO
 * build has them too.  "tables" writes the page select, and only a build
 * without it has that view.  "t2write", mode 0200,
 * stores table 2 bytes in the module's EEPROM, where they outlive every
 * power cycle, among them the rogue ONU controls; only a build with
 * CONFIG_NET_DSA_RTL9602C_OPTICS_STORE, which excludes the other, has it.
 */
void rtl9602c_optics_debugfs_init(struct rtl9602c_priv *p)
{
	debugfs_create_file("a2", 0400, p->debugfs, p,
			    &rtl9602c_a2_fops);
	debugfs_create_file("eeblocks", 0400, p->debugfs, p,
			    &rtl9602c_eeblocks_fops);
	debugfs_create_file("sfpslave", 0400, p->debugfs, p,
			    &rtl9602c_sfpslave_fops);
	if (IS_ENABLED(CONFIG_RTL9602C_OPTICS_RO))
		return;
	debugfs_create_file("tables", 0400, p->debugfs, p,
			    &rtl9602c_tables_fops);
	if (IS_ENABLED(CONFIG_NET_DSA_RTL9602C_OPTICS_STORE))
		debugfs_create_file("t2write", 0200, p->debugfs, p,
				    &rtl9602c_t2write_fops);
}

#endif
