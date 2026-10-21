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

/*
 * An own certificate, which the daemon serves on every address in place of
 * those it makes: one that an authority signed, with the CA certificates
 * that lead to the authority's own, which the browser trusts. Mongoose's TLS
 * signs with a P-256 key alone, so the certificate's key is one; the CA
 * certificates may be of any kind, as only the browser checks them. The
 * chain is at most CERT_CHAIN_MAX bytes of DER, CERT_CHAIN_COUNT
 * certificates, the server's first, and the configuration store keeps it
 * (onu-config tls own).
 */
#define CERT_CHAIN_MAX 8192
#define CERT_CHAIN_COUNT 8
#define CERT_NAME_MAX 128
#define CERT_NAMES_MAX 384
#define CERT_IPS_MAX 8

/**
 * struct cert_own - An own certificate, checked.
 * @chain: the certificates, DER, one after another, the server's first
 * @len: the bytes of @chain
 * @leaf: the bytes of the first certificate
 * @count: the certificates of @chain
 * @key: the scalar of the first certificate's key
 * @not_before: the start of its validity, in seconds from 1970
 * @not_after: its end
 * @subject: the common name and the organisation of its subject, as words
 * @issuer: the same of its issuer
 * @names: the names of its subjectAltName, "DNS:name" and "IP:address",
 *	apart by a blank
 * @ips: the IPv4 addresses among them
 * @nips: how many
 */
struct cert_own {
	uint8_t chain[CERT_CHAIN_MAX];
	size_t len;
	size_t leaf;
	unsigned int count;
	uint8_t key[CERT_KEY_LEN];
	long long not_before;
	long long not_after;
	char subject[CERT_NAME_MAX];
	char issuer[CERT_NAME_MAX];
	char names[CERT_NAMES_MAX];
	uint8_t ips[CERT_IPS_MAX][4];
	unsigned int nips;
};

/**
 * cert_own_pem() - Check an own certificate as a person gives it.
 * @cert: the certificate and the CA certificates after it, each a PEM block
 *	BEGIN CERTIFICATE, the server's first; other text is passed over
 * @key: its private key, unencrypted, a PEM block BEGIN EC PRIVATE KEY
 *	(SEC1, RFC 5915) or BEGIN PRIVATE KEY (PKCS#8, RFC 5208)
 * @own: what it is, filled in when it is taken
 * @err: why not, in words, @errcap bytes
 *
 * Return: 0, or -EINVAL with why in @err: no certificate, a chain too long,
 * a block that is no certificate, a key of another kind than P-256, an
 * encrypted key, or a key that is not the certificate's.
 */
int cert_own_pem(const char *cert, const char *key, struct cert_own *own, char *err,
		 size_t errcap);

/**
 * cert_own_der() - The same, of a chain in DER, @len bytes, and the scalar
 *	@key, as the store keeps them.
 */
int cert_own_der(const uint8_t *chain, size_t len, const uint8_t key[CERT_KEY_LEN],
		 struct cert_own *own, char *err, size_t errcap);

/**
 * cert_own_load() - The same, of the file at @path, as onu-config writes it:
 *	the key in 64 hexadecimal digits on its first line, the chain in base64
 *	on its second.
 *
 * Return: 0, -ENOENT without the file, or -EINVAL with why in @err.
 */
int cert_own_load(const char *path, struct cert_own *own, char *err, size_t errcap);

/* Whether @own names the IPv4 address @ip among its subjectAltName. */
bool cert_own_names(const struct cert_own *own, const uint8_t ip[4]);

/* The private key of @own as cert_key_der() writes the daemon's. */
void cert_own_key_der(const struct cert_own *own, uint8_t der[CERT_KEY_DER_LEN]);

/* @t, seconds from 1970, as 2026-10-21T12:00:00Z, in @out of 24 bytes. */
void cert_iso(long long t, char out[24]);

#endif
