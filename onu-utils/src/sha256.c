// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* SHA-256 of sha256.h, as FIPS 180-4 specifies it. */

#include <string.h>

#include "sha256.h"

static const uint32_t k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned int n)
{
	return x >> n | x << (32u - n);
}

/* Hash one 64-byte block into the state. */
static void compress(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, hh;
	unsigned int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^
			      w[i - 15] >> 3;
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^
			      w[i - 2] >> 10;

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = h[0];
	b = h[1];
	c = h[2];
	d = h[3];
	e = h[4];
	f = h[5];
	g = h[6];
	hh = h[7];
	for (i = 0; i < 64; i++) {
		uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) +
			      ((e & f) ^ (~e & g)) + k[i] + w[i];
		uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) +
			      ((a & b) ^ (a & c) ^ (b & c));

		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

void sha256_init(struct sha256 *s)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->fill = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t len)
{
	const uint8_t *p = data;

	s->len += len;
	if (s->fill) {
		size_t take = 64u - s->fill < len ? 64u - s->fill : len;

		memcpy(s->block + s->fill, p, take);
		s->fill += take;
		p += take;
		len -= take;
		if (s->fill < 64u)
			return;
		compress(s->h, s->block);
		s->fill = 0;
	}
	for (; len >= 64u; p += 64, len -= 64u)
		compress(s->h, p);
	memcpy(s->block, p, len);
	s->fill = len;
}

void sha256_final(struct sha256 *s, uint8_t digest[SHA256_LEN])
{
	uint64_t bits = s->len * 8u;
	unsigned int i;

	s->block[s->fill++] = 0x80;
	if (s->fill > 56u) {
		memset(s->block + s->fill, 0, 64u - s->fill);
		compress(s->h, s->block);
		s->fill = 0;
	}
	memset(s->block + s->fill, 0, 56u - s->fill);
	for (i = 0; i < 8; i++)
		s->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	compress(s->h, s->block);

	for (i = 0; i < 8; i++) {
		digest[4 * i] = (uint8_t)(s->h[i] >> 24);
		digest[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		digest[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		digest[4 * i + 3] = (uint8_t)s->h[i];
	}
}
