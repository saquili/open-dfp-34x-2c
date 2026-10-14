// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: the switch core and the DSA driver.
 *
 * The switch has four ports. Port 0 is a SerDes lane to the host;
 * port 1 exists in the silicon and is left out of the device tree where it
 * is not wired; port 2 is the PON port, behind which the GPON MAC sits;
 * port 3 is the CPU port, whose conduit is the CPU port's DMA engine.
 *
 * This file probes the switch and registers it with DSA; brings up the
 * SerDes lane and reports its link to phylink; forces each MAC's link, speed
 * and duplex, which no port learns from a partner, since none has a PHY;
 * handles the switch's interrupt; reads the per-port counters; and offers
 * devlink parameters for the DoS checks, the storm-control classes tc cannot
 * describe, loop detection, the frame capture and the ONU's serial number,
 * with a devlink region that reads the frame capture. It also holds the
 * register accessors and the access to the indirect table engine that the
 * other files go through, and sets up their blocks at the end of setup.
 *
 * A warm restart of the CPU or a reload of the module does not reset the
 * switch, which keeps running with its registers and tables as they were.
 * Setup therefore writes every piece of state the driver depends on rather
 * than assume reset values.
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
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/if_bridge.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "rtl9602c.h"

static void rtl9602c_debugfs_init(struct rtl9602c_priv *p);
static void rtl9602c_debugfs_exit(struct rtl9602c_priv *p);

/**
 * rtl9602c_r() - read a register in the switch's window
 * @p: the switch
 * @off: the register's offset from the start of the window
 *
 * The window holds the switch core and the other blocks the driver drives:
 * the GPON MAC, the PON MAC, the L3/NAT engine, the PTP clock and the I2C
 * masters. The PON MAC's part, from offset 0xf00000, answers only once the
 * system controller's gate in front of it is open; a read there before
 * that never completes and stops the board.
 *
 * Return: the register's value.
 */
u32 rtl9602c_r(struct rtl9602c_priv *p, u32 off)
{
	return readl(p->reg + off);
}

/**
 * rtl9602c_w() - write a register in the switch's window
 * @p: the switch
 * @off: the register's offset from the start of the window
 * @val: the value
 */
void rtl9602c_w(struct rtl9602c_priv *p, u32 off, u32 val)
{
	writel(val, p->reg + off);
}

/**
 * rtl9602c_rmw() - change bits of a register in the switch's window
 * @p: the switch
 * @off: the register's offset from the start of the window
 * @clr: the bits to clear
 * @set: the bits to set, after @clr has been cleared
 *
 * A read followed by a write, which nothing makes one operation: a change
 * that another context makes to the register in between is lost.
 */
void rtl9602c_rmw(struct rtl9602c_priv *p, u32 off, u32 clr, u32 set)
{
	rtl9602c_w(p, off, (rtl9602c_r(p, off) & ~clr) | set);
}

/**
 * rtl9602c_irq_enable() - change the sources of the switch interrupt
 * @p: the switch
 * @clr: the sources to disable
 * @set: the sources to enable, after @clr has been cleared
 *
 * RTL9602C_IRQ_ENABLE is changed by the interrupt handler, its re-arm work,
 * the learning-limit subscription, the loop_detect parameter, the PHC's PPS
 * request and the GPON bring-up. Every change goes through here, under
 * @p->irq_enable_lock, so that none is lost.
 *
 * Context: any context.
 */
void rtl9602c_irq_enable(struct rtl9602c_priv *p, u32 clr, u32 set)
{
	unsigned long flags;

	spin_lock_irqsave(&p->irq_enable_lock, flags);
	rtl9602c_rmw(p, RTL9602C_IRQ_ENABLE, clr, set);
	spin_unlock_irqrestore(&p->irq_enable_lock, flags);
}

/* Mask @src until the re-arm work unmasks it, a second from now. */
static void rtl9602c_irq_rearm_later(struct rtl9602c_priv *p, u32 src)
{
	unsigned long flags;

	spin_lock_irqsave(&p->irq_enable_lock, flags);
	rtl9602c_rmw(p, RTL9602C_IRQ_ENABLE, src, 0);
	p->intr_rearm_mask |= src;
	spin_unlock_irqrestore(&p->irq_enable_lock, flags);
	schedule_delayed_work(&p->intr_rearm, HZ);
}

/**
 * rtl9602c_table_op() - run one operation of the indirect table engine
 * @p: the switch
 * @type: the table, an RTL9602C_TBLACC_CMD_TABLE_* value
 * @method: how the entry is found: %RTL9602C_TBLACC_CMD_MODE_INDEX goes to
 *	entry @addr, %RTL9602C_TBLACC_CMD_MODE_HASH to the slot of the bucket
 *	the key in @data belongs in
 * @addr: the entry's index; ignored by a hashed operation
 * @data: the entry, word 0 holding its bits 31:0; given to the engine for a
 *	write and, as the key, for a hashed read; filled in by a read
 * @words: the number of words in @data
 * @write: true to write the entry, false to read it
 * @out: if not NULL, receives whether the slot reached holds an entry (a
 *	read) or took the entry (a hashed write), whether that slot is in the
 *	CAM rather than a hash bucket, and its index
 *
 * The address table, the VLAN table and the rule and action tables of the
 * ACL and the classifier sit behind one engine, with one command register,
 * one status register and one set of data registers; writing the command
 * starts the operation. A command written while another caller reads its
 * result would hand that caller the wrong entry, hence table_lock. The
 * engine is polled every 10 us, and one still busy after 1 ms is taken not
 * to be answering rather than waited on for ever.
 *
 * Context: process context, with table_lock held.
 * Return: 0, or -ETIMEDOUT when the engine stays busy.
 */
int rtl9602c_table_op(struct rtl9602c_priv *p, unsigned int type,
		      unsigned int method, unsigned int addr, u32 *data,
		      unsigned int words, bool write,
		      struct rtl9602c_lookup *out)
{
	unsigned int i;
	u32 cmd, sts;
	int err;

	lockdep_assert_held(&p->table_lock);

	if (write || method == RTL9602C_TBLACC_CMD_MODE_HASH)
		for (i = 0; i < words; i++)
			rtl9602c_w(p, RTL9602C_TBLACC_WDATA(i), data[i]);

	cmd = FIELD_PREP(RTL9602C_TBLACC_CMD_INDEX, addr) |
	      FIELD_PREP(RTL9602C_TBLACC_CMD_MODE, method) |
	      FIELD_PREP(RTL9602C_TBLACC_CMD_TABLE, type);
	if (write)
		cmd |= RTL9602C_TBLACC_CMD_WRITE;

	rtl9602c_w(p, RTL9602C_TBLACC_CMD, cmd);

	err = read_poll_timeout(rtl9602c_r, sts, !(sts & RTL9602C_TBLACC_STAT_BUSY),
				10, 1000, false, p, RTL9602C_TBLACC_STAT);
	if (err) {
		pr_err("table %u addr %u: engine still busy after 1 ms\n", type,
		       addr);
		return err;
	}

	if (out) {
		out->hit = sts & RTL9602C_TBLACC_STAT_HIT;
		out->cam = sts & RTL9602C_TBLACC_STAT_CAM;
		out->addr = FIELD_GET(RTL9602C_TBLACC_STAT_INDEX, sts);
	}

	if (!write)
		for (i = 0; i < words; i++)
			data[i] = rtl9602c_r(p, RTL9602C_TBLACC_RDATA(i));

	return 0;
}

/**
 * rtl9602c_table_read() - read a table entry by its index
 * @p: the switch
 * @type: the table, an RTL9602C_TBLACC_CMD_TABLE_* value
 * @addr: the entry's index
 * @data: filled with the entry, word 0 holding its bits 31:0
 * @words: the number of words in @data
 * @hit: if not NULL, set to whether the slot holds an entry; left as it was
 *	when an error is returned
 *
 * Context: process context; takes table_lock.
 * Return: 0, or -ETIMEDOUT when the engine stays busy.
 */
int rtl9602c_table_read(struct rtl9602c_priv *p, unsigned int type,
			unsigned int addr, u32 *data, unsigned int words,
			bool *hit)
{
	struct rtl9602c_lookup out;
	int err;

	mutex_lock(&p->table_lock);
	err = rtl9602c_table_op(p, type, RTL9602C_TBLACC_CMD_MODE_INDEX, addr,
				data, words, false, &out);
	mutex_unlock(&p->table_lock);
	if (hit && !err)
		*hit = out.hit;
	return err;
}

/**
 * rtl9602c_table_write() - write a table entry by its index
 * @p: the switch
 * @type: the table, an RTL9602C_TBLACC_CMD_TABLE_* value
 * @addr: the entry's index
 * @data: the entry, word 0 holding its bits 31:0
 * @words: the number of words in @data
 *
 * The address table accepts a write by index and discards it; its entries
 * are written through the hash, with rtl9602c_table_op().
 *
 * Context: process context; takes table_lock.
 * Return: 0, or -ETIMEDOUT when the engine stays busy.
 */
int rtl9602c_table_write(struct rtl9602c_priv *p, unsigned int type,
			 unsigned int addr, u32 *data, unsigned int words)
{
	int err;

	mutex_lock(&p->table_lock);
	err = rtl9602c_table_op(p, type, RTL9602C_TBLACC_CMD_MODE_INDEX, addr,
				data, words, true, NULL);
	mutex_unlock(&p->table_lock);
	return err;
}

/*
 * Check that the table engine reaches the VLAN entries where the driver
 * thinks. With the index field misplaced two indices alias, which only a
 * pattern written to one and read back from the other shows: so every entry
 * tested gets a pattern of its own, all are written before any is read back,
 * and the saved entries are put back at the end. A VLAN entry keeps fewer
 * than 32 bits, 18 on this chip, so the patterns are cut to the bits it
 * keeps. Setup runs this before any bridge exists and with VLAN filtering
 * off, when no VLAN entry is in use.
 */
static int rtl9602c_table_selftest(struct rtl9602c_priv *p)
{
	/*
	 * Each power of two from 2 to 2048, 3 and 4094: together they set
	 * every one of the twelve index bits. VID 0 is never written, since
	 * writing VLAN entry 0 stops all forwarding to the CPU port, and
	 * writing the old word back does not undo it; VID 1, the bridge's
	 * default VLAN, and VID 4095, reserved by IEEE 802.1Q, are left
	 * alone as well.
	 */
	static const u16 probe[] = {
		2, 3, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048,
		RTL9602C_VLAN_TABLE_ENTRIES - 2,
	};
	u32 saved[ARRAY_SIZE(probe)], want[ARRAY_SIZE(probe)];
	u32 mask, got;
	unsigned int i, j;
	int err;

	for (i = 0; i < ARRAY_SIZE(probe); i++) {
		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, probe[i],
					  &saved[i], 1, NULL);
		if (err)
			return err;
	}

	/* All ones, read back: the bits an entry keeps. */
	mask = ~0u;
	err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, probe[0], &mask, 1);
	if (!err)
		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, probe[0],
					  &mask, 1, NULL);
	if (err)
		goto restore;
	if (hweight32(mask) < 12) {
		pr_err("a VLAN entry holds only %u writable bits (%08x); too few to tell entries apart\n",
		       hweight32(mask), mask);
		err = -EIO;
		goto restore;
	}

	/* Multiples of 2^32 / phi, cut to the mask; no two may be equal. */
	for (i = 0; i < ARRAY_SIZE(probe); i++) {
		want[i] = mask & (0x9e3779b9u * (i + 1));
		for (j = 0; j < i; j++) {
			if (want[j] != want[i])
				continue;
			pr_err("two probe patterns collide inside mask %08x; this test would not detect an alias\n",
			       mask);
			err = -EIO;
			goto restore;
		}
	}

	/* Every pattern is written before any is read back. */
	for (i = 0; i < ARRAY_SIZE(probe); i++) {
		err = rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, probe[i],
					   &want[i], 1);
		if (err)
			goto restore;
	}

	for (i = 0; i < ARRAY_SIZE(probe); i++) {
		err = rtl9602c_table_read(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, probe[i],
					  &got, 1, NULL);
		if (err)
			goto restore;
		if (got != want[i]) {
			pr_err("table entry %u read back %08x, expected %08x -- entries are aliasing, so ADDR is not where this driver thinks\n",
			       probe[i], got, want[i]);
			err = -EIO;
			goto restore;
		}
	}

	pr_info("indirect table port ok: %u VLAN entries addressed distinctly, entry is %u bits wide (%08x)\n",
		(unsigned int)ARRAY_SIZE(probe), hweight32(mask), mask);

restore:
	for (i = 0; i < ARRAY_SIZE(probe); i++)
		rtl9602c_table_write(p, RTL9602C_TBLACC_CMD_TABLE_VLAN, probe[i],
				     &saved[i], 1);
	return err;
}

/*
 * The part's name, from RTL9602C_CHIP_MODEL: the part number as four hex
 * digits, then up to three letters, each a 5-bit field counting A as 1 and
 * left out when 0; a field above 26 is printed as '?'. This chip
 * reads 0x96021800: 9602, then C, then two empty fields.
 */
static void rtl9602c_model_name(struct rtl9602c_priv *p, char *out, size_t n)
{
	static const unsigned int slot[] = {
		RTL9602C_CHIP_MODEL_LETTER1, RTL9602C_CHIP_MODEL_LETTER2,
		RTL9602C_CHIP_MODEL_LETTER3,
	};
	u32 m = rtl9602c_r(p, RTL9602C_CHIP_MODEL);
	size_t len;
	unsigned int i;

	len = snprintf(out, n, "RTL%04X",
		       (unsigned int)FIELD_GET(RTL9602C_CHIP_MODEL_FAMILY, m));

	for (i = 0; i < ARRAY_SIZE(slot) && len + 1 < n; i++) {
		u32 v = (m & slot[i]) >> (ffs(slot[i]) - 1);

		if (!v)
			continue;
		out[len++] = (v <= 26) ? 'A' + v - 1 : '?';
	}
	if (len < n)
		out[len] = '\0';
}

/*
 * devlink dev info: asic.id is the part's name from the model register, and
 * gpon.mac the GPON MAC's revision word as eight hex digits. Both are read
 * from the hardware on every request; the revision word answers whether or
 * not the GPON MAC has been brought up.
 */
static int rtl9602c_devlink_info_get(struct dsa_switch *ds,
				     struct devlink_info_req *req,
				     struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = ds->priv;
	char buf[24];
	int err;

	rtl9602c_model_name(p, buf, sizeof(buf));
	err = devlink_info_version_fixed_put(req,
					     DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
					     buf);
	if (err)
		return err;

	snprintf(buf, sizeof(buf), "0x%08x", rtl9602c_r(p, RTL9602C_GPON_REVISION));
	return devlink_info_version_fixed_put(req, "gpon.mac", buf);
}

/*
 * The SerDes lane to the host runs as the host chooses, and nothing on the lane
 * says which: a host port that takes 2500BaseX sends it without in-band
 * auto-negotiation (IEEE 802.3 clause 37), and one that takes only 1000BaseX
 * commonly negotiates in band, and sometimes does not. So while the lane's
 * receiver has not synchronised, rtl9602c_sds_work() tries the settings of
 * rtl9602c_sds_tries[] in turn and keeps the first one that synchronises.
 * The devlink parameter sfp_lane narrows them to one, or allows them all.
 */
/* The lane settings, in the order they are tried, and their sfp_lane names. */
static const struct {
	u32 mode;
	bool an;
	const char *name;
	const char *param;
} rtl9602c_sds_tries[] = {
	{ RTL9602C_SERDES_CTRL_MODE_2500BASEX, false, "2500BaseX", "2500basex" },
	{ RTL9602C_SERDES_CTRL_MODE_1000BASEX, true,
	  "1000BaseX with in-band negotiation", "1000basex-an" },
	{ RTL9602C_SERDES_CTRL_MODE_1000BASEX, false, "1000BaseX", "1000basex" },
};

#define RTL9602C_SDS_ALL	GENMASK(ARRAY_SIZE(rtl9602c_sds_tries) - 1, 0)

/* How long one setting is given to synchronise, and how often it is checked. */
#define RTL9602C_SDS_TRY_MS	1000

/**
 * rtl9602c_serdes_init() - bring up the SerDes lane to the host
 * @p: the switch
 * @sds_mode: the lane's mode, %RTL9602C_SERDES_CTRL_MODE_2500BASEX or
 *	%RTL9602C_SERDES_CTRL_MODE_1000BASEX
 *
 * Parks the lane, writes its analog settings, restarts and resets it in
 * @sds_mode and turns its in-band auto-negotiation off; the lane's receiver
 * then synchronises within about 20 ms (measured on this board). The
 * analog settings are taken to act as the lane comes out of park and
 * reset, and are written only while it is parked; not verified.
 *
 * 1000BaseX gets the analog values of 2500BaseX, and traffic crosses a
 * 1000BaseX link with them (measured on this board).
 *
 * The sequence takes the link down, so it runs only when the lane's setting
 * changes (rtl9602c_sds_set()).
 */
void rtl9602c_serdes_init(struct rtl9602c_priv *p, u32 sds_mode)
{
	static const struct { u32 off, val; } init[] = RTL9602C_SERDES_ANA_BASEX_SEQ;
	static const u32 rst[] = RTL9602C_SERDES_RESET_SEQ;
	unsigned int i;
	u32 v;

	/* Stop the lane's clock, then park the lane. */
	v = rtl9602c_r(p, RTL9602C_SERDES_CTRL);
	rtl9602c_w(p, RTL9602C_SERDES_CTRL, v | RTL9602C_SERDES_CTRL_CLK_STOP);
	rtl9602c_rmw(p, RTL9602C_SERDES_CTRL,
		     RTL9602C_SERDES_CTRL_PCIE | RTL9602C_SERDES_CTRL_MODE,
		     RTL9602C_SERDES_CTRL_MODE_OFF);

	/* The analog settings, the same for both modes. */
	for (i = 0; i < ARRAY_SIZE(init); i++)
		rtl9602c_w(p, init[i].off, init[i].val);

	/* Restart the lane in the requested mode, its clock running. */
	rtl9602c_rmw(p, RTL9602C_SERDES_CTRL,
		     RTL9602C_SERDES_CTRL_PCIE | RTL9602C_SERDES_CTRL_CLK_STOP |
		     RTL9602C_SERDES_CTRL_PHY_MODE | RTL9602C_SERDES_CTRL_MODE,
		     sds_mode);

	/*
	 * Take the fibre PHY out of power-down and set PATCH_DONE. After the
	 * bootloader both already read so on this board; whether either
	 * write is needed is not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_SERDES_FIBER_CTRL, RTL9602C_SERDES_FIBER_CTRL_PDOWN, 0);
	rtl9602c_rmw(p, RTL9602C_PHY_CTRL, 0, RTL9602C_PHY_CTRL_PATCH_DONE);

	/* A reset pulse for the lane. */
	for (i = 0; i < ARRAY_SIZE(rst); i++)
		rtl9602c_w(p, RTL9602C_SERDES_RESET, rst[i]);

	/*
	 * 2.5 Gb/s cannot be written into a port's forced speed:
	 * at 2500BaseX, port 0 runs as a 1000 Mb/s port with the 2.5G
	 * line-rate override on. The port-0 egress shaper keeps its rate in
	 * the same word, in steps of twice the shaper's own, so at 2500BaseX
	 * the override is enabled with the rate of the shaper in force, which
	 * is the full rate when no tbf limits the port; in any other mode only
	 * the enable is cleared.
	 */
	if (sds_mode == RTL9602C_SERDES_CTRL_MODE_2500BASEX) {
		u32 rate = FIELD_GET(RTL9602C_SHAPE_PORT_CFG_RATE,
				     rtl9602c_r(p, RTL9602C_SHAPE_PORT_CFG));

		rtl9602c_rmw(p, RTL9602C_SHAPE_2G5_CFG,
			     RTL9602C_SHAPE_2G5_CFG_EN | RTL9602C_SHAPE_2G5_CFG_RATE,
			     RTL9602C_SHAPE_2G5_CFG_EN |
			     FIELD_PREP(RTL9602C_SHAPE_2G5_CFG_RATE, rate / 2));
	} else {
		rtl9602c_rmw(p, RTL9602C_SHAPE_2G5_CFG,
			     RTL9602C_SHAPE_2G5_CFG_EN, 0);
	}

	/*
	 * Last, in-band auto-negotiation off: without it the lane transmits,
	 * but its receiver never synchronises with a host that does not
	 * negotiate in band (measured on this board).
	 */
	rtl9602c_w(p, RTL9602C_SERDES_AN_CTRL, RTL9602C_SERDES_AN_CTRL_INIT);
	/* The programmed mode, which phylink's state and mac_config go by. */
	p->sds_mode = sds_mode;
}

/*
 * Re-latch the lane in its programmed mode: stop its clock, park it, wait,
 * then write back the saved control word with the clock running, and give
 * the lane time to settle. It runs on every link-down of port 0, and with it
 * the link has come back after every host-side bounce tried; that a lane
 * whose peer restarted stays unsynchronised without it is not verified.
 * Sleeps.
 */
static void rtl9602c_serdes_resync(struct rtl9602c_priv *p)
{
	u32 saved = rtl9602c_r(p, RTL9602C_SERDES_CTRL);
	u32 v = saved | RTL9602C_SERDES_CTRL_CLK_STOP;

	rtl9602c_w(p, RTL9602C_SERDES_CTRL, v);
	v &= ~(RTL9602C_SERDES_CTRL_PCIE | RTL9602C_SERDES_CTRL_MODE);
	rtl9602c_w(p, RTL9602C_SERDES_CTRL, v | RTL9602C_SERDES_CTRL_MODE_OFF);
	msleep(20);
	rtl9602c_w(p, RTL9602C_SERDES_CTRL, saved & ~RTL9602C_SERDES_CTRL_CLK_STOP);
	msleep(50);
}

/*
 * Bring the lane up with setting @try of rtl9602c_sds_tries[]. In-band
 * negotiation, which the bring-up turns off, is turned on by clearing the
 * lane's FORCE bit and pulsing its restart, and reaches the lane with a
 * re-latch. Under sds_lock; sleeps.
 */
static void rtl9602c_sds_set(struct rtl9602c_priv *p, unsigned int try)
{
	unsigned int lane = BIT(RTL9602C_SERDES_LANE_SFP);

	lockdep_assert_held(&p->sds_lock);
	rtl9602c_serdes_init(p, rtl9602c_sds_tries[try].mode);
	if (rtl9602c_sds_tries[try].an) {
		rtl9602c_rmw(p, RTL9602C_SERDES_AN_CTRL,
			     FIELD_PREP(RTL9602C_SERDES_AN_CTRL_FORCE, lane), 0);
		rtl9602c_rmw(p, RTL9602C_SERDES_AN_CTRL,
			     RTL9602C_SERDES_AN_CTRL_RESTART,
			     FIELD_PREP(RTL9602C_SERDES_AN_CTRL_RESTART, lane));
		rtl9602c_rmw(p, RTL9602C_SERDES_AN_CTRL,
			     RTL9602C_SERDES_AN_CTRL_RESTART, 0);
		rtl9602c_serdes_resync(p);
	}
	p->sds_an = rtl9602c_sds_tries[try].an;
	p->sds_try = try;
}

/*
 * Once a second: while the lane's receiver has synchronised, nothing to do;
 * when it has just lost the host, wait one more period in the same setting,
 * as the host may be restarting its end; otherwise go on to the next setting
 * that may be tried.
 */
static void rtl9602c_sds_work(struct work_struct *w)
{
	struct rtl9602c_priv *p =
		container_of(to_delayed_work(w), struct rtl9602c_priv, sds_work);
	unsigned int try;
	bool up;

	mutex_lock(&p->sds_lock);
	up = rtl9602c_r(p, RTL9602C_SERDES_CTRL) & RTL9602C_SERDES_CTRL_LINK_UP;
	if (up && !p->sds_found) {
		p->sds_found = true;
		pr_info("SFP lane up in %s\n", rtl9602c_sds_tries[p->sds_try].name);
	} else if (!up && p->sds_found) {
		p->sds_found = false;
		pr_info("SFP lane lost the host\n");
	} else if (!up) {
		try = p->sds_try;
		do {
			try = (try + 1) % ARRAY_SIZE(rtl9602c_sds_tries);
		} while (!(p->sds_tries & BIT(try)));
		if (try != p->sds_try)
			rtl9602c_sds_set(p, try);
	}
	mutex_unlock(&p->sds_lock);
	schedule_delayed_work(&p->sds_work,
			      msecs_to_jiffies(RTL9602C_SDS_TRY_MS));
}

/*
 * sfp_lane: "auto" for every lane setting, or the name of one. The settings
 * the module parameters left, when they are neither, read as their names
 * joined by '+'.
 */
static void rtl9602c_sds_lane_get(struct rtl9602c_priv *p, char *buf, size_t len)
{
	unsigned int i;
	size_t n = 0;

	if (p->sds_tries == RTL9602C_SDS_ALL) {
		strscpy(buf, "auto", len);
		return;
	}
	buf[0] = '\0';
	for (i = 0; i < ARRAY_SIZE(rtl9602c_sds_tries); i++)
		if (p->sds_tries & BIT(i))
			n += scnprintf(buf + n, len - n, "%s%s", n ? "+" : "",
				       rtl9602c_sds_tries[i].param);
}

/*
 * Narrow the lane to the settings @name allows. A lane in a setting that is
 * still allowed is left as it is, so "auto" never interrupts it; otherwise
 * it is brought up in the setting named, and the work looks for the host
 * again.
 */
static int rtl9602c_sds_lane_set(struct rtl9602c_priv *p, const char *name)
{
	unsigned int i;
	u8 tries = 0;

	if (!strcmp(name, "auto"))
		tries = RTL9602C_SDS_ALL;
	for (i = 0; i < ARRAY_SIZE(rtl9602c_sds_tries); i++)
		if (!strcmp(name, rtl9602c_sds_tries[i].param))
			tries = BIT(i);
	if (!tries)
		return -EINVAL;

	mutex_lock(&p->sds_lock);
	p->sds_tries = tries;
	if (!(tries & BIT(p->sds_try))) {
		p->sds_found = false;
		rtl9602c_sds_set(p, __ffs(tries));
		pr_info("SFP lane set to %s\n", rtl9602c_sds_tries[p->sds_try].name);
	}
	mutex_unlock(&p->sds_lock);
	return 0;
}

/*
 * No port has a PHY and nothing is negotiated on port 0's lane, so no MAC
 * learns its link from a partner: the driver forces link, speed, duplex and
 * pause. Every enable in RTL9602C_PORT_FORCE_EN is set, and every forced
 * ability other than these is written 0.
 */
static void rtl9602c_force_link(struct rtl9602c_priv *p, int port, bool up,
				int speed, bool duplex, bool tx_pause,
				bool rx_pause)
{
	u32 v = FIELD_PREP(RTL9602C_PORT_FORCE_SPEED, speed);

	if (duplex)
		v |= RTL9602C_PORT_FORCE_DUPLEX;
	if (up)
		v |= RTL9602C_PORT_FORCE_LINK;
	if (tx_pause)
		v |= RTL9602C_PORT_FORCE_TXPAUSE;
	if (rx_pause)
		v |= RTL9602C_PORT_FORCE_RXPAUSE;

	rtl9602c_w(p, RTL9602C_PORT_FORCE_VAL(port), v);
	rtl9602c_w(p, RTL9602C_PORT_FORCE_EN(port), RTL9602C_PORT_FORCE_EN_ALL);
}

static enum dsa_tag_protocol rtl9602c_get_tag_protocol(struct dsa_switch *ds,
						       int port,
						       enum dsa_tag_protocol m)
{
	/*
	 * The CPU tag travels in the CPU port's DMA descriptors, not in the
	 * frame, so the tagger moves no bytes. It is needed all the same:
	 * the conduit hands received frames to DSA only when its CPU port
	 * has a tagger.
	 */
	return DSA_TAG_PROTO_RTL9602C;
}

/*
 * Loop detection sends probes, broadcast frames with EtherType 0x8899, and
 * finds a loop when one of its own probes comes back, which it recognises by
 * a 48-bit magic in the payload. The magic has to be unique to this switch,
 * so it is the MAC address of the CPU port's conduit: the first two octets
 * in RTL9602C_LOOPDET_MAGIC_HI, the last four in RTL9602C_LOOPDET_MAGIC_LO.
 * The PTP timestamp tag uses the same EtherType, and the CPU-port driver
 * treats every frame that reaches the CPU with it as a tagged PTP message.
 */
static int rtl9602c_rldp_set_magic(struct rtl9602c_priv *p)
{
	struct dsa_port *cpu = dsa_to_port(&p->ds, RTL9602C_PORT_CPU);
	struct net_device *conduit;
	const u8 *a;

	conduit = cpu ? cpu->conduit : NULL;
	if (!conduit)
		return -ENODEV;

	a = conduit->dev_addr;
	rtl9602c_w(p, RTL9602C_LOOPDET_MAGIC_HI, (a[0] << 8) | a[1]);
	rtl9602c_w(p, RTL9602C_LOOPDET_MAGIC_LO,
		   (a[2] << 24) | (a[3] << 16) | (a[4] << 8) | a[5]);
	return 0;
}

/*
 * Log the ports on which a loop was found or has gone, with the port each
 * loop closes through (verified only for a loop closing through the port it
 * left), then acknowledge exactly the bits read, so that an event arriving
 * in between stays latched. The driver only reports: what to do about a loop
 * is policy, and a bridge has spanning tree for it.
 */
static void rtl9602c_rldp_report(struct rtl9602c_priv *p)
{
	u32 enter = rtl9602c_r(p, RTL9602C_LOOPDET_DETECTED);
	u32 leave = rtl9602c_r(p, RTL9602C_LOOPDET_CLEARED);
	u32 pnum = rtl9602c_r(p, RTL9602C_LOOPDET_PEER_PORT);
	int port;

	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		unsigned int peer = (pnum >> (RTL9602C_LOOPDET_PEER_PORT_BITS * port)) &
				    ((1 << RTL9602C_LOOPDET_PEER_PORT_BITS) - 1);

		if (enter & BIT(port))
			pr_warn("port %d: loop detected, closing through port %u\n",
				port, peer);
		if (leave & BIT(port))
			pr_info("port %d: loop cleared\n", port);
	}

	rtl9602c_w(p, RTL9602C_LOOPDET_DETECTED, enter);
	rtl9602c_w(p, RTL9602C_LOOPDET_CLEARED, leave);
}

/**
 * rtl9602c_lrn_over_intr_sync() - subscribe the learning-limit alarm by rule
 * @p: the switch
 *
 * The alarm is raised on each attempt to learn an unknown address on a
 * port at its learning limit, and learning off is a limit of 0. So the CPU
 * port, which never learns, raises it whenever one of its addresses ages
 * out and is offered again. The source is therefore enabled only while
 * some port has a limit that is neither 0 nor
 * %RTL9602C_FDB_LEARN_LIMIT_MAX, a limit somebody set, and its latch is
 * cleared first. Called after every write of a learning limit, at
 * interrupt setup and from the re-arm work.
 */
void rtl9602c_lrn_over_intr_sync(struct rtl9602c_priv *p)
{
	unsigned int port;
	bool want = false;

	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		unsigned int limit;

		limit = (rtl9602c_r(p, RTL9602C_FDB_LEARN_LIMIT(port)) >>
			 RTL9602C_FDB_LEARN_SHIFT(port)) & RTL9602C_FDB_LEARN_MASK;
		if (limit && limit < RTL9602C_FDB_LEARN_LIMIT_MAX)
			want = true;
	}

	if (want)
		rtl9602c_w(p, RTL9602C_IRQ_LEARN_STAT, RTL9602C_IRQ_LEARN_STAT_LIMIT);
	rtl9602c_irq_enable(p, RTL9602C_IRQ_SRC_LEARN_LIMIT,
			    want ? RTL9602C_IRQ_SRC_LEARN_LIMIT : 0);
}

/*
 * One second after the handler masked a source that masks itself (the
 * learning limit, the ACL), clear its latch again and unmask it, so that it
 * fires on a fresh event rather than the one already reported. The learning
 * limit is first subscribed again by rule and stays masked when nothing is
 * subscribed.
 */
static void rtl9602c_intr_rearm(struct work_struct *work)
{
	struct rtl9602c_priv *p = container_of(to_delayed_work(work),
					       struct rtl9602c_priv, intr_rearm);
	unsigned long flags;
	u32 mask;

	spin_lock_irqsave(&p->irq_enable_lock, flags);
	mask = p->intr_rearm_mask;
	p->intr_rearm_mask = 0;
	spin_unlock_irqrestore(&p->irq_enable_lock, flags);

	if (mask & RTL9602C_IRQ_SRC_LEARN_LIMIT) {
		rtl9602c_lrn_over_intr_sync(p);
		if (!(rtl9602c_r(p, RTL9602C_IRQ_ENABLE) &
		      RTL9602C_IRQ_SRC_LEARN_LIMIT))
			mask &= ~RTL9602C_IRQ_SRC_LEARN_LIMIT;
	}
	if (mask & RTL9602C_IRQ_SRC_LEARN_LIMIT)
		rtl9602c_w(p, RTL9602C_IRQ_LEARN_STAT,
			   RTL9602C_IRQ_LEARN_STAT_LIMIT);
	if (mask & RTL9602C_IRQ_SRC_ACL)
		rtl9602c_w(p, RTL9602C_ACL_HIT_STAT, RTL9602C_ACL_HIT_STAT_TMPL);
	rtl9602c_irq_enable(p, 0, mask);
}

/*
 * The switch's interrupt: every enabled source of its interrupt block drives
 * the one line. RTL9602C_IRQ_PENDING shows a source as pending whether it is
 * enabled or not, so only sources both pending and enabled are handled, and
 * the line stays asserted until a source's pending bit is written.
 * RTL9602C_IRQ_ENABLE is changed through rtl9602c_irq_enable() only.
 */
static irqreturn_t rtl9602c_irq(int irq, void *dev_id)
{
	struct rtl9602c_priv *p = dev_id;
	u32 pending, stat, up, down;
	irqreturn_t ret = IRQ_HANDLED;
	int port;

	pending = rtl9602c_r(p, RTL9602C_IRQ_PENDING) &
		  rtl9602c_r(p, RTL9602C_IRQ_ENABLE) &
		  (RTL9602C_IRQ_SRC_LINK | RTL9602C_IRQ_SRC_LOOP |
		   RTL9602C_IRQ_SRC_PTP_PPS | RTL9602C_IRQ_SRC_METER |
		   RTL9602C_IRQ_SRC_LEARN_LIMIT | RTL9602C_IRQ_SRC_ACL |
		   RTL9602C_IRQ_SRC_GPON);
	if (!pending)
		return IRQ_NONE;

	/*
	 * The GPON source asserts again until the GTC latches that raised it
	 * are read, so it is left pending for the thread; the time of the
	 * interrupt is noted here, for the GPON code.
	 */
	if (pending & RTL9602C_IRQ_SRC_GPON) {
		rtl9602c_pon_irq_note(p);
		pending &= ~RTL9602C_IRQ_SRC_GPON;
		ret = IRQ_WAKE_THREAD;
	}

	if (pending & RTL9602C_IRQ_SRC_LOOP)
		rtl9602c_rldp_report(p);

	/*
	 * One latch bit per ACL rule template. The source masks itself and
	 * is re-armed a second later. No ACL interrupt has been observed: no
	 * rule the driver writes asks for one.
	 */
	if (pending & RTL9602C_IRQ_SRC_ACL) {
		u32 hit = rtl9602c_r(p, RTL9602C_ACL_HIT_STAT) &
			  RTL9602C_ACL_HIT_STAT_TMPL;

		rtl9602c_irq_rearm_later(p, RTL9602C_IRQ_SRC_ACL);
		rtl9602c_w(p, RTL9602C_ACL_HIT_STAT, RTL9602C_ACL_HIT_STAT_TMPL);
		dev_warn_ratelimited(p->dev,
				     "acl templates %#x matched\n", hit);
	}

	/*
	 * Masks itself too, so that a flood of new sources on a limited port
	 * costs a few interrupts rather than one per frame.
	 */
	if (pending & RTL9602C_IRQ_SRC_LEARN_LIMIT) {
		rtl9602c_irq_rearm_later(p, RTL9602C_IRQ_SRC_LEARN_LIMIT);
		rtl9602c_w(p, RTL9602C_IRQ_LEARN_STAT,
			   RTL9602C_IRQ_LEARN_STAT_LIMIT);
		dev_warn_ratelimited(p->dev,
				     "a port reached its address-learning limit\n");
	}

	/*
	 * One latching bit per meter. It asserts once, and again only after
	 * a clear and a fresh excess, so writing back the bits read re-arms
	 * them. The source masks itself and is re-armed a second later: a
	 * meter that limits a GEM port is exceeded all the time it works.
	 */
	if (pending & RTL9602C_IRQ_SRC_METER) {
		u32 hit = rtl9602c_r(p, RTL9602C_METER_EXCEEDED);

		rtl9602c_irq_rearm_later(p, RTL9602C_IRQ_SRC_METER);
		if (hit) {
			/* A limit of a GEM port is meant to be exceeded. */
			u32 other = hit & ~READ_ONCE(p->gem_ds_meters);

			rtl9602c_w(p, RTL9602C_METER_EXCEEDED, hit);
			if (other)
				dev_warn_ratelimited(p->dev,
						     "meters %#x exceeded their rate\n",
						     other);
		}
	}

	/* Once a second while the PHC's PPS output is enabled. */
	if (pending & RTL9602C_IRQ_SRC_PTP_PPS) {
		struct ptp_clock_event ev = { .type = PTP_CLOCK_PPS };

		p->ptp_pps_count++;
		if (p->ptp_clock)
			ptp_clock_event(p->ptp_clock, &ev);
	}

	/* No link event: acknowledge what was handled. */
	if (!(pending & RTL9602C_IRQ_SRC_LINK)) {
		rtl9602c_w(p, RTL9602C_IRQ_PENDING, pending);
		return ret;
	}

	/*
	 * RTL9602C_IRQ_LINK_STAT holds per-port change, up and down events,
	 * cleared by writing 1, beside state bits that a write does not
	 * clear, so only the event fields are written back. That does not
	 * release the line; the pending bit has to be written too. phylink
	 * then hears of each change at once instead of at its next poll.
	 */
	stat = rtl9602c_r(p, RTL9602C_IRQ_LINK_STAT);

	rtl9602c_w(p, RTL9602C_IRQ_LINK_STAT, stat & RTL9602C_IRQ_LINK_STAT_EVENTS);
	rtl9602c_w(p, RTL9602C_IRQ_PENDING, pending);

	up = FIELD_GET(RTL9602C_IRQ_LINK_STAT_UP, stat);
	down = FIELD_GET(RTL9602C_IRQ_LINK_STAT_DOWN, stat);

	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		if (up & BIT(port))
			dsa_port_phylink_mac_change(&p->ds, port, true);
		if (down & BIT(port))
			dsa_port_phylink_mac_change(&p->ds, port, false);
	}

	return ret;
}

/*
 * The GPON handler reads the GTC latches, and only then is the GPON source's
 * pending bit written. The line stays masked meanwhile (IRQF_ONESHOT). With
 * no handler bound nothing reads the latches and the source would assert
 * again at once, so it is masked; the MAC's driver enables it when it brings
 * the MAC up.
 */
static irqreturn_t rtl9602c_irq_thread(int irq, void *dev_id)
{
	struct rtl9602c_priv *p = dev_id;

	if (!rtl9602c_pon_irq(p))
		rtl9602c_irq_enable(p, RTL9602C_IRQ_SRC_GPON, 0);
	rtl9602c_w(p, RTL9602C_IRQ_PENDING, RTL9602C_IRQ_SRC_GPON);
	return IRQ_HANDLED;
}

/*
 * Undo the interrupt setup, from devres, before the line is freed: mask
 * every source, then cancel the re-arm and GPON work items.
 */
static void rtl9602c_irq_disable(void *data)
{
	struct rtl9602c_priv *p = data;

	rtl9602c_irq_enable(p, ~0u, 0);
	cancel_delayed_work_sync(&p->intr_rearm);
}

/*
 * The mask goes up only once the handler is in place, so that events latched
 * before the driver cannot assert a line nobody handles; the link events a
 * full chip reset leaves latched are cleared first. LINK, METER and ACL are
 * enabled here and the learning limit by rule; the GPON, PPS and loop
 * sources are enabled by the GPON bring-up, the PHC and the loop_detect
 * parameter. Without an interrupt the switch still works, its link state
 * left to phylink's once-a-second poll of the fixed links, and the GPON MAC,
 * which handles its PLOAM messages from this interrupt, has only its
 * periodic work.
 */
static void rtl9602c_irq_setup(struct rtl9602c_priv *p)
{
	struct platform_device *pdev = to_platform_device(p->dev);
	int irq, err;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq < 0) {
		pr_info("no interrupt in the device tree; link state stays polled\n");
		return;
	}

	rtl9602c_irq_enable(p, ~0u, 0);
	rtl9602c_w(p, RTL9602C_IRQ_LINK_STAT, RTL9602C_IRQ_LINK_STAT_EVENTS);

	err = devm_request_threaded_irq(p->dev, irq, rtl9602c_irq,
					rtl9602c_irq_thread, IRQF_ONESHOT,
					"rtl9602c-switch", p);
	if (err) {
		pr_err("cannot take interrupt %d: %pe; link state stays polled\n",
		       irq, ERR_PTR(err));
		return;
	}
	if (devm_add_action_or_reset(p->dev, rtl9602c_irq_disable, p))
		return;

	rtl9602c_irq_enable(p, ~0u,
			    RTL9602C_IRQ_SRC_LINK | RTL9602C_IRQ_SRC_METER |
			    RTL9602C_IRQ_SRC_ACL);
	rtl9602c_lrn_over_intr_sync(p);
	pr_info("interrupt %d: link changes, policed drops and acl hits are events now%s\n",
		irq,
		rtl9602c_r(p, RTL9602C_IRQ_ENABLE) & RTL9602C_IRQ_SRC_LEARN_LIMIT ?
			", and a deliberate learning limit" : "");
}

/*
 * Arm the frame capture. RTL9602C_PKTDBG_LATCH freezes the parser's view of
 * one frame and the switch's verdict on it: the mode chooses the frame, the
 * two port masks restrict it by ingress and egress port, and the reason is
 * what mode 7 waits for. Writing mode none first clears VALID, so nothing
 * captured before the new setting is kept. The four devlink parameters make
 * up one setting, and each of them re-arms with all four.
 */
static void rtl9602c_pktdbg_arm(struct rtl9602c_priv *p)
{
	rtl9602c_w(p, RTL9602C_PKTDBG_LATCH,
		   FIELD_PREP(RTL9602C_PKTDBG_LATCH_MODE, RTL9602C_PKTDBG_LATCH_MODE_NONE));
	rtl9602c_w(p, RTL9602C_PKTDBG_LATCH,
		   FIELD_PREP(RTL9602C_PKTDBG_LATCH_MODE, p->pktdbg.mode) |
		   FIELD_PREP(RTL9602C_PKTDBG_LATCH_INGRESS_PORTS, p->pktdbg.igr) |
		   FIELD_PREP(RTL9602C_PKTDBG_LATCH_EGRESS_PORTS, p->pktdbg.egr) |
		   FIELD_PREP(RTL9602C_PKTDBG_LATCH_REASON, p->pktdbg.reason));
}

/*
 * The frame_capture region: one snapshot of RTL9602C_PKTDBG_SNAPSHOT_WORDS
 * words in the CPU's byte order, big-endian: the control word, the parse
 * (RTL9602C_PKTDBG_PARSE_WORDS; bits 13:0 are the frame length), then the
 * verdict (RTL9602C_PKTDBG_VERDICT_WORDS). Bit b of either is bit b % 32 of
 * its word b / 32. In the verdict, bits 331:324 are the destination port
 * mask, 0 for a dropped frame, and bits 346:339 the forwarding reason: 0 for
 * a forwarded frame, 64 + n for one stopped by DoS check n, 88 for a
 * storm-control drop and 228 for a destination behind the port the frame
 * came in by. An empty latch is refused, so every snapshot describes a
 * frame.
 */
static int rtl9602c_pktdbg_snapshot(struct devlink *dl,
				    const struct devlink_region_ops *ops,
				    struct netlink_ext_ack *extack, u8 **data)
{
	struct dsa_switch *ds = dsa_devlink_to_ds(dl);
	struct rtl9602c_priv *p = ds->priv;
	u32 ctrl = rtl9602c_r(p, RTL9602C_PKTDBG_LATCH);
	u32 *buf;
	int i, n = 0;

	if (!(ctrl & RTL9602C_PKTDBG_LATCH_VALID)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "nothing is latched: arm pktdbg_mode and offer the switch some traffic");
		return -EAGAIN;
	}

	buf = kcalloc(RTL9602C_PKTDBG_SNAPSHOT_WORDS, sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[n++] = ctrl;
	for (i = 0; i < RTL9602C_PKTDBG_PARSE_WORDS; i++)
		buf[n++] = rtl9602c_r(p, RTL9602C_PKTDBG_PARSE(i));
	for (i = 0; i < RTL9602C_PKTDBG_VERDICT_WORDS; i++)
		buf[n++] = rtl9602c_r(p, RTL9602C_PKTDBG_VERDICT(i));

	*data = (u8 *)buf;
	return 0;
}

static const struct devlink_region_ops rtl9602c_pktdbg_region_ops = {
	.name = "frame_capture",
	.snapshot = rtl9602c_pktdbg_snapshot,
	.destructor = kfree,
};

/*
 * The DoS checks act on frames entering the ports in dos_ports. Bit n of
 * dos_checks enables check n, and bit n of dos_actions makes it trap the
 * frame to the CPU instead of dropping it. The checks: 0 destination address
 * equal to source address, 1 land, 2 blat, 3 SYN and FIN, 4 Xmas scan,
 * 5 null scan, 6 SYN from a source port below 1024, 7 short TCP header,
 * 8 TCP fragment offset, 9 ICMP fragment, 10 ping of death, 11 UDP bomb,
 * 12 SYN with data, 13 SYN flood, 14 FIN flood and 15 ICMP flood. The three
 * flood checks count frames against their thresholds over a hardware window
 * of unknown length. Check 0 has not been observed to fire: ordinary
 * forwarding already drops such a frame.
 *
 * Values are read back from the hardware, except the flood thresholds, the
 * capture setting and the serial number, which come from the driver's copy.
 * The threshold registers are never read, since a read of one can stall the
 * register bus (measured on this board).
 */
static int rtl9602c_devlink_param_get(struct dsa_switch *ds, u32 id,
				      struct devlink_param_gset_ctx *ctx)
{
	struct rtl9602c_priv *p = ds->priv;
	int idx;

	switch (id) {
	case RTL9602C_DEVLINK_PARAM_ID_DOS_PORTS:
		ctx->val.vu32 = rtl9602c_r(p, RTL9602C_DOS_PORT_EN) &
				RTL9602C_DOS_PORT_EN_MASK;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_CHECKS:
		ctx->val.vu32 = FIELD_GET(RTL9602C_DOS_CHECK_CTRL_EN,
					  rtl9602c_r(p, RTL9602C_DOS_CHECK_CTRL));
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_ACTIONS:
		ctx->val.vu32 = FIELD_GET(RTL9602C_DOS_CHECK_CTRL_TRAP,
					  rtl9602c_r(p, RTL9602C_DOS_CHECK_CTRL));
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_SYN_FLOOD:
		ctx->val.vu8 = p->dos_flood_th[RTL9602C_DOS_FLOOD_SYN];
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_FIN_FLOOD:
		ctx->val.vu8 = p->dos_flood_th[RTL9602C_DOS_FLOOD_FIN];
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_ICMP_FLOOD:
		ctx->val.vu8 = p->dos_flood_th[RTL9602C_DOS_FLOOD_ICMP];
		return 0;
	default:
		break;
	}

	switch (id) {
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_MODE:
		ctx->val.vu8 = p->pktdbg.mode;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_INGRESS_PORTS:
		ctx->val.vu8 = p->pktdbg.igr;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_EGRESS_PORTS:
		ctx->val.vu8 = p->pktdbg.egr;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_REASON:
		ctx->val.vu8 = p->pktdbg.reason;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_LOOPDET_EN:
		ctx->val.vu8 = rtl9602c_r(p, RTL9602C_LOOPDET_CTRL) &
			       RTL9602C_LOOPDET_CTRL_EN ? 1 : 0;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PORTS:
		ctx->val.vu8 = rtl9602c_r(p, RTL9602C_LOOPDET_TX_PORTS) &
			       RTL9602C_DOS_PORT_EN_MASK;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PERIOD:
		ctx->val.vu32 = FIELD_GET(RTL9602C_LOOPDET_PROBE_CFG_PERIOD,
					  rtl9602c_r(p, RTL9602C_LOOPDET_PROBE_CFG));
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_GPON_SERIAL:
		/*
		 * The serial number: empty until set, then its first four
		 * octets as characters and the other four in hex (ITU-T
		 * G.984.3 clause 9.2.4.1).
		 */
		if (rtl9602c_pon_sn_get(p, ctx->val.vstr, sizeof(ctx->val.vstr)))
			ctx->val.vstr[0] = '\0';
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_GPON_PASSWORD:
		/* Whether it is set, never the password itself. */
		strscpy(ctx->val.vstr, rtl9602c_pon_password_is_set(p) ? "set" : "",
			sizeof(ctx->val.vstr));
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_CPU_RX_PPS:
		ctx->val.vu32 = rtl9602c_cpu_rate_get(p);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_L3_TTL_DEC:
		ctx->val.vbool = !!(rtl9602c_r(p, RTL9602C_L3_CFG) &
				    RTL9602C_L3_CFG_TTL_DEC);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_SFP_LANE:
		mutex_lock(&p->sds_lock);
		rtl9602c_sds_lane_get(p, ctx->val.vstr, sizeof(ctx->val.vstr));
		mutex_unlock(&p->sds_lock);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_SFP_LANE_NOW:
		mutex_lock(&p->sds_lock);
		strscpy(ctx->val.vstr, p->sds_found ?
			rtl9602c_sds_tries[p->sds_try].param : "searching",
			sizeof(ctx->val.vstr));
		mutex_unlock(&p->sds_lock);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PTP_TC:
		rtl9602c_ptp_tc_get(p, ctx->val.vstr, sizeof(ctx->val.vstr));
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PTP_PATH_DELAY:
		ctx->val.vu32 = rtl9602c_ptp_path_delay_get(p);
		return 0;
	default:
		break;
	}

	/*
	 * A storm-control class: its ports from the class's enable register,
	 * its rate from the meter it owns, a meter at its maximum reading 0.
	 */
	idx = rtl9602c_storm_param_find(id);
	if (idx < 0)
		return -EOPNOTSUPP;
	ctx->val.vu32 = rtl9602c_storm_param[idx].is_rate ?
		rtl9602c_storm_rate_get(p, rtl9602c_storm_param[idx].cls) :
		rtl9602c_storm_ports_get(p, rtl9602c_storm_param[idx].cls);
	return 0;
}

/*
 * DSA's parameter macro leaves the validate callback empty, so every range
 * check is made here, and a value too wide for its field is refused with
 * -ERANGE rather than cut to fit.
 */
static int rtl9602c_devlink_param_set(struct dsa_switch *ds, u32 id,
				      struct devlink_param_gset_ctx *ctx)
{
	struct rtl9602c_priv *p = ds->priv;
	unsigned int cls;
	int idx;

	switch (id) {
	case RTL9602C_DEVLINK_PARAM_ID_GPON_SERIAL:
		/*
		 * Refused while the PON is up. A parameter set has no
		 * message channel, so the error code has to say why.
		 */
		if (rtl9602c_pon_is_up(p))
			return -EBUSY;
		return rtl9602c_pon_sn_set(p, ctx->val.vstr);
	case RTL9602C_DEVLINK_PARAM_ID_GPON_PASSWORD:
		if (rtl9602c_pon_is_up(p))
			return -EBUSY;
		return rtl9602c_pon_password_set(p, ctx->val.vstr);
	case RTL9602C_DEVLINK_PARAM_ID_DOS_PORTS:
		if (ctx->val.vu32 & ~(u32)RTL9602C_DOS_PORT_EN_MASK)
			return -ERANGE;
		rtl9602c_w(p, RTL9602C_DOS_PORT_EN, ctx->val.vu32);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_CHECKS:
		if (ctx->val.vu32 > FIELD_MAX(RTL9602C_DOS_CHECK_CTRL_EN))
			return -ERANGE;
		rtl9602c_rmw(p, RTL9602C_DOS_CHECK_CTRL, RTL9602C_DOS_CHECK_CTRL_EN,
			     FIELD_PREP(RTL9602C_DOS_CHECK_CTRL_EN,
					ctx->val.vu32));
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_ACTIONS:
		if (ctx->val.vu32 > FIELD_MAX(RTL9602C_DOS_CHECK_CTRL_TRAP))
			return -ERANGE;
		rtl9602c_rmw(p, RTL9602C_DOS_CHECK_CTRL, RTL9602C_DOS_CHECK_CTRL_TRAP,
			     FIELD_PREP(RTL9602C_DOS_CHECK_CTRL_TRAP,
					ctx->val.vu32));
		return 0;
	/* Written, and kept for reads: the registers are never read. */
	case RTL9602C_DEVLINK_PARAM_ID_DOS_SYN_FLOOD:
		rtl9602c_w(p, RTL9602C_DOS_SYN_FLOOD_THR, ctx->val.vu8);
		p->dos_flood_th[RTL9602C_DOS_FLOOD_SYN] = ctx->val.vu8;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_FIN_FLOOD:
		rtl9602c_w(p, RTL9602C_DOS_FIN_FLOOD_THR, ctx->val.vu8);
		p->dos_flood_th[RTL9602C_DOS_FLOOD_FIN] = ctx->val.vu8;
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_DOS_ICMP_FLOOD:
		rtl9602c_w(p, RTL9602C_DOS_ICMP_FLOOD_THR, ctx->val.vu8);
		p->dos_flood_th[RTL9602C_DOS_FLOOD_ICMP] = ctx->val.vu8;
		return 0;
	default:
		break;
	}

	switch (id) {
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_MODE:
		if (ctx->val.vu8 > RTL9602C_PKTDBG_LATCH_MODE_MAX)
			return -ERANGE;
		p->pktdbg.mode = ctx->val.vu8;
		rtl9602c_pktdbg_arm(p);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_INGRESS_PORTS:
		if (ctx->val.vu8 & ~(u8)RTL9602C_DOS_PORT_EN_MASK)
			return -ERANGE;
		p->pktdbg.igr = ctx->val.vu8;
		rtl9602c_pktdbg_arm(p);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_EGRESS_PORTS:
		if (ctx->val.vu8 & ~(u8)RTL9602C_DOS_PORT_EN_MASK)
			return -ERANGE;
		p->pktdbg.egr = ctx->val.vu8;
		rtl9602c_pktdbg_arm(p);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_PKTDBG_REASON:
		p->pktdbg.reason = ctx->val.vu8;
		rtl9602c_pktdbg_arm(p);
		return 0;
	/*
	 * Enabling needs the magic first; the interrupt source follows the
	 * block's enable.
	 */
	case RTL9602C_DEVLINK_PARAM_ID_LOOPDET_EN:
		if (ctx->val.vu8 > 1)
			return -ERANGE;
		if (ctx->val.vu8) {
			int err = rtl9602c_rldp_set_magic(p);

			if (err)
				return err;
			rtl9602c_irq_enable(p, 0, RTL9602C_IRQ_SRC_LOOP);
		} else {
			rtl9602c_irq_enable(p, RTL9602C_IRQ_SRC_LOOP, 0);
		}
		rtl9602c_rmw(p, RTL9602C_LOOPDET_CTRL, RTL9602C_LOOPDET_CTRL_EN,
			     ctx->val.vu8 ? RTL9602C_LOOPDET_CTRL_EN : 0);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PORTS:
		if (ctx->val.vu8 & ~(u8)RTL9602C_DOS_PORT_EN_MASK)
			return -ERANGE;
		rtl9602c_w(p, RTL9602C_LOOPDET_TX_PORTS, ctx->val.vu8);
		return 0;
	/* The interval between probes, in milliseconds. */
	case RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PERIOD:
		if (ctx->val.vu32 > FIELD_MAX(RTL9602C_LOOPDET_PROBE_CFG_PERIOD))
			return -ERANGE;
		rtl9602c_rmw(p, RTL9602C_LOOPDET_PROBE_CFG,
			     RTL9602C_LOOPDET_PROBE_CFG_PERIOD,
			     FIELD_PREP(RTL9602C_LOOPDET_PROBE_CFG_PERIOD,
					ctx->val.vu32));
		return 0;
	/* 0 lifts the limit; the maximum is a meter's "no limit". */
	case RTL9602C_DEVLINK_PARAM_ID_CPU_RX_PPS:
		if (ctx->val.vu32 >= RTL9602C_METER_CFG_HI_RATE_MAX)
			return -ERANGE;
		return rtl9602c_cpu_rate_set(p, ctx->val.vu32);
	case RTL9602C_DEVLINK_PARAM_ID_L3_TTL_DEC:
		/* Under flow_lock, as the IPv6 routes change FLOW6_EN. */
		mutex_lock(&p->flow_lock);
		rtl9602c_rmw(p, RTL9602C_L3_CFG, RTL9602C_L3_CFG_TTL_DEC,
			     ctx->val.vbool ? RTL9602C_L3_CFG_TTL_DEC : 0);
		mutex_unlock(&p->flow_lock);
		return 0;
	case RTL9602C_DEVLINK_PARAM_ID_SFP_LANE:
		return rtl9602c_sds_lane_set(p, ctx->val.vstr);
	/* What the lane runs in is read only. */
	case RTL9602C_DEVLINK_PARAM_ID_SFP_LANE_NOW:
		return -EOPNOTSUPP;
	case RTL9602C_DEVLINK_PARAM_ID_PTP_TC:
		return rtl9602c_ptp_tc_set(p, ctx->val.vstr);
	case RTL9602C_DEVLINK_PARAM_ID_PTP_PATH_DELAY:
		return rtl9602c_ptp_path_delay_set(p, ctx->val.vu32);
	default:
		break;
	}

	idx = rtl9602c_storm_param_find(id);
	if (idx < 0)
		return -EOPNOTSUPP;

	/*
	 * A rate of 0 switches the class off and frees its meter. The
	 * maximum is a meter's "no limit", so a rate must be below it. Ports
	 * are refused while the class has no rate (-EINVAL), and a rate
	 * while all sixteen meters are taken (-ENOSPC).
	 */
	cls = rtl9602c_storm_param[idx].cls;

	if (rtl9602c_storm_param[idx].is_rate) {
		if (ctx->val.vu32 >= RTL9602C_METER_CFG_HI_RATE_MAX)
			return -ERANGE;
		return rtl9602c_storm_rate_set(p, cls, ctx->val.vu32);
	}

	if (ctx->val.vu32 & ~(u32)RTL9602C_DOS_PORT_EN_MASK)
		return -ERANGE;
	return rtl9602c_storm_ports_set(p, cls, ctx->val.vu32);
}

/*
 * The driver's devlink parameters, all runtime: the DoS checks; a packet
 * rate and a port mask for each storm-control class tc cannot describe
 * (unknown unicast, unknown multicast, DHCP, IGMP and MLD); the frame
 * capture; loop detection; the ONU's serial number and password; the limit
 * on the frames the switch hands the CPU.
 */
static const struct devlink_param rtl9602c_devlink_params[] = {
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_DOS_PORTS,
				 "dos_ports", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_DOS_CHECKS,
				 "dos_checks", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_DOS_ACTIONS,
				 "dos_actions", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_DOS_SYN_FLOOD,
				 "dos_synflood_thresh", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_DOS_FIN_FLOOD,
				 "dos_finflood_thresh", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_DOS_ICMP_FLOOD,
				 "dos_icmpflood_thresh", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_UC_PORTS,
				 "storm_unknown_unicast_ports",
				 DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_UC_RATE,
				 "storm_unknown_unicast_pps",
				 DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_UM_PORTS,
				 "storm_unknown_multicast_ports",
				 DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_UM_RATE,
				 "storm_unknown_multicast_pps",
				 DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_PORTS,
				 "storm_dhcp_ports", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_RATE,
				 "storm_dhcp_pps", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_PORTS,
				 "storm_igmp_mld_ports", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_RATE,
				 "storm_igmp_mld_pps", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_PKTDBG_MODE,
				 "pktdbg_mode", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_PKTDBG_INGRESS_PORTS,
				 "pktdbg_ingress_ports", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_PKTDBG_EGRESS_PORTS,
				 "pktdbg_egress_ports", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_PKTDBG_REASON,
				 "pktdbg_reason", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_LOOPDET_EN,
				 "loop_detect", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PORTS,
				 "loop_detect_ports", DEVLINK_PARAM_TYPE_U8,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PERIOD,
				 "loop_detect_period", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	/* The serial number the ONU presents to the OLT in activation. */
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_GPON_SERIAL,
				 "gpon_serial", DEVLINK_PARAM_TYPE_STRING,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	/* The password the ONU gives the OLT when asked; reads back "set". */
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_GPON_PASSWORD,
				 "gpon_password", DEVLINK_PARAM_TYPE_STRING,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_CPU_RX_PPS,
				 "cpu_rx_pps", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	/*
	 * Whether the engine decrements the TTL or hop limit of what it
	 * routes, as a router does, or leaves it, as a stick that stands in
	 * for an operator's ONT between a router and the operator does: it is
	 * then no hop of its own, and a frame that arrives with 1 goes on.
	 */
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_L3_TTL_DEC,
				 "l3_ttl_decrement", DEVLINK_PARAM_TYPE_BOOL,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	/* The lane settings the driver may bring the SFP lane up in. */
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_SFP_LANE,
				 "sfp_lane", DEVLINK_PARAM_TYPE_STRING,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	/* The setting the lane runs in, or "searching". */
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_SFP_LANE_NOW,
				 "sfp_lane_now", DEVLINK_PARAM_TYPE_STRING,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	/* The switch as a PTP transparent clock: "off", "e2e" or "p2p". */
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_PTP_TC,
				 "ptp_transparent_clock", DEVLINK_PARAM_TYPE_STRING,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
	/* The path delay a peer-to-peer transparent clock adds to a Sync. */
	DSA_DEVLINK_PARAM_DRIVER(RTL9602C_DEVLINK_PARAM_ID_PTP_PATH_DELAY,
				 "ptp_path_delay_ns", DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
};

/*
 * Called by DSA once the switch is registered. Everything the driver depends
 * on is written here, since a warm restart or a reload leaves the switch as
 * it was. The SerDes lane comes first, as nothing reaches the host without
 * it. From the GPON report on, a part that fails is logged and setup still
 * succeeds: a switch without its optics, its PHC, its OMCI channel or its
 * GEM data path still switches frames.
 */
static int rtl9602c_setup(struct dsa_switch *ds)
{
	struct rtl9602c_priv *p = ds->priv;
	unsigned int i;
	u32 bist;
	int err;

	/*
	 * The lane in the first setting; the work tries the others until one
	 * synchronises, unless sfp_lane later leaves one.
	 */
	p->sds_tries = RTL9602C_SDS_ALL;
	p->sds_found = false;
	mutex_lock(&p->sds_lock);
	rtl9602c_sds_set(p, __ffs(p->sds_tries));
	mutex_unlock(&p->sds_lock);
	pr_info("SFP lane in %s%s\n", rtl9602c_sds_tries[p->sds_try].name,
		hweight8(p->sds_tries) > 1 ? ", looking for the host's setting" : "");
	schedule_delayed_work(&p->sds_work, msecs_to_jiffies(RTL9602C_SDS_TRY_MS));

	/*
	 * The CPU port has no PHY and comes out of reset with its forced
	 * link clear: force it up, at 1000 Mb/s full duplex, without pause,
	 * which the CPU port's DMA engine does not take part in.
	 */
	rtl9602c_force_link(p, RTL9602C_PORT_CPU, true,
			    RTL9602C_PORT_FORCE_SPEED_1000, true, false, false);

	/*
	 * The CPU tag both ways. With INSERT the switch tags every frame it
	 * delivers to the CPU port, and the source port, the reason and the
	 * PON flow reach the receive descriptor; with PARSE it reads the tag
	 * that the DMA engine supplies with each frame from the CPU. The
	 * register reads 0 until written; that PARSE is needed is not
	 * verified.
	 */
	rtl9602c_rmw(p, RTL9602C_CPUTAG_CTRL, 0,
		     RTL9602C_CPUTAG_CTRL_PARSE | RTL9602C_CPUTAG_CTRL_INSERT);

	/* The address and NAT tables survive a warm restart: empty them. */
	rtl9602c_l2_clear(p);
	rtl9602c_nat_clear(p);

	/* A failed check is logged and setup goes on. */
	if (rtl9602c_table_selftest(p))
		pr_err("the indirect table port did not answer as expected; the FDB and VLAN table cannot be trusted\n");

	rtl9602c_irq_setup(p);

	/*
	 * One DSCP-to-priority table serves the whole switch, and no port
	 * owns the apptrust weights yet.
	 */
	ds->dscp_prio_mapping_is_global = true;
	p->apptrust_port = -1;

	/*
	 * Classifier entries from RTL9602C_CLS_CTRL_LAYOUT1_START up are
	 * matched with another rule layout, taken to differ from the one the
	 * driver writes (not verified), so only the entries below it are
	 * used; all of them if the field reads 0. It reads 128 on this chip.
	 */
	p->cf_entries = FIELD_GET(RTL9602C_CLS_CTRL_LAYOUT1_START,
				  rtl9602c_r(p, RTL9602C_CLS_CTRL));
	if (!p->cf_entries || p->cf_entries > RTL9602C_CLS_ENTRIES)
		p->cf_entries = RTL9602C_CLS_ENTRIES;
	pr_info("classifier: %u entries usable of %u\n", p->cf_entries,
		RTL9602C_CLS_ENTRIES);

	/*
	 * The verdict of the self-test the chip runs over its internal
	 * memories at power-on; the driver never starts one.
	 */
	bist = rtl9602c_r(p, RTL9602C_CHIP_MEM_SELFTEST);
	if (bist & (RTL9602C_CHIP_MEM_SELFTEST_FAIL | RTL9602C_CHIP_MEM_SELFTEST_RETAIN_FAIL))
		pr_err("power-on self test FAILED (0x%08x): this switch's internal memories are suspect\n",
		       bist);
	else if (!(bist & RTL9602C_CHIP_MEM_SELFTEST_DONE))
		pr_warn("power-on self test did not complete (0x%08x)\n", bist);
	else
		pr_info("power-on self test passed%s\n",
			bist & RTL9602C_CHIP_MEM_SELFTEST_RETAIN_DONE ?
				", data retention included" : "");

	/*
	 * No DoS check enabled before the driver started survives: all off, and the
	 * flood thresholds 0 in the registers and in the driver's copy.
	 */
	rtl9602c_w(p, RTL9602C_DOS_CHECK_CTRL, 0);
	rtl9602c_w(p, RTL9602C_DOS_PORT_EN, 0);
	rtl9602c_w(p, RTL9602C_DOS_SYN_FLOOD_THR, 0);
	rtl9602c_w(p, RTL9602C_DOS_FIN_FLOOD_THR, 0);
	rtl9602c_w(p, RTL9602C_DOS_ICMP_FLOOD_THR, 0);

	memset(p->dos_flood_th, 0, sizeof(p->dos_flood_th));

	/*
	 * Storm control off: no class enabled or bound to a meter, every
	 * meter back to "no limit" and free, no class owning one, and every
	 * meter's exceeded latch cleared, since a latch left from before
	 * would read as a policed drop that never happened.
	 */
	mutex_lock(&p->meter_lock);
	for (i = 0; i < RTL9602C_STORM_NCLASS; i++) {
		rtl9602c_w(p, rtl9602c_storm_class[i].ctrl, 0);
		rtl9602c_w(p, rtl9602c_storm_class[i].meter, 0);
	}
	for (i = 0; i < RTL9602C_METER_COUNT; i++)
		rtl9602c_meter_free(p, i);
	bitmap_zero(p->meter_used, RTL9602C_METER_COUNT);
	for (i = 0; i < ARRAY_SIZE(p->storm_meter); i++)
		p->storm_meter[i] = -1;
	p->cpu_meter = -1;
	rtl9602c_w(p, RTL9602C_METER_EXCEEDED, GENMASK(RTL9602C_METER_COUNT - 1, 0));
	mutex_unlock(&p->meter_lock);

	/*
	 * The ACL, the classifier, the priority maps, the policer, the
	 * scheduler and the shaper as a full chip reset leaves them, before
	 * the driver writes classifier entries of its own.
	 */
	rtl9602c_filter_qos_reset(p);

	/*
	 * The frames for the CPU may hold only a few of the buffer's pages:
	 * with the limit after reset, a flood the rate limit below held back
	 * filled the buffer, and the switch dropped the frames it forwards as
	 * well (measured: 59 Mb/s routed upstream instead of 932). Beyond the
	 * limit it drops a frame for the CPU where the frame enters.
	 */
	rtl9602c_rmw(p, RTL9602C_PORT_PAGE_LIMIT(RTL9602C_PORT_CPU),
		     RTL9602C_PORT_PAGE_LIMIT_MAX <<
		     RTL9602C_PORT_PAGE_LIMIT_SHIFT(RTL9602C_PORT_CPU),
		     RTL9602C_CPU_PAGE_LIMIT <<
		     RTL9602C_PORT_PAGE_LIMIT_SHIFT(RTL9602C_PORT_CPU));

	/*
	 * A flood to the CPU is held at the switch, at a rate that leaves the
	 * CPU most of its time (see rtl9602c_cpu_rate_set()).
	 */
	if (rtl9602c_cpu_rate_set(p, RTL9602C_CPU_RATE_DEFAULT))
		pr_err("no meter left for the CPU port: frames to the CPU are not limited\n");

	/* Port mirroring survives a warm restart too: none. */
	rtl9602c_w(p, RTL9602C_MIRROR_CTRL, 0);

	/*
	 * S-tags recognised on the SFP and PON ports, for the VLAN rules of
	 * tc flower, with every other frame passing as it came.
	 */
	rtl9602c_svlan_setup(p);

	/*
	 * User field selectors 0 and 1 cut the L4 source and destination
	 * ports out of the IP payload, for ACL rules that match on a port:
	 * octets 0-1 and 2-3 of a UDP or TCP header (RFC 768,
	 * RFC 9293 section 3.1).
	 */
	rtl9602c_w(p, RTL9602C_FIELDSEL_CFG + 4 * RTL9602C_FSEL_SPORT,
		   FIELD_PREP(RTL9602C_FIELDSEL_CFG_OFFSET, 0) |
		   FIELD_PREP(RTL9602C_FIELDSEL_CFG_ORIGIN,
			      RTL9602C_FIELDSEL_CFG_ORIGIN_IP_PAYLOAD));
	rtl9602c_w(p, RTL9602C_FIELDSEL_CFG + 4 * RTL9602C_FSEL_DPORT,
		   FIELD_PREP(RTL9602C_FIELDSEL_CFG_OFFSET, 2) |
		   FIELD_PREP(RTL9602C_FIELDSEL_CFG_ORIGIN,
			      RTL9602C_FIELDSEL_CFG_ORIGIN_IP_PAYLOAD));

	/* IGMP and MLD reach the bridge for multicast snooping. */
	rtl9602c_acl_snoop_setup(p);

	/*
	 * Take the reserved group addresses of spanning tree, PAUSE, the
	 * Slow Protocols (LACP), 802.1X and LLDP, 01-80-C2-00-00-00, -01,
	 * -02, -03 and -0E (IEEE 802.1Q Table 8-1), out of storm control, so
	 * that a broadcast or unknown-multicast limit does not police them.
	 * Read-modify-write, since _01 and _02 carry an action from reset.
	 * That _01, _02 and _03 serve the addresses their names give is not
	 * verified.
	 */
	rtl9602c_rmw(p, RTL9602C_RSVDMC_CTRL_00, 0,
		     RTL9602C_RSVDMC_CTRL_STORM_EXEMPT);
	rtl9602c_rmw(p, RTL9602C_RSVDMC_CTRL_01, 0,
		     RTL9602C_RSVDMC_CTRL_STORM_EXEMPT);
	rtl9602c_rmw(p, RTL9602C_RSVDMC_CTRL_02, 0,
		     RTL9602C_RSVDMC_CTRL_STORM_EXEMPT);
	rtl9602c_rmw(p, RTL9602C_RSVDMC_CTRL_03, 0,
		     RTL9602C_RSVDMC_CTRL_STORM_EXEMPT);
	rtl9602c_rmw(p, RTL9602C_RSVDMC_CTRL_0E, 0,
		     RTL9602C_RSVDMC_CTRL_STORM_EXEMPT);

	/*
	 * Frames to the nearest-bridge address, 01-80-C2-00-00-0E, go to the
	 * CPU only: no bridge forwards them (IEEE 802.1Q 8.6.3), and the
	 * Linux bridge, which receives them, does not either. The peer delay
	 * messages of PTP (IEEE 1588-2008 annex F) and LLDP are sent to it.
	 */
	rtl9602c_rmw(p, RTL9602C_RSVDMC_CTRL_0E, RTL9602C_RSVDMC_CTRL_ACTION,
		     FIELD_PREP(RTL9602C_RSVDMC_CTRL_ACTION,
				RTL9602C_RSVDMC_CTRL_ACTION_TRAP));

	/*
	 * Loop detection off: a loaded driver must not start sending probes
	 * by itself, and a latch from before would report a loop that ended
	 * before this driver existed.
	 */
	rtl9602c_w(p, RTL9602C_LOOPDET_CTRL, 0);
	rtl9602c_w(p, RTL9602C_LOOPDET_TX_PORTS, 0);

	rtl9602c_w(p, RTL9602C_LOOPDET_DETECTED, GENMASK(RTL9602C_NUM_PORTS - 1, 0));
	rtl9602c_w(p, RTL9602C_LOOPDET_CLEARED, GENMASK(RTL9602C_NUM_PORTS - 1, 0));

	/*
	 * After a reset the capture is armed for any frame and may already
	 * hold one: arm it in mode none, so that no capture predates the
	 * driver.
	 */
	p->pktdbg.mode = RTL9602C_PKTDBG_LATCH_MODE_NONE;
	p->pktdbg.igr = RTL9602C_DOS_PORT_EN_MASK;
	p->pktdbg.egr = RTL9602C_DOS_PORT_EN_MASK;
	p->pktdbg.reason = 0;
	rtl9602c_pktdbg_arm(p);

	if (dsa_devlink_params_register(ds, rtl9602c_devlink_params,
					ARRAY_SIZE(rtl9602c_devlink_params)))
		pr_err("devlink parameters unavailable; the switch's own features cannot be configured\n");
	else
		p->params_registered = true;

	p->pktdbg_region = dsa_devlink_region_create(ds, &rtl9602c_pktdbg_region_ops,
						     1, RTL9602C_PKTDBG_SNAPSHOT_WORDS *
						     sizeof(u32));
	if (IS_ERR(p->pktdbg_region)) {
		pr_err("no devlink region; frames cannot be captured\n");
		p->pktdbg_region = NULL;
	}

	/*
	 * Standalone forwarding: no GEM bridge, no stream entries, and each
	 * port's flood request as reset leaves it (unknown unicast, unknown
	 * multicast and broadcast). The bridge refresh then writes
	 * isolation, flooding and learning from that state.
	 */
	p->pon_br = NULL;
	p->pon_br_members = 0;
	p->pon_br_us_flow = -1;
	p->l34_us_flow = -1;
	bitmap_zero(p->gem_bridged, RTL9602C_GPON_NUM_FLOW);
	bitmap_zero(p->gem_bridged_up, RTL9602C_GPON_NUM_FLOW);
	for (i = 0; i < RTL9602C_NUM_PORTS; i++) {
		p->cf_sid[i] = -1;
		p->flood_req[i] = BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD;
	}
	rtl9602c_mcast_reset(p);
	rtl9602c_l2_moves_init(p);

	rtl9602c_bridge_refresh(ds);

	/*
	 * The L3/NAT engine, with flow_lock over its bookkeeping, and the
	 * notifiers that fill its tables.
	 */
	mutex_init(&p->flow_lock);
	rtl9602c_l34_init(p);
	rtl9602c_l34_notifiers_register(p);

	rtl9602c_debugfs_init(p);

	/*
	 * The optics' I2C master has a driver of its own. It registers the
	 * adapter when it binds, and that identifies the module and publishes
	 * its diagnostics (rtl9602c_optics_adapter()).
	 */
	err = rtl9602c_optics_i2c_add(p);
	if (err)
		pr_warn("i2c master not handed over: %pe\n", ERR_PTR(err));

	{
		int perr = rtl9602c_ptp_register(p);

		if (perr)
			pr_warn("no PTP clock: %pe\n", ERR_PTR(perr));
	}

	/*
	 * The GPON MAC has a driver of its own. It brings up the MAC's
	 * registration with the PON class, the OMCI port and the GEM data
	 * path when it binds.
	 */
	err = rtl9602c_pon_attach(p);
	if (err)
		pr_warn("no GPON MAC: %pe\n", ERR_PTR(err));

	/* Fold the port counters into their totals before any can wrap. */
	schedule_delayed_work(&p->mib_work, RTL9602C_MIB_FOLD_S * HZ);
	return 0;
}

/*
 * The L3 notifiers go first, as they can run on any CPU and touch the
 * tables. The GEM data path goes before the OMCI port: the PON device is
 * unregistered with its last child, the OMCI port, and the data path must
 * not outlive it. The region and the parameters go before the devlink
 * instance does, so that the next probe does not find their names taken. The
 * interrupt is undone by devres.
 */
static void rtl9602c_teardown(struct dsa_switch *ds)
{
	struct rtl9602c_priv *p = ds->priv;

	cancel_delayed_work_sync(&p->mib_work);
	cancel_delayed_work_sync(&p->sds_work);
	cancel_work_sync(&p->optics_level_work);
	rtl9602c_l34_notifiers_unregister(p);
	rtl9602c_l34_fini(p);
	rtl9602c_pon_detach(p);
	rtl9602c_debugfs_exit(p);
	rtl9602c_ptp_unregister(p);
	if (p->pktdbg_region)
		dsa_devlink_region_destroy(p->pktdbg_region);
	if (p->params_registered)
		dsa_devlink_params_unregister(ds, rtl9602c_devlink_params,
					      ARRAY_SIZE(rtl9602c_devlink_params));
	p->params_registered = false;
}

/*
 * Symmetric pause is offered. Every port is a fixed link, so phylink resolves
 * pause from the fixed-link node of the device tree ("pause"), not from
 * ethtool.
 */
static void rtl9602c_phylink_get_caps(struct dsa_switch *ds, int port,
				      struct phylink_config *config)
{
	config->mac_capabilities = MAC_2500FD | MAC_1000FD | MAC_SYM_PAUSE;

	switch (port) {
	case RTL9602C_PORT_SFP:
		/* The SerDes lane, at the speed the host chooses. */
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  config->supported_interfaces);
		break;
	default:
		/* The PON port and the CPU port. */
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		break;
	}
}

/*
 * Every port is a fixed link, so phylink asks here for its state: once a
 * second, and at once when the interrupt reports a link change.
 */
static void rtl9602c_phylink_fixed_state(struct dsa_switch *ds, int port,
					 struct phylink_link_state *state)
{
	struct rtl9602c_priv *p = ds->priv;

	state->duplex = DUPLEX_FULL;
	state->an_complete = true;

	if (port == RTL9602C_PORT_SFP) {
		/*
		 * The link is the lane's receiver having synchronised, not
		 * the host seeing carrier: the host sees carrier from this
		 * side's transmitter alone. The speed follows the programmed
		 * mode: a fixed 2500 makes phylink bring a lane set
		 * to 1000BaseX back up in 2500BaseX (measured on this
		 * board).
		 */
		state->speed = p->sds_mode == RTL9602C_SERDES_CTRL_MODE_1000BASEX ?
			       SPEED_1000 : SPEED_2500;
		state->link = !!(rtl9602c_r(p, RTL9602C_SERDES_CTRL) &
				 RTL9602C_SERDES_CTRL_LINK_UP);
	} else {
		/* Always up; the GPON code sets the PON port's operstate. */
		state->speed = SPEED_1000;
		state->link = 1;
	}
}

/*
 * Force the port up with the duplex and the pause phylink resolved, always at
 * 1000 Mb/s: at 2500BaseX, port 0 runs as a 1000 Mb/s port with the 2.5G
 * override (rtl9602c_serdes_init()).
 */
static void rtl9602c_mac_link_up(struct phylink_config *config,
				 struct phy_device *phy, unsigned int mode,
				 phy_interface_t interface, int speed,
				 int duplex, bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl9602c_priv *p = dp->ds->priv;

	rtl9602c_force_link(p, dp->index, true, RTL9602C_PORT_FORCE_SPEED_1000,
			    duplex == DUPLEX_FULL, tx_pause, rx_pause);
}

/*
 * Force the port down; on port 0 also re-latch the lane, which brings the
 * link back once the host restarts its end.
 */
static void rtl9602c_mac_link_down(struct phylink_config *config,
				   unsigned int mode, phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl9602c_priv *p = dp->ds->priv;

	rtl9602c_force_link(p, dp->index, false, RTL9602C_PORT_FORCE_SPEED_1000,
			    true, false, false);

	if (dp->index == RTL9602C_PORT_SFP) {
		mutex_lock(&p->sds_lock);
		rtl9602c_serdes_resync(p);
		mutex_unlock(&p->sds_lock);
	}
}

/*
 * Nothing to configure. The CPU and PON ports have no mode, and the lane's
 * setting is the driver's to find (rtl9602c_sds_work()): the interface that
 * phylink passes is the device tree's phy-mode, which says nothing about the
 * host, and following it would undo the setting found.
 */
static void rtl9602c_mac_config(struct phylink_config *config,
				unsigned int mode,
				const struct phylink_link_state *state)
{
}

static const struct phylink_mac_ops rtl9602c_phylink_mac_ops = {
	.mac_config	= rtl9602c_mac_config,
	.mac_link_up	= rtl9602c_mac_link_up,
	.mac_link_down	= rtl9602c_mac_link_down,
};

/*
 * Clear the forced-down state that port disable left, so that none remains
 * while phylink brings the link up and forces it again; that bit 4 of the
 * force-enable word is the forced link's enable is not verified. On the PON
 * port, start activation: a refusal fails the open rather than leave an
 * interface up with the laser dark.
 */
static int rtl9602c_port_enable(struct dsa_switch *ds, int port,
				struct phy_device *phy)
{
	struct rtl9602c_priv *p = ds->priv;

	rtl9602c_rmw(p, RTL9602C_PORT_FORCE_EN(port), RTL9602C_PORT_FORCE_LINK, 0);

	if (port == RTL9602C_PORT_PON) {
		int err = rtl9602c_pon_start(p);

		if (err)
			return err;
		rtl9602c_pon_operstate(p);
	}

	/*
	 * The port's own address behind the CPU port, so that frames for it
	 * are not flooded: setup runs before DSA creates the interfaces, so
	 * this is where a port first has one. A failure is logged there and
	 * leaves those frames flooded.
	 */
	rtl9602c_l2_own_place(p, port);
	return 0;
}

/* Stop activation on the PON port, then force the link down. */
static void rtl9602c_port_disable(struct dsa_switch *ds, int port)
{
	struct rtl9602c_priv *p = ds->priv;

	if (port == RTL9602C_PORT_PON)
		rtl9602c_pon_stop(p);

	rtl9602c_rmw(p, RTL9602C_PORT_FORCE_VAL(port), RTL9602C_PORT_FORCE_LINK, 0);
	rtl9602c_rmw(p, RTL9602C_PORT_FORCE_EN(port), 0, RTL9602C_PORT_FORCE_LINK);
}

/*
 * The per-port counters: a receive block and a transmit block per port,
 * one 32-bit counter a word, read directly with no latch step. The two octet
 * counters, marked wide, are 64 bits: the low word at the named offset, the
 * high word taken to be the next one (not verified). The unicast and octet
 * counters, rx_crc_errors, rx_drops, tx_discards, tx_512_1023_octets and
 * rx_forward_discards are checked against known traffic; the other offsets
 * are not verified. The names follow the objects of RFC 2863, RFC 2819,
 * RFC 3635 and RFC 4188 where there is one; the transmit size buckets and
 * the 1519-to-maximum counters have none.
 */
/* The counters' indices in rtl9602c_mib[], in its order. */
enum {
	RTL9602C_MIBI_RX_OCTETS,
	RTL9602C_MIBI_RX_UNICAST,
	RTL9602C_MIBI_RX_MULTICAST,
	RTL9602C_MIBI_RX_BROADCAST,
	RTL9602C_MIBI_RX_CRC_ERRORS,
	RTL9602C_MIBI_RX_DROPS,
	RTL9602C_MIBI_RX_SYMBOL_ERRORS,
	RTL9602C_MIBI_RX_PAUSE,
	RTL9602C_MIBI_RX_UNKNOWN_OPCODES,
	RTL9602C_MIBI_RX_FRAGMENTS,
	RTL9602C_MIBI_RX_JABBERS,
	RTL9602C_MIBI_RX_UNDERSIZE,
	RTL9602C_MIBI_RX_OVERSIZE,
	RTL9602C_MIBI_RX_64_OCTETS,
	RTL9602C_MIBI_RX_65_127_OCTETS,
	RTL9602C_MIBI_RX_128_255_OCTETS,
	RTL9602C_MIBI_RX_256_511_OCTETS,
	RTL9602C_MIBI_RX_512_1023_OCTETS,
	RTL9602C_MIBI_RX_1024_1518_OCTETS,
	RTL9602C_MIBI_RX_1519_MAX_OCTETS,
	RTL9602C_MIBI_RX_FORWARD_DISCARDS,
	RTL9602C_MIBI_TX_OCTETS,
	RTL9602C_MIBI_TX_UNICAST,
	RTL9602C_MIBI_TX_MULTICAST,
	RTL9602C_MIBI_TX_BROADCAST,
	RTL9602C_MIBI_TX_DISCARDS,
	RTL9602C_MIBI_TX_ETHER_MULTICAST,
	RTL9602C_MIBI_TX_ETHER_BROADCAST,
	RTL9602C_MIBI_TX_PAUSE,
	RTL9602C_MIBI_TX_SINGLE_COLLISIONS,
	RTL9602C_MIBI_TX_MULTIPLE_COLLISIONS,
	RTL9602C_MIBI_TX_DEFERRED,
	RTL9602C_MIBI_TX_LATE_COLLISIONS,
	RTL9602C_MIBI_TX_EXCESSIVE_COLLISIONS,
	RTL9602C_MIBI_TX_COLLISIONS,
	RTL9602C_MIBI_TX_UNDERSIZE,
	RTL9602C_MIBI_TX_OVERSIZE,
	RTL9602C_MIBI_TX_64_OCTETS,
	RTL9602C_MIBI_TX_65_127_OCTETS,
	RTL9602C_MIBI_TX_128_255_OCTETS,
	RTL9602C_MIBI_TX_256_511_OCTETS,
	RTL9602C_MIBI_TX_512_1023_OCTETS,
	RTL9602C_MIBI_TX_1024_1518_OCTETS,
	RTL9602C_MIBI_TX_1519_MAX_OCTETS,
};

static const struct {
	const char *name;
	bool tx;
	bool wide;
	u32 off;
} rtl9602c_mib[RTL9602C_MIB_NUM] = {
	[RTL9602C_MIBI_RX_OCTETS] = { "rx_octets", 0, 1,
		RTL9602C_MIB_RX_OCTETS },
	[RTL9602C_MIBI_RX_UNICAST] = { "rx_unicast", 0, 0,
		RTL9602C_MIB_RX_UCAST_PKTS },
	[RTL9602C_MIBI_RX_MULTICAST] = { "rx_multicast", 0, 0,
		RTL9602C_MIB_RX_MCAST_PKTS },
	[RTL9602C_MIBI_RX_BROADCAST] = { "rx_broadcast", 0, 0,
		RTL9602C_MIB_RX_BCAST_PKTS },
	[RTL9602C_MIBI_RX_CRC_ERRORS] = { "rx_crc_errors", 0, 0,
		RTL9602C_MIB_RX_CRC_ALIGN_ERRORS },
	[RTL9602C_MIBI_RX_DROPS] = { "rx_drops", 0, 0,
		RTL9602C_MIB_RX_DROP_EVENTS },
	[RTL9602C_MIBI_RX_SYMBOL_ERRORS] = { "rx_symbol_errors", 0, 0,
		RTL9602C_MIB_RX_SYMBOL_ERRORS },
	[RTL9602C_MIBI_RX_PAUSE] = { "rx_pause", 0, 0,
		RTL9602C_MIB_RX_PAUSE_FRAMES },
	[RTL9602C_MIBI_RX_UNKNOWN_OPCODES] = { "rx_unknown_opcodes", 0, 0,
		RTL9602C_MIB_RX_UNKNOWN_OPCODES },
	[RTL9602C_MIBI_RX_FRAGMENTS] = { "rx_fragments", 0, 0,
		RTL9602C_MIB_RX_FRAGMENTS },
	[RTL9602C_MIBI_RX_JABBERS] = { "rx_jabbers", 0, 0,
		RTL9602C_MIB_RX_JABBERS },
	[RTL9602C_MIBI_RX_UNDERSIZE] = { "rx_undersize", 0, 0,
		RTL9602C_MIB_RX_UNDERSIZE_PKTS },
	[RTL9602C_MIBI_RX_OVERSIZE] = { "rx_oversize", 0, 0,
		RTL9602C_MIB_RX_OVERSIZE_PKTS },
	[RTL9602C_MIBI_RX_64_OCTETS] = { "rx_64_octets", 0, 0,
		RTL9602C_MIB_RX_PKTS_64 },
	[RTL9602C_MIBI_RX_65_127_OCTETS] = { "rx_65_127_octets", 0, 0,
		RTL9602C_MIB_RX_PKTS_65_127 },
	[RTL9602C_MIBI_RX_128_255_OCTETS] = { "rx_128_255_octets", 0, 0,
		RTL9602C_MIB_RX_PKTS_128_255 },
	[RTL9602C_MIBI_RX_256_511_OCTETS] = { "rx_256_511_octets", 0, 0,
		RTL9602C_MIB_RX_PKTS_256_511 },
	[RTL9602C_MIBI_RX_512_1023_OCTETS] = { "rx_512_1023_octets", 0, 0,
		RTL9602C_MIB_RX_PKTS_512_1023 },
	[RTL9602C_MIBI_RX_1024_1518_OCTETS] = { "rx_1024_1518_octets", 0, 0,
		RTL9602C_MIB_RX_PKTS_1024_1518 },
	[RTL9602C_MIBI_RX_1519_MAX_OCTETS] = { "rx_1519_max_octets", 0, 0,
		RTL9602C_MIB_RX_PKTS_1519_MAX },
	/*
	 * In the transmit block, but it counts frames received on the port
	 * and discarded by forwarding (dot1dTpPortInDiscards, RFC 4188).
	 */
	[RTL9602C_MIBI_RX_FORWARD_DISCARDS] = { "rx_forward_discards", 1, 0,
		RTL9602C_MIB_TX_FWD_DISCARDS },

	[RTL9602C_MIBI_TX_OCTETS] = { "tx_octets", 1, 1,
		RTL9602C_MIB_TX_OCTETS },
	[RTL9602C_MIBI_TX_UNICAST] = { "tx_unicast", 1, 0,
		RTL9602C_MIB_TX_UCAST_PKTS },
	[RTL9602C_MIBI_TX_MULTICAST] = { "tx_multicast", 1, 0,
		RTL9602C_MIB_TX_MCAST_PKTS },
	[RTL9602C_MIBI_TX_BROADCAST] = { "tx_broadcast", 1, 0,
		RTL9602C_MIB_TX_BCAST_PKTS },
	[RTL9602C_MIBI_TX_DISCARDS] = { "tx_discards", 1, 0,
		RTL9602C_MIB_TX_DISCARDS },
	/*
	 * Taken to count what the MAC put on the wire, where tx_multicast
	 * and tx_broadcast count what forwarding handed to the port; not
	 * verified.
	 */
	[RTL9602C_MIBI_TX_ETHER_MULTICAST] = { "tx_ether_multicast", 1, 0,
		RTL9602C_MIB_TX_RMON_MCAST_PKTS },
	[RTL9602C_MIBI_TX_ETHER_BROADCAST] = { "tx_ether_broadcast", 1, 0,
		RTL9602C_MIB_TX_RMON_BCAST_PKTS },
	[RTL9602C_MIBI_TX_PAUSE] = { "tx_pause", 1, 0,
		RTL9602C_MIB_TX_PAUSE_FRAMES },
	[RTL9602C_MIBI_TX_SINGLE_COLLISIONS] = { "tx_single_collisions", 1, 0,
		RTL9602C_MIB_TX_SINGLE_COLLISIONS },
	[RTL9602C_MIBI_TX_MULTIPLE_COLLISIONS] = { "tx_multiple_collisions", 1, 0,
		RTL9602C_MIB_TX_MULTIPLE_COLLISIONS },
	[RTL9602C_MIBI_TX_DEFERRED] = { "tx_deferred", 1, 0,
		RTL9602C_MIB_TX_DEFERRED },
	[RTL9602C_MIBI_TX_LATE_COLLISIONS] = { "tx_late_collisions", 1, 0,
		RTL9602C_MIB_TX_LATE_COLLISIONS },
	[RTL9602C_MIBI_TX_EXCESSIVE_COLLISIONS] = { "tx_excessive_collisions", 1, 0,
		RTL9602C_MIB_TX_EXCESSIVE_COLLISIONS },
	[RTL9602C_MIBI_TX_COLLISIONS] = { "tx_collisions", 1, 0,
		RTL9602C_MIB_TX_COLLISIONS },
	[RTL9602C_MIBI_TX_UNDERSIZE] = { "tx_undersize", 1, 0,
		RTL9602C_MIB_TX_UNDERSIZE_PKTS },
	[RTL9602C_MIBI_TX_OVERSIZE] = { "tx_oversize", 1, 0,
		RTL9602C_MIB_TX_OVERSIZE_PKTS },
	[RTL9602C_MIBI_TX_64_OCTETS] = { "tx_64_octets", 1, 0,
		RTL9602C_MIB_TX_PKTS_64 },
	[RTL9602C_MIBI_TX_65_127_OCTETS] = { "tx_65_127_octets", 1, 0,
		RTL9602C_MIB_TX_PKTS_65_127 },
	[RTL9602C_MIBI_TX_128_255_OCTETS] = { "tx_128_255_octets", 1, 0,
		RTL9602C_MIB_TX_PKTS_128_255 },
	[RTL9602C_MIBI_TX_256_511_OCTETS] = { "tx_256_511_octets", 1, 0,
		RTL9602C_MIB_TX_PKTS_256_511 },
	[RTL9602C_MIBI_TX_512_1023_OCTETS] = { "tx_512_1023_octets", 1, 0,
		RTL9602C_MIB_TX_PKTS_512_1023 },
	[RTL9602C_MIBI_TX_1024_1518_OCTETS] = { "tx_1024_1518_octets", 1, 0,
		RTL9602C_MIB_TX_PKTS_1024_1518 },
	[RTL9602C_MIBI_TX_1519_MAX_OCTETS] = { "tx_1519_max_octets", 1, 0,
		RTL9602C_MIB_TX_PKTS_1519_MAX },
};

/*
 * A 64-bit counter is read high, low, high, and again while the high word
 * moved, so a carry between the reads cannot corrupt the value.
 */
static u64 rtl9602c_mib_read(struct rtl9602c_priv *p, u32 addr, bool wide)
{
	u32 hi, lo;

	if (!wide)
		return rtl9602c_r(p, addr);

	do {
		hi = rtl9602c_r(p, addr + 4);
		lo = rtl9602c_r(p, addr);
	} while (hi != rtl9602c_r(p, addr + 4));

	return (u64)hi << 32 | lo;
}

/*
 * Bring @port's 64-bit totals up to date. A 32-bit counter runs free and
 * wraps, so its total grows by the difference since the last read, modulo
 * 2^32; the two 64-bit octet counters are taken as they read. Every reader
 * folds before it reads, and rtl9602c_mib_work() folds every port every
 * RTL9602C_MIB_FOLD_S seconds, well within the 19 minutes in which a counter
 * wraps at the SFP port's 2.5 Gb/s of frames of minimum size. A first fold
 * takes the counters' values as they stand. Called with mib_lock held.
 */
static void rtl9602c_mib_fold(struct rtl9602c_priv *p, int port)
{
	unsigned int i;

	for (i = 0; i < RTL9602C_MIB_NUM; i++) {
		u32 base = rtl9602c_mib[i].tx ? RTL9602C_MIB_TX(port)
					      : RTL9602C_MIB_RX(port);
		u64 raw = rtl9602c_mib_read(p, base + rtl9602c_mib[i].off,
					    rtl9602c_mib[i].wide);

		if (rtl9602c_mib[i].wide) {
			p->mib_total[port][i] = raw;
			continue;
		}
		p->mib_total[port][i] += (u32)raw - p->mib_last[port][i];
		p->mib_last[port][i] = raw;
	}
}

/* Fold @port's counters and copy their totals into @t. */
static void rtl9602c_mib_get(struct rtl9602c_priv *p, int port, u64 *t)
{
	unsigned long flags;

	spin_lock_irqsave(&p->mib_lock, flags);
	rtl9602c_mib_fold(p, port);
	memcpy(t, p->mib_total[port], sizeof(p->mib_total[port]));
	spin_unlock_irqrestore(&p->mib_lock, flags);
}

static void rtl9602c_mib_work(struct work_struct *w)
{
	struct rtl9602c_priv *p =
		container_of(to_delayed_work(w), struct rtl9602c_priv, mib_work);
	unsigned long flags;
	int port;

	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		spin_lock_irqsave(&p->mib_lock, flags);
		rtl9602c_mib_fold(p, port);
		spin_unlock_irqrestore(&p->mib_lock, flags);
	}
	rtl9602c_acl_log_fold(p);
	schedule_delayed_work(&p->mib_work, RTL9602C_MIB_FOLD_S * HZ);
}

static int rtl9602c_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;
	return ARRAY_SIZE(rtl9602c_mib);
}

static void rtl9602c_get_strings(struct dsa_switch *ds, int port,
				 u32 stringset, uint8_t *data)
{
	unsigned int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(rtl9602c_mib); i++)
		ethtool_puts(&data, rtl9602c_mib[i].name);
}

static void rtl9602c_get_ethtool_stats(struct dsa_switch *ds, int port,
				       uint64_t *data)
{
	rtl9602c_mib_get(ds->priv, port, data);
}

/*
 * The interface statistics from the port's counters, so that they count what
 * the port carried, bridged traffic included, and not only what crossed the
 * CPU. Packets are the unicast, multicast and broadcast frames together;
 * receive errors are the CRC and alignment, symbol and length errors; frames
 * dropped on receive are those the port dropped and those forwarding
 * discarded. The oversize counter is no length error: as RFC 2819's
 * etherStatsOversizePkts it counts every good frame longer than 1518 bytes,
 * and a full-sized frame with a VLAN tag is one. On an operator's line the
 * PON port counted 21.9 million of them among 26 million frames, PPPoE in a
 * VLAN, all forwarded, with one CRC error (measured).
 */
static void rtl9602c_get_stats64(struct dsa_switch *ds, int port,
				 struct rtnl_link_stats64 *s)
{
	u64 t[RTL9602C_MIB_NUM];

	rtl9602c_mib_get(ds->priv, port, t);

	s->rx_packets = t[RTL9602C_MIBI_RX_UNICAST] +
			t[RTL9602C_MIBI_RX_MULTICAST] +
			t[RTL9602C_MIBI_RX_BROADCAST];
	s->tx_packets = t[RTL9602C_MIBI_TX_UNICAST] +
			t[RTL9602C_MIBI_TX_MULTICAST] +
			t[RTL9602C_MIBI_TX_BROADCAST];
	s->rx_bytes = t[RTL9602C_MIBI_RX_OCTETS];
	s->tx_bytes = t[RTL9602C_MIBI_TX_OCTETS];
	s->multicast = t[RTL9602C_MIBI_RX_MULTICAST];
	s->rx_crc_errors = t[RTL9602C_MIBI_RX_CRC_ERRORS];
	s->rx_length_errors = t[RTL9602C_MIBI_RX_UNDERSIZE] +
			      t[RTL9602C_MIBI_RX_FRAGMENTS] +
			      t[RTL9602C_MIBI_RX_JABBERS];
	s->rx_frame_errors = t[RTL9602C_MIBI_RX_SYMBOL_ERRORS];
	s->rx_errors = s->rx_crc_errors + s->rx_length_errors +
		       s->rx_frame_errors;
	s->rx_dropped = t[RTL9602C_MIBI_RX_DROPS] +
			t[RTL9602C_MIBI_RX_FORWARD_DISCARDS];
	s->tx_dropped = t[RTL9602C_MIBI_TX_DISCARDS];
	s->collisions = t[RTL9602C_MIBI_TX_COLLISIONS];
	s->tx_aborted_errors = t[RTL9602C_MIBI_TX_EXCESSIVE_COLLISIONS];
	s->tx_window_errors = t[RTL9602C_MIBI_TX_LATE_COLLISIONS];
	s->tx_errors = s->tx_aborted_errors + s->tx_window_errors;
}

static void rtl9602c_get_pause_stats(struct dsa_switch *ds, int port,
				     struct ethtool_pause_stats *ps)
{
	u64 t[RTL9602C_MIB_NUM];

	rtl9602c_mib_get(ds->priv, port, t);
	ps->tx_pause_frames = t[RTL9602C_MIBI_TX_PAUSE];
	ps->rx_pause_frames = t[RTL9602C_MIBI_RX_PAUSE];
}

/* The size buckets of the counters, the last up to the longest frame. */
static const struct ethtool_rmon_hist_range rtl9602c_rmon_ranges[] = {
	{ 0, 64 },
	{ 65, 127 },
	{ 128, 255 },
	{ 256, 511 },
	{ 512, 1023 },
	{ 1024, 1518 },
	{ 1519, RTL9602C_FRAMELEN_LIMIT0_BYTES_MAX },
	{}
};

static void rtl9602c_get_rmon_stats(struct dsa_switch *ds, int port,
				    struct ethtool_rmon_stats *rs,
				    const struct ethtool_rmon_hist_range **ranges)
{
	u64 t[RTL9602C_MIB_NUM];
	unsigned int i;

	rtl9602c_mib_get(ds->priv, port, t);
	rs->undersize_pkts = t[RTL9602C_MIBI_RX_UNDERSIZE];
	rs->oversize_pkts = t[RTL9602C_MIBI_RX_OVERSIZE];
	rs->fragments = t[RTL9602C_MIBI_RX_FRAGMENTS];
	rs->jabbers = t[RTL9602C_MIBI_RX_JABBERS];
	for (i = 0; i < ARRAY_SIZE(rtl9602c_rmon_ranges) - 1; i++) {
		rs->hist[i] = t[RTL9602C_MIBI_RX_64_OCTETS + i];
		rs->hist_tx[i] = t[RTL9602C_MIBI_TX_64_OCTETS + i];
	}
	*ranges = rtl9602c_rmon_ranges;
}

static void rtl9602c_get_eth_mac_stats(struct dsa_switch *ds, int port,
				       struct ethtool_eth_mac_stats *ms)
{
	u64 t[RTL9602C_MIB_NUM];

	rtl9602c_mib_get(ds->priv, port, t);
	ms->FramesTransmittedOK = t[RTL9602C_MIBI_TX_UNICAST] +
				  t[RTL9602C_MIBI_TX_MULTICAST] +
				  t[RTL9602C_MIBI_TX_BROADCAST];
	ms->SingleCollisionFrames = t[RTL9602C_MIBI_TX_SINGLE_COLLISIONS];
	ms->MultipleCollisionFrames = t[RTL9602C_MIBI_TX_MULTIPLE_COLLISIONS];
	ms->FramesReceivedOK = t[RTL9602C_MIBI_RX_UNICAST] +
			       t[RTL9602C_MIBI_RX_MULTICAST] +
			       t[RTL9602C_MIBI_RX_BROADCAST];
	ms->FrameCheckSequenceErrors = t[RTL9602C_MIBI_RX_CRC_ERRORS];
	ms->OctetsTransmittedOK = t[RTL9602C_MIBI_TX_OCTETS];
	ms->FramesWithDeferredXmissions = t[RTL9602C_MIBI_TX_DEFERRED];
	ms->LateCollisions = t[RTL9602C_MIBI_TX_LATE_COLLISIONS];
	ms->FramesAbortedDueToXSColls = t[RTL9602C_MIBI_TX_EXCESSIVE_COLLISIONS];
	ms->OctetsReceivedOK = t[RTL9602C_MIBI_RX_OCTETS];
	ms->MulticastFramesXmittedOK = t[RTL9602C_MIBI_TX_MULTICAST];
	ms->BroadcastFramesXmittedOK = t[RTL9602C_MIBI_TX_BROADCAST];
	ms->MulticastFramesReceivedOK = t[RTL9602C_MIBI_RX_MULTICAST];
	ms->BroadcastFramesReceivedOK = t[RTL9602C_MIBI_RX_BROADCAST];
	ms->FrameTooLongErrors = t[RTL9602C_MIBI_RX_OVERSIZE];
}

static void rtl9602c_get_eth_ctrl_stats(struct dsa_switch *ds, int port,
					struct ethtool_eth_ctrl_stats *cs)
{
	u64 t[RTL9602C_MIB_NUM];

	rtl9602c_mib_get(ds->priv, port, t);
	cs->UnsupportedOpcodesReceived = t[RTL9602C_MIBI_RX_UNKNOWN_OPCODES];
}

static const struct dsa_switch_ops rtl9602c_switch_ops = {
	.port_rxtstamp		= rtl9602c_port_rxtstamp,
	.port_txtstamp		= rtl9602c_port_txtstamp,
	.get_ts_info		= rtl9602c_get_ts_info,
	.port_hwtstamp_get	= rtl9602c_port_hwtstamp_get,
	.port_hwtstamp_set	= rtl9602c_port_hwtstamp_set,
	.get_tag_protocol	= rtl9602c_get_tag_protocol,
	.setup			= rtl9602c_setup,
	.teardown		= rtl9602c_teardown,
	.port_get_default_prio	= rtl9602c_port_get_default_prio,
	.port_set_default_prio	= rtl9602c_port_set_default_prio,
	.port_get_dscp_prio	= rtl9602c_port_get_dscp_prio,
	.port_add_dscp_prio	= rtl9602c_port_add_dscp_prio,
	.port_del_dscp_prio	= rtl9602c_port_del_dscp_prio,
	.port_set_apptrust	= rtl9602c_port_set_apptrust,
	.port_get_apptrust	= rtl9602c_port_get_apptrust,
	.port_set_pcp_rewr	= rtl9602c_port_set_pcp_rewr,
	.devlink_param_get	= rtl9602c_devlink_param_get,
	.devlink_param_set	= rtl9602c_devlink_param_set,
	.phylink_get_caps	= rtl9602c_phylink_get_caps,
	.phylink_fixed_state	= rtl9602c_phylink_fixed_state,
	.port_enable		= rtl9602c_port_enable,
	.port_disable		= rtl9602c_port_disable,
	.port_stp_state_set	= rtl9602c_port_stp_state_set,
	.port_bridge_join	= rtl9602c_port_bridge_join,
	.port_bridge_leave	= rtl9602c_port_bridge_leave,
	.port_pre_bridge_flags	= rtl9602c_port_pre_bridge_flags,
	.port_bridge_flags	= rtl9602c_port_bridge_flags,
	.set_ageing_time	= rtl9602c_set_ageing_time,
	.port_vlan_filtering	= rtl9602c_port_vlan_filtering,
	.port_vlan_add		= rtl9602c_port_vlan_add,
	.port_vlan_del		= rtl9602c_port_vlan_del,

	.port_change_mtu	= rtl9602c_port_change_mtu,
	.port_max_mtu		= rtl9602c_port_max_mtu,
	.port_mirror_add	= rtl9602c_port_mirror_add,
	.port_mirror_del	= rtl9602c_port_mirror_del,
	.devlink_info_get	= rtl9602c_devlink_info_get,
	.port_setup_tc		= rtl9602c_port_setup_tc,
	.port_policer_add	= rtl9602c_port_policer_add,
	.port_policer_del	= rtl9602c_port_policer_del,
	.port_fdb_add		= rtl9602c_port_fdb_add,
	.port_fdb_del		= rtl9602c_port_fdb_del,
	.port_fdb_dump		= rtl9602c_port_fdb_dump,
	.port_mdb_add		= rtl9602c_port_mdb_add,
	.port_mdb_del		= rtl9602c_port_mdb_del,
	.port_fast_age		= rtl9602c_port_fast_age,
	.cls_flower_add		= rtl9602c_cls_flower_add,
	.cls_flower_del		= rtl9602c_cls_flower_del,
	.cls_flower_stats	= rtl9602c_cls_flower_stats,
	.get_sset_count		= rtl9602c_get_sset_count,
	.get_strings		= rtl9602c_get_strings,
	.get_ethtool_stats	= rtl9602c_get_ethtool_stats,
	.get_stats64		= rtl9602c_get_stats64,
	.get_pause_stats	= rtl9602c_get_pause_stats,
	.get_rmon_stats		= rtl9602c_get_rmon_stats,
	.get_eth_mac_stats	= rtl9602c_get_eth_mac_stats,
	.get_eth_ctrl_stats	= rtl9602c_get_eth_ctrl_stats,
};

/*
 * Map the register window, take the laser's transmit-disable line asserted, and register the switch
 * with DSA, which then calls rtl9602c_setup().
 */
static int rtl9602c_probe(struct platform_device *pdev)
{
	struct rtl9602c_priv *p;
	int err;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->gem_vid_flow = devm_kcalloc(&pdev->dev, VLAN_N_VID,
				       sizeof(*p->gem_vid_flow), GFP_KERNEL);
	if (!p->gem_vid_flow)
		return -ENOMEM;
	init_rwsem(&p->pon.sem);

	p->dev = &pdev->dev;
	spin_lock_init(&p->irq_enable_lock);
	INIT_DELAYED_WORK(&p->intr_rearm, rtl9602c_intr_rearm);
	spin_lock_init(&p->mib_lock);
	INIT_DELAYED_WORK(&p->mib_work, rtl9602c_mib_work);
	mutex_init(&p->sds_lock);
	mutex_init(&p->optics_lock);
	p->i2c_nr = -1;
	INIT_DELAYED_WORK(&p->sds_work, rtl9602c_sds_work);
	mutex_init(&p->table_lock);
	mutex_init(&p->fdb_lock);
	mutex_init(&p->acl_lock);
	mutex_init(&p->meter_lock);
	spin_lock_init(&p->ptp_lock);
	spin_lock_init(&p->ptp_tx_lock);
	mutex_init(&p->ptp_cfg_lock);
	p->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->reg))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->reg),
				     "cannot map the switch registers\n");

	/*
	 * Asserted from the start, so that the laser emits nothing before
	 * the PON path allows it. Optional: without it every TX_DISABLE step
	 * is skipped.
	 */
	p->tx_disable = devm_gpiod_get_optional(&pdev->dev, "tx-disable",
						GPIOD_OUT_HIGH);
	if (IS_ERR(p->tx_disable))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->tx_disable),
				     "cannot get the laser tx-disable gpio\n");

	err = rtl9602c_optics_level_register(p);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				     "cannot register the transmit power restore\n");
	p->ds.dev = &pdev->dev;
	p->ds.num_ports = RTL9602C_NUM_PORTS;
	p->ds.ops = &rtl9602c_switch_ops;

	/* One VLAN-aware bit serves the whole switch. */
	p->ds.vlan_filtering_is_global = true;

	/* The ageing time is a 21-bit count of 100 ms units. */
	p->ds.ageing_time_min = RTL9602C_FDB_CFG_AGEING_TIME_UNIT_MS;
	p->ds.ageing_time_max = RTL9602C_FDB_CFG_AGEING_TIME_MAX *
				RTL9602C_FDB_CFG_AGEING_TIME_UNIT_MS;
	p->ds.phylink_mac_ops = &rtl9602c_phylink_mac_ops;
	p->ds.priv = p;

	platform_set_drvdata(pdev, p);

	return dsa_register_switch(&p->ds);
}

static void rtl9602c_remove(struct platform_device *pdev)
{
	struct rtl9602c_priv *p = platform_get_drvdata(pdev);

	dsa_unregister_switch(&p->ds);
}

static const struct of_device_id rtl9602c_of_match[] = {
	{ .compatible = "realtek,rtl9602c-switch" },
	{ }
};

MODULE_DEVICE_TABLE(of, rtl9602c_of_match);

static struct platform_driver rtl9602c_driver = {
	.probe	= rtl9602c_probe,
	.remove	= rtl9602c_remove,
	.driver	= {
		.name		= "rtl9602c",
		.of_match_table	= rtl9602c_of_match,
	},
};

module_platform_driver(rtl9602c_driver);

MODULE_DESCRIPTION("Realtek RTL9602C switch");

MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_DEBUG_FS)

/* One directory, rtl9602c, at the debugfs root; each block adds its views. */
static void rtl9602c_debugfs_init(struct rtl9602c_priv *p)
{
	p->debugfs = debugfs_create_dir("rtl9602c", NULL);
	rtl9602c_offload_debugfs_init(p);
	rtl9602c_l2_debugfs_init(p);
	rtl9602c_optics_debugfs_init(p);
	rtl9602c_ptp_debugfs_init(p);
}

static void rtl9602c_debugfs_exit(struct rtl9602c_priv *p)
{
	debugfs_remove_recursive(p->debugfs);
	p->debugfs = NULL;
}
#else
static void rtl9602c_debugfs_init(struct rtl9602c_priv *p) { }
static void rtl9602c_debugfs_exit(struct rtl9602c_priv *p) { }
#endif
