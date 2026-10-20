// SPDX-License-Identifier: GPL-2.0-only
/*
 * Semtech GN25L95: the laser driver and limiting amplifier of a PON ONU
 *
 * The part runs the ONU's burst-mode transmitter and its receiver from the
 * configuration and calibration in an EEPROM of its own, and serves the
 * SFF-8472 management interface: A0h at the address of its node, 0x50, and
 * A2h at the next. The driver writes nothing to it at probe.
 *
 * The driver publishes the SFF-8472 diagnostics through hwmon, drives the
 * transmitter's hard TX_DISABLE input, and offers the optics to the ONU's
 * MAC through the PON class (struct pon_optics): before every activation it
 * puts back the configuration the part loads at power-up, the automatic
 * extinction-ratio loop on (gn25l95_start()), and it sets the transmit power
 * levels the MAC asks for (gn25l95_set_power_level()). In the client's
 * debugfs directory it offers
 * two views that only read, "a2" and "eeblocks", a third, "tables", that
 * selects each table to read it, and, with CONFIG_PON_GN25L95_STORE,
 * "t2write", which stores table 2 bytes in the part's EEPROM on request.
 *
 * Whether the part may be written at all is the bus's to decide: an
 * adapter that refuses a write with -EPERM, as the RTL9602C's does for a
 * module it does not know (CONFIG_RTL9602C_OPTICS_RO), leaves the part as it
 * is, and the driver then only reads it.
 *
 * The hwmon reads, the debugfs views and the MAC's operations drive the same
 * registers, and the hwmon core and the I2C core each serialize their own
 * callers only. So every user holds the driver's lock for its whole
 * sequence and the adapter's bus lock inside it (gn25l95_lock()): a
 * bring-up switches the page of A2h 128-255 and puts it back, and no other
 * user, a client of the adapter included, sees the switched page.
 */

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pon.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/swab.h>
#include <linux/uaccess.h>

/*
 * The bytes of the management interface as the driver numbers them:
 * 0x000-0x0ff are A0h bytes 0-255, 0x100-0x1ff are A2h bytes 0-255.  Each
 * of the five diagnostics, numbered as in enum gn25l95_ddm, has a word in
 * A2h 96-105 and four in A2h 0-39: its high alarm, low alarm, high warning
 * and low warning thresholds.  A2h bytes 123 to 126 are left to the module
 * maker, and 127 is the page select of A2h 128-255.
 */
#define GN25L95_A2			0x100
#define GN25L95_A2_LIMIT(ddm, limit)	(8 * (ddm) + 2 * (limit))
#define GN25L95_A2_DDM			0x60
#define GN25L95_A2_WORD(ddm)		(GN25L95_A2_DDM + 2 * (ddm))
#define GN25L95_A2_PASSWORD		0x7b
#define GN25L95_A2_PAGE			0x7f

/* Transmit power level modes 0 to 2; see "Transmit power levelling" below. */
#define GN25L95_LEVEL_MODES	3

/**
 * enum gn25l95_ddm - the diagnostics of SFF-8472, in A2h order
 * @GN25L95_TEMP: temperature, signed, 1/256 degC
 * @GN25L95_VCC: supply voltage, 100 uV
 * @GN25L95_BIAS: laser bias current, 2 uA
 * @GN25L95_TX: transmitted optical power, 0.1 uW
 * @GN25L95_RX: received optical power, 0.1 uW
 */
enum gn25l95_ddm {
	GN25L95_TEMP,
	GN25L95_VCC,
	GN25L95_BIAS,
	GN25L95_TX,
	GN25L95_RX,
};

/**
 * enum gn25l95_limit - the thresholds of a diagnostic, in A2h order
 * @GN25L95_HIGH_ALARM: high alarm
 * @GN25L95_LOW_ALARM: low alarm
 * @GN25L95_HIGH_WARN: high warning
 * @GN25L95_LOW_WARN: low warning
 */
enum gn25l95_limit {
	GN25L95_HIGH_ALARM,
	GN25L95_LOW_ALARM,
	GN25L95_HIGH_WARN,
	GN25L95_LOW_WARN,
};

/**
 * struct gn25l95 - the driver's state
 * @client: the client at A0h; A2h is at the next address
 * @a2: the client that holds the A2h address, so that no other takes it
 * @tx_disable: the hard transmit-disable input, requested asserted; NULL
 *	when the node names none
 * @optics: the optics the MAC takes
 * @lock: held by every user for the whole of its sequence, with the
 *	adapter's bus lock inside it
 * @adap: the adapter while @lock is held
 * @hwmon: the hwmon device, once registered
 * @refused: the adapter refused a write; reset by the next write it takes
 * @told_refused: the refusal was logged
 * @apcset0: the power control's set point found at the first bring-up since
 *	probe, that of transmit power level mode 0
 * @ref: @apcset0 is known
 * @mode: the transmit power level mode the optics are set to; U8_MAX when
 *	neither the mode asked for nor mode 0 could be set
 * @p0: the transmitted power at mode 0 in 0.1 uW, measured before the first
 *	lower mode of a session
 * @apcset: the set point at which each lower mode was last measured to
 *	reach its power, 0 for a mode not measured since probe; index 0 is
 *	unused, mode 0 being @apcset0
 * @t2_open: table 2 is open for storing through t2write, which keeps
 *	@t2_eectl and @t2_tabsel, EEPROM_CTRL and the page select as found
 * @t2_eectl: EEPROM_CTRL as found by t2write's "open"
 * @t2_tabsel: the page select as found by t2write's "open"
 *
 * Every field after @hwmon is under @lock.
 */
struct gn25l95 {
	struct i2c_client *client;
	struct i2c_client *a2;
	struct gpio_desc *tx_disable;
	struct pon_optics optics;
	struct mutex lock;
	struct i2c_adapter *adap;
	struct device *hwmon;
	bool refused;
	bool told_refused;
	u8 apcset0;
	bool ref;
	u8 mode;
	u16 p0;
	u8 apcset[GN25L95_LEVEL_MODES];
	bool t2_open;
	u8 t2_eectl;
	u8 t2_tabsel;
};

/*
 * A sequence holds the driver's lock and the adapter's bus lock inside it, so
 * that a client of the adapter, such as i2c-dev, cannot put a transfer
 * between two of the sequence's.
 */
static void gn25l95_lock(struct gn25l95 *gn)
{
	mutex_lock(&gn->lock);
	i2c_lock_bus(gn->client->adapter, I2C_LOCK_SEGMENT);
	gn->adap = gn->client->adapter;
}

static void gn25l95_unlock(struct gn25l95 *gn)
{
	gn->adap = NULL;
	i2c_unlock_bus(gn->client->adapter, I2C_LOCK_SEGMENT);
	mutex_unlock(&gn->lock);
}

DEFINE_GUARD(gn25l95, struct gn25l95 *, gn25l95_lock(_T), gn25l95_unlock(_T))

/* One SMBus transfer, in a sequence under gn25l95_lock(). */
static int gn25l95_xfer(struct gn25l95 *gn, u16 addr, char rw, u8 reg, int size,
			union i2c_smbus_data *data)
{
	lockdep_assert_held(&gn->lock);
	return __i2c_smbus_xfer(gn->adap, addr, 0, rw, reg, size, data);
}

/* The address and the byte of @reg, 0x000-0x1ff as above. */
static int gn25l95_addr(struct gn25l95 *gn, u16 reg, u16 *addr, u8 *sub)
{
	if (reg >= 0x200)
		return -EINVAL;
	*addr = gn->client->addr + (reg >> 8);
	*sub = reg & 0xff;
	return 0;
}

/**
 * gn25l95_read() - read one byte of the management interface
 * @gn: driver state
 * @reg: 0x000-0x0ff for A0h bytes 0-255, 0x100-0x1ff for A2h bytes 0-255
 * @val: set to the byte read
 *
 * Called in a sequence under gn25l95_lock().
 *
 * Return: 0, -EINVAL for a @reg above 0x1ff, or an error of the transfer.
 */
static int gn25l95_read(struct gn25l95 *gn, u16 reg, u8 *val)
{
	union i2c_smbus_data data;
	u16 addr;
	u8 sub;
	int err;

	err = gn25l95_addr(gn, reg, &addr, &sub) ?:
	      gn25l95_xfer(gn, addr, I2C_SMBUS_READ, sub, I2C_SMBUS_BYTE_DATA, &data);
	if (!err)
		*val = data.byte;
	return err;
}

/*
 * A0h and A2h bytes 0-127 take writes without a password and keep them
 * across a power cycle of the module: a write there is a permanent change
 * to it.  Of those bytes the driver writes the page select, 127, through
 * gn25l95_write_changed(), and only the store view writes the password,
 * 123-126, which reads back 00 whatever is written (measured on an ODI
 * DFP-34X-2C3).  A write the adapter refuses with -EPERM is noted.
 */
static int gn25l95_write(struct gn25l95 *gn, u16 reg, u8 val)
{
	union i2c_smbus_data data = { .byte = val };
	u16 addr;
	u8 sub;
	int err;

	err = gn25l95_addr(gn, reg, &addr, &sub) ?:
	      gn25l95_xfer(gn, addr, I2C_SMBUS_WRITE, sub, I2C_SMBUS_BYTE_DATA, &data);
	gn->refused = err == -EPERM;
	return err;
}

/*
 * Write a byte only when it holds another value, so that a byte kept in the
 * part's EEPROM, such as the page select that every activation sets and puts
 * back, is written only when it changes.
 */
static int gn25l95_write_changed(struct gn25l95 *gn, u16 reg, u8 val)
{
	u8 now;
	int err;

	err = gn25l95_read(gn, reg, &now);
	if (err || now == val)
		return err;
	return gn25l95_write(gn, reg, val);
}

/*
 * Read the word at A2h byte @a2, most significant byte first, in one transfer
 * of two bytes: SFF-8472 has a diagnostic read in one two-byte sequence,
 * during which the module does not update it.
 */
static int gn25l95_a2_word(struct gn25l95 *gn, unsigned int a2, u16 *val)
{
	union i2c_smbus_data data;
	u16 addr;
	u8 sub;
	int err;

	err = gn25l95_addr(gn, GN25L95_A2 + a2, &addr, &sub) ?:
	      gn25l95_xfer(gn, addr, I2C_SMBUS_READ, sub, I2C_SMBUS_WORD_DATA, &data);
	if (err)
		return err;
	/* The first byte read, the most significant, is the SMBus word's low byte. */
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
 * external calibration is applied: the module fitted to the ODI DFP-34X-2C3
 * holds in A2h 56-91 the constants SFF-8472 gives an internally calibrated
 * module (measured), so both readings agree.  Bias and transmitted power read
 * 0 while the laser does not emit, as below O3.
 *
 * The thresholds come from A2h 0-39, in the same units, and are reported
 * as sfp.c reports them: the alarm thresholds as crit and lcrit, the
 * warning thresholds as max and min.
 */
static int gn25l95_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct gn25l95 *gn = dev_get_drvdata(dev);
	enum gn25l95_ddm ddm;
	unsigned int a2;
	int limit;
	u16 raw;
	int err;

	switch (type) {
	case hwmon_temp:
		ddm = GN25L95_TEMP;
		limit = attr == hwmon_temp_crit ? GN25L95_HIGH_ALARM :
			attr == hwmon_temp_lcrit ? GN25L95_LOW_ALARM :
			attr == hwmon_temp_max ? GN25L95_HIGH_WARN :
			attr == hwmon_temp_min ? GN25L95_LOW_WARN : -1;
		break;
	case hwmon_in:
		ddm = GN25L95_VCC;
		limit = attr == hwmon_in_crit ? GN25L95_HIGH_ALARM :
			attr == hwmon_in_lcrit ? GN25L95_LOW_ALARM :
			attr == hwmon_in_max ? GN25L95_HIGH_WARN :
			attr == hwmon_in_min ? GN25L95_LOW_WARN : -1;
		break;
	case hwmon_curr:
		ddm = GN25L95_BIAS;
		limit = attr == hwmon_curr_crit ? GN25L95_HIGH_ALARM :
			attr == hwmon_curr_lcrit ? GN25L95_LOW_ALARM :
			attr == hwmon_curr_max ? GN25L95_HIGH_WARN :
			attr == hwmon_curr_min ? GN25L95_LOW_WARN : -1;
		break;
	case hwmon_power:
		ddm = channel ? GN25L95_RX : GN25L95_TX;
		limit = attr == hwmon_power_crit ? GN25L95_HIGH_ALARM :
			attr == hwmon_power_lcrit ? GN25L95_LOW_ALARM :
			attr == hwmon_power_max ? GN25L95_HIGH_WARN :
			attr == hwmon_power_min ? GN25L95_LOW_WARN : -1;
		break;
	default:
		return -EOPNOTSUPP;
	}

	a2 = limit < 0 ? GN25L95_A2_WORD(ddm) : GN25L95_A2_LIMIT(ddm, limit);
	scoped_guard(gn25l95, gn)
		err = gn25l95_a2_word(gn, a2, &raw);
	if (err)
		return err;

	switch (ddm) {
	case GN25L95_TEMP:
		*val = DIV_ROUND_CLOSEST((long)(s16)raw * 1000, 256);
		break;
	case GN25L95_VCC:
		*val = DIV_ROUND_CLOSEST((long)raw, 10);
		break;
	case GN25L95_BIAS:
		*val = DIV_ROUND_CLOSEST((long)raw, 500);
		break;
	default:
		*val = DIV_ROUND_CLOSEST((long)raw, 10);
		break;
	}
	return 0;
}

/* The labels drivers/net/phy/sfp.c gives the same readings. */
static int gn25l95_hwmon_read_string(struct device *dev,
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

static umode_t gn25l95_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type, u32 attr,
					int channel)
{
	return 0444;
}

/*
 * temp1, in0, curr1, power1 (transmitted) and power2 (received), each with
 * its four thresholds
 */
static const struct hwmon_channel_info * const gn25l95_hwmon_info[] = {
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

static const struct hwmon_ops gn25l95_hwmon_ops = {
	.is_visible	= gn25l95_hwmon_is_visible,
	.read		= gn25l95_hwmon_read,
	.read_string	= gn25l95_hwmon_read_string,
};

static const struct hwmon_chip_info gn25l95_hwmon_chip = {
	.ops	= &gn25l95_hwmon_ops,
	.info	= gn25l95_hwmon_info,
};

/*
 * Register the hwmon device "gn25l95" once one read of the temperature word
 * has succeeded, so that no device appears when nothing answers at A2h.
 */
static int gn25l95_hwmon_register(struct gn25l95 *gn)
{
	struct device *dev = &gn->client->dev;
	struct device *hwmon;
	u16 probe;
	int err;

	scoped_guard(gn25l95, gn)
		err = gn25l95_a2_word(gn, GN25L95_A2_WORD(GN25L95_TEMP), &probe);
	if (err)
		return err;

	hwmon = devm_hwmon_device_register_with_info(dev, "gn25l95", gn,
						     &gn25l95_hwmon_chip, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	gn->hwmon = hwmon;
	return 0;
}
#else
static int gn25l95_hwmon_register(struct gn25l95 *gn)
{
	return 0;
}
#endif

/*
 * The laser driver's configuration table ("table 2"), shown at A2h 128-255
 * while the page select, A2h byte 127, holds 02h.  The addresses are A2h byte
 * numbers.  Measured on the ODI DFP-34X-2C3: ER_CTRL_AUTO runs the automatic
 * extinction-ratio loop, which sets the modulation current, and ER_CTRL reads
 * 0xe3 after the module powers up; APCSET is the power control loop's set
 * point.  With both PERSIST_OFF bits set, register changes are not copied
 * into the part's EEPROM; with PERSIST at 0x00 every change is copied, the
 * diagnostic words included, and the part does not acknowledge on the bus
 * while a copy runs.  That module reads 0x03 in PERSIST.
 *
 * Nothing else is written for the table to take a write.  The text of the
 * GN25L95's datasheet places the access control of the upper page at 0xcc,
 * and the driver used to clear bits 3:2 there first; the register map of
 * the same datasheet places that control at 0xc4 and the PW1 password at
 * 0xc9-0xcc, so that write went to the password.  That module reads 00h at
 * 0xc4.  A write the table refuses shows in the read-backs of ER_CTRL and
 * APCSET.
 */
#define GN25L95_TBL2_ER_CTRL		0xa5
#define GN25L95_TBL2_ER_CTRL_AUTO		BIT(0)
#define GN25L95_TBL2_APCSET		0xa8
#define GN25L95_TBL2_PERSIST		0xc7
#define GN25L95_TBL2_PERSIST_OFF	(BIT(1) | BIT(0))

/*
 * Transmit power levelling (ITU-T G.984.2 clause 8.3.2): level mode 0 is the
 * power the module came with, modes 1 and 2 are 3 dB and 6 dB below it.  The
 * power control loop moves the bias until the monitor photodiode reaches the
 * set point, and the automatic extinction-ratio loop sets the modulation for
 * the bias it finds, so a lower level is set by the set point alone.  That is
 * moved one step at a time, never above the module's own, until the
 * transmitted power has crossed 2^-mode times the power measured at mode 0; a
 * power that falls to 0 on the way ends the search.  The module measures the
 * power of the bursts, which come often only from O4 on, so a lower level is
 * measured only there.  The set point at which it was reached is kept, and
 * before O4, where the ONU sends only its answers to serial-number requests, a
 * lower level is set to the kept set point without measuring; a level not
 * measured since probe is not set there.
 */

/* The power is read this long after each change. */
#define GN25L95_LEVEL_SETTLE_MS	20
/* A change that needs more bias than this, in 2 uA, is given up. */
#define GN25L95_LEVEL_BIAS_MAX	(40000 / 2)

/* The transmitted power and the bias, read GN25L95_LEVEL_SETTLE_MS on. */
static int gn25l95_tx_bias(struct gn25l95 *gn, u16 *tx, u16 *bias)
{
	msleep(GN25L95_LEVEL_SETTLE_MS);
	return gn25l95_a2_word(gn, GN25L95_A2_WORD(GN25L95_TX), tx) ?:
	       gn25l95_a2_word(gn, GN25L95_A2_WORD(GN25L95_BIAS), bias);
}

/*
 * Set level mode @mode, with the configuration table shown and ready for
 * writes.  Mode 0 is the set point found at the first bring-up since probe.
 * With @measure the set point of a lower mode is found with the laser sending
 * bursts: the power read while the optics are still at mode 0 is that of mode
 * 0, a power of 0 there gives -ENODATA, and the set point at which the power
 * crosses its target is kept for the mode.  Without @measure the kept set
 * point is written, and a mode that has none gives -ENODATA with nothing
 * written.  A set point that would pass the module's own, a power that falls
 * to 0 or a bias above GN25L95_LEVEL_BIAS_MAX gives -ERANGE; the caller then
 * puts mode 0 back.
 */
static int gn25l95_level_set(struct gn25l95 *gn, unsigned int mode,
			     bool measure)
{
	const u16 apc_reg = GN25L95_A2 + GN25L95_TBL2_APCSET;
	u16 tx, bias, target;
	int apc, step, i, err;
	u8 cur;

	if (!mode)
		return gn25l95_write(gn, apc_reg, gn->apcset0);
	if (!measure)
		return gn->apcset[mode] ?
		       gn25l95_write(gn, apc_reg, gn->apcset[mode]) : -ENODATA;

	err = gn25l95_read(gn, apc_reg, &cur) ?:
	      gn25l95_tx_bias(gn, &tx, &bias);
	if (err)
		return err;
	if (!gn->mode) {
		if (!tx)
			return -ENODATA;
		gn->p0 = tx;
	}
	if (!gn->p0)
		return -ENODATA;
	target = gn->p0 >> mode;

	apc = cur;
	step = tx > target ? -1 : 1;
	for (i = 0; i <= U8_MAX; i++) {
		if (!tx || bias > GN25L95_LEVEL_BIAS_MAX)
			return -ERANGE;
		if (step < 0 ? tx <= target : tx >= target) {
			gn->apcset[mode] = apc;
			return 0;
		}
		apc += step;
		if (apc < 0 || apc > gn->apcset0)
			return -ERANGE;
		err = gn25l95_write(gn, apc_reg, apc) ?:
		      gn25l95_tx_bias(gn, &tx, &bias);
		if (err)
			return err;
	}
	return -ERANGE;
}

/*
 * Show the configuration table and check that its writes stay out of the
 * EEPROM: -EPERM unless both PERSIST_OFF bits are set.  The caller has read
 * the page select before, and puts it back with gn25l95_t2_hide() whatever
 * this returns.
 */
static int gn25l95_t2_show(struct gn25l95 *gn)
{
	u8 eectl;
	int err;

	err = gn25l95_write_changed(gn, GN25L95_A2 + GN25L95_A2_PAGE, 0x02) ?:
	      gn25l95_read(gn, GN25L95_A2 + GN25L95_TBL2_PERSIST, &eectl);
	if (!err && (eectl & GN25L95_TBL2_PERSIST_OFF) != GN25L95_TBL2_PERSIST_OFF)
		err = -EPERM;
	return err;
}

/* Put the page select back as it was found; that write is not checked. */
static void gn25l95_t2_hide(struct gn25l95 *gn, u8 tabsel)
{
	gn25l95_write_changed(gn, GN25L95_A2 + GN25L95_A2_PAGE, tabsel);
}

/*
 * Put level mode 0 back when the system restarts, powers off or halts: the
 * module keeps its registers across a warm reboot of the SoC, and the next
 * kernel takes the set point it finds at its first bring-up for the module's
 * own.  A reset that shuts no device down, such as the watchdog's, leaves a
 * lower level in place until the module loses power, and a level asked for
 * after it is counted down from that lower one.
 */
static void gn25l95_shutdown(struct i2c_client *client)
{
	struct gn25l95 *gn = i2c_get_clientdata(client);
	u8 tabsel;
	int err;

	guard(gn25l95)(gn);
	if (!gn->ref || !gn->mode)
		return;
	err = gn25l95_read(gn, GN25L95_A2 + GN25L95_A2_PAGE, &tabsel);
	if (!err) {
		err = gn25l95_t2_show(gn) ?:
		      gn25l95_level_set(gn, 0, false);
		gn25l95_t2_hide(gn, tabsel);
	}
	if (!err)
		gn->mode = 0;
	dev_info(&client->dev, "transmit power %s level mode 0 before the restart\n",
		 err ? "not put back at" : "back at");
}

/*
 * The adapter refused a write: the bus does not let the part be written, and
 * the driver only reads it.  Logged once; the next write is tried all the
 * same, since the bus may allow it then.
 */
static int gn25l95_refused(struct gn25l95 *gn, int err)
{
	if (err != -EPERM || !gn->refused)
		return err;
	if (!gn->told_refused)
		dev_info(&gn->client->dev, "the bus refuses writes; the laser driver is only read\n");
	gn->told_refused = true;
	return 0;
}

/**
 * gn25l95_set_power_level() - set the optics to a transmit power level mode
 * @optics: &gn25l95.optics
 * @mode: the mode the MAC asks for
 * @measure: the laser sends bursts often enough for its power to be measured
 * @now: set to the mode the optics are at afterwards
 *
 * Sets @mode with the configuration table shown and its writes kept out of
 * the EEPROM as at bring-up; a failure puts mode 0 back. Nothing is written
 * before the first bring-up has kept the module's own set point, nor, without
 * @measure, for a lower mode not measured since probe: the optics then stay
 * where they are.
 *
 * Context: process context.  May sleep, with the driver's lock held, for up
 * to about six seconds with @measure: 20 ms or more for each step of the set
 * point, of which there are at most 256.
 * Return: 0; -EOPNOTSUPP before the first bring-up or for a mode above 2;
 * -ENODATA without @measure for a mode not measured since probe; -EPERM when
 * table writes would reach the EEPROM or the bus refuses them; -ENODATA or
 * -ERANGE when the level could not be set; or the error of a failed transfer.
 */
static int gn25l95_set_power_level(struct pon_optics *optics, unsigned int mode,
				   bool measure, unsigned int *now)
{
	struct gn25l95 *gn = container_of(optics, struct gn25l95, optics);
	u16 tx = 0, bias = 0;
	u8 tabsel, apc = 0;
	int err;

	scoped_guard(gn25l95, gn) {
		if (mode == gn->mode) {
			*now = mode;
			return 0;
		}
		*now = gn->mode < GN25L95_LEVEL_MODES ? gn->mode : 0;
		if (!gn->ref || mode >= GN25L95_LEVEL_MODES)
			return -EOPNOTSUPP;
		if (!measure && mode && !gn->apcset[mode])
			return -ENODATA;

		err = gn25l95_read(gn, GN25L95_A2 + GN25L95_A2_PAGE, &tabsel);
		if (err)
			return err;
		err = gn25l95_t2_show(gn);
		if (!err) {
			err = gn25l95_level_set(gn, mode, measure);
			if (!err) {
				gn->mode = mode;
			} else if (gn25l95_level_set(gn, 0, false)) {
				/*
				 * Neither the mode nor mode 0: the optics are
				 * at no known level, which the next request and
				 * the next bring-up both try to leave.
				 */
				dev_err(&gn->client->dev,
					"transmit power level not restored to mode 0\n");
				gn->mode = U8_MAX;
			} else {
				gn->mode = 0;
			}
		}
		gn25l95_t2_hide(gn, tabsel);
		if (!err && measure)
			gn25l95_tx_bias(gn, &tx, &bias);
		if (!err)
			apc = mode ? gn->apcset[mode] : gn->apcset0;
		*now = gn->mode < GN25L95_LEVEL_MODES ? gn->mode : 0;
	}

	/*
	 * Before ranging the ONU may step through the modes every few
	 * serial-number requests for as long as the OLT does not answer, so
	 * that line is rate-limited.
	 */
	if (!err && measure)
		dev_info(&gn->client->dev,
			 "transmit power level mode %u: %u.%u uW, bias %u.%02u mA, set point %02x\n",
			 *now, tx / 10, tx % 10, bias / 500, bias % 500 / 5,
			 apc);
	else if (!err)
		dev_info_ratelimited(&gn->client->dev,
				     "transmit power level mode %u at set point %02x, not measured\n",
				     *now, apc);
	return err;
}

/*
 * The factory configuration, under the driver's lock; see gn25l95_start().
 */
static int __gn25l95_factory(struct gn25l95 *gn)
{
	const u16 er_reg = GN25L95_A2 + GN25L95_TBL2_ER_CTRL;
	const u16 apc_reg = GN25L95_A2 + GN25L95_TBL2_APCSET;
	struct device *dev = &gn->client->dev;
	u8 tabsel, eectl, er, apc, apc0, rb_er, rb_apc;
	int err;

	err = gn25l95_read(gn, GN25L95_A2 + GN25L95_A2_PAGE, &tabsel);
	if (err)
		return err;

	/* Show the table; from here every path puts the page select back. */
	err = gn25l95_write_changed(gn, GN25L95_A2 + GN25L95_A2_PAGE, 0x02) ?:
	      gn25l95_read(gn, GN25L95_A2 + GN25L95_TBL2_PERSIST, &eectl) ?:
	      gn25l95_read(gn, er_reg, &er) ?:
	      gn25l95_read(gn, apc_reg, &apc);
	if (err)
		goto out;

	/*
	 * Unless both PERSIST_OFF bits are set, the part could copy a table
	 * write into its EEPROM: nothing in the table is written then.
	 */
	if ((eectl & GN25L95_TBL2_PERSIST_OFF) != GN25L95_TBL2_PERSIST_OFF) {
		dev_warn(dev, "EEPROM updates are enabled (C7h %02x); the configuration table is left alone rather than written to the EEPROM\n",
			 eectl);
		err = -EPERM;
		goto out;
	}

	/*
	 * The set point of level mode 0 is the one found at the first bring-up
	 * since probe, and writing it undoes a level set since.  The loop is
	 * turned on whatever it is found at.  Both are written every time, so
	 * that a bus that refuses writes shows here, and read back.
	 */
	apc0 = gn->ref ? gn->apcset0 : apc;
	err = gn25l95_write(gn, apc_reg, apc0) ?:
	      gn25l95_write(gn, er_reg, er | GN25L95_TBL2_ER_CTRL_AUTO) ?:
	      gn25l95_read(gn, er_reg, &rb_er) ?:
	      gn25l95_read(gn, apc_reg, &rb_apc);
	if (err)
		goto out;
	if (!(rb_er & GN25L95_TBL2_ER_CTRL_AUTO) || rb_apc != apc0) {
		dev_warn(dev, "the factory configuration did not take: ER control %02x, set point %02x\n",
			 rb_er, rb_apc);
		err = -EIO;
		goto out;
	}

	gn->apcset0 = apc0;
	gn->ref = true;
	gn->mode = 0;
	dev_info(dev, "the module runs its factory configuration: automatic ER loop on (ER control %02x -> %02x), set point %02x -> %02x\n",
		 er, rb_er, apc, rb_apc);
out:
	/* Leave the page select as it was found. */
	gn25l95_t2_hide(gn, tabsel);
	return err;
}

/**
 * gn25l95_start() - put the module's factory configuration back
 * @optics: &gn25l95.optics
 *
 * The part loads its configuration table from its EEPROM at power-up, the
 * automatic extinction-ratio loop on among it, and runs from it.  The table's
 * registers keep what is written to them across a warm reboot of the SoC, so
 * a kernel can find the loop off, or a lower set point, left by an earlier
 * one.  The MAC calls this before an activation, after its own reset and
 * before the first O3: it turns the loop on, and writes the power control's
 * set point of transmit power level mode 0, which is the one the first call
 * since probe found.
 *
 * Whether that set point is the module's own cannot be told from the
 * registers: after a reset that shut no device down while the ONU sent at a
 * lower level, it is that level's.  The module's own is in its EEPROM, which
 * the part's reset loads into the table again; but the reset writes its key
 * to A2h bytes 123-127, a range whose writes the module keeps across a power
 * cycle, so it is not used here.
 *
 * The table's writes stay in registers, kept out of the EEPROM by PERSIST: a
 * power cycle gives the module its own configuration back.  PERSIST is in the
 * table, which the part shows once the page select holds 02h, and the table
 * needs no password bytes written (measured).  So a call that gives up with
 * -EPERM has written only the page select, when it did not hold 02h, which
 * the part keeps in its EEPROM; the table is written only once PERSIST keeps
 * register changes out of the EEPROM.  Once the page select has been set,
 * every path puts it back as found, without checking that write.
 *
 * When the bus refuses the writes, this returns 0 having written nothing, and
 * the first refusal is logged.
 *
 * Context: process context.  May sleep.
 * Return: 0 when the module runs its factory configuration or the bus refuses
 * writes; -EPERM when table writes would reach the EEPROM; -EIO when the loop
 * or the set point does not read back as written; otherwise the error of a
 * failed transfer.
 */
static int gn25l95_start(struct pon_optics *optics)
{
	struct gn25l95 *gn = container_of(optics, struct gn25l95, optics);

	guard(gn25l95)(gn);
	return gn25l95_refused(gn, __gn25l95_factory(gn));
}

/**
 * gn25l95_tx_disable() - drive the transmitter's hard TX_DISABLE input
 * @optics: &gn25l95.optics
 * @disable: true to assert TX_DISABLE (no light), false to release it
 *
 * The line is requested asserted at probe.  The MAC asserts it in every
 * activation state below O3 and releases it from O3 on, where an ONU first
 * transmits (ITU-T G.984.3 10.2); the MAC still keys each burst.  Without a
 * tx-disable GPIO in the node this does nothing.  Asserting it stops the
 * light: in O5 the module's bias and transmitted power read 0 within a second,
 * and the OLT deactivated the ONU (measured on the ODI DFP-34X-2C3).
 *
 * The soft TX disable of SFF-8472 (A2h byte 110, bit 6) is not used: A2h
 * bytes 0-127 are kept in the part's EEPROM, so every change would be an
 * EEPROM write.
 *
 * Context: process context.  May sleep.
 * Return: 0.
 */
static int gn25l95_tx_disable(struct pon_optics *optics, bool disable)
{
	struct gn25l95 *gn = container_of(optics, struct gn25l95, optics);

	gpiod_set_value_cansleep(gn->tx_disable, disable);
	return 0;
}

static const struct pon_optics_ops gn25l95_optics_ops = {
	.tx_disable		= gn25l95_tx_disable,
	.start			= gn25l95_start,
	.set_power_level	= gn25l95_set_power_level,
};

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * The "a2" view: A2h bytes 0-127, each read on its own, then the five
 * diagnostic words in SFF-8472 units, the password bytes 123-126 and the
 * page select, byte 127.  A failed read ends the view with its error.  It
 * only reads.
 */
static int gn25l95_a2_show(struct seq_file *s, void *unused)
{
	struct gn25l95 *gn = s->private;
	unsigned int i;
	u8 b[0x80];
	int err = 0;
	u16 w[5];
	int mdeg;

	scoped_guard(gn25l95, gn) {
		for (i = 0; i < sizeof(b) && !err; i++)
			err = gn25l95_read(gn, GN25L95_A2 + i, &b[i]);
		for (i = 0; i < ARRAY_SIZE(w) && !err; i++)
			err = gn25l95_a2_word(gn, GN25L95_A2_WORD(i), &w[i]);
	}
	if (err) {
		seq_printf(s, "A2h read failed: %d\n", err);
		return 0;
	}

	for (i = 0; i < sizeof(b); i += 16)
		seq_printf(s, "%02x: %*ph\n", i, 16, b + i);

	/*
	 * The five words are read again, each in a way that gives a sample the
	 * module held (gn25l95_a2_word()), so they may be newer than the bytes
	 * above.  The received power shows whether light reaches the receiver,
	 * which a MAC's loss of signal cannot tell: that alarm is the same for
	 * no light and for light the receiver does not recover.
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
	 * writes 0xff to 123-126, and on the ODI DFP-34X-2C3 they read 00
	 * whatever was written (measured).
	 */
	seq_printf(s, "\npassword 0x7b-0x7e  %02x %02x %02x %02x\n",
		   b[GN25L95_A2_PASSWORD], b[GN25L95_A2_PASSWORD + 1],
		   b[GN25L95_A2_PASSWORD + 2], b[GN25L95_A2_PASSWORD + 3]);
	seq_printf(s, "table select 0x7f = 0x%02x\n", b[GN25L95_A2_PAGE]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gn25l95_a2);

/*
 * The "eeblocks" view: all 256 bytes at each of the part's address and the
 * three after it, placed in a 1 KiB image, with the number of reads that
 * failed at each address ("silent" when all did).  A failed read, not
 * acknowledged or timed out, shows as 00.  On the ODI DFP-34X-2C3 0x52 and
 * 0x53 acknowledge but read 0xff throughout (measured): the module's stored
 * configuration cannot be read through them.  The view only reads, so A2h
 * 128-255 show whichever page is selected at the time.
 */
static int gn25l95_eeblocks_show(struct seq_file *s, void *unused)
{
	struct gn25l95 *gn = s->private;
	union i2c_smbus_data data;
	unsigned int blk, i;
	u8 b[0x100];
	int nack;
	u16 addr;

	for (blk = 0; blk < 4; blk++) {
		addr = gn->client->addr + blk;
		nack = 0;
		scoped_guard(gn25l95, gn) {
			for (i = 0; i < sizeof(b); i++) {
				if (gn25l95_xfer(gn, addr, I2C_SMBUS_READ, i,
						 I2C_SMBUS_BYTE_DATA, &data)) {
					b[i] = 0;
					nack++;
				} else {
					b[i] = data.byte;
				}
			}
		}
		seq_printf(s, "=== i2c 0x%02x -- image 0x%03x -- %d of 256 did not answer\n",
			   addr, blk * 0x100, nack);
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
DEFINE_SHOW_ATTRIBUTE(gn25l95_eeblocks);

/*
 * The "tables" view: A2h bytes 128-255 for each table the page select can
 * show, 0, 1, 2, 4, 5 and 6.  Table 2 holds the laser driver's
 * configuration, among it the receiver's loss-of-signal control (BBh) and
 * the APD bias control (BDh); table 6 the temperature indexed APD DAC
 * values at C0h-FFh.  The page select is put back as it was, also when a
 * table fails.  The page select is kept in the part's EEPROM, so every
 * read of the view writes it; on a bus that refuses writes only the table
 * selected at the time is shown.
 */
static int gn25l95_tables_show(struct seq_file *s, void *unused)
{
	static const u8 tables[] = { 0, 1, 2, 4, 5, 6 };
	struct gn25l95 *gn = s->private;
	unsigned int t, i;
	u8 b[0x80], was;
	int err;

	guard(gn25l95)(gn);
	err = gn25l95_read(gn, GN25L95_A2 + GN25L95_A2_PAGE, &was);
	if (err) {
		seq_printf(s, "the page select could not be read: %d\n", err);
		return 0;
	}
	for (t = 0; t < ARRAY_SIZE(tables); t++) {
		err = gn25l95_write_changed(gn, GN25L95_A2 + GN25L95_A2_PAGE, tables[t]);
		for (i = 0; i < sizeof(b) && !err; i++)
			err = gn25l95_read(gn, GN25L95_A2 + 0x80 + i, &b[i]);
		if (err) {
			seq_printf(s, "=== table %u: failed: %d\n\n", tables[t], err);
			continue;
		}
		seq_printf(s, "=== table %u\n", tables[t]);
		for (i = 0; i < sizeof(b); i += 16)
			seq_printf(s, "%02x: %*ph\n", 0x80 + i, 16, b + i);
		seq_puts(s, "\n");
	}
	err = gn25l95_write_changed(gn, GN25L95_A2 + GN25L95_A2_PAGE, was);
	if (err)
		seq_printf(s, "the page select was not put back to %02x: %d\n", was, err);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gn25l95_tables);

/*
 * The "t2write" entry stores bytes of the laser driver's configuration table
 * (table 2) in the part's EEPROM, and resets the part.  Root writes one
 * command at a time:
 *
 * "open" writes the password bytes (gn25l95_t2_unlock()), shows table 2 and
 * clears both bits of EEPROM_CTRL (C7h), so that the part copies the changes
 * of its registers into its EEPROM; C7h and the page select as found are
 * kept for "close".
 *
 * "REG VALUE", both hexadecimal, after "open": writes the password bytes and
 * shows table 2 again, so that nothing else that switched the page in between
 * sends the byte to another table, then writes VALUE at table 2 byte REG.
 * The byte is not read back: while the part copies into its EEPROM it does
 * not acknowledge and reads return a constant (measured), so a read-back then
 * proves nothing.  A refused transfer is retried for a second.
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
 * the module since (measured).
 */
#define GN25L95_RESET_MS	500
#define GN25L95_BUSY_TRIES	20
#define GN25L95_BUSY_MS		50

static bool gn25l95_t2_storable(unsigned int reg)
{
	return (reg >= 0xad && reg <= 0xaf) || (reg >= 0xb6 && reg <= 0xb8) ||
	       reg == 0xbb || reg == 0xbd || reg == 0xc1;
}

/* A write the part may refuse while it copies into its EEPROM. */
static int gn25l95_write_busy(struct gn25l95 *gn, u16 reg, u8 val)
{
	int err, i;

	for (i = 0; i < GN25L95_BUSY_TRIES; i++) {
		err = gn25l95_write(gn, reg, val);
		if (!err || err == -EPERM)
			break;
		msleep(GN25L95_BUSY_MS);
	}
	return err;
}

/* The password bytes and table 2 shown, each write retried while the part is busy. */
static int gn25l95_t2_unlock(struct gn25l95 *gn)
{
	int err = 0, i;

	for (i = 0; i < 4 && !err; i++)
		err = gn25l95_write_busy(gn, GN25L95_A2 + GN25L95_A2_PASSWORD + i, 0xff);
	return err ?: gn25l95_write_busy(gn, GN25L95_A2 + GN25L95_A2_PAGE, 0x02);
}

static int gn25l95_t2_open(struct gn25l95 *gn)
{
	u8 tabsel, eectl;
	int err;

	if (gn->t2_open)
		return -EBUSY;
	err = gn25l95_read(gn, GN25L95_A2 + GN25L95_A2_PAGE, &tabsel);
	if (err)
		return err;
	err = gn25l95_t2_unlock(gn) ?:
	      gn25l95_read(gn, GN25L95_A2 + GN25L95_TBL2_PERSIST, &eectl) ?:
	      gn25l95_write(gn, GN25L95_A2 + GN25L95_TBL2_PERSIST,
			    eectl & ~GN25L95_TBL2_PERSIST_OFF);
	if (err) {
		gn25l95_write_busy(gn, GN25L95_A2 + GN25L95_A2_PAGE, tabsel);
		return err;
	}
	gn->t2_tabsel = tabsel;
	gn->t2_eectl = eectl;
	gn->t2_open = true;
	dev_info(&gn->client->dev, "table 2 open for storing (C7h %02x -> %02x)\n",
		 eectl, (u8)(eectl & ~GN25L95_TBL2_PERSIST_OFF));
	return 0;
}

static int gn25l95_t2_close(struct gn25l95 *gn)
{
	int err;

	if (!gn->t2_open)
		return -EINVAL;
	err = gn25l95_t2_unlock(gn) ?:
	      gn25l95_write_busy(gn, GN25L95_A2 + GN25L95_TBL2_PERSIST, gn->t2_eectl) ?:
	      gn25l95_write_busy(gn, GN25L95_A2 + GN25L95_A2_PAGE, gn->t2_tabsel);
	if (err)
		return err;
	gn->t2_open = false;
	dev_info(&gn->client->dev, "table 2 closed (C7h %02x)\n", gn->t2_eectl);
	return 0;
}

static int gn25l95_t2_store(struct gn25l95 *gn, u8 reg, u8 val)
{
	int err;

	if (!gn->t2_open)
		return -EINVAL;
	err = gn25l95_t2_unlock(gn) ?:
	      gn25l95_write_busy(gn, GN25L95_A2 + reg, val);
	if (!err)
		dev_info(&gn->client->dev, "table 2 %02Xh written %02x\n", reg, val);
	return err;
}

static int gn25l95_reset(struct gn25l95 *gn)
{
	static const u8 key[] = { 0x5d, 0x2c, 0x6a, 0xc9 };
	u8 tabsel;
	int err, i;

	if (gn->t2_open)
		return -EBUSY;
	err = gn25l95_read(gn, GN25L95_A2 + GN25L95_A2_PAGE, &tabsel);
	for (i = 0; i < ARRAY_SIZE(key) && !err; i++)
		err = gn25l95_write(gn, GN25L95_A2 + GN25L95_A2_PASSWORD + i, key[i]);
	if (err)
		return err;
	err = gn25l95_write_changed(gn, GN25L95_A2 + GN25L95_A2_PAGE, 0x0b);
	msleep(GN25L95_RESET_MS);
	for (i = 0; i < 4 && !err; i++)
		err = gn25l95_write_busy(gn, GN25L95_A2 + GN25L95_A2_PASSWORD + i, 0xff);
	err = err ?: gn25l95_write_busy(gn, GN25L95_A2 + GN25L95_A2_PAGE, tabsel);
	if (!err)
		dev_info(&gn->client->dev, "reset, the tables loaded from the EEPROM\n");
	return err;
}

static ssize_t gn25l95_t2write_write(struct file *file, const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	struct gn25l95 *gn = file->private_data;
	unsigned int reg, val;
	char buf[16];
	int err;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	guard(gn25l95)(gn);
	if (sysfs_streq(buf, "open"))
		err = gn25l95_t2_open(gn);
	else if (sysfs_streq(buf, "close"))
		err = gn25l95_t2_close(gn);
	else if (sysfs_streq(buf, "reset"))
		err = gn25l95_reset(gn);
	else if (sscanf(buf, "%x %x", &reg, &val) == 2 && val <= 0xff &&
		 gn25l95_t2_storable(reg))
		err = gn25l95_t2_store(gn, reg, val);
	else
		err = -EINVAL;
	return err ?: len;
}

static const struct file_operations gn25l95_t2write_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gn25l95_t2write_write,
	.llseek = noop_llseek,
};

/*
 * The views go in the directory the I2C core gives the client, and go with
 * it.  "a2" and "eeblocks" only read; "tables" writes the page select;
 * "t2write", mode 0200, stores table 2 bytes in the part's EEPROM, where they
 * outlive every power cycle, among them the rogue ONU controls, and exists
 * only with CONFIG_PON_GN25L95_STORE.
 */
static void gn25l95_debugfs_init(struct gn25l95 *gn)
{
	struct dentry *dir = gn->client->debugfs;

	debugfs_create_file("a2", 0400, dir, gn, &gn25l95_a2_fops);
	debugfs_create_file("eeblocks", 0400, dir, gn, &gn25l95_eeblocks_fops);
	debugfs_create_file("tables", 0400, dir, gn, &gn25l95_tables_fops);
	if (IS_ENABLED(CONFIG_PON_GN25L95_STORE))
		debugfs_create_file("t2write", 0200, dir, gn, &gn25l95_t2write_fops);
}
#else
static void gn25l95_debugfs_init(struct gn25l95 *gn) { }
#endif

static int gn25l95_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gn25l95 *gn;
	int err;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_READ_WORD_DATA))
		return -ENODEV;

	gn = devm_kzalloc(dev, sizeof(*gn), GFP_KERNEL);
	if (!gn)
		return -ENOMEM;
	gn->client = client;
	err = devm_mutex_init(dev, &gn->lock);
	if (err)
		return err;
	i2c_set_clientdata(client, gn);

	/* Asserted at once: nothing lases before the MAC allows it. */
	gn->tx_disable = devm_gpiod_get_optional(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(gn->tx_disable))
		return dev_err_probe(dev, PTR_ERR(gn->tx_disable),
				     "cannot get the tx-disable gpio\n");

	gn->a2 = devm_i2c_new_dummy_device(dev, client->adapter, client->addr + 1);
	if (IS_ERR(gn->a2))
		return dev_err_probe(dev, PTR_ERR(gn->a2), "cannot take the A2h address\n");

	err = gn25l95_hwmon_register(gn);
	if (err)
		dev_warn(dev, "no diagnostics: %pe\n", ERR_PTR(err));
	gn25l95_debugfs_init(gn);

	/* Last: the MAC may use the optics from here on. */
	gn->optics.ops = &gn25l95_optics_ops;
	return devm_pon_optics_register(dev, &gn->optics);
}

static const struct of_device_id gn25l95_of_match[] = {
	{ .compatible = "semtech,gn25l95" },
	{ }
};
MODULE_DEVICE_TABLE(of, gn25l95_of_match);

static const struct i2c_device_id gn25l95_id[] = {
	{ "gn25l95" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gn25l95_id);

static struct i2c_driver gn25l95_driver = {
	.driver = {
		.name		= "gn25l95",
		.of_match_table	= gn25l95_of_match,
	},
	.probe		= gn25l95_probe,
	.shutdown	= gn25l95_shutdown,
	.id_table	= gn25l95_id,
};
module_i2c_driver(gn25l95_driver);

MODULE_DESCRIPTION("Semtech GN25L95 PON laser driver and limiting amplifier");
MODULE_AUTHOR("Paweł Phan <szop.hektar_1w@icloud.com>");
MODULE_LICENSE("GPL");
