// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Base64 of RFC 4648, the standard alphabet (base64.h). */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "base64.h"

static const char g_b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(char c)
{
	const char *p = c ? strchr(g_b64, c) : NULL;

	return p ? (int)(p - g_b64) : -1;
}

long base64_decode(const char *s, size_t n, uint8_t *out, size_t cap)
{
	size_t len = 0, pad = 0;

	if (!n || n % 4)
		return -1;
	if (s[n - 1] == '=')
		pad = s[n - 2] == '=' ? 2 : 1;
	if (n / 4 * 3 - pad > cap)
		return -1;
	for (size_t i = 0; i < n; i += 4) {
		int v[4];
		uint32_t w = 0;

		for (int j = 0; j < 4; j++) {
			bool padded = i + 4 == n && j >= 4 - (int)pad;

			v[j] = padded ? 0 : b64_value(s[i + j]);
			if (v[j] < 0)
				return -1;
			w = w << 6 | (uint32_t)v[j];
		}
		out[len++] = (uint8_t)(w >> 16);
		if (i + 4 < n || pad < 2)
			out[len++] = (uint8_t)(w >> 8);
		if (i + 4 < n || pad < 1)
			out[len++] = (uint8_t)w;
	}
	return (long)len;
}

size_t base64_encode(const uint8_t *in, size_t n, char *out, bool pad)
{
	size_t len = 0;

	for (size_t i = 0; i < n; i += 3) {
		uint32_t w = (uint32_t)in[i] << 16;
		size_t left = n - i;

		if (left > 1)
			w |= (uint32_t)in[i + 1] << 8;
		if (left > 2)
			w |= in[i + 2];
		out[len++] = g_b64[w >> 18 & 63];
		out[len++] = g_b64[w >> 12 & 63];
		if (left > 1)
			out[len++] = g_b64[w >> 6 & 63];
		else if (pad)
			out[len++] = '=';
		if (left > 2)
			out[len++] = g_b64[w & 63];
		else if (pad)
			out[len++] = '=';
	}
	out[len] = '\0';
	return len;
}
