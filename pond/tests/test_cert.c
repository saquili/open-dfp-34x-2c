// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the panel's certificate: the keys it takes and refuses, the
 * periods of its validity, and that the same key, address and period give
 * the same certificate and nothing else does. The certificate itself, its
 * fields and its signature, OpenSSL reads in test_tls.sh: this program
 * writes it to the directory of its first argument as cert.der, with its
 * fingerprint in cert.fp. Then an own certificate: those of the directory
 * of its second argument, which openssl made (make-certs.sh), taken and
 * refused, and DER that cannot be read, built here.
 */

#include "cert.h"

#include "mongoose.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static int checks;

static void check(bool cond, const char *what)
{
	checks++;
	if (cond) {
		printf("  ok    %s\n", what);
	} else {
		printf("  FAIL  %s\n", what);
		fails++;
	}
}

static char g_dir[256];

static int load(const char *text)
{
	char path[300];
	FILE *f;

	snprintf(path, sizeof(path), "%s/key", g_dir);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(text, f);
	fclose(f);
	return cert_load_key(path);
}

/* A scalar of P-256, below its order. */
#define KEY "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721"
/* 2026-06-01, 2027-12-31 and 2028-01-01 00:00 UTC. */
#define JUNE_2026 1780272000
#define END_2027 1830211200
#define START_2028 1830297600

static void test_keys(void)
{
	puts("the keys it takes");
	check(load("0000000000000000000000000000000000000000000000000000000000000000\n") != 0,
	      "not 0");
	check(load("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551\n") != 0,
	      "not the order of the curve");
	check(load("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f67\n") != 0,
	      "not one digit too few");
	check(load(KEY "00\n") != 0, "not a digit too many");
	check(load("g9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721\n") != 0,
	      "not a letter that is no hexadecimal digit");
	check(!cert_have_key(), "none of them is taken");
	check(load(KEY "\n") == 0 && cert_have_key(), "a scalar below the order, with a line end");
	check(load(KEY) == 0, "and without one");
}

static void test_validity(void)
{
	time_t from, until;

	puts("the periods");
	cert_validity(0, &from, &until);
	check(from == CERT_EPOCH && until == CERT_EPOCH + 825 * 86400 - 1,
	      "a clock of 1970 counts as the first period: 825 days from 2026-01-01");
	cert_validity(JUNE_2026, &from, &until);
	check(from == CERT_EPOCH, "June 2026 is in the first period");
	cert_validity(END_2027, &from, &until);
	check(from == CERT_EPOCH && END_2027 < until,
	      "the last day of 2027 is the first period's");
	cert_validity(START_2028, &from, &until);
	check(from == CERT_EPOCH + 730 * 86400 && from == START_2028,
	      "two years on, on 2028-01-01, the second period starts");
	check(until == from + 825 * 86400 - 1, "and lasts 825 days too");
}

static void test_certificate(void)
{
	static const uint8_t ip[4] = { 10, 0, 254, 254 }, other[4] = { 10, 0, 254, 253 };
	uint8_t a[CERT_MAX], b[CERT_MAX];
	size_t na, nb;
	char fp[CERT_FP_LEN], path[300];
	FILE *f;

	puts("the certificate");
	load(KEY "\n");
	na = cert_make(ip, 0, a, sizeof(a));
	check(na > 300 && na < 600, "it is made, of 300 to 600 bytes");
	nb = cert_make(ip, JUNE_2026, b, sizeof(b));
	check(nb == na && !memcmp(a, b, na), "the same in the same period");
	nb = cert_make(ip, START_2028, b, sizeof(b));
	check(nb && (nb != na || memcmp(a, b, na)), "another in the next period");
	nb = cert_make(other, 0, b, sizeof(b));
	check(nb && (nb != na || memcmp(a, b, na)), "another for another address");
	check(cert_make(ip, 0, b, na - 1) == 0, "none where it does not fit");
	cert_fingerprint(a, na, fp);
	check(strlen(fp) == CERT_FP_LEN - 1 && fp[2] == ':', "the fingerprint reads AB:CD:...");

	snprintf(path, sizeof(path), "%s/cert.der", g_dir);
	f = fopen(path, "wb");
	if (f) {
		fwrite(a, 1, na, f);
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s/cert.fp", g_dir);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", fp);
		fclose(f);
	}
}

/* The certificates of make-certs.sh, and what is read of them. */
static char g_certs[256];
static char g_text[65536];
static struct cert_own g_own;
static char g_err[256];

/* The file @name of make-certs.sh, whole, in a buffer of its own of @slot. */
static const char *file(const char *name, int slot)
{
	static char texts[4][24576];
	char path[400];
	size_t n = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_certs, name);
	f = fopen(path, "rb");
	if (f) {
		n = fread(texts[slot], 1, sizeof(texts[slot]) - 1, f);
		fclose(f);
	}
	texts[slot][n] = '\0';
	return texts[slot];
}

static size_t der(const char *name, uint8_t *out, size_t cap)
{
	char path[400];
	size_t n = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_certs, name);
	f = fopen(path, "rb");
	if (f) {
		n = fread(out, 1, cap, f);
		fclose(f);
	}
	return n;
}

/* The certificate of @cert with the key of @key taken; else what was said. */
static bool taken(const char *cert, const char *key)
{
	g_err[0] = '\0';
	return cert_own_pem(cert, key, &g_own, g_err, sizeof(g_err)) == 0;
}

static bool refused(const char *cert, const char *key, const char *why)
{
	g_err[0] = '\0';
	if (cert_own_pem(cert, key, &g_own, g_err, sizeof(g_err)) != -EINVAL)
		return false;
	if (!strstr(g_err, why))
		printf("        said: %s\n", g_err);
	return strstr(g_err, why) != NULL;
}

/* @bytes of @n as a PEM block of @label, in @out. */
static void pem(const char *label, const uint8_t *bytes, size_t n, char *out, size_t cap)
{
	size_t len = (size_t)snprintf(out, cap, "-----BEGIN %s-----\n", label);

	len += mg_base64_encode(bytes, n, out + len, cap - len);
	snprintf(out + len, cap - len, "\n-----END %s-----\n", label);
}

static void test_own(void)
{
	static const uint8_t here[4] = { 192, 0, 2, 254 }, there[4] = { 198, 51, 100, 1 };
	uint8_t leaf[4096], inter[4096], both[8192];
	size_t nl = der("leaf.der", leaf, sizeof(leaf)), ni = der("int.der", inter, sizeof(inter));
	char fp[CERT_FP_LEN], t[24], want[200];

	puts("an own certificate: what is taken");
	check(nl > 300 && ni > 300, "openssl made the certificates");
	check(taken(file("chain.pem", 0), file("leaf.key", 1)),
	      "a chain of two, its key in SEC1 after the curve's block");
	memcpy(both, leaf, nl);
	memcpy(both + nl, inter, ni);
	check(g_own.count == 2 && g_own.len == nl + ni && g_own.leaf == nl &&
	      !memcmp(g_own.chain, both, nl + ni), "the chain is the two certificates' DER");
	cert_fingerprint(g_own.chain, g_own.leaf, fp);
	snprintf(want, sizeof(want), "%s", file("leaf.fp", 2));
	want[strcspn(want, "\n")] = '\0';
	check(!strcmp(fp, want), "the fingerprint is openssl's");
	check(!strcmp(g_own.subject, "O=Example, CN=stick.example") &&
	      !strcmp(g_own.issuer, "O=Example CA, CN=Example Intermediate"),
	      "its subject and issuer, as words");
	cert_iso(g_own.not_before, t);
	check(!strcmp(t, "2026-01-01T00:00:00Z"), "valid from 2026-01-01");
	cert_iso(g_own.not_after, t);
	check(!strcmp(t, "2036-01-01T00:00:00Z"), "until 2036-01-01");
	check(!strcmp(g_own.names,
		      "IP:127.0.0.1 IP:192.0.2.254 DNS:stick.example IP:2001:db8:0:0:0:0:0:1"),
	      "its alternative names");
	check(cert_own_names(&g_own, here) && !cert_own_names(&g_own, there),
	      "it names 192.0.2.254, not 198.51.100.1");
	check(taken(file("chain3.pem", 0), file("leaf.p8", 1)) && g_own.count == 3,
	      "a chain of three, its key in PKCS#8");
	snprintf(g_text, sizeof(g_text), "%s%s", file("leaf.key", 0), file("chain.pem", 1));
	check(taken(g_text, file("leaf.key", 2)) && g_own.count == 2,
	      "the key's block among the certificates is passed over");
	check(taken(file("critical.pem", 0), file("leaf.key", 1)) &&
	      !strcmp(g_own.names, "IP:192.0.2.254"), "alternative names said to be critical");
	check(taken(file("far.pem", 0), file("leaf.key", 1)), "one valid past 2049");
	cert_iso(g_own.not_after, t);
	check(!strcmp(t, "2051-01-01T00:00:00Z"), "until 2051, a GeneralizedTime");
	check(taken(file("expired.pem", 0), file("leaf.key", 1)), "an expired one, which the panel says");
	cert_iso(g_own.not_after, t);
	check(!strcmp(t, "2021-01-01T00:00:00Z"), "until 2021");
	check(taken(file("long.pem", 0), file("leaf.key", 1)) &&
	      strlen(g_own.subject) == CERT_NAME_MAX - 1 && !g_own.names[0],
	      "names longer than its room, cut; no alternative names");
	cert_iso(0, t);
	check(!strcmp(t, "1970-01-01T00:00:00Z"), "the time of 0");
	cert_iso(1835481599, t);
	check(!strcmp(t, "2028-02-29T23:59:59Z"), "a leap day");
}

static void test_own_refused(void)
{
	char text[24576];
	uint8_t bytes[8192];
	size_t n;

	puts("an own certificate: what is refused");
	check(refused("", file("leaf.key", 0), "no certificate"), "no certificate");
	check(refused(file("leaf.pem", 0), "", "no key"), "no key");
	check(refused(file("chain.pem", 0), file("leaf-enc.p8", 1), "encrypted"),
	      "an encrypted key");
	check(refused(file("rsa.pem", 0), file("rsa.key", 1), "an RSA key"),
	      "a certificate of an RSA key, its key in PKCS#8");
	check(refused(file("rsa.pem", 0), file("leaf.key", 1), "an RSA key"),
	      "a certificate of an RSA key");
	check(refused(file("leaf.pem", 0), file("rsa-traditional.key", 1), "another kind"),
	      "an RSA key of the traditional form");
	check(refused(file("leaf.pem", 0), file("ed25519.key", 1), "another kind"),
	      "a key of Ed25519");
	check(refused(file("p384.pem", 0), file("p384.key", 1), "another curve"),
	      "a key of P-384");
	check(refused(file("p384.pem", 0), file("leaf.key", 1), "another curve"),
	      "a certificate of a key of P-384");
	check(refused(file("chain.pem", 0), file("other.key", 1), "not the certificate's"),
	      "a key that is not the certificate's");
	check(refused("-----BEGIN CERTIFICATE-----\n@@@@\n-----END CERTIFICATE-----\n",
		      file("leaf.key", 0), "cannot be read"), "text that is no base64");
	check(refused("-----BEGIN CERTIFICATE-----\nQQ=Q\n-----END CERTIFICATE-----\n",
		      file("leaf.key", 0), "cannot be read"), "a '=' that is not at the end");
	check(refused("-----BEGIN CERTIFICATE-----\nQUFB\n", file("leaf.key", 0),
		      "cannot be read"), "a block without its end");
	check(refused("-----BEGIN CERTIFICATE\nQUFB\n-----END CERTIFICATE-----\n",
		      file("leaf.key", 0), "cannot be read"), "a block's head without its dashes");
	check(refused(file("leaf.pem", 0), "-----BEGIN EC PRIVATE KEY-----\nQUFB\n",
		      "the key's block cannot be read"), "a key's block without its end");
	check(refused(file("leaf.pem", 0),
		      "-----BEGIN EC PRIVATE KEY-----\nMAA=\n-----END EC PRIVATE KEY-----\n",
		      "the key cannot be read"), "a key of SEC1 that is none");
	check(refused(file("leaf.pem", 0),
		      "-----BEGIN EC PRIVATE KEY-----\nMAUCAQEEAA==\n-----END EC PRIVATE KEY-----\n",
		      "the key cannot be read"), "a key of SEC1 whose scalar is empty");
	check(refused(file("leaf.pem", 0),
		      "-----BEGIN PRIVATE KEY-----\nMAA=\n-----END PRIVATE KEY-----\n",
		      "the key cannot be read"), "a key of PKCS#8 that is none");
	/* PrivateKeyInfo of version 0 with an algorithm that is no OID. */
	memcpy(bytes, "\x30\x0a\x02\x01\x00\x30\x00\x04\x03\x30\x01\x00", 12);
	n = 12;
	pem("PRIVATE KEY", bytes, n, text, sizeof(text));
	check(refused(file("leaf.pem", 0), text, "algorithm cannot be read"),
	      "a key's algorithm that cannot be read");
	/* Nine certificates, and more than 8 KiB of them. */
	snprintf(g_text, sizeof(g_text), "%s", "");
	for (int i = 0; i < 9; i++)
		strncat(g_text, file("leaf.pem", 0), sizeof(g_text) - strlen(g_text) - 1);
	check(refused(g_text, file("leaf.key", 1), "more than 8 certificates"),
	      "nine certificates");
	snprintf(g_text, sizeof(g_text), "%s", "");
	for (int i = 0; i < 8; i++)
		strncat(g_text, file("root.pem", 0), sizeof(g_text) - strlen(g_text) - 1);
	check(refused(g_text, file("leaf.key", 1), "longer than 8192 bytes"),
	      "more than 8 KiB of certificates");
}

/* The scalar of leaf.key, of leaf.hex, in @key. */
static void hexkey(uint8_t key[CERT_KEY_LEN])
{
	const char *hex = file("leaf.hex", 3);

	char two[3] = "";

	for (int i = 0; i < CERT_KEY_LEN; i++) {
		memcpy(two, hex + 2 * i, 2);
		key[i] = (uint8_t)strtoul(two, NULL, 16);
	}
}

/* The DER of leaf.der with the byte @skip after where @from is made @to. */
static size_t patched(uint8_t *out, size_t cap, const char *from, size_t len, size_t skip,
		      uint8_t to)
{
	size_t n = der("leaf.der", out, cap);

	for (size_t i = 0; i + len <= n; i++)
		if (!memcmp(out + i, from, len)) {
			out[i + skip] = to;
			break;
		}
	return n;
}

static void test_own_der(void)
{
	static const uint8_t truncated[] = { 0x30, 0x05, 0x00 }, indefinite[] = { 0x30, 0x80 };
	static const uint8_t set[] = { 0x31, 0x00 }, empty[] = { 0x30, 0x03, 0x02, 0x01, 0x00 };
	/* A certificate whose subject is its last part: no key. */
	static const uint8_t nokey[] = {
		0x30, 0x30, 0x30, 0x2e, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x01, 0x01,
		0x30, 0x00, 0x30, 0x00, 0x30, 0x1e, 0x17, 0x0d, '2', '6', '0', '1', '0', '1',
		'0', '0', '0', '0', '0', '0', 'Z', 0x17, 0x0d, '2', '7', '0', '1', '0', '1',
		'0', '0', '0', '0', '0', '0', 'Z', 0x30, 0x00,
	};
	/* The same with a key of P-256 that is three bytes. */
	static const uint8_t shortkey[] = {
		0x30, 0x4d, 0x30, 0x4b, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x01, 0x01,
		0x30, 0x00, 0x30, 0x00, 0x30, 0x1e, 0x17, 0x0d, '2', '6', '0', '1', '0', '1',
		'0', '0', '0', '0', '0', '0', 'Z', 0x17, 0x0d, '2', '7', '0', '1', '0', '1',
		'0', '0', '0', '0', '0', '0', 'Z', 0x30, 0x00, 0x30, 0x1b, 0x30, 0x13, 0x06,
		0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
		0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x04, 0x00, 0x04, 0x01, 0x02,
	};
	uint8_t key[CERT_KEY_LEN] = { 1 }, bytes[4096] = { 0 };
	size_t n;

	puts("an own certificate in DER: what cannot be read");
	check(cert_own_der(bytes, 0, key, &g_own, g_err, sizeof(g_err)) == -EINVAL,
	      "no bytes");
	check(cert_own_der(truncated, sizeof(truncated), key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "certificate 1 of the chain"), "a value longer than its bytes");
	check(cert_own_der(indefinite, sizeof(indefinite), key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "certificate 1 of the chain"), "a length that is not definite");
	check(cert_own_der(set, sizeof(set), key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "certificate 1 of the chain"), "a value that is no SEQUENCE");
	check(cert_own_der(empty, sizeof(empty), key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "the certificate cannot be read"), "a SEQUENCE that is no certificate");
	check(cert_own_der(nokey, sizeof(nokey), key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "cannot be read"), "a certificate without its key");
	check(cert_own_der(shortkey, sizeof(shortkey), key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "cannot be read"), "a key of P-256 that is no point");
	/* The validity's first time made a PrintableString, then a letter in it. */
	n = patched(bytes, sizeof(bytes), "\027\015260101", 8, 0, 0x13);
	check(cert_own_der(bytes, n, key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "cannot be read"), "a time that is no UTCTime");
	n = patched(bytes, sizeof(bytes), "\027\015260101", 8, 3, 'x');
	check(cert_own_der(bytes, n, key, &g_own, g_err, sizeof(g_err)) &&
	      strstr(g_err, "cannot be read"), "a time that is not of digits");
	/* The subjectAltName's names made a SET: none of them is taken. */
	n = der("leaf.der", bytes, sizeof(bytes));
	for (size_t i = 0; i + 4 < n; i++)
		if (bytes[i] == 0x30 && bytes[i + 2] == 0x87 && bytes[i + 3] == 0x04 &&
		    bytes[i + 4] == 0x7f) {
			bytes[i] = 0x31;
			break;
		}
	hexkey(key);
	check(!cert_own_der(bytes, n, key, &g_own, g_err, sizeof(g_err)) && !g_own.names[0] &&
	      !g_own.nips, "names that are not a SEQUENCE are read as none");
}

static void test_own_file(void)
{
	char path[400], hex[80];
	uint8_t chain[8192];
	size_t n = der("leaf.der", chain, sizeof(chain));
	char b64[12000];
	FILE *f;

	puts("an own certificate in the file onu-config writes");
	snprintf(path, sizeof(path), "%s/own", g_dir);
	remove(path);
	check(cert_own_load(path, &g_own, g_err, sizeof(g_err)) == -ENOENT, "none without the file");
	snprintf(hex, sizeof(hex), "%s", file("leaf.hex", 0));
	hex[strcspn(hex, "\n")] = '\0';
	mg_base64_encode(chain, n, b64, sizeof(b64));
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n%s\n", hex, b64);
		fclose(f);
	}
	check(cert_own_load(path, &g_own, g_err, sizeof(g_err)) == 0 && g_own.count == 1,
	      "the key and the chain in base64");
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%.63sx\n%s\n", hex, b64);
		fclose(f);
	}
	check(cert_own_load(path, &g_own, g_err, sizeof(g_err)) == -EINVAL &&
	      strstr(g_err, "not a key and a chain"), "a key that is not hexadecimal");
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s", hex);
		fclose(f);
	}
	check(cert_own_load(path, &g_own, g_err, sizeof(g_err)) == -EINVAL, "no chain");
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n@@@@\n", hex);
		fclose(f);
	}
	check(cert_own_load(path, &g_own, g_err, sizeof(g_err)) == -EINVAL,
	      "a chain that is not base64");
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\nQUFB\n", hex);
		fclose(f);
	}
	check(cert_own_load(path, &g_own, g_err, sizeof(g_err)) == -EINVAL &&
	      strstr(g_err, "certificate 1"), "a chain that is no certificate");
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: test_cert DIR CERTS\n");
		return 2;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
	snprintf(g_certs, sizeof(g_certs), "%s", argv[2]);
	test_keys();
	test_validity();
	test_certificate();
	test_own();
	test_own_refused();
	test_own_der();
	test_own_file();
	printf("test_cert: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
