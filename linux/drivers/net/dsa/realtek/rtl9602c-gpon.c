// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: the GPON MAC.
 *
 * The GPON MAC runs the ONU's side of the transmission convergence layer of
 * ITU-T G.984.3: it frames the downstream, carries the PLOAM channel, reads the
 * bandwidth map and sends the ONU's upstream bursts in the allocations the OLT
 * grants. This file brings it up and down with the pon interface, runs the
 * activation state machine from O1 to O5 on the PLOAM messages the OLT sends,
 * programs the burst overhead, the response time and the equalization delay
 * that ranging assigns, keeps the Alloc-ID and Port-ID tables, adds up the GTC
 * counters, reports the ONU's status to the PON class and offers debugfs views
 * of all of it.
 *
 * The activation state is software's: the MAC takes the state written into it
 * and classifies the OLT's grants by it. PLOAM messages are dispatched from the
 * GPON interrupt, since G.984.3 gives the ONU 750 us to act on one.
 *
 * The driver implements G.984.3's states O1 to O7 and its timers TO1 and TO2.
 * LOS or LOF sends an ONU in O2 to O4 back to O1 and one in O5 to O6, where it
 * keeps its ranging and sends nothing until POPUP or TO2; O1 moves on to O2
 * once both have cleared. Disable_Serial_Number stops the ONU in O7, laser off,
 * until it is enabled again. It acts on Upstream_Overhead,
 * Extended_Burst_Length, Assign_ONU-ID, Ranging_Time, Deactivate_ONU-ID,
 * Disable_Serial_Number, POPUP, Request_Password, Assign_Alloc-ID and
 * Configure_Port-ID, and drops every other downstream message, even where
 * G.984.3 asks for an answer: BER_Interval, Key_Switching_Time and
 * Encrypted_Port-ID get no Acknowledge. No encryption
 * key is programmed and nothing is set up for upstream FEC; that neither is
 * needed is an assumption, not verified, and a Port-ID an OLT encrypts would
 * arrive as ciphertext. There is no power levelling, no REI message and no
 * signal-fail or signal-degrade alarm, and nothing in the driver sends the
 * Dying_Gasp message armed in the MAC.
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
#include <linux/ktime.h>
#include <linux/ctype.h>
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
#include <linux/random.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include <linux/pon.h>

#include "rtl9602c.h"

/**
 * rtl9602c_gpon_report() - log the GPON MAC's self-test result and revision
 * @p: the switch
 *
 * Called once at switch setup; writes nothing. The MAC's top registers, among
 * them the revision and the self-test result, answer at any time. Its other
 * registers answer only once the MAC has been taken out of reset; until then
 * they read RTL9602C_GPON_NO_RESPONSE, the bus's reply when nothing decodes an
 * access, and reading them is harmless. The second line logged says which:
 * after a cold start the MAC is still in reset, while a warm reset leaves it
 * running.
 *
 * The self-test bits are taken to mean memory test done, data-retention test
 * done, memory test failed and retention test failed. That is not verified: no
 * failure bit has been seen set.
 */
void rtl9602c_gpon_report(struct rtl9602c_priv *p)
{
	u32 bist = rtl9602c_r(p, RTL9602C_GPON_MEM_SELFTEST);
	u32 gtc = rtl9602c_r(p, RTL9602C_GTC_DS_IRQ_STAT);

	if (bist & (RTL9602C_GPON_MEM_SELFTEST_FAIL | RTL9602C_GPON_MEM_SELFTEST_RETAIN_FAIL))
		pr_err("<GPON> MAC self test FAILED (0x%08x)\n", bist);
	else if (!(bist & RTL9602C_GPON_MEM_SELFTEST_DONE))
		pr_warn("<GPON> MAC self test did not complete (0x%08x)\n", bist);
	else
		pr_info("<GPON> MAC rev 0x%08x, self test passed%s\n",
			rtl9602c_r(p, RTL9602C_GPON_REVISION),
			bist & RTL9602C_GPON_MEM_SELFTEST_RETAIN_DONE ?
			", data retention included" : "");

	if (gtc == RTL9602C_GPON_NO_RESPONSE)
		pr_info("<GPON> MAC: nothing answers below the top level yet\n");
	else
		pr_info("<GPON> MAC: the transmission-convergence layer is answering (0x%08x)\n",
			gtc);
}

/*
 * The analog front end of the PON SerDes. RTL9602C_SERDES_ANA_RX_CFG_SD_POR
 * must be clear: set, the receiver's signal detect reports no signal and the
 * downstream does not frame (measured on this board). None of the other writes
 * is verified to be needed; the downstream locks and the ONU reaches O5 with
 * all of them in place.
 */
static void rtl9602c_pon_analog_init(struct rtl9602c_priv *p)
{
	/*
	 * Five tuning words written whole, the second of them the clock
	 * multiplier; not verified.
	 */
	rtl9602c_w(p, RTL9602C_SERDES_ANA_TUNE2, RTL9602C_SERDES_ANA_TUNE2_INIT);
	rtl9602c_w(p, RTL9602C_SERDES_ANA_PLL_CFG, RTL9602C_SERDES_ANA_PLL_CFG_INIT);
	rtl9602c_w(p, RTL9602C_SERDES_ANA_TUNE8, RTL9602C_SERDES_ANA_TUNE8_INIT);
	rtl9602c_w(p, RTL9602C_SERDES_ANA_TUNE25, RTL9602C_SERDES_ANA_TUNE25_INIT);
	rtl9602c_w(p, RTL9602C_SERDES_ANA_TUNE24, RTL9602C_SERDES_ANA_TUNE24_INIT);

	/*
	 * One clock-recovery loop filter setting for all three line-rate
	 * profiles, taken to be integral gain 1 and proportional gains 4 and 2;
	 * neither the value nor that reading of it is verified.
	 */
	rtl9602c_w(p, RTL9602C_SERDES_ANA_CDR_LOOP_1G25, RTL9602C_SERDES_ANA_CDR_LOOP_INIT);
	rtl9602c_w(p, RTL9602C_SERDES_ANA_CDR_LOOP_EPON, RTL9602C_SERDES_ANA_CDR_LOOP_INIT);
	rtl9602c_w(p, RTL9602C_SERDES_ANA_CDR_LOOP_GPON, RTL9602C_SERDES_ANA_CDR_LOOP_INIT);

	/*
	 * Enable the analog front end of the clock recovery (not verified), and
	 * clear SD_POR. What SD_POR selects is not known: the downstream can
	 * frame with it set too, under a condition not identified.
	 */
	rtl9602c_rmw(p, RTL9602C_SERDES_ANA_RX_CFG,
		     RTL9602C_SERDES_ANA_RX_CFG_CDR_AFE | RTL9602C_SERDES_ANA_RX_CFG_SD_POR,
		     RTL9602C_SERDES_ANA_RX_CFG_CDR_AFE);

	/*
	 * Force the analog line-rate profile to GPON's, 2.48832 Gbit/s down and
	 * 1.24416 Gbit/s up, rather than let it follow the digital side; not
	 * verified.
	 */
	rtl9602c_rmw(p, RTL9602C_SERDES_ANA_FORCE_RATE,
		     RTL9602C_SERDES_ANA_FORCE_RATE_VAL | RTL9602C_SERDES_ANA_FORCE_RATE_EN,
		     FIELD_PREP(RTL9602C_SERDES_ANA_FORCE_RATE_VAL,
				RTL9602C_SERDES_ANA_FORCE_RATE_VAL_GPON) |
		     RTL9602C_SERDES_ANA_FORCE_RATE_EN);

	/* The burst-enable output to the optics at TTL level; not verified. */
	rtl9602c_rmw(p, RTL9602C_SERDES_ANA_BURST_OUT,
		     RTL9602C_SERDES_ANA_BURST_OUT_TTL, RTL9602C_SERDES_ANA_BURST_OUT_TTL);

	/*
	 * Upstream transmit amplitude 5, where it resets to 7; the receiver
	 * filter configuration cleared; and the manual controls of the clock
	 * recovery's reset and loop filter set, though which level of the reset
	 * bit holds the clock recovery in reset is not known. None of it is
	 * verified.
	 */
	rtl9602c_rmw(p, RTL9602C_SERDES_ANA_TX_CFG, RTL9602C_SERDES_ANA_TX_CFG_AMP,
		     FIELD_PREP(RTL9602C_SERDES_ANA_TX_CFG_AMP,
				RTL9602C_SERDES_ANA_TX_CFG_AMP_GPON));
	rtl9602c_rmw(p, RTL9602C_SERDES_ANA_RX_FILTER,
		     RTL9602C_SERDES_ANA_RX_FILTER_CFG, 0);
	rtl9602c_rmw(p, RTL9602C_SERDES_ANA_CDR_CTRL,
		     RTL9602C_SERDES_ANA_CDR_CTRL_RESET |
		     RTL9602C_SERDES_ANA_CDR_CTRL_LPF_EN,
		     RTL9602C_SERDES_ANA_CDR_CTRL_RESET |
		     RTL9602C_SERDES_ANA_CDR_CTRL_LPF_EN);

	/*
	 * Override the receiver enable with the receiver off, then on, then
	 * wait 50 ms; not verified.
	 */
	rtl9602c_w(p, RTL9602C_SERDES_ANA_FORCE_RX, RTL9602C_SERDES_ANA_FORCE_RX_OFF);
	rtl9602c_w(p, RTL9602C_SERDES_ANA_FORCE_RX, RTL9602C_SERDES_ANA_FORCE_RX_ON);
	msleep(50);

	/* Last, the burst-enable path out of power-down; not verified. */
	rtl9602c_rmw(p, RTL9602C_SERDES_PON_PDOWN,
		     RTL9602C_SERDES_PON_PDOWN_BURST, 0);
}

/*
 * Switch the PON SerDes to GPON mode, without which the MAC reset does not
 * complete (measured on this board).
 */
static void rtl9602c_pon_serdes_gpon(struct rtl9602c_priv *p)
{
	/*
	 * Park the SerDes and clear its reset register, so that it is stopped
	 * while it is set up; not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_SERDES_PON_CTRL, RTL9602C_SERDES_PON_CTRL_MODE,
		     FIELD_PREP(RTL9602C_SERDES_PON_CTRL_MODE, RTL9602C_SERDES_PON_CTRL_MODE_OFF));
	rtl9602c_w(p, RTL9602C_SERDES_PON_RESET, 0);

	/*
	 * Reset the PON SerDes and give it 10 ms; the bit is never cleared.
	 * That this is needed is not verified, nor whether it resets the GPON
	 * MAC as well.
	 */
	rtl9602c_rmw(p, RTL9602C_CHIP_RESET, RTL9602C_CHIP_RESET_PON_SERDES,
		     RTL9602C_CHIP_RESET_PON_SERDES);

	usleep_range(10000, 11000);

	/*
	 * Enable the output driver of the burst-enable signal to the optics,
	 * with no delay against the data, and select data source 0 for the
	 * transmitter; not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_SERDES_PON_OPTIC_CTRL, RTL9602C_SERDES_PON_OPTIC_CTRL_BURST_OE,
		     RTL9602C_SERDES_PON_OPTIC_CTRL_BURST_OE);
	rtl9602c_rmw(p, RTL9602C_SERDES_PON_TX_CFG,
		     RTL9602C_SERDES_PON_TX_CFG_BURST_DLY |
		     RTL9602C_SERDES_PON_TX_CFG_D2A_SEL, 0);

	/* GPON mode, which the MAC reset waits for. */
	rtl9602c_rmw(p, RTL9602C_SERDES_PON_CTRL, RTL9602C_SERDES_PON_CTRL_MODE,
		     FIELD_PREP(RTL9602C_SERDES_PON_CTRL_MODE, RTL9602C_SERDES_PON_CTRL_MODE_GPON));

	/*
	 * Override the SerDes's bit-error indication (not verified), and force
	 * no burst enable: forced, it does not make the module emit (measured
	 * on this board), and what it does force is not known.
	 */
	rtl9602c_rmw(p, RTL9602C_SERDES_ANA_FORCE_BER,
		     RTL9602C_SERDES_ANA_FORCE_BER_VAL |
		     RTL9602C_SERDES_ANA_FORCE_BER_EN,
		     RTL9602C_SERDES_ANA_FORCE_BER_VAL |
		     RTL9602C_SERDES_ANA_FORCE_BER_EN);
	rtl9602c_rmw(p, RTL9602C_SERDES_PON_BURST_FORCE, RTL9602C_SERDES_PON_BURST_FORCE_EN, 0);
}

/*
 * Reset the GPON MAC and wait up to 20 ms for it to finish, which it does only
 * with the SerDes in GPON mode. From then on the GTC registers answer, and the
 * MAC holds O1 and no ONU-ID.
 */
static int rtl9602c_gpon_reset(struct rtl9602c_priv *p)
{
	u32 v;
	int err;

	rtl9602c_rmw(p, RTL9602C_GPON_RESET_CTRL, RTL9602C_GPON_RESET_CTRL_TRIGGER,
		     RTL9602C_GPON_RESET_CTRL_TRIGGER);
	rtl9602c_rmw(p, RTL9602C_GPON_RESET_CTRL, RTL9602C_GPON_RESET_CTRL_TRIGGER, 0);

	err = read_poll_timeout(rtl9602c_r, v, v & RTL9602C_GPON_RESET_CTRL_DONE,
				100, 20000, false, p, RTL9602C_GPON_RESET_CTRL);
	if (err)
		pr_err("<GPON> MAC did not leave reset (reset control %08x)\n",
		       rtl9602c_r(p, RTL9602C_GPON_RESET_CTRL));
	return err;
}

/*
 * Add an event to the ploamlog ring: its kind, the state then, a word of detail
 * (@aux) and the message, if any. An event equal in all of these to the one
 * before it counts as a repeat of that one and moves its time on, so an
 * unbroken run of one message takes one entry.
 */
static void rtl9602c_ploam_log(struct rtl9602c_priv *p, u8 kind, const u8 *msg,
			       u32 aux)
{
	struct rtl9602c_ploam_ev *ev;
	unsigned long flags;
	u8 state = READ_ONCE(p->gpon_state);

	spin_lock_irqsave(&p->ploam_log_lock, flags);
	if (p->ploam_log_next) {
		ev = &p->ploam_log[(p->ploam_log_next - 1) %
				   RTL9602C_PLOAM_LOG_LEN];
		if (ev->kind == kind && ev->aux == aux && ev->state == state &&
		    ev->repeat < U16_MAX &&
		    (msg ? !memcmp(ev->msg, msg, sizeof(ev->msg)) :
			   !memchr_inv(ev->msg, 0, sizeof(ev->msg)))) {
			ev->repeat++;
			ev->ns = ktime_get_ns();
			goto out;
		}
	}
	ev = &p->ploam_log[p->ploam_log_next++ % RTL9602C_PLOAM_LOG_LEN];
	ev->ns = ktime_get_ns();
	ev->kind = kind;
	ev->state = state;
	ev->aux = aux;
	ev->repeat = 0;
	if (msg)
		memcpy(ev->msg, msg, sizeof(ev->msg));
	else
		memset(ev->msg, 0, sizeof(ev->msg));
out:
	spin_unlock_irqrestore(&p->ploam_log_lock, flags);
}

/*
 * Record the empty flags of the normal and urgent upstream PLOAM queues when
 * they have changed. A queue's flag clears when it takes a message and sets
 * again once the message has been sent (measured on this board).
 */
static void rtl9602c_ploam_log_usq(struct rtl9602c_priv *p)
{
	u32 ind = rtl9602c_r(p, RTL9602C_PLOAM_TX_CTRL);
	u32 bits = ind & (RTL9602C_PLOAM_TX_CTRL_NORMAL_EMPTY |
			  RTL9602C_PLOAM_TX_CTRL_URGENT_EMPTY);

	if (bits == p->ploam_usq_last)
		return;
	p->ploam_usq_last = bits;
	rtl9602c_ploam_log(p, RTL9602C_EV_USQ, NULL, ind);
}

/*
 * Copy the message at the head of the downstream PLOAM queue into @msg, two
 * octets to a data word with the earlier in the high byte, then pop it. The MAC
 * checks the CRC itself. Returns false when the queue is empty.
 */
static bool rtl9602c_ploam_pop(struct rtl9602c_priv *p, u8 *msg)
{
	int i;

	if (rtl9602c_r(p, RTL9602C_PLOAM_RX_CTRL) &
	    RTL9602C_PLOAM_RX_CTRL_EMPTY)
		return false;

	for (i = 0; i < RTL9602C_GPON_PLOAM_WORDS; i++) {
		u32 w = rtl9602c_r(p, RTL9602C_PLOAM_RX_DATA(i));

		msg[i * 2]     = FIELD_GET(RTL9602C_PLOAM_RX_DATA_WORD, w) >> 8;
		msg[i * 2 + 1] = FIELD_GET(RTL9602C_PLOAM_RX_DATA_WORD, w);
	}
	rtl9602c_rmw(p, RTL9602C_PLOAM_RX_CTRL,
		     RTL9602C_PLOAM_RX_CTRL_POP, RTL9602C_PLOAM_RX_CTRL_POP);
	return true;
}

/*
 * Set the equalization delay to @delay bits. RTL9602C_RANGING_EQD takes @delay
 * plus the coarse part of the response delay, in its units of 128 bits, split
 * into whole upstream frames and the bits left over; with that sum the bursts
 * land where the OLT listens, in ranging and in O5. @delay is kept, so that a
 * Ranging_Time in O5 is applied only when it changes the delay.
 */
static void rtl9602c_gpon_set_eqd(struct rtl9602c_priv *p, u32 delay)
{
	u32 min_delay = FIELD_GET(RTL9602C_GTC_US_RESP_DELAY_COARSE,
				   rtl9602c_r(p, RTL9602C_GTC_US_RESP_DELAY));
	u32 eqd1 = delay + min_delay * RTL9602C_GTC_US_RESP_DELAY_COARSE_BITS;
	u32 multi = eqd1 / RTL9602C_GPON_EQD_FRAME_BITS;
	u32 intra = eqd1 % RTL9602C_GPON_EQD_FRAME_BITS;

	p->gpon_eqd_delay = delay;
	rtl9602c_w(p, RTL9602C_RANGING_EQD,
		   FIELD_PREP(RTL9602C_RANGING_EQD_FRAMES, multi) |
		   FIELD_PREP(RTL9602C_RANGING_EQD_BITS, intra));
	pr_info("<GPON> EQD: %u + %u * 128 = %u -> mf %u, in-frame %u\n",
		delay, min_delay, eqd1, multi, intra);
}

/*
 * The burst overhead in bytes that the stored Upstream_Overhead and a type 3
 * preamble of @type3 bytes ask for: the guard bits, of which at most 32 count,
 * the type 3 preamble, the three delimiter octets and the type 1 and type 2
 * preamble bits.
 */
static unsigned int rtl9602c_gpon_boh_bytes(struct rtl9602c_priv *p, u8 type3)
{
	const u8 *ovh = p->gpon_overhead;

	return min_t(unsigned int, ovh[RTL9602C_PLOAM_OVERHEAD_GUARD_BITS], 32) / 8 +
	       type3 + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN +
	       (ovh[RTL9602C_PLOAM_OVERHEAD_TYPE1_BITS] +
		ovh[RTL9602C_PLOAM_OVERHEAD_TYPE2_BITS]) / 8;
}

/*
 * The burst overhead in bytes, as RTL9602C_BURST_CFG_LEN takes it, for a type 3
 * preamble of @type3 bytes: rtl9602c_gpon_boh_bytes() clamped between
 * RTL9602C_BURST_CFG_LEN_DEF and RTL9602C_BURST_CFG_LEN_MAX, the 128 bytes
 * G.984.3 allows. Without a type 3 length it is RTL9602C_BURST_CFG_LEN_DEF, 12
 * bytes, the 96 bits of burst overhead G.984.2 gives for the 1.24416 Gbit/s
 * upstream.
 *
 * The ONU reaches O5 with the lengths this gives. That the guard belongs in the
 * length, and only up to 32 bits, is not verified.
 */
static u8 rtl9602c_gpon_boh_len(struct rtl9602c_priv *p, u8 type3)
{
	if (!type3)
		return RTL9602C_BURST_CFG_LEN_DEF;
	return clamp_t(unsigned int, rtl9602c_gpon_boh_bytes(p, type3),
		       RTL9602C_BURST_CFG_LEN_DEF, RTL9602C_BURST_CFG_LEN_MAX);
}

/*
 * Set the burst overhead to @len bytes. The repeat index stays at the last
 * pattern byte before the delimiter; that it names the byte the MAC repeats to
 * make up the length is inferred from its value, not verified.
 */
static void rtl9602c_gpon_set_boh(struct rtl9602c_priv *p, u8 len)
{
	rtl9602c_w(p, RTL9602C_BURST_CFG,
		   FIELD_PREP(RTL9602C_BURST_CFG_REPEAT_IDX,
			      RTL9602C_BURST_CFG_REPEAT_IDX_DEF) |
		   FIELD_PREP(RTL9602C_BURST_CFG_LEN, len));
}

/*
 * Take the burst overhead and the delay from Upstream_Overhead (G.984.3
 * clause 9.2.3.1). The nine preamble bytes follow the order of G.984.2: as
 * many bytes as the guard counts in the length, which carry the type 3
 * pattern as before, then the type 1 preamble (all ones) and the type 2
 * preamble (all zeros) in whole bytes, then the type 3 pattern; then come the
 * three delimiter octets. At least the last preamble byte, the one at the
 * repeat index, stays the pattern, so a type 1 or type 2 count too long for
 * the nine bytes is cut short; the length still counts it, and the repeated
 * pattern fills it. The length starts at RTL9602C_BURST_CFG_PRERANGED, 12
 * bytes, with which the OLT decodes the serial-number bursts (measured on this
 * board). Extended_Burst_Length and Ranging_Time replace it; without
 * Extended_Burst_Length it stays 12 bytes in every state, since the stored
 * type 3 lengths are zeroed here.
 *
 * The number of extra serial-number transmissions, which G.984.3 deprecates
 * (00), goes into RTL9602C_GTC_DS_CTRL_EXTRA_SN. The equalization delay starts
 * at the pre-assigned delay, given in units of 32 bytes, when the message says
 * it applies, and at 0 otherwise. The message is kept for
 * rtl9602c_gpon_boh_len().
 */
static void rtl9602c_gpon_set_overhead(struct rtl9602c_priv *p, const u8 *msg)
{
	const unsigned int pre = RTL9602C_GPON_BOH_BYTES - RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN;
	u8 boh[RTL9602C_GPON_BOH_BYTES];
	unsigned int guard, ones, zeros, i;
	u32 eqd;

	guard = min_t(unsigned int, msg[RTL9602C_PLOAM_OVERHEAD_GUARD_BITS], 32) / 8;
	ones = min_t(unsigned int, msg[RTL9602C_PLOAM_OVERHEAD_TYPE1_BITS] / 8,
		     pre - 1 - guard);
	zeros = min_t(unsigned int, msg[RTL9602C_PLOAM_OVERHEAD_TYPE2_BITS] / 8,
		      pre - 1 - guard - ones);

	for (i = 0; i < pre; i++) {
		if (i >= guard && i < guard + ones)
			boh[i] = RTL9602C_PREAMBLE_TYPE1;
		else if (i >= guard + ones && i < guard + ones + zeros)
			boh[i] = RTL9602C_PREAMBLE_TYPE2;
		else
			boh[i] = msg[RTL9602C_PLOAM_OVERHEAD_TYPE3_PATTERN];
	}
	for (i = 0; i < RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN; i++)
		boh[pre + i] = msg[RTL9602C_PLOAM_OVERHEAD_DELIMITER + i];

	for (i = 0; i < pre + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN; i++)
		rtl9602c_w(p, RTL9602C_BURST_DATA(i),
			   FIELD_PREP(RTL9602C_BURST_DATA_BYTE, boh[i]));

	rtl9602c_w(p, RTL9602C_BURST_CFG,
		   RTL9602C_BURST_CFG_PRERANGED);

	rtl9602c_rmw(p, RTL9602C_GTC_DS_CTRL, RTL9602C_GTC_DS_CTRL_EXTRA_SN,
		     FIELD_PREP(RTL9602C_GTC_DS_CTRL_EXTRA_SN,
				(msg[RTL9602C_PLOAM_OVERHEAD_FLAGS] >>
				 RTL9602C_PLOAM_OVERHEAD_EXTRA_SN_SHIFT) &
				RTL9602C_PLOAM_OVERHEAD_EXTRA_SN_MASK));

	eqd = ((msg[RTL9602C_PLOAM_OVERHEAD_PRE_EQD_HI] << 8) |
	       msg[RTL9602C_PLOAM_OVERHEAD_PRE_EQD_LO]) << RTL9602C_PLOAM_OVERHEAD_PRE_EQD_SHIFT;

	if (!(msg[RTL9602C_PLOAM_OVERHEAD_FLAGS] & RTL9602C_PLOAM_OVERHEAD_PRE_EQD_EN))
		eqd = 0;
	rtl9602c_gpon_set_eqd(p, eqd);

	memcpy(p->gpon_overhead, msg, RTL9602C_GPON_PLOAM_BYTES);

	p->gpon_t3_preranged = 0;
	p->gpon_t3_ranged = 0;
	p->gpon_ebl_seen = false;
	p->gpon_sn_seen = false;

	pr_info("<GPON> burst: guard %u, t1 %u, t2 %u, pat %02x, delim %02x%02x%02x, eqd %u\n",
		msg[RTL9602C_PLOAM_OVERHEAD_GUARD_BITS], msg[RTL9602C_PLOAM_OVERHEAD_TYPE1_BITS],
		msg[RTL9602C_PLOAM_OVERHEAD_TYPE2_BITS], msg[RTL9602C_PLOAM_OVERHEAD_TYPE3_PATTERN],
		msg[RTL9602C_PLOAM_OVERHEAD_DELIMITER], msg[RTL9602C_PLOAM_OVERHEAD_DELIMITER + 1],
		msg[RTL9602C_PLOAM_OVERHEAD_DELIMITER + 2], eqd);
	pr_info("<GPON> Upstream_Overhead raw: %12ph\n", msg);
}

/*
 * Write a 12-octet upstream PLOAM message into slot @type; the MAC adds the
 * CRC. The normal and urgent slots are queues, sent in the ONU's allocations.
 * The others hold templates the MAC sends by itself, Serial_Number_ONU in
 * answer to every serial-number request. The data window addresses the slot
 * selected last, so the slot is selected, with PUSH clear, before the data is
 * written, and PUSH is set after it.
 *
 * A queue's data window is taken to be its tail, so a message written into a
 * full queue would overwrite the one waiting there (not verified): a full queue
 * refuses the message with -EBUSY. Every write and every refusal is recorded in
 * the ploamlog ring with the slot and the queue flags.
 */
static int rtl9602c_ploam_enqueue(struct rtl9602c_priv *p, const u8 *msg,
				  unsigned int type)
{
	u8 shown[RTL9602C_GPON_PLOAM_WORDS * 2];
	int i;

	mutex_lock(&p->ploam_lock);

	if (type == RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL ||
	    type == RTL9602C_PLOAM_TX_CTRL_SLOT_URGENT) {
		u32 full = type == RTL9602C_PLOAM_TX_CTRL_SLOT_URGENT ?
			   RTL9602C_PLOAM_TX_CTRL_URGENT_FULL :
			   RTL9602C_PLOAM_TX_CTRL_NORMAL_FULL;

		u32 ind = rtl9602c_r(p, RTL9602C_PLOAM_TX_CTRL);

		if (ind & full) {
			mutex_unlock(&p->ploam_lock);
			rtl9602c_ploam_log(p, RTL9602C_EV_US_REFUSED, msg,
					   (type << 24) | (ind & 0xffffff));
			dev_warn_ratelimited(p->dev,
					     "<GPON> upstream PLOAM queue %u full\n",
					     type);
			return -EBUSY;
		}
	}

	rtl9602c_rmw(p, RTL9602C_PLOAM_TX_CTRL,
		     RTL9602C_PLOAM_TX_CTRL_SLOT | RTL9602C_PLOAM_TX_CTRL_PUSH,
		     FIELD_PREP(RTL9602C_PLOAM_TX_CTRL_SLOT, type));

	for (i = 0; i < RTL9602C_GPON_PLOAM_PAYLOAD_WORDS; i++)
		rtl9602c_w(p, RTL9602C_PLOAM_TX_DATA(i),
			   (msg[i * 2] << 8) | msg[i * 2 + 1]);

	rtl9602c_rmw(p, RTL9602C_PLOAM_TX_CTRL,
		     RTL9602C_PLOAM_TX_CTRL_PUSH, RTL9602C_PLOAM_TX_CTRL_PUSH);

	/*
	 * The ring is readable in debugfs: a password, or a fragment of a
	 * key, stays out of it.
	 */
	memcpy(shown, msg, sizeof(shown));
	if (msg[1] == RTL9602C_PLOAM_US_PASSWORD)
		memset(&shown[2], 0, RTL9602C_GPON_PASSWORD_BYTES);
	if (msg[1] == RTL9602C_PLOAM_US_ENCRYPTION_KEY)
		memset(&shown[RTL9602C_PLOAM_KEY_BYTES], 0,
		       RTL9602C_GPON_AES_FRAG_BYTES);
	rtl9602c_ploam_log(p, RTL9602C_EV_US, shown,
			   (type << 24) |
			   (rtl9602c_r(p, RTL9602C_PLOAM_TX_CTRL) & 0xffffff));
	mutex_unlock(&p->ploam_lock);
	return 0;
}

/*
 * Arm the Serial_Number_ONU template (G.984.3 clause 9.2.4.1), which the MAC
 * sends by itself in answer to every serial-number request: no ONU-ID yet, the
 * serial number, GEM transport supported and the transmit power level the ONU
 * sends at, high for level mode 0 and lower by one for each mode below. The
 * random delay is left 0; that the MAC inserts one and writes it into the
 * message is an assumption, not verified. Nothing is armed until a serial
 * number is set.
 */
static void rtl9602c_gpon_arm_sn(struct rtl9602c_priv *p)
{
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2] = {};

	if (!p->gpon_have_sn)
		return;

	msg[0] = RTL9602C_ONU_ONUID_UNASSIGNED;
	msg[1] = RTL9602C_PLOAM_US_SERIAL_NUMBER_ONU;
	memcpy(&msg[RTL9602C_PLOAM_SN_ONU_SERIAL], p->gpon_sn, RTL9602C_GPON_SN_BYTES);

	msg[RTL9602C_PLOAM_SN_ONU_FLAGS] = RTL9602C_PLOAM_SN_ONU_GEM |
		FIELD_PREP(RTL9602C_PLOAM_SN_ONU_POWER_LEVEL,
			   RTL9602C_PLOAM_SN_ONU_POWER_LEVEL_HIGH - p->gpon_power_mode);

	rtl9602c_ploam_enqueue(p, msg, RTL9602C_PLOAM_TX_CTRL_SLOT_SN_ONU);
}

/*
 * Send Dying_Gasp before an orderly restart, power-off or halt, while the ONU
 * holds an ONU-ID and sends, in O4 and O5: G.984.3 has the ONU send it, at
 * least three times, when it is about to change its powering conditions, so
 * that the OLT does not report the loss of the ONU that follows (clause 9.2.2
 * and clause 9.2.4.3). Each copy goes on the urgent queue and is given
 * RTL9602C_GPON_DG_WAIT_MS to leave it, in the ONU's next allocation that
 * carries a PLOAM message; the copies sent are logged.
 */
static int rtl9602c_gpon_reboot(struct notifier_block *nb, unsigned long action,
				void *data)
{
	struct rtl9602c_priv *p = container_of(nb, struct rtl9602c_priv,
					       gpon_reboot_nb);
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2] = {};
	unsigned int sent = 0;
	u32 ctrl;

	mutex_lock(&p->gpon_lock);
	if (p->gpon_up && (p->gpon_state == RTL9602C_ONU_O4_RANGING ||
			   p->gpon_state == RTL9602C_ONU_O5_OPERATION)) {
		msg[0] = p->gpon_onu_id;
		msg[1] = RTL9602C_PLOAM_US_DYING_GASP;
		while (sent < RTL9602C_GPON_DG_COPIES &&
		       !rtl9602c_ploam_enqueue(p, msg, RTL9602C_PLOAM_TX_CTRL_SLOT_URGENT) &&
		       !read_poll_timeout(rtl9602c_r, ctrl,
					  ctrl & RTL9602C_PLOAM_TX_CTRL_URGENT_EMPTY,
					  USEC_PER_MSEC, RTL9602C_GPON_DG_WAIT_MS * USEC_PER_MSEC,
					  false, p, RTL9602C_PLOAM_TX_CTRL))
			sent++;
		pr_info("<GPON> Dying_Gasp sent %u of %u times before the restart\n",
			sent, RTL9602C_GPON_DG_COPIES);
	}
	mutex_unlock(&p->gpon_lock);
	return NOTIFY_DONE;
}

/**
 * rtl9602c_gpon_reboot_register() - send Dying_Gasp before every restart
 * @p: the switch
 *
 * Return: 0, or the error of registering the notifier.
 */
int rtl9602c_gpon_reboot_register(struct rtl9602c_priv *p)
{
	p->gpon_reboot_nb.notifier_call = rtl9602c_gpon_reboot;
	return devm_register_reboot_notifier(p->dev, &p->gpon_reboot_nb);
}

/**
 * rtl9602c_gpon_power_applied() - take what the optics set
 * @p: the switch
 * @asked: the transmit power level mode optics_level_work set out to set
 * @mode: the mode the optics are at now
 * @err: 0, or why @asked was not set
 *
 * On a failure the ONU stays at @mode, unless a newer mode has been asked
 * for meanwhile, which the next run of the work sets; the failure is logged.
 * The Serial_Number_ONU template is armed again with the mode.
 *
 * Context: process context; takes gpon_lock.
 */
void rtl9602c_gpon_power_applied(struct rtl9602c_priv *p, u8 asked, u8 mode,
				 int err)
{
	mutex_lock(&p->gpon_lock);
	if (err && p->gpon_power_mode == asked) {
		pr_warn("<GPON> transmit power level mode %u not set (%d); the ONU stays at mode %u\n",
			asked, err, mode);
		p->gpon_power_mode = mode;
	}
	rtl9602c_gpon_arm_sn(p);
	mutex_unlock(&p->gpon_lock);
}

/*
 * Read-modify-write an upstream GTC register under the write key: a write to
 * RTL9602C_GTC_US_CTRL without the key is dropped, with no error (measured on
 * this board). Which other registers the key guards is not verified; the
 * transmit mode, the burst power save and the response delay are written under
 * it too, since a key that is not needed costs nothing.
 */
static void rtl9602c_gpon_us_locked_rmw(struct rtl9602c_priv *p, u32 off,
					u32 clr, u32 set)
{
	rtl9602c_w(p, RTL9602C_GTC_US_WRITE_KEY, RTL9602C_GTC_US_WRITE_KEY_UNLOCK);
	rtl9602c_rmw(p, off, clr, set);
	rtl9602c_w(p, RTL9602C_GTC_US_WRITE_KEY, RTL9602C_GTC_US_WRITE_KEY_LOCK);
}

/*
 * Arm the No_message and Dying_Gasp templates. Every allocation that asks for a
 * PLOAMu carries a message, No_message when nothing else waits, so that
 * template must hold a well-formed one; until it is armed it can hold leftover
 * bytes after a correct header (measured on this board). What makes the MAC
 * send the Dying_Gasp template is not known; nothing in the driver does.
 */
static void rtl9602c_gpon_arm_idle_slots(struct rtl9602c_priv *p)
{
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2] = {};

	msg[0] = RTL9602C_ONU_ONUID_UNASSIGNED;
	msg[1] = RTL9602C_PLOAM_US_NO_MESSAGE;
	rtl9602c_ploam_enqueue(p, msg, RTL9602C_PLOAM_TX_CTRL_SLOT_NO_MESSAGE);

	msg[1] = RTL9602C_PLOAM_US_DYING_GASP;
	rtl9602c_ploam_enqueue(p, msg, RTL9602C_PLOAM_TX_CTRL_SLOT_DYING_GASP);
}

static int rtl9602c_gpon_alloc_unbind(struct rtl9602c_priv *p);
static void rtl9602c_gpon_o5_teardown(struct rtl9602c_priv *p);
static void rtl9602c_gpon_aes_reset(struct rtl9602c_priv *p);

/**
 * rtl9602c_gpon_operstate() - make the pon interface dormant outside O5
 * @p: the switch
 *
 * The interface of the PON port is dormant, up but waiting for an external
 * event as RFC 2863 defines it, in every activation state but O5, where the ONU
 * carries traffic. Called on every change of state and when the port is
 * enabled; does nothing while the port has no user interface. That the
 * interface then shows dormant below O5 and up in O5 is not verified.
 */
void rtl9602c_gpon_operstate(struct rtl9602c_priv *p)
{
	struct dsa_port *dp = dsa_to_port(&p->ds, RTL9602C_PORT_PON);

	if (!dp || !dp->user)
		return;

	if (READ_ONCE(p->gpon_state) == RTL9602C_ONU_O5_OPERATION)
		netif_dormant_off(dp->user);
	else
		netif_dormant_on(dp->user);
}

/**
 * rtl9602c_gpon_cntr_sync() - add the GTC counters into their totals
 * @p: the switch
 *
 * The totals since bring-up are what the gpon and ponpath views and the PON
 * class report. Every counter from 0x701180 to 0x7011fc clears when read
 * (measured on this board), so this is the one reader of those it sums, and
 * everyone else reads the totals, brought up to date by a call. The worker
 * calls it every RTL9602C_GPON_POLL_MS, well within the 8 s in which the
 * bandwidth-map counter, taken to be 16 bits wide (not verified), wraps at
 * 8000 maps a second. Nothing is read while the PON is down.
 *
 * The serial-number request, PLOAM and bandwidth-map counts are measured on
 * this board; the ranging-request, error, overflow, BIP and GEM frame counts
 * are taken to be what their names say, which is not verified.
 *
 * Context: gpon_lock held.
 */
void rtl9602c_gpon_cntr_sync(struct rtl9602c_priv *p)
{
	struct pon_dev_stats *s = &p->gpon_stats;
	u32 active, ploam_fail, bwm_fail;

	lockdep_assert_held(&p->gpon_lock);

	if (!p->gpon_up)
		return;

	active = rtl9602c_r(p, RTL9602C_GPON_CNT_ACTIVATION);
	s->sn_requests += FIELD_GET(RTL9602C_GPON_CNT_ACTIVATION_SN, active);
	s->ranging_requests += FIELD_GET(RTL9602C_GPON_CNT_ACTIVATION_RANGING, active);

	s->ploam_rx += rtl9602c_r(p, RTL9602C_GPON_CNT_PLOAM_RX);
	ploam_fail = rtl9602c_r(p, RTL9602C_GPON_CNT_PLOAM_ERR);
	s->ploam_crc_errors += FIELD_GET(RTL9602C_GPON_CNT_PLOAM_ERR_CRC, ploam_fail);
	s->ploam_overflows += FIELD_GET(RTL9602C_GPON_CNT_PLOAM_ERR_OVERFLOW, ploam_fail);

	s->bwmap_rx += rtl9602c_r(p, RTL9602C_GPON_CNT_BWMAP_RX);
	bwm_fail = rtl9602c_r(p, RTL9602C_GPON_CNT_BWMAP_ERR);
	s->bwmap_crc_errors += FIELD_GET(RTL9602C_GPON_CNT_BWMAP_ERR_CRC, bwm_fail);
	s->bwmap_overflows += FIELD_GET(RTL9602C_GPON_CNT_BWMAP_ERR_OVERFLOW, bwm_fail);

	s->gem_frames += rtl9602c_r(p, RTL9602C_GPON_CNT_GEM_FRAMES);
	s->bip_errors += rtl9602c_r(p, RTL9602C_GPON_CNT_BIP_ERR);
}

/**
 * rtl9602c_gpon_fill_status() - build the ONU's status for the PON class
 * @p: the switch
 * @st: the status; cleared first
 *
 * Always: type GPON, the ifindex of the pon interface and, once one is set, the
 * serial number. The rest only while the PON is up, since while it is down
 * there is no state, alarm or count to report, and absent is not zero: the
 * activation state, whose numbers 1 to 5 are the class's O1 to O5; the ONU-ID
 * unless unassigned; the OMCC's Port-ID while it is bound; the Alloc-IDs of
 * T-CONTs 0 to 15 in use, the default Alloc-ID not among them; the counter
 * totals, brought up to date; and the alarms.
 *
 * The alarms are the levels in RTL9602C_GTC_DS_IRQ_STAT, the conditions present
 * now: loss of signal, loss of frame, and the MAC's LOM bit, reported as loss
 * of PLOAM although what that bit detects is not verified. The FEC bit there
 * says that the downstream carries FEC and is no alarm. The PEE alarm is the
 * driver's own, raised by Physical_Equipment_Error.
 *
 * Context: gpon_lock held.
 */
void rtl9602c_gpon_fill_status(struct rtl9602c_priv *p,
			       struct pon_dev_status *st)
{
	struct dsa_port *dp = dsa_to_port(&p->ds, RTL9602C_PORT_PON);
	unsigned int i;

	lockdep_assert_held(&p->gpon_lock);

	memset(st, 0, sizeof(*st));
	st->type = PON_TYPE_GPON;
	st->onu_id = -1;
	st->omcc_port_id = -1;
	if (dp && dp->user)
		st->ifindex = dp->user->ifindex;
	if (p->gpon_have_sn) {
		memcpy(st->serial_number, p->gpon_sn, sizeof(st->serial_number));
		st->have_serial = true;
	}

	if (!p->gpon_up)
		return;

	st->onu_state = p->gpon_state;
	if (p->gpon_onu_id != RTL9602C_ONU_ONUID_UNASSIGNED)
		st->onu_id = p->gpon_onu_id;
	if (p->gpon_omcc_bound)
		st->omcc_port_id = p->gpon_omcc_port;
	for (i = 0; i < RTL9602C_GPON_DEFAULT_TCONT_IDX; i++)
		if (p->gpon_tcont_used & BIT(i))
			st->alloc_id[st->num_alloc_ids++] = p->gpon_alloc[i];

	rtl9602c_gpon_cntr_sync(p);
	st->stats = p->gpon_stats;
	st->have_stats = true;

	i = rtl9602c_r(p, RTL9602C_GTC_DS_IRQ_STAT);
	st->have_alarms = true;
	if (i & RTL9602C_GTC_DS_IRQ_LOS)
		st->alarms |= PON_ALARM_LOS;
	if (i & RTL9602C_GTC_DS_IRQ_LOF)
		st->alarms |= PON_ALARM_LOF;
	if (i & RTL9602C_GTC_DS_IRQ_LOM)
		st->alarms |= PON_ALARM_LOM;
	if (p->gpon_pee)
		st->alarms |= PON_ALARM_PEE;
}

/*
 * Send the PON class the ONU's status when it differs from the last one sent,
 * counters left out of the comparison: counters moving is no change, and a
 * steady O5 sends nothing.
 */
static void rtl9602c_gpon_notify(struct rtl9602c_priv *p)
{
	struct pon_dev_status st, key;

	lockdep_assert_held(&p->gpon_lock);

	if (!p->pon)
		return;

	rtl9602c_gpon_fill_status(p, &st);

	key = st;
	memset(&key.stats, 0, sizeof(key.stats));
	if (!memcmp(&key, &p->gpon_status, sizeof(key)))
		return;

	p->gpon_status = key;
	pon_device_notify(p->pon, &st);
}

/*
 * Move the activation state machine to @state. The MAC takes its state from
 * RTL9602C_ONU_CTRL_STATE and classifies the OLT's grants by it: in O1 it
 * counts no serial-number request, in O3 it takes the discovery grants
 * (measured on this board). The transition is logged last, once the state, the
 * ONU-ID and TX_DISABLE are written, since a console line takes milliseconds;
 * leaving O5 logs how long the session lasted.
 */
static void rtl9602c_gpon_apply_state(struct rtl9602c_priv *p, u8 state)
{
	static const char * const name[] = {
		[RTL9602C_ONU_O1_INITIAL]	= "O1 initial",
		[RTL9602C_ONU_O2_STANDBY]	= "O2 standby",
		[RTL9602C_ONU_O3_SERIAL_NUMBER]	= "O3 serial-number",
		[RTL9602C_ONU_O4_RANGING]	= "O4 ranging",
		[RTL9602C_ONU_O5_OPERATION]	= "O5 operation",
		[RTL9602C_ONU_O6_POPUP]		= "O6 popup",
		[RTL9602C_ONU_O7_EMERGENCY_STOP] = "O7 emergency stop",
	};
	u8 from;

	if (p->gpon_state == state)
		return;

	from = p->gpon_state;
	WRITE_ONCE(p->gpon_state, state);
	rtl9602c_gpon_operstate(p);
	rtl9602c_ploam_log(p, RTL9602C_EV_STATE, NULL, from);

	/*
	 * TO1 bounds the serial-number and ranging states together (G.984.3
	 * clause 10.1): it starts on entering O3, runs on through O4, and the
	 * worker expires it; ranging again after a broadcast POPUP starts it
	 * too. Any other state stops it. TO2 bounds O6 the same way.
	 */
	if (state == RTL9602C_ONU_O3_SERIAL_NUMBER ||
	    (state == RTL9602C_ONU_O4_RANGING && from == RTL9602C_ONU_O6_POPUP))
		p->gpon_to1 = jiffies + msecs_to_jiffies(RTL9602C_GPON_TO1_MS);
	else if (state != RTL9602C_ONU_O4_RANGING)
		p->gpon_to1 = 0;
	p->gpon_to2 = state == RTL9602C_ONU_O6_POPUP ?
		      jiffies + msecs_to_jiffies(RTL9602C_GPON_TO2_MS) : 0;

	rtl9602c_rmw(p, RTL9602C_ONU_CTRL, RTL9602C_ONU_CTRL_STATE,
		     FIELD_PREP(RTL9602C_ONU_CTRL_STATE, state));

	/*
	 * Below O4, and in O7, the ONU holds no ONU-ID, and G.984.3 has it
	 * discard its Alloc-IDs and OMCI Port-ID with it (clause 9.2.1); O6
	 * keeps them for a directed POPUP. An ONU-ID left in
	 * RTL9602C_ONU_US_ONUID keeps discovery from ever succeeding again
	 * (measured on this board); which upstream field that register feeds is
	 * not verified.
	 */
	if ((state < RTL9602C_ONU_O4_RANGING ||
	     state == RTL9602C_ONU_O7_EMERGENCY_STOP) &&
	    p->gpon_onu_id != RTL9602C_ONU_ONUID_UNASSIGNED) {
		p->gpon_onu_id = RTL9602C_ONU_ONUID_UNASSIGNED;
		p->gpon_rei_every = 0;
		rtl9602c_gpon_aes_reset(p);
		rtl9602c_w(p, RTL9602C_ONU_US_ONUID,
			   FIELD_PREP(RTL9602C_ONU_CTRL_ONUID,
				      RTL9602C_ONU_ONUID_UNASSIGNED));
		rtl9602c_rmw(p, RTL9602C_ONU_CTRL,
			     RTL9602C_ONU_CTRL_ONUID,
			     FIELD_PREP(RTL9602C_ONU_CTRL_ONUID,
					RTL9602C_ONU_ONUID_UNASSIGNED));
		rtl9602c_gpon_alloc_unbind(p);
		rtl9602c_gpon_o5_teardown(p);
	}

	/*
	 * Light only from O3 to O5, where the ONU transmits, and there only in
	 * the allocations the OLT grants: not below O3, nor in O6, where the
	 * downstream was lost, nor in O7, where the OLT stopped the ONU.
	 */
	rtl9602c_l_txdis_gpio(p, state < RTL9602C_ONU_O3_SERIAL_NUMBER ||
			      state > RTL9602C_ONU_O5_OPERATION);

	if (state == RTL9602C_ONU_O5_OPERATION) {
		p->gpon_o5_since = jiffies;
		p->gpon_rei_sent = 0;
		p->gpon_pst_seen = false;
	}
	/* In O1 there is no downstream, so nothing that keeps PEE raised. */
	if (state == RTL9602C_ONU_O1_INITIAL)
		p->gpon_pee = false;
	if (from == RTL9602C_ONU_O5_OPERATION) {
		unsigned int ms = jiffies_to_msecs(jiffies - p->gpon_o5_since);

		pr_info("<GPON> %s -> %s after %u.%03u s in O5\n", name[from],
			name[state] ?: "?", ms / 1000, ms % 1000);
		return;
	}

	pr_info("<GPON> %s -> %s\n", name[from] ?: "?", name[state] ?: "?");
}

/* Change the state, then report the ONU's status to the PON class. */
static void rtl9602c_gpon_set_state(struct rtl9602c_priv *p, u8 state)
{
	rtl9602c_gpon_apply_state(p, state);
	rtl9602c_gpon_notify(p);
}

/*
 * Follow the downstream's synchronisation (G.984.3 clause 10.2): with LOS or
 * LOF present, an ONU in O2 to O4 goes back to O1, and one in O5 to O6, where
 * it keeps its ranging and sends nothing until POPUP or TO2; with both clear,
 * one in O1 goes on to O2. O6 and O7 wait for the OLT. The conditions are read
 * as levels, so a call after a missed change still finds the state right.
 */
static void rtl9602c_gpon_ds_sync(struct rtl9602c_priv *p)
{
	bool lost;

	lockdep_assert_held(&p->gpon_lock);

	if (!p->gpon_up)
		return;

	lost = rtl9602c_r(p, RTL9602C_GTC_DS_IRQ_STAT) &
	       (RTL9602C_GTC_DS_IRQ_LOS | RTL9602C_GTC_DS_IRQ_LOF);
	switch (p->gpon_state) {
	case RTL9602C_ONU_O1_INITIAL:
		if (!lost)
			rtl9602c_gpon_set_state(p, RTL9602C_ONU_O2_STANDBY);
		break;
	case RTL9602C_ONU_O2_STANDBY:
	case RTL9602C_ONU_O3_SERIAL_NUMBER:
	case RTL9602C_ONU_O4_RANGING:
		if (lost)
			rtl9602c_gpon_set_state(p, RTL9602C_ONU_O1_INITIAL);
		break;
	case RTL9602C_ONU_O5_OPERATION:
		if (lost)
			rtl9602c_gpon_set_state(p, RTL9602C_ONU_O6_POPUP);
		break;
	default:
		break;
	}
}

/*
 * Write or clear entry @idx of the Alloc-ID or Port-ID table, through its
 * command register @ind and write-data register @wr. The command names the
 * operation and the entry before the data is written, since data written first
 * goes to another entry (measured on this board); the command is then repeated
 * with EXEC, and DONE polled for about 100 us.
 */
static int rtl9602c_gpon_tbl_op(struct rtl9602c_priv *p, u32 ind, u32 wr,
				unsigned int mode, unsigned int idx, u16 data)
{
	u32 cmd = FIELD_PREP(RTL9602C_GPON_TBL_CMD_OP, mode) |
		  FIELD_PREP(RTL9602C_GPON_TBL_CMD_IDX, idx);
	int i;

	rtl9602c_w(p, ind, cmd);
	rtl9602c_w(p, wr, FIELD_PREP(RTL9602C_GPON_TBL_DATA_ID, data));
	rtl9602c_w(p, ind, RTL9602C_GPON_TBL_CMD_EXEC | cmd);
	for (i = 0; i < 100; i++) {
		if (rtl9602c_r(p, ind) & RTL9602C_GPON_TBL_CMD_DONE)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/*
 * The default Alloc-ID equals the ONU-ID (G.984.3 clause 5.5.3) and goes into
 * T-CONT 16, whose allocations carry the upstream PLOAMs and the OMCC. It is
 * bound with the ONU-ID and cleared when the ONU-ID is given back.
 */
static int rtl9602c_gpon_alloc_bind(struct rtl9602c_priv *p, u8 onu_id)
{
	return rtl9602c_gpon_tbl_op(p, RTL9602C_ALLOCID_TBL_CMD,
				    RTL9602C_ALLOCID_TBL_WDATA,
				    RTL9602C_GPON_TBL_CMD_OP_WRITE,
				    RTL9602C_GPON_DEFAULT_TCONT_IDX, onu_id);
}

static int rtl9602c_gpon_alloc_unbind(struct rtl9602c_priv *p)
{
	return rtl9602c_gpon_tbl_op(p, RTL9602C_ALLOCID_TBL_CMD,
				    RTL9602C_ALLOCID_TBL_WDATA,
				    RTL9602C_GPON_TBL_CMD_OP_CLEAR,
				    RTL9602C_GPON_DEFAULT_TCONT_IDX, 0);
}

/**
 * rtl9602c_gpon_port_bind() - take a GEM Port-ID into a flow downstream
 * @p: the switch
 * @idx: the flow, below RTL9602C_GPON_NUM_FLOW
 * @port_id: the GEM Port-ID
 *
 * Writes entry @idx of the Port-ID table, the ONU's GEM port filter: the MAC
 * takes the downstream GEM frames that carry @port_id into flow @idx.
 *
 * Context: gpon_lock held, which serializes the table's command and data
 * registers.
 * Return: 0, or -ETIMEDOUT when the table does not complete the write.
 */
int rtl9602c_gpon_port_bind(struct rtl9602c_priv *p, unsigned int idx,
			    u16 port_id)
{
	int err;

	err = rtl9602c_gpon_tbl_op(p, RTL9602C_GEM_DS_PORTID_TBL_CMD,
				   RTL9602C_GEM_DS_PORTID_TBL_WDATA,
				   RTL9602C_GPON_TBL_CMD_OP_WRITE, idx, port_id);
	if (!err)
		p->gpon_flow_port[idx] = port_id;
	return err;
}

/**
 * rtl9602c_gpon_port_unbind() - clear a flow's entry in the Port-ID table
 * @p: the switch
 * @idx: the flow
 *
 * Context: gpon_lock held.
 * Return: 0, or -ETIMEDOUT when the table does not complete the clear.
 */
int rtl9602c_gpon_port_unbind(struct rtl9602c_priv *p, unsigned int idx)
{
	p->gpon_flow_port[idx] = RTL9602C_GPON_PORTID_NONE;
	return rtl9602c_gpon_tbl_op(p, RTL9602C_GEM_DS_PORTID_TBL_CMD,
				    RTL9602C_GEM_DS_PORTID_TBL_WDATA,
				    RTL9602C_GPON_TBL_CMD_OP_CLEAR, idx, 0);
}

/**
 * rtl9602c_gpon_port_aes() - the decryption bit of a Port-ID's flow
 * @p: the switch
 * @port_id: the GEM Port-ID
 *
 * Return: RTL9602C_GEM_DS_FLOW_CFG_AES when the OLT has @port_id encrypted
 * (Encrypted_Port-ID), for the type of the flow bound to it; 0 otherwise.
 *
 * Context: gpon_lock held.
 */
u32 rtl9602c_gpon_port_aes(struct rtl9602c_priv *p, u16 port_id)
{
	lockdep_assert_held(&p->gpon_lock);

	return port_id < RTL9602C_GPON_NUM_PORTID &&
	       test_bit(port_id, p->gpon_aes_ports) ?
	       RTL9602C_GEM_DS_FLOW_CFG_AES : 0;
}

/*
 * Mark Port-ID @port_id encrypted or not, and set or clear the decryption bit
 * of every flow bound to it; a flow bound later takes the bit with its type.
 */
static void rtl9602c_gpon_port_aes_set(struct rtl9602c_priv *p, u16 port_id,
				       bool on)
{
	unsigned int f;

	if (on)
		set_bit(port_id, p->gpon_aes_ports);
	else
		clear_bit(port_id, p->gpon_aes_ports);

	for (f = 0; f < RTL9602C_GPON_NUM_FLOW; f++)
		if (p->gpon_flow_port[f] == port_id)
			rtl9602c_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
				     RTL9602C_GEM_DS_FLOW_CFG_AES,
				     on ? RTL9602C_GEM_DS_FLOW_CFG_AES : 0);
}

/* Forget every encrypted Port-ID, and clear the bit of every flow. */
static void rtl9602c_gpon_aes_reset(struct rtl9602c_priv *p)
{
	unsigned int f;

	bitmap_zero(p->gpon_aes_ports, RTL9602C_GPON_NUM_PORTID);
	for (f = 0; f < RTL9602C_GPON_NUM_FLOW; f++)
		if (p->gpon_flow_port[f] != RTL9602C_GPON_PORTID_NONE)
			rtl9602c_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
				     RTL9602C_GEM_DS_FLOW_CFG_AES, 0);
}

/*
 * Read entry @idx of the Alloc-ID or Port-ID table, through its command
 * register @ind and read-data register @rd: @hit says whether the entry is in
 * use and @data holds its identifier. -ETIMEDOUT when the table does not answer
 * within about 100 us.
 */
static int rtl9602c_gpon_tbl_read(struct rtl9602c_priv *p, u32 ind, u32 rd,
				  unsigned int idx, u16 *data, bool *hit)
{
	u32 cmd = FIELD_PREP(RTL9602C_GPON_TBL_CMD_OP,
			     RTL9602C_GPON_TBL_CMD_OP_READ) |
		  FIELD_PREP(RTL9602C_GPON_TBL_CMD_IDX, idx);
	u32 v;
	int i;

	rtl9602c_w(p, ind, cmd);
	rtl9602c_w(p, ind, RTL9602C_GPON_TBL_CMD_EXEC | cmd);
	for (i = 0; i < 100; i++) {
		v = rtl9602c_r(p, ind);
		if (v & RTL9602C_GPON_TBL_CMD_DONE) {
			*hit = v & RTL9602C_GPON_TBL_CMD_VALID;
			*data = rtl9602c_r(p, rd) & RTL9602C_GPON_TBL_DATA_ID;
			return 0;
		}
		udelay(1);
	}
	return -ETIMEDOUT;
}

/**
 * rtl9602c_gpon_port_get() - read a flow's entry in the Port-ID table
 * @p: the switch
 * @flow: the flow
 * @port_id: set to the entry's GEM Port-ID
 * @valid: set to whether the entry is in use
 *
 * The table keeps its entries through the MAC reset and the watchdog's chip
 * reset, so an entry says what was written into it, not what the OLT has
 * configured.
 *
 * Context: gpon_lock held.
 * Return: 0, or -ETIMEDOUT when the table does not complete the read.
 */
int rtl9602c_gpon_port_get(struct rtl9602c_priv *p, unsigned int flow,
			   u16 *port_id, bool *valid)
{
	return rtl9602c_gpon_tbl_read(p, RTL9602C_GEM_DS_PORTID_TBL_CMD,
				      RTL9602C_GEM_DS_PORTID_TBL_RDATA, flow, port_id,
				      valid);
}

/**
 * rtl9602c_gpon_flow_cntr() - read a flow's downstream GEM counter
 * @p: the switch
 * @flow: the flow
 * @bytes: true for the byte count, false for the frame count
 *
 * The counters count what the flow receives downstream only: upstream traffic
 * leaves them at zero (measured on this board). Whether a read clears them is
 * not verified.
 *
 * Return: the count, or U32_MAX when the MAC does not answer within 1 ms.
 */
u32 rtl9602c_gpon_flow_cntr(struct rtl9602c_priv *p, unsigned int flow,
			    bool bytes)
{
	u32 v;

	rtl9602c_w(p, RTL9602C_GEM_DS_CNT_CMD,
		   FIELD_PREP(RTL9602C_GEM_DS_CNT_CMD_FLOW, flow) |
		   (bytes ? RTL9602C_GEM_DS_CNT_CMD_BYTES : 0));
	if (read_poll_timeout(rtl9602c_r, v, v & RTL9602C_GEM_DS_CNT_CMD_DONE,
			      0, 1000, false, p, RTL9602C_GEM_DS_CNT_CMD))
		return U32_MAX;
	return rtl9602c_r(p, RTL9602C_GEM_DS_CNT_DATA);
}

/*
 * Read or clear entry @idx of the downstream multicast filter table. A read
 * says only whether the entry is in use, in @hit when that is given; the driver
 * writes no entry.
 */
static int rtl9602c_gpon_mc_op(struct rtl9602c_priv *p, unsigned int mode,
			       unsigned int idx, bool *hit)
{
	u32 cmd = FIELD_PREP(RTL9602C_GPON_TBL_CMD_OP, mode) |
		  FIELD_PREP(RTL9602C_GEM_DS_MC_TBL_CMD_IDX, idx);
	u32 v;
	int i;

	rtl9602c_w(p, RTL9602C_GEM_DS_MC_TBL_CMD, cmd);
	rtl9602c_w(p, RTL9602C_GEM_DS_MC_TBL_CMD, RTL9602C_GPON_TBL_CMD_EXEC | cmd);
	for (i = 0; i < 100; i++) {
		v = rtl9602c_r(p, RTL9602C_GEM_DS_MC_TBL_CMD);
		if (v & RTL9602C_GPON_TBL_CMD_DONE) {
			if (hit)
				*hit = v & RTL9602C_GPON_TBL_CMD_VALID;
			return 0;
		}
		udelay(1);
	}
	return -ETIMEDOUT;
}

/*
 * Empty the Alloc-ID, Port-ID and multicast tables, and each flow's downstream
 * type. The tables are RAM that neither the MAC reset nor the watchdog's chip
 * reset clears, and what an earlier session left in them breaks the next: the
 * OMCC's flow then matches no OMCI frame, and every upstream grant goes out as
 * an empty burst (measured on this board). Why stale Port-ID entries of other
 * flows stop the OMCC's flow from matching is not known; emptying every entry
 * does not depend on it.
 *
 * Entries found in use are counted and logged, the Alloc-IDs with their
 * T-CONTs. The driver's record of T-CONTs 0 to 15 is emptied with the table,
 * and the OMCC is marked unbound, which takes the OMCI channel down. Returns 0,
 * or the error of the first entry that does not complete.
 */
static int rtl9602c_gpon_tables_clear(struct rtl9602c_priv *p)
{
	unsigned int i, allocs = 0, ports = 0, mcs = 0;
	char seen[96] = "";
	size_t n = 0;
	bool hit;
	u16 v;
	int err;

	for (i = 0; i < RTL9602C_GPON_NUM_TCONT; i++) {
		if (!rtl9602c_gpon_tbl_read(p, RTL9602C_ALLOCID_TBL_CMD,
					    RTL9602C_ALLOCID_TBL_RDATA, i, &v,
					    &hit) && hit) {
			n += scnprintf(seen + n, sizeof(seen) - n, " %u:%u",
				       i, v);
			allocs++;
		}
		err = rtl9602c_gpon_tbl_op(p, RTL9602C_ALLOCID_TBL_CMD,
					   RTL9602C_ALLOCID_TBL_WDATA,
					   RTL9602C_GPON_TBL_CMD_OP_CLEAR, i, 0);
		if (err)
			goto out;
	}

	for (i = 0; i < RTL9602C_GPON_NUM_FLOW; i++) {
		if (!rtl9602c_gpon_tbl_read(p, RTL9602C_GEM_DS_PORTID_TBL_CMD,
					    RTL9602C_GEM_DS_PORTID_TBL_RDATA, i, &v,
					    &hit) && hit)
			ports++;
		err = rtl9602c_gpon_port_unbind(p, i);
		if (err)
			goto out;
		rtl9602c_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(i),
			     RTL9602C_GEM_DS_FLOW_CFG_TYPE, 0);
	}

	for (i = 0; i < RTL9602C_GPON_NUM_MC; i++) {
		if (!rtl9602c_gpon_mc_op(p, RTL9602C_GPON_TBL_CMD_OP_READ, i,
					 &hit) && hit)
			mcs++;
		err = rtl9602c_gpon_mc_op(p, RTL9602C_GPON_TBL_CMD_OP_CLEAR, i,
					  NULL);
		if (err)
			goto out;
	}

	p->gpon_tcont_used = 0;
	WRITE_ONCE(p->gpon_omcc_bound, false);
	if (p->omci_port)
		pon_omci_port_link_set(p->omci_port, false);

	if (allocs || ports || mcs)
		pr_info("<GPON> tables emptied: %u Alloc-IDs (%s), %u Port-IDs, %u multicast\n",
			allocs, allocs ? seen : " none", ports, mcs);
	return 0;

out:
	pr_err("<GPON> table clear stopped at entry %u: %d\n", i, err);
	return err;
}

/*
 * Bind the OMCC, the ONU's OMCI channel, to GEM Port-ID @port_id: flow
 * RTL9602C_GPON_OMCC_FLOW takes the Port-ID from the downstream and hands its
 * frames on as OMCI, its upstream GEM frames carry the Port-ID, and its stream
 * is admitted into the PON MAC. The OMCI port's link goes up only then.
 */
static int rtl9602c_gpon_omcc_bind(struct rtl9602c_priv *p, u16 port_id)
{
	unsigned int f = RTL9602C_GPON_OMCC_FLOW;
	int err;

	err = rtl9602c_gpon_port_bind(p, f, port_id);
	if (err)
		return err;

	rtl9602c_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
		     RTL9602C_GEM_DS_FLOW_CFG_TYPE,
		     RTL9602C_GEM_DS_FLOW_CFG_TYPE_OMCI |
		     rtl9602c_gpon_port_aes(p, port_id));
	rtl9602c_rmw(p, RTL9602C_GEM_US_FLOW_CFG(f), RTL9602C_GEM_US_FLOW_CFG_PORTID,
		     FIELD_PREP(RTL9602C_GEM_US_FLOW_CFG_PORTID, port_id));
	rtl9602c_ponmac_sid_valid(p, f, true);

	p->gpon_omcc_port = port_id;
	WRITE_ONCE(p->gpon_omcc_bound, true);
	if (p->omci_port)
		pon_omci_port_link_set(p->omci_port, true);
	return 0;
}

/*
 * Undo rtl9602c_gpon_omcc_bind(). The OMCI port's link goes down first, which
 * drops the messages still waiting for the reader.
 */
static void rtl9602c_gpon_omcc_unbind(struct rtl9602c_priv *p)
{
	unsigned int f = RTL9602C_GPON_OMCC_FLOW;

	WRITE_ONCE(p->gpon_omcc_bound, false);
	if (p->omci_port)
		pon_omci_port_link_set(p->omci_port, false);

	rtl9602c_gpon_port_unbind(p, f);
	rtl9602c_rmw(p, RTL9602C_GEM_DS_FLOW_CFG(f),
		     RTL9602C_GEM_DS_FLOW_CFG_TYPE, 0);
	rtl9602c_rmw(p, RTL9602C_GEM_US_FLOW_CFG(f), RTL9602C_GEM_US_FLOW_CFG_PORTID,
		     0);
	rtl9602c_ponmac_sid_valid(p, f, false);
}

/*
 * Bind Alloc-ID @alloc into the lowest idle T-CONT of those the GEM ports use
 * (rtl9602c_gem_tcont()), unless a T-CONT holds it already, as it does for
 * the repeated copies of an Assign_Alloc-ID. The driver keeps its own record
 * of what each T-CONT holds, since the table keeps its entries through a
 * reset and so tells what was written, not what the OLT still assigns. The
 * GEM ports on the T-CONT then get their upstream.
 */
static void rtl9602c_gpon_tcont_add(struct rtl9602c_priv *p, u16 alloc)
{
	unsigned int n, t;

	for (t = 0; t < RTL9602C_GPON_DEFAULT_TCONT_IDX; t++)
		if ((p->gpon_tcont_used & BIT(t)) && p->gpon_alloc[t] == alloc)
			return;
	for (n = 0; n < RTL9602C_GEM_NUM_TCONT; n++)
		if (!(p->gpon_tcont_used & BIT(rtl9602c_gem_tcont(n))))
			break;
	if (n == RTL9602C_GEM_NUM_TCONT) {
		pr_warn("<GPON> no idle T-CONT for Alloc-ID %u\n", alloc);
		return;
	}
	t = rtl9602c_gem_tcont(n);
	if (rtl9602c_gpon_tbl_op(p, RTL9602C_ALLOCID_TBL_CMD,
				 RTL9602C_ALLOCID_TBL_WDATA,
				 RTL9602C_GPON_TBL_CMD_OP_WRITE, t, alloc)) {
		pr_warn("<GPON> T-CONT %u bind did not complete\n", t);
		return;
	}
	p->gpon_alloc[t] = alloc;
	p->gpon_tcont_used |= BIT(t);
	pr_debug("<GPON> Alloc-ID %u in T-CONT %u (GEM T-CONT %u)\n", alloc,
		 t, n);

	rtl9602c_gem_tcont_ready(p, n);
}

/* Unbind Alloc-ID @alloc from every T-CONT from 0 to 15 that holds it. */
static void rtl9602c_gpon_tcont_del(struct rtl9602c_priv *p, u16 alloc)
{
	unsigned int i;

	for (i = 0; i < RTL9602C_GPON_DEFAULT_TCONT_IDX; i++) {
		if (!(p->gpon_tcont_used & BIT(i)) || p->gpon_alloc[i] != alloc)
			continue;
		rtl9602c_gpon_tbl_op(p, RTL9602C_ALLOCID_TBL_CMD,
				     RTL9602C_ALLOCID_TBL_WDATA,
				     RTL9602C_GPON_TBL_CMD_OP_CLEAR, i, 0);
		p->gpon_tcont_used &= ~BIT(i);
	}
}

/*
 * Unbind what the OLT assigned after the ONU-ID: the Alloc-IDs in T-CONTs 0 to
 * 15, and the OMCC.
 */
static void rtl9602c_gpon_o5_teardown(struct rtl9602c_priv *p)
{
	unsigned int i;

	for (i = 0; i < RTL9602C_GPON_DEFAULT_TCONT_IDX; i++)
		if (p->gpon_tcont_used & BIT(i))
			rtl9602c_gpon_tbl_op(p, RTL9602C_ALLOCID_TBL_CMD,
					     RTL9602C_ALLOCID_TBL_WDATA,
					     RTL9602C_GPON_TBL_CMD_OP_CLEAR, i, 0);
	p->gpon_tcont_used = 0;
	if (p->gpon_omcc_bound)
		rtl9602c_gpon_omcc_unbind(p);
}

/*
 * Downstream message names by identifier, as G.984.3 clause 9.2.3 and its
 * annexes give them; 2 and 7 are deprecated.
 */
static const char *rtl9602c_ploam_ds_name(u8 id)
{
	static const char * const names[] = {
		[1]  = "Upstream_Overhead",	[2]  = "Serial_number_mask",
		[3]  = "Assign_ONU-ID",		[4]  = "Ranging_Time",
		[5]  = "Deactivate_ONU-ID",	[6]  = "Disable_serial_number",
		[7]  = "Configure_VP/VC",	[8]  = "Encrypted_Port-ID",
		[9]  = "Request_password",	[10] = "Assign_Alloc-ID",
		[11] = "No_message",		[12] = "POPUP",
		[13] = "Request_Key",		[14] = "Configure_Port-ID",
		[15] = "Physical_Equipment_Error", [16] = "Change_Power_Level",
		[17] = "PST",			[18] = "BER_Interval",
		[19] = "Key_switching_time",	[20] = "Extended_Burst_Length",
		[21] = "PON-ID",		[22] = "Swift_POPUP",
		[23] = "Ranging_adjustment",	[24] = "Sleep_Allow",
	};

	return id < ARRAY_SIZE(names) && names[id] ? names[id] : "unknown";
}

/*
 * Upstream message names by identifier, as G.984.3 clause 9.2.4 and its annex E
 * give them.
 */
static const char *rtl9602c_ploam_us_name(u8 id)
{
	static const char * const names[] = {
		[1] = "Serial_Number_ONU",	[2] = "Password",
		[3] = "Dying_Gasp",		[4] = "No_message",
		[5] = "Encryption_Key",		[6] = "Physical_Equipment_Error",
		[7] = "PST",			[8] = "REI",
		[9] = "Acknowledge",		[10] = "Sleep_Request",
	};

	return id < ARRAY_SIZE(names) && names[id] ? names[id] : "unknown";
}

/*
 * Acknowledge downstream message @dm (G.984.3 clause 9.2.4.9) on the urgent
 * queue: its identifier, then its first nine octets. Unacknowledged,
 * Assign_Alloc-ID and Configure_Port-ID are sent three times and then followed
 * by Deactivate_ONU-ID (measured on this board).
 */
static void rtl9602c_gpon_ploam_ack(struct rtl9602c_priv *p, const u8 *dm)
{
	u8 ack[RTL9602C_GPON_PLOAM_WORDS * 2] = {};

	ack[0] = p->gpon_onu_id;
	ack[1] = RTL9602C_PLOAM_US_ACKNOWLEDGE;
	ack[2] = dm[1];
	memcpy(&ack[3], dm, 9);
	rtl9602c_ploam_enqueue(p, ack, RTL9602C_PLOAM_TX_CTRL_SLOT_URGENT);
}

/*
 * Answer Request_Password (G.984.3 clause 9.2.3.9) with the Password message
 * (clause 9.2.4.2) three times, on the normal queue: the ONU-ID, the
 * identifier, then the ten octets of the password. Without a password set
 * the request is not answered, and the OLT decides what that means.
 */
static void rtl9602c_gpon_password_send(struct rtl9602c_priv *p)
{
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2] = {};
	unsigned int i;

	if (!p->gpon_have_password) {
		dev_warn_ratelimited(p->dev,
				     "<GPON> the OLT asks for the password, and none is set\n");
		return;
	}

	msg[0] = p->gpon_onu_id;
	msg[1] = RTL9602C_PLOAM_US_PASSWORD;
	memcpy(&msg[2], p->gpon_password, RTL9602C_GPON_PASSWORD_BYTES);
	for (i = 0; i < 3; i++)
		rtl9602c_ploam_enqueue(p, msg, RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL);
	memzero_explicit(msg, sizeof(msg));
	pr_debug("<GPON> password sent\n");
}

/*
 * Start the REI reports of BER_Interval (G.984.3 clause 9.2.3.18), one every
 * @frames downstream frames, counting the BIP errors from now; 0 stops them.
 * The worker sends them (rtl9602c_gpon_rei_tick()), so an interval is kept to
 * within its period.
 */
static void rtl9602c_gpon_rei_start(struct rtl9602c_priv *p, u32 frames)
{
	unsigned int ms = frames / RTL9602C_GPON_DS_FRAMES_PER_MS;

	rtl9602c_gpon_cntr_sync(p);
	p->gpon_rei_bip = p->gpon_stats.bip_errors;
	p->gpon_rei_seq = 0;
	p->gpon_rei_every = ms ? max(msecs_to_jiffies(ms), 1UL) : 0;
	p->gpon_rei_next = jiffies + p->gpon_rei_every;
	pr_info("<GPON> BER interval %u ms: %s\n", ms,
		ms ? "an REI every interval" : "no REI");
}

/*
 * Send REI (G.984.3 clause 9.2.4.8) once a BER interval has passed, in O5: the
 * BIP errors since the last one, and the sequence number, which counts the
 * reports modulo 16. A report that falls more than an interval behind starts
 * the cadence again from now. Context: gpon_lock held, the counters just
 * taken.
 */
static void rtl9602c_gpon_rei_tick(struct rtl9602c_priv *p)
{
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2] = {};
	u64 errors;

	if (!p->gpon_rei_every || p->gpon_state != RTL9602C_ONU_O5_OPERATION ||
	    time_before(jiffies, p->gpon_rei_next))
		return;

	p->gpon_rei_next += p->gpon_rei_every;
	if (time_after_eq(jiffies, p->gpon_rei_next))
		p->gpon_rei_next = jiffies + p->gpon_rei_every;

	errors = p->gpon_stats.bip_errors - p->gpon_rei_bip;
	p->gpon_rei_bip = p->gpon_stats.bip_errors;

	msg[0] = p->gpon_onu_id;
	msg[1] = RTL9602C_PLOAM_US_REI;
	put_unaligned_be32(min_t(u64, errors, U32_MAX),
			   &msg[RTL9602C_PLOAM_REI_ERRORS]);
	msg[RTL9602C_PLOAM_REI_SEQ] = p->gpon_rei_seq &
				      RTL9602C_PLOAM_REI_SEQ_MASK;
	p->gpon_rei_seq++;
	rtl9602c_ploam_enqueue(p, msg, RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL);
	p->gpon_rei_sent++;
}

/*
 * Write @key as the next key of the downstream decryption, which the MAC makes
 * the active one at the superframe of Key_Switching_Time. Returns 0, or
 * -ETIMEDOUT when a word is not taken.
 */
static int rtl9602c_gpon_key_write(struct rtl9602c_priv *p, const u8 *key)
{
	unsigned int i, spin;

	rtl9602c_w(p, RTL9602C_GPON_AES_KEY_CFG, 0);
	rtl9602c_w(p, RTL9602C_GPON_AES_KEY_CFG, RTL9602C_GPON_AES_KEY_CFG_REQ);

	for (i = 0; i < RTL9602C_GPON_AES_KEY_WORDS; i++) {
		u32 idx = FIELD_PREP(RTL9602C_GPON_AES_KEY_WORD_IDX, i);

		rtl9602c_w(p, RTL9602C_GPON_AES_KEY_DATA,
			   key[2 * i] << 8 | key[2 * i + 1]);
		rtl9602c_w(p, RTL9602C_GPON_AES_KEY_WORD, idx);
		rtl9602c_w(p, RTL9602C_GPON_AES_KEY_WORD,
			   idx | RTL9602C_GPON_AES_KEY_WORD_REQ);
		for (spin = 0; spin < 100; spin++) {
			if (rtl9602c_r(p, RTL9602C_GPON_AES_KEY_WORD) &
			    RTL9602C_GPON_AES_KEY_WORD_DONE)
				break;
			udelay(1);
		}
		if (spin == 100)
			return -ETIMEDOUT;
	}
	rtl9602c_w(p, RTL9602C_GPON_AES_KEY_DATA, 0);
	return 0;
}

/*
 * Answer Request_Key (G.984.3 clause 9.2.3.13): a new random key, under the
 * next index, sent as two Encryption_Key fragments three times over on the
 * normal queue, and written as the next key. The key is not kept, and the
 * ploamlog ring holds the fragments zeroed.
 */
static void rtl9602c_gpon_key_send(struct rtl9602c_priv *p)
{
	u8 key[RTL9602C_GPON_AES_KEY_BYTES];
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2] = {};
	unsigned int i, frag;

	get_random_bytes(key, sizeof(key));
	p->gpon_key_index++;

	msg[0] = p->gpon_onu_id;
	msg[1] = RTL9602C_PLOAM_US_ENCRYPTION_KEY;
	msg[RTL9602C_PLOAM_KEY_INDEX] = p->gpon_key_index;
	for (i = 0; i < 3; i++) {
		for (frag = 0; frag < 2; frag++) {
			msg[RTL9602C_PLOAM_KEY_FRAG] = frag;
			memcpy(&msg[RTL9602C_PLOAM_KEY_BYTES],
			       &key[frag * RTL9602C_GPON_AES_FRAG_BYTES],
			       RTL9602C_GPON_AES_FRAG_BYTES);
			rtl9602c_ploam_enqueue(p, msg,
					       RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL);
		}
	}

	if (rtl9602c_gpon_key_write(p, key))
		pr_warn("<GPON> key %u was not taken by the MAC\n",
			p->gpon_key_index);
	else
		pr_info("<GPON> key %u sent\n", p->gpon_key_index);
	memzero_explicit(key, sizeof(key));
	memzero_explicit(msg, sizeof(msg));
}

/*
 * Capture the next @entries allocations the MAC matches to this ONU's T-CONTs,
 * two words each in RTL9602C_BWMAP_CAPTURE_DATA(): the control register is
 * zeroed, cleared with the count, then started with it.
 */
static void rtl9602c_gpon_bwm_arm(struct rtl9602c_priv *p, unsigned int entries)
{
	u32 n = FIELD_PREP(RTL9602C_BWMAP_CAPTURE_CTRL_COUNT, entries);

	rtl9602c_w(p, RTL9602C_BWMAP_CAPTURE_CTRL, 0);
	rtl9602c_w(p, RTL9602C_BWMAP_CAPTURE_CTRL, RTL9602C_BWMAP_CAPTURE_CTRL_CLEAR | n);
	rtl9602c_w(p, RTL9602C_BWMAP_CAPTURE_CTRL, RTL9602C_BWMAP_CAPTURE_CTRL_START | n);
}

static void rtl9602c_gpon_bwm_read(struct rtl9602c_priv *p, u32 *buf,
				   unsigned int entries)
{
	unsigned int i;

	for (i = 0; i < entries * 2; i++)
		buf[i] = rtl9602c_r(p, RTL9602C_BWMAP_CAPTURE_DATA(i));
}

/*
 * Act on a downstream PLOAM message. A message counts only when it is broadcast
 * or directed to this ONU's ONU-ID, and each type only in the states noted
 * below; other types are dropped.
 *
 * G.984.3 gives the ONU 750 us to act on a message, and a console line takes
 * about 9 ms at 115200 baud, so the per-message trace is at debug level and the
 * information lines come after the writes they describe. The ploamlog ring
 * records every message but PON-ID, which the OLT broadcasts about once a
 * second, and Upstream_Overhead and Extended_Burst_Length outside the one state
 * each is acted on in.
 */
static void rtl9602c_gpon_ploam_rx(struct rtl9602c_priv *p, const u8 *msg)
{
	u8 boh;

	if (msg[1] != RTL9602C_PLOAM_DS_EXTENDED_BURST_LENGTH &&
	    (msg[1] != RTL9602C_PLOAM_DS_UPSTREAM_OVERHEAD ||
	     p->gpon_state == RTL9602C_ONU_O2_STANDBY))
		pr_debug("<GPON> PLOAM rx: onu %u type %u %12ph (state O%u)\n",
			 msg[0], msg[1], msg, p->gpon_state);

	if (msg[1] != RTL9602C_PLOAM_DS_PONID &&
	    (msg[1] != RTL9602C_PLOAM_DS_UPSTREAM_OVERHEAD ||
	     p->gpon_state == RTL9602C_ONU_O2_STANDBY) &&
	    (msg[1] != RTL9602C_PLOAM_DS_EXTENDED_BURST_LENGTH ||
	     p->gpon_state == RTL9602C_ONU_O3_SERIAL_NUMBER))
		rtl9602c_ploam_log(p, RTL9602C_EV_DS, msg, 0);

	if (msg[0] != RTL9602C_ONU_ONUID_UNASSIGNED && msg[0] != p->gpon_onu_id)
		return;

	switch (msg[1]) {
	case RTL9602C_PLOAM_DS_UPSTREAM_OVERHEAD:
		/*
		 * In O2 only: the burst overhead and the delay, the
		 * serial-number template armed again, and O3.
		 */
		if (p->gpon_state != RTL9602C_ONU_O2_STANDBY)
			return;
		rtl9602c_gpon_set_overhead(p, msg);
		/*
		 * Every activation starts at transmit power level mode 0, which
		 * the optics go back to if an earlier session left them lower;
		 * the default mode the OLT names here needs the power measured
		 * in bursts, which come often only from O4 on.
		 */
		p->gpon_power_mode = 0;
		queue_work(system_wq, &p->optics_level_work);
		rtl9602c_gpon_arm_sn(p);
		rtl9602c_gpon_set_state(p, RTL9602C_ONU_O3_SERIAL_NUMBER);
		return;

	case RTL9602C_PLOAM_DS_ASSIGN_ONUID:
		/*
		 * In O3, for this ONU's serial number: the ONU-ID, the default
		 * Alloc-ID and O4. The OLT then sends a ranging request, a
		 * grant to the new ONU-ID, which the MAC answers by itself from
		 * what is written here, within the 750 us G.984.3 allows. The
		 * upstream ONU-ID register takes the ONU-ID at its position in
		 * RTL9602C_ONU_CTRL.
		 */
		if (p->gpon_state == RTL9602C_ONU_O3_SERIAL_NUMBER &&
		    p->gpon_have_sn &&
		    !memcmp(&msg[RTL9602C_PLOAM_ASSIGN_ONUID_SERIAL], p->gpon_sn,
			    RTL9602C_GPON_SN_BYTES)) {
			u64 written;

			p->gpon_onu_id = msg[RTL9602C_PLOAM_ASSIGN_ONUID_ONUID];
			rtl9602c_w(p, RTL9602C_ONU_US_ONUID,
				   FIELD_PREP(RTL9602C_ONU_CTRL_ONUID,
					      p->gpon_onu_id));
			rtl9602c_rmw(p, RTL9602C_ONU_CTRL,
				     RTL9602C_ONU_CTRL_ONUID,
				     FIELD_PREP(RTL9602C_ONU_CTRL_ONUID,
						p->gpon_onu_id));

			if (rtl9602c_gpon_alloc_bind(p, p->gpon_onu_id))
				pr_warn("<GPON> Alloc-ID %u bind did not complete\n",
					p->gpon_onu_id);

			/* The bandwidth-map filter as at bring-up. */
			rtl9602c_rmw(p, RTL9602C_GTC_DS_CTRL,
				     RTL9602C_GTC_DS_CTRL_BWMAP_ACCEPT_ALL |
				     RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID,
				     RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID);

			/*
			 * The time of these writes, for the log below: the
			 * state follows at once, and only then the console
			 * lines, which take milliseconds each.
			 */
			written = ktime_get_ns();
			rtl9602c_gpon_set_state(p, RTL9602C_ONU_O4_RANGING);

			/*
			 * Capture the grants the ONU is ranged on, for the
			 * bwmap view; Ranging_Time reads them back.
			 */
			p->gpon_bwm_o4_valid = false;
			rtl9602c_gpon_bwm_arm(p, RTL9602C_GPON_BWM_O4_ENTRIES);

			/*
			 * How long after the interrupt the writes were done,
			 * against the 750 us G.984.3 allows.
			 */
			if (p->gpon_from_irq)
				pr_info("<GPON> ONU-ID %u assigned; written %llu us after the interrupt\n",
					p->gpon_onu_id,
					div_u64(written - p->gpon_irq_ns, NSEC_PER_USEC));
			else
				pr_info("<GPON> ONU-ID %u assigned; seen by the poll, not the IRQ\n",
					p->gpon_onu_id);

			/*
			 * The serial-number template once more, unchanged; that
			 * this is needed is not verified.
			 */
			rtl9602c_gpon_arm_sn(p);
		}
		return;

	case RTL9602C_PLOAM_DS_RANGING_TIME:
		/*
		 * Directed to this ONU in O4 or O5 (G.984.3 clause 9.2.3.4):
		 * the equalization delay in bits, most significant octet first.
		 * A delay for the protection path is logged and not applied.
		 */
		if ((p->gpon_state != RTL9602C_ONU_O4_RANGING &&
		     p->gpon_state != RTL9602C_ONU_O5_OPERATION) ||
		    msg[0] != p->gpon_onu_id)
			return;

		if (msg[RTL9602C_PLOAM_RANGING_TIME_PATH] &
		    RTL9602C_PLOAM_RANGING_TIME_PATH_PROTECT) {
			pr_info("<GPON> Ranging_Time for the protection path, ignored: %12ph\n",
				msg);
			return;
		}
		{
			u32 eqd = 0;
			unsigned int i;

			for (i = 0; i < RTL9602C_PLOAM_RANGING_TIME_EQD_LEN; i++)
				eqd = (eqd << 8) | msg[RTL9602C_PLOAM_RANGING_TIME_EQD + i];

			/*
			 * The OLT may send the delay again at any time to
			 * correct it; in O5 only a change is written.
			 */
			if (p->gpon_state == RTL9602C_ONU_O5_OPERATION) {
				if (eqd != p->gpon_eqd_delay) {
					pr_info("<GPON> Ranging_Time in O5: eqd %u -> %u\n",
						p->gpon_eqd_delay, eqd);
					rtl9602c_gpon_set_eqd(p, eqd);
				}
				return;
			}

			/*
			 * Ranging is done: keep the grants captured since
			 * Assign_ONU-ID, apply the delay and the overhead
			 * length of the ranged state, and go to O5, which stops
			 * TO1.
			 */
			rtl9602c_gpon_bwm_read(p, p->gpon_bwm_o4,
					       RTL9602C_GPON_BWM_O4_ENTRIES);
			p->gpon_bwm_o4_valid = true;
			rtl9602c_gpon_set_eqd(p, eqd);

			boh = rtl9602c_gpon_boh_len(p, p->gpon_t3_ranged);
			rtl9602c_gpon_set_boh(p, boh);

			/*
			 * A rising edge of the flush bit is taken to empty the
			 * upstream PLOAM queues, so that nothing queued in
			 * discovery goes out ahead of the first acknowledgment;
			 * not verified. The bit is left set.
			 */
			rtl9602c_rmw(p, RTL9602C_PLOAM_TX_CFG,
				     RTL9602C_PLOAM_TX_CFG_FLUSH, 0);
			rtl9602c_rmw(p, RTL9602C_PLOAM_TX_CFG,
				     RTL9602C_PLOAM_TX_CFG_FLUSH,
				     RTL9602C_PLOAM_TX_CFG_FLUSH);
			rtl9602c_gpon_set_state(p, RTL9602C_ONU_O5_OPERATION);
			pr_info("<GPON> Ranging_Time raw: %12ph -- eqd %u\n",
				msg, eqd);
		}
		return;

	case RTL9602C_PLOAM_DS_EXTENDED_BURST_LENGTH:
		/*
		 * Broadcast, in O3 before the first serial-number request
		 * (G.984.3 clause 9.2.3.20): the type 3 preamble lengths before
		 * and after ranging. The first applies at once, the second at
		 * Ranging_Time; a copy that repeats the stored lengths is
		 * ignored, and so is the message once the interrupt has seen a
		 * serial-number request in O3.
		 */
		if (p->gpon_state != RTL9602C_ONU_O3_SERIAL_NUMBER ||
		    msg[0] != RTL9602C_ONU_ONUID_UNASSIGNED || p->gpon_sn_seen)
			return;
		if (p->gpon_ebl_seen &&
		    msg[RTL9602C_PLOAM_EXT_BURST_LEN_PRERANGED] == p->gpon_t3_preranged &&
		    msg[RTL9602C_PLOAM_EXT_BURST_LEN_RANGED] == p->gpon_t3_ranged)
			return;
		p->gpon_ebl_seen = true;
		p->gpon_t3_preranged = msg[RTL9602C_PLOAM_EXT_BURST_LEN_PRERANGED];
		p->gpon_t3_ranged = msg[RTL9602C_PLOAM_EXT_BURST_LEN_RANGED];
		if (rtl9602c_gpon_boh_bytes(p, p->gpon_t3_preranged) >
		    RTL9602C_BURST_CFG_LEN_MAX ||
		    rtl9602c_gpon_boh_bytes(p, p->gpon_t3_ranged) >
		    RTL9602C_BURST_CFG_LEN_MAX)
			pr_warn("<GPON> the OLT asks for more burst overhead than the %u bytes G.984.3 allows, which are sent\n",
				RTL9602C_BURST_CFG_LEN_MAX);
		boh = rtl9602c_gpon_boh_len(p, p->gpon_t3_preranged);
		rtl9602c_gpon_set_boh(p, boh);
		pr_info("<GPON> burst length: %u B pre-ranging, %u after; overhead %u/%u (%12ph)\n",
			p->gpon_t3_preranged, p->gpon_t3_ranged,
			rtl9602c_gpon_boh_len(p, p->gpon_t3_preranged),
			rtl9602c_gpon_boh_len(p, p->gpon_t3_ranged), msg);
		return;

	case RTL9602C_PLOAM_DS_DEACTIVATE_ONUID:
		/*
		 * Directed or broadcast, in O4 to O6 (G.984.3 clause 9.2.3.5):
		 * back to O2, which asserts TX_DISABLE and gives back the
		 * ONU-ID, the Alloc-IDs and the OMCC. The upstream queue flags
		 * are recorded as well, to show whether messages were still
		 * waiting to be sent.
		 */
		if (p->gpon_state < RTL9602C_ONU_O4_RANGING ||
		    p->gpon_state == RTL9602C_ONU_O7_EMERGENCY_STOP)
			return;
		rtl9602c_ploam_log(p, RTL9602C_EV_USQ, NULL,
				   rtl9602c_r(p, RTL9602C_PLOAM_TX_CTRL));
		pr_info("<GPON> the OLT deactivated this ONU in O%u\n",
			p->gpon_state);
		rtl9602c_gpon_set_state(p, RTL9602C_ONU_O2_STANDBY);
		return;

	case RTL9602C_PLOAM_DS_ASSIGN_ALLOCID: {
		/*
		 * Directed, in O4 and O5 (G.984.3 clause 9.2.3.10): a 12-bit
		 * Alloc-ID, bound to a T-CONT for type 1, GEM payload, and
		 * unbound for
		 * type 255; other types change nothing. Every copy is
		 * acknowledged.
		 */
		u16 alloc = (msg[2] << 4) | (msg[3] >> 4);

		if (msg[0] != p->gpon_onu_id ||
		    (p->gpon_state != RTL9602C_ONU_O4_RANGING &&
		     p->gpon_state != RTL9602C_ONU_O5_OPERATION))
			return;
		if (msg[4] == RTL9602C_PLOAM_ASSIGN_ALLOCID_GEM)
			rtl9602c_gpon_tcont_add(p, alloc);
		else if (msg[4] == RTL9602C_PLOAM_ASSIGN_ALLOCID_DEALLOC)
			rtl9602c_gpon_tcont_del(p, alloc);
		rtl9602c_gpon_ploam_ack(p, msg);
		pr_debug("<GPON> Assign_Alloc-ID %u type %u acknowledged\n",
			 alloc, msg[4]);
		return;
	}

	case RTL9602C_PLOAM_DS_DISABLE_SERIAL_NUMBER: {
		/*
		 * Broadcast (G.984.3 clause 9.2.3.6). Disable with this ONU's
		 * serial number: O7, from any state the downstream can reach,
		 * with the laser off and the ONU-ID given back. Enable with
		 * this serial number, or enable for every ONU: an ONU in O7
		 * goes to O2 and is discovered again.
		 */
		bool mine = p->gpon_have_sn &&
			    !memcmp(&msg[RTL9602C_PLOAM_DISABLE_SN_SERIAL],
				    p->gpon_sn, RTL9602C_GPON_SN_BYTES);
		u8 ctrl = msg[RTL9602C_PLOAM_DISABLE_SN_CTRL];

		if (ctrl == RTL9602C_PLOAM_DISABLE_SN_DISABLE && mine &&
		    p->gpon_state != RTL9602C_ONU_O7_EMERGENCY_STOP) {
			rtl9602c_gpon_set_state(p, RTL9602C_ONU_O7_EMERGENCY_STOP);
			pr_info("<GPON> the OLT disabled this ONU\n");
		} else if (p->gpon_state == RTL9602C_ONU_O7_EMERGENCY_STOP &&
			   (ctrl == RTL9602C_PLOAM_DISABLE_SN_ENABLE_ALL ||
			    (ctrl == RTL9602C_PLOAM_DISABLE_SN_ENABLE && mine))) {
			rtl9602c_gpon_set_state(p, RTL9602C_ONU_O2_STANDBY);
			pr_info("<GPON> the OLT enabled this ONU again\n");
		}
		return;
	}

	case RTL9602C_PLOAM_DS_POPUP:
		/*
		 * In O6 (G.984.3 clause 9.2.3.12): a broadcast POPUP sends the
		 * ONU to O4, to be ranged again with the delay it holds, and one
		 * directed to it back to O5.
		 */
		if (p->gpon_state != RTL9602C_ONU_O6_POPUP)
			return;
		rtl9602c_gpon_set_state(p, msg[0] == RTL9602C_ONU_ONUID_UNASSIGNED ?
					   RTL9602C_ONU_O4_RANGING :
					   RTL9602C_ONU_O5_OPERATION);
		return;

	case RTL9602C_PLOAM_DS_SWIFT_POPUP:
		/*
		 * Broadcast, in O6 (G.984.3 clause 9.2.3.22): straight back to
		 * O5, as a POPUP directed to the ONU does.
		 */
		if (p->gpon_state != RTL9602C_ONU_O6_POPUP ||
		    msg[0] != RTL9602C_ONU_ONUID_UNASSIGNED)
			return;
		rtl9602c_gpon_set_state(p, RTL9602C_ONU_O5_OPERATION);
		return;

	case RTL9602C_PLOAM_DS_BER_INTERVAL:
		/*
		 * Directed or broadcast, in O5: the interval of the REI
		 * reports. Every copy is acknowledged.
		 */
		if ((msg[0] != p->gpon_onu_id &&
		     msg[0] != RTL9602C_ONU_ONUID_UNASSIGNED) ||
		    p->gpon_state != RTL9602C_ONU_O5_OPERATION)
			return;
		rtl9602c_gpon_rei_start(p,
					get_unaligned_be32(&msg[RTL9602C_PLOAM_BER_INTERVAL]));
		rtl9602c_gpon_ploam_ack(p, msg);
		return;

	case RTL9602C_PLOAM_DS_RANGING_ADJUSTMENT: {
		/*
		 * Directed or broadcast, in O5 (G.984.3 clause 9.2.3.23): a
		 * change of the equalization delay. A delay that would fall
		 * below 0, or beyond the 7 frames RTL9602C_RANGING_EQD holds
		 * less one for the response delay, is not applied.
		 */
		u32 delta = get_unaligned_be32(&msg[RTL9602C_PLOAM_RANGING_ADJ_DELAY]);
		u32 eqd = p->gpon_eqd_delay;

		if ((msg[0] != p->gpon_onu_id &&
		     msg[0] != RTL9602C_ONU_ONUID_UNASSIGNED) ||
		    p->gpon_state != RTL9602C_ONU_O5_OPERATION)
			return;
		if (msg[RTL9602C_PLOAM_RANGING_ADJ_SIGN] &
		    RTL9602C_PLOAM_RANGING_ADJ_DECREASE)
			eqd = delta <= eqd ? eqd - delta : U32_MAX;
		else
			eqd = delta < U32_MAX - eqd ? eqd + delta : U32_MAX;
		if (eqd >= FIELD_MAX(RTL9602C_RANGING_EQD_FRAMES) *
			   RTL9602C_GPON_EQD_FRAME_BITS) {
			pr_warn("<GPON> Ranging_Adjustment out of range, not applied: %12ph\n",
				msg);
			return;
		}
		pr_info("<GPON> Ranging_Adjustment: eqd %u -> %u\n",
			p->gpon_eqd_delay, eqd);
		rtl9602c_gpon_set_eqd(p, eqd);
		return;
	}

	case RTL9602C_PLOAM_DS_REQUEST_KEY:
		/* Directed, in O5. */
		if (msg[0] != p->gpon_onu_id ||
		    p->gpon_state != RTL9602C_ONU_O5_OPERATION)
			return;
		rtl9602c_gpon_key_send(p);
		return;

	case RTL9602C_PLOAM_DS_KEY_SWITCHING_TIME:
		/*
		 * Directed or broadcast, in O5: the superframe from which the
		 * key last sent applies. Every copy is acknowledged.
		 */
		if ((msg[0] != p->gpon_onu_id &&
		     msg[0] != RTL9602C_ONU_ONUID_UNASSIGNED) ||
		    p->gpon_state != RTL9602C_ONU_O5_OPERATION)
			return;
		p->gpon_key_switch =
			get_unaligned_be32(&msg[RTL9602C_PLOAM_KEY_SWITCH_TIME]) &
			RTL9602C_GPON_AES_SWITCH_TIME_SF;
		rtl9602c_w(p, RTL9602C_GPON_AES_SWITCH_TIME, p->gpon_key_switch);
		rtl9602c_gpon_ploam_ack(p, msg);
		return;

	case RTL9602C_PLOAM_DS_ENCRYPTED_PORTID: {
		/*
		 * Directed, in O4 and O5: a Port-ID's downstream encrypted or
		 * not, for the flow bound to it now and any bound later.
		 * Every copy is acknowledged.
		 */
		u16 port = (msg[RTL9602C_PLOAM_ENC_PORT_HI] << 4) |
			   (msg[RTL9602C_PLOAM_ENC_PORT_LO] >> 4);
		bool on = msg[RTL9602C_PLOAM_ENC_PORT_CTRL] &
			  RTL9602C_PLOAM_ENC_PORT_ENCRYPT;

		if (msg[0] != p->gpon_onu_id ||
		    (p->gpon_state != RTL9602C_ONU_O4_RANGING &&
		     p->gpon_state != RTL9602C_ONU_O5_OPERATION))
			return;
		if (on != test_bit(port, p->gpon_aes_ports))
			pr_info("<GPON> GEM port %u %s\n", port,
				on ? "encrypted" : "no longer encrypted");
		rtl9602c_gpon_port_aes_set(p, port, on);
		rtl9602c_gpon_ploam_ack(p, msg);
		return;
	}

	case RTL9602C_PLOAM_DS_PEE:
		/*
		 * Broadcast, about once a second while the OLT cannot send GEM
		 * frames or OMCC frames (G.984.3 clause 9.2.3.15): the PEE alarm
		 * is raised, and the worker clears it once none has come for 3 s
		 * (clause 11.1.2).
		 */
		p->gpon_pee_last = jiffies;
		if (!p->gpon_pee) {
			p->gpon_pee = true;
			pr_warn("<GPON> the OLT reports a physical equipment error\n");
			rtl9602c_gpon_notify(p);
		}
		return;

	case RTL9602C_PLOAM_DS_PST: {
		/*
		 * Directed or broadcast, about once a second, in O5 (G.984.3
		 * clause 9.2.3.17): the OLT's line number and the K1 and K2
		 * bytes of its protection switching. Each is answered with the
		 * ONU's own PST (clause 9.2.4.7) carrying the same three octets
		 * on the normal queue, so that the OLT finds no link mismatch:
		 * the ONU has one line and no protection switching to act on.
		 * The first PST in O5, and every change, is logged.
		 */
		u8 pst[RTL9602C_GPON_PLOAM_WORDS * 2] = {};
		const u8 *got = &msg[RTL9602C_PLOAM_PST_LINE];

		if (p->gpon_state != RTL9602C_ONU_O5_OPERATION)
			return;
		pst[0] = p->gpon_onu_id;
		pst[1] = RTL9602C_PLOAM_US_PST;
		memcpy(&pst[RTL9602C_PLOAM_PST_LINE], got, RTL9602C_PLOAM_PST_BYTES);
		rtl9602c_ploam_enqueue(p, pst, RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL);
		if (!p->gpon_pst_seen ||
		    memcmp(p->gpon_pst, got, RTL9602C_PLOAM_PST_BYTES)) {
			memcpy(p->gpon_pst, got, RTL9602C_PLOAM_PST_BYTES);
			p->gpon_pst_seen = true;
			pr_info("<GPON> PST: line %u, K1 %02x, K2 %02x; answered in kind\n",
				got[0], got[1], got[2]);
		}
		return;
	}

	case RTL9602C_PLOAM_DS_CHANGE_POWER_LEVEL: {
		/*
		 * Directed or broadcast, in O4 and O5 (G.984.3 clause 9.2.3.16
		 * and clause 10.5.2): one transmit power level mode up or down,
		 * within 0 to 2, which optics_level_work then sets; '00' and
		 * '11' ask for nothing, and neither does a step past either end.
		 */
		u8 ind = msg[RTL9602C_PLOAM_CPL_IND] & RTL9602C_PLOAM_CPL_IND_MASK;
		u8 mode = p->gpon_power_mode;

		if (p->gpon_state != RTL9602C_ONU_O4_RANGING &&
		    p->gpon_state != RTL9602C_ONU_O5_OPERATION)
			return;
		if (ind == RTL9602C_PLOAM_CPL_IND_INCREASE && mode)
			mode--;
		else if (ind == RTL9602C_PLOAM_CPL_IND_DECREASE &&
			 mode < RTL9602C_GPON_POWER_MODE_MAX)
			mode++;
		pr_info("<GPON> Change_Power_Level %s: transmit power level mode %u -> %u\n",
			ind == RTL9602C_PLOAM_CPL_IND_INCREASE ? "up" :
			ind == RTL9602C_PLOAM_CPL_IND_DECREASE ? "down" : "neither",
			p->gpon_power_mode, mode);
		if (mode == p->gpon_power_mode)
			return;
		p->gpon_power_mode = mode;
		queue_work(system_wq, &p->optics_level_work);
		return;
	}

	case RTL9602C_PLOAM_DS_SLEEP_ALLOW:
		/*
		 * Not acted on: power saving, which an ONU ignores unless OMCI
		 * has enabled its power management (G.984.3 Annex E.2.1), as no
		 * OMCI entity of this ONU offers any. Nothing is acknowledged.
		 * It is logged once, the first time one comes for this ONU.
		 */
		if (msg[0] != p->gpon_onu_id &&
		    msg[0] != RTL9602C_ONU_ONUID_UNASSIGNED)
			return;
		if (!(p->gpon_ploam_ignored & BIT(msg[1]))) {
			p->gpon_ploam_ignored |= BIT(msg[1]);
			pr_info("<GPON> %s is not supported, ignored: %12ph\n",
				rtl9602c_ploam_ds_name(msg[1]), msg);
		}
		return;

	case RTL9602C_PLOAM_DS_REQUEST_PASSWORD:
		/* Directed, in O5. */
		if (msg[0] != p->gpon_onu_id ||
		    p->gpon_state != RTL9602C_ONU_O5_OPERATION)
			return;
		rtl9602c_gpon_password_send(p);
		return;

	case RTL9602C_PLOAM_DS_CONFIGURE_PORTID: {
		/*
		 * Directed, in O4 and O5 (G.984.3 clause 9.2.3.14): the OMCC's
		 * 12-bit Port-ID, bound on activation, in place of an earlier one
		 * since an ONU has one OMCI connection, and unbound on
		 * deactivation. Every copy is acknowledged.
		 */
		u16 port = (msg[3] << 4) | (msg[4] >> 4);

		if (msg[0] != p->gpon_onu_id ||
		    (p->gpon_state != RTL9602C_ONU_O4_RANGING &&
		     p->gpon_state != RTL9602C_ONU_O5_OPERATION))
			return;
		if (msg[2] & BIT(0))
			rtl9602c_gpon_omcc_bind(p, port);
		else if (p->gpon_omcc_bound)
			rtl9602c_gpon_omcc_unbind(p);
		rtl9602c_gpon_ploam_ack(p, msg);
		pr_debug("<GPON> OMCC on GEM port %u (%s), acknowledged\n",
			 port, msg[2] & BIT(0) ? "on" : "off");
		return;
	}
	}
}

/**
 * rtl9602c_gpon_work() - the GPON MAC's periodic work
 * @w: the work item, the switch's gpon_work
 *
 * Runs every RTL9602C_GPON_POLL_MS, and goes on after the PON is taken down. It
 * dispatches what the interrupt left in the downstream PLOAM queue, normally
 * nothing; activation cannot rely on it, since a message dispatched this late
 * misses the ranging request that follows Assign_ONU-ID (measured on this
 * board). Once the PON MAC is built it records changes of the upstream PLOAM
 * queues. It expires TO1, takes the counters, and sends the PON class the ONU's
 * status when that has changed.
 */
void rtl9602c_gpon_work(struct work_struct *w)
{
	struct rtl9602c_priv *p =
		container_of(to_delayed_work(w), struct rtl9602c_priv, gpon_work);
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2];
	int n = 0;

	mutex_lock(&p->gpon_lock);
	while (n++ < 32 && rtl9602c_ploam_pop(p, msg))
		rtl9602c_gpon_ploam_rx(p, msg);
	if (p->ponmac_up)
		rtl9602c_ploam_log_usq(p);

	/* A change of LOS or LOF that the interrupt did not report. */
	rtl9602c_gpon_ds_sync(p);

	/* TO1: back to O2, where the next Upstream_Overhead starts over. */
	if (p->gpon_to1 && time_after(jiffies, p->gpon_to1)) {
		pr_info("<GPON> TO1 expired in O%u; trying again\n",
			p->gpon_state);
		rtl9602c_gpon_set_state(p, RTL9602C_ONU_O2_STANDBY);
	}

	/*
	 * TO2: no POPUP came; O1, giving back the ONU-ID, and on to O2 at once
	 * when the downstream is back.
	 */
	if (p->gpon_to2 && time_after(jiffies, p->gpon_to2)) {
		pr_info("<GPON> TO2 expired in O6\n");
		rtl9602c_gpon_set_state(p, RTL9602C_ONU_O1_INITIAL);
		rtl9602c_gpon_ds_sync(p);
	}

	rtl9602c_gpon_cntr_sync(p);
	rtl9602c_gpon_rei_tick(p);

	/* PEE clears once no Physical_Equipment_Error has come for 3 s. */
	if (p->gpon_pee &&
	    time_after(jiffies, p->gpon_pee_last + msecs_to_jiffies(RTL9602C_GPON_PEE_MS))) {
		p->gpon_pee = false;
		pr_info("<GPON> the OLT's physical equipment error has cleared\n");
	}

	rtl9602c_gpon_notify(p);
	mutex_unlock(&p->gpon_lock);

	schedule_delayed_work(&p->gpon_work,
			      msecs_to_jiffies(RTL9602C_GPON_POLL_MS));
}

/**
 * rtl9602c_gpon_irq() - handle the GPON source of the switch interrupt
 * @p: the switch
 *
 * Reads the latch of each GTC sub-block that signals; a read clears the latch
 * and acknowledges its events. Received PLOAM messages are dispatched here, up
 * to 32 at a time, and not left to the worker: after Assign_ONU-ID the OLT
 * sends a ranging request that the MAC answers from registers the message sets,
 * and G.984.3 gives the ONU 750 us for that. From a poll, even one every 4 ms,
 * ranging does not complete (measured on this board).
 *
 * Context: the switch's interrupt thread; takes gpon_lock to dispatch.
 */
void rtl9602c_gpon_irq(struct rtl9602c_priv *p)
{
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2];
	u32 ds;
	int n = 0;

	u32 top = rtl9602c_r(p, RTL9602C_GPON_IRQ_STAT);

	/*
	 * The upstream latch is read whenever the sub-block signals: left
	 * unread, it keeps the source asserted and the thread re-enters without
	 * end (measured on this board). Its two signal-detect bits say that the
	 * optics' signal detect disagreed with a burst or stayed asserted too
	 * long; what exactly they detect is not verified.
	 */
	if (top & RTL9602C_GPON_IRQ_GTC_US) {
		u32 us = rtl9602c_r(p, RTL9602C_GTC_US_IRQ_LATCH);

		if (us)
			rtl9602c_ploam_log(p, RTL9602C_EV_US_INTR, NULL, us);

		if (us & (RTL9602C_GTC_US_IRQ_SD_MISMATCH |
			  RTL9602C_GTC_US_IRQ_SD_TOO_LONG))
			dev_warn_ratelimited(p->dev,
					     "<GPON> signal detect disagrees with the burst (latch %#x)\n",
					     us);
	}

	if (!(top & RTL9602C_GPON_IRQ_GTC_DS))
		return;

	/*
	 * Downstream events other than a received PLOAM are recorded; a change
	 * of LOS or LOF moves the state at once, since in O5 the laser has to
	 * go off without waiting for the worker. A serial-number request in O3
	 * ends the taking of Extended_Burst_Length, before any message popped
	 * below is dispatched.
	 */
	ds = rtl9602c_r(p, RTL9602C_GTC_DS_IRQ_LATCH);
	if (ds & ~RTL9602C_GTC_DS_IRQ_PLOAM_RX)
		rtl9602c_ploam_log(p, RTL9602C_EV_DS_INTR, NULL, ds);
	if (ds & (RTL9602C_GTC_DS_IRQ_LOS | RTL9602C_GTC_DS_IRQ_LOF |
		  RTL9602C_GTC_DS_IRQ_SN_REQ)) {
		mutex_lock(&p->gpon_lock);
		if ((ds & RTL9602C_GTC_DS_IRQ_SN_REQ) &&
		    p->gpon_state == RTL9602C_ONU_O3_SERIAL_NUMBER)
			p->gpon_sn_seen = true;
		if (ds & (RTL9602C_GTC_DS_IRQ_LOS | RTL9602C_GTC_DS_IRQ_LOF))
			rtl9602c_gpon_ds_sync(p);
		mutex_unlock(&p->gpon_lock);
	}
	if (!(ds & RTL9602C_GTC_DS_IRQ_PLOAM_RX))
		return;

	/* gpon_from_irq lets Assign_ONU-ID log which path dispatched it. */
	mutex_lock(&p->gpon_lock);
	p->gpon_from_irq = true;
	while (n++ < 32 && rtl9602c_ploam_pop(p, msg))
		rtl9602c_gpon_ploam_rx(p, msg);
	p->gpon_from_irq = false;
	mutex_unlock(&p->gpon_lock);
}

/*
 * Bring the GPON MAC up and leave the ONU in O2, waiting for Upstream_Overhead.
 * The GPON interrupt is enabled last, so that the first PLOAM message
 * dispatched finds the whole activation path programmed.
 */
static int rtl9602c_gpon_bringup(struct rtl9602c_priv *p)
{
	int err;

	/*
	 * A warm reset restarts the CPU but not the GPON MAC, which stays in
	 * its session, ranged and sending its bursts. The PON MAC cannot be
	 * built beneath such a session, since the drain of the OMCC's queue
	 * does not complete (measured on this board); O1 ends it first.
	 */
	rtl9602c_gpon_set_state(p, RTL9602C_ONU_O1_INITIAL);

	/*
	 * The PON MAC carries every OMCI message. It has no part in activation,
	 * but an ONU that reaches O5 without it answers no OMCI, and the OLT
	 * deactivates it (measured on this board), so a failure ends the
	 * bring-up and the PON stays down. It is built before the SerDes goes
	 * to GPON mode; that this order is needed is not verified.
	 */
	err = rtl9602c_ponmac_init(p);
	if (err) {
		pr_err("PON MAC not built (%d): the PON stays down\n", err);
		return err;
	}

	/* The MAC reset completes only once the SerDes is in GPON mode. */
	rtl9602c_pon_serdes_gpon(p);
	rtl9602c_pon_analog_init(p);

	err = rtl9602c_gpon_reset(p);
	if (err)
		return err;

	/*
	 * Empty the tables, then program the GEM ports again: they live in the
	 * tables, and their interfaces outlive a bring-up.
	 */
	err = rtl9602c_gpon_tables_clear(p);
	if (err)
		return err;
	rtl9602c_gem_restore(p);

	/*
	 * Hold the optics' modulation before the laser is first released in O3;
	 * the hold also puts transmit power level mode 0 back. A failure is
	 * logged and bring-up goes on; whether activation needs the hold is not
	 * verified.
	 */
	p->gpon_power_mode = 0;
	err = rtl9602c_optics_hold_modulation(p);
	if (err)
		pr_warn("GN25L95: modulation not held (%d)\n", err);

	/*
	 * The threshold below which the ends of adjacent bursts are merged; its
	 * unit is not known, and the value is not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_GEM_US_BURST_MERGE,
		     RTL9602C_GEM_US_BURST_MERGE_THR,
		     FIELD_PREP(RTL9602C_GEM_US_BURST_MERGE_THR,
				RTL9602C_GEM_US_BURST_MERGE_THR_DEF));

	/*
	 * The ONU's response delay, under the write key: coarse and fine parts
	 * in units of 16 bytes, 128 bits. Raising the fine part lowers the
	 * delay the OLT assigns in ranging by as much (measured on this board).
	 * The values written add up to 43520 bits, 34.98 us at the upstream
	 * rate, the response time G.984.3 gives the ONU (clause 10.4.1); that
	 * the register realises that time is inferred from this, not verified.
	 */
	rtl9602c_w(p, RTL9602C_GTC_US_WRITE_KEY, RTL9602C_GTC_US_WRITE_KEY_UNLOCK);
	rtl9602c_rmw(p, RTL9602C_GTC_US_RESP_DELAY,
		     RTL9602C_GTC_US_RESP_DELAY_COARSE | RTL9602C_GTC_US_RESP_DELAY_FINE,
		     FIELD_PREP(RTL9602C_GTC_US_RESP_DELAY_COARSE,
				RTL9602C_GTC_US_RESP_DELAY_COARSE_DEF) |
		     FIELD_PREP(RTL9602C_GTC_US_RESP_DELAY_FINE,
				RTL9602C_GTC_US_RESP_DELAY_FINE_DEF));
	rtl9602c_w(p, RTL9602C_GTC_US_WRITE_KEY, RTL9602C_GTC_US_WRITE_KEY_LOCK);

	/*
	 * A pulse every 8000 downstream frames, once a second, at offset 1.
	 * Nothing the driver reports uses it; what consumes it, what the offset
	 * counts and whether it is needed are not known.
	 */
	rtl9602c_rmw(p, RTL9602C_GTC_DS_PPS,
		     RTL9602C_GTC_DS_PPS_PERIOD | RTL9602C_GTC_DS_PPS_OFFSET,
		     FIELD_PREP(RTL9602C_GTC_DS_PPS_PERIOD,
				RTL9602C_GTC_DS_PPS_PERIOD_1S) |
		     FIELD_PREP(RTL9602C_GTC_DS_PPS_OFFSET, 1));

	/*
	 * The signal-detect mismatch limit, 0x50; the rest of the word keeps
	 * its reset value. Not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_GTC_US_SD_THR,
		     RTL9602C_GTC_US_SD_THR_MISMATCH,
		     FIELD_PREP(RTL9602C_GTC_US_SD_THR_MISMATCH,
				RTL9602C_GTC_US_SD_THR_MISMATCH_DEF));

	/*
	 * Let the optical module's LOS input act on the receiver, with the
	 * polarity bit set; both bits reset clear. That the module drives the
	 * pin as signal detect, which would make this the right polarity, and
	 * that either bit is needed are not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_GTC_DS_LOS,
		     RTL9602C_GTC_DS_LOS_OPTIC_EN | RTL9602C_GTC_DS_LOS_OPTIC_POL,
		     RTL9602C_GTC_DS_LOS_OPTIC_EN | RTL9602C_GTC_DS_LOS_OPTIC_POL);

	/*
	 * Downstream FEC detection threshold 0, where it resets to 1; nothing
	 * else of FEC is set up. Not verified.
	 */
	rtl9602c_rmw(p, RTL9602C_GTC_DS_CTRL, RTL9602C_GTC_DS_CTRL_FEC_DET_THR,
		     FIELD_PREP(RTL9602C_GTC_DS_CTRL_FEC_DET_THR,
				RTL9602C_GTC_DS_CTRL_FEC_DET_THR_DEF));

	/*
	 * Keep only the allocations for this ONU, in every state and so for the
	 * discovery grants too. With RTL9602C_GTC_DS_CTRL_BWMAP_ACCEPT_ALL the
	 * MAC takes every allocation as its own, and a queued message leaves
	 * long before the next grant meant for this ONU (measured on this
	 * board).
	 */
	rtl9602c_rmw(p, RTL9602C_GTC_DS_CTRL,
		     RTL9602C_GTC_DS_CTRL_BWMAP_ACCEPT_ALL | RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID,
		     RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID);

	/*
	 * The last GEM fragment of an OMCI message: compare PTI bit 0 only, and
	 * require it set, as in the fragment that ends a user-data frame
	 * (G.984.3 clause 8.3.1). Both fields reset to 5.
	 */
	rtl9602c_rmw(p, RTL9602C_GEM_DS_OMCI_PTI,
		     RTL9602C_GEM_DS_OMCI_PTI_MASK | RTL9602C_GEM_DS_OMCI_PTI_END,
		     FIELD_PREP(RTL9602C_GEM_DS_OMCI_PTI_MASK,
				RTL9602C_GEM_DS_OMCI_PTI_EOF) |
		     FIELD_PREP(RTL9602C_GEM_DS_OMCI_PTI_END,
				RTL9602C_GEM_DS_OMCI_PTI_EOF));

	/*
	 * Let broadcast and unicast GEM frames through downstream and check
	 * their FCS; the multicast table lists frames to exclude, and it is
	 * empty; the IPv4 and IPv6 overrides are off. At reset the filter
	 * includes only what the empty table lists and refuses broadcast and
	 * unicast.
	 */
	rtl9602c_rmw(p, RTL9602C_GEM_DS_MC_FILTER,
		     RTL9602C_GEM_DS_MC_FILTER_IPV6_PASS |
		     RTL9602C_GEM_DS_MC_FILTER_IPV6_DROP |
		     RTL9602C_GEM_DS_MC_FILTER_BCAST_PASS |
		     RTL9602C_GEM_DS_MC_FILTER_UCAST_PASS | RTL9602C_GEM_DS_MC_FILTER_FCS_CHECK |
		     RTL9602C_GEM_DS_MC_FILTER_IPV4_PASS |
		     RTL9602C_GEM_DS_MC_FILTER_IPV4_DROP |
		     RTL9602C_GEM_DS_MC_FILTER_EXCLUDE,
		     RTL9602C_GEM_DS_MC_FILTER_BCAST_PASS |
		     RTL9602C_GEM_DS_MC_FILTER_UCAST_PASS | RTL9602C_GEM_DS_MC_FILTER_FCS_CHECK |
		     RTL9602C_GEM_DS_MC_FILTER_EXCLUDE);

	/*
	 * Every downstream GTC event: a received PLOAM, which the interrupt
	 * dispatches, and the serial-number and ranging requests and the LOM,
	 * FEC, LOF and LOS conditions, which it only records. Then the two
	 * sub-blocks at the top level.
	 */
	rtl9602c_w(p, RTL9602C_GTC_DS_IRQ_EN,
		   RTL9602C_GTC_DS_IRQ_PLOAM_RX | RTL9602C_GTC_DS_IRQ_RANGING_REQ |
		   RTL9602C_GTC_DS_IRQ_SN_REQ | RTL9602C_GTC_DS_IRQ_LOM |
		   RTL9602C_GTC_DS_IRQ_FEC | RTL9602C_GTC_DS_IRQ_LOF |
		   RTL9602C_GTC_DS_IRQ_LOS);
	rtl9602c_w(p, RTL9602C_GPON_IRQ_EN,
		   RTL9602C_GPON_IRQ_GTC_US | RTL9602C_GPON_IRQ_GTC_DS);

	/*
	 * How early the laser is keyed before a burst, and how long it stays
	 * keyed after. Lags below 0x28 break discovery, and the lag resets to
	 * 0x18; leads of 0 and 0x20 make no measurable difference (measured on
	 * this board). The unit of either is not known.
	 */
	rtl9602c_rmw(p, RTL9602C_GTC_US_LASER,
		     RTL9602C_GTC_US_LASER_ON_LEAD | RTL9602C_GTC_US_LASER_OFF_LAG,
		     FIELD_PREP(RTL9602C_GTC_US_LASER_ON_LEAD,
				RTL9602C_GTC_US_LASER_ON_LEAD_DEF) |
		     FIELD_PREP(RTL9602C_GTC_US_LASER_OFF_LAG,
				RTL9602C_GTC_US_LASER_OFF_LAG_DEF));

	/*
	 * Upstream GTC interrupts; which events bits 2, 5 and 7 enable is not
	 * known. Their latch must be read (rtl9602c_gpon_irq()).
	 */
	rtl9602c_w(p, RTL9602C_GTC_US_IRQ_EN, RTL9602C_GTC_US_IRQ_EN_DEF);

	/*
	 * Burst power save, the transmitter enabled only around bursts: in the
	 * GTC, under the write key, and in the GEM framer, with a window of
	 * 0x10 cycles after a burst and 0x100 before it. Not verified.
	 */
	rtl9602c_gpon_us_locked_rmw(p, RTL9602C_GTC_US_POWERSAVE,
				    RTL9602C_GTC_US_POWERSAVE_EN,
				    RTL9602C_GTC_US_POWERSAVE_EN);
	rtl9602c_rmw(p, RTL9602C_GEM_US_POWERSAVE,
		     RTL9602C_GEM_US_POWERSAVE_LAG |
		     RTL9602C_GEM_US_POWERSAVE_LEAD,
		     FIELD_PREP(RTL9602C_GEM_US_POWERSAVE_LAG, 0x10) |
		     FIELD_PREP(RTL9602C_GEM_US_POWERSAVE_LEAD, 0x100));

	/*
	 * The PTI codes the upstream GEM framer puts in its headers: 0, 1, 0
	 * and 1. Which kind of fragment takes which of the four is not known.
	 */
	rtl9602c_rmw(p, RTL9602C_GEM_US_PTI_MAP,
		     RTL9602C_GEM_US_PTI_MAP_3 | RTL9602C_GEM_US_PTI_MAP_2 |
		     RTL9602C_GEM_US_PTI_MAP_1 | RTL9602C_GEM_US_PTI_MAP_0,
		     FIELD_PREP(RTL9602C_GEM_US_PTI_MAP_3, 1) |
		     FIELD_PREP(RTL9602C_GEM_US_PTI_MAP_1, 1));

	/*
	 * Under the write key: flag a waiting normal PLOAM in the burst
	 * header's Ind field, keep the PLOAMu field and the upstream scrambler
	 * on, and set the burst-enable polarity. Which level that polarity
	 * selects, and that the Ind bit does what its use suggests, are not
	 * verified.
	 */
	rtl9602c_gpon_us_locked_rmw(p, RTL9602C_GTC_US_CTRL,
				    RTL9602C_GTC_US_CTRL_IND_NORMAL_PLOAM |
				    RTL9602C_GTC_US_CTRL_PLOAM_DIS |
				    RTL9602C_GTC_US_CTRL_SCRAMBLE_DIS |
				    RTL9602C_GTC_US_CTRL_BEN_POL,
				    RTL9602C_GTC_US_CTRL_IND_NORMAL_PLOAM |
				    RTL9602C_GTC_US_CTRL_BEN_POL);

	/*
	 * Under the write key: a burst whose StartTime is smaller than its
	 * overhead starts in the previous upstream frame, which with StartTime
	 * 16 in every grant is every burst (measured on this board); and the
	 * automatic laser suppression stays on. The first bit resets set, and
	 * is written because the driver depends on it. Both meanings are
	 * inferred from use, not verified, and what the suppression acts on is
	 * not known.
	 */
	rtl9602c_gpon_us_locked_rmw(p, RTL9602C_GTC_US_TX_MODE,
				    RTL9602C_GTC_US_TX_MODE_SMALL_SSTART |
				    RTL9602C_GTC_US_TX_MODE_SUPPRESS_DIS,
				    RTL9602C_GTC_US_TX_MODE_SMALL_SSTART);

	/* The bandwidth-map filter once more. */
	rtl9602c_rmw(p, RTL9602C_GTC_DS_CTRL,
		     RTL9602C_GTC_DS_CTRL_BWMAP_ACCEPT_ALL | RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID,
		     RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID);

	/*
	 * No ONU-ID, and O1 until LOS and LOF have cleared, then O2, to wait
	 * for Upstream_Overhead; on a dark fibre the ONU stays in O1 with the
	 * LOS alarm, and the worker moves it on once the light comes.
	 */
	p->gpon_onu_id = RTL9602C_ONU_ONUID_UNASSIGNED;
	rtl9602c_gpon_ds_sync(p);

	/* The templates, ready before the first discovery grant. */
	rtl9602c_gpon_arm_idle_slots(p);
	rtl9602c_gpon_arm_sn(p);

	/* Last, the GPON source of the switch interrupt; and run the worker. */
	rtl9602c_irq_enable(p, 0, RTL9602C_IRQ_SRC_GPON);
	mod_delayed_work(system_wq, &p->gpon_work, 0);
	return 0;
}

/**
 * rtl9602c_gpon_sn_set() - set the ONU's serial number
 * @p: the switch
 * @s: four printable ASCII characters of Vendor_ID, then eight hex digits of
 *     the vendor-specific serial number (G.984.3 clause 9.2.4.1); characters
 *     after the twelfth are ignored
 *
 * Keeps the serial number, without which the PON cannot be brought up, and
 * writes it into the Serial_Number_ONU template at once. The devlink parameter
 * gpon_serial sets it, and refuses a change while the PON is up.
 *
 * Return: 0, or -EINVAL for a malformed @s.
 */
int rtl9602c_gpon_sn_set(struct rtl9602c_priv *p, const char *s)
{
	u8 sn[RTL9602C_GPON_SN_BYTES];
	unsigned int i;

	if (strlen(s) < 12)
		return -EINVAL;

	for (i = 0; i < 4; i++) {
		if (s[i] < 0x20 || s[i] > 0x7e)
			return -EINVAL;
		sn[i] = s[i];
	}
	for (i = 0; i < 4; i++) {
		int hi = hex_to_bin(s[4 + i * 2]);
		int lo = hex_to_bin(s[5 + i * 2]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		sn[4 + i] = (hi << 4) | lo;
	}

	memcpy(p->gpon_sn, sn, sizeof(sn));
	p->gpon_have_sn = true;
	pr_info("<GPON> serial number set\n");

	rtl9602c_gpon_arm_sn(p);
	return 0;
}

/**
 * rtl9602c_gpon_password_set() - set the password the ONU gives the OLT
 * @p: the switch
 * @s: one to ten printable ASCII characters, padded with zero octets to the
 *     ten of the Password message (G.984.3 clause 9.2.4.2); or "0x" and
 *     twenty hex digits, the ten octets themselves; or "" to forget it
 *
 * The devlink parameter gpon_password sets it, and refuses a change while the
 * PON is up.
 *
 * Return: 0, or -EINVAL for a malformed @s.
 */
int rtl9602c_gpon_password_set(struct rtl9602c_priv *p, const char *s)
{
	u8 pw[RTL9602C_GPON_PASSWORD_BYTES] = {};
	size_t len = strlen(s);
	unsigned int i;

	if (!len) {
		memzero_explicit(p->gpon_password, sizeof(p->gpon_password));
		p->gpon_have_password = false;
		return 0;
	}

	if (len == 2 + 2 * sizeof(pw) && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X')) {
		if (hex2bin(pw, s + 2, sizeof(pw)))
			return -EINVAL;
	} else {
		if (len > sizeof(pw))
			return -EINVAL;
		for (i = 0; i < len; i++) {
			if (s[i] < 0x20 || s[i] > 0x7e)
				return -EINVAL;
			pw[i] = s[i];
		}
	}

	memcpy(p->gpon_password, pw, sizeof(pw));
	memzero_explicit(pw, sizeof(pw));
	p->gpon_have_password = true;
	pr_info("<GPON> password set\n");
	return 0;
}

/**
 * rtl9602c_gpon_start() - bring the PON up
 * @p: the switch
 *
 * Called when the pon interface is opened. Brings the GPON MAC up, building the
 * PON MAC first if it is not built yet, and leaves the ONU in O2 waiting for
 * Upstream_Overhead; the counter totals start again from zero. A PON that is up
 * already is left alone, since a second bring-up would reset the MAC under a
 * live session.
 *
 * Context: process context; takes gpon_lock.
 * Return: 0; -ENXIO when no serial number has been set; the error of the PON
 * MAC's build; -ETIMEDOUT when the MAC does not leave reset or its tables
 * cannot be emptied. On an error the PON stays down.
 */
int rtl9602c_gpon_start(struct rtl9602c_priv *p)
{
	int err = 0;

	mutex_lock(&p->gpon_lock);
	if (p->gpon_up)
		goto out;

	if (!p->gpon_have_sn) {
		pr_err("the PON has no serial number: set one before bringing the interface up\n");
		err = -ENXIO;
		goto out;
	}

	memset(&p->gpon_stats, 0, sizeof(p->gpon_stats));

	err = rtl9602c_gpon_bringup(p);
	if (!err) {
		p->gpon_up = true;
		pr_info("<GPON> MAC brought up\n");
	}
out:
	mutex_unlock(&p->gpon_lock);
	return err;
}

/**
 * rtl9602c_gpon_stop() - take the PON down
 * @p: the switch
 *
 * Called when the pon interface is stopped. The ONU goes to O1, which asserts
 * TX_DISABLE and gives back the ONU-ID, the Alloc-IDs and the OMCC, and the PON
 * counts as down. The MAC is not reset, and its interrupt and the worker stay
 * active; in O1 no message from the OLT is acted on.
 *
 * Context: process context; takes gpon_lock.
 */
void rtl9602c_gpon_stop(struct rtl9602c_priv *p)
{
	mutex_lock(&p->gpon_lock);
	if (p->gpon_up) {
		rtl9602c_gpon_set_state(p, RTL9602C_ONU_O1_INITIAL);
		p->gpon_up = false;
	}
	mutex_unlock(&p->gpon_lock);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * gpon: the downstream and the activation at a glance, from the registers at
 * every read: the top-level interrupt status; the downstream conditions present
 * now, with whether the line carries FEC; the state and ONU-ID the MAC holds;
 * the loss bits of the clock recovery and of the optical module's LOS input,
 * 1 for loss; the PON receiver's signal bit, 1 for a signal, and its sync bit,
 * whose meaning is not known (it reads 0 on a framed downstream); and the
 * superframe counter, which advances 8000 times a second, once per 125 us
 * frame, while the downstream is framed and not at all otherwise. Then the
 * counter totals since bring-up, which a read brings up to date, and the REI
 * reports: the BER interval the OLT set, 0 for none, and the reports sent since
 * the ONU entered O5; and the encryption: the index of the last key sent, 0 for
 * none, the superframe of the last Key_Switching_Time with the register, and
 * how many Port-IDs the OLT has encrypted. No key is shown, and no register is
 * written.
 */
static int rtl9602c_gpon_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int rei_ms, rei_sent, aes_ports;
	u32 key_switch;
	u8 key_index;
	struct pon_dev_stats c;
	u32 sts, onu, los, sig;

	sts = rtl9602c_r(p, RTL9602C_GTC_DS_IRQ_STAT);
	onu = rtl9602c_r(p, RTL9602C_ONU_CTRL);
	los = rtl9602c_r(p, RTL9602C_GTC_DS_LOS);

	seq_printf(s, "top   %08x\n", rtl9602c_r(p, RTL9602C_GPON_IRQ_STAT));
	seq_printf(s, "ds    %08x  los %u lof %u lom %u fec %u\n", sts,
		   !!(sts & RTL9602C_GTC_DS_IRQ_LOS),
		   !!(sts & RTL9602C_GTC_DS_IRQ_LOF),
		   !!(sts & RTL9602C_GTC_DS_IRQ_LOM),
		   !!(sts & RTL9602C_GTC_DS_IRQ_FEC));
	seq_printf(s, "onu   %08x  id %lu state %lu\n", onu,
		   FIELD_GET(RTL9602C_ONU_CTRL_ONUID, onu),
		   FIELD_GET(RTL9602C_ONU_CTRL_STATE, onu));

	seq_printf(s, "los   %08x  cdr %u optical %u\n", los,
		   !!(los & RTL9602C_GTC_DS_LOS_CDR),
		   !!(los & RTL9602C_GTC_DS_LOS_OPTIC));
	sig = rtl9602c_r(p, RTL9602C_SERDES_PON_RX_STAT);
	seq_printf(s, "sig   %08x  signok %u lecsync %u   <- positive logic\n",
		   sig, !!(sig & RTL9602C_SERDES_PON_RX_STAT_SIGNAL),
		   !!(sig & RTL9602C_SERDES_PON_RX_STAT_SYNC));

	seq_printf(s, "frame %08lu  superframes (8000/s when locked)\n",
		   FIELD_GET(RTL9602C_GTC_DS_SUPERFRAME_CNT,
			     rtl9602c_r(p, RTL9602C_GTC_DS_SUPERFRAME)));

	mutex_lock(&p->gpon_lock);
	rtl9602c_gpon_cntr_sync(p);
	c = p->gpon_stats;
	rei_ms = jiffies_to_msecs(p->gpon_rei_every);
	rei_sent = p->gpon_rei_sent;
	key_index = p->gpon_key_index;
	key_switch = p->gpon_key_switch;
	aes_ports = bitmap_weight(p->gpon_aes_ports, RTL9602C_GPON_NUM_PORTID);
	mutex_unlock(&p->gpon_lock);

	seq_printf(s, "cntr  sn-requests %llu ranging-requests %llu   (totals)\n",
		   c.sn_requests, c.ranging_requests);
	seq_printf(s, "ploam %llu accepted, %llu crc err, %llu overflow\n",
		   c.ploam_rx, c.ploam_crc_errors, c.ploam_overflows);
	seq_printf(s, "bwmap %llu accepted, %llu crc err, %llu overflow\n",
		   c.bwmap_rx, c.bwmap_crc_errors, c.bwmap_overflows);
	seq_printf(s, "gem   %llu non-idle frames, %llu BIP error blocks\n",
		   c.gem_frames, c.bip_errors);
	seq_printf(s, "rei   every %u ms, %u sent\n", rei_ms, rei_sent);
	seq_printf(s, "aes   key %u, switch at superframe %u (register %08x), %u Port-IDs encrypted\n",
		   key_index, key_switch,
		   rtl9602c_r(p, RTL9602C_GPON_AES_SWITCH_TIME), aes_ports);
	return 0;
}


/*
 * ploamlog: the last RTL9602C_PLOAM_LOG_LEN events of the PLOAM channel, oldest
 * first, each with its time since boot and the state it came in: downstream
 * messages; upstream messages written, with their slot and the queue flags, or
 * refused; state changes; changes of the upstream queues' empty flags; and the
 * upstream and downstream interrupt latches. A repeated event shows its count.
 * Times come from the kernel's clock, in steps of 10 ns on this board. The
 * ring is copied under its lock and printed from the copy.
 */
static int rtl9602c_ploamlog_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	struct rtl9602c_ploam_ev *log;
	unsigned int next, n, i;
	unsigned long flags;

	log = kmalloc_array(RTL9602C_PLOAM_LOG_LEN, sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;

	spin_lock_irqsave(&p->ploam_log_lock, flags);
	memcpy(log, p->ploam_log, sizeof(p->ploam_log));
	next = p->ploam_log_next;
	spin_unlock_irqrestore(&p->ploam_log_lock, flags);

	n = min(next, RTL9602C_PLOAM_LOG_LEN);
	for (i = next - n; i != next; i++) {
		const struct rtl9602c_ploam_ev *ev = &log[i % RTL9602C_PLOAM_LOG_LEN];
		u64 us = div_u64(ev->ns, NSEC_PER_USEC);
		u32 rem;
		u64 sec = div_u64_rem(us, USEC_PER_SEC, &rem);

		seq_printf(s, "%5llu.%06u O%u ", sec, rem, ev->state);
		switch (ev->kind) {
		case RTL9602C_EV_DS:
			seq_printf(s, "rx  %12ph  %s", ev->msg,
				   rtl9602c_ploam_ds_name(ev->msg[1]));
			break;
		case RTL9602C_EV_US:
		case RTL9602C_EV_US_REFUSED:
			seq_printf(s, "tx  %12ph  %s  slot %u ind %06x%s",
				   ev->msg, rtl9602c_ploam_us_name(ev->msg[1]),
				   ev->aux >> 24, ev->aux & 0xffffff,
				   ev->kind == RTL9602C_EV_US_REFUSED ?
				   "  REFUSED, queue full" : "");
			break;
		case RTL9602C_EV_STATE:
			seq_printf(s, "state O%u -> O%u", ev->aux, ev->state);
			break;
		case RTL9602C_EV_USQ:
			seq_printf(s, "us queues %08x  normal %s  urgent %s",
				   ev->aux,
				   ev->aux & RTL9602C_PLOAM_TX_CTRL_NORMAL_EMPTY ?
				   "empty" : "HOLDING",
				   ev->aux & RTL9602C_PLOAM_TX_CTRL_URGENT_EMPTY ?
				   "empty" : "HOLDING");
			break;
		case RTL9602C_EV_US_INTR:
			seq_printf(s, "us interrupt %08x", ev->aux);
			break;
		case RTL9602C_EV_DS_INTR:
			seq_printf(s, "ds interrupt %08x", ev->aux);
			break;
		}
		if (ev->repeat)
			seq_printf(s, "  (x%u)", ev->repeat + 1);
		seq_putc(s, '\n');
	}
	kfree(log);
	return 0;
}

static int rtl9602c_ploamlog_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_ploamlog_show, inode->i_private);
}

/* Any write empties the log. */
static ssize_t rtl9602c_ploamlog_write(struct file *file,
				       const char __user *buf, size_t count,
				       loff_t *ppos)
{
	struct rtl9602c_priv *p = ((struct seq_file *)file->private_data)->private;
	unsigned long flags;

	spin_lock_irqsave(&p->ploam_log_lock, flags);
	p->ploam_log_next = 0;
	spin_unlock_irqrestore(&p->ploam_log_lock, flags);
	return count;
}

static const struct file_operations rtl9602c_ploamlog_fops = {
	.owner   = THIS_MODULE,
	.open    = rtl9602c_ploamlog_open,
	.read    = seq_read,
	.write   = rtl9602c_ploamlog_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

#if IS_ENABLED(CONFIG_NET_DSA_RTL9602C_PLOAM_INJECT)
/*
 * ploaminject: one downstream PLOAM message per write, the twelve octets before
 * its CRC in hex, spaces allowed between them, handed to the dispatcher under
 * gpon_lock as if the interrupt had popped it, the rest of its buffer zero.
 * Refused while the PON is down.
 */
static ssize_t rtl9602c_ploaminject_write(struct file *file,
					  const char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct rtl9602c_priv *p = file->private_data;
	u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2] = {};
	const unsigned int len = RTL9602C_GPON_PLOAM_BYTES - 1;
	unsigned int n = 0, i;
	char buf[64];
	int err = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	for (i = 0; buf[i]; i++) {
		int hi, lo;

		if (isspace(buf[i]))
			continue;
		hi = hex_to_bin(buf[i]);
		lo = buf[i + 1] ? hex_to_bin(buf[i + 1]) : -1;
		if (hi < 0 || lo < 0 || n == len)
			return -EINVAL;
		msg[n++] = hi << 4 | lo;
		i++;
	}
	if (n != len)
		return -EINVAL;

	mutex_lock(&p->gpon_lock);
	if (p->gpon_up)
		rtl9602c_gpon_ploam_rx(p, msg);
	else
		err = -ENETDOWN;
	mutex_unlock(&p->gpon_lock);
	return err ?: count;
}

static const struct file_operations rtl9602c_ploaminject_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= rtl9602c_ploaminject_write,
};
#endif

/*
 * One captured allocation. StartTime and StopTime count bytes from the start of
 * the upstream frame, StopTime the last one (G.984.3 clause 8.1.3.6), so the
 * allocation is StopTime + 1 - StartTime bytes long; one that starts after it
 * stops, or ends beyond the 19440-byte frame, is marked. The T-CONT, PLOAMu,
 * StartTime and StopTime fields are measured on this board; the frame and
 * multiframe numbers, FEC, DBRu mode and the burst start, end and last flags
 * are taken to be what their names say, which is not verified.
 */
static void rtl9602c_bwm_print(struct seq_file *s, u32 w0, u32 w1)
{
	u32 start = FIELD_GET(RTL9602C_BWMAP_ALLOC_SSTART, w1);
	u32 stop = FIELD_GET(RTL9602C_BWMAP_ALLOC_SSTOP, w1);

	seq_printf(s, " %3lu %2lu %5lu %6u %3u %4lu  %6u %6u %5d  %3u %3u %3u%s\n",
		   FIELD_GET(RTL9602C_BWMAP_ALLOC_FRAME, w0),
		   FIELD_GET(RTL9602C_BWMAP_ALLOC_MULTIFRAME, w0),
		   FIELD_GET(RTL9602C_BWMAP_ALLOC_TCONT, w0),
		   !!(w0 & RTL9602C_BWMAP_ALLOC_PLOAMU), !!(w0 & RTL9602C_BWMAP_ALLOC_FEC),
		   FIELD_GET(RTL9602C_BWMAP_ALLOC_DBRU, w0),
		   start, stop, (int)(stop + 1 - start),
		   !!(w0 & RTL9602C_BWMAP_ALLOC_BURST_START),
		   !!(w0 & RTL9602C_BWMAP_ALLOC_BURST_END),
		   !!(w0 & RTL9602C_BWMAP_ALLOC_LAST),
		   start > stop || stop >= RTL9602C_GPON_EQD_FRAME_BITS / 8 ?
		   "  <- not inside one frame" : "");
}

/* The columns rtl9602c_bwm_print() fills. */
#define RTL9602C_BWM_HEADER \
	" FR# MF TCONT PLOAMu FEC DBRu  SStart  SStop  BLen  SoB EoB LST\n"

/*
 * bwmap: a fresh capture of RTL9602C_GPON_BWM_O4_ENTRIES allocations matched to
 * this ONU's T-CONTs, with the capture status word, whose fields are not known;
 * then the capture kept from ranging, the grants the ONU was ranged on.
 */
static int rtl9602c_bwmap_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int n = RTL9602C_GPON_BWM_O4_ENTRIES;
	u32 o4[RTL9602C_GPON_BWM_O4_ENTRIES * 2];
	unsigned int i, valid = 0;
	bool have_o4;

	mutex_lock(&p->gpon_lock);
	rtl9602c_gpon_bwm_arm(p, n);
	mutex_unlock(&p->gpon_lock);

	/* 5 ms, and one 125 us frame for each allocation captured. */
	usleep_range(5000 + n * 125, 6000 + n * 125);

	mutex_lock(&p->gpon_lock);
	seq_printf(s, "capture of %u entries, sts %08x\n" RTL9602C_BWM_HEADER,
		   n, rtl9602c_r(p, RTL9602C_BWMAP_CAPTURE_STAT));
	for (i = 0; i < n; i++) {
		u32 w0 = rtl9602c_r(p, RTL9602C_BWMAP_CAPTURE_DATA(2 * i));
		u32 w1 = rtl9602c_r(p, RTL9602C_BWMAP_CAPTURE_DATA(2 * i + 1));

		if (!(w0 & RTL9602C_BWMAP_ALLOC_VALID))
			continue;
		rtl9602c_bwm_print(s, w0, w1);
		valid++;
	}
	have_o4 = p->gpon_bwm_o4_valid;
	memcpy(o4, p->gpon_bwm_o4, sizeof(o4));
	mutex_unlock(&p->gpon_lock);
	if (!valid)
		seq_puts(s, " no valid entry\n");

	seq_puts(s, "captured on entering O4:\n");
	if (!have_o4) {
		seq_puts(s, " none\n");
		return 0;
	}
	seq_puts(s, RTL9602C_BWM_HEADER);
	for (i = 0; i < RTL9602C_GPON_BWM_O4_ENTRIES; i++)
		if (o4[2 * i] & RTL9602C_BWMAP_ALLOC_VALID)
			rtl9602c_bwm_print(s, o4[2 * i], o4[2 * i + 1]);
	return 0;
}

static int rtl9602c_bwmap_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_bwmap_show, inode->i_private);
}

static const struct file_operations rtl9602c_bwmap_fops = {
	.owner   = THIS_MODULE,
	.open    = rtl9602c_bwmap_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/*
 * The upstream GEM framer's events: RTL9602C_GEM_US_IRQ_LATCH holds those seen
 * since it was last read, RTL9602C_GEM_US_IRQ_STAT the conditions present now.
 * Only the gemusintr view reads them. What each bit detects beyond the name the
 * view gives it, and that a read clears the latch, are not verified.
 */
#define RTL9602C_GEM_US_IRQ_LATCH	0x706000
#define RTL9602C_GEM_US_IRQ_STAT	0x706008

/*
 * gemusintr: both words, and the name of each of bits 0 to 9 set in either,
 * marked as seen since the last read, present now, or both.
 */
static int rtl9602c_gemusintr_show(struct seq_file *s, void *unused)
{
	static const char * const name[] = {
		"underflow", "overflow", "residue",
		"excess", "burst-over-grant", "pli-error",
		"short-grant", "late-request", "sd-mismatch", "sd-too-long",
	};
	struct rtl9602c_priv *p = s->private;
	u32 dlt = rtl9602c_r(p, RTL9602C_GEM_US_IRQ_LATCH);
	u32 sts = rtl9602c_r(p, RTL9602C_GEM_US_IRQ_STAT);
	unsigned int i;

	seq_printf(s, "latch %08x  status %08x\n", dlt, sts);
	for (i = 0; i < ARRAY_SIZE(name); i++)
		if ((dlt | sts) & BIT(i))
			seq_printf(s, "  %-22s %s%s\n", name[i],
				   dlt & BIT(i) ? "since-last-read " : "",
				   sts & BIT(i) ? "now" : "");
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(rtl9602c_gemusintr);

/*
 * usburst: the upstream burst settings, read back: the laser lead and lag; the
 * upstream control word; burst power save in the GTC, and in the GEM framer
 * with its window and bit 31, shown as tx-disable but not verified; the burst
 * overhead's repeat index and length, and its bytes; and the small-StartTime
 * and suppression bits.
 */
static int rtl9602c_usburst_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int i;
	u32 laser = rtl9602c_r(p, RTL9602C_GTC_US_LASER);
	u32 cfg = rtl9602c_r(p, RTL9602C_GTC_US_CTRL);
	u32 ps = rtl9602c_r(p, RTL9602C_GTC_US_POWERSAVE);
	u32 gps = rtl9602c_r(p, RTL9602C_GEM_US_POWERSAVE);
	u32 boh = rtl9602c_r(p, RTL9602C_BURST_CFG);
	u32 proc = rtl9602c_r(p, RTL9602C_GTC_US_TX_MODE);

	seq_printf(s, "laser         %08x  on-lead %lu  off-lag %lu\n", laser,
		   FIELD_GET(RTL9602C_GTC_US_LASER_ON_LEAD, laser),
		   FIELD_GET(RTL9602C_GTC_US_LASER_OFF_LAG, laser));
	seq_printf(s, "tx ctrl       %08x\n", cfg);
	seq_printf(s, "power save    %08x  %s\n", ps,
		   ps & RTL9602C_GTC_US_POWERSAVE_EN ? "on" : "off");
	seq_printf(s, "gem power sv  %08x  tx-disable %u  lag %lu  lead %lu\n",
		   gps, !!(gps & BIT(31)),
		   FIELD_GET(RTL9602C_GEM_US_POWERSAVE_LAG, gps),
		   FIELD_GET(RTL9602C_GEM_US_POWERSAVE_LEAD, gps));
	seq_printf(s, "burst ovh     %08x  repeat %lu  length %lu\n", boh,
		   FIELD_GET(RTL9602C_BURST_CFG_REPEAT_IDX, boh),
		   FIELD_GET(RTL9602C_BURST_CFG_LEN, boh));
	seq_puts(s, "burst bytes  ");
	for (i = 0; i < RTL9602C_GPON_BOH_BYTES; i++)
		seq_printf(s, " %02lx", FIELD_GET(RTL9602C_BURST_DATA_BYTE,
						  rtl9602c_r(p, RTL9602C_BURST_DATA(i))));
	seq_putc(s, '\n');
	seq_printf(s, "tx mode       %08x  small-sstart %u  suppress-off %u\n",
		   proc, !!(proc & RTL9602C_GTC_US_TX_MODE_SMALL_SSTART),
		   !!(proc & RTL9602C_GTC_US_TX_MODE_SUPPRESS_DIS));
	return 0;
}

static int rtl9602c_usburst_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl9602c_usburst_show, inode->i_private);
}

static const struct file_operations rtl9602c_usburst_fops = {
	.owner   = THIS_MODULE,
	.open    = rtl9602c_usburst_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/*
 * uscntr: the upstream counters, selected one at a time. Index 0, shown as
 * ploam-overhead, counts the burst overheads sent, 2 the PLOAMs sent from the
 * normal and urgent queues and 3 those sent from the templates (measured on
 * this board); 1 and 4 are taken to count DBRu and GEM bytes sent, which is not
 * verified, and 5 to 7 have no name. Whether a read clears them is not known.
 */
static int rtl9602c_uscntr_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int i;

	static const char * const name[RTL9602C_GPON_US_CNTR_COUNT] = {
		"ploam-overhead", "dbru", "ploam-cpu", "ploam-auto",
		"gem-bytes",
	};

	for (i = 0; i < RTL9602C_GPON_US_CNTR_COUNT; i++) {
		rtl9602c_w(p, RTL9602C_GPON_CNT_US_SEL, i);
		seq_printf(s, "[%u] %08x  %s\n", i,
			   rtl9602c_r(p, RTL9602C_GPON_CNT_US_DATA),
			   name[i] ?: "");
	}
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(rtl9602c_uscntr);

DEFINE_SHOW_ATTRIBUTE(rtl9602c_gpon);

/*
 * gpontbl: the GPON MAC's tables as the hardware holds them: the Alloc-ID
 * entries in use, by T-CONT; the Port-ID entries in use, by flow, with the
 * flow's downstream type and upstream Port-ID; and each flow whose downstream
 * frame or byte count is not zero. Those counters count the downstream only
 * and clear when read, so each read shows what came since the last one, of
 * this view or, for the OMCC's flow, of ponpath. Read under gpon_lock, since
 * the PLOAM path writes the tables through the same registers.
 */
static int rtl9602c_gpontbl_show(struct seq_file *s, void *unused)
{
	struct rtl9602c_priv *p = s->private;
	unsigned int f, moved = 0;
	bool hit;
	u16 v;

	mutex_lock(&p->gpon_lock);
	seq_puts(s, "Alloc-ID  (T-CONT:Alloc-ID)");
	for (f = 0; f < RTL9602C_GPON_NUM_TCONT; f++)
		if (!rtl9602c_gpon_tbl_read(p, RTL9602C_ALLOCID_TBL_CMD,
					    RTL9602C_ALLOCID_TBL_RDATA, f, &v,
					    &hit) && hit)
			seq_printf(s, " %u:%u", f, v);

	seq_puts(s, "\nPort-ID   (flow: Port-ID, type, upstream Port-ID)\n");
	for (f = 0; f < RTL9602C_GPON_NUM_FLOW; f++) {
		if (rtl9602c_gpon_port_get(p, f, &v, &hit) || !hit)
			continue;
		seq_printf(s, "  %3u: %4u  %02lx  %4lu\n", f, v,
			   FIELD_GET(RTL9602C_GEM_DS_FLOW_CFG_TYPE,
				     rtl9602c_r(p, RTL9602C_GEM_DS_FLOW_CFG(f))),
			   FIELD_GET(RTL9602C_GEM_US_FLOW_CFG_PORTID,
				     rtl9602c_r(p, RTL9602C_GEM_US_FLOW_CFG(f))));
	}

	seq_puts(s, "DOWNSTREAM GEM counters that moved (flow: frames, bytes)\n");
	for (f = 0; f < RTL9602C_GPON_NUM_FLOW; f++) {
		u32 frames = rtl9602c_gpon_flow_cntr(p, f, false);
		u32 bytes = rtl9602c_gpon_flow_cntr(p, f, true);

		if (frames || bytes) {
			seq_printf(s, "  %3u: %u %u\n", f, frames, bytes);
			moved++;
		}
	}
	if (!moved)
		seq_puts(s, "  none -- and this says nothing about upstream\n");
	mutex_unlock(&p->gpon_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_gpontbl);

/**
 * rtl9602c_gpon_debugfs_init() - create the GPON MAC's debugfs views
 * @p: the switch, with its debugfs directory created
 *
 * Creates gpontbl, gpon, usburst, gemusintr, bwmap and uscntr, mode 0400,
 * ploamlog, mode 0600, since a write to it empties the log, and, with
 * CONFIG_NET_DSA_RTL9602C_PLOAM_INJECT, ploaminject, mode 0200.
 */
void rtl9602c_gpon_debugfs_init(struct rtl9602c_priv *p)
{
	debugfs_create_file("gpontbl", 0400, p->debugfs, p,
			    &rtl9602c_gpontbl_fops);
	debugfs_create_file("gpon", 0400, p->debugfs, p,
			    &rtl9602c_gpon_fops);
	debugfs_create_file("ploamlog", 0600, p->debugfs, p,
			    &rtl9602c_ploamlog_fops);
	debugfs_create_file("usburst", 0400, p->debugfs, p,
			    &rtl9602c_usburst_fops);
	debugfs_create_file("gemusintr", 0400, p->debugfs, p,
			    &rtl9602c_gemusintr_fops);
	debugfs_create_file("bwmap", 0400, p->debugfs, p,
			    &rtl9602c_bwmap_fops);
	debugfs_create_file("uscntr", 0400, p->debugfs, p,
			    &rtl9602c_uscntr_fops);
#if IS_ENABLED(CONFIG_NET_DSA_RTL9602C_PLOAM_INJECT)
	debugfs_create_file("ploaminject", 0200, p->debugfs, p,
			    &rtl9602c_ploaminject_fops);
#endif
}

#endif
