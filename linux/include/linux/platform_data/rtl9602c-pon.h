/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The contract between the RTL9602C switch driver and the driver of its GPON
 * MAC.
 *
 * The GPON MAC, the GEM ports and the OMCI channel sit behind the switch's
 * PON port, in the same register window and behind the same interrupt. The
 * switch driver (drivers/net/dsa/realtek/rtl9602c-*.c) makes an auxiliary
 * device named "rtl9602c.pon" for them, with a struct rtl9602c_pon_data as
 * its platform data, and the PON driver (drivers/net/pon/rtl9602c/) binds to
 * it.
 *
 * The two drivers call each other through two tables. The host operations are
 * the switch driver's services to the MAC: the interrupt, the laser's
 * transmit-disable line and power level, and the switch's side of the GEM
 * ports, which it forwards between. The PON operations are the MAC's to the
 * switch driver: the bring-up with the PON port, the interrupt's thread, the
 * ONU's identity and the state the switch needs to know of.
 */

#ifndef _LINUX_PLATFORM_DATA_RTL9602C_PON_H
#define _LINUX_PLATFORM_DATA_RTL9602C_PON_H

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/rwsem.h>
#include <linux/types.h>

struct dentry;
struct dsa_switch;
struct fwnode_handle;
struct net_device;
struct netlink_ext_ack;
struct pon_gem_rx_filter;
struct timespec64;

/* The switch port whose DMA engine is the conduit. */
#define RTL9602C_PORT_CPU		3

/*
 * The GPON MAC's flows, one for each GEM port it carries: the number of them,
 * and the switch interrupt's GPON source.
 */
#define RTL9602C_GPON_NUM_FLOW		128
#define RTL9602C_IRQ_SRC_GPON		BIT(10)

/* The blocks of a port's counters in the switch's window. */
#define RTL9602C_MIB_TX(p)		(0x032000 + 128 * (p))
#define RTL9602C_MIB_RX(p)		(0x032200 + 128 * (p))

/* The GPON MAC's revision register, which devlink info reads too. */
#define RTL9602C_GPON_REVISION		0x700010

/*
 * The received superframe count, advancing once per 125 us frame while the
 * downstream is framed; the switch's PTP clock reads it beside its time in
 * the first half of the GPON interrupt.
 */
#define RTL9602C_GTC_DS_SUPERFRAME	0x701048
#define RTL9602C_GTC_DS_SUPERFRAME_CNT		GENMASK(29, 0)

/**
 * struct rtl9602c_pon_host_ops - what the switch driver does for the MAC
 * @irq_enable: change the sources of the switch interrupt: clear the bits of
 *	@clr, then set those of @set
 * @gem_added: a flow now carries a GEM port
 * @gem_unbind: a GEM port is going: leave the bridge, give up its VLAN claims
 *	and the routed flows that name it
 * @gem_forget: a GEM port is gone: forget its addresses and its say on
 *	unknown unicast
 * @gem_bridge_join: the GEM interface of @flow joined the bridge @br;
 *	@upstream says that it carries upstream traffic
 * @gem_bridge_leave: it left
 * @gem_vlan_add: the GEM interface of @flow carries VLAN @vid with @flags;
 *	@pbits are the priorities of its upstream frames, if @upstream
 * @gem_vlan_del: it no longer does; @last says that no GEM interface does
 * @gem_pbits_change: the upstream priorities of the GEM port in @flow are now
 *	@pbits
 * @gem_mrouter: a multicast router appeared or went behind a GEM port
 * @gem_rx_filter: the unicast frames the GEM interface of @flow takes
 * @gem_ds_limit: limit the traffic the OLT sends the GEM port of @flow to
 *	@rate bytes per second, with a bucket of @burst bytes, 0 for the
 *	switch's own; a @rate of 0 takes the limit away; refuse with a message
 *	in @extack what the switch cannot do
 * @tod_arm: give the PTP clock @ts to take at the MAC's time-of-day
 *	superframe, about @delay jiffies from now; -EOPNOTSUPP without a
 *	clock. The MAC arms the superframe after this call.
 * @second: a second of the OLT's downstream frames began, in the GPON
 *	interrupt the switch driver noted the time of in its first half
 *	(rtl9602c_pon_ops.second_enable())
 *
 * All are called in process context, with the PON driver's lock not held but
 * for @tod_arm.
 */
struct rtl9602c_pon_host_ops {
	void (*irq_enable)(void *host, u32 clr, u32 set);
	void (*gem_added)(void *host, unsigned int flow);
	void (*gem_unbind)(void *host, unsigned int flow);
	void (*gem_forget)(void *host, unsigned int flow);
	int (*gem_bridge_join)(void *host, unsigned int flow, bool upstream,
			       struct net_device *br,
			       struct netlink_ext_ack *extack);
	void (*gem_bridge_leave)(void *host, unsigned int flow);
	int (*gem_vlan_add)(void *host, unsigned int flow, bool upstream,
			    u8 pbits, u16 vid, u16 flags,
			    struct netlink_ext_ack *extack);
	void (*gem_vlan_del)(void *host, unsigned int flow, u16 vid, bool last);
	int (*gem_pbits_change)(void *host, unsigned int flow, u8 pbits,
				struct netlink_ext_ack *extack);
	void (*gem_mrouter)(void *host);
	void (*gem_rx_filter)(void *host, unsigned int flow,
			      const struct pon_gem_rx_filter *f);
	int (*gem_ds_limit)(void *host, unsigned int flow, u64 rate, u32 burst,
			    struct netlink_ext_ack *extack);
	int (*tod_arm)(void *host, const struct timespec64 *ts,
		       unsigned long delay);
	void (*second)(void *host);
};

/**
 * struct rtl9602c_pon_ops - what the MAC does for the switch driver
 * @start: the PON port is enabled: bring the MAC up
 * @stop: the PON port is disabled
 * @operstate: report the activation state as the PON port's carrier
 * @irq: the thread of the GPON interrupt
 * @is_up: whether the MAC is up
 * @sn_set: set the ONU's serial number, from its devlink string
 * @sn_get: write the serial number as that string into @buf, @len bytes,
 *	-ENODATA if none is set
 * @password_set: set the ONU's registration password
 * @password_is_set: whether one is set
 * @us_ready: whether the GEM port in @flow has its upstream programmed
 * @second_enable: whether the GPON interrupt comes once every 8000
 *	downstream frames, a second of the OLT's clock, for
 *	rtl9602c_pon_host_ops.second()
 */
struct rtl9602c_pon_ops {
	int (*start)(void *pon);
	void (*stop)(void *pon);
	void (*operstate)(void *pon);
	void (*irq)(void *pon);
	bool (*is_up)(void *pon);
	int (*sn_set)(void *pon, const char *s);
	int (*sn_get)(void *pon, char *buf, size_t len);
	int (*password_set)(void *pon, const char *s);
	bool (*password_is_set)(void *pon);
	bool (*us_ready)(void *pon, unsigned int flow);
	void (*second_enable)(void *pon, bool on);
};

/**
 * struct rtl9602c_pon_data - the platform data of the "rtl9602c.pon" device
 * @ds: the switch
 * @reg: the switch's register window, which holds the MAC's registers
 * @debugfs: the switch driver's debugfs directory, which the MAC adds its
 *	views to
 * @host: the cookie the host operations are called with
 * @host_ops: the switch driver's services
 * @optics: the firmware node of the optics the switch node names in
 *	realtek,pon-optics, which the MAC drives through the PON class; NULL
 *	when it names none
 * @irq_ns: the time of the last GPON interrupt, which the switch driver notes
 *	in the interrupt's first half
 * @sem: held for reading while the switch driver calls @pon_ops; the PON
 *	driver takes it for writing to set and to clear @pon and @pon_ops
 * @pon: the cookie the PON operations are called with, NULL while no driver
 *	is bound
 * @pon_ops: the PON driver's services, NULL while none is bound
 */
struct rtl9602c_pon_data {
	struct dsa_switch *ds;
	void __iomem *reg;
	struct dentry *debugfs;
	void *host;
	const struct rtl9602c_pon_host_ops *host_ops;
	struct fwnode_handle *optics;
	atomic64_t irq_ns;
	struct rw_semaphore sem;
	void *pon;
	const struct rtl9602c_pon_ops *pon_ops;
};

#endif /* _LINUX_PLATFORM_DATA_RTL9602C_PON_H */
