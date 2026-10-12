// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: the PON MAC, the block between the GPON MAC and
 * switch port 2, the PON port.
 *
 * The block holds a packet buffer for each direction, which keeps frames in
 * on-chip SRAM and spills them into a reserved DRAM region, and, upstream,
 * the streams, queues and T-CONT scheduler. A frame the switch sends to the
 * PON port carries a stream, for a GEM port its GPON MAC flow; the stream
 * names a queue, and a T-CONT sends the queues in its map in the allocations
 * the OLT grants its Alloc-ID. T-CONT 16, its queue 64 and stream 64 carry
 * the OMCC, the ONU's OMCI channel. Building the block also sets up the
 * switch for the OMCI path.
 *
 * Every register of the block sits behind a gate in the system controller,
 * and a read while the gate is closed stops the board. The gate is opened as
 * the first step of the build, at a bring-up of the PON, and the block is
 * accessed only from then on. A built block is never torn down.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/minmax.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>

#include "rtl9602c-pon.h"

/*
 * The chip identification word; RTL9602C_CHIP_IDENT_REV is the silicon
 * revision. RTL9602C_CHIP_IDENT_READ_KEY is written before the word is read,
 * and 0 after; that the read needs the key is not verified.
 */
#define RTL9602C_CHIP_IDENT		0x010004
#define RTL9602C_CHIP_IDENT_READ_KEY		0xa0000000
#define RTL9602C_CHIP_IDENT_REV		GENMASK(19, 16)

static unsigned int rtl9602c_chip_rev(struct rtl9602c_pon *p)
{
	u32 v;

	rtl9602c_pon_w(p, RTL9602C_CHIP_IDENT, RTL9602C_CHIP_IDENT_READ_KEY);
	v = rtl9602c_pon_r(p, RTL9602C_CHIP_IDENT);
	rtl9602c_pon_w(p, RTL9602C_CHIP_IDENT, 0);
	return FIELD_GET(RTL9602C_CHIP_IDENT_REV, v);
}

/**
 * rtl9602c_ponmac_probe() - find what the PON MAC needs outside the switch
 * @p: the switch
 *
 * The PON MAC needs the system controller, which holds its gate, as a syscon
 * found through the switch node's realtek,sysctl phandle; and two
 * reserved-memory regions for its packet buffer, named pbo-us and pbo-ds in
 * the node's memory-region-names, each at least RTL9602C_PBO_DRAM bytes and
 * no-map, since the buffer uses them as DRAM of its own from their physical
 * start. Without any of them the driver logs what is missing and leaves
 * p->sysctl NULL, which the rest of the driver takes as "no PON MAC": no
 * build, no OMCI port and no GEM interfaces. Nothing in the block is accessed
 * here.
 *
 * Return: 0, also when the PON MAC is not available, or -EPROBE_DEFER while
 * the system controller is not ready.
 */
int rtl9602c_ponmac_probe(struct rtl9602c_pon *p)
{
	static const char * const names[] = {
		[RTL9602C_PBO_US] = "pbo-us",
		[RTL9602C_PBO_DS] = "pbo-ds",
	};
	struct device_node *np = p->dev->of_node;
	struct regmap *sysctl;
	unsigned int d;

	sysctl = syscon_regmap_lookup_by_phandle(np, "realtek,sysctl");
	if (IS_ERR(sysctl)) {
		if (PTR_ERR(sysctl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		pr_info("no system controller: PON MAC not available\n");
		return 0;
	}

	for (d = 0; d < ARRAY_SIZE(names); d++) {
		struct resource res;
		int err;

		err = of_reserved_mem_region_to_resource_byname(np, names[d],
								&res);
		if (err || resource_size(&res) < RTL9602C_PBO_DRAM) {
			pr_info("no %s buffer of %u bytes: PON MAC not available\n",
				names[d], RTL9602C_PBO_DRAM);
			return 0;
		}
		p->pbo_base[d] = res.start;
	}

	p->sysctl = sysctl;
	return 0;
}

/**
 * rtl9602c_ponq_stream_queue_set() - point an upstream stream at a queue
 * @p: the switch
 * @sid: the stream; a GEM port's stream is its GPON MAC flow
 * @queue: the queue, as RTL9602C_PON_QUEUE() numbers them
 *
 * The stream's frames go to @queue while the stream is admitted
 * (rtl9602c_ponmac_sid_valid()). Called only with the block's gate open: an
 * access with the gate closed stops the board.
 *
 * The block has streams 0 to RTL9602C_PON_NUM_SID - 1. The map holds a
 * 7-bit queue number per stream, four streams to a word, so the word of a
 * stream from 68 up would lie past the map, among the admission words and
 * the registers after them: a stream the block does not have is refused.
 */
void rtl9602c_ponq_stream_queue_set(struct rtl9602c_pon *p, unsigned int sid,
				    unsigned int queue)
{
	unsigned int sh;

	if (WARN_ON_ONCE(sid >= RTL9602C_PON_NUM_SID))
		return;

	sh = RTL9602C_PONQ_US_STREAM_QUEUE_SHIFT(sid);
	rtl9602c_pon_rmw(p, RTL9602C_PONQ_US_STREAM_QUEUE(sid),
			 RTL9602C_PONQ_US_STREAM_QUEUE_MASK << sh,
		     (queue & RTL9602C_PONQ_US_STREAM_QUEUE_MASK) << sh);
}

/* Admit stream @sid's frames to its queue, or stop admitting them. */
static void rtl9602c_ponq_stream_enable(struct rtl9602c_pon *p, unsigned int sid,
					bool valid)
{
	rtl9602c_pon_rmw(p, RTL9602C_PONQ_US_STREAM_EN(sid), BIT(sid % 32),
			 valid ? BIT(sid % 32) : 0);
}

/**
 * rtl9602c_ponmac_sid_valid() - admit an upstream stream, or stop admitting it
 * @p: the switch
 * @sid: the stream
 * @valid: whether the stream's frames are admitted to its queue
 *
 * Ignored until the PON MAC is built, since until then its gate may be
 * closed, and for a stream the block does not have: RTL9602C_PON_NUM_SID
 * and up.
 */
void rtl9602c_ponmac_sid_valid(struct rtl9602c_pon *p, unsigned int sid,
			       bool valid)
{
	if (p->ponmac_up && sid < RTL9602C_PON_NUM_SID)
		rtl9602c_ponq_stream_enable(p, sid, valid);
}

/**
 * rtl9602c_pon_queue_set() - set an upstream queue's scheduling
 * @p: the switch
 * @q: the queue, as RTL9602C_PON_QUEUE() numbers them
 * @weight: 0 for strict priority; otherwise the queue's weight, up to
 *	    RTL9602C_PONSCH_QUEUE_WEIGHT_MASK
 * @cir: the committed rate, in RTL9602C_PONSCH_QUEUE_RATE units
 * @pir: the peak rate, in the same units; RTL9602C_PONSCH_QUEUE_RATE_MAX
 *	 leaves the queue unshaped, sending as much as its T-CONT is granted
 *
 * What the settings do, all measured on this board with two loaded queues
 * of one T-CONT: a T-CONT serves the committed rates of its queues first,
 * then its strict queues, the higher queue number first, then its weighted
 * queues in proportion to their weights; a queue sends nothing above its
 * peak rate while RTL9602C_PONSCH_CFG_PIR_DROP is set. A rate unit is 1024
 * bytes per second of frames counted with their FCS. A strict queue's
 * weight word is written 1.
 */
void rtl9602c_pon_queue_set(struct rtl9602c_pon *p, unsigned int q,
			    unsigned int weight, u32 cir, u32 pir)
{
	unsigned int sh = RTL9602C_PONSCH_QUEUE_WEIGHT_SHIFT(q);

	rtl9602c_pon_rmw(p, RTL9602C_PONSCH_QUEUE_WFQ(q), BIT(q % 32),
			 weight ? BIT(q % 32) : 0);
	rtl9602c_pon_w(p, RTL9602C_PONSCH_QUEUE_CIR(q),
		       FIELD_PREP(RTL9602C_PONSCH_QUEUE_RATE, cir));
	rtl9602c_pon_w(p, RTL9602C_PONSCH_QUEUE_PIR(q),
		       FIELD_PREP(RTL9602C_PONSCH_QUEUE_RATE, pir));
	rtl9602c_pon_rmw(p, RTL9602C_PONSCH_QUEUE_WEIGHT(q),
			 RTL9602C_PONSCH_QUEUE_WEIGHT_MASK << sh,
		     (weight ? weight : 1) << sh);
}

/*
 * A queue drain is polled back to back for up to RTL9602C_PON_DRN_US, enough
 * for every drain that completes on this board (measured), then every 100 us
 * up to RTL9602C_PON_DRN_SLOW_US, to tell a slow queue from one that does not
 * empty. Both budgets are the driver's choice. A sleep between polls lasts
 * until the next 4 ms tick on this board (HZ 250, and the tick timer has no
 * one-shot mode), so the slow polls come a tick apart, and a duration the
 * driver reports, timed in steps of 10 ns, ends at the poll that saw the end.
 */
#define RTL9602C_PON_DRN_US		10000
#define RTL9602C_PON_DRN_SLOW_US	500000

/*
 * How long each half of the packet buffer is given to finish initialising,
 * the driver's choice. On this board both halves are ready within a few
 * ticks, nearly always one (measured).
 */
#define RTL9602C_PON_IPRDY_US		50000

/*
 * Empty upstream queue @q. The hardware takes the command, clearing
 * RTL9602C_PONQ_DRAIN_CMD_START, and keeps RTL9602C_PONQ_DRAIN_CMD_BUSY set
 * until the drain ends. The upstream pool in use is read first and reported
 * with a slow or failed drain: an empty pool beside a queue that does not
 * empty is a different fault from a full one.
 *
 * The drain does not complete while the GPON MAC is still in a session that
 * survived a warm reset (measured on this board), which is why bring-up
 * takes the ONU to O1 before it builds the PON MAC. What exactly the drain
 * waits for is not verified, nor what it does to frames that other streams
 * go on feeding into the queue.
 */
static int rtl9602c_pon_drain(struct rtl9602c_pon *p, unsigned int q)
{
	u32 use = rtl9602c_pon_r(p, RTL9602C_PKTBUF_POOL_USED(RTL9602C_PBO_US));
	unsigned long sram = FIELD_GET(RTL9602C_PKTBUF_POOL_USED_SRAM, use);
	unsigned long dram = FIELD_GET(RTL9602C_PKTBUF_POOL_USED_DRAM, use);
	ktime_t t0;
	u32 v;
	int err;

	rtl9602c_pon_w(p, RTL9602C_PONQ_DRAIN_CMD,
		       FIELD_PREP(RTL9602C_PONQ_DRAIN_CMD_IDX, q) |
		   RTL9602C_PONQ_DRAIN_CMD_BY_QUEUE | RTL9602C_PONQ_DRAIN_CMD_START);

	t0 = ktime_get();
	err = read_poll_timeout(rtl9602c_pon_r, v, !(v & RTL9602C_PONQ_DRAIN_CMD_BUSY),
				0, RTL9602C_PON_DRN_US, false, p,
				RTL9602C_PONQ_DRAIN_CMD);
	if (!err)
		return 0;

	err = read_poll_timeout(rtl9602c_pon_r, v, !(v & RTL9602C_PONQ_DRAIN_CMD_BUSY),
				100, RTL9602C_PON_DRN_SLOW_US, false, p,
				RTL9602C_PONQ_DRAIN_CMD);
	if (err)
		pr_err("PON queue %u did not drain in %u ms: %08x, with %lu SRAM and %lu DRAM descriptors in use and the block %s ready\n",
		       q, RTL9602C_PON_DRN_SLOW_US / 1000, v, sram, dram,
		       rtl9602c_pon_r(p, RTL9602C_PKTBUF_STAT(RTL9602C_PBO_US)) &
		       RTL9602C_PKTBUF_STAT_INIT_DONE ? "was" : "was NOT");
	else
		pr_warn("PON queue %u drained in %lld us, past the %u us this port allows, with %lu SRAM and %lu DRAM descriptors in use when it started\n",
			q, ktime_us_delta(ktime_get(), t0), RTL9602C_PON_DRN_US,
			sram, dram);
	return err;
}

/**
 * rtl9602c_pon_queue_add() - attach an upstream queue to a T-CONT
 * @p: the switch
 * @tcont: the T-CONT, 0 to 16
 * @queue: the queue's bit in the T-CONT's map
 *
 * The queue is RTL9602C_PON_QUEUE(@tcont, @queue): each group of eight
 * T-CONTs draws on 32 queues, and bit @queue of a T-CONT's map is queue
 * @queue of its group. That numbering is measured on this board for T-CONTs
 * 0 and 16; that the other T-CONTs follow it is not verified.
 *
 * The first queue attached switches the T-CONT on. A queue that no T-CONT of
 * the group has in its map yet is drained, so that it is attached empty, and
 * made strict and unshaped (rtl9602c_pon_queue_set()); one already in a map
 * carries other GEM ports' frames, so it is neither drained, which would
 * discard them, nor set again. The queue is then added to the map. No
 * stream is admitted here:
 * the streams pointed at the queue may be parked ones, since queue 63 is a GEM
 * T-CONT's queue too (rtl9602c_gem_tcont_qbit()), and each stream that sends
 * is admitted by its owner. Queues are never detached, nor T-CONTs switched
 * off again. Called only with the block's gate open.
 *
 * Return: 0, or -ETIMEDOUT when the drain does not complete; the T-CONT is
 * then switched on and its map left as it was.
 */
int rtl9602c_pon_queue_add(struct rtl9602c_pon *p, unsigned int tcont,
			   unsigned int queue)
{
	unsigned int pq = RTL9602C_PON_QUEUE(tcont, queue);
	unsigned int first = tcont / 8 * 8, t;
	bool in_use = false;
	u32 qmap;
	int err;

	qmap = rtl9602c_pon_r(p, RTL9602C_PONSCH_TCONT_QUEUES(tcont));
	if (!qmap)
		rtl9602c_pon_rmw(p, RTL9602C_PONSCH_TCONT_EN, 0, BIT(tcont));

	/* The T-CONTs of a group, eight of them, share its 32 queues. */
	for (t = first; t < first + 8 && t < RTL9602C_PON_NUM_TCONT; t++)
		if (rtl9602c_pon_r(p, RTL9602C_PONSCH_TCONT_QUEUES(t)) & BIT(queue))
			in_use = true;

	if (!in_use) {
		err = rtl9602c_pon_drain(p, pq);
		if (err)
			return err;
		rtl9602c_pon_queue_set(p, pq, 0, 0,
				       RTL9602C_PONSCH_QUEUE_RATE_MAX);
	}

	rtl9602c_pon_w(p, RTL9602C_PONSCH_TCONT_QUEUES(tcont), qmap | BIT(queue));
	return 0;
}

/*
 * Build the packet buffer. Each half keeps frames in pages, in its on-chip
 * SRAM first and then in its DRAM region; the driver sets the halves up for
 * 128-byte pages. Both DMA engines are stopped and both halves disabled
 * while the buffer is reconfigured. The block is never reset, and bit 6 of
 * the upstream RTL9602C_PKTBUF_CTRL is left alone: setting it with the
 * engines running and the half enabled freezes the board beyond the
 * watchdog's reach (measured on this board).
 *
 * Sets @ready_us to the time the halves took to finish initialising, and
 * returns 0, or -ETIMEDOUT when a half does not finish.
 */
static int rtl9602c_pbo_init(struct rtl9602c_pon *p, s64 *ready_us)
{
	static const u32 sram[2] = {
		[RTL9602C_PBO_US] = RTL9602C_PBO_SRAM_US,
		[RTL9602C_PBO_DS] = RTL9602C_PBO_SRAM_DS,
	};

	/* The DRAM pool: RTL9602C_PBO_DRAM in pages, 8192 at most. */
	const u32 ram_pages = min_t(u32, RTL9602C_PBO_DRAM / RTL9602C_PBO_PAGE,
				    0x2000);
	u32 sram_runout[2], dram_runout[2], th;
	unsigned int d, s;
	ktime_t t0;

	for (d = 0; d < 2; d++)
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_DMA_CTRL(d),
				 RTL9602C_PKTBUF_DMA_CTRL_GMII_RX_EN |
			     RTL9602C_PKTBUF_DMA_CTRL_GMII_TX_EN, 0);
	for (d = 0; d < 2; d++)
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_CTRL(d),
				 RTL9602C_PKTBUF_CTRL_EN, 0);

	for (d = 0; d < 2; d++) {
		u32 sram_pages = sram[d] / RTL9602C_PBO_PAGE;

		/*
		 * The DRAM base is the region's physical start. The pools
		 * are given in pages, each count written as count - 1: the
		 * whole SRAM (128 pages up, 32 down) and ram_pages of DRAM.
		 * A pool counts as exhausted at its limit: two pages short
		 * of the SRAM, and in DRAM 40 pages plus the SRAM's pages
		 * short of the region. The SRAM sizes, the encoding and the
		 * limits are not verified.
		 */
		rtl9602c_pon_w(p, RTL9602C_PKTBUF_DRAM_BASE(d),
			       lower_32_bits(p->pbo_base[d]));
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_POOL_CFG(d),
				 RTL9602C_PKTBUF_POOL_CFG_SRAM_PAGES |
			     RTL9602C_PKTBUF_POOL_CFG_DRAM_PAGES,
			     FIELD_PREP(RTL9602C_PKTBUF_POOL_CFG_SRAM_PAGES,
					sram_pages - 1) |
			     FIELD_PREP(RTL9602C_PKTBUF_POOL_CFG_DRAM_PAGES,
					ram_pages - 1));

		sram_runout[d] = sram_pages - 2;
		dram_runout[d] = RTL9602C_PBO_DRAM / RTL9602C_PBO_PAGE - 0x28 -
				 sram_pages;
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_POOL_LIMIT(d),
				 RTL9602C_PKTBUF_POOL_LIMIT_SRAM |
			     RTL9602C_PKTBUF_POOL_LIMIT_DRAM,
			     FIELD_PREP(RTL9602C_PKTBUF_POOL_LIMIT_SRAM,
					sram_runout[d]) |
			     FIELD_PREP(RTL9602C_PKTBUF_POOL_LIMIT_DRAM,
					dram_runout[d]));
	}

	/*
	 * Upstream back-pressure marks, in pages: where a stream is stopped,
	 * the marks shared by all streams, and each stream's own. The stop
	 * mark and the shared ones follow from the upstream DRAM limit.
	 * Downstream flow control follows from the sum of the downstream
	 * limits, its off level written first. Upstream the on level lies
	 * above the off level and downstream below it, so which field is the
	 * level that asserts is not known. None of these marks is verified.
	 */
	rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_THR_US_STOP, RTL9602C_PKTBUF_THR_OFF,
			 FIELD_PREP(RTL9602C_PKTBUF_THR_OFF,
				    dram_runout[RTL9602C_PBO_US] - 0x28));
	rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_THR_US_GLOBAL,
			 RTL9602C_PKTBUF_THR_ON | RTL9602C_PKTBUF_THR_OFF,
		     FIELD_PREP(RTL9602C_PKTBUF_THR_ON,
				dram_runout[RTL9602C_PBO_US] - 0x78) |
		     FIELD_PREP(RTL9602C_PKTBUF_THR_OFF,
				dram_runout[RTL9602C_PBO_US] - 0x118));
	for (s = 0; s < RTL9602C_PON_NUM_SID; s++)
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_THR_US_STREAM(s),
				 RTL9602C_PKTBUF_THR_ON | RTL9602C_PKTBUF_THR_OFF,
			     FIELD_PREP(RTL9602C_PKTBUF_THR_ON, 0x96) |
			     FIELD_PREP(RTL9602C_PKTBUF_THR_OFF, 0x82));

	th = dram_runout[RTL9602C_PBO_DS] + sram_runout[RTL9602C_PBO_DS];
	rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_THR_DS, RTL9602C_PKTBUF_THR_OFF,
			 FIELD_PREP(RTL9602C_PKTBUF_THR_OFF, th - 0x84));
	rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_THR_DS, RTL9602C_PKTBUF_THR_ON,
			 FIELD_PREP(RTL9602C_PKTBUF_THR_ON, th - 0xa4));

	/*
	 * GPON framing in both halves. The rest is taken to be needed and is
	 * not verified: the upstream half's receive stop; when the upstream
	 * FIFO starts passing a frame on to the GPON MAC; and a page size code
	 * of 0, taken to mean 128 bytes, in both pools and both engines.
	 */
	for (d = 0; d < 2; d++)
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_CTRL(d),
				 RTL9602C_PKTBUF_CTRL_EPON_MODE, 0);
	rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_CTRL(RTL9602C_PBO_US), 0,
			 RTL9602C_PKTBUF_CTRL_RX_STOP_EN);
	rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_US_FIFO_CFG,
			 RTL9602C_PKTBUF_US_FIFO_CFG_SPACE | RTL9602C_PKTBUF_US_FIFO_CFG_START,
		     FIELD_PREP(RTL9602C_PKTBUF_US_FIFO_CFG_SPACE, 1) |
		     FIELD_PREP(RTL9602C_PKTBUF_US_FIFO_CFG_START, 3));
	for (d = 0; d < 2; d++) {
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_POOL_CFG(d),
				 RTL9602C_PKTBUF_POOL_CFG_PAGE_SIZE, 0);
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_DMA_PAGE_CFG(d),
				 RTL9602C_PKTBUF_DMA_PAGE_CFG_RX_SIZE |
			     RTL9602C_PKTBUF_DMA_PAGE_CFG_TX_SIZE, 0);
	}

	for (d = 0; d < 2; d++)
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_CTRL(d), 0,
				 RTL9602C_PKTBUF_CTRL_EN);

	/*
	 * A debug selection in each DMA engine; that the data path depends on
	 * it is not verified.
	 */
	for (d = 0; d < 2; d++) {
		u32 sel = FIELD_GET(RTL9602C_PKTBUF_DMA_DBG_SEL_FUNC,
				    rtl9602c_pon_r(p, RTL9602C_PKTBUF_DMA_DBG_SEL(d)));

		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_DMA_DBG_SEL(d),
				 RTL9602C_PKTBUF_DMA_DBG_SEL_FUNC,
			     FIELD_PREP(RTL9602C_PKTBUF_DMA_DBG_SEL_FUNC, sel | 2));
	}

	/*
	 * The switch-facing MAC of each half: its receive almost-full,
	 * transmit stop and extra transmit enables; on transmit a gap of 3,
	 * preamble length 1 and padding; on receive, frames with errors
	 * accepted. None of it is verified.
	 */
	for (d = 0; d < 2; d++) {
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_MAC_CFG(d), 0,
				 RTL9602C_PKTBUF_MAC_CFG_RX_AFULL_EN |
			     RTL9602C_PKTBUF_MAC_CFG_TX_STOP_EN |
			     RTL9602C_PKTBUF_MAC_CFG_TX_EN_EXTRA);
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_MAC_TX_CFG(d),
				 RTL9602C_PKTBUF_MAC_TX_CFG_IPG |
			     RTL9602C_PKTBUF_MAC_TX_CFG_PREAMBLE_LEN |
			     RTL9602C_PKTBUF_MAC_TX_CFG_PAD_EN,
			     FIELD_PREP(RTL9602C_PKTBUF_MAC_TX_CFG_IPG, 3) |
			     FIELD_PREP(RTL9602C_PKTBUF_MAC_TX_CFG_PREAMBLE_LEN, 1) |
			     RTL9602C_PKTBUF_MAC_TX_CFG_PAD_EN);
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_MAC_RX_CFG(d), 0,
				 RTL9602C_PKTBUF_MAC_RX_CFG_ACCEPT_ERR);
	}

	/*
	 * Each DMA control word's fields in one write, which also enables the
	 * engine, last of all: the burst-size selects (the first set, the
	 * second clear); early transmit, which sends before the whole frame is
	 * in the FIFO; transmit FIFO threshold 2 upstream and 1 downstream;
	 * receive FIFO threshold 2; receive burst 1. Only the enables are
	 * known to be needed.
	 */
	for (d = 0; d < 2; d++)
		rtl9602c_pon_rmw(p, RTL9602C_PKTBUF_DMA_CTRL(d),
				 RTL9602C_PKTBUF_DMA_CTRL_MAX_BURST_SEL0 |
			     RTL9602C_PKTBUF_DMA_CTRL_MAX_BURST_SEL1 |
			     RTL9602C_PKTBUF_DMA_CTRL_EARLY_TX |
			     RTL9602C_PKTBUF_DMA_CTRL_TX_FIFO_THR |
			     RTL9602C_PKTBUF_DMA_CTRL_RX_FIFO_THR |
			     RTL9602C_PKTBUF_DMA_CTRL_RX_MAX_BURST |
			     RTL9602C_PKTBUF_DMA_CTRL_GMII_RX_EN |
			     RTL9602C_PKTBUF_DMA_CTRL_GMII_TX_EN,
			     RTL9602C_PKTBUF_DMA_CTRL_MAX_BURST_SEL0 |
			     RTL9602C_PKTBUF_DMA_CTRL_EARLY_TX |
			     FIELD_PREP(RTL9602C_PKTBUF_DMA_CTRL_TX_FIFO_THR,
					d == RTL9602C_PBO_US ? 2 : 1) |
			     FIELD_PREP(RTL9602C_PKTBUF_DMA_CTRL_RX_FIFO_THR, 2) |
			     FIELD_PREP(RTL9602C_PKTBUF_DMA_CTRL_RX_MAX_BURST, 1) |
			     RTL9602C_PKTBUF_DMA_CTRL_GMII_RX_EN |
			     RTL9602C_PKTBUF_DMA_CTRL_GMII_TX_EN);

	/*
	 * Each half sets RTL9602C_PKTBUF_STAT_INIT_DONE once it has finished
	 * initialising after its engine starts (measured on this board). A
	 * queue drain issued before then is taken not to complete, which is
	 * not verified; the build waits for both halves.
	 */
	t0 = ktime_get();
	for (d = 0; d < 2; d++) {
		u32 sts;
		int err = read_poll_timeout(rtl9602c_pon_r, sts,
					    sts & RTL9602C_PKTBUF_STAT_INIT_DONE,
					    10, RTL9602C_PON_IPRDY_US, false, p,
					    RTL9602C_PKTBUF_STAT(d));
		if (err) {
			pr_err("the PON packet buffer's %s half did not finish initialising in %u ms: %08x\n",
			       d == RTL9602C_PBO_US ? "upstream" : "downstream",
			       RTL9602C_PON_IPRDY_US / 1000, sts);
			return err;
		}
	}
	*ready_us = ktime_us_delta(ktime_get(), t0);

	return 0;
}

/*
 * Give the PON port its own pause marks, RTL9602C_FC_PON_EXTRA pages above
 * the switch's global ones, each clamped to its field. That the PON port
 * needs them is not verified.
 */
static void rtl9602c_pon_fc_thresholds(struct rtl9602c_pon *p)
{
	static const u32 regs[][2] = {
		{ RTL9602C_PAUSE_THR_GLOBAL_HIGH, RTL9602C_PAUSE_THR_PON_HIGH },
		{ RTL9602C_PAUSE_THR_GLOBAL_LOW, RTL9602C_PAUSE_THR_PON_LOW },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u32 v = rtl9602c_pon_r(p, regs[i][0]);
		u32 on = min_t(u32, FIELD_GET(RTL9602C_PAUSE_THR_ON, v) +
			       RTL9602C_FC_PON_EXTRA,
			       FIELD_MAX(RTL9602C_PAUSE_THR_ON));
		u32 off = min_t(u32, FIELD_GET(RTL9602C_PAUSE_THR_OFF, v) +
				RTL9602C_FC_PON_EXTRA,
				FIELD_MAX(RTL9602C_PAUSE_THR_OFF));

		rtl9602c_pon_rmw(p, regs[i][1],
				 RTL9602C_PAUSE_THR_ON | RTL9602C_PAUSE_THR_OFF,
			     FIELD_PREP(RTL9602C_PAUSE_THR_ON, on) |
			     FIELD_PREP(RTL9602C_PAUSE_THR_OFF, off));
	}
}

/**
 * rtl9602c_ponmac_init() - build the PON MAC
 * @p: the switch
 *
 * Called at the start of every bring-up of the PON. The block is built by the
 * first call that succeeds and never torn down, so later calls return at
 * once. The GPON MAC must not be in a session here, or the drain of the
 * OMCC's queue does not complete (see rtl9602c_pon_drain()).
 *
 * A failed build leaves the block unbuilt, and the next call builds it again
 * from the start. The ONU can still be activated meanwhile, but no OMCI
 * message crosses and no GEM port can be created.
 *
 * Return: 0; -ENODEV without the system controller or the buffers
 * (rtl9602c_ponmac_probe()); -ETIMEDOUT when a half of the packet buffer
 * does not finish initialising or the OMCC's queue does not drain.
 */
int rtl9602c_ponmac_init(struct rtl9602c_pon *p)
{
	unsigned int omcc_q = RTL9602C_PON_QUEUE(RTL9602C_GPON_DEFAULT_TCONT_IDX,
						 0);
	unsigned int i, rev;
	s64 ready_us;
	int err;

	if (p->ponmac_up)
		return 0;
	if (!p->sysctl)
		return -ENODEV;

	/*
	 * Open the gate before anything else, and never close it: while
	 * RTL9602C_SYSCTL_BLOCK_EN_PKTBUF is clear, a read anywhere in the
	 * block never completes and the board stops, and the watchdog does
	 * not reliably recover it (measured on this board). Nothing before
	 * Linux opens it. RTL9602C_SYSCTL_SOC_STATE_READY is set first; what
	 * it does is not known, and that the block needs it is not verified.
	 */
	regmap_set_bits(p->sysctl, RTL9602C_SYSCTL_SOC_STATE,
			RTL9602C_SYSCTL_SOC_STATE_READY);
	regmap_set_bits(p->sysctl, RTL9602C_SYSCTL_BLOCK_EN,
			RTL9602C_SYSCTL_BLOCK_EN_PKTBUF);

	/*
	 * The token clock of the queue rate limiters. With every peak rate at
	 * the maximum nothing depends on it; its values are not verified.
	 */
	rtl9602c_pon_rmw(p, RTL9602C_PONSCH_TOKEN_CLK,
			 RTL9602C_PONSCH_TOKEN_CLK_PERIOD | RTL9602C_PONSCH_TOKEN_CLK_TOKENS,
		     FIELD_PREP(RTL9602C_PONSCH_TOKEN_CLK_PERIOD, 0x6e) |
		     FIELD_PREP(RTL9602C_PONSCH_TOKEN_CLK_TOKENS, 0x95));

	/*
	 * Per-bit forcing of the queues' committed-rate state, written whole,
	 * bits of no known field included; not verified.
	 */
	rtl9602c_pon_w(p, RTL9602C_PONSCH_CIR_FORCE_EN, 0xffffffff);
	rtl9602c_pon_w(p, RTL9602C_PONSCH_CIR_FORCE_VAL, 0xffffffff);

	/*
	 * Invert the output of the dying-gasp comparator, the supply-loss
	 * detector. Nothing else in the driver uses it, and which polarity
	 * reports a real loss is not verified.
	 */
	rtl9602c_pon_rmw(p, RTL9602C_DGASP_CFG, 0, RTL9602C_DGASP_CFG_INVERT);
	/* Thresholds on the remainder of a grant; not verified. */
	rtl9602c_pon_rmw(p, RTL9602C_PONSCH_GRANT_THR,
			 RTL9602C_PONSCH_GRANT_THR_LAST | RTL9602C_PONSCH_GRANT_THR_RUNT,
		     FIELD_PREP(RTL9602C_PONSCH_GRANT_THR_LAST, 5) |
		     FIELD_PREP(RTL9602C_PONSCH_GRANT_THR_RUNT, 5));
	/*
	 * Switch T-CONTs 0 to 15 off and empty their maps, so that no T-CONT
	 * schedules a queue the driver has not attached. T-CONT 16, the
	 * OMCC's, is left as it is.
	 */
	for (i = 0; i < RTL9602C_PON_NUM_TCONT - 1; i++) {
		rtl9602c_pon_rmw(p, RTL9602C_PONSCH_TCONT_EN, BIT(i), 0);
		rtl9602c_pon_w(p, RTL9602C_PONSCH_TCONT_QUEUES(i), 0);
	}
	/*
	 * Make a queue's peak rate a limit. With the bit set, a queue sends
	 * nothing above its peak rate, and dropping the excess takes from the
	 * grant of the T-CONT's other queues only when the queue is offered
	 * far more than its peak rate; with it clear, a queue goes over its
	 * peak rate whenever its T-CONT's other queues leave the grant unused
	 * (all measured on this board, see rtl9602c.rst).
	 */
	rtl9602c_pon_rmw(p, RTL9602C_PONSCH_CFG, 0, RTL9602C_PONSCH_CFG_PIR_DROP);
	for (i = 0; i < RTL9602C_PON_NUM_QUEUE; i++)
		rtl9602c_pon_queue_set(p, i, 0, 0, RTL9602C_PONSCH_QUEUE_RATE_MAX);
	/*
	 * The internal priority the switch gives the OMCI frames it traps to
	 * the CPU: the conduit takes priority 7 on a receive ring of its own,
	 * which a flood of data to the CPU cannot fill. The messages arrive
	 * there; that this register is what gives them the priority is not
	 * verified.
	 */
	rtl9602c_pon_rmw(p, RTL9602C_OMCI_TRAP_CFG, RTL9602C_OMCI_TRAP_CFG_PRI,
			 FIELD_PREP(RTL9602C_OMCI_TRAP_CFG_PRI,
				    RTL9602C_OMCI_TRAP_CFG_PRI_HIGHEST));

	/*
	 * Streams 0 to 63 are for GEM ports: each is parked, pointed at queue
	 * 63 and not admitted, until its GEM port's upstream is programmed. A
	 * stream that is not admitted sends nothing, even when its queue is
	 * scheduled (measured on this board). Stream 64, the OMCC's, goes to
	 * queue 64 and is admitted, and it is named to the PON MAC as the
	 * OMCC's stream; that the upstream OMCC needs the naming is not
	 * verified.
	 */
	for (i = 0; i < RTL9602C_PON_NUM_SID - 1; i++) {
		rtl9602c_ponq_stream_queue_set(p, i, RTL9602C_PON_QUEUE(15, 31));
		rtl9602c_ponq_stream_enable(p, i, false);
	}
	rtl9602c_ponq_stream_queue_set(p, RTL9602C_GPON_OMCC_FLOW, omcc_q);
	rtl9602c_ponq_stream_enable(p, RTL9602C_GPON_OMCC_FLOW, true);
	rtl9602c_pon_rmw(p, RTL9602C_OMCI_OMCC_CFG, RTL9602C_OMCI_OMCC_CFG_STREAM,
			 FIELD_PREP(RTL9602C_OMCI_OMCC_CFG_STREAM,
				    RTL9602C_GPON_OMCC_FLOW));

	err = rtl9602c_pbo_init(p, &ready_us);
	if (err)
		return err;

	/*
	 * Downstream, stream 64 takes the first of the buffer's four queues
	 * towards the switch; the other streams are left as they are. Not
	 * verified.
	 */
	rtl9602c_pon_rmw(p, RTL9602C_PONQ_DS_STREAM_QUEUE(RTL9602C_GPON_OMCC_FLOW),
			 RTL9602C_PONQ_DS_STREAM_QUEUE_MASK <<
		     RTL9602C_PONQ_DS_STREAM_QUEUE_SHIFT(RTL9602C_GPON_OMCC_FLOW), 0);

	/*
	 * Let the PON port and the CPU port pass PON control frames such as
	 * OMCI, and give the PON port its own pause marks. Neither is
	 * verified to be needed.
	 */
	rtl9602c_pon_rmw(p, RTL9602C_PORT_MAC_CFG(RTL9602C_PORT_PON), 0,
			 RTL9602C_PORT_MAC_CFG_RX_PON_CTRL);
	rtl9602c_pon_fc_thresholds(p);
	rtl9602c_pon_rmw(p, RTL9602C_PORT_MAC_CFG(RTL9602C_PORT_CPU), 0,
			 RTL9602C_PORT_MAC_CFG_RX_PON_CTRL);

	/*
	 * No peak-rate drop on chip revision 1. This board's chip reads
	 * revision 2 (measured), so the exception is not verified.
	 */
	rev = rtl9602c_chip_rev(p);
	if (rev == 1)
		rtl9602c_pon_rmw(p, RTL9602C_PONSCH_CFG,
				 RTL9602C_PONSCH_CFG_PIR_DROP, 0);

	/*
	 * The GPON MAC grants the allocations of the default Alloc-ID, which
	 * carry the OMCC (ITU-T G.984.3 clause 14.1), to T-CONT 16 (measured
	 * on this board). Attaching queue 64 there drains it; stream 64 is
	 * then pointed at the queue and admitted once more.
	 */
	err = rtl9602c_pon_queue_add(p, RTL9602C_GPON_DEFAULT_TCONT_IDX, 0);
	if (err)
		return err;
	rtl9602c_ponq_stream_queue_set(p, RTL9602C_GPON_OMCC_FLOW, omcc_q);
	rtl9602c_ponq_stream_enable(p, RTL9602C_GPON_OMCC_FLOW, true);

	p->ponmac_up = true;
	pr_info("PON MAC built: chip rev %u, OMCC in T-CONT %u queue %u, buffers at %pa and %pa, ready in %lld us\n",
		rev, RTL9602C_GPON_DEFAULT_TCONT_IDX, omcc_q,
		&p->pbo_base[RTL9602C_PBO_US], &p->pbo_base[RTL9602C_PBO_DS],
		ready_us);
	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/* One queue's scheduling as rtl9602c_pon_queue_set() writes it, rates raw. */
static void rtl9602c_queue_show(struct seq_file *m, struct rtl9602c_pon *p,
				unsigned int q)
{
	u32 weight = (rtl9602c_pon_r(p, RTL9602C_PONSCH_QUEUE_WEIGHT(q)) >>
		      RTL9602C_PONSCH_QUEUE_WEIGHT_SHIFT(q)) &
		     RTL9602C_PONSCH_QUEUE_WEIGHT_MASK;

	if (rtl9602c_pon_r(p, RTL9602C_PONSCH_QUEUE_WFQ(q)) & BIT(q % 32))
		seq_printf(m, "    queue %-2u  weight %-4u", q, weight);
	else
		seq_printf(m, "    queue %-2u  strict     ", q);
	seq_printf(m, "  cir %05lx  pir %05lx\n",
		   FIELD_GET(RTL9602C_PONSCH_QUEUE_RATE,
			     rtl9602c_pon_r(p, RTL9602C_PONSCH_QUEUE_CIR(q))),
		   FIELD_GET(RTL9602C_PONSCH_QUEUE_RATE,
			     rtl9602c_pon_r(p, RTL9602C_PONSCH_QUEUE_PIR(q))));
}

/* The queue stream @sid points at. */
static unsigned int rtl9602c_ponq_stream_queue_get(struct rtl9602c_pon *p,
						   unsigned int sid)
{
	return (rtl9602c_pon_r(p, RTL9602C_PONQ_US_STREAM_QUEUE(sid)) >>
		RTL9602C_PONQ_US_STREAM_QUEUE_SHIFT(sid)) & RTL9602C_PONQ_US_STREAM_QUEUE_MASK;
}

/*
 * ponmac: the OMCC's path in the order a message crosses it. First the gate
 * and whether the block is built, then the GPON MAC's downstream type and
 * upstream Port-ID words for flow 64; until the GPON MAC has been taken out
 * of reset, they read as a fixed pattern, which is harmless. Once the block
 * is built: the OMCC's stream, its queue and admission, the T-CONT enables
 * and T-CONT 16's map, the maps of T-CONTs 0 to 15 in use and the queues of
 * the streams admitted, and for each half the control word, readiness, DRAM
 * base, pool in use and DMA control word. The upstream pool holds three
 * descriptors when idle and one more for each message waiting (measured on
 * this board). Last the switch: the OMCI trap priority, the words holding
 * the PON and CPU ports' control-frame bits, and the OMCI counters, whose
 * meanings (frames dropped, frames and octets each way, received frames
 * with a bad CRC) are not verified.
 */
static int rtl9602c_ponmac_show(struct seq_file *m, void *unused)
{
	struct rtl9602c_pon *p = m->private;
	unsigned int f = RTL9602C_GPON_OMCC_FLOW, d, t, s;
	u32 qmap = RTL9602C_PONSCH_TCONT_QUEUES(RTL9602C_GPON_DEFAULT_TCONT_IDX);
	u32 v = 0;

	if (p->sysctl)
		regmap_read(p->sysctl, RTL9602C_SYSCTL_BLOCK_EN, &v);
	seq_printf(m, "gate          %s (sysctl 0x%03x = %08x)\n",
		   !p->sysctl ? "no system controller" :
		   v & RTL9602C_SYSCTL_BLOCK_EN_PKTBUF ? "open" : "CLOSED",
		   RTL9602C_SYSCTL_BLOCK_EN, v);
	seq_printf(m, "built         %s\n", p->ponmac_up ? "yes" : "no");

	seq_puts(m, "\nGPON MAC, OMCC flow\n");
	seq_printf(m, "  traffic     %08x\n",
		   rtl9602c_pon_r(p, RTL9602C_GEM_DS_FLOW_CFG(f)));
	seq_printf(m, "  us port     %08x\n",
		   rtl9602c_pon_r(p, RTL9602C_GEM_US_FLOW_CFG(f)));

	if (p->ponmac_up) {
		seq_puts(m, "\nPON MAC\n");
		seq_printf(m, "  omci sid    %lu\n",
			   FIELD_GET(RTL9602C_OMCI_OMCC_CFG_STREAM,
				     rtl9602c_pon_r(p, RTL9602C_OMCI_OMCC_CFG)));
		if (f < RTL9602C_PON_NUM_SID)
			seq_printf(m, "  sid %u       queue %u, %s\n", f,
				   rtl9602c_ponq_stream_queue_get(p, f),
				   rtl9602c_pon_r(p, RTL9602C_PONQ_US_STREAM_EN(f)) &
				   BIT(f % 32) ? "valid" : "invalid");
		else
			seq_printf(m, "  sid %u       none: no upstream stream\n",
				   f);
		seq_printf(m, "  tcont en    %08x  qmap[16] %08x\n",
			   rtl9602c_pon_r(p, RTL9602C_PONSCH_TCONT_EN),
			   rtl9602c_pon_r(p, qmap));
		/*
		 * The maps of T-CONTs 0 to 15 in use, each queue in them with
		 * its scheduling, and the streams admitted.
		 */
		for (t = 0; t < RTL9602C_GPON_DEFAULT_TCONT_IDX; t++) {
			unsigned long map;
			unsigned int b;

			map = rtl9602c_pon_r(p, RTL9602C_PONSCH_TCONT_QUEUES(t));
			if (!map)
				continue;
			seq_printf(m, "  qmap[%2u]    %08lx\n", t, map);
			for_each_set_bit(b, &map, 32)
				rtl9602c_queue_show(m, p, RTL9602C_PON_QUEUE(t, b));
		}
		for (s = 0; s < RTL9602C_GPON_OMCC_FLOW; s++)
			if (rtl9602c_pon_r(p, RTL9602C_PONQ_US_STREAM_EN(s)) & BIT(s % 32))
				seq_printf(m, "  sid %-2u      queue %u, valid\n", s,
					   rtl9602c_ponq_stream_queue_get(p, s));
		for (d = 0; d < 2; d++)
			seq_printf(m,
				   "  %s          ctl %08x  ready %u  base %08x  usage %08x  io %08x\n",
				   d == RTL9602C_PBO_US ? "us" : "ds",
				   rtl9602c_pon_r(p, RTL9602C_PKTBUF_CTRL(d)),
				   (unsigned int)!!(rtl9602c_pon_r(p, RTL9602C_PKTBUF_STAT(d)) &
						    RTL9602C_PKTBUF_STAT_INIT_DONE),
				   rtl9602c_pon_r(p, RTL9602C_PKTBUF_DRAM_BASE(d)),
				   rtl9602c_pon_r(p, RTL9602C_PKTBUF_POOL_USED(d)),
				   rtl9602c_pon_r(p, RTL9602C_PKTBUF_DMA_CTRL(d)));
	}

	seq_puts(m, "\nswitch\n");
	seq_printf(m, "  trap pri    %lu\n",
		   FIELD_GET(RTL9602C_OMCI_TRAP_CFG_PRI,
			     rtl9602c_pon_r(p, RTL9602C_OMCI_TRAP_CFG)));
	seq_printf(m, "  port misc   pon %08x  cpu %08x\n",
		   rtl9602c_pon_r(p, RTL9602C_PORT_MAC_CFG(RTL9602C_PORT_PON)),
		   rtl9602c_pon_r(p, RTL9602C_PORT_MAC_CFG(RTL9602C_PORT_CPU)));
	seq_printf(m, "  omci rx     %u pkts %u bytes, crc %u, drop %u\n",
		   rtl9602c_pon_r(p, RTL9602C_MIB_OMCI_RX_PKTS),
		   rtl9602c_pon_r(p, RTL9602C_MIB_OMCI_RX_OCTETS),
		   rtl9602c_pon_r(p, RTL9602C_MIB_OMCI_RX_CRC_ERRORS),
		   rtl9602c_pon_r(p, RTL9602C_MIB_OMCI_DISCARDS));
	seq_printf(m, "  omci tx     %u pkts %u bytes\n",
		   rtl9602c_pon_r(p, RTL9602C_MIB_OMCI_TX_PKTS),
		   rtl9602c_pon_r(p, RTL9602C_MIB_OMCI_TX_OCTETS));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_ponmac);

/*
 * GTC counters of the downstream GEM frames, which clear when read. Bits
 * 31:16 of RTL9602C_GTC_DS_GEM_MISMATCH_CNT count frames whose Port-ID the
 * Port-ID table does not hold (measured on this board). Bits 15:0 are taken
 * to count frames whose length disagrees with their header; not verified.
 * The frames that are not idle are counted in RTL9602C_GPON_CNT_GEM_FRAMES,
 * which only rtl9602c_gpon_cntr_sync() reads.
 *
 * RTL9602C_GEM_DS_STATS_IDX selects one of sixteen downstream GEM counters,
 * which RTL9602C_GEM_DS_STATS_DATA returns; what each counts, and whether it
 * clears when read, is not verified.
 */
#define RTL9602C_GTC_DS_GEM_MISMATCH_CNT	0x7011c0
#define RTL9602C_GEM_DS_STATS_IDX	0x704064
#define RTL9602C_GEM_DS_STATS_DATA	0x704068

static void rtl9602c_ponpath_regs(struct seq_file *m, struct rtl9602c_pon *p,
				  const char *label, const u32 *regs,
				  unsigned int n)
{
	unsigned int i;

	seq_printf(m, "  %-12s", label);
	for (i = 0; i < n; i++)
		seq_printf(m, " %08x", rtl9602c_pon_r(p, regs[i]));
	seq_putc(m, '\n');
}

/*
 * ponpath, once the block is built: the counters along the OMCC's path, from
 * the GPON MAC through the packet buffer to the switch's PON and CPU ports.
 * It writes only the selectors of indirect counters, but reading it clears
 * the counters it reads that clear on read and that nothing else reads; the
 * GEM frame count it shows is the GPON MAC's total since bring-up.
 */
static int rtl9602c_ponpath_show(struct seq_file *m, void *unused)
{
	/*
	 * The packet buffer's counters and words, read by offset; what each
	 * one holds is not verified, except where marked as measured. The
	 * downstream half: good frames received, bad frames received,
	 * fragments, frames dropped, errors, frames transmitted.
	 */
	static const u32 pbo_ds[] = {
		0xf0a024, 0xf0a038, 0xf0a04c, 0xf0a060, 0xf0a064, 0xf0a068,
	};

	/*
	 * The upstream half: good frames received, receive errors, bad frames
	 * received, frames transmitted, fragments transmitted, bursts that
	 * ended empty, frames received. The empty-burst count climbs while the
	 * grants go out as empty bursts with messages waiting in the half
	 * (measured on this board).
	 */
	static const u32 pbo_us[] = {
		0xf0203c, 0xf02050, 0xf02054, 0xf02068, 0xf0207c, 0xf02090,
		0xf02094,
	};

	/*
	 * The downstream half's page accounting: pages in total, queue 0's
	 * pages, a counter mask and the queue configuration.
	 */
	static const u32 pbo_ds_pages[] = {
		0xf0a0d8, 0xf0a100, 0xf0a07c, 0xf0a0f8,
	};

	/*
	 * The switch-facing MAC of each half, at 0xf0c000 downstream and
	 * 0xf04000 upstream: good frames, errors, frames missed, command,
	 * interrupt, RTL9602C_PKTBUF_MAC_RX_CFG, RTL9602C_PKTBUF_MAC_CFG and
	 * the media status. The receive half of the upstream MAC's good count
	 * moves as messages from the switch enter the buffer (measured on this
	 * board).
	 */
	static const u32 nic_ds[] = {
		0xf0c010, 0xf0c014, 0xf0c018, 0xf0c038, 0xf0c03c, 0xf0c044,
		0xf0c04c, 0xf0c058,
	};
	static const u32 nic_us[] = {
		0xf04010, 0xf04014, 0xf04018, 0xf04038, 0xf0403c, 0xf04044,
		0xf0404c, 0xf04058,
	};
	struct rtl9602c_pon *p = m->private;
	unsigned int f = RTL9602C_GPON_OMCC_FLOW, i;

	if (!p->ponmac_up) {
		seq_puts(m, "PON MAC not built: the PON link is down\n");
		return 0;
	}

	/*
	 * Flow 64's entry in the Port-ID table and the GEM frames since
	 * bring-up, both under gpon_lock, which the PLOAM path holds while it
	 * writes the table and the worker while it sums the counters. Then the
	 * mismatch counter, which only this view reads: reading it clears it,
	 * so it shows the frames since the last read.
	 */
	{
		u64 frames;
		u32 fail;
		u16 port = 0;
		bool hit = false;
		int err;

		mutex_lock(&p->gpon_lock);
		err = rtl9602c_gpon_port_get(p, f, &port, &hit);
		rtl9602c_gpon_cntr_sync(p);
		frames = p->gpon_stats.gem_frames;
		mutex_unlock(&p->gpon_lock);
		if (err)
			seq_printf(m, "<GPON> MAC cam[%u]    read timed out\n", f);
		else
			seq_printf(m, "<GPON> MAC cam[%u]    Port-ID %u, %s\n", f,
				   port, hit ? "hit" : "no hit");

		fail = rtl9602c_pon_r(p, RTL9602C_GTC_DS_GEM_MISMATCH_CNT);
		seq_printf(m, "<GPON> GTC gem        non-idle %llu  port-id mismatch %lu  length mismatch %lu\n",
			   frames, FIELD_GET(GENMASK(31, 16), fail),
			   FIELD_GET(GENMASK(15, 0), fail));
	}
	/*
	 * Flow 64's downstream frame and byte counts, through the GPON MAC's
	 * indirect counter port, then its upstream octets, low word first. The
	 * octet counter clears when read; the OMCC's flow is no GEM port's, so
	 * no GEM port's total loses by it.
	 */
	seq_printf(m, "<GPON> MAC flow %u  gem counters: sel0 %08x  sel1 %08x\n",
		   f, rtl9602c_gpon_flow_cntr(p, f, false),
		   rtl9602c_gpon_flow_cntr(p, f, true));
	seq_printf(m, "<GPON> MAC flow %u  upstream bytes: %08x %08x\n", f,
		   rtl9602c_pon_r(p, RTL9602C_GEM_US_FLOW_BYTES(f)),
		   rtl9602c_pon_r(p, RTL9602C_GEM_US_FLOW_BYTES(f) + 4));
	seq_puts(m, "<GPON> GEM misc      ");
	for (i = 0; i < 16; i++) {
		rtl9602c_pon_w(p, RTL9602C_GEM_DS_STATS_IDX, i);
		seq_printf(m, " %x", rtl9602c_pon_r(p, RTL9602C_GEM_DS_STATS_DATA));
	}
	seq_putc(m, '\n');

	seq_puts(m, "\npacket buffer\n");
	rtl9602c_ponpath_regs(m, p, "ds rx/tx", pbo_ds, ARRAY_SIZE(pbo_ds));
	rtl9602c_ponpath_regs(m, p, "ds pages", pbo_ds_pages,
			      ARRAY_SIZE(pbo_ds_pages));
	rtl9602c_ponpath_regs(m, p, "us rx/tx", pbo_us, ARRAY_SIZE(pbo_us));

	seq_puts(m, "\nbuffer MACs (ok err miss cmd intr rxcfg cfg media)\n");
	rtl9602c_ponpath_regs(m, p, "ds", nic_ds, ARRAY_SIZE(nic_ds));
	rtl9602c_ponpath_regs(m, p, "us", nic_us, ARRAY_SIZE(nic_us));

	seq_puts(m, "\nswitch MIB, first 16 words\n");
	seq_puts(m, "  pon rx      ");
	for (i = 0; i < 16; i++)
		seq_printf(m, " %08x",
			   rtl9602c_pon_r(p, RTL9602C_MIB_RX(RTL9602C_PORT_PON) + 4 * i));
	seq_puts(m, "\n  pon tx      ");
	for (i = 0; i < 16; i++)
		seq_printf(m, " %08x",
			   rtl9602c_pon_r(p, RTL9602C_MIB_TX(RTL9602C_PORT_PON) + 4 * i));
	seq_puts(m, "\n  cpu tx      ");
	for (i = 0; i < 16; i++)
		seq_printf(m, " %08x",
			   rtl9602c_pon_r(p, RTL9602C_MIB_TX(RTL9602C_PORT_CPU) + 4 * i));
	seq_putc(m, '\n');
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_ponpath);

/*
 * pondp, once the block is built: the upstream FIFO's start and space fields
 * and the upstream DMA engine's transmit threshold and early-transmit bit,
 * which are taken to decide together how much of a frame the upstream half
 * holds before it passes the frame on to the GPON MAC; not verified.
 */
static int rtl9602c_pondp_show(struct seq_file *m, void *unused)
{
	struct rtl9602c_pon *p = m->private;
	u32 fifo, io;

	if (!p->ponmac_up) {
		seq_puts(m, "PON MAC not built: the PON link is down\n");
		return 0;
	}
	fifo = rtl9602c_pon_r(p, RTL9602C_PKTBUF_US_FIFO_CFG);
	io = rtl9602c_pon_r(p, RTL9602C_PKTBUF_DMA_CTRL(RTL9602C_PBO_US));
	seq_printf(m, "us fifo cfg     %08x  start %lu  space %lu\n", fifo,
		   FIELD_GET(RTL9602C_PKTBUF_US_FIFO_CFG_START, fifo),
		   FIELD_GET(RTL9602C_PKTBUF_US_FIFO_CFG_SPACE, fifo));
	seq_printf(m, "us dma ctrl     %08x  tx-thr %lu  early-tx %u\n", io,
		   FIELD_GET(RTL9602C_PKTBUF_DMA_CTRL_TX_FIFO_THR, io),
		   !!(io & RTL9602C_PKTBUF_DMA_CTRL_EARLY_TX));
	return 0;
}

static int rtl9602c_pondp_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_pondp_show, inode->i_private);
}

static const struct file_operations rtl9602c_pondp_fops = {
	.owner   = THIS_MODULE,
	.open    = rtl9602c_pondp_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/**
 * rtl9602c_ponmac_debugfs_init() - create the PON MAC's debugfs views
 * @p: the switch, with its debugfs directory created
 *
 * Creates ponmac, ponpath and pondp, mode 0400. They are safe to read before
 * the block is built: ponpath and pondp then read nothing, and ponmac only
 * the system controller, the GPON MAC and the switch.
 */
void rtl9602c_ponmac_debugfs_init(struct rtl9602c_pon *p)
{
	debugfs_create_file("ponmac", 0400, p->pd->debugfs, p,
			    &rtl9602c_ponmac_fops);
	debugfs_create_file("ponpath", 0400, p->pd->debugfs, p,
			    &rtl9602c_ponpath_fops);
	debugfs_create_file("pondp", 0400, p->pd->debugfs, p,
			    &rtl9602c_pondp_fops);
}

#endif
