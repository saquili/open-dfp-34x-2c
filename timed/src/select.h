/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Which source sets the clock. Only one does at a time, so that two never
 * pull the clock two ways: the best of those that are good, in the order PTP,
 * NTP, the OLT. A source is good once it has given enough samples in a row
 * and until its last one grows old; the one in use is dropped for a better
 * one as soon as that one is good, and for a worse one only when it is not.
 */

#ifndef TIMED_SELECT_H
#define TIMED_SELECT_H

#include "timed.h"

enum source {
	SRC_NONE = 0,
	SRC_OLT,
	SRC_NTP,
	SRC_PTP,
	SRC_COUNT
};

struct source_state {
	unsigned int good;	/* samples in a row, 0 once the source went stale */
	ns_t valid_until;	/* monotonic time the last sample holds until */
	ns_t offset;		/* of the last sample, for the status */
	ns_t last_at;
	bool seen;
};

struct select {
	struct source_state s[SRC_COUNT];
	enum source cur;
};

void select_init(struct select *sel);

/*
 * A sample from @src at monotonic time @now, good for @hold. Return the
 * source in use afterwards, which the caller compares with @src to know
 * whether to apply the sample.
 */
enum source select_sample(struct select *sel, enum source src, ns_t offset, ns_t now, ns_t hold);

/*
 * @src answered at @now but gave no sample to apply (NTP's filter kept an
 * older one as the best): a good source stays good for @hold more, without a
 * new offset. A source not good yet gains nothing.
 */
void select_refresh(struct select *sel, enum source src, ns_t now, ns_t hold);

/* Time passes: stale sources are dropped. Return the source in use. */
enum source select_pick(struct select *sel, ns_t now);

/* Whether @src is good at @now. */
bool select_good(const struct select *sel, enum source src, ns_t now);

const char *source_name(enum source src);

#endif
