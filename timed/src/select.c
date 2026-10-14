// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

#include <string.h>

#include "select.h"

/*
 * Samples in a row before a source is good. PTP gives one a second and its
 * first offsets are the noisiest; NTP's come through a filter of their delay,
 * and the OLT gives one time and no more.
 */
static const unsigned int need[SRC_COUNT] = {
	[SRC_NONE] = 0,
	[SRC_OLT] = 1,
	[SRC_NTP] = 1,
	[SRC_PTP] = 3,
};

void select_init(struct select *sel)
{
	memset(sel, 0, sizeof(*sel));
}

bool select_good(const struct select *sel, enum source src, ns_t now)
{
	const struct source_state *st = &sel->s[src];

	return src != SRC_NONE && st->seen && st->good >= need[src] && now <= st->valid_until;
}

enum source select_pick(struct select *sel, ns_t now)
{
	enum source src;
	enum source best = SRC_NONE;

	for (src = SRC_OLT; src < SRC_COUNT; src++) {
		struct source_state *st = &sel->s[src];

		/* A source that went stale starts its run of samples again. */
		if (st->seen && now > st->valid_until)
			st->good = 0;
		if (select_good(sel, src, now))
			best = src;
	}
	sel->cur = best;
	return sel->cur;
}

enum source select_sample(struct select *sel, enum source src, ns_t offset, ns_t now, ns_t hold)
{
	struct source_state *st = &sel->s[src];

	if (src == SRC_NONE || src >= SRC_COUNT)
		return sel->cur;
	/* A gap longer than the last sample held starts the run again. */
	if (!st->seen || now > st->valid_until)
		st->good = 0;
	st->seen = true;
	st->good++;
	st->offset = offset;
	st->last_at = now;
	st->valid_until = now + hold;
	return select_pick(sel, now);
}

void select_refresh(struct select *sel, enum source src, ns_t now, ns_t hold)
{
	if (src == SRC_NONE || src >= SRC_COUNT || !select_good(sel, src, now))
		return;
	sel->s[src].valid_until = now + hold;
}

const char *source_name(enum source src)
{
	switch (src) {
	case SRC_NONE:
		return "none";
	case SRC_OLT:
		return "olt";
	case SRC_NTP:
		return "ntp";
	case SRC_PTP:
		return "ptp";
	case SRC_COUNT:
		break;
	}
	return "?";
}
