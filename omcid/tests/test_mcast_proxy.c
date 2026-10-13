// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the multicast proxy: what the OLT is told of the groups the
 * subscribers hold, how it answers the OLT's queries, how it checks a leave
 * and what the querier of function 2 sends, through operations that record
 * the frames they are given.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mcast/proxy.h"
#include "mcast/report.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"

#include "harness.h"

#define IP4(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

#define FRAMES 64u

static const uint8_t GROUP6[16] = { 0xff, 0x3e, 0, 0, 0, 0, 0,	  0,
				    0,	  0,	0, 0, 0, 0, 0x12, 0x34 };

/* What the operations were given, and what they are to answer. */
static struct {
	uint8_t data[FRAMES][512];
	size_t len[FRAMES];
	struct omci_mcast_tag tag[FRAMES];
	size_t n;
} g_up, g_down;
static uint8_t g_dropped[8][16];
static size_t g_n_dropped;
static bool g_up_ok, g_down_ok, g_have_mac;
static const uint8_t MAC[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x09 };

static bool op_up(void *ctx, const uint8_t *frame, size_t len,
		  const struct omci_mcast_tag *tag)
{
	(void)ctx;
	if (!g_up_ok || g_up.n == FRAMES)
		return false;
	memcpy(g_up.data[g_up.n], frame, len);
	g_up.tag[g_up.n] = *tag;
	g_up.len[g_up.n++] = len;
	return true;
}

static bool op_down(void *ctx, const uint8_t *frame, size_t len,
		    const struct omci_mcast_tag *tag)
{
	(void)ctx;
	if (!g_down_ok || g_down.n == FRAMES)
		return false;
	memcpy(g_down.data[g_down.n], frame, len);
	g_down.tag[g_down.n] = *tag;
	g_down.len[g_down.n++] = len;
	return true;
}

static void op_drop(void *ctx, const uint8_t group[16])
{
	(void)ctx;
	memcpy(g_dropped[g_n_dropped++ % 8u], group, 16);
}

static bool op_mac(void *ctx, uint8_t mac[6])
{
	(void)ctx;
	if (!g_have_mac)
		return false;
	memcpy(mac, MAC, 6);
	return true;
}

static const struct omci_mcast_proxy_ops OPS = {
	.up = op_up,
	.down = op_down,
	.drop = op_drop,
	.mac = op_mac,
};

static struct omci_mcast_proxy g_p;
static const struct omci_mcast_tag NOTAG;

static void up(void)
{
	omci_mcast_proxy_reset(&g_p);
	memset(&g_up, 0, sizeof(g_up));
	memset(&g_down, 0, sizeof(g_down));
	g_n_dropped = 0;
	g_up_ok = g_down_ok = g_have_mac = true;
}

static void a4(uint8_t out[16], uint32_t v)
{
	uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
			 (uint8_t)(v >> 8), (uint8_t)v };

	omci_mcast_addr_set_v4(out, b);
}

static struct omci_mcast_chan chan(uint32_t group, uint32_t source)
{
	struct omci_mcast_chan c = { { 0 }, { 0 } };

	a4(c.group, group);
	if (source != 0u)
		a4(c.source, source);
	return c;
}

static struct omci_mcast_proxy_params params(uint8_t function, uint8_t version)
{
	struct omci_mcast_proxy_params pr = { .function = function,
					      .version = version };

	return pr;
}

/* One frame the proxy made, taken apart. */
static struct omci_mcast_report parsed(const uint8_t *f, size_t len)
{
	struct omci_mcast_report r;

	CHECK(omci_mcast_report_parse(f, len, 0, false, &r));
	return r;
}

static uint32_t ip_of(const uint8_t a[16])
{
	return omci_be32(&a[12]);
}

static void test_a_proxy_that_is_off_tells_nothing(void)
{
	struct omci_mcast_proxy_params pr = params(0, 3);
	struct omci_mcast_chan held[1] = { chan(IP4(239, 1, 1, 5), 0) };

	up();
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 100);
	CHECK_EQ(g_up.n, 0u);
	CHECK_EQ(g_down.n, 0u);
	/* What it was told is forgotten when the function goes off. */
	pr.function = 1;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 101);
	CHECK_EQ(g_up.n, 1u);
	pr.function = 0;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 102);
	CHECK_EQ(g_p.n_told, 0u);
	pr.function = 1;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 103);
	CHECK_EQ(g_up.n, 2u);
}

static void
test_a_group_is_told_once_when_first_held_and_once_when_last_let_go(void)
{
	struct omci_mcast_proxy_params pr = params(1, 3);
	struct omci_mcast_chan held[2] = { chan(IP4(239, 1, 1, 5), 0),
					   chan(IP4(232, 1, 1, 1),
						IP4(10, 1, 1, 1)) };
	struct omci_mcast_report r;

	up();
	/* The first subscriber of a group: a join, as an exclude record. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 100);
	CHECK_EQ(g_up.n, 1u);
	r = parsed(g_up.data[0], g_up.len[0]);
	CHECK_EQ(r.version, 3u);
	CHECK(r.n == 1u && r.rec[0].join);
	CHECK_EQ(ip_of(r.rec[0].group), IP4(239, 1, 1, 5));
	CHECK_EQ(g_up.data[0][46], 4u);
	CHECK_EQ(ip_of(r.client), 0u);

	/* More subscribers of it change nothing the OLT needs to hear. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 101);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 102);
	CHECK_EQ(g_up.n, 1u);

	/* A group from one source joins that source; a second source changes it. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 2, 103);
	CHECK_EQ(g_up.n, 2u);
	r = parsed(g_up.data[1], g_up.len[1]);
	CHECK_EQ(g_up.data[1][46], 3u);
	CHECK(r.n == 2u && !r.rec[0].join && r.rec[1].join);
	CHECK_EQ(ip_of(r.rec[1].source), IP4(10, 1, 1, 1));
	held[1] = chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 2));
	{
		struct omci_mcast_chan both[3] = {
			held[0], chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 1)),
			held[1]
		};

		omci_mcast_proxy_run(&g_p, &pr, &OPS, both, 3, 104);
		CHECK_EQ(g_up.n, 3u);
		r = parsed(g_up.data[2], g_up.len[2]);
		CHECK_EQ(r.n, 3u);
		/* The same sources in another order are no change. */
		both[1] = held[1];
		both[2] = chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 1));
		omci_mcast_proxy_run(&g_p, &pr, &OPS, both, 3, 105);
		CHECK_EQ(g_up.n, 3u);
	}

	/* A group no longer held, and another held from every source, go in one frame. */
	{
		struct omci_mcast_chan any[2] = { chan(IP4(232, 1, 1, 1),
						       IP4(10, 1, 1, 1)),
						  chan(IP4(232, 1, 1, 1), 0) };

		omci_mcast_proxy_run(&g_p, &pr, &OPS, any, 2, 106);
		CHECK_EQ(g_up.n, 4u);
		r = parsed(g_up.data[3], g_up.len[3]);
		CHECK_EQ(g_up.data[3][46], 3u);
		CHECK(r.n == 2u && !r.rec[0].join && r.rec[1].join);
		CHECK_EQ(ip_of(r.rec[1].group), IP4(232, 1, 1, 1));
		CHECK(g_p.told[0].item.any);
	}

	/* The last subscriber lets go: a leave, which is an include record of no source. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 107);
	CHECK_EQ(g_up.n, 5u);
	r = parsed(g_up.data[4], g_up.len[4]);
	CHECK(r.n == 1u && !r.rec[0].join);
	CHECK_EQ(g_up.data[4][46], 3u);
	CHECK_EQ(g_p.n_told, 0u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 108);
	CHECK_EQ(g_up.n, 5u);
}

static void test_the_versions_before_3_tell_only_what_they_can(void)
{
	struct omci_mcast_proxy_params pr = params(1, 2);
	struct omci_mcast_chan some[1] = { chan(IP4(232, 1, 1, 1),
						IP4(10, 1, 1, 1)) };
	struct omci_mcast_chan more[2] = {
		chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 1)),
		chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 2))
	};
	struct omci_mcast_report r;

	up();
	omci_mcast_proxy_run(&g_p, &pr, &OPS, some, 1, 100);
	CHECK_EQ(g_up.n, 1u);
	r = parsed(g_up.data[0], g_up.len[0]);
	CHECK_EQ(r.version, 2u);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_REPORT);
	/* A source more is nothing a version 2 report holds: it is noted, not sent. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, more, 2, 101);
	CHECK_EQ(g_up.n, 1u);
	CHECK_EQ(g_p.told[0].item.n, 2u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 102);
	CHECK_EQ(g_up.n, 2u);
	r = parsed(g_up.data[1], g_up.len[1]);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_LEAVE);

	/* Version 1 has no leave: the group is let go unheard. */
	pr.version = 1;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, some, 1, 103);
	CHECK_EQ(g_up.n, 3u);
	CHECK_EQ(g_up.data[2][38], 0x12u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 104);
	CHECK_EQ(g_up.n, 3u);
	CHECK_EQ(g_p.n_told, 0u);

	/* A profile of MLD takes the highest version of IGMP, and an IGMP one MLD's. */
	pr.version = 16;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, some, 1, 105);
	r = parsed(g_up.data[3], g_up.len[3]);
	CHECK_EQ(r.version, 3u);
	pr.version = 2;
	{
		struct omci_mcast_chan v6[1] = { { { 0 }, { 0 } } };

		memcpy(v6[0].group, GROUP6, 16);
		omci_mcast_proxy_run(&g_p, &pr, &OPS, v6, 1, 106);
		r = parsed(g_up.data[g_up.n - 1u], g_up.len[g_up.n - 1u]);
		CHECK_EQ(r.version, 17u);
		/* MLD version 1, with the profile at it: a done when the group goes. */
		pr.version = 16;
		omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 107);
		r = parsed(g_up.data[g_up.n - 1u], g_up.len[g_up.n - 1u]);
		CHECK_EQ(r.version, 16u);
		CHECK_EQ(r.kind, OMCI_MCAST_KIND_LEAVE);
	}
}

static void test_what_could_not_be_sent_is_sent_again(void)
{
	struct omci_mcast_proxy_params pr = params(1, 3);
	struct omci_mcast_chan held[2] = { chan(IP4(239, 1, 1, 5), 0),
					   chan(IP4(239, 1, 1, 6), 0) };

	up();
	g_up_ok = false;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 2, 100);
	CHECK_EQ(g_up.n, 0u);
	CHECK_EQ(g_p.n_told, 0u);
	g_up_ok = true;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 2, 101);
	CHECK_EQ(g_up.n, 1u);
	CHECK_EQ(g_p.n_told, 2u);

	/* Without an Ethernet address there is no message to make. */
	g_have_mac = false;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 102);
	CHECK_EQ(g_up.n, 1u);
	CHECK_EQ(g_p.n_told, 2u);
	g_have_mac = true;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 103);
	CHECK_EQ(g_p.n_told, 0u);

	/* A family's change that does not fit is given up; the rest go. */
	{
		struct omci_mcast_chan many[OMCI_MCAST_PROXY_GROUPS + 4u];

		for (unsigned int i = 0; i < ARRAY_SIZE(many); i++)
			many[i] = chan(IP4(239, 2, 0, i), 0);
		omci_mcast_proxy_run(&g_p, &pr, &OPS, many, ARRAY_SIZE(many),
				     104);
		CHECK_EQ(g_p.n_told, OMCI_MCAST_PROXY_GROUPS);
		/* At most the table's groups are told; one gone makes room for one more, a run later. */
		omci_mcast_proxy_run(&g_p, &pr, &OPS, many + 1,
				     ARRAY_SIZE(many) - 1u, 105);
		CHECK_EQ(g_p.n_told, OMCI_MCAST_PROXY_GROUPS - 1u);
		omci_mcast_proxy_run(&g_p, &pr, &OPS, many + 1,
				     ARRAY_SIZE(many) - 1u, 106);
		CHECK_EQ(g_p.n_told, OMCI_MCAST_PROXY_GROUPS);
	}
}

static void test_the_sources_of_a_group_are_few(void)
{
	struct omci_mcast_proxy_params pr = params(1, 3);
	struct omci_mcast_chan held[OMCI_MCAST_ITEM_SOURCES + 3u];

	up();
	for (unsigned int i = 0; i < OMCI_MCAST_ITEM_SOURCES + 2u; i++)
		held[i] = chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 1 + i));
	/* A source twice, and more than a report names. */
	held[OMCI_MCAST_ITEM_SOURCES + 2u] = held[0];
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, ARRAY_SIZE(held), 100);
	CHECK_EQ(g_p.told[0].item.n, OMCI_MCAST_ITEM_SOURCES);
	/* The same number of sources, but not the same: told again. */
	{
		const size_t told = g_up.n;
		struct omci_mcast_chan two[2] = {
			chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 1)),
			chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 2))
		};

		omci_mcast_proxy_run(&g_p, &pr, &OPS, two, 2, 101);
		CHECK_EQ(g_p.told[0].item.n, 2u);
		two[1] = chan(IP4(232, 1, 1, 1), IP4(10, 1, 1, 3));
		omci_mcast_proxy_run(&g_p, &pr, &OPS, two, 2, 102);
		CHECK(g_up.n > told + 1u);
	}
	/* Every source, once told, makes the sources more not count. */
	held[0] = chan(IP4(232, 1, 1, 1), 0);
	held[1] = chan(IP4(232, 1, 1, 1), IP4(10, 9, 9, 9));
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 2, 101);
	CHECK(g_p.told[0].item.any);
	CHECK_EQ(g_p.told[0].item.n, 0u);
}

static void test_the_olts_general_query_is_answered_within_its_time(void)
{
	struct omci_mcast_proxy_params pr = params(1, 3);
	struct omci_mcast_chan held[2] = { chan(IP4(239, 1, 1, 5), 0),
					   chan(IP4(239, 1, 1, 6), 0) };
	struct omci_mcast_chan v6[1] = { { { 0 }, { 0 } } };
	struct omci_mcast_report q = { .kind = OMCI_MCAST_KIND_QUERY,
				       .version = 3,
				       .max_resp = 100 };
	struct omci_mcast_report r;
	uint64_t now;

	up();
	memcpy(v6[0].group, GROUP6, 16);
	{
		struct omci_mcast_chan all[3] = { held[0], held[1], v6[0] };

		omci_mcast_proxy_run(&g_p, &pr, &OPS, all, 3, 100);
		CHECK_EQ(g_up.n, 2u);
		g_up.n = 0;

		/* A general query of IGMP: answered with the IPv4 groups, in one frame. */
		omci_mcast_proxy_query(&g_p, &q, 200);
		CHECK(g_p.answering[0] && g_p.answer_at[0] >= 200 &&
		      g_p.answer_at[0] < 210);
		for (now = 200; now < 210 && g_up.n == 0; now++)
			omci_mcast_proxy_run(&g_p, &pr, &OPS, all, 3, now);
		CHECK_EQ(g_up.n, 1u);
		CHECK(now - 1u >= g_p.answer_at[0]);
		CHECK(!g_p.answering[0]);
		r = parsed(g_up.data[0], g_up.len[0]);
		CHECK_EQ(r.n, 2u);
		CHECK_EQ(g_up.data[0][46], 2u);
		omci_mcast_proxy_run(&g_p, &pr, &OPS, all, 3, 300);
		CHECK_EQ(g_up.n, 1u);

		/* One of MLD: the IPv6 group. A query soon again keeps the earlier time. */
		q.version = 17;
		omci_mcast_proxy_query(&g_p, &q, 400);
		CHECK(g_p.answering[1] && !g_p.answering[0]);
		{
			const uint64_t at = g_p.answer_at[1];

			omci_mcast_proxy_query(&g_p, &q, 400);
			CHECK(g_p.answer_at[1] <= at);
			g_p.answer_at[1] = 400;
			omci_mcast_proxy_query(&g_p, &q, 405);
			CHECK_EQ(g_p.answer_at[1], 400u);
		}
		omci_mcast_proxy_run(&g_p, &pr, &OPS, all, 3, 400);
		CHECK_EQ(g_up.n, 2u);
		r = parsed(g_up.data[1], g_up.len[1]);
		CHECK_EQ(r.version, 17u);
		CHECK_EQ(r.n, 1u);

		/* A time of a second or less is not waited for. */
		q.version = 3;
		q.max_resp = 10;
		omci_mcast_proxy_query(&g_p, &q, 500);
		CHECK_EQ(g_p.answer_at[0], 500u);

		/* With no Ethernet address there is no answer to make; with the OLT's side down, it is lost. */
		g_have_mac = false;
		omci_mcast_proxy_run(&g_p, &pr, &OPS, all, 3, 500);
		CHECK_EQ(g_up.n, 2u);
		g_have_mac = true;
		omci_mcast_proxy_query(&g_p, &q, 501);
		g_up_ok = false;
		omci_mcast_proxy_run(&g_p, &pr, &OPS, all, 3, 501);
		CHECK_EQ(g_up.n, 2u);
		CHECK(!g_p.answering[0]);
		g_up_ok = true;
	}
}

static void test_a_query_of_a_group_is_answered_when_the_group_is_held(void)
{
	struct omci_mcast_proxy_params pr = params(1, 2);
	struct omci_mcast_chan held[1] = { chan(IP4(239, 1, 1, 5), 0) };
	struct omci_mcast_report q = { .kind = OMCI_MCAST_KIND_QUERY,
				       .version = 2,
				       .max_resp = 100 };
	struct omci_mcast_report r;

	up();
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 100);
	g_up.n = 0;
	a4(q.query_group, IP4(239, 1, 1, 5));
	omci_mcast_proxy_query(&g_p, &q, 200);
	omci_mcast_proxy_query(&g_p, &q, 200);
	CHECK_EQ(g_p.n_asked, 1u);
	a4(q.query_group, IP4(239, 9, 9, 9));
	omci_mcast_proxy_query(&g_p, &q, 200);
	CHECK_EQ(g_p.n_asked, 2u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 200);
	/* Only the group that is held is answered, with a report of version 2. */
	CHECK_EQ(g_up.n, 1u);
	r = parsed(g_up.data[0], g_up.len[0]);
	CHECK_EQ(r.version, 2u);
	CHECK_EQ(ip_of(r.rec[0].group), IP4(239, 1, 1, 5));
	CHECK_EQ(g_p.n_asked, 0u);

	/* The list of questions is short; a MLD one is for the IPv6 groups. */
	for (unsigned int i = 0; i < OMCI_MCAST_PROXY_ASKED + 2u; i++) {
		a4(q.query_group, IP4(239, 8, 0, i));
		omci_mcast_proxy_query(&g_p, &q, 201);
	}
	CHECK_EQ(g_p.n_asked, OMCI_MCAST_PROXY_ASKED);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 1, 201);
	{
		struct omci_mcast_chan v6[1] = { { { 0 }, { 0 } } };

		memcpy(v6[0].group, GROUP6, 16);
		omci_mcast_proxy_run(&g_p, &pr, &OPS, v6, 1, 202);
		g_up.n = 0;
		q.version = 16;
		memcpy(q.query_group, GROUP6, 16);
		omci_mcast_proxy_query(&g_p, &q, 203);
		omci_mcast_proxy_run(&g_p, &pr, &OPS, v6, 1, 203);
		CHECK_EQ(g_up.n, 1u);
		r = parsed(g_up.data[0], g_up.len[0]);
		CHECK_EQ(r.version, 17u);
	}
}

static void test_a_leave_is_checked_before_the_group_goes(void)
{
	struct omci_mcast_proxy_params pr = params(1, 3);
	uint8_t group[16], other[16];
	struct omci_mcast_report r;

	up();
	a4(group, IP4(239, 1, 1, 5));
	a4(other, IP4(239, 1, 1, 6));
	omci_mcast_proxy_left(&g_p, &pr, group, &NOTAG, 100);
	omci_mcast_proxy_left(&g_p, &pr, group, &NOTAG, 100);
	CHECK_EQ(g_p.n_check, 1u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 100);
	/* A query of the group to the subscribers, then the next a second later. */
	CHECK_EQ(g_down.n, 1u);
	r = parsed(g_down.data[0], g_down.len[0]);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(ip_of(r.query_group), IP4(239, 1, 1, 5));
	CHECK_EQ(r.max_resp, 10u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 100);
	CHECK_EQ(g_down.n, 1u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 101);
	CHECK_EQ(g_down.n, 2u);
	CHECK_EQ(g_n_dropped, 0u);
	/* Nobody answered: the group goes, and the check with it. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 102);
	CHECK_EQ(g_down.n, 2u);
	CHECK_EQ(g_n_dropped, 1u);
	CHECK_MEM(g_dropped[0], group, 16);
	CHECK_EQ(g_p.n_check, 0u);

	/* A join in the meantime ends the check; one of another group does not. */
	omci_mcast_proxy_left(&g_p, &pr, group, &NOTAG, 200);
	omci_mcast_proxy_left(&g_p, &pr, other, &NOTAG, 200);
	omci_mcast_proxy_joined(&g_p, other);
	omci_mcast_proxy_joined(&g_p, other);
	CHECK_EQ(g_p.n_check, 1u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 200);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 201);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 202);
	CHECK_EQ(g_n_dropped, 2u);
	CHECK_MEM(g_dropped[1], group, 16);

	/* The robustness and the last member timer of the profile; no check at all with fast leave or function 0. */
	pr.robustness = 3;
	pr.last_member = 25;
	g_down.n = 0;
	omci_mcast_proxy_left(&g_p, &pr, group, &NOTAG, 300);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 300);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 302);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 303);
	CHECK_EQ(g_down.n, 2u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 305);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 308);
	CHECK_EQ(g_down.n, 3u);
	CHECK_EQ(g_n_dropped, 2u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 311);
	CHECK_EQ(g_n_dropped, 3u);
	pr.fast_leave = true;
	omci_mcast_proxy_left(&g_p, &pr, group, &NOTAG, 400);
	CHECK_EQ(g_p.n_check, 0u);
	pr.fast_leave = false;
	pr.function = 0;
	omci_mcast_proxy_left(&g_p, &pr, group, &NOTAG, 400);
	CHECK_EQ(g_p.n_check, 0u);

	/* The table of checks is short. */
	pr.function = 1;
	for (unsigned int i = 0; i < OMCI_MCAST_PROXY_CHECKS + 2u; i++) {
		a4(other, IP4(239, 3, 0, i));
		omci_mcast_proxy_left(&g_p, &pr, other, &NOTAG, 500);
	}
	CHECK_EQ(g_p.n_check, OMCI_MCAST_PROXY_CHECKS);

	/* A check of an IPv6 group asks with MLD; an unsendable query is not the end of it. */
	up();
	omci_mcast_proxy_left(&g_p, &pr, GROUP6, &NOTAG, 600);
	g_down_ok = false;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 600);
	CHECK_EQ(g_down.n, 0u);
	g_down_ok = true;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 603);
	CHECK_EQ(g_down.n, 1u);
	r = parsed(g_down.data[0], g_down.len[0]);
	CHECK_EQ(r.version, 17u);
	CHECK_MEM(r.query_group, GROUP6, 16);
}

static void test_function_2_asks_the_subscribers_itself(void)
{
	struct omci_mcast_proxy_params pr = params(2, 3);
	struct omci_mcast_report r;

	up();
	pr.querier[0] = 10;
	pr.querier[3] = 77;
	pr.robustness = 4;
	pr.query_interval = 60;
	pr.query_response = 50;
	/* Queries of both protocols at once, then again each interval. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 1000);
	CHECK_EQ(g_down.n, 2u);
	r = parsed(g_down.data[0], g_down.len[0]);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(r.version, 3u);
	CHECK_EQ(r.max_resp, 50u);
	CHECK_EQ(ip_of(r.client), IP4(10, 0, 0, 77));
	CHECK_EQ(g_down.data[0][46], 4u);
	CHECK_EQ(g_down.data[0][47], 60u);
	r = parsed(g_down.data[1], g_down.len[1]);
	CHECK_EQ(r.version, 17u);
	CHECK_EQ(r.max_resp, 50u);
	/* Its address is the link-local one of the Ethernet address. */
	CHECK_EQ(g_down.data[1][22], 0xfeu);
	CHECK_EQ(g_down.data[1][23], 0x80u);
	CHECK_EQ(g_down.data[1][30], 0x02u);
	CHECK_EQ(g_down.data[1][33], 0xffu);
	CHECK_EQ(g_down.data[1][34], 0xfeu);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 1059);
	CHECK_EQ(g_down.n, 2u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 1060);
	CHECK_EQ(g_down.n, 4u);

	/* The defaults of the timers, and no query from function 1. */
	up();
	pr = params(2, 2);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 2000);
	r = parsed(g_down.data[0], g_down.len[0]);
	CHECK_EQ(r.version, 2u);
	CHECK_EQ(r.max_resp, 100u);
	CHECK_EQ(ip_of(r.client), 0u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 2124);
	CHECK_EQ(g_down.n, 2u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 2125);
	CHECK_EQ(g_down.n, 4u);
	up();
	pr.function = 1;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 3000);
	CHECK_EQ(g_down.n, 0u);

	/* No Ethernet address, no query; a query that cannot be sent waits for the next time. */
	up();
	pr.function = 2;
	g_have_mac = false;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 4000);
	CHECK_EQ(g_down.n, 0u);
	g_have_mac = true;
	g_down_ok = false;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 4001);
	CHECK_EQ(g_down.n, 0u);
}

/* A group and source held in a tag. */
static struct omci_mcast_chan chan_in(uint32_t group, uint32_t source,
				      uint16_t vid, uint8_t pcp)
{
	struct omci_mcast_chan c = chan(group, source);

	c.tag.tagged = true;
	c.tag.vid = vid;
	c.tag.pcp = pcp;
	return c;
}

static void test_a_group_is_told_in_the_tag_it_was_joined_in(void)
{
	struct omci_mcast_proxy_params pr = params(1, 3);
	struct omci_mcast_chan held[4] = {
		chan_in(IP4(239, 1, 1, 5), 0, 421, 4),
		chan_in(IP4(239, 1, 1, 6), 0, 421, 4),
		chan_in(IP4(239, 1, 1, 7), 0, 840, 2),
		chan_in(IP4(239, 1, 1, 5), 0, 840, 2),
	};
	struct omci_mcast_report q = { .kind = OMCI_MCAST_KIND_QUERY,
				       .version = 3,
				       .max_resp = 10 };
	struct omci_mcast_report r;
	uint8_t group[16];

	up();
	/* One frame for each tag: the same group in two tags is two groups. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 4, 100);
	CHECK_EQ(g_up.n, 2u);
	CHECK_EQ(g_p.n_told, 4u);
	CHECK(g_up.tag[0].tagged && g_up.tag[0].vid == 421 &&
	      g_up.tag[0].pcp == 4);
	CHECK(g_up.tag[1].tagged && g_up.tag[1].vid == 840 &&
	      g_up.tag[1].pcp == 2);
	r = parsed(g_up.data[0], g_up.len[0]);
	CHECK_EQ(r.n, 2u);
	r = parsed(g_up.data[1], g_up.len[1]);
	CHECK_EQ(r.n, 2u);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held, 4, 101);
	CHECK_EQ(g_up.n, 2u);

	/* Let go in one tag only: a leave in that tag, and the group stays held in the other. */
	held[0] = held[1];
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held + 1, 3, 102);
	CHECK_EQ(g_up.n, 3u);
	CHECK(g_up.tag[2].tagged && g_up.tag[2].vid == 421);
	r = parsed(g_up.data[2], g_up.len[2]);
	CHECK(r.n == 1u && !r.rec[0].join);
	CHECK_EQ(ip_of(r.rec[0].group), IP4(239, 1, 1, 5));
	CHECK_EQ(g_p.n_told, 3u);

	/* A query of the OLT is answered in the tag of each group; one of a group in the tag it is held in. */
	g_up.n = 0;
	omci_mcast_proxy_query(&g_p, &q, 200);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held + 1, 3, 200);
	CHECK_EQ(g_up.n, 2u);
	CHECK(g_up.tag[0].vid != g_up.tag[1].vid);
	g_up.n = 0;
	a4(q.query_group, IP4(239, 1, 1, 5));
	omci_mcast_proxy_query(&g_p, &q, 201);
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held + 1, 3, 201);
	CHECK_EQ(g_up.n, 1u);
	CHECK(g_up.tag[0].tagged && g_up.tag[0].vid == 840);

	/* A leave is checked in the tag it came in, and the general queries of function 2 go in each tag. */
	a4(group, IP4(239, 1, 1, 7));
	{
		const struct omci_mcast_tag tag = { .tagged = true,
						    .vid = 840,
						    .pcp = 2 };

		omci_mcast_proxy_left(&g_p, &pr, group, &tag, 300);
	}
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held + 1, 3, 300);
	CHECK_EQ(g_down.n, 1u);
	CHECK(g_down.tag[0].tagged && g_down.tag[0].vid == 840 &&
	      g_down.tag[0].pcp == 2);
	omci_mcast_proxy_joined(&g_p, group);
	g_down.n = 0;
	pr.function = 2;
	g_p.query_at = 0;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, held + 1, 3, 400);
	/* IPv4 in the two tags, IPv6 where nothing is held untagged. */
	CHECK_EQ(g_down.n, 3u);
	CHECK(g_down.tag[0].tagged && g_down.tag[1].tagged &&
	      !g_down.tag[2].tagged);
}

static void test_the_jitter_of_the_answers_differs_with_the_address(void)
{
	struct omci_mcast_proxy_params pr = params(1, 3);
	struct omci_mcast_proxy a, b;
	struct omci_mcast_report q = { .kind = OMCI_MCAST_KIND_QUERY,
				       .version = 3,
				       .max_resp = 1000 };
	uint64_t seen = 0;

	up();
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 100);
	a = g_p;
	CHECK(a.seeded);
	/* The seed is the address, once: a second run leaves the state alone. */
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 101);
	CHECK_EQ(g_p.rng, a.rng);
	omci_mcast_proxy_reset(&b);
	CHECK(!b.seeded);
	CHECK(a.rng != b.rng);
	/* The answers fall at many different seconds of the time allowed. */
	for (unsigned int i = 0; i < 40; i++) {
		g_p.answering[0] = false;
		omci_mcast_proxy_query(&g_p, &q, 0);
		CHECK(g_p.answer_at[0] < 100u);
		seen |= (uint64_t)1 << (g_p.answer_at[0] % 64u);
	}
	CHECK(__builtin_popcountll(seen) > 10);
	/* A state that the address mixes to zero, which a shift of bits could not leave, is put right. */
	omci_mcast_proxy_reset(&g_p);
	g_p.rng = 0xeb002108u;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 100);
	CHECK(g_p.rng != 0u);
	/* Without an address the proxy goes on, unseeded. */
	omci_mcast_proxy_reset(&g_p);
	g_have_mac = false;
	omci_mcast_proxy_run(&g_p, &pr, &OPS, NULL, 0, 100);
	CHECK(!g_p.seeded);
}

int main(void)
{
	RUN(test_a_proxy_that_is_off_tells_nothing);
	RUN(test_a_group_is_told_once_when_first_held_and_once_when_last_let_go);
	RUN(test_the_versions_before_3_tell_only_what_they_can);
	RUN(test_what_could_not_be_sent_is_sent_again);
	RUN(test_the_sources_of_a_group_are_few);
	RUN(test_the_olts_general_query_is_answered_within_its_time);
	RUN(test_a_query_of_a_group_is_answered_when_the_group_is_held);
	RUN(test_a_leave_is_checked_before_the_group_goes);
	RUN(test_function_2_asks_the_subscribers_itself);
	RUN(test_a_group_is_told_in_the_tag_it_was_joined_in);
	RUN(test_the_jitter_of_the_answers_differs_with_the_address);
	return OMCI_T_REPORT("test_mcast_proxy");
}
