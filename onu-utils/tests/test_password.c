// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of root's password against its hash (password.h). */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "password.h"

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static bool block_is(const uint8_t got[32], const char *want)
{
	char hex[65];

	for (int i = 0; i < 32; i++)
		snprintf(hex + 2 * i, 3, "%02x", got[i]);
	return !strcmp(hex, want);
}

static void test_pbkdf2(void)
{
	uint8_t out[32];

	puts("PBKDF2-HMAC-SHA256, RFC 7914 section 11");
	password_pbkdf2((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, out);
	CHECK(block_is(out, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"));
	password_pbkdf2((const uint8_t *)"Password", 8, (const uint8_t *)"NaCl", 4, 80000, out);
	CHECK(block_is(out, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"));
}

/* PBKDF2-HMAC-SHA256 of "example-secret", 1000 iterations, salt 01..10. */
static const char g_older[] =
	"pbkdf2-sha256$1000$0102030405060708090a0b0c0d0e0f10$682b24383b17ce055ed70ead8c23895db05d4a3717dffd00133fc69d366d3627";

/* The SHA-256 crypt specification's own vectors. */
static const char g_crypt[] = "$5$saltstring$5B8vYYiY.CVt1RlTTf8KbXBH3hsxY/GNooZaBBGWEc5";
static const char g_rounds[] =
	"$5$rounds=10000$saltstringsaltst$3xv.VbSHBb41AL9AvLeujZkZRBAwqFMz2.opqey6IcA";

static void test_forms(void)
{
	char bad[200];

	puts("the stored forms");
	CHECK(password_verify("Hello world!", g_crypt) == PASSWORD_MATCHES);
	CHECK(password_verify("Hello world!", g_rounds) == PASSWORD_MATCHES);
	CHECK(password_verify("Hello world?", g_crypt) == PASSWORD_WRONG);
	snprintf(bad, sizeof(bad), "%s", g_crypt);
	bad[strlen(bad) - 1] = '6';
	CHECK(password_verify("Hello world!", bad) == PASSWORD_WRONG);
	CHECK(password_verify("example-secret", g_older) == PASSWORD_OLDER);
	CHECK(password_verify("example-secreT", g_older) == PASSWORD_WRONG);
	snprintf(bad, sizeof(bad), "%s", g_older);
	bad[strlen(bad) - 1] = '8';
	CHECK(password_verify("example-secret", bad) == PASSWORD_WRONG);
	CHECK(password_verify("example-secret", "md5$whatever") == PASSWORD_WRONG);
	CHECK(password_verify("example-secret", "pbkdf2-sha256$0$00$00") == PASSWORD_WRONG);
	CHECK(password_verify("", "*") == PASSWORD_WRONG);
	CHECK(password_verify("", "") == PASSWORD_WRONG);
}

int main(void)
{
	test_pbkdf2();
	test_forms();
	printf("test_password: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
