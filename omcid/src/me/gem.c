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

/* The instance bit of an upstream priority queue (G.988 9.2.10). */
#define GEM_PRIQ_UPSTREAM 0x8000u

/* The values of a T-CONT's Alloc-ID that map it: twelve bits, but 0x00FF. */
#define GEM_ALLOC_UNSET 0x00ffu
#define GEM_ALLOC_MAX 4095u

static struct omci_hw_gem_port g_last[OMCI_HW_GEM_MAX_PORTS];
static size_t g_last_n;

/* The T-CONTs' Alloc-IDs in force, and the T-CONT instances refused. */
static uint16_t g_tcont_last[OMCI_HW_TCONT_MAX];
static size_t g_tcont_last_n;
static uint16_t g_tcont_bad[OMCI_HW_TCONT_MAX];
static uint8_t g_tcont_bad_n;

static uint16_t g_bad[OMCI_HW_GEM_MAX_PORTS * 2];
static uint8_t g_bad_n;

static void bad_add(uint16_t instance)
{
	if (g_bad_n < ARRAY_SIZE(g_bad))
		g_bad[g_bad_n++] = instance;
}

/* An attribute of @class_id as a big-endian number of @size bytes, 0 when absent. */
static uint32_t attr_of(const struct omci_mib *mib, uint16_t class_id,
			uint16_t instance, uint8_t attr, size_t size)
{
	uint8_t buf[4] = { 0 };
	uint32_t v = 0;
	size_t len = 0;

	(void)!omci_mib_attr_get(mib, class_id, instance, attr, buf, size,
				 &len);
	for (size_t i = 0; i < size; i++)
		v = v << 8 | buf[i];
	return v;
}

static uint16_t attr_u16(const struct omci_mib *mib, uint16_t instance,
			 uint8_t attr)
{
	return (uint16_t)attr_of(mib, OMCI_GEM_CLASS_ID, instance, attr, 2);
}

static uint8_t attr_u8(const struct omci_mib *mib, uint16_t instance,
		       uint8_t attr)
{
	return (uint8_t)attr_of(mib, OMCI_GEM_CLASS_ID, instance, attr, 1);
}

/* A pointer that names no instance: 0 or 0xFFFF, as the bridges read them. */
static bool null_pointer(uint16_t ptr)
{
	return ptr == 0 || ptr == 0xffffu;
}

/*
 * The upstream priority queues of T-CONT @tcont: those the ONU made for it,
 * which its related port names, at least one.
 */
static uint8_t tcont_queues(const struct omci_mib *mib, uint16_t tcont)
{
	struct omci_mib_cursor cur;
	unsigned int n = 0;
	enum omci_status rc;

	for (rc = omci_mib_walk_first(mib, &cur); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &cur))
		if (cur.class_id == OMCI_GEM_PRIQ_CLASS_ID &&
		    (cur.instance & GEM_PRIQ_UPSTREAM) &&
		    attr_of(mib, OMCI_GEM_PRIQ_CLASS_ID, cur.instance,
			    OMCI_GEM_PRIQ_ATTR_RELATED_PORT,
			    4) >> 16 ==
			    tcont)
			n++;
	return n ? (uint8_t)(n > UINT8_MAX ? UINT8_MAX : n) : 1u;
}

/*
 * The queue and weight of port @p, of instance @instance on T-CONT @tcont,
 * from the priority queue its traffic management pointer @tm names: the
 * queue's priority within its T-CONT, 0 the highest and at most the T-CONT's
 * last queue, and the queue's weight, at least 1, when the queue's traffic
 * scheduler, or the T-CONT when the queue has none, has the weighted round
 * robin policy. A pointer that names no upstream priority queue leaves queue
 * 0 and strict priority, and so does any pointer of a rate-controlled ONU, whose
 * pointer names the T-CONT (G.988 9.2.3), an instance number a priority
 * queue may have too.
 */
static void gem_queue(const struct omci_mib *mib, uint16_t instance,
		      uint16_t tm, uint16_t tcont, struct omci_hw_gem_port *p)
{
	uint32_t related;
	uint16_t owner, prio, sched;
	uint8_t queues, policy;

	if (attr_of(mib, OMCI_GEM_ONUG_CLASS_ID, 0,
		    OMCI_GEM_ONUG_ATTR_TM_OPTION,
		    1) == OMCI_GEM_TM_RATE_CONTROLLED ||
	    null_pointer(tm) || !(tm & GEM_PRIQ_UPSTREAM) ||
	    !omci_mib_exists(mib, OMCI_GEM_PRIQ_CLASS_ID, tm))
		return;

	related = attr_of(mib, OMCI_GEM_PRIQ_CLASS_ID, tm,
			  OMCI_GEM_PRIQ_ATTR_RELATED_PORT, 4);
	owner = (uint16_t)(related >> 16);
	prio = (uint16_t)related;
	if (owner != tcont)
		OMCI_WARN(
			"me268: instance %#06x sends through priority queue %#06x, which belongs to %#06x and not to its T-CONT %#06x; the queue's priority is kept",
			instance, tm, owner, tcont);
	queues = tcont_queues(mib, owner);
	p->queue = prio < queues ? (uint8_t)prio : (uint8_t)(queues - 1u);

	sched = (uint16_t)attr_of(mib, OMCI_GEM_PRIQ_CLASS_ID, tm,
				  OMCI_GEM_PRIQ_ATTR_SCHEDULER, 2);
	if (!null_pointer(sched) &&
	    omci_mib_exists(mib, OMCI_GEM_SCHED_CLASS_ID, sched))
		policy = (uint8_t)attr_of(mib, OMCI_GEM_SCHED_CLASS_ID, sched,
					  OMCI_GEM_SCHED_ATTR_POLICY, 1);
	else
		policy = (uint8_t)attr_of(mib, OMCI_GEM_TCONT_CLASS_ID, tcont,
					  OMCI_GEM_TCONT_ATTR_POLICY, 1);
	if (policy == OMCI_GEM_POLICY_WRR) {
		const uint8_t w =
			(uint8_t)attr_of(mib, OMCI_GEM_PRIQ_CLASS_ID, tm,
					 OMCI_GEM_PRIQ_ATTR_WEIGHT, 1);

		p->weight = w ? w : 1u;
	}
}

/*
 * The rates of port @p from the traffic descriptor @td names, in bytes per
 * second; none without one. G.988 uses the pointer for a rate-controlled ONU
 * and one controlled by both priority and rate (9.2.3); an OLT that points a
 * priority-controlled ONU's port at a descriptor still means its rates, so
 * they are taken whatever the option. A peak rate below the committed rate is
 * raised to it, since the committed rate is the one the OLT counts on.
 */
static void gem_rates(const struct omci_mib *mib, uint16_t td,
		      struct omci_hw_gem_port *p)
{
	if (null_pointer(td) || !omci_mib_exists(mib, OMCI_GEM_TD_CLASS_ID, td))
		return;

	p->cir =
		attr_of(mib, OMCI_GEM_TD_CLASS_ID, td, OMCI_GEM_TD_ATTR_CIR, 4);
	p->pir =
		attr_of(mib, OMCI_GEM_TD_CLASS_ID, td, OMCI_GEM_TD_ATTR_PIR, 4);
	if (p->pir && p->pir < p->cir) {
		OMCI_DEBUG(
			"me280: instance %#06x has a peak rate below its committed rate; the committed rate is taken for both",
			td);
		p->pir = p->cir;
	}
}

/*
 * The priorities the IEEE 802.1p mappers of the MIB send to GEM port network
 * CTP @instance, through the GEM interworking termination points their
 * priorities point at; all eight when no mapper leads to it.
 */
static uint8_t gem_pbits(const struct omci_mib *mib, uint16_t instance)
{
	struct omci_mib_cursor cur;
	bool mapped = false;
	uint8_t pbits = 0;
	enum omci_status rc;

	for (rc = omci_mib_walk_first(mib, &cur); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &cur)) {
		if (cur.class_id != OMCI_GEM_MAPPER_CLASS_ID)
			continue;
		for (uint8_t b = 0; b < 8u; b++) {
			const uint16_t iw = (uint16_t)attr_of(
				mib, OMCI_GEM_MAPPER_CLASS_ID, cur.instance,
				(uint8_t)(OMCI_GEM_MAPPER_ATTR_PBIT0 + b), 2);

			if (null_pointer(iw) ||
			    attr_of(mib, OMCI_GEM_IW_CLASS_ID, iw,
				    OMCI_GEM_IW_ATTR_CTP, 2) != instance ||
			    !omci_mib_exists(mib, OMCI_GEM_IW_CLASS_ID, iw))
				continue;
			pbits |= (uint8_t)(1u << b);
			mapped = true;
		}
	}
	return mapped ? pbits : 0xffu;
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
		struct omci_hw_gem_port p = { .pbits = 0xffu };
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
			gem_queue(mib, cur.instance,
				  attr_u16(mib, cur.instance,
					   OMCI_GEM_ATTR_TM_US),
				  tcont, &p);
			gem_rates(mib,
				  attr_u16(mib, cur.instance,
					   OMCI_GEM_ATTR_TD_US),
				  &p);
			p.pbits = gem_pbits(mib, cur.instance);
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

static void tcont_bad_add(uint16_t instance)
{
	if (g_tcont_bad_n < ARRAY_SIZE(g_tcont_bad))
		g_tcont_bad[g_tcont_bad_n++] = instance;
}

/* The Alloc-ID each T-CONT instance names, into @alloc. Return: how many T-CONTs. */
static size_t tcont_gather(const struct omci_mib *mib, uint16_t *alloc)
{
	struct omci_mib_cursor cur;
	enum omci_status rc;
	size_t n = 0;

	for (size_t i = 0; i < OMCI_HW_TCONT_MAX; i++)
		alloc[i] = OMCI_HW_ALLOC_NONE;
	for (rc = omci_mib_walk_first(mib, &cur); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &cur)) {
		size_t idx;
		uint16_t a;

		if (cur.class_id != OMCI_GEM_TCONT_CLASS_ID ||
		    cur.instance < GEM_TCONT_BASE ||
		    cur.instance - GEM_TCONT_BASE >= OMCI_HW_TCONT_MAX)
			continue;
		idx = cur.instance - GEM_TCONT_BASE;
		a = (uint16_t)attr_of(mib, OMCI_GEM_TCONT_CLASS_ID,
				      cur.instance, OMCI_GEM_TCONT_ATTR_ALLOC,
				      2);
		alloc[idx] = (a == GEM_ALLOC_UNSET || a > GEM_ALLOC_MAX) ?
				     (uint16_t)OMCI_HW_ALLOC_NONE :
				     a;
		if (idx >= n)
			n = idx + 1;
	}
	return n;
}

/* Whether T-CONT @idx is mapped to @alloc in force. */
static bool tcont_in_force(size_t idx, uint16_t alloc)
{
	return idx < g_tcont_last_n && g_tcont_last[idx] == alloc;
}

/* The T-CONTs' mapping (omci_gem_program()). Return: as the hardware's. */
static enum omci_status tcont_program(const struct omci_mib *mib)
{
	uint16_t alloc[OMCI_HW_TCONT_MAX];
	struct omci_hw_reject rejected = { 0 };
	bool refused = false;
	enum omci_status rc;
	size_t n;

	g_tcont_bad_n = 0;
	if (omci_hw()->tcont_apply == NULL)
		return OMCI_OK;
	n = tcont_gather(mib, alloc);

	for (size_t i = 0; i < n; i++) {
		for (size_t j = i + 1; j < n && alloc[i] != OMCI_HW_ALLOC_NONE;
		     j++) {
			const size_t lose = tcont_in_force(j, alloc[j]) ? i : j;

			if (alloc[j] != alloc[i])
				continue;
			OMCI_WARN(
				"me262: instance %#06zx asks for Alloc-ID %u, which instance %#06zx has; refused",
				GEM_TCONT_BASE + lose, alloc[lose],
				GEM_TCONT_BASE + (lose == i ? j : i));
			tcont_bad_add((uint16_t)(GEM_TCONT_BASE + lose));
			alloc[lose] = OMCI_HW_ALLOC_NONE;
			refused = true;
		}
	}

	rc = omci_hw()->tcont_apply(alloc, n, &rejected);
	for (size_t i = 0; i < n; i++)
		if (omci_hw_reject_has(&rejected, (uint16_t)i))
			tcont_bad_add((uint16_t)(GEM_TCONT_BASE + i));
	if (rc != OMCI_OK && rc != OMCI_DECLINED) {
		OMCI_WARN("me262: the MAC refused the T-CONTs' Alloc-IDs: %s",
			  omci_status_str(rc));
		return rc;
	}

	for (size_t i = 0; i < n; i++)
		g_tcont_last[i] = alloc[i];
	g_tcont_last_n = n;
	return refused ? OMCI_DECLINED : rc;
}

enum omci_status omci_gem_program(const struct omci_mib *mib,
				  unsigned int *refused)
{
	const enum omci_status tc = tcont_program(mib);

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
	/* The GEM ports' status is OK or a refusal here; the T-CONTs' may be worse. */
	if (tc != OMCI_OK && tc != OMCI_DECLINED)
		return tc;
	return rc == OMCI_DECLINED ? rc : tc;
}

bool omci_tcont_instance_rejected(uint16_t instance)
{
	for (uint8_t i = 0; i < g_tcont_bad_n; i++)
		if (g_tcont_bad[i] == instance)
			return true;
	return false;
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
