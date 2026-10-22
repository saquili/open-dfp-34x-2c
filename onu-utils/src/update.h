/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * An update file: what goes into one bank of the stick's flash, a kernel
 * image and a root filesystem image, with a header in front and a checksum
 * behind.
 *
 *   header    UPD_HEADER bytes, below
 *   kernel    the kernel image, a uImage, as the boot loader loads it
 *   rootfs    the root filesystem, a SquashFS image, and in a release its
 *             signature right behind it, below
 *   trailer   the SHA-256 of the header, the kernel and the rootfs
 *
 * The header, every integer big-endian:
 *
 *   0    4   magic, "ONUF"
 *   4    4   format version, 1
 *   8    4   length of the header, UPD_HEADER
 *   12   4   length of the kernel image
 *   16   4   length of the root filesystem image
 *   20   4   flags, 0
 *   24   8   time of the build, in seconds since 1970
 *   32   64  the board, the first entry of the device tree's compatible
 *            property, NUL-padded
 *   96   64  the version of the system, NUL-padded
 *   160  92  zero
 *   252  4   CRC32 of bytes 0 to 251
 *
 * The checksum guards against a damaged file. The file is read and written
 * as a stream, front to back, so neither end needs to hold it whole.
 *
 * A release is signed (docs/adr/0022-releases-are-signed.rst): UPD_SIG bytes
 * follow the SquashFS image, at its bytes_used, inside the root filesystem's
 * part, whose length in the header counts them. An onu-update of before
 * signatures takes such a file as it is, and the bank keeps the signature
 * behind the root filesystem, where the kernel does not look. Its integers
 * are big-endian:
 *
 *   0    4   magic, "ONUS"
 *   4    4   format version, 1
 *   8    32  the key's id: the SHA-256 of its raw Ed25519 public key
 *   40   64  the Ed25519 signature of UPD_SIG_CONTEXT, with its NUL,
 *            followed by the SHA-256 of the header, the kernel image and
 *            the SquashFS image, up to its bytes_used
 *   104  24  zero
 */

#ifndef ONU_UPDATE_H
#define ONU_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#define UPD_MAGIC 0x4f4e5546u
#define UPD_FORMAT 1u
#define UPD_HEADER 256u
#define UPD_TRAILER SHA256_LEN
#define UPD_TEXT 64u

#define UPD_SIG 128u
#define UPD_SIG_MAGIC 0x4f4e5553u
#define UPD_SIG_FORMAT 1u
#define UPD_SIG_CONTEXT "open-dfp-34x-2c update"
#define UPD_KEY 32u
#define UPD_SIG_BYTES 64u

/* The uImage header and its magic number. */
#define UIMAGE_HEADER 64u
#define UIMAGE_MAGIC 0x27051956u

/* The SquashFS superblock, the part of it read here, and its magic number. */
#define SQUASHFS_SUPER 96u
#define SQUASHFS_MAGIC 0x73717368u

struct upd_header {
	uint32_t kernel_len;
	uint32_t rootfs_len;
	uint64_t time;
	char board[UPD_TEXT];	/* NUL-terminated */
	char version[UPD_TEXT];	/* NUL-terminated */
};

/**
 * upd_parse() - Read a header.
 *
 * Return: 0, -EBADMSG when the magic, the length, the CRC or a text does not
 * check out, -EPROTONOSUPPORT for a format version other than UPD_FORMAT.
 */
int upd_parse(const uint8_t raw[UPD_HEADER], struct upd_header *h);

/* Write the header of @h into @raw, with its CRC. */
void upd_build(uint8_t raw[UPD_HEADER], const struct upd_header *h);

struct upd_sig {
	uint8_t key_id[SHA256_LEN];
	uint8_t sig[UPD_SIG_BYTES];
};

/**
 * upd_sig_parse() - Read a signature.
 *
 * Return: 0, -ENOENT when @raw does not start with the magic, so that it is
 * no signature, -EPROTONOSUPPORT for a format version other than
 * UPD_SIG_FORMAT, -EBADMSG for a reserved byte that is not zero.
 */
int upd_sig_parse(const uint8_t raw[UPD_SIG], struct upd_sig *s);

/**
 * upd_sig_check() - Check a signature.
 * @s: the signature
 * @key: the raw Ed25519 public key that @s names
 * @digest: the SHA-256 it signs: of the header, the kernel image and the
 *          SquashFS image
 *
 * Return: 0 when @s is the signature of @digest by @key, -EBADMSG when not.
 */
int upd_sig_check(const struct upd_sig *s, const uint8_t key[UPD_KEY],
		  const uint8_t digest[SHA256_LEN]);

/**
 * uimage_check() - Read the header of a uImage.
 * @raw: its first UIMAGE_HEADER bytes
 * @len: the length of the whole image, header and data, if it checks out
 * @data_crc: the CRC32 its data must have
 * @time: the time it was made
 *
 * Return: 0, or -EBADMSG when the magic or the header's CRC is wrong.
 */
int uimage_check(const uint8_t raw[UIMAGE_HEADER], uint32_t *len,
		 uint32_t *data_crc, uint32_t *time);

/**
 * squashfs_check() - Read the superblock of a SquashFS image.
 * @raw: its first SQUASHFS_SUPER bytes
 * @used: the bytes the image takes
 *
 * Return: 0, or -EBADMSG when the magic is wrong or the image claims more
 * than 4 GiB.
 */
int squashfs_check(const uint8_t raw[SQUASHFS_SUPER], uint32_t *used);

#endif
