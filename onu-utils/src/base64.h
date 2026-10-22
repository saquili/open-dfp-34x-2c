/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Base64 of RFC 4648, the standard alphabet: public keys as text. */

#ifndef ONU_BASE64_H
#define ONU_BASE64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * base64_decode() - The bytes of a base64 text.
 * @s: the text, padded to a multiple of four characters
 * @n: its length
 * @out: where the bytes go
 * @cap: the room in @out
 *
 * Return: the number of bytes, or -1 for a text that is not base64 or does
 * not fit.
 */
long base64_decode(const char *s, size_t n, uint8_t *out, size_t cap);

/**
 * base64_encode() - @n bytes of @in as base64 into @out, NUL-terminated,
 * with the padding when @pad: @out needs (n + 2) / 3 * 4 + 1 bytes.
 *
 * Return: the length written, the NUL aside.
 */
size_t base64_encode(const uint8_t *in, size_t n, char *out, bool pad);

#endif
