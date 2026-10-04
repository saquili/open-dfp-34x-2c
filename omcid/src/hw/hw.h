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
 *	each taken as one bit in error
 * @operating: the ONU is in operation, the state O5
 * @bip_valid: @bip_errors was read
 * @power_level: the level the transmitter is at, as Serial_Number_ONU
 *	codes it (G.984.3 9.2.4.1): 2 high, 1 medium, 0 low
 * @power_level_valid: @power_level was read
 */
struct omci_hw_pon {
	uint64_t bip_errors;
	bool operating;
	bool bip_valid;
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

void omci_hw_pon_fail_next(bool fail);
#endif

/**
 * omci_hw_counter() - Read a counter of the switch (host backend).
 * @port: the switch port
 * @idx: the counter, in the order the switch keeps them
 * @out: its value
 *
 * Return: %OMCI_OK, or the failure of the read.
 */
__must_check enum omci_status omci_hw_counter(unsigned int port,
					      unsigned int idx, uint64_t *out);

#if defined(OMCID_HW_STUB)

/* Test hooks of the host backend for counters. */
void omci_hw_counter_force(unsigned int port, unsigned int idx, uint64_t value);

void omci_hw_counter_fail_next(bool fail);
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
 * @counter: read a counter of the switch
 * @optics: read the transceiver
 * @optics_limits: read the transceiver's thresholds; NULL in a backend
 *	without them
 * @set_router: route instead of bridging (true), or bridge again (false):
 *	in router mode the bridges and the VLAN rules are not applied; NULL in
 *	a backend without a router mode
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
	enum omci_status (*counter)(unsigned int port, unsigned int idx,
				    uint64_t *out);
	enum omci_status (*optics)(struct omci_hw_optics *out);
	enum omci_status (*optics_limits)(struct omci_hw_optics_limits *out);
	void (*set_router)(bool on);
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
