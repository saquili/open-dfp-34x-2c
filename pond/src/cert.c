// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The panel's certificate (cert.h): an X.509 version 3 certificate (RFC
 * 5280), DER-encoded, signed with ECDSA on P-256 and SHA-256 by the key it
 * certifies. Its subject and issuer are one common name, "ONU" and the
 * address; its serial number is the first 16 bytes of the SHA-256 of the
 * public key, the address and the period, so that a new key or a new period
 * never reuses a serial number for the same name. Its extensions are those
 * that browsers look for in a server's certificate: the address as the
 * subject's alternative name, not a CA (critical), the key for signatures
 * (critical), and for TLS servers.
 */

#include "cert.h"

#include "mongoose.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_key[CERT_KEY_LEN];
static uint8_t g_pub[2 * CERT_KEY_LEN];
static bool g_have;

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* The first line of the file at @path, without its end. */
static int read_line(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	int err = 0;

	if (!f)
		return -errno;
	if (!fgets(out, (int)cap, f))
		err = ferror(f) ? -EIO : -ENODATA;
	fclose(f);
	if (!err)
		out[strcspn(out, "\n")] = '\0';
	return err;
}

int cert_load_key(const char *path)
{
	char line[2 * CERT_KEY_LEN + 8];
	uint8_t key[CERT_KEY_LEN];
	size_t i;
	int rc;

	rc = read_line(path, line, sizeof(line));
	if (rc)
		return rc;
	for (i = 0; i < 2 * CERT_KEY_LEN; i++)
		if (hexval(line[i]) < 0)
			break;
	if (i < 2 * CERT_KEY_LEN || line[i]) {
		memset(line, 0, sizeof(line));
		return -EINVAL;
	}
	for (i = 0; i < CERT_KEY_LEN; i++)
		key[i] = (uint8_t)(hexval(line[2 * i]) << 4 | hexval(line[2 * i + 1]));
	memset(line, 0, sizeof(line));
	/* micro-ecc refuses a scalar that is 0 or not below the curve's order. */
	if (!mg_uecc_compute_public_key(key, g_pub, mg_uecc_secp256r1())) {
		memset(key, 0, sizeof(key));
		return -EINVAL;
	}
	memcpy(g_key, key, sizeof(g_key));
	memset(key, 0, sizeof(key));
	g_have = true;
	return 0;
}

bool cert_have_key(void)
{
	return g_have;
}

void cert_validity(time_t now, time_t *not_before, time_t *not_after)
{
	long long since = (long long)now - CERT_EPOCH;
	long long period = 0;

	if (since > 0)
		period = since / (CERT_PERIOD_DAYS * 86400LL);
	*not_before = (time_t)(CERT_EPOCH + period * CERT_PERIOD_DAYS * 86400LL);
	*not_after = *not_before + CERT_DAYS * 86400LL - 1;
}

/*
 * The DER encoding, written forwards into a fixed buffer: put() appends,
 * tlv() appends a tag, a definite length and a value. A value that is itself
 * built of parts is built in a buffer of its own first.
 */
struct der {
	uint8_t b[CERT_MAX];
	size_t n;
	bool full;
};

static void put(struct der *d, const void *p, size_t n)
{
	if (d->full || n > sizeof(d->b) - d->n) {
		d->full = true;
		return;
	}
	if (n)
		memcpy(d->b + d->n, p, n);
	d->n += n;
}

static void tlv(struct der *d, uint8_t tag, const void *v, size_t n)
{
	uint8_t h[4] = { tag };
	size_t hn;

	if (n < 0x80) {
		h[1] = (uint8_t)n;
		hn = 2;
	} else if (n < 0x100) {
		h[1] = 0x81;
		h[2] = (uint8_t)n;
		hn = 3;
	} else {
		h[1] = 0x82;
		h[2] = (uint8_t)(n >> 8);
		h[3] = (uint8_t)n;
		hn = 4;
	}
	put(d, h, hn);
	put(d, v, n);
}

/* Wrap what @in holds as one value of @tag, appended to @out. */
static void wrap(struct der *out, uint8_t tag, const struct der *in)
{
	if (in->full)
		out->full = true;
	else
		tlv(out, tag, in->b, in->n);
}

/* An unsigned big-endian number as a DER INTEGER: no leading zeros but one
 * that keeps it positive. */
static void integer(struct der *d, const uint8_t *v, size_t n)
{
	uint8_t buf[33];

	while (n > 1 && !v[0]) {
		v++;
		n--;
	}
	buf[0] = 0;
	memcpy(buf + 1, v, n);
	if (v[0] & 0x80)
		tlv(d, 0x02, buf, n + 1);
	else
		tlv(d, 0x02, buf + 1, n);
}

/* UTCTime to 2049, GeneralizedTime from 2050 (RFC 5280 clause 4.1.2.5). */
static void when(struct der *d, time_t t)
{
	struct tm tm;
	char s[64];

	gmtime_r(&t, &tm);
	if (tm.tm_year + 1900 < 2050) {
		snprintf(s, sizeof(s), "%02d%02d%02d%02d%02d%02dZ", tm.tm_year % 100,
			 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		tlv(d, 0x17, s, 13);
	} else {
		snprintf(s, sizeof(s), "%04d%02d%02d%02d%02d%02dZ", tm.tm_year + 1900,
			 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		tlv(d, 0x18, s, 15);
	}
}

/* The extension @oid, critical or not, around the DER value @v. */
static void extension(struct der *d, const uint8_t *oid, size_t oidn, bool critical,
		      const struct der *v)
{
	static const uint8_t yes[] = { 0x01, 0x01, 0xff };
	struct der e = { .n = 0 };

	tlv(&e, 0x06, oid, oidn);
	if (critical)
		put(&e, yes, sizeof(yes));
	wrap(&e, 0x04, v);
	wrap(d, 0x30, &e);
}

/* The hash of micro-ecc's deterministic signing (RFC 6979) is SHA-256. */
struct sha_ctx {
	MG_UECC_HashContext uecc;
	mg_sha256_ctx sha;
};

static void sha_init(const MG_UECC_HashContext *base)
{
	mg_sha256_init(&((struct sha_ctx *)(uintptr_t)base)->sha);
}

static void sha_update(const MG_UECC_HashContext *base, const uint8_t *m, unsigned n)
{
	mg_sha256_update(&((struct sha_ctx *)(uintptr_t)base)->sha, m, n);
}

static void sha_finish(const MG_UECC_HashContext *base, uint8_t *out)
{
	mg_sha256_final(out, &((struct sha_ctx *)(uintptr_t)base)->sha);
}

static void sha256(const void *p, size_t n, uint8_t out[32])
{
	mg_sha256_ctx c;

	mg_sha256_init(&c);
	mg_sha256_update(&c, p, n);
	mg_sha256_final(out, &c);
}

size_t cert_make(const uint8_t ip[4], time_t now, uint8_t *der, size_t cap)
{
	/* ecdsa-with-SHA256, id-ecPublicKey, prime256v1 and the attribute CN. */
	static const uint8_t alg[] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce,
				       0x3d, 0x04, 0x03, 0x02 };
	static const uint8_t ecpk[] = { 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce,
					0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a,
					0x86, 0x48, 0xce, 0x3d, 0x03, 0x01,
					0x07 };
	static const uint8_t cn[] = { 0x55, 0x04, 0x03 };
	/* subjectAltName, basicConstraints, keyUsage, extKeyUsage, serverAuth. */
	static const uint8_t san[] = { 0x55, 0x1d, 0x11 };
	static const uint8_t bc[] = { 0x55, 0x1d, 0x13 };
	static const uint8_t ku[] = { 0x55, 0x1d, 0x0f };
	static const uint8_t eku[] = { 0x55, 0x1d, 0x25 };
	static const uint8_t server_auth[] = { 0x2b, 0x06, 0x01, 0x05, 0x05,
					       0x07, 0x03, 0x01 };
	/* The key for digital signatures alone: a BIT STRING, bit 0. */
	static const uint8_t sig_only[] = { 0x03, 0x02, 0x07, 0x80 };
	static const uint8_t v3[] = { 0xa0, 0x03, 0x02, 0x01, 0x02 };
	struct der tbs = { .n = 0 }, t = { .n = 0 }, u = { .n = 0 };
	struct der name = { .n = 0 }, exts = { .n = 0 }, cert = { .n = 0 };
	struct sha_ctx hc = { .uecc = { sha_init, sha_update, sha_finish, 64, 32, NULL } };
	uint8_t tmp[2 * 32 + 64], hash[32], sig[64], point[66], serial[32];
	char common[24];
	time_t from, until;
	mg_sha256_ctx sc;
	uint32_t period;

	if (!g_have)
		return 0;
	hc.uecc.tmp = tmp;
	cert_validity(now, &from, &until);
	period = (uint32_t)((from - CERT_EPOCH) / (CERT_PERIOD_DAYS * 86400LL));

	/* The serial number: of the key, the address and the period. */
	mg_sha256_init(&sc);
	mg_sha256_update(&sc, g_pub, sizeof(g_pub));
	mg_sha256_update(&sc, ip, 4);
	{
		const uint8_t p[4] = { (uint8_t)(period >> 24), (uint8_t)(period >> 16),
				       (uint8_t)(period >> 8), (uint8_t)period };

		mg_sha256_update(&sc, p, sizeof(p));
	}
	mg_sha256_final(serial, &sc);
	serial[0] &= 0x7f;
	serial[0] |= 0x40;

	/* The name: CN=ONU a.b.c.d, as subject and as issuer. */
	snprintf(common, sizeof(common), "ONU %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	tlv(&t, 0x06, cn, sizeof(cn));
	tlv(&t, 0x0c, common, strlen(common));
	wrap(&u, 0x30, &t);
	wrap(&name, 0x31, &u);
	t.n = u.n = 0;
	wrap(&u, 0x30, &name);
	name = u;
	u.n = 0;

	put(&tbs, v3, sizeof(v3));
	integer(&tbs, serial, 16);
	tlv(&tbs, 0x30, alg, sizeof(alg));
	put(&tbs, name.b, name.n);
	when(&t, from);
	when(&t, until);
	wrap(&tbs, 0x30, &t);
	t.n = 0;
	put(&tbs, name.b, name.n);

	/* The public key: a BIT STRING of the uncompressed point. */
	tlv(&t, 0x30, ecpk, sizeof(ecpk));
	point[0] = 0;
	point[1] = 0x04;
	memcpy(point + 2, g_pub, sizeof(g_pub));
	tlv(&t, 0x03, point, sizeof(point));
	wrap(&tbs, 0x30, &t);
	t.n = 0;

	/* The extensions. */
	tlv(&u, 0x87, ip, 4);
	wrap(&t, 0x30, &u);
	extension(&exts, san, sizeof(san), false, &t);
	t.n = u.n = 0;
	tlv(&t, 0x30, NULL, 0);
	extension(&exts, bc, sizeof(bc), true, &t);
	t.n = 0;
	put(&t, sig_only, sizeof(sig_only));
	extension(&exts, ku, sizeof(ku), true, &t);
	t.n = 0;
	tlv(&u, 0x06, server_auth, sizeof(server_auth));
	wrap(&t, 0x30, &u);
	extension(&exts, eku, sizeof(eku), false, &t);
	t.n = u.n = 0;
	wrap(&t, 0x30, &exts);
	wrap(&tbs, 0xa3, &t);
	t.n = 0;

	wrap(&u, 0x30, &tbs);
	if (u.full)
		return 0;
	sha256(u.b, u.n, hash);
	if (!mg_uecc_sign_deterministic(g_key, hash, sizeof(hash), &hc.uecc, sig,
					mg_uecc_secp256r1()))
		return 0;

	/* Certificate: the TBS, the algorithm and the signature (r, s). */
	put(&cert, u.b, u.n);
	tlv(&cert, 0x30, alg, sizeof(alg));
	integer(&t, sig, 32);
	integer(&t, sig + 32, 32);
	{
		struct der bits = { .n = 0 };
		static const uint8_t unused = 0;

		put(&bits, &unused, 1);
		tlv(&bits, 0x30, t.b, t.n);
		wrap(&cert, 0x03, &bits);
	}
	u.n = 0;
	wrap(&u, 0x30, &cert);
	if (u.full || u.n > cap)
		return 0;
	memcpy(der, u.b, u.n);
	return u.n;
}

void cert_fingerprint(const uint8_t *der, size_t len, char out[CERT_FP_LEN])
{
	static const char hex[] = "0123456789ABCDEF";
	uint8_t h[32];
	size_t i;

	sha256(der, len, h);
	for (i = 0; i < sizeof(h); i++) {
		out[3 * i] = hex[h[i] >> 4];
		out[3 * i + 1] = hex[h[i] & 15];
		out[3 * i + 2] = i + 1 < sizeof(h) ? ':' : '\0';
	}
}

void cert_key_der(uint8_t der[CERT_KEY_DER_LEN])
{
	static const uint8_t head[] = { 0x30, 0x25, 0x02, 0x01, 0x01, 0x04, 0x20 };

	memcpy(der, head, sizeof(head));
	memcpy(der + sizeof(head), g_key, CERT_KEY_LEN);
}
