// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Signal fail and signal degrade from the BIP errors of the downstream. */

#include <stdbool.h>
#include <string.h>

#include "omci/ber.h"

#define SLOTS (OMCI_BER_HISTORY + 1u)

/* The downstream line rate, in bit/s (G.984.2). */
#define LINE_BPS 2488320000ull

/*
 * The window over which a rate of 10^-k is judged, in seconds: one down to
 * 10^-8, ten times longer for each decade below. At the rate judged it
 * holds about 25 errors, enough that a few errors in a quiet second do not
 * declare anything.
 */
static unsigned int window_of(unsigned int k)
{
	unsigned int secs = 1u;

	for (unsigned int i = 8u; i < k; i++)
		secs *= 10u;
	return secs;
}

/* The errors in that window at which the rate reaches 10^-k, rounded up. */
static uint64_t errors_at(unsigned int k)
{
	const uint64_t bits = LINE_BPS * window_of(k);
	uint64_t per = 1u;

	for (unsigned int i = 0; i < k; i++)
		per *= 10u;
	return (bits + per - 1u) / per;
}

void omci_ber_reset(struct omci_ber *b)
{
	memset(b, 0, sizeof(*b));
}

/* Start the history again at @total, keeping the verdicts. */
static void restart(struct omci_ber *b, uint64_t sec, uint64_t total)
{
	b->head = 0;
	b->total[0] = total;
	b->count = 1;
	b->sec = sec;
}

static void push(struct omci_ber *b, uint64_t total)
{
	b->head = (b->head + 1u) % SLOTS;
	b->total[b->head] = total;
	if (b->count < SLOTS)
		b->count++;
}

void omci_ber_sample(struct omci_ber *b, uint64_t sec, uint64_t total)
{
	if (b->count == 0u || sec < b->sec || total < b->total[b->head] ||
	    sec - b->sec > OMCI_BER_HISTORY) {
		restart(b, sec, total);
		return;
	}

	if (sec == b->sec) {
		b->total[b->head] = total;
		return;
	}

	/* The seconds skipped had no error; all of them fall in this one. */
	const uint64_t last = b->total[b->head];

	for (uint64_t s = b->sec + 1u; s < sec; s++)
		push(b, last);
	push(b, total);
	b->sec = sec;
}

bool omci_ber_errors(const struct omci_ber *b, unsigned int secs, uint64_t *out)
{
	if (secs == 0u || secs >= b->count)
		return false;

	*out = b->total[b->head] - b->total[(b->head + SLOTS - secs) % SLOTS];
	return true;
}

/* Whether the rate has reached 10^-k. */
static bool reached(const struct omci_ber *b, unsigned int k)
{
	uint64_t e;

	return omci_ber_errors(b, window_of(k), &e) && e >= errors_at(k);
}

/* Whether the rate is known to be below 10^-k. */
static bool below(const struct omci_ber *b, unsigned int k)
{
	uint64_t e;

	return omci_ber_errors(b, window_of(k), &e) && e < errors_at(k);
}

/* A verdict at threshold 10^-k: declared at it, cleared a decade below it. */
static bool verdict(const struct omci_ber *b, bool stands, unsigned int k)
{
	return stands ? !below(b, k + 1u) : reached(b, k);
}

void omci_ber_judge(struct omci_ber *b, uint8_t sf, uint8_t sd)
{
	if (sf < OMCI_BER_SF_MIN || sf > OMCI_BER_SF_MAX)
		sf = OMCI_BER_SF_DEFAULT;
	if (sd < OMCI_BER_SD_MIN || sd > OMCI_BER_SD_MAX)
		sd = OMCI_BER_SD_DEFAULT;

	b->sf = verdict(b, b->sf, sf);
	b->sd = verdict(b, b->sd, sd);
}
