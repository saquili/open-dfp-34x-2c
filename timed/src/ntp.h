/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The NTP client side (RFC 5905): the 48-byte request, the checks of the
 * answer, the offset and delay it gives, and the filter that keeps out an
 * answer that waited in a queue on its way.
 */

#ifndef TIMED_NTP_H
#define TIMED_NTP_H

#include "timed.h"

#define NTP_PACKET_LEN 48
#define NTP_FILTER_SIZE 8

enum ntp_status {
	NTP_OK = 0,
	NTP_SHORT,	/* shorter than a packet */
	NTP_MODE,	/* not a server's answer, or a version this does not speak */
	NTP_ORIGIN,	/* does not answer the request: another's, or a replay */
	NTP_KOD,	/* a kiss-o'-death: the server asks to be left alone */
	NTP_UNSYNC,	/* the server has no time to give: stratum 0 or 16, an alarm */
	NTP_TIME,	/* a time of zero */
};

/* What one exchange tells. */
struct ntp_sample {
	ns_t offset;		/* the local clock less the server's */
	ns_t delay;		/* the round trip less the server's time in it */
	unsigned int stratum;
};

/* The NTP era is not on the wire: the seconds since 1900 are read as era 0 until 2036, then as era 1. */
ns_t ntp_to_unix_ns(uint32_t sec, uint32_t frac);
void ntp_from_unix_ns(ns_t t, uint32_t *sec, uint32_t *frac);

/* Fill @buf with a client request that carries @t1, the local time of sending. */
void ntp_request(uint8_t buf[NTP_PACKET_LEN], ns_t t1);

/*
 * Judge an answer of @len bytes to the request sent at @t1 and received at
 * @t4. With NTP_OK, @out holds the sample.
 */
enum ntp_status ntp_answer(const uint8_t *buf, size_t len, ns_t t1, ns_t t4,
			   struct ntp_sample *out);

/* The last samples; @at is the local time of each, to age them out and to tell a new best from one used. */
struct ntp_filter {
	struct {
		ns_t offset;
		ns_t delay;
		ns_t at;
		bool used;
	} s[NTP_FILTER_SIZE];
	unsigned int next;
};

void ntp_filter_init(struct ntp_filter *f);
void ntp_filter_add(struct ntp_filter *f, const struct ntp_sample *s, ns_t at);

/* An answer's delay may exceed the least of the last eight by half of it, and at least by this. */
#define NTP_DELAY_MARGIN_MIN (100 * NS_PER_US)

/*
 * The newest sample, when its delay is within the margin of the least delay
 * of the samples not older than @max_age, and it was not handed out before
 * (so that a sample is applied once). An answer that waited longer on its way
 * than the others is one whose two legs took different times: its offset is
 * off by half the difference, and it is kept out. Taking only the sample of
 * the least delay, as an earlier version did, let one fast answer keep every
 * later one out for minutes, while the clock ran on at a rate gone stale.
 */
bool ntp_filter_take(struct ntp_filter *f, ns_t now, ns_t max_age, struct ntp_sample *out, ns_t *at);

const char *ntp_status_name(enum ntp_status st);

#endif
