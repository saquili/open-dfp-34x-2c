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
 * that keeps it positive.
 */
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

static void sha_update(const MG_UECC_HashContext *base, const uint8_t *m, unsigned int n)
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
		static const uint8_t unused;

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

/*
 * An own certificate (cert.h). The DER of a certificate is read as RFC 5280
 * section 4.1 lays it out, far enough to find what the daemon and the panel
 * need: the key, its validity, its names. A value is a tag of one byte and a
 * length of at most three; nothing in a certificate of a few kilobytes needs
 * more.
 */

/* A value of DER being read, or the rest of one. */
struct der_in {
	const uint8_t *p;
	size_t n;
};

/* The next value of @in, its tag in @tag and its bytes in @v: false at the end. */
static bool der_next(struct der_in *in, uint8_t *tag, struct der_in *v)
{
	size_t len, hn = 2;

	if (in->n < 2)
		return false;
	*tag = in->p[0];
	len = in->p[1];
	if (len & 0x80) {
		size_t k = len & 0x7f;

		if (!k || k > 3 || in->n < 2 + k)
			return false;
		len = 0;
		for (size_t i = 0; i < k; i++)
			len = len << 8 | in->p[2 + i];
		hn += k;
	}
	if (len > in->n - hn)
		return false;
	v->p = in->p + hn;
	v->n = len;
	in->p += hn + len;
	in->n -= hn + len;
	return true;
}

/* The next value of @in, which must be of @tag. */
static bool der_take(struct der_in *in, uint8_t tag, struct der_in *v)
{
	uint8_t t;

	return der_next(in, &t, v) && t == tag;
}

static bool der_is(const struct der_in *v, const uint8_t *oid, size_t n)
{
	return v->n == n && !memcmp(v->p, oid, n);
}

/* id-ecPublicKey and prime256v1 (RFC 5480), rsaEncryption (RFC 8017). */
static const uint8_t g_ec_oid[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const uint8_t g_p256_oid[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const uint8_t g_rsa_oid[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };

/* Whether @alg, a key's algorithm and its parameters, is ECDSA of P-256; why not in @err. */
static bool p256_alg(struct der_in alg, char *err, size_t errcap)
{
	struct der_in oid, curve;

	if (!der_take(&alg, 0x06, &oid)) {
		snprintf(err, errcap, "the key's algorithm cannot be read");
		return false;
	}
	if (der_is(&oid, g_rsa_oid, sizeof(g_rsa_oid))) {
		snprintf(err, errcap, "an RSA key: the panel's HTTPS takes an ECDSA key of P-256 alone");
		return false;
	}
	if (!der_is(&oid, g_ec_oid, sizeof(g_ec_oid))) {
		snprintf(err, errcap,
			 "a key of another kind: the panel's HTTPS takes an ECDSA key of P-256 alone");
		return false;
	}
	if (!der_take(&alg, 0x06, &curve) || !der_is(&curve, g_p256_oid, sizeof(g_p256_oid))) {
		snprintf(err, errcap,
			 "an ECDSA key of another curve: the panel's HTTPS takes P-256 alone");
		return false;
	}
	return true;
}

/* Days from 1970-01-01 to the date (Howard Hinnant's days_from_civil). */
static long long days_from(int y, int m, int d)
{
	long long era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* The digits @s of @n characters as a number, or -1. */
static int digits(const uint8_t *s, int n)
{
	int v = 0;

	for (int i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (s[i] - '0');
	}
	return v;
}

/* A UTCTime or GeneralizedTime of RFC 5280 section 4.1.2.5, in @t. */
static bool der_time(uint8_t tag, const struct der_in *v, long long *t)
{
	const uint8_t *s = v->p;
	int y, f[5];

	if (tag == 0x17 && v->n == 13) {
		y = digits(s, 2);
		y += y < 0 ? 0 : y < 50 ? 2000 : 1900;
		s += 2;
	} else if (tag == 0x18 && v->n == 15) {
		y = digits(s, 4);
		s += 4;
	} else {
		return false;
	}
	for (int i = 0; i < 5; i++)
		f[i] = digits(s + 2 * i, 2);
	if (y < 1970 || s[10] != 'Z' || f[0] < 1 || f[0] > 12 || f[1] < 1 || f[1] > 31 ||
	    f[2] < 0 || f[2] > 23 || f[3] < 0 || f[3] > 59 || f[4] < 0 || f[4] > 60)
		return false;
	*t = days_from(y, f[0], f[1]) * 86400LL + f[2] * 3600LL + f[3] * 60LL + f[4];
	return true;
}

void cert_iso(long long t, char out[24])
{
	long long z = t / 86400 + 719468, s = t % 86400, era, doe, yoe, doy, mp, y;
	char text[64];
	int d, m;

	era = z / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = (int)(doy - (153 * mp + 2) / 5 + 1);
	m = (int)(mp < 10 ? mp + 3 : mp - 9);
	y += m <= 2;
	/* 20 characters for a year of four digits, which a certificate's is. */
	snprintf(text, sizeof(text), "%04lld-%02d-%02dT%02lld:%02lld:%02lldZ", y % 10000, m, d,
		 s / 3600, s / 60 % 60, s % 60);
	memcpy(out, text, 20);
	out[20] = '\0';
}

/* @v, a string of a name, after "@label=" at the end of @out, a control as '?'. */
static void words(char *out, size_t cap, const char *label, const struct der_in *v)
{
	size_t len = strlen(out);

	if (len + strlen(label) + 4 >= cap)
		return;
	len += (size_t)snprintf(out + len, cap - len, "%s%s=", len ? ", " : "", label);
	for (size_t i = 0; i < v->n && len + 1 < cap; i++)
		out[len++] = v->p[i] < 0x20 || v->p[i] == 0x7f ? '?' : (char)v->p[i];
	out[len] = '\0';
}

/* The common name and the organisation of the Name @name, as words in @out. */
static void name_words(struct der_in name, char *out, size_t cap)
{
	static const uint8_t cn[] = { 0x55, 0x04, 0x03 }, o[] = { 0x55, 0x04, 0x0a };
	struct der_in rdn, atv, oid, val;
	uint8_t tag;

	out[0] = '\0';
	while (der_take(&name, 0x31, &rdn)) {
		while (der_take(&rdn, 0x30, &atv) && der_take(&atv, 0x06, &oid) &&
		       der_next(&atv, &tag, &val)) {
			if (der_is(&oid, cn, sizeof(cn)))
				words(out, cap, "CN", &val);
			else if (der_is(&oid, o, sizeof(o)))
				words(out, cap, "O", &val);
		}
	}
}

/* The names of a subjectAltName, @v its GeneralNames, into @own. */
static void alt_names(struct der_in v, struct cert_own *own)
{
	struct der_in names, n;
	char one[80];
	size_t at;
	uint8_t tag;

	if (!der_take(&v, 0x30, &names))
		return;
	while (der_next(&names, &tag, &n)) {
		one[0] = '\0';
		if (tag == 0x82 && n.n < 64) {
			snprintf(one, sizeof(one), "DNS:%.*s", (int)n.n, (const char *)n.p);
		} else if (tag == 0x87 && n.n == 4) {
			snprintf(one, sizeof(one), "IP:%u.%u.%u.%u", n.p[0], n.p[1], n.p[2], n.p[3]);
			if (own->nips < CERT_IPS_MAX)
				memcpy(own->ips[own->nips++], n.p, 4);
		} else if (tag == 0x87 && n.n == 16) {
			at = (size_t)snprintf(one, sizeof(one), "IP:");
			for (int i = 0; i < 16; i += 2)
				at += (size_t)snprintf(one + at, sizeof(one) - at, "%s%x",
						       i ? ":" : "", n.p[i] << 8 | n.p[i + 1]);
		}
		at = strlen(own->names);
		if (*one && at + strlen(one) + 2 < sizeof(own->names))
			snprintf(own->names + at, sizeof(own->names) - at, "%s%s", at ? " " : "",
				 one);
	}
}

/*
 * The first certificate of @own, its @n bytes at the chain's start: its
 * key's point into @pub, its validity, names and alternative names.
 */
static bool leaf_read(struct cert_own *own, size_t n, uint8_t pub[64], char *err, size_t errcap)
{
	static const uint8_t san[] = { 0x55, 0x1d, 0x11 };
	struct der_in in = { own->chain, n }, cert, tbs, v, alg, bits, ext, e, oid;
	uint8_t tag;

	/* version, serialNumber, signature, issuer */
	if (!der_take(&in, 0x30, &cert) || !der_take(&cert, 0x30, &tbs) ||
	    !der_next(&tbs, &tag, &v) || (tag == 0xa0 && !der_take(&tbs, 0x02, &v)) ||
	    (tag != 0xa0 && tag != 0x02) || !der_take(&tbs, 0x30, &v) ||
	    !der_take(&tbs, 0x30, &v))
		goto bad;
	name_words(v, own->issuer, sizeof(own->issuer));
	/* validity, subject, subjectPublicKeyInfo */
	if (!der_take(&tbs, 0x30, &v) || !der_next(&v, &tag, &e) ||
	    !der_time(tag, &e, &own->not_before) || !der_next(&v, &tag, &e) ||
	    !der_time(tag, &e, &own->not_after) || !der_take(&tbs, 0x30, &v))
		goto bad;
	name_words(v, own->subject, sizeof(own->subject));
	if (!der_take(&tbs, 0x30, &v) || !der_take(&v, 0x30, &alg) || !der_take(&v, 0x03, &bits))
		goto bad;
	if (!p256_alg(alg, err, errcap))
		return false;
	if (bits.n != 66 || bits.p[0] || bits.p[1] != 0x04)
		goto bad;
	memcpy(pub, bits.p + 2, 64);
	/* The extensions, after the unique identifiers that may come first. */
	while (der_next(&tbs, &tag, &v)) {
		ext = (struct der_in){ NULL, 0 };
		if (tag == 0xa3)
			der_take(&v, 0x30, &ext);
		while (der_take(&ext, 0x30, &e)) {
			if (!der_take(&e, 0x06, &oid) || !der_is(&oid, san, sizeof(san)) ||
			    !der_next(&e, &tag, &v))
				continue;
			/* critical, when it is said */
			if (tag == 0x01)
				der_next(&e, &tag, &v);
			if (tag == 0x04)
				alt_names(v, own);
		}
	}
	return true;
bad:
	snprintf(err, errcap, "the certificate cannot be read");
	return false;
}

int cert_own_der(const uint8_t *chain, size_t len, const uint8_t key[CERT_KEY_LEN],
		 struct cert_own *own, char *err, size_t errcap)
{
	struct der_in in = { chain, len }, v;
	uint8_t pub[64], mine[64], tag;

	if (!len || len > CERT_CHAIN_MAX) {
		snprintf(err, errcap, "the certificate and its chain are none, or longer than %d bytes",
			 CERT_CHAIN_MAX);
		return -EINVAL;
	}
	memset(own, 0, sizeof(*own));
	memcpy(own->chain, chain, len);
	own->len = len;
	while (in.n) {
		if (!der_next(&in, &tag, &v) || tag != 0x30) {
			snprintf(err, errcap, "certificate %u of the chain cannot be read",
				 own->count + 1);
			return -EINVAL;
		}
		if (++own->count > CERT_CHAIN_COUNT) {
			snprintf(err, errcap, "more than %d certificates", CERT_CHAIN_COUNT);
			return -EINVAL;
		}
		if (own->count == 1)
			own->leaf = len - in.n;
	}
	if (!leaf_read(own, own->leaf, pub, err, errcap))
		return -EINVAL;
	if (!mg_uecc_compute_public_key(key, mine, mg_uecc_secp256r1()) ||
	    memcmp(pub, mine, sizeof(mine))) {
		snprintf(err, errcap, "the key is not the certificate's");
		return -EINVAL;
	}
	memcpy(own->key, key, CERT_KEY_LEN);
	return 0;
}

/*
 * The next PEM block of @*at: its label in @label of @lcap, its bytes in @out
 * of @cap, and @*at past it. Return its length, 0 without one, or -1 for a
 * block that cannot be read.
 */
static long pem_next(const char **at, char *label, size_t lcap, uint8_t *out, size_t cap)
{
	static char text[2 * CERT_CHAIN_MAX];
	const char *head = strstr(*at, "-----BEGIN "), *end;
	char tail[64];
	size_t n = 0, lsz;

	if (!head)
		return 0;
	head += 11;
	*at = head;
	lsz = strcspn(head, "-\r\n");
	if (lsz >= lcap || strncmp(head + lsz, "-----", 5))
		return -1;
	snprintf(label, lcap, "%.*s", (int)lsz, head);
	snprintf(tail, sizeof(tail), "-----END %s-----", label);
	end = strstr(head + lsz + 5, tail);
	if (!end)
		return -1;
	*at = end + strlen(tail);
	for (const char *p = head + lsz + 5; p < end && n + 1 < sizeof(text); p++)
		if (!strchr(" \t\r\n", *p))
			text[n++] = *p;
	text[n] = '\0';
	/* Whole groups of four, '=' at the end alone, and room for the bytes. */
	if (!n || n % 4 || strcspn(text, "=") < n - 2 ||
	    (text[n - 2] == '=' && text[n - 1] != '=') || n / 4 * 3 + 1 > cap)
		return -1;
	n = mg_base64_decode(text, n, (char *)out, cap);
	return n ? (long)n : -1;
}

/* The scalar of P-256 of SEC1's ECPrivateKey @in (RFC 5915). */
static bool sec1_key(struct der_in in, uint8_t key[CERT_KEY_LEN], char *err, size_t errcap)
{
	struct der_in seq, v, priv, param;
	uint8_t tag;

	if (!der_take(&in, 0x30, &seq) || !der_take(&seq, 0x02, &v) || v.n != 1 || v.p[0] != 1 ||
	    !der_take(&seq, 0x04, &priv)) {
		snprintf(err, errcap, "the key cannot be read");
		return false;
	}
	/* The curve, when it is said, before the scalar's size, which is the curve's. */
	if (der_next(&seq, &tag, &v) && tag == 0xa0 &&
	    (!der_take(&v, 0x06, &param) || !der_is(&param, g_p256_oid, sizeof(g_p256_oid)))) {
		snprintf(err, errcap,
			 "an ECDSA key of another curve: the panel's HTTPS takes P-256 alone");
		return false;
	}
	if (!priv.n || priv.n > CERT_KEY_LEN) {
		snprintf(err, errcap, "the key cannot be read");
		return false;
	}
	memset(key, 0, CERT_KEY_LEN);
	memcpy(key + CERT_KEY_LEN - priv.n, priv.p, priv.n);
	return true;
}

/* The scalar of the key of a PEM block of @label, its bytes @der. */
static bool pem_key(const char *label, struct der_in der, uint8_t key[CERT_KEY_LEN], char *err,
		    size_t errcap)
{
	struct der_in seq, v, alg, inner;

	if (!strcmp(label, "EC PRIVATE KEY"))
		return sec1_key(der, key, err, errcap);
	if (!strcmp(label, "ENCRYPTED PRIVATE KEY")) {
		snprintf(err, errcap, "the key is encrypted: give it without its passphrase");
		return false;
	}
	if (strcmp(label, "PRIVATE KEY")) {
		snprintf(err, errcap,
			 "a key of another kind: the panel's HTTPS takes an ECDSA key of P-256 alone");
		return false;
	}
	/* PrivateKeyInfo: version 0, the algorithm, the key in an OCTET STRING. */
	if (!der_take(&der, 0x30, &seq) || !der_take(&seq, 0x02, &v) || v.n != 1 || v.p[0] ||
	    !der_take(&seq, 0x30, &alg) || !der_take(&seq, 0x04, &inner)) {
		snprintf(err, errcap, "the key cannot be read");
		return false;
	}
	return p256_alg(alg, err, errcap) && sec1_key(inner, key, err, errcap);
}

int cert_own_pem(const char *cert, const char *key, struct cert_own *own, char *err,
		 size_t errcap)
{
	static uint8_t chain[CERT_CHAIN_MAX], der[CERT_CHAIN_MAX];
	uint8_t scalar[CERT_KEY_LEN];
	const char *at = cert;
	char label[40];
	size_t len = 0;
	long n;
	int rc = -EINVAL;

	while ((n = pem_next(&at, label, sizeof(label), der, sizeof(der))) != 0) {
		if (n < 0) {
			snprintf(err, errcap, "a block of the certificate's text cannot be read");
			return -EINVAL;
		}
		if (strcmp(label, "CERTIFICATE"))
			continue;
		if (len + (size_t)n > sizeof(chain)) {
			snprintf(err, errcap,
				 "the certificate and its chain are longer than %d bytes",
				 CERT_CHAIN_MAX);
			return -EINVAL;
		}
		memcpy(chain + len, der, (size_t)n);
		len += (size_t)n;
	}
	if (!len) {
		snprintf(err, errcap, "no certificate: a PEM block BEGIN CERTIFICATE");
		return -EINVAL;
	}
	/* The key's block, after the curve's that openssl may write first. */
	at = key;
	do {
		n = pem_next(&at, label, sizeof(label), der, sizeof(der));
	} while (n > 0 && !strstr(label, "PRIVATE KEY"));
	if (n <= 0)
		snprintf(err, errcap, n ? "the key's block cannot be read" :
			 "no key: a PEM block BEGIN PRIVATE KEY or BEGIN EC PRIVATE KEY");
	else if (pem_key(label, (struct der_in){ der, (size_t)n }, scalar, err, errcap))
		rc = cert_own_der(chain, len, scalar, own, err, errcap);
	memset(scalar, 0, sizeof(scalar));
	memset(der, 0, sizeof(der));
	return rc;
}

int cert_own_load(const char *path, struct cert_own *own, char *err, size_t errcap)
{
	static char text[2 * CERT_KEY_LEN + 4 * (CERT_CHAIN_MAX / 3 + 1) + 16];
	static uint8_t chain[CERT_CHAIN_MAX + 3];
	uint8_t key[CERT_KEY_LEN];
	int hi = -1, lo = -1, rc = -EINVAL;
	FILE *f = fopen(path, "r");
	size_t len, n;
	char *line;

	if (!f)
		return -ENOENT;
	len = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[len] = '\0';
	line = strchr(text, '\n');
	for (size_t i = 0; line && line - text == 2 * CERT_KEY_LEN && i < CERT_KEY_LEN; i++) {
		hi = hexval(text[2 * i]);
		lo = hexval(text[2 * i + 1]);
		if (hi < 0 || lo < 0)
			break;
		key[i] = (uint8_t)(hi << 4 | lo);
	}
	n = line ? strcspn(++line, "\r\n") : 0;
	/* mg_base64_decode() gives 0 for text that is not base64. */
	if (hi >= 0 && lo >= 0 && n && n % 4 == 0 && n / 4 * 3 <= CERT_CHAIN_MAX + 2)
		n = mg_base64_decode(line, n, (char *)chain, sizeof(chain));
	else
		n = 0;
	if (!n)
		snprintf(err, errcap, "not a key and a chain in base64");
	else
		rc = cert_own_der(chain, n, key, own, err, errcap);
	memset(key, 0, sizeof(key));
	memset(text, 0, sizeof(text));
	return rc;
}

bool cert_own_names(const struct cert_own *own, const uint8_t ip[4])
{
	for (unsigned int i = 0; i < own->nips; i++)
		if (!memcmp(own->ips[i], ip, 4))
			return true;
	return false;
}

void cert_own_key_der(const struct cert_own *own, uint8_t der[CERT_KEY_DER_LEN])
{
	static const uint8_t head[] = { 0x30, 0x25, 0x02, 0x01, 0x01, 0x04, 0x20 };

	memcpy(der, head, sizeof(head));
	memcpy(der + sizeof(head), own->key, CERT_KEY_LEN);
}
