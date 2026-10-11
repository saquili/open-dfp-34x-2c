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

/*
 * Wrong passwords stop sign-ins for LOCK_S seconds: LOCK_AFTER of them from
 * one address stop that address, so that a page of another site, or one
 * machine of the host side, cannot lock the others out; ALL_AFTER of them
 * from any stop everyone, so that guesses spread over many addresses stay
 * as few. A count is forgotten FORGET_S seconds after its last wrong one.
 */
#define LOCK_AFTER 5
#define ALL_AFTER 20
#define LOCK_S 30
#define FORGET_S 300
#define SOURCES 16
#define SOURCE_LEN 48

/* What onu-config password-check says of a password (onu-utils). */
enum { CHECK_MATCHES = 0, CHECK_DEFAULT = 3, CHECK_OLDER = 4, CHECK_UNKNOWN = 5 };

static struct {
	char id[AUTH_ID_LEN + 1];
	time_t created;
	time_t seen;
} g_sess[AUTH_SESSIONS];

/* The wrong passwords of each address lately, and of all together. */
static struct fails {
	char from[SOURCE_LEN];
	int n;
	time_t last;
} g_src[SOURCES], g_all;

static bool g_default;

void auth_init(void)
{
	char out[32];

	memset(g_sess, 0, sizeof(g_sess));
	memset(g_src, 0, sizeof(g_src));
	memset(&g_all, 0, sizeof(g_all));
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
 * 0 when @secret is the password in force, -EACCES when it is not, and
 * -ENOKEY while no password is known, without a store onu-config could
 * read; @older says whether it matched a hash of the older form, which a
 * sign-in replaces.
 */
static int correct(const char *secret, bool *older)
{
	int rc = check(secret);

	*older = rc == CHECK_OLDER;
	if (rc == CHECK_UNKNOWN)
		return -ENOKEY;
	if (rc == CHECK_MATCHES || rc == CHECK_OLDER)
		g_default = false;
	else if (rc == CHECK_DEFAULT)
		g_default = true;
	return rc == CHECK_MATCHES || rc == CHECK_OLDER || rc == CHECK_DEFAULT ?
		0 : -EACCES;
}

/*
 * Have onu-config hash @secret for crypt() and keep it; onu-act wants the
 * password in force, @current, with it.
 */
static int store_password(const char *secret, const char *current)
{
	static const char *const names[] = { "password" };
	char in[AUTH_SECRET_MAX + 2], out[256];
	int rc;

	if (strlen(secret) > AUTH_SECRET_MAX)
		return -EINVAL;
	snprintf(in, sizeof(in), "%s\n", secret);
	rc = bus_act("password_set", names, &current, 1, in, out, sizeof(out));
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
/* The count of @from, a free or the stalest one when @take, else NULL. */
static struct fails *source(const char *from, bool take)
{
	struct fails *pick = NULL;

	from = from ? from : "";
	for (int i = 0; i < SOURCES; i++)
		if (g_src[i].n && !strcmp(g_src[i].from, from))
			return &g_src[i];
	if (!take)
		return NULL;
	for (int i = 0; i < SOURCES; i++) {
		if (!g_src[i].n) {
			pick = &g_src[i];
			break;
		}
		if (!pick || g_src[i].last < pick->last)
			pick = &g_src[i];
	}
	memset(pick, 0, sizeof(*pick));
	snprintf(pick->from, sizeof(pick->from), "%s", from);
	return pick;
}

/* Whether @f stops sign-ins now, after @after wrong ones, and how long. */
static bool stopped(struct fails *f, int after, time_t now, int *retry_after)
{
	if (!f)
		return false;
	if (f->n && now - f->last > FORGET_S)
		f->n = 0;
	if (f->n >= after && now - f->last < LOCK_S) {
		if (retry_after)
			*retry_after = (int)(LOCK_S - (now - f->last));
		return true;
	}
	return false;
}

/* Whether sign-ins from @from are stopped now, and for how long. */
static bool locked(const char *from, time_t now, int *retry_after)
{
	return stopped(&g_all, ALL_AFTER, now, retry_after) ||
	       stopped(source(from, false), LOCK_AFTER, now, retry_after);
}

static void failed(const char *from, time_t now)
{
	struct fails *f = source(from, true);

	f->n++;
	f->last = now;
	g_all.n++;
	g_all.last = now;
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

int auth_sign_in(const char *user, const char *secret, const char *from,
		 time_t now, char id[AUTH_ID_LEN + 1], int *retry_after)
{
	int i, slot = 0, rc;
	bool older;

	if (locked(from, now, retry_after))
		return -EAGAIN;
	rc = user && secret && !strcmp(user, AUTH_USER) ?
		correct(secret, &older) : -EACCES;
	if (rc == -EACCES)
		failed(from, now);
	if (rc)
		return rc;
	if (source(from, false))
		source(from, false)->n = 0;
	/*
	 * A password kept in the older form becomes crypt()'s, which the
	 * console and the ssh server check too. Should that fail, the panel
	 * still takes it as it is.
	 */
	if (older && store_password(secret, secret))
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

int auth_confirm(const char *secret, const char *from, time_t now, int *retry_after)
{
	bool older;
	int rc;

	if (locked(from, now, retry_after))
		return -EAGAIN;
	rc = secret ? correct(secret, &older) : -EACCES;
	if (rc == -EACCES)
		failed(from, now);
	return rc;
}

int auth_change(const char *current, const char *next, const char *keep,
		const char *from, time_t now, int *retry_after)
{
	bool older;
	int rc, i;

	if (locked(from, now, retry_after))
		return -EAGAIN;
	rc = current ? correct(current, &older) : -EACCES;
	if (rc == -EACCES)
		failed(from, now);
	if (rc)
		return rc;
	if (!next || !printable(next, AUTH_SECRET_MIN, AUTH_SECRET_MAX) ||
	    !strcmp(next, current))
		return -EINVAL;

	rc = store_password(next, current);
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
