// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

#include "olt.h"
#include "pon.h"

bool pon_tod_parse(const char *text, size_t len, struct pon_tod *t)
{
	int64_t frame, sec, nsec, at;

	/* The frame counter's 30 bits, seconds up to the year 2200, and a monotonic time that fits in ns_t. */
	if (!olt_number(text, len, "superframe", 0x3fffffff, &frame) ||
	    !olt_number(text, len, "sec", 7258118400LL, &sec) ||
	    !olt_number(text, len, "nsec", NS_PER_SEC - 1, &nsec) ||
	    !olt_number(text, len, "monotonic_ns", INT64_MAX, &at))
		return false;
	t->superframe = (uint32_t)frame;
	t->tstamp = sec * NS_PER_SEC + nsec;
	t->at = at;
	return true;
}

void pon_reset(struct pon *p)
{
	p->anchored = false;
}

enum pon_second pon_second(struct pon *p, ns_t t, ns_t *offset)
{
	ns_t since, n, off;

	if (!p->anchored) {
		p->anchored = true;
		p->anchor = t;
		return PON_ANCHOR;
	}
	since = t - p->anchor;
	n = (since + NS_PER_SEC / 2) / NS_PER_SEC;
	off = since - n * NS_PER_SEC;
	if (since <= 0 || off > PON_JUMP || off < -PON_JUMP) {
		p->anchor = t;
		return PON_JUMPED;
	}
	*offset = off;
	return PON_OFFSET;
}
