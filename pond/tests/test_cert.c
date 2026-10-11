// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the panel's certificate: the keys it takes and refuses, the
 * periods of its validity, and that the same key, address and period give
 * the same certificate and nothing else does. The certificate itself, its
 * fields and its signature, OpenSSL reads in test_tls.sh: this program
 * writes it to the directory of its first argument as cert.der, with its
 * fingerprint in cert.fp.
 */

#include "cert.h"

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

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: test_cert DIR\n");
		return 2;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
	test_keys();
	test_validity();
	test_certificate();
	printf("test_cert: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
