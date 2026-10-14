// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

#include <string.h>

#include "stamp.h"

bool stamp_parse(const void *data, size_t len, ns_t *out)
{
	int64_t sec;
	int64_t nsec;

	if (len >= 2 * sizeof(int64_t)) {
		int64_t v[2];

		memcpy(v, data, sizeof(v));
		sec = v[0];
		nsec = v[1];
	} else if (len >= 2 * sizeof(int32_t)) {
		int32_t v[2];

		memcpy(v, data, sizeof(v));
		sec = v[0];
		nsec = v[1];
	} else {
		return false;
	}
	/* A stamp with no seconds is the kernel's way of saying it took none. */
	if (sec <= 0 || nsec < 0 || nsec >= NS_PER_SEC)
		return false;
	*out = sec * NS_PER_SEC + nsec;
	return true;
}
