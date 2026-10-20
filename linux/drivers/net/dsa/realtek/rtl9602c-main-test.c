// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests of the pure functions of rtl9602c-main.c, which includes this
 * file when CONFIG_NET_DSA_RTL9602C_KUNIT_TEST is set: the checks of the
 * search for the host's setting of the SerDes lane.
 */

#include <kunit/test.h>

/* The entry of rtl9602c_sds_tries[] that sfp_lane names @param. */
static unsigned int rtl9602c_test_sds(struct kunit *test, const char *param)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rtl9602c_sds_tries); i++)
		if (!strcmp(rtl9602c_sds_tries[i].param, param))
			return i;
	KUNIT_FAIL(test, "no lane setting is named %s", param);
	return 0;
}

/*
 * "auto" tries 2500BaseX, 1000BaseX with in-band negotiation and 1000BaseX
 * without, in that order and over again, one at each check, and never
 * 2500BaseX with negotiation. 1000BaseX without negotiation comes straight
 * after the setting with it, in the same mode: the bring-up that has to be
 * re-latched before its negotiation word is in force (rtl9602c_sds_set()).
 */
static void rtl9602c_test_sds_auto(struct kunit *test)
{
	static const char *const order[] = {
		"2500basex", "1000basex-an", "1000basex",
		"2500basex", "1000basex-an", "1000basex",
	};
	unsigned int cur = rtl9602c_test_sds(test, order[0]);
	unsigned int i, next;

	KUNIT_EXPECT_EQ(test, hweight8(RTL9602C_SDS_AUTO), 3);
	KUNIT_EXPECT_FALSE(test, RTL9602C_SDS_AUTO &
			   BIT(rtl9602c_test_sds(test, "2500basex-an")));

	for (i = 1; i < ARRAY_SIZE(order); i++) {
		next = ~0u;
		KUNIT_ASSERT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur,
							false, false,
							rtl9602c_sds_tries[cur].checks - 1,
							&next),
				RTL9602C_SDS_NEXT);
		KUNIT_ASSERT_LT(test, next, ARRAY_SIZE(rtl9602c_sds_tries));
		KUNIT_EXPECT_STREQ(test, rtl9602c_sds_tries[next].param, order[i]);
		cur = next;
	}

	i = rtl9602c_test_sds(test, "1000basex");
	KUNIT_EXPECT_EQ(test, rtl9602c_sds_tries[i].mode,
			rtl9602c_sds_tries[rtl9602c_test_sds(test, "1000basex-an")].mode);
}

/*
 * A lane whose receiver synchronises is found once and then kept; one that
 * loses the host waits a check in its setting, and goes on once that setting
 * has had its checks.
 */
static void rtl9602c_test_sds_found_lost(struct kunit *test)
{
	unsigned int cur = rtl9602c_test_sds(test, "1000basex");
	unsigned int next = ~0u;

	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, true,
						false, 0, &next),
			RTL9602C_SDS_FOUND);
	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, true,
						true, 0, &next),
			RTL9602C_SDS_KEEP);
	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, false,
						true, 0, &next),
			RTL9602C_SDS_LOST);
	KUNIT_EXPECT_EQ(test, next, ~0u);

	KUNIT_ASSERT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, false,
						false,
						rtl9602c_sds_tries[cur].checks - 1,
						&next),
			RTL9602C_SDS_NEXT);
	KUNIT_ASSERT_LT(test, next, ARRAY_SIZE(rtl9602c_sds_tries));
	KUNIT_EXPECT_STREQ(test, rtl9602c_sds_tries[next].param, "2500basex");
}

/*
 * With sfp_lane naming one setting, the search never brings the lane up
 * again, whether or not it synchronises. With none allowed, which no set
 * leaves, a check ends all the same.
 */
static void rtl9602c_test_sds_one(struct kunit *test)
{
	unsigned int i, next;

	for (i = 0; i < ARRAY_SIZE(rtl9602c_sds_tries); i++) {
		next = ~0u;
		KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(BIT(i), i, false, false,
							rtl9602c_sds_tries[i].checks - 1,
							&next),
				RTL9602C_SDS_KEEP);
		KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(BIT(i), i, true, true, 0,
							&next),
				RTL9602C_SDS_KEEP);
		KUNIT_EXPECT_EQ(test, next, ~0u);
	}

	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(0, 0, false, false, 0, &next),
			RTL9602C_SDS_KEEP);
}

/*
 * 1000BaseX without negotiation is given twelve checks, longer than the 8.7 s
 * it took to synchronise with a host that does not negotiate, before the
 * search goes on; every other setting is given one.
 */
static void rtl9602c_test_sds_wait(struct kunit *test)
{
	unsigned int slow = rtl9602c_test_sds(test, "1000basex");
	unsigned int i, w, next;

	KUNIT_EXPECT_GE(test, rtl9602c_sds_tries[slow].checks * RTL9602C_SDS_TRY_MS,
			9000);
	for (w = 0; w + 1 < rtl9602c_sds_tries[slow].checks; w++)
		KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, slow,
							false, false, w, &next),
				RTL9602C_SDS_WAIT);
	next = ~0u;
	KUNIT_ASSERT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, slow, false,
						false, w, &next),
			RTL9602C_SDS_NEXT);
	KUNIT_EXPECT_STREQ(test, rtl9602c_sds_tries[next].param, "2500basex");

	for (i = 0; i < ARRAY_SIZE(rtl9602c_sds_tries); i++)
		if (i != slow)
			KUNIT_EXPECT_EQ(test, rtl9602c_sds_tries[i].checks, 1);
}

static struct kunit_case rtl9602c_main_test_cases[] = {
	KUNIT_CASE(rtl9602c_test_sds_auto),
	KUNIT_CASE(rtl9602c_test_sds_found_lost),
	KUNIT_CASE(rtl9602c_test_sds_one),
	KUNIT_CASE(rtl9602c_test_sds_wait),
	{ }
};

static struct kunit_suite rtl9602c_main_test_suite = {
	.name = "rtl9602c-main",
	.test_cases = rtl9602c_main_test_cases,
};

kunit_test_suite(rtl9602c_main_test_suite);
