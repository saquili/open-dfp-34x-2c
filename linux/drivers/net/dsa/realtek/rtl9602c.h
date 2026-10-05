/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL9602C switch: what the files of the DSA driver share.
 *
 * The register map comes first, block by block. An offset is from the start
 * of the switch's register window, the switch node's reg, unless it is said
 * to be the system controller's; a value is a 32-bit word as readl() and
 * writel() see it on this big-endian CPU. The driver's state, struct
 * rtl9602c_priv, the other shared types and the functions the files call
 * across follow.
 */

#ifndef _RTL9602C_H
#define _RTL9602C_H

#include <linux/bits.h>
#include <linux/if_vlan.h>
#include <linux/pon.h>
#include <linux/dsa/rtl9602c.h>

/*
 * Ports: 0 is the SerDes lane to the host, 1 is not wired on this board, 2 is
 * the PON port, behind which the GPON MAC sits (RTL9602C_PORT_PON, in the
 * header the tagger and the conduit share), and 3 the CPU port, whose conduit
 * is the CPU port's DMA engine.
 */
#define RTL9602C_PORT_SFP		0
#define RTL9602C_PORT_CPU		3
#define RTL9602C_NUM_PORTS		4

/*
 * The link, speed and duplex each port's MAC is forced to, as no port
 * negotiates: FORCE_VAL(p) holds the values and FORCE_EN(p) which of them
 * apply. 2.5 Gb/s is not among the speeds; see RTL9602C_SHAPE_2G5_CFG_EN.
 */
#define RTL9602C_PORT_FORCE_VAL(p)		(0x000180 + 4 * (p))
#define RTL9602C_PORT_FORCE_EN(p)		(0x0001b4 + 4 * (p))
#define RTL9602C_PORT_FORCE_SPEED		GENMASK(1, 0)
#define RTL9602C_PORT_FORCE_SPEED_1000		2
#define RTL9602C_PORT_FORCE_DUPLEX		BIT(2)	/* full duplex */

/*
 * Link up. In FORCE_EN the same bit is taken to enable the forced link (not
 * verified).
 */
#define RTL9602C_PORT_FORCE_LINK		BIT(4)

/*
 * Pause: with RXPAUSE the MAC stops sending while a received pause frame
 * holds it, with TXPAUSE it sends pause frames when its port's buffer fills.
 */
#define RTL9602C_PORT_FORCE_RXPAUSE		BIT(5)
#define RTL9602C_PORT_FORCE_TXPAUSE		BIT(6)
#define RTL9602C_PORT_FORCE_EN_ALL	GENMASK(11, 0)

/*
 * Bits of RTL9602C_SHAPE_2G5_CFG, port 0's 2.5G line-rate override: at
 * 2500BaseX the port is forced to 1000 Mb/s with the override on.
 */
#define RTL9602C_SHAPE_2G5_CFG_EN		BIT(19)
#define RTL9602C_SHAPE_2G5_CFG_RATE_FULL		0x3ffff

/*
 * The SerDes lane to the host. LINK_UP, read-only, says that the lane's own
 * receiver has synchronised; the host sees carrier from this side's
 * transmitter alone.
 */
#define RTL9602C_SERDES_CTRL		0x000214
#define RTL9602C_SERDES_CTRL_PCIE		BIT(15)
#define RTL9602C_SERDES_CTRL_CLK_STOP	BIT(13)
#define RTL9602C_SERDES_CTRL_PHY_MODE	BIT(8)
#define RTL9602C_SERDES_CTRL_LINK_UP	BIT(17)
#define RTL9602C_SERDES_CTRL_MODE		GENMASK(4, 0)
#define RTL9602C_SERDES_CTRL_MODE_1000BASEX		4
#define RTL9602C_SERDES_CTRL_MODE_2500BASEX		22
#define RTL9602C_SERDES_CTRL_MODE_OFF		0x1f	/* the lane parked */

/*
 * The lane's analog settings, offset and value, written in order while the
 * lane is parked. 1000BaseX gets them too; that it needs no others is not
 * verified.
 */
#define RTL9602C_SERDES_ANA_BASEX_SEQ		{			\
	{ 0x021700, 0x1800 }, { 0x02170c, 0x7b01 },		\
	{ 0x021718, 0x0168 }, { 0x0215b8, 0x98c5 },		\
	{ 0x0215b4, 0x1764 }, { 0x0215bc, 0x400f },		\
	{ 0x0215e4, 0xfc00 },					\
}

/*
 * The fibre PHY's power-down and PATCH_DONE: the bootloader leaves both as
 * the bring-up writes them, and whether either write is needed is not
 * verified. RESET_SEQ, written in turn, pulses the lane's reset.
 */
#define RTL9602C_SERDES_FIBER_CTRL		0x021c00
#define RTL9602C_SERDES_FIBER_CTRL_PDOWN		BIT(11)
#define RTL9602C_PHY_CTRL		0x000110
#define RTL9602C_PHY_CTRL_PATCH_DONE	BIT(0)
#define RTL9602C_SERDES_RESET		0x021500
#define RTL9602C_SERDES_RESET_SEQ		{ 0x10, 0x30, 0x00 }

/*
 * In-band auto-negotiation (IEEE 802.3 clause 37), one bit per SerDes lane in
 * FORCE and RESTART; a set FORCE bit turns it off. A change reaches a running
 * link only when the lane is next parked and restarted. INIT turns it off on
 * the SFP lane, without which the lane's receiver never synchronises with a
 * host that does not negotiate in band (measured on this board).
 */
#define RTL9602C_SERDES_AN_CTRL		0x021808
#define RTL9602C_SERDES_AN_CTRL_INIT		0x71e0
#define RTL9602C_SERDES_AN_CTRL_FORCE	GENMASK(9, 8)
#define RTL9602C_SERDES_AN_CTRL_RESTART	GENMASK(3, 0)

/* The SFP lane's bit in FORCE and RESTART. */
#define RTL9602C_SERDES_LANE_SFP		0

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
 * burst-enable output level; the transmit amplitude. SD_POR must be clear or
 * the receiver reports no signal (measured on this board); what it selects is
 * not known, and none of the other settings is verified to be needed.
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

/* The GPON MAC's T-CONTs, flows and downstream multicast filter entries. */
#define RTL9602C_GPON_NUM_TCONT			32
#define RTL9602C_GPON_NUM_FLOW			128
#define RTL9602C_GPON_NUM_MC			256

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

static inline unsigned int rtl9602c_gem_tcont(unsigned int n)
{
	return 8 * (n / 4) + n % 4;
}

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

/*
 * Spanning-tree state, two bits per instance; the encodings other than
 * forwarding are not verified.
 */
#define RTL9602C_STP_PORT_STATE(p)		(0x01702c + 4 * (p))
#define RTL9602C_STP_PORT_STATE_MSTI0		GENMASK(1, 0)
#define RTL9602C_STP_PORT_STATE_DISABLED		0
#define RTL9602C_STP_PORT_STATE_BLOCKING		1
#define RTL9602C_STP_PORT_STATE_LEARNING		2
#define RTL9602C_STP_PORT_STATE_FORWARDING		3

/*
 * The ports a frame arriving on port p may leave by. Like every forwarding
 * mask, it holds the four ports and seven extension ports; a frame reaches
 * the CPU port only when the mask names EXT0 as well (measured on this
 * board).
 */
#define RTL9602C_ISOLATE_EGRESS(p)		(0x027000 + 4 * ((p) / 2))
#define RTL9602C_ISOLATE_EGRESS_WIDTH		11
#define RTL9602C_ISOLATE_EGRESS_MASK		GENMASK(10, 0)
#define RTL9602C_ISOLATE_EGRESS_EXT0	BIT(4)
#define RTL9602C_ISOLATE_EGRESS_SHIFT(p)	(((p) % 2) * RTL9602C_ISOLATE_EGRESS_WIDTH)

/*
 * The ports a broadcast, an unknown multicast and an unknown unicast frame may
 * leave by.
 */
#define RTL9602C_FLOOD_BCAST		0x01c020
#define RTL9602C_FLOOD_UNKNOWN_MCAST	0x01c024
#define RTL9602C_FLOOD_UNKNOWN_UCAST	0x01c028

/*
 * What becomes of a multicast frame that no address-table entry knows, by the
 * port it came in by, two bits a port: FLOOD sends it by the unknown-multicast
 * flood mask, as at reset, and DROP drops it. IPv4 and IPv6 frames, as the
 * switch parses them, take the first two registers, other frames the third.
 * A reserved group is flooded even where DROP is set (see
 * RTL9602C_MC_UNKNOWN_CFG). The IPv4 and IPv6 registers were measured on the
 * PON port; the frame-type register is only written as at reset.
 */
#define RTL9602C_MC_UNKNOWN_L2			0x01c010
#define RTL9602C_MC_UNKNOWN_IP4			0x01c014
#define RTL9602C_MC_UNKNOWN_IP6			0x01c018
#define RTL9602C_MC_UNKNOWN_ACT(p)		(GENMASK(1, 0) << (2 * (p)))
#define RTL9602C_MC_UNKNOWN_ACT_FLOOD		0
#define RTL9602C_MC_UNKNOWN_ACT_DROP		1

/*
 * The reserved groups: the IPv4 groups 224.0.0.0/24 and the IPv6 classes
 * selected here, the link-scope groups ff02::/16 (LINK) and the
 * solicited-node groups ff02::1:ff00:0/104 (SOLICITED). With RSV4 and RSV6
 * at RSV_FLOOD, as at reset, these are flooded whatever their port's
 * RTL9602C_MC_UNKNOWN_IP4 or _IP6 setting says (measured on the PON port).
 * At reset no IPv6 class is selected.
 */
#define RTL9602C_MC_UNKNOWN_CFG			0x01c01c
#define RTL9602C_MC_UNKNOWN_CFG_RSV6_SOLICITED	BIT(12)
#define RTL9602C_MC_UNKNOWN_CFG_RSV6_LINK		BIT(8)
#define RTL9602C_MC_UNKNOWN_CFG_RSV6		GENMASK(6, 5)
#define RTL9602C_MC_UNKNOWN_CFG_RSV4		GENMASK(4, 3)
#define RTL9602C_MC_UNKNOWN_CFG_RSV_FLOOD		1

/*
 * Each port's learning limit, the maximum being unlimited and 0 learning off,
 * and the addresses it has learned.
 */
#define RTL9602C_FDB_LEARN_LIMIT(p)		(0x017008 + 4 * ((p) / 2))
#define RTL9602C_FDB_LEARN_CNT(p)		(0x017010 + 4 * ((p) / 2))
#define RTL9602C_FDB_LEARN_SHIFT(p)		(11 * ((p) % 2))
#define RTL9602C_FDB_LEARN_MASK		GENMASK(10, 0)
#define RTL9602C_FDB_LEARN_LIMIT_MAX		0x7ff

/* Taken to allow hairpinning per extension port (not verified). */
#define RTL9602C_ISOLATE_EXT_HAIRPIN	0x01c08c

/*
 * What becomes of a frame from a new source at the limit: FORWARD forwards it
 * without learning it. The packing per port is not verified.
 */
#define RTL9602C_FDB_LEARN_LIMIT_ACT		0x01c00c
#define RTL9602C_FDB_LEARN_LIMIT_ACT_SHIFT(p)	(2 * (p))
#define RTL9602C_FDB_LEARN_LIMIT_ACT_MASK		GENMASK(1, 0)
#define RTL9602C_FDB_LEARN_LIMIT_ACT_FORWARD	0

/*
 * How long a learned entry lives unrefreshed, for the whole switch, and the
 * CAM's off bit, set out of reset.
 */
#define RTL9602C_FDB_CFG		0x017000
#define RTL9602C_FDB_CFG_CAM_OFF	BIT(21)
#define RTL9602C_FDB_CFG_AGEING_TIME	GENMASK(20, 0)
#define RTL9602C_FDB_CFG_AGEING_TIME_UNIT_MS		100
#define RTL9602C_FDB_CFG_AGEING_TIME_MAX		0x1fffff

/* The bridge port flags the switch offers: learning and the three floods. */
#define RTL9602C_BRIDGE_FLAGS		(BR_LEARNING | BR_FLOOD | \
					 BR_MCAST_FLOOD | BR_BCAST_FLOOD)

/*
 * The longest frame accepted: FRAMELEN_SEL(p) picks one of two limits per
 * speed class, a clear bit LIMIT0 and a set bit LIMIT1; that the 10/100
 * selector works so is not verified. A frame from the CPU port counts the
 * CPU tag too (measured on this board).
 */
#define RTL9602C_FRAMELEN_SEL(p)		(0x011008 + 4 * (p))
#define RTL9602C_FRAMELEN_SEL_1000M	BIT(1)
#define RTL9602C_FRAMELEN_SEL_10_100M	BIT(0)
#define RTL9602C_FRAMELEN_LIMIT1		0x011018
#define RTL9602C_FRAMELEN_LIMIT0		0x023034
#define RTL9602C_FRAMELEN_LIMIT0_BYTES		GENMASK(13, 0)
#define RTL9602C_FRAMELEN_LIMIT0_BYTES_MAX		0x3fff

/*
 * What the limit counts beyond the MTU: header, FCS, two VLAN tags and the
 * CPU tag.
 */
#define RTL9602C_CPUTAG_LEN		8
#define RTL9602C_FRAME_OVERHEAD		(ETH_HLEN + ETH_FCS_LEN + \
					 2 * VLAN_HLEN + RTL9602C_CPUTAG_LEN)

/*
 * Port mirroring, one monitor port and one direction for the whole switch; no
 * copy has been seen (not verified).
 */
#define RTL9602C_MIRROR_CTRL		0x01c094
#define RTL9602C_MIRROR_CTRL_SRC_PORTS		GENMASK(19, 16)
#define RTL9602C_MIRROR_CTRL_EGRESS			BIT(5)
#define RTL9602C_MIRROR_CTRL_INGRESS			BIT(4)
#define RTL9602C_MIRROR_CTRL_DEST_PORT		GENMASK(1, 0)

/* The ingress policer, in units of 8 kbit/s; the maximum does not limit. */
#define RTL9602C_POLICE_PORT(p)		(0x020010 + 0x400 * (p))
#define RTL9602C_POLICE_PORT_RATE		GENMASK(20, 2)
#define RTL9602C_POLICE_PORT_RATE_MAX		0x7ffff

/*
 * The switch's interrupt. PENDING shows a source whether it is enabled or
 * not, and the line stays asserted until the source's bit is written 1.
 */
#define RTL9602C_IRQ_ENABLE		0x01d00c
#define RTL9602C_IRQ_PENDING		0x01d010
#define RTL9602C_IRQ_LINK_STAT		0x01d014

/* One latch for all ports, a port at its learning limit; write 1 to clear. */
#define RTL9602C_IRQ_LEARN_STAT	0x01d020
#define RTL9602C_IRQ_LEARN_STAT_LIMIT		BIT(0)

/* The sources the driver uses, as bits of ENABLE and PENDING. */
#define RTL9602C_IRQ_SRC_LINK		BIT(0)
#define RTL9602C_IRQ_SRC_METER	BIT(1)
#define RTL9602C_IRQ_SRC_LEARN_LIMIT	BIT(2)
#define RTL9602C_IRQ_SRC_LOOP		BIT(5)
#define RTL9602C_IRQ_SRC_ACL		BIT(7)
#define RTL9602C_IRQ_SRC_GPON		BIT(10)
#define RTL9602C_IRQ_SRC_PTP_PPS		BIT(12)

/*
 * Link events, one bit per port, cleared by writing 1; the register's other
 * bits hold a state that a write does not clear.
 */
#define RTL9602C_IRQ_LINK_STAT_CHANGE	GENMASK(3, 0)
#define RTL9602C_IRQ_LINK_STAT_UP	GENMASK(11, 8)
#define RTL9602C_IRQ_LINK_STAT_DOWN	GENMASK(19, 16)
#define RTL9602C_IRQ_LINK_STAT_EVENTS		(RTL9602C_IRQ_LINK_STAT_CHANGE | \
					 RTL9602C_IRQ_LINK_STAT_UP | \
					 RTL9602C_IRQ_LINK_STAT_DOWN)

/* One bit for the whole switch: forward by VLAN table membership. */
#define RTL9602C_VLAN_CFG		0x013008
#define RTL9602C_VLAN_CFG_AWARE		BIT(0)

/*
 * The CPU tag, carried in the DMA descriptors: INSERT tags each frame sent
 * to the CPU port; that PARSE, reading the CPU's tag, is needed is not
 * verified.
 */
#define RTL9602C_CPUTAG_CTRL		0x023030
#define RTL9602C_CPUTAG_CTRL_INSERT	BIT(8)
#define RTL9602C_CPUTAG_CTRL_PARSE		BIT(9)

/*
 * Per-port counters, 32 bits a word, read with no latch step. The two octet
 * counters are 64 bits, the high word taken to be the next (not verified).
 */
#define RTL9602C_MIB_TX(p)		(0x032000 + 128 * (p))
#define RTL9602C_MIB_RX(p)		(0x032200 + 128 * (p))

/* The counters the driver reads per port, and how often it folds them. */
#define RTL9602C_MIB_NUM		44
#define RTL9602C_MIB_FOLD_S		60

/*
 * Receive block. Checked against known traffic: RX_OCTETS,
 * RX_CRC_ALIGN_ERRORS, RX_UCAST_PKTS and RX_DROP_EVENTS; the other offsets
 * are not verified.
 */
#define RTL9602C_MIB_RX_OCTETS		0x00
#define RTL9602C_MIB_RX_CRC_ALIGN_ERRORS		0x08
#define RTL9602C_MIB_RX_SYMBOL_ERRORS	0x0c
#define RTL9602C_MIB_RX_PAUSE_FRAMES		0x10
#define RTL9602C_MIB_RX_UNKNOWN_OPCODES	0x14
#define RTL9602C_MIB_RX_FRAGMENTS	0x18
#define RTL9602C_MIB_RX_JABBERS		0x1c
#define RTL9602C_MIB_RX_UCAST_PKTS		0x20
#define RTL9602C_MIB_RX_DROP_EVENTS		0x24
#define RTL9602C_MIB_RX_MCAST_PKTS		0x28
#define RTL9602C_MIB_RX_BCAST_PKTS		0x2c
#define RTL9602C_MIB_RX_PKTS_1519_MAX	0x30
#define RTL9602C_MIB_RX_UNDERSIZE_PKTS	0x38
#define RTL9602C_MIB_RX_OVERSIZE_PKTS	0x3c
#define RTL9602C_MIB_RX_PKTS_64		0x40
#define RTL9602C_MIB_RX_PKTS_65_127		0x44
#define RTL9602C_MIB_RX_PKTS_128_255		0x48
#define RTL9602C_MIB_RX_PKTS_256_511		0x4c
#define RTL9602C_MIB_RX_PKTS_512_1023	0x50
#define RTL9602C_MIB_RX_PKTS_1024_1518	0x54

/*
 * Transmit block. Checked: TX_OCTETS, TX_UCAST_PKTS, TX_PKTS_512_1023 and
 * TX_DISCARDS; the other offsets are not verified.
 */
#define RTL9602C_MIB_TX_RMON_MCAST_PKTS	0x00
#define RTL9602C_MIB_TX_RMON_BCAST_PKTS	0x04
#define RTL9602C_MIB_TX_UNDERSIZE_PKTS	0x08
#define RTL9602C_MIB_TX_OVERSIZE_PKTS	0x0c
#define RTL9602C_MIB_TX_PKTS_64		0x10
#define RTL9602C_MIB_TX_PKTS_65_127		0x14
#define RTL9602C_MIB_TX_PKTS_128_255		0x18
#define RTL9602C_MIB_TX_PKTS_256_511		0x1c
#define RTL9602C_MIB_TX_PKTS_512_1023	0x20
#define RTL9602C_MIB_TX_PKTS_1024_1518	0x24
#define RTL9602C_MIB_TX_OCTETS		0x28
#define RTL9602C_MIB_TX_SINGLE_COLLISIONS	0x30
#define RTL9602C_MIB_TX_MULTIPLE_COLLISIONS	0x34
#define RTL9602C_MIB_TX_DEFERRED	0x38
#define RTL9602C_MIB_TX_LATE_COLLISIONS	0x3c
#define RTL9602C_MIB_TX_COLLISIONS	0x40
#define RTL9602C_MIB_TX_EXCESSIVE_COLLISIONS	0x44
#define RTL9602C_MIB_TX_PAUSE_FRAMES		0x48
#define RTL9602C_MIB_TX_DISCARDS	0x4c
#define RTL9602C_MIB_TX_PKTS_1519_MAX	0x50
#define RTL9602C_MIB_TX_UCAST_PKTS		0x5c
#define RTL9602C_MIB_TX_MCAST_PKTS		0x60
#define RTL9602C_MIB_TX_BCAST_PKTS		0x64

/*
 * In the transmit block, but it counts frames received and discarded by
 * forwarding.
 */
#define RTL9602C_MIB_TX_FWD_DISCARDS	0x58

/*
 * The indirect table engine, which serves the address, VLAN, ACL and
 * classifier tables; writing the command starts an operation. MODE_HASH goes
 * to the slot that the key in the write registers hashes to.
 */
#define RTL9602C_TBLACC_CMD		0x012000
#define RTL9602C_TBLACC_CMD_INDEX		GENMASK(20, 9)
#define RTL9602C_TBLACC_CMD_MODE	GENMASK(6, 4)
#define RTL9602C_TBLACC_CMD_MODE_HASH	0
#define RTL9602C_TBLACC_CMD_MODE_INDEX	1
#define RTL9602C_TBLACC_CMD_WRITE		BIT(3)
#define RTL9602C_TBLACC_CMD_TABLE		GENMASK(2, 0)
#define RTL9602C_TBLACC_CMD_TABLE_FDB		0
#define RTL9602C_TBLACC_CMD_TABLE_VLAN		1
#define RTL9602C_VLAN_TABLE_ENTRIES	4096	/* one word each, by VID */

/*
 * HIT: the slot reached holds an entry, or took the one written through the
 * hash; CAM: the slot is in the address table's CAM, not a hash bucket.
 */
#define RTL9602C_TBLACC_STAT		0x012004
#define RTL9602C_TBLACC_STAT_BUSY		BIT(13)
#define RTL9602C_TBLACC_STAT_HIT		BIT(12)
#define RTL9602C_TBLACC_STAT_CAM		BIT(10)
#define RTL9602C_TBLACC_STAT_INDEX		GENMASK(9, 0)

/* The entry in and out, word 0 holding its bits 31:0. */
#define RTL9602C_TBLACC_WDATA(i)		(0x012008 + 4 * (i))
#define RTL9602C_TBLACC_RDATA(i)		(0x01201c + 4 * (i))

/* Taken to enable the per-L3-interface counters (not verified). */
#define RTL9602C_MIB_L3IF_CTRL		0x03401c
#define RTL9602C_MIB_L3IF_CTRL_EN	BIT(0)

/* The L3/NAT engine's master enable; clear, the switch ignores its verdict. */
#define RTL9602C_L3_GLOBAL_CTRL		0x01106c
#define RTL9602C_L3_GLOBAL_CTRL_EN	BIT(0)

/*
 * Actions for frames whose WAN binding disagrees with the lookup; not
 * verified to be needed.
 */
#define RTL9602C_L3_BIND_CFG			0x80002c
#define RTL9602C_L3_BIND_CFG_INIT			0xa2400000

/*
 * A plain read/write word; what the bits of ON enable, and whether it is
 * needed, are not verified.
 */
#define RTL9602C_L3_AUX_CFG			0x02a104
#define RTL9602C_L3_AUX_CFG_ON		0xf5

/*
 * The engine's configuration. L3IF_LOOKUP_MAC is taken to match frames to
 * the interface rows by destination MAC (not verified). Changing more than
 * NAT_MODE and L3IF_LOOKUP sends translated flows to the CPU (measured on
 * this board).
 */
#define RTL9602C_L3_CFG			0x800010
#define RTL9602C_L3_CFG_FLOW4_EN		BIT(30)
#define RTL9602C_L3_CFG_FLOW6_EN		BIT(29)
#define RTL9602C_L3_CFG_NAT_MODE		GENMASK(11, 10)
#define RTL9602C_L3_CFG_NAT_MODE_NAPT			3
#define RTL9602C_L3_CFG_L3IF_LOOKUP		GENMASK(9, 8)
#define RTL9602C_L3_CFG_L3IF_LOOKUP_MAC		2

/* Hit bitmaps; whether a read clears them is not verified. */
#define RTL9602C_PPPOE_HIT		0x800014
#define RTL9602C_NEIGH6_HIT(i)		(0x800018 + 4 * (i))
#define RTL9602C_L3_FLOW_HIT(i)		(0x800030 + 4 * (i))
#define RTL9602C_NEIGH_HIT(i)		(0x800050 + 4 * (i))

/*
 * The engine's per-frame capture: its parse of the frame (IN) and its verdict
 * (OUT). The words latch separately, so a reading can mix two frames.
 */
#define RTL9602C_L3_CAPTURE_CTRL		0x800200
#define RTL9602C_L3_CAPTURE_CTRL_MODE	GENMASK(4, 2)
#define RTL9602C_L3_CAPTURE_CTRL_EN	BIT(1)
#define RTL9602C_L3_CAPTURE_CTRL_BUSY	BIT(0)
#define RTL9602C_L3_CAPTURE_IN(i)		(0x800204 + 4 * (i))
#define RTL9602C_L3_CAPTURE_IN_WORDS	14
#define RTL9602C_L3_CAPTURE_OUT(i)		(0x800280 + 4 * (i))
#define RTL9602C_L3_CAPTURE_OUT_WORDS	5

/* One bit per outbound NAPT entry; reading a word clears all 32 bits. */
#define RTL9602C_NAPT_HIT		0x800400
#define RTL9602C_NAPT_HIT_WORDS	128

/*
 * The engine's table port: READ or WRITE starts an access and clears when it
 * is done. A selector naming no table never completes and wedges the port
 * until a chip reset (measured on this board).
 */
#define RTL9602C_L3TBL_CTRL		0x800100
#define RTL9602C_L3TBL_CTRL_INDEX	GENMASK(15, 0)
#define RTL9602C_L3TBL_CTRL_TABLE	GENMASK(19, 16)
#define RTL9602C_L3TBL_CTRL_WRITE	BIT(24)
#define RTL9602C_L3TBL_CTRL_READ	BIT(25)

/*
 * Reset strobes per table, self-clearing; a strobe does not empty its table,
 * and which table each resets is not verified.
 */
#define RTL9602C_L3TBL_RESET		0x800104
#define RTL9602C_L3TBL_RESET_NEIGH	BIT(15)
#define RTL9602C_L3TBL_RESET_FLOW		BIT(14)
#define RTL9602C_L3TBL_RESET_ROUTE6	BIT(13)
#define RTL9602C_L3TBL_RESET_NEIGH6		BIT(12)
#define RTL9602C_L3TBL_RESET_BIND		BIT(11)
#define RTL9602C_L3TBL_RESET_WANTYPE		BIT(10)
#define RTL9602C_L3TBL_RESET_ROUTE		BIT(9)
#define RTL9602C_L3TBL_RESET_PPPOE		BIT(8)
#define RTL9602C_L3TBL_RESET_NEXTHOP		BIT(7)
#define RTL9602C_L3TBL_RESET_L3IF		BIT(6)
#define RTL9602C_L3TBL_RESET_EXTADDR		BIT(5)
#define RTL9602C_L3TBL_RESET_NAPT_IN	BIT(3)
#define RTL9602C_L3TBL_RESET_NAPT	BIT(2)

/* Data register i holds entry bits 32i + 31 to 32i. */
#define RTL9602C_L3TBL_READ_DATA(i)	(0x800108 + 4 * (i))
#define RTL9602C_L3TBL_WRITE_DATA(i)	(0x80011c + 4 * (i))
#define RTL9602C_L3TBL_DATA_WORDS		5

/* Table selectors; 8 names no table. */
#define RTL9602C_L3TBL_CTRL_TABLE_ROUTE	0
#define RTL9602C_L3TBL_CTRL_TABLE_PPPOE		1
#define RTL9602C_L3TBL_CTRL_TABLE_NEXTHOP	2
#define RTL9602C_L3TBL_CTRL_TABLE_L3IF		3
#define RTL9602C_L3TBL_CTRL_TABLE_EXTADDR	4
#define RTL9602C_L3TBL_CTRL_TABLE_ROUTE6	5
#define RTL9602C_L3TBL_CTRL_TABLE_BIND	6
#define RTL9602C_L3TBL_CTRL_TABLE_WANTYPE	7
#define RTL9602C_L3TBL_CTRL_TABLE_NAPT_IN		9
#define RTL9602C_L3TBL_CTRL_TABLE_NAPT		10
#define RTL9602C_L3TBL_CTRL_TABLE_NEIGH6	11
#define RTL9602C_L3TBL_CTRL_TABLE_FLOW	12
#define RTL9602C_L3TBL_CTRL_TABLE_NEIGH	13

/**
 * struct rtl9602c_field - a field of an L3/NAT table entry
 * @lsb: its lowest bit, entry bit n being bit n % 32 of data word n / 32
 * @len: its width in bits
 *
 * A field is set and read bit by bit, so it may cross from one word into the
 * next.
 */
struct rtl9602c_field {
	u16 lsb;
	u16 len;
};

/* The field of n bits from entry bit l. */
#define RTL9602C_FIELD(l, n)	((struct rtl9602c_field){ .lsb = (l), .len = (n) })

/*
 * Route entry. DST_LEN is the prefix length minus one. Action 0 hands the
 * frame to the CPU; NEIGH makes the prefix on-link on the interface row in
 * L3IF_IDX, and NEXTHOP sends it via the next-hop row in NEXTHOP_IDX, the
 * same bits. That DROP drops, and what INTERNAL does, are not verified.
 */
#define RTL9602C_ROUTE_DST		RTL9602C_FIELD(0, 32)
#define RTL9602C_ROUTE_DST_LEN		RTL9602C_FIELD(32, 5)
#define RTL9602C_ROUTE_VALID		RTL9602C_FIELD(37, 1)
#define RTL9602C_ROUTE_ACTION		RTL9602C_FIELD(38, 2)
#define RTL9602C_ROUTE_INTERNAL		RTL9602C_FIELD(40, 1)
#define RTL9602C_ROUTE_L3IF_IDX		RTL9602C_FIELD(41, 4)
#define RTL9602C_ROUTE_NEXTHOP_IDX		RTL9602C_FIELD(41, 4)
#define RTL9602C_ROUTE_ACTION_DROP	1
#define RTL9602C_ROUTE_ACTION_NEIGH		2
#define RTL9602C_ROUTE_ACTION_NEXTHOP		3

/* L3-interface entry; how the engine applies MTU is not verified. */
#define RTL9602C_L3IF_VALID		RTL9602C_FIELD(0, 1)
#define RTL9602C_L3IF_VID		RTL9602C_FIELD(1, 12)
#define RTL9602C_L3IF_MAC		RTL9602C_FIELD(13, 48)
#define RTL9602C_L3IF_MAC_MASK		RTL9602C_FIELD(61, 3)
#define RTL9602C_L3IF_MAC_MASK_EXACT	7
#define RTL9602C_L3IF_ROUTE_EN		RTL9602C_FIELD(64, 1)
#define RTL9602C_L3IF_MTU		RTL9602C_FIELD(65, 14)
#define RTL9602C_L3IF_OFFLOAD_EN		RTL9602C_FIELD(82, 1)
#define RTL9602C_L3IF_IP		RTL9602C_FIELD(83, 32)

/*
 * Outbound NAPT entry, four ways to a hash bucket, holding the index of its
 * inbound entry.
 */
#define RTL9602C_NAPT_WAYS		4
#define RTL9602C_NAPT_INBOUND_IDX	RTL9602C_FIELD(0, 12)
#define RTL9602C_NAPT_VALID		RTL9602C_FIELD(12, 1)

/*
 * Inbound NAPT entry. The engine does not check REMOTE_HASH on replies, and
 * that TCP is 1 for a TCP flow is not verified.
 */
#define RTL9602C_NAPT_IN_INTERNAL_ADDR		RTL9602C_FIELD(0, 32)
#define RTL9602C_NAPT_IN_INTERNAL_PORT		RTL9602C_FIELD(32, 16)
#define RTL9602C_NAPT_IN_REMOTE_HASH		RTL9602C_FIELD(48, 16)
#define RTL9602C_NAPT_IN_EXTADDR_IDX	RTL9602C_FIELD(64, 3)
#define RTL9602C_NAPT_IN_EXTERNAL_PORT		RTL9602C_FIELD(67, 16)
#define RTL9602C_NAPT_IN_TCP		RTL9602C_FIELD(83, 1)
#define RTL9602C_NAPT_IN_VALID		RTL9602C_FIELD(84, 2)
#define RTL9602C_NAPT_IN_VALID_ACTIVE		2

/* External-address entry: a translated source and the next hop it leaves by. */
#define RTL9602C_EXTADDR_IP		RTL9602C_FIELD(32, 32)
#define RTL9602C_EXTADDR_VALID		RTL9602C_FIELD(64, 1)
#define RTL9602C_EXTADDR_MODE		RTL9602C_FIELD(65, 2)
#define RTL9602C_EXTADDR_MODE_NAPT	0
#define RTL9602C_EXTADDR_NEXTHOP_IDX		RTL9602C_FIELD(67, 4)

/* Neighbour entry; what the engine uses it for is not verified. */
#define RTL9602C_NEIGH_IP			RTL9602C_FIELD(0, 32)
#define RTL9602C_NEIGH_VALID		RTL9602C_FIELD(32, 1)
#define RTL9602C_NEIGH_L2_IDX		RTL9602C_FIELD(33, 11)

/* PPPoE entry: the session ID, with no valid bit. */
#define RTL9602C_PPPOE_ENTRY_SESSION		RTL9602C_FIELD(0, 16)

/*
 * Next-hop entry. The destination MAC and egress port come from the
 * address-table entry L2_IDX names.
 */
#define RTL9602C_NEXTHOP_ENCAP		RTL9602C_FIELD(0, 1)
#define RTL9602C_NEXTHOP_ENCAP_PPPOE		1
#define RTL9602C_NEXTHOP_L3IF_IDX		RTL9602C_FIELD(1, 4)
#define RTL9602C_NEXTHOP_PPPOE_IDX		RTL9602C_FIELD(5, 3)
#define RTL9602C_NEXTHOP_L2_IDX		RTL9602C_FIELD(8, 11)

/*
 * VLAN entry; the CPU port is a member only with EXT0_MEMBER as well
 * (measured on this board).
 */
#define RTL9602C_VLAN_ENTRY_MEMBER_SET		GENMASK(3, 0)
#define RTL9602C_VLAN_ENTRY_UNTAGGED_SET		GENMASK(7, 4)
#define RTL9602C_VLAN_ENTRY_IVL		BIT(10)
#define RTL9602C_VLAN_ENTRY_EXT0_MEMBER		BIT(11)

/* The VID an untagged frame arriving on port p gets. */
#define RTL9602C_VLAN_PVID(p)		(0x01300c + 4 * ((p) / 2))
#define RTL9602C_VLAN_PVID_SHIFT(p)	(12 * ((p) % 2))
#define RTL9602C_VLAN_PVID_MASK		GENMASK(11, 0)

/* The part number, then three letters counting A as 1; this is RTL9602C. */
#define RTL9602C_CHIP_MODEL	0x010000
#define RTL9602C_CHIP_MODEL_FAMILY		GENMASK(31, 16)
#define RTL9602C_CHIP_MODEL_LETTER1		GENMASK(15, 11)
#define RTL9602C_CHIP_MODEL_LETTER2		GENMASK(10, 6)
#define RTL9602C_CHIP_MODEL_LETTER3		GENMASK(5, 1)

/*
 * The GPON MAC's revision and memory self-test, which answer at any time
 * (the self-test bits are not verified), and the downstream conditions now.
 */
#define RTL9602C_GPON_REVISION		0x700010
#define RTL9602C_GPON_MEM_SELFTEST	0x700280
#define RTL9602C_GTC_DS_IRQ_STAT	0x701008
#define RTL9602C_GPON_MEM_SELFTEST_DONE		BIT(0)
#define RTL9602C_GPON_MEM_SELFTEST_RETAIN_DONE	BIT(1)
#define RTL9602C_GPON_MEM_SELFTEST_FAIL		BIT(3)
#define RTL9602C_GPON_MEM_SELFTEST_RETAIN_FAIL	BIT(4)

/* What a GTC register reads before the MAC leaves reset; harmless. */
#define RTL9602C_GPON_NO_RESPONSE		0xca0eca0f

/*
 * The PTP clock. CLK_SEC and CLK_NSEC hold the last time latched. No register
 * takes an absolute time: a step adds the signed offset in the CLK_STEP
 * registers. Seconds span two words, the high part first.
 */
#define RTL9602C_PTP_CLK_SEC		0x01b000
#define RTL9602C_PTP_CLK_NSEC		0x01b008
#define RTL9602C_PTP_CLK_STEP_SEC	0x01b00c
#define RTL9602C_PTP_CLK_STEP_NSEC	0x01b014
#define RTL9602C_PTP_CLK_FREQ		0x01b018
#define RTL9602C_PTP_CLK_CTRL		0x01b01c

/* The time-of-day transmitter, only read; the field layout is not verified. */
#define RTL9602C_TOD_VERSION		0x01b030
#define RTL9602C_TOD_PPS_CFG		0x01b034
#define RTL9602C_TOD_PPS_CFG_MSG_DELAY		GENMASK(11, 6)
#define RTL9602C_TOD_PPS_CFG_WIDTH	GENMASK(5, 0)
#define RTL9602C_TOD_MSG_CFG	0x01b038
#define RTL9602C_TOD_MSG_CFG_LEN	GENMASK(8, 1)
#define RTL9602C_TOD_MSG_CFG_SRC_SW	BIT(0)
#define RTL9602C_TOD_GPS_WEEK	0x01b03c
#define RTL9602C_TOD_GPS_WEEK_NUM	GENMASK(15, 0)
#define RTL9602C_TOD_UTC_SEC		0x01b040
#define RTL9602C_TOD_MSG_DATA(n)	(0x01b060 - 4 * (n))
#define RTL9602C_TOD_MSG_BYTES		32
#define RTL9602C_TOD_BAUD		0x01b064
#define RTL9602C_TOD_BAUD_DIVISOR		GENMASK(15, 0)

/*
 * A second time value, taken to be the PON's time of day (not verified), and
 * a per-port PTP enable that receive stamping does not depend on.
 */
#define RTL9602C_PTP_PON_TIME_SEC	0x01b024
#define RTL9602C_PTP_PON_TIME_NSEC	0x01b02c
#define RTL9602C_PTP_PORT_EN		0x0111e8

/* In RTL9602C_PTP_CLK_CTRL; LATCH and STEP act once per 1 written. */
#define RTL9602C_PTP_CLK_CTRL_PON_TIME_EN	BIT(2)
#define RTL9602C_PTP_CLK_CTRL_LATCH		BIT(1)
#define RTL9602C_PTP_CLK_CTRL_STEP		BIT(0)

/*
 * Seconds bits 47:32, nanoseconds in 8 ns ticks, and the frequency word, one
 * second per second at NOMINAL.
 */
#define RTL9602C_PTP_SEC_UPPER		GENMASK(15, 0)
#define RTL9602C_PTP_NSEC_TICKS		GENMASK(29, 3)
#define RTL9602C_PTP_NSEC_TICK_NS	8
#define RTL9602C_PTP_CLK_FREQ_WORD		GENMASK(27, 0)
#define RTL9602C_PTP_CLK_FREQ_WORD_NOMINAL	0x8000000

/*
 * Each egress queue's buffer pages, now and at peak. PEAK_CLR_PORT clears the
 * per-port peaks, not these.
 */
#define RTL9602C_BUF_QUEUE_PAGES	0x02d038
#define RTL9602C_BUF_QUEUE_PAGES_CUR	GENMASK(9, 0)
#define RTL9602C_BUF_QUEUE_PAGES_PEAK	GENMASK(25, 16)
#define RTL9602C_BUF_PEAK_CLR	0x0230fc
#define RTL9602C_BUF_PEAK_CLR_PORT	BIT(0)

/*
 * The egress scheduler and shaper: strict queues first, the highest first,
 * then weighted ones by weight. Queue 0's weight is 16 bits, two ports to a
 * word; those of queues 1 to 7 are 10 bits, three to a word, three words a
 * port.
 */
#define RTL9602C_SCHED_CTRL		0x02d800
#define RTL9602C_SHAPE_PORT_CFG	0x02d804
#define RTL9602C_SCHED_WEIGHT_Q0		0x02d824
#define RTL9602C_SCHED_WEIGHT_Q1_7	0x02d82c
#define RTL9602C_SCHED_WFQ_QUEUES	0x02d85c

/*
 * Per-port and per-message PTP settings, only read; the transparent-clock
 * bits and the receive capture's layout are not verified.
 */
#define RTL9602C_PTP_TRANSPARENT_PORTS	0x0111d4
#define RTL9602C_PTP_ACTION_INGRESS	0x0111d8
#define RTL9602C_PTP_ACTION_EGRESS	0x0111dc
#define RTL9602C_PTP_RX_TSTAMP		0x0111e0
#define RTL9602C_PTP_RX_TSTAMP_SEC	GENMASK(2, 0)
#define RTL9602C_PTP_RX_TSTAMP_NSEC	GENMASK(29, 3)

/*
 * Set when a PTP message leaves the port, cleared when read; no timestamp
 * comes with it.
 */
#define RTL9602C_PTP_TX_STATUS(p)	(0x020024 + 0x400 * (p))
#define RTL9602C_PTP_TX_STATUS_LATCHED	BIT(0)
#define RTL9602C_PTP_MSG_TYPES		10

/*
 * The last egress descriptor that matched CFG; writing CFG clears VALID. MODE
 * and REASON are not verified.
 */
#define RTL9602C_PKTDBG_DATA(i)		(0x02a070 + 4 * (i))
#define RTL9602C_PKTDBG_DATA_WORDS		15
#define RTL9602C_PKTDBG_CFG		0x02a0b4
#define RTL9602C_PKTDBG_CFG_VALID		BIT(16)
#define RTL9602C_PKTDBG_CFG_REASON		GENMASK(15, 8)
#define RTL9602C_PKTDBG_CFG_MODE		GENMASK(7, 6)
#define RTL9602C_PKTDBG_CFG_INGRESS_PORT	GENMASK(5, 3)
#define RTL9602C_PKTDBG_CFG_EGRESS_PORT	GENMASK(2, 0)

/* PTP fields of the descriptor, by bit; ID and ACTION are not verified. */
#define RTL9602C_PKTDBG_PTP_SEC_LSB		92
#define RTL9602C_PKTDBG_PTP_SEC_BITS	48
#define RTL9602C_PKTDBG_PTP_NSEC_LSB		140
#define RTL9602C_PKTDBG_PTP_NSEC_BITS	32
#define RTL9602C_PKTDBG_PTP_ID_LSB		172
#define RTL9602C_PKTDBG_PTP_ID_BITS	7
#define RTL9602C_PKTDBG_PTP_ACTION_LSB		179
#define RTL9602C_PKTDBG_PTP_ACTION_BITS	2
#define RTL9602C_PKTDBG_PTP_FLAG_LSB		181

/* A message type's action in RTL9602C_PTP_ACTION_INGRESS and _EGRESS. */
#define RTL9602C_PTP_ACTION_MASK		GENMASK(1, 0)

/*
 * Port 0 enforces its shaper only with the rate here as well, in units of
 * 16 kbit/s. The token size, the bytes a weight unit stands for, reads 66.
 */
#define RTL9602C_SHAPE_2G5_CFG	0x02d8b8
#define RTL9602C_SCHED_TOKEN_SIZE	0x02d890

/* Set, weights count packets; clear, bytes. */
#define RTL9602C_SCHED_CTRL_WEIGHT_PKTS	BIT(17)

/* In units of 8 kbit/s; the maximum does not limit. */
#define RTL9602C_SHAPE_PORT_CFG_RATE		GENMASK(19, 1)
#define RTL9602C_SHAPE_PORT_CFG_RATE_MAX		0x7ffff
#define RTL9602C_SHAPE_2G5_CFG_RATE		GENMASK(18, 0)
#define RTL9602C_SCHED_TOKEN_SIZE_BYTES	GENMASK(7, 0)

/* Egress queues per port, and the largest weights. */
#define RTL9602C_QUEUES			8
#define RTL9602C_SCHED_WEIGHT_Q0_MAX		0xffff
#define RTL9602C_SCHED_WEIGHT_Q1_7_MAX		0x3ff

/*
 * The ACL: its templates, lookup and a default permit per port (lookup
 * without the permit drops every frame), and each rule's action classes,
 * where 0 chains the rule to the one before it.
 */
#define RTL9602C_ACL_TMPL_FIELD	0x015008
#define RTL9602C_ACL_PORT_EN			0x015040
#define RTL9602C_ACL_DEFAULT_PERMIT		0x015044
#define RTL9602C_ACL_ACT_EN		0x015048

/*
 * A latch per template for the ACL interrupt, which no rule the driver
 * writes raises; not verified.
 */
#define RTL9602C_ACL_HIT_STAT		0x0151c8
#define RTL9602C_ACL_HIT_STAT_TMPL	GENMASK(7, 0)

/*
 * User field selectors: a 16-bit window at OFFSET from ORIGIN, compared as
 * field code RTL9602C_ACL_TMPL_FIELD_USER0 + n. ORIGIN_IP_PAYLOAD is the
 * start of the IPv4 or IPv6 payload, behind any IPv6 extension headers
 * (measured): the window at offset 0 holds a UDP or TCP source port, or the
 * type and code of an ICMPv6 message.
 */
#define RTL9602C_FIELDSEL_CFG	0x023144
#define RTL9602C_FIELDSEL_CFG_OFFSET	GENMASK(10, 3)
#define RTL9602C_FIELDSEL_CFG_ORIGIN	GENMASK(2, 0)
#define RTL9602C_FIELDSEL_CFG_ORIGIN_IP_PAYLOAD	6
#define RTL9602C_ACL_TMPL_FIELD_USER0		0x40
#define RTL9602C_FSEL_SPORT		0
#define RTL9602C_FSEL_DPORT		1

/*
 * ACL rules and templates; template 7 matches nothing. A rule is two entries:
 * i, the bits that must be 0, and i + ONES_PLANE, those that must be 1.
 */
#define RTL9602C_ACL_ENTRIES		96
#define RTL9602C_ACL_TMPL_COUNT		7
#define RTL9602C_ACL_TMPL_SLOTS		8
#define RTL9602C_TBLACC_CMD_TABLE_ACL_RULE		2
#define RTL9602C_TBLACC_CMD_TABLE_ACL_ACT	3
#define RTL9602C_ACL_RULE_ONES_PLANE		128
#define RTL9602C_ACL_RULE_WORDS		5
#define RTL9602C_ACL_ACT_WORDS		3

/* Rule entry bits: 16-bit slots from SLOT0_LSB, and the valid bit. */
#define RTL9602C_ACL_RULE_SLOT0_LSB	16
#define RTL9602C_ACL_RULE_VALID_BIT	144

/*
 * Template field codes; a MAC address takes three consecutive codes, an IPv4
 * address two. CTAG holds the C-tag's VID in bits 11:0 and its priority in
 * bits 15:13, and 0 for a frame without a C-tag. IPV4_TOS_PROTO holds the
 * IPv4 TOS octet above the protocol,
 * IPV6_TC_NH the IPv6 traffic class above the protocol of the payload that
 * follows any extension headers: 58 for an MLD message behind its hop-by-hop
 * header (measured).
 */
#define RTL9602C_ACL_TMPL_FIELD_NONE		0x00
#define RTL9602C_ACL_TMPL_FIELD_DMAC0		0x01
#define RTL9602C_ACL_TMPL_FIELD_SMAC0		0x04
#define RTL9602C_ACL_TMPL_FIELD_ETHERTYPE	0x07
#define RTL9602C_ACL_TMPL_FIELD_CTAG		0x09
#define RTL9602C_ACL_TMPL_FIELD_IPV4_SRC0		0x10
#define RTL9602C_ACL_TMPL_FIELD_IPV4_DST0		0x12
#define RTL9602C_ACL_TMPL_FIELD_IPV4_TOS_PROTO	0x14
#define RTL9602C_ACL_TMPL_FIELD_IPV6_TC_NH	0x15

/*
 * The forwarding decision and its port mask: REDIRECT with no port drops the
 * frame; MIRROR forwards it as usual and sends a copy to the ports of the
 * mask, the CPU port's bit alone reaching the CPU; that TRAP sends it to the
 * CPU is not verified. EN_FWD is the forwarding class of RTL9602C_ACL_ACT_EN,
 * which reads EN_DEFAULT at reset. NONE, with EN_FWD clear, decides nothing:
 * a rule with it only counts.
 */
#define RTL9602C_ACL_ACT_FWD_PORTS_LSB	37
#define RTL9602C_ACL_ACT_FWD_LSB	41
#define RTL9602C_ACL_ACT_FWD_NONE	0
#define RTL9602C_ACL_ACT_FWD_REDIRECT	1
#define RTL9602C_ACL_ACT_FWD_MIRROR	2
#define RTL9602C_ACL_ACT_FWD_TRAP	3
#define RTL9602C_ACL_ACT_EN_FWD		BIT(4)
#define RTL9602C_ACL_ACT_EN_DEFAULT	0x7f

/*
 * The log class of an action: its type at LOG_LSB, 2 bits, and a counter at
 * LOG_IDX_LSB, 5 bits. Type LOG_COUNTER counts each frame the rule matches in
 * log counter LOG_IDX. EN_LOG is the class's bit of RTL9602C_ACL_ACT_EN; the
 * driver sets it only for a rule with a counter, as the class's other types
 * police.
 */
#define RTL9602C_ACL_ACT_LOG_LSB	35
#define RTL9602C_ACL_ACT_LOG_IDX_LSB	30
#define RTL9602C_ACL_ACT_LOG_COUNTER	1
#define RTL9602C_ACL_ACT_EN_LOG		BIT(3)

/*
 * The log counters: 32 counters of 32 bits, each counting what an ACL rule's
 * log class sends it. Bit k of LOG_WIDE joins counters 2k and 2k + 1 into one
 * of 64 bits, and bit k of LOG_TYPE chooses what they count.
 */
#define RTL9602C_LOG_COUNTERS		32
#define RTL9602C_LOG_CNT(i)		(0x032600 + 4 * (i))
#define RTL9602C_LOG_WIDE		0x034004
#define RTL9602C_LOG_TYPE		0x034008
#define RTL9602C_LOG_TYPE_PACKETS	0

/*
 * The classifier, whose rules are two entries as in the ACL; the lowest
 * matching entry applies. Entries from LAYOUT1_START up are matched with
 * another rule layout, taken to differ from the driver's (not verified), and
 * are not used.
 */
#define RTL9602C_TBLACC_CMD_TABLE_CLS_RULE	4
#define RTL9602C_TBLACC_CMD_TABLE_CLS_ACT	5
#define RTL9602C_CLS_RULE_ONES_PLANE		0x100
#define RTL9602C_CLS_ENTRIES		256
#define RTL9602C_CLS_CTRL			0x01600c
#define RTL9602C_CLS_CTRL_LAYOUT1_START	GENMASK(12, 5)

/*
 * The classifier's actions on the C-tag apply to the PON port's frames only
 * with PON_EN set; without it an entry still assigns a stream, but a frame
 * leaves with its tag as it came (both measured on this board).
 */
#define RTL9602C_CLS_CTRL_PON_EN		BIT(3)

/*
 * Rule word 0: direction, tags, VID, priority and ingress port. CTAG says
 * whether the frame carries a C-tag, at its front or behind an S-tag, and STAG
 * whether it carries an S-tag, which the switch recognises only on a port in
 * S-VLAN uplink mode (RTL9602C_SVLAN_UPLINK_PORTS). VID and PRI are those of
 * the tag that the entry's template takes them from, RTL9602C_CLS_TMPL_CFG
 * (all measured).
 */
#define RTL9602C_CLS_RULE_DS		BIT(31)
#define RTL9602C_CLS_RULE_CTAG	BIT(3)
#define RTL9602C_CLS_RULE_STAG	BIT(4)
#define RTL9602C_CLS_RULE_VID			GENMASK(22, 11)
#define RTL9602C_CLS_RULE_PRI			GENMASK(10, 8)
#define RTL9602C_CLS_RULE_UNI			GENMASK(2, 0)
#define RTL9602C_CLS_RULE_VLAN_CARE		(RTL9602C_CLS_RULE_CTAG | \
					 RTL9602C_CLS_RULE_STAG | \
					 RTL9602C_CLS_RULE_VID)

/* Rule word 1, in the ones plane. */
#define RTL9602C_CLS_RULE_VALID		BIT(16)

/*
 * Action word 1: assign the C-VID (CVID_SET with CVID) and the C-tag's
 * priority (CPRI_SET with CPRI) for the C-tag that TAG writes.
 */
#define RTL9602C_CLS_ACT_CVID_SET	BIT(18)
#define RTL9602C_CLS_ACT_CVID		GENMASK(14, 3)
#define RTL9602C_CLS_ACT_CPRI_SET	BIT(21)
#define RTL9602C_CLS_ACT_CPRI		GENMASK(17, 15)

/*
 * Action word 1: what becomes of the C-tag. TAG gives the frame a C-tag with
 * the VID that CVID assigns and keeps its priority bits, UNTAG takes the
 * C-tag off, and with 0 or 3 the frame leaves with the tag it came with, its
 * VID unchanged even when CVID assigns one (all measured on this board).
 */
#define RTL9602C_CLS_ACT_CTAG		GENMASK(2, 1)
#define RTL9602C_CLS_ACT_CTAG_TAG	1
#define RTL9602C_CLS_ACT_CTAG_UNTAG	2

/*
 * Action word 0: what becomes of the outer tag, with the VID and priority that
 * SVID_SET with SVID and SPRI_SET with SPRI give it. ADD puts an S-tag with the
 * S-VLAN block's TPID (RTL9602C_SVLAN_TPID_CFG_PRIMARY, 0x88a8 at reset) in
 * front of a frame without one, and rewrites the S-tag of a frame with one;
 * CTPID rewrites a frame's S-tag into a tag with the C-tag's TPID, 0x8100; POP
 * takes the S-tag off; 0 leaves the frame as it is (all measured, POP and the
 * rewrites on frames that an uplink port took in). In one test each, 4 left
 * the frame alone and 5 did as ADD.
 */
#define RTL9602C_CLS_ACT_STAG		GENMASK(2, 0)
#define RTL9602C_CLS_ACT_STAG_ADD	1
#define RTL9602C_CLS_ACT_STAG_CTPID	2
#define RTL9602C_CLS_ACT_STAG_POP	3
#define RTL9602C_CLS_ACT_SVID		GENMASK(14, 3)
#define RTL9602C_CLS_ACT_SPRI		GENMASK(17, 15)
#define RTL9602C_CLS_ACT_SPRI_SET	BIT(18)
#define RTL9602C_CLS_ACT_SVID_SET	BIT(21)

/* Action word 0, upstream only: assign the stream, the GEM flow. */
#define RTL9602C_CLS_ACT_FLOW_SET	BIT(23)
#define RTL9602C_CLS_ACT_FLOW		GENMASK(30, 24)

/* Each entry's action classes; the meanings within OTHERS are not verified. */
#define RTL9602C_CLS_ACT_EN(idx)	(0x016038 + 4 * (idx))
#define RTL9602C_CLS_ACT_EN_FLOW		BIT(5)
#define RTL9602C_CLS_ACT_EN_OTHERS	GENMASK(4, 0)

/* One bit per entry, its template, RTL9602C_CLS_TMPL_C or _S. */
#define RTL9602C_CLS_ENTRY_TMPL	0x016010

/*
 * Each template's sources of the rule's VID and PRI, upstream and downstream:
 * TAG_C takes them from the C-tag, the inner tag of a frame with an S-tag,
 * and TAG_S from the S-tag (both measured; 2 did as TAG_C and 3 matched
 * nothing, in one test each). Both read 0 out of reset. The driver gives
 * template RTL9602C_CLS_TMPL_C the C-tag and RTL9602C_CLS_TMPL_S the S-tag.
 */
#define RTL9602C_CLS_TMPL_CFG(t)	(0x016030 + 4 * (t))
#define RTL9602C_CLS_TMPL_CFG_US_VID	GENMASK(1, 0)
#define RTL9602C_CLS_TMPL_CFG_US_PRI	GENMASK(3, 2)
#define RTL9602C_CLS_TMPL_CFG_DS_VID	GENMASK(5, 4)
#define RTL9602C_CLS_TMPL_CFG_DS_PRI	GENMASK(7, 6)
#define RTL9602C_CLS_TMPL_CFG_TAG_C	0
#define RTL9602C_CLS_TMPL_CFG_TAG_S	1
#define RTL9602C_CLS_TMPL_C		0
#define RTL9602C_CLS_TMPL_S		1

/*
 * The address table: 1024 hashed entries, four ways to a bucket, then a CAM of
 * 64 for keys whose bucket is full. Word 0 holds address bits 31:0.
 */
#define RTL9602C_FDB_ENTRY_WORDS		3
#define RTL9602C_FDB_HASH_ENTRIES		1024
#define RTL9602C_FDB_ENTRIES			1088
#define RTL9602C_FDB_HASH_WAYS		4

/* Word 1: address bits 47:32 and the VID the entry is keyed by. */
#define RTL9602C_FDB_W1_MAC_HI		GENMASK(15, 0)
#define RTL9602C_FDB_W1_VID		GENMASK(27, 16)

/*
 * ROUTER_MAC is taken to hand frames for this device's address to the L3
 * engine (not verified). A STATIC entry neither ages nor goes with the
 * dynamic flush. IVL must agree with the VLAN entry's.
 */
#define RTL9602C_FDB_W1_ROUTER_MAC		BIT(29)
#define RTL9602C_FDB_W1_STATIC	BIT(30)
#define RTL9602C_FDB_W1_IVL		BIT(31)

/* Word 2 of a unicast entry: the port, and the age, counting down to 0. */
#define RTL9602C_FDB_W2_PORT		GENMASK(2, 1)
#define RTL9602C_FDB_W2_AGE		GENMASK(5, 3)

/*
 * Needed on the entry a next hop names, or the switch discards the routed
 * frame; it takes only on an entry written through the hash.
 */
#define RTL9602C_FDB_W2_NEXTHOP	BIT(9)

/*
 * Reads 1 in every hashed slot, occupied or not; written clear through the
 * hash, it deletes the entry.
 */
#define RTL9602C_FDB_W2_VALID		BIT(13)

/* The age a fresh entry starts at. */
#define RTL9602C_FDB_W2_AGE_MAX		7

/* Word 2 of a multicast entry, over the unicast fields. */
#define RTL9602C_FDB_W2_MC_PORTS		GENMASK(5, 2)
#define RTL9602C_FDB_W2_MC_EXT_PORTS		GENMASK(12, 6)
#define RTL9602C_FDB_W2_MC_EXT_PORTS_CPU		BIT(0)

/* Flush of the ports in FLUSH_PORTS; START reads 0 once it is done. */
#define RTL9602C_FDB_FLUSH_CTRL		0x017024
#define RTL9602C_FDB_FLUSH_CTRL_START		BIT(0)
#define RTL9602C_FDB_FLUSH_CTRL_MODE		GENMASK(2, 1)
#define RTL9602C_FDB_FLUSH_CTRL_MODE_PORT	0
#define RTL9602C_FDB_FLUSH_CTRL_DYNAMIC	BIT(4)
#define RTL9602C_FDB_FLUSH_PORTS		0x017028

/*
 * DoS checks: ports, enables and trap-or-drop per check, and three flood
 * thresholds, whose registers can stall the bus when read (measured on this
 * board).
 */
#define RTL9602C_DOS_PORT_EN			0x026000
#define RTL9602C_DOS_PORT_EN_MASK		GENMASK(RTL9602C_NUM_PORTS - 1, 0)
#define RTL9602C_DOS_CHECK_CTRL		0x026004
#define RTL9602C_DOS_CHECK_CTRL_EN		GENMASK(15, 0)
#define RTL9602C_DOS_CHECK_CTRL_TRAP	GENMASK(31, 16)
#define RTL9602C_DOS_SYN_FLOOD_THR	0x026008
#define RTL9602C_DOS_FIN_FLOOD_THR	0x02600c
#define RTL9602C_DOS_ICMP_FLOOD_THR	0x026010

/*
 * Storm control: each class's enable register and meter selector. That the
 * multicast, ARP, DHCP and IGMP classes police is not verified.
 */
#define RTL9602C_STORM_UNKNOWN_MCAST_EN		0x01703c
#define RTL9602C_STORM_UNKNOWN_UCAST_EN		0x017040
#define RTL9602C_STORM_MCAST_EN		0x017044
#define RTL9602C_STORM_BCAST_EN		0x017048
#define RTL9602C_STORM_ARP_EN		0x01705c
#define RTL9602C_STORM_DHCP_EN	0x017060
#define RTL9602C_STORM_IGMP_EN	0x017064

#define RTL9602C_STORM_UNKNOWN_MCAST_METER_SEL		0x01704c
#define RTL9602C_STORM_UNKNOWN_UCAST_METER_SEL		0x017050
#define RTL9602C_STORM_MCAST_METER_SEL		0x017054
#define RTL9602C_STORM_BCAST_METER_SEL		0x017058
#define RTL9602C_STORM_ARP_METER_SEL	0x017068
#define RTL9602C_STORM_DHCP_METER_SEL	0x01706c
#define RTL9602C_STORM_IGMP_METER_SEL	0x017070

#define RTL9602C_STORM_METER_SEL_PORT(p)	(0xfu << (4 * (p)))

/*
 * Sixteen meters of two words, the high one at the lower address; byte mode
 * does not police (measured on this board), and the maximum rate does not
 * limit.
 */
#define RTL9602C_METER_COUNT			16

/*
 * The egress packet-rate limit per queue: per port, one enable bit per queue
 * and a 3-bit meter index per queue, queue q at bits 3q+2:3q, which binds the
 * queue to a meter of the port's bank: meters 0 to 7 on an even port, 8 to 15
 * on an odd one, the CPU port included. A queue counts its frames against the
 * meter it names, on a user port and on the CPU port alike (both measured on
 * this board). The driver only resets them.
 */
#define RTL9602C_QUEUE_RATE_EN(p)		(0x02d86c + 4 * (p))
#define RTL9602C_QUEUE_RATE_METER(p)		(0x02d87c + 4 * (p))
#define RTL9602C_QUEUE_RATE_METER_SHIFT(q)	(3 * (q))
#define RTL9602C_METER_BANK_SIZE		8
#define RTL9602C_METER_BANK(port)		(((port) & 1) * RTL9602C_METER_BANK_SIZE)

/*
 * The limit on the frames the switch hands the CPU (rtl9602c_cpu_rate_set()):
 * the CPU port's queues that it binds, and its rate at setup in packets a
 * second. Queue 7 is left out: it carries internal priority 7, which the
 * switch gives the OMCI messages.
 */
#define RTL9602C_CPU_RATE_QUEUES		GENMASK(6, 0)
#define RTL9602C_CPU_RATE_DEFAULT		5000
#define RTL9602C_METER_CFG_HI(i)		(0x025004 + 8 * (i))
#define RTL9602C_METER_CFG_LO(i)		(0x025008 + 8 * (i))
#define RTL9602C_METER_CFG_HI_RATE		GENMASK(18, 0)
#define RTL9602C_METER_CFG_LO_BURST		GENMASK(16, 1)
#define RTL9602C_METER_CFG_LO_MODE		BIT(17)
#define RTL9602C_METER_CFG_LO_MODE_PACKET	1
#define RTL9602C_METER_CFG_HI_RATE_MAX		0x7ffff
#define RTL9602C_METER_CFG_LO_BURST_DEFAULT	0x3fff

/* A latching bit per meter, cleared by writing 1. */
#define RTL9602C_METER_EXCEEDED		0x025084

/*
 * The frame capture: MODE chooses the frame, and writing mode NONE clears
 * VALID; modes 3 and 4 are not verified.
 */
#define RTL9602C_PKTDBG_LATCH		0x028000
#define RTL9602C_PKTDBG_LATCH_MODE		GENMASK(4, 2)
#define RTL9602C_PKTDBG_LATCH_INGRESS_PORTS	GENMASK(8, 5)
#define RTL9602C_PKTDBG_LATCH_EGRESS_PORTS	GENMASK(12, 9)
#define RTL9602C_PKTDBG_LATCH_REASON	GENMASK(20, 13)
#define RTL9602C_PKTDBG_LATCH_VALID		BIT(21)
#define RTL9602C_PKTDBG_LATCH_MODE_NONE		1
#define RTL9602C_PKTDBG_LATCH_MODE_MAX		7

/* The captured parse and verdict. */
#define RTL9602C_PKTDBG_PARSE(i)		(0x028040 + 4 * (i))
#define RTL9602C_PKTDBG_VERDICT(i)		(0x028148 + 4 * (i))
#define RTL9602C_PKTDBG_PARSE_WORDS		28
#define RTL9602C_PKTDBG_VERDICT_WORDS		13
#define RTL9602C_PKTDBG_SNAPSHOT_WORDS \
	(1 + RTL9602C_PKTDBG_PARSE_WORDS + RTL9602C_PKTDBG_VERDICT_WORDS)

/*
 * Controls for the reserved group addresses 01-80-C2-00-00-xx and the CDP
 * and SSTP ones; only _00 is verified to serve the address it names.
 */
#define RTL9602C_RSVDMC_CTRL_00		0x01c038
#define RTL9602C_RSVDMC_CTRL_01		0x01c03c
#define RTL9602C_RSVDMC_CTRL_02		0x01c040
#define RTL9602C_RSVDMC_CTRL_03		0x01c044
#define RTL9602C_RSVDMC_CTRL_04		0x01c048
#define RTL9602C_RSVDMC_CTRL_08		0x01c04c
#define RTL9602C_RSVDMC_CTRL_0D		0x01c050
#define RTL9602C_RSVDMC_CTRL_0E		0x01c054
#define RTL9602C_RSVDMC_CTRL_10		0x01c058
#define RTL9602C_RSVDMC_CTRL_11		0x01c05c
#define RTL9602C_RSVDMC_CTRL_12		0x01c060
#define RTL9602C_RSVDMC_CTRL_13		0x01c064
#define RTL9602C_RSVDMC_CTRL_18		0x01c068
#define RTL9602C_RSVDMC_CTRL_1A		0x01c06c
#define RTL9602C_RSVDMC_CTRL_20		0x01c070
#define RTL9602C_RSVDMC_CTRL_21		0x01c074
#define RTL9602C_RSVDMC_CTRL_22		0x01c078
#define RTL9602C_RSVDMC_CTRL_CDP		0x01c07c
#define RTL9602C_RSVDMC_CTRL_SSTP		0x01c080
#define RTL9602C_RSVDMC_GLOBAL_CFG		0x01c084

/* Fields of each control; only STORM_EXEMPT is verified. */
#define RTL9602C_RSVDMC_CTRL_ACTION		GENMASK(5, 4)
#define RTL9602C_RSVDMC_CTRL_STORM_EXEMPT BIT(3)
#define RTL9602C_RSVDMC_CTRL_KEEP_TAG	BIT(2)
#define RTL9602C_RSVDMC_CTRL_VLAN_BYPASS		BIT(1)
#define RTL9602C_RSVDMC_CTRL_ISOLATE_BYPASS	BIT(0)

/*
 * The S-VLAN block. A port in RTL9602C_SVLAN_UPLINK_PORTS recognises an S-tag
 * on the frames coming in, which then join the S-VLAN of the tag; a frame
 * without one is dealt with as RTL9602C_SVLAN_POLICY_UNTAGGED_ACT says. A
 * frame from any other port joins the port's S-VLAN, RTL9602C_SVLAN_PVID. A
 * frame leaving by an uplink port carries the S-tag of its S-VLAN unless the
 * port is in the untagged set of that VID's VLAN table entry, which with the
 * member set is also the S-VLAN's (all measured). RTL9602C_SVLAN_PVID_NONE is
 * the S-VLAN the driver gives frames without an S-tag: its entry has every
 * port a member and untagged, so the block passes them as they came
 * (measured). It is a VID that IEEE 802.1Q reserves (clause 9.6, Table 9-2),
 * so no bridge VLAN can share the entry. The other fields are not verified
 * beyond their names.
 */
#define RTL9602C_SVLAN_DMAC_VID_SEL	0x014000
#define RTL9602C_SVLAN_PVID(p)	(0x014004 + 4 * ((p) / 2))
#define RTL9602C_SVLAN_PVID_SHIFT(p)	(12 * ((p) % 2))
#define RTL9602C_SVLAN_PVID_MASK	GENMASK(11, 0)
#define RTL9602C_SVLAN_PVID_NONE	4095
#define RTL9602C_SVLAN_POLICY		0x01400c
#define RTL9602C_SVLAN_POLICY_INGRESS_FILTER	BIT(27)
#define RTL9602C_SVLAN_POLICY_KEEP_DEI		BIT(21)
#define RTL9602C_SVLAN_POLICY_PCP		GENMASK(20, 18)

/*
 * What becomes of a frame without an S-tag that comes in by an uplink port:
 * with 0, the value out of reset, or 1 it is dropped; with JOIN it joins the
 * port's S-VLAN, and 3 did the same in one test (all measured).
 */
#define RTL9602C_SVLAN_POLICY_UNTAGGED_ACT		GENMASK(3, 2)
#define RTL9602C_SVLAN_POLICY_UNTAGGED_JOIN		2
#define RTL9602C_SVLAN_POLICY_PCP_SRC		GENMASK(1, 0)
#define RTL9602C_SVLAN_UPLINK_PORTS	0x0230c4
#define RTL9602C_SVLAN_TPID_CFG		0x0230c8
#define RTL9602C_SVLAN_TPID_CFG_SECOND_EN		BIT(0)
#define RTL9602C_SVLAN_TPID_CFG_SECOND		GENMASK(31, 16)
#define RTL9602C_SVLAN_TPID_CFG_PRIMARY		GENMASK(15, 0)
#define RTL9602C_SVLAN_XLATE(i)		(0x02a018 + 4 * (i))
#define RTL9602C_SVLAN_XLATE_ENTRIES	16
#define RTL9602C_SVLAN_XLATE_PRIO		GENMASK(29, 27)
#define RTL9602C_SVLAN_XLATE_CVID		GENMASK(26, 15)
#define RTL9602C_SVLAN_XLATE_PORT		GENMASK(14, 13)
#define RTL9602C_SVLAN_XLATE_SVID		GENMASK(12, 1)
#define RTL9602C_SVLAN_XLATE_VALID		BIT(0)

/*
 * Loop detection by probes that carry a 48-bit magic; the latches are cleared
 * by writing 1. PEER_PORT is verified only for a loop closing through the
 * port it left.
 */
#define RTL9602C_LOOPDET_CTRL		0x01a000
#define RTL9602C_LOOPDET_CTRL_EN		BIT(0)
#define RTL9602C_LOOPDET_PROBE_CFG		0x01a004
#define RTL9602C_LOOPDET_PROBE_CFG_PERIOD	GENMASK(23, 8)
#define RTL9602C_LOOPDET_MAGIC_HI		0x01a014
#define RTL9602C_LOOPDET_MAGIC_LO		0x01a018
#define RTL9602C_LOOPDET_TX_PORTS		0x01a01c
#define RTL9602C_LOOPDET_DETECTED		0x01a020
#define RTL9602C_LOOPDET_CLEARED		0x01a024
#define RTL9602C_LOOPDET_PEER_PORT		0x01a02c
#define RTL9602C_LOOPDET_PEER_PORT_BITS	3

/* The verdict of the chip's power-on memory self-test. */
#define RTL9602C_CHIP_MEM_SELFTEST		0x0310b8
#define RTL9602C_CHIP_MEM_SELFTEST_DONE		BIT(0)
#define RTL9602C_CHIP_MEM_SELFTEST_RETAIN_DONE		BIT(1)
#define RTL9602C_CHIP_MEM_SELFTEST_FAIL		BIT(3)
#define RTL9602C_CHIP_MEM_SELFTEST_RETAIN_FAIL		BIT(4)

/* Per port, each internal priority's egress queue. */
#define RTL9602C_QOS_PRIO_TO_QUEUE	0x01c0b0

/*
 * The egress PCP remark: one map for the whole switch, 3 bits per index, and
 * per port, bit p, an enable and the index, the frame's internal priority (0)
 * or its own PCP (1). A port with the enable set writes the map's PCP into
 * every tagged frame it sends; an untagged one stays untagged.
 */
#define RTL9602C_QOS_REMARK_EN		0x02a058
#define RTL9602C_QOS_REMARK_MAP	0x02a05c
#define RTL9602C_QOS_REMARK_MAP_SHIFT(i)	(3 * (i))
#define RTL9602C_QOS_REMARK_SRC		0x02a06c

/*
 * Priority selection: sources weighted 0 to 7, the heaviest taken to decide
 * (not verified); SEL_IDX names the weight table in force.
 */
#define RTL9602C_QOS_DSCP_TO_PRIO		0x01c0d4
#define RTL9602C_QOS_PORT_PRIO		0x01c0f0
#define RTL9602C_QOS_PRIO_WEIGHT	0x01c0f4
#define RTL9602C_QOS_PRIO_WEIGHT_SEL	0x01c0fc
#define RTL9602C_QOS_PRIO_WEIGHT_SEL_IDX	BIT(0)

/* The weight sources, 3 bits each; PORT's and PCP's are not verified. */
#define RTL9602C_QOS_PRIO_WEIGHT_PORT		0
#define RTL9602C_QOS_PRIO_WEIGHT_PCP		1
#define RTL9602C_QOS_PRIO_WEIGHT_DSCP		2
#define RTL9602C_QOS_PRIO_WEIGHT_MAX		7

/* Priorities, DSCPs, and the maps' packing. */
#define RTL9602C_QOS_PRIO_COUNT		8
#define RTL9602C_QOS_DSCP_COUNT		64
#define RTL9602C_QOS_MAP_PER_WORD	10
#define RTL9602C_QOS_MAP_ENTRY_BITS	3

/* The sources dcb apptrust can order: PCP and DSCP. */
#define RTL9602C_APPTRUST_MAX		2

/*
 * The I2C master the optics are on, the second of two. Without BUS_EN a
 * transfer runs without driving the bus.
 */
#define RTL9602C_I2C_OPTICS_MASTER		1
#define RTL9602C_I2C_XFER_CFG		(0x023004 + 4 * RTL9602C_I2C_OPTICS_MASTER)
#define RTL9602C_I2C_XFER_CFG_BUS_EN		BIT(21)
#define RTL9602C_I2C_XFER_CFG_DEV_ADDR	GENMASK(20, 14)
#define RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH	GENMASK(13, 12)
#define RTL9602C_I2C_XFER_CFG_DATA_WIDTH	GENMASK(11, 10)
#define RTL9602C_I2C_XFER_CFG_CLK_DIV	GENMASK(9, 0)
#define RTL9602C_I2C_TX_DATA		(0x0000b0 + 4 * RTL9602C_I2C_OPTICS_MASTER)
#define RTL9602C_I2C_SUBADDR		(0x0000b8 + 4 * RTL9602C_I2C_OPTICS_MASTER)
#define RTL9602C_I2C_CMD		(0x0000c0 + 4 * RTL9602C_I2C_OPTICS_MASTER)
#define RTL9602C_I2C_RX_DATA		(0x0000c8 + 4 * RTL9602C_I2C_OPTICS_MASTER)
#define RTL9602C_I2C_CMD_NACK		BIT(3)
#define RTL9602C_I2C_CMD_BUSY		BIT(2)
#define RTL9602C_I2C_CMD_WRITE		BIT(1)
#define RTL9602C_I2C_CMD_START		BIT(0)

/* Pad functions; the master ignores its commands until FUNC_EN_I2C is set. */
#define RTL9602C_PADMUX_FUNC_EN		0x023018
#define RTL9602C_PADMUX_FUNC_EN_I2C	BIT(13 + RTL9602C_I2C_OPTICS_MASTER)

/*
 * A one-byte register address, and one data byte or, with WIDTH_16BIT, two
 * (2 and 3 move four); a read leaves them in RX_DATA in the order they
 * came, the first in bits 7:0 (measured). SCL runs at 31.25 MHz /
 * (CLK_DIV + 1) (measured), so 311 gives 100 kHz, I2C's standard mode.
 */
#define RTL9602C_I2C_XFER_CFG_WIDTH_8BIT	0
#define RTL9602C_I2C_XFER_CFG_WIDTH_16BIT	1
#define RTL9602C_I2C_XFER_CFG_CLK_DIV_100KHZ	311

/*
 * The optics' SFF-8472 bytes as the driver numbers them, A2h from A2_BASE:
 * the thresholds, the diagnostics, and bytes 123 to 126, left to the
 * module maker.  Each of the five diagnostics, numbered as in enum
 * rtl9602c_optics_ddm, has a word in A2h 96-105 and four in A2h 0-39: its
 * high alarm, low alarm, high warning and low warning thresholds.
 */
#define RTL9602C_OPTICS_A2_BASE		0x100
#define RTL9602C_OPTICS_A2_LIMIT(ddm, limit)	(8 * (ddm) + 2 * (limit))
#define RTL9602C_OPTICS_A2_DDM		0x60
#define RTL9602C_OPTICS_A2_WORD(ddm)		(RTL9602C_OPTICS_A2_DDM + 2 * (ddm))
#define RTL9602C_OPTICS_A2_PASSWORD		0x7b

/**
 * enum rtl9602c_optics_ddm - the diagnostics of SFF-8472, in A2h order
 * @RTL9602C_OPTICS_TEMP: temperature, signed, 1/256 degC
 * @RTL9602C_OPTICS_VCC: supply voltage, 100 uV
 * @RTL9602C_OPTICS_BIAS: laser bias current, 2 uA
 * @RTL9602C_OPTICS_TX: transmitted optical power, 0.1 uW
 * @RTL9602C_OPTICS_RX: received optical power, 0.1 uW
 */
enum rtl9602c_optics_ddm {
	RTL9602C_OPTICS_TEMP,
	RTL9602C_OPTICS_VCC,
	RTL9602C_OPTICS_BIAS,
	RTL9602C_OPTICS_TX,
	RTL9602C_OPTICS_RX,
};

/**
 * enum rtl9602c_optics_limit - the thresholds of a diagnostic, in A2h order
 * @RTL9602C_OPTICS_HIGH_ALARM: high alarm
 * @RTL9602C_OPTICS_LOW_ALARM: low alarm
 * @RTL9602C_OPTICS_HIGH_WARN: high warning
 * @RTL9602C_OPTICS_LOW_WARN: low warning
 */
enum rtl9602c_optics_limit {
	RTL9602C_OPTICS_HIGH_ALARM,
	RTL9602C_OPTICS_LOW_ALARM,
	RTL9602C_OPTICS_HIGH_WARN,
	RTL9602C_OPTICS_LOW_WARN,
};

/*
 * A set bit routes its pad to the GPIO function; bit 13 is TX_DISABLE's.
 * Not verified.
 */
#define RTL9602C_PADMUX_GPIO_EN_TX_DISABLE		13
#define RTL9602C_PADMUX_GPIO_EN		0x48

#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/if_bridge.h>
#include <net/dsa.h>
#include <linux/net_tstamp.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

struct pon_omci_port;
struct regmap;

/**
 * struct rtl9602c_lookup - the result of an indirect table operation
 * @hit: the slot reached holds an entry (a read) or took the entry (a
 *	hashed write)
 * @cam: the slot is in the address table's CAM rather than a hash bucket;
 *	recorded, not used
 * @addr: the slot's index
 */
struct rtl9602c_lookup {
	bool hit;
	bool cam;
	unsigned int addr;
};

/**
 * struct rtl9602c_l34_neigh_ev - a neighbour update queued for the work item
 * @list: its place in rtl9602c_priv.l34_neigh_q
 * @dev: the neighbour's device, held until the event is applied
 * @addr: the neighbour's IPv4 address
 * @add: the neighbour was in a valid state
 *
 * The neighbour notifier can run in softirq, and the table writes sleep, so
 * the event is queued and applied under flow_lock.
 */
struct rtl9602c_l34_neigh_ev {
	struct list_head	list;
	struct net_device	*dev;
	__be32			addr;
	bool			add;
};

/* Flags of struct rtl9602c_l34_route. */
#define RTL9602C_L34_RT_CPU	BIT(0)	/* for the CPU: Linux decides */
#define RTL9602C_L34_RT_DROP	BIT(1)	/* blackhole: dropped */

/**
 * struct rtl9602c_l34_route - a route in the shadow of the routing table
 * @dst: the destination prefix
 * @gw: the gateway, 0 for an on-link route
 * @ifindex: the nexthop's device
 * @port: the switch port that device is or runs over, -1 for none
 * @prefix: the prefix length
 * @flags: %RTL9602C_L34_RT_CPU or %RTL9602C_L34_RT_DROP
 * @linkdown: the nexthop is dead or has no carrier; its row goes to the CPU
 *	until the nexthop is usable again
 * @used: the shadow slot holds a route
 */
struct rtl9602c_l34_route {
	__be32	dst;
	__be32	gw;
	int	ifindex;
	s8	port;
	u8	prefix;
	u8	flags;
	bool	linkdown;
	bool	used;
};

/**
 * struct rtl9602c_l34_fib_ev - a route event queued for the work item
 * @list: its place in rtl9602c_priv.l34_fib_q
 * @route: what the row needs, copied out in the notifier
 * @add: the route was added or replaced, or the nexthop became usable
 * @nh: a nexthop event: @route holds only its device and gateway
 *
 * The route notifier can be called under rcu_read_lock(), and the table
 * writes sleep, so the event is queued and applied under flow_lock.
 */
struct rtl9602c_l34_fib_ev {
	struct list_head		list;
	struct rtl9602c_l34_route	route;
	bool				add;
	bool				nh;
};

/**
 * struct rtl9602c_pppoe_wan - a PPPoE WAN of the L3/NAT engine
 * @extip: the PPP interface's address, which keys the WAN; 0 when free
 * @refs: the flows using it
 * @sid: its PPPoE session
 * @port: the switch port the session leaves by
 * @extidx: its external-address row
 * @took_extip: that row was taken for the WAN and is freed with it
 * @dmac: the concentrator's MAC address
 *
 * Its index w in rtl9602c_priv.l34_pppoe, 1 to 7, is also its row in the
 * L3-interface, next-hop and PPPoE session tables.
 */
struct rtl9602c_pppoe_wan {
	__be32		extip;
	unsigned int	refs;
	u16		sid;
	int		port;
	int		extidx;
	bool		took_extip;
	u8		dmac[ETH_ALEN];
};

/**
 * struct rtl9602c_egress - the egress qdiscs a port follows
 * @tbf: the handle of the tbf that sets the port's shaper, 0 for none
 * @ets: the handle of the ets that sets its scheduler, 0 for none
 * @qid_reset: the port's priority-to-queue word before the first ets
 * @qid_saved: @qid_reset holds it
 */
struct rtl9602c_egress {
	u32	tbf;
	u32	ets;
	u32	qid_reset;
	bool	qid_saved;
};

/**
 * struct rtl9602c_priv - the driver's state for one RTL9602C switch
 *
 * Shared by the files of the driver; each member is described where it is
 * declared. The locks nest with table_lock innermost, taken around each
 * access to either table port.
 */
struct rtl9602c_priv {
	/** @ds: the switch as DSA knows it; ds.priv points back here */
	struct dsa_switch ds;
	/** @dev: the switch's platform device */
	struct device *dev;
	/** @reg: the switch's register window */
	void __iomem *reg;

	/**
	 * @tx_disable: the laser's TX_DISABLE line, requested asserted; NULL
	 * when the device tree gives none
	 */
	struct gpio_desc *tx_disable;

	/**
	 * @table_lock: serialises the indirect table engine and the L3/NAT
	 * engine's table port, from a command to its result
	 */
	struct mutex table_lock;

	/**
	 * @fdb_lock: makes the read, change and write of a multicast group one
	 * operation, and serialises the flush
	 */
	struct mutex fdb_lock;

	/** @ploam_lock: serialises the writers of upstream PLOAM messages */
	struct mutex ploam_lock;

	/** @ploam_log: the ploamlog ring of PLOAM channel events */
	struct rtl9602c_ploam_ev {
		/** @ploam_log.ns: time of the event or of its last repeat */
		u64 ns;
		/**
		 * @ploam_log.aux: its detail: the slot and queue flags of an
		 * upstream message, the state left, the queue flags, or the
		 * latch read
		 */
		u32 aux;
		/** @ploam_log.repeat: how often it repeated after the first */
		u16 repeat;
		/** @ploam_log.kind: an RTL9602C_EV_* value */
		u8 kind;
		/** @ploam_log.state: the activation state at the event */
		u8 state;
		/** @ploam_log.msg: the message, or zeros */
		u8 msg[RTL9602C_GPON_PLOAM_WORDS * 2];
	} ploam_log[RTL9602C_PLOAM_LOG_LEN];
	/**
	 * @ploam_log_next: the events logged; the next goes to this index
	 * modulo RTL9602C_PLOAM_LOG_LEN
	 */
	unsigned int ploam_log_next;
	/** @ploam_usq_last: the upstream queues' empty flags last recorded */
	u32 ploam_usq_last;
	/** @ploam_log_lock: protects @ploam_log and @ploam_log_next */
	spinlock_t ploam_log_lock;

	/**
	 * @gpon_lock: serialises the GPON MAC: PLOAM dispatch from the
	 * interrupt thread and the worker, the Alloc-ID and Port-ID tables,
	 * bring-up and take-down, and the status report
	 */
	struct mutex gpon_lock;

	/** @gpon_irq_ns: time of the last GPON interrupt */
	u64 gpon_irq_ns;
	/** @gpon_from_irq: the PLOAM in dispatch came from the interrupt */
	bool gpon_from_irq;

	/** @gpon_alloc: the Alloc-ID bound to each of T-CONTs 0 to 15 */
	u16 gpon_alloc[RTL9602C_GPON_DEFAULT_TCONT_IDX];
	/**
	 * @gpon_tcont_used: which of T-CONTs 0 to 15 hold an Alloc-ID: the
	 * driver's own record, as the table keeps its entries through a reset
	 */
	unsigned long gpon_tcont_used;
	/**
	 * @gpon_tcont_map: the Alloc-ID management mapped each GEM T-CONT to
	 * (tcont_set), RTL9602C_GPON_ALLOC_NONE for none; kept across
	 * bring-ups, as the OMCI MIB is
	 */
	u16 gpon_tcont_map[RTL9602C_GEM_NUM_TCONT];
	/** @gpon_assigned: the Alloc-IDs the OLT assigned with Assign_Alloc-ID */
	DECLARE_BITMAP(gpon_assigned, RTL9602C_GPON_NUM_ALLOC);
	/** @gpon_omcc_bound: the OMCC has a Port-ID (Configure_Port-ID) */
	bool gpon_omcc_bound;

	/**
	 * @gpon_overhead: the last Upstream_Overhead message, which the burst
	 * overhead length is computed from
	 */
	u8 gpon_overhead[RTL9602C_GPON_PLOAM_BYTES];

	/** @gpon_t3_preranged: type 3 preamble bytes before ranging */
	u8 gpon_t3_preranged;
	/** @gpon_t3_ranged: type 3 preamble bytes from O5 on */
	u8 gpon_t3_ranged;

	/**
	 * @gpon_bwm_o4: the allocations the ONU was ranged on, captured from
	 * Assign_ONU-ID to Ranging_Time
	 */
	u32 gpon_bwm_o4[RTL9602C_GPON_BWM_O4_ENTRIES * 2];
	/** @gpon_bwm_o4_valid: @gpon_bwm_o4 holds a capture */
	bool gpon_bwm_o4_valid;
	/** @gpon_eqd_delay: the equalization delay last written, in bits */
	u32 gpon_eqd_delay;

	/**
	 * @gpon_ebl_seen: Extended_Burst_Length came after Upstream_Overhead
	 */
	bool gpon_ebl_seen;
	/**
	 * @gpon_sn_seen: a serial-number request was taken in O3, after which
	 * Extended_Burst_Length is no longer acted on
	 */
	bool gpon_sn_seen;

	/** @gpon_sn: the ONU's serial number */
	u8 gpon_sn[RTL9602C_GPON_SN_BYTES];
	/** @gpon_have_sn: @gpon_sn is set; the PON cannot come up before */
	bool gpon_have_sn;
	/** @gpon_password: the password the ONU gives when the OLT asks */
	u8 gpon_password[RTL9602C_GPON_PASSWORD_BYTES];
	/** @gpon_have_password: @gpon_password is set */
	bool gpon_have_password;

	/** @gpon_state: the activation state, an RTL9602C_ONU_O* value */
	u8 gpon_state;
	/** @gpon_onu_id: the ONU-ID, or RTL9602C_ONU_ONUID_UNASSIGNED */
	u8 gpon_onu_id;

	/** @gpon_stats: the GTC counters' totals since the PON came up */
	struct pon_dev_stats gpon_stats;
	/** @gpon_bip_blocks: the frames whose BIP disagreed, likewise */
	u64 gpon_bip_blocks;
	/** @gpon_fec_frame: the superframe count at the last read */
	u32 gpon_fec_frame;
	/** @gpon_fec_frame_valid: whether @gpon_fec_frame holds a read */
	bool gpon_fec_frame_valid;

	/** @gpon_status: the status last reported, counters zeroed */
	struct pon_dev_status gpon_status;

	/** @gpon_up: the PON is up, brought up with the pon interface */
	bool gpon_up;
	/** @gpon_to1: when TO1 expires, in jiffies; 0 while it is not armed */
	unsigned long gpon_to1;
	/** @gpon_to2: when TO2 expires, in jiffies; 0 while it is not armed */
	unsigned long gpon_to2;
	/** @gpon_o5_since: when the ONU entered O5, in jiffies */
	unsigned long gpon_o5_since;
	/**
	 * @gpon_rei_every: the BER interval in jiffies; 0 while the OLT has
	 * set none and no REI is sent
	 */
	unsigned long gpon_rei_every;
	/** @gpon_rei_next: when the next REI is due, in jiffies */
	unsigned long gpon_rei_next;
	/** @gpon_rei_bip: the BIP error total when the last REI was sent */
	u64 gpon_rei_bip;
	/** @gpon_rei_seq: the sequence number of the next REI */
	u8 gpon_rei_seq;
	/** @gpon_rei_sent: the REI messages sent since the ONU entered O5 */
	u32 gpon_rei_sent;
	/** @gpon_ploam_ignored: the downstream identifiers already logged */
	u32 gpon_ploam_ignored;
	/**
	 * @gpon_pee: the OLT reported a physical equipment error, and the PEE
	 * alarm is raised
	 */
	bool gpon_pee;
	/** @gpon_pee_last: when the last Physical_Equipment_Error came */
	unsigned long gpon_pee_last;
	/** @gpon_pst: the line number, K1 and K2 of the last PST answered */
	u8 gpon_pst[RTL9602C_PLOAM_PST_BYTES];
	/** @gpon_pst_seen: a PST has been answered since the ONU entered O5 */
	bool gpon_pst_seen;
	/**
	 * @gpon_power_mode: the transmit power level mode the ONU is to send
	 * at, 0 normal to 2 (ITU-T G.984.2 clause 8.3.2); the optics follow it
	 * in optics_level_work, and Serial_Number_ONU declares it
	 */
	u8 gpon_power_mode;
	/**
	 * @gpon_flow_port: the Port-ID each flow is bound to in the Port-ID
	 * table, or RTL9602C_GPON_PORTID_NONE
	 */
	u16 gpon_flow_port[RTL9602C_GPON_NUM_FLOW];
	/** @gpon_aes_ports: the Port-IDs the OLT has encrypted */
	DECLARE_BITMAP(gpon_aes_ports, RTL9602C_GPON_NUM_PORTID);
	/** @gpon_key_index: the index of the last key sent, 0 for none */
	u8 gpon_key_index;
	/**
	 * @gpon_key_switch: the superframe of the last Key_Switching_Time, 0
	 * after a deactivation
	 */
	u32 gpon_key_switch;
	/** @gpon_work: the GPON worker, run every RTL9602C_GPON_POLL_MS */
	struct delayed_work gpon_work;

	/**
	 * @sysctl: the system controller, which holds the PON MAC's gate; NULL
	 * when the PON MAC is not available
	 */
	struct regmap *sysctl;
	/** @pbo_base: physical start of each half's DRAM region */
	phys_addr_t pbo_base[2];
	/** @ponmac_up: the PON MAC is built; nothing touches it before */
	bool ponmac_up;

	/**
	 * @l34_up: the L3/NAT engine's enable read back set; everything that
	 * hands the engine frames or rows waits for it
	 */
	bool l34_up;

	/**
	 * @l34_nh_ports: the ports an L3 next hop lives behind, which every
	 * other user port may reach
	 */
	u32 l34_nh_ports;

	/**
	 * @flood_req: each port's flood flags as the bridge asked for them,
	 * applied only while the port has a bridge peer
	 */
	unsigned long flood_req[RTL9602C_NUM_PORTS];
	/**
	 * @mc_drop: the unregistered multicast the PON port drops, under
	 * @fdb_lock: BIT(0) for IPv4 and BIT(1) for IPv6
	 */
	u8 mc_drop;

	/**
	 * @l34_netif_ifindex: the device of each L3-interface row by ifindex, 0
	 * for a free row and -1 for one a PPPoE WAN holds
	 */
	int l34_netif_ifindex[16];
	/** @l34_extip: each external-address row's address, 0 when free */
	__be32 l34_extip[8];

	/** @l34_pppoe: the PPPoE WANs by row; row 0 is the Ethernet uplink's */
	struct rtl9602c_pppoe_wan l34_pppoe[8];

	/**
	 * @l34_extip_nh: the next-hop row the external-address rows of the
	 * addresses and of the flows name, the uplink's gateway's
	 */
	unsigned int l34_extip_nh;
	/** @l34_us_flow: the GEM flow routed frames leave in, -1 for none */
	int l34_us_flow;
	/**
	 * @l34_us_refs: the references on @l34_us_flow, one for each device
	 * over it that has an address
	 */
	unsigned int l34_us_refs;
	/** @l34_inetaddr_nb: the IPv4 address notifier */
	struct notifier_block l34_inetaddr_nb;
	/**
	 * @l34_netdev_nb: the netdevice notifier, for the MAC address and the
	 * MTU of a device that holds an L3-interface row
	 */
	struct notifier_block l34_netdev_nb;

	/** @l34_netevent_nb: the neighbour notifier */
	struct notifier_block l34_netevent_nb;
	/** @l34_neigh_work: applies the queued neighbour events */
	struct work_struct l34_neigh_work;

	/** @l34_neigh_lock: protects @l34_neigh_q */
	spinlock_t l34_neigh_lock;
	/** @l34_neigh_q: neighbour events waiting for @l34_neigh_work */
	struct list_head l34_neigh_q;
	/**
	 * @l34_nh_addr: the neighbour address of each next-hop row, 0 for a
	 * free row; a row a PPPoE WAN holds has an address no neighbour can
	 * have
	 */
	__be32 l34_nh_addr[16];
	/**
	 * @l34_nh_mac: the address of the address-table entry that each
	 * next-hop row names, zero for none
	 */
	u8 l34_nh_mac[16][ETH_ALEN];
	/** @l34_nh_vid: the VLAN that entry is filed under */
	u16 l34_nh_vid[16];

	/** @l34_fib_nb: the route notifier */
	struct notifier_block l34_fib_nb;
	/** @l34_fib_work: applies the queued route events */
	struct work_struct l34_fib_work;

	/** @l34_fib_lock: protects @l34_fib_q */
	spinlock_t l34_fib_lock;
	/** @l34_fib_q: route events waiting for @l34_fib_work */
	struct list_head l34_fib_q;
	/** @l34_route: the shadow of the main routing table */
	struct rtl9602c_l34_route l34_route[32];
	/**
	 * @l34_route_hw: the route rows last written, to skip unchanged ones
	 */
	u32 l34_route_hw[16][2];
	/**
	 * @l34_route_over: a route did not fit @l34_route: hardware routing
	 * stays off until the driver is reloaded
	 */
	bool l34_route_over;
	/**
	 * @l34_route_warned: the route table is written empty and the warning
	 * has been given
	 */
	bool l34_route_warned;

	/**
	 * @flow_lock: serialises the L3/NAT bookkeeping: rows, routes, flows
	 * and PPPoE WANs
	 */
	struct mutex flow_lock;

	/**
	 * @l34_trf: NAPT hit bits read and not yet taken: a read clears 32
	 * entries' bits at once
	 */
	u32 l34_trf[RTL9602C_NAPT_HIT_WORDS];
	/** @l34_flow: the flows the engine carries, one slot each */
	struct {
		/** @l34_flow.cookie: the flow rule's cookie */
		unsigned long	cookie;
		/** @l34_flow.napt: its outbound NAPT entry */
		u16		napt;
		/** @l34_flow.naptr: its inbound NAPT entry */
		u16		naptr;
		/** @l34_flow.wan: its PPPoE WAN, 0 for none */
		u8		wan;
		/** @l34_flow.ports: the ports whose flow blocks offered it */
		u8		ports;
		/** @l34_flow.used: the slot holds a flow */
		bool		used;
	} l34_flow[8];

	/** @conduit: the CPU port's conduit, which the OMCI messages cross */
	struct net_device *conduit;
	/** @omci_port: the PON class's OMCI port; NULL when not created */
	struct pon_omci_port *omci_port;
	/** @pon: the data path as a PON device; NULL when not registered */
	struct pon_device *pon;
	/** @gem_flows: the flows GEM ports hold */
	DECLARE_BITMAP(gem_flows, RTL9602C_GPON_NUM_FLOW);
	/** @gem_cfg: each GEM port's configuration, by flow */
	struct pon_gem_config gem_cfg[RTL9602C_GPON_NUM_FLOW];

	/**
	 * @gem_tx_octets: the octets each GEM port has sent upstream, kept here
	 * as the counter clears when read
	 */
	u64 gem_tx_octets[RTL9602C_GPON_NUM_FLOW];
	/** @gem_tx_frames: the GEM frames each GEM port has sent upstream */
	u64 gem_tx_frames[RTL9602C_GPON_NUM_FLOW];
	/** @gem_rx_frames: the GEM frames each GEM port has received */
	u64 gem_rx_frames[RTL9602C_GPON_NUM_FLOW];
	/** @gem_rx_octets: the payload octets each GEM port has received */
	u64 gem_rx_octets[RTL9602C_GPON_NUM_FLOW];
	/** @gem_cntr_polls: the worker's runs since the totals were last taken */
	unsigned int gem_cntr_polls;
	/** @gpon_omcc_port: the OMCC's Port-ID while @gpon_omcc_bound */
	u16 gpon_omcc_port;

	/** @pon_br: the bridge the GEM interfaces are in; NULL for none */
	struct net_device *pon_br;
	/** @pon_br_members: the GEM interfaces in @pon_br */
	unsigned int pon_br_members;
	/**
	 * @pon_br_us_flow: the flow of the last upstream GEM interface to join
	 * @pon_br, or once it has left of the upstream GEM interface of the
	 * lowest flow in @pon_br, which bridged frames are given while no VLAN
	 * claims one; -1 for none
	 */
	int pon_br_us_flow;

	/**
	 * @gem_vid_flow: per VID and IEEE 802.1p priority, the flow plus one
	 * of the upstream GEM interface that claims the VID's frames of that
	 * priority; 0 for none. VLAN_N_VID rows of eight, allocated at probe.
	 */
	u8 (*gem_vid_flow)[8];
	/** @cf_sid_owned: the classifier entries that hold stream entries */
	DECLARE_BITMAP(cf_sid_owned, RTL9602C_CLS_ENTRIES);
	/** @gem_bridged: the flows whose GEM interfaces are in @pon_br */
	DECLARE_BITMAP(gem_bridged, RTL9602C_GPON_NUM_FLOW);

	/** @cf_sid: each port's port-keyed stream entry, -1 for none */
	s16 cf_sid[RTL9602C_NUM_PORTS];
	/**
	 * @cf_sid_flow: the flow each port's port-keyed stream entry gives,
	 * -1 for none
	 */
	s16 cf_sid_flow[RTL9602C_NUM_PORTS];

	/** @mib_lock: protects @mib_total and @mib_last */
	spinlock_t mib_lock;
	/** @mib_total: each port's counters, as 64-bit totals */
	u64 mib_total[RTL9602C_NUM_PORTS][RTL9602C_MIB_NUM];
	/** @mib_last: the 32-bit counters as last read */
	u32 mib_last[RTL9602C_NUM_PORTS][RTL9602C_MIB_NUM];
	/** @mib_work: folds every port's counters before one can wrap */
	struct delayed_work mib_work;

	/**
	 * @mtu: each port's MTU, 0 for a port never given one; the two
	 * frame-length limits follow the two largest
	 */
	unsigned int mtu[RTL9602C_NUM_PORTS];

	/** @sds_mode: the SerDes lane's mode as programmed */
	u32 sds_mode;
	/** @sds_an: the lane negotiates in band */
	bool sds_an;
	/** @sds_found: the lane's receiver has synchronised with the host */
	bool sds_found;
	/** @sds_try: the entry of the lane settings in force */
	u8 sds_try;
	/** @sds_tries: the lane settings that may be tried, one bit each */
	u8 sds_tries;
	/** @sds_lock: serialises the bring-ups and re-latches of the lane */
	struct mutex sds_lock;
	/** @sds_work: tries the lane settings until one synchronises */
	struct delayed_work sds_work;

	/** @mirror_to: the monitor port, while a port is mirrored */
	int mirror_to;
	/** @mirror_rx: the ports whose received traffic is mirrored */
	u8 mirror_rx;
	/** @mirror_tx: the ports whose sent traffic is mirrored */
	u8 mirror_tx;

	/** @cf_used: classifier entries in use, translations and streams */
	DECLARE_BITMAP(cf_used, RTL9602C_CLS_ENTRIES);
	/**
	 * @cf_cookie: the flower rule of each translation entry; 0 for a stream
	 * entry
	 */
	unsigned long cf_cookie[RTL9602C_CLS_ENTRIES];
	/**
	 * @cf_xlate: each VLAN rule's entry as composed, without the stream;
	 * the VLAN whose upstream GEM interface gives an upstream rule its
	 * stream, VLAN_N_VID for none; the port the rule is on, so that the
	 * stream can be chosen again when the streams change; and its
	 * template, RTL9602C_CLS_TMPL_C or _S
	 */
	struct rtl9602c_cf_xlate {
		u32 value;
		u32 care;
		u32 act[3];
		u16 out_vid;
		u8 port;
		u8 tmpl;
	} cf_xlate[RTL9602C_CLS_ENTRIES];

	/**
	 * @cf_entries: the classifier entries usable with the driver's layout
	 */
	unsigned int cf_entries;

	/** @acl_lock: serialises the ACL's rules and templates */
	struct mutex acl_lock;

	/**
	 * @ptp_lock: makes a clock latch and its reads, or a step and its
	 * writes, one operation; initialised at probe, before any view that
	 * reads the clock exists
	 */
	struct mutex		ptp_lock;

	/** @debugfs: the driver's debugfs directory */
	struct dentry		*debugfs;

	/** @i2c: the optics' I2C master as an SMBus adapter */
	struct i2c_adapter	i2c;
	/**
	 * @i2c_lock: held by every user of the optics' I2C master for the
	 * whole of its sequence of transfers
	 */
	struct mutex		i2c_lock;
	/**
	 * @optics_apcset0: the power control's set point the module came
	 * with, that of transmit power level mode 0; under i2c_lock
	 */
	u8			optics_apcset0;
	/** @optics_mod0: the modulation DAC the module came with */
	u16			optics_mod0;
	/** @optics_ref: @optics_apcset0 and @optics_mod0 are known */
	bool			optics_ref;
	/**
	 * @optics_mode: the transmit power level mode the optics are set to:
	 * 0 normal, 1 normal - 3 dB, 2 normal - 6 dB
	 */
	u8			optics_mode;
	/**
	 * @optics_p0: the transmitted power at mode 0 in 0.1 uW, measured
	 * when a lower mode is first set; 0 while not measured
	 */
	u16			optics_p0;
	/** @optics_level_work: sets the optics to @gpon_power_mode */
	struct work_struct	optics_level_work;
	/** @optics_reboot_nb: puts level mode 0 back before a restart */
	struct notifier_block	optics_reboot_nb;
	/** @gpon_reboot_nb: sends Dying_Gasp before a restart */
	struct notifier_block	gpon_reboot_nb;
	/** @ptp_info: the switch clock as a PHC */
	struct ptp_clock_info	ptp_info;
	/** @ptp_clock: the PHC; NULL when none is registered */
	struct ptp_clock	*ptp_clock;
	/**
	 * @ptp_pps_count: once-a-second pulses counted, for the ptpstat view
	 */
	u32			ptp_pps_count;
	/**
	 * @intr_rearm: unmasks the self-masking sources a second after they
	 * fire
	 */
	struct delayed_work	intr_rearm;
	/** @intr_rearm_mask: the sources @intr_rearm unmasks */
	u32			intr_rearm_mask;
	/**
	 * @irq_enable_lock: serialises the changes of RTL9602C_IRQ_ENABLE
	 * and of @intr_rearm_mask (rtl9602c_irq_enable())
	 */
	spinlock_t		irq_enable_lock;

	/** @acl_used: the ACL entries in use */
	DECLARE_BITMAP(acl_used, RTL9602C_ACL_ENTRIES);
	/** @acl: the rule in each ACL entry */
	struct {
		/** @acl.cookie: the flower rule's cookie */
		unsigned long	cookie;
		/** @acl.tmpl: the template it uses */
		u8		tmpl;
		/** @acl.port: the port it is on */
		u8		port;
		/** @acl.prio: the tc priority of the rule */
		u16		prio;
		/** @acl.log: its log counter, or -1 */
		s8		log;
		/**
		 * @acl.words: the entry as written: the zeros plane, the ones
		 * plane and the action, so that the rule can be moved
		 */
		u32		words[2 * RTL9602C_ACL_RULE_WORDS +
				      RTL9602C_ACL_ACT_WORDS];
	} acl[RTL9602C_ACL_ENTRIES];
	/** @acl_log_used: the log counters given to ACL rules */
	DECLARE_BITMAP(acl_log_used, RTL9602C_LOG_COUNTERS);
	/**
	 * @acl_log: each log counter, under @acl_lock: its value at the last
	 * read, and the frames counted and reported since it was given out
	 */
	struct {
		/** @acl_log.last: the counter at the last read */
		u32		last;
		/** @acl_log.total: the frames counted */
		u64		total;
		/** @acl_log.reported: the frames reported to tc */
		u64		reported;
	} acl_log[RTL9602C_LOG_COUNTERS];
	/** @acl_tmpl: the field code of each template slot, as written */
	u8 acl_tmpl[RTL9602C_ACL_TMPL_COUNT][RTL9602C_ACL_TMPL_SLOTS];
	/** @acl_tmpl_refs: the rules using each template, 0 when free */
	unsigned int acl_tmpl_refs[RTL9602C_ACL_TMPL_COUNT];

	/**
	 * @ptp_sec_seen: the seconds of the last clock read, which receive
	 * timestamps are completed from
	 */
	atomic64_t	ptp_sec_seen;
	/** @hwtstamp: each port's timestamping configuration, as set */
	struct kernel_hwtstamp_config hwtstamp[RTL9602C_NUM_PORTS];
	/**
	 * @meter_lock: serialises the meters, @meter_used and @cpu_meter, and
	 * the storm-control state, @storm, @storm_seq and @storm_meter, which
	 * tc and devlink both change
	 */
	struct mutex meter_lock;
	/** @meter_used: the meters handed out */
	DECLARE_BITMAP(meter_used, RTL9602C_METER_COUNT);
	/**
	 * @cpu_meter: the meter that limits the frames handed to the CPU, or
	 * -1 (rtl9602c_cpu_rate_set())
	 */
	int cpu_meter;
	/** @storm: storm-control rules from tc, one meter each */
	struct {
		/** @storm.cookie: the flower rule's cookie */
		unsigned long	cookie;
		/** @storm.cls: its class, an RTL9602C_STORM_* index */
		u8		cls;
		/** @storm.port: its port */
		u8		port;
		/** @storm.meter: its meter */
		u8		meter;
		/** @storm.used: the slot holds a rule */
		bool		used;
		/** @storm.prio: its tc priority, lower first */
		u32		prio;
		/** @storm.seq: the order it was added in, from @storm_seq */
		u32		seq;
	} storm[RTL9602C_METER_COUNT];
	/** @storm_seq: the storm-control rules added so far, wrapping */
	u32 storm_seq;

	/**
	 * @storm_meter: the meter each class owns for its devlink rate, or -1
	 */
	int storm_meter[7];

	/** @apptrust: the trusted selectors, most trusted first, as last set */
	u8 apptrust[RTL9602C_APPTRUST_MAX];
	/** @apptrust_n: the number of selectors in @apptrust */
	int apptrust_n;
	/** @apptrust_port: the port owning the switch-wide weights, or -1 */
	int apptrust_port;
	/** @rewr_ports: the ports that rewrite PCP, sharing @rewr_map */
	u8 rewr_ports;
	/** @rewr_map: RTL9602C_QOS_REMARK_MAP as written while a port uses it */
	u32 rewr_map;

	/** @egress: the qdiscs each port follows */
	struct rtl9602c_egress egress[RTL9602C_NUM_PORTS];

	/** @hsb: the frame capture's setting, as armed */
	struct {
		/** @hsb.mode: the capture mode */
		u8	mode;
		/** @hsb.igr: the ingress port mask */
		u8	igr;
		/** @hsb.egr: the egress port mask */
		u8	egr;
		/** @hsb.reason: the forwarding reason mode 7 waits for */
		u8	reason;
	} hsb;

	/**
	 * @dos_flood_th: the flood thresholds last written, by
	 * RTL9602C_DOS_FLOOD_*; the registers are never read
	 */
	u8 dos_flood_th[3];
	/** @hsb_region: the frame_capture devlink region */
	struct devlink_region *hsb_region;
	/** @params_registered: setup registered the devlink parameters */
	bool params_registered;
};

/**
 * enum rtl9602c_devlink_param_id - the driver's devlink parameters
 * @RTL9602C_DEVLINK_PARAM_ID_BASE: DEVLINK_PARAM_GENERIC_ID_MAX; the
 *	driver's IDs follow it
 * @RTL9602C_DEVLINK_PARAM_ID_DOS_PORTS: dos_ports, the ports whose entering
 *	frames the DoS checks judge
 * @RTL9602C_DEVLINK_PARAM_ID_DOS_CHECKS: dos_checks, one enable per check
 * @RTL9602C_DEVLINK_PARAM_ID_DOS_ACTIONS: dos_actions, trap rather than
 *	drop, per check
 * @RTL9602C_DEVLINK_PARAM_ID_DOS_SYN_FLOOD: dos_synflood_thresh
 * @RTL9602C_DEVLINK_PARAM_ID_DOS_FIN_FLOOD: dos_finflood_thresh
 * @RTL9602C_DEVLINK_PARAM_ID_DOS_ICMP_FLOOD: dos_icmpflood_thresh
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_UC_PORTS: storm_unknown_unicast_ports
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_UC_RATE: storm_unknown_unicast_pps
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_UM_PORTS: storm_unknown_multicast_ports
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_UM_RATE: storm_unknown_multicast_pps
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_PORTS: storm_dhcp_ports
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_RATE: storm_dhcp_pps
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_PORTS: storm_igmp_mld_ports
 * @RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_RATE: storm_igmp_mld_pps
 * @RTL9602C_DEVLINK_PARAM_ID_PKTDBG_MODE: pktdbg_mode, the frame capture's
 *	mode
 * @RTL9602C_DEVLINK_PARAM_ID_PKTDBG_INGRESS_PORTS: pktdbg_ingress_ports
 * @RTL9602C_DEVLINK_PARAM_ID_PKTDBG_EGRESS_PORTS: pktdbg_egress_ports
 * @RTL9602C_DEVLINK_PARAM_ID_PKTDBG_REASON: pktdbg_reason
 * @RTL9602C_DEVLINK_PARAM_ID_LOOPDET_EN: loop_detect
 * @RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PORTS: loop_detect_ports, the ports
 *	that send probes
 * @RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PERIOD: loop_detect_period, in
 *	milliseconds
 * @RTL9602C_DEVLINK_PARAM_ID_GPON_SERIAL: gpon_serial, the ONU's serial
 *	number
 * @RTL9602C_DEVLINK_PARAM_ID_GPON_PASSWORD: gpon_password, the password the
 *	ONU gives when the OLT asks for it
 * @RTL9602C_DEVLINK_PARAM_ID_CPU_RX_PPS: cpu_rx_pps, the packets a second
 *	the switch hands the CPU
 */
enum rtl9602c_devlink_param_id {
	RTL9602C_DEVLINK_PARAM_ID_BASE = DEVLINK_PARAM_GENERIC_ID_MAX,
	RTL9602C_DEVLINK_PARAM_ID_DOS_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_DOS_CHECKS,
	RTL9602C_DEVLINK_PARAM_ID_DOS_ACTIONS,
	RTL9602C_DEVLINK_PARAM_ID_DOS_SYN_FLOOD,
	RTL9602C_DEVLINK_PARAM_ID_DOS_FIN_FLOOD,
	RTL9602C_DEVLINK_PARAM_ID_DOS_ICMP_FLOOD,
	RTL9602C_DEVLINK_PARAM_ID_STORM_UC_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_STORM_UC_RATE,
	RTL9602C_DEVLINK_PARAM_ID_STORM_UM_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_STORM_UM_RATE,
	RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_STORM_DHCP_RATE,
	RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_STORM_IGMP_RATE,
	RTL9602C_DEVLINK_PARAM_ID_PKTDBG_MODE,
	RTL9602C_DEVLINK_PARAM_ID_PKTDBG_INGRESS_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_PKTDBG_EGRESS_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_PKTDBG_REASON,
	RTL9602C_DEVLINK_PARAM_ID_LOOPDET_EN,
	RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PORTS,
	RTL9602C_DEVLINK_PARAM_ID_LOOPDET_PERIOD,
	RTL9602C_DEVLINK_PARAM_ID_GPON_SERIAL,
	RTL9602C_DEVLINK_PARAM_ID_GPON_PASSWORD,
	RTL9602C_DEVLINK_PARAM_ID_CPU_RX_PPS,
};

/*
 * Indices into rtl9602c_priv.dos_flood_th, the driver's copy of the flood
 * thresholds; not hardware values.
 */
#define RTL9602C_DOS_FLOOD_SYN	0
#define RTL9602C_DOS_FLOOD_FIN	1
#define RTL9602C_DOS_FLOOD_ICMP	2

/*
 * The storm-control classes, as indices into rtl9602c_storm_class[]; the
 * numbering is the table's own, not a hardware value.
 */
#define RTL9602C_STORM_UM	0
#define RTL9602C_STORM_UC	1
#define RTL9602C_STORM_MC	2
#define RTL9602C_STORM_BC	3
#define RTL9602C_STORM_ARP	4
#define RTL9602C_STORM_DHCP	5
#define RTL9602C_STORM_IGMP	6

/* The driver's state from its PHC description. */
#define to_rtl9602c_ptp(x) container_of((x), struct rtl9602c_priv, ptp_info)

/* Bytes a second per unit of RTL9602C_SHAPE_PORT_CFG_RATE, 8 kbit/s. */
#define RTL9602C_SHAPE_PORT_CFG_RATE_STEP	1000

/**
 * struct rtl9602c_storm_class_row - a storm-control class
 * @name: its name
 * @ctrl: its enable register, one bit per port
 * @meter: its meter-select register, RTL9602C_STORM_METER_SEL_PORT() per
 *	port
 */
struct rtl9602c_storm_class_row { const char *name; u32 ctrl; u32 meter; };

/**
 * struct rtl9602c_storm_param_row - a devlink storm-control parameter
 * @id: its ID, a value of enum rtl9602c_devlink_param_id
 * @cls: the class it sets, an RTL9602C_STORM_* index
 * @is_rate: it is the class's packet rate rather than its ports
 */
struct rtl9602c_storm_param_row { u32 id; u8 cls; bool is_rate; };
#define RTL9602C_STORM_NCLASS 7	/* rows of rtl9602c_storm_class[] */

int rtl9602c_cls_flower_add(struct dsa_switch *ds, int port, struct flow_cls_offload *cls,
			    bool ingress);
int rtl9602c_cls_flower_stats(struct dsa_switch *ds, int port, struct flow_cls_offload *cls,
			      bool ingress);
void rtl9602c_acl_log_fold(struct rtl9602c_priv *p);
int rtl9602c_cls_flower_del(struct dsa_switch *ds, int port, struct flow_cls_offload *cls,
			    bool ingress);
int rtl9602c_get_ts_info(struct dsa_switch *ds, int port, struct kernel_ethtool_ts_info *ts);
void rtl9602c_gpon_debugfs_init(struct rtl9602c_priv *p);
void rtl9602c_gpon_cntr_sync(struct rtl9602c_priv *p);
void rtl9602c_gpon_report(struct rtl9602c_priv *p);
void rtl9602c_gpon_work(struct work_struct *w);
void rtl9602c_gpon_irq(struct rtl9602c_priv *p);
int rtl9602c_i2c_read_byte(struct rtl9602c_priv *p, u16 slave, u8 reg, u8 *val);
int rtl9602c_i2c_register(struct rtl9602c_priv *p);
void rtl9602c_l2_clear(struct rtl9602c_priv *p);
int rtl9602c_l2_l3lookup_cpu(struct rtl9602c_priv *p, bool on);
int rtl9602c_l2_own_place(struct rtl9602c_priv *p, int port);
int rtl9602c_l2_own_drop(struct rtl9602c_priv *p, int port);
int rtl9602c_vlan_set_member(struct rtl9602c_priv *p, u16 vid,
			     int port, bool member, bool untagged);

void rtl9602c_lrn_over_intr_sync(struct rtl9602c_priv *p);

/* For rtl9602c_l2_nexthop_place(): the port's PVID rather than a VID. */
#define RTL9602C_L2_VID_PVID		0xffff
int rtl9602c_l2_nexthop_place(struct rtl9602c_priv *p, const unsigned char *mac,
			      int port, u16 *want_vid, unsigned int *slot);
int rtl9602c_l2_nexthop_drop(struct rtl9602c_priv *p, const unsigned char *mac,
			     u16 vid);
void rtl9602c_nat_clear(struct rtl9602c_priv *p);
void rtl9602c_l34_init(struct rtl9602c_priv *p);
void rtl9602c_l34_fini(struct rtl9602c_priv *p);
void rtl9602c_filter_qos_reset(struct rtl9602c_priv *p);
void rtl9602c_svlan_setup(struct rtl9602c_priv *p);
void rtl9602c_l34_notifiers_register(struct rtl9602c_priv *p);
void rtl9602c_l34_notifiers_unregister(struct rtl9602c_priv *p);
int rtl9602c_l_read(struct rtl9602c_priv *p, u16 reg, u8 *val);
int rtl9602c_l_txdis_gpio(struct rtl9602c_priv *p, bool disable);
int rtl9602c_optics_hold_modulation(struct rtl9602c_priv *p);
void rtl9602c_optics_level_work(struct work_struct *work);
int rtl9602c_optics_level_register(struct rtl9602c_priv *p);
int rtl9602c_gpon_reboot_register(struct rtl9602c_priv *p);
void rtl9602c_gpon_power_applied(struct rtl9602c_priv *p, u8 asked, u8 mode,
				 int err);
int rtl9602c_optics_hwmon_register(struct rtl9602c_priv *p);
void rtl9602c_meter_free(struct rtl9602c_priv *p, unsigned int meter);
void rtl9602c_offload_debugfs_init(struct rtl9602c_priv *p);
void rtl9602c_l2_debugfs_init(struct rtl9602c_priv *p);
void rtl9602c_optics_debugfs_init(struct rtl9602c_priv *p);
int rtl9602c_port_add_dscp_prio(struct dsa_switch *ds, int port, u8 dscp, u8 prio);
int rtl9602c_port_bridge_flags(struct dsa_switch *ds, int port,
			       struct switchdev_brport_flags flags, struct netlink_ext_ack *extack);
void rtl9602c_bridge_refresh(struct dsa_switch *ds);
void rtl9602c_mcast_reset(struct rtl9602c_priv *p);
void rtl9602c_mcast_refresh(struct rtl9602c_priv *p);
void rtl9602c_acl_snoop_setup(struct rtl9602c_priv *p);
void rtl9602c_cf_sid_refresh(struct rtl9602c_priv *p);
int rtl9602c_port_bridge_join(struct dsa_switch *ds, int port, struct dsa_bridge bridge,
			      bool *tx_fwd_offload, struct netlink_ext_ack *extack);
void rtl9602c_port_bridge_leave(struct dsa_switch *ds, int port, struct dsa_bridge bridge);
int rtl9602c_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu);
int rtl9602c_port_del_dscp_prio(struct dsa_switch *ds, int port, u8 dscp, u8 prio);
void rtl9602c_port_fast_age(struct dsa_switch *ds, int port);
int rtl9602c_port_fdb_add(struct dsa_switch *ds, int port, const unsigned char *addr, u16 vid,
			  struct dsa_db db);
int rtl9602c_port_fdb_del(struct dsa_switch *ds, int port, const unsigned char *addr, u16 vid,
			  struct dsa_db db);
int rtl9602c_port_fdb_dump(struct dsa_switch *ds, int port, dsa_fdb_dump_cb_t *cb, void *data);
int rtl9602c_port_get_apptrust(struct dsa_switch *ds, int port, u8 *sel, int *nsel);
int rtl9602c_port_get_default_prio(struct dsa_switch *ds, int port);
int rtl9602c_port_get_dscp_prio(struct dsa_switch *ds, int port, u8 dscp);
int rtl9602c_port_hwtstamp_get(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config);
int rtl9602c_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config,
			       struct netlink_ext_ack *extack);
int rtl9602c_port_max_mtu(struct dsa_switch *ds, int port);
int rtl9602c_port_mdb_add(struct dsa_switch *ds, int port,
			  const struct switchdev_obj_port_mdb *mdb, struct dsa_db db);
int rtl9602c_port_mdb_del(struct dsa_switch *ds, int port,
			  const struct switchdev_obj_port_mdb *mdb, struct dsa_db db);
int rtl9602c_port_mirror_add(struct dsa_switch *ds, int port,
			     struct dsa_mall_mirror_tc_entry *mirror,
			     bool ingress, struct netlink_ext_ack *extack);
void rtl9602c_port_mirror_del(struct dsa_switch *ds, int port,
			      struct dsa_mall_mirror_tc_entry *mirror);
int rtl9602c_port_policer_add(struct dsa_switch *ds, int port,
			      struct dsa_mall_policer_tc_entry *policer);
void rtl9602c_port_policer_del(struct dsa_switch *ds, int port);
int rtl9602c_port_pre_bridge_flags(struct dsa_switch *ds, int port,
				   struct switchdev_brport_flags flags,
				   struct netlink_ext_ack *extack);
bool rtl9602c_port_rxtstamp(struct dsa_switch *ds, int port,
			    struct sk_buff *skb, unsigned int type);
int rtl9602c_port_set_apptrust(struct dsa_switch *ds, int port, const u8 *sel, int nsel);
int rtl9602c_port_set_pcp_rewr(struct dsa_switch *ds, int port, const u8 *pcp, u8 mapped);
int rtl9602c_port_set_default_prio(struct dsa_switch *ds, int port, u8 prio);
int rtl9602c_port_setup_tc(struct dsa_switch *ds, int port, enum tc_setup_type type,
			   void *type_data);
void rtl9602c_port_stp_state_set(struct dsa_switch *ds, int port, u8 state);
int rtl9602c_port_vlan_add(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan,
			   struct netlink_ext_ack *extack);
int rtl9602c_port_vlan_del(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan);
int rtl9602c_port_vlan_filtering(struct dsa_switch *ds, int port, bool on,
				 struct netlink_ext_ack *extack);
void rtl9602c_ptp_debugfs_init(struct rtl9602c_priv *p);
u64 rtl9602c_ptp_rd_sec(struct rtl9602c_priv *p, u32 base);
void rtl9602c_ptp_now(struct rtl9602c_priv *p, struct timespec64 *ts);
int rtl9602c_ptp_register(struct rtl9602c_priv *p);
void rtl9602c_ptp_unregister(struct rtl9602c_priv *p);
u32 rtl9602c_r(struct rtl9602c_priv *p, u32 off);
void rtl9602c_rmw(struct rtl9602c_priv *p, u32 off, u32 clr, u32 set);
void rtl9602c_irq_enable(struct rtl9602c_priv *p, u32 clr, u32 set);
void rtl9602c_serdes_init(struct rtl9602c_priv *p, u32 sds_mode);
int rtl9602c_set_ageing_time(struct dsa_switch *ds, unsigned int msecs);
extern const struct rtl9602c_storm_class_row rtl9602c_storm_class[];
extern const struct rtl9602c_storm_param_row rtl9602c_storm_param[];
int rtl9602c_storm_param_find(u32 id);
u32 rtl9602c_storm_ports_get(struct rtl9602c_priv *p, unsigned int cls);
int rtl9602c_storm_ports_set(struct rtl9602c_priv *p, unsigned int cls, u32 ports);
u32 rtl9602c_storm_rate_get(struct rtl9602c_priv *p, unsigned int cls);
u32 rtl9602c_cpu_rate_get(struct rtl9602c_priv *p);
int rtl9602c_cpu_rate_set(struct rtl9602c_priv *p, u32 rate);
int rtl9602c_storm_rate_set(struct rtl9602c_priv *p, unsigned int cls, u32 rate);
int rtl9602c_table_op(struct rtl9602c_priv *p, unsigned int type,
		      unsigned int method, unsigned int addr, u32 *data,
		      unsigned int words, bool write,
		      struct rtl9602c_lookup *out);
int rtl9602c_table_read(struct rtl9602c_priv *p, unsigned int type, unsigned int addr, u32 *data,
			unsigned int words, bool *hit);
int rtl9602c_table_write(struct rtl9602c_priv *p, unsigned int type, unsigned int addr, u32 *data,
			 unsigned int words);
void rtl9602c_w(struct rtl9602c_priv *p, u32 off, u32 val);

int rtl9602c_ponmac_probe(struct rtl9602c_priv *p);
int rtl9602c_ponmac_init(struct rtl9602c_priv *p);
void rtl9602c_ponmac_sid_valid(struct rtl9602c_priv *p, unsigned int sid,
			       bool valid);
void rtl9602c_ponmac_debugfs_init(struct rtl9602c_priv *p);
int rtl9602c_omci_setup(struct rtl9602c_priv *p);
void rtl9602c_omci_teardown(struct rtl9602c_priv *p);
void rtl9602c_gpon_operstate(struct rtl9602c_priv *p);
void rtl9602c_gpon_fill_status(struct rtl9602c_priv *p,
			       struct pon_dev_status *st);
int rtl9602c_gpon_start(struct rtl9602c_priv *p);
int rtl9602c_gpon_sn_set(struct rtl9602c_priv *p, const char *s);
int rtl9602c_gpon_password_set(struct rtl9602c_priv *p, const char *s);
void rtl9602c_gpon_stop(struct rtl9602c_priv *p);
int rtl9602c_gpon_port_bind(struct rtl9602c_priv *p, unsigned int idx,
			    u16 port_id);
int rtl9602c_gpon_port_unbind(struct rtl9602c_priv *p, unsigned int idx);
u32 rtl9602c_gpon_port_aes(struct rtl9602c_priv *p, u16 port_id);
void rtl9602c_pon_queue_set(struct rtl9602c_priv *p, unsigned int q,
			    unsigned int weight, u32 cir, u32 pir);
int rtl9602c_pon_queue_add(struct rtl9602c_priv *p, unsigned int tcont,
			   unsigned int queue);
void rtl9602c_ponq_stream_queue_set(struct rtl9602c_priv *p, unsigned int sid,
				    unsigned int queue);
int rtl9602c_gem_setup(struct rtl9602c_priv *p);
void rtl9602c_gem_teardown(struct rtl9602c_priv *p);
void rtl9602c_gem_restore(struct rtl9602c_priv *p);
void rtl9602c_gem_tcont_ready(struct rtl9602c_priv *p, u32 tcont);
int rtl9602c_gpon_tcont_map(struct rtl9602c_priv *p, unsigned int n, u16 alloc,
			    struct netlink_ext_ack *extack);
bool rtl9602c_gem_us_ready(struct rtl9602c_priv *p, unsigned int flow);
int rtl9602c_gpon_port_get(struct rtl9602c_priv *p, unsigned int flow,
			   u16 *port_id, bool *valid);
u32 rtl9602c_gpon_flow_cntr(struct rtl9602c_priv *p, unsigned int flow,
			    bool bytes);
void rtl9602c_gem_cntr_sync_all(struct rtl9602c_priv *p);

#endif /* _RTL9602C_H */
