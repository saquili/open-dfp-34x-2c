// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the sign-in: PBKDF2 against RFC 7914's vectors, the stored form,
 * the default password, the stop after wrong passwords, sessions that end,
 * and a change of password through a stand-in for onu-store.
 */

#include "auth.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
	auth_pbkdf2((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, out);
	check(block_is(out, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"),
	      "one iteration gives the first block of the vector");
	auth_pbkdf2((const uint8_t *)"Password", 8, (const uint8_t *)"NaCl", 4, 80000, out);
	check(block_is(out, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"),
	      "80000 iterations give the first block of the vector");
}

static void test_stored_form(void)
{
	const uint8_t salt[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	char h[AUTH_HASH_MAX], bad[AUTH_HASH_MAX];

	puts("the stored form");
	check(auth_hash("example-secret", 1000, salt, h, sizeof(h)) == 0 &&
		      !strncmp(h, "pbkdf2-sha256$1000$0102030405060708090a0b0c0d0e0f10$", 52),
	      "names the method, the iterations and the salt");
	check(auth_verify("example-secret", h), "the same secret matches");
	check(!auth_verify("example-secreT", h), "another does not");
	snprintf(bad, sizeof(bad), "%s", h);
	bad[strlen(bad) - 1] = bad[strlen(bad) - 1] == '0' ? '1' : '0';
	check(!auth_verify("example-secret", bad), "a changed hash does not");
	check(!auth_verify("example-secret", "md5$whatever"), "another method does not");
	check(!auth_verify("example-secret", "pbkdf2-sha256$0$00$00"), "a malformed one does not");
}

static char g_root[256];
static char g_hashfile[300];

static void test_sessions(void)
{
	char id[AUTH_ID_LEN + 1], ids[AUTH_SESSIONS + 1][AUTH_ID_LEN + 1];
	int retry = 0;
	time_t t = 1000000;

	puts("the default password and sessions");
	auth_init("/run/onu-auth", NULL);
	check(auth_default_password(), "without a stored hash the default is in force");
	check(auth_sign_in("admin", "admin", t, id, &retry) == 0 && strlen(id) == AUTH_ID_LEN,
	      "admin and the default password open a session");
	check(auth_session(id, t + 10), "the session is open");
	check(!auth_session("0123456789abcdef0123456789abcdef", t + 10), "another id is not");
	check(!auth_session(id, t + 10 + AUTH_IDLE_S + 1), "it ends after the idle time");

	for (int i = 0; i < 5; i++)
		auth_sign_in("admin", "wrong", t + 2000, id, &retry);
	check(auth_sign_in("admin", "admin", t + 2001, id, &retry) == -EAGAIN && retry > 0,
	      "five wrong passwords stop sign-ins, even the right one");
	check(auth_sign_in("admin", "admin", t + 2040, id, &retry) == 0,
	      "and half a minute later they go on");
	check(auth_sign_in("root", "admin", t + 2041, id, &retry) == -EACCES,
	      "another user is refused");

	auth_init("/run/onu-auth", NULL);
	for (int i = 0; i <= AUTH_SESSIONS; i++)
		auth_sign_in("admin", "admin", t + 3000 + i, ids[i], &retry);
	check(!auth_session(ids[0], t + 3010), "a sign-in past the last slot ends the oldest session");
	check(auth_session(ids[AUTH_SESSIONS], t + 3010), "and the new one is open");
}

static void test_change(void)
{
	char a[AUTH_ID_LEN + 1], b[AUTH_ID_LEN + 1], script[400];
	int retry = 0;
	time_t t = 2000000;
	FILE *f;

	puts("a change of password");
	snprintf(script, sizeof(script), "%s/store.sh", g_root);
	f = fopen(script, "w");
	fprintf(f, "#!/bin/sh\n[ \"$1\" = auth ] && echo \"$2\" > %s\n", g_hashfile);
	fclose(f);
	chmod(script, 0755);
	auth_init("/run/onu-auth", script);

	auth_sign_in("admin", "admin", t, a, &retry);
	auth_sign_in("admin", "admin", t + 1, b, &retry);
	check(auth_change("admin", "short", a, t + 2, &retry) == -EINVAL, "a short password is refused");
	check(auth_change("admin", "admin", a, t + 2, &retry) == -EINVAL,
	      "and so is the current one again");
	check(auth_change("wrong-one", "example-secret", a, t + 2, &retry) == -EACCES,
	      "a wrong current password is refused");
	check(auth_change("admin", "example-secret", a, t + 3, &retry) == 0,
	      "a good one is stored");
	check(!auth_default_password(), "the default is no longer in force");
	check(auth_session(a, t + 4), "the session that changed it stays");
	check(!auth_session(b, t + 4), "the others, opened with the old password, end");
	check(auth_sign_in("admin", "admin", t + 5, b, &retry) == -EACCES,
	      "the old password no longer opens a session");
	check(auth_sign_in("admin", "example-secret", t + 6, b, &retry) == 0,
	      "the new one does");
}

int main(void)
{
	const char *root = getenv("POND_ROOT");

	if (!root) {
		fprintf(stderr, "POND_ROOT is unset: point it at an empty directory\n");
		return 2;
	}
	snprintf(g_root, sizeof(g_root), "%s", root);
	snprintf(g_hashfile, sizeof(g_hashfile), "%s/run/onu-auth", root);
	{
		char run[300];

		snprintf(run, sizeof(run), "%s/run", root);
		mkdir(run, 0755);
		unlink(g_hashfile);
	}

	test_pbkdf2();
	test_stored_form();
	test_sessions();
	test_change();

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
