// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests of the pure functions of rtl9602c-offload.c, which includes
 * this file when CONFIG_NET_DSA_RTL9602C_KUNIT_TEST is set: the NAPT hashes,
 * the fields of the engine's table entries, the records of the flows it
 * carries, the action of an ACL rule, the numbers of a GEM port's limit, and
 * the classifier entries of the VLAN rules.
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

/*
 * The classifier's bookkeeping, on a switch with the 128 usable entries of
 * this board, no claimed VLAN and no port stream.
 */
static struct rtl9602c_priv *rtl9602c_test_cf_priv(struct kunit *test)
{
	struct rtl9602c_priv *p;
	unsigned int i;

	p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->gem_vid_flow = kunit_kcalloc(test, VLAN_N_VID,
					sizeof(*p->gem_vid_flow), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p->gem_vid_flow);
	p->cf_entries = 128;
	for (i = 0; i < RTL9602C_NUM_PORTS; i++) {
		p->cf_sid[i] = -1;
		p->cf_sid_flow[i] = -1;
	}
	return p;
}

/*
 * VLAN 3001 as a mapper divides it: flow 3 takes priorities 0 to 3, flow 5
 * priorities 4 and 5, and none takes 6 and 7.
 */
static void rtl9602c_test_cf_divide(struct rtl9602c_priv *p, u16 vid)
{
	static const u8 row[8] = { 4, 4, 4, 4, 6, 6, 0, 0 };

	memcpy(p->gem_vid_flow[vid], row, sizeof(row));
}

/* One flow claims every priority of @vid. */
static void rtl9602c_test_cf_whole(struct rtl9602c_priv *p, u16 vid, u8 flow)
{
	memset(p->gem_vid_flow[vid], flow + 1, 8);
}

/*
 * A translation on port @port of C-VID @match into @out that keeps the
 * priority, as rtl9602c_flower_vlan() composes it from
 * "vlan_id @match action vlan modify id @out".
 */
static struct rtl9602c_cf_xlate rtl9602c_test_cf_rule(int port, u16 match,
						      u16 out)
{
	return (struct rtl9602c_cf_xlate) {
		.value	 = FIELD_PREP(RTL9602C_CLS_RULE_UNI, port) |
			   RTL9602C_CLS_RULE_CTAG |
			   FIELD_PREP(RTL9602C_CLS_RULE_VID, match),
		.care	 = RTL9602C_CLS_RULE_UNI | RTL9602C_CLS_RULE_CTAG |
			   RTL9602C_CLS_RULE_STAG | RTL9602C_CLS_RULE_VID,
		.act	 = { 0, RTL9602C_CLS_ACT_CVID_SET |
			     FIELD_PREP(RTL9602C_CLS_ACT_CVID, out) |
			     FIELD_PREP(RTL9602C_CLS_ACT_CTAG,
					RTL9602C_CLS_ACT_CTAG_TAG), 0 },
		.out_vid = out,
		.port	 = port,
		.tmpl	 = RTL9602C_CLS_TMPL_C,
		.pcp	 = -1,
	};
}

/* The entries of the VLAN rule of @cookie on @port. */
static unsigned int rtl9602c_test_cf_count(struct rtl9602c_priv *p,
					   unsigned long cookie, int port)
{
	unsigned int idx, n = 0;

	for (idx = 0; idx < RTL9602C_CLS_ENTRIES; idx++)
		n += rtl9602c_cf_xlate_of(p, idx, cookie, port);
	return n;
}

/* The stream entry @idx composes, -1 for none. */
static int rtl9602c_test_cf_flow(struct rtl9602c_priv *p, unsigned int idx)
{
	struct rtl9602c_cf_entry e;

	rtl9602c_cf_xlate_entry(p, idx, &e);
	if (!(e.act[0] & RTL9602C_CLS_ACT_FLOW_SET))
		return -1;
	return FIELD_GET(RTL9602C_CLS_ACT_FLOW, e.act[0]);
}

/*
 * A translation that keeps the priority, into a VLAN whose priorities GEM
 * ports divide, takes eight entries: each matches what the rule matches and
 * one priority, keeps the rule's action, and gives the flow of its priority,
 * or no stream for a priority no flow claims.
 */
static void rtl9602c_test_cf_split_entries(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_xlate x = rtl9602c_test_cf_rule(0, 410, 3001);
	static const int flow[8] = { 3, 3, 3, 3, 5, 5, -1, -1 };
	DECLARE_BITMAP(stale, RTL9602C_CLS_ENTRIES);
	struct rtl9602c_cf_entry e;
	unsigned int idx;

	bitmap_zero(stale, RTL9602C_CLS_ENTRIES);
	rtl9602c_test_cf_divide(p, 3001);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_split(p, &x));
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       stale), 8);

	for (idx = 0; idx < 8; idx++) {
		KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, idx,
							     TEST_COOKIE(1), 0));
		KUNIT_EXPECT_EQ(test, p->cf_xlate[idx].pcp, (s8)idx);
		rtl9602c_cf_xlate_entry(p, idx, &e);
		KUNIT_EXPECT_FALSE(test, e.downstream);
		KUNIT_EXPECT_EQ(test, e.tmpl, (u8)RTL9602C_CLS_TMPL_C);
		KUNIT_EXPECT_EQ(test, e.care, x.care | RTL9602C_CLS_RULE_PRI);
		KUNIT_EXPECT_EQ(test, e.value,
				x.value | FIELD_PREP(RTL9602C_CLS_RULE_PRI, idx));
		KUNIT_EXPECT_EQ(test, e.act[1], x.act[1]);
		KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, idx), flow[idx]);
	}
	KUNIT_EXPECT_FALSE(test, test_bit(8, p->cf_used));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(stale, RTL9602C_CLS_ENTRIES));

	/* An S-tag rewritten on the S-tag's own template: its priorities. */
	x = rtl9602c_test_cf_rule(0, 200, 3001);
	x.value = (x.value & ~RTL9602C_CLS_RULE_CTAG) | RTL9602C_CLS_RULE_STAG;
	x.care &= ~RTL9602C_CLS_RULE_CTAG;
	x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG, RTL9602C_CLS_ACT_STAG_ADD) |
		   RTL9602C_CLS_ACT_SVID_SET |
		   FIELD_PREP(RTL9602C_CLS_ACT_SVID, 3001);
	x.act[1] = 0;
	x.tmpl = RTL9602C_CLS_TMPL_S;
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(2),
						       stale), 8);
	for (idx = 8; idx < 16; idx++) {
		rtl9602c_cf_xlate_entry(p, idx, &e);
		KUNIT_EXPECT_EQ(test, e.tmpl, (u8)RTL9602C_CLS_TMPL_S);
		KUNIT_EXPECT_EQ(test, FIELD_GET(RTL9602C_CLS_RULE_PRI, e.value),
				idx - 8);
		KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, idx),
				flow[idx - 8]);
	}
}

/*
 * The rules that stay one entry: one that matches a priority, one that
 * writes one, a rewrite of an S-tag matched on the C-tag behind it (which
 * takes priority 0's flow), a push onto an untagged frame, a downstream
 * rule, and a rule into a VLAN one flow claims whole or none claims.
 */
static void rtl9602c_test_cf_split_which(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	DECLARE_BITMAP(stale, RTL9602C_CLS_ENTRIES);
	struct rtl9602c_cf_xlate x;
	unsigned long c = 0;

	bitmap_zero(stale, RTL9602C_CLS_ENTRIES);
	rtl9602c_test_cf_divide(p, 3001);
	rtl9602c_test_cf_whole(p, 3002, 7);
	p->cf_sid_flow[0] = 9;

	x = rtl9602c_test_cf_rule(0, 410, 3001);
	x.value |= FIELD_PREP(RTL9602C_CLS_RULE_PRI, 5);
	x.care |= RTL9602C_CLS_RULE_PRI;
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       stale), 1);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[0].pcp, (s8)-1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 0), 5);

	x = rtl9602c_test_cf_rule(0, 411, 3001);
	x.act[1] |= RTL9602C_CLS_ACT_CPRI_SET |
		    FIELD_PREP(RTL9602C_CLS_ACT_CPRI, 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       stale), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 1), 3);

	x = rtl9602c_test_cf_rule(0, 412, 3001);
	x.value |= RTL9602C_CLS_RULE_STAG;
	x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG, RTL9602C_CLS_ACT_STAG_ADD) |
		   RTL9602C_CLS_ACT_SVID_SET |
		   FIELD_PREP(RTL9602C_CLS_ACT_SVID, 3001);
	x.act[1] = 0;
	KUNIT_EXPECT_FALSE(test, rtl9602c_cf_xlate_split(p, &x));
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       stale), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 2), 3);

	x = rtl9602c_test_cf_rule(0, 0, 3001);
	x.value &= ~(RTL9602C_CLS_RULE_CTAG | RTL9602C_CLS_RULE_VID);
	x.care &= ~RTL9602C_CLS_RULE_VID;
	x.act[1] |= RTL9602C_CLS_ACT_CPRI_SET |
		    FIELD_PREP(RTL9602C_CLS_ACT_CPRI, 6);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       stale), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 3), -1);

	x = rtl9602c_test_cf_rule(RTL9602C_PORT_PON, 3001, 410);
	x.value &= ~RTL9602C_CLS_RULE_UNI;
	x.care &= ~RTL9602C_CLS_RULE_UNI;
	rtl9602c_test_cf_divide(p, 410);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       stale), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 4), -1);

	x = rtl9602c_test_cf_rule(0, 413, 3002);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       stale), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 5), 7);

	x = rtl9602c_test_cf_rule(0, 414, 3003);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       stale), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 6), 9);

	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			7U);
}

/*
 * A change of the claims lays the rules out again: a rule into a VLAN that
 * comes to be divided takes eight entries, below the stream entries, and gives
 * them up again when one flow takes the VLAN whole; another rule can take the
 * entries given up at once, and they are then not retired. The rules on
 * other ports, or of the other direction, are left as they are.
 */
static void rtl9602c_test_cf_relayout(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	DECLARE_BITMAP(stale, RTL9602C_CLS_ENTRIES);
	struct rtl9602c_cf_xlate x;
	unsigned int idx, h;

	bitmap_zero(stale, RTL9602C_CLS_ENTRIES);
	rtl9602c_test_cf_whole(p, 3001, 3);
	set_bit(127, p->cf_used);
	set_bit(127, p->cf_sid_owned);
	set_bit(126, p->cf_used);
	set_bit(126, p->cf_sid_owned);

	x = rtl9602c_test_cf_rule(0, 410, 3001);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       stale), 1);
	x = rtl9602c_test_cf_rule(0, 420, 3002);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(2),
						       stale), 1);
	x = rtl9602c_test_cf_rule(RTL9602C_PORT_PON, 3001, 410);
	x.value &= ~RTL9602C_CLS_RULE_UNI;
	x.care &= ~RTL9602C_CLS_RULE_UNI;
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(3),
						       stale), 1);

	/* 3001 divided: the first rule takes 3 to 9 besides 0. */
	rtl9602c_test_cf_divide(p, 3001);
	rtl9602c_cf_xlate_relayout(p, stale);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(1), 0), 8U);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[0].pcp, (s8)0);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 0), 3);
	for (idx = 3; idx < 10; idx++) {
		KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, idx,
							     TEST_COOKIE(1), 0));
		KUNIT_EXPECT_EQ(test, p->cf_xlate[idx].pcp, (s8)(idx - 2));
	}
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 7), 5);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 8), -1);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[1].pcp, (s8)-1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(2), 0), 1U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p,
						     TEST_COOKIE(3),
						     RTL9602C_PORT_PON), 1U);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[2].pcp, (s8)-1);
	KUNIT_EXPECT_TRUE(test, test_bit(126, p->cf_sid_owned));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(stale, RTL9602C_CLS_ENTRIES));

	/* Laid out again with nothing changed, nothing moves. */
	rtl9602c_cf_xlate_relayout(p, stale);
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			12U);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(stale, RTL9602C_CLS_ENTRIES));

	/*
	 * 3001 whole again and 3002 divided in one change: the first rule
	 * keeps entry 0 for every priority, and the second takes the seven
	 * entries the first gave up, which are then not retired.
	 */
	rtl9602c_test_cf_whole(p, 3001, 3);
	rtl9602c_test_cf_divide(p, 3002);
	rtl9602c_cf_xlate_relayout(p, stale);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(1), 0), 1U);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[0].pcp, (s8)-1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 0), 3);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(2), 0), 8U);
	for (idx = 3; idx < 10; idx++)
		KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, idx,
							     TEST_COOKIE(2), 0));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(stale, RTL9602C_CLS_ENTRIES));

	/* 3002 whole: the seven are given up, to be retired. */
	rtl9602c_test_cf_whole(p, 3002, 4);
	rtl9602c_cf_xlate_relayout(p, stale);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(2), 0), 1U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 1), 4);
	KUNIT_EXPECT_EQ(test, bitmap_weight(stale, RTL9602C_CLS_ENTRIES), 7U);
	for (idx = 3; idx < 10; idx++) {
		KUNIT_EXPECT_TRUE(test, test_bit(idx, stale));
		KUNIT_EXPECT_FALSE(test, test_bit(idx, p->cf_used));
	}

	/*
	 * With the first rule gone, the second rule's entry 1 lies above a free
	 * one: the priorities still go in index order, entry 0 is taken out of
	 * the entries to retire, and a layout again moves nothing.
	 */
	bitmap_zero(stale, RTL9602C_CLS_ENTRIES);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_forget(p, TEST_COOKIE(1), 0,
						       stale), 1U);
	rtl9602c_test_cf_divide(p, 3002);
	for (h = 0; h < 2; h++) {
		rtl9602c_cf_xlate_relayout(p, stale);
		KUNIT_EXPECT_EQ(test, p->cf_xlate[0].pcp, (s8)0);
		KUNIT_EXPECT_EQ(test, p->cf_xlate[1].pcp, (s8)1);
		for (idx = 3; idx < 9; idx++)
			KUNIT_EXPECT_EQ(test, p->cf_xlate[idx].pcp,
					(s8)(idx - 1));
		KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(2),
							     0), 8U);
		KUNIT_EXPECT_TRUE(test, bitmap_empty(stale,
						     RTL9602C_CLS_ENTRIES));
	}
}

/*
 * The classifier's depth: a new rule that needs eight entries where fewer are
 * free is refused, and so is a rule of one where none is; a rule with an entry
 * already keeps one when its VLAN comes to be divided and there is no room, and
 * takes eight once there is. The entries are never taken above a stream entry.
 */
static void rtl9602c_test_cf_depth(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	DECLARE_BITMAP(stale, RTL9602C_CLS_ENTRIES);
	struct rtl9602c_cf_xlate x;
	unsigned int idx;

	bitmap_zero(stale, RTL9602C_CLS_ENTRIES);
	for (idx = 6; idx < p->cf_entries; idx++) {
		set_bit(idx, p->cf_used);
		set_bit(idx, p->cf_sid_owned);
	}

	rtl9602c_test_cf_divide(p, 3001);
	x = rtl9602c_test_cf_rule(0, 410, 3001);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       stale), -ENOSPC);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(1), 0), 0U);
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			122U);

	/* Into 3002, whole: one entry, 0, and 1 to 5 left free. */
	rtl9602c_test_cf_whole(p, 3002, 3);
	x = rtl9602c_test_cf_rule(0, 420, 3002);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(2),
						       stale), 1);

	/* 3002 divided with five free: the rule keeps one entry. */
	rtl9602c_test_cf_divide(p, 3002);
	rtl9602c_cf_xlate_relayout(p, stale);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(2), 0), 1U);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[0].pcp, (s8)-1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 0), 3);

	/* Two stream entries fewer, seven free: it takes 1 to 7. */
	for (idx = 6; idx < 8; idx++) {
		clear_bit(idx, p->cf_used);
		clear_bit(idx, p->cf_sid_owned);
	}
	rtl9602c_cf_xlate_relayout(p, stale);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(2), 0), 8U);
	for (idx = 0; idx < 8; idx++)
		KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, idx,
							     TEST_COOKIE(2), 0));
	KUNIT_EXPECT_EQ(test, find_first_zero_bit(p->cf_used, p->cf_entries),
			(unsigned long)p->cf_entries);

	/* Full: a rule of one entry is refused too. */
	x = rtl9602c_test_cf_rule(0, 430, 3003);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(3),
						       stale), -ENOSPC);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(stale, RTL9602C_CLS_ENTRIES));
}

/*
 * A rule of a block that several ports share comes with one cookie for each
 * port: removing it from one port leaves the other port's entries.
 */
static void rtl9602c_test_cf_forget(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	DECLARE_BITMAP(stale, RTL9602C_CLS_ENTRIES);
	struct rtl9602c_cf_xlate x;

	bitmap_zero(stale, RTL9602C_CLS_ENTRIES);
	rtl9602c_test_cf_divide(p, 3001);
	x = rtl9602c_test_cf_rule(0, 410, 3001);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       stale), 8);
	x = rtl9602c_test_cf_rule(1, 410, 3001);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       stale), 8);

	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_forget(p, TEST_COOKIE(1), 0,
						       stale), 8U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(1), 1), 8U);
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			8U);
	KUNIT_EXPECT_EQ(test, bitmap_weight(stale, RTL9602C_CLS_ENTRIES), 8U);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_forget(p, TEST_COOKIE(1), 0,
						       stale), 0U);
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
	KUNIT_CASE(rtl9602c_test_cf_split_entries),
	KUNIT_CASE(rtl9602c_test_cf_split_which),
	KUNIT_CASE(rtl9602c_test_cf_relayout),
	KUNIT_CASE(rtl9602c_test_cf_depth),
	KUNIT_CASE(rtl9602c_test_cf_forget),
	{ }
};

static struct kunit_suite rtl9602c_offload_test_suite = {
	.name = "rtl9602c-offload",
	.test_cases = rtl9602c_offload_test_cases,
};

kunit_test_suite(rtl9602c_offload_test_suite);
