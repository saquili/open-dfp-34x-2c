// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the time of day set by Synchronize time. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "mib/store.h"
#include "omci/clock.h"
#include "omcid/array_size.h"

#define MT_SYNC_TIME 24u
#define MT_CREATE 4u
#define MT_GET 9u
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

static void test_sync_time_is_acknowledged(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	omci_t_tx_reset();

	CHECK_EQ(send_sync(fx.eng, 0x0101u, 2026u, 8u, 15u, 12u, 30u, 45u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	CHECK_EQ(rsp[2] & 0x1Fu, MT_SYNC_TIME);
	CHECK((rsp[2] & 0x20u) != 0u);
	CHECK_EQ(rsp[8], 0u);
	CHECK_EQ(rsp[9], 0u);

	omci_t_down(&fx);
}

static void test_an_impossible_stamp_is_refused_and_not_applied(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);
	omci_t_tx_reset();

	CHECK_EQ(send_sync(fx.eng, 0x0102u, 2026u, 0u, 15u, 12u, 0u, 0u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	CHECK_EQ(rsp[8], 3u);

	read_pm(fx.eng);
	uint8_t end_time = 0xFFu;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &end_time, 1u, NULL),
		 OMCI_OK);
	CHECK_EQ(end_time, 0u);

	omci_t_down(&fx);
}

static void test_a_get_sees_the_interval_the_olt_is_counting(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);

	read_pm(fx.eng);
	uint8_t before = 0xFFu;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &before, 1u, NULL),
		 OMCI_OK);
	CHECK_EQ(before, 0u);

	omci_t_tx_reset();
	CHECK_EQ(send_sync(fx.eng, 0x0103u, 2026u, 8u, 15u, 12u, 34u, 56u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.frame[0][8], 0u);

	read_pm(fx.eng);
	uint8_t after = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &after, 1u, NULL),
		 OMCI_OK);

	const uint64_t wall =
		(uint64_t)20680 * 86400u + 12u * 3600u + 34u * 60u + 56u;
	const uint8_t expect = (uint8_t)((wall / 900u) & 0xffu);

	if (after != expect)
		printf("    interval end time %u, want %u (wall %lu)\n", after,
		       expect, (unsigned long)wall);
	CHECK_EQ(after, expect);
	CHECK(after != before);

	omci_t_down(&fx);
}

static void test_a_mib_reset_forgets_the_time(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);

	CHECK_EQ(send_sync(fx.eng, 0x0105u, 2026u, 8u, 15u, 12u, 34u, 56u),
		 OMCI_OK);
	read_pm(fx.eng);

	uint8_t synced = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &synced, 1u, NULL),
		 OMCI_OK);
	CHECK(synced != 0u);

	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, 0x0107u, 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);

	make_pm_entity(fx.eng);

	omci_engine_tick(fx.eng, 1000u);
	read_pm(fx.eng);

	uint8_t after_reset = 0xFFu;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &after_reset, 1u, NULL),
		 OMCI_OK);
	CHECK_EQ(after_reset, 0u);

	omci_t_down(&fx);
}

static void test_a_window_that_ends_advances_the_number_by_one(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);

	omci_engine_tick(fx.eng, 1000u);
	CHECK_EQ(send_sync(fx.eng, 0x0110u, 2026u, 8u, 15u, 12u, 34u, 56u),
		 OMCI_OK);

	read_pm(fx.eng);
	uint8_t first = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &first, 1u, NULL),
		 OMCI_OK);

	omci_engine_tick(fx.eng, 1000u + 901u);

	read_pm(fx.eng);
	uint8_t second = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &second, 1u, NULL),
		 OMCI_OK);

	CHECK_EQ(second, (uint8_t)(first + 1u));

	omci_engine_tick(fx.eng, 1000u + 902u);
	read_pm(fx.eng);
	uint8_t third = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &third, 1u, NULL),
		 OMCI_OK);
	CHECK_EQ(third, second);

	omci_t_down(&fx);
}

static void test_resyncing_inside_a_window_changes_nothing(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	make_pm_entity(fx.eng);

	CHECK_EQ(send_sync(fx.eng, 0x0120u, 2026u, 8u, 15u, 12u, 34u, 56u),
		 OMCI_OK);
	read_pm(fx.eng);
	uint8_t before = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &before, 1u, NULL),
		 OMCI_OK);

	CHECK_EQ(send_sync(fx.eng, 0x0121u, 2026u, 8u, 15u, 12u, 38u, 56u),
		 OMCI_OK);
	read_pm(fx.eng);
	uint8_t after = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &after, 1u, NULL),
		 OMCI_OK);
	CHECK_EQ(after, before);

	CHECK_EQ(send_sync(fx.eng, 0x0122u, 2026u, 8u, 15u, 12u, 49u, 56u),
		 OMCI_OK);
	read_pm(fx.eng);
	uint8_t crossed = 0u;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ETH_PM_UP, PM_INSTANCE, 1u,
				   &crossed, 1u, NULL),
		 OMCI_OK);
	CHECK_EQ(crossed, (uint8_t)(before + 1u));

	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_days_from_civil);
	RUN(test_the_clock_anchors_wall_time_to_the_monotonic_one);
	RUN(test_the_clock_refuses_impossible_stamps);

	RUN(test_sync_time_is_acknowledged);
	RUN(test_an_impossible_stamp_is_refused_and_not_applied);
	RUN(test_a_get_sees_the_interval_the_olt_is_counting);
	RUN(test_a_mib_reset_forgets_the_time);
	RUN(test_a_window_that_ends_advances_the_number_by_one);
	RUN(test_resyncing_inside_a_window_changes_nothing);

	return OMCI_T_REPORT("clock");
}
