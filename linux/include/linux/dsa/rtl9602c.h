/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The contract between the RTL9602C switch driver, its DSA tagger and the
 * driver of its CPU port (the conduit, rtl9602c-eth).
 *
 * The CPU port's DMA engine carries the CPU tag in its descriptors, so the
 * switch driver reaches the PON port's control and data paths through the
 * conduit: it attaches one receive handler for OMCI frames and one for
 * every tagged data frame, and it sends OMCI frames with
 * rtl9602c_conduit_ctrl_xmit().
 *
 * Transmit timestamps take both drivers as well. The switch driver marks a
 * PTP message whose departure time it awaits, or a one-step Sync or
 * Pdelay_Resp into which the switch writes a time, with SKBTX_IN_PROGRESS in
 * skb_shinfo()->tx_flags, which reaches the conduit's transmit path where
 * skb->cb does not; the conduit sends such a frame so that the switch
 * applies its PTP egress action to it, and hands the departure report that
 * the switch then sends to the CPU to the handler attached with
 * rtl9602c_conduit_tstamp_attach().
 */

#ifndef _LINUX_DSA_RTL9602C_H
#define _LINUX_DSA_RTL9602C_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/types.h>

struct net_device;
struct sk_buff;

/* The switch port behind which the GPON MAC sits */
#define RTL9602C_PORT_PON	2

/**
 * typedef rtl9602c_ctrl_rx_t - receive handler for OMCI frames
 * @ctx: the context given to rtl9602c_conduit_ctrl_attach()
 * @skb: the OMCI frame; it has no Ethernet header, so its data starts at
 *	the message's first byte
 * @port: the switch port the frame came in by
 *
 * Called from the conduit's NAPI poll for each frame that the PON MAC
 * marks as OMCI and that carries a CPU tag.
 *
 * Return: true if the handler took @skb, which it then owns; false to let
 * the frame continue up the ordinary receive path.
 */
typedef bool (*rtl9602c_ctrl_rx_t)(void *ctx, struct sk_buff *skb,
				   unsigned int port);

/*
 * The flow given to the receive handler for data frames when the conduit
 * cannot tell it: for a PTP message with the switch's whole timestamp tag,
 * whose receive descriptor holds part of the message's sequenceId where it
 * otherwise holds the flow, and whose tag holds no flow either.
 */
#define RTL9602C_FLOW_UNKNOWN	UINT_MAX

/**
 * typedef rtl9602c_flow_rx_t - receive handler for tagged data frames
 * @ctx: the context given to rtl9602c_conduit_flow_attach()
 * @skb: the frame, from its Ethernet header
 * @port: the switch port the frame came in by
 * @flow: the PON flow the frame arrived on, 0 to 127, or
 *	%RTL9602C_FLOW_UNKNOWN; meaningful only for a frame from the PON port
 *
 * Called from the conduit's NAPI poll for each frame that carries a CPU
 * tag, before DSA sees it. The conduit has taken off the timestamp tag of a
 * PTP message and made its time the frame's hardware timestamp.
 *
 * Return: true if the handler took @skb, which it then owns; false to let
 * DSA deliver the frame to the user port of @port.
 */
typedef bool (*rtl9602c_flow_rx_t)(void *ctx, struct sk_buff *skb,
				   unsigned int port, unsigned int flow);

/**
 * typedef rtl9602c_tstamp_rx_t - handler for departure reports
 * @ctx: the context given to rtl9602c_conduit_tstamp_attach()
 * @stamp: the time at which a PTP message left the switch, the PTP clock's
 *	seconds and nanoseconds
 *
 * Called from the conduit's NAPI poll for each departure report, a frame the
 * switch sends to the CPU when a PTP message leaves under the egress action
 * that latches its time and mirrors it to the CPU. Of the message the report
 * carries only the port it left by and the low 7 bits of its sequenceId,
 * which the conduit does not pass on. The conduit consumes the report.
 */
typedef void (*rtl9602c_tstamp_rx_t)(void *ctx, ktime_t stamp);

#if IS_ENABLED(CONFIG_RTL9602C_ETH)
/* Provided by the conduit driver, rtl9602c-eth.c, which documents them */
int rtl9602c_conduit_ctrl_attach(struct net_device *conduit,
				 rtl9602c_ctrl_rx_t rx, void *ctx);
void rtl9602c_conduit_ctrl_detach(struct net_device *conduit);
int rtl9602c_conduit_ctrl_xmit(struct net_device *conduit,
			       struct sk_buff *skb, unsigned int port,
			       unsigned int stream);
int rtl9602c_conduit_flow_attach(struct net_device *conduit,
				 rtl9602c_flow_rx_t rx, void *ctx);
void rtl9602c_conduit_flow_detach(struct net_device *conduit);
int rtl9602c_conduit_tstamp_attach(struct net_device *conduit,
				   rtl9602c_tstamp_rx_t rx, void *ctx);
void rtl9602c_conduit_tstamp_detach(struct net_device *conduit);
#else
/* Without the conduit driver there is no path to the PON port. */
static inline int rtl9602c_conduit_ctrl_attach(struct net_device *conduit,
					       rtl9602c_ctrl_rx_t rx,
					       void *ctx)
{
	return -EOPNOTSUPP;
}

static inline void rtl9602c_conduit_ctrl_detach(struct net_device *conduit)
{
}

static inline int rtl9602c_conduit_ctrl_xmit(struct net_device *conduit,
					     struct sk_buff *skb,
					     unsigned int port,
					     unsigned int stream)
{
	return -EOPNOTSUPP;
}

static inline int rtl9602c_conduit_flow_attach(struct net_device *conduit,
					       rtl9602c_flow_rx_t rx,
					       void *ctx)
{
	return -EOPNOTSUPP;
}

static inline void rtl9602c_conduit_flow_detach(struct net_device *conduit)
{
}

/* Nor any departure report. */
static inline int rtl9602c_conduit_tstamp_attach(struct net_device *conduit,
						 rtl9602c_tstamp_rx_t rx,
						 void *ctx)
{
	return -EOPNOTSUPP;
}

static inline void rtl9602c_conduit_tstamp_detach(struct net_device *conduit)
{
}
#endif

#endif
