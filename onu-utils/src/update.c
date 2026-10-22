// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The update file of update.h, and the images it carries: the format alone. */

#include <errno.h>
#include <string.h>

#include <zlib.h>

#include "monocypher-ed25519.h"
#include "update.h"

/* The header fields, as byte offsets. */
#define H_MAGIC 0u
#define H_FORMAT 4u
#define H_LENGTH 8u
#define H_KERNEL 12u
#define H_ROOTFS 16u
#define H_FLAGS 20u
#define H_TIME 24u
#define H_BOARD 32u
#define H_VERSION 96u
#define H_RESERVED 160u
#define H_CRC 252u

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static uint32_t get32le(const uint8_t *p)
{
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[1] << 8 | p[0];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t crc(const uint8_t *p, size_t len)
{
	return (uint32_t)crc32(crc32(0L, Z_NULL, 0), p, (unsigned int)len);
}

/*
 * Copy the text field at @p into @out: printable bytes, NUL-padded, and not
 * empty.
 */
static int text(const uint8_t *p, char out[UPD_TEXT])
{
	size_t len = strnlen((const char *)p, UPD_TEXT), i;

	if (!len || len == UPD_TEXT)
		return -EBADMSG;
	for (i = 0; i < UPD_TEXT; i++) {
		if (i < len && (p[i] < 0x20 || p[i] > 0x7e))
			return -EBADMSG;
		if (i >= len && p[i])
			return -EBADMSG;
	}
	memcpy(out, p, len + 1);
	return 0;
}

int upd_parse(const uint8_t raw[UPD_HEADER], struct upd_header *h)
{
	size_t i;

	if (get32(raw + H_MAGIC) != UPD_MAGIC ||
	    get32(raw + H_LENGTH) != UPD_HEADER ||
	    get32(raw + H_CRC) != crc(raw, H_CRC))
		return -EBADMSG;
	if (get32(raw + H_FORMAT) != UPD_FORMAT)
		return -EPROTONOSUPPORT;
	if (get32(raw + H_FLAGS))
		return -EBADMSG;
	for (i = H_RESERVED; i < H_CRC; i++)
		if (raw[i])
			return -EBADMSG;

	memset(h, 0, sizeof(*h));
	h->kernel_len = get32(raw + H_KERNEL);
	h->rootfs_len = get32(raw + H_ROOTFS);
	h->time = (uint64_t)get32(raw + H_TIME) << 32 | get32(raw + H_TIME + 4);
	if (text(raw + H_BOARD, h->board) || text(raw + H_VERSION, h->version))
		return -EBADMSG;
	return 0;
}

void upd_build(uint8_t raw[UPD_HEADER], const struct upd_header *h)
{
	memset(raw, 0, UPD_HEADER);
	put32(raw + H_MAGIC, UPD_MAGIC);
	put32(raw + H_FORMAT, UPD_FORMAT);
	put32(raw + H_LENGTH, UPD_HEADER);
	put32(raw + H_KERNEL, h->kernel_len);
	put32(raw + H_ROOTFS, h->rootfs_len);
	put32(raw + H_TIME, (uint32_t)(h->time >> 32));
	put32(raw + H_TIME + 4, (uint32_t)h->time);
	memcpy(raw + H_BOARD, h->board, strnlen(h->board, UPD_TEXT - 1));
	memcpy(raw + H_VERSION, h->version, strnlen(h->version, UPD_TEXT - 1));
	put32(raw + H_CRC, crc(raw, H_CRC));
}

int uimage_check(const uint8_t raw[UIMAGE_HEADER], uint32_t *len,
		 uint32_t *data_crc, uint32_t *time)
{
	uint8_t copy[UIMAGE_HEADER];
	uint32_t size;

	if (get32(raw) != UIMAGE_MAGIC)
		return -EBADMSG;
	memcpy(copy, raw, UIMAGE_HEADER);
	memset(copy + 4, 0, 4);
	if (crc(copy, UIMAGE_HEADER) != get32(raw + 4))
		return -EBADMSG;
	size = get32(raw + 12);
	if (size > UINT32_MAX - UIMAGE_HEADER)
		return -EBADMSG;
	*len = UIMAGE_HEADER + size;
	*data_crc = get32(raw + 24);
	*time = get32(raw + 8);
	return 0;
}

int squashfs_check(const uint8_t raw[SQUASHFS_SUPER], uint32_t *used)
{
	/* The superblock is little-endian; bytes_used is a 64-bit field at 40. */
	if (get32le(raw) != SQUASHFS_MAGIC || get32le(raw + 44))
		return -EBADMSG;
	*used = get32le(raw + 40);
	return 0;
}

int upd_sig_parse(const uint8_t raw[UPD_SIG], struct upd_sig *s)
{
	size_t i;

	if (get32(raw) != UPD_SIG_MAGIC)
		return -ENOENT;
	if (get32(raw + 4) != UPD_SIG_FORMAT)
		return -EPROTONOSUPPORT;
	for (i = 8 + SHA256_LEN + UPD_SIG_BYTES; i < UPD_SIG; i++)
		if (raw[i])
			return -EBADMSG;
	memcpy(s->key_id, raw + 8, SHA256_LEN);
	memcpy(s->sig, raw + 8 + SHA256_LEN, UPD_SIG_BYTES);
	return 0;
}

int upd_sig_check(const struct upd_sig *s, const uint8_t key[UPD_KEY],
		  const uint8_t digest[SHA256_LEN])
{
	uint8_t msg[sizeof(UPD_SIG_CONTEXT) + SHA256_LEN];

	memcpy(msg, UPD_SIG_CONTEXT, sizeof(UPD_SIG_CONTEXT));
	memcpy(msg + sizeof(UPD_SIG_CONTEXT), digest, SHA256_LEN);
	return crypto_ed25519_check(s->sig, key, msg, sizeof(msg)) ? -EBADMSG : 0;
}
