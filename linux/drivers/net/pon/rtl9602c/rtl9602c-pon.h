/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL9602C GPON MAC: what the files of the PON driver share.
 *
 * The GPON MAC (rtl9602c-gpon.c), the PON MAC behind it (rtl9602c-ponmac.c),
 * the GEM ports (rtl9602c-gem.c) and the OMCI channel (rtl9602c-omci.c) are
 * the PON driver of the switch's auxiliary device. They use the switch driver
 * through the two tables of <linux/platform_data/rtl9602c-pon.h> only, and do
 * not include rtl9602c.h.
 */

#ifndef _RTL9602C_PON_H
#define _RTL9602C_PON_H

#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dsa/rtl9602c.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_data/rtl9602c-pon.h>
#include <linux/pon.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "rtl9602c-pon-regs.h"

struct dsa_switch;
struct pon_omci_port;
struct regmap;

/**
 * struct rtl9602c_pon - the state of the GPON MAC's driver
 * @dev: the device the driver is bound to
 * @pd: the platform data, which the switch driver shares
 * @ds: the switch
 * @reg: the switch's register window
 */
struct rtl9602c_pon {
	struct device *dev;
	struct rtl9602c_pon_data *pd;
	struct dsa_switch *ds;
	void __iomem *reg;
	/** @dbg: the debugfs files the driver made in the switch's directory */
	struct dentry *dbg[12];
	/** @n_dbg: how many of @dbg are made */
	unsigned int n_dbg;

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
	 * @gpon_tod: the time of day the OLT gave last: the superframe, its
	 * TstampN, and whether the clock is still to take it
	 */
	struct {
		u32 superframe;
		struct timespec64 tstamp;
		bool armed;
	} gpon_tod;

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
	/**
	 * @gpon_second: the switch driver wants the interrupt of every second
	 * of downstream frames (rtl9602c_gpon_second_enable())
	 */
	bool gpon_second;
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
	/** @gpon_reboot_nb: sends Dying_Gasp before a restart */
	struct notifier_block gpon_reboot_nb;

	/**
	 * @sysctl: the system controller, which holds the PON MAC's gate; NULL
	 * when the PON MAC is not available
	 */
	struct regmap *sysctl;
	/** @pbo_base: physical start of each half's DRAM region */
	phys_addr_t pbo_base[2];
	/** @ponmac_up: the PON MAC is built; nothing touches it before */
	bool ponmac_up;

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
};

/**
 * RTL9602C_HOST() - call a host operation, the switch driver's service
 * @p: the PON driver
 * @op: the operation's name in struct rtl9602c_pon_host_ops
 *
 * The switch driver outlives the PON driver: it removes the device before it
 * goes, so the table is always there.
 */
#define RTL9602C_HOST(p, op, ...) ({ \
	struct rtl9602c_pon *__p = (p); \
	__p->pd->host_ops->op(__p->pd->host, ##__VA_ARGS__); \
})

/**
 * rtl9602c_pon_debugfs_file() - add a view to the switch's debugfs directory
 * @p: the PON driver
 * @name: the file's name
 * @mode: its mode
 * @fops: its operations; the file's private data is @p
 *
 * The files are in the switch driver's directory, so that their paths stay
 * as they were, and the driver removes them when it unbinds
 * (rtl9602c_pon_debugfs_exit()).
 */
static inline void rtl9602c_pon_debugfs_file(struct rtl9602c_pon *p,
					     const char *name, umode_t mode,
					     const struct file_operations *fops)
{
	struct dentry *d = debugfs_create_file(name, mode, p->pd->debugfs, p,
					       fops);

	if (p->n_dbg < ARRAY_SIZE(p->dbg))
		p->dbg[p->n_dbg++] = d;
}

/**
 * rtl9602c_pon_debugfs_exit() - remove the views the driver made
 * @p: the PON driver
 */
static inline void rtl9602c_pon_debugfs_exit(struct rtl9602c_pon *p)
{
	while (p->n_dbg)
		debugfs_remove(p->dbg[--p->n_dbg]);
}

/**
 * rtl9602c_pon_irq_ns() - the time of the last GPON interrupt
 * @p: the PON driver
 *
 * Return: when the switch driver last noted one, in nanoseconds.
 */
static inline u64 rtl9602c_pon_irq_ns(struct rtl9602c_pon *p)
{
	return atomic64_read(&p->pd->irq_ns);
}

/**
 * rtl9602c_pon_r() - read a register in the switch's window
 * @p: the PON driver
 * @off: the register's offset
 *
 * Return: its value.
 */
static inline u32 rtl9602c_pon_r(struct rtl9602c_pon *p, u32 off)
{
	return readl(p->reg + off);
}

/**
 * rtl9602c_pon_w() - write a register in the switch's window
 * @p: the PON driver
 * @off: the register's offset
 * @val: the value
 */
static inline void rtl9602c_pon_w(struct rtl9602c_pon *p, u32 off, u32 val)
{
	writel(val, p->reg + off);
}

/**
 * rtl9602c_pon_rmw() - change bits of a register in the switch's window
 * @p: the PON driver
 * @off: the register's offset
 * @clr: the bits to clear
 * @set: the bits to set after that
 */
static inline void rtl9602c_pon_rmw(struct rtl9602c_pon *p, u32 off, u32 clr,
				    u32 set)
{
	rtl9602c_pon_w(p, off, (rtl9602c_pon_r(p, off) & ~clr) | set);
}

void rtl9602c_gpon_debugfs_init(struct rtl9602c_pon *p);
void rtl9602c_gpon_cntr_sync(struct rtl9602c_pon *p);
void rtl9602c_gpon_report(struct rtl9602c_pon *p);
void rtl9602c_gpon_work(struct work_struct *w);
void rtl9602c_gpon_irq(struct rtl9602c_pon *p);
void rtl9602c_gpon_second_enable(struct rtl9602c_pon *p, bool on);
int rtl9602c_gpon_reboot_register(struct rtl9602c_pon *p);
void rtl9602c_gpon_power_applied(struct rtl9602c_pon *p, u8 asked, u8 mode,
				 int err);
int rtl9602c_ponmac_probe(struct rtl9602c_pon *p);
int rtl9602c_ponmac_init(struct rtl9602c_pon *p);
void rtl9602c_ponmac_sid_valid(struct rtl9602c_pon *p, unsigned int sid,
			       bool valid);
void rtl9602c_ponmac_debugfs_init(struct rtl9602c_pon *p);
int rtl9602c_omci_setup(struct rtl9602c_pon *p);
void rtl9602c_omci_teardown(struct rtl9602c_pon *p);
void rtl9602c_gpon_operstate(struct rtl9602c_pon *p);
void rtl9602c_gpon_fill_status(struct rtl9602c_pon *p,
			       struct pon_dev_status *st);
int rtl9602c_gpon_start(struct rtl9602c_pon *p);
int rtl9602c_gpon_sn_set(struct rtl9602c_pon *p, const char *s);
int rtl9602c_gpon_password_set(struct rtl9602c_pon *p, const char *s);
void rtl9602c_gpon_stop(struct rtl9602c_pon *p);
int rtl9602c_gpon_port_bind(struct rtl9602c_pon *p, unsigned int idx,
			    u16 port_id);
int rtl9602c_gpon_port_unbind(struct rtl9602c_pon *p, unsigned int idx);
u32 rtl9602c_gpon_port_aes(struct rtl9602c_pon *p, u16 port_id);
void rtl9602c_pon_queue_set(struct rtl9602c_pon *p, unsigned int q,
			    unsigned int weight, u32 cir, u32 pir);
int rtl9602c_pon_queue_add(struct rtl9602c_pon *p, unsigned int tcont,
			   unsigned int queue);
void rtl9602c_ponq_stream_queue_set(struct rtl9602c_pon *p, unsigned int sid,
				    unsigned int queue);
int rtl9602c_gem_setup(struct rtl9602c_pon *p);
void rtl9602c_gem_teardown(struct rtl9602c_pon *p);
void rtl9602c_gem_restore(struct rtl9602c_pon *p);
void rtl9602c_gem_tcont_ready(struct rtl9602c_pon *p, u32 tcont);
int rtl9602c_gpon_tod_set(struct rtl9602c_pon *p, u32 superframe,
			  const struct timespec64 *tstamp,
			  struct netlink_ext_ack *extack);
int rtl9602c_gpon_tcont_map(struct rtl9602c_pon *p, unsigned int n, u16 alloc,
			    struct netlink_ext_ack *extack);
bool rtl9602c_gem_us_ready(struct rtl9602c_pon *p, unsigned int flow);
int rtl9602c_gpon_port_get(struct rtl9602c_pon *p, unsigned int flow,
			   u16 *port_id, bool *valid);
u32 rtl9602c_gpon_flow_cntr(struct rtl9602c_pon *p, unsigned int flow,
			    bool bytes);
void rtl9602c_gem_cntr_sync_all(struct rtl9602c_pon *p);

/*
 * The unit of the queue rate fields: 1024 bytes per second of frames counted
 * with their FCS, with the token clock the build sets (measured on this
 * board).
 */
#define RTL9602C_GEM_RATE_UNIT		1024

int rtl9602c_gpon_sn_parse(const char *s, u8 sn[RTL9602C_GPON_SN_BYTES]);
int rtl9602c_gpon_password_parse(const char *s,
				 u8 pw[RTL9602C_GPON_PASSWORD_BYTES]);
unsigned int rtl9602c_gpon_boh_bytes_of(const u8 *ovh, u8 type3);
u8 rtl9602c_gpon_boh_len_of(const u8 *ovh, u8 type3);
void rtl9602c_gpon_eqd_split(u32 eqd, u32 *frames, u32 *bits);
u32 rtl9602c_gpon_tod_delta_ns(u32 bits);
const char *rtl9602c_ploam_ds_name(u8 id);
const char *rtl9602c_ploam_us_name(u8 id);
void rtl9602c_gem_rate_units(u64 cir, u64 pir, bool limited, u32 *cir_u,
			     u32 *pir_u);

int rtl9602c_pon_probe(struct rtl9602c_pon_data *pd, struct device *dev);
void rtl9602c_pon_remove(struct rtl9602c_pon_data *pd);

#endif /* _RTL9602C_PON_H */
