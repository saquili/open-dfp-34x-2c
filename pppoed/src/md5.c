// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * MD5 (RFC 1321), written from the RFC's description of the algorithm, for
 * the CHAP response and nothing else; md5_selftest() checks it against the
 * RFC's test suite.
 */

#include <string.h>

#include "pppoed.h"

struct md5_ctx {
	uint32_t state[4];
	uint64_t bits;
	uint8_t buf[64];
	size_t used;
};

static uint32_t rol(uint32_t x, unsigned int n)
{
	return (x << n) | (x >> (32 - n));
}

static uint32_t f_f(uint32_t x, uint32_t y, uint32_t z)
{
	return (x & y) | (~x & z);
}
static uint32_t f_g(uint32_t x, uint32_t y, uint32_t z)
{
	return (x & z) | (y & ~z);
}
static uint32_t f_h(uint32_t x, uint32_t y, uint32_t z)
{
	return x ^ y ^ z;
}
static uint32_t f_i(uint32_t x, uint32_t y, uint32_t z)
{
	return y ^ (x | ~z);
}

static const uint32_t T[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
	0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
	0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
	0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
	0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
	0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
	0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
	0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
	0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const unsigned int S[64] = { 7,	12, 17, 22, 7,	12, 17, 22, 7,	12, 17,
				    22, 7,  12, 17, 22, 5,  9,	14, 20, 5,  9,
				    14, 20, 5,	9,  14, 20, 5,	9,  14, 20, 4,
				    11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
				    4,	11, 16, 23, 6,	10, 15, 21, 6,	10, 15,
				    21, 6,  10, 15, 21, 6,  10, 15, 21 };

static void md5_block(struct md5_ctx *c, const uint8_t *p)
{
	uint32_t m[16], a = c->state[0], b = c->state[1];
	uint32_t cc = c->state[2], d = c->state[3];
	unsigned int i;

	for (i = 0; i < 16; i++)
		m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 |
		       (uint32_t)p[4 * i + 2] << 16 |
		       (uint32_t)p[4 * i + 3] << 24;

	for (i = 0; i < 64; i++) {
		uint32_t fv;
		unsigned int k;

		if (i < 16) {
			fv = f_f(b, cc, d);
			k = i;
		} else if (i < 32) {
			fv = f_g(b, cc, d);
			k = (5 * i + 1) % 16;
		} else if (i < 48) {
			fv = f_h(b, cc, d);
			k = (3 * i + 5) % 16;
		} else {
			fv = f_i(b, cc, d);
			k = (7 * i) % 16;
		}

		{
			uint32_t tmp = d;

			d = cc;
			cc = b;
			b = b + rol(a + fv + T[i] + m[k], S[i]);
			a = tmp;
		}
	}

	c->state[0] += a;
	c->state[1] += b;
	c->state[2] += cc;
	c->state[3] += d;
}

static void md5_init(struct md5_ctx *c)
{
	c->state[0] = 0x67452301;
	c->state[1] = 0xefcdab89;
	c->state[2] = 0x98badcfe;
	c->state[3] = 0x10325476;
	c->bits = 0;
	c->used = 0;
}

static void md5_update(struct md5_ctx *c, const void *data, size_t len)
{
	const uint8_t *p = data;

	c->bits += (uint64_t)len * 8;
	while (len) {
		size_t n = 64 - c->used;

		if (n > len)
			n = len;
		memcpy(c->buf + c->used, p, n);
		c->used += n;
		p += n;
		len -= n;
		if (c->used == 64) {
			md5_block(c, c->buf);
			c->used = 0;
		}
	}
}

static void md5_final(struct md5_ctx *c, uint8_t out[16])
{
	uint64_t bits = c->bits;
	unsigned int i;

	md5_update(c, "\x80", 1);
	while (c->used != 56) {
		static const uint8_t z;

		md5_update(c, &z, 1);
	}
	c->bits = bits;
	for (i = 0; i < 8; i++)
		c->buf[56 + i] = (uint8_t)(bits >> (8 * i));
	md5_block(c, c->buf);

	for (i = 0; i < 4; i++) {
		out[4 * i + 0] = (uint8_t)(c->state[i]);
		out[4 * i + 1] = (uint8_t)(c->state[i] >> 8);
		out[4 * i + 2] = (uint8_t)(c->state[i] >> 16);
		out[4 * i + 3] = (uint8_t)(c->state[i] >> 24);
	}
}

void md5(const void *data, size_t len, uint8_t out[16])
{
	struct md5_ctx c;

	md5_init(&c);
	md5_update(&c, data, len);
	md5_final(&c, out);
}

void md5_three(const void *a, size_t alen, const void *b, size_t blen,
	       const void *c, size_t clen, uint8_t out[16])
{
	struct md5_ctx x;

	md5_init(&x);
	md5_update(&x, a, alen);
	md5_update(&x, b, blen);
	md5_update(&x, c, clen);
	md5_final(&x, out);
}

bool md5_selftest(void)
{
	static const struct {
		const char *in;
		const uint8_t want[16];
	} v[] = {
		{ "",
		  { 0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04, 0xe9, 0x80,
		    0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e } },
		{ "abc",
		  { 0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96,
		    0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72 } },
		{ "message digest",
		  { 0xf9, 0x6b, 0x69, 0x7d, 0x7c, 0xb7, 0x93, 0x8d, 0x52, 0x5a,
		    0x2f, 0x31, 0xaa, 0xf1, 0x61, 0xd0 } },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(v); i++) {
		uint8_t got[16];

		md5(v[i].in, strlen(v[i].in), got);
		if (memcmp(got, v[i].want, 16) != 0)
			return false;
	}
	return true;
}
