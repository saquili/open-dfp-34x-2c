/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * SHA-256 (FIPS 180-4), which the update files carry as their checksum. The
 * stick has no library for it, and the digest of a few megabytes is taken
 * as the bytes stream past, so the state is fed in pieces of any size.
 */

#ifndef ONU_SHA256_H
#define ONU_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32u

struct sha256 {
	uint32_t h[8];
	uint64_t len;		/* the bytes fed so far */
	uint8_t block[64];	/* the bytes of the block not yet hashed */
	size_t fill;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t digest[SHA256_LEN]);

#endif
