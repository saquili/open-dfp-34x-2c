// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* A public key for root's ssh login, checked (sshkey.h). */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sha256.h"
#include "sshkey.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The longest blob a line of SSHKEY_LINE_MAX bytes can carry. */
#define BLOB_MAX (SSHKEY_LINE_MAX / 4 * 3)

static const char g_b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The types taken, with the curve's name and point, or 0 for none. */
static const struct {
	const char *type;
	const char *curve;
	unsigned int bits;
	size_t point;
} g_types[] = {
	{ "ssh-ed25519", NULL, 256, 0 },
	{ "ecdsa-sha2-nistp256", "nistp256", 256, 65 },
	{ "ecdsa-sha2-nistp384", "nistp384", 384, 97 },
	{ "ecdsa-sha2-nistp521", "nistp521", 521, 133 },
	{ "ssh-rsa", NULL, 0, 0 },
};

static int b64_value(char c)
{
	const char *p = c ? strchr(g_b64, c) : NULL;

	return p ? (int)(p - g_b64) : -1;
}

/*
 * The bytes of the base64 @s of @n characters, padded to a multiple of four,
 * into @out of @cap bytes: their number, or -1 for a text that is not that.
 */
static long b64_decode(const char *s, size_t n, uint8_t *out, size_t cap)
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

/* @in of @n bytes as base64 into @out, padded with @pad: the length written. */
static size_t b64_encode(const uint8_t *in, size_t n, char *out, bool pad)
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

/* A blob being read: a string of RFC 4251 section 5 at a time. */
struct reader {
	const uint8_t *p;
	size_t left;
};

/* The next string of @r, its bytes in @s and @n: false when there is none. */
static bool take(struct reader *r, const uint8_t **s, size_t *n)
{
	uint32_t len;

	if (r->left < 4)
		return false;
	len = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 |
	      (uint32_t)r->p[2] << 8 | r->p[3];
	if (len > r->left - 4)
		return false;
	*s = r->p + 4;
	*n = len;
	r->p += 4 + len;
	r->left -= 4 + len;
	return true;
}

static bool take_is(struct reader *r, const char *want)
{
	const uint8_t *s;
	size_t n;

	return take(r, &s, &n) && n == strlen(want) && !memcmp(s, want, n);
}

/*
 * The bits of the positive mpint @s of @n bytes (RFC 4251 section 5): none
 * for one that is negative or zero, or written with a zero too many.
 */
static unsigned int mpint_bits(const uint8_t *s, size_t n)
{
	unsigned int bits;

	if (!n || s[0] & 0x80)
		return 0;
	if (!s[0]) {
		if (n == 1 || !(s[1] & 0x80))
			return 0;
		s++;
		n--;
	}
	bits = (unsigned int)n * 8;
	for (uint8_t top = s[0]; !(top & 0x80); top <<= 1)
		bits--;
	return bits;
}

/*
 * Whether @blob of @n bytes is a key of the type @t of g_types, its size in
 * @bits; why not in @err.
 */
static bool blob_ok(size_t t, const uint8_t *blob, size_t n, unsigned int *bits,
		    char *err, size_t errcap)
{
	struct reader r = { blob, n };
	const uint8_t *s, *e;
	size_t len, elen;

	if (!take_is(&r, g_types[t].type)) {
		snprintf(err, errcap, "the key inside is not of the type %s", g_types[t].type);
		return false;
	}
	*bits = g_types[t].bits;
	if (!strcmp(g_types[t].type, "ssh-ed25519")) {
		if (!take(&r, &s, &len) || len != 32) {
			snprintf(err, errcap, "not an Ed25519 key of 32 bytes");
			return false;
		}
	} else if (g_types[t].curve) {
		if (!take_is(&r, g_types[t].curve) || !take(&r, &s, &len) ||
		    len != g_types[t].point || s[0] != 0x04) {
			snprintf(err, errcap, "not a point of the curve %s", g_types[t].curve);
			return false;
		}
	} else {
		if (!take(&r, &e, &elen) || !take(&r, &s, &len) ||
		    mpint_bits(e, elen) < 2 || !(e[elen - 1] & 1) ||
		    !mpint_bits(s, len)) {
			snprintf(err, errcap, "not an RSA key: an odd exponent and a modulus");
			return false;
		}
		*bits = mpint_bits(s, len);
		if (*bits < SSHKEY_RSA_MIN || *bits > SSHKEY_RSA_MAX) {
			snprintf(err, errcap,
				 "an RSA key of %u bits: the stick takes %u to %u bits",
				 *bits, SSHKEY_RSA_MIN, SSHKEY_RSA_MAX);
			return false;
		}
	}
	if (r.left) {
		snprintf(err, errcap, "the key has bytes after its end");
		return false;
	}
	return true;
}

/* A word of a line: a run of characters that are not blanks or a line's end. */
static const char *word(const char *s, size_t *n)
{
	s += strspn(s, " \t");
	*n = strcspn(s, " \t\r\n");
	return s;
}

int sshkey_parse(const char *text, struct sshkey *k, char *err, size_t errcap)
{
	static uint8_t blob[BLOB_MAX + 3];
	uint8_t digest[SHA256_LEN];
	const char *type, *data, *comment;
	size_t tlen, dlen, clen, t;
	struct sha256 h;
	long n;

	memset(k, 0, sizeof(*k));
	type = word(text, &tlen);
	data = word(type + tlen, &dlen);
	comment = data + dlen;
	comment += strspn(comment, " \t");
	clen = strlen(comment);
	while (clen && strchr(" \t\r\n", comment[clen - 1]))
		clen--;
	for (t = 0; t < ARRAY_SIZE(g_types); t++)
		if (strlen(g_types[t].type) == tlen && !strncmp(type, g_types[t].type, tlen))
			break;
	if (t == ARRAY_SIZE(g_types)) {
		if (!tlen)
			snprintf(err, errcap, "no key: a line TYPE BASE64 [COMMENT]");
		else if (memchr(type, '=', tlen) || memchr(type, ',', tlen))
			snprintf(err, errcap, "a key with options in front of it: give the key alone");
		else
			snprintf(err, errcap,
				 "a key of the type %.*s: the stick takes ssh-ed25519, ecdsa-sha2-nistp256, -nistp384, -nistp521 and ssh-rsa",
				 (int)(tlen > 40 ? 40 : tlen), type);
		return -EINVAL;
	}
	n = b64_decode(data, dlen, blob, BLOB_MAX);
	if (n < 0) {
		snprintf(err, errcap, "the key is not base64, or too long for the store");
		return -EINVAL;
	}
	if (!blob_ok(t, blob, (size_t)n, &k->bits, err, errcap))
		return -EINVAL;
	if (strcspn(comment, "\r\n") < clen) {
		snprintf(err, errcap, "more than one line: one key at a time");
		return -EINVAL;
	}
	if (clen > SSHKEY_COMMENT_MAX) {
		snprintf(err, errcap, "the comment is longer than %u bytes", SSHKEY_COMMENT_MAX);
		return -EINVAL;
	}
	for (size_t i = 0; i < clen; i++) {
		unsigned char c = (unsigned char)comment[i];

		if (c < 0x20 || c == 0x7f) {
			snprintf(err, errcap, "the comment holds a control character");
			return -EINVAL;
		}
	}
	snprintf(k->type, sizeof(k->type), "%s", g_types[t].type);
	memcpy(k->comment, comment, clen);
	k->comment[clen] = '\0';
	/* The longest blob of a type taken, with its comment, fits: no check. */
	t = (size_t)snprintf(k->line, sizeof(k->line), "%s ", k->type);
	t += b64_encode(blob, (size_t)n, k->line + t, true);
	if (clen)
		snprintf(k->line + t, sizeof(k->line) - t, " %s", k->comment);

	sha256_init(&h);
	sha256_update(&h, blob, (size_t)n);
	sha256_final(&h, digest);
	memcpy(k->fingerprint, "SHA256:", 7);
	b64_encode(digest, sizeof(digest), k->fingerprint + 7, false);
	return 0;
}
