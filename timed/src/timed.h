/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * timed: the clock of the stick. It asks the host for the time in the two
 * ways the host may offer it, NTP (RFC 5905, as a client) and PTP (IEEE
 * 1588-2008, as a slave over the management link), and sets the clock from
 * the better of the two that answer; the time the OLT gives with Synchronize
 * time is the third, for a host that gives none.
 *
 * The protocols are plain functions over bytes and a clock in nanoseconds,
 * which the tests drive; main.c holds the sockets, the clock and the files.
 */

#ifndef TIMED_H
#define TIMED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Time and offsets are signed nanoseconds; an offset is the local clock less the reference. */
typedef int64_t ns_t;

#define NS_PER_SEC 1000000000LL
#define NS_PER_MS 1000000LL
#define NS_PER_US 1000LL

/* Big-endian fields of the wire formats. */
static inline uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint64_t get48(const uint8_t *p)
{
	return (uint64_t)get16(p) << 32 | get32(p + 2);
}

static inline uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) << 32 | get32(p + 4);
}

static inline void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)(v >> 16));
	put16(p + 2, (uint16_t)v);
}

static inline void put48(uint8_t *p, uint64_t v)
{
	put16(p, (uint16_t)(v >> 32));
	put32(p + 2, (uint32_t)v);
}

static inline void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)(v >> 32));
	put32(p + 4, (uint32_t)v);
}

#endif
