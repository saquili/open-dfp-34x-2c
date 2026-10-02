// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The alarm state of the daemon and the alarm notification. */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "hw/hw.h"
#include "omci/alarm.h"
#include "omcid/byteorder.h"
#include "omci/frame.h"
#include "sys/log.h"

#define OMCI_MT_ALARM 16u

/* The ANI-G and its one instance, the PON port. */
#define ME_ANI_G 263u

#define ANI_G_INSTANCE 0x8001u

/* Marks a hardware alarm that no OMCI alarm reports. */
#define NO_OMCI_BIT 0xFFu

/*
 * Where each alarm of the hardware is reported: the entity and the bit of
 * its alarm bitmap. Only the ANI-G (class 263, G.988 9.2.1) defines bits for
 * the conditions the PON receiver sees: bit 0 for low received power (loss
 * of signal is that condition at its extreme), bit 2 for signal fail and
 * bit 3 for signal degrade. Loss of frame and loss of the message channel
 * are conditions of the transmission convergence layer, and an upstream
 * that transmits too long or out of turn is detected by the OLT; no alarm
 * the ONU reports stands for them.
 */
static const struct {
	uint16_t class_id;
	uint16_t instance;
	uint8_t bit;
} g_map[OMCI_HW_ALARM_COUNT] = {
	[OMCI_HW_ALARM_LOS] = { ME_ANI_G, ANI_G_INSTANCE, 0u },
	[OMCI_HW_ALARM_LOF] = { 0u, 0u, NO_OMCI_BIT },
	[OMCI_HW_ALARM_LOM] = { 0u, 0u, NO_OMCI_BIT },
	[OMCI_HW_ALARM_SF] = { ME_ANI_G, ANI_G_INSTANCE, 2u },
	[OMCI_HW_ALARM_SD] = { ME_ANI_G, ANI_G_INSTANCE, 3u },
	[OMCI_HW_ALARM_TX_TOO_LONG] = { 0u, 0u, NO_OMCI_BIT },
	[OMCI_HW_ALARM_TX_MISMATCH] = { 0u, 0u, NO_OMCI_BIT },
};

struct entity_state {
	uint16_t class_id;
	uint16_t instance;
	uint8_t bitmap[OMCI_ALARM_BITMAP_LEN];
	bool used;
	bool changed;
};

struct omci_alarm {
	struct entity_state live[OMCI_ALARM_MAX_ENTITIES];
	uint8_t live_n;
	struct omci_alarm_entry snap[OMCI_ALARM_MAX_ENTITIES];
	uint16_t snap_n;
	uint8_t seq;
};

static struct omci_alarm g_alarm;
static bool g_alarm_taken;

static_assert((unsigned int)OMCI_HW_ALARM_COUNT <= OMCI_ALARM_MAX_ENTITIES,
	      "every alarm can have its own entity");

/* The state of an entity, made on first use. */
static struct entity_state *entity_for(struct omci_alarm *al, uint16_t class_id,
				       uint16_t instance)
{
	for (unsigned int i = 0; i < al->live_n; i++)
		if (al->live[i].class_id == class_id &&
		    al->live[i].instance == instance)
			return &al->live[i];

	struct entity_state *e = &al->live[al->live_n++];

	e->used = true;
	e->class_id = class_id;
	e->instance = instance;
	memset(e->bitmap, 0, sizeof(e->bitmap));
	return e;
}

/* Set alarm @n: alarm 0 is the most significant bit of the first byte. */
static void bitmap_set(uint8_t *map, uint8_t n)
{
	map[n / 8u] |= (uint8_t)(0x80u >> (n % 8u));
}

enum omci_status omci_alarm_init(struct omci_alarm **out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	if (g_alarm_taken)
		return OMCI_ERR_BUSY;

	memset(&g_alarm, 0, sizeof(g_alarm));
	g_alarm.seq = 1u;
	g_alarm_taken = true;
	*out = &g_alarm;
	return OMCI_OK;
}

void omci_alarm_fini(struct omci_alarm *al)
{
	if (al == NULL)
		return;
	memset(al, 0, sizeof(*al));
	g_alarm_taken = false;
}

enum omci_status omci_alarm_poll(struct omci_alarm *al, bool *changed)
{
	if (al == NULL)
		return OMCI_ERR_INVAL;
	if (changed != NULL)
		*changed = false;

	uint32_t mask = 0;
	const enum omci_status rc = omci_hw()->alarms(&mask);

	if (rc != OMCI_OK)
		return rc;

	struct entity_state next[OMCI_ALARM_MAX_ENTITIES];

	memcpy(next, al->live, sizeof(next));
	for (unsigned int i = 0; i < OMCI_ALARM_MAX_ENTITIES; i++)
		memset(next[i].bitmap, 0, sizeof(next[i].bitmap));

	for (unsigned int a = 0; a < (unsigned int)OMCI_HW_ALARM_COUNT; a++) {
		if (g_map[a].bit == NO_OMCI_BIT)
			continue;
		if ((mask & (1u << a)) == 0u)
			continue;

		struct entity_state *e =
			entity_for(al, g_map[a].class_id, g_map[a].instance);

		const size_t idx = (size_t)(e - al->live);

		next[idx].used = true;
		next[idx].class_id = e->class_id;
		next[idx].instance = e->instance;
		bitmap_set(next[idx].bitmap, g_map[a].bit);
	}

	bool any = false;

	for (unsigned int i = 0; i < OMCI_ALARM_MAX_ENTITIES; i++) {
		if (!al->live[i].used)
			continue;
		const bool moved = memcmp(al->live[i].bitmap, next[i].bitmap,
					  OMCI_ALARM_BITMAP_LEN) != 0;
		al->live[i].changed = moved;
		if (moved) {
			memcpy(al->live[i].bitmap, next[i].bitmap,
			       OMCI_ALARM_BITMAP_LEN);
			any = true;
		}
	}

	if (changed != NULL)
		*changed = any;
	return OMCI_OK;
}

static bool bitmap_any(const uint8_t *map)
{
	for (unsigned int i = 0; i < OMCI_ALARM_BITMAP_LEN; i++)
		if (map[i] != 0)
			return true;
	return false;
}

uint16_t omci_alarm_snapshot(struct omci_alarm *al)
{
	if (al == NULL)
		return 0;

	al->snap_n = 0;
	for (unsigned int i = 0; i < OMCI_ALARM_MAX_ENTITIES; i++) {
		if (!al->live[i].used || !bitmap_any(al->live[i].bitmap))
			continue;
		struct omci_alarm_entry *e = &al->snap[al->snap_n];

		e->class_id = al->live[i].class_id;
		e->instance = al->live[i].instance;
		memcpy(e->bitmap, al->live[i].bitmap, OMCI_ALARM_BITMAP_LEN);
		al->snap_n++;
	}
	return al->snap_n;
}

bool omci_alarm_snapshot_get(const struct omci_alarm *al, uint16_t seq,
			     struct omci_alarm_entry *out)
{
	if (al == NULL || out == NULL || seq >= al->snap_n)
		return false;
	*out = al->snap[seq];
	return true;
}

size_t omci_alarm_changed(const struct omci_alarm *al,
			  struct omci_alarm_entry *out, size_t cap)
{
	if (al == NULL || out == NULL)
		return 0;

	size_t n = 0;

	for (unsigned int i = 0; i < OMCI_ALARM_MAX_ENTITIES && n < cap; i++) {
		if (!al->live[i].used || !al->live[i].changed)
			continue;
		out[n].class_id = al->live[i].class_id;
		out[n].instance = al->live[i].instance;
		memcpy(out[n].bitmap, al->live[i].bitmap,
		       OMCI_ALARM_BITMAP_LEN);
		n++;
	}
	return n;
}

void omci_alarm_build_notification(struct omci_alarm *al,
				   const struct omci_alarm_entry *entry,
				   uint8_t out[static OMCI_FRAME_LEN])
{
	struct omci_frame f = {
		.tci = 0,
		.msg_type = OMCI_MT_ALARM,
		.ar = false,
		.ak = false,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = entry->class_id,
		.instance = entry->instance,
	};

	/*
	 * The contents: the bitmap, three reserved bytes and the alarm sequence
	 * number, which runs from 1 to 255 and then starts again at 1 (G.988
	 * A.1.4.1, A.3.19).
	 */
	memcpy(&f.contents[0], entry->bitmap, OMCI_ALARM_BITMAP_LEN);
	f.contents[28] = 0;
	f.contents[29] = 0;
	f.contents[30] = 0;
	f.contents[31] = al->seq;

	al->seq = (al->seq == 255u) ? 1u : (uint8_t)(al->seq + 1u);

	(void)!omci_frame_build(&f, out);
}

void omci_alarm_reset_sequence(struct omci_alarm *al)
{
	if (al != NULL)
		al->seq = 1u;
}

uint8_t omci_alarm_sequence(const struct omci_alarm *al)
{
	return al == NULL ? 0u : al->seq;
}
