// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the time of day set by Synchronize time. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/clock.h"
#include "omcid/array_size.h"

#define MT_SYNC_TIME 24u
#define MT_CREATE 4u
#define MT_GET 9u
#define MT_SET 8u
#define ME_OLT_G 131u
#define ME_ETH_PM_UP 322u
#define PM_INSTANCE 0x0101u

static void make_pm_entity(struct omci_engine *eng)
{
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, 0x00F0u, MT_CREATE, true, false, ME_ETH_PM_UP,
		     PM_INSTANCE);
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
}

static void read_pm(struct omci_engine *eng)
{
	static uint16_t tid = 0x00F1u;
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tid++, MT_GET, true, false, ME_ETH_PM_UP,
		     PM_INSTANCE);
	wire[8] = 0x40u;
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
}

static enum omci_status send_sync(struct omci_engine *eng, uint16_t tci,
				  uint16_t year, uint8_t month, uint8_t day,
				  uint8_t hour, uint8_t minute, uint8_t second)
{
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tci, MT_SYNC_TIME, true, false, 256u, 0u);

	wire[8] = (uint8_t)(year >> 8);
	wire[9] = (uint8_t)(year & 0xffu);
	wire[10] = month;
	wire[11] = day;
	wire[12] = hour;
	wire[13] = minute;
	wire[14] = second;

	return omci_t_rx(eng, wire);
}

static void test_days_from_civil(void)
{
	static const struct {
		int64_t y;
		unsigned int m, d;
		int64_t want;
		const char *why;
	} k[] = {
		{ 1970, 1, 1, 0, "the epoch itself" },
		{ 1970, 1, 2, 1, "and the day after it" },
		{ 1969, 12, 31, -1, "dates before the epoch are negative" },
		{ 1972, 2, 28, 788, "the day before a leap day" },
		{ 1972, 2, 29, 789, "1972 was a leap year: this date exists" },
		{ 1972, 3, 1, 790, "and the day after it" },
		{ 2000, 2, 29, 11016,
		  "2000 IS a leap year — divisible by 400" },
		{ 2000, 3, 1, 11017, "the century rule does not apply here" },
		{ 1900, 3, 1, -25508,
		  "1900 is NOT a leap year — divisible by 100, not 400" },
		{ 2026, 8, 15, 20680, "today, as this is written" },
		{ 2038, 1, 19, 24855,
		  "the 32-bit time_t rollover, which this arithmetic does not have" },
		{ 2100, 3, 1, 47541, "the next skipped leap year" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(k); i++) {
		const int64_t got =
			omci_clock_days_from_civil(k[i].y, k[i].m, k[i].d);
		if (got != k[i].want)
			printf("    %04ld-%02u-%02u: got %ld, want %ld (%s)\n",
			       (long)k[i].y, k[i].m, k[i].d, (long)got,
			       (long)k[i].want, k[i].why);
		CHECK(got == k[i].want);
	}

	int64_t prev = omci_clock_days_from_civil(1899, 12, 31);

	for (int64_t y = 1900; y <= 1901; y++) {
		static const unsigned int dim[12] = { 31, 28, 31, 30, 31, 30,
						      31, 31, 30, 31, 30, 31 };
		for (unsigned int m = 1; m <= 12u; m++) {
			for (unsigned int d = 1; d <= dim[m - 1u]; d++) {
				const int64_t got =
					omci_clock_days_from_civil(y, m, d);
				CHECK(got == prev + 1);
				prev = got;
			}
		}
	}
}

static void test_the_clock_anchors_wall_time_to_the_monotonic_one(void)
{
	struct omci_clock c;

	memset(&c, 0, sizeof(c));

	CHECK(!omci_clock_is_set(&c));
	CHECK_EQ((unsigned long)omci_clock_now(&c, 5000u), 0ul);

	CHECK(omci_clock_sync(&c, 2026u, 8u, 15u, 12u, 0u, 0u, 1000u));
	CHECK(omci_clock_is_set(&c));

	const uint64_t at_sync = (uint64_t)20680 * 86400u + 43200u;

	CHECK(omci_clock_now(&c, 1000u) == at_sync);
	CHECK(omci_clock_now(&c, 1060u) == at_sync + 60u);
	CHECK(omci_clock_now(&c, 1000u + 86400u) == at_sync + 86400u);

	CHECK(omci_clock_now(&c, 0u) == at_sync);

	omci_clock_forget(&c);
	CHECK(!omci_clock_is_set(&c));
	CHECK_EQ((unsigned long)omci_clock_now(&c, 9999u), 0ul);
}

static void test_the_clock_refuses_impossible_stamps(void)
{
	static const struct {
		uint16_t y;
		uint8_t mo, d, h, mi, s;
		bool ok;
		const char *why;
	} k[] = {
		{ 2026, 8, 15, 12, 30, 45, true, "an ordinary instant" },
		{ 2026, 2, 28, 23, 59, 59, true,
		  "the last second of a non-leap February" },
		{ 2024, 2, 29, 0, 0, 0, true, "2024 was a leap year" },
		{ 2026, 2, 29, 0, 0, 0, false,
		  "2026 was not: this date does not exist" },
		{ 2000, 2, 29, 0, 0, 0, true, "but 2000 was" },
		{ 2100, 2, 29, 0, 0, 0, false,
		  "2100 is NOT — divisible by 100, not 400. This case is why 1900 is not used: the epoch bound would reject it first and the leap rule would never be consulted" },
		{ 2026, 6, 30, 23, 59, 60, true,
		  "second 60 is a leap second, and they happen" },
		{ 2026, 0, 15, 12, 0, 0, false, "month 0" },
		{ 2026, 13, 15, 12, 0, 0, false, "month 13" },
		{ 2026, 4, 31, 12, 0, 0, false, "April has 30 days" },
		{ 2026, 8, 0, 12, 0, 0, false, "day 0" },
		{ 2026, 8, 15, 24, 0, 0, false,
		  "hour 24 is an end-of-day marker, not a clock reading" },
		{ 2026, 8, 15, 12, 60, 0, false, "minute 60" },
		{ 2026, 8, 15, 12, 0, 61, false,
		  "second 61 is not a leap second, it is a broken field" },
		{ 0, 0, 0, 0, 0, 0, false,
		  "an all-zero payload: a frame that lost its body" },
		{ 256, 8, 15, 12, 0, 0, false,
		  "year 256 — what a two-byte read at the wrong offset gives" },
		{ 1969, 12, 31, 23, 59, 59, false,
		  "before the epoch this arithmetic counts from" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(k); i++) {
		struct omci_clock c;

		memset(&c, 0, sizeof(c));

		const bool got = omci_clock_sync(&c, k[i].y, k[i].mo, k[i].d,
						 k[i].h, k[i].mi, k[i].s, 1u);
		if (got != k[i].ok)
			printf("    %04u-%02u-%02u %02u:%02u:%02u: got %d, want %d (%s)\n",
			       k[i].y, k[i].mo, k[i].d, k[i].h, k[i].mi, k[i].s,
			       got, k[i].ok, k[i].why);
		CHECK_EQ(got, k[i].ok);

		CHECK_EQ(omci_clock_is_set(&c), k[i].ok);
	}
}

static const uint8_t *reply_of(void)
{
	static const uint8_t none[OMCI_FRAME_LEN] = { 0xFFu };
	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	return rsp != NULL ? rsp : none;
}

static uint8_t end_time(struct omci_t_fixture *fx)
{
	uint8_t v = 0xEEu;

	read_pm(fx->eng);
	CHECK_EQ(omci_mib_attr_get(fx->mib, ME_ETH_PM_UP, PM_INSTANCE, 1u, &v,
				   1u, NULL),
		 OMCI_OK);
	return v;
}

static void test_sync_time_is_acknowledged(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	omci_t_tx_reset();

	CHECK_EQ(send_sync(fx.eng, 0x0101u, 2026u, 8u, 15u, 12u, 30u, 45u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	const uint8_t *rsp = reply_of();

	CHECK_EQ(rsp[2] & 0x1Fu, MT_SYNC_TIME);
	CHECK((rsp[2] & 0x20u) != 0u);
	CHECK_EQ(rsp[8], 0u);
	/* The date and the boundary were both set (G.988 A.3.34). */
	CHECK_EQ(rsp[9], 1u);

	omci_t_down(&fx);
}

static void test_a_sync_without_a_date_sets_only_the_boundary(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);
	omci_engine_tick(fx.eng, 1000u);

	/* Nothing at all: the interval starts now. */
	omci_t_tx_reset();
	CHECK_EQ(send_sync(fx.eng, 0x0130u, 0u, 0u, 0u, 0u, 0u, 0u), OMCI_OK);
	CHECK_EQ(reply_of()[8], 0u);
	CHECK_EQ(reply_of()[9], 0u);
	omci_engine_tick(fx.eng, 1899u);
	CHECK_EQ(end_time(&fx), 0u);
	omci_engine_tick(fx.eng, 1900u);
	CHECK_EQ(end_time(&fx), 1u);

	/* A time of day alone places the boundary: 00:20:00 is 300 s past it. */
	omci_t_tx_reset();
	CHECK_EQ(send_sync(fx.eng, 0x0131u, 0u, 0u, 0u, 0u, 20u, 0u), OMCI_OK);
	CHECK_EQ(reply_of()[8], 0u);
	CHECK_EQ(reply_of()[9], 0u);
	CHECK_EQ(end_time(&fx), 0u);
	omci_engine_tick(fx.eng, 1900u + 599u);
	CHECK_EQ(end_time(&fx), 0u);
	omci_engine_tick(fx.eng, 1900u + 600u);
	CHECK_EQ(end_time(&fx), 1u);

	/* A time of day that does not exist is refused like a date. */
	omci_t_tx_reset();
	CHECK_EQ(send_sync(fx.eng, 0x0132u, 0u, 0u, 0u, 24u, 0u, 0u), OMCI_OK);
	CHECK_EQ(reply_of()[8], 3u);
	CHECK_EQ(end_time(&fx), 1u);

	omci_t_down(&fx);
}

static void test_an_impossible_stamp_is_refused_and_not_applied(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);
	omci_engine_tick(fx.eng, 1000u);
	omci_engine_tick(fx.eng, 1900u);
	omci_t_tx_reset();

	CHECK_EQ(send_sync(fx.eng, 0x0102u, 2026u, 0u, 15u, 12u, 0u, 0u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK_EQ(reply_of()[8], 3u);

	/* The intervals go on as they were. */
	CHECK_EQ(end_time(&fx), 1u);

	omci_t_down(&fx);
}

static void test_a_sync_numbers_the_intervals_from_zero(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);

	omci_engine_tick(fx.eng, 1000u);
	CHECK_EQ(end_time(&fx), 0u);
	omci_engine_tick(fx.eng, 1900u);
	CHECK_EQ(end_time(&fx), 1u);

	/* 12:34:56 is 296 s into its quarter hour: 604 s are left of it. */
	CHECK_EQ(send_sync(fx.eng, 0x0103u, 2026u, 8u, 15u, 12u, 34u, 56u),
		 OMCI_OK);
	CHECK_EQ(end_time(&fx), 0u);
	omci_engine_tick(fx.eng, 1900u + 603u);
	CHECK_EQ(end_time(&fx), 0u);
	omci_engine_tick(fx.eng, 1900u + 604u);
	CHECK_EQ(end_time(&fx), 1u);
	omci_engine_tick(fx.eng, 1900u + 604u + 900u);
	CHECK_EQ(end_time(&fx), 2u);

	omci_t_down(&fx);
}

static void test_a_mib_reset_keeps_the_intervals(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);
	omci_engine_tick(fx.eng, 1000u);
	omci_engine_tick(fx.eng, 1900u);

	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, 0x0107u, 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
	make_pm_entity(fx.eng);

	/* G.988 I.4 leaves this open; the ONU keeps one clock for all. */
	CHECK_EQ(end_time(&fx), 1u);

	omci_t_down(&fx);
}

static void test_every_sync_starts_the_numbering_again(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);
	omci_engine_tick(fx.eng, 1000u);

	CHECK_EQ(send_sync(fx.eng, 0x0120u, 2026u, 8u, 15u, 12u, 34u, 56u),
		 OMCI_OK);
	omci_engine_tick(fx.eng, 1000u + 604u);
	CHECK_EQ(end_time(&fx), 1u);

	CHECK_EQ(send_sync(fx.eng, 0x0121u, 2026u, 8u, 15u, 12u, 45u, 0u),
		 OMCI_OK);
	CHECK_EQ(end_time(&fx), 0u);

	omci_t_down(&fx);
}

static uint64_t g_told[4];
static unsigned int g_tells;

static void tell(uint64_t utc)
{
	if (g_tells < ARRAY_SIZE(g_told))
		g_told[g_tells] = utc;
	g_tells++;
}

/*
 * A date reaches the backend, for the stick's time daemon, as seconds since
 * 1970; a time of day alone and a refused stamp do not.
 */
static void test_civil_time_takes_a_count_of_seconds_apart(void)
{
	static const struct {
		uint64_t secs;
		struct omci_civil want;
	} k[] = {
		{ 0u, { 1970, 1, 1, 0, 0, 0 } },
		{ 86399u, { 1970, 1, 1, 23, 59, 59 } },
		{ 789u * 86400u + 43200u, { 1972, 2, 29, 12, 0, 0 } },
		{ 11017u * 86400u + 1u, { 2000, 3, 1, 0, 0, 1 } },
		{ 20680u * 86400u + 45045u, { 2026, 8, 15, 12, 30, 45 } },
	};

	for (size_t i = 0; i < ARRAY_SIZE(k); i++) {
		struct omci_civil t;

		omci_clock_civil(k[i].secs, &t);
		CHECK_EQ(t.year, k[i].want.year);
		CHECK_EQ(t.month, k[i].want.month);
		CHECK_EQ(t.day, k[i].want.day);
		CHECK_EQ(t.hour, k[i].want.hour);
		CHECK_EQ(t.minute, k[i].want.minute);
		CHECK_EQ(t.second, k[i].want.second);
	}

	/* Every day to the year 2200 comes back as the day it was counted. */
	for (int64_t day = 0; day < 83000; day++) {
		struct omci_civil t;

		omci_clock_civil((uint64_t)day * 86400u + 3661u, &t);
		if (omci_clock_days_from_civil(t.year, t.month, t.day) != day ||
		    t.hour != 1u || t.minute != 1u || t.second != 1u) {
			CHECK_EQ(day, -1);
			break;
		}
	}
}

/* Get of the ONU time configuration: the time, then the qualification. */
static void read_onu_time(struct omci_engine *eng, uint8_t out[7])
{
	static uint16_t tid = 0x0150u;
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tid++, MT_GET, true, false, 457u, 0u);
	wire[8] = 0x80u;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	const uint8_t *rsp = reply_of();

	CHECK_EQ(rsp[8], 0u);
	memcpy(out, &rsp[11], 7);
}

static void test_the_onu_time_is_the_time_the_olt_gave(void)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;
	uint8_t t[7];

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);
	omci_engine_tick(fx.eng, 1000u);

	/* Before the OLT gives the time, the ONU has none. */
	read_onu_time(fx.eng, t);
	for (unsigned int i = 0; i < sizeof(t); i++)
		CHECK_EQ(t[i], 0u);

	CHECK_EQ(send_sync(fx.eng, 0x0151u, 2026u, 10u, 22u, 18u, 30u, 15u),
		 OMCI_OK);
	omci_engine_tick(fx.eng, 1042u);

	const uint32_t before = omci_mib_generation(fx.mib);

	read_onu_time(fx.eng, t);
	CHECK_EQ(t[0] << 8 | t[1], 2026u);
	CHECK_EQ(t[2], 10u);
	CHECK_EQ(t[3], 22u);
	CHECK_EQ(t[4], 18u);
	CHECK_EQ(t[5], 30u);
	CHECK_EQ(t[6], 57u);
	CHECK_EQ(omci_mib_generation(fx.mib), before);

	omci_t_down(&fx);
}

static void test_a_date_is_handed_to_the_backend(void)
{
	struct omci_hw_ops ops = omci_hw_host_ops;
	struct omci_t_fixture fx;

	ops.time_of_day = tell;
	g_tells = 0;
	omci_hw_ops_set(&ops);
	if (!omci_t_up(&fx)) {
		omci_hw_ops_set(NULL);
		return;
	}
	omci_engine_tick(fx.eng, 1000u);

	CHECK_EQ(send_sync(fx.eng, 0x0140u, 2026u, 8u, 15u, 12u, 30u, 45u),
		 OMCI_OK);
	CHECK_EQ(g_tells, 1u);
	/* 2026-08-15 is day 20680; 12:30:45 is 45045 s into it. */
	CHECK_EQ(g_told[0], 20680u * 86400u + 45045u);

	CHECK_EQ(send_sync(fx.eng, 0x0141u, 0u, 0u, 0u, 0u, 20u, 0u), OMCI_OK);
	CHECK_EQ(send_sync(fx.eng, 0x0142u, 2026u, 0u, 15u, 12u, 0u, 0u),
		 OMCI_OK);
	CHECK_EQ(g_tells, 1u);

	omci_t_down(&fx);
	omci_hw_ops_set(NULL);
}

static uint32_t g_tod_sf;
static uint64_t g_tod_sec;
static uint32_t g_tod_nsec;
static unsigned int g_tods;

static void tod_tell(uint32_t superframe, uint64_t sec, uint32_t nsec)
{
	g_tod_sf = superframe;
	g_tod_sec = sec;
	g_tod_nsec = nsec;
	g_tods++;
}

/* A Set of OLT-G's Time of day information: its 14 bytes, attribute 4. */
static enum omci_status send_tod(struct omci_engine *eng, uint16_t tci,
				 const uint8_t info[14])
{
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tci, MT_SET, true, false, ME_OLT_G, 0u);
	wire[8] = 0x10u;
	wire[9] = 0x00u;
	memcpy(&wire[10], info, 14);
	return omci_t_rx(eng, wire);
}

/*
 * The Time of day information the OLT sets in OLT-G reaches the backend as
 * the superframe and TstampN, its 48-bit seconds and its nanoseconds; an
 * all-zero one, no time, does not.
 */
static void test_olt_g_time_of_day_is_handed_to_the_backend(void)
{
	static const uint8_t info[14] = {
		0x02u, 0x34u, 0x56u, 0x78u, /* N */
		0x00u, 0x01u, 0x6au, 0x0bu, 0x9cu, 0x00u, /* seconds */
		0x07u, 0x5bu, 0xcdu, 0x15u, /* nanoseconds */
	};
	static const uint8_t none[14];
	struct omci_hw_ops ops = omci_hw_host_ops;
	struct omci_t_fixture fx;

	ops.tod_info = tod_tell;
	g_tods = 0;
	omci_hw_ops_set(&ops);
	if (!omci_t_up(&fx)) {
		omci_hw_ops_set(NULL);
		return;
	}
	CHECK_EQ(omci_mib_create(fx.mib, ME_OLT_G, 0u), OMCI_OK);

	CHECK_EQ(send_tod(fx.eng, 0x0150u, info), OMCI_OK);
	CHECK_EQ(reply_of()[8], 0u);
	CHECK_EQ(g_tods, 1u);
	CHECK_EQ(g_tod_sf, 0x02345678u);
	CHECK_EQ(g_tod_sec, 0x00016a0b9c00ull);
	CHECK_EQ(g_tod_nsec, 123456789u);

	CHECK_EQ(send_tod(fx.eng, 0x0151u, none), OMCI_OK);
	CHECK_EQ(g_tods, 1u);

	omci_t_down(&fx);
	omci_hw_ops_set(NULL);
}

int main(void)
{
	RUN(test_days_from_civil);
	RUN(test_the_clock_anchors_wall_time_to_the_monotonic_one);
	RUN(test_the_clock_refuses_impossible_stamps);

	RUN(test_sync_time_is_acknowledged);
	RUN(test_a_sync_without_a_date_sets_only_the_boundary);
	RUN(test_an_impossible_stamp_is_refused_and_not_applied);
	RUN(test_a_sync_numbers_the_intervals_from_zero);
	RUN(test_a_mib_reset_keeps_the_intervals);
	RUN(test_every_sync_starts_the_numbering_again);
	RUN(test_civil_time_takes_a_count_of_seconds_apart);
	RUN(test_the_onu_time_is_the_time_the_olt_gave);
	RUN(test_a_date_is_handed_to_the_backend);
	RUN(test_olt_g_time_of_day_is_handed_to_the_backend);

	return OMCI_T_REPORT("clock");
}
