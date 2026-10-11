// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests of the pure functions of rtl9602c-offload.c, which includes
 * this file when CONFIG_NET_DSA_RTL9602C_KUNIT_TEST is set: the NAPT hashes,
 * and the fields of the engine's table entries.
 */

#include <kunit/test.h>

/*
 * The worked example of Documentation/networking/dsa/rtl9602c-l3-nat.rst: the
 * LAN host 192.0.2.2 sends UDP from port 5000 to 203.0.113.9 port 9999, and
 * the flow leaves translated to 198.51.100.7 port 5000.
 */
#define TEST_LIP	0xc0000202	/* 192.0.2.2 */
#define TEST_RIP	0xcb007109	/* 203.0.113.9 */
#define TEST_EXTIP	0xc6336407	/* 198.51.100.7 */

static void rtl9602c_test_napt_example(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, rtl9602c_napt_bucket(TEST_LIP, 5000, TEST_RIP, 9999, false),
			477);
	KUNIT_EXPECT_EQ(test, rtl9602c_napt_in_bucket(TEST_EXTIP, 5000, false), 430);
}

/* The remote digest of the example's flow, as read in a live entry */
static void rtl9602c_test_napt_rem_hash(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, rtl9602c_napt_rem_hash(TEST_RIP, 9999), 0x9d06);
}

/*
 * Every bucket is one of the 1024 of a table, the inbound one does not depend
 * on the remote end, and the protocol is one input bit.
 */
static void rtl9602c_test_napt_buckets(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < 4096; i++) {
		u32 ip = TEST_LIP ^ (i * 0x9e3779b9);
		u16 port = 1024 + i * 13;

		KUNIT_EXPECT_LT(test, rtl9602c_napt_bucket(ip, port, TEST_RIP, 9999, false),
				1024U);
		KUNIT_EXPECT_LT(test, rtl9602c_napt_bucket(ip, port, TEST_RIP, 9999, true),
				1024U);
		KUNIT_EXPECT_LT(test, rtl9602c_napt_in_bucket(ip, port, i & 1), 1024U);
		KUNIT_EXPECT_EQ(test,
				rtl9602c_napt_bucket(ip, port, TEST_RIP, 9999, false) ^
				rtl9602c_napt_bucket(ip, port, TEST_RIP, 9999, true),
				BIT(9));
	}
}

/*
 * A field written into a zeroed entry reads back as written, wherever it
 * starts and whether or not it crosses a word, and sets no bit outside it.
 */
static void rtl9602c_test_flow_fields(struct kunit *test)
{
	unsigned int lsb, len;

	for (len = 1; len <= 64; len++) {
		for (lsb = 0; lsb + len <= 128; lsb++) {
			struct rtl9602c_field f = { .lsb = lsb, .len = len };
			u64 val = 0xa5c3f00f96695aa5ULL & GENMASK_ULL(len - 1, 0);
			u32 e[4] = { };

			rtl9602c_flow_put(e, f, val);
			KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(e, f), val);
			KUNIT_EXPECT_EQ(test, hweight32(e[0]) + hweight32(e[1]) +
					hweight32(e[2]) + hweight32(e[3]),
					hweight64(val));
		}
	}
}

static struct kunit_case rtl9602c_offload_test_cases[] = {
	KUNIT_CASE(rtl9602c_test_napt_example),
	KUNIT_CASE(rtl9602c_test_napt_rem_hash),
	KUNIT_CASE(rtl9602c_test_napt_buckets),
	KUNIT_CASE(rtl9602c_test_flow_fields),
	{ }
};

static struct kunit_suite rtl9602c_offload_test_suite = {
	.name = "rtl9602c-offload",
	.test_cases = rtl9602c_offload_test_cases,
};

kunit_test_suite(rtl9602c_offload_test_suite);
