// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests of the pure functions of rtl9602c-offload.c, which includes
 * this file when CONFIG_NET_DSA_RTL9602C_KUNIT_TEST is set: the NAPT hashes,
 * the fields of the engine's table entries, the records of the flows it
 * carries, the action of an ACL rule and the numbers of a GEM port's limit.
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

/*
 * A flow rule's cookie, which netfilter makes of a pointer: here any distinct
 * values, 128 apart as objects would be.
 */
#define TEST_COOKIE(i)	(0x10000UL + (i) * 128)

/*
 * Each of as many flows as the engine has outbound entries gets a record that
 * its cookie finds; one more is refused until one is forgotten, and the
 * teardown leaves none.
 */
static void rtl9602c_test_l34_flow_records(struct kunit *test)
{
	struct rtl9602c_l34_flow *f;
	struct rtl9602c_priv *p;
	unsigned int i;

	p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_EQ(test, rtl9602c_l34_flows_init(p), 0);

	KUNIT_EXPECT_NULL(test, rtl9602c_l34_flow_find(p, TEST_COOKIE(0)));
	for (i = 0; i < RTL9602C_NAPT_ENTRIES; i++) {
		f = rtl9602c_l34_flow_new(p, TEST_COOKIE(i));
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);
		f->napt = i;
	}
	KUNIT_EXPECT_EQ(test, p->l34_flows_n, (unsigned int)RTL9602C_NAPT_ENTRIES);
	KUNIT_EXPECT_EQ(test, PTR_ERR(rtl9602c_l34_flow_new(p, TEST_COOKIE(i))),
			-ENOSPC);

	for (i = 0; i < RTL9602C_NAPT_ENTRIES; i++) {
		f = rtl9602c_l34_flow_find(p, TEST_COOKIE(i));
		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_EXPECT_EQ(test, f->cookie, TEST_COOKIE(i));
		KUNIT_EXPECT_EQ(test, f->napt, (u16)i);
	}
	KUNIT_EXPECT_NULL(test, rtl9602c_l34_flow_find(p, TEST_COOKIE(i)));

	rtl9602c_l34_flow_forget(p, rtl9602c_l34_flow_find(p, TEST_COOKIE(7)));
	KUNIT_EXPECT_NULL(test, rtl9602c_l34_flow_find(p, TEST_COOKIE(7)));
	KUNIT_EXPECT_EQ(test, p->l34_flows_n, (unsigned int)RTL9602C_NAPT_ENTRIES - 1);
	f = rtl9602c_l34_flow_new(p, TEST_COOKIE(i));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);
	KUNIT_EXPECT_PTR_EQ(test, rtl9602c_l34_flow_find(p, TEST_COOKIE(i)), f);

	rtl9602c_l34_flows_fini(p);
	KUNIT_EXPECT_EQ(test, p->l34_flows_n, 0U);
	KUNIT_EXPECT_TRUE(test, hash_empty(p->l34_flows));
	KUNIT_EXPECT_NULL(test, p->l34_flow_cache);
}

/* The @len bits at @lsb of the action of ACL rule words @w. */
static u32 rtl9602c_test_act_get(const u32 *w, unsigned int lsb, unsigned int len)
{
	const u32 *act = &w[RTL9602C_ACL_W_ACT];
	u64 v = act[lsb / 32] | (u64)act[lsb / 32 + 1] << 32;

	return (v >> (lsb % 32)) & GENMASK_ULL(len - 1, 0);
}

/*
 * The log class of a rule is a counter, or a meter that polices; the forwarding
 * class is on only for a rule that decides forwarding; the classes it names are
 * in the word after the action, and a rule with neither log class nor
 * forwarding names none of them.
 */
static void rtl9602c_test_acl_classes(struct kunit *test)
{
	static const u8 slot_of[RTL9602C_ACL_TMPL_SLOTS] = { };
	struct rtl9602c_acl_match m = { };
	u32 w[2 * RTL9602C_ACL_RULE_WORDS + RTL9602C_ACL_ACT_WORDS + 1] = { };

	rtl9602c_acl_push(&m, RTL9602C_ACL_TMPL_FIELD_GEMIDX, 5, 0xffff);

	rtl9602c_acl_compose(w, RTL9602C_PORT_PON, 1, slot_of, &m, RTL9602C_ACL_ACT_FWD_NONE, 0, -1,
			     9);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_act_get(w, RTL9602C_ACL_ACT_LOG_LSB, 2),
			(u32)RTL9602C_ACL_ACT_LOG_METER);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_act_get(w, RTL9602C_ACL_ACT_LOG_IDX_LSB, 5), 9U);
	KUNIT_EXPECT_EQ(test, w[RTL9602C_ACL_W_EN], (u32)RTL9602C_ACL_ACT_EN_LOG);

	memset(w, 0, sizeof(w));
	rtl9602c_acl_compose(w, RTL9602C_PORT_PON, 1, slot_of, &m, RTL9602C_ACL_ACT_FWD_REDIRECT, 0,
			     3, -1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_act_get(w, RTL9602C_ACL_ACT_LOG_LSB, 2),
			(u32)RTL9602C_ACL_ACT_LOG_COUNTER);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_act_get(w, RTL9602C_ACL_ACT_LOG_IDX_LSB, 5), 3U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_act_get(w, RTL9602C_ACL_ACT_FWD_LSB, 2),
			(u32)RTL9602C_ACL_ACT_FWD_REDIRECT);
	KUNIT_EXPECT_EQ(test, w[RTL9602C_ACL_W_EN],
			(u32)(RTL9602C_ACL_ACT_EN_LOG | RTL9602C_ACL_ACT_EN_FWD));

	memset(w, 0, sizeof(w));
	rtl9602c_acl_compose(w, RTL9602C_PORT_PON, 1, slot_of, &m, RTL9602C_ACL_ACT_FWD_TRAP,
			     0, -1, -1);
	KUNIT_EXPECT_EQ(test, w[RTL9602C_ACL_W_EN], (u32)RTL9602C_ACL_ACT_EN_FWD);
}

/* The units of a byte-mode meter round to the nearest, and stay in the field. */
static void rtl9602c_test_gem_ds_units(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_units(0), 1U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_units(1), 1U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_units(1499), 1U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_units(1500), 2U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_units(12500000), 12500U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_units(524287000),
			(u32)RTL9602C_METER_CFG_HI_RATE_MAX);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_units(U64_MAX), (u32)RTL9602C_METER_CFG_HI_RATE_MAX);
}

/* The bucket is the default for 0, and otherwise between what passes a frame and the field. */
static void rtl9602c_test_gem_ds_bucket(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_bucket(0), (u32)RTL9602C_METER_CFG_LO_BURST_DEFAULT);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_bucket(1), (u32)RTL9602C_GEM_DS_BURST_MIN);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_bucket(2048), 2048U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_bucket(16000), 16000U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_bucket(65535), 65535U);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_bucket(70000), (u32)RTL9602C_GEM_DS_BURST_MAX);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_ds_bucket(U32_MAX), (u32)RTL9602C_GEM_DS_BURST_MAX);
}

static struct kunit_case rtl9602c_offload_test_cases[] = {
	KUNIT_CASE(rtl9602c_test_napt_example),
	KUNIT_CASE(rtl9602c_test_napt_rem_hash),
	KUNIT_CASE(rtl9602c_test_napt_buckets),
	KUNIT_CASE(rtl9602c_test_flow_fields),
	KUNIT_CASE(rtl9602c_test_l34_flow_records),
	KUNIT_CASE(rtl9602c_test_acl_classes),
	KUNIT_CASE(rtl9602c_test_gem_ds_units),
	KUNIT_CASE(rtl9602c_test_gem_ds_bucket),
	{ }
};

static struct kunit_suite rtl9602c_offload_test_suite = {
	.name = "rtl9602c-offload",
	.test_cases = rtl9602c_offload_test_cases,
};

kunit_test_suite(rtl9602c_offload_test_suite);
