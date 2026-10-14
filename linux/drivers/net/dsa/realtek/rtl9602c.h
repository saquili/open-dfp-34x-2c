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
#include <linux/auxiliary_bus.h>
#include <linux/dsa/rtl9602c.h>
#include <linux/platform_data/rtl9602c-pon.h>

/*
 * Ports: 0 is the SerDes lane to the host, 1 is not wired on this board, 2 is
 * the PON port, behind which the GPON MAC sits (RTL9602C_PORT_PON, in the
 * header the tagger and the conduit share), and 3 the CPU port, whose conduit
 * is the CPU port's DMA engine.
 */
#define RTL9602C_PORT_SFP		0
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
 * What becomes of a unicast frame that no address-table entry knows, by the
 * port it came in by, two bits a port: FLOOD sends it by the unknown-unicast
 * flood mask, as at reset, and DROP drops it, a frame the L3 engine would
 * route as well (measured on the PON port).
 */
#define RTL9602C_UC_UNKNOWN			0x01c008
#define RTL9602C_UC_UNKNOWN_ACT(p)		(GENMASK(1, 0) << (2 * (p)))
#define RTL9602C_UC_UNKNOWN_ACT_FLOOD		0
#define RTL9602C_UC_UNKNOWN_ACT_DROP		1

/*
 * What becomes of a frame whose source address the table holds for another
 * port, by the port it came in by, two bits a port: MOVE moves the entry to
 * that port, and DROP drops the frame and learns nothing from it.
 */
#define RTL9602C_SA_MOVED			0x01c000
#define RTL9602C_SA_MOVED_ACT(p)		(GENMASK(1, 0) << (2 * (p)))
#define RTL9602C_SA_MOVED_ACT_MOVE		0
#define RTL9602C_SA_MOVED_ACT_DROP		1

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
 * The first words of each port's blocks (RTL9602C_MIB_TX() and
 * RTL9602C_MIB_RX(), in <linux/platform_data/rtl9602c-pon.h>) are shown by the
 * PON driver's ponpath view too.
 */

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
 * the interface rows by destination MAC (not verified). A whole-word write
 * that changed more than NAT_MODE and L3IF_LOOKUP sent translated flows to
 * the CPU (measured on this board). With FLOW6_EN set and the five-tuple
 * table empty, every IPv6 frame the engine takes goes to the CPU (reason
 * 16); with it clear, the engine routes IPv6 by the IPv6 route rows. With
 * TTL_DEC set the engine decrements the TTL or hop limit of the frames it
 * routes and hands one that arrives with 1 to the CPU; clear, it forwards
 * them unchanged (both measured).
 */
#define RTL9602C_L3_CFG			0x800010
#define RTL9602C_L3_CFG_FLOW4_EN		BIT(30)
#define RTL9602C_L3_CFG_FLOW6_EN		BIT(29)
#define RTL9602C_L3_CFG_TTL_DEC		BIT(12)
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

/*
 * IPv6 route entry: as the IPv4 one, with a 128-bit destination, its high
 * word last, and a prefix length that is the length itself. RT2WAN is set
 * on a row whose frames leave by the WAN. Action 0 hands the frame to the
 * CPU. The layout and the actions are not verified beyond what routes
 * through the default route and a gateway on the LAN need.
 */
#define RTL9602C_ROUTE6_DST(w)		RTL9602C_FIELD(32 * (w), 32)
#define RTL9602C_ROUTE6_DST_LEN		RTL9602C_FIELD(128, 8)
#define RTL9602C_ROUTE6_L3IF_IDX	RTL9602C_FIELD(136, 4)
#define RTL9602C_ROUTE6_NEXTHOP_IDX	RTL9602C_FIELD(136, 4)
#define RTL9602C_ROUTE6_ACTION		RTL9602C_FIELD(140, 2)
#define RTL9602C_ROUTE6_VALID		RTL9602C_FIELD(142, 1)
#define RTL9602C_ROUTE6_RT2WAN		RTL9602C_FIELD(143, 1)

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

/*
 * External-address entry: a translated source and the next hop it leaves by.
 * In one-to-one mode the entry maps the external address to INT_IP, with no
 * state per flow. Holding a host's address as both, it has the engine route
 * that host's frames in hardware both ways without translating them: from
 * the host out by NEXTHOP_IDX, and to the host, unsolicited ones from the WAN
 * as well, by its route. Mode 2 carries only the frames from the host; those
 * to it from the WAN go to the CPU (both measured on this board, over a PPPoE
 * WAN).
 */
#define RTL9602C_EXTADDR_INT_IP		RTL9602C_FIELD(0, 32)
#define RTL9602C_EXTADDR_IP		RTL9602C_FIELD(32, 32)
#define RTL9602C_EXTADDR_VALID		RTL9602C_FIELD(64, 1)
#define RTL9602C_EXTADDR_MODE		RTL9602C_FIELD(65, 2)
#define RTL9602C_EXTADDR_MODE_NAPT	0
#define RTL9602C_EXTADDR_MODE_ONE_TO_ONE	1
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
 * The PON time, which the clock takes when the GPON MAC's time-of-day
 * superframe arrives while PON_TIME_EN is set (measured), in the layout of
 * the clock, and a per-port PTP enable that receive stamping does not depend
 * on.
 */
#define RTL9602C_PTP_PON_TIME_SEC	0x01b024
#define RTL9602C_PTP_PON_TIME_NSEC	0x01b02c
#define RTL9602C_PTP_PORT_EN		0x0111e8

/*
 * In RTL9602C_PTP_CLK_CTRL; LATCH and STEP act once per 1 written, and the
 * hardware clears PON_TIME_EN when the clock has taken the PON time.
 */
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
 * Per port, three to a word, the buffer pages the frames that leave by the
 * port may hold. Beyond it the switch drops a frame for the port where the
 * frame enters, counted in that port's RX_FORWARD_DISCARDS. After reset every
 * port may hold 1012 pages, more than the global pause mark of 512
 * (RTL9602C_PAUSE_THR_GLOBAL_HIGH): with one queue of the CPU port at 540 to
 * 588 pages, the switch dropped frames for the other ports too (both measured
 * on this board).
 */
#define RTL9602C_PORT_PAGE_LIMIT(port)		(0x02d018 + 4 * ((port) / 3))
#define RTL9602C_PORT_PAGE_LIMIT_SHIFT(port)	(10 * ((port) % 3))
#define RTL9602C_PORT_PAGE_LIMIT_MAX		GENMASK(9, 0)

/* The pages the frames for the CPU may hold (rtl9602c_setup()). */
#define RTL9602C_CPU_PAGE_LIMIT			128

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
 * Per-port and per-message PTP settings. Of these the driver writes the egress
 * actions of the event messages while transmit stamping is on, the ingress
 * actions while a port stamps what it receives, and the reference time for a
 * one-step Pdelay_Resp; the transparent-clock bits act on no frame the CPU
 * sends or receives (measured). RTL9602C_PTP_RX_TSTAMP is the reference time
 * that the egress action RTL9602C_PTP_ACTION_CORRECT takes off a message's
 * departure time before it adds the difference, modulo 8 s, to the
 * correctionField: the seconds' low three bits in the first word, the
 * nanoseconds in 8 ns ticks in the second (both measured). The same action
 * adds RTL9602C_PTP_PATH_DELAY, in nanoseconds (measured).
 */
#define RTL9602C_PTP_TRANSPARENT_PORTS	0x0111d4
#define RTL9602C_PTP_ACTION_INGRESS	0x0111d8
#define RTL9602C_PTP_ACTION_EGRESS	0x0111dc
#define RTL9602C_PTP_RX_TSTAMP		0x0111e0
#define RTL9602C_PTP_RX_TSTAMP_SEC	GENMASK(2, 0)
#define RTL9602C_PTP_RX_TSTAMP_NSEC	GENMASK(29, 3)
#define RTL9602C_PTP_PATH_DELAY		0x01b020
#define RTL9602C_PTP_PATH_DELAY_NS		GENMASK(30, 0)

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
 * The egress actions: one that latches the time a message leaves and, for a
 * one-step Sync, writes it into the originTimestamp; one that latches it and
 * sends the CPU a departure report with it; and one that adds the time from
 * the reference to the departure to the correctionField (all measured). The
 * ingress action that takes a message to the CPU and forwards it no further.
 * The slots of the event
 * messages in the action registers, measured for every message: a Sync goes
 * by slot 0 with the twoStepFlag set and by slot 8 without it, a Pdelay_Req
 * by slot 2 with it and by slot 9 without it; a Pdelay_Resp by slot 3 either
 * way. The general messages go by slots 4 to 7: Follow_Up, Delay_Resp,
 * Pdelay_Resp_Follow_Up and Announce.
 */
#define RTL9602C_PTP_ACTION_LATCH		1
#define RTL9602C_PTP_ACTION_LATCH_REPORT	2
#define RTL9602C_PTP_ACTION_CORRECT		3
#define RTL9602C_PTP_ACTION_TRAP		1
/*
 * The ingress actions that forward a message through the transparent clock,
 * to the ports whose transparent-clock bit is set, and that do so and copy it
 * to the CPU as well (measured upstream).
 */
#define RTL9602C_PTP_ACTION_FORWARD_TC		2
#define RTL9602C_PTP_ACTION_FORWARD_TC_MIRROR	3

/**
 * enum rtl9602c_ptp_tc - what the switch does as a transparent clock
 * @RTL9602C_PTP_TC_OFF: nothing: it forwards PTP messages as other frames
 * @RTL9602C_PTP_TC_E2E: an end-to-end transparent clock: every event message
 *	forwarded between the wire ports gets its residence time added to its
 *	correctionField (IEEE 1588-2008 11.5.2)
 * @RTL9602C_PTP_TC_P2P: a peer-to-peer transparent clock: a forwarded Sync
 *	gets its residence time and the path delay set with the
 *	ptp_path_delay_ns parameter; the peer delay messages go to the CPU
 *	(IEEE 1588-2008 11.5.4)
 */
enum rtl9602c_ptp_tc {
	RTL9602C_PTP_TC_OFF,
	RTL9602C_PTP_TC_E2E,
	RTL9602C_PTP_TC_P2P,
};
#define RTL9602C_PTP_SLOT_SYNC			0
#define RTL9602C_PTP_SLOT_DELAY_REQ		1
#define RTL9602C_PTP_SLOT_PDELAY_REQ_2STEP	2
#define RTL9602C_PTP_SLOT_PDELAY_RESP		3
#define RTL9602C_PTP_SLOT_SYNC_1STEP		8
#define RTL9602C_PTP_SLOT_PDELAY_REQ		9

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
 * header (measured). GEMIDX holds the GEM flow of a frame that entered by the
 * PON port, the number the GPON MAC gives the port (measured).
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
#define RTL9602C_ACL_TMPL_FIELD_GEMIDX		0x0a

/*
 * The forwarding decision and its port mask: REDIRECT with no port drops the
 * frame; MIRROR forwards it as usual and sends a copy to the ports of the
 * mask, the CPU port's bit alone reaching the CPU; that TRAP sends it to the
 * CPU is not verified. EN_FWD is the forwarding class of RTL9602C_ACL_ACT_EN,
 * which reads EN_DEFAULT at reset. MIRROR to no port forwards a frame as usual
 * and decides its forwarding, so that no rule of lower priority does.
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
 * The log class of an action: its type at LOG_LSB, 2 bits, and a counter or
 * meter at LOG_IDX_LSB, 5 bits. Type LOG_COUNTER counts each frame the rule
 * matches in log counter LOG_IDX. Type LOG_METER, 0, hands each such frame to
 * meter LOG_IDX, which drops it when the meter is exceeded (measured, in
 * packet and in byte mode). EN_LOG is the class's bit of
 * RTL9602C_ACL_ACT_EN; the driver sets it for a rule with a counter or a
 * meter.
 */
#define RTL9602C_ACL_ACT_LOG_LSB	35
#define RTL9602C_ACL_ACT_LOG_IDX_LSB	30
#define RTL9602C_ACL_ACT_LOG_METER	0
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
 * Sixteen meters of two words, the high one at the lower address. A meter
 * bound to an ACL rule polices in byte mode as in packet mode, in units of
 * 8 kbit/s, and the maximum rate does not limit (see "Meters" in the ACL and
 * QoS page).
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

/*
 * What a byte-mode rate unit stands for, in bytes per second (8 kbit/s), and
 * the bucket, in bytes, a downstream limit of a GEM port takes: from what
 * passes the largest frame to what the field holds. A bucket of 1024 bytes
 * passed no full-size frame, and one of 1536 did (measured).
 */
#define RTL9602C_METER_BYTE_RATE_UNIT		1000
#define RTL9602C_GEM_DS_BURST_MIN		2048
#define RTL9602C_GEM_DS_BURST_MAX		0xffff

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
 * and SSTP ones; only _00 and _0E are verified to serve the addresses they
 * name. ACTION: 0 forward, the CPU included, 1 to the CPU only, 2 drop, 3
 * forward but not to the CPU (measured on _0E).
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
#define RTL9602C_RSVDMC_CTRL_ACTION_TRAP	1
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
#include <linux/platform_data/i2c-rtl9602c.h>
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
 * @addr6: the neighbour's IPv6 address, for an IPv6 neighbour
 * @v6: an IPv6 neighbour
 * @add: the neighbour was in a valid state
 *
 * The neighbour notifier can run in softirq, and the table writes sleep, so
 * the event is queued and applied under flow_lock.
 */
struct rtl9602c_l34_neigh_ev {
	struct list_head	list;
	struct net_device	*dev;
	__be32			addr;
	struct in6_addr		addr6;
	bool			v6;
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
 * struct rtl9602c_l34_route6 - an IPv6 route in the shadow of the routing table
 * @dst: the destination prefix
 * @gw: the gateway, the unspecified address for an on-link route
 * @ifindex: the device of the nexthop
 * @port: the switch port of that device, or -1
 * @prefix: the prefix length
 * @flags: %RTL9602C_L34_RT_CPU or %RTL9602C_L34_RT_DROP
 * @linkdown: the nexthop is dead or without carrier
 * @used: the slot holds a route
 */
struct rtl9602c_l34_route6 {
	struct in6_addr	dst;
	struct in6_addr	gw;
	int		ifindex;
	s8		port;
	u8		prefix;
	u8		flags;
	bool		linkdown;
	bool		used;
};

/**
 * struct rtl9602c_l34_fib_ev - a route event queued for the work item
 * @list: its place in rtl9602c_priv.l34_fib_q
 * @route: what the row needs, copied out in the notifier
 * @route6: the same, for an IPv6 route
 * @v6: an IPv6 route, in @route6
 * @add: the route was added or replaced, or the nexthop became usable
 * @nh: a nexthop event: @route holds only its device and gateway
 *
 * The route notifier can be called under rcu_read_lock(), and the table
 * writes sleep, so the event is queued and applied under flow_lock.
 */
struct rtl9602c_l34_fib_ev {
	struct list_head		list;
	struct rtl9602c_l34_route	route;
	struct rtl9602c_l34_route6	route6;
	bool				v6;
	bool				add;
	bool				nh;
	bool				resync;
};

/**
 * struct rtl9602c_l34_src_wan - the PPPoE WAN of a routed host's own table
 * @ifindex: the PPP device the host's frames leave by, 0 for a free slot
 * @wan: its row, as rtl9602c_priv.l34_route_wan holds the default route's
 * @us: it holds a reference on rtl9602c_priv.l34_us_flow
 * @used: a routed host leaves by it, in the sync under way
 */
struct rtl9602c_l34_src_wan {
	int	ifindex;
	int	wan;
	bool	us;
	bool	used;
};

/* The PPPoE WANs of the routed hosts' own tables: the rows but the default route's. */
#define RTL9602C_L34_SRC_WANS	6

/**
 * struct rtl9602c_pppoe_wan - a PPPoE WAN of the L3/NAT engine
 * @extip: the PPP interface's address, which keys a WAN made for flows; 0 when
 *	free, and for the WAN of the default route
 * @refs: the flows using it
 * @sid: its PPPoE session
 * @port: the switch port the session leaves by
 * @extidx: its external-address row
 * @took_extip: that row was taken for the WAN and is freed with it
 * @dmac: the concentrator's MAC address
 * @ifindex: the PPP device, for the WAN of the default route; 0 for a WAN made
 *	for flows
 * @vid: the VLAN the session runs in, 0 for none
 * @smac: the MAC address the session's frames leave from
 * @mtu: the PPP device's MTU
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
	int		ifindex;
	u16		vid;
	u8		smac[ETH_ALEN];
	unsigned int	mtu;
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

/* The most addresses and VLANs a GEM interface hands the driver at once. */
#define RTL9602C_GEM_RX_ADDRS		8
#define RTL9602C_GEM_RX_VIDS		8

/**
 * struct rtl9602c_gem_rx - the unicast frames a GEM interface takes
 * @known: the interface has said, through rtl9602c_gem_rx_filter()
 * @promisc: it takes every frame of its GEM port
 * @n_addrs: the number of addresses in @addrs
 * @n_vids: the number of VIDs in @vids
 * @addrs: its addresses
 * @vids: the VLANs it takes them in, besides untagged
 */
struct rtl9602c_gem_rx {
	bool known;
	bool promisc;
	u8 n_addrs;
	u8 n_vids;
	u8 addrs[RTL9602C_GEM_RX_ADDRS][ETH_ALEN];
	u16 vids[RTL9602C_GEM_RX_VIDS];
};

/* The most static entries that take the GEM interfaces' addresses to the CPU. */
#define RTL9602C_GEM_HOSTS		32

/**
 * struct rtl9602c_host - an address the CPU takes in a VLAN
 * @addr: the address
 * @vid: the VLAN
 */
struct rtl9602c_host {
	u8 addr[ETH_ALEN];
	u16 vid;
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

	/**
	 * @pon: what the GPON MAC's driver and this driver share: the
	 * services each does for the other, and the interrupt's time
	 */
	struct rtl9602c_pon_data pon;
	/** @pon_adev: the device the GPON MAC's driver binds to; NULL when none is made */
	struct auxiliary_device *pon_adev;

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
	 * @uc_drop: whether the PON port drops unknown unicast, under
	 * @fdb_lock
	 */
	bool uc_drop;
	/** @gem_rx: what each GEM interface takes, under RTNL */
	struct rtl9602c_gem_rx gem_rx[RTL9602C_GPON_NUM_FLOW];
	/**
	 * @gem_hosts: the static entries placed for the GEM interfaces'
	 * addresses, under RTNL
	 */
	struct rtl9602c_host gem_hosts[RTL9602C_GEM_HOSTS];
	/** @n_gem_hosts: how many of @gem_hosts are placed, under RTNL */
	unsigned int n_gem_hosts;
	/**
	 * @gem_hosts_full: an address of a GEM interface has no entry, so the
	 * CPU needs the PON port's unknown unicast; under RTNL
	 */
	bool gem_hosts_full;

	/**
	 * @l34_netif_ifindex: the device of each L3-interface row by ifindex, 0
	 * for a free row and -1 for one a PPPoE WAN holds
	 */
	int l34_netif_ifindex[16];
	/** @l34_extip: each external-address row's address, 0 when free */
	__be32 l34_extip[8];
	/**
	 * @l34_extip_mode: each external-address row's mode,
	 * %RTL9602C_EXTADDR_MODE_ONE_TO_ONE for a routed host's row
	 */
	u8 l34_extip_mode[8];
	/** @l34_extip_nhs: the next-hop row each external-address row names */
	u8 l34_extip_nhs[8];
	/**
	 * @l34_src_wan: the PPPoE WANs that routed hosts leave by through a
	 * table of their own, chosen by a rule on their address
	 */
	struct rtl9602c_l34_src_wan l34_src_wan[RTL9602C_L34_SRC_WANS];

	/** @l34_pppoe: the PPPoE WANs by row; row 0 is the Ethernet uplink's */
	struct rtl9602c_pppoe_wan l34_pppoe[8];
	/**
	 * @l34_route_wan: the PPPoE WAN the default route leaves by, 0 for
	 * none
	 */
	int l34_route_wan;
	/** @l34_route_wan_us: that WAN holds a reference on @l34_us_flow */
	bool l34_route_wan_us;
	/**
	 * @l34_route6_wan: the PPPoE WAN the IPv6 default route leaves by, 0
	 * for none; an operator may give IPv6 on a session of its own
	 */
	int l34_route6_wan;
	/** @l34_route6_wan_us: that WAN holds a reference on @l34_us_flow */
	bool l34_route6_wan_us;
	/** @l34_routed_warned: a routed host found no row, and it was said */
	bool l34_routed_warned;

	/**
	 * @l34_extip_nh: the next-hop row the external-address rows of the
	 * addresses and of the flows name, the uplink's gateway's
	 */
	unsigned int l34_extip_nh;
	/** @l34_us_flow: the GEM flow routed frames leave in, -1 for none */
	int l34_us_flow;
	/**
	 * @l34_us_refs: the references on @l34_us_flow, one for each device
	 * over it that has an address and one for the WAN of the default route
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
	 * @l34_nh_addr6: the IPv6 neighbour address of each next-hop row, the
	 * unspecified address for a row it does not hold
	 */
	struct in6_addr l34_nh_addr6[16];
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
	/** @l34_route6: the shadow of the IPv6 routing table */
	struct rtl9602c_l34_route6 l34_route6[32];
	/** @l34_route6_hw: the IPv6 route rows last written */
	u32 l34_route6_hw[8][5];
	/** @l34_route6_over: as @l34_route_over, for IPv6 */
	bool l34_route6_over;
	/** @l34_route6_warned: as @l34_route_warned, for IPv6 */
	bool l34_route6_warned;

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

	/** @gem_flows: the flows GEM ports hold; the MAC tells the switch of each */
	DECLARE_BITMAP(gem_flows, RTL9602C_GPON_NUM_FLOW);

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
	/** @gem_bridged_up: the flows in @gem_bridged that carry upstream traffic */
	DECLARE_BITMAP(gem_bridged_up, RTL9602C_GPON_NUM_FLOW);

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
	 * @ptp_lock: makes a clock latch and its reads, a step and its
	 * writes, or a write of the reference time, one operation; a spinlock,
	 * as the transmit path writes the reference time;
	 * initialised at probe, before any view that reads the clock exists
	 */
	spinlock_t		ptp_lock;

	/** @debugfs: the driver's debugfs directory */
	struct dentry		*debugfs;

	/**
	 * @i2c: what the optics' I2C master gets from the driver and gives it
	 * back (include/linux/platform_data/i2c-rtl9602c.h)
	 */
	struct rtl9602c_i2c_data	i2c;
	/**
	 * @i2c_nr: the number of the master's adapter once its driver has
	 * registered it, else -1; under @optics_lock
	 */
	int			i2c_nr;
	/**
	 * @i2c_adap: the adapter while @optics_lock is held and the master's
	 * driver is there, else NULL
	 */
	struct i2c_adapter	*i2c_adap;
	/**
	 * @optics_lock: held by every user of the optics for the whole of its
	 * sequence of transfers, with the adapter's bus lock inside it
	 */
	struct mutex		optics_lock;
	/**
	 * @optics_hwmon: the hwmon device of the diagnostics, once registered
	 */
	struct device		*optics_hwmon;
	/**
	 * @optics_apcset0: the power control's set point the module came
	 * with, that of transmit power level mode 0; under optics_lock
	 */
	u8			optics_apcset0;
	/** @optics_mod0: the modulation DAC the module came with */
	u16			optics_mod0;
	/** @optics_ref: @optics_apcset0 and @optics_mod0 are known */
	bool			optics_ref;
	/**
	 * @optics_known: the module declares itself as one whose laser driver
	 * the driver writes (rtl9602c_optics_identify()); without it the
	 * driver only reads the laser driver
	 */
	bool			optics_known;
	/**
	 * @optics_t2_open: table 2 is open for storing through the debugfs
	 * entry t2write, which keeps @optics_t2_eectl and @optics_t2_tabsel,
	 * EEPROM_CTRL and the page select as found; under optics_lock
	 */
	bool			optics_t2_open;
	/** @optics_t2_eectl: EEPROM_CTRL as found by t2write's "open" */
	u8			optics_t2_eectl;
	/** @optics_t2_tabsel: the page select as found by t2write's "open" */
	u8			optics_t2_tabsel;
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
		 * plane, the action and its classes, so that the rule can be
		 * moved
		 */
		u32		words[2 * RTL9602C_ACL_RULE_WORDS +
				      RTL9602C_ACL_ACT_WORDS + 1];
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
	 * @ptp_tx_lock: protects @ptp_tx_skb, @ptp_tx_start, @ptp_pdelay and
	 * the counters of transmit stamps; taken on the transmit path and in
	 * the conduit's receive poll
	 */
	spinlock_t	ptp_tx_lock;
	/**
	 * @ptp_tx_skb: the clone of the one PTP message whose departure
	 * report is awaited, NULL when none is
	 */
	struct sk_buff	*ptp_tx_skb;
	/** @ptp_tx_start: jiffies when @ptp_tx_skb was taken */
	unsigned long	ptp_tx_start;
	/** @ptp_tx_stamped: messages given their departure time */
	u32		ptp_tx_stamped;
	/** @ptp_tx_busy: messages refused a stamp, one being awaited */
	u32		ptp_tx_busy;
	/** @ptp_tx_lost: awaited messages whose report never came */
	u32		ptp_tx_lost;
	/** @ptp_tx_orphans: reports that came with no message awaited */
	u32		ptp_tx_orphans;
	/**
	 * @ptp_tx_ready: the conduit hands departure reports on; without
	 * it no port offers transmit stamping
	 */
	bool		ptp_tx_ready;
	/**
	 * @ptp_cfg_lock: serialises the PTP block's settings, which the
	 * timestamping configuration (under RTNL) and the transparent clock's
	 * devlink parameters change
	 */
	struct mutex	ptp_cfg_lock;
	/** @ptp_tc: the transparent clock's mode, under @ptp_cfg_lock */
	enum rtl9602c_ptp_tc ptp_tc;
	/**
	 * @ptp_path_delay: the path delay a peer-to-peer transparent clock
	 * adds to a Sync, in ns, under @ptp_cfg_lock
	 */
	u32		ptp_path_delay;
	/**
	 * @ptp_onestep_at: when the reference time was last written for a
	 * one-step message, under @ptp_lock
	 */
	ktime_t		ptp_onestep_at;
	/** @ptp_tx_onestep: one-step Syncs sent */
	u32		ptp_tx_onestep;
	/**
	 * @ptp_pdelay: per port, the last Pdelay_Req received on a port that
	 * answers one-step: its sequence ID, its source port identity and its
	 * receive time
	 */
	struct {
		bool		valid;
		u16		seq;
		u8		id[10];
		ktime_t		t2;
	} ptp_pdelay[RTL9602C_NUM_PORTS];
	/** @ptp_tx_p2p: one-step Pdelay_Resp sent with their turnaround */
	u32		ptp_tx_p2p;
	/** @ptp_tx_p2p_missed: one-step Pdelay_Resp whose Pdelay_Req was not known */
	u32		ptp_tx_p2p_missed;
	/**
	 * @meter_lock: serialises the meters, @meter_used and @cpu_meter, and
	 * the storm-control state, @storm, @storm_seq and @storm_meter, which
	 * tc and devlink both change
	 */
	struct mutex meter_lock;
	/** @meter_used: the meters handed out */
	DECLARE_BITMAP(meter_used, RTL9602C_METER_COUNT);
	/**
	 * @gem_ds_meter: the meter of the downstream limit of each GEM flow,
	 * plus one; 0 for a flow with none. Under RTNL, where the PON class
	 * changes a GEM port
	 */
	u8 gem_ds_meter[RTL9602C_GPON_NUM_FLOW];
	/**
	 * @gem_ds_meters: the meters of those limits, a bit each, which the
	 * interrupt handler reads and does not report as exceeded
	 */
	u16 gem_ds_meters;
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

	/** @pktdbg: the frame capture's setting, as armed */
	struct {
		/** @pktdbg.mode: the capture mode */
		u8	mode;
		/** @pktdbg.igr: the ingress port mask */
		u8	igr;
		/** @pktdbg.egr: the egress port mask */
		u8	egr;
		/** @pktdbg.reason: the forwarding reason mode 7 waits for */
		u8	reason;
	} pktdbg;

	/**
	 * @dos_flood_th: the flood thresholds last written, by
	 * RTL9602C_DOS_FLOOD_*; the registers are never read
	 */
	u8 dos_flood_th[3];
	/** @pktdbg_region: the frame_capture devlink region */
	struct devlink_region *pktdbg_region;
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
 * @RTL9602C_DEVLINK_PARAM_ID_L3_TTL_DEC: l3_ttl_decrement, whether the
 *	engine decrements the TTL or hop limit of what it routes
 * @RTL9602C_DEVLINK_PARAM_ID_SFP_LANE: sfp_lane, the settings the SFP lane
 *	may be brought up in
 * @RTL9602C_DEVLINK_PARAM_ID_SFP_LANE_NOW: sfp_lane_now, the setting it
 *	runs in
 * @RTL9602C_DEVLINK_PARAM_ID_PTP_TC: ptp_transparent_clock, "off", "e2e" or
 *	"p2p" (enum rtl9602c_ptp_tc)
 * @RTL9602C_DEVLINK_PARAM_ID_PTP_PATH_DELAY: ptp_path_delay_ns, the path
 *	delay a peer-to-peer transparent clock adds to a Sync
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
	RTL9602C_DEVLINK_PARAM_ID_L3_TTL_DEC,
	RTL9602C_DEVLINK_PARAM_ID_SFP_LANE,
	RTL9602C_DEVLINK_PARAM_ID_SFP_LANE_NOW,
	RTL9602C_DEVLINK_PARAM_ID_PTP_TC,
	RTL9602C_DEVLINK_PARAM_ID_PTP_PATH_DELAY,
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
int rtl9602c_optics_i2c_add(struct rtl9602c_priv *p);
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
void rtl9602c_meter_free(struct rtl9602c_priv *p, unsigned int meter);
int rtl9602c_gem_ds_limit(struct rtl9602c_priv *p, unsigned int flow, u64 rate,
			  u32 burst, struct netlink_ext_ack *extack);
void rtl9602c_gem_ds_forget(struct rtl9602c_priv *p, unsigned int flow);
void rtl9602c_offload_debugfs_init(struct rtl9602c_priv *p);
void rtl9602c_l2_debugfs_init(struct rtl9602c_priv *p);
void rtl9602c_optics_debugfs_init(struct rtl9602c_priv *p);
int rtl9602c_port_add_dscp_prio(struct dsa_switch *ds, int port, u8 dscp, u8 prio);
int rtl9602c_port_bridge_flags(struct dsa_switch *ds, int port,
			       struct switchdev_brport_flags flags, struct netlink_ext_ack *extack);
void rtl9602c_bridge_refresh(struct dsa_switch *ds);
void rtl9602c_mcast_reset(struct rtl9602c_priv *p);
void rtl9602c_l2_moves_init(struct rtl9602c_priv *p);
void rtl9602c_mcast_refresh(struct rtl9602c_priv *p);
void rtl9602c_gem_hosts_sync(struct rtl9602c_priv *p);
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
void rtl9602c_port_txtstamp(struct dsa_switch *ds, int port,
			    struct sk_buff *skb);
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
int rtl9602c_ptp_pon_tod(struct rtl9602c_priv *p, const struct timespec64 *ts,
			 unsigned long delay);
int rtl9602c_ptp_tc_set(struct rtl9602c_priv *p, const char *mode);
void rtl9602c_ptp_tc_get(struct rtl9602c_priv *p, char *buf, size_t len);
int rtl9602c_ptp_path_delay_set(struct rtl9602c_priv *p, u32 ns);
u32 rtl9602c_ptp_path_delay_get(struct rtl9602c_priv *p);
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

/* The GPON MAC's driver: rtl9602c-pon-host.c and the driver's own files. */
int rtl9602c_pon_attach(struct rtl9602c_priv *p);
void rtl9602c_pon_detach(struct rtl9602c_priv *p);
void rtl9602c_pon_irq_note(struct rtl9602c_priv *p);
int rtl9602c_pon_start(struct rtl9602c_priv *p);
void rtl9602c_pon_stop(struct rtl9602c_priv *p);
void rtl9602c_pon_operstate(struct rtl9602c_priv *p);
bool rtl9602c_pon_irq(struct rtl9602c_priv *p);
bool rtl9602c_pon_is_up(struct rtl9602c_priv *p);
int rtl9602c_pon_sn_set(struct rtl9602c_priv *p, const char *s);
int rtl9602c_pon_sn_get(struct rtl9602c_priv *p, char *buf, size_t len);
int rtl9602c_pon_password_set(struct rtl9602c_priv *p, const char *s);
bool rtl9602c_pon_password_is_set(struct rtl9602c_priv *p);
bool rtl9602c_pon_us_ready(struct rtl9602c_priv *p, unsigned int flow);
u8 rtl9602c_pon_power_mode(struct rtl9602c_priv *p);
void rtl9602c_pon_power_applied(struct rtl9602c_priv *p, u8 asked, u8 mode,
				int err);

#endif /* _RTL9602C_H */
