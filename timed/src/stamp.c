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

bool stamp_parse_hw(const void *data, size_t len, ns_t *out)
{
	const unsigned char *p = data;

	/* The third of three: after two stamps of 16 bytes, or of 8. */
	if (len >= 6 * sizeof(int64_t))
		return stamp_parse(p + 4 * sizeof(int64_t), 2 * sizeof(int64_t), out);
	if (len >= 6 * sizeof(int32_t))
		return stamp_parse(p + 4 * sizeof(int32_t), 2 * sizeof(int32_t), out);
	return false;
}
