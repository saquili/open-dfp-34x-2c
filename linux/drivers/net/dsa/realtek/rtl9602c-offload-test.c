// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests of the pure functions of rtl9602c-offload.c, which includes
 * this file when CONFIG_NET_DSA_RTL9602C_KUNIT_TEST is set: the NAPT hashes,
 * the fields of the engine's table entries, the records of the flows it
 * carries, the action of an ACL rule, the numbers of a GEM port's limit, and
 * the tag operations and the classifier entries of the VLAN rules.
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

#if IS_ENABLED(CONFIG_IPV6)
/*
 * A local network, 2001:db8:1:1::/64 (RFC 3849), on L3-interface row 4, and a
 * host on it, whose interface identifier's octets fold to 0x12 and then to 3.
 */
#define TEST_NB6_HOST	"2001:db8:1:1:211:22ff:fe33:4455"
#define TEST_NB6_NETIF	4

static struct in6_addr rtl9602c_test_in6(struct kunit *test, const char *s)
{
	struct in6_addr a;

	KUNIT_ASSERT_EQ(test, in6_pton(s, -1, a.s6_addr, -1, NULL), 1);
	return a;
}

/*
 * The neighbour row: the interface identifier across words 0 and 1, its
 * first half in word 1, and the route row, the address-table row and the
 * valid bit in word 2.
 */
static void rtl9602c_test_nb6_entry(struct kunit *test)
{
	struct in6_addr a = rtl9602c_test_in6(test, TEST_NB6_HOST);
	u32 e[3] = { };

	rtl9602c_l34_nb6_encode(e, &a, 2, 132);
	KUNIT_EXPECT_EQ(test, e[0], 0xfe334455U);
	KUNIT_EXPECT_EQ(test, e[1], 0x021122ffU);
	KUNIT_EXPECT_EQ(test, e[2], 0x00008422U);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(e, RTL9602C_NEIGH6_IFID),
			0x021122fffe334455ULL);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(e, RTL9602C_NEIGH6_RT_IDX), 2ULL);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(e, RTL9602C_NEIGH6_L2_IDX), 132ULL);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(e, RTL9602C_NEIGH6_VALID), 1ULL);

	/* The largest values stay in their fields, and bit 78 stays clear. */
	memset(e, 0, sizeof(e));
	rtl9602c_l34_nb6_encode(e, &a, 7, 2047);
	KUNIT_EXPECT_EQ(test, e[2], 0x0000bfffU);
}

/*
 * The bucket: the identifier's octets folded to one, its nibbles to four bits,
 * and the route row mixed in; the two forms differ for rows 4 and 5 only, and
 * neither depends on the prefix or on the order of the octets.
 */
static void rtl9602c_test_nb6_bucket(struct kunit *test)
{
	static const u8 one[8] = { 5, 3, 1, 1, 1, 1, 1, 1 };
	struct in6_addr a = rtl9602c_test_in6(test, TEST_NB6_HOST);
	struct in6_addr b = rtl9602c_test_in6(test, "2001:db8:ffff:9:5544:33fe:ff22:1102");
	struct in6_addr c = { };
	unsigned int rt;

	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 0, false), 3U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 1, false), 10U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 2, false), 5U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 3, false), 12U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 4, false), 7U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 4, true), 3U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 5, false), 14U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 5, true), 10U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 6, false), 5U);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, 7, false), 12U);

	for (rt = 0; rt < 8; rt++) {
		KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&a, rt, false) ==
				rtl9602c_l34_nb6_bucket(&a, rt, true), rt != 4 && rt != 5);
		KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&b, rt, false),
				rtl9602c_l34_nb6_bucket(&a, rt, false));
		KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_bucket(&b, rt, true),
				rtl9602c_l34_nb6_bucket(&a, rt, true));
	}

	/* Every bucket is one of the sixteen. */
	for (rt = 0; rt < 8 * 256; rt++) {
		memcpy(&c.s6_addr[8], one, sizeof(one));
		c.s6_addr[15] = rt / 8;
		KUNIT_EXPECT_LT(test, rtl9602c_l34_nb6_bucket(&c, rt % 8, false), 16U);
		KUNIT_EXPECT_LT(test, rtl9602c_l34_nb6_bucket(&c, rt % 8, true), 16U);
	}
}

/*
 * The route rows of a stick whose IPv6 default route leaves by PPPoE WAN 3,
 * with the local network of TEST_NB6_NETIF in row 3 and another, on
 * L3-interface row 5, in row 4, as rtl9602c_l34_route6_build() lays them out.
 */
static void rtl9602c_test_nb6_rows(struct kunit *test, u32 (*rows)[5])
{
	struct in6_addr d;

	memset(rows, 0, 8 * sizeof(rows[0]));
	d = rtl9602c_test_in6(test, "fe80::");
	rtl9602c_l34_route6_encode(rows[0], &d, 10, 0, 0, false);
	d = rtl9602c_test_in6(test, "ff00::");
	rtl9602c_l34_route6_encode(rows[1], &d, 8, 0, 0, false);
	d = rtl9602c_test_in6(test, "2001:db8:1:1::1");
	rtl9602c_l34_route6_encode(rows[2], &d, 128, 0, 0, false);
	d = rtl9602c_test_in6(test, "2001:db8:1:1::");
	rtl9602c_l34_route6_encode(rows[3], &d, 64, RTL9602C_ROUTE_ACTION_NEIGH,
				   TEST_NB6_NETIF, false);
	d = rtl9602c_test_in6(test, "2001:db8:1:2::");
	rtl9602c_l34_route6_encode(rows[4], &d, 64, RTL9602C_ROUTE_ACTION_NEIGH, 5,
				   false);
	rtl9602c_l34_route6_encode(rows[7], &in6addr_any, 0,
				   RTL9602C_ROUTE_ACTION_NEXTHOP, 3, true);
}

/*
 * A neighbour hangs on the on-link row of its own interface that a frame to
 * it matches first, and on no other: not on this device's own address, not on
 * another network's row, not on the default route.
 */
static void rtl9602c_test_nb6_route(struct kunit *test)
{
	struct in6_addr a;
	u32 rows[8][5];

	rtl9602c_test_nb6_rows(test, rows);

	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(rows[3], RTL9602C_ROUTE6_ACTION),
			(u64)RTL9602C_ROUTE_ACTION_NEIGH);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(rows[3], RTL9602C_ROUTE6_L3IF_IDX),
			(u64)TEST_NB6_NETIF);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(rows[3], RTL9602C_ROUTE6_RT2WAN), 0ULL);
	KUNIT_EXPECT_EQ(test, rows[3][0], 0U);
	KUNIT_EXPECT_EQ(test, rows[3][1], 0U);
	KUNIT_EXPECT_EQ(test, rows[3][2], 0x00010001U);
	KUNIT_EXPECT_EQ(test, rows[3][3], 0x20010db8U);
	KUNIT_EXPECT_EQ(test, rows[3][4], 0x00006440U);

	a = rtl9602c_test_in6(test, TEST_NB6_HOST);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_route(rows, 8, &a, TEST_NB6_NETIF), 3);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_route(rows, 8, &a, 5), -1);
	a = rtl9602c_test_in6(test, "2001:db8:1:1::1");
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_route(rows, 8, &a, TEST_NB6_NETIF), -1);
	a = rtl9602c_test_in6(test, "2001:db8:1:2::9");
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_route(rows, 8, &a, 5), 4);
	a = rtl9602c_test_in6(test, "2001:db8:9::1");
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_route(rows, 8, &a, TEST_NB6_NETIF), -1);
	a = rtl9602c_test_in6(test, "fe80::4455");
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_route(rows, 8, &a, TEST_NB6_NETIF), -1);

	/* With the network's row gone, the neighbour has none. */
	memset(rows[3], 0, sizeof(rows[3]));
	a = rtl9602c_test_in6(test, TEST_NB6_HOST);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_route(rows, 8, &a, TEST_NB6_NETIF), -1);
}

static void rtl9602c_test_nb6_put(struct kunit *test, struct rtl9602c_l34_nb6 *nb,
				  const char *addr, int netif, u16 slot)
{
	memset(nb, 0, sizeof(*nb));
	nb->addr = rtl9602c_test_in6(test, addr);
	nb->netif = netif;
	nb->slot = slot;
	nb->used = true;
	nb->seen = true;
}

/* The row index of @nb on route row @rt in @want, or -1. */
static int rtl9602c_test_nb6_at(u32 (*want)[3], const struct rtl9602c_l34_nb6 *nb,
				unsigned int rt, unsigned int from)
{
	u32 e[3] = { };
	unsigned int i;

	rtl9602c_l34_nb6_encode(e, &nb->addr, rt, nb->slot);
	for (i = from; i < RTL9602C_NEIGH6_ENTRIES; i++)
		if (!memcmp(want[i], e, sizeof(e)))
			return i;
	return -1;
}

/*
 * The layout: a neighbour takes the lowest free row of its bucket, both
 * buckets on row 4; a row already held is kept when another neighbour of
 * the bucket comes before it; a neighbour that leaves frees its row and
 * moves no other; one neither seen nor in use gets none; a ninth neighbour of
 * a bucket finds it full.
 */
static void rtl9602c_test_nb6_plan(struct kunit *test)
{
	static const char * const same[] = {
		"2001:db8:1:1::3", "2001:db8:1:1::21", "2001:db8:1:1::30",
		"2001:db8:1:1::47", "2001:db8:1:1::56", "2001:db8:1:1::65",
		"2001:db8:1:1::74", "2001:db8:1:1::8b",
	};
	struct rtl9602c_l34_nb6 *nb;
	u32 (*cur)[3], (*want)[3];
	u32 rows[8][5];
	unsigned int i;

	nb = kunit_kcalloc(test, RTL9602C_NEIGH6_ENTRIES, sizeof(*nb), GFP_KERNEL);
	cur = kunit_kcalloc(test, RTL9602C_NEIGH6_ENTRIES, sizeof(*cur), GFP_KERNEL);
	want = kunit_kcalloc(test, RTL9602C_NEIGH6_ENTRIES, sizeof(*want), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nb);
	KUNIT_ASSERT_NOT_NULL(test, cur);
	KUNIT_ASSERT_NOT_NULL(test, want);
	rtl9602c_test_nb6_rows(test, rows);

	/* The host on row 3, bucket 12; one on row 4, buckets 13 and 9. */
	rtl9602c_test_nb6_put(test, &nb[1], TEST_NB6_HOST, TEST_NB6_NETIF, 132);
	rtl9602c_test_nb6_put(test, &nb[2], "2001:db8:1:2::9", 5, 133);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_plan(nb, RTL9602C_NEIGH6_ENTRIES, rows, 8,
						    cur, want), 0U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[1], 3, 0), 96);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[2], 4, 0), 72);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[2], 4, 73), 104);
	for (i = 0; i < RTL9602C_NEIGH6_ENTRIES; i++)
		if (i != 96 && i != 72 && i != 104)
			KUNIT_EXPECT_EQ(test, want[i][2], 0U);

	/* A newcomer of bucket 12, before the host in the shadow, takes way 1. */
	memcpy(cur, want, RTL9602C_NEIGH6_ENTRIES * sizeof(*cur));
	rtl9602c_test_nb6_put(test, &nb[0], "2001:db8:1:1::12", TEST_NB6_NETIF, 140);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_plan(nb, RTL9602C_NEIGH6_ENTRIES, rows, 8,
						    cur, want), 0U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[1], 3, 0), 96);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[0], 3, 0), 97);

	/* The host leaves: its row is free, and the newcomer stays. */
	memcpy(cur, want, RTL9602C_NEIGH6_ENTRIES * sizeof(*cur));
	nb[1].used = false;
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_plan(nb, RTL9602C_NEIGH6_ENTRIES, rows, 8,
						    cur, want), 0U);
	KUNIT_EXPECT_EQ(test, want[96][2], 0U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[0], 3, 0), 97);

	/* Not seen in the kernel's table, or not yet placed: no row. */
	nb[0].seen = false;
	nb[2].fresh = true;
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_plan(nb, RTL9602C_NEIGH6_ENTRIES, rows, 8,
						    cur, want), 0U);
	for (i = 0; i < RTL9602C_NEIGH6_ENTRIES; i++)
		KUNIT_EXPECT_EQ(test, want[i][2], 0U);

	/* Nine neighbours of bucket 12: eight rows, one left out. */
	memset(nb, 0, RTL9602C_NEIGH6_ENTRIES * sizeof(*nb));
	memset(cur, 0, RTL9602C_NEIGH6_ENTRIES * sizeof(*cur));
	for (i = 0; i < ARRAY_SIZE(same); i++)
		rtl9602c_test_nb6_put(test, &nb[i], same[i], TEST_NB6_NETIF, 200 + i);
	rtl9602c_test_nb6_put(test, &nb[i], "2001:db8:1:1::12", TEST_NB6_NETIF, 300);
	KUNIT_EXPECT_EQ(test, rtl9602c_l34_nb6_plan(nb, RTL9602C_NEIGH6_ENTRIES, rows, 8,
						    cur, want), 1U);
	for (i = 0; i < ARRAY_SIZE(same); i++)
		KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[i], 3, 0), 96 + (int)i);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_nb6_at(want, &nb[i], 3, 0), -1);
}

/*
 * Which neighbours may have a row: resolved ones, a stale one included, at a
 * unicast address beyond the link; not one that failed, is still being
 * resolved or is gone, nor a link-local or multicast address.
 */
static void rtl9602c_test_nb6_wanted(struct kunit *test)
{
	struct in6_addr a = rtl9602c_test_in6(test, TEST_NB6_HOST);
	struct in6_addr ll = rtl9602c_test_in6(test, "fe80::4455");
	struct in6_addr mc = rtl9602c_test_in6(test, "ff02::1");

	KUNIT_EXPECT_TRUE(test, rtl9602c_l34_nb6_wanted(NUD_REACHABLE, false, &a));
	KUNIT_EXPECT_TRUE(test, rtl9602c_l34_nb6_wanted(NUD_STALE, false, &a));
	KUNIT_EXPECT_TRUE(test, rtl9602c_l34_nb6_wanted(NUD_DELAY, false, &a));
	KUNIT_EXPECT_TRUE(test, rtl9602c_l34_nb6_wanted(NUD_PROBE, false, &a));
	KUNIT_EXPECT_TRUE(test, rtl9602c_l34_nb6_wanted(NUD_PERMANENT, false, &a));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_nb6_wanted(NUD_FAILED, false, &a));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_nb6_wanted(NUD_INCOMPLETE, false, &a));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_nb6_wanted(NUD_NONE, false, &a));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_nb6_wanted(NUD_STALE, true, &a));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_nb6_wanted(NUD_REACHABLE, false, &ll));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_nb6_wanted(NUD_REACHABLE, false, &mc));
}

/* Route @i of the shadow of @p: @dst/@prefix on @ifindex and @port. */
static void rtl9602c_test_rt6(struct kunit *test, struct rtl9602c_priv *p,
			      unsigned int i, const char *dst, u8 prefix,
			      int ifindex, int port, u8 flags)
{
	struct rtl9602c_l34_route6 *r = &p->l34_route6[i];

	memset(r, 0, sizeof(*r));
	r->dst = rtl9602c_test_in6(test, dst);
	r->prefix = prefix;
	r->ifindex = ifindex;
	r->port = port;
	r->flags = flags;
	r->used = true;
}

/*
 * The row budget: a local network takes its on-link row, NEIGH on its
 * interface row, and a CPU row for each address of this device in it; with
 * the link-local, multicast, loopback and default rows one network fits, a
 * second does not, and the table is then left empty. A network whose device
 * has no interface row, or whose link is down, is the CPU's.
 */
static void rtl9602c_test_route6_budget(struct kunit *test)
{
	struct rtl9602c_priv *p;
	u32 hw[8][5] = { };
	unsigned int i, neigh = 0, cpu = 0;
	bool carried;

	p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->l34_netif_ifindex[TEST_NB6_NETIF] = 10;
	p->l34_netif_ifindex[5] = 11;
	p->l34_route6_wan = 3;
	p->l34_pppoe[3].ifindex = 20;

	rtl9602c_test_rt6(test, p, 0, "::", 0, 20, -1, 0);
	rtl9602c_test_rt6(test, p, 1, "::1", 128, 1, -1, RTL9602C_L34_RT_CPU);
	rtl9602c_test_rt6(test, p, 2, "2001:db8:1:1::", 64, 10, 0, 0);
	rtl9602c_test_rt6(test, p, 3, "2001:db8:1:1::1", 128, 10, 0, RTL9602C_L34_RT_CPU);
	rtl9602c_test_rt6(test, p, 4, "2001:db8:1:1::", 128, 10, 0, RTL9602C_L34_RT_CPU);

	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_route6_build(p, hw, &carried));
	KUNIT_EXPECT_TRUE(test, carried);
	for (i = 0; i < 7; i++) {
		u64 act = rtl9602c_flow_get(hw[i], RTL9602C_ROUTE6_ACTION);

		if (!rtl9602c_flow_get(hw[i], RTL9602C_ROUTE6_VALID))
			continue;
		if (act == RTL9602C_ROUTE_ACTION_NEIGH) {
			neigh++;
			KUNIT_EXPECT_EQ(test, i, 5U);
			KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[i], RTL9602C_ROUTE6_DST_LEN),
					64ULL);
			KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[i], RTL9602C_ROUTE6_L3IF_IDX),
					(u64)TEST_NB6_NETIF);
			KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[i], RTL9602C_ROUTE6_RT2WAN),
					0ULL);
		} else {
			KUNIT_EXPECT_EQ(test, act, 0ULL);
			cpu++;
		}
	}
	KUNIT_EXPECT_EQ(test, neigh, 1U);
	KUNIT_EXPECT_EQ(test, cpu, 5U);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[7], RTL9602C_ROUTE6_ACTION),
			(u64)RTL9602C_ROUTE_ACTION_NEXTHOP);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[7], RTL9602C_ROUTE6_NEXTHOP_IDX), 3ULL);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[7], RTL9602C_ROUTE6_RT2WAN), 1ULL);

	/* Without its interface row, or with its link down, the CPU's. */
	p->l34_netif_ifindex[TEST_NB6_NETIF] = 0;
	memset(hw, 0, sizeof(hw));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_route6_build(p, hw, &carried));
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[5], RTL9602C_ROUTE6_ACTION), 0ULL);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[5], RTL9602C_ROUTE6_VALID), 1ULL);
	p->l34_netif_ifindex[TEST_NB6_NETIF] = 10;
	p->l34_route6[2].linkdown = true;
	memset(hw, 0, sizeof(hw));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_route6_build(p, hw, &carried));
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[5], RTL9602C_ROUTE6_ACTION), 0ULL);
	p->l34_route6[2].linkdown = false;

	/* A second network: nine rows before the default route's. */
	rtl9602c_test_rt6(test, p, 5, "2001:db8:1:2::", 64, 11, 0, 0);
	rtl9602c_test_rt6(test, p, 6, "2001:db8:1:2::1", 128, 11, 0, RTL9602C_L34_RT_CPU);
	rtl9602c_test_rt6(test, p, 7, "2001:db8:1:2::", 128, 11, 0, RTL9602C_L34_RT_CPU);
	memset(hw, 0, sizeof(hw));
	KUNIT_EXPECT_TRUE(test, rtl9602c_l34_route6_build(p, hw, &carried));
	for (i = 0; i < 8; i++)
		KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[i], RTL9602C_ROUTE6_VALID), 0ULL);

	/* Without this device's addresses in it, the second network fits. */
	p->l34_route6[6].used = false;
	p->l34_route6[7].used = false;
	memset(hw, 0, sizeof(hw));
	KUNIT_EXPECT_FALSE(test, rtl9602c_l34_route6_build(p, hw, &carried));
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[6], RTL9602C_ROUTE6_ACTION),
			(u64)RTL9602C_ROUTE_ACTION_NEIGH);
	KUNIT_EXPECT_EQ(test, rtl9602c_flow_get(hw[6], RTL9602C_ROUTE6_L3IF_IDX), 5ULL);
}
#endif

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
 * The classifier's bookkeeping, on a switch with the driver's patterns:
 * entries 0 to 31 of pattern 0 and 32 to 255 of pattern 1, no stream entry,
 * no claimed VLAN and no port stream.
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
	p->cf_p1 = RTL9602C_CLS_ENTRIES - RTL9602C_CLS_PATTERN1_NUM;
	for (i = 0; i < RTL9602C_NUM_PORTS; i++) {
		p->cf_sid[i] = -1;
		p->cf_sid_flow[i] = -1;
	}
	return p;
}

/* The first entry of pattern 1. */
#define TEST_P1		(RTL9602C_CLS_ENTRIES - RTL9602C_CLS_PATTERN1_NUM)

/* Stream entries from @lo to the top of the table. */
static void rtl9602c_test_cf_streams(struct rtl9602c_priv *p, unsigned int lo)
{
	unsigned int idx;

	for (idx = lo; idx < RTL9602C_CLS_ENTRIES; idx++) {
		set_bit(idx, p->cf_used);
		set_bit(idx, p->cf_sid_owned);
	}
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
 * "vlan_id @match action vlan modify id @out", at tc priority 0.
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

/* The rewrite of an S-tag of S-VID @match into @out on its own template. */
static struct rtl9602c_cf_xlate rtl9602c_test_cf_srule(int port, u16 match,
						       u16 out)
{
	struct rtl9602c_cf_xlate x = rtl9602c_test_cf_rule(port, match, out);

	x.value = (x.value & ~RTL9602C_CLS_RULE_CTAG) | RTL9602C_CLS_RULE_STAG;
	x.care &= ~RTL9602C_CLS_RULE_CTAG;
	x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG,
			      RTL9602C_CLS_ACT_STAG_ADD) |
		   RTL9602C_CLS_ACT_SVID_SET |
		   FIELD_PREP(RTL9602C_CLS_ACT_SVID, out);
	x.act[1] = 0;
	x.tmpl = RTL9602C_CLS_TMPL_S;
	return x;
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

/* Whether entries @lo to @lo + @n - 1 all hold the rule of @cookie on @port. */
static bool rtl9602c_test_cf_at(struct rtl9602c_priv *p, unsigned int lo,
				unsigned int n, unsigned long cookie, int port)
{
	unsigned int idx;

	for (idx = lo; idx < lo + n; idx++)
		if (!rtl9602c_cf_xlate_of(p, idx, cookie, port))
			return false;
	return rtl9602c_test_cf_count(p, cookie, port) == n;
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
 * or no stream for a priority no flow claims. A rule on a C-tag takes them at
 * the bottom of pattern 1, one on an S-tag at the bottom of pattern 0.
 */
static void rtl9602c_test_cf_split_entries(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_xlate x = rtl9602c_test_cf_rule(0, 410, 3001);
	static const int flow[8] = { 3, 3, 3, 3, 5, 5, -1, -1 };
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_entry e;
	unsigned int idx;

	rtl9602c_test_cf_divide(p, 3001);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_split(p, &x));
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       &t), 8);

	for (idx = 0; idx < 8; idx++) {
		KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, TEST_P1 + idx,
							     TEST_COOKIE(1), 0));
		KUNIT_EXPECT_EQ(test, p->cf_xlate[TEST_P1 + idx].pcp, (s8)idx);
		rtl9602c_cf_xlate_entry(p, TEST_P1 + idx, &e);
		KUNIT_EXPECT_FALSE(test, e.downstream);
		KUNIT_EXPECT_EQ(test, e.tmpl, (u8)RTL9602C_CLS_TMPL_C);
		KUNIT_EXPECT_EQ(test, e.care, x.care | RTL9602C_CLS_RULE_PRI);
		KUNIT_EXPECT_EQ(test, e.value,
				x.value | FIELD_PREP(RTL9602C_CLS_RULE_PRI, idx));
		KUNIT_EXPECT_EQ(test, e.act[1], x.act[1]);
		KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + idx),
				flow[idx]);
	}
	KUNIT_EXPECT_FALSE(test, test_bit(TEST_P1 + 8, p->cf_used));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(p->cf_used, TEST_P1));

	/* An S-tag rewritten on the S-tag's own template: its priorities. */
	x = rtl9602c_test_cf_srule(0, 200, 3001);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(2),
						       &t), 8);
	for (idx = 0; idx < 8; idx++) {
		rtl9602c_cf_xlate_entry(p, idx, &e);
		KUNIT_EXPECT_EQ(test, e.tmpl, (u8)RTL9602C_CLS_TMPL_S);
		KUNIT_EXPECT_EQ(test, FIELD_GET(RTL9602C_CLS_RULE_PRI, e.value),
				idx);
		KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, idx), flow[idx]);
	}
}

/*
 * The rules that stay one entry: one that matches a priority, one that
 * writes one, a rewrite of an S-tag matched on the C-tag behind it (which
 * takes priority 0's flow, in pattern 0), a push onto an untagged frame, a
 * downstream rule, and a rule into a VLAN one flow claims whole or none
 * claims.
 */
static void rtl9602c_test_cf_split_which(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;
	unsigned long c = 0;

	rtl9602c_test_cf_divide(p, 3001);
	rtl9602c_test_cf_whole(p, 3002, 7);
	p->cf_sid_flow[0] = 9;

	x = rtl9602c_test_cf_rule(0, 410, 3001);
	x.value |= FIELD_PREP(RTL9602C_CLS_RULE_PRI, 5);
	x.care |= RTL9602C_CLS_RULE_PRI;
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[TEST_P1].pcp, (s8)-1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1), 5);

	x = rtl9602c_test_cf_rule(0, 411, 3001);
	x.act[1] |= RTL9602C_CLS_ACT_CPRI_SET |
		    FIELD_PREP(RTL9602C_CLS_ACT_CPRI, 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 1), 3);

	x = rtl9602c_test_cf_rule(0, 412, 3001);
	x.value |= RTL9602C_CLS_RULE_STAG;
	x.act[0] = FIELD_PREP(RTL9602C_CLS_ACT_STAG, RTL9602C_CLS_ACT_STAG_ADD) |
		   RTL9602C_CLS_ACT_SVID_SET |
		   FIELD_PREP(RTL9602C_CLS_ACT_SVID, 3001);
	x.act[1] = 0;
	KUNIT_EXPECT_FALSE(test, rtl9602c_cf_xlate_split(p, &x));
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, 0, TEST_COOKIE(c), 0));
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, 0), 3);

	x = rtl9602c_test_cf_rule(0, 0, 3001);
	x.value &= ~(RTL9602C_CLS_RULE_CTAG | RTL9602C_CLS_RULE_VID);
	x.care &= ~RTL9602C_CLS_RULE_VID;
	x.act[1] |= RTL9602C_CLS_ACT_CPRI_SET |
		    FIELD_PREP(RTL9602C_CLS_ACT_CPRI, 6);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 2), -1);

	x = rtl9602c_test_cf_rule(RTL9602C_PORT_PON, 3001, 410);
	x.value &= ~RTL9602C_CLS_RULE_UNI;
	x.care &= ~RTL9602C_CLS_RULE_UNI;
	rtl9602c_test_cf_divide(p, 410);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 3), -1);

	x = rtl9602c_test_cf_rule(0, 413, 3002);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 4), 7);

	x = rtl9602c_test_cf_rule(0, 414, 3003);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 5), 9);

	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			7U);
}

/*
 * A change of the claims lays the rules out again: a rule into a VLAN that
 * comes to be divided takes eight entries, right above its own, the rules
 * above it moving up, all below the stream entries; it gives seven up again
 * when one flow takes the VLAN whole, and takes seven back where they are
 * free without moving another rule. The downstream rules are left as they
 * are, but for the moves.
 */
static void rtl9602c_test_cf_relayout(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;
	unsigned int idx, h;

	rtl9602c_test_cf_whole(p, 3001, 3);
	rtl9602c_test_cf_streams(p, 254);

	x = rtl9602c_test_cf_rule(0, 410, 3001);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       &t), 1);
	x = rtl9602c_test_cf_rule(0, 420, 3002);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(2),
						       &t), 1);
	x = rtl9602c_test_cf_rule(RTL9602C_PORT_PON, 3001, 410);
	x.value &= ~RTL9602C_CLS_RULE_UNI;
	x.care &= ~RTL9602C_CLS_RULE_UNI;
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(3),
						       &t), 1);

	/* 3001 divided: the first rule takes 33 to 39, the others move up. */
	rtl9602c_test_cf_divide(p, 3001);
	rtl9602c_cf_xlate_relayout(p, &t);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1, 8,
						    TEST_COOKIE(1), 0));
	for (idx = 0; idx < 8; idx++)
		KUNIT_EXPECT_EQ(test, p->cf_xlate[TEST_P1 + idx].pcp, (s8)idx);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1), 3);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 4), 5);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 6), -1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 8, 1,
						    TEST_COOKIE(2), 0));
	KUNIT_EXPECT_EQ(test, p->cf_xlate[TEST_P1 + 8].pcp, (s8)-1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 9, 1,
						    TEST_COOKIE(3),
						    RTL9602C_PORT_PON));
	KUNIT_EXPECT_TRUE(test, test_bit(254, p->cf_sid_owned));

	/* Laid out again with nothing changed, nothing moves. */
	rtl9602c_cf_xlate_relayout(p, &t);
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			12U);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1, 8,
						    TEST_COOKIE(1), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 9, 1,
						    TEST_COOKIE(3),
						    RTL9602C_PORT_PON));

	/*
	 * 3001 whole again and 3002 divided in one change: the first rule
	 * keeps its lowest entry for every priority, and the second takes
	 * seven right above its own, the downstream rule moving up.
	 */
	rtl9602c_test_cf_whole(p, 3001, 3);
	rtl9602c_test_cf_divide(p, 3002);
	rtl9602c_cf_xlate_relayout(p, &t);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1, 1,
						    TEST_COOKIE(1), 0));
	KUNIT_EXPECT_EQ(test, p->cf_xlate[TEST_P1].pcp, (s8)-1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1), 3);
	for (idx = TEST_P1 + 1; idx < TEST_P1 + 8; idx++)
		KUNIT_EXPECT_FALSE(test, test_bit(idx, p->cf_used));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 8, 8,
						    TEST_COOKIE(2), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 16, 1,
						    TEST_COOKIE(3),
						    RTL9602C_PORT_PON));

	/* 3002 whole: the seven are given up. */
	rtl9602c_test_cf_whole(p, 3002, 4);
	rtl9602c_cf_xlate_relayout(p, &t);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 8, 1,
						    TEST_COOKIE(2), 0));
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1 + 8), 4);
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			5U);

	/*
	 * With the first rule gone, 3002 divided again: the second rule takes
	 * the seven free entries above its own, where the downstream rule no
	 * longer is in the way, and a layout again moves nothing.
	 */
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_forget(p, TEST_COOKIE(1), 0,
						       &t), 1U);
	rtl9602c_test_cf_divide(p, 3002);
	for (h = 0; h < 2; h++) {
		rtl9602c_cf_xlate_relayout(p, &t);
		KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 8, 8,
							    TEST_COOKIE(2), 0));
		for (idx = 0; idx < 8; idx++)
			KUNIT_EXPECT_EQ(test,
					p->cf_xlate[TEST_P1 + 8 + idx].pcp,
					(s8)idx);
		KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 16, 1,
							    TEST_COOKIE(3),
							    RTL9602C_PORT_PON));
	}
}

/*
 * The classifier's depth: a new rule that needs eight entries where its
 * pattern has fewer free is refused, and so is a rule of one where none is;
 * a rule with an entry already keeps one when its VLAN comes to be divided and
 * there is no room, and takes eight once there is. The entries are never
 * taken above a stream entry, nor in the other pattern.
 */
static void rtl9602c_test_cf_depth(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;
	unsigned int idx;

	rtl9602c_test_cf_streams(p, TEST_P1 + 6);

	rtl9602c_test_cf_divide(p, 3001);
	x = rtl9602c_test_cf_rule(0, 410, 3001);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       &t), -ENOSPC);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(1), 0), 0U);
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			218U);

	/* Into 3002, whole: one entry, the first of pattern 1. */
	rtl9602c_test_cf_whole(p, 3002, 3);
	x = rtl9602c_test_cf_rule(0, 420, 3002);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(2),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1, 1,
						    TEST_COOKIE(2), 0));

	/* 3002 divided with five free: the rule keeps one entry. */
	rtl9602c_test_cf_divide(p, 3002);
	rtl9602c_cf_xlate_relayout(p, &t);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(2), 0), 1U);
	KUNIT_EXPECT_EQ(test, p->cf_xlate[TEST_P1].pcp, (s8)-1);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_flow(p, TEST_P1), 3);

	/* Two stream entries fewer, seven free: it takes the seven. */
	for (idx = TEST_P1 + 6; idx < TEST_P1 + 8; idx++) {
		clear_bit(idx, p->cf_used);
		clear_bit(idx, p->cf_sid_owned);
	}
	rtl9602c_cf_xlate_relayout(p, &t);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1, 8,
						    TEST_COOKIE(2), 0));
	KUNIT_EXPECT_EQ(test, find_next_zero_bit(p->cf_used,
						 RTL9602C_CLS_ENTRIES, TEST_P1),
			(unsigned long)RTL9602C_CLS_ENTRIES);

	/* Pattern 1 full: a rule of one entry is refused too. */
	x = rtl9602c_test_cf_rule(0, 430, 3003);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(3),
						       &t), -ENOSPC);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(p->cf_used, TEST_P1));
}

/*
 * A rule of a block that several ports share comes with one cookie for each
 * port: removing it from one port leaves the other port's entries.
 */
static void rtl9602c_test_cf_forget(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;

	rtl9602c_test_cf_divide(p, 3001);
	x = rtl9602c_test_cf_rule(0, 410, 3001);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       &t), 8);
	x = rtl9602c_test_cf_rule(1, 410, 3001);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       &t), 8);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 8, 8,
						    TEST_COOKIE(1), 1));

	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_forget(p, TEST_COOKIE(1), 0,
						       &t), 8U);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_cf_count(p, TEST_COOKIE(1), 1), 8U);
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			8U);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_forget(p, TEST_COOKIE(1), 0,
						       &t), 0U);
}

/*
 * The patterns: a rule that compares an S-tag goes in pattern 0, on the
 * S-tag's template or the C-tag's; one on a C-tag or an untagged frame in
 * pattern 1, upstream or downstream. Each pattern has its own room: with
 * pattern 0 full a rule on an S-tag is refused while one on a C-tag still
 * fits. Stream entries come from the top of pattern 1 and never below it.
 */
static void rtl9602c_test_cf_patterns(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;
	unsigned long c = 0;
	unsigned int idx;

	KUNIT_EXPECT_EQ(test, rtl9602c_cf_top(p, false), (unsigned int)TEST_P1);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_top(p, true),
			(unsigned int)RTL9602C_CLS_ENTRIES);

	x = rtl9602c_test_cf_srule(RTL9602C_PORT_PON, 200, 300);
	x.value &= ~RTL9602C_CLS_RULE_UNI;
	x.care &= ~RTL9602C_CLS_RULE_UNI;
	KUNIT_EXPECT_FALSE(test, rtl9602c_cf_xlate_p1(&x));
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, 0, TEST_COOKIE(c),
						     RTL9602C_PORT_PON));

	x = rtl9602c_test_cf_rule(0, 410, 1410);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_p1(&x));
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, TEST_P1,
						     TEST_COOKIE(c), 0));

	x = rtl9602c_test_cf_rule(0, 0, 410);
	x.value &= ~(RTL9602C_CLS_RULE_CTAG | RTL9602C_CLS_RULE_VID);
	x.care &= ~RTL9602C_CLS_RULE_VID;
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_p1(&x));
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, TEST_P1 + 1,
						     TEST_COOKIE(c), 0));

	/* The C-tag behind an S-tag: pattern 0 on the C-tag's template. */
	x = rtl9602c_test_cf_rule(0, 100, 300);
	x.value |= RTL9602C_CLS_RULE_STAG;
	KUNIT_EXPECT_FALSE(test, rtl9602c_cf_xlate_p1(&x));
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, 1, TEST_COOKIE(c), 0));

	/* Pattern 0 full: an S-tag rule is refused, a C-tag rule is not. */
	for (idx = 2; idx < TEST_P1; idx++) {
		x = rtl9602c_test_cf_srule(0, 200 + idx, 300);
		KUNIT_ASSERT_EQ(test,
				rtl9602c_cf_xlate_layout(p, &x,
							 TEST_COOKIE(++c), &t),
				1);
	}
	x = rtl9602c_test_cf_srule(0, 199, 300);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), -ENOSPC);
	x = rtl9602c_test_cf_rule(0, 411, 1411);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(++c),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_of(p, TEST_P1 + 2,
						     TEST_COOKIE(c), 0));

	/* Stream entries: the top of pattern 1, downwards, and no further. */
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_sid_alloc(p),
			RTL9602C_CLS_ENTRIES - 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_sid_alloc(p),
			RTL9602C_CLS_ENTRIES - 2);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_top(p, true),
			(unsigned int)RTL9602C_CLS_ENTRIES - 2);
	for (idx = RTL9602C_CLS_ENTRIES - 3; idx >= TEST_P1 + 3; idx--)
		KUNIT_EXPECT_EQ(test, rtl9602c_cf_sid_alloc(p), (int)idx);
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_sid_alloc(p), -ENOSPC);
}

/*
 * The rules of a pattern keep the order of their tc priorities whatever order
 * they come in: a rule goes after those of its own value and of smaller
 * ones, and those of larger values move up to make room, by one entry, or
 * by eight for a rule written per priority.
 */
static void rtl9602c_test_cf_order(struct kunit *test)
{
	static const struct {
		u16 prio, match;
	} add[] = {
		{ 30, 401 }, { 10, 402 }, { 20, 403 }, { 20, 404 }, { 40, 405 },
	};
	static const unsigned int at[] = { 3, 0, 1, 2, 4 };
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;
	unsigned int i, idx;
	int err;

	rtl9602c_test_cf_streams(p, 250);
	for (i = 0; i < ARRAY_SIZE(add); i++) {
		x = rtl9602c_test_cf_rule(0, add[i].match, 1400);
		x.prio = add[i].prio;
		err = rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(i + 1), &t);
		KUNIT_ASSERT_EQ(test, err, 1);
	}
	for (i = 0; i < ARRAY_SIZE(add); i++)
		KUNIT_EXPECT_TRUE(test,
				  rtl9602c_test_cf_at(p, TEST_P1 + at[i], 1,
						      TEST_COOKIE(i + 1), 0));

	/* At 15, into a divided VLAN: eight right above the rule at 10. */
	rtl9602c_test_cf_divide(p, 3001);
	x = rtl9602c_test_cf_rule(0, 406, 3001);
	x.prio = 15;
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(6),
						       &t), 8);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1, 1,
						    TEST_COOKIE(2), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 1, 8,
						    TEST_COOKIE(6), 0));
	for (i = 0; i < ARRAY_SIZE(add); i++) {
		if (!at[i])
			continue;
		idx = TEST_P1 + 8 + at[i];
		KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, idx, 1,
							    TEST_COOKIE(i + 1),
							    0));
	}
	KUNIT_EXPECT_EQ(test, bitmap_weight(p->cf_used, RTL9602C_CLS_ENTRIES),
			19U);
}

/*
 * A rule that finds too few free entries above the rules of its tc priority
 * has the free ones below gathered first: the rules move down into them, in
 * their order, and the new rule takes what is then free above them.
 */
static void rtl9602c_test_cf_gather(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;
	unsigned int i;
	int err;

	rtl9602c_test_cf_streams(p, TEST_P1 + 12);
	for (i = 0; i < 10; i++) {
		x = rtl9602c_test_cf_rule(0, 400 + i, 1400);
		x.prio = 10 + i;
		err = rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(i + 1), &t);
		KUNIT_ASSERT_EQ(test, err, 1);
	}
	/* Holes at 33, 35, 37 and 39, two free at the top: six. */
	for (i = 1; i < 8; i += 2)
		KUNIT_EXPECT_EQ(test,
				rtl9602c_cf_xlate_forget(p, TEST_COOKIE(i + 1),
							 0, &t), 1U);

	rtl9602c_test_cf_divide(p, 3001);
	x = rtl9602c_test_cf_rule(0, 420, 3001);
	x.prio = 30;
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(20),
						       &t), -ENOSPC);
	x = rtl9602c_test_cf_rule(0, 421, 1400);
	x.prio = 30;
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(21),
						       &t), 1);
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 10, 1,
						    TEST_COOKIE(21), 0));

	/*
	 * Two more free at the top, eight in all: the split rule at 15 has the
	 * rules gathered at the bottom and takes the eight above the rules
	 * before it, those after it moving up.
	 */
	for (i = TEST_P1 + 12; i < TEST_P1 + 14; i++) {
		clear_bit(i, p->cf_used);
		clear_bit(i, p->cf_sid_owned);
	}
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_forget(p, TEST_COOKIE(21), 0,
						       &t), 1U);
	x = rtl9602c_test_cf_rule(0, 420, 3001);
	x.prio = 15;
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(20),
						       &t), 8);
	/* 10, 12 and 14 at 32 to 34, the eight at 35 to 42, then the rest. */
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1, 1,
						    TEST_COOKIE(1), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 1, 1,
						    TEST_COOKIE(3), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 2, 1,
						    TEST_COOKIE(5), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 3, 8,
						    TEST_COOKIE(20), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 11, 1,
						    TEST_COOKIE(7), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 12, 1,
						    TEST_COOKIE(9), 0));
	KUNIT_EXPECT_TRUE(test, rtl9602c_test_cf_at(p, TEST_P1 + 13, 1,
						    TEST_COOKIE(10), 0));
}

/*
 * The planes of an entry. A rule that names the EtherType of an untagged
 * frame compares it in rule word 1, below the valid bit: a push of VID 777
 * onto the downstream frames of EtherType 0x88b5. A translation of 410 that
 * names none has the words measured for it, nothing compared in word 1.
 */
static void rtl9602c_test_cf_planes(struct kunit *test)
{
	struct rtl9602c_priv *p = rtl9602c_test_cf_priv(test);
	struct rtl9602c_cf_txn t = {};
	struct rtl9602c_cf_xlate x;
	struct rtl9602c_cf_entry e;
	u32 zeros[2], ones[2];

	x = rtl9602c_test_cf_rule(RTL9602C_PORT_PON, 0, 777);
	x.value = 0;
	x.care = RTL9602C_CLS_RULE_CTAG | RTL9602C_CLS_RULE_STAG;
	x.etype = 0x88b5;
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_p1(&x));
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(1),
						       &t), 1);
	rtl9602c_cf_xlate_entry(p, TEST_P1, &e);
	KUNIT_EXPECT_EQ(test, e.value1, 0x88b5U);
	KUNIT_EXPECT_EQ(test, e.care1, (u32)RTL9602C_CLS_RULE1_ETYPE);
	rtl9602c_cf_planes(&e, zeros, ones);
	KUNIT_EXPECT_EQ(test, ones[0], 0x80000000U);
	KUNIT_EXPECT_EQ(test, zeros[0], 0x00000018U);
	KUNIT_EXPECT_EQ(test, ones[1], 0x000188b5U);
	KUNIT_EXPECT_EQ(test, zeros[1], 0x0000774aU);

	x = rtl9602c_test_cf_rule(0, 410, 1410);
	KUNIT_ASSERT_EQ(test, rtl9602c_cf_xlate_layout(p, &x, TEST_COOKIE(2),
						       &t), 1);
	rtl9602c_cf_xlate_entry(p, TEST_P1 + 1, &e);
	KUNIT_EXPECT_EQ(test, e.care1, 0U);
	rtl9602c_cf_planes(&e, zeros, ones);
	KUNIT_EXPECT_EQ(test, ones[0], 0x000cd008U);
	KUNIT_EXPECT_EQ(test, zeros[0], (x.care | RTL9602C_CLS_RULE_DS) &
					~x.value);
	KUNIT_EXPECT_EQ(test, ones[1], (u32)RTL9602C_CLS_RULE_VALID);
	KUNIT_EXPECT_EQ(test, zeros[1], 0U);
}

/* A vlan action of a VLAN rule, as tc flower hands it over. */
struct rtl9602c_test_vlan_act {
	enum flow_action_id id;
	u16 proto;
	u16 vid;
	u8 prio;
};

/* The tag operation of actions @v, @n of them, on frames with outer tag @outer. */
static int rtl9602c_test_vlan_act(struct kunit *test,
				  const struct rtl9602c_test_vlan_act *v,
				  unsigned int n, u16 outer,
				  struct rtl9602c_cf_xlate *x)
{
	struct flow_action *a;
	unsigned int i;

	a = kunit_kzalloc(test, struct_size(a, entries, n), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, a);
	a->num_entries = n;
	for (i = 0; i < n; i++) {
		a->entries[i].id = v[i].id;
		a->entries[i].vlan.proto = htons(v[i].proto);
		a->entries[i].vlan.vid = v[i].vid;
		a->entries[i].vlan.prio = v[i].prio;
	}
	memset(x, 0, sizeof(*x));
	x->out_vid = VLAN_N_VID;
	return rtl9602c_flower_vlan_act(a, outer, x, NULL);
}

/*
 * A modify of a C-tag writes the action word measured for a translation to
 * 1410, and keeps the frame's priority when it gives 0, as it cannot say that
 * it gives one; a pop and a push of the same protocol rewrite the outer tag
 * with the push's priority, 0 included; a pop and a push of the other
 * protocol, and no action, are refused.
 */
static void rtl9602c_test_vlan_rewrite(struct kunit *test)
{
	static const struct rtl9602c_test_vlan_act modify[] = {
		{ FLOW_ACTION_VLAN_MANGLE, ETH_P_8021Q, 1410, 0 },
	};
	static const struct rtl9602c_test_vlan_act repush_c[] = {
		{ FLOW_ACTION_VLAN_POP },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021Q, 1410, 0 },
	};
	static const struct rtl9602c_test_vlan_act repush_s[] = {
		{ FLOW_ACTION_VLAN_POP },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021AD, 300, 0 },
	};
	struct rtl9602c_cf_xlate x;

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, modify, 1, ETH_P_8021Q, &x), 0);
	KUNIT_EXPECT_EQ(test, x.act[1], 0x00042c12U);
	KUNIT_EXPECT_EQ(test, x.act[0], 0U);
	KUNIT_EXPECT_EQ(test, x.out_vid, 1410);

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush_c, 2, ETH_P_8021Q, &x), 0);
	KUNIT_EXPECT_EQ(test, x.act[1], 0x00042c12U | RTL9602C_CLS_ACT_CPRI_SET);
	KUNIT_EXPECT_EQ(test, FIELD_GET(RTL9602C_CLS_ACT_CPRI, x.act[1]), 0U);
	KUNIT_EXPECT_EQ(test, x.act[0], 0U);
	KUNIT_EXPECT_EQ(test, x.out_vid, 1410);

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush_s, 2, ETH_P_8021AD, &x), 0);
	KUNIT_EXPECT_EQ(test, x.act[0],
			(u32)(FIELD_PREP(RTL9602C_CLS_ACT_STAG, RTL9602C_CLS_ACT_STAG_ADD) |
			      RTL9602C_CLS_ACT_SVID_SET | FIELD_PREP(RTL9602C_CLS_ACT_SVID, 300) |
			      RTL9602C_CLS_ACT_SPRI_SET));
	KUNIT_EXPECT_EQ(test, x.act[1], 0U);

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush_s, 2, ETH_P_8021Q, &x),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush_c, 2, ETH_P_8021AD, &x),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush_c, 2, 0, &x), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush_c, 0, ETH_P_8021Q, &x),
			-EOPNOTSUPP);
}

/*
 * A C-tag written, onto an untagged frame or over the outer C-tag, and an
 * S-tag pushed in front of it make one entry with both planes; the S-tag
 * comes last, onto a frame with a C-tag and no S-tag.
 */
static void rtl9602c_test_vlan_two_tags(struct kunit *test)
{
	static const struct rtl9602c_test_vlan_act untagged[] = {
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021Q, 410, 5 },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021AD, 200, 3 },
	};
	static const struct rtl9602c_test_vlan_act modify[] = {
		{ FLOW_ACTION_VLAN_MANGLE, ETH_P_8021Q, 100, 6 },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021AD, 200, 3 },
	};
	static const struct rtl9602c_test_vlan_act repush[] = {
		{ FLOW_ACTION_VLAN_POP },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021Q, 100, 0 },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021AD, 200, 3 },
	};
	static const struct rtl9602c_test_vlan_act s_first[] = {
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021AD, 200, 3 },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021Q, 410, 5 },
	};
	static const struct rtl9602c_test_vlan_act s_rewrite[] = {
		{ FLOW_ACTION_VLAN_MANGLE, ETH_P_8021AD, 300, 4 },
		{ FLOW_ACTION_VLAN_PUSH, ETH_P_8021AD, 200, 3 },
	};
	const u32 stag = FIELD_PREP(RTL9602C_CLS_ACT_STAG, RTL9602C_CLS_ACT_STAG_ADD) |
			 RTL9602C_CLS_ACT_SVID_SET | FIELD_PREP(RTL9602C_CLS_ACT_SVID, 200) |
			 RTL9602C_CLS_ACT_SPRI_SET | FIELD_PREP(RTL9602C_CLS_ACT_SPRI, 3);
	const u32 ctag = RTL9602C_CLS_ACT_CVID_SET |
			 FIELD_PREP(RTL9602C_CLS_ACT_CTAG, RTL9602C_CLS_ACT_CTAG_TAG);
	struct rtl9602c_cf_xlate x;

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, untagged, 2, 0, &x), 0);
	KUNIT_EXPECT_EQ(test, x.act[0], stag);
	KUNIT_EXPECT_EQ(test, x.act[1],
			ctag | FIELD_PREP(RTL9602C_CLS_ACT_CVID, 410) |
			RTL9602C_CLS_ACT_CPRI_SET | FIELD_PREP(RTL9602C_CLS_ACT_CPRI, 5));
	KUNIT_EXPECT_EQ(test, x.out_vid, 200);

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, modify, 2, ETH_P_8021Q, &x), 0);
	KUNIT_EXPECT_EQ(test, x.act[0], stag);
	KUNIT_EXPECT_EQ(test, x.act[1],
			ctag | FIELD_PREP(RTL9602C_CLS_ACT_CVID, 100) |
			RTL9602C_CLS_ACT_CPRI_SET | FIELD_PREP(RTL9602C_CLS_ACT_CPRI, 6));

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush, 3, ETH_P_8021Q, &x), 0);
	KUNIT_EXPECT_EQ(test, x.act[0], stag);
	KUNIT_EXPECT_EQ(test, x.act[1],
			ctag | FIELD_PREP(RTL9602C_CLS_ACT_CVID, 100) | RTL9602C_CLS_ACT_CPRI_SET);
	KUNIT_EXPECT_EQ(test, x.out_vid, 200);

	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, untagged, 2, ETH_P_8021Q, &x),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, modify, 2, 0, &x), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, repush, 3, 0, &x), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, s_first, 2, 0, &x), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_vlan_act(test, s_rewrite, 2, ETH_P_8021AD, &x),
			-EOPNOTSUPP);
}

/*
 * Of a rule that writes both tags into a VLAN whose GEM ports divide its
 * priorities, the S-tag's priority chooses the flow, and the rule, which
 * writes it, stays one entry.
 */
static void rtl9602c_test_vlan_two_tags_flow(struct kunit *test)
{
	struct rtl9602c_cf_xlate x = { .out_vid = 200, .pcp = -1 };
	struct rtl9602c_priv *p;
	unsigned int pcp;

	p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->gem_vid_flow = kunit_kcalloc(test, VLAN_N_VID, sizeof(*p->gem_vid_flow),
					GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p->gem_vid_flow);
	for (pcp = 0; pcp < 8; pcp++)
		p->gem_vid_flow[200][pcp] = pcp < 4 ? 6 : 7;

	x.act[0] = RTL9602C_CLS_ACT_SPRI_SET | FIELD_PREP(RTL9602C_CLS_ACT_SPRI, 3);
	x.act[1] = RTL9602C_CLS_ACT_CPRI_SET | FIELD_PREP(RTL9602C_CLS_ACT_CPRI, 5);
	KUNIT_EXPECT_FALSE(test, rtl9602c_cf_xlate_split(p, &x));
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_flow(p, &x), 5);
	x.act[0] = 0;
	KUNIT_EXPECT_EQ(test, rtl9602c_cf_xlate_flow(p, &x), 6);
}

/* The keys of a flower rule that rtl9602c_flower_vlan_match() reads. */
struct rtl9602c_test_keys {
	struct flow_dissector_key_control control;
	struct flow_dissector_key_basic basic;
	struct flow_dissector_key_vlan vlan;
	struct flow_dissector_key_vlan cvlan;
	struct flow_dissector_key_num_of_vlans nvlans;
	struct flow_dissector_key_pppoe pppoe;
};

/* A flower rule with no action, its keys and masks, as flower builds it. */
struct rtl9602c_test_rule {
	struct flow_dissector d;
	struct rtl9602c_test_keys key;
	struct rtl9602c_test_keys mask;
	struct flow_rule rule;
};

/*
 * The match of @r, which uses the keys @used besides the control and basic
 * keys that flower always uses, into @x and @outer.
 */
static int rtl9602c_test_match(struct rtl9602c_test_rule *r, u64 used,
			       struct rtl9602c_cf_xlate *x, u16 *outer)
{
	r->d.used_keys = used | BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL) |
			 BIT_ULL(FLOW_DISSECTOR_KEY_BASIC);
	r->d.offset[FLOW_DISSECTOR_KEY_CONTROL] =
		offsetof(struct rtl9602c_test_keys, control);
	r->d.offset[FLOW_DISSECTOR_KEY_BASIC] =
		offsetof(struct rtl9602c_test_keys, basic);
	r->d.offset[FLOW_DISSECTOR_KEY_VLAN] =
		offsetof(struct rtl9602c_test_keys, vlan);
	r->d.offset[FLOW_DISSECTOR_KEY_CVLAN] =
		offsetof(struct rtl9602c_test_keys, cvlan);
	r->d.offset[FLOW_DISSECTOR_KEY_NUM_OF_VLANS] =
		offsetof(struct rtl9602c_test_keys, nvlans);
	r->d.offset[FLOW_DISSECTOR_KEY_PPPOE] =
		offsetof(struct rtl9602c_test_keys, pppoe);
	r->rule.match.dissector = &r->d;
	r->rule.match.key = &r->key;
	r->rule.match.mask = &r->mask;
	memset(x, 0, sizeof(*x));
	return rtl9602c_flower_vlan_match(&r->rule, x, outer, NULL);
}

/*
 * The EtherType of an untagged frame, named in full, is matched: one from the
 * basic key, and a PPPoE session's from the PPPoE key, where flower carries
 * it, without its session or PPP protocol. One named in part, a length, one
 * behind a C-tag and an IP protocol are refused.
 */
static void rtl9602c_test_vlan_etype(struct kunit *test)
{
	const u64 nv = BIT_ULL(FLOW_DISSECTOR_KEY_NUM_OF_VLANS);
	const u64 ppp = BIT_ULL(FLOW_DISSECTOR_KEY_PPPOE);
	const u64 vl = BIT_ULL(FLOW_DISSECTOR_KEY_VLAN);
	struct rtl9602c_test_rule *r;
	struct rtl9602c_cf_xlate x;
	u16 outer;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);

	r->mask.nvlans.num_of_vlans = 0xff;
	r->key.basic.n_proto = htons(0x88b5);
	r->mask.basic.n_proto = htons(0xffff);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv, &x, &outer), 0);
	KUNIT_EXPECT_EQ(test, x.etype, 0x88b5);
	KUNIT_EXPECT_EQ(test, outer, 0);
	KUNIT_EXPECT_EQ(test, x.care,
			(u32)(RTL9602C_CLS_RULE_CTAG | RTL9602C_CLS_RULE_STAG));
	KUNIT_EXPECT_EQ(test, x.value, 0U);
	KUNIT_EXPECT_TRUE(test, rtl9602c_cf_xlate_p1(&x));

	r->mask.basic.n_proto = htons(0xfff8);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv, &x, &outer),
			-EOPNOTSUPP);
	r->key.basic.n_proto = htons(0x05dc);
	r->mask.basic.n_proto = htons(0xffff);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv, &x, &outer),
			-EOPNOTSUPP);
	r->key.basic.n_proto = 0;
	r->mask.basic.n_proto = 0;
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv, &x, &outer), 0);
	KUNIT_EXPECT_EQ(test, x.etype, 0);

	r->key.pppoe.type = htons(ETH_P_PPP_SES);
	r->mask.pppoe.type = htons(0xffff);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv | ppp, &x, &outer), 0);
	KUNIT_EXPECT_EQ(test, x.etype, ETH_P_PPP_SES);
	r->mask.pppoe.session_id = htons(0xffff);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv | ppp, &x, &outer),
			-EOPNOTSUPP);
	r->mask.pppoe.session_id = 0;
	r->key.pppoe.ppp_proto = htons(PPP_IP);
	r->mask.pppoe.ppp_proto = htons(0xffff);
	r->key.basic.n_proto = htons(ETH_P_IP);
	r->mask.basic.n_proto = htons(0xffff);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv | ppp, &x, &outer),
			-EOPNOTSUPP);

	memset(r, 0, sizeof(*r));
	r->mask.nvlans.num_of_vlans = 0xff;
	r->key.basic.ip_proto = IPPROTO_UDP;
	r->mask.basic.ip_proto = 0xff;
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, nv, &x, &outer),
			-EOPNOTSUPP);

	memset(r, 0, sizeof(*r));
	r->key.vlan.vlan_tpid = htons(ETH_P_8021Q);
	r->mask.vlan.vlan_tpid = htons(0xffff);
	r->key.vlan.vlan_id = 410;
	r->mask.vlan.vlan_id = VLAN_VID_MASK;
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, vl, &x, &outer), 0);
	KUNIT_EXPECT_EQ(test, outer, ETH_P_8021Q);
	KUNIT_EXPECT_EQ(test, x.etype, 0);
	r->key.vlan.vlan_eth_type = htons(ETH_P_IP);
	r->mask.vlan.vlan_eth_type = htons(0xffff);
	r->key.basic.n_proto = htons(ETH_P_IP);
	r->mask.basic.n_proto = htons(0xffff);
	KUNIT_EXPECT_EQ(test, rtl9602c_test_match(r, vl, &x, &outer),
			-EOPNOTSUPP);
}

static struct kunit_case rtl9602c_offload_test_cases[] = {
	KUNIT_CASE(rtl9602c_test_napt_example),
	KUNIT_CASE(rtl9602c_test_napt_rem_hash),
	KUNIT_CASE(rtl9602c_test_napt_buckets),
	KUNIT_CASE(rtl9602c_test_flow_fields),
	KUNIT_CASE(rtl9602c_test_l34_flow_records),
#if IS_ENABLED(CONFIG_IPV6)
	KUNIT_CASE(rtl9602c_test_nb6_entry),
	KUNIT_CASE(rtl9602c_test_nb6_bucket),
	KUNIT_CASE(rtl9602c_test_nb6_route),
	KUNIT_CASE(rtl9602c_test_nb6_plan),
	KUNIT_CASE(rtl9602c_test_nb6_wanted),
	KUNIT_CASE(rtl9602c_test_route6_budget),
#endif
	KUNIT_CASE(rtl9602c_test_acl_classes),
	KUNIT_CASE(rtl9602c_test_gem_ds_units),
	KUNIT_CASE(rtl9602c_test_gem_ds_bucket),
	KUNIT_CASE(rtl9602c_test_cf_split_entries),
	KUNIT_CASE(rtl9602c_test_cf_split_which),
	KUNIT_CASE(rtl9602c_test_cf_relayout),
	KUNIT_CASE(rtl9602c_test_cf_depth),
	KUNIT_CASE(rtl9602c_test_cf_forget),
	KUNIT_CASE(rtl9602c_test_cf_patterns),
	KUNIT_CASE(rtl9602c_test_cf_order),
	KUNIT_CASE(rtl9602c_test_cf_gather),
	KUNIT_CASE(rtl9602c_test_cf_planes),
	KUNIT_CASE(rtl9602c_test_vlan_rewrite),
	KUNIT_CASE(rtl9602c_test_vlan_two_tags),
	KUNIT_CASE(rtl9602c_test_vlan_two_tags_flow),
	KUNIT_CASE(rtl9602c_test_vlan_etype),
	{ }
};

static struct kunit_suite rtl9602c_offload_test_suite = {
	.name = "rtl9602c-offload",
	.test_cases = rtl9602c_offload_test_cases,
};

kunit_test_suite(rtl9602c_offload_test_suite);
