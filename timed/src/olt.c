// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

#include <string.h>

#include "olt.h"

bool olt_number(const char *text, size_t len, const char *key, int64_t max, int64_t *out)
{
	size_t klen = strlen(key);
	size_t i = 0;

	while (i < len) {
		size_t end = i;

		while (end < len && text[end] != '\n')
			end++;
		if (end - i > klen && !memcmp(text + i, key, klen) && text[i + klen] == '=') {
			int64_t v = 0;
			size_t j = i + klen + 1;

			if (j == end)
				return false;
			for (; j < end; j++) {
				if (text[j] < '0' || text[j] > '9' || v > (max - (text[j] - '0')) / 10)
					return false;
				v = v * 10 + (text[j] - '0');
			}
			*out = v;
			return true;
		}
		i = end + 1;
	}
	return false;
}

bool olt_parse(const char *text, size_t len, ns_t *utc, ns_t *at)
{
	int64_t sec, mono;

	/* Seconds up to the year 2200, as omcid takes them, and a monotonic time that fits in ns_t. */
	if (!olt_number(text, len, "utc", 7258118400LL, &sec) || !olt_number(text, len, "monotonic_ns", INT64_MAX, &mono))
		return false;
	*utc = sec * NS_PER_SEC;
	*at = mono;
	return true;
}

ns_t olt_offset(ns_t utc, ns_t at, ns_t real, ns_t mono)
{
	return real - (utc + (mono - at));
}
