// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9602C switch: the IEEE 1588 clock and receive timestamps
 *
 * The switch has a clock of its own, with 48-bit seconds and 8 ns ticks,
 * which is registered here as a PTP hardware clock.  It is read by latching
 * it, stepped by adding a signed offset, and slewed through a frequency
 * word.  Each PTP message the switch sends to the CPU port carries a tag
 * with the clock's time at the frame; the CPU-port driver turns the tag
 * into the frame's hardware timestamp, and rtl9602c_port_rxtstamp()
 * completes its seconds.  The switch produces no departure time for a
 * message the CPU sends, so only receive stamping is offered.
 *
 * The debugfs views show the per-port and per-message PTP settings and the
 * time-of-day transmitter, which the driver reads and never writes.
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
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "rtl9602c.h"

/**
 * rtl9602c_ptp_rd_sec() - read a 48-bit seconds value
 * @p: driver state
 * @base: offset of the first of its two words
 *
 * Seconds take two words, the high part at the lower address: bits 47:32
 * of the value are bits 15:0 of the first word, and bits 31:0 are the whole
 * second word.  The clock's own words (%RTL9602C_PTP_CLK_SEC) hold what the
 * last latch command captured, not the running time, so a caller reading
 * the clock sets %RTL9602C_PTP_CLK_CTRL_LATCH first, holding @p->ptp_lock
 * across the latch and the reads, as rtl9602c_ptp_gettime() does.
 *
 * Return: the seconds.
 */
u64 rtl9602c_ptp_rd_sec(struct rtl9602c_priv *p, u32 base)
{
	return (u64)rtl9602c_r(p, base + 4) |
	       (u64)FIELD_GET(RTL9602C_PTP_SEC_UPPER, rtl9602c_r(p, base)) << 32;
}

/* The two-word layout of rtl9602c_ptp_rd_sec(). */
static void rtl9602c_ptp_wr_sec(struct rtl9602c_priv *p, u32 base, u64 sec)
{
	rtl9602c_w(p, base, FIELD_PREP(RTL9602C_PTP_SEC_UPPER, sec >> 32));
	rtl9602c_w(p, base + 4, lower_32_bits(sec));
}

/**
 * rtl9602c_ptp_now() - read the switch's clock
 * @p: driver private data
 * @ts: set to the time
 *
 * Every read of the clock goes through here, so that each one keeps the
 * seconds from which rtl9602c_port_rxtstamp() completes receive stamps.
 *
 * Context: process context; takes @p->ptp_lock.
 */
void rtl9602c_ptp_now(struct rtl9602c_priv *p, struct timespec64 *ts)
{
	u32 nsec;

	mutex_lock(&p->ptp_lock);

	/*
	 * The time registers keep what the last latch captured: latch the
	 * running clock into both of them as one instant, then read them.
	 * The lock makes the latch and the reads one operation, so that two
	 * callers cannot hand each other's capture back.  Each write of 1 to
	 * LATCH or STEP acts once, and setting one does not repeat the other
	 * (measured on this board), so the driver only ever sets them.
	 */
	rtl9602c_rmw(p, RTL9602C_PTP_CLK_CTRL, 0, RTL9602C_PTP_CLK_CTRL_LATCH);
	ts->tv_sec = rtl9602c_ptp_rd_sec(p, RTL9602C_PTP_CLK_SEC);
	nsec = rtl9602c_r(p, RTL9602C_PTP_CLK_NSEC);
	mutex_unlock(&p->ptp_lock);

	/* The seconds that rtl9602c_port_rxtstamp() completes stamps from. */
	atomic64_set(&p->ptp_sec_seen, ts->tv_sec);

	ts->tv_nsec = FIELD_GET(RTL9602C_PTP_NSEC_TICKS, nsec) *
		      RTL9602C_PTP_NSEC_TICK_NS;
}

static int rtl9602c_ptp_gettime(struct ptp_clock_info *info,
				struct timespec64 *ts)
{
	rtl9602c_ptp_now(to_rtl9602c_ptp(info), ts);
	return 0;
}

/*
 * No register takes an absolute time.  The step registers hold a signed
 * offset, which the step command adds to the running clock: the seconds in
 * two's complement over 48 bits and the nanoseconds as the non-negative
 * remainder within the second, so -1.5 s is written as -2 s and
 * 500,000,000 ns.
 */
static int rtl9602c_ptp_offset(struct rtl9602c_priv *p, s64 delta_ns)
{
	u64 sec;
	u32 nsec;

	if (delta_ns >= 0) {
		sec = div_u64_rem((u64)delta_ns, NSEC_PER_SEC, &nsec);
	} else {
		u64 mag = (u64)-delta_ns;
		u32 rem;

		sec = div_u64_rem(mag, NSEC_PER_SEC, &rem);
		if (rem) {
			/* Borrow a second to keep the nanoseconds positive. */
			sec = ~(sec + 1) + 1;
			nsec = NSEC_PER_SEC - rem;
		} else {
			sec = ~sec + 1;
			nsec = 0;
		}
	}

	mutex_lock(&p->ptp_lock);
	rtl9602c_ptp_wr_sec(p, RTL9602C_PTP_CLK_STEP_SEC, sec);
	rtl9602c_w(p, RTL9602C_PTP_CLK_STEP_NSEC,
		   FIELD_PREP(RTL9602C_PTP_NSEC_TICKS,
			      nsec / RTL9602C_PTP_NSEC_TICK_NS));
	/*
	 * Adds the offset once.  The read-modify-write keeps the PON
	 * time-of-day enable, bit 2 of the same register.
	 */
	rtl9602c_rmw(p, RTL9602C_PTP_CLK_CTRL, 0, RTL9602C_PTP_CLK_CTRL_STEP);
	mutex_unlock(&p->ptp_lock);
	return 0;
}

static int rtl9602c_ptp_adjtime(struct ptp_clock_info *info, s64 delta)
{
	return rtl9602c_ptp_offset(to_rtl9602c_ptp(info), delta);
}

/*
 * With no absolute time register, setting the clock is a read followed by
 * a step of the difference.  The time between the two passes on the clock
 * as well, so what is lost is only the time from the caller's read of its
 * own clock to this read, about 20 us on this board.
 */
static int rtl9602c_ptp_settime(struct ptp_clock_info *info,
				const struct timespec64 *ts)
{
	struct rtl9602c_priv *p = to_rtl9602c_ptp(info);
	struct timespec64 now;
	s64 delta;
	int err;

	err = rtl9602c_ptp_gettime(info, &now);
	if (err)
		return err;

	delta = timespec64_to_ns(ts) - timespec64_to_ns(&now);
	return rtl9602c_ptp_offset(p, delta);
}

/*
 * The frequency word reads 2^27 (RTL9602C_PTP_CLK_FREQ_WORD_NOMINAL) when
 * nothing has adjusted it, and the clock then advances one second per
 * second.  The clock's rate follows the word in proportion, so an offset
 * scales the word to 2^27 * (1 + ppm / 10^6), computed in 64 bits:
 * measured from 4.5 ppm to 94 % of the rate either way, the clock timed
 * against CLOCK_MONOTONIC.
 */
static int rtl9602c_ptp_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	struct rtl9602c_priv *p = to_rtl9602c_ptp(info);
	u64 word = adjust_by_scaled_ppm(RTL9602C_PTP_CLK_FREQ_WORD_NOMINAL,
					scaled_ppm);
	u32 freq;

	freq = clamp_t(u64, word, 1, RTL9602C_PTP_CLK_FREQ_WORD);
	mutex_lock(&p->ptp_lock);
	rtl9602c_rmw(p, RTL9602C_PTP_CLK_FREQ, RTL9602C_PTP_CLK_FREQ_WORD,
		     FIELD_PREP(RTL9602C_PTP_CLK_FREQ_WORD, freq));
	mutex_unlock(&p->ptp_lock);
	return 0;
}

/*
 * The clock's only request is PPS.  The switch raises an interrupt once a
 * second, which the interrupt handler counts and reports as a
 * PTP_CLOCK_PPS event.  The source is enabled only while the PTP core
 * wants PPS events: enabled with no consumer, it is one interrupt a second
 * for nothing.  The source latches in RTL9602C_IRQ_PENDING while it is
 * disabled too, so the latch is cleared before the source is enabled, and
 * the first event is the next pulse rather than one from before.  That the
 * pulse falls on the clock's second boundary and follows its steps and
 * frequency changes is assumed, not verified.
 */
static int rtl9602c_ptp_enable(struct ptp_clock_info *info,
			       struct ptp_clock_request *rq, int on)
{
	struct rtl9602c_priv *p = to_rtl9602c_ptp(info);

	if (rq->type != PTP_CLK_REQ_PPS)
		return -EOPNOTSUPP;

	if (on)
		rtl9602c_w(p, RTL9602C_IRQ_PENDING, RTL9602C_IRQ_SRC_PTP_PPS);
	rtl9602c_irq_enable(p, on ? 0 : RTL9602C_IRQ_SRC_PTP_PPS,
			    on ? RTL9602C_IRQ_SRC_PTP_PPS : 0);
	return 0;
}

/**
 * rtl9602c_get_ts_info() - report the timestamping capabilities of a port
 * @ds: the switch
 * @port: the port
 * @ts: filled with the capabilities
 *
 * Hardware stamps cover received PTP event messages only.  The switch
 * produces no departure time for a message the CPU sends, so the only
 * transmit type is %HWTSTAMP_TX_OFF.  Without that departure time a port
 * can stamp the Syncs it receives but cannot time its own Delay_Req, which
 * the delay request-response mechanism needs to measure the path delay
 * (IEEE 1588-2008 11.3).  The PHC index is -1 when no clock is registered.
 *
 * Return: 0.
 */
int rtl9602c_get_ts_info(struct dsa_switch *ds, int port,
			 struct kernel_ethtool_ts_info *ts)
{
	struct rtl9602c_priv *p = ds->priv;

	ts->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
			      SOF_TIMESTAMPING_RX_SOFTWARE |
			      SOF_TIMESTAMPING_SOFTWARE |
			      SOF_TIMESTAMPING_RX_HARDWARE |
			      SOF_TIMESTAMPING_RAW_HARDWARE;
	ts->phc_index = p->ptp_clock ? ptp_clock_index(p->ptp_clock) : -1;
	ts->tx_types = BIT(HWTSTAMP_TX_OFF);
	ts->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			 BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
			 BIT(HWTSTAMP_FILTER_PTP_V2_L2_SYNC) |
			 BIT(HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ);
	return 0;
}

/**
 * rtl9602c_ptp_register() - register the switch clock as a PTP hardware clock
 * @p: driver state
 *
 * The clock runs from power-on, and a warm reboot does not reset it.  It is
 * neither started, reset nor set here, so the PHC is independent of the
 * kernel's clocks.  It has one PPS source and no external timestamp or
 * periodic output channel; no pulse is driven out of the chip.  Without
 * CONFIG_PTP_1588_CLOCK no clock is registered and @p->ptp_clock stays
 * NULL.
 *
 * Return: 0 or a negative error code.
 */
/*
 * The tag of a received PTP message carries the low 16 bits of the seconds,
 * and rtl9602c_port_rxtstamp() takes the rest from the last read of the
 * clock. Reading the clock once a minute keeps that read within a minute of
 * the clock, far inside half the tag's window of 65536 s, whether or not
 * anything else reads it; the first read comes at registration.
 */
#define RTL9602C_PTP_AUX_PERIOD		(60 * HZ)

static long rtl9602c_ptp_aux_work(struct ptp_clock_info *info)
{
	struct timespec64 ts;

	rtl9602c_ptp_gettime(info, &ts);
	return RTL9602C_PTP_AUX_PERIOD;
}

int rtl9602c_ptp_register(struct rtl9602c_priv *p)
{
	p->ptp_info = (struct ptp_clock_info) {
		.owner		= THIS_MODULE,
		.name		= "rtl9602c-ptp",
		/*
		 * The largest offset whose scaled_ppm fits the long that
		 * adjfine takes on this 32-bit architecture: about 3.3 %,
		 * well inside the word's range.
		 */
		.max_adj	= (S32_MAX >> 16) * 1000,
		.n_ext_ts	= 0,
		.pps		= 1,
		.adjfine	= rtl9602c_ptp_adjfine,
		.adjtime	= rtl9602c_ptp_adjtime,
		.gettime64	= rtl9602c_ptp_gettime,
		.settime64	= rtl9602c_ptp_settime,
		.enable		= rtl9602c_ptp_enable,
		.do_aux_work	= rtl9602c_ptp_aux_work,
	};

	p->ptp_clock = ptp_clock_register(&p->ptp_info, p->dev);
	if (IS_ERR(p->ptp_clock)) {
		int err = PTR_ERR(p->ptp_clock);

		p->ptp_clock = NULL;
		return err;
	}
	ptp_schedule_worker(p->ptp_clock, 0);
	return 0;
}

/**
 * rtl9602c_ptp_unregister() - remove the PTP hardware clock, if registered
 * @p: driver state
 */
void rtl9602c_ptp_unregister(struct rtl9602c_priv *p)
{
	if (p->ptp_clock) {
		ptp_clock_unregister(p->ptp_clock);
		p->ptp_clock = NULL;
	}
}

/**
 * rtl9602c_port_rxtstamp() - complete the hardware timestamp of a PTP frame
 * @ds: the switch
 * @port: the port the frame arrived on
 * @skb: the frame
 * @type: its PTP class
 *
 * The switch puts a 16-byte tag into each PTP message it sends to the CPU
 * port, holding the time of its clock at the frame; the CPU-port driver
 * has already taken the tag off and made that time the frame's hardware
 * timestamp.  The tag carries only the low 16 bits of the seconds.  The
 * rest come from the seconds of the last clock read, in the 65,536-second
 * window that puts the stamp nearest to them.  The result is right only
 * while the clock has been read within about nine hours (32,768 s) of the
 * frame, which rtl9602c_ptp_aux_work() makes sure of by reading it at
 * registration and once a minute after.  The choice of window is an
 * assumption, not verified across a window boundary.  The
 * switch tags its PTP messages whatever the filter, so on a port whose
 * receive filter is %HWTSTAMP_FILTER_NONE the stamp is taken off instead.
 *
 * Return: false; the frame is delivered at once.
 */
bool rtl9602c_port_rxtstamp(struct dsa_switch *ds, int port,
			    struct sk_buff *skb, unsigned int type)
{
	struct skb_shared_hwtstamps *hw = skb_hwtstamps(skb);
	struct rtl9602c_priv *p = ds->priv;
	u64 ns, tag_sec, seen, sec;
	u32 rem;

	/* No tag, no stamp to complete. */
	if (!hw->hwtstamp)
		return false;

	if (p->hwtstamp[port].rx_filter == HWTSTAMP_FILTER_NONE) {
		hw->hwtstamp = 0;
		return false;
	}

	ns = ktime_to_ns(hw->hwtstamp);
	tag_sec = div_u64_rem(ns, NSEC_PER_SEC, &rem) & 0xffff;
	seen = (u64)atomic64_read(&p->ptp_sec_seen);
	sec = (seen & ~0xffffULL) | tag_sec;

	/* Move to the neighbouring window when that one is nearer. */
	if (sec > seen + 0x8000)
		sec -= 0x10000;
	else if (seen > sec + 0x8000)
		sec += 0x10000;

	hw->hwtstamp = ktime_set(sec, rem);
	return false;
}

/**
 * rtl9602c_port_hwtstamp_get() - report a port's timestamping configuration
 * @ds: the switch
 * @port: the port
 * @config: filled with the configuration last set on @port
 *
 * Return: 0.
 */
int rtl9602c_port_hwtstamp_get(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config)
{
	struct rtl9602c_priv *p = ds->priv;

	*config = p->hwtstamp[port];
	return 0;
}

/**
 * rtl9602c_port_hwtstamp_set() - set a port's timestamping configuration
 * @ds: the switch
 * @port: the port
 * @config: the requested configuration, updated to the receive filter
 *	applied
 * @extack: netlink extended ack, told why a transmit type is refused
 *
 * The switch tags every PTP message it sends to the CPU port whatever the
 * per-port PTP enable and the message actions say, so there is nothing to
 * switch on: the configuration is recorded and no register is written, and
 * rtl9602c_port_rxtstamp() takes the stamps off under %HWTSTAMP_FILTER_NONE.
 * Transmit timestamping stays off, as no departure time is produced for a
 * message the CPU sends.
 *
 * Return: 0, or -ERANGE for a transmit type other than %HWTSTAMP_TX_OFF or
 * a receive filter the port cannot provide.
 */
int rtl9602c_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config,
			       struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = ds->priv;

	if (config->tx_type != HWTSTAMP_TX_OFF) {
		NL_SET_ERR_MSG_MOD(extack,
				   "transmit timestamping is not implemented: the hardware has a per-port indication and no driver behind it");
		return -ERANGE;
	}

	/*
	 * Only PTP messages are tagged, and a tagged message over UDP has not
	 * been seen, so a layer-2 PTP filter is answered with the layer-2
	 * event filter, which covers the others, and a request for every
	 * frame or for UDP messages is refused: a filter may be widened, not
	 * narrowed.  That the layer-2 filter covers every event message is
	 * assumed: it is verified for Sync only.
	 */
	switch (config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		config->rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack,
				   "only PTP version 2 messages over Ethernet are stamped");
		return -ERANGE;
	}

	p->hwtstamp[port] = *config;
	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * PTP message types in the order of the 2-bit fields of the action
 * registers, type n at bits 2n+1:2n.  Type 0 is Sync; the names of the
 * others are not verified.
 */
static const char * const rtl9602c_ptp_msg_names[RTL9602C_PTP_MSG_TYPES] = {
	"sync", "delay_req", "pdelay_req", "pdelay_resp", "follow_up",
	"delay_resp", "pdelay_resp_fup", "announce", "sync_1step",
	"pdelay_req_1step",
};

/*
 * ptpstat, per port: the PTP enable, which receive stamping does not depend
 * on; the transparent-clock bit, whose effect is not verified; and whether
 * a PTP message has left the port since the last read.  That indication
 * says a message left, not that a timestamp is ready, and reading it clears
 * it.  Then the ingress and egress action of each message type, the count
 * of once-per-second pulses and whether their interrupt is enabled, the PON
 * time of day and the receive capture.  Apart from that interrupt enable,
 * the driver writes none of these registers.
 */
static int rtl9602c_ptpstat_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 igr = rtl9602c_r(p, RTL9602C_PTP_ACTION_INGRESS);
	u32 egr = rtl9602c_r(p, RTL9602C_PTP_ACTION_EGRESS);
	u32 trans = rtl9602c_r(p, RTL9602C_PTP_TRANSPARENT_PORTS);
	unsigned int port, type;
	u32 hi, lo;

	seq_puts(s, "port  enabled transparent tx_latched\n");
	for (port = 0; port < RTL9602C_NUM_PORTS; port++) {
		u32 ind = rtl9602c_r(p, RTL9602C_PTP_TX_STATUS(port));

		seq_printf(s, "%4u %8u %11u %10u\n", port,
			   rtl9602c_r(p, RTL9602C_PTP_PORT_EN + 4 * port) & 1,
			   !!(trans & BIT(port)),
			   !!(ind & RTL9602C_PTP_TX_STATUS_LATCHED));
	}

	seq_puts(s, "\nmessage          ingress egress\n");
	for (type = 0; type < RTL9602C_PTP_MSG_TYPES; type++)
		seq_printf(s, "%-16s %7u %6u\n", rtl9602c_ptp_msg_names[type],
			   (u32)((igr >> (2 * type)) & RTL9602C_PTP_ACTION_MASK),
			   (u32)((egr >> (2 * type)) & RTL9602C_PTP_ACTION_MASK));

	seq_printf(s, "\npulses %u  armed %u\n", READ_ONCE(p->ptp_pps_count),
		   !!(rtl9602c_r(p, RTL9602C_IRQ_ENABLE) &
		      RTL9602C_IRQ_SRC_PTP_PPS));

	/*
	 * A second time value beside the clock, with its enable in CLK_CTRL;
	 * nothing in the driver writes either.  It is taken to hold the time
	 * of day an ONU learns from the OLT (ITU-T G.988 9.12.2, OLT-G), in
	 * the clock's layout; not verified.
	 */
	{
		u32 ctrl = rtl9602c_r(p, RTL9602C_PTP_CLK_CTRL);

		seq_printf(s, "pon tod %llu.%09lu  enabled %u\n",
			   rtl9602c_ptp_rd_sec(p, RTL9602C_PTP_PON_TIME_SEC),
			   FIELD_GET(RTL9602C_PTP_NSEC_TICKS,
				     rtl9602c_r(p, RTL9602C_PTP_PON_TIME_NSEC)) *
			   RTL9602C_PTP_NSEC_TICK_NS,
			   !!(ctrl & RTL9602C_PTP_CLK_CTRL_PON_TIME_EN));
	}

	/*
	 * A receive capture that timestamping does not use: it stays zero
	 * while stamped Syncs arrive.  The field layout decoded here is not
	 * verified.
	 */
	hi = rtl9602c_r(p, RTL9602C_PTP_RX_TSTAMP);
	lo = rtl9602c_r(p, RTL9602C_PTP_RX_TSTAMP + 4);
	seq_printf(s, "\ncapture %u.%09lu  (raw %08x %08x)\n",
		   (u32)FIELD_GET(RTL9602C_PTP_RX_TSTAMP_SEC, hi),
		   FIELD_GET(RTL9602C_PTP_RX_TSTAMP_NSEC, lo) *
		   RTL9602C_PTP_NSEC_TICK_NS, hi, lo);
	return 0;
}

static int rtl9602c_ptpstat_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_ptpstat_show, inode->i_private);
}

/*
 * todstat shows the time-of-day transmitter, which sends a pulse once a
 * second and, after a programmable delay, a serial time message.  The
 * driver neither writes it nor enables an output pad for it, so this is
 * the state the board comes up in.  The GPS week and UTC second fields read
 * 1877 and 1,451,606,400 on this board, one and the same instant
 * (2016-01-01T00:00:00Z).  The positions and units of the pulse, message
 * and baud fields, and the byte order of the message, are not verified.
 * The divisor reads 1085: about 115,200 baud if it divides a 125 MHz clock.
 */
static int rtl9602c_todstat_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	u32 pps = rtl9602c_r(p, RTL9602C_TOD_PPS_CFG);
	u32 cfg = rtl9602c_r(p, RTL9602C_TOD_MSG_CFG);
	unsigned int i;

	seq_printf(s, "version   %08x\n", rtl9602c_r(p, RTL9602C_TOD_VERSION));
	seq_printf(s, "pulse     width %lu (x10 ms)  delay %lu (ms)\n",
		   FIELD_GET(RTL9602C_TOD_PPS_CFG_WIDTH, pps),
		   FIELD_GET(RTL9602C_TOD_PPS_CFG_MSG_DELAY, pps));
	seq_printf(s, "frame     %lu bytes, %s\n",
		   FIELD_GET(RTL9602C_TOD_MSG_CFG_LEN, cfg),
		   (cfg & RTL9602C_TOD_MSG_CFG_SRC_SW) ? "software" : "hardware");
	seq_printf(s, "hardware  gps week %lu  utc sec %u\n",
		   FIELD_GET(RTL9602C_TOD_GPS_WEEK_NUM,
			     rtl9602c_r(p, RTL9602C_TOD_GPS_WEEK)),
		   rtl9602c_r(p, RTL9602C_TOD_UTC_SEC));
	seq_printf(s, "baud div  %lu\n",
		   FIELD_GET(RTL9602C_TOD_BAUD_DIVISOR,
			     rtl9602c_r(p, RTL9602C_TOD_BAUD)));

	/*
	 * The bytes sent as the message when its source is software, in
	 * message order: byte i is byte i % 4 of word i / 4, and word 0 is at
	 * the highest address.
	 */
	seq_puts(s, "software ");
	for (i = 0; i < RTL9602C_TOD_MSG_BYTES; i++) {
		u32 w = rtl9602c_r(p, RTL9602C_TOD_MSG_DATA(i / 4));

		seq_printf(s, " %02x", (w >> (8 * (i % 4))) & 0xff);
	}
	seq_puts(s, "\n");
	return 0;
}

static int rtl9602c_todstat_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_todstat_show, inode->i_private);
}

static const struct file_operations rtl9602c_todstat_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_todstat_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations rtl9602c_ptpstat_fops = {
	.owner		= THIS_MODULE,
	.open		= rtl9602c_ptpstat_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/**
 * rtl9602c_ptp_debugfs_init() - create the PTP debugfs views
 * @p: driver state, with its debugfs directory created
 *
 * Creates "ptpstat" and "todstat", readable by root only.  Neither writes a
 * register, but reading "ptpstat" clears the per-port indications of a PTP
 * message sent.
 */
void rtl9602c_ptp_debugfs_init(struct rtl9602c_priv *p)
{
	debugfs_create_file("todstat", 0400, p->debugfs, p,
			    &rtl9602c_todstat_fops);
	debugfs_create_file("ptpstat", 0400, p->debugfs, p,
			    &rtl9602c_ptpstat_fops);
}

#endif /* CONFIG_DEBUG_FS */
