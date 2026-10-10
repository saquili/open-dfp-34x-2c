// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Root's password against its hash (password.h). The older form's HMAC
 * keeps the two padded key states, so an iteration costs two compressions.
 */

#define _GNU_SOURCE

#include <crypt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "password.h"
#include "sha256.h"

struct hmac {
	struct sha256 inner;
	struct sha256 outer;
};

static void hmac_key(struct hmac *h, const uint8_t *key, size_t len)
{
	uint8_t k[64] = { 0 }, pad[64];
	size_t i;

	if (len > sizeof(k)) {
		struct sha256 c;

		sha256_init(&c);
		sha256_update(&c, key, len);
		sha256_final(&c, k);
	} else {
		memcpy(k, key, len);
	}
	for (i = 0; i < sizeof(pad); i++)
		pad[i] = k[i] ^ 0x36;
	sha256_init(&h->inner);
	sha256_update(&h->inner, pad, sizeof(pad));
	for (i = 0; i < sizeof(pad); i++)
		pad[i] = k[i] ^ 0x5c;
	sha256_init(&h->outer);
	sha256_update(&h->outer, pad, sizeof(pad));
	explicit_bzero(k, sizeof(k));
	explicit_bzero(pad, sizeof(pad));
}

static void hmac_run(const struct hmac *h, const uint8_t *a, size_t alen,
		     const uint8_t *b, size_t blen, uint8_t out[32])
{
	struct sha256 c = h->inner;
	uint8_t inner[32];

	sha256_update(&c, a, alen);
	if (blen)
		sha256_update(&c, b, blen);
	sha256_final(&c, inner);
	c = h->outer;
	sha256_update(&c, inner, sizeof(inner));
	sha256_final(&c, out);
}

void password_pbkdf2(const uint8_t *secret, size_t secret_len,
		     const uint8_t *salt, size_t salt_len,
		     unsigned int iterations, uint8_t out[32])
{
	static const uint8_t block1[4] = { 0, 0, 0, 1 };
	struct hmac h;
	uint8_t u[32];
	unsigned int i;
	size_t j;

	hmac_key(&h, secret, secret_len);
	hmac_run(&h, salt, salt_len, block1, sizeof(block1), u);
	memcpy(out, u, sizeof(u));
	for (i = 1; i < iterations; i++) {
		hmac_run(&h, u, sizeof(u), NULL, 0, u);
		for (j = 0; j < sizeof(u); j++)
			out[j] ^= u[j];
	}
	explicit_bzero(&h, sizeof(h));
	explicit_bzero(u, sizeof(u));
}

static bool unhex(const char *in, size_t n, uint8_t *out)
{
	for (size_t i = 0; i < 2 * n; i++) {
		char c = in[i];
		int v = c >= '0' && c <= '9' ? c - '0' :
			c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

		if (v < 0)
			return false;
		out[i / 2] = (uint8_t)(i % 2 ? (out[i / 2] | v) : v << 4);
	}
	return in[2 * n] == '\0' || in[2 * n] == '$';
}

static bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t diff = 0;

	for (size_t i = 0; i < n; i++)
		diff |= a[i] ^ b[i];
	return diff == 0;
}

static const char g_older[] = "pbkdf2-sha256$";

enum password_match password_verify(const char *secret, const char *stored)
{
	uint8_t salt[16], want[32], got[32];
	const char *p = stored, *c;
	unsigned long iterations;
	char *end;
	bool ok;

	if (*p == '$') {
		c = crypt(secret, stored);
		return c && strlen(c) == strlen(stored) &&
		       same((const uint8_t *)c, (const uint8_t *)stored, strlen(stored)) ?
			       PASSWORD_MATCHES :
			       PASSWORD_WRONG;
	}
	if (strncmp(p, g_older, sizeof(g_older) - 1))
		return PASSWORD_WRONG;
	p += sizeof(g_older) - 1;
	iterations = strtoul(p, &end, 10);
	if (end == p || *end != '$' || iterations < 1 || iterations > 10000000)
		return PASSWORD_WRONG;
	p = end + 1;
	if (!unhex(p, sizeof(salt), salt) || p[2 * sizeof(salt)] != '$')
		return PASSWORD_WRONG;
	p += 2 * sizeof(salt) + 1;
	if (!unhex(p, sizeof(want), want))
		return PASSWORD_WRONG;

	password_pbkdf2((const uint8_t *)secret, strlen(secret), salt,
			sizeof(salt), (unsigned int)iterations, got);
	ok = same(got, want, sizeof(got));
	explicit_bzero(got, sizeof(got));
	return ok ? PASSWORD_OLDER : PASSWORD_WRONG;
}
