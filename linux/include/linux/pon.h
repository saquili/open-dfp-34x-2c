/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The interface between the PON class and the driver of an ONU MAC.
 *
 * An optical network unit (ONU) terminates a passive optical network (PON)
 * on the subscriber's side. The driver of its MAC registers two things with
 * the PON class, both against its own device:
 *
 * - an OMCI control port, which carries the ONU management and control
 *   interface (ITU-T G.988) between the OLT and a userspace daemon;
 * - a data path, on which management creates one "gem" network interface
 *   per GEM port.
 *
 * Both belong to one PON device, ponN, which the "pon" generic netlink
 * family reports on.
 */

#ifndef _LINUX_PON_H
#define _LINUX_PON_H

#include <linux/err.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <uapi/linux/pon.h>

struct device;
struct net_device;
struct netlink_ext_ack;
struct pon_device;
struct pon_omci_port;

/**
 * struct pon_gem_config - a GEM port as management created it
 * @port_id: the Port-ID in the headers of the port's GEM frames, 0 to 4095
 * @direction: %IFLA_GEM_DIR_UPSTREAM, %IFLA_GEM_DIR_DOWNSTREAM or
 *	       %IFLA_GEM_DIR_BOTH
 * @tcont: index of the T-CONT that schedules the port's upstream traffic;
 *	   meaningful only when @direction includes upstream
 * @multicast: the port is a multicast GEM port, and so downstream only
 * @queue: the port's upstream queue in @tcont, 0 the highest priority
 * @weight: 0 to schedule @queue by strict priority, otherwise its weight
 *	    among the T-CONT's weighted queues, 1 to 255
 * @cir: the committed rate of the port's upstream traffic, bytes per second;
 *	 0 for none
 * @pir: the peak rate of the port's upstream traffic, bytes per second; 0
 *	 for no limit but the T-CONT's grants
 * @pbits: the IEEE 802.1p priorities of the upstream frames the port takes
 *	   from its VLANs, bit p for priority p; 0xff for all
 *
 * The class has checked the combination: a multicast port is downstream
 * only, a port that carries upstream traffic has a T-CONT, @queue is below
 * the queues per T-CONT the MAC registered, @cir is not above a @pir that
 * limits, and a port that carries no upstream traffic has the upstream
 * defaults (@pbits 0xff, the others 0). Several GEM ports may share one T-CONT, and one queue
 * of it; the rates of the ports that share a queue add up. The MAC checks
 * @tcont against the T-CONTs it has, and refuses what it cannot do.
 */
struct pon_gem_config {
	u16 port_id;
	u8 direction;
	u32 tcont;
	bool multicast;
	u8 queue;
	u8 weight;
	u64 cir;
	u64 pir;
	u8 pbits;
};

/**
 * define PON_SERIAL_NUMBER_LEN - length of an ONU's serial number
 *
 * As the ONU sends it in Serial_Number_ONU (ITU-T G.984.3 clause 9.2.4.1):
 * four ASCII characters of Vendor_ID, then four bytes of vendor-specific
 * serial number.
 */
#define PON_SERIAL_NUMBER_LEN	8

/**
 * define PON_MAX_ALLOC_IDS - Alloc-IDs one status report can carry
 *
 * A MAC with more reports the first ones. That no OLT assigns this many to
 * one ONU is an assumption, not verified.
 */
#define PON_MAX_ALLOC_IDS	32

/**
 * struct pon_dev_stats - counters of the transmission convergence layer
 * @sn_requests: serial number requests: allocations to the activation
 *		 Alloc-ID, 254, with the PLOAMu flag
 * @ranging_requests: ranging requests: allocations to the ONU's default
 *		      Alloc-ID, its ONU-ID, with the PLOAMu flag. In O5 they
 *		      cannot be told from the OLT's other requests for a
 *		      PLOAM message.
 * @ploam_rx: downstream PLOAM messages received
 * @ploam_crc_errors: downstream PLOAM messages with a bad CRC, which are
 *		      discarded
 * @ploam_overflows: overflows in receiving downstream PLOAM messages
 * @bwmap_rx: bandwidth maps the MAC accepted, which may be fewer than one
 *	      in every frame: what a MAC accepts is its own
 * @bwmap_crc_errors: bandwidth map CRC errors, counted per map or per
 *		      allocation structure, since each allocation structure
 *		      carries its own CRC
 * @bwmap_overflows: overflows in receiving bandwidth maps
 * @gem_frames: downstream GEM frames that carried something; idle frames
 *		are not counted
 * @bip_errors: errors of the downstream bit-interleaved parity (BIP,
 *		G.984.3 clause 8.1.3.4), counted as the bits that differed,
 *		which is G.984.3's ERR (clause 11.2.2)
 * @fec_corrected_bytes: downstream bytes that FEC corrected (G.984.3
 *			 clause 13.1.3)
 * @fec_corrected_codewords: downstream codewords that FEC corrected
 * @fec_uncorrectable_codewords: downstream codewords with more errors than
 *				 FEC corrects
 * @fec_codewords: downstream codewords received while the downstream
 *		   carried FEC, all of them decoded
 *
 * Totals since the MAC was brought up, not register values: a MAC whose
 * counters clear when read adds them up, so that every reader sees the same
 * numbers. What exactly each counts is the MAC's own definition. That an
 * overflow is a PLOAM message or bandwidth map lost for want of room in the
 * MAC is an assumption, not verified.
 */
struct pon_dev_stats {
	u64 sn_requests;
	u64 ranging_requests;
	u64 ploam_rx;
	u64 ploam_crc_errors;
	u64 ploam_overflows;
	u64 bwmap_rx;
	u64 bwmap_crc_errors;
	u64 bwmap_overflows;
	u64 gem_frames;
	u64 bip_errors;
	u64 fec_corrected_bytes;
	u64 fec_corrected_codewords;
	u64 fec_uncorrectable_codewords;
	u64 fec_codewords;
};

/**
 * struct pon_dev_status - what a MAC reports about its ONU
 * @type: a PON_TYPE_* value; 0 when unknown
 * @onu_state: the activation state, %PON_ONU_STATE_O1 to %PON_ONU_STATE_O7;
 *	       0 when unknown
 * @alarms: PON_ALARM_* flags for the conditions present now; valid with
 *	    @have_alarms
 * @have_alarms: @alarms is a report, so a clear bit says "no alarm"
 * @onu_id: the ONU-ID the OLT assigned, 0 to 253; -1 while there is none
 * @omcc_port_id: the Port-ID of the OMCI channel once the OLT has
 *		  configured it; -1 until then
 * @ifindex: the interface that carries the ONU's traffic; 0 when unknown
 * @serial_number: the ONU's serial number; valid with @have_serial
 * @have_serial: @serial_number is set
 * @alloc_id: the Alloc-IDs the OLT assigned to the ONU
 * @num_alloc_ids: how many entries of @alloc_id are used
 * @stats: the totals; valid with @have_stats
 * @have_stats: @stats is filled in
 * @power_level: the transmit power level, a PON_POWER_LEVEL_* value; valid
 *		 with @have_power_level
 * @have_power_level: @power_level is set
 *
 * What is not known stays "nothing known": @onu_id and @omcc_port_id -1,
 * everything else 0. The class hands the structure to
 * &pon_device_ops.dev_status in that state, and leaves out of a report
 * whatever the MAC did not set, because 0 is a valid ONU-ID, Port-ID and
 * Alloc-ID, and "no alarms" is a claim a MAC in reset cannot make.
 */
struct pon_dev_status {
	u32 type;
	u32 onu_state;
	u32 alarms;
	bool have_alarms;
	int onu_id;
	int omcc_port_id;
	int ifindex;
	u8 serial_number[PON_SERIAL_NUMBER_LEN];
	bool have_serial;
	u16 alloc_id[PON_MAX_ALLOC_IDS];
	unsigned int num_alloc_ids;
	struct pon_dev_stats stats;
	bool have_stats;
	u32 power_level;
	bool have_power_level;
};

/**
 * struct pon_gem_stats - a GEM port's counters, as the MAC counts them
 * @tx_octets: the octets of the GEM frames the MAC sent upstream on the port,
 *	their headers included
 * @tx_frames: the GEM frames the MAC sent upstream on the port, a fragment
 *	counting as one
 * @rx_frames: the GEM frames the MAC received on the port, a fragment counting
 *	as one
 * @rx_octets: the payload octets of the GEM frames the MAC received on the
 *	port, their headers left out
 */
struct pon_gem_stats {
	u64 tx_octets;
	u64 tx_frames;
	u64 rx_frames;
	u64 rx_octets;
};

/**
 * struct pon_device_ops - a MAC's data path operations
 *
 * The operations owns_netdev, gem_add, gem_del and gem_xmit are required.
 * The others are optional; gem_bridge_join and gem_bridge_leave come as a
 * pair, and so do gem_vlan_add and gem_vlan_del. Each operation reaches the
 * MAC's state through pon_device_get_drvdata(). A flow is the MAC's number
 * for a GEM port, below the num_flows given to pon_device_register().
 */
struct pon_device_ops {
	/**
	 * @owns_netdev: Say whether dev is the interface that carries this
	 * ONU's traffic, the one its GEM interfaces stack on. Called with
	 * RTNL held and under the class's registration lock; it must not
	 * take RTNL.
	 */
	bool (*owns_netdev)(struct pon_device *pon,
			    const struct net_device *dev);
	/**
	 * @gem_add: Create the GEM port that cfg describes, and store in
	 * flow the MAC's flow for it, one that no other GEM port uses.
	 * Refuse what the MAC cannot do, with a message in extack. Called
	 * with RTNL held. Returns 0 or a negative error code.
	 */
	int (*gem_add)(struct pon_device *pon, const struct pon_gem_config *cfg,
		       unsigned int *flow, struct netlink_ext_ack *extack);
	/**
	 * @gem_change: Apply cfg to the GEM port of flow. cfg differs from the
	 * port's configuration only in its upstream attributes (pbits, queue,
	 * weight, cir and pir), and the class has checked it as for gem_add.
	 * Refuse what the MAC cannot do with a message in extack, leaving the
	 * port as it was. Called with RTNL held. Without it, a GEM port's
	 * upstream attributes stay as the port was created.
	 */
	int (*gem_change)(struct pon_device *pon, unsigned int flow,
			  const struct pon_gem_config *cfg,
			  struct netlink_ext_ack *extack);
	/**
	 * @gem_del: Remove the GEM port of flow. Called with RTNL held. When
	 * a bridged GEM interface is deleted, gem_del comes first and
	 * gem_bridge_leave for the same flow follows, when the bridge lets
	 * go of the interface during its unregistration; a MAC must accept
	 * the two in either order.
	 */
	void (*gem_del)(struct pon_device *pon, unsigned int flow);
	/**
	 * @gem_xmit: Send skb upstream on its GEM port. skb carries a
	 * %METADATA_HW_PORT_MUX dst whose port_id is the flow. The MAC owns
	 * skb whatever the outcome, and returns a NET_XMIT_* code. Called
	 * from ndo_start_xmit with bottom halves disabled. GEM interfaces
	 * transmit without a lock, so calls may run on several CPUs at once.
	 */
	int (*gem_xmit)(struct pon_device *pon, struct sk_buff *skb);
	/**
	 * @gem_bridge_join: The GEM interface of flow joins bridge br:
	 * forward in hardware between its GEM port and the MAC's other ports
	 * in br, or refuse with a message in extack, which fails the join.
	 * Called with RTNL held. Without this pair, the bridge forwards the
	 * traffic of GEM interfaces in software.
	 */
	int (*gem_bridge_join)(struct pon_device *pon, unsigned int flow,
			       struct net_device *br,
			       struct netlink_ext_ack *extack);
	/**
	 * @gem_bridge_leave: The GEM interface of flow has left bridge br, or
	 * the class could not complete its join: undo gem_bridge_join.
	 * Called with RTNL held, possibly after gem_del (see there).
	 */
	void (*gem_bridge_leave)(struct pon_device *pon, unsigned int flow,
				 struct net_device *br);
	/**
	 * @gem_vlan_add: Add bridge VLAN vid to the GEM port of flow; flags
	 * are the bridge's %BRIDGE_VLAN_INFO_PVID and
	 * %BRIDGE_VLAN_INFO_UNTAGGED. Called with RTNL held, and only while
	 * the port is offloaded, that is after gem_bridge_join succeeded.
	 */
	int (*gem_vlan_add)(struct pon_device *pon, unsigned int flow, u16 vid,
			    u16 flags, struct netlink_ext_ack *extack);
	/**
	 * @gem_vlan_del: Remove bridge VLAN vid from the GEM port of flow;
	 * last says that no other GEM interface of the same bridge still has
	 * vid, so that a MAC that holds VLAN membership once for all of its
	 * GEM ports can drop the VLAN then. Called with RTNL held.
	 */
	void (*gem_vlan_del)(struct pon_device *pon, unsigned int flow, u16 vid,
			     bool last);
	/**
	 * @gem_mrouter: The bridge has started (mrouter true) or stopped
	 * seeing a multicast router behind the GEM port of flow, from the
	 * queries it snoops or from the port's multicast_router setting.
	 * Called with RTNL held, and only while the port is offloaded.
	 */
	void (*gem_mrouter)(struct pon_device *pon, unsigned int flow,
			    bool mrouter);
	/**
	 * @dev_status: Fill st with the state of the transmission convergence
	 * layer: the activation state, the identifiers the OLT assigned, the
	 * alarms present now and the totals. st arrives set to "nothing
	 * known" (see &struct pon_dev_status); leave unset what is not known.
	 * An error discards the whole report. Called under the class's
	 * registration lock; it may sleep and must not take RTNL.
	 */
	int (*dev_status)(struct pon_device *pon, struct pon_dev_status *st);
	/**
	 * @gem_stats: Fill st with the MAC's counters for the GEM port of
	 * flow. May sleep. Without it, the ethtool statistics of GEM
	 * interfaces are empty.
	 */
	int (*gem_stats)(struct pon_device *pon, unsigned int flow,
			 struct pon_gem_stats *st);
	/**
	 * @tcont_get: Store in alloc_id the Alloc-ID bound to T-CONT index,
	 * which is below the num_tconts given at registration, or return
	 * -ENOENT when that T-CONT is idle. Idle needs an error code because
	 * 0 is a valid Alloc-ID: the default Alloc-ID equals the ONU-ID, and
	 * ONU-ID 0 is valid. Any other error fails the report of that T-CONT,
	 * so only 0 and -ENOENT are expected. Called with the class's
	 * registration lock held, so the data path stays registered for the
	 * call; may sleep.
	 */
	int (*tcont_get)(struct pon_device *pon, unsigned int index,
			 u16 *alloc_id);
	/**
	 * @tcont_set: Map T-CONT index, below the num_tconts given at
	 * registration, to Alloc-ID alloc_id, 0 to 4095, or unmap it when
	 * alloc_id is negative, as the OLT does through the OMCC (ITU-T
	 * G.984.3 clause 5.5.4). Bind the T-CONT to the Alloc-ID once the OLT
	 * has assigned the Alloc-ID to the ONU too. Return 0, -EBUSY when
	 * another T-CONT is mapped to alloc_id, or another negative error
	 * code with a message in extack, leaving the mappings as they were.
	 * Called with the class's registration lock held; may sleep.
	 * Optional: without it, tcont-set fails with -EOPNOTSUPP.
	 */
	int (*tcont_set)(struct pon_device *pon, unsigned int index,
			 int alloc_id, struct netlink_ext_ack *extack);
};

/**
 * define PON_OMCI_MAX_MSG - the largest OMCI message, in bytes
 *
 * An extended OMCI message (ITU-T G.988 clause 11.2): ten bytes of header,
 * up to 1966 bytes of contents and a four-byte message integrity check. A
 * baseline message is 48 bytes.
 */
#define PON_OMCI_MAX_MSG	1980

/**
 * struct pon_omci_port_ops - a MAC's OMCI port operations
 * @start: optional; called when the port is opened, under the port's lock.
 *	   An error fails the open.
 * @stop: optional; called under the port's lock when the port is closed,
 *	  and when the MAC removes a port that is open.
 * @tx: send one OMCI message, skb, towards the OLT. On success the MAC owns
 *	skb; on error it must not touch it, and the class frees it. -EAGAIN
 *	means the transmit path is momentarily full. Called in process
 *	context under the port's lock.
 */
struct pon_omci_port_ops {
	int (*start)(struct pon_omci_port *port);
	void (*stop)(struct pon_omci_port *port);
	int (*tx)(struct pon_omci_port *port, struct sk_buff *skb);
};

#if IS_ENABLED(CONFIG_PON)
/**
 * pon_device_register() - register a MAC's data path
 * @parent: the MAC driver's device
 * @ops: the data path operations
 * @num_flows: the size of the MAC's flow space; every flow @ops->gem_add
 *	       hands back is below it
 * @num_tconts: the T-CONTs the ONU can be given for subscriber traffic
 * @num_queues: the upstream queues of each of those T-CONTs, at least 1;
 *		every &pon_gem_config.queue is below it
 * @drvdata: the MAC's private data, for pon_device_get_drvdata()
 *
 * Attaches the data path to the PON device of @parent, creating ponN when
 * the MAC has not created its OMCI port yet. From then on management can
 * create GEM interfaces on the interface @ops->owns_netdev claims.
 *
 * @num_tconts leaves out a T-CONT the MAC keeps for the ONU's own
 * management channel, because no T-CONT managed entity (ITU-T G.988 clause
 * 9.2.2) need point at that one: the default Alloc-ID, which carries the
 * OMCI channel, can stay outside every T-CONT managed entity.
 *
 * Context: process context; may sleep.
 * Return: the PON device, or an ERR_PTR(): -EINVAL when a required
 * operation is missing, @num_flows or @num_queues is 0, or an optional pair
 * is half given; -EBUSY when @parent already has a data path; -EOPNOTSUPP
 * without CONFIG_PON.
 */
struct pon_device *pon_device_register(struct device *parent,
				       const struct pon_device_ops *ops,
				       unsigned int num_flows,
				       unsigned int num_tconts,
				       unsigned int num_queues, void *drvdata);
/**
 * pon_device_unregister() - unregister a MAC's data path
 * @pon: the PON device pon_device_register() returned
 *
 * Deletes the ONU's GEM interfaces, then detaches the operations. The MAC
 * must have stopped calling pon_gem_rx() before. The data path and the OMCI
 * port may go in either order: ponN is unregistered with the last of them.
 *
 * Context: process context, RTNL not held.
 */
void pon_device_unregister(struct pon_device *pon);
/**
 * pon_device_get_drvdata() - the MAC's private data for its data path
 * @pon: the PON device
 *
 * Return: the @drvdata given to pon_device_register().
 */
void *pon_device_get_drvdata(struct pon_device *pon);
/**
 * pon_gem_rx() - deliver a downstream frame to the GEM interface of its flow
 * @pon: the PON device
 * @flow: the flow the frame arrived on
 * @skb: the frame, with skb->data at its Ethernet header
 *
 * Context: the MAC's receive path, as for netif_receive_skb().
 * Return: true when @flow has a GEM interface: the frame was delivered, or
 * dropped because the interface is down, and is no longer the caller's.
 * False when @flow has none: the frame is still the caller's.
 */
bool pon_gem_rx(struct pon_device *pon, unsigned int flow, struct sk_buff *skb);
/**
 * pon_gem_netdev_flow() - the flow of a GEM interface
 * @dev: any network device
 * @flow: set to the flow of @dev when it is a GEM interface
 *
 * For MAC code that is handed devices by stacks that know nothing of GEM,
 * such as a flow offload naming its egress device, or a route naming its
 * output interface.
 *
 * Return: true when @dev is a GEM interface.
 */
bool pon_gem_netdev_flow(const struct net_device *dev, unsigned int *flow);
/**
 * pon_device_notify() - report a change of the ONU's status
 * @pon: the PON device
 * @st: the status the MAC has just built, with what it does not know left
 *	as &struct pon_dev_status describes
 *
 * Sends one dev-change-ntf message, with the attributes of a dev-get reply,
 * to the "monitor" group of the pon family, or nothing when nobody listens.
 * The class does not call back into the MAC, so the MAC may call this under
 * the lock it made the change under.
 *
 * Context: process context; allocates with GFP_KERNEL.
 */
void pon_device_notify(struct pon_device *pon, const struct pon_dev_status *st);

/**
 * pon_omci_port_create() - create a MAC's OMCI control port
 * @parent: the MAC driver's device
 * @ops: the port's operations; @ops->tx is required
 * @drvdata: the MAC's private data, for pon_omci_port_get_drvdata()
 *
 * Creates ponN for @parent when the MAC has not registered its data path
 * yet, and registers the port as the character device ponNomci0. A MAC has
 * one port, always number 0, because an ONU has at most one OMCI
 * connection (ITU-T G.984.3 clause 9.2.3.14). The port starts without the
 * OMCI channel; the MAC reports it with pon_omci_port_link_set().
 *
 * Context: process context; may sleep.
 * Return: the port, or an ERR_PTR(): -EINVAL without @ops->tx; -ENOSPC when
 * all 256 minors of the class are taken; -EOPNOTSUPP without CONFIG_PON.
 */
struct pon_omci_port *pon_omci_port_create(struct device *parent,
					   const struct pon_omci_port_ops *ops,
					   void *drvdata);
/**
 * pon_omci_port_remove() - remove a MAC's OMCI control port
 * @port: the port
 *
 * Calls the stop operation if the port is open, detaches the operations,
 * wakes readers, who then get -ENODEV, and drops the queued messages. A
 * file that holds the port open stays valid until it is closed. The MAC
 * must have stopped calling pon_omci_port_rx(). The port and the data path
 * may go in either order (see pon_device_unregister()).
 *
 * Context: process context; may sleep.
 */
void pon_omci_port_remove(struct pon_omci_port *port);
/**
 * pon_omci_port_rx() - hand one received OMCI message to the port
 * @port: the port
 * @skb: exactly one message, linear; the class owns it from here on
 *
 * The message is queued for the reader, or dropped and counted when nobody
 * holds the port open or the reader has fallen too far behind.
 *
 * Context: any context the MAC receives in.
 */
void pon_omci_port_rx(struct pon_omci_port *port, struct sk_buff *skb);
/**
 * pon_omci_port_link_set() - say whether the OMCI channel exists
 * @port: the port
 * @up: whether the MAC has a GEM port for the OMCI channel
 *
 * Writes fail with -ENOLINK while the channel is down, and poll reports the
 * port writable only while it is up. Going down drops the messages still
 * queued. A repeated value is ignored.
 */
void pon_omci_port_link_set(struct pon_omci_port *port, bool up);
/**
 * pon_omci_port_get_drvdata() - the MAC's private data for its OMCI port
 * @port: the port
 *
 * Return: the @drvdata given to pon_omci_port_create().
 */
void *pon_omci_port_get_drvdata(struct pon_omci_port *port);
#else
/*
 * Without the PON class a MAC driver still builds: registration and port
 * creation fail with -EOPNOTSUPP, a received OMCI message is freed, no flow
 * has a GEM interface, and the rest do nothing.
 */
static inline struct pon_device *
pon_device_register(struct device *parent, const struct pon_device_ops *ops,
		    unsigned int num_flows, unsigned int num_tconts,
		    unsigned int num_queues, void *drvdata)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void pon_device_unregister(struct pon_device *pon) { }

static inline void *pon_device_get_drvdata(struct pon_device *pon)
{
	return NULL;
}

static inline bool pon_gem_rx(struct pon_device *pon, unsigned int flow,
			      struct sk_buff *skb)
{
	return false;
}

static inline bool pon_gem_netdev_flow(const struct net_device *dev,
				       unsigned int *flow)
{
	return false;
}

static inline void pon_device_notify(struct pon_device *pon,
				     const struct pon_dev_status *st) { }

static inline struct pon_omci_port *
pon_omci_port_create(struct device *parent,
		     const struct pon_omci_port_ops *ops, void *drvdata)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void pon_omci_port_remove(struct pon_omci_port *port) { }

static inline void pon_omci_port_rx(struct pon_omci_port *port,
				    struct sk_buff *skb)
{
	kfree_skb(skb);
}

static inline void pon_omci_port_link_set(struct pon_omci_port *port,
					  bool up) { }

static inline void *pon_omci_port_get_drvdata(struct pon_omci_port *port)
{
	return NULL;
}
#endif

#endif
