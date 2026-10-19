/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Signing in to the panel: one user, root, the stick's one account, whose
 * password the console's login and the ssh server take too. onu-config keeps
 * its hash, of crypt()'s form, in the configuration store and in
 * /run/onu-auth, which root alone reads: onu-config checks a password and
 * hashes a new one, which the panel has onu-act run (ADR 0011). A few
 * sessions, each a random id the browser holds in an HttpOnly cookie.
 *
 * Until a password is set, the password is "admin" and every session must
 * change it before it may do anything else. Five wrong passwords in a row
 * stop sign-ins for half a minute. A hash of the form the panel made before,
 * PBKDF2-HMAC-SHA256, is still taken, and the first sign-in with it has
 * onu-config keep the password in crypt()'s form.
 */

#ifndef POND_AUTH_H
#define POND_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define AUTH_USER "root"
#define AUTH_SESSIONS 3
#define AUTH_IDLE_S 900
#define AUTH_MAX_AGE_S (12 * 3600)
#define AUTH_ID_LEN 32
#define AUTH_SECRET_MIN 8
#define AUTH_SECRET_MAX 128
#define AUTH_HASH_MAX 160

/* No session, no failure, and whether the default password is in force, asked. */
void auth_init(void);

/**
 * Whether the default password is the one in force: as the start found it,
 * and each sign-in and change since. A password set at the console counts
 * from the next sign-in.
 */
bool auth_default_password(void);

/**
 * auth_sign_in() - Check a user and a password and open a session.
 * @from: the address the request came from: five wrong passwords from one
 *	address stop sign-ins from it for half a minute, twenty from all of
 *	them stop every sign-in as long
 * @id: the new session's id, AUTH_ID_LEN hex digits and a NUL
 * @retry_after: when refused for too many attempts, the seconds to wait
 *
 * A full table loses its least recently used session. A password kept in
 * the older form is stored again in crypt()'s.
 *
 * Return: 0, -EACCES for a wrong user or password, -ENOKEY while no
 * password is known (no store could be read; the console takes the
 * default one), -EAGAIN while sign-ins are stopped, -EIO when no random id
 * could be had.
 */
int auth_sign_in(const char *user, const char *secret, const char *from,
		 time_t now, char id[AUTH_ID_LEN + 1], int *retry_after);

/*
 * The end of a sign-in or a change of the password not waited for: @ctx as
 * given, the result as auth_sign_in() or auth_change() returns it, and after
 * a sign-in the new session's id, empty otherwise.
 */
typedef void (*auth_done)(void *ctx, int rc, const char *id);

/**
 * auth_sign_in_begin() - auth_sign_in() without waiting for the check.
 *
 * Return: -EINPROGRESS, and @done is told the end; any other return is the
 * end, as auth_sign_in() returns it, and @done is not called. One check
 * runs at a time: while one runs, -EAGAIN, with a second to wait.
 */
int auth_sign_in_begin(const char *user, const char *secret, const char *from,
		       time_t now, int *retry_after, auth_done done, void *ctx);

/**
 * auth_change_begin() - auth_change() without waiting for the check and the
 *	store.
 *
 * Return: as auth_sign_in_begin().
 */
int auth_change_begin(const char *current, const char *next, const char *keep,
		      const char *from, time_t now, int *retry_after,
		      auth_done done, void *ctx);

/** Whether @id is an open session; a hit counts as activity. */
bool auth_session(const char *id, time_t now);

void auth_sign_out(const char *id);

/**
 * auth_confirm() - Check root's password again, for what needs it as well
 *	as a session: an update written, a start of the other bank. A wrong
 *	one counts as a wrong sign-in.
 *
 * Return: 0, -EACCES for a wrong password, -ENOKEY while no password is
 * known, -EAGAIN while sign-ins are stopped.
 */
int auth_confirm(const char *secret, const char *from, time_t now, int *retry_after);

/**
 * auth_change() - Replace the password, which @current must match.
 * @keep: the session that asked, which stays open; every other one closes
 *
 * Return: 0, -EACCES for a wrong current password, -ENOKEY while no
 * password is known, -EINVAL for a new one that is too short, too long or
 * not printable, -EAGAIN while sign-ins are stopped, or the error of
 * storing it.
 */
int auth_change(const char *current, const char *next, const char *keep,
		const char *from, time_t now, int *retry_after);

#endif
