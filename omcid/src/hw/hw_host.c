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

#include "hw/hw.h"
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
	out->num_gem_ports = 32;

	omci_wire_copy(out->identity.vendor_id, "ZZZZ", 5);
	omci_wire_copy(out->identity.version, "0.1-synthetic", 14);
	omci_wire_copy(out->identity.equipment_id, "SYNTHETIC-ONU", 14);
	omci_wire_copy(out->identity.sw_version[0], "0.1.0-synth", 12);
	omci_wire_copy(out->identity.sw_version[1], "0.0.0-empty", 12);

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

void omci_hw_vlan_reject_next(uint16_t match_cvid)
{
	g_vlan_reject_id = match_cvid;
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
			if (rules[i].match_cvid == g_vlan_reject_id) {
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

#define STUB_PORTS 8u
#define STUB_CTRS 80u
static uint64_t g_ctr[STUB_PORTS][STUB_CTRS];

static bool g_ctr_fail;

void omci_hw_counter_fail_next(bool fail)
{
	g_ctr_fail = fail;
}

enum omci_status omci_hw_counter(unsigned int port, unsigned int idx,
				 uint64_t *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	*out = 0;
	if (g_ctr_fail)
		return OMCI_ERR_HW;
	if (port >= STUB_PORTS || idx >= STUB_CTRS)
		return OMCI_ERR_INVAL;
	*out = g_ctr[port][idx];
	return OMCI_OK;
}

void omci_hw_counter_force(unsigned int port, unsigned int idx, uint64_t value)
{
	if (port < STUB_PORTS && idx < STUB_CTRS)
		g_ctr[port][idx] = value;
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

static const struct omci_hw_ops g_host_ops = {
	.caps = omci_hw_caps,
	.source = omci_hw_source,
	.vlan_apply = omci_hw_vlan_apply,
	.gem_apply = omci_hw_gem_apply,
	.bridge_apply = omci_hw_bridge_apply,
	.alarms = omci_hw_alarms,
	.counter = omci_hw_counter,
	.optics = omci_hw_optics,
};

static const struct omci_hw_ops *g_ops = &g_host_ops;

const struct omci_hw_ops *omci_hw(void)
{
	return g_ops;
}

void omci_hw_ops_set(const struct omci_hw_ops *ops)
{
	g_ops = ops != NULL ? ops : &g_host_ops;
}
