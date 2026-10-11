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
#include <poll.h>
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
 * From what onu-config password-check said of a password, @rc: 0 when it is
 * the password in force, -EACCES when it is not, and -ENOKEY while no
 * password is known, without a store onu-config could read; @older says
 * whether it matched a hash of the older form, which a sign-in replaces.
 */
static int verdict(int rc, bool *older)
{
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

static int correct(const char *secret, bool *older)
{
	return verdict(check(secret), older);
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

/*
 * A sign-in or a change of the password whose check runs: one at a time, so
 * that a wrong password is counted before the next one is tried.
 */
static struct {
	auth_done done;
	void *ctx;
	bool change;
	char from[SOURCE_LEN];
	time_t now;
	char secret[AUTH_SECRET_MAX + 1];
	char next[AUTH_SECRET_MAX + 1];
	bool next_ok;
	char keep[AUTH_ID_LEN + 1];
} g_op;

/* End the sign-in or change under way with @rc, and tell its caller. */
static void op_end(int rc, const char *id)
{
	auth_done done = g_op.done;
	void *ctx = g_op.ctx;

	explicit_bzero(&g_op, sizeof(g_op));
	if (done)
		done(ctx, rc, id ? id : "");
}

/* Open a session at @now for a password that was right: 0, or -EIO. */
static int open_session(time_t now, char id[AUTH_ID_LEN + 1])
{
	int i, slot = 0;

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

static void stored(void *ctx, int code, const char *output)
{
	int i;

	(void)ctx;
	(void)output;
	if (code) {
		op_end(-EIO, NULL);
		return;
	}
	g_default = false;
	/* The other sessions were opened with the old password. */
	i = slot_of(g_op.keep, g_op.now);
	for (int j = 0; j < AUTH_SESSIONS; j++)
		if (j != i)
			memset(&g_sess[j], 0, sizeof(g_sess[j]));
	op_end(0, NULL);
}

static void checked(void *ctx, int code, const char *output)
{
	static const char *const names[] = { "password" };
	const char *const values[] = { g_op.secret };
	char id[AUTH_ID_LEN + 1] = "", in[AUTH_SECRET_MAX + 2];
	bool older;
	int rc = verdict(code, &older);

	(void)ctx;
	(void)output;
	if (rc == -EACCES)
		failed(g_op.from, g_op.now);
	if (rc) {
		op_end(rc, NULL);
		return;
	}
	if (g_op.change) {
		if (!g_op.next_ok ||
		    !printable(g_op.next, AUTH_SECRET_MIN, AUTH_SECRET_MAX) ||
		    !strcmp(g_op.next, g_op.secret)) {
			op_end(-EINVAL, NULL);
			return;
		}
		snprintf(in, sizeof(in), "%s\n", g_op.next);
		rc = bus_act_begin("password_set", names, values, 1, in, stored, NULL);
		explicit_bzero(in, sizeof(in));
		if (rc)
			op_end(-EIO, NULL);
		return;
	}
	if (source(g_op.from, false))
		source(g_op.from, false)->n = 0;
	/*
	 * A password kept in the older form becomes crypt()'s, which the
	 * console and the ssh server check too; this happens once, and is
	 * waited for. Should that fail, the panel still takes it as it is.
	 */
	if (older && store_password(g_op.secret, g_op.secret))
		fprintf(stderr, "pond: the password stays in its older form\n");
	rc = open_session(g_op.now, id);
	op_end(rc, rc ? NULL : id);
	explicit_bzero(id, sizeof(id));
}

/* Start the check of @secret for g_op: -EINPROGRESS, or why not. */
static int op_check(const char *secret)
{
	char in[AUTH_SECRET_MAX + 2];
	int rc;

	snprintf(in, sizeof(in), "%s\n", secret);
	rc = bus_act_begin("password_check", NULL, NULL, 0, in, checked, NULL);
	explicit_bzero(in, sizeof(in));
	if (rc) {
		explicit_bzero(&g_op, sizeof(g_op));
		return rc == -EBUSY ? -EAGAIN : -EIO;
	}
	return -EINPROGRESS;
}

/* Take g_op for a new check; -EAGAIN for a second while one runs. */
static int op_take(const char *from, time_t now, int *retry_after,
		   auth_done done, void *ctx)
{
	if (g_op.done) {
		if (retry_after)
			*retry_after = 1;
		return -EAGAIN;
	}
	if (locked(from, now, retry_after))
		return -EAGAIN;
	g_op.done = done;
	g_op.ctx = ctx;
	snprintf(g_op.from, sizeof(g_op.from), "%s", from ? from : "");
	g_op.now = now;
	return 0;
}

int auth_sign_in_begin(const char *user, const char *secret, const char *from,
		       time_t now, int *retry_after, auth_done done, void *ctx)
{
	int rc = op_take(from, now, retry_after, done, ctx);

	if (rc)
		return rc;
	if (!user || !secret || strcmp(user, AUTH_USER)) {
		explicit_bzero(&g_op, sizeof(g_op));
		failed(from, now);
		return -EACCES;
	}
	if (strlen(secret) > AUTH_SECRET_MAX || strchr(secret, '\n')) {
		explicit_bzero(&g_op, sizeof(g_op));
		failed(from, now);
		return -EACCES;
	}
	snprintf(g_op.secret, sizeof(g_op.secret), "%s", secret);
	return op_check(secret);
}

/* What a sign-in or a change not waited for gave, for the waiting wrappers. */
struct waited {
	bool ended;
	int rc;
	char *id;
};

static void op_waited(void *ctx, int rc, const char *id)
{
	struct waited *w = ctx;

	w->rc = rc;
	if (w->id)
		memcpy(w->id, id, strlen(id) + 1);
	w->ended = true;
}

/* Wait for the sign-in or change whose start returned @rc. */
static int op_wait(int rc, struct waited *w)
{
	if (rc != -EINPROGRESS)
		return rc;
	while (!w->ended) {
		bus_poll();
		if (!w->ended)
			poll(NULL, 0, 10);
	}
	return w->rc;
}

int auth_sign_in(const char *user, const char *secret, const char *from,
		 time_t now, char id[AUTH_ID_LEN + 1], int *retry_after)
{
	struct waited w = { .id = id };

	id[0] = '\0';
	return op_wait(auth_sign_in_begin(user, secret, from, now, retry_after,
					  op_waited, &w), &w);
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

int auth_change_begin(const char *current, const char *next, const char *keep,
		      const char *from, time_t now, int *retry_after,
		      auth_done done, void *ctx)
{
	int rc = op_take(from, now, retry_after, done, ctx);

	if (rc)
		return rc;
	if (!current || strlen(current) > AUTH_SECRET_MAX || strchr(current, '\n')) {
		explicit_bzero(&g_op, sizeof(g_op));
		failed(from, now);
		return -EACCES;
	}
	g_op.change = true;
	snprintf(g_op.secret, sizeof(g_op.secret), "%s", current);
	/* A new password that cannot be one is refused once the current one is right. */
	g_op.next_ok = next && strlen(next) <= AUTH_SECRET_MAX;
	if (g_op.next_ok)
		snprintf(g_op.next, sizeof(g_op.next), "%s", next);
	snprintf(g_op.keep, sizeof(g_op.keep), "%s", keep ? keep : "");
	return op_check(current);
}

int auth_change(const char *current, const char *next, const char *keep,
		const char *from, time_t now, int *retry_after)
{
	struct waited w = { 0 };

	return op_wait(auth_change_begin(current, next, keep, from, now,
					 retry_after, op_waited, &w), &w);
}
