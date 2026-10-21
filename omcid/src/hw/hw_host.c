// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The host backend: a synthetic device for tests and for running the daemon
 * away from the board. It reports a fixed set of capabilities, keeps what
 * it was asked to program, and has hooks that make its next answer fail or
 * refuse, or that set what it reads.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hw/hw.h"
#include "me/svcrule.h"
#include "sys/log.h"
#include "sys/wire.h"

/*
 * One UNI and the PON side, four T-CONTs. The identity names no real maker,
 * and the MAC address is from the range RFC 7042 sets aside for documents.
 */
static void synthetic_caps(struct omci_hw_caps *out)
{
	omci_wire_fill(out, 0, sizeof(*out));

	out->ports[0] = (struct omci_hw_port){ .pptp_instance = 0x0101,
					       .cardholder = 0x0101,
					       .plugin_type = 0x2f };
	out->ports[1] = (struct omci_hw_port){ .pptp_instance = 0,
					       .cardholder = 0x0180,
					       .plugin_type = 0xf8 };
	out->port_count = 2;
	out->ani_instance = 0x8001;
	out->ip_host_count = 1;
	out->sw_committed_image = 0;
	out->sw_valid_images = 1;
	out->num_gem_ports = 32;

	omci_wire_copy(out->identity.vendor_id, "ZZZZ", 5);
	omci_wire_copy(out->identity.version, "0.1-synthetic", 14);
	omci_wire_copy(out->identity.equipment_id, "SYNTHETIC-ONU", 14);
	omci_wire_copy(out->identity.sw_version[0], "0.1.0-synth", 12);
	omci_wire_copy(out->identity.sw_version[1], "0.0.0-empty", 12);
	omci_wire_copy(out->identity.loid, "synthetic-loid", 15);

	omci_wire_copy(out->identity.serial, out->identity.vendor_id, 4);
	out->identity.serial[7] = 1;

	out->identity.tm_option = 0;
	out->identity.omcc_version = 0xa0;
	out->identity.vendor_product_code = 1;

	const uint8_t mac[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 };

	omci_wire_copy(out->identity.mac, mac, sizeof(mac));

	out->num_tcont = 4;
	out->num_priq_per_tcont = 2;
	out->num_priq_per_port = 2;
	out->uni_port_count = 1;
}

enum omci_status omci_hw_caps(struct omci_hw_caps *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;

	synthetic_caps(out);
	return OMCI_OK;
}

static struct omci_hw_vlan_xlate g_vlan[OMCI_HW_VLAN_MAX_RULES];
static size_t g_vlan_n;
static bool g_vlan_fail_next;

void omci_hw_vlan_fail_next(void)
{
	g_vlan_fail_next = true;
}

static uint16_t g_vlan_reject_id;
static bool g_vlan_reject_next;

void omci_hw_vlan_reject_next(uint16_t key)
{
	g_vlan_reject_id = key;
	g_vlan_reject_next = true;
}

enum omci_status omci_hw_vlan_apply(const struct omci_hw_vlan_xlate *rules,
				    size_t n, struct omci_hw_reject *rejected)
{
	if (rejected != NULL)
		*rejected = (struct omci_hw_reject){ 0 };

	if (rules == NULL && n > 0)
		return OMCI_ERR_INVAL;

	if (g_vlan_fail_next) {
		g_vlan_fail_next = false;
		return OMCI_ERR_HW;
	}

	if (n > OMCI_HW_VLAN_MAX_RULES)
		return OMCI_ERR_NOMEM;

	for (size_t i = 0; i < n; i++)
		g_vlan[i] = rules[i];
	g_vlan_n = n;

	if (g_vlan_reject_next) {
		g_vlan_reject_next = false;
		for (size_t i = 0; i < n; i++)
			if (omci_hw_vlan_key(&rules[i]) == g_vlan_reject_id) {
				omci_hw_reject_add(rejected, g_vlan_reject_id);
				return OMCI_DECLINED;
			}
	}
	return OMCI_OK;
}

static struct omci_hw_gem_port g_gem[OMCI_HW_GEM_MAX_PORTS];
static size_t g_gem_n;
static bool g_gem_fail_next;

static uint16_t g_gem_reject_id;
static bool g_gem_reject_next;

void omci_hw_gem_fail_next(void)
{
	g_gem_fail_next = true;
}

void omci_hw_gem_reject_next(uint16_t port_id)
{
	g_gem_reject_id = port_id;
	g_gem_reject_next = true;
}

enum omci_status omci_hw_gem_apply(const struct omci_hw_gem_port *ports,
				   size_t n, struct omci_hw_reject *rejected)
{
	if (rejected != NULL)
		*rejected = (struct omci_hw_reject){ 0 };

	if (ports == NULL && n > 0)
		return OMCI_ERR_INVAL;

	if (g_gem_fail_next) {
		g_gem_fail_next = false;
		return OMCI_ERR_HW;
	}

	if (n > OMCI_HW_GEM_MAX_PORTS)
		return OMCI_ERR_NOMEM;

	for (size_t i = 0; i < n; i++)
		g_gem[i] = ports[i];
	g_gem_n = n;

	if (g_gem_reject_next) {
		g_gem_reject_next = false;
		for (size_t i = 0; i < n; i++)
			if (ports[i].port_id == g_gem_reject_id) {
				omci_hw_reject_add(rejected, g_gem_reject_id);
				return OMCI_DECLINED;
			}
	}
	return OMCI_OK;
}

static uint16_t g_tcont[OMCI_HW_TCONT_MAX];
static size_t g_tcont_n;
static bool g_tcont_fail_next;

static uint16_t g_tcont_reject_index;
static bool g_tcont_reject_next;

void omci_hw_tcont_fail_next(void)
{
	g_tcont_fail_next = true;
}

void omci_hw_tcont_reject_next(uint16_t index)
{
	g_tcont_reject_index = index;
	g_tcont_reject_next = true;
}

enum omci_status omci_hw_tcont_apply(const uint16_t *alloc, size_t n,
				     struct omci_hw_reject *rejected)
{
	if (rejected != NULL)
		*rejected = (struct omci_hw_reject){ 0 };

	if (alloc == NULL && n > 0)
		return OMCI_ERR_INVAL;

	if (g_tcont_fail_next) {
		g_tcont_fail_next = false;
		return OMCI_ERR_HW;
	}

	if (n > OMCI_HW_TCONT_MAX)
		return OMCI_ERR_NOMEM;

	for (size_t i = 0; i < n; i++)
		g_tcont[i] = alloc[i];
	g_tcont_n = n;

	if (g_tcont_reject_next && g_tcont_reject_index < n) {
		g_tcont_reject_next = false;
		omci_hw_reject_add(rejected, g_tcont_reject_index);
		return OMCI_DECLINED;
	}
	return OMCI_OK;
}

size_t omci_hw_tcont_recorded(const uint16_t **out)
{
	if (out != NULL)
		*out = g_tcont;
	return g_tcont_n;
}

static struct omci_hw_bridge_port g_br[OMCI_HW_BRIDGE_MAX_PORTS];
static size_t g_br_n;
static bool g_br_fail_next;

static uint16_t g_br_reject_id;
static bool g_br_reject_next;

void omci_hw_bridge_fail_next(void)
{
	g_br_fail_next = true;
}

void omci_hw_bridge_reject_next(uint16_t port_id)
{
	g_br_reject_id = port_id;
	g_br_reject_next = true;
}

enum omci_status omci_hw_bridge_apply(const struct omci_hw_bridge_port *ports,
				      size_t n, struct omci_hw_reject *rejected)
{
	if (rejected != NULL)
		*rejected = (struct omci_hw_reject){ 0 };

	if (ports == NULL && n > 0)
		return OMCI_ERR_INVAL;

	if (g_br_fail_next) {
		g_br_fail_next = false;
		return OMCI_ERR_HW;
	}

	if (n > OMCI_HW_BRIDGE_MAX_PORTS)
		return OMCI_ERR_NOMEM;

	for (size_t i = 0; i < n; i++)
		g_br[i] = ports[i];
	g_br_n = n;

	if (g_br_reject_next) {
		g_br_reject_next = false;
		for (size_t i = 0; i < n; i++)
			if (ports[i].port_id == g_br_reject_id) {
				omci_hw_reject_add(rejected, g_br_reject_id);
				return OMCI_DECLINED;
			}
	}
	return OMCI_OK;
}

size_t omci_hw_bridge_recorded(const struct omci_hw_bridge_port **out)
{
	if (out != NULL)
		*out = g_br;
	return g_br_n;
}

size_t omci_hw_gem_recorded(const struct omci_hw_gem_port **out)
{
	if (out != NULL)
		*out = g_gem;
	return g_gem_n;
}

size_t omci_hw_vlan_recorded(const struct omci_hw_vlan_xlate **out)
{
	if (out != NULL)
		*out = g_vlan;
	return g_vlan_n;
}

/* The rules of the services that a test sets, as a person's. */
static struct omci_svc_rule g_svc[OMCI_SVC_MAX];
static size_t g_svc_n;

void omci_hw_service_rules_set(const struct omci_svc_rule *rules, size_t n)
{
	g_svc_n = n < OMCI_SVC_MAX ? n : OMCI_SVC_MAX;
	memcpy(g_svc, rules, g_svc_n * sizeof(g_svc[0]));
}

static size_t omci_hw_service_rules(struct omci_svc_rule *out, size_t cap)
{
	const size_t n = g_svc_n < cap ? g_svc_n : cap;

	memcpy(out, g_svc, n * sizeof(g_svc[0]));
	return n;
}

const char *omci_hw_source(void)
{
	return "synthetic ONU (host build, no device present)";
}

static uint32_t g_alarm_mask;
static bool g_alarm_fail;

enum omci_status omci_hw_alarms(uint32_t *mask)
{
	if (mask == NULL)
		return OMCI_ERR_INVAL;
	if (g_alarm_fail) {
		*mask = 0;
		return OMCI_ERR_HW;
	}
	*mask = g_alarm_mask;
	return OMCI_OK;
}

void omci_hw_alarm_fail_next(bool fail)
{
	g_alarm_fail = fail;
}

void omci_hw_alarm_force(enum omci_hw_alarm alarm, bool asserted)
{
	if ((unsigned int)alarm >= (unsigned int)OMCI_HW_ALARM_COUNT)
		return;
	if (asserted)
		g_alarm_mask |= 1u << (unsigned int)alarm;
	else
		g_alarm_mask &= ~(1u << (unsigned int)alarm);
}

static struct omci_hw_pon g_pon;
static bool g_pon_fail;

enum omci_status omci_hw_pon(struct omci_hw_pon *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	if (g_pon_fail) {
		omci_wire_fill(out, 0, sizeof(*out));
		return OMCI_ERR_HW;
	}
	*out = g_pon;
	return OMCI_OK;
}

void omci_hw_pon_force(bool operating, bool bip_valid, uint64_t bip_errors)
{
	g_pon.operating = operating;
	g_pon.bip_valid = bip_valid;
	g_pon.bip_errors = bip_errors;
}

void omci_hw_pon_fail_next(bool fail)
{
	g_pon_fail = fail;
}

void omci_hw_pon_level_force(bool valid, uint8_t level)
{
	g_pon.power_level_valid = valid;
	g_pon.power_level = level;
}

void omci_hw_pon_fec_force(bool valid, uint64_t corrected_bytes,
			   uint64_t corrected_codewords,
			   uint64_t uncorrectable_codewords, uint64_t codewords)
{
	g_pon.fec_valid = valid;
	g_pon.fec_corrected_bytes = corrected_bytes;
	g_pon.fec_corrected_codewords = corrected_codewords;
	g_pon.fec_uncorrectable_codewords = uncorrectable_codewords;
	g_pon.fec_codewords = codewords;
}

#define STUB_PORTS 8u
#define STUB_CTRS 80u
static uint64_t g_ctr[STUB_PORTS][STUB_CTRS];

static bool g_ctr_fail;

void omci_hw_counter_fail_next(bool fail)
{
	g_ctr_fail = fail;
}

enum omci_status omci_hw_port_counters(unsigned int port,
				       uint64_t out[OMCI_HW_PORT_COUNTERS])
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	omci_wire_fill(out, 0, OMCI_HW_PORT_COUNTERS * sizeof(out[0]));
	if (g_ctr_fail)
		return OMCI_ERR_HW;
	if (port >= STUB_PORTS)
		return OMCI_ERR_INVAL;
	for (unsigned int i = 0; i < OMCI_HW_PORT_COUNTERS; i++)
		out[i] = g_ctr[port][i];
	return OMCI_OK;
}

static struct omci_hw_fdb g_fdb[OMCI_HW_FDB_MAX];
static size_t g_fdb_n;
static bool g_fdb_fail;

enum omci_status omci_hw_fdb(struct omci_hw_fdb *out, size_t cap, size_t *n)
{
	if (out == NULL || n == NULL)
		return OMCI_ERR_INVAL;
	*n = 0;
	if (g_fdb_fail)
		return OMCI_ERR_HW;
	for (size_t i = 0; i < g_fdb_n && i < cap; i++)
		out[(*n)++] = g_fdb[i];
	return OMCI_OK;
}

void omci_hw_fdb_force(const struct omci_hw_fdb *entries, size_t n)
{
	g_fdb_n = 0;
	for (size_t i = 0; i < n && i < OMCI_HW_FDB_MAX; i++)
		g_fdb[g_fdb_n++] = entries[i];
}

void omci_hw_fdb_fail_next(bool fail)
{
	g_fdb_fail = fail;
}

static bool g_routing;

bool omci_hw_routing(void)
{
	return g_routing;
}

void omci_hw_routing_force(bool routing)
{
	g_routing = routing;
}

void omci_hw_counter_force(unsigned int port, unsigned int idx, uint64_t value)
{
	if (port < STUB_PORTS && idx < STUB_CTRS)
		g_ctr[port][idx] = value;
}

/* The GEM ports the host backend counts for, by Port-ID. */
#define STUB_GEM_COUNTS 8u

static struct {
	bool used;
	uint16_t port_id;
	struct omci_hw_gem_counts counts;
} g_gem_counts[STUB_GEM_COUNTS];

enum omci_status omci_hw_gem_counts(uint16_t port_id,
				    struct omci_hw_gem_counts *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	omci_wire_fill(out, 0, sizeof(*out));
	for (unsigned int i = 0; i < STUB_GEM_COUNTS; i++)
		if (g_gem_counts[i].used &&
		    g_gem_counts[i].port_id == port_id) {
			*out = g_gem_counts[i].counts;
			return OMCI_OK;
		}
	return OMCI_ERR_HW;
}

void omci_hw_gem_counts_force(uint16_t port_id,
			      const struct omci_hw_gem_counts *counts)
{
	unsigned int i, free_at = STUB_GEM_COUNTS;

	for (i = 0; i < STUB_GEM_COUNTS; i++) {
		if (g_gem_counts[i].used && g_gem_counts[i].port_id == port_id)
			break;
		if (!g_gem_counts[i].used && free_at == STUB_GEM_COUNTS)
			free_at = i;
	}
	if (i == STUB_GEM_COUNTS)
		i = free_at;
	if (i == STUB_GEM_COUNTS)
		return;
	g_gem_counts[i].used = true;
	g_gem_counts[i].port_id = port_id;
	g_gem_counts[i].counts = *counts;
}

void omci_hw_gem_counts_reset(void)
{
	omci_wire_fill(g_gem_counts, 0, sizeof(g_gem_counts));
}

static struct omci_hw_encrypted g_encrypted;
static bool g_encrypted_fail;

enum omci_status omci_hw_encrypted(struct omci_hw_encrypted *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	if (g_encrypted_fail) {
		omci_wire_fill(out, 0, sizeof(*out));
		return OMCI_ERR_HW;
	}
	*out = g_encrypted;
	return OMCI_OK;
}

void omci_hw_encrypted_force(uint16_t port_id, bool on)
{
	if (port_id >= OMCI_HW_PORT_IDS)
		return;
	if (on)
		g_encrypted.map[port_id / 32u] |= 1u << (port_id % 32u);
	else
		g_encrypted.map[port_id / 32u] &= ~(1u << (port_id % 32u));
}

void omci_hw_encrypted_fail(bool fail)
{
	g_encrypted_fail = fail;
}

void omci_hw_encrypted_reset(void)
{
	omci_wire_fill(&g_encrypted, 0, sizeof(g_encrypted));
	g_encrypted_fail = false;
}

#define STUB_OPTIC_TEMPERATURE 12405u
#define STUB_OPTIC_VOLTAGE 32327u
#define STUB_OPTIC_BIAS 6950u
#define STUB_OPTIC_TX_POWER 16412u
#define STUB_OPTIC_RX_POWER 34u

static uint16_t g_optic[OMCI_HW_OPTIC_COUNT];
static uint8_t g_optic_valid;
static bool g_optic_init;

static void optics_defaults(void)
{
	g_optic[OMCI_HW_OPTIC_TEMPERATURE] = STUB_OPTIC_TEMPERATURE;
	g_optic[OMCI_HW_OPTIC_VOLTAGE] = STUB_OPTIC_VOLTAGE;
	g_optic[OMCI_HW_OPTIC_BIAS] = STUB_OPTIC_BIAS;
	g_optic[OMCI_HW_OPTIC_TX_POWER] = STUB_OPTIC_TX_POWER;
	g_optic[OMCI_HW_OPTIC_RX_POWER] = STUB_OPTIC_RX_POWER;
	g_optic_valid =
		(uint8_t)((1u << (unsigned int)OMCI_HW_OPTIC_COUNT) - 1u);
	g_optic_init = true;
}

void omci_hw_optics_reset(void)
{
	optics_defaults();
}

void omci_hw_optics_force(enum omci_hw_optic which, bool present, uint16_t raw)
{
	if ((unsigned int)which >= (unsigned int)OMCI_HW_OPTIC_COUNT)
		return;
	if (!g_optic_init)
		optics_defaults();

	g_optic[which] = raw;
	if (present)
		g_optic_valid |= (uint8_t)(1u << (unsigned int)which);
	else
		g_optic_valid &= (uint8_t)~(1u << (unsigned int)which);
}

enum omci_status omci_hw_optics(struct omci_hw_optics *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	if (!g_optic_init)
		optics_defaults();

	out->temperature = g_optic[OMCI_HW_OPTIC_TEMPERATURE];
	out->voltage = g_optic[OMCI_HW_OPTIC_VOLTAGE];
	out->bias = g_optic[OMCI_HW_OPTIC_BIAS];
	out->tx_power = g_optic[OMCI_HW_OPTIC_TX_POWER];
	out->rx_power = g_optic[OMCI_HW_OPTIC_RX_POWER];
	out->valid = g_optic_valid;

	return g_optic_valid != 0u ? OMCI_OK : OMCI_ERR_HW;
}

/*
 * The thresholds of the synthetic transceiver: those of a module for a
 * class B+ ONU, which its readings sit within.
 */
static const uint16_t g_limit_default[OMCI_HW_LIMIT_COUNT] = {
	[OMCI_HW_LIMIT_BIAS_HIGH] = 50000u, [OMCI_HW_LIMIT_TX_LOW] = 8912u,
	[OMCI_HW_LIMIT_TX_HIGH] = 39810u,   [OMCI_HW_LIMIT_RX_LOW] = 10u,
	[OMCI_HW_LIMIT_RX_HIGH] = 1995u,
};

static uint16_t g_limit[OMCI_HW_LIMIT_COUNT];
static uint8_t g_limit_valid;
static bool g_limit_init;

void omci_hw_optics_limits_reset(void)
{
	for (unsigned int i = 0; i < (unsigned int)OMCI_HW_LIMIT_COUNT; i++)
		g_limit[i] = g_limit_default[i];
	g_limit_valid =
		(uint8_t)((1u << (unsigned int)OMCI_HW_LIMIT_COUNT) - 1u);
	g_limit_init = true;
}

void omci_hw_optics_limit_force(enum omci_hw_limit which, bool present,
				uint16_t raw)
{
	if ((unsigned int)which >= (unsigned int)OMCI_HW_LIMIT_COUNT)
		return;
	if (!g_limit_init)
		omci_hw_optics_limits_reset();

	g_limit[which] = raw;
	if (present)
		g_limit_valid |= (uint8_t)(1u << (unsigned int)which);
	else
		g_limit_valid &= (uint8_t)~(1u << (unsigned int)which);
}

enum omci_status omci_hw_optics_limits(struct omci_hw_optics_limits *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	if (!g_limit_init)
		omci_hw_optics_limits_reset();

	for (unsigned int i = 0; i < (unsigned int)OMCI_HW_LIMIT_COUNT; i++)
		out->value[i] = g_limit[i];
	out->valid = g_limit_valid;
	return g_limit_valid != 0u ? OMCI_OK : OMCI_ERR_HW;
}

/*
 * Multicast: the groups the bridge forwards to the subscriber side, the
 * messages waiting to be handed over, and what the port was set to.
 */
#define MCAST_QUEUE 8u

static struct omci_hw_mcast_group g_mc[OMCI_HW_MCAST_MAX_GROUPS];
static size_t g_mc_n;
static struct omci_hw_mcast_port g_mc_port;
static unsigned int g_mc_port_set;
static bool g_mc_fail_list, g_mc_fail_add, g_mc_fail_del;
static struct omci_hw_mcast_frame g_mc_q[MCAST_QUEUE];
static size_t g_mc_q_head, g_mc_q_n;
static struct {
	uint16_t port_id;
	struct omci_hw_mcast_frame f;
} g_mc_sent[MCAST_QUEUE];
static size_t g_mc_sent_n;
static bool g_mc_fail_send;
static struct omci_hw_mcast_ds g_mc_ds[OMCI_HW_MCAST_DS_MAX];
static size_t g_mc_ds_n;
static bool g_mc_fail_ds;
static struct {
	uint8_t group[OMCI_HW_MCAST_ADDR_LEN];
	uint64_t frames;
} g_mc_watch[OMCI_HW_MCAST_WATCH_MAX];
static size_t g_mc_watch_n;
static bool g_mc_fail_watch, g_mc_fail_seen;

void omci_hw_mcast_reset(void)
{
	omci_wire_fill(g_mc, 0, sizeof(g_mc));
	g_mc_n = 0;
	omci_wire_fill(&g_mc_port, 0, sizeof(g_mc_port));
	g_mc_port_set = 0;
	g_mc_fail_list = false;
	g_mc_fail_add = false;
	g_mc_fail_del = false;
	g_mc_q_head = 0;
	g_mc_q_n = 0;
	g_mc_sent_n = 0;
	g_mc_fail_send = false;
	g_mc_ds_n = 0;
	g_mc_fail_ds = false;
	g_mc_watch_n = 0;
	g_mc_fail_watch = false;
	g_mc_fail_seen = false;
}

void omci_hw_mcast_watch_fail_next(bool watch, bool seen)
{
	g_mc_fail_watch = watch;
	g_mc_fail_seen = seen;
}

/* The place of a counted group; g_mc_watch_n when it is not counted. */
static size_t watch_find(const uint8_t *group)
{
	size_t i = 0;

	while (i < g_mc_watch_n && !omci_wire_equal(g_mc_watch[i].group, group,
						    OMCI_HW_MCAST_ADDR_LEN))
		i++;
	return i;
}

void omci_hw_mcast_flow(const uint8_t group[static OMCI_HW_MCAST_ADDR_LEN],
			uint64_t frames)
{
	const size_t i = watch_find(group);

	if (i < g_mc_watch_n)
		g_mc_watch[i].frames = frames;
}

enum omci_status
omci_hw_mcast_watch(const uint8_t group[static OMCI_HW_MCAST_ADDR_LEN], bool on)
{
	const size_t i = watch_find(group);

	if (g_mc_fail_watch) {
		g_mc_fail_watch = false;
		return OMCI_ERR_HW;
	}
	if (!on) {
		if (i < g_mc_watch_n)
			g_mc_watch[i] = g_mc_watch[--g_mc_watch_n];
		return OMCI_OK;
	}
	if (i < g_mc_watch_n)
		return OMCI_OK;
	if (g_mc_watch_n == OMCI_HW_MCAST_WATCH_MAX)
		return OMCI_ERR_NOMEM;
	omci_wire_copy(g_mc_watch[g_mc_watch_n].group, group,
		       OMCI_HW_MCAST_ADDR_LEN);
	g_mc_watch[g_mc_watch_n++].frames = 0;
	return OMCI_OK;
}

enum omci_status
omci_hw_mcast_seen(const uint8_t group[static OMCI_HW_MCAST_ADDR_LEN],
		   uint64_t *frames)
{
	const size_t i = watch_find(group);

	if (g_mc_fail_seen) {
		g_mc_fail_seen = false;
		return OMCI_ERR_HW;
	}
	if (i == g_mc_watch_n)
		return OMCI_DECLINED;
	*frames = g_mc_watch[i].frames;
	return OMCI_OK;
}

void omci_hw_mcast_ds_fail_next(void)
{
	g_mc_fail_ds = true;
}

enum omci_status omci_hw_mcast_ds(const struct omci_hw_mcast_ds *rules,
				  size_t n)
{
	if (n > OMCI_HW_MCAST_DS_MAX)
		return OMCI_ERR_NOMEM;
	if (g_mc_fail_ds) {
		g_mc_fail_ds = false;
		return OMCI_ERR_HW;
	}
	for (size_t i = 0; i < n; i++)
		g_mc_ds[i] = rules[i];
	g_mc_ds_n = n;
	return OMCI_OK;
}

size_t omci_hw_mcast_ds_recorded(const struct omci_hw_mcast_ds **out)
{
	*out = g_mc_ds;
	return g_mc_ds_n;
}

static uint64_t g_uni_rate;
static unsigned int g_uni_rate_changes;
static bool g_uni_rate_fail;

void omci_hw_uni_rate_fail_next(void)
{
	g_uni_rate_fail = true;
}

enum omci_status omci_hw_uni_rate(uint64_t bytes_ps)
{
	if (g_uni_rate_fail) {
		g_uni_rate_fail = false;
		return OMCI_ERR_HW;
	}
	g_uni_rate = bytes_ps;
	g_uni_rate_changes++;
	return OMCI_OK;
}

uint64_t omci_hw_uni_rate_recorded(unsigned int *changes)
{
	if (changes != NULL)
		*changes = g_uni_rate_changes;
	return g_uni_rate;
}

static struct omci_hw_filter g_filters[OMCI_HW_FILTER_MAX];
static size_t g_filter_n;
static bool g_filter_fail;

void omci_hw_filter_fail_next(void)
{
	g_filter_fail = true;
}

enum omci_status omci_hw_filter_apply(const struct omci_hw_filter *rules,
				      size_t n)
{
	if (n > OMCI_HW_FILTER_MAX)
		return OMCI_ERR_NOMEM;
	if (g_filter_fail) {
		g_filter_fail = false;
		return OMCI_ERR_HW;
	}
	for (size_t i = 0; i < n; i++)
		g_filters[i] = rules[i];
	g_filter_n = n;
	return OMCI_OK;
}

size_t omci_hw_filter_recorded(const struct omci_hw_filter **out)
{
	*out = g_filters;
	return g_filter_n;
}

void omci_hw_mcast_send_fail_next(void)
{
	g_mc_fail_send = true;
}

enum omci_status omci_hw_mcast_send(uint16_t port_id, const uint8_t *frame,
				    size_t len)
{
	struct omci_hw_mcast_frame *f;

	if (len > sizeof(f->data))
		return OMCI_ERR_INVAL;
	if (g_mc_fail_send) {
		g_mc_fail_send = false;
		return OMCI_ERR_HW;
	}
	g_mc_sent[g_mc_sent_n % MCAST_QUEUE].port_id = port_id;
	f = &g_mc_sent[g_mc_sent_n++ % MCAST_QUEUE].f;
	f->len = (uint16_t)len;
	omci_wire_copy(f->data, frame, len);
	return OMCI_OK;
}

enum omci_status omci_hw_mcast_mac(uint8_t mac[static 6])
{
	static const uint8_t own[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 };

	omci_wire_copy(mac, own, sizeof(own));
	return OMCI_OK;
}

size_t omci_hw_mcast_sent(size_t index, uint16_t *port_id, uint8_t *frame,
			  size_t cap)
{
	const struct omci_hw_mcast_frame *f;

	if (index >= g_mc_sent_n || index >= MCAST_QUEUE)
		return 0;
	*port_id = g_mc_sent[(g_mc_sent_n - 1u - index) % MCAST_QUEUE].port_id;
	f = &g_mc_sent[(g_mc_sent_n - 1u - index) % MCAST_QUEUE].f;
	omci_wire_copy(frame, f->data, f->len < cap ? f->len : cap);
	return f->len;
}

void omci_hw_mcast_fail_next(bool list, bool add, bool del)
{
	g_mc_fail_list = list;
	g_mc_fail_add = add;
	g_mc_fail_del = del;
}

/* The place of a group, by its VLAN, group and source; g_mc_n when absent. */
static size_t mcast_find(const struct omci_hw_mcast_group *g)
{
	size_t i = 0;

	while (i < g_mc_n &&
	       (g_mc[i].vid != g->vid ||
		!omci_wire_equal(g_mc[i].group, g->group, sizeof(g->group)) ||
		!omci_wire_equal(g_mc[i].source, g->source, sizeof(g->source))))
		i++;
	return i;
}

static void mcast_remove(size_t i)
{
	for (; i + 1u < g_mc_n; i++)
		g_mc[i] = g_mc[i + 1u];
	g_mc_n--;
}

void omci_hw_mcast_snoop(const struct omci_hw_mcast_group *g, bool join)
{
	const size_t i = mcast_find(g);

	if (join && i == g_mc_n && g_mc_n < OMCI_HW_MCAST_MAX_GROUPS) {
		g_mc[g_mc_n] = *g;
		g_mc[g_mc_n++].permanent = false;
	}
	if (!join && i < g_mc_n && !g_mc[i].permanent)
		mcast_remove(i);
}

static void mcast_queue(const uint8_t *frame, size_t len, bool from_pon,
			bool tagged, uint16_t tci)
{
	struct omci_hw_mcast_frame *f;

	if (g_mc_q_n == MCAST_QUEUE)
		return;
	f = &g_mc_q[(g_mc_q_head + g_mc_q_n++) % MCAST_QUEUE];
	if (len > sizeof(f->data))
		len = sizeof(f->data);
	f->from_pon = from_pon;
	f->tagged = tagged;
	f->tci = tci;
	f->len = (uint16_t)len;
	omci_wire_copy(f->data, frame, len);
}

void omci_hw_mcast_receive(const uint8_t *frame, size_t len, bool tagged,
			   uint16_t tci)
{
	mcast_queue(frame, len, false, tagged, tci);
}

void omci_hw_mcast_receive_pon(const uint8_t *frame, size_t len)
{
	mcast_queue(frame, len, true, false, 0);
}

unsigned int omci_hw_mcast_port_recorded(struct omci_hw_mcast_port *out)
{
	if (out != NULL)
		*out = g_mc_port;
	return g_mc_port_set;
}

enum omci_status omci_hw_mcast_port(const struct omci_hw_mcast_port *cfg)
{
	if (cfg == NULL)
		return OMCI_ERR_INVAL;
	g_mc_port = *cfg;
	g_mc_port_set++;
	return OMCI_OK;
}

enum omci_status omci_hw_mcast_groups(struct omci_hw_mcast_group *out,
				      size_t cap, size_t *n)
{
	size_t k = 0;

	if (g_mc_fail_list) {
		g_mc_fail_list = false;
		return OMCI_ERR_HW;
	}
	for (; k < g_mc_n && k < cap; k++)
		out[k] = g_mc[k];
	*n = k;
	return OMCI_OK;
}

enum omci_status omci_hw_mcast_group_add(const struct omci_hw_mcast_group *g)
{
	if (g_mc_fail_add) {
		g_mc_fail_add = false;
		return OMCI_ERR_HW;
	}
	if (mcast_find(g) < g_mc_n)
		return OMCI_OK;
	if (g_mc_n == OMCI_HW_MCAST_MAX_GROUPS)
		return OMCI_ERR_NOMEM;
	g_mc[g_mc_n++] = *g;
	return OMCI_OK;
}

enum omci_status omci_hw_mcast_group_del(const struct omci_hw_mcast_group *g)
{
	const size_t i = mcast_find(g);

	if (g_mc_fail_del) {
		g_mc_fail_del = false;
		return OMCI_ERR_HW;
	}
	if (i == g_mc_n)
		return OMCI_DECLINED;
	mcast_remove(i);
	return OMCI_OK;
}

enum omci_status omci_hw_mcast_frame(struct omci_hw_mcast_frame *out)
{
	if (g_mc_q_n == 0)
		return OMCI_DECLINED;
	*out = g_mc_q[g_mc_q_head];
	g_mc_q_head = (g_mc_q_head + 1u) % MCAST_QUEUE;
	g_mc_q_n--;
	return OMCI_OK;
}

/* The host backend has no descriptor: the daemon looks once a second. */
static int host_mcast_fd(void)
{
	return -1;
}

const struct omci_hw_ops omci_hw_host_ops = {
	.caps = omci_hw_caps,
	.source = omci_hw_source,
	.vlan_apply = omci_hw_vlan_apply,
	.gem_apply = omci_hw_gem_apply,
	.tcont_apply = omci_hw_tcont_apply,
	.bridge_apply = omci_hw_bridge_apply,
	.alarms = omci_hw_alarms,
	.pon = omci_hw_pon,
	.port_counters = omci_hw_port_counters,
	.gem_counts = omci_hw_gem_counts,
	.encrypted = omci_hw_encrypted,
	.routing = omci_hw_routing,
	.service_rules = omci_hw_service_rules,
	.fdb = omci_hw_fdb,
	.optics = omci_hw_optics,
	.optics_limits = omci_hw_optics_limits,
	.mcast_port = omci_hw_mcast_port,
	.mcast_groups = omci_hw_mcast_groups,
	.mcast_group_add = omci_hw_mcast_group_add,
	.mcast_group_del = omci_hw_mcast_group_del,
	.mcast_fd = host_mcast_fd,
	.mcast_frame = omci_hw_mcast_frame,
	.mcast_send = omci_hw_mcast_send,
	.mcast_mac = omci_hw_mcast_mac,
	.mcast_ds = omci_hw_mcast_ds,
	.mcast_watch = omci_hw_mcast_watch,
	.mcast_seen = omci_hw_mcast_seen,
	.filter_apply = omci_hw_filter_apply,
	.uni_rate = omci_hw_uni_rate,
};
