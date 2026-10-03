// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: the CPU port's DMA engine, as the DSA conduit.
 *
 * Switch port 3 has no MAC behind it, only a DMA engine that moves frames
 * between memory and the switch through a transmit and a receive descriptor
 * ring. What decides where a frame goes travels in the descriptors as the CPU
 * tag, never in the frame: on transmit the destination port, a priority and a
 * PON flow; on receive the source port, the reason the switch sent the frame to
 * the CPU and the PON flow it arrived on. The tagger therefore moves no bytes.
 * It passes the destination port as the queue mapping, and a received frame
 * reaches its user port through a METADATA_HW_PORT_MUX dst that names the
 * source port.
 *
 * The switch driver attaches receive handlers here for OMCI messages from the
 * PON MAC and for the frames of PON flows, and sends OMCI messages through
 * rtl9602c_conduit_ctrl_xmit(). The timestamp tag that the switch inserts into
 * the PTP messages it sends to the CPU is taken off here, its time kept as the
 * frame's hardware timestamp.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/unaligned.h>
#include <linux/netdevice.h>
#include <linux/seq_file.h>
#include <linux/dsa/rtl9602c.h>
#include <net/dsa.h>
#include <net/dst_metadata.h>
#include <linux/platform_device.h>

#include <asm/mach-rtl9602c/rtl9602c.h>

/*
 * The engine's registers. They are big-endian, and readl() and writel() access
 * them in native order on this big-endian CPU; byte and half-word registers are
 * accessed at their own width.
 */
#define RTL9602C_NIC_MAC_ADDR_HI	0x0000
#define RTL9602C_NIC_MAC_ADDR_LO	0x0004
#define RTL9602C_NIC_MC_HASH_LO	0x0008
#define RTL9602C_NIC_MC_HASH_HI	0x000c
#define RTL9602C_NIC_RX_NO_DESC_CNT	0x002a
#define RTL9602C_NIC_CTRL	0x003b
#define RTL9602C_NIC_TX_CFG	0x0040
#define RTL9602C_NIC_RX_FILTER	0x0044
#define RTL9602C_NIC_CPUTAG	0x0048
#define RTL9602C_NIC_CPUTAG_SRC	0x0050

/*
 * Written to RTL9602C_NIC_CPUTAG_SRC, this reads back as 0x4002 (measured on
 * this board): bits 7:4 hold nothing. Bits 3:0 (2) are taken to be the source
 * port reported for frames from the PON MAC, and bits 15:8 (64) a default flow;
 * neither is verified.
 */
#define RTL9602C_NIC_CPUTAG_SRC_INIT	0x00004072

#define RTL9602C_NIC_IRQ_MASK	0x003c
#define RTL9602C_NIC_IRQ_STAT	0x003e
#define RTL9602C_NIC_MEDIA_STAT	0x0058
#define RTL9602C_NIC_CFG	0x004c
#define RTL9602C_NIC_TX_RING_BASE	0x1300
#define RTL9602C_NIC_TX_RING_CUR	0x1304
#define RTL9602C_NIC_RX_RING_BASE	0x13f0
#define RTL9602C_NIC_RX_RING_CUR	0x13f4
#define RTL9602C_NIC_RX_RING_LEN	0x13f6
#define RTL9602C_NIC_RX_RING_LEN_HI	0x13f7
#define RTL9602C_NIC_REG_13FC	0x13fc
#define RTL9602C_NIC_RX_PAUSE_OFF_THR_HI	0x142c
#define RTL9602C_NIC_RX_RING_TAIL	0x1430
#define RTL9602C_NIC_RX_PAUSE_ON_THR	0x1431
#define RTL9602C_NIC_RX_PAUSE_OFF_THR	0x1432
#define RTL9602C_NIC_RX_RING_TAIL_HI	0x1433
#define RTL9602C_NIC_DMA_CTRL	0x1434
#define RTL9602C_NIC_RING_CFG	0x1438

/*
 * The second receive ring's registers, in a block of their own: the bus
 * address of descriptor 0, the engine's index into the ring, the ring's length
 * less one, the tail and the free-descriptor thresholds for asserting and
 * releasing receive pause. The thresholds are taken to be in bits 23:16 and
 * 7:0, which is not verified: pause is off. The engine is taken to have further
 * rings in blocks of 16 bytes after this one, which the driver leaves alone.
 */
#define RTL9602C_NIC_RX2_RING_BASE	0x1390
#define RTL9602C_NIC_RX2_RING_CUR	0x1394
#define RTL9602C_NIC_RX2_RING_LEN	0x1396
#define RTL9602C_NIC_RX2_RING_TAIL	0x1398
#define RTL9602C_NIC_RX2_PAUSE_THR	0x139c
#define RTL9602C_NIC_RX2_PAUSE_ON	GENMASK(23, 16)
#define RTL9602C_NIC_RX2_PAUSE_OFF	GENMASK(7, 0)

/*
 * One 4-bit field per internal priority, priority p in bits 4p + 3 to 4p,
 * names the receive ring for that priority: 0 the ring at
 * RTL9602C_NIC_RX_RING_BASE and 2 the ring at RTL9602C_NIC_RX2_RING_BASE
 * (measured on this board).
 */
#define RTL9602C_NIC_RX_PRI_TO_RING	0x1370
#define RTL9602C_NIC_RX_PRI_RING(pri, ring)	((u32)(ring) << (4 * (pri)))

/*
 * Values written to whole registers. The driver uses none of their fields
 * separately, and what the individual fields do is not verified;
 * RTL9602C_NIC_TX_CFG_IPG_3 is taken to set an inter-packet gap of 3 with
 * loopback off.
 */
#define RTL9602C_NIC_RX_FILTER_PROMISC	0x0000000f
#define RTL9602C_NIC_TX_CFG_IPG_3	0x00000c00
#define RTL9602C_NIC_DMA_CTRL_INIT	0xc059f130
#define RTL9602C_NIC_DMA_CTRL_TX_JUMBO	BIT(28)
#define RTL9602C_NIC_TX_CFG_JUMBO	BIT(15)
#define RTL9602C_NIC_CFG_TX_JUMBO	BIT(16)
#define RTL9602C_NIC_RING_CFG_INIT	0x32000001

/*
 * RTL9602C_NIC_CPUTAG_INIT, with the switch's own CPU-tag settings, makes the
 * switch obey the port mask of transmit word 3 and fill receive words 2 and 3.
 * It is written whole; what its fields are named after (enable, tag sizes and
 * format, a match value) is not verified.
 */
#define RTL9602C_NIC_CPUTAG_EN	BIT(31)
#define RTL9602C_NIC_CPUTAG_TX_SIZE_2	(2 << 27)
#define RTL9602C_NIC_CPUTAG_FMT_ONCHIP	(7 << 18)
#define RTL9602C_NIC_CPUTAG_RX_SIZE_2	(2 << 16)
#define RTL9602C_NIC_CPUTAG_PROTO_MATCH	((0xff << 8) | 0x04)
#define RTL9602C_NIC_CPUTAG_INIT	(RTL9602C_NIC_CPUTAG_EN | RTL9602C_NIC_CPUTAG_TX_SIZE_2 |  \
				 RTL9602C_NIC_CPUTAG_FMT_ONCHIP | RTL9602C_NIC_CPUTAG_RX_SIZE_2 | \
				 RTL9602C_NIC_CPUTAG_PROTO_MATCH)
#define RTL9602C_NIC_DMA_CTRL_TX_POLL	BIT(0)

/*
 * RTL9602C_NIC_CTRL is written whole rather than inherited, so the receive
 * format does not depend on what the bootloader left. RTL9602C_NIC_CTRL_RX_CSUM
 * is set, but no checksum result is read, and what the bit does is not
 * verified.
 */
#define RTL9602C_NIC_CTRL_RX_CSUM	BIT(1)

/*
 * Bit 2 stays clear: with it clear, a received frame keeps its 802.1Q tag. With
 * RTL9602C_NIC_CTRL_RX_JUMBO set, a 1522-byte VLAN-tagged frame is received
 * (measured on this board); that this bit is what admits frames above 1518
 * bytes is not verified.
 */
#define RTL9602C_NIC_CTRL_RX_JUMBO	BIT(3)
#define RTL9602C_NIC_CTRL_INIT	(RTL9602C_NIC_CTRL_RX_CSUM | RTL9602C_NIC_CTRL_RX_JUMBO)

/*
 * Pause between the engine and the switch, which the driver turns off. Measured
 * on this board: with bits 7, 6 and 5 set, a counter of the switch's CPU port
 * climbs with no traffic offered and nothing leaves by the SerDes port; with
 * 7 and 6 clear, transmit works, and the register reads 0x10. Which bit covers
 * which direction is not verified.
 */
#define RTL9602C_NIC_MEDIA_STAT_PAUSE_EN	(BIT(7) | BIT(6))

/*
 * RTL9602C_NIC_CFG is written with this field alone. Bit 24 in particular stays
 * clear, which keeps the ring's receive interrupt in RTL9602C_NIC_IRQ_STAT
 * (measured on this board). That the field sets a receive FIFO size is not
 * verified.
 */
#define RTL9602C_NIC_CFG_RX_FIFO_2K	(2 << 28)

/*
 * Transmit word 2. RTL9602C_NIC_TXD_CPUTAG_EN is set on every frame and taken
 * to say that the descriptor carries CPU-tag fields; with it set, word 3
 * decides the egress port (measured on this board). What the switch does with a
 * frame without it is not verified.
 */
#define RTL9602C_NIC_TXD_CPUTAG_EN	BIT(31)

/*
 * With RTL9602C_NIC_TXD_PRI_EN set, the switch takes the frame's internal
 * priority from RTL9602C_NIC_TXD_PRI, and its priority maps choose the egress
 * queue from it (measured on this board). On a data frame the rest of word 2 is
 * zero, which leaves the frame's own VLAN tag as it is and applies no PPPoE
 * action (also measured).
 */
#define RTL9602C_NIC_TXD_PRI_EN	BIT(30)
#define RTL9602C_NIC_TXD_PRI		GENMASK(29, 27)

/*
 * The timestamp tag the switch inserts into a PTP message it sends to the CPU:
 * 16 bytes between the source address and the message's own EtherType, opening
 * with EtherType 0x8899. Offsets are from the start of the frame; bytes
 * 16 to 21 are not used.
 */
#define RTL9602C_ETH_P_REALTEK	0x8899
#define RTL9602C_PTP_TAG_LEN	16
#define RTL9602C_PTP_TAG_PROTO_OFFSET	14
/* The protocol byte of a PTP tag: 4 in every tag seen on this board. */
#define RTL9602C_PTP_TAG_PROTO_PTP	4
#define RTL9602C_PTP_TAG_REASON_OFFSET	15
/*
 * The trap reasons in the receive descriptor of a frame that carries a tag:
 * a PTP message sent to the CPU, and a copy made by the ingress mirror
 * action. The tag's reason byte holds the same values.
 */
#define RTL9602C_RX_REASON_PTP		240
#define RTL9602C_RX_REASON_PTP_MIRROR	241
#define RTL9602C_PTP_TAG_SEC_OFFSET	22
#define RTL9602C_PTP_TAG_NSEC_OFFSET	24

/*
 * Transmit word 3: the destination port mask, bit n for switch port n. The
 * switch sends the frame out of the port the mask names, and the driver always
 * names exactly one; a zero mask would not drop the frame but leave it to the
 * switch's own forwarding (both measured on this board).
 */
#define RTL9602C_NIC_TXD_DST_PORTS	GENMASK(28, 23)
#define RTL9602C_NIC_TXD_DST_PORT(p)						\
	FIELD_PREP(RTL9602C_NIC_TXD_DST_PORTS,				\
		   BIT((p) & (RTL9602C_SW_PORTS - 1)))

/* The SerDes lane to the host, where a frame goes when nothing chose a port. */
#define RTL9602C_PORT_SFP	0
#define RTL9602C_NIC_TXD_DST_SFP	RTL9602C_NIC_TXD_DST_PORT(RTL9602C_PORT_SFP)

/*
 * RTL9602C_NIC_IRQ_MASK and RTL9602C_NIC_IRQ_STAT share this layout; a status
 * bit is acknowledged by writing it back as 1. Bit 0 is a frame received, into
 * either receive ring (measured on this board), and bit 6 a transmit
 * completed, which the driver needs to reap completed descriptors and wake the
 * queues while nothing is being received. The other four are named after what
 * they are taken to mean, which is not verified: every status bit is handled
 * alike, acknowledged and followed by a poll, so nothing depends on their
 * meaning.
 */
#define RTL9602C_NIC_IRQ_RX_DONE	BIT(0)
#define RTL9602C_NIC_IRQ_RX_RUNT	BIT(2)
#define RTL9602C_NIC_IRQ_RX_OVERFLOW	BIT(4)
#define RTL9602C_NIC_IRQ_RX_NO_DESC	BIT(5)
#define RTL9602C_NIC_IRQ_TX_DONE	BIT(6)
#define RTL9602C_NIC_IRQ_TX_ERR	BIT(7)
#define RTL9602C_NIC_IRQ_MASK_RXTX	(RTL9602C_NIC_IRQ_RX_DONE | RTL9602C_NIC_IRQ_RX_RUNT | \
				 RTL9602C_NIC_IRQ_RX_OVERFLOW | RTL9602C_NIC_IRQ_RX_NO_DESC | \
				 RTL9602C_NIC_IRQ_TX_DONE | RTL9602C_NIC_IRQ_TX_ERR)

/*
 * Word 0, both directions. With RTL9602C_NIC_DESC_OWN set the descriptor is the
 * engine's: the driver sets it when handing a descriptor over, and the engine
 * clears it once it has sent the frame or filled the buffer.
 * RTL9602C_NIC_DESC_RING_END marks the last descriptor, after which the engine
 * returns to descriptor 0. Every frame is sent from one buffer, as its own
 * first and last segment, and received into a buffer large enough for it.
 * RTL9602C_NIC_TXD_LEN is the frame's length without the FCS.
 */
#define RTL9602C_NIC_DESC_OWN	BIT(31)
#define RTL9602C_NIC_DESC_RING_END	BIT(30)
#define RTL9602C_NIC_DESC_FIRST_SEG	BIT(29)
#define RTL9602C_NIC_DESC_LAST_SEG	BIT(28)
#define RTL9602C_NIC_DESC_SEGS \
	(RTL9602C_NIC_DESC_FIRST_SEG | RTL9602C_NIC_DESC_LAST_SEG)
#define RTL9602C_NIC_TXD_LEN	GENMASK(16, 0)

/*
 * Set on every frame. RTL9602C_NIC_TXD_ADD_FCS (word 0) makes the engine append
 * the FCS; without it the switch discards every frame from the CPU as a CRC
 * error (measured on this board). RTL9602C_NIC_TXD_NO_REWRITE (word 0) is meant
 * to keep the switch from rewriting a frame the CPU has already aimed; its
 * effect is not verified. RTL9602C_NIC_TXD_L34_BYPASS (word 3) is taken, as its
 * name says, to keep the frame away from the L3 engine, which is not verified:
 * a frame aimed by port mask leaves unchanged with it clear, even with the L3
 * engine on (measured).
 */
#define RTL9602C_NIC_TXD_ADD_FCS	BIT(23)
#define RTL9602C_NIC_TXD_NO_REWRITE	BIT(25)
#define RTL9602C_NIC_TXD_L34_BYPASS	BIT(1)

/*
 * RTL9602C_NIC_TXD_FLOW (word 3) is the PON flow, 0 to 127, that a frame goes
 * upstream in: the PON MAC files the frame under that flow's T-CONT queue and
 * GEM Port-ID. RTL9602C_NIC_TXD_FLOW_SEL (word 0) is set whenever word 3 names
 * a flow; whether the flow is honoured without it is not verified.
 * RTL9602C_NIC_TXD_IP_CSUM and RTL9602C_NIC_TXD_NO_LEARN (word 0), and
 * RTL9602C_NIC_TXD_FID_EXT_EN (word 2), are set on OMCI messages only, and what
 * they do is not verified; the last is taken to mark as valid a
 * filtering-database id, which is left 0.
 */
#define RTL9602C_NIC_TXD_IP_CSUM	BIT(27)
#define RTL9602C_NIC_TXD_NO_LEARN	BIT(21)
#define RTL9602C_NIC_TXD_FLOW_SEL	BIT(18)
#define RTL9602C_NIC_TXD_FID_EXT_EN	BIT(19)
#define RTL9602C_NIC_TXD_FLOW	GENMASK(22, 16)

/*
 * Receive words 2 and 3 are filled only for a frame the switch attached a CPU
 * tag to, which RTL9602C_NIC_RXD_CPUTAG_VALID (word 2) says; the switch driver
 * has one attached to every frame sent to the CPU. RTL9602C_NIC_RXD_FLOW
 * (word 2) is the PON flow, 0 to 127, that a frame from the PON port arrived
 * on, and RTL9602C_NIC_RXD_SRC_PORT (word 3) the switch port the frame entered
 * by.
 */
#define RTL9602C_NIC_RXD_CPUTAG_VALID	BIT(31)
#define RTL9602C_NIC_RXD_FLOW	GENMASK(26, 20)
#define RTL9602C_NIC_RXD_SRC_PORT	GENMASK(31, 28)

/*
 * Receive word 3: why the switch sent the frame to the CPU. 0 is ordinary
 * forwarding; the other values are set by the blocks that trap frames to the
 * CPU.
 */
#define RTL9602C_NIC_RXD_TRAP_REASON	GENMASK(20, 13)
#define RTL9602C_RX_REASONS	256

/*
 * How many received frames, and OMCI messages each way, are logged with their
 * descriptor words.
 */
#define RTL9602C_RX_LOG		4

/*
 * Switch ports: 0 the SerDes lane to the host, 1 unused on this board,
 * 2 the PON MAC, 3 the CPU.
 */
#define RTL9602C_SW_PORTS	4

/*
 * Receive word 0: taken to mark a frame that failed its FCS, and one whose DMA
 * did not complete. Neither meaning is verified.
 */
#define RTL9602C_NIC_RXD_FCS_ERR	BIT(27)
#define RTL9602C_NIC_RXD_DMA_ERR	BIT(24)

/*
 * Receive word 0: an OMCI message from the PON MAC. It has no Ethernet header:
 * once the offset and the FCS are off, the data starts at the message.
 */
#define RTL9602C_NIC_RXD_OMCI	BIT(14)

/*
 * The received length counts the FCS, but not the 2 bytes the engine leaves
 * free at the start of the buffer.
 */
#define RTL9602C_NIC_RXD_LEN		GENMASK(11, 0)
#define RTL9602C_NIC_RX_FCS_LEN		4
#define RTL9602C_NIC_RX_DATA_OFFSET	2

/*
 * One transmit ring serves every queue and the OMCI path. One descriptor is
 * always left unused, so that head equal to tail means empty.
 */
#define RTL9602C_TX_RING	32

/*
 * Two receive rings: the data ring for internal priorities 0 to 6 and the
 * control ring for 7, the priority the switch driver gives the OMCI messages it
 * traps. Measured on this board, the CPU receives at most about 24,000
 * frames/s, and beyond that the engine drops what it has no descriptor for;
 * with one ring for every priority, a flood to the CPU dropped OMCI messages in
 * the engine like any other frame. In a ring of their own they keep free
 * descriptors whatever the data ring holds, and the poll serves that ring
 * first.
 */
#define RTL9602C_RX_DATA	0
#define RTL9602C_RX_CTRL	1
#define RTL9602C_RX_RINGS	2

static const char * const rtl9602c_rx_ring_name[RTL9602C_RX_RINGS] = {
	[RTL9602C_RX_DATA] = "data",
	[RTL9602C_RX_CTRL] = "control",
};

/*
 * At 20,000 frames/s a data ring of 16 descriptors loses frames and one of 256
 * does not (measured on this board). Only the low byte of the data ring's tail
 * is written per frame, so that ring can have no more than 256 descriptors. The
 * control ring carries OMCI alone unless something else is given priority 7:
 * at registration some 420 messages in a few seconds, then one every few
 * seconds.
 */
#define RTL9602C_RX_RING	256
static_assert(RTL9602C_RX_RING <= 256);
#define RTL9602C_RX_CTRL_RING	32

/* The buffer size each receive descriptor advertises to the engine. */
#define RTL9602C_RX_BUF		0x600

/*
 * Allocated beyond RTL9602C_RX_BUF, so that a frame of the advertised size,
 * written 2 bytes in, still ends inside the buffer. Whether the engine ever
 * writes that far is not verified; the margin makes it harmless.
 */
#define RTL9602C_RX_SLACK	64
#define RTL9602C_RX_ALLOC	(RTL9602C_RX_BUF + RTL9602C_RX_SLACK)
/*
 * A received frame is longer than the MTU by its header, two VLAN tags, a PTP
 * timestamp tag and the FCS at most.
 */
#define RTL9602C_RX_OVERHEAD	(ETH_HLEN + 2 * VLAN_HLEN + RTL9602C_PTP_TAG_LEN + \
				 RTL9602C_NIC_RX_FCS_LEN)
/*
 * The largest MTU leaves room for PPPoE's 8 bytes of headers around a 1500-byte
 * packet (RFC 4638), and no more. In its default mode the transmit engine stops
 * on long frames, keeping the descriptor its own, and nothing more leaves until
 * the conduit is restarted: one at a time it sent frames of up to 1820 bytes,
 * FCS not counted, and back to back frames of 1751 bytes and more stopped it.
 * Its jumbo mode, RTL9602C_NIC_TX_CFG_JUMBO, RTL9602C_NIC_CFG_TX_JUMBO and
 * RTL9602C_NIC_DMA_CTRL_TX_JUMBO together, sent frames of up to 8000 bytes back
 * to back and went on sending (all measured); the driver does not use it.
 * RTL9602C_NIC_TX_MAX is the longest frame that the MTU lets the stack make,
 * with the header and two VLAN tags; the driver drops a longer one.
 */
#define RTL9602C_ETH_MAX_MTU	(ETH_DATA_LEN + 8)
#define RTL9602C_NIC_TX_MAX	(RTL9602C_ETH_MAX_MTU + ETH_HLEN + 2 * VLAN_HLEN)

/*
 * Descriptors are big-endian words in coherent memory, which is uncached on
 * this CPU (it is not cache-coherent with DMA), so the driver and the engine
 * see each other's stores without cache maintenance.
 *
 * Transmit: word 0 status and length, word 1 the buffer's bus address, word 2
 * CPU-tag flags and priority, word 3 the destination, word 4 large-send
 * offload, always 0 here.
 */
struct rtl9602c_tx_desc {
	__be32 stslen;
	__be32 addr;
	__be32 tag;
	__be32 dest;
	__be32 lso;
};

/* Receive: the same four roles, word 3 saying where the frame came from. */
struct rtl9602c_rx_desc {
	__be32 stslen;
	__be32 addr;
	__be32 tag;
	__be32 dest;
};

/*
 * A receive ring: its descriptors and their bus address, each descriptor's
 * buffer and its mapping, the ring's length, the next descriptor to look at,
 * and the frames taken from it.
 */
struct rtl9602c_rx_ring {
	struct rtl9602c_rx_desc *desc;
	dma_addr_t dma;
	struct sk_buff **skb;
	dma_addr_t *buf;
	unsigned int len;
	unsigned int tail;
	u32 frames;
	/*
	 * A frame arriving in pieces (rtl9602c_eth_rx_piece()): the buffer it
	 * is gathered in, NULL if none could be had, and word 0 and the CPU-tag
	 * words of its first descriptor.
	 */
	struct sk_buff *head;
	bool gathering;
	bool head_bad;
	u32 head_sts, head_w2, head_w3;
};

struct rtl9602c_eth {
	struct net_device *netdev;
	void __iomem *base;
	struct device *dev;

	/* The transmit ring and the receive rings. */
	struct rtl9602c_tx_desc *tx;
	dma_addr_t tx_dma;
	struct rtl9602c_rx_ring rx[RTL9602C_RX_RINGS];

	/* Tagged frames received per trap reason, shown through debugfs. */
	u32 rx_reason[RTL9602C_RX_REASONS];
	struct dentry *debugfs;

	/*
	 * The frame each transmit descriptor carries and its mapping; NULL when
	 * there is none, as for the test frame.
	 */
	struct sk_buff *tx_skb[RTL9602C_TX_RING];
	dma_addr_t tx_buf[RTL9602C_TX_RING];

	/*
	 * Serialises the transmit paths and the reaper: protects tx_head,
	 * tx_tail, tx_skb[], tx_buf[] and the transmit descriptors, and the
	 * stopping and waking of the queues.
	 */
	spinlock_t tx_lock;
	/* Next descriptor to post; oldest descriptor not yet reaped. */
	unsigned int tx_head;
	unsigned int tx_tail;
	/* RTL9602C_NIC_RX_NO_DESC_CNT at the last poll. */
	u16 rx_missed_last;

	/* What has been logged: frames, source ports and PTP tag reasons. */
	unsigned int rx_logged;
	unsigned int rx_ports_seen;
	DECLARE_BITMAP(ptp_reason_seen, 256);

	struct napi_struct napi;
	int irq;

	/* One METADATA_HW_PORT_MUX dst per switch port, for received frames. */
	struct metadata_dst *port_meta[RTL9602C_SW_PORTS];

	/*
	 * The switch driver's receive handlers, for OMCI messages and for data
	 * frames, with their contexts; and how many OMCI messages have been
	 * logged each way. A handler is published after its context, with a
	 * release that the receive path pairs with an acquire.
	 */
	rtl9602c_ctrl_rx_t ctrl_rx;
	void *ctrl_ctx;
	unsigned int ctrl_logged;
	unsigned int ctrl_tx_logged;
	rtl9602c_flow_rx_t flow_rx;
	void *flow_ctx;
};

/*
 * The bootloader leaves the unit's address in the identity registers:
 * bytes 0 to 3 in RTL9602C_NIC_MAC_ADDR_HI, byte 0 in bits 31:24, and
 * bytes 4 and 5 in bits 31:16 of RTL9602C_NIC_MAC_ADDR_LO. The driver adopts it
 * and never writes these registers.
 */
static void rtl9602c_eth_read_addr(void __iomem *base, u8 *addr)
{
	u32 id0 = readl(base + RTL9602C_NIC_MAC_ADDR_HI);
	u32 id1 = readl(base + RTL9602C_NIC_MAC_ADDR_LO);

	addr[0] = id0 >> 24;
	addr[1] = id0 >> 16;
	addr[2] = id0 >> 8;
	addr[3] = id0;
	addr[4] = id1 >> 24;
	addr[5] = id1 >> 16;
}

/* Stop the engine, then clear its ring configuration. */
static void rtl9602c_eth_halt(void __iomem *base)
{
	writel(0, base + RTL9602C_NIC_DMA_CTRL);
	writel(0, base + RTL9602C_NIC_RING_CFG);
}

/*
 * Bring the engine to a state of its own, whatever ran before this kernel left
 * it in. What ran before may have used the engine and left it enabled, with
 * rings of its own in memory that this kernel now owns: the engine may then
 * still be working through those rings. Measured on this board, a transmit
 * test that follows such a start finds its descriptor still held by the engine
 * (see rtl9602c_eth_selftest()). So the probe stops the engine before it
 * allocates anything, silences its interrupt, waits out a frame in flight and
 * acknowledges what is pending; only then are rings set up. The stop is the
 * same two writes as in "ip link set cpu down", after which the engine takes
 * its rings: the transmit test at open passes.
 *
 * That these writes stop the engine in every state it can be left in, and that
 * the wait is long enough, are not verified; the wait is far longer than the
 * transfer of one frame. The registers as found, and the control register after
 * the stop, are logged.
 */
static void rtl9602c_eth_quiesce(struct device *dev, void __iomem *base)
{
	u32 ctrl = readl(base + RTL9602C_NIC_DMA_CTRL);
	u16 status = readw(base + RTL9602C_NIC_IRQ_STAT);
	u32 tx_base = readl(base + RTL9602C_NIC_TX_RING_BASE);
	u16 tx_cur = readw(base + RTL9602C_NIC_TX_RING_CUR);
	u32 rx_base = readl(base + RTL9602C_NIC_RX_RING_BASE);
	u16 rx_cur = readw(base + RTL9602C_NIC_RX_RING_CUR);
	u32 rx2_base = readl(base + RTL9602C_NIC_RX2_RING_BASE);
	u16 rx2_cur = readw(base + RTL9602C_NIC_RX2_RING_CUR);

	writew(0, base + RTL9602C_NIC_IRQ_MASK);
	rtl9602c_eth_halt(base);
	/*
	 * The read cannot complete before the writes ahead of it, so the engine
	 * has been told to stop before the wait begins.
	 */
	readl(base + RTL9602C_NIC_DMA_CTRL);
	usleep_range(1000, 2000);
	writew(0xffff, base + RTL9602C_NIC_IRQ_STAT);

	dev_info(dev,
		 "engine found with DMA_CTRL %08x, IRQ_STAT %04x, TX ring %08x at %u, RX rings %08x at %u and %08x at %u; stopped, DMA_CTRL %08x\n",
		 ctrl, status, tx_base, tx_cur, rx_base, rx_cur, rx2_base,
		 rx2_cur, readl(base + RTL9602C_NIC_DMA_CTRL));
}

/*
 * Unmap and free every buffer, then the rings; also unwinds rings that were
 * only partly set up.
 */
static void rtl9602c_eth_free_rings(struct rtl9602c_eth *priv)
{
	unsigned int i, r;

	for (r = 0; r < RTL9602C_RX_RINGS; r++) {
		struct rtl9602c_rx_ring *ring = &priv->rx[r];

		for (i = 0; i < ring->len; i++) {
			if (!ring->skb[i])
				continue;
			dma_unmap_single(priv->dev, ring->buf[i],
					 RTL9602C_RX_ALLOC, DMA_FROM_DEVICE);
			dev_kfree_skb(ring->skb[i]);
			ring->skb[i] = NULL;
		}
		dev_kfree_skb(ring->head);
		ring->head = NULL;
		ring->gathering = false;
	}

	for (i = 0; i < RTL9602C_TX_RING; i++) {
		if (!priv->tx_skb[i])
			continue;
		dma_unmap_single(priv->dev, priv->tx_buf[i],
				 priv->tx_skb[i]->len, DMA_TO_DEVICE);
		dev_kfree_skb(priv->tx_skb[i]);
		priv->tx_skb[i] = NULL;
	}

	if (priv->tx) {
		dma_free_coherent(priv->dev,
				  RTL9602C_TX_RING * sizeof(*priv->tx),
				  priv->tx, priv->tx_dma);
		priv->tx = NULL;
	}
	for (r = 0; r < RTL9602C_RX_RINGS; r++) {
		struct rtl9602c_rx_ring *ring = &priv->rx[r];

		if (!ring->desc)
			continue;
		dma_free_coherent(priv->dev, ring->len * sizeof(*ring->desc),
				  ring->desc, ring->dma);
		ring->desc = NULL;
	}
}

static struct sk_buff *rtl9602c_eth_rx_alloc(struct net_device *dev,
					     dma_addr_t *dma)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	struct sk_buff *skb;

	skb = netdev_alloc_skb(dev, RTL9602C_RX_ALLOC);
	if (!skb)
		return NULL;

	*dma = dma_map_single(priv->dev, skb->data, RTL9602C_RX_ALLOC,
			      DMA_FROM_DEVICE);
	if (dma_mapping_error(priv->dev, *dma)) {
		dev_kfree_skb_any(skb);
		return NULL;
	}
	return skb;
}

/*
 * Hand descriptor @i of @ring to the engine with @skb, mapped at @dma.
 * Words 2 and 3 are cleared: the engine writes them only for a frame with a CPU
 * tag, and a value left from an earlier frame must not pass for this one's.
 */
static void rtl9602c_eth_rx_give(struct rtl9602c_rx_ring *ring, unsigned int i,
				 struct sk_buff *skb, dma_addr_t dma)
{
	struct rtl9602c_rx_desc *desc = &ring->desc[i];
	u32 sts = RTL9602C_NIC_DESC_OWN | RTL9602C_RX_BUF;

	if (i == ring->len - 1)
		sts |= RTL9602C_NIC_DESC_RING_END;

	ring->skb[i] = skb;
	ring->buf[i] = dma;
	desc->tag = 0;
	desc->dest = 0;
	desc->addr = cpu_to_be32(dma);
	/*
	 * The address and the cleared words must reach memory before word 0
	 * hands the descriptor to the engine.
	 */
	dma_wmb();
	desc->stslen = cpu_to_be32(sts);
}

/*
 * Take the frame out of descriptor @i of @ring and give the descriptor a new
 * buffer. The new buffer comes first: without one, NULL is returned and the
 * frame stays in its buffer, which the caller gives back to the descriptor, so
 * the ring never holds a descriptor without a buffer.
 */
static struct sk_buff *rtl9602c_eth_rx_swap(struct net_device *dev,
					    struct rtl9602c_rx_ring *ring,
					    unsigned int i)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	struct sk_buff *skb = ring->skb[i], *fresh;
	dma_addr_t dma;

	fresh = rtl9602c_eth_rx_alloc(dev, &dma);
	if (!fresh)
		return NULL;

	dma_unmap_single(priv->dev, ring->buf[i], RTL9602C_RX_ALLOC,
			 DMA_FROM_DEVICE);
	rtl9602c_eth_rx_give(ring, i, fresh, dma);
	return skb;
}

#define eth_info(dev, fmt, ...) \
	pr_info("rtl9602c-eth %s: " fmt, (dev)->name, ##__VA_ARGS__)
#define eth_warn(dev, fmt, ...) \
	pr_warn("rtl9602c-eth %s: " fmt, (dev)->name, ##__VA_ARGS__)
#define eth_err(dev, fmt, ...) \
	pr_err("rtl9602c-eth %s: " fmt, (dev)->name, ##__VA_ARGS__)

/*
 * Allocate the rings, zeroed, and give every receive descriptor a buffer: once
 * started, the engine may fill any of them.
 */
static int rtl9602c_eth_alloc_rings(struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	unsigned int i, r;

	priv->tx = dma_alloc_coherent(priv->dev,
				      RTL9602C_TX_RING * sizeof(*priv->tx),
				      &priv->tx_dma, GFP_KERNEL);
	if (!priv->tx)
		goto fail;

	priv->tx_head = 0;
	priv->tx_tail = 0;
	priv->tx[RTL9602C_TX_RING - 1].stslen = cpu_to_be32(RTL9602C_NIC_DESC_RING_END);

	priv->rx_logged = 0;
	priv->rx_ports_seen = 0;
	for (r = 0; r < RTL9602C_RX_RINGS; r++) {
		struct rtl9602c_rx_ring *ring = &priv->rx[r];

		ring->desc = dma_alloc_coherent(priv->dev,
						ring->len * sizeof(*ring->desc),
						&ring->dma, GFP_KERNEL);
		if (!ring->desc)
			goto fail;

		ring->tail = 0;
		ring->frames = 0;
		for (i = 0; i < ring->len; i++) {
			struct sk_buff *skb;
			dma_addr_t dma;

			skb = rtl9602c_eth_rx_alloc(dev, &dma);
			if (!skb)
				goto fail;
			rtl9602c_eth_rx_give(ring, i, skb, dma);
		}
	}

	return 0;

fail:
	rtl9602c_eth_free_rings(priv);
	return -ENOMEM;
}

/* Program the engine and start it; the rings must be allocated. */
static void rtl9602c_eth_start(struct rtl9602c_eth *priv)
{
	struct rtl9602c_rx_ring *ctrl = &priv->rx[RTL9602C_RX_CTRL];
	u32 pri_map;
	u16 cur;

	/*
	 * The bus address of descriptor 0, as dma_alloc_coherent() returned it,
	 * with no uncached-alias bits added. RTL9602C_NIC_TX_RING_CUR and
	 * RTL9602C_NIC_RX_RING_CUR are the engine's own indices into the rings;
	 * zeroed, both sides start at descriptor 0.
	 */
	writel(priv->tx_dma, priv->base + RTL9602C_NIC_TX_RING_BASE);
	writew(0, priv->base + RTL9602C_NIC_TX_RING_CUR);
	writel(priv->rx[RTL9602C_RX_DATA].dma, priv->base + RTL9602C_NIC_RX_RING_BASE);
	writew(0, priv->base + RTL9602C_NIC_RX_RING_CUR);

	/*
	 * Accept every frame the switch hands over, whatever its destination:
	 * the switch has already decided what reaches the CPU.
	 */
	writel(RTL9602C_NIC_RX_FILTER_PROMISC, priv->base + RTL9602C_NIC_RX_FILTER);
	writel(RTL9602C_NIC_TX_CFG_IPG_3, priv->base + RTL9602C_NIC_TX_CFG);

	writeb(RTL9602C_NIC_CTRL_INIT, priv->base + RTL9602C_NIC_CTRL);

	/* A ring of RTL9602C_RX_RING descriptors, all of them the engine's. */
	writeb(RTL9602C_RX_RING - 1, priv->base + RTL9602C_NIC_RX_RING_LEN);
	writeb(RTL9602C_RX_RING - 1, priv->base + RTL9602C_NIC_RX_RING_TAIL);

	/*
	 * Free-descriptor thresholds for asserting and releasing receive pause
	 * toward the switch. Measured on this board, they have no effect: the
	 * switch's side of that pause is not enabled, so under a flood it
	 * delivers every frame and the engine drops what it has no descriptor
	 * for.
	 */
	writeb(16, priv->base + RTL9602C_NIC_RX_PAUSE_ON_THR);
	writeb(48, priv->base + RTL9602C_NIC_RX_PAUSE_OFF_THR);

	/*
	 * The high parts are written rather than inherited, so that a value
	 * left over cannot make the engine walk a longer ring than was
	 * allocated. RTL9602C_NIC_RX_RING_TAIL_HI holds those of the tail and
	 * of the pause-on threshold; that RTL9602C_NIC_RX_RING_LEN_HI holds the
	 * length's is not verified.
	 */
	writeb(0, priv->base + RTL9602C_NIC_RX_RING_LEN_HI);
	writeb(0, priv->base + RTL9602C_NIC_RX_RING_TAIL_HI);
	writeb(0, priv->base + RTL9602C_NIC_RX_PAUSE_OFF_THR_HI);

	/*
	 * The control ring: every descriptor the engine's, and pause thresholds
	 * at the same share of the ring as the data ring's, which have no effect
	 * while pause is off. Its index cannot be written: measured on this
	 * board, it keeps the value it had when the engine stopped whatever is
	 * written to it, 16 or 32 bits wide, so the driver starts where the
	 * engine is. An index beyond the ring, which only a longer ring left by
	 * earlier software could leave, would have the engine write past the
	 * ring; priority 7 then stays on the data ring.
	 */
	writel(ctrl->dma, priv->base + RTL9602C_NIC_RX2_RING_BASE);
	writew(ctrl->len - 1, priv->base + RTL9602C_NIC_RX2_RING_LEN);
	cur = readw(priv->base + RTL9602C_NIC_RX2_RING_CUR);
	if (cur < ctrl->len) {
		ctrl->tail = cur;
		pri_map = RTL9602C_NIC_RX_PRI_RING(7, 2);
	} else {
		eth_warn(priv->netdev,
			 "control ring index %u is beyond the ring; OMCI shares the data ring\n",
			 cur);
		pri_map = 0;
	}
	writel((ctrl->tail + ctrl->len - 1) % ctrl->len,
	       priv->base + RTL9602C_NIC_RX2_RING_TAIL);
	writel(FIELD_PREP(RTL9602C_NIC_RX2_PAUSE_ON, ctrl->len / 16) |
	       FIELD_PREP(RTL9602C_NIC_RX2_PAUSE_OFF, ctrl->len * 3 / 16),
	       priv->base + RTL9602C_NIC_RX2_PAUSE_THR);

	/* What this register does is not known; it is cleared. */
	writel(0, priv->base + RTL9602C_NIC_REG_13FC);

	/* The multicast hash filter, all ones: no group is filtered out. */
	writel(0xffffffff, priv->base + RTL9602C_NIC_MC_HASH_LO);
	writel(0xffffffff, priv->base + RTL9602C_NIC_MC_HASH_HI);

	writel(RTL9602C_NIC_CFG_RX_FIFO_2K, priv->base + RTL9602C_NIC_CFG);
	/* Priority 7 to the control ring, the others to the data ring. */
	writel(pri_map, priv->base + RTL9602C_NIC_RX_PRI_TO_RING);

	/* No pause between the engine and the switch; other bits are kept. */
	writeb(readb(priv->base + RTL9602C_NIC_MEDIA_STAT) & ~RTL9602C_NIC_MEDIA_STAT_PAUSE_EN,
	       priv->base + RTL9602C_NIC_MEDIA_STAT);

	/* The CPU tag in the descriptors, both directions. */
	writel(RTL9602C_NIC_CPUTAG_INIT, priv->base + RTL9602C_NIC_CPUTAG);
	writel(RTL9602C_NIC_CPUTAG_SRC_INIT, priv->base + RTL9602C_NIC_CPUTAG_SRC);

	/*
	 * Configure the rings, then start the engine: RTL9602C_NIC_DMA_CTRL
	 * goes last.
	 */
	writel(RTL9602C_NIC_RING_CFG_INIT, priv->base + RTL9602C_NIC_RING_CFG);
	writel(RTL9602C_NIC_DMA_CTRL_INIT, priv->base + RTL9602C_NIC_DMA_CTRL);
}

/*
 * Tell the engine that the transmit ring has work. The poll bit is written
 * together with the configured word rather than by read-modify-write: the word
 * is known, and a read could only keep a bit the hardware set.
 */
static void rtl9602c_eth_kick(struct rtl9602c_eth *priv)
{
	/*
	 * Order the descriptor stores before the read and the doorbell write
	 * below: every word must be in memory once the engine is told.
	 */
	wmb();

	/*
	 * A read from the device cannot complete before the writes ahead of it,
	 * so the descriptor is in memory when the engine is told. Whether
	 * stores to uncached memory could otherwise still be in flight at the
	 * kick is not verified.
	 */
	readl(priv->base + RTL9602C_NIC_DMA_CTRL);

	writel(RTL9602C_NIC_DMA_CTRL_INIT | RTL9602C_NIC_DMA_CTRL_TX_POLL,
	       priv->base + RTL9602C_NIC_DMA_CTRL);
}

/*
 * Free what the engine has sent, oldest first, up to the first descriptor it
 * still owns; only this counts transmitted frames. It runs from the poll, which
 * a completed transmit schedules, and at stop. The queues are stopped when the
 * ring fills and woken here, both under tx_lock, so a wake cannot be lost
 * between a full ring and a reap.
 */
static void rtl9602c_eth_reap(struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	bool freed = false;

	spin_lock_bh(&priv->tx_lock);
	while (priv->tx_tail != priv->tx_head) {
		unsigned int i = priv->tx_tail;

		/*
		 * The engine clears OWN; READ_ONCE loads word 0 afresh on every
		 * pass. Nothing else is read from the descriptor, so no read
		 * barrier is needed.
		 */
		if (be32_to_cpu(READ_ONCE(priv->tx[i].stslen)) &
		    RTL9602C_NIC_DESC_OWN)
			break;

		if (priv->tx_skb[i]) {
			dma_unmap_single(priv->dev, priv->tx_buf[i],
					 priv->tx_skb[i]->len, DMA_TO_DEVICE);
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += priv->tx_skb[i]->len;
			dev_kfree_skb_any(priv->tx_skb[i]);
			priv->tx_skb[i] = NULL;
		}
		priv->tx_tail = (i + 1) % RTL9602C_TX_RING;
		freed = true;
	}

	if (freed)
		netif_tx_wake_all_queues(dev);
	spin_unlock_bh(&priv->tx_lock);
}

/* Free transmit descriptors, one always kept unused. */
static unsigned int rtl9602c_eth_tx_room(const struct rtl9602c_eth *priv)
{
	return (priv->tx_tail + RTL9602C_TX_RING - priv->tx_head - 1) %
	       RTL9602C_TX_RING;
}

/*
 * The transmit test: one minimum-size broadcast frame from the interface's own
 * address, aimed at the SerDes port from the descriptor at tx_head, a kick,
 * then up to 100 waits of 100 to 200 us for the engine to release the
 * descriptor. Returns 0 if it did, -ETIMEDOUT if it kept it, -ENOMEM without
 * memory for the frame.
 *
 * The engine advances its own transmit index over every descriptor it takes,
 * and nothing reconciles that index with the driver's, so on success both
 * driver indices step past the descriptor too.
 */
static int rtl9602c_eth_test_frame(struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	unsigned int i = priv->tx_head;
	dma_addr_t dma;
	unsigned int spin;
	u32 sts;
	void *buf;
	int err;

	buf = dma_alloc_coherent(priv->dev, ETH_ZLEN, &dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memset(buf, 0, ETH_ZLEN);
	eth_broadcast_addr(buf);
	memcpy(buf + ETH_ALEN, dev->dev_addr, ETH_ALEN);

	sts = RTL9602C_NIC_DESC_OWN | RTL9602C_NIC_DESC_FIRST_SEG | RTL9602C_NIC_DESC_LAST_SEG |
	      RTL9602C_NIC_TXD_ADD_FCS | RTL9602C_NIC_TXD_NO_REWRITE | ETH_ZLEN;
	if (i == RTL9602C_TX_RING - 1)
		sts |= RTL9602C_NIC_DESC_RING_END;

	priv->tx[i].addr = cpu_to_be32(dma);
	priv->tx[i].tag = cpu_to_be32(RTL9602C_NIC_TXD_CPUTAG_EN | RTL9602C_NIC_TXD_PRI_EN);
	priv->tx[i].dest = cpu_to_be32(RTL9602C_NIC_TXD_DST_SFP | RTL9602C_NIC_TXD_L34_BYPASS);
	priv->tx[i].lso = 0;
	priv->tx[i].stslen = cpu_to_be32(sts);

	rtl9602c_eth_kick(priv);

	/* The engine clears OWN; READ_ONCE reloads word 0 on every wait. */
	err = -ETIMEDOUT;
	for (spin = 0; spin < 100; spin++) {
		if (!(be32_to_cpu(READ_ONCE(priv->tx[i].stslen)) &
		      RTL9602C_NIC_DESC_OWN)) {
			err = 0;
			break;
		}
		usleep_range(100, 200);
	}

	/* Take the descriptor back either way: its buffer is freed next. */
	priv->tx[i].stslen = 0;
	dma_free_coherent(priv->dev, ETH_ZLEN, buf, dma);

	/* The engine is past the descriptor; the driver's indices follow. */
	if (!err) {
		priv->tx_head = (i + 1) % RTL9602C_TX_RING;
		priv->tx_tail = priv->tx_head;
	}
	return err;
}

/*
 * Offer an OMCI message to the switch driver's handler, with the port it came
 * from. It is offered only with a CPU tag, without which word 3, and so the
 * port, is not written. Returns true if the handler took the frame; the
 * caller drops a message that no handler takes, since it has no Ethernet
 * header to go up with.
 */
static bool rtl9602c_eth_rx_ctrl(struct net_device *dev, struct sk_buff *skb,
				 unsigned int r, u32 sts, u32 tag, u32 dest)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	unsigned int port = FIELD_GET(RTL9602C_NIC_RXD_SRC_PORT, dest);

	/*
	 * Acquire, paired with the release in rtl9602c_conduit_ctrl_attach():
	 * the context, read below with READ_ONCE against the WRITE_ONCE there,
	 * is then the one stored before this handler was published.
	 */
	rtl9602c_ctrl_rx_t rx = smp_load_acquire(&priv->ctrl_rx);

	if (priv->ctrl_logged < RTL9602C_RX_LOG) {
		priv->ctrl_logged++;
		eth_info(dev,
			 "pon ctrl[%u] on the %s ring, %u bytes w0 %08x w2 %08x w3 %08x src %u: %*ph\n",
			 priv->ctrl_logged, rtl9602c_rx_ring_name[r], skb->len,
			 sts, tag, dest, port, min_t(int, skb->len, 16),
			 skb->data);
	}

	if (!rx || !(tag & RTL9602C_NIC_RXD_CPUTAG_VALID))
		return false;

	if (!rx(READ_ONCE(priv->ctrl_ctx), skb, port))
		return false;

	dev->stats.rx_packets++;
	return true;
}

/*
 * Give descriptor @i of ring @r back to the engine by writing its index to the
 * ring's tail; without this write the ring runs dry and receive stops. The
 * data ring's tail is a byte of its own, the control ring's a word, written
 * whole.
 */
static void rtl9602c_eth_rx_tail(struct rtl9602c_eth *priv, unsigned int r,
				 unsigned int i)
{
	if (r == RTL9602C_RX_DATA)
		writeb(i, priv->base + RTL9602C_NIC_RX_RING_TAIL);
	else
		writel(i, priv->base + RTL9602C_NIC_RX2_RING_TAIL);
}

/*
 * Hand on frame @skb of ring @r, the engine's offset and the FCS taken off,
 * with word 0 (@sts) and the CPU-tag words (@w2, @w3) of its first
 * descriptor.
 */
static void rtl9602c_eth_rx_frame(struct net_device *dev, unsigned int r,
				  struct sk_buff *skb, u32 sts, u32 w2, u32 w3)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	unsigned int why = FIELD_GET(RTL9602C_NIC_RXD_TRAP_REASON, w3);

	/* An OMCI message goes to its handler, or nowhere. */
	if (sts & RTL9602C_NIC_RXD_OMCI) {
		if (!rtl9602c_eth_rx_ctrl(dev, skb, r, sts, w2,
					  w3)) {
			dev->stats.rx_dropped++;
			dev_kfree_skb_any(skb);
		}
		return;
	}

	/*
	 * With DSA, a frame with a CPU tag is counted under its
	 * trap reason and offered to the flow handler. If the
	 * handler leaves it, the frame gets its source port's
	 * dst, on which DSA delivers it to the user port
	 * without calling the tagger. A frame without a CPU tag
	 * gets no dst, and the tagger drops it, its source port
	 * being unknown.
	 */
	if (netdev_uses_dsa(dev) && (w2 & RTL9602C_NIC_RXD_CPUTAG_VALID)) {
		unsigned int port =
			FIELD_GET(RTL9602C_NIC_RXD_SRC_PORT, w3);
		rtl9602c_flow_rx_t flow_rx;
		struct metadata_dst *md = NULL;

		priv->rx_reason[why]++;

		/*
		 * Acquire, paired with the release in
		 * rtl9602c_conduit_flow_attach(): the context,
		 * read below with READ_ONCE against the
		 * WRITE_ONCE there, is then the one stored
		 * before this handler was published.
		 */
		flow_rx = smp_load_acquire(&priv->flow_rx);

		if (flow_rx &&
		    flow_rx(READ_ONCE(priv->flow_ctx), skb, port,
			    FIELD_GET(RTL9602C_NIC_RXD_FLOW, w2))) {
			dev->stats.rx_packets++;
			return;
		}

		/*
		 * A port with no dst leaves the frame to the
		 * tagger, which drops it.
		 */
		if (port < RTL9602C_SW_PORTS)
			md = priv->port_meta[port];
		if (md)
			skb_dst_set_noref(skb, &md->dst);
	}

	/*
	 * Take off the timestamp tag of a PTP message (see
	 * RTL9602C_ETH_P_REALTEK). Its time, the low 16 bits of
	 * the PTP clock's seconds and the nanoseconds, becomes
	 * the frame's hardware timestamp; the switch driver
	 * fills in the upper seconds from its clock. The reason
	 * byte, 240 for a message sent to the CPU and 241 for a
	 * copy made by the ingress mirror action (measured on
	 * this board), is logged once per value and otherwise
	 * not interpreted. The addresses then move up over the
	 * tag, so the frame reaches the stack as it was sent: a
	 * PTP message over Ethernet is recognised by its own
	 * EtherType, 0x88F7 (IEEE 1588-2008 Annex F), which
	 * neither DSA nor a PTP stack would find behind the
	 * tag.
	 *
	 * A frame longer than the tag with 0x8899 at offset 12
	 * and the protocol byte 4 at offset 14, as in every tag
	 * seen on this board, is taken to carry one, but only
	 * if the switch sent it up for one of the two PTP trap
	 * reasons: a host can send such a frame itself, and the
	 * switch then floods it to the CPU with the reason of
	 * an ordinary frame (measured). Other frames use that
	 * EtherType with other protocol bytes, among them the
	 * switch's own loop-detection probes, and go up as they
	 * came.
	 */
	if ((w2 & RTL9602C_NIC_RXD_CPUTAG_VALID) &&
	    (why == RTL9602C_RX_REASON_PTP ||
	     why == RTL9602C_RX_REASON_PTP_MIRROR) &&
	    skb->len > RTL9602C_PTP_TAG_LEN &&
	    get_unaligned_be16(skb->data + 2 * ETH_ALEN) ==
	    RTL9602C_ETH_P_REALTEK &&
	    skb->data[RTL9602C_PTP_TAG_PROTO_OFFSET] ==
	    RTL9602C_PTP_TAG_PROTO_PTP) {
		u32 sec = get_unaligned_be16(skb->data +
					     RTL9602C_PTP_TAG_SEC_OFFSET);
		u32 nsec = get_unaligned_be32(skb->data +
					      RTL9602C_PTP_TAG_NSEC_OFFSET);
		u8 reason = skb->data[RTL9602C_PTP_TAG_REASON_OFFSET];

		if (!test_and_set_bit(reason,
				      priv->ptp_reason_seen))
			eth_info(dev,
				 "ptp tag reason %u, protocol %u: %16phN\n",
				 reason,
				 skb->data[RTL9602C_PTP_TAG_PROTO_OFFSET],
				 skb->data + 2 * ETH_ALEN);

		skb_hwtstamps(skb)->hwtstamp =
			ktime_set(sec, nsec);
		memmove(skb->data + RTL9602C_PTP_TAG_LEN,
			skb->data, 2 * ETH_ALEN);
		skb_pull(skb, RTL9602C_PTP_TAG_LEN);
	}

	skb->protocol = eth_type_trans(skb, dev);

	/*
	 * Log the first RTL9602C_RX_LOG frames, and the first
	 * frame from each source port. A frame without a CPU
	 * tag has no source port: word 3 stays as the driver
	 * cleared it, and the port printed is 0.
	 */
	{
		unsigned int src = FIELD_GET(RTL9602C_NIC_RXD_SRC_PORT, w3);
		bool newport = (w2 & RTL9602C_NIC_RXD_CPUTAG_VALID) &&
			       !(priv->rx_ports_seen & BIT(src));

		if (newport)
			priv->rx_ports_seen |= BIT(src);
		if (priv->rx_logged < RTL9602C_RX_LOG || newport) {
			priv->rx_logged++;
			eth_info(dev,
				 "<eth rx> port %lu receives on the %s ring: %u bytes from %pM, proto 0x%04x, %s; descriptor %08x %08x %08x\n",
				 FIELD_GET(RTL9602C_NIC_RXD_SRC_PORT, w3),
				 rtl9602c_rx_ring_name[r], skb->len,
				 eth_hdr(skb)->h_source,
				 ntohs(skb->protocol),
				 (w2 & RTL9602C_NIC_RXD_CPUTAG_VALID) ?
				 "cpu-tagged" :
				 "NO CPU TAG, so the source port is a guess",
				 sts, w2, w3);
		}
	}

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += skb->len;
	netif_receive_skb(skb);
}

/*
 * A frame longer than its buffer comes in several descriptors, the first with
 * RTL9602C_NIC_DESC_FIRST_SEG and the last with RTL9602C_NIC_DESC_LAST_SEG,
 * each holding the bytes in its own buffer: the first from the engine's
 * 2-byte offset, the others from the start of their buffers, the last with
 * the FCS; only the first carries the CPU tag (measured). The pieces are
 * copied into one buffer, sized for the longest frame that the MTU lets
 * through, and every descriptor keeps its own buffer. A frame with an error
 * in any piece, one longer than that buffer, and a piece whose first piece
 * never came are dropped and counted.
 */
static void rtl9602c_eth_rx_piece(struct net_device *dev, unsigned int r,
				  unsigned int i, u32 sts, u32 w2, u32 w3,
				  unsigned int len)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	struct rtl9602c_rx_ring *ring = &priv->rx[r];
	unsigned int off = 0;
	struct sk_buff *head;

	if (sts & RTL9602C_NIC_DESC_FIRST_SEG) {
		/* A frame whose last piece never came is lost. */
		if (ring->gathering) {
			dev->stats.rx_length_errors++;
			dev_kfree_skb_any(ring->head);
		}
		ring->gathering = true;
		ring->head_bad = false;
		ring->head_sts = sts;
		ring->head_w2 = w2;
		ring->head_w3 = w3;
		ring->head = netdev_alloc_skb_ip_align(dev, dev->mtu +
						       RTL9602C_RX_OVERHEAD);
		if (!ring->head)
			dev->stats.rx_dropped++;
		off = RTL9602C_NIC_RX_DATA_OFFSET;
	} else if (!ring->gathering) {
		dev->stats.rx_length_errors++;
		return;
	}

	head = ring->head;
	if ((sts & (RTL9602C_NIC_RXD_FCS_ERR | RTL9602C_NIC_RXD_DMA_ERR)) &&
	    !ring->head_bad) {
		ring->head_bad = true;
		dev->stats.rx_errors++;
		if (sts & RTL9602C_NIC_RXD_FCS_ERR)
			dev->stats.rx_crc_errors++;
		if (sts & RTL9602C_NIC_RXD_DMA_ERR)
			dev->stats.rx_fifo_errors++;
	}

	if (head && !ring->head_bad) {
		if (len > skb_tailroom(head)) {
			ring->head_bad = true;
			dev->stats.rx_length_errors++;
		} else {
			dma_sync_single_for_cpu(priv->dev, ring->buf[i],
						off + len, DMA_FROM_DEVICE);
			skb_put_data(head, ring->skb[i]->data + off, len);
			dma_sync_single_for_device(priv->dev, ring->buf[i],
						   off + len, DMA_FROM_DEVICE);
		}
	}

	if (!(sts & RTL9602C_NIC_DESC_LAST_SEG))
		return;

	ring->gathering = false;
	ring->head = NULL;
	if (!head)
		return;
	if (ring->head_bad ||
	    head->len < ETH_HLEN + RTL9602C_NIC_RX_FCS_LEN) {
		if (!ring->head_bad)
			dev->stats.rx_length_errors++;
		dev_kfree_skb_any(head);
		return;
	}
	skb_trim(head, head->len - RTL9602C_NIC_RX_FCS_LEN);
	rtl9602c_eth_rx_frame(dev, r, head, ring->head_sts, ring->head_w2,
			      ring->head_w3);
}

/*
 * Receive up to @budget frames from ring @r, stopping at the first descriptor
 * the engine still owns. A frame in one buffer is handed on only once its
 * descriptor has a new buffer, a frame in several is copied out of them
 * (rtl9602c_eth_rx_piece()), and every descriptor processed is given back.
 */
static int rtl9602c_eth_rx(struct net_device *dev, unsigned int r, int budget)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	struct rtl9602c_rx_ring *ring = &priv->rx[r];
	int done = 0;

	while (done < budget) {
		struct rtl9602c_rx_desc *desc = &ring->desc[ring->tail];
		unsigned int i = ring->tail;
		struct sk_buff *skb = NULL;
		unsigned int len;
		u32 sts, w2, w3;

		/*
		 * The engine clears OWN once the buffer is filled; READ_ONCE
		 * loads word 0 afresh on every pass.
		 */
		sts = be32_to_cpu(READ_ONCE(desc->stslen));
		if (sts & RTL9602C_NIC_DESC_OWN)
			break;

		/*
		 * Words 2 and 3 are read only after word 0 showed the engine
		 * done with the descriptor, so they are this frame's. They are
		 * read once, here: the descriptor goes back to the engine
		 * before the frame is handed on.
		 */
		dma_rmb();
		w2 = be32_to_cpu(desc->tag);
		w3 = be32_to_cpu(desc->dest);
		len = sts & RTL9602C_NIC_RXD_LEN;
		ring->frames++;

		/* A frame longer than a buffer comes in pieces. */
		if ((sts & RTL9602C_NIC_DESC_SEGS) != RTL9602C_NIC_DESC_SEGS) {
			rtl9602c_eth_rx_piece(dev, r, i, sts, w2, w3, len);
			goto rearm;
		}

		/*
		 * A frame marked in error, too short, or longer than its buffer
		 * is counted, and the buffer goes straight back to the engine.
		 */
		if (sts & (RTL9602C_NIC_RXD_FCS_ERR | RTL9602C_NIC_RXD_DMA_ERR)) {
			dev->stats.rx_errors++;
			if (sts & RTL9602C_NIC_RXD_FCS_ERR)
				dev->stats.rx_crc_errors++;
			if (sts & RTL9602C_NIC_RXD_DMA_ERR)
				dev->stats.rx_fifo_errors++;
		} else if (len < RTL9602C_NIC_RX_DATA_OFFSET + ETH_HLEN + RTL9602C_NIC_RX_FCS_LEN ||
			   len > RTL9602C_RX_BUF) {
			dev->stats.rx_length_errors++;
		} else {
			skb = rtl9602c_eth_rx_swap(dev, ring, i);
			if (!skb) {
				dev->stats.rx_dropped++;
				goto rearm;
			}

			/* Skip the 2-byte offset; drop the FCS by length. */
			skb_reserve(skb, RTL9602C_NIC_RX_DATA_OFFSET);
			skb_put(skb, len - RTL9602C_NIC_RX_FCS_LEN);

			rtl9602c_eth_rx_frame(dev, r, skb, sts, w2, w3);
		}

rearm:
		/*
		 * Nothing was taken from the descriptor (an error, a bad
		 * length, or no new buffer): it keeps its own buffer.
		 */
		if (!skb)
			rtl9602c_eth_rx_give(ring, i, ring->skb[i], ring->buf[i]);

		rtl9602c_eth_rx_tail(priv, r, i);
		ring->tail = (i + 1) % ring->len;
		done++;
	}

	return done;
}

/*
 * RTL9602C_NIC_RX_NO_DESC_CNT is a free-running 16-bit count of the frames the
 * engine dropped for want of a receive descriptor. Under a flood it wraps in
 * under a second, so its 16-bit difference is added to rx_missed_errors on
 * every poll.
 */
static void rtl9602c_eth_count_missed(struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	u16 now = readw(priv->base + RTL9602C_NIC_RX_NO_DESC_CNT);

	dev->stats.rx_missed_errors += (u16)(now - priv->rx_missed_last);
	priv->rx_missed_last = now;
}

/*
 * Fold in missed frames, reap transmit completions, then receive: the control
 * ring first, so that a full budget of data cannot hold OMCI back. The
 * interrupts that rtl9602c_eth_irq() masked are unmasked once a poll finishes
 * under budget.
 */
static int rtl9602c_eth_poll(struct napi_struct *napi, int budget)
{
	struct net_device *dev = napi->dev;
	int done;

	rtl9602c_eth_count_missed(dev);
	rtl9602c_eth_reap(dev);
	done = rtl9602c_eth_rx(dev, RTL9602C_RX_CTRL, budget);
	done += rtl9602c_eth_rx(dev, RTL9602C_RX_DATA, budget - done);

	if (done < budget && napi_complete_done(napi, done)) {
		struct rtl9602c_eth *priv = netdev_priv(dev);

		writew(RTL9602C_NIC_IRQ_MASK_RXTX, priv->base + RTL9602C_NIC_IRQ_MASK);
	}
	return done;
}

/*
 * Acknowledge every pending status bit, mask them all, and leave the work to
 * the poll.
 */
static irqreturn_t rtl9602c_eth_irq(int irq, void *data)
{
	struct rtl9602c_eth *priv = data;
	u16 status = readw(priv->base + RTL9602C_NIC_IRQ_STAT);

	if (!status)
		return IRQ_NONE;

	writew(status, priv->base + RTL9602C_NIC_IRQ_STAT);

	if (napi_schedule_prep(&priv->napi)) {
		writew(0, priv->base + RTL9602C_NIC_IRQ_MASK);
		__napi_schedule(&priv->napi);
	}
	return IRQ_HANDLED;
}

/*
 * The transmit test at probe, on rings set up for it alone and freed afterwards;
 * its result is only logged.
 *
 * Before the probe stopped the engine first (rtl9602c_eth_quiesce()), the test
 * passed or failed with what ran before the kernel. Measured on this board with
 * the probe at 0.288 s: after a cold start, with no network transfer before it,
 * the test passed within 4 ms; after the boot loader had received the kernel
 * over the network, whether the kernel was then started from RAM or written to
 * the flash and started from there, the engine kept the descriptor through the
 * whole wait of 400 ms. A kernel with an initial RAM disk, whose probe ran at
 * 0.416 s after a transfer, passed. The switch driver's setup of the CPU port,
 * which comes later, is therefore not the cause. What the engine was doing is
 * not known; the probe logs the registers it finds. With the engine stopped
 * first the test is meant to pass in every start, which the board has not yet
 * confirmed.
 */
static void rtl9602c_eth_selftest(struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	int err;

	err = rtl9602c_eth_alloc_rings(dev);
	if (err) {
		eth_warn(dev, "self-test skipped: no memory for the rings\n");
		return;
	}

	rtl9602c_eth_start(priv);
	err = rtl9602c_eth_test_frame(dev);
	rtl9602c_eth_halt(priv->base);
	rtl9602c_eth_free_rings(priv);

	if (err)
		eth_err(dev, "SELF-TEST FAILED: the engine never released the descriptor, so the ring is not being consumed\n");
	else
		eth_info(dev, "self-test passed: the engine took a frame and returned the descriptor\n");
}

/*
 * Set the engine up and start it, run the transmit test, then take interrupts
 * and start the queues. The test's result is only logged.
 */
static int rtl9602c_eth_open(struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	int err, test;

	err = rtl9602c_eth_alloc_rings(dev);
	if (err)
		return err;

	rtl9602c_eth_start(priv);
	/* Missed frames are counted from here on. */
	priv->rx_missed_last = readw(priv->base + RTL9602C_NIC_RX_NO_DESC_CNT);

	test = rtl9602c_eth_test_frame(dev);

	napi_enable(&priv->napi);

	/* Clear old status first, so nothing stale reaches the handler. */
	writew(0xffff, priv->base + RTL9602C_NIC_IRQ_STAT);
	err = request_irq(priv->irq, rtl9602c_eth_irq, 0, dev->name, priv);
	if (err) {
		napi_disable(&priv->napi);
		rtl9602c_eth_halt(priv->base);
		rtl9602c_eth_free_rings(priv);
		return err;
	}
	writew(RTL9602C_NIC_IRQ_MASK_RXTX, priv->base + RTL9602C_NIC_IRQ_MASK);

	netif_tx_start_all_queues(dev);
	eth_info(dev, "up: interrupt %d, msr %02x, config %08x; transmit test %s\n",
		 priv->irq, readb(priv->base + RTL9602C_NIC_MEDIA_STAT),
		 readl(priv->base + RTL9602C_NIC_CFG),
		 test ? "FAILED -- the engine kept the descriptor" : "ok");
	return 0;
}

/*
 * Log the totals, then stop the queues, the interrupt and the poll before
 * halting the engine. The reap after the halt frees what the engine sent, and
 * rtl9602c_eth_free_rings() the rest.
 */
static int rtl9602c_eth_stop(struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);

	eth_info(dev, "down: rx %lu frames / %lu bytes, tx %lu / %lu; dropped rx %lu tx %lu, tx errors %lu\n",
		 dev->stats.rx_packets, dev->stats.rx_bytes,
		 dev->stats.tx_packets, dev->stats.tx_bytes,
		 dev->stats.rx_dropped, dev->stats.tx_dropped,
		 dev->stats.tx_errors);

	netif_tx_disable(dev);
	writew(0, priv->base + RTL9602C_NIC_IRQ_MASK);
	free_irq(priv->irq, priv);
	napi_disable(&priv->napi);
	rtl9602c_eth_halt(priv->base);
	rtl9602c_eth_reap(dev);
	rtl9602c_eth_free_rings(priv);
	return 0;
}

/*
 * Post a data frame on the descriptor at tx_head. A short frame is padded to
 * 60 bytes, which with the FCS the engine appends makes the 64-byte minimum
 * (IEEE 802.3 clause 4.4.2). Word 3 aims the frame at the port of its queue.
 */
static netdev_tx_t rtl9602c_eth_start_xmit(struct sk_buff *skb,
					   struct net_device *dev)
{
	struct rtl9602c_eth *priv = netdev_priv(dev);
	struct metadata_dst *md;
	dma_addr_t dma;
	unsigned int i;
	u32 flow = 0;
	u32 sts;

	if (skb_put_padto(skb, ETH_ZLEN)) {
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	/* A frame the engine cannot send would stop it for every frame after. */
	if (unlikely(skb->len > RTL9602C_NIC_TX_MAX)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	dma = dma_map_single(priv->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(priv->dev, dma)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	sts = RTL9602C_NIC_DESC_OWN | RTL9602C_NIC_DESC_FIRST_SEG | RTL9602C_NIC_DESC_LAST_SEG |
	      RTL9602C_NIC_TXD_ADD_FCS | RTL9602C_NIC_TXD_NO_REWRITE |
	      (skb->len & RTL9602C_NIC_TXD_LEN);

	/*
	 * A frame from a GEM interface names its PON flow as the port_id of a
	 * METADATA_HW_PORT_MUX dst; the flow goes into word 3.
	 */
	md = skb_metadata_dst(skb);
	if (md && md->type == METADATA_HW_PORT_MUX) {
		flow = FIELD_PREP(RTL9602C_NIC_TXD_FLOW, md->u.port_info.port_id);
		sts |= RTL9602C_NIC_TXD_FLOW_SEL;
	}

	spin_lock(&priv->tx_lock);

	/* All queues share the one ring, so they stop together. */
	if (unlikely(!rtl9602c_eth_tx_room(priv))) {
		netif_tx_stop_all_queues(dev);
		spin_unlock(&priv->tx_lock);
		dma_unmap_single(priv->dev, dma, skb->len, DMA_TO_DEVICE);
		return NETDEV_TX_BUSY;
	}

	i = priv->tx_head;
	if (i == RTL9602C_TX_RING - 1)
		sts |= RTL9602C_NIC_DESC_RING_END;

	priv->tx_skb[i] = skb;
	priv->tx_buf[i] = dma;
	priv->tx[i].addr = cpu_to_be32(dma);
	/*
	 * The frame's internal priority, and so its egress queue, comes from
	 * skb->priority: SO_PRIORITY and the tc priority map.
	 */
	priv->tx[i].tag = cpu_to_be32(RTL9602C_NIC_TXD_CPUTAG_EN | RTL9602C_NIC_TXD_PRI_EN |
				      FIELD_PREP(RTL9602C_NIC_TXD_PRI,
						 min_t(u32, skb->priority, 7)));
	priv->tx[i].dest = cpu_to_be32(RTL9602C_NIC_TXD_DST_PORT(skb_get_queue_mapping(skb)) |
				       flow | RTL9602C_NIC_TXD_L34_BYPASS);
	priv->tx[i].lso = 0;
	/*
	 * Word 0 last, carrying OWN. The words are stores to uncached memory in
	 * program order, and the kick's barrier follows them; this relies on
	 * the engine not taking a descriptor before all five words are in
	 * memory.
	 */
	priv->tx[i].stslen = cpu_to_be32(sts);
	priv->tx_head = (i + 1) % RTL9602C_TX_RING;

	if (!rtl9602c_eth_tx_room(priv))
		netif_tx_stop_all_queues(dev);

	rtl9602c_eth_kick(priv);
	spin_unlock(&priv->tx_lock);
	return NETDEV_TX_OK;
}

/*
 * The conduit has a transmit queue per switch port, and the tagger records a
 * frame's destination port as its queue mapping, which this keeps. With a
 * single queue the core would reset the mapping to 0, and skb->cb, the other
 * obvious carrier, is overwritten by struct qdisc_skb_cb in dev_queue_xmit().
 * Without DSA, or with a mapping out of range, a frame goes to the SerDes port.
 */
static u16 rtl9602c_eth_select_queue(struct net_device *dev,
				     struct sk_buff *skb,
				     struct net_device *sb_dev)
{
	u16 port = skb_get_queue_mapping(skb);

	if (!netdev_uses_dsa(dev) || port >= dev->real_num_tx_queues)
		return RTL9602C_PORT_SFP;

	return port;
}

static const struct net_device_ops rtl9602c_eth_netdev_ops = {
	.ndo_open		= rtl9602c_eth_open,
	.ndo_stop		= rtl9602c_eth_stop,
	.ndo_start_xmit		= rtl9602c_eth_start_xmit,
	.ndo_select_queue	= rtl9602c_eth_select_queue,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

/**
 * rtl9602c_conduit_ctrl_attach() - attach the OMCI receive handler
 * @dev: the conduit
 * @rx: the handler, called from the receive poll with @ctx, the frame and the
 *	switch port it came from; it returns true when it has taken the frame
 * @ctx: passed to @rx
 *
 * A received OMCI message from the PON MAC that carries a CPU tag is offered to
 * @rx first. It has no Ethernet header: its data starts at the message. A frame
 * the handler takes is its own; one it leaves goes on as an ordinary frame.
 * There is one handler at a time.
 *
 * Return: 0, -EINVAL if @dev is not this driver's conduit or @rx is NULL, or
 * -EBUSY if a handler is attached already.
 */
int rtl9602c_conduit_ctrl_attach(struct net_device *dev,
				 rtl9602c_ctrl_rx_t rx, void *ctx)
{
	struct rtl9602c_eth *priv;

	if (!dev || dev->netdev_ops != &rtl9602c_eth_netdev_ops || !rx)
		return -EINVAL;

	priv = netdev_priv(dev);
	/* READ_ONCE: detach clears the handler without a lock. */
	if (READ_ONCE(priv->ctrl_rx))
		return -EBUSY;

	/* The receive path reads this with READ_ONCE after the acquire. */
	WRITE_ONCE(priv->ctrl_ctx, ctx);

	/*
	 * Release: the context is visible before the handler is, so the receive
	 * path, which acquires the handler, never calls it with a stale
	 * context.
	 */
	smp_store_release(&priv->ctrl_rx, rx);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9602c_conduit_ctrl_attach);

/**
 * rtl9602c_conduit_ctrl_detach() - detach the OMCI receive handler
 * @dev: the conduit; nothing is done if it is not this driver's
 *
 * On return no receive poll is still running the handler, and its context may
 * be freed.
 *
 * Context: process context; sleeps.
 */
void rtl9602c_conduit_ctrl_detach(struct net_device *dev)
{
	struct rtl9602c_eth *priv;

	if (!dev || dev->netdev_ops != &rtl9602c_eth_netdev_ops)
		return;

	priv = netdev_priv(dev);
	/*
	 * WRITE_ONCE, against the acquire on the receive path;
	 * synchronize_net() then waits out every poll that may still hold the
	 * old handler.
	 */
	WRITE_ONCE(priv->ctrl_rx, NULL);
	synchronize_net();
}
EXPORT_SYMBOL_GPL(rtl9602c_conduit_ctrl_detach);

/**
 * rtl9602c_conduit_flow_attach() - attach the receive handler for data frames
 * @dev: the conduit
 * @rx: the handler, called from the receive poll with @ctx, the frame, the
 *	switch port it came from and the PON flow it arrived on; it returns true
 *	when it has taken the frame
 * @ctx: passed to @rx
 *
 * While DSA uses the conduit, every received frame with a CPU tag that the OMCI
 * handler has not taken is offered to @rx as it arrived: from its Ethernet
 * header, before any PTP timestamp tag is taken off. The flow means something
 * only for frames from the PON port. A frame the handler takes is its own; one
 * it leaves goes on to its user port. There is one handler at a time.
 *
 * Return: 0, -EINVAL if @dev is not this driver's conduit or @rx is NULL, or
 * -EBUSY if a handler is attached already.
 */
int rtl9602c_conduit_flow_attach(struct net_device *dev,
				 rtl9602c_flow_rx_t rx, void *ctx)
{
	struct rtl9602c_eth *priv;

	if (!dev || dev->netdev_ops != &rtl9602c_eth_netdev_ops || !rx)
		return -EINVAL;

	priv = netdev_priv(dev);
	/* READ_ONCE: detach clears the handler without a lock. */
	if (READ_ONCE(priv->flow_rx))
		return -EBUSY;

	/* The receive path reads this with READ_ONCE after the acquire. */
	WRITE_ONCE(priv->flow_ctx, ctx);

	/*
	 * Release: the context is visible before the handler is, so the receive
	 * path, which acquires the handler, never calls it with a stale
	 * context.
	 */
	smp_store_release(&priv->flow_rx, rx);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9602c_conduit_flow_attach);

/**
 * rtl9602c_conduit_flow_detach() - detach the receive handler for data frames
 * @dev: the conduit; nothing is done if it is not this driver's
 *
 * On return no receive poll is still running the handler, and its context may
 * be freed.
 *
 * Context: process context; sleeps.
 */
void rtl9602c_conduit_flow_detach(struct net_device *dev)
{
	struct rtl9602c_eth *priv;

	if (!dev || dev->netdev_ops != &rtl9602c_eth_netdev_ops)
		return;

	priv = netdev_priv(dev);
	/*
	 * WRITE_ONCE, against the acquire on the receive path;
	 * synchronize_net() then waits out every poll that may still hold the
	 * old handler.
	 */
	WRITE_ONCE(priv->flow_rx, NULL);
	synchronize_net();
}
EXPORT_SYMBOL_GPL(rtl9602c_conduit_flow_detach);

/**
 * rtl9602c_conduit_ctrl_xmit() - send an OMCI message
 * @dev: the conduit
 * @skb: the message, linear
 * @port: the switch port it leaves by, the PON port
 * @stream: the PON flow to send it in, the OMCC's, 0 to 127
 *
 * The message shares the transmit ring with data frames. It is not padded to
 * the Ethernet minimum: the message leaves at the length given. It carries no
 * priority.
 *
 * Context: process or softirq context.
 * Return: 0 once the message is queued; the driver then owns @skb and frees it
 * once sent, or when the conduit stops. Otherwise @skb stays the caller's and
 * the error is -EINVAL for a device that is not this driver's conduit, a port
 * or flow out of range, or an empty, oversized or nonlinear @skb; -ENETDOWN
 * while the conduit is down; -ENOMEM if @skb cannot be mapped; or -EAGAIN while
 * the transmit ring is full.
 */
int rtl9602c_conduit_ctrl_xmit(struct net_device *dev, struct sk_buff *skb,
			       unsigned int port, unsigned int stream)
{
	struct rtl9602c_eth *priv;
	dma_addr_t dma;
	unsigned int i;
	u32 sts;

	if (!dev || dev->netdev_ops != &rtl9602c_eth_netdev_ops)
		return -EINVAL;
	if (port >= RTL9602C_SW_PORTS ||
	    stream > FIELD_MAX(RTL9602C_NIC_TXD_FLOW) ||
	    !skb->len || skb->len > RTL9602C_NIC_TX_MAX || skb_is_nonlinear(skb))
		return -EINVAL;
	if (!netif_running(dev))
		return -ENETDOWN;

	priv = netdev_priv(dev);

	dma = dma_map_single(priv->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(priv->dev, dma))
		return -ENOMEM;

	sts = RTL9602C_NIC_DESC_OWN | RTL9602C_NIC_DESC_FIRST_SEG | RTL9602C_NIC_DESC_LAST_SEG |
	      RTL9602C_NIC_TXD_IP_CSUM | RTL9602C_NIC_TXD_NO_REWRITE | RTL9602C_NIC_TXD_ADD_FCS |
	      RTL9602C_NIC_TXD_NO_LEARN | RTL9602C_NIC_TXD_FLOW_SEL |
	      (skb->len & RTL9602C_NIC_TXD_LEN);

	spin_lock_bh(&priv->tx_lock);

	if (!rtl9602c_eth_tx_room(priv)) {
		spin_unlock_bh(&priv->tx_lock);
		dma_unmap_single(priv->dev, dma, skb->len, DMA_TO_DEVICE);
		return -EAGAIN;
	}

	i = priv->tx_head;
	if (i == RTL9602C_TX_RING - 1)
		sts |= RTL9602C_NIC_DESC_RING_END;

	priv->tx_skb[i] = skb;
	priv->tx_buf[i] = dma;
	priv->tx[i].addr = cpu_to_be32(dma);
	priv->tx[i].tag = cpu_to_be32(RTL9602C_NIC_TXD_CPUTAG_EN | RTL9602C_NIC_TXD_FID_EXT_EN);
	priv->tx[i].dest = cpu_to_be32(RTL9602C_NIC_TXD_DST_PORT(port) |
				       FIELD_PREP(RTL9602C_NIC_TXD_FLOW, stream) |
				       RTL9602C_NIC_TXD_L34_BYPASS);
	priv->tx[i].lso = 0;
	/* Word 0 last, as in rtl9602c_eth_start_xmit(). */
	priv->tx[i].stslen = cpu_to_be32(sts);
	priv->tx_head = (i + 1) % RTL9602C_TX_RING;

	if (!rtl9602c_eth_tx_room(priv))
		netif_tx_stop_all_queues(dev);

	rtl9602c_eth_kick(priv);

	if (priv->ctrl_tx_logged < RTL9602C_RX_LOG) {
		priv->ctrl_tx_logged++;
		eth_info(dev,
			 "pon ctrl tx[%u] %u bytes w0 %08x w2 %08x w3 %08x slot %u: %*ph\n",
			 priv->ctrl_tx_logged, skb->len, sts,
			 be32_to_cpu(priv->tx[i].tag),
			 be32_to_cpu(priv->tx[i].dest), i,
			 min_t(int, skb->len, 16), skb->data);
	}

	spin_unlock_bh(&priv->tx_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9602c_conduit_ctrl_xmit);

/*
 * The rxreason file: one line per trap reason with a non-zero count, as many as
 * fit in a page.
 */
static ssize_t rtl9602c_eth_rxreason_read(struct file *file, char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct rtl9602c_eth *priv = file->private_data;
	unsigned int i, n = 0;
	char *buf;
	ssize_t ret;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < RTL9602C_RX_REASONS; i++) {
		/*
		 * The poll counts without a lock; READ_ONCE loads each counter
		 * once. A count a frame behind does no harm in this view.
		 */
		u32 v = READ_ONCE(priv->rx_reason[i]);

		if (!v || n > PAGE_SIZE - 32)
			continue;
		n += scnprintf(buf + n, PAGE_SIZE - n, "reason %3u  %u\n", i, v);
	}
	if (!n)
		n = scnprintf(buf, PAGE_SIZE,
			      "no tagged frame has been received\n");

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, n);
	kfree(buf);
	return ret;
}

/* Any write clears the counts. */
static ssize_t rtl9602c_eth_rxreason_write(struct file *file,
					   const char __user *ubuf,
					   size_t len, loff_t *ppos)
{
	struct rtl9602c_eth *priv = file->private_data;

	memset(priv->rx_reason, 0, sizeof(priv->rx_reason));
	return len;
}

static const struct file_operations rtl9602c_eth_rxreason_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= rtl9602c_eth_rxreason_read,
	.write	= rtl9602c_eth_rxreason_write,
	.llseek	= default_llseek,
};

/*
 * The rings file: the priority-to-ring map, then per receive ring its length,
 * the engine's index into it, the driver's, and the frames the driver has taken
 * from it since the interface came up.
 */
static int rtl9602c_eth_rings_show(struct seq_file *m, void *v)
{
	struct rtl9602c_eth *priv = m->private;
	static const unsigned int cur[RTL9602C_RX_RINGS] = {
		[RTL9602C_RX_DATA] = RTL9602C_NIC_RX_RING_CUR,
		[RTL9602C_RX_CTRL] = RTL9602C_NIC_RX2_RING_CUR,
	};
	unsigned int r;

	seq_printf(m, "priority to ring %08x\n",
		   readl(priv->base + RTL9602C_NIC_RX_PRI_TO_RING));
	for (r = 0; r < RTL9602C_RX_RINGS; r++) {
		const struct rtl9602c_rx_ring *ring = &priv->rx[r];

		/*
		 * The poll updates the index and the count without a lock;
		 * READ_ONCE loads each once. A value a frame behind does no
		 * harm in this view.
		 */
		seq_printf(m, "%-7s ring %3u descriptors, engine at %3u, driver at %3u, %u frames\n",
			   rtl9602c_rx_ring_name[r], ring->len,
			   readw(priv->base + cur[r]),
			   READ_ONCE(ring->tail), READ_ONCE(ring->frames));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl9602c_eth_rings);

static void rtl9602c_eth_debugfs_put(void *d)
{
	debugfs_remove_recursive(d);
}

static int rtl9602c_eth_probe(struct platform_device *pdev)
{
	struct net_device *dev;
	struct rtl9602c_eth *priv;
	void __iomem *base;
	u8 addr[ETH_ALEN];
	unsigned int i;
	int err;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return dev_err_probe(&pdev->dev, PTR_ERR(base),
				     "cannot map the CPU port registers\n");

	/* Before anything is allocated: see rtl9602c_eth_quiesce(). */
	rtl9602c_eth_quiesce(&pdev->dev, base);

	/*
	 * A transmit queue per switch port (see rtl9602c_eth_select_queue()),
	 * one receive queue.
	 */
	dev = devm_alloc_etherdev_mqs(&pdev->dev, sizeof(*priv),
				      RTL9602C_SW_PORTS, 1);
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, &pdev->dev);
	dev->netdev_ops = &rtl9602c_eth_netdev_ops;
	dev->max_mtu = RTL9602C_ETH_MAX_MTU;

	/* Keep dsts: a GEM interface's frame carries its PON flow in one. */
	netif_keep_dst(dev);

	/* The user ports then appear as sfp@cpu and pon@cpu. */
	strscpy(dev->name, "cpu", IFNAMSIZ);

	priv = netdev_priv(dev);
	priv->netdev = dev;
	priv->dev = &pdev->dev;
	spin_lock_init(&priv->tx_lock);
	priv->base = base;

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(&pdev->dev, priv->irq,
				     "no interrupt declared for the CPU port\n");

	/* Each receive descriptor's buffer and its mapping, per ring. */
	priv->rx[RTL9602C_RX_DATA].len = RTL9602C_RX_RING;
	priv->rx[RTL9602C_RX_CTRL].len = RTL9602C_RX_CTRL_RING;
	for (i = 0; i < RTL9602C_RX_RINGS; i++) {
		struct rtl9602c_rx_ring *ring = &priv->rx[i];

		ring->skb = devm_kcalloc(&pdev->dev, ring->len,
					 sizeof(*ring->skb), GFP_KERNEL);
		ring->buf = devm_kcalloc(&pdev->dev, ring->len,
					 sizeof(*ring->buf), GFP_KERNEL);
		if (!ring->skb || !ring->buf)
			return -ENOMEM;
	}

	rtl9602c_eth_read_addr(priv->base, addr);

	if (is_valid_ether_addr(addr)) {
		eth_hw_addr_set(dev, addr);
	} else {
		/* Not a usable unicast address: say so, and make one up. */
		dev_warn(&pdev->dev,
			 "identity registers hold %pM, which is not a usable address; using a random one\n",
			 addr);
		eth_hw_addr_random(dev);
	}

	/*
	 * One METADATA_HW_PORT_MUX dst per switch port, attached to the tagged
	 * frames received from it.
	 */
	for (i = 0; i < RTL9602C_SW_PORTS; i++) {
		priv->port_meta[i] = metadata_dst_alloc(0, METADATA_HW_PORT_MUX,
							GFP_KERNEL);
		if (!priv->port_meta[i]) {
			while (i--)
				metadata_dst_free(priv->port_meta[i]);
			return -ENOMEM;
		}
		priv->port_meta[i]->u.port_info.port_id = i;
	}

	netif_napi_add(dev, &priv->napi, rtl9602c_eth_poll);

	/*
	 * rtl9602c-eth/rxreason: the received frames per trap reason;
	 * rtl9602c-eth/rings: the receive rings.
	 */
	priv->debugfs = debugfs_create_dir("rtl9602c-eth", NULL);
	err = devm_add_action_or_reset(&pdev->dev, rtl9602c_eth_debugfs_put,
				       priv->debugfs);
	if (err)
		return err;
	debugfs_create_file("rxreason", 0600, priv->debugfs, priv,
			    &rtl9602c_eth_rxreason_fops);
	debugfs_create_file("rings", 0400, priv->debugfs, priv,
			    &rtl9602c_eth_rings_fops);

	err = devm_register_netdev(&pdev->dev, dev);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				     "cannot register the interface\n");

	eth_info(dev, "CPU port at %pa, address %pM\n",
		 &pdev->resource[0].start, dev->dev_addr);

	rtl9602c_eth_selftest(dev);
	return 0;
}

static const struct of_device_id rtl9602c_eth_of_match[] = {
	{ .compatible = "realtek,rtl9602c-eth" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9602c_eth_of_match);

static struct platform_driver rtl9602c_eth_driver = {
	.probe	= rtl9602c_eth_probe,
	.driver	= {
		.name		= "rtl9602c-eth",
		.of_match_table	= rtl9602c_eth_of_match,
	},
};
module_platform_driver(rtl9602c_eth_driver);

MODULE_DESCRIPTION("Realtek RTL9602C switch CPU port");
MODULE_LICENSE("GPL");
