/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Byte copies, fills and comparisons through volatile pointers: the
 * compiler can neither turn them into C library calls nor merge the bytes
 * into word accesses. The frame fields they move are often unaligned, and
 * the processor has no instructions for unaligned word access.
 */

#ifndef OMCI_WIRE_H
#define OMCI_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline void omci_wire_copy(void *dst, const void *src, size_t n)
{
	volatile unsigned char *d = dst;
	const volatile unsigned char *s = src;

	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
}

static inline void omci_wire_fill(void *dst, uint8_t value, size_t n)
{
	volatile unsigned char *d = dst;

	for (size_t i = 0; i < n; i++)
		d[i] = value;
}

static inline bool omci_wire_equal(const void *a, const void *b, size_t n)
{
	const volatile unsigned char *p = a;
	const volatile unsigned char *q = b;

	for (size_t i = 0; i < n; i++)
		if (p[i] != q[i])
			return false;
	return true;
}

#endif
