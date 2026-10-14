// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests of the pure functions of rtl9602c-ptp.c, which includes this
 * file when CONFIG_NET_DSA_RTL9602C_KUNIT_TEST is set: which of the OLT's
 * seconds are reported.
 */

#include <kunit/test.h>

#define TEST_SECOND	RTL9602C_PTP_PON_SECOND_FRAMES

/* Seconds 8000 frames apart are reported, also across one not seen. */
static void rtl9602c_test_pon_on_time(struct kunit *test)
{
	struct rtl9602c_ptp_pon_grid g = { };

	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001 + TEST_SECOND));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001 + 3 * TEST_SECOND));
	KUNIT_EXPECT_EQ(test, g.frame, 1001U + 3 * TEST_SECOND);
}

/* An interrupt a frame late is not reported, and the next second is. */
static void rtl9602c_test_pon_late(struct kunit *test)
{
	struct rtl9602c_ptp_pon_grid g = { };

	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001));
	KUNIT_EXPECT_FALSE(test, rtl9602c_ptp_pon_take(&g, 1002 + TEST_SECOND));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001 + 2 * TEST_SECOND));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001 + 3 * TEST_SECOND));
}

/*
 * The OLT started again and its counter with it: the first second after is
 * not reported, and the next, which agrees with it, starts the count.
 */
static void rtl9602c_test_pon_restart(struct kunit *test)
{
	struct rtl9602c_ptp_pon_grid g = { };

	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 14138461));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 14138461 + TEST_SECOND));
	KUNIT_EXPECT_FALSE(test, rtl9602c_ptp_pon_take(&g, 7042158));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 7042158 + TEST_SECOND));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 7042158 + 2 * TEST_SECOND));
	KUNIT_EXPECT_EQ(test, g.frame, 7042158U + 2 * TEST_SECOND);
}

/* The counter's 30 bits wrap, and the seconds go on. */
static void rtl9602c_test_pon_wrap(struct kunit *test)
{
	u32 last = RTL9602C_GTC_DS_SUPERFRAME_CNT - 99;
	struct rtl9602c_ptp_pon_grid g = { };

	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, last));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, TEST_SECOND - 100));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 2 * TEST_SECOND - 100));
}

/*
 * Two interrupts late by the same frame in a row look like a counter that
 * started again: the second is reported a frame late, and the count is back
 * on time a second after the next.
 */
static void rtl9602c_test_pon_late_twice(struct kunit *test)
{
	struct rtl9602c_ptp_pon_grid g = { };

	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001));
	KUNIT_EXPECT_FALSE(test, rtl9602c_ptp_pon_take(&g, 1002 + TEST_SECOND));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1002 + 2 * TEST_SECOND));
	KUNIT_EXPECT_FALSE(test, rtl9602c_ptp_pon_take(&g, 1001 + 3 * TEST_SECOND));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001 + 4 * TEST_SECOND));
	KUNIT_EXPECT_TRUE(test, rtl9602c_ptp_pon_take(&g, 1001 + 5 * TEST_SECOND));
}

static struct kunit_case rtl9602c_ptp_test_cases[] = {
	KUNIT_CASE(rtl9602c_test_pon_on_time),
	KUNIT_CASE(rtl9602c_test_pon_late),
	KUNIT_CASE(rtl9602c_test_pon_restart),
	KUNIT_CASE(rtl9602c_test_pon_wrap),
	KUNIT_CASE(rtl9602c_test_pon_late_twice),
	{ }
};

static struct kunit_suite rtl9602c_ptp_test_suite = {
	.name = "rtl9602c-ptp",
	.test_cases = rtl9602c_ptp_test_cases,
};

kunit_test_suite(rtl9602c_ptp_test_suite);
