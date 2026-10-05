// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The alarm state of the daemon and the alarm notification. */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "hw/hw.h"
#include "omci/alarm.h"
#include "omci/ber.h"
#include "omci/optical.h"
#include "omcid/byteorder.h"
#include "omci/frame.h"
#include "sys/log.h"

#define OMCI_MT_ALARM 16u

#define ME_ANI_G OMCI_ALARM_ANI_G
#define ANI_G_INSTANCE OMCI_ALARM_ANI_G_INSTANCE

/* Marks a hardware alarm that no OMCI alarm reports. */
#define NO_OMCI_BIT 0xFFu

/*
 * Where each alarm of the hardware is reported: the entity and the bit of
 * its alarm bitmap. Only the ANI-G (class 263, G.988 9.2.1) defines bits for
 * the conditions the PON receiver sees: bit 0 for low received power (loss
 * of signal is that condition at its extreme), bit 2 for signal fail and
 * bit 3 for signal degrade, which omci_alarm_judge() declares from the BIP
 * errors when the hardware does not. Loss of frame and loss of the message
 * channel are conditions of the transmission convergence layer, and an
 * upstream that transmits too long or out of turn is detected by the OLT;
 * no alarm the ONU reports stands for them.
 */
/* The alarms the daemon judges itself from the transceiver's readings. */
enum {
	ALARM_RX_LOW = OMCI_HW_ALARM_COUNT,
	ALARM_RX_HIGH,
	ALARM_TX_LOW,
	ALARM_TX_HIGH,
	ALARM_BIAS_HIGH,
	ALARM_COUNT,
};

static_assert(ALARM_COUNT <= 32, "every alarm has a bit of the mask");

/*
 * The optical alarms (G.988 9.2.1): low and high received optical power,
 * bits 0 and 1, the first also standing for loss of signal; low and high
 * transmit optical power, bits 4 and 5; and the laser bias current over
 * its limit, bit 6.
 */
static const struct {
	uint16_t class_id;
	uint16_t instance;
	uint8_t bit;
} g_map[ALARM_COUNT] = {
	[OMCI_HW_ALARM_LOS] = { ME_ANI_G, ANI_G_INSTANCE, 0u },
	[OMCI_HW_ALARM_LOF] = { 0u, 0u, NO_OMCI_BIT },
	[OMCI_HW_ALARM_LOM] = { 0u, 0u, NO_OMCI_BIT },
	[OMCI_HW_ALARM_SF] = { ME_ANI_G, ANI_G_INSTANCE, 2u },
	[OMCI_HW_ALARM_SD] = { ME_ANI_G, ANI_G_INSTANCE, 3u },
	[OMCI_HW_ALARM_TX_TOO_LONG] = { 0u, 0u, NO_OMCI_BIT },
	[OMCI_HW_ALARM_TX_MISMATCH] = { 0u, 0u, NO_OMCI_BIT },
	[ALARM_RX_LOW] = { ME_ANI_G, ANI_G_INSTANCE, 0u },
	[ALARM_RX_HIGH] = { ME_ANI_G, ANI_G_INSTANCE, 1u },
	[ALARM_TX_LOW] = { ME_ANI_G, ANI_G_INSTANCE, 4u },
	[ALARM_TX_HIGH] = { ME_ANI_G, ANI_G_INSTANCE, 5u },
	[ALARM_BIAS_HIGH] = { ME_ANI_G, ANI_G_INSTANCE, 6u },
};

/*
 * An entity: its alarms, what the OLT was last told of them, and whether it
 * is under alarm-reporting control.
 */
struct entity_state {
	uint16_t class_id;
	uint16_t instance;
	uint8_t bitmap[OMCI_ALARM_BITMAP_LEN];
	uint8_t told[OMCI_ALARM_BITMAP_LEN];
	bool suppressed;
};

/*
 * The alarm state: the entities, the copy Get all alarms froze, the sequence
 * number of the next notification, the rate of BIP errors, the optical
 * alarms and the transceiver's thresholds and levels, the alarms judged
 * from them, as bits of the conditions of g_map, and the alarms raised
 * outside them, each an entity and a bit.
 */
struct omci_alarm {
	struct entity_state live[OMCI_ALARM_MAX_ENTITIES];
	uint8_t live_n;
	struct omci_alarm_entry snap[OMCI_ALARM_MAX_ENTITIES];
	uint16_t snap_n;
	uint8_t seq;
	struct omci_ber ber;
	struct omci_optical optical;
	struct omci_hw_optics_limits own;
	bool own_read;
	int16_t rx_level;
	int16_t tx_level;
	uint8_t levels;
	uint32_t judged;
	struct {
		uint16_t class_id;
		uint16_t instance;
		uint8_t bit;
	} raised[OMCI_ALARM_MAX_RAISED];
	uint8_t raised_n;
};

static struct omci_alarm g_alarm;
static bool g_alarm_taken;

/* The place of an entity's state, or -1 when it has none. */
static int entity_index(const struct omci_alarm *al, uint16_t class_id,
			uint16_t instance)
{
	for (unsigned int i = 0; i < al->live_n; i++)
		if (al->live[i].class_id == class_id &&
		    al->live[i].instance == instance)
			return (int)i;
	return -1;
}

/* The state of an entity, made on first use; NULL when there is no room. */
static struct entity_state *entity_for(struct omci_alarm *al, uint16_t class_id,
				       uint16_t instance)
{
	const int i = entity_index(al, class_id, instance);

	if (i >= 0)
		return &al->live[i];
	if (al->live_n == OMCI_ALARM_MAX_ENTITIES)
		return NULL;

	struct entity_state *e = &al->live[al->live_n++];

	memset(e, 0, sizeof(*e));
	e->class_id = class_id;
	e->instance = instance;
	return e;
}

/* Set alarm @n: alarm 0 is the most significant bit of the first byte. */
static void bitmap_set(uint8_t *map, uint8_t n)
{
	map[n / 8u] |= (uint8_t)(0x80u >> (n % 8u));
}

static bool bitmap_any(const uint8_t *map)
{
	for (unsigned int i = 0; i < OMCI_ALARM_BITMAP_LEN; i++)
		if (map[i] != 0)
			return true;
	return false;
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

/* Log what an alarm judged here became. */
static void say(bool was, bool is, const char *what)
{
	if (was != is)
		OMCI_INFO("alarm: %s %s", what, is ? "declared" : "cleared");
}

/* Signal fail and degrade, from the BIP errors of the receiver. */
static void judge_ber(struct omci_alarm *al, uint64_t now_sec,
		      const struct omci_hw_pon *pon, bool read,
		      const struct omci_alarm_thresholds *t)
{
	const bool was_sf = al->ber.sf, was_sd = al->ber.sd;

	if (!read || !pon->operating || !pon->bip_valid) {
		omci_ber_reset(&al->ber);
	} else {
		omci_ber_sample(&al->ber, now_sec, pon->bip_errors);
		omci_ber_judge(&al->ber, t->sf, t->sd);
	}

	if (al->ber.sf != was_sf)
		OMCI_INFO("alarm: signal fail %s (threshold 10^-%u)",
			  al->ber.sf ? "declared" : "cleared", t->sf);
	if (al->ber.sd != was_sd)
		OMCI_INFO("alarm: signal degrade %s (threshold 10^-%u)",
			  al->ber.sd ? "declared" : "cleared", t->sd);
}

/* The optical alarms and levels, from the readings of the transceiver. */
static void judge_optics(struct omci_alarm *al, const struct omci_hw_pon *pon,
			 bool read, const struct omci_alarm_thresholds *t)
{
	const struct omci_hw_ops *hw = omci_hw();

	if (!al->own_read && hw->optics_limits != NULL &&
	    hw->optics_limits(&al->own) == OMCI_OK)
		al->own_read = true;

	struct omci_hw_optics o;

	if (hw->optics(&o) != OMCI_OK)
		return;

	struct omci_optical_limits lim;
	const struct omci_optical was = al->optical;

	omci_optical_limits(&t->optical, al->own_read ? &al->own : NULL,
			    read ? pon : NULL, &lim);
	omci_optical_judge(&al->optical, &o, &lim, read && pon->operating);

	say(was.rx_low, al->optical.rx_low, "low received optical power");
	say(was.rx_high, al->optical.rx_high, "high received optical power");
	say(was.tx_low, al->optical.tx_low, "low transmit optical power");
	say(was.tx_high, al->optical.tx_high, "high transmit optical power");
	say(was.bias_high, al->optical.bias_high, "laser bias current");

	if ((o.valid & (1u << OMCI_HW_OPTIC_RX_POWER)) != 0u) {
		al->rx_level = omci_optical_level(o.rx_power);
		al->levels |= OMCI_ALARM_LEVEL_RX;
	}
	if ((o.valid & (1u << OMCI_HW_OPTIC_TX_POWER)) != 0u) {
		al->tx_level = omci_optical_level(o.tx_power);
		al->levels |= OMCI_ALARM_LEVEL_TX;
	}
}

static uint32_t bit_if(bool on, unsigned int condition)
{
	return on ? 1u << condition : 0u;
}

enum omci_status omci_alarm_judge(struct omci_alarm *al, uint64_t now_sec,
				  const struct omci_alarm_thresholds *t)
{
	if (al == NULL || t == NULL)
		return OMCI_ERR_INVAL;

	struct omci_hw_pon pon = { 0 };
	const enum omci_status rc =
		omci_hw()->pon != NULL ? omci_hw()->pon(&pon) : OMCI_ERR_HW;

	judge_ber(al, now_sec, &pon, rc == OMCI_OK, t);
	judge_optics(al, &pon, rc == OMCI_OK, t);

	al->judged = bit_if(al->ber.sf, OMCI_HW_ALARM_SF) |
		     bit_if(al->ber.sd, OMCI_HW_ALARM_SD) |
		     bit_if(al->optical.rx_low, ALARM_RX_LOW) |
		     bit_if(al->optical.rx_high, ALARM_RX_HIGH) |
		     bit_if(al->optical.tx_low, ALARM_TX_LOW) |
		     bit_if(al->optical.tx_high, ALARM_TX_HIGH) |
		     bit_if(al->optical.bias_high, ALARM_BIAS_HIGH);
	return rc;
}

uint8_t omci_alarm_levels(const struct omci_alarm *al, int16_t *rx, int16_t *tx)
{
	if (al == NULL || rx == NULL || tx == NULL)
		return 0u;
	if ((al->levels & OMCI_ALARM_LEVEL_RX) != 0u)
		*rx = al->rx_level;
	if ((al->levels & OMCI_ALARM_LEVEL_TX) != 0u)
		*tx = al->tx_level;
	return al->levels;
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
	mask |= al->judged;

	uint8_t next[OMCI_ALARM_MAX_ENTITIES][OMCI_ALARM_BITMAP_LEN];

	memset(next, 0, sizeof(next));
	for (unsigned int r = 0; r < al->raised_n; r++) {
		const struct entity_state *e = entity_for(
			al, al->raised[r].class_id, al->raised[r].instance);

		if (e != NULL)
			bitmap_set(next[e - al->live], al->raised[r].bit);
	}
	for (unsigned int a = 0; a < (unsigned int)ALARM_COUNT; a++) {
		if (g_map[a].bit == NO_OMCI_BIT || (mask & (1u << a)) == 0u)
			continue;

		const struct entity_state *e =
			entity_for(al, g_map[a].class_id, g_map[a].instance);

		if (e != NULL)
			bitmap_set(next[e - al->live], g_map[a].bit);
	}

	bool any = false;

	for (unsigned int i = 0; i < al->live_n; i++) {
		if (memcmp(al->live[i].bitmap, next[i],
			   OMCI_ALARM_BITMAP_LEN) == 0)
			continue;
		memcpy(al->live[i].bitmap, next[i], OMCI_ALARM_BITMAP_LEN);
		any = true;
	}

	/*
	 * An entity with no alarm, none told and no alarm-reporting control is
	 * forgotten, and its place serves another.
	 */
	for (unsigned int i = al->live_n; i-- > 0;) {
		const struct entity_state *s = &al->live[i];

		if (bitmap_any(s->bitmap) || bitmap_any(s->told) ||
		    s->suppressed)
			continue;
		memmove(&al->live[i], &al->live[i + 1u],
			(al->live_n - i - 1u) * sizeof(al->live[0]));
		al->live_n--;
	}

	if (changed != NULL)
		*changed = any;
	return OMCI_OK;
}

void omci_alarm_raised(struct omci_alarm *al, uint16_t class_id, uint8_t bit,
		       const uint16_t *instances, size_t n)
{
	if (al == NULL)
		return;
	for (unsigned int i = al->raised_n; i-- > 0;)
		if (al->raised[i].class_id == class_id &&
		    al->raised[i].bit == bit)
			al->raised[i] = al->raised[--al->raised_n];
	for (size_t k = 0; k < n && al->raised_n < OMCI_ALARM_MAX_RAISED; k++) {
		al->raised[al->raised_n].class_id = class_id;
		al->raised[al->raised_n].instance = instances[k];
		al->raised[al->raised_n].bit = bit;
		al->raised_n++;
	}
}

void omci_alarm_suppress(struct omci_alarm *al, uint16_t class_id,
			 uint16_t instance, bool suppressed)
{
	if (al == NULL)
		return;

	if (!suppressed) {
		const int i = entity_index(al, class_id, instance);

		if (i >= 0)
			al->live[i].suppressed = false;
		return;
	}

	struct entity_state *e = entity_for(al, class_id, instance);

	if (e != NULL)
		e->suppressed = true;
}

bool omci_alarm_standing(const struct omci_alarm *al, uint16_t class_id,
			 uint16_t instance)
{
	if (al == NULL)
		return false;

	const int i = entity_index(al, class_id, instance);

	return i >= 0 && bitmap_any(al->live[i].bitmap);
}

uint16_t omci_alarm_snapshot(struct omci_alarm *al, bool skip_suppressed)
{
	if (al == NULL)
		return 0;

	al->snap_n = 0;
	for (unsigned int i = 0; i < al->live_n; i++) {
		struct entity_state *s = &al->live[i];

		if (!bitmap_any(s->bitmap) ||
		    (skip_suppressed && s->suppressed)) {
			memset(s->told, 0, sizeof(s->told));
			continue;
		}

		struct omci_alarm_entry *e = &al->snap[al->snap_n++];

		e->class_id = s->class_id;
		e->instance = s->instance;
		memcpy(e->bitmap, s->bitmap, OMCI_ALARM_BITMAP_LEN);
		memcpy(s->told, s->bitmap, OMCI_ALARM_BITMAP_LEN);
	}
	return al->snap_n;
}

void omci_alarm_snapshot_drop(struct omci_alarm *al)
{
	if (al != NULL)
		al->snap_n = 0;
}

bool omci_alarm_snapshot_get(const struct omci_alarm *al, uint16_t seq,
			     struct omci_alarm_entry *out)
{
	if (al == NULL || out == NULL || seq >= al->snap_n)
		return false;
	*out = al->snap[seq];
	return true;
}

size_t omci_alarm_take_changes(struct omci_alarm *al,
			       struct omci_alarm_entry *out, size_t cap)
{
	if (al == NULL || out == NULL)
		return 0;

	size_t n = 0;

	for (unsigned int i = 0; i < al->live_n && n < cap; i++) {
		struct entity_state *s = &al->live[i];

		if (s->suppressed ||
		    memcmp(s->bitmap, s->told, OMCI_ALARM_BITMAP_LEN) == 0)
			continue;
		out[n].class_id = s->class_id;
		out[n].instance = s->instance;
		memcpy(out[n].bitmap, s->bitmap, OMCI_ALARM_BITMAP_LEN);
		memcpy(s->told, s->bitmap, OMCI_ALARM_BITMAP_LEN);
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
