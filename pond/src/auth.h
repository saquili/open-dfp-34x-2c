/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Signing in to the panel: one user, admin, whose password is kept as a
 * PBKDF2-HMAC-SHA256 hash in the configuration store, and a few sessions,
 * each a random id the browser holds in an HttpOnly cookie.
 *
 * Until a password is set, the password is "admin" and every session must
 * change it before it may do anything else. Five wrong passwords in a row
 * stop sign-ins for half a minute.
 */

#ifndef POND_AUTH_H
#define POND_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define AUTH_USER "admin"
#define AUTH_SESSIONS 3
#define AUTH_IDLE_S 900
#define AUTH_MAX_AGE_S (12 * 3600)
#define AUTH_ID_LEN 32
#define AUTH_SECRET_MIN 8
#define AUTH_SECRET_MAX 128
/* A sign-in with this many takes about 0.55 s on the stick, measured. */
#define AUTH_ITERATIONS 20000
#define AUTH_HASH_MAX 160

/*
 * The file of the hash (/run/onu-auth, which onu-store keeps) and the
 * program that stores a new one (onu-store, given "auth HASH"). The tests
 * name others.
 */
void auth_init(const char *hashfile, const char *store);

/** Whether the default password is the one in force. */
bool auth_default_password(void);

/**
 * auth_sign_in() - Check a user and a password and open a session.
 * @id: the new session's id, AUTH_ID_LEN hex digits and a NUL
 * @retry_after: when refused for too many attempts, the seconds to wait
 *
 * A full table loses its least recently used session.
 *
 * Return: 0, -EACCES for a wrong user or password, -EAGAIN while sign-ins
 * are stopped, -EIO when no random id could be had.
 */
int auth_sign_in(const char *user, const char *secret, time_t now,
		 char id[AUTH_ID_LEN + 1], int *retry_after);

/** Whether @id is an open session; a hit counts as activity. */
bool auth_session(const char *id, time_t now);

void auth_sign_out(const char *id);

/**
 * auth_change() - Replace the password, which @current must match.
 * @keep: the session that asked, which stays open; every other one closes
 *
 * Return: 0, -EACCES for a wrong current password, -EINVAL for a new one
 * that is too short, too long or not printable, -EAGAIN while sign-ins are
 * stopped, or the error of storing it.
 */
int auth_change(const char *current, const char *next, const char *keep,
		time_t now, int *retry_after);

/**
 * auth_hash() - The stored form of a password:
 * "pbkdf2-sha256$ITERATIONS$SALT$HASH", salt and hash in hex.
 */
int auth_hash(const char *secret, unsigned int iterations,
	      const uint8_t salt[16], char *out, size_t cap);

/** Whether @secret matches @stored, compared in constant time. */
bool auth_verify(const char *secret, const char *stored);

/** PBKDF2-HMAC-SHA256 (RFC 8018), one 32-byte block. */
void auth_pbkdf2(const uint8_t *secret, size_t secret_len,
		 const uint8_t *salt, size_t salt_len, unsigned int iterations,
		 uint8_t out[32]);

#endif
