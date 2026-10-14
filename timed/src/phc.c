// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

#include "phc.h"

bool phc_sys_offset(const struct phc_reading *r, ns_t *offset, ns_t *window)
{
	bool found = false;
	unsigned int i;
	ns_t best = 0;

	for (i = 0; i < r->n && i < PHC_SAMPLES; i++) {
		ns_t w = r->sys[i + 1] - r->sys[i];

		/* A system clock stepped between two readings gives a pair out of order. */
		if (w < 0 || (found && w >= best))
			continue;
		found = true;
		best = w;
		*offset = r->sys[i] + w / 2 - r->phc[i];
	}
	if (found)
		*window = best;
	return found;
}
