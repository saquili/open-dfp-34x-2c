// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The NTP client side. RFC 5905 gives the packet (section 7.3) and the offset
 * and delay of an exchange (section 8):
 *
 *	theta = ((T2 - T1) + (T3 - T4)) / 2
 *	delta = (T4 - T1) - (T3 - T2)
 *
 * with T1 the client's time of sending, T2 and T3 the server's of receiving
 * and sending, T4 the client's of receiving. The RFC's theta is the server's
 * time less the client's; here, as in the PTP code, an offset is the local
 * clock less the reference, so it is -theta.
 */

#include <string.h>

#include "ntp.h"

/* Seconds from 1900-01-01 to 1970-01-01. */
#define NTP_UNIX_EPOCH 2208988800LL
#define NTP_ERA 4294967296LL

/* The packet's fields, by byte. */
#define F_FLAGS 0
#define F_STRATUM 1
#define F_ROOT_DELAY 4
#define F_ROOT_DISP 8
#define F_REFID 12
#define F_ORIGIN 24
#define F_RECEIVE 32
#define F_TRANSMIT 40

/* A server farther than this from its source of time (RFC 5905: MAXDIST) is not believed. */
#define ROOT_DISTANCE_MAX (16 * NS_PER_SEC)

ns_t ntp_to_unix_ns(uint32_t sec, uint32_t frac)
{
	int64_t s = sec;

	/* Era 0 runs from 1900 to 2036-02-07; before 1970 the seconds are era 1's. */
	if (s < NTP_UNIX_EPOCH)
		s += NTP_ERA;
	return (s - NTP_UNIX_EPOCH) * NS_PER_SEC + (ns_t)(((uint64_t)frac * 1000000000ULL) >> 32);
}

void ntp_from_unix_ns(ns_t t, uint32_t *sec, uint32_t *frac)
{
	int64_t s = t / NS_PER_SEC;
	int64_t ns = t % NS_PER_SEC;

	if (ns < 0) {
		ns += NS_PER_SEC;
		s -= 1;
	}
	*sec = (uint32_t)(s + NTP_UNIX_EPOCH);
	*frac = (uint32_t)(((uint64_t)ns << 32) / 1000000000ULL);
}

void ntp_request(uint8_t buf[NTP_PACKET_LEN], ns_t t1)
{
	uint32_t sec;
	uint32_t frac;

	memset(buf, 0, NTP_PACKET_LEN);
	buf[F_FLAGS] = 0 << 6 | 4 << 3 | 3;	/* no warning, version 4, client */
	ntp_from_unix_ns(t1, &sec, &frac);
	put32(buf + F_TRANSMIT, sec);
	put32(buf + F_TRANSMIT + 4, frac);
}

/* A 16.16 fixed-point number of seconds, in nanoseconds. */
static ns_t short_ns(const uint8_t *p)
{
	return (ns_t)(((uint64_t)get32(p) * 1000000000ULL) >> 16);
}

enum ntp_status ntp_answer(const uint8_t *buf, size_t len, ns_t t1, ns_t t4,
			   struct ntp_sample *out)
{
	uint32_t osec;
	uint32_t ofrac;
	unsigned int version;
	unsigned int leap;
	ns_t t2;
	ns_t t3;
	ns_t root;

	if (len < NTP_PACKET_LEN)
		return NTP_SHORT;
	version = (buf[F_FLAGS] >> 3) & 7;
	if ((buf[F_FLAGS] & 7) != 4 || version < 3 || version > 4)
		return NTP_MODE;

	/* The answer repeats what the request carried as its transmit time. */
	ntp_from_unix_ns(t1, &osec, &ofrac);
	if (get32(buf + F_ORIGIN) != osec || get32(buf + F_ORIGIN + 4) != ofrac)
		return NTP_ORIGIN;

	if (buf[F_STRATUM] == 0)
		return NTP_KOD;
	leap = buf[F_FLAGS] >> 6;
	if (leap == 3 || buf[F_STRATUM] > 15)
		return NTP_UNSYNC;
	if (!get32(buf + F_TRANSMIT) && !get32(buf + F_TRANSMIT + 4))
		return NTP_TIME;
	if (!get32(buf + F_RECEIVE) && !get32(buf + F_RECEIVE + 4))
		return NTP_TIME;

	root = short_ns(buf + F_ROOT_DELAY) / 2 + short_ns(buf + F_ROOT_DISP);
	if (root > ROOT_DISTANCE_MAX)
		return NTP_UNSYNC;

	t2 = ntp_to_unix_ns(get32(buf + F_RECEIVE), get32(buf + F_RECEIVE + 4));
	t3 = ntp_to_unix_ns(get32(buf + F_TRANSMIT), get32(buf + F_TRANSMIT + 4));

	out->offset = ((t1 - t2) + (t4 - t3)) / 2;
	out->delay = (t4 - t1) - (t3 - t2);
	/* The clocks of the two ends never agree to the nanosecond: a delay below 0 is 0. */
	if (out->delay < 0)
		out->delay = 0;
	out->stratum = buf[F_STRATUM];
	return NTP_OK;
}

void ntp_filter_init(struct ntp_filter *f)
{
	memset(f, 0, sizeof(*f));
}

void ntp_filter_add(struct ntp_filter *f, const struct ntp_sample *s, ns_t at)
{
	f->s[f->next].offset = s->offset;
	f->s[f->next].delay = s->delay;
	f->s[f->next].at = at;
	f->s[f->next].used = false;
	f->next = (f->next + 1) % NTP_FILTER_SIZE;
	/* A slot never written has at 0: ntp_filter_take() passes it over. */
}

bool ntp_filter_take(struct ntp_filter *f, ns_t now, ns_t max_age, struct ntp_sample *out, ns_t *at)
{
	int newest = -1;
	ns_t least = -1;
	ns_t margin;
	unsigned int i;

	for (i = 0; i < NTP_FILTER_SIZE; i++) {
		if (!f->s[i].at || now - f->s[i].at > max_age)
			continue;
		if (least < 0 || f->s[i].delay < least)
			least = f->s[i].delay;
		if (newest < 0 || f->s[i].at > f->s[newest].at)
			newest = (int)i;
	}
	if (newest < 0 || f->s[newest].used)
		return false;
	margin = least / 2 > NTP_DELAY_MARGIN_MIN ? least / 2 : NTP_DELAY_MARGIN_MIN;
	if (f->s[newest].delay > least + margin)
		return false;
	f->s[newest].used = true;
	out->offset = f->s[newest].offset;
	out->delay = f->s[newest].delay;
	out->stratum = 0;
	*at = f->s[newest].at;
	return true;
}

const char *ntp_status_name(enum ntp_status st)
{
	switch (st) {
	case NTP_OK:
		return "ok";
	case NTP_SHORT:
		return "short packet";
	case NTP_MODE:
		return "not a server's answer";
	case NTP_ORIGIN:
		return "does not answer the request";
	case NTP_KOD:
		return "kiss-o'-death";
	case NTP_UNSYNC:
		return "server has no time to give";
	case NTP_TIME:
		return "no time in the packet";
	}
	return "?";
}
