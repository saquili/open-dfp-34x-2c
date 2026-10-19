/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Root's password against its hash (ADR 0009): a hash of crypt()'s form,
 * "$5$...", which the console's login and the ssh server check too, or one
 * of the form the status daemon made before, PBKDF2-HMAC-SHA256,
 * "pbkdf2-sha256$ITERATIONS$SALT$HASH" with a 16-byte salt and the hash in
 * hexadecimal, which a sign-in replaces with crypt()'s. onu-config
 * password-check carries it out as root, so that the hash stays root's.
 */

#ifndef ONU_PASSWORD_H
#define ONU_PASSWORD_H

#include <stddef.h>
#include <stdint.h>

/**
 * enum password_match - What a password is to a hash.
 * @PASSWORD_WRONG: it does not match, or the hash is of no form known
 * @PASSWORD_MATCHES: it matches a hash of crypt()'s form
 * @PASSWORD_OLDER: it matches a hash of the older form
 */
enum password_match {
	PASSWORD_WRONG,
	PASSWORD_MATCHES,
	PASSWORD_OLDER,
};

/* Whether @secret matches @stored, compared in constant time. */
enum password_match password_verify(const char *secret, const char *stored);

/* PBKDF2-HMAC-SHA256 (RFC 8018), the first 32-byte block. */
void password_pbkdf2(const uint8_t *secret, size_t secret_len,
		     const uint8_t *salt, size_t salt_len,
		     unsigned int iterations, uint8_t out[32]);

#endif
