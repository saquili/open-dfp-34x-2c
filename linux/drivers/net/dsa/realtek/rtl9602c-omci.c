// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C switch: the OMCI channel, as the PON class's OMCI port.
 *
 * OMCI messages cross the PON port in the flow of the OMCC, the ONU's OMCI
 * channel. Downstream, the switch traps them to the CPU port, and the conduit
 * hands each one to the OMCI receive handler here; it reaches the port as it
 * arrived, less what the hardware appends. Upstream, a message written to the
 * port goes to the conduit for the PON port in the OMCC's flow, which the PON
 * MAC schedules in T-CONT 16, the T-CONT of the default Alloc-ID. The driver
 * neither checks nor computes the message integrity check: the daemon behind
 * the port builds every message whole.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/netdevice.h>
#include <linux/pon.h>
#include <linux/skbuff.h>
#include <linux/dsa/rtl9602c.h>
#include <net/dsa.h>

#include "rtl9602c.h"

/*
 * Every downstream OMCI message arrives with four zero bytes after it, added
 * between the GPON MAC and the switch: a 48-byte baseline message arrives 52
 * bytes long once the FCS is off (measured on this board). They are cut from
 * every message received; that an extended message carries them as well is
 * an assumption, not verified.
 */
#define RTL9602C_OMCI_RX_TRAILER	4

/*
 * The OMCI receive handler. Only a message from the PON port is taken; any
 * other frame goes on as an ordinary frame. The trailer is cut, and a frame
 * no longer than it is dropped. A message is delivered only in O5, the state
 * in which the ONU exchanges traffic with the OLT (ITU-T G.984.3 clause
 * 10.2.2); in any other state it is dropped with a debug line, and the port's
 * statistics do not count it. That messages received outside O5 must be
 * dropped is not verified. The state is read once and without gpon_lock:
 * that is a mutex, which the receive path cannot take.
 */
static bool rtl9602c_omci_rx(void *ctx, struct sk_buff *skb, unsigned int port)
{
	struct rtl9602c_priv *p = ctx;
	u8 state = READ_ONCE(p->gpon_state);

	if (port != RTL9602C_PORT_PON)
		return false;

	if (skb->len <= RTL9602C_OMCI_RX_TRAILER) {
		dev_dbg_ratelimited(p->dev, "<OMCI> %u-byte frame dropped\n",
				    skb->len);
		dev_kfree_skb(skb);
		return true;
	}
	skb_trim(skb, skb->len - RTL9602C_OMCI_RX_TRAILER);

	if (state != RTL9602C_ONU_O5_OPERATION) {
		dev_dbg_ratelimited(p->dev,
				    "<OMCI> %u-byte message in O%u dropped\n",
				    skb->len, state);
		dev_kfree_skb(skb);
		return true;
	}

	pon_omci_port_rx(p->omci_port, skb);
	return true;
}

/*
 * The PON class sends only while the OMCI channel exists. The OMCC's binding
 * is checked again here, closing the race with an unbind in between, since
 * the OMCC's GEM frames must carry the Port-ID that Configure_Port-ID
 * assigned (ITU-T G.984.3 clause 14.1). The conduit's errors, -EAGAIN for a
 * full ring among them, are returned as they are.
 */
static int rtl9602c_omci_tx(struct pon_omci_port *port, struct sk_buff *skb)
{
	struct rtl9602c_priv *p = pon_omci_port_get_drvdata(port);

	if (!READ_ONCE(p->gpon_omcc_bound))
		return -ENOLINK;

	return rtl9602c_conduit_ctrl_xmit(p->conduit, skb, RTL9602C_PORT_PON,
					  RTL9602C_GPON_OMCC_FLOW);
}

static const struct pon_omci_port_ops rtl9602c_omci_ops = {
	.tx = rtl9602c_omci_tx,
};

/**
 * rtl9602c_omci_setup() - create the switch's OMCI port
 * @p: the switch
 *
 * Creates the PON class's OMCI port for the switch, ponNomci0, and attaches
 * the OMCI receive handler to the conduit. The port starts without the OMCI
 * channel, which the GPON MAC code reports (pon_omci_port_link_set()) once
 * Configure_Port-ID has bound the OMCC. Nothing is created without the PON
 * MAC's system controller and buffers, or without a conduit: the OMCI path
 * needs both.
 *
 * Return: 0, also when nothing is created, or a negative error code, with
 * nothing created.
 */
int rtl9602c_omci_setup(struct rtl9602c_priv *p)
{
	struct dsa_port *cpu_dp = dsa_to_port(&p->ds, RTL9602C_PORT_CPU);
	struct pon_omci_port *port;
	int err;

	if (!p->sysctl || !cpu_dp || !cpu_dp->conduit)
		return 0;

	port = pon_omci_port_create(p->dev, &rtl9602c_omci_ops, p);
	if (IS_ERR(port))
		return PTR_ERR(port);

	/* The handler delivers to p->omci_port, so both are set first. */
	p->conduit = cpu_dp->conduit;
	p->omci_port = port;

	err = rtl9602c_conduit_ctrl_attach(p->conduit, rtl9602c_omci_rx, p);
	if (err) {
		pon_omci_port_remove(port);
		p->omci_port = NULL;
		p->conduit = NULL;
		return err;
	}

	return 0;
}

/**
 * rtl9602c_omci_teardown() - remove the switch's OMCI port
 * @p: the switch
 *
 * Detaches the OMCI receive handler, so that no message reaches
 * pon_omci_port_rx() any more, then removes the port. Runs after
 * rtl9602c_gem_teardown(): the PON class needs the data path unregistered
 * before the OMCI port goes.
 */
void rtl9602c_omci_teardown(struct rtl9602c_priv *p)
{
	if (!p->omci_port)
		return;

	rtl9602c_conduit_ctrl_detach(p->conduit);
	pon_omci_port_remove(p->omci_port);
	p->omci_port = NULL;
	p->conduit = NULL;
}
