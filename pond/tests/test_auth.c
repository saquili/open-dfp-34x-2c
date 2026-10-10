// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the sign-in: the default password, the stop after wrong
 * passwords, sessions that end, a change of password, and a password of the
 * older form that a sign-in stores again. onu-config checks the passwords,
 * as onu-act has it do on the stick (onu-utils tests the hashes); a
 * stand-in in front of it keeps a new one.
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

/* PBKDF2-HMAC-SHA256 of "example-secret", 1000 iterations, salt 01..10. */
#define OLDER_HASH "pbkdf2-sha256$1000$0102030405060708090a0b0c0d0e0f10$" \
	"682b24383b17ce055ed70ead8c23895db05d4a3717dffd00133fc69d366d3627"

static char g_root[256];
static char g_hashfile[300];

static void test_sessions(void)
{
	char id[AUTH_ID_LEN + 1], ids[AUTH_SESSIONS + 1][AUTH_ID_LEN + 1];
	int retry = 0;
	time_t t = 1000000;

	puts("the default password and sessions");
	auth_init();
	check(auth_default_password(), "without a stored hash the default is in force");
	check(auth_sign_in("root", "admin", t, id, &retry) == 0 && strlen(id) == AUTH_ID_LEN,
	      "root and the default password open a session");
	check(auth_session(id, t + 10), "the session is open");
	check(!auth_session("0123456789abcdef0123456789abcdef", t + 10), "another id is not");
	check(!auth_session(id, t + 10 + AUTH_IDLE_S + 1), "it ends after the idle time");

	for (int i = 0; i < 5; i++)
		auth_sign_in("root", "wrong", t + 2000, id, &retry);
	check(auth_sign_in("root", "admin", t + 2001, id, &retry) == -EAGAIN && retry > 0,
	      "five wrong passwords stop sign-ins, even the right one");
	check(auth_sign_in("root", "admin", t + 2040, id, &retry) == 0,
	      "and half a minute later they go on");
	check(auth_sign_in("admin", "admin", t + 2041, id, &retry) == -EACCES,
	      "another user is refused");

	auth_init();
	for (int i = 0; i <= AUTH_SESSIONS; i++)
		auth_sign_in("root", "admin", t + 3000 + i, ids[i], &retry);
	check(!auth_session(ids[0], t + 3010), "a sign-in past the last slot ends the oldest session");
	check(auth_session(ids[AUTH_SESSIONS], t + 3010), "and the new one is open");
}

/*
 * A stand-in for onu-config in front of the real one, $POND_ONU_CONFIG as the
 * tests start: "password" hashes the line of its standard input with crypt()
 * (Perl's, which is the C library's) and keeps it in the hash file; the
 * password "store-refuses" fails, as a store that cannot be written does.
 * Every other verb is the real onu-config's.
 */
static char g_real[300];

static void write_store(char *script, size_t cap)
{
	FILE *f;

	snprintf(script, cap, "%s/store.sh", g_root);
	f = fopen(script, "w");
	fprintf(f, "#!/bin/sh\n"
		"[ \"$1\" = password ] || exec %s \"$@\"\n"
		"IFS= read -r s\n"
		"[ \"$s\" = store-refuses ] && exit 1\n"
		"perl -e 'print crypt($ARGV[0], q($5$rounds=1000$teststandinsalt)), \"\\n\"' "
		"\"$s\" > %s\n", g_real, g_hashfile);
	fclose(f);
	chmod(script, 0755);
	setenv("POND_ONU_CONFIG", script, 1);
}

static bool hash_file_starts(const char *prefix)
{
	char line[AUTH_HASH_MAX] = "";
	FILE *f = fopen(g_hashfile, "r");

	if (!f)
		return false;
	if (!fgets(line, sizeof(line), f))
		line[0] = '\0';
	fclose(f);
	return !strncmp(line, prefix, strlen(prefix));
}

static void test_change(void)
{
	char a[AUTH_ID_LEN + 1], b[AUTH_ID_LEN + 1], script[400];
	int retry = 0;
	time_t t = 2000000;

	puts("a change of password");
	write_store(script, sizeof(script));
	auth_init();

	auth_sign_in("root", "admin", t, a, &retry);
	auth_sign_in("root", "admin", t + 1, b, &retry);
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
	check(auth_sign_in("root", "admin", t + 5, b, &retry) == -EACCES,
	      "the old password no longer opens a session");
	check(auth_sign_in("root", "example-secret", t + 6, b, &retry) == 0,
	      "the new one does");
	check(hash_file_starts("$5$rounds=1000$teststandinsalt$"),
	      "the hash kept is crypt()'s");
	check(auth_change("example-secret", "store-refuses", a, t + 7, &retry) == -EIO,
	      "a store that cannot keep a new one is an error");
	check(auth_sign_in("root", "example-secret", t + 8, b, &retry) == 0,
	      "and the password stays as it was");
}

static void test_older_form(void)
{
	char id[AUTH_ID_LEN + 1], script[400];
	int retry = 0;
	time_t t = 3000000;
	FILE *f;

	puts("a password kept in the older form");
	write_store(script, sizeof(script));
	f = fopen(g_hashfile, "w");
	fprintf(f, "%s\n", OLDER_HASH);
	fclose(f);
	auth_init();

	check(!auth_default_password(), "it is no default");
	check(auth_sign_in("root", "example-secreT", t, id, &retry) == -EACCES,
	      "a wrong password is refused");
	check(hash_file_starts("pbkdf2-sha256$"), "and the hash stays as it was");
	check(auth_sign_in("root", "example-secret", t + 1, id, &retry) == 0,
	      "the right one opens a session");
	check(hash_file_starts("$5$rounds=1000$teststandinsalt$"),
	      "and has the password kept in crypt()'s form");
	check(auth_sign_in("root", "example-secret", t + 2, id, &retry) == 0,
	      "which opens a session as well");
}

int main(void)
{
	const char *root = getenv("POND_ROOT");

	if (!root) {
		fprintf(stderr, "POND_ROOT is unset: point it at an empty directory\n");
		return 2;
	}
	snprintf(g_root, sizeof(g_root), "%s", root);
	snprintf(g_real, sizeof(g_real), "%s", getenv("POND_ONU_CONFIG") ?: "onu-config");
	snprintf(g_hashfile, sizeof(g_hashfile), "%s/run/onu-auth", root);
	{
		char run[300];

		snprintf(run, sizeof(run), "%s/run", root);
		mkdir(run, 0755);
		unlink(g_hashfile);
	}

	test_sessions();
	test_change();
	test_older_form();

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
