// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9602C switch: the optics I2C master, diagnostics and TX_DISABLE
 *
 * The transmitter and the receiver are run by a laser driver IC that
 * serves the SFF-8472 management interface, A0h at 0x50 and A2h at 0x51.
 * The SoC has two I2C masters with the same set of registers; the part
 * answers on master 1 only (through master 0 its addresses read zeros),
 * and the RTL9602C_I2C_* registers used here are master 1's.  The part
 * runs from the configuration and calibration in its own EEPROM, and the
 * driver writes nothing to it at probe.
 *
 * This file moves single bytes over the master and registers it as an
 * SMBus adapter, publishes the SFF-8472 diagnostics through hwmon, drives
 * the laser's TX_DISABLE line, turns the part's automatic extinction-ratio
 * loop off at every PON bring-up (rtl9602c_optics_hold_modulation()), and
 * offers two debugfs views that only read, "a2" and "eeblocks".
 *
 * CONFIG_RTL9602C_OPTICS_RO makes the modulation hold, the driver's only
 * write to the part, return without writing.  The hwmon device and the
 * debugfs views stay as they are, since they only read, and the adapter
 * is not restricted: a client of it, userspace through i2c-dev included,
 * can still write A0h and A2h.
 *
 * No lock covers a transfer.  The hwmon reads, the debugfs views, the
 * modulation hold and clients of the adapter all drive the same registers,
 * and the hwmon core and the I2C core each serialize their own callers
 * only, so two users at once could mix their transfers.  This has not been
 * observed.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/ptp_clock_kernel.h>
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
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "rtl9602c.h"

/*
 * BUSY is set while a transfer runs.  Poll up to 1000 times, 1 us apart:
 * a byte at about 1 MHz takes tens of microseconds.  The last command word
 * read, with its NACK bit, is left in @cmd.
 */
static int rtl9602c_i2c_wait(struct rtl9602c_priv *p, u32 *cmd)
{
	int i;

	for (i = 0; i < 1000; i++) {
		*cmd = rtl9602c_r(p, RTL9602C_I2C_CMD);
		if (!(*cmd & RTL9602C_I2C_CMD_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void rtl9602c_i2c_setup_slave(struct rtl9602c_priv *p, u16 slave)
{
	/*
	 * Describe the next transfer: the 7-bit device address, one
	 * register-address byte and one data byte, and the SCL divider, taken
	 * to give about 1 MHz (not verified).  Without BUS_EN the master runs
	 * the transfer without driving the bus: it ends without NACK and every
	 * read returns the same byte.  Bits 31:22 are left as they are.
	 */
	rtl9602c_rmw(p, RTL9602C_I2C_XFER_CFG,
		     RTL9602C_I2C_XFER_CFG_BUS_EN | RTL9602C_I2C_XFER_CFG_DEV_ADDR |
		     RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH |
		     RTL9602C_I2C_XFER_CFG_DATA_WIDTH | RTL9602C_I2C_XFER_CFG_CLK_DIV,
		     RTL9602C_I2C_XFER_CFG_BUS_EN |
		     FIELD_PREP(RTL9602C_I2C_XFER_CFG_DEV_ADDR, slave) |
		     FIELD_PREP(RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH,
				RTL9602C_I2C_XFER_CFG_WIDTH_8BIT) |
		     FIELD_PREP(RTL9602C_I2C_XFER_CFG_DATA_WIDTH,
				RTL9602C_I2C_XFER_CFG_WIDTH_8BIT) |
		     FIELD_PREP(RTL9602C_I2C_XFER_CFG_CLK_DIV,
				RTL9602C_I2C_XFER_CFG_CLK_DIV_1MHZ));
}

/**
 * rtl9602c_i2c_read_byte() - read one register of a device on the optics bus
 * @p: driver state
 * @slave: 7-bit device address
 * @reg: register address inside the device
 * @val: set to the byte read
 *
 * Busy-waits for up to about a millisecond.  No lock is taken: see the
 * note at the top of this file.
 *
 * Return: 0, -ETIMEDOUT if the transfer does not end, or -ENXIO if the
 * device does not acknowledge.
 */
int rtl9602c_i2c_read_byte(struct rtl9602c_priv *p, u16 slave, u8 reg,
			   u8 *val)
{
	u32 cmd;
	int err;

	rtl9602c_i2c_setup_slave(p, slave);
	rtl9602c_w(p, RTL9602C_I2C_SUBADDR, reg);

	rtl9602c_w(p, RTL9602C_I2C_CMD, RTL9602C_I2C_CMD_START);
	err = rtl9602c_i2c_wait(p, &cmd);
	if (err)
		return err;
	if (cmd & RTL9602C_I2C_CMD_NACK)
		return -ENXIO;
	*val = rtl9602c_r(p, RTL9602C_I2C_RX_DATA);
	return 0;
}

static int rtl9602c_i2c_write_byte(struct rtl9602c_priv *p, u16 slave, u8 reg,
				   u8 val)
{
	u32 cmd;
	int err;

	rtl9602c_i2c_setup_slave(p, slave);
	rtl9602c_w(p, RTL9602C_I2C_SUBADDR, reg);
	rtl9602c_w(p, RTL9602C_I2C_TX_DATA, val);

	rtl9602c_w(p, RTL9602C_I2C_CMD,
		   RTL9602C_I2C_CMD_START | RTL9602C_I2C_CMD_WRITE);
	err = rtl9602c_i2c_wait(p, &cmd);
	if (err)
		return err;
	if (cmd & RTL9602C_I2C_CMD_NACK)
		return -ENXIO;
	return 0;
}

/*
 * One register-address byte and one data byte is the only transfer the
 * master is set up for, so the adapter offers SMBus byte data and nothing
 * else.  Access through the adapter is not verified.
 */
static s32 rtl9602c_i2c_smbus_xfer(struct i2c_adapter *adap, u16 addr,
				   unsigned short flags, char read_write,
				   u8 command, int size,
				   union i2c_smbus_data *data)
{
	struct rtl9602c_priv *p = i2c_get_adapdata(adap);

	if (size != I2C_SMBUS_BYTE_DATA)
		return -EOPNOTSUPP;

	if (read_write == I2C_SMBUS_READ)
		return rtl9602c_i2c_read_byte(p, addr, command, &data->byte);
	return rtl9602c_i2c_write_byte(p, addr, command, data->byte);
}

static u32 rtl9602c_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_BYTE_DATA;
}

static const struct i2c_algorithm rtl9602c_i2c_algo = {
	.smbus_xfer	= rtl9602c_i2c_smbus_xfer,
	.functionality	= rtl9602c_i2c_functionality,
};

/**
 * rtl9602c_i2c_register() - register the optics I2C master as an adapter
 * @p: driver state
 *
 * Enables the master and registers it as the SMBus adapter "rtl9602c i2c".
 * The driver itself reaches the part without going through the adapter,
 * and CONFIG_RTL9602C_OPTICS_RO does not restrict what the adapter's
 * clients send.
 *
 * Return: 0 or a negative error code.
 */
int rtl9602c_i2c_register(struct rtl9602c_priv *p)
{
	/*
	 * Until its pad function is enabled the master ignores the start
	 * command: the transfer ends at once, with no BUSY, no NACK and a zero
	 * byte.  The register holds the enables of other pad functions too, so
	 * only this bit is changed.
	 */
	rtl9602c_rmw(p, RTL9602C_PADMUX_FUNC_EN, 0, RTL9602C_PADMUX_FUNC_EN_I2C);

	p->i2c.owner = THIS_MODULE;
	p->i2c.algo = &rtl9602c_i2c_algo;
	p->i2c.dev.parent = p->dev;
	/*
	 * The adapter takes the switch's node, so the I2C core also treats the
	 * node's children as I2C devices; the ports node is not one, and each
	 * registration logs that no I2C device could be created for it.
	 */
	p->i2c.dev.of_node = p->dev->of_node;
	i2c_set_adapdata(&p->i2c, p);
	strscpy(p->i2c.name, "rtl9602c i2c", sizeof(p->i2c.name));
	return devm_i2c_add_adapter(p->dev, &p->i2c);
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
 * A0h and A2h bytes 0-127 take writes without a password and keep them
 * across a power cycle of the module: a write there is a permanent change
 * to it.  Of those bytes the driver writes only A2h 123-127, in the
 * modulation hold.
 */
static int rtl9602c_l_write(struct rtl9602c_priv *p, u16 reg, u8 val)
{
	u16 slave;
	u8 sub;
	int err;

	err = rtl9602c_l_addr(reg, &slave, &sub);
	if (err)
		return err;
	return rtl9602c_i2c_write_byte(p, slave, sub, val);
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
 * device tree this does nothing.  That the line reaches the laser's
 * disable input, and that asserting it stops the light, is not verified.
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
	 * only that bit changes, and setting it again is harmless.  That this
	 * bit routes the pad is not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_PADMUX_GPIO_EN, 0, BIT(RTL9602C_PADMUX_GPIO_EN_TX_DISABLE));
	gpiod_set_value_cansleep(p->tx_disable, disable);
	return 0;
}

#if IS_ENABLED(CONFIG_HWMON)
/*
 * Read the diagnostic word @off bytes into A2h 96-105, most significant
 * byte first.  SFF-8472 expects such a field to be read in one two-byte
 * sequence, during which the module does not update it; here the two
 * bytes are separate transfers, so a value that changes between them can
 * come back as the halves of two samples.  How often that happens on this
 * module is not measured.
 */
static int rtl9602c_l_a2_word(struct rtl9602c_priv *p, unsigned int off, u16 *val)
{
	u8 hi, lo;
	int err;

	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_A2_DDM + off,
			      &hi);
	if (err)
		return err;
	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_A2_DDM +
			      off + 1, &lo);
	if (err)
		return err;

	*val = (u16)hi << 8 | lo;
	return 0;
}

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
 * A value is computed even when its read fails; the error returned keeps
 * the hwmon core from using it.
 */
static int rtl9602c_hwmon_read(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, long *val)
{
	struct rtl9602c_priv *p = dev_get_drvdata(dev);
	u16 raw;
	int err;

	switch (type) {
	case hwmon_temp:
		err = rtl9602c_l_a2_word(p, 0, &raw);
		*val = DIV_ROUND_CLOSEST((long)(s16)raw * 1000, 256);
		break;
	case hwmon_in:
		err = rtl9602c_l_a2_word(p, 2, &raw);
		*val = DIV_ROUND_CLOSEST((long)raw, 10);
		break;
	case hwmon_curr:
		err = rtl9602c_l_a2_word(p, 4, &raw);
		*val = DIV_ROUND_CLOSEST((long)raw, 500);
		break;
	case hwmon_power:
		err = rtl9602c_l_a2_word(p, channel ? 8 : 6, &raw);
		*val = DIV_ROUND_CLOSEST((long)raw, 10);
		break;
	default:
		return -EOPNOTSUPP;
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

/* temp1, in0, curr1, power1 (transmitted) and power2 (received) */
static const struct hwmon_channel_info * const rtl9602c_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
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
 * answers at A2h.  Without CONFIG_HWMON this is a stub that returns 0.
 *
 * Return: 0, the error of the test read, or an error from the hwmon core.
 */
int rtl9602c_optics_hwmon_register(struct rtl9602c_priv *p)
{
	struct device *hwmon;
	u16 probe;
	int err;

	err = rtl9602c_l_a2_word(p, 0, &probe);
	if (err)
		return err;

	hwmon = devm_hwmon_device_register_with_info(p->dev, "rtl9602c_optics",
						     p, &rtl9602c_hwmon_chip,
						     NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}
#else
int rtl9602c_optics_hwmon_register(struct rtl9602c_priv *p)
{
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * The "a2" view: A2h bytes 0-127, each read on its own, then the five
 * diagnostic words in SFF-8472 units, the password bytes 123-126 and the
 * page select, byte 127.  The first failed read ends the view with its
 * error.  It only reads.
 */
static int rtl9602c_a2_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u8 b[0x80];
	unsigned int i;
	int err;

	for (i = 0; i < sizeof(b); i++) {
		err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + i, &b[i]);
		if (err) {
			seq_printf(s, "A2h read failed at 0x%02x: %d\n", i, err);
			return 0;
		}
	}

	for (i = 0; i < sizeof(b); i += 16)
		seq_printf(s, "%02x: %*ph\n", i, 16, b + i);

	/*
	 * The received power shows whether light reaches the receiver, which
	 * the GPON MAC's loss of signal cannot tell: that alarm is the same
	 * for no light and for light the receiver does not recover.  A
	 * temperature between -1 and 0 degC prints without its minus sign.
	 */
	{
		s16 temp = get_unaligned_be16(b + RTL9602C_OPTICS_A2_DDM);
		u16 vcc  = get_unaligned_be16(b + RTL9602C_OPTICS_A2_DDM + 2);
		u16 bias = get_unaligned_be16(b + RTL9602C_OPTICS_A2_DDM + 4);
		u16 txp  = get_unaligned_be16(b + RTL9602C_OPTICS_A2_DDM + 6);
		u16 rxp  = get_unaligned_be16(b + RTL9602C_OPTICS_A2_DDM + 8);

		seq_printf(s, "\ntemperature %d.%02u C\n",
			   temp / 256, ((temp < 0 ? -temp : temp) % 256) * 100 / 256);
		seq_printf(s, "vcc         %u.%03u V\n", vcc / 10000, (vcc / 10) % 1000);
		seq_printf(s, "tx bias     %u uA\n", bias * 2);
		seq_printf(s, "tx power    %u.%u uW\n", txp / 10, txp % 10);
		seq_printf(s, "rx power    %u.%u uW   <- light on the fibre\n",
			   rxp / 10, rxp % 10);
	}

	/*
	 * SFF-8472 leaves bytes 120-126 to the module maker.  The modulation
	 * hold writes 0xff to 123-126, and on this module they read 00
	 * whatever was written (measured on this board).
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
#define RTL9602C_OPTICS_TBL2_MOD_DAC		0xab
#define RTL9602C_OPTICS_TBL2_MOD_LIMIT		0xaf
#define RTL9602C_OPTICS_TBL2_PERSIST	0xc7
#define RTL9602C_OPTICS_TBL2_PERSIST_OFF	(BIT(1) | BIT(0))

/*
 * Prepare the configuration table for writing and show it at A2h 128-255:
 * write 0xff to each password byte, keep the page select in @tabsel for
 * the caller to restore, and select page 02h.  If bit 3 or 2 of table byte
 * 0xcc is set, clear both and wait 400 ms; this module reads 0xff there.
 * That the table needs the password bytes or the 0xcc step before it takes
 * a write is not verified.  As A2h bytes 0-127 are kept in the module's
 * EEPROM, every call writes the EEPROM; whether the part skips a byte that
 * already holds the value is not known.
 */
static int rtl9602c_gn_t2_open(struct rtl9602c_priv *p, u8 *tabsel)
{
	u8 acc;
	int err, i;

	for (i = 0; i < 4; i++) {
		err = rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_A2_PASSWORD + i,
				       0xff);
		if (err)
			return err;
	}
	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tabsel) ?:
	      rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + 0x7f, 0x02) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + 0xcc, &acc);
	if (err)
		return err;
	if (acc & (BIT(3) | BIT(2))) {
		err = rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + 0xcc,
				       acc & ~(BIT(3) | BIT(2)));
		if (err)
			return err;
		msleep(400);
	}
	return 0;
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
 * carries on.  With CONFIG_RTL9602C_OPTICS_RO this returns 0 at once and
 * writes nothing.
 *
 * The table change stays in registers, kept out of the EEPROM by PERSIST:
 * the loop stays off until the module loses power, across a warm reboot
 * of the SoC too, and a power cycle gives the module its own configuration
 * back.  Opening the table writes the password bytes, the page select and
 * possibly table byte 0xcc before PERSIST is checked, so even a hold that
 * gives up with -EPERM has written A2h bytes 123-127, which the module
 * keeps in its EEPROM, and a write to 0xcc is then copied there too.
 * After the table is open every path writes the page select back as
 * found, without checking that write; an error while opening it returns
 * before that and may leave page 02h selected.
 *
 * Context: Process context.  May sleep.
 * Return: 0 when the modulation is held; -EPERM when table writes would
 * reach the EEPROM; -EINVAL when the loop does not turn off or MOD_DAC
 * holds no usable value; otherwise the error of a failed transfer.
 */
int rtl9602c_optics_hold_modulation(struct rtl9602c_priv *p)
{
	u8 hi, lo, er, eectl, modmax, tabsel, rb_er;
	unsigned int dac;
	int err;

	if (IS_ENABLED(CONFIG_RTL9602C_OPTICS_RO))
		return 0;

	err = rtl9602c_gn_t2_open(p, &tabsel);
	if (err)
		return err;

	err = rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_PERSIST,
			      &eectl) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_MOD_LIMIT,
			      &modmax) ?:
	      rtl9602c_l_read(p, RTL9602C_OPTICS_A2_BASE + RTL9602C_OPTICS_TBL2_ER_CTRL,
			      &er);
	if (err)
		goto out;

	/*
	 * Unless both PERSIST_OFF bits are set, the part could copy the table
	 * write into its EEPROM.
	 */
	if ((eectl & RTL9602C_OPTICS_TBL2_PERSIST_OFF) != RTL9602C_OPTICS_TBL2_PERSIST_OFF) {
		pr_warn("GN25L95: EEPROM updates are enabled (C7h %02x); modulation left alone rather than written to the EEPROM\n",
			eectl);
		err = -EPERM;
		goto out;
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
	rtl9602c_l_write(p, RTL9602C_OPTICS_A2_BASE + 0x7f, tabsel);
	return err;
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
		for (i = 0; i < sizeof(b); i++) {
			err = rtl9602c_i2c_read_byte(p, slave, i, &b[i]);
			if (err) {
				b[i] = 0;
				nack++;
			}
		}
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

/**
 * rtl9602c_optics_debugfs_init() - create the optics' debugfs views
 * @p: driver state
 *
 * Creates "a2" and "eeblocks", both mode 0400, in the switch's debugfs
 * directory.  Both only read the part, so a CONFIG_RTL9602C_OPTICS_RO
 * build has them too.
 */
void rtl9602c_optics_debugfs_init(struct rtl9602c_priv *p)
{
	debugfs_create_file("a2", 0400, p->debugfs, p,
			    &rtl9602c_a2_fops);
	debugfs_create_file("eeblocks", 0400, p->debugfs, p,
			    &rtl9602c_eeblocks_fops);
}

#endif
