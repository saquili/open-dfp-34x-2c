// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The PTP slave and master. IEEE 1588-2008 gives the header (clause 13.3), the
 * messages (13.5 to 13.8) and the delay request-response mechanism (11.3):
 *
 *	t1 the master's time of sending the Sync, t2 the slave's of receiving it,
 *	t3 the slave's of sending the Delay_Req, t4 the master's of receiving it;
 *	meanPathDelay = ((t2 - t1) + (t4 - t3)) / 2
 *	offsetFromMaster = t2 - t1 - meanPathDelay
 *
 * with the correctionField of each message taken off the time it travelled.
 */

#include <string.h>

#include "ptp.h"

/* Header fields, by byte. */
#define H_TYPE 0
#define H_VERSION 1
#define H_LENGTH 2
#define H_DOMAIN 4
#define H_FLAGS 6
#define H_CORRECTION 8
#define H_SOURCE 20
#define H_SEQ 30
#define H_CONTROL 32
#define H_LOG_INTERVAL 33

/* flagField, byte 0 and byte 1. */
#define FLAG0_TWO_STEP 0x02
#define FLAG1_UTC_VALID 0x04
#define FLAG1_PTP_TIMESCALE 0x08
#define FLAG1_TIME_TRACEABLE 0x10
#define FLAG1_FREQUENCY_TRACEABLE 0x20

/* controlField (table 23), of version 1 still filled in. */
#define CONTROL_SYNC 0
#define CONTROL_DELAY_RESP 3
#define CONTROL_OTHER 5

/* Announce body, by byte of the message. */
#define A_UTC_OFFSET 44
#define A_PRIORITY1 47
#define A_CLASS 48
#define A_ACCURACY 49
#define A_VARIANCE 50
#define A_PRIORITY2 52
#define A_GRANDMASTER 53
#define A_STEPS 61
#define A_TIME_SOURCE 63
#define ANNOUNCE_LEN PTP_ANNOUNCE_LEN

#define TIMESTAMP_LEN 10	/* 48 bits of seconds and 32 of nanoseconds */
#define DELAY_RESP_LEN PTP_DELAY_RESP_LEN

#define ANNOUNCE_TIMEOUT_MIN (8 * NS_PER_SEC)

void ptp_clock_id_from_mac(uint8_t id[8], const uint8_t mac[6])
{
	id[0] = mac[0];
	id[1] = mac[1];
	id[2] = mac[2];
	id[3] = 0xff;
	id[4] = 0xfe;
	id[5] = mac[3];
	id[6] = mac[4];
	id[7] = mac[5];
}

void ptp_slave_init(struct ptp_slave *s, uint8_t domain, const uint8_t clock_id[8])
{
	memset(s, 0, sizeof(*s));
	s->domain = domain;
	memcpy(s->own.clock, clock_id, 8);
	s->own.port = 1;
	s->utc_offset = PTP_UTC_OFFSET_DEFAULT;
}

static void read_id(struct ptp_id *id, const uint8_t *p)
{
	memcpy(id->clock, p, 8);
	id->port = get16(p + 8);
}

static bool same_id(const struct ptp_id *a, const struct ptp_id *b)
{
	return !memcmp(a->clock, b->clock, 8) && a->port == b->port;
}

/* A timestamp field: seconds and nanoseconds. A nanosecond count of 10^9 or more is not one. */
static bool read_timestamp(const uint8_t *p, ns_t *t)
{
	uint64_t sec = get48(p);
	uint32_t nsec = get32(p + 6);

	if (nsec >= NS_PER_SEC || sec > (uint64_t)(INT64_MAX / NS_PER_SEC) - 1)
		return false;
	*t = (ns_t)sec * NS_PER_SEC + nsec;
	return true;
}

/* The correctionField, in nanoseconds: it is a signed count of 2^-16 ns. */
static ns_t read_correction(const uint8_t *p)
{
	return (ns_t)((int64_t)get64(p) >> 16);
}

int ptp_dataset_cmp(const struct ptp_dataset *a, const struct ptp_dataset *b)
{
	if (a->priority1 != b->priority1)
		return a->priority1 < b->priority1 ? -1 : 1;
	if (a->clock_class != b->clock_class)
		return a->clock_class < b->clock_class ? -1 : 1;
	if (a->accuracy != b->accuracy)
		return a->accuracy < b->accuracy ? -1 : 1;
	if (a->variance != b->variance)
		return a->variance < b->variance ? -1 : 1;
	if (a->priority2 != b->priority2)
		return a->priority2 < b->priority2 ? -1 : 1;
	if (a->steps_removed != b->steps_removed)
		return a->steps_removed < b->steps_removed ? -1 : 1;
	return memcmp(a->grandmaster, b->grandmaster, 8);
}

bool ptp_master_alive(const struct ptp_slave *s, ns_t mono)
{
	ns_t interval = NS_PER_SEC;
	ns_t limit;

	if (!s->have_master)
		return false;
	/* The announce interval is 2^n seconds (n is the log of it in the header). */
	if (s->announce_log_interval > 0 && s->announce_log_interval < 8)
		interval <<= s->announce_log_interval;
	limit = 3 * interval;
	if (limit < ANNOUNCE_TIMEOUT_MIN)
		limit = ANNOUNCE_TIMEOUT_MIN;
	return mono - s->announce_at <= limit;
}

void ptp_master_lost(struct ptp_slave *s)
{
	s->have_master = false;
	s->sync_wait = false;
	s->have_ms = false;
	s->request_wait = false;
	s->delay_known = false;
	s->delay = 0;
}

static bool announce(struct ptp_slave *s, const uint8_t *m, size_t len, ns_t mono,
		     struct ptp_result *r)
{
	struct ptp_dataset ds;
	struct ptp_id from;
	bool take;

	if (len < ANNOUNCE_LEN)
		return false;
	read_id(&from, m + H_SOURCE);
	ds.priority1 = m[A_PRIORITY1];
	ds.clock_class = m[A_CLASS];
	ds.accuracy = m[A_ACCURACY];
	ds.variance = get16(m + A_VARIANCE);
	ds.priority2 = m[A_PRIORITY2];
	memcpy(ds.grandmaster, m + A_GRANDMASTER, 8);
	ds.steps_removed = get16(m + A_STEPS);
	/* Class 255 is a clock that is never a master (clause 7.6.2.4): slave-only. */
	if (ds.clock_class == 255)
		return false;

	/* The first master heard, a better one, or the one in place gone quiet. */
	take = !s->have_master || !ptp_master_alive(s, mono) ||
	       (!same_id(&from, &s->master) && ptp_dataset_cmp(&ds, &s->master_ds) < 0);
	if (!take && !same_id(&from, &s->master))
		return false;

	if (take) {
		ptp_master_lost(s);
		s->have_master = true;
		s->master = from;
		r->ev = PTP_EV_MASTER;
	}
	s->master_ds = ds;
	s->announce_at = mono;
	s->announce_log_interval = (int8_t)m[H_LOG_INTERVAL];
	s->ptp_timescale = !!(m[H_FLAGS + 1] & FLAG1_PTP_TIMESCALE);
	if (m[H_FLAGS + 1] & FLAG1_UTC_VALID)
		s->utc_offset = (int16_t)get16(m + A_UTC_OFFSET);
	return take;
}

ns_t ptp_utc_shift(const struct ptp_slave *s)
{
	return s->ptp_timescale ? (ns_t)s->utc_offset * NS_PER_SEC : 0;
}

/* The time to take off a master's time to get the local clock's timescale. */
static ns_t timescale_shift(const struct ptp_slave *s)
{
	return s->local_timescale ? 0 : ptp_utc_shift(s);
}

void ptp_set_local_timescale(struct ptp_slave *s, bool master)
{
	if (s->local_timescale == master)
		return;
	s->local_timescale = master;
	ptp_clock_stepped(s);
}

/* A Sync is complete with its t1: the offset, and the master-to-slave leg of the delay. */
static bool sync_complete(struct ptp_slave *s, ns_t t1, ns_t corr, struct ptp_result *r)
{
	ns_t ms;

	ms = s->sync_t2 - (t1 - timescale_shift(s)) - corr;
	s->sync_ms = ms;
	s->have_ms = true;
	r->ev = PTP_EV_OFFSET;
	r->offset = ms - s->delay;
	r->delay = s->delay;
	r->delay_known = s->delay_known;
	return true;
}

bool ptp_rx(struct ptp_slave *s, const uint8_t *m, size_t len, ns_t rx, ns_t mono,
	    struct ptp_result *r)
{
	struct ptp_id from;
	unsigned int type;
	ns_t t;

	r->ev = PTP_EV_NONE;
	if (len < PTP_HEADER_LEN + TIMESTAMP_LEN)
		return false;
	if ((m[H_VERSION] & 0x0f) != 2 || m[H_DOMAIN] != s->domain)
		return false;
	if (get16(m + H_LENGTH) > len)
		return false;
	type = m[H_TYPE] & 0x0f;
	read_id(&from, m + H_SOURCE);

	if (type == PTP_ANNOUNCE)
		return announce(s, m, len, mono, r);

	/* All the rest is the master's, or somebody else's. */
	if (!s->have_master || !same_id(&from, &s->master))
		return false;

	switch (type) {
	case PTP_SYNC:
		if (m[H_LOG_INTERVAL] != 0x7f)
			s->sync_log_interval = (int8_t)m[H_LOG_INTERVAL];
		s->sync_t2 = rx;
		s->sync_seq = get16(m + H_SEQ);
		s->sync_corr = read_correction(m + H_CORRECTION);
		if (m[H_FLAGS] & FLAG0_TWO_STEP) {
			s->sync_wait = true;
			return false;
		}
		s->sync_wait = false;
		if (!read_timestamp(m + PTP_HEADER_LEN, &t))
			return false;
		return sync_complete(s, t, s->sync_corr, r);
	case PTP_FOLLOW_UP:
		if (!s->sync_wait || get16(m + H_SEQ) != s->sync_seq)
			return false;
		s->sync_wait = false;
		if (!read_timestamp(m + PTP_HEADER_LEN, &t))
			return false;
		return sync_complete(s, t, s->sync_corr + read_correction(m + H_CORRECTION), r);
	case PTP_DELAY_RESP: {
		struct ptp_id req;
		ns_t t4;
		ns_t sm;

		if (len < DELAY_RESP_LEN || !s->request_wait)
			return false;
		/* Its Delay_Req's time of sending is not known: there is no delay to measure. */
		if (!s->request_t3_known) {
			s->request_wait = false;
			return false;
		}
		read_id(&req, m + PTP_HEADER_LEN + TIMESTAMP_LEN);
		if (!same_id(&req, &s->own) || get16(m + H_SEQ) != s->request_sent_seq)
			return false;
		if (!read_timestamp(m + PTP_HEADER_LEN, &t4))
			return false;
		s->request_wait = false;
		if (!s->have_ms)
			return false;
		/* t4 is in the master's timescale, as t1 is: the same shift puts it in UTC. */
		sm = (t4 - timescale_shift(s)) - read_correction(m + H_CORRECTION) - s->request_t3;
		s->delay = (s->sync_ms + sm) / 2;
		/* A negative delay is a path of zero length and clocks that do not agree. */
		if (s->delay < 0)
			s->delay = 0;
		s->delay_known = true;
		return false;
	}
	default:
		return false;
	}
}

void ptp_clock_stepped(struct ptp_slave *s)
{
	/* The stamps taken before and after a step are of two clocks: none of those in flight can be used. */
	s->sync_wait = false;
	s->have_ms = false;
	s->request_wait = false;
}

static size_t delay_req_build(struct ptp_slave *s, uint8_t *buf)
{
	if (!s->have_master)
		return 0;
	memset(buf, 0, PTP_DELAY_REQ_LEN);
	buf[H_TYPE] = PTP_DELAY_REQ;
	buf[H_VERSION] = 2;
	put16(buf + H_LENGTH, PTP_DELAY_REQ_LEN);
	buf[H_DOMAIN] = s->domain;
	memcpy(buf + H_SOURCE, s->own.clock, 8);
	put16(buf + H_SOURCE + 8, s->own.port);
	s->request_seq++;
	put16(buf + H_SEQ, s->request_seq);
	buf[H_CONTROL] = 1;
	buf[H_LOG_INTERVAL] = 0x7f;
	s->request_sent_seq = s->request_seq;
	s->request_wait = true;
	return PTP_DELAY_REQ_LEN;
}

size_t ptp_delay_req(struct ptp_slave *s, uint8_t *buf, ns_t t3)
{
	size_t len = delay_req_build(s, buf);

	if (len) {
		s->request_t3 = t3;
		s->request_t3_known = true;
	}
	return len;
}

size_t ptp_delay_req_later(struct ptp_slave *s, uint8_t *buf)
{
	size_t len = delay_req_build(s, buf);

	if (len)
		s->request_t3_known = false;
	return len;
}

void ptp_delay_req_sent(struct ptp_slave *s, ns_t t3)
{
	if (!s->request_wait)
		return;
	s->request_t3 = t3;
	s->request_t3_known = true;
}

/* ---- the master ---- */

void ptp_master_init(struct ptp_master *m, uint8_t domain, const uint8_t clock_id[8])
{
	memset(m, 0, sizeof(*m));
	m->domain = domain;
	memcpy(m->own.clock, clock_id, 8);
	m->own.port = 1;
	m->ds.priority1 = PTP_PRIORITY_DEFAULT;
	m->ds.clock_class = PTP_CLASS_DEFAULT;
	m->ds.accuracy = PTP_ACCURACY_UNKNOWN;
	m->ds.variance = 0xffff;
	m->ds.priority2 = PTP_PRIORITY_DEFAULT;
	memcpy(m->ds.grandmaster, clock_id, 8);
	m->time_source = PTP_TIME_SOURCE_INTERNAL;
	m->utc_offset = PTP_UTC_OFFSET_DEFAULT;
}

/* The header every message the master sends begins with. */
static void master_header(const struct ptp_master *m, uint8_t *buf, unsigned int type, size_t len,
			  uint16_t seq, uint8_t control)
{
	memset(buf, 0, len);
	buf[H_TYPE] = (uint8_t)type;
	buf[H_VERSION] = 2;
	put16(buf + H_LENGTH, (uint16_t)len);
	buf[H_DOMAIN] = m->domain;
	memcpy(buf + H_SOURCE, m->own.clock, 8);
	put16(buf + H_SOURCE + 8, m->own.port);
	put16(buf + H_SEQ, seq);
	buf[H_CONTROL] = control;
	buf[H_LOG_INTERVAL] = 0;	/* once a second, 2^0 s */
}

static void put_timestamp(uint8_t *p, ns_t t)
{
	if (t < 0)
		t = 0;
	put48(p, (uint64_t)(t / NS_PER_SEC));
	put32(p + 6, (uint32_t)(t % NS_PER_SEC));
}

size_t ptp_announce_build(struct ptp_master *m, uint8_t *buf, ns_t now)
{
	master_header(m, buf, PTP_ANNOUNCE, PTP_ANNOUNCE_LEN, m->announce_seq++, CONTROL_OTHER);
	buf[H_FLAGS + 1] = FLAG1_PTP_TIMESCALE | FLAG1_UTC_VALID |
			   (m->traceable ? FLAG1_TIME_TRACEABLE | FLAG1_FREQUENCY_TRACEABLE : 0);
	put_timestamp(buf + PTP_HEADER_LEN, now);
	put16(buf + A_UTC_OFFSET, (uint16_t)m->utc_offset);
	buf[A_PRIORITY1] = m->ds.priority1;
	buf[A_CLASS] = m->ds.clock_class;
	buf[A_ACCURACY] = m->ds.accuracy;
	put16(buf + A_VARIANCE, m->ds.variance);
	buf[A_PRIORITY2] = m->ds.priority2;
	memcpy(buf + A_GRANDMASTER, m->ds.grandmaster, 8);
	put16(buf + A_STEPS, m->ds.steps_removed);
	buf[A_TIME_SOURCE] = m->time_source;
	return PTP_ANNOUNCE_LEN;
}

size_t ptp_sync_build(struct ptp_master *m, uint8_t *buf)
{
	/* One-step: no twoStepFlag, and the interface writes the time it leaves into originTimestamp. */
	master_header(m, buf, PTP_SYNC, PTP_SYNC_LEN, m->sync_seq++, CONTROL_SYNC);
	buf[H_FLAGS + 1] = FLAG1_PTP_TIMESCALE;
	return PTP_SYNC_LEN;
}

size_t ptp_delay_resp_build(const struct ptp_master *m, const uint8_t *req, size_t len, ns_t t4,
			    uint8_t *buf)
{
	if (len < PTP_DELAY_REQ_LEN || (req[H_VERSION] & 0x0f) != 2 || (req[H_TYPE] & 0x0f) != PTP_DELAY_REQ ||
	    req[H_DOMAIN] != m->domain || get16(req + H_LENGTH) > len)
		return 0;
	master_header(m, buf, PTP_DELAY_RESP, PTP_DELAY_RESP_LEN, get16(req + H_SEQ), CONTROL_DELAY_RESP);
	buf[H_FLAGS + 1] = FLAG1_PTP_TIMESCALE;
	/* The request's correction comes back with it (clause 11.3.2). */
	memcpy(buf + H_CORRECTION, req + H_CORRECTION, 8);
	put_timestamp(buf + PTP_HEADER_LEN, t4);
	memcpy(buf + PTP_HEADER_LEN + TIMESTAMP_LEN, req + H_SOURCE, 10);
	return PTP_DELAY_RESP_LEN;
}
