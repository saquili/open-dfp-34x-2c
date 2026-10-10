// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Passwords and sessions (auth.h). Root's password is checked, and a new
 * one kept, by onu-config as root, through onu-act (bus.h): pond holds
 * neither the hash nor a program of the stick. Whether the default password
 * is in force is asked at the start and known after each sign-in and
 * change.
 */

#define _GNU_SOURCE

#include "auth.h"
#include "bus.h"
#include "prov.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCK_AFTER 5
#define LOCK_S 30
#define FORGET_S 300

/* What onu-config password-check says of a password (onu-utils). */
enum { CHECK_MATCHES = 0, CHECK_DEFAULT = 3, CHECK_OLDER = 4 };

static struct {
	char id[AUTH_ID_LEN + 1];
	time_t created;
	time_t seen;
} g_sess[AUTH_SESSIONS];

static int g_fails;
static time_t g_last_fail;
static bool g_default;

void auth_init(void)
{
	char out[32];

	memset(g_sess, 0, sizeof(g_sess));
	g_fails = 0;
	g_last_fail = 0;
	g_default = bus_act("password_state", NULL, NULL, 0, NULL, out, sizeof(out)) == 0 &&
		    !strncmp(out, "default", 7);
}

bool auth_default_password(void)
{
	return g_default;
}

/* What onu-config password-check says of @secret, or a negative errno. */
static int check(const char *secret)
{
	char in[AUTH_SECRET_MAX + 2], out[64];
	int rc;

	if (strlen(secret) > AUTH_SECRET_MAX || strchr(secret, '\n'))
		return 1;
	snprintf(in, sizeof(in), "%s\n", secret);
	rc = bus_act("password_check", NULL, NULL, 0, in, out, sizeof(out));
	explicit_bzero(in, sizeof(in));
	return rc;
}

/*
 * Whether @secret is the password in force; @older says whether it matched
 * a hash of the older form, which a sign-in replaces.
 */
static bool correct(const char *secret, bool *older)
{
	int rc = check(secret);

	*older = rc == CHECK_OLDER;
	if (rc == CHECK_MATCHES || rc == CHECK_OLDER)
		g_default = false;
	else if (rc == CHECK_DEFAULT)
		g_default = true;
	return rc == CHECK_MATCHES || rc == CHECK_OLDER || rc == CHECK_DEFAULT;
}

/* Have onu-config hash @secret for crypt() and keep it. */
static int store_password(const char *secret)
{
	char in[AUTH_SECRET_MAX + 2], out[256];
	int rc;

	if (strlen(secret) > AUTH_SECRET_MAX)
		return -EINVAL;
	snprintf(in, sizeof(in), "%s\n", secret);
	rc = bus_act("password_set", NULL, NULL, 0, in, out, sizeof(out));
	explicit_bzero(in, sizeof(in));
	return rc ? -EIO : 0;
}

static void hex(const uint8_t *in, size_t n, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * n] = '\0';
}

/* Whether @a and @b are the same, compared in constant time. */
static bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t diff = 0;

	for (size_t i = 0; i < n; i++)
		diff |= a[i] ^ b[i];
	return diff == 0;
}

/* Whether sign-ins are stopped now, and for how long. */
static bool locked(time_t now, int *retry_after)
{
	if (g_fails && now - g_last_fail > FORGET_S)
		g_fails = 0;
	if (g_fails >= LOCK_AFTER && now - g_last_fail < LOCK_S) {
		if (retry_after)
			*retry_after = (int)(LOCK_S - (now - g_last_fail));
		return true;
	}
	return false;
}

static void failed(time_t now)
{
	g_fails++;
	g_last_fail = now;
}

/* ---- sessions ---- */

static int new_id(char id[AUTH_ID_LEN + 1])
{
	uint8_t raw[AUTH_ID_LEN / 2];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	ssize_t got;

	if (fd < 0)
		return -EIO;
	got = read(fd, raw, sizeof(raw));
	close(fd);
	if (got != (ssize_t)sizeof(raw))
		return -EIO;
	hex(raw, sizeof(raw), id);
	explicit_bzero(raw, sizeof(raw));
	return 0;
}

/* Close the sessions idle too long or open too long. */
static void expire(time_t now)
{
	int i;

	for (i = 0; i < AUTH_SESSIONS; i++)
		if (g_sess[i].id[0] && (now - g_sess[i].seen > AUTH_IDLE_S ||
					now - g_sess[i].created > AUTH_MAX_AGE_S))
			memset(&g_sess[i], 0, sizeof(g_sess[i]));
}

/* The slot of an open session, every slot compared. */
static int slot_of(const char *id, time_t now)
{
	int i, hit = -1;

	expire(now);
	if (!id || strlen(id) != AUTH_ID_LEN)
		return -1;
	for (i = 0; i < AUTH_SESSIONS; i++)
		if (g_sess[i].id[0] &&
		    same((const uint8_t *)id, (const uint8_t *)g_sess[i].id,
			 AUTH_ID_LEN))
			hit = i;
	return hit;
}

int auth_sign_in(const char *user, const char *secret, time_t now,
		 char id[AUTH_ID_LEN + 1], int *retry_after)
{
	int i, slot = 0;
	bool older;

	if (locked(now, retry_after))
		return -EAGAIN;
	if (!user || !secret || strcmp(user, AUTH_USER) ||
	    !correct(secret, &older)) {
		failed(now);
		return -EACCES;
	}
	g_fails = 0;
	/*
	 * A password kept in the older form becomes crypt()'s, which the
	 * console and the ssh server check too. Should that fail, the panel
	 * still takes it as it is.
	 */
	if (older && store_password(secret))
		fprintf(stderr, "pond: the password stays in its older form\n");

	/* A free slot, else the one idle the longest. */
	expire(now);
	for (i = 0; i < AUTH_SESSIONS; i++) {
		if (!g_sess[i].id[0]) {
			slot = i;
			break;
		}
		if (g_sess[i].seen < g_sess[slot].seen)
			slot = i;
	}
	if (new_id(g_sess[slot].id)) {
		memset(&g_sess[slot], 0, sizeof(g_sess[slot]));
		return -EIO;
	}
	g_sess[slot].created = now;
	g_sess[slot].seen = now;
	memcpy(id, g_sess[slot].id, AUTH_ID_LEN + 1);
	return 0;
}

bool auth_session(const char *id, time_t now)
{
	int i = slot_of(id, now);

	if (i < 0)
		return false;
	g_sess[i].seen = now;
	return true;
}

void auth_sign_out(const char *id)
{
	int i = slot_of(id, time(NULL));

	if (i >= 0)
		memset(&g_sess[i], 0, sizeof(g_sess[i]));
}

static bool printable(const char *s, size_t min, size_t max)
{
	size_t n = strlen(s), i;

	if (n < min || n > max)
		return false;
	for (i = 0; i < n; i++)
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
			return false;
	return true;
}

int auth_change(const char *current, const char *next, const char *keep,
		time_t now, int *retry_after)
{
	bool older;
	int rc, i;

	if (locked(now, retry_after))
		return -EAGAIN;
	if (!current || !correct(current, &older)) {
		failed(now);
		return -EACCES;
	}
	if (!next || !printable(next, AUTH_SECRET_MIN, AUTH_SECRET_MAX) ||
	    !strcmp(next, current))
		return -EINVAL;

	rc = store_password(next);
	if (rc)
		return rc;
	g_default = false;

	/* The other sessions were opened with the old password. */
	i = slot_of(keep, now);
	for (int j = 0; j < AUTH_SESSIONS; j++)
		if (j != i)
			memset(&g_sess[j], 0, sizeof(g_sess[j]));
	return 0;
}
