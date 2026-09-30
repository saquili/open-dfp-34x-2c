// SPDX-License-Identifier: GPL-2.0-only
/*
 * DSA tag driver for the Realtek RTL9602C switch.
 *
 * The RTL9602C carries its CPU tag in the DMA descriptors of the CPU port,
 * never in the frame, so this tagger adds and removes no bytes. On transmit
 * it records the destination port as the queue mapping, from which the
 * conduit driver (rtl9602c-eth) fills in the descriptor's port mask. On
 * receive the conduit attaches a METADATA_HW_PORT_MUX dst naming the source
 * port, and DSA delivers the frame without calling this tagger.
 */

#include <linux/dsa/rtl9602c.h>

#include "tag.h"

#define RTL9602C_NAME	"rtl9602c"

static struct sk_buff *rtl9602c_tag_xmit(struct sk_buff *skb,
					 struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);

	/*
	 * Towards the OLT, the PON MAC files a frame under the T-CONT queue
	 * and GEM Port-ID of the flow its descriptor names. A frame sent on
	 * the PON port's own interface names no flow, so it is dropped; GEM
	 * interfaces hand their frames to the conduit directly.
	 */
	if (dp->index == RTL9602C_PORT_PON) {
		dev_core_stats_tx_dropped_inc(dev);
		return NULL;
	}

	/*
	 * The conduit has one transmit queue per switch port and turns the
	 * queue into the descriptor's destination port.
	 */
	skb_set_queue_mapping(skb, dp->index);

	return skb;
}

static struct sk_buff *rtl9602c_tag_rcv(struct sk_buff *skb,
					struct net_device *dev)
{
	/*
	 * Only a frame without a CPU tag gets here: its source port is
	 * unknown, so it is dropped. The callback has to exist all the same:
	 * netdev_uses_dsa() is true for the conduit only when its CPU port
	 * has a receive callback, and eth_type_trans() hands frames to DSA
	 * only then.
	 */
	return NULL;
}

static const struct dsa_device_ops rtl9602c_netdev_ops = {
	.name	= RTL9602C_NAME,
	.proto	= DSA_TAG_PROTO_RTL9602C,
	.xmit	= rtl9602c_tag_xmit,
	.rcv	= rtl9602c_tag_rcv,
};

module_dsa_tag_driver(rtl9602c_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_RTL9602C, RTL9602C_NAME);
MODULE_DESCRIPTION("DSA tag driver for the Realtek RTL9602C");
MODULE_LICENSE("GPL");
