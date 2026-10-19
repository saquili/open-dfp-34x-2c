// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C master of the Realtek RTL9602C, the bus of the optics
 *
 * The SoC has two I2C masters with the same set of registers, in the
 * register window of its switch. The laser driver of the SFP module answers
 * on master 1 only (through master 0 its addresses read zeros), so this
 * driver runs master 1. The switch driver maps the window and makes the
 * auxiliary device this driver binds to (include/linux/platform_data/
 * i2c-rtl9602c.h).
 *
 * A transfer moves a device address, one register-address byte and one data
 * byte, or two for a word that the module must not change in the middle, so
 * the adapter offers SMBus byte data and word data and nothing else. A word
 * is read in one transfer, the first byte read in the low byte, as SMBus
 * has it. A client's write is passed on only if the switch driver allows it
 * (@writable), so that a module it does not know is only read.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_data/i2c-rtl9602c.h>

/* The optics hang on master 1; master 0 has its own registers, 4 below. */
#define RTL9602C_I2C_MASTER		1
#define RTL9602C_I2C_XFER_CFG		(0x023004 + 4 * RTL9602C_I2C_MASTER)
#define RTL9602C_I2C_XFER_CFG_BUS_EN		BIT(21)
#define RTL9602C_I2C_XFER_CFG_DEV_ADDR	GENMASK(20, 14)
#define RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH	GENMASK(13, 12)
#define RTL9602C_I2C_XFER_CFG_DATA_WIDTH	GENMASK(11, 10)
#define RTL9602C_I2C_XFER_CFG_CLK_DIV	GENMASK(9, 0)
#define RTL9602C_I2C_TX_DATA		(0x0000b0 + 4 * RTL9602C_I2C_MASTER)
#define RTL9602C_I2C_SUBADDR		(0x0000b8 + 4 * RTL9602C_I2C_MASTER)
#define RTL9602C_I2C_CMD		(0x0000c0 + 4 * RTL9602C_I2C_MASTER)
#define RTL9602C_I2C_RX_DATA		(0x0000c8 + 4 * RTL9602C_I2C_MASTER)
#define RTL9602C_I2C_CMD_NACK		BIT(3)
#define RTL9602C_I2C_CMD_BUSY		BIT(2)
#define RTL9602C_I2C_CMD_WRITE		BIT(1)
#define RTL9602C_I2C_CMD_START		BIT(0)

/* The pad function enables, which hold those of other functions too. */
#define RTL9602C_PADMUX_FUNC_EN		0x023018
#define RTL9602C_PADMUX_FUNC_EN_I2C	BIT(13 + RTL9602C_I2C_MASTER)

#define RTL9602C_I2C_XFER_CFG_WIDTH_8BIT	0
#define RTL9602C_I2C_XFER_CFG_WIDTH_16BIT	1
/* 100 kHz, from the clock the divider counts */
#define RTL9602C_I2C_XFER_CFG_CLK_DIV_100KHZ	311

/* Poll BUSY 1 us apart for up to 2 ms: a byte at 100 kHz is 460 us. */
#define RTL9602C_I2C_POLL_US		1
#define RTL9602C_I2C_TIMEOUT_US		2000

/**
 * struct rtl9602c_i2c - the master
 * @adap: its adapter
 * @data: what the switch driver gave
 */
struct rtl9602c_i2c {
	struct i2c_adapter adap;
	struct rtl9602c_i2c_data *data;
};

static u32 rtl9602c_i2c_r(struct rtl9602c_i2c *i2c, u32 off)
{
	return readl(i2c->data->reg + off);
}

static void rtl9602c_i2c_w(struct rtl9602c_i2c *i2c, u32 off, u32 val)
{
	writel(val, i2c->data->reg + off);
}

/* Change bits of a register in the window; the I2C core serializes callers. */
static void rtl9602c_i2c_rmw(struct rtl9602c_i2c *i2c, u32 off, u32 clr, u32 set)
{
	rtl9602c_i2c_w(i2c, off, (rtl9602c_i2c_r(i2c, off) & ~clr) | set);
}

/*
 * BUSY is set while a transfer runs. The last command word read, with its
 * NACK bit, is left in @cmd. The wait does not sleep: without high
 * resolution timers, as in the board's configuration, a sleep lasts at
 * least a tick, 4 ms at 250 Hz, where a transfer takes about half a
 * millisecond.
 */
static int rtl9602c_i2c_wait(struct rtl9602c_i2c *i2c, u32 *cmd)
{
	return read_poll_timeout_atomic(rtl9602c_i2c_r, *cmd,
					!(*cmd & RTL9602C_I2C_CMD_BUSY),
					RTL9602C_I2C_POLL_US, RTL9602C_I2C_TIMEOUT_US,
					false, i2c, RTL9602C_I2C_CMD);
}

/*
 * Describe the next transfer: the 7-bit device address, one register-address
 * byte, @width for the data bytes, and the SCL divider for 100 kHz. Without
 * BUS_EN the master runs the transfer without driving the bus: it ends
 * without NACK and every read returns the same byte. Bits 31:22 are left as
 * they are.
 */
static void rtl9602c_i2c_setup(struct rtl9602c_i2c *i2c, u16 slave, u32 width)
{
	rtl9602c_i2c_rmw(i2c, RTL9602C_I2C_XFER_CFG,
			 RTL9602C_I2C_XFER_CFG_BUS_EN | RTL9602C_I2C_XFER_CFG_DEV_ADDR |
			 RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH |
			 RTL9602C_I2C_XFER_CFG_DATA_WIDTH | RTL9602C_I2C_XFER_CFG_CLK_DIV,
			 RTL9602C_I2C_XFER_CFG_BUS_EN |
			 FIELD_PREP(RTL9602C_I2C_XFER_CFG_DEV_ADDR, slave) |
			 FIELD_PREP(RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH,
				    RTL9602C_I2C_XFER_CFG_WIDTH_8BIT) |
			 FIELD_PREP(RTL9602C_I2C_XFER_CFG_DATA_WIDTH, width) |
			 FIELD_PREP(RTL9602C_I2C_XFER_CFG_CLK_DIV,
				    RTL9602C_I2C_XFER_CFG_CLK_DIV_100KHZ));
}

/* Run the transfer set up, with the command @cmd; -ENXIO if it is not acknowledged. */
static int rtl9602c_i2c_run(struct rtl9602c_i2c *i2c, u32 cmd)
{
	u32 status;
	int err;

	rtl9602c_i2c_w(i2c, RTL9602C_I2C_CMD, cmd);
	err = rtl9602c_i2c_wait(i2c, &status);
	if (err)
		return err;
	return status & RTL9602C_I2C_CMD_NACK ? -ENXIO : 0;
}

static s32 rtl9602c_i2c_smbus_xfer(struct i2c_adapter *adap, u16 addr,
				   unsigned short flags, char read_write,
				   u8 command, int size,
				   union i2c_smbus_data *data)
{
	struct rtl9602c_i2c *i2c = i2c_get_adapdata(adap);
	struct rtl9602c_i2c_data *pd = i2c->data;
	int err;

	if (size != I2C_SMBUS_BYTE_DATA && size != I2C_SMBUS_WORD_DATA)
		return -EOPNOTSUPP;
	if (read_write != I2C_SMBUS_READ) {
		/* A word is only read: the module's words are read as one. */
		if (size == I2C_SMBUS_WORD_DATA)
			return -EOPNOTSUPP;
		if (!pd->writable || !pd->writable(pd->priv))
			return -EPERM;
	}

	rtl9602c_i2c_setup(i2c, addr,
			   size == I2C_SMBUS_WORD_DATA ? RTL9602C_I2C_XFER_CFG_WIDTH_16BIT :
							 RTL9602C_I2C_XFER_CFG_WIDTH_8BIT);
	rtl9602c_i2c_w(i2c, RTL9602C_I2C_SUBADDR, command);
	if (read_write == I2C_SMBUS_READ) {
		err = rtl9602c_i2c_run(i2c, RTL9602C_I2C_CMD_START);
		if (err)
			return err;
		/* RX_DATA holds the first byte read in bits 7:0, the second in 15:8. */
		if (size == I2C_SMBUS_WORD_DATA)
			data->word = rtl9602c_i2c_r(i2c, RTL9602C_I2C_RX_DATA) & 0xffff;
		else
			data->byte = rtl9602c_i2c_r(i2c, RTL9602C_I2C_RX_DATA);
		return 0;
	}
	rtl9602c_i2c_w(i2c, RTL9602C_I2C_TX_DATA, data->byte);
	return rtl9602c_i2c_run(i2c, RTL9602C_I2C_CMD_START | RTL9602C_I2C_CMD_WRITE);
}

static u32 rtl9602c_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_READ_WORD_DATA;
}

static const struct i2c_algorithm rtl9602c_i2c_algo = {
	.smbus_xfer	= rtl9602c_i2c_smbus_xfer,
	.functionality	= rtl9602c_i2c_functionality,
};

/* Before the adapter goes, tell the switch driver. */
static void rtl9602c_i2c_gone(void *arg)
{
	struct rtl9602c_i2c_data *pd = arg;

	if (pd->adapter)
		pd->adapter(pd->priv, NULL);
}

static int rtl9602c_i2c_probe(struct auxiliary_device *adev,
			      const struct auxiliary_device_id *id)
{
	struct rtl9602c_i2c_data *pd = dev_get_platdata(&adev->dev);
	struct rtl9602c_i2c *i2c;
	int err;

	if (!pd || !pd->reg)
		return -EINVAL;
	i2c = devm_kzalloc(&adev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;
	i2c->data = pd;

	/*
	 * Until its pad function is enabled the master ignores the start
	 * command: the transfer ends at once, with no BUSY, no NACK and a zero
	 * byte. The register holds the enables of other pad functions too, so
	 * only this bit is changed.
	 */
	rtl9602c_i2c_rmw(i2c, RTL9602C_PADMUX_FUNC_EN, 0, RTL9602C_PADMUX_FUNC_EN_I2C);

	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &rtl9602c_i2c_algo;
	i2c->adap.dev.parent = &adev->dev;
	/*
	 * The adapter has no device tree node. The switch's would make the I2C
	 * core treat the node's children as I2C devices, and the ports node is
	 * not one. No node refers to the adapter, and the devices at 0x50 and
	 * 0x51 are not described.
	 */
	i2c_set_adapdata(&i2c->adap, i2c);
	strscpy(i2c->adap.name, "rtl9602c i2c", sizeof(i2c->adap.name));
	err = devm_i2c_add_adapter(&adev->dev, &i2c->adap);
	if (err)
		return err;

	/* Undone first, before the adapter. */
	err = devm_add_action_or_reset(&adev->dev, rtl9602c_i2c_gone, pd);
	if (err)
		return err;
	if (pd->adapter)
		pd->adapter(pd->priv, &i2c->adap);
	return 0;
}

static const struct auxiliary_device_id rtl9602c_i2c_id_table[] = {
	{ .name = "rtl9602c.i2c" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, rtl9602c_i2c_id_table);

static struct auxiliary_driver rtl9602c_i2c_driver = {
	.probe		= rtl9602c_i2c_probe,
	.id_table	= rtl9602c_i2c_id_table,
};
module_auxiliary_driver(rtl9602c_i2c_driver);

MODULE_DESCRIPTION("I2C master of the Realtek RTL9602C, the bus of the optics");
MODULE_AUTHOR("Paweł Phan <szop.hektar_1w@icloud.com>");
MODULE_LICENSE("GPL");
