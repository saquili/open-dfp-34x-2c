// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The GEM ports of gem.h. */

#include <stdbool.h>
#include <string.h>

#include "me/gem.h"
#include "omcid/array_size.h"
#include "sys/log.h"

/* T-CONT instances are 0x8000 to 0x80ff; 0 and 0xffff point at none. */
#define GEM_TCONT_BASE 0x8000u
#define GEM_TCONT_MAX 0x80ffu

#define GEM_TCONT_NONE_A 0x0000u
#define GEM_TCONT_NONE_B 0xffffu

/* A GEM header carries a port identifier of twelve bits. */
#define GEM_PORT_ID_MAX 4095u

static struct omci_hw_gem_port g_last[OMCI_HW_GEM_MAX_PORTS];
static size_t g_last_n;

static uint16_t g_bad[OMCI_HW_GEM_MAX_PORTS * 2];
static uint8_t g_bad_n;

static void bad_add(uint16_t instance)
{
	if (g_bad_n < ARRAY_SIZE(g_bad))
		g_bad[g_bad_n++] = instance;
}

static uint16_t attr_u16(const struct omci_mib *mib, uint16_t instance,
			 uint8_t attr)
{
	uint8_t buf[2] = { 0, 0 };
	size_t len = 0;

	(void)!omci_mib_attr_get(mib, OMCI_GEM_CLASS_ID, instance, attr, buf,
				 sizeof(buf), &len);
	return (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
}

static uint8_t attr_u8(const struct omci_mib *mib, uint16_t instance,
		       uint8_t attr)
{
	uint8_t buf[1] = { 0 };
	size_t len = 0;

	(void)!omci_mib_attr_get(mib, OMCI_GEM_CLASS_ID, instance, attr, buf,
				 sizeof(buf), &len);
	return buf[0];
}

/*
 * Collect the valid GEM ports of the MIB into @out, with the instance of
 * each in @inst_out; the others are counted in @bad_out and remembered.
 *
 * Return: the number of ports collected.
 */
static size_t gather(const struct omci_mib *mib, struct omci_hw_gem_port *out,
		     uint16_t *inst_out, size_t cap, unsigned int *bad_out)
{
	struct omci_mib_cursor cur;
	unsigned int bad = 0;
	size_t n = 0;
	enum omci_status rc;

	for (rc = omci_mib_walk_first(mib, &cur); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &cur)) {
		struct omci_hw_gem_port p = { 0 };
		uint16_t port_id = 0;
		uint16_t tcont = GEM_TCONT_NONE_A;
		uint8_t dir = 0;
		bool dup = false;

		if (cur.class_id != OMCI_GEM_CLASS_ID)
			continue;

		port_id = attr_u16(mib, cur.instance, OMCI_GEM_ATTR_PORT_ID);
		dir = attr_u8(mib, cur.instance, OMCI_GEM_ATTR_DIRECTION);

		tcont = attr_u16(mib, cur.instance, OMCI_GEM_ATTR_TCONT);

		if (port_id > GEM_PORT_ID_MAX) {
			OMCI_WARN(
				"me268: instance %#06x asks for Port-ID %u, and a GEM header carries twelve bits",
				cur.instance, port_id);
			bad++;
			bad_add(cur.instance);
			continue;
		}

		if (dir != OMCI_HW_GEM_DIR_UPSTREAM &&
		    dir != OMCI_HW_GEM_DIR_DOWNSTREAM &&
		    dir != OMCI_HW_GEM_DIR_BOTH) {
			OMCI_WARN(
				"me268: instance %#06x asks for direction %u, which G.988 does not define",
				cur.instance, dir);
			bad++;
			bad_add(cur.instance);
			continue;
		}

		if (dir == OMCI_HW_GEM_DIR_UPSTREAM ||
		    dir == OMCI_HW_GEM_DIR_BOTH) {
			if (tcont < GEM_TCONT_BASE || tcont > GEM_TCONT_MAX) {
				OMCI_WARN(
					"me268: instance %#06x carries traffic upstream and points at %#06x, which is not a T-CONT instance id at all",
					cur.instance, tcont);
				bad++;
				bad_add(cur.instance);
				continue;
			}

			if (!omci_mib_exists(mib, OMCI_GEM_TCONT_CLASS_ID,
					     tcont)) {
				OMCI_WARN(
					"me268: instance %#06x points at ME 262 %#06x, which this ONU does not have",
					cur.instance, tcont);
				bad++;
				bad_add(cur.instance);
				continue;
			}
			p.tcont = (uint8_t)(tcont - GEM_TCONT_BASE);
			/* A port that only carries traffic downstream needs no T-CONT. */
		} else if (tcont != GEM_TCONT_NONE_A &&
			   tcont != GEM_TCONT_NONE_B) {
			OMCI_DEBUG(
				"me268: instance %#06x is downstream only and still points at T-CONT %#06x; ignoring the pointer",
				cur.instance, tcont);
		}

		for (size_t i = 0; i < n; i++)
			if (out[i].port_id == port_id)
				dup = true;
		if (dup) {
			OMCI_WARN(
				"me268: instance %#06x asks for Port-ID %u, which another instance already has",
				cur.instance, port_id);
			bad++;
			bad_add(cur.instance);
			continue;
		}

		if (n >= cap) {
			OMCI_WARN(
				"me268: more than %zu GEM ports — instance %#06x is NOT programmed",
				cap, cur.instance);
			bad++;
			bad_add(cur.instance);
			continue;
		}

		p.port_id = port_id;
		p.direction = dir;
		inst_out[n] = cur.instance;
		out[n++] = p;
	}

	if (bad_out != NULL)
		*bad_out = bad;
	return n;
}

enum omci_status omci_gem_program(const struct omci_mib *mib,
				  unsigned int *refused)
{
	struct omci_hw_gem_port ports[OMCI_HW_GEM_MAX_PORTS];
	uint16_t inst[OMCI_HW_GEM_MAX_PORTS] = { 0 };
	struct omci_hw_reject rejected = { 0 };
	unsigned int bad;
	size_t n;
	enum omci_status rc;

	g_bad_n = 0;
	bad = 0;
	n = gather(mib, ports, inst, ARRAY_SIZE(ports), &bad);

	if (refused != NULL)
		*refused = bad;

	rc = omci_hw()->gem_apply(ports, n, &rejected);

	for (size_t i = 0; i < n; i++)
		if (omci_hw_reject_has(&rejected, ports[i].port_id))
			bad_add(inst[i]);

	if (rc != OMCI_OK && rc != OMCI_DECLINED) {
		OMCI_WARN("me268: the MAC refused %zu GEM port(s): %s", n,
			  omci_status_str(rc));
		return rc;
	}

	for (size_t i = 0; i < n; i++)
		g_last[i] = ports[i];
	g_last_n = n;

	OMCI_DEBUG(
		"me268: %zu GEM port(s) programmed, %u refused, %u rejected by the MAC",
		n, bad, rejected.n);
	return rc;
}

bool omci_gem_instance_rejected(uint16_t instance)
{
	for (uint8_t i = 0; i < g_bad_n; i++)
		if (g_bad[i] == instance)
			return true;
	return false;
}

size_t omci_gem_last(const struct omci_hw_gem_port **out)
{
	if (out != NULL)
		*out = g_last;
	return g_last_n;
}
