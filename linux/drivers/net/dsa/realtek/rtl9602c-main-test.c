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
							false, false, &next),
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
 * loses the host waits a check in its setting, and only at the next goes on.
 */
static void rtl9602c_test_sds_found_lost(struct kunit *test)
{
	unsigned int cur = rtl9602c_test_sds(test, "1000basex");
	unsigned int next = ~0u;

	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, true,
						false, &next),
			RTL9602C_SDS_FOUND);
	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, true,
						true, &next),
			RTL9602C_SDS_KEEP);
	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, false,
						true, &next),
			RTL9602C_SDS_LOST);
	KUNIT_EXPECT_EQ(test, next, ~0u);

	KUNIT_ASSERT_EQ(test, rtl9602c_sds_step(RTL9602C_SDS_AUTO, cur, false,
						false, &next),
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
							&next),
				RTL9602C_SDS_KEEP);
		KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(BIT(i), i, true, true,
							&next),
				RTL9602C_SDS_KEEP);
		KUNIT_EXPECT_EQ(test, next, ~0u);
	}

	KUNIT_EXPECT_EQ(test, rtl9602c_sds_step(0, 0, false, false, &next),
			RTL9602C_SDS_KEEP);
}

static struct kunit_case rtl9602c_main_test_cases[] = {
	KUNIT_CASE(rtl9602c_test_sds_auto),
	KUNIT_CASE(rtl9602c_test_sds_found_lost),
	KUNIT_CASE(rtl9602c_test_sds_one),
	{ }
};

static struct kunit_suite rtl9602c_main_test_suite = {
	.name = "rtl9602c-main",
	.test_cases = rtl9602c_main_test_cases,
};

kunit_test_suite(rtl9602c_main_test_suite);
