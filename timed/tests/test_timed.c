// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The protocols of timed, driven by bytes and a clock in nanoseconds. */

#include <stdio.h>
#include <string.h>

#include "ntp.h"
#include "olt.h"
#include "phc.h"
#include "ptp.h"
#include "select.h"
#include "servo.h"
#include "stamp.h"

static int failures;
static int checks;

#define CHECK(cond)							\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			failures++;					\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}							\
	} while (0)

#define CHECK_EQ(a, b)							\
	do {								\
		long long va = (long long)(a), vb = (long long)(b);	\
		checks++;						\
		if (va != vb) {						\
			failures++;					\
			printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
		}							\
	} while (0)

#define SEC(n) ((ns_t)(n) * NS_PER_SEC)

/* 2026-10-13 12:00:00 UTC, in seconds since 1970. */
#define T0 1791892800LL

/* ---- NTP ---- */

/* An answer to a request sent at @t1, with the server's receive and transmit times. */
static void ntp_reply(uint8_t buf[NTP_PACKET_LEN], ns_t t1, ns_t t2, ns_t t3, unsigned int stratum)
{
	uint32_t sec, frac;

	memset(buf, 0, NTP_PACKET_LEN);
	buf[0] = 0 << 6 | 4 << 3 | 4;	/* version 4, server */
	buf[1] = (uint8_t)stratum;
	ntp_from_unix_ns(t1, &sec, &frac);
	put32(buf + 24, sec);
	put32(buf + 28, frac);
	ntp_from_unix_ns(t2, &sec, &frac);
	put32(buf + 32, sec);
	put32(buf + 36, frac);
	ntp_from_unix_ns(t3, &sec, &frac);
	put32(buf + 40, sec);
	put32(buf + 44, frac);
}

static void test_ntp_time_conversion(void)
{
	uint32_t sec, frac;
	ns_t t = SEC(T0) + 123456789;

	ntp_from_unix_ns(t, &sec, &frac);
	CHECK_EQ(sec, T0 + 2208988800LL);
	/* The fraction is 2^-32 of a second: back within a nanosecond. */
	CHECK(ntp_to_unix_ns(sec, frac) - t >= -1 && ntp_to_unix_ns(sec, frac) - t <= 1);
	/* 1970 is 2208988800 seconds into the era. */
	CHECK_EQ(ntp_to_unix_ns(2208988800u, 0), 0);
	/* Before 1970 the seconds are those of the next era: 2036-02-07 06:28:16 is 0. */
	CHECK_EQ(ntp_to_unix_ns(0, 0), (4294967296LL - 2208988800LL) * NS_PER_SEC);
	CHECK_EQ(ntp_to_unix_ns(1000, 0), (4294967296LL - 2208988800LL + 1000) * NS_PER_SEC);
}

static void test_ntp_request_and_answer(void)
{
	uint8_t req[NTP_PACKET_LEN], rep[NTP_PACKET_LEN];
	struct ntp_sample s;
	ns_t t1 = SEC(T0) + 250000000;

	ntp_request(req, t1);
	CHECK_EQ(req[0], 0x23);		/* version 4, mode 3 */
	CHECK_EQ(get32(req + 40), T0 + 2208988800LL);

	/* The server is 1 s ahead; the path is 2 ms each way and the server takes 1 ms. */
	ntp_reply(rep, t1, t1 + SEC(1) + 2 * NS_PER_MS, t1 + SEC(1) + 3 * NS_PER_MS, 2);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t1 + 5 * NS_PER_MS, &s), NTP_OK);
	/* offset = local - server: the local clock is 1 s behind. */
	CHECK(s.offset + SEC(1) >= -2 && s.offset + SEC(1) <= 2);
	CHECK(s.delay - 4 * NS_PER_MS >= -2 && s.delay - 4 * NS_PER_MS <= 2);
	CHECK_EQ(s.stratum, 2);

	/* A local clock that is ahead gives the other sign. */
	ntp_reply(rep, t1, t1 - NS_PER_SEC / 2 + NS_PER_MS, t1 - NS_PER_SEC / 2 + 2 * NS_PER_MS, 1);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t1 + 3 * NS_PER_MS, &s), NTP_OK);
	CHECK(s.offset - NS_PER_SEC / 2 >= -2 && s.offset - NS_PER_SEC / 2 <= 2);
}

static void test_ntp_refuses_bad_answers(void)
{
	uint8_t rep[NTP_PACKET_LEN];
	struct ntp_sample s;
	ns_t t1 = SEC(T0), t2 = t1 + NS_PER_MS, t3 = t1 + 2 * NS_PER_MS, t4 = t1 + 3 * NS_PER_MS;

	ntp_reply(rep, t1, t2, t3, 2);
	CHECK_EQ(ntp_answer(rep, NTP_PACKET_LEN - 1, t1, t4, &s), NTP_SHORT);

	/* Not an answer to this request: another transmit time. */
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1 + 1, t4, &s), NTP_ORIGIN);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1 + SEC(1), t4, &s), NTP_ORIGIN);

	/* A client's packet, a symmetric peer's, version 2. */
	rep[0] = 0 << 6 | 4 << 3 | 3;
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_MODE);
	ntp_reply(rep, t1, t2, t3, 2);
	rep[0] = 0 << 6 | 2 << 3 | 4;
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_MODE);

	/* Stratum 0 is a kiss-o'-death, 16 and an alarm are no time. */
	ntp_reply(rep, t1, t2, t3, 0);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_KOD);
	ntp_reply(rep, t1, t2, t3, 16);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_UNSYNC);
	ntp_reply(rep, t1, t2, t3, 2);
	rep[0] |= 3 << 6;
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_UNSYNC);

	/* A server far from its source: root delay 40 s. */
	ntp_reply(rep, t1, t2, t3, 2);
	put32(rep + 4, 40u << 16);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_UNSYNC);

	/* A zero transmit or receive time. */
	ntp_reply(rep, t1, t2, t3, 2);
	memset(rep + 40, 0, 8);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_TIME);
	ntp_reply(rep, t1, t2, t3, 2);
	memset(rep + 32, 0, 8);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t4, &s), NTP_TIME);
}

static void test_ntp_delay_never_negative(void)
{
	uint8_t rep[NTP_PACKET_LEN];
	struct ntp_sample s;
	ns_t t1 = SEC(T0);

	/* The server takes longer than the round trip says: clocks that disagree. */
	ntp_reply(rep, t1, t1 + NS_PER_MS, t1 + 3 * NS_PER_MS, 2);
	CHECK_EQ(ntp_answer(rep, sizeof(rep), t1, t1 + NS_PER_MS, &s), NTP_OK);
	CHECK_EQ(s.delay, 0);
}

static void test_ntp_filter(void)
{
	struct ntp_filter f;
	struct ntp_sample s, out;
	ns_t at;
	ns_t now = SEC(1000);
	int i;

	ntp_filter_init(&f);
	CHECK(!ntp_filter_take(&f, now, SEC(600), &out, &at));

	/* Each new answer is taken, once, while its delay is near the least. */
	s.stratum = 2;
	s.offset = 5000;
	s.delay = 900 * NS_PER_US;
	ntp_filter_add(&f, &s, now - SEC(40));
	CHECK(ntp_filter_take(&f, now, SEC(600), &out, &at));
	CHECK_EQ(out.offset, 5000);
	CHECK_EQ(at, now - SEC(40));
	CHECK(!ntp_filter_take(&f, now, SEC(600), &out, &at));
	s.offset = 100;
	s.delay = 300 * NS_PER_US;
	ntp_filter_add(&f, &s, now - SEC(30));
	CHECK(ntp_filter_take(&f, now, SEC(600), &out, &at));
	CHECK_EQ(out.offset, 100);

	/* 700 us against a least of 300: beyond 300 + 150, kept out. */
	s.offset = 9000;
	s.delay = 700 * NS_PER_US;
	ntp_filter_add(&f, &s, now - SEC(20));
	CHECK(!ntp_filter_take(&f, now, SEC(600), &out, &at));
	/* An older one is never handed out in its place. */
	CHECK(!ntp_filter_take(&f, now, SEC(600), &out, &at));

	/* 440 us is within the margin. */
	s.offset = 7;
	s.delay = 440 * NS_PER_US;
	ntp_filter_add(&f, &s, now - SEC(10));
	CHECK(ntp_filter_take(&f, now, SEC(600), &out, &at));
	CHECK_EQ(out.offset, 7);

	/* When they have all grown old, there is none. */
	CHECK(!ntp_filter_take(&f, now + SEC(3600), SEC(600), &out, &at));

	/* A least delay of 50 us still leaves a margin of 100 us. */
	ntp_filter_init(&f);
	s.delay = 50 * NS_PER_US;
	ntp_filter_add(&f, &s, now - SEC(3));
	s.offset = 11;
	s.delay = 149 * NS_PER_US;
	ntp_filter_add(&f, &s, now - SEC(2));
	CHECK(ntp_filter_take(&f, now, SEC(600), &out, &at));
	CHECK_EQ(out.offset, 11);
	s.delay = 151 * NS_PER_US;
	ntp_filter_add(&f, &s, now - SEC(1));
	CHECK(!ntp_filter_take(&f, now, SEC(600), &out, &at));

	/* The eight slots are a ring: the ninth sample takes the first one's, and its 1 us goes with it. */
	ntp_filter_init(&f);
	for (i = 0; i < 9; i++) {
		s.offset = i;
		s.delay = (i == 0 ? 1 : 1000 + i) * NS_PER_US;
		ntp_filter_add(&f, &s, now - SEC(100 - i));
	}
	CHECK(ntp_filter_take(&f, now, SEC(600), &out, &at));
	CHECK_EQ(out.offset, 8);
}

/* ---- PTP ---- */

static const uint8_t MASTER[8] = { 0x00, 0x1b, 0x19, 0xff, 0xfe, 0x00, 0x00, 0x01 };
static const uint8_t SLAVE[8] = { 0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x02 };

/* The header of a message of @type from the master, in domain 0. */
static void ptp_header(uint8_t *m, unsigned int type, size_t len, uint16_t seq, uint8_t flag0,
		       uint8_t flag1, int64_t correction_ns)
{
	memset(m, 0, len);
	m[0] = (uint8_t)type;
	m[1] = 2;
	put16(m + 2, (uint16_t)len);
	m[6] = flag0;
	m[7] = flag1;
	put64(m + 8, (uint64_t)(correction_ns * 65536));
	memcpy(m + 20, MASTER, 8);
	put16(m + 28, 1);
	put16(m + 30, seq);
	m[33] = 0x7f;
}

static void ptp_stamp(uint8_t *p, ns_t t)
{
	put48(p, (uint64_t)(t / NS_PER_SEC));
	put32(p + 6, (uint32_t)(t % NS_PER_SEC));
}

static void make_announce(uint8_t *m, unsigned int prio1, unsigned int cls, bool utc_valid, int utc_offset)
{
	ptp_header(m, PTP_ANNOUNCE, 64, 7, 0, 0x08 | (utc_valid ? 0x04 : 0), 0);
	m[33] = 0;	/* announce interval 1 s */
	put16(m + 44, (uint16_t)utc_offset);
	m[47] = (uint8_t)prio1;
	m[48] = (uint8_t)cls;
	m[49] = 0x21;
	put16(m + 50, 0x4e5d);
	m[52] = 128;
	memcpy(m + 53, MASTER, 8);
}

static void make_sync(uint8_t *m, uint16_t seq, bool two_step, ns_t t1, int64_t corr)
{
	ptp_header(m, PTP_SYNC, 44, seq, two_step ? 0x02 : 0, 0x08, corr);
	m[33] = 0;
	ptp_stamp(m + 34, two_step ? 0 : t1);
}

static void make_follow_up(uint8_t *m, uint16_t seq, ns_t t1, int64_t corr)
{
	ptp_header(m, PTP_FOLLOW_UP, 44, seq, 0, 0x08, corr);
	m[33] = 0;
	ptp_stamp(m + 34, t1);
}

static void make_delay_resp(uint8_t *m, uint16_t seq, ns_t t4, const uint8_t *req_clock, int64_t corr)
{
	ptp_header(m, PTP_DELAY_RESP, 54, seq, 0, 0x08, corr);
	ptp_stamp(m + 34, t4);
	memcpy(m + 44, req_clock, 8);
	put16(m + 52, 1);
}

static void test_ptp_master_and_one_step(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64];
	/* The master's clock is TAI: 37 s ahead of UTC. The slave is 100 us ahead of UTC, the path 20 us. */
	ns_t utc = SEC(T0);
	ns_t t1_tai = utc + SEC(37);
	ns_t t2 = utc + 20 * NS_PER_US + 100 * NS_PER_US;

	ptp_slave_init(&s, 0, SLAVE);
	/* Nothing from a master that has not announced itself. */
	make_sync(m, 1, false, t1_tai, 0);
	CHECK(!ptp_rx(&s, m, 44, t2, SEC(10), &r));

	make_announce(m, 128, 6, true, 37);
	CHECK(ptp_rx(&s, m, 64, 0, SEC(10), &r));
	CHECK_EQ(r.ev, PTP_EV_MASTER);
	CHECK(s.have_master);

	make_sync(m, 2, false, t1_tai, 0);
	CHECK(ptp_rx(&s, m, 44, t2, SEC(11), &r));
	CHECK_EQ(r.ev, PTP_EV_OFFSET);
	/* Without a measured delay the offset carries the path: 120 us. */
	CHECK_EQ(r.offset, 120 * NS_PER_US);
	CHECK(!r.delay_known);

	/* The correctionField (here 5 us of transparent-clock residence) is taken off. */
	make_sync(m, 3, false, t1_tai, 5 * NS_PER_US);
	CHECK(ptp_rx(&s, m, 44, t2, SEC(12), &r));
	CHECK_EQ(r.offset, 115 * NS_PER_US);
}

static void test_ptp_two_step_and_delay(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64], req[PTP_DELAY_REQ_LEN];
	ns_t utc = SEC(T0);
	ns_t offset = 100 * NS_PER_US, delay = 20 * NS_PER_US;
	ns_t t1, t2, t3, t4;
	uint16_t seq;

	ptp_slave_init(&s, 0, SLAVE);
	make_announce(m, 128, 6, true, 37);
	ptp_rx(&s, m, 64, 0, SEC(1), &r);

	t1 = utc + SEC(37);
	t2 = utc + offset + delay;
	/* A two-step Sync alone gives nothing; its Follow_Up does. */
	make_sync(m, 10, true, 0, 0);
	CHECK(!ptp_rx(&s, m, 44, t2, SEC(2), &r));
	make_follow_up(m, 11, t1, 0);	/* another sequence id: not this Sync's */
	CHECK(!ptp_rx(&s, m, 44, t2, SEC(2), &r));
	make_follow_up(m, 10, t1, 0);
	CHECK(ptp_rx(&s, m, 44, t2, SEC(2), &r));
	CHECK_EQ(r.offset, offset + delay);

	/* The slave sends a Delay_Req at t3; the master received it at t4 = t3 - offset + delay. */
	t3 = utc + SEC(1) + offset;
	CHECK_EQ(ptp_delay_req(&s, req, t3), PTP_DELAY_REQ_LEN);
	CHECK_EQ(req[0], PTP_DELAY_REQ);
	CHECK_EQ(req[1], 2);
	CHECK(!memcmp(req + 20, SLAVE, 8));
	seq = get16(req + 30);
	t4 = (t3 - offset) + delay + SEC(37);

	/* A Delay_Resp for another requester, or another sequence id, is not ours. */
	make_delay_resp(m, seq, t4, MASTER, 0);
	CHECK(!ptp_rx(&s, m, 54, 0, SEC(3), &r));
	CHECK(!s.delay_known);
	make_delay_resp(m, (uint16_t)(seq + 1), t4, SLAVE, 0);
	CHECK(!ptp_rx(&s, m, 54, 0, SEC(3), &r));
	CHECK(!s.delay_known);

	make_delay_resp(m, seq, t4, SLAVE, 0);
	CHECK(!ptp_rx(&s, m, 54, 0, SEC(3), &r));
	CHECK(s.delay_known);
	CHECK_EQ(s.delay, delay);

	/* The next Sync gives the true offset. */
	make_sync(m, 12, true, 0, 0);
	ptp_rx(&s, m, 44, t2, SEC(4), &r);
	make_follow_up(m, 12, t1, 0);
	CHECK(ptp_rx(&s, m, 44, t2, SEC(4), &r));
	CHECK_EQ(r.offset, offset);
	CHECK(r.delay_known);
	CHECK_EQ(r.delay, delay);
}

static void test_ptp_timescale(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64];
	ns_t utc = SEC(T0);

	/* A master on the PTP timescale that does not say the offset is valid: 37 s is assumed. */
	ptp_slave_init(&s, 0, SLAVE);
	make_announce(m, 128, 6, false, 0);
	ptp_rx(&s, m, 64, 0, SEC(1), &r);
	make_sync(m, 1, false, utc + SEC(37), 0);
	CHECK(ptp_rx(&s, m, 44, utc, SEC(2), &r));
	CHECK_EQ(r.offset, 0);

	/* One that says 38 s is believed. */
	make_announce(m, 128, 6, true, 38);
	ptp_rx(&s, m, 64, 0, SEC(3), &r);
	make_sync(m, 2, false, utc + SEC(38), 0);
	CHECK(ptp_rx(&s, m, 44, utc, SEC(4), &r));
	CHECK_EQ(r.offset, 0);

	/* An arbitrary timescale (no ptpTimescale flag) is taken as it is. */
	ptp_slave_init(&s, 0, SLAVE);
	make_announce(m, 128, 6, false, 0);
	m[7] = 0;
	ptp_rx(&s, m, 64, 0, SEC(1), &r);
	make_sync(m, 1, false, utc, 0);
	m[7] = 0;
	CHECK(ptp_rx(&s, m, 44, utc + 5, SEC(2), &r));
	CHECK_EQ(r.offset, 5);
}

/*
 * With a PHC for its clock the slave keeps the master's timescale: no shift
 * to UTC, which the caller applies when it sets the system clock from the PHC.
 */
static void test_ptp_phc_timescale(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64];
	ns_t tai = SEC(T0) + SEC(37);

	ptp_slave_init(&s, 0, SLAVE);
	make_announce(m, 128, 6, true, 37);
	ptp_rx(&s, m, 64, 0, SEC(1), &r);
	CHECK_EQ(ptp_utc_shift(&s), SEC(37));
	ptp_set_local_timescale(&s, true);
	make_sync(m, 1, false, tai, 0);
	CHECK(ptp_rx(&s, m, 44, tai + 250, SEC(2), &r));
	CHECK_EQ(r.offset, 250);

	/* A change of clock drops a two-step Sync in flight: its stamp is of the other clock. */
	make_sync(m, 2, true, 0, 0);
	CHECK(!ptp_rx(&s, m, 44, tai, SEC(3), &r));
	ptp_set_local_timescale(&s, false);
	make_follow_up(m, 2, tai, 0);
	CHECK(!ptp_rx(&s, m, 44, 0, SEC(3), &r));

	/* Back on the system clock, the shift applies again. */
	make_sync(m, 3, false, tai, 0);
	CHECK(ptp_rx(&s, m, 44, SEC(T0) + 250, SEC(4), &r));
	CHECK_EQ(r.offset, 250);

	/* An arbitrary timescale has no shift: the UTC offset of the Announce is not applied. */
	make_announce(m, 128, 6, true, 37);
	m[7] = 0x04;
	ptp_rx(&s, m, 64, 0, SEC(5), &r);
	CHECK_EQ(ptp_utc_shift(&s), 0);
}

/* A Delay_Req stamped as it leaves: its time comes after it is sent. */
static void test_ptp_delay_req_stamped_later(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64], req[PTP_DELAY_REQ_LEN];
	ns_t tai = SEC(T0) + SEC(37);
	ns_t offset = 300, delay = 2 * NS_PER_US;
	ns_t t2 = tai + offset + delay, t3, t4;
	uint16_t seq;

	ptp_slave_init(&s, 0, SLAVE);
	ptp_set_local_timescale(&s, true);
	make_announce(m, 128, 6, true, 37);
	ptp_rx(&s, m, 64, 0, SEC(1), &r);
	make_sync(m, 1, false, tai, 0);
	CHECK(ptp_rx(&s, m, 44, t2, SEC(2), &r));

	/* The answer before the stamp: no delay measured, and the request is done with. */
	CHECK_EQ(ptp_delay_req_later(&s, req), PTP_DELAY_REQ_LEN);
	seq = get16(req + 30);
	t3 = tai + SEC(1) + offset;
	t4 = t3 - offset + delay;
	make_delay_resp(m, seq, t4, SLAVE, 0);
	CHECK(!ptp_rx(&s, m, 54, 0, SEC(3), &r));
	CHECK(!s.delay_known);
	CHECK(!s.request_wait);
	/* A stamp that comes after that is of no request. */
	ptp_delay_req_sent(&s, t3);
	CHECK(!s.request_t3_known);

	/* The stamp first, then the answer: the delay. */
	CHECK_EQ(ptp_delay_req_later(&s, req), PTP_DELAY_REQ_LEN);
	seq = get16(req + 30);
	ptp_delay_req_sent(&s, t3);
	make_delay_resp(m, seq, t4, SLAVE, 0);
	CHECK(!ptp_rx(&s, m, 54, 0, SEC(4), &r));
	CHECK(s.delay_known);
	CHECK_EQ(s.delay, delay);

	make_sync(m, 2, false, tai, 0);
	CHECK(ptp_rx(&s, m, 44, t2, SEC(5), &r));
	CHECK_EQ(r.offset, offset);
}

static void test_ptp_rejects(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64];

	ptp_slave_init(&s, 4, SLAVE);
	make_announce(m, 128, 6, true, 37);
	/* Another domain. */
	CHECK(!ptp_rx(&s, m, 64, 0, SEC(1), &r));
	CHECK(!s.have_master);
	m[4] = 4;
	/* Version 1, a truncated message, a length longer than what came. */
	m[1] = 1;
	CHECK(!ptp_rx(&s, m, 64, 0, SEC(1), &r));
	m[1] = 2;
	CHECK(!ptp_rx(&s, m, 40, 0, SEC(1), &r));
	CHECK(!ptp_rx(&s, m, 30, 0, SEC(1), &r));
	put16(m + 2, 200);
	CHECK(!ptp_rx(&s, m, 64, 0, SEC(1), &r));
	put16(m + 2, 64);
	/* A clock of class 255 is slave-only and no master. */
	m[48] = 255;
	CHECK(!ptp_rx(&s, m, 64, 0, SEC(1), &r));
	m[48] = 6;
	CHECK(ptp_rx(&s, m, 64, 0, SEC(1), &r));

	/* A Sync from somebody else than the master. */
	make_sync(m, 1, false, SEC(T0), 0);
	m[28] ^= 0xff;
	CHECK(!ptp_rx(&s, m, 44, SEC(T0), SEC(2), &r));

	/* A nanosecond field of a second or more is no time. */
	make_sync(m, 2, false, SEC(T0), 0);
	put32(m + 40, 1000000000u);
	CHECK(!ptp_rx(&s, m, 44, SEC(T0), SEC(2), &r));
}

static void test_ptp_master_choice_and_loss(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64];

	ptp_slave_init(&s, 0, SLAVE);
	make_announce(m, 128, 6, true, 37);
	CHECK(ptp_rx(&s, m, 64, 0, SEC(1), &r));
	CHECK(ptp_master_alive(&s, SEC(1)));
	CHECK(ptp_master_alive(&s, SEC(9)));	/* three intervals, at least eight seconds */
	CHECK(!ptp_master_alive(&s, SEC(10)));

	/* Another node announces: a worse priority is ignored, a better one taken. */
	make_announce(m, 200, 6, true, 37);
	m[20] ^= 0x7f;
	m[53] ^= 0x7f;
	CHECK(!ptp_rx(&s, m, 64, 0, SEC(2), &r));
	CHECK(!memcmp(s.master.clock, MASTER, 8));
	make_announce(m, 100, 6, true, 37);
	m[20] ^= 0x7f;
	m[53] ^= 0x7f;
	CHECK(ptp_rx(&s, m, 64, 0, SEC(3), &r));
	CHECK_EQ(r.ev, PTP_EV_MASTER);
	CHECK(memcmp(s.master.clock, MASTER, 8));

	/* The one in place gone quiet is replaced by anyone. */
	make_announce(m, 250, 6, true, 37);
	CHECK(ptp_rx(&s, m, 64, 0, SEC(60), &r));
	CHECK(!memcmp(s.master.clock, MASTER, 8));

	ptp_master_lost(&s);
	CHECK(!s.have_master);
	CHECK_EQ(ptp_delay_req(&s, m, 0), 0);
}

static void test_ptp_step_drops_stamps_in_flight(void)
{
	struct ptp_slave s;
	struct ptp_result r;
	uint8_t m[64], req[PTP_DELAY_REQ_LEN];
	ns_t utc = SEC(T0);

	ptp_slave_init(&s, 0, SLAVE);
	make_announce(m, 128, 6, true, 37);
	ptp_rx(&s, m, 64, 0, SEC(1), &r);
	make_sync(m, 1, false, utc + SEC(37), 0);
	CHECK(ptp_rx(&s, m, 44, utc, SEC(2), &r));
	ptp_delay_req(&s, req, utc);
	ptp_clock_stepped(&s);
	/* The Delay_Resp that comes after the step is of the clock before it: no delay from it. */
	make_delay_resp(m, get16(req + 30), utc + SEC(37), SLAVE, 0);
	CHECK(!ptp_rx(&s, m, 54, 0, SEC(3), &r));
	CHECK(!s.delay_known);
}

static void test_clock_id(void)
{
	static const uint8_t mac[6] = { 0x40, 0xed, 0x00, 0x81, 0x5b, 0xfb };
	uint8_t id[8];

	ptp_clock_id_from_mac(id, mac);
	CHECK(!memcmp(id, "\x40\xed\x00\xff\xfe\x81\x5b\xfb", 8));
}

/* ---- the choice of source ---- */

static void test_select(void)
{
	struct select sel;
	ns_t t = SEC(100);

	select_init(&sel);
	CHECK_EQ(select_pick(&sel, t), SRC_NONE);

	/* NTP is good at its first sample, PTP only at its third. */
	CHECK_EQ(select_sample(&sel, SRC_NTP, 1000, t, SEC(200)), SRC_NTP);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 10, t + SEC(1), SEC(8)), SRC_NTP);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 10, t + SEC(2), SEC(8)), SRC_NTP);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 10, t + SEC(3), SEC(8)), SRC_PTP);
	CHECK(select_good(&sel, SRC_NTP, t + SEC(3)));

	/* PTP stays while it gives samples, and NTP's samples do not take it over. */
	CHECK_EQ(select_sample(&sel, SRC_NTP, 5, t + SEC(10), SEC(200)), SRC_PTP);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 10, t + SEC(10), SEC(8)), SRC_PTP);

	/* PTP goes silent: after its hold NTP is used. */
	CHECK_EQ(select_pick(&sel, t + SEC(18)), SRC_PTP);
	CHECK_EQ(select_pick(&sel, t + SEC(19)), SRC_NTP);
	CHECK_EQ(sel.s[SRC_PTP].good, 0);

	/* PTP comes back: three samples again before it is trusted. */
	CHECK_EQ(select_sample(&sel, SRC_PTP, 10, t + SEC(30), SEC(8)), SRC_NTP);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 10, t + SEC(31), SEC(8)), SRC_NTP);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 10, t + SEC(32), SEC(8)), SRC_PTP);

	/* A gap between two samples longer than the hold breaks the run. */
	select_init(&sel);
	select_sample(&sel, SRC_PTP, 1, t, SEC(8));
	select_sample(&sel, SRC_PTP, 1, t + SEC(1), SEC(8));
	select_sample(&sel, SRC_PTP, 1, t + SEC(30), SEC(8));
	CHECK_EQ(select_pick(&sel, t + SEC(30)), SRC_NONE);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 1, t + SEC(31), SEC(8)), SRC_NONE);
	CHECK_EQ(select_sample(&sel, SRC_PTP, 1, t + SEC(32), SEC(8)), SRC_PTP);

	/* The OLT is the last resort, good for a long hold. */
	select_init(&sel);
	CHECK_EQ(select_sample(&sel, SRC_OLT, 0, t, SEC(36 * 3600)), SRC_OLT);
	CHECK_EQ(select_sample(&sel, SRC_NTP, 0, t + SEC(5), SEC(200)), SRC_NTP);
	CHECK_EQ(select_pick(&sel, t + SEC(300)), SRC_OLT);
	CHECK_EQ(select_pick(&sel, t + SEC(37 * 3600)), SRC_NONE);

	CHECK_EQ(select_sample(&sel, SRC_NONE, 0, t, SEC(1)), SRC_NONE);
	CHECK(!strcmp(source_name(SRC_PTP), "ptp"));
	CHECK(!strcmp(source_name(SRC_NONE), "none"));
	CHECK(!strcmp(source_name(SRC_OLT), "olt"));
	CHECK(!strcmp(source_name(SRC_NTP), "ntp"));
}

/*
 * A server that keeps answering while its filter holds an older best sample
 * stays in use: without the refresh NTP went stale after its hold and the
 * source fell to none between two new best samples.
 */
static void test_select_refresh(void)
{
	struct select sel;
	ns_t t = SEC(100);

	select_init(&sel);
	/* Nothing to keep alive before a first sample, nor for a source not yet good. */
	select_refresh(&sel, SRC_NTP, t, SEC(192));
	CHECK(!select_good(&sel, SRC_NTP, t));
	select_sample(&sel, SRC_PTP, 1, t, SEC(8));
	select_refresh(&sel, SRC_PTP, t + SEC(1), SEC(100));
	CHECK_EQ(sel.s[SRC_PTP].valid_until, t + SEC(8));

	CHECK_EQ(select_sample(&sel, SRC_NTP, 1000, t, SEC(192)), SRC_NTP);
	/* Answers every 64 s that bring no new best keep it, well past its first hold. */
	for (int i = 1; i <= 10; i++)
		select_refresh(&sel, SRC_NTP, t + SEC(64 * i), SEC(192));
	CHECK_EQ(select_pick(&sel, t + SEC(64 * 10 + 100)), SRC_NTP);
	/* The offset is the last sample's: a refresh brings none. */
	CHECK_EQ(sel.s[SRC_NTP].offset, 1000);
	/* Silence after the last answer: gone after its hold. */
	CHECK_EQ(select_pick(&sel, t + SEC(64 * 10 + 193)), SRC_NONE);
	/* A stale source is not brought back by a refresh: it needs a sample. */
	select_refresh(&sel, SRC_NTP, t + SEC(64 * 11), SEC(192));
	CHECK_EQ(select_pick(&sel, t + SEC(64 * 11)), SRC_NONE);
	select_refresh(&sel, SRC_NONE, t, SEC(1));
	select_refresh(&sel, SRC_COUNT, t, SEC(1));
}

/* ---- the OLT's time ---- */

static void test_olt(void)
{
	static const char good[] = "utc=1791892800\nmonotonic_ns=5000000000\n";
	ns_t utc = 0, at = 0;

	CHECK(olt_parse(good, sizeof(good) - 1, &utc, &at));
	CHECK_EQ(utc, SEC(T0));
	CHECK_EQ(at, SEC(5));
	/* Any order, no last newline, other keys around. */
	{
		static const char other[] = "monotonic_ns=7\nsource=omci\nutc=1";

		CHECK(olt_parse(other, sizeof(other) - 1, &utc, &at));
		CHECK_EQ(utc, SEC(1));
		CHECK_EQ(at, 7);
	}
	/* A key is matched whole: "utc_s=" is not "utc=". */
	{
		static const char near[] = "utc_s=5\nmonotonic_ns=7\n";

		CHECK(!olt_parse(near, sizeof(near) - 1, &utc, &at));
	}
	/* Missing, empty, not a number, too large. */
	{
		static const char *const bad[] = {
			"utc=1791892800\n",
			"monotonic_ns=5\n",
			"utc=\nmonotonic_ns=5\n",
			"utc=-1\nmonotonic_ns=5\n",
			"utc=12x\nmonotonic_ns=5\n",
			"utc=7258118401\nmonotonic_ns=5\n",
			"utc=1\nmonotonic_ns=99999999999999999999\n",
			"",
		};

		for (size_t i = 0; i < ARRAY_SIZE(bad); i++)
			CHECK(!olt_parse(bad[i], strlen(bad[i]), &utc, &at));
	}
	/* The length is the caller's: bytes after it are not read. */
	CHECK(!olt_parse(good, 10, &utc, &at));

	/* The OLT said T0 at monotonic 5 s; at monotonic 65 s the local clock reads T0 + 62 s: 2 s ahead. */
	CHECK_EQ(olt_offset(SEC(T0), SEC(5), SEC(T0 + 62), SEC(65)), SEC(2));
	/* A clock still at 1970 is behind by the whole date. */
	CHECK_EQ(olt_offset(SEC(T0), SEC(5), SEC(60), SEC(65)), SEC(60) - SEC(T0 + 60));
}

/* ---- the servo ---- */

/* A noise of up to @amp either way, the same every run. */
static ns_t noise(unsigned int *seed, ns_t amp)
{
	*seed = *seed * 1103515245u + 12345u;
	return amp ? (ns_t)((*seed >> 8) % (2 * (unsigned int)amp + 1)) - amp : 0;
}

struct run {
	double worst;		/* the largest true offset over the last quarter of the samples */
	double drift;
	unsigned int steps;
	unsigned int jumps;
};

/*
 * A clock that gains @gain_ppb on its source and starts @start ns ahead,
 * sampled every @interval with a noise of up to @amp, @n times: what the
 * servo leaves of the offset.
 */
static struct run simulate_spikes(double gain_ppb, ns_t start, ns_t interval, ns_t amp, int n,
				  int spike_every, bool median)
{
	struct servo s;
	struct median3 m;
	struct run r = { 0, 0, 0, 0 };
	double e = (double)start, freq = 0;
	unsigned int seed = 1;
	ns_t at = SEC(100);

	servo_init(&s, 128 * NS_PER_MS, 0, false);
	s.baseline = interval > SEC(1) ? SERVO_BASELINE_NTP : SERVO_BASELINE_PTP;
	median3_reset(&m);
	for (int i = 0; i < n; i++) {
		ns_t measured = (ns_t)e + noise(&seed, amp);
		double f = 0;

		/* A stamp that a stalled sender put 300 us late. */
		if (spike_every && i > 20 && i % spike_every == 0)
			measured -= 300 * NS_PER_US;
		if (median)
			measured = median3_add(&m, measured);

		switch (servo_sample(&s, measured, at, &f)) {
		case SERVO_STEP:
			e -= (double)measured;
			r.steps++;
			break;
		case SERVO_JUMP:
			e -= (double)measured;
			freq = f;
			r.jumps++;
			break;
		case SERVO_FREQ:
			freq = f;
			break;
		case SERVO_HOLD:
			break;
		}
		if (i >= n - n / 4 && (e < 0 ? -e : e) > r.worst)
			r.worst = e < 0 ? -e : e;
		e += (gain_ppb - freq) * (double)interval / 1e9;
		at += interval;
	}
	r.drift = s.drift;
	return r;
}

static struct run simulate(double gain_ppb, ns_t start, ns_t interval, ns_t amp, int n)
{
	return simulate_spikes(gain_ppb, start, interval, amp, n, 0, false);
}

/*
 * A reference whose rate moves 1 ppm every 30 s, as a PHC steered from a
 * master does, read with @noise once a second, the samples through a median
 * of three or not: the largest error over the second half.
 */
static double simulate_wander(enum servo_gains gains, ns_t noise_amp, bool median)
{
	struct servo s;
	struct median3 m;
	double e = 50 * NS_PER_US, freq = 0, worst = 0;
	unsigned int seed = 3;
	ns_t at = SEC(100);

	servo_init(&s, 128 * NS_PER_MS, 0, false);
	s.baseline = SERVO_BASELINE_PTP;
	s.gains = gains;
	median3_reset(&m);
	for (int i = 0; i < 300; i++) {
		double gain = (i / 30) % 2 ? 6000 : 5000;
		ns_t measured = (ns_t)e + noise(&seed, noise_amp);
		double f = 0;

		if (median)
			measured = median3_add(&m, measured);
		switch (servo_sample(&s, measured, at, &f)) {
		case SERVO_STEP:
			e -= (double)measured;
			median3_reset(&m);
			break;
		case SERVO_JUMP:
			e -= (double)measured;
			freq = f;
			break;
		case SERVO_FREQ:
			freq = f;
			break;
		case SERVO_HOLD:
			break;
		}
		if (i >= 150 && (e < 0 ? -e : e) > worst)
			worst = e < 0 ? -e : e;
		e += (gain - freq) / 1e9 * (double)NS_PER_SEC;
		at += SEC(1);
	}
	return worst;
}

static void test_servo(void)
{
	struct servo s;
	struct run r;
	double f = 0;

	/*
	 * A PHC whose rate moves, read near exactly: the software gains lag
	 * by microseconds, the hardware ones keep within a few of the noise.
	 */
	CHECK(simulate_wander(SERVO_GAINS_SOFT, NS_PER_US, false) > 5 * NS_PER_US);
	CHECK(simulate_wander(SERVO_GAINS_HARD, NS_PER_US, false) < 3 * NS_PER_US);
	/*
	 * Through a median of three, a sample late: the hard gains swing, the
	 * filtered ones hold, with 1 us of noise and with a master's 10 us.
	 */
	CHECK(simulate_wander(SERVO_GAINS_HARD, NS_PER_US, true) > 100 * NS_PER_US);
	CHECK(simulate_wander(SERVO_GAINS_FILTERED, NS_PER_US, true) < 4 * NS_PER_US);
	CHECK(simulate_wander(SERVO_GAINS_FILTERED, 10 * NS_PER_US, true) < 20 * NS_PER_US);

	/* A rate left at the limit by a servo before is not taken over. */
	servo_init(&s, 128 * NS_PER_MS, -500000, true);
	CHECK(!s.rate_known);
	CHECK_EQ((long long)s.freq, 0);
	servo_init(&s, 128 * NS_PER_MS, -6300, true);
	CHECK(s.rate_known);
	CHECK_EQ((long long)s.freq, -6300);

	/* PTP: a crystal 5 ppm fast, 200 us out, a sample a second: below 1 us, the rate found. */
	r = simulate(5000, 200 * NS_PER_US, SEC(1), 0, 240);
	CHECK(r.worst < 1000);
	CHECK(r.drift > 4990 && r.drift < 5010);
	CHECK_EQ(r.steps, 0);
	CHECK_EQ(r.jumps, 1);
	/* The same with the 10 us of a software stamp: the clock stays within it. */
	r = simulate(5000, 200 * NS_PER_US, SEC(1), 10 * NS_PER_US, 600);
	CHECK(r.worst < 15 * NS_PER_US);
	CHECK(r.drift > 4000 && r.drift < 6000);
	/* Slow the other way. */
	r = simulate(-30000, -50 * NS_PER_US, SEC(1), 10 * NS_PER_US, 600);
	CHECK(r.worst < 15 * NS_PER_US);
	/* A stamp 300 us out every 50 s: the median of three does not let it through. */
	r = simulate_spikes(5000, 200 * NS_PER_US, SEC(1), 10 * NS_PER_US, 600, 50, false);
	CHECK(r.worst > 25 * NS_PER_US);
	r = simulate_spikes(5000, 200 * NS_PER_US, SEC(1), 10 * NS_PER_US, 600, 50, true);
	CHECK(r.worst < 15 * NS_PER_US);
	/* A change of source halfway, 85 us away from the first: the rate is not estimated again. */
	{
		struct servo sv;
		double e = 200 * NS_PER_US, fr = 0, worst = 0, f2 = 0;
		unsigned int seed = 7;
		ns_t at = SEC(100);

		servo_init(&sv, 128 * NS_PER_MS, 0, false);
		for (int i = 0; i < 400; i++) {
			ns_t bias = i >= 200 ? 85 * NS_PER_US : 0;
			ns_t m = (ns_t)e + bias + noise(&seed, 10 * NS_PER_US);
			enum servo_action act;

			if (i == 200)
				servo_reset(&sv);
			act = servo_sample(&sv, m, at, &f2);
			if (act == SERVO_STEP || act == SERVO_JUMP)
				e -= (double)m;
			if (act == SERVO_JUMP || act == SERVO_FREQ)
				fr = f2;
			/* What is measured against the new source: the true offset plus its bias. */
			if (i >= 210 && ((e + (double)bias) < 0 ? -(e + (double)bias) : e + (double)bias) > worst)
				worst = (e + (double)bias) < 0 ? -(e + (double)bias) : e + (double)bias;
			e += (5000 - fr) * 1.0;
			at += SEC(1);
		}
		CHECK(worst < 25 * NS_PER_US);
		CHECK(sv.drift > 4500 && sv.drift < 5500);
	}
	/* NTP polled every 16 s and every 64 s, with 100 us of noise. */
	r = simulate(5000, 300 * NS_PER_US, SEC(16), 100 * NS_PER_US, 300);
	CHECK(r.worst < 300 * NS_PER_US);
	r = simulate(5000, 300 * NS_PER_US, SEC(64), 100 * NS_PER_US, 200);
	CHECK(r.worst < 500 * NS_PER_US);
	/* A clock 1 s out is stepped once, then held. */
	r = simulate(5000, SEC(1), SEC(1), 0, 240);
	CHECK_EQ(r.steps, 1);
	CHECK(r.worst < 1000);
	/* 100 ms out once locked: the frequency stops at the kernel's 500 ppm, and the integral takes nothing. */
	{
		struct servo sv;
		double f2 = 0;

		servo_init(&sv, 128 * NS_PER_MS, 0, true);
		CHECK_EQ(servo_sample(&sv, 0, SEC(10), &f2), SERVO_FREQ);
		CHECK_EQ(servo_sample(&sv, 100 * NS_PER_MS, SEC(11), &f2), SERVO_FREQ);
		CHECK_EQ(f2, SERVO_MAX_PPB);
		CHECK_EQ(sv.drift, 0);
		CHECK_EQ(servo_sample(&sv, -100 * NS_PER_MS, SEC(12), &f2), SERVO_FREQ);
		CHECK_EQ(f2, -SERVO_MAX_PPB);
	}

	/* The steps of the state: hold, hold until 4 s, the rate, then the loop. */
	servo_init(&s, 128 * NS_PER_MS, 1000, false);
	CHECK_EQ(servo_sample(&s, 0, SEC(10), &f), SERVO_HOLD);
	CHECK_EQ(servo_sample(&s, 1000, SEC(11), &f), SERVO_HOLD);
	CHECK_EQ(servo_sample(&s, 4000, SEC(14), &f), SERVO_FREQ);
	/* 4000 ns gained in 4 s at 1000 ppb: the clock needs 2000 ppb; 4 us out is left to the loop. */
	CHECK(f > 1999 && f < 2001);
	CHECK_EQ(servo_sample(&s, 0, SEC(14), &f), SERVO_HOLD);
	CHECK_EQ(servo_sample(&s, 0, SEC(15), &f), SERVO_FREQ);
	/* Beyond the limit either way: a step; the rate is kept, and the next sample locks at once. */
	CHECK_EQ(servo_sample(&s, 129 * NS_PER_MS, SEC(16), &f), SERVO_STEP);
	CHECK_EQ(servo_sample(&s, -129 * NS_PER_MS, SEC(17), &f), SERVO_STEP);
	CHECK_EQ(servo_sample(&s, 0, SEC(18), &f), SERVO_FREQ);
	CHECK(f > 1999 && f < 2001);
	/* A new source 85 us away from the old: the phase is stepped, the rate stays. */
	servo_reset(&s);
	CHECK_EQ(s.count, 0);
	CHECK_EQ(servo_sample(&s, 85 * NS_PER_US, SEC(19), &f), SERVO_JUMP);
	CHECK(f > 1999 && f < 2001);
	CHECK_EQ(servo_sample(&s, 1000, SEC(20), &f), SERVO_FREQ);
	/* 40 us out when the rate is found: a jump. */
	servo_init(&s, 128 * NS_PER_MS, 0, false);
	CHECK_EQ(servo_sample(&s, 0, SEC(10), &f), SERVO_HOLD);
	CHECK_EQ(servo_sample(&s, 40 * NS_PER_US, SEC(14), &f), SERVO_JUMP);
	CHECK(f > 9999 && f < 10001);
	/* A start at a frequency beyond the limit, no crystal's, starts from 0. */
	servo_init(&s, 128 * NS_PER_MS, -2 * SERVO_MAX_PPB, false);
	CHECK_EQ(s.freq, 0);
	/* A rate the kernel held at the start is not estimated again: the first sample locks. */
	servo_init(&s, 128 * NS_PER_MS, 6000, true);
	CHECK_EQ(servo_sample(&s, 300 * NS_PER_US, SEC(10), &f), SERVO_JUMP);
	CHECK(f > 5999 && f < 6001);
	/* 1.6 ms over 4 s would be 400 ppm, no crystal's: the estimate starts again from there. */
	servo_init(&s, 128 * NS_PER_MS, 0, false);
	CHECK_EQ(servo_sample(&s, 0, SEC(10), &f), SERVO_HOLD);
	CHECK_EQ(servo_sample(&s, 1600 * NS_PER_US, SEC(14), &f), SERVO_HOLD);
	/* NTP's baseline is 64 s: 640 us over it is 10 ppm. */
	s.baseline = SERVO_BASELINE_NTP;
	CHECK_EQ(servo_sample(&s, 1700 * NS_PER_US, SEC(40), &f), SERVO_HOLD);
	CHECK_EQ(servo_sample(&s, 2240 * NS_PER_US, SEC(78), &f), SERVO_JUMP);
	CHECK(f > 9999 && f < 10001);
}

static void test_median3(void)
{
	struct median3 m;

	median3_reset(&m);
	CHECK_EQ(median3_add(&m, 5), 5);
	CHECK_EQ(median3_add(&m, 9), 9);
	CHECK_EQ(median3_add(&m, 1), 5);
	CHECK_EQ(median3_add(&m, 300), 9);
	CHECK_EQ(median3_add(&m, 7), 7);
	CHECK_EQ(median3_add(&m, 8), 8);
	CHECK_EQ(median3_add(&m, -400), 7);
	CHECK_EQ(median3_add(&m, 6), 6);
	median3_reset(&m);
	CHECK_EQ(median3_add(&m, -3), -3);
}

/* ---- the receive stamp ---- */

static void test_stamp(void)
{
	int64_t wide[2] = { 1791915498, 433484729 };
	int32_t narrow[2] = { 1791915498, 433484729 };
	ns_t t = 0;

	/* Both forms of the kernel's timespec, by their length: the nanoseconds are kept. */
	CHECK(stamp_parse(wide, sizeof(wide), &t));
	CHECK_EQ(t, 1791915498LL * NS_PER_SEC + 433484729);
	CHECK(stamp_parse(narrow, sizeof(narrow), &t));
	CHECK_EQ(t, 1791915498LL * NS_PER_SEC + 433484729);
	/* Padding after the stamp does not matter. */
	{
		int64_t padded[4] = { 5, 7, 0, 0 };

		CHECK(stamp_parse(padded, sizeof(padded), &t));
		CHECK_EQ(t, 5 * NS_PER_SEC + 7);
	}
	/* Too short, no seconds, a nanosecond count of a second or more. */
	CHECK(!stamp_parse(wide, 7, &t));
	wide[0] = 0;
	CHECK(!stamp_parse(wide, sizeof(wide), &t));
	wide[0] = 5;
	wide[1] = 1000000000;
	CHECK(!stamp_parse(wide, sizeof(wide), &t));
	wide[1] = -1;
	CHECK(!stamp_parse(wide, sizeof(wide), &t));
}

/* ---- the hardware stamp ---- */

static void test_stamp_hw(void)
{
	int64_t wide[6] = { 9, 1, 0, 0, 1791915498, 433484729 };
	int32_t narrow[6] = { 9, 1, 0, 0, 51, 8 };
	ns_t t = 0;

	/* The third of three stamps, in either size. */
	CHECK(stamp_parse_hw(wide, sizeof(wide), &t));
	CHECK_EQ(t, 1791915498LL * NS_PER_SEC + 433484729);
	CHECK(stamp_parse_hw(narrow, sizeof(narrow), &t));
	CHECK_EQ(t, 51 * NS_PER_SEC + 8);
	/* No hardware stamp (zero), or not three stamps. */
	wide[4] = 0;
	wide[5] = 0;
	CHECK(!stamp_parse_hw(wide, sizeof(wide), &t));
	CHECK(!stamp_parse_hw(narrow, 5 * sizeof(int32_t), &t));
}

/* ---- the system clock against the PHC ---- */

static void test_phc_sys_offset(void)
{
	struct phc_reading r;
	ns_t off = 0, win = 0;

	/* The PHC 37 s ahead and 1 us more; the second pair is the closest, 4 us apart. */
	memset(&r, 0, sizeof(r));
	r.n = 3;
	r.sys[0] = SEC(T0);
	r.phc[0] = SEC(T0 + 37) + 20 * NS_PER_US;
	r.sys[1] = SEC(T0) + 30 * NS_PER_US;
	r.phc[1] = SEC(T0 + 37) + 33 * NS_PER_US;
	r.sys[2] = SEC(T0) + 34 * NS_PER_US;
	r.phc[2] = SEC(T0 + 37) + 50 * NS_PER_US;
	r.sys[3] = SEC(T0) + 60 * NS_PER_US;
	CHECK(phc_sys_offset(&r, &off, &win));
	CHECK_EQ(win, 4 * NS_PER_US);
	CHECK_EQ(off, -SEC(37) - NS_PER_US);

	/* A pair out of order (the system clock stepped back between) is passed over. */
	r.sys[2] = SEC(T0) + 20 * NS_PER_US;
	CHECK(phc_sys_offset(&r, &off, &win));
	CHECK_EQ(win, 30 * NS_PER_US);
	CHECK_EQ(off, SEC(T0) + 15 * NS_PER_US - r.phc[0]);

	/* None in order, or none at all. */
	r.n = 1;
	r.sys[1] = r.sys[0] - 1;
	CHECK(!phc_sys_offset(&r, &off, &win));
	r.n = 0;
	CHECK(!phc_sys_offset(&r, &off, &win));
}

int main(void)
{
	test_ntp_time_conversion();
	test_ntp_request_and_answer();
	test_ntp_refuses_bad_answers();
	test_ntp_delay_never_negative();
	test_ntp_filter();
	test_ptp_master_and_one_step();
	test_ptp_two_step_and_delay();
	test_ptp_timescale();
	test_ptp_phc_timescale();
	test_ptp_delay_req_stamped_later();
	test_ptp_rejects();
	test_ptp_master_choice_and_loss();
	test_ptp_step_drops_stamps_in_flight();
	test_clock_id();
	test_select();
	test_select_refresh();
	test_olt();
	test_servo();
	test_median3();
	test_stamp();
	test_stamp_hw();
	test_phc_sys_offset();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
