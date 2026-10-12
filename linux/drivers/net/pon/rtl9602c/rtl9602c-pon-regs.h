/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL9602C switch: the registers and constants of the GPON MAC, the
 * GEM ports, the PON buffers and their DMA, and of the T-CONT scheduler.
 *
 * Only the files of the PON MAC use them (rtl9602c-gpon.c, -ponmac.c,
 * -gem.c, -omci.c); they were moved out of rtl9602c.h so that those files
 * can leave the switch driver.
 */

#ifndef _RTL9602C_PON_REGS_H
#define _RTL9602C_PON_REGS_H

/*
 * The GPON MAC's T-CONTs and downstream multicast filter entries. Its flows
 * are RTL9602C_GPON_NUM_FLOW, which the switch driver needs too.
 */
#define RTL9602C_GPON_NUM_TCONT			32
#define RTL9602C_GPON_NUM_MC			256

static inline unsigned int rtl9602c_gem_tcont(unsigned int n)
{
	return 8 * (n / 4) + n % 4;
}

/* The PON SerDes's mode; the GPON MAC's reset completes only in MODE_GPON. */
#define RTL9602C_SERDES_PON_CTRL		0x0001d0
#define RTL9602C_SERDES_PON_CTRL_MODE		GENMASK(4, 0)
#define RTL9602C_SERDES_PON_CTRL_MODE_OFF		0x1f
#define RTL9602C_SERDES_PON_CTRL_MODE_GPON		0x08

/*
 * PON_SERDES resets the PON SerDes, and the driver never clears it; that the
 * reset is needed, and whether it resets the GPON MAC too, are not verified.
 */
#define RTL9602C_CHIP_RESET		0x000104
#define RTL9602C_CHIP_RESET_PON_SERDES		BIT(0)

/*
 * The PON SerDes's reset register, and the burst-enable signal to the optics:
 * BURST_OE drives it, BURST_DLY delays it against the data and D2A_SEL picks
 * the transmitter's data source. None of it is verified to be needed.
 */
#define RTL9602C_SERDES_PON_RESET		0x022034
#define RTL9602C_SERDES_PON_TX_CFG		0x02203c
#define RTL9602C_SERDES_PON_TX_CFG_BURST_DLY	GENMASK(6, 4)
#define RTL9602C_SERDES_PON_TX_CFG_D2A_SEL	GENMASK(3, 0)
#define RTL9602C_SERDES_PON_OPTIC_CTRL		0x022090
#define RTL9602C_SERDES_PON_OPTIC_CTRL_BURST_OE	BIT(12)

/*
 * The PON SerDes's analog front end: tuning words, PLL_CFG the clock
 * multiplier; the clock recovery (CDR), with its analog front end, a manual
 * reset of unknown polarity, the loop filter and a loop setting per line-rate
 * profile; overrides of the receiver enable and the line-rate profile; the
 * burst-enable output level; the transmit amplitude. SD_POR must be set
 * for a module whose loss-of-signal output has the polarity its maker gives
 * it (measured); none of the other settings is verified to be needed.
 */
#define RTL9602C_SERDES_ANA_TUNE2	0x022588
#define RTL9602C_SERDES_ANA_PLL_CFG	0x02258c
#define RTL9602C_SERDES_ANA_TUNE8	0x0225a0
#define RTL9602C_SERDES_ANA_RX_FILTER	0x0225ac
#define RTL9602C_SERDES_ANA_RX_FILTER_CFG	GENMASK(7, 0)
#define RTL9602C_SERDES_ANA_RX_CFG	0x0225b0
#define RTL9602C_SERDES_ANA_RX_CFG_SD_POR	BIT(15)
#define RTL9602C_SERDES_ANA_RX_CFG_CDR_AFE	BIT(14)
#define RTL9602C_SERDES_ANA_CDR_CTRL	0x0225cc
#define RTL9602C_SERDES_ANA_CDR_CTRL_RESET	BIT(14)
#define RTL9602C_SERDES_ANA_CDR_CTRL_LPF_EN	BIT(10)
#define RTL9602C_SERDES_ANA_TUNE24	0x0225e0
#define RTL9602C_SERDES_ANA_TUNE25	0x0225e4
#define RTL9602C_SERDES_ANA_CDR_LOOP_1G25	0x0226b8
#define RTL9602C_SERDES_ANA_CDR_LOOP_EPON	0x0227b8
#define RTL9602C_SERDES_ANA_CDR_LOOP_GPON	0x022738
#define RTL9602C_SERDES_ANA_FORCE_RX	0x022500
#define RTL9602C_SERDES_ANA_FORCE_RATE	0x022504
#define RTL9602C_SERDES_ANA_FORCE_RATE_VAL	GENMASK(7, 5)
#define RTL9602C_SERDES_ANA_FORCE_RATE_EN	BIT(4)
#define RTL9602C_SERDES_ANA_FORCE_RATE_VAL_GPON	1
#define RTL9602C_SERDES_ANA_BURST_OUT	0x022584
#define RTL9602C_SERDES_ANA_BURST_OUT_TTL	BIT(14)
#define RTL9602C_SERDES_ANA_TX_CFG	0x0225d8
#define RTL9602C_SERDES_ANA_TX_CFG_AMP		GENMASK(5, 3)
#define RTL9602C_SERDES_ANA_TX_CFG_AMP_GPON	5
#define RTL9602C_SERDES_PON_PDOWN		0x022038
#define RTL9602C_SERDES_PON_PDOWN_BURST	BIT(10)

/*
 * What the PON bring-up writes; CDR_LOOP_INIT is taken to be integral gain 1
 * and proportional gains 4 and 2 (not verified).
 */
#define RTL9602C_SERDES_ANA_TUNE2_INIT		0x6df8
#define RTL9602C_SERDES_ANA_PLL_CFG_INIT		0x8941
#define RTL9602C_SERDES_ANA_TUNE8_INIT		0x0713
#define RTL9602C_SERDES_ANA_TUNE24_INIT		0x8001
#define RTL9602C_SERDES_ANA_TUNE25_INIT		0x001f
#define RTL9602C_SERDES_ANA_CDR_LOOP_INIT		0x80c5
#define RTL9602C_SERDES_ANA_FORCE_RX_OFF	0x0010
#define RTL9602C_SERDES_ANA_FORCE_RX_ON	0x0030

/*
 * The PON receiver: SIGNAL is set while it detects a signal; what SYNC
 * reports is not known.
 */
#define RTL9602C_SERDES_PON_RX_STAT		0x022a74
#define RTL9602C_SERDES_PON_RX_STAT_SYNC	BIT(9)
#define RTL9602C_SERDES_PON_RX_STAT_SIGNAL	BIT(8)

/* An override of the SerDes's bit-error indication; not verified. */
#define RTL9602C_SERDES_ANA_FORCE_BER	0x022508
#define RTL9602C_SERDES_ANA_FORCE_BER_VAL	BIT(13)
#define RTL9602C_SERDES_ANA_FORCE_BER_EN	BIT(12)

/*
 * Forces a burst enable; set, it does not make the module emit (measured on
 * this board), and what it forces is not known.
 */
#define RTL9602C_SERDES_PON_BURST_FORCE		0x0220e4
#define RTL9602C_SERDES_PON_BURST_FORCE_EN		BIT(0)

/*
 * The threshold below which adjacent upstream bursts are merged; its unit is
 * not known, and the value is not verified.
 */
#define RTL9602C_GEM_US_BURST_MERGE	0x706260
#define RTL9602C_GEM_US_BURST_MERGE_THR	GENMASK(6, 0)
#define RTL9602C_GEM_US_BURST_MERGE_THR_DEF	40

/*
 * The upstream GTC write key: a guarded register drops a write, with no
 * error, unless UNLOCK was written first. RTL9602C_GTC_US_CTRL is guarded
 * (measured on this board); which others are is not verified.
 */
#define RTL9602C_GTC_US_WRITE_KEY	0x705018
#define RTL9602C_GTC_US_WRITE_KEY_UNLOCK	0xcc19
#define RTL9602C_GTC_US_WRITE_KEY_LOCK	0x0000

/*
 * The ONU's response delay, in units of 16 bytes (128 bits). The defaults add
 * up to 34.98 us at 1.24416 Gbit/s, the response time of ITU-T G.984.3 clause
 * 10.4.1; that the register realises that time is not verified.
 */
#define RTL9602C_GTC_US_RESP_DELAY	0x705040
#define RTL9602C_GTC_US_RESP_DELAY_COARSE	GENMASK(15, 7)
#define RTL9602C_GTC_US_RESP_DELAY_FINE	GENMASK(6, 0)
#define RTL9602C_GTC_US_RESP_DELAY_COARSE_DEF	290
#define RTL9602C_GTC_US_RESP_DELAY_FINE_DEF	50

/*
 * A pulse derived from the downstream frames, one a second at PERIOD_1S.
 * Nothing the driver reports uses it; what OFFSET counts is not known.
 */
#define RTL9602C_GTC_DS_PPS	0x70126c
#define RTL9602C_GTC_DS_PPS_OFFSET	GENMASK(31, 26)
#define RTL9602C_GTC_DS_PPS_PERIOD	GENMASK(12, 0)
#define RTL9602C_GTC_DS_PPS_PERIOD_1S	7999

/*
 * Limits on the optics' signal detect against the burst; what they count is
 * not known. The driver writes the mismatch limit only.
 */
#define RTL9602C_GTC_US_SD_THR	0x705188
#define RTL9602C_GTC_US_SD_THR_MISMATCH	GENMASK(30, 16)
#define RTL9602C_GTC_US_SD_THR_MISMATCH_DEF	0x50

/*
 * The GPON MAC's reset: TRIGGER set, then cleared; DONE once it is over. The
 * GTC registers answer only afterwards.
 */
#define RTL9602C_GPON_RESET_CTRL		0x70000c
#define RTL9602C_GPON_RESET_CTRL_DONE	BIT(8)
#define RTL9602C_GPON_RESET_CTRL_TRIGGER	BIT(0)

/* The GPON MAC's interrupt status, with the bits of RTL9602C_GPON_IRQ_EN. */
#define RTL9602C_GPON_IRQ_STAT		0x700044

/*
 * Downstream conditions, as bits of RTL9602C_GTC_DS_IRQ_STAT (present now),
 * _LATCH and _EN. FEC means that the downstream carries FEC; what LOM detects
 * is not verified.
 */
#define RTL9602C_GTC_DS_IRQ_LOM	BIT(3)
#define RTL9602C_GTC_DS_IRQ_FEC	BIT(2)
#define RTL9602C_GTC_DS_IRQ_LOF	BIT(1)
#define RTL9602C_GTC_DS_IRQ_LOS	BIT(0)

/*
 * The activation state, RTL9602C_ONU_O1_INITIAL to _O5_OPERATION, and the
 * ONU-ID, 255 until one is assigned. Software sets the state, and the MAC
 * classifies the OLT's grants by it.
 */
#define RTL9602C_ONU_CTRL	0x701010
#define RTL9602C_ONU_CTRL_ONUID		GENMASK(15, 8)
#define RTL9602C_ONU_CTRL_STATE		GENMASK(3, 0)

/*
 * Downstream loss, 1 for loss: the clock recovery's, and the optical module's
 * LOS input, which OPTIC_EN lets act on the receiver at the level OPTIC_POL
 * selects. The polarity, and the need for either bit, are not verified.
 */
#define RTL9602C_GTC_DS_LOS	0x701040
#define RTL9602C_GTC_DS_LOS_CDR	BIT(10)
#define RTL9602C_GTC_DS_LOS_OPTIC	BIT(8)
#define RTL9602C_GTC_DS_LOS_OPTIC_POL		BIT(1)
#define RTL9602C_GTC_DS_LOS_OPTIC_EN		BIT(0)

/*
 * Downstream control: BWMAP_MATCH_ONUID keeps the allocations for this ONU,
 * BWMAP_ACCEPT_ALL takes all of them as its own; FEC_DET_THR, a FEC
 * detection threshold, is not verified.
 */
#define RTL9602C_GTC_DS_CTRL	0x701014
#define RTL9602C_GTC_DS_CTRL_BWMAP_ACCEPT_ALL	BIT(11)
#define RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID	BIT(10)
#define RTL9602C_GTC_DS_CTRL_FEC_DET_THR	GENMASK(3, 1)
#define RTL9602C_GTC_DS_CTRL_FEC_DET_THR_DEF	0

/*
 * How an OMCI message's last GEM fragment is told: its PTI bits in MASK equal
 * END (ITU-T G.984.3 clause 8.3.1).
 */
#define RTL9602C_GEM_DS_OMCI_PTI		0x701204
#define RTL9602C_GEM_DS_OMCI_PTI_MASK	GENMASK(6, 4)
#define RTL9602C_GEM_DS_OMCI_PTI_END	GENMASK(2, 0)
#define RTL9602C_GEM_DS_OMCI_PTI_EOF	1

/* The GPON MAC's interrupt enable, one bit per GTC sub-block. */
#define RTL9602C_GPON_IRQ_EN		0x700040
#define RTL9602C_GPON_IRQ_GTC_US	BIT(5)
#define RTL9602C_GPON_IRQ_GTC_DS	BIT(1)

/*
 * Downstream GTC events, latched at these bits and at those of the conditions
 * above; a read clears the latch. SN_REQ and RANGING_REQ: a grant taken as a
 * serial-number or a ranging request.
 */
#define RTL9602C_GTC_DS_IRQ_LATCH	0x701000
#define RTL9602C_GTC_DS_IRQ_EN	0x701004
#define RTL9602C_GTC_DS_IRQ_PLOAM_RX	BIT(10)
#define RTL9602C_GTC_DS_IRQ_RANGING_REQ	BIT(9)
#define RTL9602C_GTC_DS_IRQ_SN_REQ	BIT(8)

/*
 * GTC counters, every one from 0x701180 to 0x7011fc clearing when read
 * (measured on this board). The ranging, error, overflow, BIP and GEM frame
 * counts are taken to be what their names say (not verified).
 */
#define RTL9602C_GPON_CNT_ACTIVATION	0x7011ac
#define RTL9602C_GPON_CNT_ACTIVATION_RANGING	GENMASK(31, 16)
#define RTL9602C_GPON_CNT_ACTIVATION_SN	GENMASK(15, 0)

/*
 * The received superframe count, advancing once per 125 us frame while the
 * downstream is framed.
 */
#define RTL9602C_GTC_DS_SUPERFRAME	0x701048
#define RTL9602C_GTC_DS_SUPERFRAME_CNT		GENMASK(29, 0)

/*
 * More GTC counters. BWMAP_RX is taken to be 16 bits wide (not verified);
 * GEM_FRAMES is also RTL9602C_GTC_DS_GEM_NON_IDLE_CNT.
 */
#define RTL9602C_GPON_CNT_PLOAM_RX	0x70119c
#define RTL9602C_GPON_CNT_PLOAM_ERR	0x7011a0
#define RTL9602C_GPON_CNT_PLOAM_ERR_OVERFLOW	GENMASK(31, 16)
#define RTL9602C_GPON_CNT_PLOAM_ERR_CRC	GENMASK(15, 0)
#define RTL9602C_GPON_CNT_BWMAP_RX	0x7011b0
#define RTL9602C_GPON_CNT_BWMAP_ERR	0x7011a4
#define RTL9602C_GPON_CNT_BWMAP_ERR_OVERFLOW	GENMASK(31, 16)
#define RTL9602C_GPON_CNT_BWMAP_ERR_CRC	GENMASK(15, 0)
#define RTL9602C_GPON_CNT_GEM_FRAMES	0x7011c4

/*
 * The downstream BIP errors, in two counters: BLOCKS counts the frames whose
 * BIP disagreed, BITS the parity bits that differed, which is what G.984.3
 * accumulates as ERR (clause 11.2.2). Measured on this board: the first
 * frame after each bring-up counted as one block and 2 or 4 bits.
 */
#define RTL9602C_GPON_CNT_BIP_BLOCKS	0x701180
#define RTL9602C_GPON_CNT_BIP_BITS	0x701184

/*
 * The downstream FEC counts (G.984.3 clause 13.1.3): the bytes and the
 * codewords FEC corrected, and the codewords it could not correct. Taken to
 * count what their names say, which is not verified: the test OLT sends no
 * FEC, and they read 0.
 */
#define RTL9602C_GPON_CNT_FEC_BYTES		0x70118c
#define RTL9602C_GPON_CNT_FEC_CODEWORDS		0x701190
#define RTL9602C_GPON_CNT_FEC_UNCORRECTABLE	0x701194

/*
 * The codewords of a downstream frame that carries FEC: 152 of 255 bytes and
 * a shortened last one of 120 (G.984.3 clause 13.2.1). The driver counts the
 * codewords received as this many a frame, the frames as the advance of the
 * superframe count, while the downstream carries FEC and is locked; a step
 * of more than RTL9602C_GPON_FEC_MAX_FRAMES between two reads, as when the
 * downstream is found again, counts nothing.
 */
#define RTL9602C_GPON_FEC_CODEWORDS		153
#define RTL9602C_GPON_FEC_MAX_FRAMES		8000

/*
 * The downstream PLOAM queue: EMPTY while nothing waits, POP to remove the
 * head message, which the data words hold two octets each, the earlier in
 * bits 15:8.
 */
#define RTL9602C_PLOAM_RX_CTRL	0x701080
#define RTL9602C_PLOAM_RX_CTRL_EMPTY	BIT(5)
#define RTL9602C_PLOAM_RX_CTRL_POP		BIT(0)
#define RTL9602C_PLOAM_RX_DATA(i)	(0x7010a0 + (i) * 4)
#define RTL9602C_PLOAM_RX_DATA_WORD	GENMASK(15, 0)

/*
 * The GPON MAC's tables: the Alloc-ID of each T-CONT, and the GEM Port-ID each
 * flow takes from the downstream, the ONU's GEM port filter. They are RAM
 * that neither the MAC reset nor a chip reset clears.
 */
#define RTL9602C_ALLOCID_TBL_CMD		0x7010c0
#define RTL9602C_ALLOCID_TBL_WDATA		0x7010c4
#define RTL9602C_ALLOCID_TBL_RDATA		0x7010cc
#define RTL9602C_GEM_DS_PORTID_TBL_CMD		0x701100
#define RTL9602C_GEM_DS_PORTID_TBL_WDATA		0x701104
#define RTL9602C_GEM_DS_PORTID_TBL_RDATA		0x70110c

/*
 * The command word of the three tables. The operation and index go in with
 * EXEC clear before the data, which otherwise lands in another entry
 * (measured on this board); EXEC then starts the operation, DONE follows, and
 * VALID says whether the entry read is in use.
 */
#define RTL9602C_GPON_TBL_CMD_EXEC	BIT(15)
#define RTL9602C_GPON_TBL_CMD_DONE	BIT(14)
#define RTL9602C_GPON_TBL_CMD_VALID	BIT(13)
#define RTL9602C_GPON_TBL_CMD_OP	GENMASK(9, 8)
#define RTL9602C_GPON_TBL_CMD_IDX	GENMASK(6, 0)
#define RTL9602C_GPON_TBL_DATA_ID		GENMASK(11, 0)
#define RTL9602C_GPON_TBL_CMD_OP_WRITE	1
#define RTL9602C_GPON_TBL_CMD_OP_READ	2
#define RTL9602C_GPON_TBL_CMD_OP_CLEAR	3

/*
 * Per-flow downstream GEM counters: the flow into CMD, with BYTES for bytes,
 * then DATA once DONE. Each counts what the flow received, 32 bits, cleared
 * when read: the GEM frames, a fragment counting as one, and the bytes of
 * their payloads, the Ethernet frames with their FCS (measured on this board).
 */
#define RTL9602C_GEM_DS_CNT_CMD	0x701140
#define RTL9602C_GEM_DS_CNT_CMD_DONE	BIT(15)
#define RTL9602C_GEM_DS_CNT_CMD_BYTES		BIT(8)
#define RTL9602C_GEM_DS_CNT_CMD_FLOW		GENMASK(6, 0)
#define RTL9602C_GEM_DS_CNT_DATA	0x701144

/* The T-CONT of the default Alloc-ID, which carries PLOAM and the OMCC. */
#define RTL9602C_GPON_DEFAULT_TCONT_IDX	16

/* The 12-bit Alloc-IDs (G.984.3 clause 5.5.3), and "none" in a mapping. */
#define RTL9602C_GPON_NUM_ALLOC		4096
#define RTL9602C_GPON_ALLOC_NONE	0xffff

/*
 * Bandwidth-map capture of the next COUNT allocations matched to this ONU's
 * T-CONTs, two words each; the fields of STAT are not known.
 */
#define RTL9602C_BWMAP_CAPTURE_CTRL	0x70200c
#define RTL9602C_BWMAP_CAPTURE_CTRL_START	BIT(15)
#define RTL9602C_BWMAP_CAPTURE_CTRL_CLEAR	BIT(14)
#define RTL9602C_BWMAP_CAPTURE_CTRL_COUNT	GENMASK(7, 0)
#define RTL9602C_BWMAP_CAPTURE_STAT		0x702010
#define RTL9602C_BWMAP_CAPTURE_DATA(i)	(0x702400 + (i) * 4)

/*
 * Word 0 of a captured allocation. TCONT, PLOAMU and VALID are measured on
 * this board; the other fields are taken to be what their names say (not
 * verified).
 */
#define RTL9602C_BWMAP_ALLOC_FRAME		GENMASK(27, 24)
#define RTL9602C_BWMAP_ALLOC_VALID		BIT(23)
#define RTL9602C_BWMAP_ALLOC_LAST		BIT(22)
#define RTL9602C_BWMAP_ALLOC_BURST_END		BIT(21)
#define RTL9602C_BWMAP_ALLOC_BURST_START		BIT(20)
#define RTL9602C_BWMAP_ALLOC_PLOAMU		BIT(19)
#define RTL9602C_BWMAP_ALLOC_FEC		BIT(18)
#define RTL9602C_BWMAP_ALLOC_DBRU		GENMASK(17, 16)
#define RTL9602C_BWMAP_ALLOC_MULTIFRAME			GENMASK(14, 12)
#define RTL9602C_BWMAP_ALLOC_TCONT		GENMASK(4, 0)

/* Word 1: StartTime and StopTime, in bytes from the upstream frame's start. */
#define RTL9602C_BWMAP_ALLOC_SSTOP		GENMASK(31, 16)
#define RTL9602C_BWMAP_ALLOC_SSTART		GENMASK(15, 0)

/* The allocations captured while the ONU is ranged, for the bwmap view. */
#define RTL9602C_GPON_BWM_O4_ENTRIES	16

/*
 * A PLOAM message is 13 octets (ITU-T G.984.3 clause 9.1) and a serial number
 * 8; a received message fills 7 data words.
 */
#define RTL9602C_GPON_PLOAM_BYTES	13
#define RTL9602C_GPON_SN_BYTES		8
/* The password of the Password message, G.984.3 clause 9.2.4.2. */
#define RTL9602C_GPON_PASSWORD_BYTES	10
#define RTL9602C_GPON_PLOAM_WORDS	7

/* The events of the PLOAM channel the ploamlog view keeps, and their kinds. */
#define RTL9602C_PLOAM_LOG_LEN		256
enum {
	RTL9602C_EV_DS = 1,	/* a downstream message */
	RTL9602C_EV_US,		/* an upstream message written */
	RTL9602C_EV_US_REFUSED,	/* one refused, its queue full */
	RTL9602C_EV_STATE,	/* a change of activation state */
	RTL9602C_EV_USQ,	/* the upstream queue flags changed */
	RTL9602C_EV_US_INTR,	/* the upstream GTC latch */
	RTL9602C_EV_DS_INTR,	/* the downstream GTC latch */
};

/*
 * The overhead that opens each upstream burst: its length in bytes, and its
 * bytes in BURST_DATA(), the preamble and then the delimiter. REPEAT_IDX, the
 * last preamble byte, is taken to be the byte repeated to make up the length
 * (not verified).
 */
#define RTL9602C_BURST_CFG	0x705054
#define RTL9602C_BURST_CFG_REPEAT_IDX	GENMASK(11, 8)
#define RTL9602C_BURST_CFG_LEN	GENMASK(7, 0)
#define RTL9602C_BURST_DATA(i)	(0x705080 + (i) * 4)

/*
 * Index 8 and 12 bytes, with which the OLT decodes the serial-number bursts
 * (measured on this board).
 */
#define RTL9602C_BURST_CFG_PRERANGED	0x080c

/*
 * The 96 bits ITU-T G.984.2 gives the 1.24416 Gbit/s upstream, and the 128
 * bytes ITU-T G.984.3 allows for the whole burst mode overhead.
 */
#define RTL9602C_BURST_CFG_LEN_DEF	(96 / 8)
#define RTL9602C_BURST_CFG_LEN_MAX	128
#define RTL9602C_BURST_CFG_REPEAT_IDX_DEF	8

/*
 * How early the laser is keyed before a burst and how long it stays keyed
 * after, in units not known; a lag below 0x28 breaks discovery (measured on
 * this board).
 */
#define RTL9602C_GTC_US_LASER		0x70504c
#define RTL9602C_GTC_US_LASER_ON_LEAD	GENMASK(13, 8)
#define RTL9602C_GTC_US_LASER_OFF_LAG	GENMASK(5, 0)
#define RTL9602C_GTC_US_LASER_ON_LEAD_DEF	0x20
#define RTL9602C_GTC_US_LASER_OFF_LAG_DEF	0x28

/*
 * Upstream GTC control, under the write key. IND_NORMAL_PLOAM is taken to
 * flag a waiting normal PLOAM in the burst header's Ind field (not verified).
 */
#define RTL9602C_GTC_US_CTRL		0x705014
#define RTL9602C_GTC_US_CTRL_IND_NORMAL_PLOAM	BIT(10)

/*
 * Upstream GTC events, cleared when read; left unread, the latch keeps the
 * GPON interrupt asserted. The signal-detect bits, and the events EN_DEF
 * enables, are not verified.
 */
#define RTL9602C_GTC_US_IRQ_LATCH	0x705000
#define RTL9602C_GTC_US_IRQ_SD_MISMATCH	BIT(9)
#define RTL9602C_GTC_US_IRQ_SD_TOO_LONG	BIT(8)
#define RTL9602C_GTC_US_IRQ_EN	0x705004
#define RTL9602C_GTC_US_IRQ_EN_DEF	0x00a4

/* Burst power save in the GTC, under the write key; not verified. */
#define RTL9602C_GTC_US_POWERSAVE	0x70526c
#define RTL9602C_GTC_US_POWERSAVE_EN	BIT(0)

/*
 * The ONU-ID the upstream carries. Left assigned below O4, it keeps discovery
 * from ever succeeding again (measured on this board).
 */
#define RTL9602C_ONU_US_ONUID		0x705010

/*
 * Upstream transmit mode, under the write key: SMALL_SSTART starts a burst
 * whose StartTime is below its overhead in the previous frame; SUPPRESS_DIS
 * would turn the automatic laser suppression off. Both meanings are inferred
 * from use, not verified.
 */
#define RTL9602C_GTC_US_TX_MODE	0x705200
#define RTL9602C_GTC_US_TX_MODE_SUPPRESS_DIS	BIT(1)
#define RTL9602C_GTC_US_TX_MODE_SMALL_SSTART	BIT(0)

/* More of RTL9602C_GTC_US_CTRL; which level BEN_POL selects is not known. */
#define RTL9602C_GTC_US_CTRL_PLOAM_DIS	BIT(9)
#define RTL9602C_GTC_US_CTRL_SCRAMBLE_DIS	BIT(0)
#define RTL9602C_GTC_US_CTRL_BEN_POL	BIT(3)

/*
 * The PTI codes the upstream GEM framer uses; which kind of fragment takes
 * which is not known.
 */
#define RTL9602C_GEM_US_PTI_MAP	0x706020
#define RTL9602C_GEM_US_PTI_MAP_3	GENMASK(14, 12)
#define RTL9602C_GEM_US_PTI_MAP_2	GENMASK(10, 8)
#define RTL9602C_GEM_US_PTI_MAP_1	GENMASK(6, 4)
#define RTL9602C_GEM_US_PTI_MAP_0	GENMASK(2, 0)

/*
 * The equalization delay plus the coarse response delay, as whole upstream
 * frames and the bits left over.
 */
#define RTL9602C_RANGING_EQD		0x705044
#define RTL9602C_RANGING_EQD_FRAMES	GENMASK(26, 24)
#define RTL9602C_RANGING_EQD_BITS	GENMASK(17, 0)

/*
 * Upstream PLOAM messages: SLOT selects what RTL9602C_PLOAM_TX_DATA()
 * addresses, and PUSH hands the message over. A queue's EMPTY bit clears as
 * it takes a message and sets once the message is sent; that a write to a
 * FULL queue overwrites the message waiting there is not verified.
 */
#define RTL9602C_PLOAM_TX_CTRL	0x7050c0
#define RTL9602C_PLOAM_TX_CTRL_SLOT	GENMASK(10, 8)
#define RTL9602C_PLOAM_TX_CTRL_NORMAL_EMPTY	BIT(7)
#define RTL9602C_PLOAM_TX_CTRL_NORMAL_FULL	BIT(6)
#define RTL9602C_PLOAM_TX_CTRL_URGENT_EMPTY	BIT(5)
#define RTL9602C_PLOAM_TX_CTRL_URGENT_FULL	BIT(4)
#define RTL9602C_PLOAM_TX_CTRL_PUSH	BIT(0)

/* The normal and urgent queues, and templates the MAC sends itself. */
#define RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL	0x0
#define RTL9602C_PLOAM_TX_CTRL_SLOT_URGENT	0x1
#define RTL9602C_PLOAM_TX_CTRL_SLOT_DYING_GASP	0x5
#define RTL9602C_PLOAM_TX_CTRL_SLOT_SN_ONU	0x6
#define RTL9602C_PLOAM_TX_CTRL_SLOT_NO_MESSAGE	0x7

/* An upstream message: octets 1 to 12; the MAC adds the CRC. */
#define RTL9602C_GPON_PLOAM_PAYLOAD_WORDS	6

/*
 * Octet 12 of Serial_Number_ONU (ITU-T G.984.3 clause 9.2.4.1), at offset
 * RTL9602C_PLOAM_SN_ONU_FLAGS; message offsets count from octet 1 as 0.
 * POWER_LEVEL, TT, is the transmit power level the ONU sends at: 2 high, 1
 * medium, 0 low, the reverse of the level modes of Upstream_Overhead, where 0
 * is normal, the highest.
 */
#define RTL9602C_PLOAM_SN_ONU_GEM		BIT(2)
#define RTL9602C_PLOAM_SN_ONU_POWER_LEVEL	GENMASK(1, 0)
#define RTL9602C_PLOAM_SN_ONU_POWER_LEVEL_HIGH	2
#define RTL9602C_PLOAM_SN_ONU_FLAGS		11

/* The data window of the selected slot. */
#define RTL9602C_PLOAM_TX_DATA(i)	(0x7050e0 + (i) * 4)

/*
 * A rising edge of FLUSH is taken to empty the upstream PLOAM queues (not
 * verified).
 */
#define RTL9602C_PLOAM_TX_CFG	0x705100
#define RTL9602C_PLOAM_TX_CFG_FLUSH	BIT(4)

/*
 * Upstream counters, selected by index: 0 counts burst overheads sent, 2 the
 * PLOAMs sent from the queues and 3 those from the templates (measured on
 * this board); 1 and 4 are taken to count DBRu and GEM bytes (not verified).
 */
#define RTL9602C_GPON_CNT_US_SEL	0x705140
#define RTL9602C_GPON_CNT_US_DATA	0x705148
#define RTL9602C_GPON_US_CNTR_COUNT	8

/*
 * Change_Power_Level (ITU-T G.984.3 clause 9.2.3.16): octet 3 says to increase
 * the transmitted power, to decrease it, or neither.
 */
#define RTL9602C_PLOAM_CPL_IND			2
#define RTL9602C_PLOAM_CPL_IND_MASK		GENMASK(1, 0)
#define RTL9602C_PLOAM_CPL_IND_INCREASE	2
#define RTL9602C_PLOAM_CPL_IND_DECREASE	1
#define RTL9602C_GPON_POWER_MODE_MAX		2

/*
 * Dying_Gasp (ITU-T G.984.3 clause 9.2.4.3) is sent at least this many times,
 * each copy given this long to leave the urgent queue.
 */
#define RTL9602C_GPON_DG_COPIES		3
#define RTL9602C_GPON_DG_WAIT_MS		50

/*
 * The PEE alarm clears when no Physical_Equipment_Error has come for this long
 * (ITU-T G.984.3 clause 11.1.2).
 */
#define RTL9602C_GPON_PEE_MS		3000

/* Upstream PLOAM message identifiers (ITU-T G.984.3 clause 9.2.4). */
#define RTL9602C_PLOAM_US_SERIAL_NUMBER_ONU	1
#define RTL9602C_PLOAM_US_PASSWORD	2
#define RTL9602C_PLOAM_US_DYING_GASP	3
#define RTL9602C_PLOAM_US_NO_MESSAGE	4
#define RTL9602C_PLOAM_US_ENCRYPTION_KEY	5
#define RTL9602C_PLOAM_US_PST		7
#define RTL9602C_PLOAM_US_REI		8

/*
 * PST, downstream and upstream alike (G.984.3 clause 9.2.3.17 and
 * clause 9.2.4.7): the line number, then the K1 and K2 bytes of protection
 * switching.
 */
#define RTL9602C_PLOAM_PST_LINE		2
#define RTL9602C_PLOAM_PST_BYTES		3

/*
 * Encryption_Key (G.984.3 clause 9.2.4.5): the key's index, the fragment's, 0
 * for octets 0 to 7 of the key and 1 for octets 8 to 15, then the fragment.
 */
#define RTL9602C_PLOAM_KEY_INDEX		2
#define RTL9602C_PLOAM_KEY_FRAG		3
#define RTL9602C_PLOAM_KEY_BYTES		4
#define RTL9602C_GPON_AES_KEY_BYTES		16
#define RTL9602C_GPON_AES_FRAG_BYTES		8
#define RTL9602C_PLOAM_US_ACKNOWLEDGE	9

/*
 * REI (G.984.3 clause 9.2.4.8): the BIP errors of the last BER interval, most
 * significant octet first, then a 4-bit sequence number.
 */
#define RTL9602C_PLOAM_REI_ERRORS	2
#define RTL9602C_PLOAM_REI_SEQ		6
#define RTL9602C_PLOAM_REI_SEQ_MASK	GENMASK(3, 0)

/* The broadcast ONU-ID downstream; no ONU-ID yet upstream. */
#define RTL9602C_ONU_ONUID_UNASSIGNED	0xff

/* The serial number's offset in Serial_Number_ONU. */
#define RTL9602C_PLOAM_SN_ONU_SERIAL		2

/*
 * One overhead byte in each of the burst data registers, and the bytes of the
 * type 1 (all ones) and type 2 (all zeros) preambles.
 */
#define RTL9602C_BURST_DATA_BYTE		GENMASK(7, 0)
#define RTL9602C_GPON_BOH_BYTES		12
#define RTL9602C_PREAMBLE_TYPE1		0xff
#define RTL9602C_PREAMBLE_TYPE2		0x00

/*
 * Upstream_Overhead (ITU-T G.984.3 clause 9.2.3.1). PRE_EQD_EN, in the flags
 * octet, says that the pre-assigned delay applies; the delay is in units of
 * 32 bytes, which PRE_EQD_SHIFT turns into bits.
 */
#define RTL9602C_PLOAM_OVERHEAD_GUARD_BITS		2
#define RTL9602C_PLOAM_OVERHEAD_TYPE1_BITS		3
#define RTL9602C_PLOAM_OVERHEAD_TYPE2_BITS		4
#define RTL9602C_PLOAM_OVERHEAD_TYPE3_PATTERN	5
#define RTL9602C_PLOAM_OVERHEAD_DELIMITER		6
#define RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN	3
#define RTL9602C_PLOAM_OVERHEAD_FLAGS		9
#define RTL9602C_PLOAM_OVERHEAD_EXTRA_SN_SHIFT	2
#define RTL9602C_PLOAM_OVERHEAD_EXTRA_SN_MASK	0x3
#define RTL9602C_PLOAM_OVERHEAD_PRE_EQD_EN		BIT(5)
#define RTL9602C_PLOAM_OVERHEAD_PRE_EQD_HI		10
#define RTL9602C_PLOAM_OVERHEAD_PRE_EQD_LO		11
#define RTL9602C_PLOAM_OVERHEAD_PRE_EQD_SHIFT	8

/* An upstream frame, and a unit of the response delay, in bits. */
#define RTL9602C_GPON_EQD_FRAME_BITS	(19440 * 8)
#define RTL9602C_GTC_US_RESP_DELAY_COARSE_BITS	(16 * 8)

/* In RTL9602C_GTC_DS_CTRL: the extra serial-number transmissions. */
#define RTL9602C_GTC_DS_CTRL_EXTRA_SN	GENMASK(7, 6)

/* Assign_ONU-ID (ITU-T G.984.3 clause 9.2.3.3). */
#define RTL9602C_PLOAM_ASSIGN_ONUID_ONUID		2
#define RTL9602C_PLOAM_ASSIGN_ONUID_SERIAL		3

/* Ranging_Time (G.984.3 clause 9.2.3.4); the delay in bits, MSB first. */
#define RTL9602C_PLOAM_RANGING_TIME_PATH		2
#define RTL9602C_PLOAM_RANGING_TIME_PATH_PROTECT	BIT(0)
#define RTL9602C_PLOAM_RANGING_TIME_EQD		3
#define RTL9602C_PLOAM_RANGING_TIME_EQD_LEN		4

/* Activation states (ITU-T G.984.3 clause 10.2.1). */
#define RTL9602C_ONU_O1_INITIAL		1
#define RTL9602C_ONU_O2_STANDBY		2
#define RTL9602C_ONU_O3_SERIAL_NUMBER	3
#define RTL9602C_ONU_O4_RANGING		4
#define RTL9602C_ONU_O5_OPERATION	5
#define RTL9602C_ONU_O6_POPUP		6
#define RTL9602C_ONU_O7_EMERGENCY_STOP	7

/* The period of the GPON worker. */
#define RTL9602C_GPON_POLL_MS		50

/*
 * The GEM ports' counters are added into their totals every this many runs of
 * the worker, 5 s: the downstream byte counter, 32 bits, wraps in 13.8 s at the
 * 2.48832 Gbit/s of the downstream.
 */
#define RTL9602C_GEM_CNTR_POLLS		100

/* TO1, armed on entering O3, and on entering O4 from O6. */
#define RTL9602C_GPON_TO1_MS		10000

/* TO2, armed on entering O6: how long the ONU waits there for POPUP. */
#define RTL9602C_GPON_TO2_MS		100

/* Downstream PLOAM message identifiers (ITU-T G.984.3 clause 9.2.3). */
#define RTL9602C_PLOAM_DS_UPSTREAM_OVERHEAD	1
#define RTL9602C_PLOAM_DS_ASSIGN_ONUID	3
#define RTL9602C_PLOAM_DS_RANGING_TIME	4
#define RTL9602C_PLOAM_DS_DEACTIVATE_ONUID	5
#define RTL9602C_PLOAM_DS_DISABLE_SERIAL_NUMBER	6
#define RTL9602C_PLOAM_DS_REQUEST_PASSWORD	9
#define RTL9602C_PLOAM_DS_ASSIGN_ALLOCID	10
#define RTL9602C_PLOAM_DS_POPUP			12
#define RTL9602C_PLOAM_DS_CONFIGURE_PORTID	14
#define RTL9602C_PLOAM_DS_EXTENDED_BURST_LENGTH	20
#define RTL9602C_PLOAM_DS_PONID		21

/*
 * The downstream messages the driver acts on besides, and those it only logs:
 * encryption, the OLT's equipment errors, power levelling, protection
 * switching and power saving.
 */
#define RTL9602C_PLOAM_DS_ENCRYPTED_PORTID	8
#define RTL9602C_PLOAM_DS_REQUEST_KEY		13
#define RTL9602C_PLOAM_DS_PEE			15
#define RTL9602C_PLOAM_DS_CHANGE_POWER_LEVEL	16
#define RTL9602C_PLOAM_DS_PST			17
#define RTL9602C_PLOAM_DS_BER_INTERVAL		18
#define RTL9602C_PLOAM_DS_KEY_SWITCHING_TIME	19
#define RTL9602C_PLOAM_DS_SWIFT_POPUP		22
#define RTL9602C_PLOAM_DS_RANGING_ADJUSTMENT	23
#define RTL9602C_PLOAM_DS_SLEEP_ALLOW		24

/*
 * BER_Interval (G.984.3 clause 9.2.3.18): the interval in downstream frames,
 * most significant octet first; a frame lasts 125 us.
 */
#define RTL9602C_PLOAM_BER_INTERVAL		2
#define RTL9602C_GPON_DS_FRAMES_PER_MS		8

/*
 * Ranging_Adjustment (G.984.3 clause 9.2.3.23): with DECREASE set the change is
 * taken from the equalization delay, otherwise added; the change in bits, most
 * significant octet first.
 */
#define RTL9602C_PLOAM_RANGING_ADJ_SIGN		2
#define RTL9602C_PLOAM_RANGING_ADJ_DECREASE	BIT(0)
#define RTL9602C_PLOAM_RANGING_ADJ_DELAY	3

/*
 * Key_Switching_Time (G.984.3 clause 9.2.3.19): the superframe counter value
 * from which the new key applies, 30 bits, most significant octet first.
 */
#define RTL9602C_PLOAM_KEY_SWITCH_TIME		2

/*
 * Encrypted_Port-ID (G.984.3 clause 9.2.3.8): ENCRYPT set encrypts the
 * Port-ID, clear stops it; the Port-ID as in Configure_Port-ID.
 */
#define RTL9602C_PLOAM_ENC_PORT_CTRL		2
#define RTL9602C_PLOAM_ENC_PORT_ENCRYPT		BIT(0)
#define RTL9602C_PLOAM_ENC_PORT_HI		3
#define RTL9602C_PLOAM_ENC_PORT_LO		4

/*
 * The AES keys of the downstream decryption (G.984.3 clause 12), the next one
 * and the active one. A rising request bit in RTL9602C_GPON_AES_KEY_CFG
 * addresses the next key, or the active key with the active-key bit set; each
 * of its eight 16-bit words then goes through RTL9602C_GPON_AES_KEY_DATA,
 * written by a rising request bit in RTL9602C_GPON_AES_KEY_WORD with the word's
 * index, whose done bit says that the write is complete. The data register
 * reads back the last word written. The MAC makes the next key the active one
 * at the superframe in RTL9602C_GPON_AES_SWITCH_TIME, when the superframe
 * counter equals it and only then, so a superframe already passed waits for the
 * counter to come round. The switched bit reads 0 from the write of a next key
 * until the MAC has switched to it, and 1 after the switch or a write of the
 * active key.
 */
#define RTL9602C_GPON_AES_KEY_CFG		0x703010
#define RTL9602C_GPON_AES_KEY_CFG_REQ		BIT(15)
#define RTL9602C_GPON_AES_KEY_CFG_ACTIVE	BIT(14)
#define RTL9602C_GPON_AES_SWITCH_TIME		0x703014
#define RTL9602C_GPON_AES_SWITCH_TIME_SF	GENMASK(29, 0)
#define RTL9602C_GPON_AES_KEY_WORD		0x703020
#define RTL9602C_GPON_AES_KEY_WORD_REQ		BIT(15)
#define RTL9602C_GPON_AES_KEY_WORD_DONE		BIT(14)
#define RTL9602C_GPON_AES_KEY_WORD_SWITCHED	BIT(7)
#define RTL9602C_GPON_AES_KEY_WORD_IDX		GENMASK(2, 0)
#define RTL9602C_GPON_AES_KEY_DATA		0x703024
#define RTL9602C_GPON_AES_KEY_WORDS		8

/* The GEM Port-IDs, 12 bits, and the mark of a flow bound to none. */
#define RTL9602C_GPON_NUM_PORTID		4096
#define RTL9602C_GPON_PORTID_NONE		0xffff

/*
 * Disable_Serial_Number (G.984.3 clause 9.2.3.6): whether the ONU with the
 * serial number that follows is disabled or enabled again, or every disabled
 * ONU enabled again.
 */
#define RTL9602C_PLOAM_DISABLE_SN_CTRL		2
#define RTL9602C_PLOAM_DISABLE_SN_DISABLE	0xff
#define RTL9602C_PLOAM_DISABLE_SN_ENABLE_ALL	0x0f
#define RTL9602C_PLOAM_DISABLE_SN_ENABLE	0x00
#define RTL9602C_PLOAM_DISABLE_SN_SERIAL	3

/*
 * Extended_Burst_Length (G.984.3 clause 9.2.3.20): the type 3 preamble bytes
 * before ranging, and from O5 on.
 */
#define RTL9602C_PLOAM_EXT_BURST_LEN_PRERANGED	2
#define RTL9602C_PLOAM_EXT_BURST_LEN_RANGED		3

/* Assign_Alloc-ID types (G.984.3 clause 9.2.3.10). */
#define RTL9602C_PLOAM_ASSIGN_ALLOCID_GEM		1
#define RTL9602C_PLOAM_ASSIGN_ALLOCID_DEALLOC	0xff

/* The OMCC's flow and upstream stream, never a GEM port's. */
#define RTL9602C_GPON_OMCC_FLOW		64

/*
 * What each flow carries downstream; what TYPE_MC does on its own is not
 * verified.
 */
#define RTL9602C_GEM_DS_FLOW_CFG(f)	(0x701400 + 4 * (f))
#define RTL9602C_GEM_DS_FLOW_CFG_TYPE_MC		BIT(0)
#define RTL9602C_GEM_DS_FLOW_CFG_TYPE_ETH		BIT(1)
#define RTL9602C_GEM_DS_FLOW_CFG_TYPE_OMCI		BIT(2)

/*
 * The flow's downstream is decrypted with the active AES key: the OLT has its
 * Port-ID encrypted (Encrypted_Port-ID). It is written with the type.
 */
#define RTL9602C_GEM_DS_FLOW_CFG_AES		BIT(4)

/* The Port-ID of each flow's upstream GEM frames. */
#define RTL9602C_GEM_US_FLOW_CFG(f)	(0x706400 + 4 * (f))

/*
 * Each flow's upstream octets, whole GEM frames with their 5-byte headers: 64
 * bits, the low word first, cleared when read and kept across a warm reset
 * (measured on this board).
 */
#define RTL9602C_GEM_US_FLOW_BYTES(f)	(0x706800 + 8 * (f))

/*
 * Each flow's upstream frame count, through an indirect port: the flow into
 * SEL, with ETH set for the Ethernet frames instead of the GEM frames, then
 * DATA once DONE. 32 bits, cleared when read; a fragment counts as one GEM
 * frame (measured on this board).
 */
#define RTL9602C_GEM_US_FLOW_CNT_SEL		0x706048
#define RTL9602C_GEM_US_FLOW_CNT_SEL_FLOW	GENMASK(6, 0)
#define RTL9602C_GEM_US_FLOW_CNT_SEL_ETH	BIT(7)
#define RTL9602C_GEM_US_FLOW_CNT_SEL_DONE	BIT(15)
#define RTL9602C_GEM_US_FLOW_CNT_DATA		0x70604c

/* Fields of RTL9602C_GEM_US_FLOW_CFG() and RTL9602C_GEM_DS_FLOW_CFG(). */
#define RTL9602C_GEM_US_FLOW_CFG_PORTID		GENMASK(11, 0)
#define RTL9602C_GEM_DS_FLOW_CFG_TYPE		GENMASK(4, 0)

/* The multicast filter table, with the other two tables' command bits. */
#define RTL9602C_GEM_DS_MC_TBL_CMD			0x704084
#define RTL9602C_GEM_DS_MC_TBL_CMD_IDX			GENMASK(7, 0)

/*
 * The downstream GEM frame filter. At reset it passes only what the empty
 * multicast table includes, so no broadcast or unicast.
 */
#define RTL9602C_GEM_DS_MC_FILTER		0x704080
#define RTL9602C_GEM_DS_MC_FILTER_IPV6_PASS	BIT(10)
#define RTL9602C_GEM_DS_MC_FILTER_IPV6_DROP	BIT(9)
#define RTL9602C_GEM_DS_MC_FILTER_BCAST_PASS		BIT(6)
#define RTL9602C_GEM_DS_MC_FILTER_UCAST_PASS		BIT(4)
#define RTL9602C_GEM_DS_MC_FILTER_FCS_CHECK		BIT(3)
#define RTL9602C_GEM_DS_MC_FILTER_IPV4_PASS	BIT(2)
#define RTL9602C_GEM_DS_MC_FILTER_IPV4_DROP	BIT(1)
#define RTL9602C_GEM_DS_MC_FILTER_EXCLUDE		BIT(0)

/*
 * Burst power save in the upstream GEM framer, its window in cycles; not
 * verified.
 */
#define RTL9602C_GEM_US_POWERSAVE	0x706024
#define RTL9602C_GEM_US_POWERSAVE_LAG		GENMASK(20, 16)
#define RTL9602C_GEM_US_POWERSAVE_LEAD		GENMASK(9, 0)

/* The switch's OMCI counters; what each one counts is not verified. */
#define RTL9602C_MIB_OMCI_DISCARDS		0x0329b8
#define RTL9602C_MIB_OMCI_TX_PKTS	0x0329bc
#define RTL9602C_MIB_OMCI_RX_PKTS	0x0329c0
#define RTL9602C_MIB_OMCI_TX_OCTETS	0x0329c4
#define RTL9602C_MIB_OMCI_RX_OCTETS	0x0329c8
#define RTL9602C_MIB_OMCI_RX_CRC_ERRORS	0x0329cc

/*
 * The internal priority of the OMCI frames trapped to the CPU, which the
 * conduit takes on a receive ring of its own; that this register is what gives
 * them the priority is not verified.
 */
#define RTL9602C_OMCI_TRAP_CFG		0x0111f8
#define RTL9602C_OMCI_TRAP_CFG_PRI	GENMASK(2, 0)
#define RTL9602C_OMCI_TRAP_CFG_PRI_HIGHEST	7

/*
 * Per-port MAC configuration. RX_PON_CTRL passes PON control frames such as
 * OMCI; that it is needed is not verified.
 */
#define RTL9602C_PORT_MAC_CFG(p)		(0x020004 + 0x400 * (p))
#define RTL9602C_PORT_MAC_CFG_RX_PON_CTRL		BIT(2)

/*
 * INVERT inverts the dying-gasp (supply loss) comparator; which polarity
 * reports a real loss is not verified.
 */
#define RTL9602C_DGASP_CFG		0x0001ec
#define RTL9602C_DGASP_CFG_INVERT	BIT(3)

/*
 * Flow-control thresholds in buffer pages, switch-wide and the PON port's,
 * each an on and an off level; not verified.
 */
#define RTL9602C_PAUSE_THR_GLOBAL_HIGH		0x0230e4
#define RTL9602C_PAUSE_THR_GLOBAL_LOW		0x0230e8
#define RTL9602C_PAUSE_THR_PON_HIGH	0x02311c
#define RTL9602C_PAUSE_THR_PON_LOW	0x023120
#define RTL9602C_PAUSE_THR_ON		GENMASK(25, 16)
#define RTL9602C_PAUSE_THR_OFF		GENMASK(9, 0)
#define RTL9602C_FC_PON_EXTRA		0x30

/*
 * In the system controller's page, not the switch's window. The PON MAC, from
 * 0xf00000 in the switch's window, answers only once BLOCK_EN_PKTBUF is set:
 * before that, a read there never completes and stops the board (measured on
 * this board). What SOC_STATE_READY does is not known.
 */
#define RTL9602C_SYSCTL_SOC_STATE		0x044
#define RTL9602C_SYSCTL_SOC_STATE_READY	BIT(0)
#define RTL9602C_SYSCTL_BLOCK_EN		0x63c
#define RTL9602C_SYSCTL_BLOCK_EN_PKTBUF	BIT(5)

/* The halves of the packet buffer, for the RTL9602C_PKTBUF_*(d) macros. */
#define RTL9602C_PBO_US			0
#define RTL9602C_PBO_DS			1

/*
 * The packet buffer, a register set per half, keeping frames in pages in
 * SRAM and then in the DRAM region at DRAM_BASE. Page counts are written as
 * count - 1, and page size code 0 is taken to be 128 bytes; the counts and
 * the limits are not verified. Setting bit 6 of the upstream CTRL with the
 * engines running froze the board (measured on this board).
 */
#define RTL9602C_PKTBUF_CTRL(d)		((d) ? 0xf0a0ac : 0xf020d8)
#define RTL9602C_PKTBUF_CTRL_EPON_MODE		BIT(2)
#define RTL9602C_PKTBUF_CTRL_RX_STOP_EN	BIT(1)
#define RTL9602C_PKTBUF_CTRL_EN	BIT(0)
#define RTL9602C_PKTBUF_POOL_LIMIT(d)	((d) ? 0xf0a0b4 : 0xf020e0)
#define RTL9602C_PKTBUF_POOL_LIMIT_DRAM	GENMASK(28, 16)
#define RTL9602C_PKTBUF_POOL_LIMIT_SRAM	GENMASK(12, 0)
#define RTL9602C_PKTBUF_DRAM_BASE(d)		((d) ? 0xf0a0b8 : 0xf020e8)
#define RTL9602C_PKTBUF_STAT(d)		((d) ? 0xf0a0c0 : 0xf020f4)
#define RTL9602C_PKTBUF_STAT_INIT_DONE	BIT(0)
#define RTL9602C_PKTBUF_POOL_CFG(d)		((d) ? 0xf0a0cc : 0xf0215c)
#define RTL9602C_PKTBUF_POOL_CFG_DRAM_PAGES		GENMASK(28, 16)
#define RTL9602C_PKTBUF_POOL_CFG_PAGE_SIZE	GENMASK(14, 13)
#define RTL9602C_PKTBUF_POOL_CFG_SRAM_PAGES	GENMASK(12, 0)
#define RTL9602C_PKTBUF_POOL_USED(d)	((d) ? 0xf0a0bc : 0xf020ec)
#define RTL9602C_PKTBUF_POOL_USED_DRAM	GENMASK(28, 16)
#define RTL9602C_PKTBUF_POOL_USED_SRAM	GENMASK(12, 0)

/*
 * Each half's DMA engine, which runs while both its enables are set, and its
 * switch-facing MAC. Only the two enables are known to be needed.
 */
#define RTL9602C_PKTBUF_DMA_DBG_SEL(d)	((d) ? 0xf0d400 : 0xf05400)
#define RTL9602C_PKTBUF_DMA_DBG_SEL_FUNC	GENMASK(7, 5)
#define RTL9602C_PKTBUF_DMA_CTRL(d)		((d) ? 0xf0d434 : 0xf05434)
#define RTL9602C_PKTBUF_DMA_CTRL_MAX_BURST_SEL0	BIT(31)
#define RTL9602C_PKTBUF_DMA_CTRL_MAX_BURST_SEL1	BIT(29)
#define RTL9602C_PKTBUF_DMA_CTRL_EARLY_TX	BIT(28)
#define RTL9602C_PKTBUF_DMA_CTRL_TX_FIFO_THR	GENMASK(20, 19)
#define RTL9602C_PKTBUF_DMA_CTRL_RX_FIFO_THR	GENMASK(12, 11)
#define RTL9602C_PKTBUF_DMA_CTRL_RX_MAX_BURST	GENMASK(7, 6)
#define RTL9602C_PKTBUF_DMA_CTRL_GMII_RX_EN	BIT(5)
#define RTL9602C_PKTBUF_DMA_CTRL_GMII_TX_EN	BIT(4)
#define RTL9602C_PKTBUF_DMA_PAGE_CFG(d)		((d) ? 0xf0d438 : 0xf05438)
#define RTL9602C_PKTBUF_DMA_PAGE_CFG_RX_SIZE	GENMASK(5, 4)
#define RTL9602C_PKTBUF_DMA_PAGE_CFG_TX_SIZE	GENMASK(1, 0)
#define RTL9602C_PKTBUF_MAC_TX_CFG(d)		((d) ? 0xf0c040 : 0xf04040)
#define RTL9602C_PKTBUF_MAC_TX_CFG_IPG		GENMASK(12, 10)
#define RTL9602C_PKTBUF_MAC_TX_CFG_PREAMBLE_LEN	GENMASK(2, 1)
#define RTL9602C_PKTBUF_MAC_TX_CFG_PAD_EN		BIT(0)
#define RTL9602C_PKTBUF_MAC_RX_CFG(d)		((d) ? 0xf0c044 : 0xf04044)
#define RTL9602C_PKTBUF_MAC_RX_CFG_ACCEPT_ERR		BIT(5)
#define RTL9602C_PKTBUF_MAC_CFG(d)		((d) ? 0xf0c04c : 0xf0404c)
#define RTL9602C_PKTBUF_MAC_CFG_RX_AFULL_EN	BIT(26)
#define RTL9602C_PKTBUF_MAC_CFG_TX_STOP_EN	BIT(17)
#define RTL9602C_PKTBUF_MAC_CFG_TX_EN_EXTRA	BIT(16)

/* When the upstream FIFO passes a frame on; not verified. */
#define RTL9602C_PKTBUF_US_FIFO_CFG	0xf020f0
#define RTL9602C_PKTBUF_US_FIFO_CFG_SPACE	GENMASK(5, 4)
#define RTL9602C_PKTBUF_US_FIFO_CFG_START	GENMASK(3, 0)

/*
 * Empties one upstream queue: START clears when the command is taken, BUSY
 * when the drain ends, which it does not while the GPON MAC is still in a
 * session that survived a warm reset (measured on this board).
 */
#define RTL9602C_PONQ_DRAIN_CMD		0xf020e4
#define RTL9602C_PONQ_DRAIN_CMD_IDX		GENMASK(9, 3)
#define RTL9602C_PONQ_DRAIN_CMD_BY_QUEUE	BIT(2)
#define RTL9602C_PONQ_DRAIN_CMD_START		BIT(1)
#define RTL9602C_PONQ_DRAIN_CMD_BUSY		BIT(0)

/*
 * Each upstream stream's queue and admission bit; a GEM port's stream is its
 * flow. Streams 0 to RTL9602C_PON_NUM_SID - 1 exist; for a stream from 68 up
 * STREAM_QUEUE() would lie past the map, and the driver never asks for one.
 */
#define RTL9602C_PONQ_US_STREAM_QUEUE(s)		(0xf020f8 + 4 * ((s) / 4))
#define RTL9602C_PONQ_US_STREAM_QUEUE_SHIFT(s)	(((s) % 4) * 7)
#define RTL9602C_PONQ_US_STREAM_QUEUE_MASK	GENMASK(6, 0)
#define RTL9602C_PONQ_US_STREAM_EN(s)	(0xf0213c + 4 * ((s) / 32))

/* Thresholds on the remainder of a grant; not verified. */
#define RTL9602C_PONSCH_GRANT_THR		0xf02150
#define RTL9602C_PONSCH_GRANT_THR_LAST		GENMASK(29, 16)
#define RTL9602C_PONSCH_GRANT_THR_RUNT		GENMASK(13, 0)

/* The OMCC's stream, named to the PON MAC; not verified to be needed. */
#define RTL9602C_OMCI_OMCC_CFG		0xf02154
#define RTL9602C_OMCI_OMCC_CFG_STREAM		GENMASK(6, 0)

/* Per-bit forcing of the queues' committed-rate state; no field is known. */
#define RTL9602C_PONSCH_CIR_FORCE_EN	0xf02170
#define RTL9602C_PONSCH_CIR_FORCE_VAL	0xf02174

/* The token clock of the queue rate limiters; not verified. */
#define RTL9602C_PONSCH_TOKEN_CLK		0xf02190
#define RTL9602C_PONSCH_TOKEN_CLK_TOKENS		GENMASK(15, 8)
#define RTL9602C_PONSCH_TOKEN_CLK_PERIOD		GENMASK(7, 0)

/*
 * PIR_DROP is taken to drop what exceeds a queue's peak rate (not verified).
 */
#define RTL9602C_PONSCH_CFG		0xf02194
#define RTL9602C_PONSCH_CFG_PIR_DROP	BIT(18)

/* Each queue's committed and peak rates, in a unit not known. */
#define RTL9602C_PONSCH_QUEUE_CIR(q)		(0xf02198 + 4 * (q))
#define RTL9602C_PONSCH_QUEUE_PIR(q)		(0xf0229c + 4 * (q))
#define RTL9602C_PONSCH_QUEUE_RATE		GENMASK(17, 0)

/* Each T-CONT's queues, a bitmap over its group's, and its enable bit. */
#define RTL9602C_PONSCH_TCONT_QUEUES(t)	(0xf023a0 + 4 * (t))
#define RTL9602C_PONSCH_TCONT_EN		0xf023e4

/* Each queue's weighted, rather than strict, bit and its 10-bit weight. */
#define RTL9602C_PONSCH_QUEUE_WFQ(q)	(0xf023e8 + 4 * ((q) / 32))
#define RTL9602C_PONSCH_QUEUE_WEIGHT(q)	(0xf023f8 + 4 * ((q) / 3))
#define RTL9602C_PONSCH_QUEUE_WEIGHT_SHIFT(q)	(((q) % 3) * 10)
#define RTL9602C_PONSCH_QUEUE_WEIGHT_MASK	GENMASK(9, 0)

/*
 * Upstream back-pressure marks in pages, each an on and an off level; not
 * verified.
 */
#define RTL9602C_PKTBUF_THR_US_STOP	0xf02450
#define RTL9602C_PKTBUF_THR_US_GLOBAL		0xf02454
#define RTL9602C_PKTBUF_THR_US_STREAM(s)	(0xf02458 + 4 * (s))
#define RTL9602C_PKTBUF_THR_ON		GENMASK(28, 16)
#define RTL9602C_PKTBUF_THR_OFF		GENMASK(12, 0)

/*
 * Downstream: each stream's buffer queue, and the flow-control marks, whose
 * asserting field is not known; not verified.
 */
#define RTL9602C_PONQ_DS_STREAM_QUEUE(s)	(0xf0a0e4 + 4 * ((s) / 16))
#define RTL9602C_PONQ_DS_STREAM_QUEUE_SHIFT(s)	(((s) % 16) * 2)
#define RTL9602C_PONQ_DS_STREAM_QUEUE_MASK	GENMASK(1, 0)
#define RTL9602C_PKTBUF_THR_DS	0xf0a0fc

/*
 * The upstream scheduler's T-CONTs, queues and streams. T-CONT t's queue q is
 * RTL9602C_PON_QUEUE(t, q), measured for T-CONTs 0 and 16 and taken to hold
 * for the others (not verified).
 */
#define RTL9602C_PON_NUM_TCONT		17
#define RTL9602C_PON_NUM_QUEUE		65
#define RTL9602C_PON_NUM_SID		65
#define RTL9602C_PON_QUEUE(t, q)	(32 * ((t) / 8) + (q))

/*
 * The T-CONTs GEM ports use and their queues. A group of eight T-CONTs draws
 * on 32 queues, so the driver uses the first four T-CONTs of each group,
 * with eight queues each, one per IEEE 802.1p priority: the PON class's
 * T-CONT n, 0 to RTL9602C_GEM_NUM_TCONT - 1, is the scheduler's and the
 * GPON MAC's T-CONT rtl9602c_gem_tcont(n), and its queue of priority k, 0
 * the highest, is bit rtl9602c_gem_tcont_qbit(n, k) of that T-CONT's map.
 * Among a T-CONT's strict queues the higher queue number goes first
 * (measured on this board), so priority 0 takes the T-CONT's highest bit.
 */
#define RTL9602C_GEM_NUM_TCONT		8
#define RTL9602C_GEM_TCONT_QUEUES	8

static inline unsigned int rtl9602c_gem_tcont_qbit(unsigned int n,
						   unsigned int k)
{
	return RTL9602C_GEM_TCONT_QUEUES * (n % 4 + 1) - 1 - k;
}

#define RTL9602C_PONSCH_QUEUE_RATE_MAX		0x3ffff

/* The page size, each half's SRAM (not verified) and its DRAM region. */
#define RTL9602C_PBO_PAGE		128
#define RTL9602C_PBO_SRAM_US		0x4000
#define RTL9602C_PBO_SRAM_DS		0x1000
#define RTL9602C_PBO_DRAM		0x100000

/* What a GTC register reads before the MAC leaves reset; harmless. */
#define RTL9602C_GPON_NO_RESPONSE		0xca0eca0f

/*
 * The GPON MAC's memory self-test and the downstream interrupt status; the
 * revision register is the switch driver's too (RTL9602C_GPON_REVISION in
 * <linux/platform_data/rtl9602c-pon.h>).
 */
#define RTL9602C_GPON_MEM_SELFTEST	0x700280
#define RTL9602C_GTC_DS_IRQ_STAT	0x701008
#define RTL9602C_GPON_MEM_SELFTEST_DONE		BIT(0)
#define RTL9602C_GPON_MEM_SELFTEST_RETAIN_DONE	BIT(1)
#define RTL9602C_GPON_MEM_SELFTEST_FAIL		BIT(3)
#define RTL9602C_GPON_MEM_SELFTEST_RETAIN_FAIL	BIT(4)

#endif /* _RTL9602C_PON_REGS_H */
