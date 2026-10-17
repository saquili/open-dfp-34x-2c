// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The conversation with the OLT (history.h). */

#include <stddef.h>
#include <string.h>

#include "me/table.h"
#include "omcid/byteorder.h"
#include "sys/history.h"
#include "sys/publish.h"

/* The ONU-G, and its attribute that holds the logical password. */
#define ONU_G 256u
#define LOGICAL_PASSWORD 11u

/* The message types that carry an ONU-G's values (G.988 11.2.2). */
#define MT_SET 8u
#define MT_GET 9u
#define MT_MIB_UPLOAD_NEXT 14u
#define MT_AVC 17u

/* The type byte: the acknowledgement bit and the message type. */
#define TYPE_AK 0x20u
#define TYPE_MT 0x1fu

/* Where a frame's class and contents start (G.988 11.2.1). */
#define AT_CLASS 4u
#define AT_CONTENTS 8u

/*
 * The room for values after the mask: of a Set and an AVC, of a Get
 * response before its two closing masks, and of a MIB upload next response
 * after the class and the instance it carries (G.988 A.3).
 */
#define SET_VALUES (OMCI_CONTENTS_LEN - 2u)
#define GET_VALUES 25u
#define UPLOAD_VALUES (OMCI_CONTENTS_LEN - 6u)

static uint16_t attr_bit(uint8_t attr)
{
	return (uint16_t)(0x8000u >> (attr - 1u));
}

/*
 * Zero the logical password among the ONU-G's values of @mask, which start
 * at @v and take at most @room bytes: what it holds of the password when
 * the values run past the room. Return whether anything was zeroed.
 */
static bool zero_password(uint8_t *v, size_t room, uint16_t mask)
{
	const struct omci_me_class *cls = omci_me_class_by_id(ONU_G);
	size_t at = 0;

	if ((mask & attr_bit(LOGICAL_PASSWORD)) == 0u)
		return false;
	for (uint8_t attr = 1; attr < LOGICAL_PASSWORD; attr++)
		if ((mask & attr_bit(attr)) != 0u)
			at += omci_me_attr_get(cls, attr)->size;
	if (at >= room)
		return false;

	const size_t size = omci_me_attr_get(cls, LOGICAL_PASSWORD)->size;

	memset(v + at, 0, size < room - at ? size : room - at);
	return true;
}

/* Zero the logical password wherever a frame carries the ONU-G's values. */
static bool mask_password(uint8_t frame[static OMCI_FRAME_LEN])
{
	const uint8_t mt = frame[2] & TYPE_MT;
	const bool ak = (frame[2] & TYPE_AK) != 0u;
	uint8_t *c = &frame[AT_CONTENTS];

	if (mt == MT_MIB_UPLOAD_NEXT && ak && omci_be16(c) == ONU_G)
		return zero_password(&c[6], UPLOAD_VALUES, omci_be16(&c[4]));
	if (omci_be16(&frame[AT_CLASS]) != ONU_G)
		return false;
	if ((mt == MT_SET && !ak) || mt == MT_AVC)
		return zero_password(&c[2], SET_VALUES, omci_be16(c));
	if (mt == MT_GET && ak)
		return zero_password(&c[3], GET_VALUES, omci_be16(&c[1]));
	return false;
}

void omci_history_add(struct omci_history *h, uint32_t sec, uint16_t ms,
		      uint8_t verdict, uint8_t type,
		      const uint8_t frame[static OMCI_FRAME_LEN])
{
	struct omci_history_rec *r = &h->rec[h->head % OMCI_HISTORY_LEN];

	r->sec = sec;
	r->ms = ms;
	r->verdict = verdict;
	r->type = type;
	memcpy(r->frame, frame, OMCI_FRAME_LEN);
	r->masked = mask_password(r->frame);
	h->head++;
}

static void write_rec(uint32_t seq, const struct omci_history_rec *r,
		      const struct omci_publish_ops *ops, void *ctx)
{
	static const char digit[] = "0123456789abcdef";
	char hex[2u * OMCI_FRAME_LEN + 1u];

	for (size_t i = 0; i < OMCI_FRAME_LEN; i++) {
		hex[2u * i] = digit[r->frame[i] >> 4];
		hex[2u * i + 1u] = digit[r->frame[i] & 15u];
	}
	hex[2u * OMCI_FRAME_LEN] = '\0';

	ops->open(ctx, NULL, false);
	ops->num(ctx, "seq", seq);
	ops->num(ctx, "sec", r->sec);
	ops->num(ctx, "ms", r->ms);
	ops->num(ctx, "verdict", r->verdict);
	ops->num(ctx, "type", r->type);
	ops->flag(ctx, "masked", r->masked);
	ops->str(ctx, "frame", hex);
	ops->close(ctx, false);
}

void omci_publish_frames(const struct omci_history *h, uint32_t since,
			 uint32_t max, const struct omci_publish_ops *ops,
			 void *ctx)
{
	const uint32_t first =
		h->head > OMCI_HISTORY_LEN ? h->head - OMCI_HISTORY_LEN : 0u;
	const uint32_t from = since < first || since > h->head ? first : since;
	uint32_t n = h->head - from;

	if (max == 0u || max > OMCI_HISTORY_PAGE)
		max = OMCI_HISTORY_PAGE;
	if (n > max)
		n = max;

	ops->num(ctx, "head", h->head);
	ops->num(ctx, "first", first);
	ops->open(ctx, "frames", true);
	for (uint32_t seq = from; seq < from + n; seq++)
		write_rec(seq, &h->rec[seq % OMCI_HISTORY_LEN], ops, ctx);
	ops->close(ctx, true);
}
