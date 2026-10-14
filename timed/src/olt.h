/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The time the OLT gave. An OLT may send the date and time with the OMCI
 * message Synchronize time (G.988 A.3.33), to the second; omcid writes it in
 * a file of KEY=VALUE lines with the monotonic time it came at:
 *
 *	utc=SECONDS		since 1970-01-01, UTC
 *	monotonic_ns=NS		CLOCK_MONOTONIC when the message came
 *
 * so that the time now is the one given plus the time since. It is one
 * reading, not a clock that keeps running: the source of last resort, for a
 * host that gives no time.
 */

#ifndef TIMED_OLT_H
#define TIMED_OLT_H

#include "timed.h"

/* The OLT gives whole seconds: an offset within this is no offset. */
#define OLT_RESOLUTION NS_PER_SEC

/*
 * Read the file's @len bytes of @text into the time the OLT gave and the
 * monotonic time it came at. Return false when either is missing or not a
 * number.
 */
bool olt_parse(const char *text, size_t len, ns_t *utc, ns_t *at);

/*
 * The decimal number of at most @max after "KEY=" at the start of a line of
 * @text, up to the line's end, into @out. False when there is none.
 */
bool olt_number(const char *text, size_t len, const char *key, int64_t max, int64_t *out);

/* The local clock less the OLT's, at local time @real and monotonic time @mono. */
ns_t olt_offset(ns_t utc, ns_t at, ns_t real, ns_t mono);

#endif
