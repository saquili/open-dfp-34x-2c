/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * An update file: what goes into one bank of the stick's flash, a kernel
 * image and a root filesystem image, with a header in front and a checksum
 * behind.
 *
 *   header    UPD_HEADER bytes, below
 *   kernel    the kernel image, a uImage, as the boot loader loads it
 *   rootfs    the root filesystem, a SquashFS image
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
 * The checksum guards against a damaged file; the file is not signed. The
 * file is read and written as a stream, front to back, so neither end needs
 * to hold it whole.
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
