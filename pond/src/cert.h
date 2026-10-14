/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The panel's certificate (cert.c). The stick keeps one private key, a P-256
 * scalar that onu-config made once and keeps in the configuration store, and
 * the daemon makes from it, for each address it listens on, a self-signed
 * certificate with that address as its subject's alternative name. Nothing
 * else goes in that would change from one start to the next, and the
 * signature is deterministic (RFC 6979), so the same key and address give
 * the same certificate, and the same fingerprint, for as long as it is
 * valid.
 *
 * The stick has no clock of its own: the validity follows a calendar of
 * two-year periods from CERT_EPOCH, each certificate valid for CERT_DAYS
 * days from the start of its period, the 825 days that Apple's systems take
 * at most, so that two periods overlap by three months. A clock that reads
 * before CERT_EPOCH, as one does until timed sets it, counts as the first
 * period.
 */

#ifndef POND_CERT_H
#define POND_CERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CERT_KEY_LEN 32
/* 2026-01-01 00:00:00 UTC, the start of the first period. */
#define CERT_EPOCH 1767225600
#define CERT_PERIOD_DAYS 730
#define CERT_DAYS 825
/* A certificate is about 500 bytes. */
#define CERT_MAX 1024
/* "AB:CD:...", 32 bytes in hexadecimal with a colon between. */
#define CERT_FP_LEN (32 * 3)

/*
 * Take the key of the file at @path: 64 hexadecimal digits, and a line end.
 * Returns 0, or a negative errno: -EINVAL for a key that is not a scalar of
 * the curve.
 */
int cert_load_key(const char *path);

/* Whether a key was taken. */
bool cert_have_key(void);

/* The start and the end of the validity of the period that @now falls in. */
void cert_validity(time_t now, time_t *not_before, time_t *not_after);

/*
 * The certificate for the IPv4 address @ip (four bytes, as on the wire) at
 * the time @now, DER-encoded into @der of @cap bytes. Returns its length,
 * or 0 without a key or room.
 */
size_t cert_make(const uint8_t ip[4], time_t now, uint8_t *der, size_t cap);

/* The SHA-256 of @der, as browsers show it, in @out of CERT_FP_LEN bytes. */
void cert_fingerprint(const uint8_t *der, size_t len, char out[CERT_FP_LEN]);

/*
 * The private key, DER-encoded as the start of an ECPrivateKey (RFC 5915),
 * which is all Mongoose reads, into @der of CERT_KEY_DER_LEN bytes.
 */
#define CERT_KEY_DER_LEN 39
void cert_key_der(uint8_t der[CERT_KEY_DER_LEN]);

#endif
