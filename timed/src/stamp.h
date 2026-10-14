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

/*
 * The hardware stamp of SCM_TIMESTAMPING: its data are three stamps in one of
 * the two sizes above, the software stamp, one no longer used and the raw
 * hardware stamp, in the clock of the network device (its PHC). Return the
 * third, or false when the data are not three stamps or the device gave none.
 */
bool stamp_parse_hw(const void *data, size_t len, ns_t *out);

#endif
