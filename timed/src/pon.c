// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

#include "pon.h"

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
