// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Passwords and sessions (auth.h). The hash is PBKDF2-HMAC-SHA256 over a
 * random 16-byte salt, on Mongoose's SHA-256; the HMAC keeps its two padded
 * key states, so an iteration costs two compressions.
 */

#define _GNU_SOURCE

#include "auth.h"
#include "prov.h"

#include "mongoose.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOCK_AFTER 5
#define LOCK_S 30
#define FORGET_S 300

static const char *g_hashfile = "/run/onu-auth";
static const char *g_store = "onu-store";

static struct {
	char id[AUTH_ID_LEN + 1];
	time_t created;
	time_t seen;
} g_sess[AUTH_SESSIONS];

static int g_fails;
static time_t g_last_fail;

void auth_init(const char *hashfile, const char *store)
{
	if (hashfile)
		g_hashfile = hashfile;
	if (store)
		g_store = store;
	memset(g_sess, 0, sizeof(g_sess));
	g_fails = 0;
	g_last_fail = 0;
}

/* ---- PBKDF2-HMAC-SHA256 ---- */

struct hmac {
	mg_sha256_ctx inner;
	mg_sha256_ctx outer;
};

static void hmac_key(struct hmac *h, const uint8_t *key, size_t len)
{
	uint8_t k[64] = { 0 }, pad[64];
	size_t i;

	if (len > sizeof(k)) {
		mg_sha256_ctx c;

		mg_sha256_init(&c);
		mg_sha256_update(&c, key, len);
		mg_sha256_final(k, &c);
	} else {
		memcpy(k, key, len);
	}
	for (i = 0; i < sizeof(pad); i++)
		pad[i] = k[i] ^ 0x36;
	mg_sha256_init(&h->inner);
	mg_sha256_update(&h->inner, pad, sizeof(pad));
	for (i = 0; i < sizeof(pad); i++)
		pad[i] = k[i] ^ 0x5c;
	mg_sha256_init(&h->outer);
	mg_sha256_update(&h->outer, pad, sizeof(pad));
	explicit_bzero(k, sizeof(k));
	explicit_bzero(pad, sizeof(pad));
}

static void hmac_run(const struct hmac *h, const uint8_t *a, size_t alen,
		     const uint8_t *b, size_t blen, uint8_t out[32])
{
	mg_sha256_ctx c = h->inner;
	uint8_t inner[32];

	mg_sha256_update(&c, a, alen);
	if (blen)
		mg_sha256_update(&c, b, blen);
	mg_sha256_final(inner, &c);
	c = h->outer;
	mg_sha256_update(&c, inner, sizeof(inner));
	mg_sha256_final(out, &c);
}

void auth_pbkdf2(const uint8_t *secret, size_t secret_len,
		 const uint8_t *salt, size_t salt_len, unsigned int iterations,
		 uint8_t out[32])
{
	static const uint8_t block1[4] = { 0, 0, 0, 1 };
	struct hmac h;
	uint8_t u[32];
	unsigned int i;
	size_t j;

	hmac_key(&h, secret, secret_len);
	hmac_run(&h, salt, salt_len, block1, sizeof(block1), u);
	memcpy(out, u, sizeof(u));
	for (i = 1; i < iterations; i++) {
		hmac_run(&h, u, sizeof(u), NULL, 0, u);
		for (j = 0; j < sizeof(u); j++)
			out[j] ^= u[j];
	}
	explicit_bzero(&h, sizeof(h));
	explicit_bzero(u, sizeof(u));
}

/* ---- the stored form ---- */

static void hex(const uint8_t *in, size_t n, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * n] = '\0';
}

static bool unhex(const char *in, size_t n, uint8_t *out)
{
	size_t i;

	for (i = 0; i < 2 * n; i++) {
		char c = in[i];
		int v = c >= '0' && c <= '9' ? c - '0' :
			c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

		if (v < 0)
			return false;
		out[i / 2] = (uint8_t)(i % 2 ? (out[i / 2] | v) : v << 4);
	}
	return in[2 * n] == '\0' || in[2 * n] == '$';
}

int auth_hash(const char *secret, unsigned int iterations,
	      const uint8_t salt[16], char *out, size_t cap)
{
	uint8_t dk[32];
	char salt_hex[33], dk_hex[65];
	int n;

	auth_pbkdf2((const uint8_t *)secret, strlen(secret), salt, 16,
		    iterations, dk);
	hex(salt, 16, salt_hex);
	hex(dk, sizeof(dk), dk_hex);
	explicit_bzero(dk, sizeof(dk));
	n = snprintf(out, cap, "pbkdf2-sha256$%u$%s$%s", iterations, salt_hex,
		     dk_hex);
	return n > 0 && (size_t)n < cap ? 0 : -ENOSPC;
}

static bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t diff = 0;
	size_t i;

	for (i = 0; i < n; i++)
		diff |= a[i] ^ b[i];
	return diff == 0;
}

bool auth_verify(const char *secret, const char *stored)
{
	static const char prefix[] = "pbkdf2-sha256$";
	uint8_t salt[16], want[32], got[32];
	const char *p = stored;
	unsigned long iterations;
	char *end;
	bool ok;

	if (strncmp(p, prefix, sizeof(prefix) - 1))
		return false;
	p += sizeof(prefix) - 1;
	iterations = strtoul(p, &end, 10);
	if (end == p || *end != '$' || iterations < 1 || iterations > 10000000)
		return false;
	p = end + 1;
	if (!unhex(p, sizeof(salt), salt) || p[2 * sizeof(salt)] != '$')
		return false;
	p += 2 * sizeof(salt) + 1;
	if (!unhex(p, sizeof(want), want))
		return false;

	auth_pbkdf2((const uint8_t *)secret, strlen(secret), salt, sizeof(salt),
		    (unsigned int)iterations, got);
	ok = same(got, want, sizeof(got));
	explicit_bzero(got, sizeof(got));
	return ok;
}

/* ---- the password in force ---- */

/* The stored hash, or an empty string while the default is in force. */
static void stored(char *out, size_t cap)
{
	if (prov_read_line(g_hashfile, out, cap))
		*out = '\0';
	out[strcspn(out, "\r\n ")] = '\0';
}

bool auth_default_password(void)
{
	char h[AUTH_HASH_MAX];

	stored(h, sizeof(h));
	return *h == '\0';
}

static bool correct(const char *secret)
{
	char h[AUTH_HASH_MAX];

	stored(h, sizeof(h));
	if (*h)
		return auth_verify(secret, h);
	/* The default password, compared without a shortcut either way. */
	return strlen(secret) == 5 && same((const uint8_t *)secret,
					   (const uint8_t *)"admin", 5);
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

	if (locked(now, retry_after))
		return -EAGAIN;
	if (!user || !secret || strcmp(user, AUTH_USER) || !correct(secret)) {
		failed(now);
		return -EACCES;
	}
	g_fails = 0;

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

/* Run "STORE auth HASH" and wait for it. */
static int store_hash(const char *hash)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return -errno;
	if (pid == 0) {
		execlp(g_store, g_store, "auth", hash, (char *)NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -errno;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

int auth_change(const char *current, const char *next, const char *keep,
		time_t now, int *retry_after)
{
	uint8_t salt[16];
	char hash[AUTH_HASH_MAX];
	int fd, rc, i;
	ssize_t got;

	if (locked(now, retry_after))
		return -EAGAIN;
	if (!current || !correct(current)) {
		failed(now);
		return -EACCES;
	}
	if (!next || !printable(next, AUTH_SECRET_MIN, AUTH_SECRET_MAX) ||
	    !strcmp(next, current))
		return -EINVAL;

	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -EIO;
	got = read(fd, salt, sizeof(salt));
	close(fd);
	if (got != (ssize_t)sizeof(salt))
		return -EIO;

	rc = auth_hash(next, AUTH_ITERATIONS, salt, hash, sizeof(hash));
	if (!rc)
		rc = store_hash(hash);
	explicit_bzero(hash, sizeof(hash));
	if (rc)
		return rc;

	/* The other sessions were opened with the old password. */
	i = slot_of(keep, now);
	for (int j = 0; j < AUTH_SESSIONS; j++)
		if (j != i)
			memset(&g_sess[j], 0, sizeof(g_sess[j]));
	return 0;
}
