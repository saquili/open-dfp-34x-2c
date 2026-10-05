/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The daemon's view of the hardware: what the device has, and the few
 * things the daemon asks of it: VLAN rules, GEM ports and bridges to
 * program, alarms, counters, the state of the PON receiver and optical
 * readings to read. A backend fills
 * in struct omci_hw_ops; the functions declared here are the host backend,
 * which tests use.
 */

#ifndef OMCI_HW_H
#define OMCI_HW_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omcid/attrs.h"
#include "omcid/status.h"

/**
 * struct omci_hw_port - One slot of the device.
 * @pptp_instance: the instance of its Ethernet UNI; 0 for the PON side
 * @cardholder: the instance of its cardholder and circuit pack
 * @plugin_type: the plug-in unit type its equipment reports
 */
struct omci_hw_port {
	alignas(4) uint16_t pptp_instance;
	uint16_t cardholder;
	uint8_t plugin_type;
};

/**
 * struct omci_hw_caps - What the device has and how it identifies itself.
 * @ports: its slots
 * @port_count: how many there are
 * @ani_instance: the instance of the ANI-G
 * @ip_host_count: the IP hosts
 * @sw_committed_image: the software image that is committed (0 or 1)
 * @num_gem_ports: the GEM ports the PON MAC can carry
 * @identity: the identity of the ONU: vendor ID, version, serial number,
 *	traffic management option, equipment ID, OMCC version,
 *	vendor product code, versions of the two software images, MAC address
 * @num_tcont: the T-CONTs
 * @num_priq_per_tcont: the upstream priority queues of each T-CONT
 * @num_priq_per_port: the downstream priority queues of each UNI
 * @uni_port_count: the subscriber ports
 */
struct omci_hw_caps {
	struct omci_hw_port ports[8];
	uint8_t port_count;

	uint16_t ani_instance;
	uint8_t ip_host_count;
	uint8_t sw_committed_image;

	uint16_t num_gem_ports;

	struct {
		char vendor_id[5];
		char version[15];

		uint8_t serial[8];
		uint8_t tm_option;

		char equipment_id[21];
		uint8_t omcc_version;
		uint16_t vendor_product_code;

		char sw_version[2][15];

		uint8_t mac[6];
	} identity;

	uint8_t num_tcont;
	uint8_t num_priq_per_tcont;
	uint8_t num_priq_per_port;

	uint8_t uni_port_count;
};

/**
 * omci_hw_caps() - What the host backend has.
 * @out: a fixed, synthetic set
 *
 * Return: %OMCI_OK, or %OMCI_ERR_INVAL without @out.
 */
__must_check enum omci_status omci_hw_caps(struct omci_hw_caps *out);

/**
 * omci_hw_source() - Name the backend, for a log line.
 *
 * Return: a constant string.
 */
__must_check const char *omci_hw_source(void);

/* The items a programming request may refuse in part. */
#define OMCI_HW_REJECT_MAX 32u

/**
 * struct omci_hw_reject - The items the hardware refused.
 * @id: the VID or port identifier of each
 * @n: how many there are
 */
struct omci_hw_reject {
	uint16_t id[OMCI_HW_REJECT_MAX];
	uint8_t n;
};

/* Add an item to the refused ones, once. */
static inline void omci_hw_reject_add(struct omci_hw_reject *r, uint16_t id)
{
	if (r == NULL)
		return;
	for (uint8_t i = 0; i < r->n; i++)
		if (r->id[i] == id)
			return;
	if (r->n < OMCI_HW_REJECT_MAX)
		r->id[r->n++] = id;
}

/* True when the hardware refused the item. */
static inline bool omci_hw_reject_has(const struct omci_hw_reject *r,
				      uint16_t id)
{
	if (r == NULL)
		return false;
	for (uint8_t i = 0; i < r->n; i++)
		if (r->id[i] == id)
			return true;
	return false;
}

/* The VLAN rules the classifier can hold. */
#define OMCI_HW_VLAN_MAX_RULES 16u

/**
 * enum omci_hw_vlan_kind - The VLAN rules the hardware carries.
 * @OMCI_HW_VLAN_CVID_XLATE: replace the VID of a single-tagged frame
 * @OMCI_HW_VLAN_SVID_PUSH: add an outer tag
 * @OMCI_HW_VLAN_CVID_PUSH: give an untagged frame a tag
 */
enum omci_hw_vlan_kind {
	OMCI_HW_VLAN_CVID_XLATE = 0,
	OMCI_HW_VLAN_SVID_PUSH,
	OMCI_HW_VLAN_CVID_PUSH,
};

/*
 * The priority a rule gives the tag it writes, besides 0 to 7: the one the tag
 * had (KEEP), or for an added outer tag that of the tag behind it (COPY).
 */
#define OMCI_HW_VLAN_PRI_KEEP 0xFFu
#define OMCI_HW_VLAN_PRI_COPY 0xFEu

/**
 * struct omci_hw_vlan_xlate - A VLAN rule, as the subscriber side sees it.
 * @kind: what it does
 * @match_cvid: the VID of the frames it takes from the subscriber side; 0,
 *	which no rule's VID can be, for the untagged frames of a C-VID push
 * @assign_cvid: the VID they leave with, for a translation or a C-VID push
 * @assign_svid: the VID of the added tag, for an S-VID push
 * @assign_pri: the priority of the tag the rule writes, 0 to 7,
 *	%OMCI_HW_VLAN_PRI_KEEP or %OMCI_HW_VLAN_PRI_COPY
 * @match_has_pri: the rule takes only the tagged frames of priority
 *	@match_pri; without it, those of any priority
 * @match_pri: that priority, 0 to 7
 * @up_only: the rule works upstream only: an earlier rule gives frames the
 *	same tag toward the network, and the way back downstream is that
 *	rule's (G.988 9.3.13, many-to-one mappings)
 */
struct omci_hw_vlan_xlate {
	enum omci_hw_vlan_kind kind;
	uint16_t match_cvid;
	uint16_t assign_cvid;
	uint16_t assign_svid;
	uint8_t assign_pri;
	bool match_has_pri;
	uint8_t match_pri;
	bool up_only;
};

/**
 * omci_hw_vlan_apply() - Replace the VLAN rules of the hardware (host backend).
 * @rules: the complete set
 * @n: how many
 * @rejected: the rules the hardware refused, by matched VID
 *
 * Return: %OMCI_OK; %OMCI_DECLINED when some rules were refused; another
 * error when none could be programmed.
 */
__must_check enum omci_status
omci_hw_vlan_apply(const struct omci_hw_vlan_xlate *rules, size_t n,
		   struct omci_hw_reject *rejected);

#if defined(OMCID_HW_STUB)

/*
 * Test hooks of the host backend: what was last programmed, and failures
 * or refusals to give on the next call.
 */
size_t omci_hw_vlan_recorded(const struct omci_hw_vlan_xlate **out);

void omci_hw_vlan_fail_next(void);

void omci_hw_vlan_reject_next(uint16_t match_cvid);
#endif

/* The GEM ports the PON MAC can carry. */
#define OMCI_HW_GEM_MAX_PORTS 32u

/**
 * enum omci_hw_gem_dir - The directions of a GEM port (G.988 9.2.3).
 * @OMCI_HW_GEM_DIR_UPSTREAM: from the ONU to the OLT
 * @OMCI_HW_GEM_DIR_DOWNSTREAM: from the OLT to the ONU
 * @OMCI_HW_GEM_DIR_BOTH: both ways
 */
enum omci_hw_gem_dir {
	OMCI_HW_GEM_DIR_UPSTREAM = 1,
	OMCI_HW_GEM_DIR_DOWNSTREAM = 2,
	OMCI_HW_GEM_DIR_BOTH = 3,
};

/**
 * struct omci_hw_gem_port - A GEM port.
 * @port_id: its port identifier
 * @direction: an &enum omci_hw_gem_dir
 * @tcont: the T-CONT that carries it upstream, from 0
 * @multicast: whether it carries multicast downstream
 * @pbits: the IEEE 802.1p priorities of its VLANs' upstream frames it takes,
 *	bit p for priority p; 0xff for all
 * @queue: its upstream queue in the T-CONT, 0 the highest priority
 * @weight: 0 to schedule the queue by strict priority, else its weight
 * @cir: the committed rate of its upstream traffic, in bytes per second; 0
 *	for none
 * @pir: the peak rate of its upstream traffic, in bytes per second; 0 for no
 *	limit
 *
 * The last five apply to a port that carries traffic upstream; a port that
 * does not has 0xff and zeros.
 */
struct omci_hw_gem_port {
	uint16_t port_id;
	uint8_t direction;
	uint8_t tcont;

	bool multicast;
	uint8_t pbits;
	uint8_t queue;
	uint8_t weight;
	uint32_t cir;
	uint32_t pir;
};

/**
 * omci_hw_gem_apply() - Replace the GEM ports of the PON MAC (host backend).
 * @ports: the complete set
 * @n: how many
 * @rejected: the ports the hardware refused, by port identifier
 *
 * Return: as omci_hw_vlan_apply().
 */
__must_check enum omci_status
omci_hw_gem_apply(const struct omci_hw_gem_port *ports, size_t n,
		  struct omci_hw_reject *rejected);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for GEM ports. */
size_t omci_hw_gem_recorded(const struct omci_hw_gem_port **out);

void omci_hw_gem_fail_next(void);

void omci_hw_gem_reject_next(uint16_t port_id);
#endif

/* The T-CONTs a mapping names at most, and a T-CONT no Alloc-ID serves. */
#define OMCI_HW_TCONT_MAX 32u
#define OMCI_HW_ALLOC_NONE 0xffffu

/**
 * omci_hw_tcont_apply() - Map the T-CONTs to Alloc-IDs (host backend).
 * @alloc: the Alloc-ID of each T-CONT, from T-CONT 0; %OMCI_HW_ALLOC_NONE
 *	for a T-CONT no Alloc-ID serves
 * @n: how many T-CONTs, at most %OMCI_HW_TCONT_MAX
 * @rejected: the T-CONTs the hardware refused, by index
 *
 * Return: as omci_hw_vlan_apply().
 */
__must_check enum omci_status
omci_hw_tcont_apply(const uint16_t *alloc, size_t n,
		    struct omci_hw_reject *rejected);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for T-CONTs. */
size_t omci_hw_tcont_recorded(const uint16_t **out);

void omci_hw_tcont_fail_next(void);

void omci_hw_tcont_reject_next(uint16_t index);
#endif

/* The bridge ports, and the VIDs each may carry. */
#define OMCI_HW_BRIDGE_MAX_PORTS 16u
#define OMCI_HW_BRIDGE_MAX_VLANS 12u

/* The port identifier that stands for the subscriber side. */
#define OMCI_HW_BRIDGE_UNI 0xffffu

/**
 * struct omci_hw_bridge_port - A port of a bridge.
 * @bridge: the bridge it belongs to
 * @port_id: a GEM port identifier, or %OMCI_HW_BRIDGE_UNI
 * @vlan: the VIDs it carries
 * @num_vlan: how many; 0 for all
 */
struct omci_hw_bridge_port {
	uint16_t bridge;
	uint16_t port_id;
	uint16_t vlan[OMCI_HW_BRIDGE_MAX_VLANS];
	uint8_t num_vlan;
};

/**
 * omci_hw_bridge_apply() - Replace the bridges of the switch (host backend).
 * @ports: the complete set of ports
 * @n: how many
 * @rejected: the ports the hardware refused, by port identifier
 *
 * Return: as omci_hw_vlan_apply().
 */
__must_check enum omci_status
omci_hw_bridge_apply(const struct omci_hw_bridge_port *ports, size_t n,
		     struct omci_hw_reject *rejected);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for bridges. */
size_t omci_hw_bridge_recorded(const struct omci_hw_bridge_port **out);

void omci_hw_bridge_fail_next(void);

void omci_hw_bridge_reject_next(uint16_t port_id);
#endif

/**
 * enum omci_hw_alarm - The alarms of the PON side, one bit each.
 * @OMCI_HW_ALARM_LOS: loss of signal
 * @OMCI_HW_ALARM_LOF: loss of frame
 * @OMCI_HW_ALARM_LOM: loss of the message channel
 * @OMCI_HW_ALARM_SF: signal fail
 * @OMCI_HW_ALARM_SD: signal degrade
 * @OMCI_HW_ALARM_TX_TOO_LONG: the transmitter stayed on too long
 * @OMCI_HW_ALARM_TX_MISMATCH: the transmitter was on out of turn
 * @OMCI_HW_ALARM_COUNT: the number of alarms
 */
enum omci_hw_alarm {
	OMCI_HW_ALARM_LOS = 0,
	OMCI_HW_ALARM_LOF,
	OMCI_HW_ALARM_LOM,
	OMCI_HW_ALARM_SF,
	OMCI_HW_ALARM_SD,
	OMCI_HW_ALARM_TX_TOO_LONG,
	OMCI_HW_ALARM_TX_MISMATCH,
	OMCI_HW_ALARM_COUNT,
};

/**
 * omci_hw_alarms() - Read the alarms (host backend).
 * @mask: bit N set while &enum omci_hw_alarm N stands
 *
 * Return: %OMCI_OK, or the failure of the read.
 */
__must_check enum omci_status omci_hw_alarms(uint32_t *mask);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for alarms. */
void omci_hw_alarm_force(enum omci_hw_alarm alarm, bool asserted);

void omci_hw_alarm_fail_next(bool fail);
#endif

/**
 * struct omci_hw_pon - The state of the PON receiver.
 * @bip_errors: the downstream BIP errors counted since the receiver came up,
 *	in bits
 * @fec_corrected_bytes: the downstream bytes FEC corrected since the
 *	receiver came up
 * @fec_corrected_codewords: the downstream codewords FEC corrected, likewise
 * @fec_uncorrectable_codewords: the downstream codewords FEC could not
 *	correct, likewise
 * @fec_codewords: the downstream codewords received while the downstream
 *	carried FEC, likewise
 * @operating: the ONU is in operation, the state O5
 * @bip_valid: @bip_errors was read
 * @fec_valid: the four FEC counts were read
 * @power_level: the level the transmitter is at, as Serial_Number_ONU
 *	codes it (G.984.3 9.2.4.1): 2 high, 1 medium, 0 low
 * @power_level_valid: @power_level was read
 */
struct omci_hw_pon {
	uint64_t bip_errors;
	uint64_t fec_corrected_bytes;
	uint64_t fec_corrected_codewords;
	uint64_t fec_uncorrectable_codewords;
	uint64_t fec_codewords;
	bool operating;
	bool bip_valid;
	bool fec_valid;
	uint8_t power_level;
	bool power_level_valid;
};

/**
 * omci_hw_pon() - Read the state of the PON receiver (host backend).
 * @out: the state
 *
 * Return: %OMCI_OK, or the failure of the read.
 */
__must_check enum omci_status omci_hw_pon(struct omci_hw_pon *out);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for the PON receiver. */
void omci_hw_pon_force(bool operating, bool bip_valid, uint64_t bip_errors);

void omci_hw_pon_level_force(bool valid, uint8_t level);

void omci_hw_pon_fec_force(bool valid, uint64_t corrected_bytes,
			   uint64_t corrected_codewords,
			   uint64_t uncorrectable_codewords,
			   uint64_t codewords);

void omci_hw_pon_fail_next(bool fail);
#endif

/* The counters of a switch port, numbered in the order the switch keeps them. */
#define OMCI_HW_PORT_COUNTERS 70u

/**
 * omci_hw_port_counters() - Read every counter of a switch port (host
 *	backend).
 * @port: the switch port
 * @out: the counters, by their number; one the switch does not keep is 0
 *
 * Return: %OMCI_OK, or the failure of the read.
 */
__must_check enum omci_status
omci_hw_port_counters(unsigned int port, uint64_t out[OMCI_HW_PORT_COUNTERS]);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for counters. */
void omci_hw_counter_force(unsigned int port, unsigned int idx, uint64_t value);

void omci_hw_counter_fail_next(bool fail);
#endif

/**
 * struct omci_hw_gem_counts - What a GEM port has counted since it was made.
 * @tx_frames: the GEM frames it sent, a fragment counting as one
 * @rx_frames: the GEM frames it received, likewise
 * @tx_payload: the payload bytes it sent, the GEM headers left out
 * @rx_payload: the payload bytes it received, likewise
 */
struct omci_hw_gem_counts {
	uint64_t tx_frames;
	uint64_t rx_frames;
	uint64_t tx_payload;
	uint64_t rx_payload;
};

/**
 * omci_hw_gem_counts() - Read the counts of a GEM port (host backend).
 * @port_id: the GEM port
 * @out: its counts
 *
 * Return: %OMCI_OK; %OMCI_ERR_HW when there is no such port or it could not
 * be read.
 */
__must_check enum omci_status
omci_hw_gem_counts(uint16_t port_id, struct omci_hw_gem_counts *out);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for the counts of the GEM ports. */
void omci_hw_gem_counts_force(uint16_t port_id,
			      const struct omci_hw_gem_counts *counts);

void omci_hw_gem_counts_reset(void);
#endif

/**
 * enum omci_hw_optic - The readings of the transceiver.
 * @OMCI_HW_OPTIC_TEMPERATURE: temperature
 * @OMCI_HW_OPTIC_VOLTAGE: supply voltage
 * @OMCI_HW_OPTIC_BIAS: laser bias current
 * @OMCI_HW_OPTIC_TX_POWER: transmitted optical power
 * @OMCI_HW_OPTIC_RX_POWER: received optical power
 * @OMCI_HW_OPTIC_COUNT: the number of readings
 */
enum omci_hw_optic {
	OMCI_HW_OPTIC_TEMPERATURE = 0,
	OMCI_HW_OPTIC_VOLTAGE,
	OMCI_HW_OPTIC_BIAS,
	OMCI_HW_OPTIC_TX_POWER,
	OMCI_HW_OPTIC_RX_POWER,
	OMCI_HW_OPTIC_COUNT,
};

/**
 * struct omci_hw_optics - The readings, in the units of SFF-8472.
 * @temperature: 1/256 degree Celsius, two's complement
 * @voltage: 0.1 mV
 * @bias: 2 uA
 * @tx_power: 0.1 uW
 * @rx_power: 0.1 uW
 * @valid: bit N set when &enum omci_hw_optic N was read
 */
struct omci_hw_optics {
	uint16_t temperature;
	uint16_t voltage;
	uint16_t bias;
	uint16_t tx_power;
	uint16_t rx_power;
	uint8_t valid;
};

/**
 * omci_hw_optics() - Read the transceiver (host backend).
 * @out: the readings
 *
 * Return: %OMCI_OK, or the failure of the read.
 */
__must_check enum omci_status omci_hw_optics(struct omci_hw_optics *out);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for optical readings. */
void omci_hw_optics_force(enum omci_hw_optic which, bool present, uint16_t raw);

void omci_hw_optics_reset(void);
#endif

/**
 * enum omci_hw_limit - The alarm thresholds of the transceiver.
 * @OMCI_HW_LIMIT_BIAS_HIGH: laser bias current, high
 * @OMCI_HW_LIMIT_TX_LOW: transmitted optical power, low
 * @OMCI_HW_LIMIT_TX_HIGH: transmitted optical power, high
 * @OMCI_HW_LIMIT_RX_LOW: received optical power, low
 * @OMCI_HW_LIMIT_RX_HIGH: received optical power, high
 * @OMCI_HW_LIMIT_COUNT: the number of thresholds
 */
enum omci_hw_limit {
	OMCI_HW_LIMIT_BIAS_HIGH = 0,
	OMCI_HW_LIMIT_TX_LOW,
	OMCI_HW_LIMIT_TX_HIGH,
	OMCI_HW_LIMIT_RX_LOW,
	OMCI_HW_LIMIT_RX_HIGH,
	OMCI_HW_LIMIT_COUNT,
};

/**
 * struct omci_hw_optics_limits - The transceiver's own alarm thresholds.
 * @value: each threshold, in the unit of its reading in struct
 *	omci_hw_optics (2 uA, 0.1 uW)
 * @valid: bit N set when &enum omci_hw_limit N was read
 */
struct omci_hw_optics_limits {
	uint16_t value[OMCI_HW_LIMIT_COUNT];
	uint8_t valid;
};

/**
 * omci_hw_optics_limits() - Read the thresholds of the transceiver (host
 *	backend).
 * @out: the thresholds
 *
 * Return: %OMCI_OK, or the failure of the read.
 */
__must_check enum omci_status
omci_hw_optics_limits(struct omci_hw_optics_limits *out);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for the thresholds of the transceiver. */
void omci_hw_optics_limit_force(enum omci_hw_limit which, bool present,
				uint16_t raw);

void omci_hw_optics_limits_reset(void);
#endif

/*
 * The groups of the subscriber side a backend lists at once, the longest
 * IGMP or MLD message it hands over, and the size of an address: IPv6, or
 * IPv4 in the IPv4-mapped form ::ffff:a.b.c.d.
 */
#define OMCI_HW_MCAST_MAX_GROUPS 64u
#define OMCI_HW_MCAST_FRAME_MAX 512u
#define OMCI_HW_MCAST_ADDR_LEN 16u

/**
 * struct omci_hw_mcast_group - A multicast group the bridge forwards to the
 *	subscriber side.
 * @vid: its VLAN: of a group the bridge holds, the one it holds the group
 *	in, 0 when it filters no VLAN; of a static group to add, the one that
 *	carries the group downstream, which a bridge that filters no VLAN does
 *	without
 * @group: the group
 * @source: its source, all zero for every source
 * @permanent: a static group, which no join or leave ends
 */
struct omci_hw_mcast_group {
	uint16_t vid;
	uint8_t group[OMCI_HW_MCAST_ADDR_LEN];
	uint8_t source[OMCI_HW_MCAST_ADDR_LEN];
	bool permanent;
};

/**
 * struct omci_hw_mcast_port - How the bridge snoops the subscriber side.
 * @fast_leave: a leave ends the group at once, without a query after it
 * @robustness: the robustness variable (RFC 3376 8.1); 0 for the bridge's
 *	own, 2
 * @query_interval: the time between general queries, in seconds; 0 for the
 *	bridge's own, 125
 * @query_response: the most time to answer a general query, in units of
 *	0.1 s; 0 for the bridge's own, 100
 * @last_member: the most time to answer a group's query, in units of
 *	0.1 s; 0 for the bridge's own, 10
 * @trap: the IGMP and MLD messages of the subscriber side go to the CPU
 *	only, which the bridge snoops but does not forward, for the agent
 *	to send upstream itself (mcast_send); otherwise the switch forwards
 *	them and the CPU sees a copy
 */
struct omci_hw_mcast_port {
	bool fast_leave;
	uint8_t robustness;
	uint32_t query_interval;
	uint32_t query_response;
	uint32_t last_member;
	bool trap;
};

/**
 * struct omci_hw_mcast_frame - An IGMP or MLD message of the subscriber side.
 * @tagged: the receive path took a VLAN tag off the frame
 * @tci: that tag
 * @len: the bytes in @data
 * @data: the frame from its destination address, cut at
 *	%OMCI_HW_MCAST_FRAME_MAX bytes
 */
struct omci_hw_mcast_frame {
	bool tagged;
	uint16_t tci;
	uint16_t len;
	uint8_t data[OMCI_HW_MCAST_FRAME_MAX];
};

/**
 * enum omci_hw_mcast_ds_action - What the downstream IGMP and multicast TCI
 *	of a profile (G.988 9.3.27) does to the frames of a multicast VLAN.
 * @OMCI_HW_MCAST_DS_STRIP: the outer tag comes off
 * @OMCI_HW_MCAST_DS_ADD: a tag goes on in front, an S-tag (802.1ad) as the
 *	switch adds one in front of a C-tag
 * @OMCI_HW_MCAST_DS_REPLACE: the outer tag is written over
 * @OMCI_HW_MCAST_DS_REPLACE_VID: the outer tag's VID is written over, its
 *	priority kept
 */
enum omci_hw_mcast_ds_action {
	OMCI_HW_MCAST_DS_STRIP = 1,
	OMCI_HW_MCAST_DS_ADD,
	OMCI_HW_MCAST_DS_REPLACE,
	OMCI_HW_MCAST_DS_REPLACE_VID,
};

/* The multicast VLANs that can have a downstream tag at once. */
#define OMCI_HW_MCAST_DS_MAX 8u

/**
 * struct omci_hw_mcast_ds - The downstream tag of a multicast VLAN.
 * @vid: the VLAN the groups come down in, a C-tag of 1 to 4094
 * @action: an &enum omci_hw_mcast_ds_action
 * @tci: the tag added or written; of %OMCI_HW_MCAST_DS_REPLACE_VID the VID
 *	alone counts
 */
struct omci_hw_mcast_ds {
	uint16_t vid;
	uint8_t action;
	uint16_t tci;
};

/**
 * omci_hw_mcast_ds() - Replace the downstream tags of the multicast VLANs
 *	(host backend).
 * @rules: the complete set
 * @n: how many, at most %OMCI_HW_MCAST_DS_MAX
 *
 * Return: %OMCI_OK, %OMCI_ERR_NOMEM for too many, or the failure of the
 * change.
 */
__must_check enum omci_status
omci_hw_mcast_ds(const struct omci_hw_mcast_ds *rules, size_t n);

/* The groups whose downstream frames a backend counts at once. */
#define OMCI_HW_MCAST_WATCH_MAX 16u

/**
 * omci_hw_mcast_watch() - Count the downstream frames of a group, or stop
 *	(host backend).
 * @group: the group
 * @on: count (true) or stop
 *
 * Return: %OMCI_OK, %OMCI_ERR_NOMEM when no counter is free, or the
 * failure of the change.
 */
__must_check enum omci_status
omci_hw_mcast_watch(const uint8_t group[static OMCI_HW_MCAST_ADDR_LEN],
		    bool on);

/**
 * omci_hw_mcast_seen() - The downstream frames of a counted group so far
 *	(host backend).
 * @group: the group
 * @frames: how many came
 *
 * Return: %OMCI_OK, %OMCI_DECLINED for a group not counted, or the failure
 * of the read.
 */
__must_check enum omci_status
omci_hw_mcast_seen(const uint8_t group[static OMCI_HW_MCAST_ADDR_LEN],
		   uint64_t *frames);

/**
 * omci_hw_mcast_port() - Set how the bridge snoops (host backend).
 * @cfg: the settings
 *
 * Return: %OMCI_OK, or %OMCI_ERR_INVAL without @cfg.
 */
__must_check enum omci_status
omci_hw_mcast_port(const struct omci_hw_mcast_port *cfg);

/**
 * omci_hw_mcast_groups() - List the groups of the subscriber side (host
 *	backend).
 * @out: the groups
 * @cap: room in @out
 * @n: how many were written
 *
 * Return: %OMCI_OK, or the failure of the read.
 */
__must_check enum omci_status
omci_hw_mcast_groups(struct omci_hw_mcast_group *out, size_t cap, size_t *n);

/**
 * omci_hw_mcast_group_add() - Add a static group (host backend).
 * @g: the group
 *
 * Return: %OMCI_OK, %OMCI_ERR_NOMEM when the groups are full, or the
 * failure of the change.
 */
__must_check enum omci_status
omci_hw_mcast_group_add(const struct omci_hw_mcast_group *g);

/**
 * omci_hw_mcast_group_del() - Remove a group, static or not (host backend).
 * @g: the group, by its VLAN, group and source
 *
 * Return: %OMCI_OK, %OMCI_DECLINED for a group that is not there, or the
 * failure of the change.
 */
__must_check enum omci_status
omci_hw_mcast_group_del(const struct omci_hw_mcast_group *g);

/**
 * omci_hw_mcast_frame() - Take the next IGMP or MLD message (host backend).
 * @out: the message
 *
 * Return: %OMCI_OK with one, %OMCI_DECLINED when none waits.
 */
__must_check enum omci_status
omci_hw_mcast_frame(struct omci_hw_mcast_frame *out);

/**
 * omci_hw_mcast_send() - Send an IGMP or MLD message upstream (host
 *	backend).
 * @port_id: the GEM port that carries it
 * @frame: the frame, from its destination address, with its tags
 * @len: its length
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL for a frame that is too long, or the
 * failure of the send.
 */
__must_check enum omci_status
omci_hw_mcast_send(uint16_t port_id, const uint8_t *frame, size_t len);

#if defined(OMCID_HW_STUB)

/*
 * Test hooks of the host backend for multicast: a group the bridge snoops,
 * joined or left; a message from the subscriber side to hand over; what the
 * port was last set to, and how often; failures of the next list, add,
 * delete and send; the messages sent upstream, the last first, with their
 * GEM port (0 for none); and an empty start.
 */
void omci_hw_mcast_snoop(const struct omci_hw_mcast_group *g, bool join);

void omci_hw_mcast_receive(const uint8_t *frame, size_t len, bool tagged,
			   uint16_t tci);

unsigned int omci_hw_mcast_port_recorded(struct omci_hw_mcast_port *out);

void omci_hw_mcast_fail_next(bool list, bool add, bool del);

void omci_hw_mcast_send_fail_next(void);

void omci_hw_mcast_ds_fail_next(void);

void omci_hw_mcast_flow(const uint8_t group[static OMCI_HW_MCAST_ADDR_LEN],
			uint64_t frames);

void omci_hw_mcast_watch_fail_next(bool watch, bool seen);

size_t omci_hw_mcast_ds_recorded(const struct omci_hw_mcast_ds **out);

size_t omci_hw_mcast_sent(size_t index, uint16_t *port_id, uint8_t *frame,
			  size_t cap);

void omci_hw_mcast_reset(void);
#endif

/**
 * struct omci_hw_ops - A backend.
 * @caps: what the device has
 * @source: the backend's name
 * @vlan_apply: replace the VLAN rules
 * @gem_apply: replace the GEM ports
 * @tcont_apply: map the T-CONTs to Alloc-IDs; NULL in a backend whose
 *	hardware binds the Alloc-IDs itself
 * @bridge_apply: replace the bridges
 * @alarms: read the alarms
 * @pon: read the state of the PON receiver
 * @port_counters: read every counter of a switch port
 * @gem_counts: read the counts of a GEM port; NULL in a backend without them
 * @optics: read the transceiver
 * @optics_limits: read the transceiver's thresholds; NULL in a backend
 *	without them
 * @set_router: route instead of bridging (true), or bridge again (false):
 *	in router mode the bridges and the VLAN rules are not applied; NULL in
 *	a backend without a router mode
 * @mcast_port: set how the bridge snoops the subscriber side; NULL in a
 *	backend without multicast, which then has none of the mcast_
 *	operations
 * @mcast_groups: list the groups the bridge forwards to the subscriber side
 * @mcast_group_add: add a static group
 * @mcast_group_del: remove a group
 * @mcast_fd: a descriptor that becomes readable when a group changes or a
 *	message comes, or -1 for none: the daemon then looks once a second
 * @mcast_frame: take the next IGMP or MLD message of the subscriber side
 * @mcast_send: send an IGMP or MLD message upstream through a GEM port;
 *	NULL in a backend that cannot, whose agent leaves the messages to the
 *	switch
 * @mcast_ds: replace the downstream tags of the multicast VLANs; NULL in a
 *	backend without them
 * @mcast_watch: count the downstream frames of a group, or stop; NULL in a
 *	backend that cannot, where no group is ever lost
 * @mcast_seen: the downstream frames of a counted group so far
 */
struct omci_hw_ops {
	enum omci_status (*caps)(struct omci_hw_caps *out);
	const char *(*source)(void);
	enum omci_status (*vlan_apply)(const struct omci_hw_vlan_xlate *rules,
				       size_t n,
				       struct omci_hw_reject *rejected);
	enum omci_status (*gem_apply)(const struct omci_hw_gem_port *ports,
				      size_t n,
				      struct omci_hw_reject *rejected);
	enum omci_status (*tcont_apply)(const uint16_t *alloc, size_t n,
					struct omci_hw_reject *rejected);
	enum omci_status (*bridge_apply)(const struct omci_hw_bridge_port *ports,
					 size_t n,
					 struct omci_hw_reject *rejected);
	enum omci_status (*alarms)(uint32_t *mask);
	enum omci_status (*pon)(struct omci_hw_pon *out);
	enum omci_status (*port_counters)(unsigned int port,
					  uint64_t out[OMCI_HW_PORT_COUNTERS]);
	enum omci_status (*gem_counts)(uint16_t port_id,
				       struct omci_hw_gem_counts *out);
	enum omci_status (*optics)(struct omci_hw_optics *out);
	enum omci_status (*optics_limits)(struct omci_hw_optics_limits *out);
	void (*set_router)(bool on);
	enum omci_status (*mcast_port)(const struct omci_hw_mcast_port *cfg);
	enum omci_status (*mcast_groups)(struct omci_hw_mcast_group *out,
					 size_t cap, size_t *n);
	enum omci_status (*mcast_group_add)(const struct omci_hw_mcast_group *g);
	enum omci_status (*mcast_group_del)(const struct omci_hw_mcast_group *g);
	int (*mcast_fd)(void);
	enum omci_status (*mcast_frame)(struct omci_hw_mcast_frame *out);
	enum omci_status (*mcast_send)(uint16_t port_id, const uint8_t *frame,
				       size_t len);
	enum omci_status (*mcast_ds)(const struct omci_hw_mcast_ds *rules,
				     size_t n);
	enum omci_status (*mcast_watch)(const uint8_t *group, bool on);
	enum omci_status (*mcast_seen)(const uint8_t *group, uint64_t *frames);
};

/* The host backend, and the backend of the kernel interfaces of the board. */
extern const struct omci_hw_ops omci_hw_host_ops;
extern const struct omci_hw_ops omci_hw_kernel_ops;

/**
 * omci_hw() - The backend in use.
 *
 * Until omci_hw_ops_set() chooses another, the backend of the build: the
 * host backend in a host build, the kernel backend on the board.
 *
 * Return: the backend.
 */
__must_check const struct omci_hw_ops *omci_hw(void);

/**
 * omci_hw_ops_set() - Choose the backend.
 * @ops: the backend, or NULL for the backend of the build
 */
void omci_hw_ops_set(const struct omci_hw_ops *ops);

#endif
