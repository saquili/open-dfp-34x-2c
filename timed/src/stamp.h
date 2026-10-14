/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The receive stamp the kernel puts with a frame (SCM_TIMESTAMPNS). Its size
 * is that of the kernel's own timespec, which a 32-bit kernel gives in two
 * forms: two 32-bit words (the old one) or two 64-bit words (the new one).
 * It is read by the length of what came, not by the libc's struct timespec,
 * which is another size on some systems (a 64-bit time_t over a 32-bit
 * tv_nsec): read as that, the nanoseconds of this board's stamps came out 0.
 */

#ifndef TIMED_STAMP_H
#define TIMED_STAMP_H

#include "timed.h"

/* The stamp in nanoseconds from @len bytes at @data, or false when they are not one. */
bool stamp_parse(const void *data, size_t len, ns_t *out);

#endif
