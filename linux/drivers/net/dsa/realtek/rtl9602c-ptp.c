// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9602C switch: the IEEE 1588 clock and its timestamps
 *
 * The switch has a clock of its own, with 48-bit seconds and 8 ns ticks,
 * which is registered here as a PTP hardware clock.  It is read by latching
 * it, stepped by adding a signed offset, and slewed through a frequency
 * word.  Each PTP message the switch sends to the CPU port carries a tag
 * with the clock's time at the frame; the CPU-port driver turns the tag
 * into the frame's hardware timestamp, and rtl9602c_port_rxtstamp()
 * completes its seconds.
 *
 * A PTP event message the CPU sends is stamped when it leaves: under the
 * egress action that latches the time and reports it, the switch sends the
 * CPU a departure report, which the CPU-port driver hands to
 * rtl9602c_ptp_departure().  The report does not say which message left, so
 * one message at a time awaits its report (rtl9602c_port_txtstamp()).
 *
 * The debugfs views show the per-port and per-message PTP settings and the
 * time-of-day transmitter.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/dsa/rtl9602c.h>
#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>
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
 * Context: any; takes @p->ptp_lock, a spinlock.
 */
void rtl9602c_ptp_now(struct rtl9602c_priv *p, struct timespec64 *ts)
{
	unsigned long flags;
	u32 nsec;

	spin_lock_irqsave(&p->ptp_lock, flags);

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
	spin_unlock_irqrestore(&p->ptp_lock, flags);

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
	unsigned long flags;
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

	spin_lock_irqsave(&p->ptp_lock, flags);
	rtl9602c_ptp_wr_sec(p, RTL9602C_PTP_CLK_STEP_SEC, sec);
	rtl9602c_w(p, RTL9602C_PTP_CLK_STEP_NSEC,
		   FIELD_PREP(RTL9602C_PTP_NSEC_TICKS,
			      nsec / RTL9602C_PTP_NSEC_TICK_NS));
	/*
	 * Adds the offset once.  The read-modify-write keeps the PON
	 * time-of-day enable, bit 2 of the same register.
	 */
	rtl9602c_rmw(p, RTL9602C_PTP_CLK_CTRL, 0, RTL9602C_PTP_CLK_CTRL_STEP);
	spin_unlock_irqrestore(&p->ptp_lock, flags);
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
	unsigned long flags;
	u32 freq;

	freq = clamp_t(u64, word, 1, RTL9602C_PTP_CLK_FREQ_WORD);
	spin_lock_irqsave(&p->ptp_lock, flags);
	rtl9602c_rmw(p, RTL9602C_PTP_CLK_FREQ, RTL9602C_PTP_CLK_FREQ_WORD,
		     FIELD_PREP(RTL9602C_PTP_CLK_FREQ_WORD, freq));
	spin_unlock_irqrestore(&p->ptp_lock, flags);
	return 0;
}

/* Downstream frames in a second of the OLT's clock, the period of its pulse. */
#define RTL9602C_PTP_PON_SECOND_FRAMES	8000

/**
 * rtl9602c_ptp_pon_irq() - note the clock at a GPON interrupt
 * @p: driver state
 *
 * While the OLT's seconds are wanted (rtl9602c_ptp_pon_second()), the first
 * half of every GPON interrupt reads the superframe counter and the clock,
 * as near to the frame the interrupt came for as the interrupt itself.
 *
 * Context: the first half of the switch interrupt.
 */
void rtl9602c_ptp_pon_irq(struct rtl9602c_priv *p)
{
	struct timespec64 ts;

	if (!READ_ONCE(p->ptp_pon_extts))
		return;

	p->ptp_pon_irq_frame = FIELD_GET(RTL9602C_GTC_DS_SUPERFRAME_CNT,
					 rtl9602c_r(p, RTL9602C_GTC_DS_SUPERFRAME));
	rtl9602c_ptp_now(p, &ts);
	p->ptp_pon_irq_ns = timespec64_to_ns(&ts);
}

/**
 * rtl9602c_ptp_pon_second() - report a second of the OLT's clock
 * @p: driver state
 *
 * The GPON MAC raises its interrupt once every 8000 downstream frames, a
 * second of the OLT's clock (rtl9602c_pon_second_enable()).  The time noted
 * in the interrupt's first half (rtl9602c_ptp_pon_irq()) is reported as an
 * external timestamp of channel 0 when the interrupt came in the frame the
 * second began in, that is a whole number of seconds of frames after the one
 * reported last.  An interrupt that came a frame or more late is counted and
 * not reported; one that finds the last reported late reports nothing and
 * starts the count of frames from itself.
 *
 * Context: the GPON interrupt's thread.
 */
void rtl9602c_ptp_pon_second(struct rtl9602c_priv *p)
{
	struct ptp_clock_event ev = { .type = PTP_CLOCK_EXTTS, .index = 0 };
	u32 frame = p->ptp_pon_irq_frame;
	u32 off;

	if (!READ_ONCE(p->ptp_pon_extts) || !p->ptp_clock)
		return;

	off = ((frame - p->ptp_pon_frame) & RTL9602C_GTC_DS_SUPERFRAME_CNT) %
	      RTL9602C_PTP_PON_SECOND_FRAMES;
	if (p->ptp_pon_frame_valid && off) {
		if (off < RTL9602C_PTP_PON_SECOND_FRAMES / 2) {
			p->ptp_pon_late++;
			return;
		}
		p->ptp_pon_frame = frame;
		return;
	}

	p->ptp_pon_frame = frame;
	p->ptp_pon_frame_valid = true;
	p->ptp_pon_seconds++;
	ev.timestamp = p->ptp_pon_irq_ns;
	ptp_clock_event(p->ptp_clock, &ev);
}

/*
 * The OLT's seconds as external timestamps of channel 0, on or off: the GPON
 * MAC's interrupt once a second, and the clock noted at it.
 */
static int rtl9602c_ptp_pon_extts(struct rtl9602c_priv *p, bool on)
{
	int err;

	if (on) {
		p->ptp_pon_frame_valid = false;
		WRITE_ONCE(p->ptp_pon_extts, true);
	}
	err = rtl9602c_pon_second_enable(p, on);
	if (err || !on)
		WRITE_ONCE(p->ptp_pon_extts, false);
	return err;
}

/*
 * The clock's requests are PPS and an external timestamp, the OLT's seconds
 * (rtl9602c_ptp_pon_second()).  For PPS the switch raises an interrupt once a
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

	if (rq->type == PTP_CLK_REQ_EXTTS)
		return rtl9602c_ptp_pon_extts(p, on);
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
 * Hardware stamps cover the PTP messages over Ethernet that a port receives
 * and the event messages over Ethernet that it sends, the latter as two-step
 * stamps (%HWTSTAMP_TX_ON), or with the time written into a Sync as it leaves
 * (%HWTSTAMP_TX_ONESTEP_SYNC) and the turnaround into a Pdelay_Resp
 * (%HWTSTAMP_TX_ONESTEP_P2P), and only while the CPU-port driver hands the
 * switch's departure reports on (@p->ptp_tx_ready).  The PHC index is -1
 * when no clock is registered.
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
	if (p->ptp_tx_ready) {
		ts->so_timestamping |= SOF_TIMESTAMPING_TX_HARDWARE;
		ts->tx_types |= BIT(HWTSTAMP_TX_ON) | BIT(HWTSTAMP_TX_ONESTEP_SYNC) |
				BIT(HWTSTAMP_TX_ONESTEP_P2P);
	}
	ts->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			 BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
			 BIT(HWTSTAMP_FILTER_PTP_V2_L2_SYNC) |
			 BIT(HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ);
	return 0;
}

/*
 * A stamp from the switch, in a receive tag or a departure report, carries
 * the low 16 bits of the clock's seconds.  The rest come from the seconds of
 * the last clock read, in the 65,536-second window that puts the stamp
 * nearest to them.  The result is right only while the clock has been read
 * within about nine hours (32,768 s) of the stamp, which
 * rtl9602c_ptp_aux_work() makes sure of by reading it at registration and
 * once a minute after.
 */
static ktime_t rtl9602c_ptp_complete(struct rtl9602c_priv *p, ktime_t stamp)
{
	u64 tag_sec, seen, sec;
	u32 rem;

	tag_sec = div_u64_rem(ktime_to_ns(stamp), NSEC_PER_SEC, &rem) & 0xffff;
	seen = (u64)atomic64_read(&p->ptp_sec_seen);
	sec = (seen & ~0xffffULL) | tag_sec;

	/* Move to the neighbouring window when that one is nearer. */
	if (sec > seen + 0x8000)
		sec -= 0x10000;
	else if (seen > sec + 0x8000)
		sec += 0x10000;

	return ktime_set(sec, rem);
}

/*
 * The egress action slots of the event messages: the two kinds of Sync,
 * Delay_Req, the two kinds of Pdelay_Req and Pdelay_Resp.
 */
#define RTL9602C_PTP_EVENT_SLOTS	(BIT(RTL9602C_PTP_SLOT_SYNC) |		\
					 BIT(RTL9602C_PTP_SLOT_DELAY_REQ) |	\
					 BIT(RTL9602C_PTP_SLOT_PDELAY_REQ_2STEP) | \
					 BIT(RTL9602C_PTP_SLOT_PDELAY_RESP) |	\
					 BIT(RTL9602C_PTP_SLOT_SYNC_1STEP) |	\
					 BIT(RTL9602C_PTP_SLOT_PDELAY_REQ))

/*
 * How long a message waits for its departure report before the next message
 * may take its place.  Reports come within milliseconds; a report that comes
 * later than this is handed to the next message awaited, if there is one.
 */
#define RTL9602C_PTP_TX_TIMEOUT		(HZ / 10)

/*
 * Turn the departure reports on or off.  A message the CPU sends gets its
 * egress action only when the PTP enable of the port it enters by, the CPU
 * port, is set: the enable of the port it leaves by makes no difference
 * (measured).  The action latches the time and reports it for each event
 * message, but for a one-step Sync while a port sends them (@onestep), whose
 * action writes its departure time into its originTimestamp, and a
 * Pdelay_Resp while a port answers one-step (@p2p), whose action adds the
 * time from the reference to its departure to its correctionField.  The
 * general messages keep no action.  A message gets the action
 * only when the CPU-port driver sends it so (see the transmit path of
 * rtl9602c-eth), so the other messages the CPU sends, and those the switch
 * forwards between ports, leave unchanged and unreported (measured).
 *
 * Context: process context, at registration and teardown or under RTNL
 * (rtl9602c_port_hwtstamp_set()), which keeps the callers apart.
 */
static void rtl9602c_ptp_tx_reports(struct rtl9602c_priv *p, bool on, bool onestep,
				    bool p2p)
{
	u32 clr = 0, set = 0;
	unsigned int slot;

	for (slot = 0; slot < RTL9602C_PTP_MSG_TYPES; slot++) {
		u32 act = RTL9602C_PTP_ACTION_LATCH_REPORT;

		if (!(RTL9602C_PTP_EVENT_SLOTS & BIT(slot)))
			continue;
		/*
		 * A one-step Sync carries its time, and a one-step Pdelay_Resp
		 * its turnaround: neither is reported.
		 */
		if (slot == RTL9602C_PTP_SLOT_SYNC_1STEP && onestep)
			act = RTL9602C_PTP_ACTION_LATCH;
		if (slot == RTL9602C_PTP_SLOT_PDELAY_RESP && p2p)
			act = RTL9602C_PTP_ACTION_CORRECT;
		clr |= RTL9602C_PTP_ACTION_MASK << (2 * slot);
		if (on)
			set |= act << (2 * slot);
	}
	rtl9602c_rmw(p, RTL9602C_PTP_ACTION_EGRESS, clr, set);
	rtl9602c_rmw(p, RTL9602C_PTP_PORT_EN + 4 * RTL9602C_PORT_CPU, 1, on);
}

/* The slots of the two Syncs, and those of the peer delay messages. */
#define RTL9602C_PTP_SYNC_SLOTS		(BIT(RTL9602C_PTP_SLOT_SYNC) |		\
					 BIT(RTL9602C_PTP_SLOT_SYNC_1STEP))
#define RTL9602C_PTP_PDELAY_SLOTS	(BIT(RTL9602C_PTP_SLOT_PDELAY_REQ_2STEP) | \
					 BIT(RTL9602C_PTP_SLOT_PDELAY_RESP) |	\
					 BIT(RTL9602C_PTP_SLOT_PDELAY_RESP_FUP) | \
					 BIT(RTL9602C_PTP_SLOT_PDELAY_REQ))
#define RTL9602C_PTP_ALL_SLOTS		GENMASK(RTL9602C_PTP_MSG_TYPES - 1, 0)

/* An action register's value with @act in each of @slots. */
static u32 rtl9602c_ptp_actions(unsigned long slots, u32 act)
{
	unsigned int slot;
	u32 v = 0;

	for_each_set_bit(slot, &slots, RTL9602C_PTP_MSG_TYPES)
		v |= act << (2 * slot);
	return v;
}

/*
 * The transparent clock (@p->ptp_tc), on the user ports: their PTP enables and
 * transparent-clock bits are set, the event messages that arrive on them are
 * forwarded through the clock, and leave with their residence time added to
 * their correctionField (action 3, which for a forwarded message takes its
 * time of arrival where it takes the reference for one from the CPU).  A
 * peer-to-peer clock corrects only the Syncs, adds the path delay to them,
 * and takes the peer delay messages to the CPU, which answers them per link.
 * While a port stamps what it receives (@rx_any) every message is copied to
 * the CPU as well.  Transmit stamping is off, as the egress actions are the
 * clock's.
 */
static void rtl9602c_ptp_tc_apply(struct rtl9602c_priv *p, bool rx_any)
{
	bool p2p = p->ptp_tc == RTL9602C_PTP_TC_P2P;
	unsigned long fwd = p2p ? RTL9602C_PTP_SYNC_SLOTS : RTL9602C_PTP_EVENT_SLOTS;
	struct dsa_port *dp;
	u32 ports = 0, igr;
	int port;

	dsa_switch_for_each_user_port(dp, &p->ds)
		ports |= BIT(dp->index);

	if (rx_any)
		igr = rtl9602c_ptp_actions(RTL9602C_PTP_ALL_SLOTS &
					   ~(p2p ? RTL9602C_PTP_PDELAY_SLOTS : 0),
					   RTL9602C_PTP_ACTION_FORWARD_TC_MIRROR);
	else
		igr = rtl9602c_ptp_actions(fwd, RTL9602C_PTP_ACTION_FORWARD_TC);
	if (p2p)
		igr |= rtl9602c_ptp_actions(RTL9602C_PTP_PDELAY_SLOTS,
					    RTL9602C_PTP_ACTION_TRAP);

	rtl9602c_w(p, RTL9602C_PTP_ACTION_EGRESS,
		   rtl9602c_ptp_actions(fwd, RTL9602C_PTP_ACTION_CORRECT));
	rtl9602c_w(p, RTL9602C_PTP_ACTION_INGRESS, igr);
	rtl9602c_w(p, RTL9602C_PTP_PATH_DELAY, p2p ? p->ptp_path_delay : 0);
	rtl9602c_w(p, RTL9602C_PTP_TRANSPARENT_PORTS, ports);
	for (port = 0; port < RTL9602C_NUM_PORTS; port++)
		rtl9602c_rmw(p, RTL9602C_PTP_PORT_EN + 4 * port, 1,
			     !!(ports & BIT(port)));
}

/*
 * The state the PTP block starts from: no port enabled, no ingress action, no
 * transparent-clock bit and no path delay added to a correction.  The CPU
 * port's enable and the egress actions follow transmit stamping
 * (rtl9602c_ptp_tx_reports()).
 */
static void rtl9602c_ptp_block_reset(struct rtl9602c_priv *p)
{
	int port;

	for (port = 0; port < RTL9602C_NUM_PORTS; port++)
		if (port != RTL9602C_PORT_CPU)
			rtl9602c_rmw(p, RTL9602C_PTP_PORT_EN + 4 * port, 1, 0);
	rtl9602c_w(p, RTL9602C_PTP_ACTION_INGRESS, 0);
	rtl9602c_w(p, RTL9602C_PTP_PATH_DELAY, 0);
	rtl9602c_w(p, RTL9602C_PTP_TRANSPARENT_PORTS, 0);
}

/*
 * Write the PTP block from the timestamping configuration of the ports and the
 * transparent clock.  Without the clock the switch forwards PTP messages as
 * other frames, whatever the ports stamp: the ingress actions are off, and so
 * are the PTP enables of the ports other than the CPU's, as an action acts on a
 * message only when the PTP enable of the port it arrives by is set
 * (measured), and the departure reports and one-step actions of
 * rtl9602c_ptp_tx_reports() must not change the messages the switch forwards.
 * The CPU's copies of the messages that reach it carry their stamps whatever
 * the actions and enables say (see rtl9602c_port_hwtstamp_set()).
 *
 * Context: under @p->ptp_cfg_lock.
 */
static void rtl9602c_ptp_apply(struct rtl9602c_priv *p)
{
	bool tx_on = false, onestep = false, p2p = false, rx_any = false;
	int i;

	lockdep_assert_held(&p->ptp_cfg_lock);

	for (i = 0; i < RTL9602C_NUM_PORTS; i++) {
		int tx = p->hwtstamp[i].tx_type;

		p2p |= tx == HWTSTAMP_TX_ONESTEP_P2P;
		onestep |= tx == HWTSTAMP_TX_ONESTEP_SYNC ||
			   tx == HWTSTAMP_TX_ONESTEP_P2P;
		tx_on |= tx != HWTSTAMP_TX_OFF;
		rx_any |= p->hwtstamp[i].rx_filter != HWTSTAMP_FILTER_NONE;
	}

	if (p->ptp_tc != RTL9602C_PTP_TC_OFF) {
		rtl9602c_ptp_tc_apply(p, rx_any);
		return;
	}

	rtl9602c_ptp_block_reset(p);
	rtl9602c_ptp_tx_reports(p, tx_on, onestep, p2p);
}

/*
 * A departure report, from the CPU-port driver's receive poll: the time goes
 * to the message awaited, if any, and completes it.
 */
static void rtl9602c_ptp_departure(void *ctx, ktime_t stamp)
{
	struct skb_shared_hwtstamps hw = {};
	struct rtl9602c_priv *p = ctx;
	struct sk_buff *skb;

	spin_lock_bh(&p->ptp_tx_lock);
	skb = p->ptp_tx_skb;
	p->ptp_tx_skb = NULL;
	if (skb)
		p->ptp_tx_stamped++;
	else
		p->ptp_tx_orphans++;
	spin_unlock_bh(&p->ptp_tx_lock);

	if (!skb)
		return;

	hw.hwtstamp = rtl9602c_ptp_complete(p, stamp);
	skb_complete_tx_timestamp(skb, &hw);
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
 * Transmit stamping needs the clock and the departure reports, which reach
 * the switch driver through the CPU port's conduit; without either the ports
 * offer receive stamping only.  The reports start off, as the board comes
 * up, until a port asks for transmit stamps.
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

/* How long after the time-of-day superframe the worker reads the clock. */
#define RTL9602C_PTP_TOD_SETTLE		(HZ / 10)

/**
 * rtl9602c_ptp_pon_tod() - give the clock a time to take at a PON frame
 * @p: driver state
 * @ts: the time
 * @delay: jiffies until the frame, about
 *
 * Writes @ts into the PON time and sets its enable; the clock takes the time
 * when the GPON MAC's time-of-day superframe arrives, which the MAC arms
 * after this, and the hardware then clears the enable.  The time replaces
 * one given before and not yet taken.  The worker reads the clock shortly
 * after the frame, so that receive stamps complete their seconds from the
 * new time (rtl9602c_ptp_complete()).
 *
 * Context: process context.
 * Return: 0, or -EOPNOTSUPP when no clock is registered.
 */
int rtl9602c_ptp_pon_tod(struct rtl9602c_priv *p, const struct timespec64 *ts,
			 unsigned long delay)
{
	unsigned long flags;

	if (!p->ptp_clock)
		return -EOPNOTSUPP;

	spin_lock_irqsave(&p->ptp_lock, flags);
	rtl9602c_ptp_wr_sec(p, RTL9602C_PTP_PON_TIME_SEC, ts->tv_sec);
	rtl9602c_w(p, RTL9602C_PTP_PON_TIME_NSEC,
		   FIELD_PREP(RTL9602C_PTP_NSEC_TICKS,
			      ts->tv_nsec / RTL9602C_PTP_NSEC_TICK_NS));
	rtl9602c_rmw(p, RTL9602C_PTP_CLK_CTRL, 0,
		     RTL9602C_PTP_CLK_CTRL_PON_TIME_EN);
	spin_unlock_irqrestore(&p->ptp_lock, flags);

	ptp_schedule_worker(p->ptp_clock, delay + RTL9602C_PTP_TOD_SETTLE);
	return 0;
}

int rtl9602c_ptp_register(struct rtl9602c_priv *p)
{
	struct dsa_port *cpu = dsa_to_port(&p->ds, RTL9602C_PORT_CPU);
	int err;

	p->ptp_info = (struct ptp_clock_info) {
		.owner		= THIS_MODULE,
		.name		= "rtl9602c-ptp",
		/*
		 * The largest offset whose scaled_ppm fits the long that
		 * adjfine takes on this 32-bit architecture: about 3.3 %,
		 * well inside the word's range.
		 */
		.max_adj	= (S32_MAX >> 16) * 1000,
		.n_ext_ts	= 1,
		.supported_extts_flags = PTP_RISING_EDGE | PTP_STRICT_FLAGS,
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
		err = PTR_ERR(p->ptp_clock);
		p->ptp_clock = NULL;
		return err;
	}
	ptp_schedule_worker(p->ptp_clock, 0);

	rtl9602c_ptp_block_reset(p);
	rtl9602c_ptp_tx_reports(p, false, false, false);
	mutex_lock(&p->ptp_cfg_lock);
	if (p->ptp_tc != RTL9602C_PTP_TC_OFF)
		rtl9602c_ptp_apply(p);
	mutex_unlock(&p->ptp_cfg_lock);
	err = rtl9602c_conduit_tstamp_attach(cpu ? cpu->conduit : NULL,
					     rtl9602c_ptp_departure, p);
	if (err)
		pr_warn("no transmit timestamps: %pe\n", ERR_PTR(err));
	p->ptp_tx_ready = !err;
	return 0;
}

/**
 * rtl9602c_ptp_unregister() - remove the PTP hardware clock, if registered
 * @p: driver state
 *
 * Detaches the handler of departure reports first, turns the reports off and
 * drops the message still awaiting one, if any.
 *
 * Context: process context; sleeps.
 */
void rtl9602c_ptp_unregister(struct rtl9602c_priv *p)
{
	if (p->ptp_tx_ready) {
		struct dsa_port *cpu = dsa_to_port(&p->ds, RTL9602C_PORT_CPU);
		struct sk_buff *skb;

		rtl9602c_conduit_tstamp_detach(cpu ? cpu->conduit : NULL);
		p->ptp_tx_ready = false;
		rtl9602c_ptp_tx_reports(p, false, false, false);
		rtl9602c_ptp_block_reset(p);

		spin_lock_bh(&p->ptp_tx_lock);
		skb = p->ptp_tx_skb;
		p->ptp_tx_skb = NULL;
		spin_unlock_bh(&p->ptp_tx_lock);
		kfree_skb(skb);
	}

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
 * timestamp.  The tag carries only the low 16 bits of the seconds, and
 * rtl9602c_ptp_complete() adds the rest.  On a port that answers Pdelay_Req
 * one-step (%HWTSTAMP_TX_ONESTEP_P2P) the receive time of a Pdelay_Req is
 * kept for its Pdelay_Resp.  The switch tags its PTP messages
 * whatever the filter, so on a port whose receive filter is
 * %HWTSTAMP_FILTER_NONE the stamp is taken off instead.
 *
 * Return: false; the frame is delivered at once.
 */
bool rtl9602c_port_rxtstamp(struct dsa_switch *ds, int port,
			    struct sk_buff *skb, unsigned int type)
{
	struct skb_shared_hwtstamps *hw = skb_hwtstamps(skb);
	struct rtl9602c_priv *p = ds->priv;

	/* No tag, no stamp to complete. */
	if (!hw->hwtstamp)
		return false;

	if (p->hwtstamp[port].rx_filter == HWTSTAMP_FILTER_NONE) {
		hw->hwtstamp = 0;
		return false;
	}

	hw->hwtstamp = rtl9602c_ptp_complete(p, hw->hwtstamp);

	/* A port that answers one-step keeps the receive time of a Pdelay_Req. */
	if (p->hwtstamp[port].tx_type == HWTSTAMP_TX_ONESTEP_P2P) {
		struct ptp_header *hdr = ptp_parse_header(skb, type);

		if (hdr && ptp_get_msgtype(hdr, type) == PTP_MSGTYPE_PDELAY_REQ) {
			spin_lock_bh(&p->ptp_tx_lock);
			p->ptp_pdelay[port].valid = true;
			p->ptp_pdelay[port].seq = be16_to_cpu(hdr->sequence_id);
			memcpy(p->ptp_pdelay[port].id, &hdr->source_port_identity,
			       sizeof(p->ptp_pdelay[port].id));
			p->ptp_pdelay[port].t2 = hw->hwtstamp;
			spin_unlock_bh(&p->ptp_tx_lock);
		}
	}
	return false;
}

/*
 * The reference time is one register for every port: a one-step Pdelay_Resp
 * must have left the switch before the next one's is written.  The next waits
 * out what is left of this time after the last, with the interrupts off; a
 * message took 51 to 84 us from the write to leaving (measured: the
 * correction it carried).
 */
#define RTL9602C_PTP_ONESTEP_GAP_US	250

/*
 * Write the reference time @ts: the switch's egress action adds the time from
 * it to a message's departure to the message's correctionField, modulo 8 s.
 * The reference has 8 ns ticks; @ts is cut to them.  A write waits out what
 * is left of RTL9602C_PTP_ONESTEP_GAP_US after the last.
 */
static void rtl9602c_ptp_reference(struct rtl9602c_priv *p,
				   const struct timespec64 *ts)
{
	unsigned long flags;
	s64 since;

	spin_lock_irqsave(&p->ptp_lock, flags);
	since = ktime_us_delta(ktime_get(), p->ptp_onestep_at);
	if (since >= 0 && since < RTL9602C_PTP_ONESTEP_GAP_US)
		udelay(RTL9602C_PTP_ONESTEP_GAP_US - since);

	rtl9602c_w(p, RTL9602C_PTP_RX_TSTAMP,
		   FIELD_PREP(RTL9602C_PTP_RX_TSTAMP_SEC, ts->tv_sec & 7));
	rtl9602c_w(p, RTL9602C_PTP_RX_TSTAMP + 4,
		   FIELD_PREP(RTL9602C_PTP_RX_TSTAMP_NSEC,
			      ts->tv_nsec / RTL9602C_PTP_NSEC_TICK_NS));
	p->ptp_onestep_at = ktime_get();
	spin_unlock_irqrestore(&p->ptp_lock, flags);
}

/*
 * A one-step Sync: the switch's egress action writes the time the Sync leaves
 * into its originTimestamp (measured), so the driver only marks it for the
 * conduit, which sends it so that the switch applies the action.
 */
static void rtl9602c_ptp_onestep(struct rtl9602c_priv *p, struct sk_buff *skb)
{
	spin_lock_bh(&p->ptp_tx_lock);
	p->ptp_tx_onestep++;
	spin_unlock_bh(&p->ptp_tx_lock);
	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
}

/*
 * A one-step Pdelay_Resp (IEEE 1588-2008 11.4.3, one-step): its turnaround,
 * from the arrival of the Pdelay_Req it answers to its own departure, goes to
 * its correctionField.  The reference time is that Pdelay_Req's receive time,
 * kept by rtl9602c_port_rxtstamp(); the answer is the Pdelay_Req's by its
 * sequence ID and requestingPortIdentity.  Without it the Pdelay_Resp leaves
 * as it is.
 */
static void rtl9602c_ptp_onestep_p2p(struct rtl9602c_priv *p, int port,
				     struct sk_buff *skb, struct ptp_header *hdr)
{
	/* requestingPortIdentity, after requestReceiptTimestamp */
	const u8 *requester = (const u8 *)(hdr + 1) + 10;
	size_t idlen = sizeof(p->ptp_pdelay[port].id);
	struct timespec64 t2;
	bool match;

	if (requester + idlen > skb_tail_pointer(skb))
		return;

	spin_lock_bh(&p->ptp_tx_lock);
	match = p->ptp_pdelay[port].valid &&
		p->ptp_pdelay[port].seq == be16_to_cpu(hdr->sequence_id) &&
		!memcmp(p->ptp_pdelay[port].id, requester, idlen);
	if (match) {
		t2 = ktime_to_timespec64(p->ptp_pdelay[port].t2);
		p->ptp_pdelay[port].valid = false;
		p->ptp_tx_p2p++;
	} else {
		p->ptp_tx_p2p_missed++;
	}
	spin_unlock_bh(&p->ptp_tx_lock);
	if (!match)
		return;

	rtl9602c_ptp_reference(p, &t2);
	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
}

/**
 * rtl9602c_port_txtstamp() - ask for the departure time of a PTP message
 * @ds: the switch
 * @port: the port the message leaves by
 * @skb: the message
 *
 * Called by DSA for a frame whose socket asked for a hardware transmit
 * stamp.  On a port with %HWTSTAMP_TX_ONESTEP_SYNC or
 * %HWTSTAMP_TX_ONESTEP_P2P, a Sync without the twoStepFlag gets its time from
 * the switch as it leaves (rtl9602c_ptp_onestep()); on one with
 * %HWTSTAMP_TX_ONESTEP_P2P a Pdelay_Resp gets its turnaround
 * (rtl9602c_ptp_onestep_p2p()).  On any port with transmit stamping, any
 * other event message over Ethernet
 * (Sync, Delay_Req, Pdelay_Req or Pdelay_Resp, IEEE 1588-2008 6.4; with or
 * without a VLAN tag) is kept as a clone and marked %SKBTX_IN_PROGRESS,
 * which makes the CPU-port driver send it under the switch's egress action;
 * the departure report then completes the clone, in
 * rtl9602c_ptp_departure().  The report does not say which message left,
 * so one message at a time awaits it: a message sent while another waits
 * goes unstamped (@p->ptp_tx_busy), unless the other has waited longer than
 * %RTL9602C_PTP_TX_TIMEOUT and is given up (@p->ptp_tx_lost).  A message
 * over UDP is not stamped: the switch does not recognise one (measured on
 * the receive side).
 *
 * Context: the transmit path, with bottom halves disabled.
 */
void rtl9602c_port_txtstamp(struct dsa_switch *ds, int port,
			    struct sk_buff *skb)
{
	struct rtl9602c_priv *p = ds->priv;
	enum hwtstamp_tx_types tx = p->hwtstamp[port].tx_type;
	struct sk_buff *clone, *stale = NULL;
	struct ptp_header *hdr;
	unsigned int type;
	u8 msgtype;

	if (tx == HWTSTAMP_TX_OFF || !p->ptp_tx_ready)
		return;

	type = ptp_classify_raw(skb);
	if ((type & PTP_CLASS_PMASK) != PTP_CLASS_L2 ||
	    (type & PTP_CLASS_VMASK) != PTP_CLASS_V2)
		return;

	hdr = ptp_parse_header(skb, type);
	if (!hdr)
		return;

	msgtype = ptp_get_msgtype(hdr, type);
	if (msgtype == PTP_MSGTYPE_SYNC && tx != HWTSTAMP_TX_ON &&
	    !(hdr->flag_field[0] & PTP_FLAG_TWOSTEP)) {
		rtl9602c_ptp_onestep(p, skb);
		return;
	}
	if (msgtype == PTP_MSGTYPE_PDELAY_RESP && tx == HWTSTAMP_TX_ONESTEP_P2P) {
		rtl9602c_ptp_onestep_p2p(p, port, skb, hdr);
		return;
	}

	switch (msgtype) {
	case PTP_MSGTYPE_SYNC:
	case PTP_MSGTYPE_DELAY_REQ:
	case PTP_MSGTYPE_PDELAY_REQ:
	case PTP_MSGTYPE_PDELAY_RESP:
		break;
	default:
		return;
	}

	clone = skb_clone_sk(skb);
	if (!clone)
		return;

	spin_lock_bh(&p->ptp_tx_lock);
	if (p->ptp_tx_skb &&
	    time_before(jiffies, p->ptp_tx_start + RTL9602C_PTP_TX_TIMEOUT)) {
		p->ptp_tx_busy++;
		spin_unlock_bh(&p->ptp_tx_lock);
		kfree_skb(clone);
		return;
	}
	if (p->ptp_tx_skb) {
		stale = p->ptp_tx_skb;
		p->ptp_tx_lost++;
	}
	p->ptp_tx_skb = clone;
	p->ptp_tx_start = jiffies;
	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	spin_unlock_bh(&p->ptp_tx_lock);

	kfree_skb(stale);
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
 * @extack: netlink extended ack, told why a transmit type or a receive
 *	filter is refused
 *
 * The switch tags every PTP message it sends to the CPU port whatever the
 * per-port PTP enable and the message actions say, so the receive filter
 * needs no register: rtl9602c_port_rxtstamp() takes the stamps off under
 * %HWTSTAMP_FILTER_NONE.  Transmit stamps are two-step (%HWTSTAMP_TX_ON),
 * or one-step for Sync and two-step for the other event messages
 * (%HWTSTAMP_TX_ONESTEP_SYNC), and the same with one-step Pdelay_Resp
 * (%HWTSTAMP_TX_ONESTEP_P2P): while any port asks for them, the switch
 * reports the departures of event messages, and while any port asks for
 * one-step messages it writes their times into them
 * (rtl9602c_ptp_tx_reports()).
 *
 * Context: under RTNL.
 *
 * Return: 0, or -ERANGE for a transmit type or a receive filter the port
 * cannot provide, or for %HWTSTAMP_TX_ONESTEP_P2P beside another port's
 * two-step Pdelay_Resp, which leaves the configuration as it was.
 */
int rtl9602c_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config,
			       struct netlink_ext_ack *extack)
{
	struct rtl9602c_priv *p = ds->priv;
	bool p2p = false, resp_2step = false;
	int i;

	switch (config->tx_type) {
	case HWTSTAMP_TX_OFF:
		break;
	case HWTSTAMP_TX_ON:
	case HWTSTAMP_TX_ONESTEP_SYNC:
	case HWTSTAMP_TX_ONESTEP_P2P:
		if (p->ptp_tx_ready)
			break;
		NL_SET_ERR_MSG_MOD(extack,
				   "no transmit timestamps: the switch's departure reports do not reach the driver");
		return -ERANGE;
	default:
		NL_SET_ERR_MSG_MOD(extack,
				   "transmit timestamps are two-step, or one-step for Sync and Pdelay_Resp");
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

	for (i = 0; i < RTL9602C_NUM_PORTS; i++) {
		int tx = i == port ? config->tx_type : p->hwtstamp[i].tx_type;

		p2p |= tx == HWTSTAMP_TX_ONESTEP_P2P;
		resp_2step |= tx == HWTSTAMP_TX_ON || tx == HWTSTAMP_TX_ONESTEP_SYNC;
	}

	/*
	 * Pdelay_Resp, with the twoStepFlag or without it, has one action
	 * for the whole switch: it is one-step on every port that stamps or
	 * on none.
	 */
	if (p2p && resp_2step) {
		NL_SET_ERR_MSG_MOD(extack,
				   "one-step Pdelay_Resp holds for the whole switch: every port that stamps must ask for it");
		return -ERANGE;
	}

	mutex_lock(&p->ptp_cfg_lock);
	if (p->ptp_tc != RTL9602C_PTP_TC_OFF &&
	    config->tx_type != HWTSTAMP_TX_OFF) {
		mutex_unlock(&p->ptp_cfg_lock);
		NL_SET_ERR_MSG_MOD(extack,
				   "the transparent clock holds the egress actions: no transmit stamps while it is on");
		return -EBUSY;
	}
	p->hwtstamp[port] = *config;
	rtl9602c_ptp_apply(p);
	mutex_unlock(&p->ptp_cfg_lock);
	return 0;
}

static const char * const rtl9602c_ptp_tc_names[] = {
	[RTL9602C_PTP_TC_OFF]	= "off",
	[RTL9602C_PTP_TC_E2E]	= "e2e",
	[RTL9602C_PTP_TC_P2P]	= "p2p",
};

/**
 * rtl9602c_ptp_tc_set() - make the switch a transparent clock, or stop it
 * @p: driver state
 * @mode: "off", "e2e" or "p2p" (enum rtl9602c_ptp_tc)
 *
 * The devlink parameter ptp_transparent_clock.  The clock holds the egress
 * actions, so it is refused while a port has transmit stamping on, and
 * transmit stamping is refused while it is on (rtl9602c_port_hwtstamp_set()).
 *
 * Context: process context.
 * Return: 0, -EINVAL for an unknown mode, or -EBUSY while a port stamps what
 * it sends.
 */
int rtl9602c_ptp_tc_set(struct rtl9602c_priv *p, const char *mode)
{
	int tc, i, err = 0;

	tc = match_string(rtl9602c_ptp_tc_names,
			  ARRAY_SIZE(rtl9602c_ptp_tc_names), mode);
	if (tc < 0)
		return -EINVAL;

	mutex_lock(&p->ptp_cfg_lock);
	for (i = 0; i < RTL9602C_NUM_PORTS && tc != RTL9602C_PTP_TC_OFF; i++)
		if (p->hwtstamp[i].tx_type != HWTSTAMP_TX_OFF)
			err = -EBUSY;
	if (!err) {
		p->ptp_tc = tc;
		rtl9602c_ptp_apply(p);
	}
	mutex_unlock(&p->ptp_cfg_lock);
	if (err)
		dev_warn(p->ds.dev,
			 "no transparent clock while a port stamps what it sends\n");
	return err;
}

/* The transparent clock's mode as rtl9602c_ptp_tc_set() takes it. */
void rtl9602c_ptp_tc_get(struct rtl9602c_priv *p, char *buf, size_t len)
{
	mutex_lock(&p->ptp_cfg_lock);
	strscpy(buf, rtl9602c_ptp_tc_names[p->ptp_tc], len);
	mutex_unlock(&p->ptp_cfg_lock);
}

/**
 * rtl9602c_ptp_path_delay_set() - set the path delay of a P2P transparent clock
 * @p: driver state
 * @ns: the delay, in nanoseconds
 *
 * The devlink parameter ptp_path_delay_ns: what a peer-to-peer transparent
 * clock adds to a Sync's correctionField besides its residence time, the
 * mean path delay of the link the Syncs arrive by (IEEE 1588-2008 11.5.4).
 * The switch holds one value for every port.
 *
 * Context: process context.
 * Return: 0, or -ERANGE for a delay over 31 bits.
 */
int rtl9602c_ptp_path_delay_set(struct rtl9602c_priv *p, u32 ns)
{
	if (ns > FIELD_MAX(RTL9602C_PTP_PATH_DELAY_NS))
		return -ERANGE;

	mutex_lock(&p->ptp_cfg_lock);
	p->ptp_path_delay = ns;
	rtl9602c_ptp_apply(p);
	mutex_unlock(&p->ptp_cfg_lock);
	return 0;
}

/* The path delay rtl9602c_ptp_path_delay_set() last took. */
u32 rtl9602c_ptp_path_delay_get(struct rtl9602c_priv *p)
{
	u32 ns;

	mutex_lock(&p->ptp_cfg_lock);
	ns = p->ptp_path_delay;
	mutex_unlock(&p->ptp_cfg_lock);
	return ns;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * PTP message types in the order of the 2-bit fields of the action
 * registers, type n at bits 2n+1:2n, as measured for every message (see the
 * RTL9602C_PTP_SLOT_* in rtl9602c.h): "sync" is a Sync with the twoStepFlag,
 * "pdelay_req" a Pdelay_Req with it, "_1step" the two without it.
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
 * of once-per-second pulses and whether their interrupt is enabled, the
 * OLT's seconds reported and those come late (rtl9602c_ptp_pon_second()), the
 * transmit stamps, the PON time of day and the receive capture.  Apart from
 * that interrupt enable, and the egress actions and the CPU port's enable
 * while transmit stamping is on, the driver writes none of these registers.
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
	seq_printf(s, "olt seconds %u  late %u  wanted %u  last at frame %u\n",
		   READ_ONCE(p->ptp_pon_seconds), READ_ONCE(p->ptp_pon_late),
		   READ_ONCE(p->ptp_pon_extts), READ_ONCE(p->ptp_pon_frame));

	spin_lock_bh(&p->ptp_tx_lock);
	seq_printf(s, "tx stamps %u  busy %u  lost %u  orphan reports %u  awaited %u\n",
		   p->ptp_tx_stamped, p->ptp_tx_busy, p->ptp_tx_lost,
		   p->ptp_tx_orphans, !!p->ptp_tx_skb);
	spin_unlock_bh(&p->ptp_tx_lock);
	seq_printf(s, "transparent clock %s  path delay %lu ns\n",
		   rtl9602c_ptp_tc_names[READ_ONCE(p->ptp_tc)],
		   FIELD_GET(RTL9602C_PTP_PATH_DELAY_NS,
			     rtl9602c_r(p, RTL9602C_PTP_PATH_DELAY)));
	seq_printf(s, "one-step syncs %u  pdelay_resp %u  without their pdelay_req %u\n",
		   READ_ONCE(p->ptp_tx_onestep), READ_ONCE(p->ptp_tx_p2p),
		   READ_ONCE(p->ptp_tx_p2p_missed));

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
	 * The reference time a one-step Sync's correction is taken from: the
	 * last one-step Sync's origin, zero before any.
	 */
	hi = rtl9602c_r(p, RTL9602C_PTP_RX_TSTAMP);
	lo = rtl9602c_r(p, RTL9602C_PTP_RX_TSTAMP + 4);
	seq_printf(s, "\nreference %u.%09lu  (raw %08x %08x)\n",
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
