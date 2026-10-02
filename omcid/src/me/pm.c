// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Performance monitoring of pm.h. */

#include <stdbool.h>
#include <string.h>

#include "hw/hw.h"
#include "me/pm.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "sys/log.h"

/* Ethernet frame performance monitoring history data, downstream and upstream. */
#define ME_ETH_PM_DS 321u
#define ME_ETH_PM_US 322u

#define PM_MAX_SOURCES 4u

/* An attribute and the switch counters whose sum it reports. */
struct pm_attr {
	uint8_t attr_no;
	uint8_t nsrc;
	uint16_t src[PM_MAX_SOURCES];
	const char *why;
};

/*
 * Downstream: what the switch sends out of the UNI. Attribute 5 counts all
 * frames, so it adds unicast, multicast and broadcast.
 */
static const struct pm_attr g_ds[] = {

	{ 3u, 1u, { 6u }, "ifOutDiscards" },
	{ 4u, 1u, { 5u }, "ifOutOctets" },
	{ 5u,
	  3u,
	  { 7u, 8u, 9u },
	  "ifOutUcastPkts + ifOutMulticastPkts + ifOutBroadcastPkts" },
	{ 6u, 1u, { 9u }, "ifOutBroadcastPkts" },
	{ 7u, 1u, { 8u }, "ifOutMulticastPkts" },
	{ 8u,
	  0u,
	  { 0 },
	  "none: a CRC error is counted where frames are received" },
	{ 9u, 1u, { 43u }, "etherStatsTxUndersizePkts" },
	{ 10u, 1u, { 44u }, "etherStatsTxOversizePkts" },
	{ 11u, 1u, { 45u }, "etherStatsTxPkts64Octets" },
	{ 12u, 1u, { 46u }, "etherStatsTxPkts65to127Octets" },
	{ 13u, 1u, { 47u }, "etherStatsTxPkts128to255Octets" },
	{ 14u, 1u, { 48u }, "etherStatsTxPkts256to511Octets" },
	{ 15u, 1u, { 49u }, "etherStatsTxPkts512to1023Octets" },
	{ 16u, 1u, { 50u }, "etherStatsTxPkts1024to1518Octets" },
};

/* Upstream: what the switch receives on the UNI. */
static const struct pm_attr g_us[] = {

	{ 3u, 1u, { 26u }, "etherStatsDropEvents" },
	{ 4u, 1u, { 0u }, "ifInOctets" },
	{ 5u,
	  3u,
	  { 1u, 2u, 3u },
	  "ifInUcastPkts + ifInMulticastPkts + ifInBroadcastPkts" },
	{ 6u, 1u, { 3u }, "ifInBroadcastPkts" },
	{ 7u, 1u, { 2u }, "ifInMulticastPkts" },
	{ 8u, 1u, { 35u }, "etherStatsCRCAlignErrors" },
	{ 9u, 1u, { 57u }, "etherStatsRxUndersizePkts" },
	{ 10u, 1u, { 59u }, "etherStatsRxOversizePkts" },
	{ 11u, 1u, { 60u }, "etherStatsRxPkts64Octets" },
	{ 12u, 1u, { 61u }, "etherStatsRxPkts65to127Octets" },
	{ 13u, 1u, { 62u }, "etherStatsRxPkts128to255Octets" },
	{ 14u, 1u, { 63u }, "etherStatsRxPkts256to511Octets" },
	{ 15u, 1u, { 64u }, "etherStatsRxPkts512to1023Octets" },
	{ 16u, 1u, { 65u }, "etherStatsRxPkts1024to1518Octets" },
};

/* The counters of the switch that feed no attribute, and why. */
static const struct {
	uint16_t idx;
	const char *why;
} g_unmapped[] = {
	{ 11u,
	  "dot1dTpPortInDiscards: a bridge port counter, not part of these entities" },
	{ 13u,
	  "dot3InPauseFrames: these entities have no pause frame attribute" },
	{ 14u,
	  "dot3OutPauseFrames: these entities have no pause frame attribute" },
	{ 18u,
	  "dot3StatsSingleCollisionFrames: collisions need a half-duplex link" },
	{ 19u,
	  "dot3StatsMultipleCollisionFrames: collisions need a half-duplex link" },
	{ 20u,
	  "dot3StatsDeferredTransmissions: collisions need a half-duplex link" },
	{ 21u, "dot3StatsLateCollisions: collisions need a half-duplex link" },
	{ 22u,
	  "dot3StatsExcessiveCollisions: collisions need a half-duplex link" },
	{ 24u,
	  "dot3StatsSymbolErrors: these entities have no symbol error attribute" },
	{ 25u,
	  "dot3ControlInUnknownOpcodes: these entities have no such attribute" },
	{ 32u,
	  "etherStatsFragments: these entities have no fragment attribute" },
	{ 33u, "etherStatsJabbers: these entities have no jabber attribute" },
	{ 34u,
	  "etherStatsCollisions: these entities have no collision attribute" },
	{ 51u,
	  "etherStatsTxPkts1519toMaxOctets: the size buckets end at 1518 bytes" },
	{ 52u,
	  "etherStatsTxBroadcastPkts: ifOutBroadcastPkts feeds the attribute" },
	{ 53u,
	  "etherStatsTxMulticastPkts: ifOutMulticastPkts feeds the attribute" },
	{ 66u,
	  "etherStatsRxPkts1519toMaxOctets: the size buckets end at 1518 bytes" },
	{ 67u, "inOamPduPkts: an EPON counter, which no GPON entity reports" },
	{ 68u, "outOamPduPkts: an EPON counter, which no GPON entity reports" },
};

struct pm_class {
	uint16_t class_id;
	const struct pm_attr *attr;
	size_t n;
};

static const struct pm_class g_class[] = {
	{ ME_ETH_PM_DS, g_ds, ARRAY_SIZE(g_ds) },
	{ ME_ETH_PM_US, g_us, ARRAY_SIZE(g_us) },
};

static const struct pm_class *class_of(uint16_t class_id)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(g_class); i++)
		if (g_class[i].class_id == class_id)
			return &g_class[i];
	return NULL;
}

bool omci_pm_owns(uint16_t class_id)
{
	return class_of(class_id) != NULL;
}

bool omci_pm_counter_accounted(unsigned int idx, const char **why)
{
	for (unsigned int c = 0; c < ARRAY_SIZE(g_class); c++)
		for (size_t a = 0; a < g_class[c].n; a++)
			for (unsigned int k = 0; k < g_class[c].attr[a].nsrc;
			     k++)
				if (g_class[c].attr[a].src[k] == idx) {
					if (why != NULL)
						*why = g_class[c].attr[a].why;
					return true;
				}

	for (unsigned int i = 0; i < ARRAY_SIZE(g_unmapped); i++)
		if (g_unmapped[i].idx == idx) {
			if (why != NULL)
				*why = g_unmapped[i].why;
			return true;
		}

	if (why != NULL)
		*why = NULL;
	return false;
}

/* The instances counted at once, and the attributes of each. */
#define PM_MAX_TRACKED 16u

#define PM_MAX_ATTR 17u

/*
 * The counts of an instance: the counter values at the start of the
 * interval in progress, and the counts of the last closed interval.
 */
struct pm_row {
	bool used;
	uint16_t class_id;
	uint16_t instance;
	uint64_t base[PM_MAX_ATTR];
	bool seeded[PM_MAX_ATTR];
	uint32_t prev[PM_MAX_ATTR];
};

struct omci_pm {
	struct omci_mib *mib;
	struct pm_row row[PM_MAX_TRACKED];
	uint64_t interval_start;
	uint8_t interval_end_time;
	bool started;

	uint64_t wall_sec;
	uint64_t wall_index;
	uint64_t rolled_index;
	bool wall_known;
};

static struct omci_pm g_pm;
static bool g_pm_taken;

#define UNI_SWITCH_PORT 0u

#define PON_SWITCH_PORT 2u

unsigned int omci_pm_port_of(uint16_t class_id, uint16_t instance)
{
	(void)class_id;
	(void)instance;
	return UNI_SWITCH_PORT;
}

static enum omci_status source_read(unsigned int port, const struct pm_attr *a,
				    uint64_t *out)
{
	*out = 0;
	if (a->nsrc == 0)
		return OMCI_OK;

	uint64_t total = 0;

	for (unsigned int i = 0; i < a->nsrc && i < PM_MAX_SOURCES; i++) {
		uint64_t v = 0;

		if (omci_hw()->counter(port, a->src[i], &v) != OMCI_OK)
			return OMCI_ERR_HW;
		total += v;
	}
	*out = total;
	return OMCI_OK;
}

static struct pm_row *row_for(struct omci_pm *pm, uint16_t class_id,
			      uint16_t instance)
{
	for (unsigned int i = 0; i < PM_MAX_TRACKED; i++)
		if (pm->row[i].used && pm->row[i].class_id == class_id &&
		    pm->row[i].instance == instance)
			return &pm->row[i];
	for (unsigned int i = 0; i < PM_MAX_TRACKED; i++) {
		if (!pm->row[i].used) {
			memset(&pm->row[i], 0, sizeof(pm->row[i]));
			pm->row[i].used = true;
			pm->row[i].class_id = class_id;
			pm->row[i].instance = instance;

			const struct pm_class *cls = class_of(class_id);

			if (cls != NULL) {
				const unsigned int port =
					omci_pm_port_of(class_id, instance);
				for (size_t a = 0; a < cls->n; a++) {
					const uint8_t no = cls->attr[a].attr_no;
					uint64_t now = 0;

					if (source_read(port, &cls->attr[a],
							&now) == OMCI_OK) {
						pm->row[i].base[no] = now;
						pm->row[i].seeded[no] = true;
					}
				}
			}
			return &pm->row[i];
		}
	}
	return NULL;
}

void omci_pm_forget(struct omci_pm *pm, uint16_t class_id, uint16_t instance)
{
	if (pm == NULL)
		return;
	for (unsigned int i = 0; i < PM_MAX_TRACKED; i++)
		if (pm->row[i].used && pm->row[i].class_id == class_id &&
		    pm->row[i].instance == instance)
			pm->row[i].used = false;
}

void omci_pm_forget_all(struct omci_pm *pm)
{
	if (pm == NULL)
		return;
	for (unsigned int i = 0; i < PM_MAX_TRACKED; i++)
		pm->row[i].used = false;
	pm->interval_end_time = 0;
	pm->started = false;
}

enum omci_status omci_pm_init(struct omci_pm **out, struct omci_mib *mib)
{
	if (out == NULL || mib == NULL)
		return OMCI_ERR_INVAL;
	if (g_pm_taken)
		return OMCI_ERR_BUSY;

	memset(&g_pm, 0, sizeof(g_pm));
	g_pm.mib = mib;
	g_pm_taken = true;
	*out = &g_pm;
	return OMCI_OK;
}

void omci_pm_fini(struct omci_pm *pm)
{
	if (pm == NULL)
		return;
	memset(pm, 0, sizeof(*pm));
	g_pm_taken = false;
}

/*
 * The count since the start of the interval; a counter read for the first
 * time only marks that start.
 *
 * Return: false for such a first reading.
 */
static bool seed_or_report(struct pm_row *r, uint8_t no, uint64_t now,
			   uint32_t *out)
{
	if (!r->seeded[no]) {
		r->base[no] = now;
		r->seeded[no] = true;
		*out = 0;
		return false;
	}
	*out = (uint32_t)(now - r->base[no]);
	return true;
}

/* Follow the time of day: the interval number is the quarter hour since 1970. */
static void adopt_window(struct omci_pm *pm, uint64_t wall_sec)
{
	pm->wall_sec = wall_sec;
	pm->wall_index = wall_sec / OMCI_PM_INTERVAL_SEC;
	pm->interval_end_time = (uint8_t)(pm->wall_index & 0xffu);
	pm->wall_known = true;
}

void omci_pm_set_wall(struct omci_pm *pm, uint64_t wall_sec)
{
	if (pm == NULL)
		return;

	if (wall_sec == 0u) {
		pm->wall_known = false;
		pm->wall_sec = 0;
		pm->rolled_index = 0;
		return;
	}

	if (!pm->wall_known)
		pm->rolled_index = wall_sec / OMCI_PM_INTERVAL_SEC;

	adopt_window(pm, wall_sec);
}

void omci_pm_resync(struct omci_pm *pm, uint64_t wall_sec)
{
	if (pm == NULL)
		return;
	if (wall_sec == 0u) {
		omci_pm_set_wall(pm, 0u);
		return;
	}

	adopt_window(pm, wall_sec);

	pm->rolled_index = pm->wall_index;
}

bool omci_pm_tick(struct omci_pm *pm, uint64_t now_sec)
{
	if (pm == NULL)
		return false;

	if (!pm->started) {
		pm->started = true;
		pm->interval_start = now_sec;
		return false;
	}

	if (pm->wall_known) {
		if (pm->wall_sec / OMCI_PM_INTERVAL_SEC <= pm->rolled_index)
			return false;
	} else if (now_sec - pm->interval_start < OMCI_PM_INTERVAL_SEC) {
		return false;
	}

	for (unsigned int i = 0; i < PM_MAX_TRACKED; i++) {
		struct pm_row *r = &pm->row[i];

		if (!r->used)
			continue;
		const struct pm_class *cls = class_of(r->class_id);

		const unsigned int port =
			omci_pm_port_of(r->class_id, r->instance);
		for (size_t a = 0; a < cls->n; a++) {
			const uint8_t no = cls->attr[a].attr_no;
			uint64_t now = 0;

			if (source_read(port, &cls->attr[a], &now) != OMCI_OK)
				continue;
			uint32_t elapsed = 0;
			(void)seed_or_report(r, no, now, &elapsed);
			r->prev[no] = elapsed;
			r->base[no] = now;
			r->seeded[no] = true;
		}
	}

	pm->interval_start = now_sec;

	if (pm->wall_known)
		pm->rolled_index = pm->wall_sec / OMCI_PM_INTERVAL_SEC;
	else
		pm->interval_end_time = (uint8_t)(pm->interval_end_time + 1u);
	return true;
}

enum omci_status omci_pm_refresh(struct omci_pm *pm, uint16_t class_id,
				 uint16_t instance, bool current)
{
	if (pm == NULL)
		return OMCI_ERR_INVAL;

	const struct pm_class *cls = class_of(class_id);

	if (cls == NULL)
		return OMCI_ERR_ME_UNKNOWN;

	if (!omci_mib_exists(pm->mib, class_id, instance))
		return OMCI_ERR_INST_UNKNOWN;

	struct pm_row *r = row_for(pm, class_id, instance);

	if (r == NULL) {
		OMCI_WARN("pm: no room to track %u/%u", class_id, instance);
		return OMCI_ERR_NOMEM;
	}

	const unsigned int port = omci_pm_port_of(class_id, instance);
	enum omci_status rc = OMCI_OK;

	/* Attribute 1, the interval end time: the interval number modulo 256. */
	(void)!omci_mib_attr_set(pm->mib, class_id, instance, 1u,
				 &pm->interval_end_time, 1u);

	for (size_t a = 0; a < cls->n; a++) {
		const uint8_t no = cls->attr[a].attr_no;

		if (no >= PM_MAX_ATTR)
			continue;

		uint32_t value;

		if (current) {
			uint64_t now = 0;

			if (source_read(port, &cls->attr[a], &now) != OMCI_OK) {
				rc = OMCI_ERR_HW;
				continue;
			}
			if (!seed_or_report(r, no, now, &value))
				rc = OMCI_DECLINED;
		} else {
			value = r->prev[no];
		}

		uint8_t wire[4];

		omci_be32_store(wire, value);
		(void)!omci_mib_attr_set(pm->mib, class_id, instance, no, wire,
					 sizeof(wire));
	}

	return rc;
}
