/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Big-endian loads and stores. Every field of an OMCI frame that is longer
 * than a byte is in network order.
 */

#ifndef OMCI_BYTEORDER_H
#define OMCI_BYTEORDER_H

static inline unsigned int omci_be32(const volatile unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static inline unsigned int omci_be16(const volatile unsigned char *p)
{
	return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static inline void omci_be32_store(volatile unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static inline void omci_be16_store(volatile unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

#endif
