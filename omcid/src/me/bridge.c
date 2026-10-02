// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The bridges of bridge.h. */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "me/bridge.h"
#include "omcid/array_size.h"
#include "sys/log.h"

static struct omci_hw_bridge_port g_last[OMCI_HW_BRIDGE_MAX_PORTS];
static size_t g_last_n;

static uint16_t g_bad[OMCI_HW_BRIDGE_MAX_PORTS];
static uint8_t g_bad_n;

static void bad_add(uint16_t instance)
{
	for (uint8_t i = 0; i < g_bad_n; i++)
		if (g_bad[i] == instance)
			return;
	if (g_bad_n < ARRAY_SIZE(g_bad))
		g_bad[g_bad_n++] = instance;
}

static bool attr_u16(const struct omci_mib *mib, uint16_t class_id,
		     uint16_t instance, uint8_t attr, uint16_t *out)
{
	uint8_t buf[2] = { 0, 0 };
	size_t len = 0;

	if (omci_mib_attr_get(mib, class_id, instance, attr, buf, sizeof(buf),
			      &len) != OMCI_OK ||
	    len < sizeof(buf))
		return false;

	*out = (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
	return true;
}

static uint8_t attr_u8(const struct omci_mib *mib, uint16_t class_id,
		       uint16_t instance, uint8_t attr)
{
	uint8_t buf[1] = { 0 };
	size_t len = 0;

	(void)!omci_mib_attr_get(mib, class_id, instance, attr, buf,
				 sizeof(buf), &len);
	return buf[0];
}

/* The port identifier of the GEM port behind an interworking termination point. */
static bool gem_port_id(const struct omci_mib *mib, uint16_t iw_class,
			uint16_t iw_instance, uint16_t *port_id)
{
	uint16_t ctp = 0;

	if (!attr_u16(mib, iw_class, iw_instance, OMCI_BRIDGE_GEMIW_ATTR_CTP,
		      &ctp))
		return false;
	return attr_u16(mib, OMCI_BRIDGE_GEM_CLASS_ID, ctp,
			OMCI_BRIDGE_GEM_ATTR_PORT_ID, port_id);
}

static void add_port_id(uint16_t *ids, uint8_t *n, uint16_t id)
{
	for (uint8_t i = 0; i < *n; i++)
		if (ids[i] == id)
			return;
	ids[(*n)++] = id;
}

/*
 * The GEM ports a network-side bridge port leads to. The switch bridges
 * them all together: a mapper's choice by priority does not survive.
 *
 * Return: their number, 0 when none can be found.
 */
static uint8_t resolve_gem_ports(const struct omci_mib *mib, uint16_t instance,
				 uint8_t tp_type, uint16_t tp_ptr,
				 uint16_t *ids)
{
	uint8_t n = 0;

	switch (tp_type) {
	case OMCI_BRIDGE_TP_GEM_IW:
	case OMCI_BRIDGE_TP_MCAST_GEM: {
		const uint16_t cls = tp_type == OMCI_BRIDGE_TP_GEM_IW ?
					     OMCI_BRIDGE_GEMIW_CLASS_ID :
					     OMCI_BRIDGE_MCIW_CLASS_ID;
		uint16_t id = 0;

		if (!gem_port_id(mib, cls, tp_ptr, &id)) {
			OMCI_WARN(
				"me47: port %#06x points at %s %#06x, which names no GEM port",
				instance,
				tp_type == OMCI_BRIDGE_TP_GEM_IW ? "ME 266" :
								   "ME 281",
				tp_ptr);
			return 0;
		}
		add_port_id(ids, &n, id);
		return n;
	}

	case OMCI_BRIDGE_TP_MAPPER: {
		unsigned int found = 0;

		for (uint8_t b = 0; b < OMCI_BRIDGE_MAPPER_PBITS; b++) {
			uint16_t iw = 0;
			uint16_t id = 0;

			if (!attr_u16(mib, OMCI_BRIDGE_MAPPER_CLASS_ID, tp_ptr,
				      (uint8_t)(OMCI_BRIDGE_MAPPER_ATTR_FIRST +
						b),
				      &iw))
				continue;

			/* A priority the mapper sends nowhere. */
			if (iw == 0xffffu || iw == 0)
				continue;
			if (!gem_port_id(mib, OMCI_BRIDGE_GEMIW_CLASS_ID, iw,
					 &id)) {
				OMCI_WARN(
					"me130: mapper %#06x p-bit %u points at ME 266 %#06x, which names no GEM port",
					tp_ptr, b, iw);
				continue;
			}
			found++;
			add_port_id(ids, &n, id);
		}

		if (n > 1)
			OMCI_WARN(
				"me130: mapper %#06x sorts %u priorities into %u GEM ports; this ONU bridges them together and the p-bit does not choose",
				tp_ptr, found, n);
		return n;
	}

	default:
		OMCI_WARN(
			"me47: port %#06x is a termination point of type %u, which this ONU does not bridge",
			instance, tp_type);
		return 0;
	}
}

/*
 * The VIDs of the VLAN tagging filter data of a bridge port: a list of up
 * to twelve tags; the count attribute, or the first empty tag, ends it.
 */
static uint8_t vlans_of(const struct omci_mib *mib, uint16_t instance,
			uint16_t *vlan)
{
	uint8_t table[24] = { 0 };

	static_assert(sizeof(table) / 2u <= OMCI_HW_BRIDGE_MAX_VLANS,
		      "a port carries every entry of the filter table");
	uint8_t count = 0;
	size_t len = 0;
	uint8_t n = 0;

	if (omci_mib_attr_get(mib, OMCI_BRIDGE_VLAN_CLASS_ID, instance,
			      OMCI_BRIDGE_VLAN_ATTR_TABLE, table, sizeof(table),
			      &len) != OMCI_OK)
		return 0;
	count = attr_u8(mib, OMCI_BRIDGE_VLAN_CLASS_ID, instance,
			OMCI_BRIDGE_VLAN_ATTR_COUNT);

	if (count > sizeof(table) / 2)
		count = (uint8_t)(sizeof(table) / 2);

	for (uint8_t i = 0; i < sizeof(table) / 2; i++) {
		const uint16_t tci = (uint16_t)((uint16_t)table[i * 2] << 8 |
						table[i * 2 + 1]);
		const uint16_t vid = tci & 0x0fffu;

		if (tci == 0)
			break;
		if (count && i >= count)
			break;
		vlan[n++] = vid;
	}
	return n;
}

/*
 * Collect the bridge ports of the MIB into @out, with the instance of each
 * in @inst_out; the ports that lead nowhere are counted in @bad_out.
 *
 * Return: the number of ports collected.
 */
static size_t gather(const struct omci_mib *mib,
		     struct omci_hw_bridge_port *out, uint16_t *inst_out,
		     size_t cap, unsigned int *bad_out)
{
	struct omci_mib_cursor cur;
	unsigned int bad = 0;
	size_t n = 0;
	enum omci_status rc;

	for (rc = omci_mib_walk_first(mib, &cur); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &cur)) {
		uint16_t ids[OMCI_BRIDGE_MAPPER_PBITS] = { 0 };
		uint16_t vlan[OMCI_HW_BRIDGE_MAX_VLANS] = { 0 };
		uint16_t bridge = 0, tp_ptr = 0;
		uint8_t tp_type = 0, nids = 0, nvlan = 0;

		if (cur.class_id != OMCI_BRIDGE_PORT_CLASS_ID)
			continue;

		(void)attr_u16(mib, cur.class_id, cur.instance,
			       OMCI_BRIDGE_PORT_ATTR_BRIDGE, &bridge);
		tp_type = attr_u8(mib, cur.class_id, cur.instance,
				  OMCI_BRIDGE_PORT_ATTR_TPTYPE);
		(void)attr_u16(mib, cur.class_id, cur.instance,
			       OMCI_BRIDGE_PORT_ATTR_TPPTR, &tp_ptr);

		nvlan = vlans_of(mib, cur.instance, vlan);

		if (tp_type == OMCI_BRIDGE_TP_PPTP_ETH ||
		    tp_type == OMCI_BRIDGE_TP_VEIP) {
			if (n >= cap) {
				bad++;
				continue;
			}
			out[n].bridge = bridge;
			out[n].port_id = OMCI_HW_BRIDGE_UNI;
			out[n].num_vlan = nvlan;
			memcpy(out[n].vlan, vlan, sizeof(vlan));
			inst_out[n] = cur.instance;
			n++;
			continue;
		}

		nids = resolve_gem_ports(mib, cur.instance, tp_type, tp_ptr,
					 ids);
		if (!nids) {
			bad++;
			continue;
		}

		for (uint8_t i = 0; i < nids; i++) {
			bool dup = false;

			for (size_t j = 0; j < n; j++)
				if (out[j].port_id == ids[i])
					dup = true;
			if (dup)
				continue;
			if (n >= cap) {
				OMCI_WARN(
					"me47: more than %zu bridge ports — Port-ID %u is NOT bridged",
					cap, ids[i]);
				bad++;
				continue;
			}
			out[n].bridge = bridge;
			out[n].port_id = ids[i];
			out[n].num_vlan = nvlan;
			memcpy(out[n].vlan, vlan, sizeof(vlan));

			inst_out[n] = cur.instance;
			n++;
		}
	}

	if (bad_out != NULL)
		*bad_out = bad;
	return n;
}

enum omci_status omci_bridge_program(const struct omci_mib *mib,
				     unsigned int *refused)
{
	struct omci_hw_bridge_port ports[OMCI_HW_BRIDGE_MAX_PORTS];
	uint16_t inst[OMCI_HW_BRIDGE_MAX_PORTS] = { 0 };
	struct omci_hw_reject rejected = { 0 };
	unsigned int bad;
	size_t n;
	enum omci_status rc;

	g_bad_n = 0;
	bad = 0;
	n = gather(mib, ports, inst, ARRAY_SIZE(ports), &bad);

	if (refused != NULL)
		*refused = bad;

	rc = omci_hw()->bridge_apply(ports, n, &rejected);

	for (size_t i = 0; i < n; i++)
		if (omci_hw_reject_has(&rejected, ports[i].port_id))
			bad_add(inst[i]);

	if (rc != OMCI_OK && rc != OMCI_DECLINED) {
		OMCI_WARN(
			"me47: the device refused a bridge of %zu port(s): %s",
			n, omci_status_str(rc));
		return rc;
	}

	for (size_t i = 0; i < n; i++)
		g_last[i] = ports[i];
	g_last_n = n;

	OMCI_DEBUG(
		"me47: %zu bridge port(s) programmed, %u unresolved, %u refused by the device",
		n, bad, rejected.n);
	return rc;
}

bool omci_bridge_instance_rejected(uint16_t instance)
{
	for (uint8_t i = 0; i < g_bad_n; i++)
		if (g_bad[i] == instance)
			return true;
	return false;
}

size_t omci_bridge_last(const struct omci_hw_bridge_port **out)
{
	if (out != NULL)
		*out = g_last;
	return g_last_n;
}
