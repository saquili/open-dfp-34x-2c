// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the multicast agent: the IGMP and MLD messages taken apart, and
 * the OLT's policy over the groups of the subscriber side, on the host
 * backend's bridge.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "mcast/acl.h"
#include "mcast/agent.h"
#include "mcast/report.h"
#include "me/bridge.h"
#include "me/defaults.h"
#include "me/gem.h"
#include "me/tables.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"

#include "harness.h"

#define PORT 0x0101u
#define PROFILE 0x0201u
#define PROFILE_B 0x0202u

#define IP4(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

static struct omci_mib *g_mib;
static struct omci_mcast *g_m;
static uint64_t g_now;

static void put16(uint8_t *p, unsigned int v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t be32_at(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

/* An IPv4 address as the agent keeps it. */
static void a4(uint8_t out[16], uint32_t v)
{
	uint8_t b[4];

	put32(b, v);
	omci_mcast_addr_set_v4(out, b);
}

/* ---- Messages ------------------------------------------------------- */

/* The priority of the tag eth() writes, and its TPID. */
static unsigned int g_pcp = 5;
static unsigned int g_tpid = 0x8100u;

/* The Ethernet header, with a tag when @vid is not negative; the offset after it. */
static size_t eth(uint8_t *f, int vid, unsigned int type)
{
	static const uint8_t mac[12] = { 0x01, 0x00, 0x5e, 0x01, 0x01, 0x01,
					 0x00, 0x00, 0x5e, 0x00, 0x53, 0x07 };
	size_t at = 12;

	memcpy(f, mac, sizeof(mac));
	if (vid >= 0) {
		put16(&f[at], g_tpid);
		put16(&f[at + 2], (g_pcp << 13) | (unsigned int)vid);
		at += 4;
	}
	put16(&f[at], type);
	return at + 2;
}

/* An IPv4 header with the router alert option, for @len bytes of IGMP. */
static size_t ip4(uint8_t *p, uint32_t client, uint32_t dst, size_t len)
{
	memset(p, 0, 24);
	p[0] = 0x46u;
	put16(&p[2], (unsigned int)(24u + len));
	p[8] = 1u;
	p[9] = 2u;
	put32(&p[12], client);
	put32(&p[16], dst);
	p[20] = 0x94u;
	p[21] = 0x04u;
	return 24;
}

/* An IGMPv1 or v2 message: a report (0x12, 0x16) or a leave (0x17). */
static size_t igmp(uint8_t *f, int vid, uint8_t type, uint32_t client,
		   uint32_t group)
{
	size_t at = eth(f, vid, 0x0800u);

	at += ip4(&f[at], client, group, 8);
	memset(&f[at], 0, 8);
	f[at] = type;
	put32(&f[at + 4], group);
	return at + 8;
}

/* An IGMPv3 report with one record of @type, for @nsrc sources counting from @source. */
static size_t igmp3(uint8_t *f, int vid, uint32_t client, uint8_t type,
		    uint32_t group, unsigned int nsrc, uint32_t source)
{
	size_t at = eth(f, vid, 0x0800u);
	const size_t len = 8u + 8u + 4u * nsrc;

	at += ip4(&f[at], client, IP4(224, 0, 0, 22), len);
	memset(&f[at], 0, len);
	f[at] = 0x22u;
	put16(&f[at + 6], 1u);
	f[at + 8] = type;
	put16(&f[at + 10], nsrc);
	put32(&f[at + 12], group);
	for (unsigned int i = 0; i < nsrc; i++)
		put32(&f[at + 16 + 4 * i], source + i);
	return at + len;
}

/* An MLD message after a hop-by-hop header: v1 (131, 132) or a v2 report of one record. */
static size_t mld(uint8_t *f, int vid, uint8_t type, const uint8_t *client,
		  const uint8_t *group, uint8_t record)
{
	size_t at = eth(f, vid, 0x86ddu);
	uint8_t *ip = &f[at];
	const size_t body = type == 143u ? 8u + 20u : 24u;

	memset(ip, 0, 40 + 8 + body);
	ip[0] = 0x60u;
	put16(&ip[4], (unsigned int)(8u + body));
	ip[6] = 0u;
	ip[7] = 1u;
	memcpy(&ip[8], client, 16);
	memcpy(&ip[24], group, 16);
	ip[40] = 58u;
	ip[42] = 5u;
	ip[43] = 2u;
	ip[48] = type;
	if (type == 143u) {
		put16(&ip[54], 1u);
		ip[56] = record;
		memcpy(&ip[60], group, 16);
	} else {
		memcpy(&ip[56], group, 16);
	}
	return at + 48 + body;
}

static void test_messages_are_taken_apart(void)
{
	uint8_t f[256], g[16], s[16];
	struct omci_mcast_report r;
	size_t n;

	/* IGMPv2 report, tagged in the frame. */
	n = igmp(f, 410, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_REPORT);
	CHECK_EQ(r.version, 2u);
	CHECK(r.tagged);
	CHECK_EQ(r.vid, 410u);
	CHECK_EQ(r.pcp, 5u);
	CHECK_EQ(r.n, 1u);
	CHECK(r.rec[0].join);
	a4(g, IP4(239, 1, 1, 5));
	CHECK_MEM(r.rec[0].group, g, 16);
	a4(s, IP4(10, 0, 0, 2));
	CHECK_MEM(r.client, s, 16);

	/* The tag the receive path took off wins over one left in the frame. */
	CHECK(omci_mcast_report_parse(f, n, 0x0064u, true, &r));
	CHECK_EQ(r.vid, 100u);
	CHECK_EQ(r.pcp, 0u);

	/* IGMPv1 report, untagged; IGMPv2 leave. */
	n = igmp(f, -1, 0x12u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 1u);
	CHECK(!r.tagged);
	n = igmp(f, -1, 0x17u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_LEAVE);
	CHECK(!r.rec[0].join);

	/* A query is IGMP, but nothing to count: a general one of version 1 (a zero response time) or 2. */
	n = igmp(f, -1, 0x11u, IP4(10, 0, 0, 1), 0);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(r.n, 0u);
	CHECK_EQ(r.version, 1u);
	CHECK_EQ(r.max_resp, 100u);
	CHECK_EQ(be32_at(&r.query_group[12]), 0u);
	f[14 + 24 + 1] = 50u;
	put32(&f[14 + 24 + 4], IP4(239, 1, 1, 5));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 2u);
	CHECK_EQ(r.max_resp, 50u);
	CHECK_EQ(be32_at(&r.query_group[12]), IP4(239, 1, 1, 5));

	/* An IGMP message of a type that is not taken apart is none to count either. */
	n = igmp(f, -1, 0x13u, IP4(10, 0, 0, 1), IP4(224, 0, 0, 1));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_OTHER);
	CHECK_EQ(r.n, 0u);

	/* Version 3 is the longer message, its response time a small number or a floating point one. */
	n = igmp3(f, -1, IP4(10, 0, 0, 1), 0u, 0, 0, 0);
	f[14 + 24] = 0x11u;
	f[14 + 24 + 1] = 100u;
	put16(&f[14 + 24 + 6], 0u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(r.version, 3u);
	CHECK_EQ(r.max_resp, 100u);
	f[14 + 24 + 1] = 0xa2u;
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.max_resp, 576u);

	/* IGMPv3: exclude mode joins every source. */
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 4u, IP4(239, 1, 1, 6), 1,
		  IP4(10, 9, 9, 9));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 3u);
	CHECK_EQ(r.n, 1u);
	CHECK(r.rec[0].join);
	CHECK_EQ(be32_at(&r.rec[0].source[12]), 0u);
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 2u, IP4(239, 1, 1, 6), 0, 0);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK(r.n == 1u && r.rec[0].join);

	/* Include mode joins each source; to include, it leaves the group first. */
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 1u, IP4(232, 1, 1, 1), 2,
		  IP4(10, 1, 1, 1));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.n, 2u);
	CHECK(r.rec[0].join && r.rec[1].join);
	CHECK_EQ(be32_at(&r.rec[1].source[12]), IP4(10, 1, 1, 2));
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 3u, IP4(232, 1, 1, 1), 1,
		  IP4(10, 1, 1, 1));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.n, 2u);
	CHECK(!r.rec[0].join && r.rec[1].join);
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 3u, IP4(232, 1, 1, 1), 0, 0);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK(r.n == 1u && !r.rec[0].join);
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 1u, IP4(232, 1, 1, 1), 0, 0);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK(r.n == 1u && !r.rec[0].join);

	/* Allow and block; a record type that is not known is skipped. */
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 5u, IP4(232, 1, 1, 1), 1,
		  IP4(10, 1, 1, 7));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK(r.n == 1u && r.rec[0].join);
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 6u, IP4(232, 1, 1, 1), 1,
		  IP4(10, 1, 1, 7));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK(r.n == 1u && !r.rec[0].join);
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 9u, IP4(232, 1, 1, 1), 1,
		  IP4(10, 1, 1, 7));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.n, 0u);

	/* More records than are kept: the rest are counted as cut. */
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 1u, IP4(232, 1, 1, 1), 20,
		  IP4(10, 1, 1, 1));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.n, OMCI_MCAST_REPORT_RECORDS);
	CHECK_EQ(r.cut, 4u);

	/* MLD: v1 report and done, v2 report. */
	static const uint8_t client6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
					     0,	   0,	 0, 0, 0, 0, 0, 2 };
	static const uint8_t group6[16] = {
		0xff, 0x3e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34
	};

	n = mld(f, 410, 131u, client6, group6, 0);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 16u);
	CHECK(r.n == 1u && r.rec[0].join);
	CHECK_MEM(r.rec[0].group, group6, 16);
	CHECK_MEM(r.client, client6, 16);
	n = mld(f, -1, 132u, client6, group6, 0);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_LEAVE);
	n = mld(f, -1, 143u, client6, group6, 4u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 17u);
	CHECK(r.n == 1u && r.rec[0].join);
	n = mld(f, -1, 130u, client6, group6, 0);
	put16(&f[14 + 48 + 4], 5000u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(r.version, 16u);
	CHECK_EQ(r.max_resp, 50u);
	CHECK_MEM(r.query_group, group6, 16);
	/* An ICMPv6 message after the options that is not MLD is none to count. */
	n = mld(f, -1, 133u, client6, group6, 0);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_OTHER);
	n = mld(f, -1, 130u, client6, group6, 0);
	put16(&f[14 + 48 + 4], 5000u);
	/* A longer one is version 2, its response time in milliseconds as a small or floating point number. */
	{
		uint8_t q2[128];

		memcpy(q2, f, n);
		memset(&q2[n], 0, 4);
		put16(&q2[14 + 4], 8u + 28u);
		put16(&q2[14 + 48 + 4], 0x9000u);
		CHECK(omci_mcast_report_parse(q2, n + 4, 0, false, &r));
		CHECK_EQ(r.version, 17u);
		CHECK_EQ(r.max_resp, (((0x1000u | 0u) << 4) + 99u) / 100u);
		/* One cut short of the group is not a query. */
		put16(&f[14 + 4], 8u + 20u);
		CHECK(!omci_mcast_report_parse(f, n - 4, 0, false, &r));
	}

	/* An MLD message without the hop-by-hop header. */
	n = mld(f, -1, 131u, client6, group6, 0);
	f[14 + 6] = 58u;
	memmove(&f[14 + 40], &f[14 + 48], 24);
	put16(&f[14 + 4], 24u);
	CHECK(omci_mcast_report_parse(f, n - 8, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_REPORT);
}

static void test_frames_that_are_not_messages_are_refused(void)
{
	static const uint8_t client6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
					     0,	   0,	 0, 0, 0, 0, 0, 2 };
	static const uint8_t group6[16] = {
		0xff, 0x3e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34
	};
	struct omci_mcast_report r;
	uint8_t f[256];
	size_t n;

	CHECK(!omci_mcast_report_parse(NULL, 60, 0, false, &r));
	CHECK(!omci_mcast_report_parse(f, 13, 0, false, &r));

	/* Two tags, then one too many; a tag cut short. */
	n = igmp(f, 410, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	memmove(&f[16], &f[12], n - 12);
	put16(&f[12], 0x88a8u);
	put16(&f[14], 10u);
	CHECK(omci_mcast_report_parse(f, n + 4, 0, false, &r));
	CHECK_EQ(r.vid, 10u);
	memmove(&f[16], &f[12], n + 4 - 12);
	put16(&f[12], 0x8100u);
	CHECK(!omci_mcast_report_parse(f, n + 8, 0, false, &r));
	CHECK(!omci_mcast_report_parse(f, 17, 0, false, &r));

	/* Neither IPv4 nor IPv6. */
	n = igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	put16(&f[12], 0x0806u);
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));

	/* IPv4 that is short, not version 4, a bad header length, not IGMP, or longer than the frame. */
	n = igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	CHECK(!omci_mcast_report_parse(f, 14 + 19, 0, false, &r));
	f[14] = 0x56u;
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));
	f[14] = 0x44u;
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));
	f[14] = 0x46u;
	f[14 + 9] = 17u;
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));
	f[14 + 9] = 2u;
	CHECK(!omci_mcast_report_parse(f, n - 1, 0, false, &r));
	put16(&f[14 + 2], 24u + 7u);
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));

	/* IGMPv3 records that run past the end. */
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 1u, IP4(232, 1, 1, 1), 2,
		  IP4(10, 1, 1, 1));
	put16(&f[14 + 24 + 6], 2u);
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));
	put16(&f[14 + 24 + 6], 1u);
	put16(&f[14 + 24 + 10], 3u);
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));

	/* IPv6 that is short, not version 6, longer than the frame, its options cut, not ICMPv6, or MLD cut short. */
	n = mld(f, -1, 131u, client6, group6, 0);
	CHECK(!omci_mcast_report_parse(f, 14 + 39, 0, false, &r));
	f[14] = 0x40u;
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));
	f[14] = 0x60u;
	CHECK(!omci_mcast_report_parse(f, n - 1, 0, false, &r));
	put16(&f[14 + 4], 4u);
	CHECK(!omci_mcast_report_parse(f, 14 + 44, 0, false, &r));
	put16(&f[14 + 4], 8u);
	f[14 + 41] = 1u;
	CHECK(!omci_mcast_report_parse(f, 14 + 48, 0, false, &r));
	f[14 + 41] = 0u;
	f[14 + 40] = 17u;
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));
	f[14 + 40] = 58u;
	CHECK(!omci_mcast_report_parse(f, 14 + 48, 0, false, &r));
	put16(&f[14 + 4], 8u + 12u);
	CHECK(!omci_mcast_report_parse(f, 14 + 48 + 12, 0, false, &r));
	n = mld(f, -1, 143u, client6, group6, 4u);
	put16(&f[14 + 48 + 6], 2u);
	CHECK(!omci_mcast_report_parse(f, n, 0, false, &r));
}

/* ---- The agent ------------------------------------------------------ */

static void up(void)
{
	omci_hw_mcast_reset();
	omci_hw_ops_set(NULL);
	g_mib = NULL;
	g_m = NULL;
	CHECK_EQ(omci_mib_init(&g_mib), OMCI_OK);
	CHECK_EQ(omci_mcast_init(&g_m), OMCI_OK);
	g_now = 1000;
}

static void down(void)
{
	omci_mcast_fini(g_m);
	omci_mib_fini(g_mib);
	omci_hw_ops_set(NULL);
	omci_hw_mcast_reset();
}

static void me(uint16_t cls, uint16_t inst)
{
	CHECK_EQ(omci_mib_create(g_mib, cls, inst), OMCI_OK);
}

static void set_n(uint16_t cls, uint16_t inst, uint8_t attr, uint32_t v,
		  size_t size)
{
	uint8_t b[4];

	for (size_t i = 0; i < size; i++)
		b[i] = (uint8_t)(v >> (8u * (size - 1u - i)));
	CHECK_EQ(omci_mib_attr_set_by_olt(g_mib, cls, inst, attr, b, size),
		 OMCI_OK);
}

static uint32_t get32(uint16_t cls, uint16_t inst, uint8_t attr)
{
	uint8_t b[4] = { 0 };
	size_t len = 0;

	(void)!omci_mib_attr_get(g_mib, cls, inst, attr, b, sizeof(b), &len);
	return be32_at(b);
}

/*
 * The subscriber side: the UNI's bridge port and a profile; with @policy a
 * multicast subscriber config info that points at it; a monitor.
 */
static void side(bool policy)
{
	me(47, PORT);
	set_n(47, PORT, 3, 1, 1);
	me(309, PROFILE);
	set_n(309, PROFILE, 1, 3, 1);
	if (policy) {
		me(310, PORT);
		set_n(310, PORT, 2, PROFILE, 2);
	}
	me(311, PORT);
}

static void table_row(uint16_t cls, uint16_t inst, uint8_t attr,
		      const uint8_t *row, size_t size)
{
	struct omci_me_table_policy policy;

	CHECK(omci_me_table_policy(cls, attr, &policy));
	CHECK_EQ(omci_me_table_ctrl_set(g_mib, cls, inst, attr, &policy, row,
					size),
		 OMCI_OK);
}

/* An access control list row (part 0) of a profile; a part 1 with a preview. */
static void acl(uint16_t profile, uint8_t attr, unsigned int key,
		uint32_t source, uint32_t first, uint32_t last, uint32_t bw,
		unsigned int vlan)
{
	uint8_t row[24] = { 0 };

	row[0] = (uint8_t)(0x40u | key >> 8);
	row[1] = (uint8_t)key;
	put16(&row[2], 4095u);
	put16(&row[4], vlan);
	put32(&row[6], source);
	put32(&row[10], first);
	put32(&row[14], last);
	put32(&row[18], bw);
	table_row(309, profile, attr, row, sizeof(row));
}

static void preview_of(uint16_t profile, unsigned int key, unsigned int length,
		       unsigned int repeat_time, unsigned int repeat_count,
		       unsigned int reset)
{
	uint8_t row[24] = { 0 };

	row[0] = (uint8_t)(0x48u | key >> 8);
	row[1] = (uint8_t)key;
	put16(&row[14], length);
	put16(&row[16], repeat_time);
	put16(&row[18], repeat_count);
	put16(&row[20], reset);
	table_row(309, profile, 7, row, sizeof(row));
}

static void delete_row(uint16_t cls, uint16_t inst, uint8_t attr,
		       unsigned int key, size_t size)
{
	uint8_t row[24] = { 0 };

	row[0] = (uint8_t)(0x80u | key >> 8);
	row[1] = (uint8_t)key;
	table_row(cls, inst, attr, row, size);
}

/* A group of the bridge, joined or left there as the bridge's snooping would. */
static void snoop(uint32_t group, uint32_t source, bool join)
{
	struct omci_hw_mcast_group g = { .vid = 0 };

	a4(g.group, group);
	if (source != 0u)
		a4(g.source, source);
	omci_hw_mcast_snoop(&g, join);
}

static void send(const uint8_t *f, size_t n)
{
	omci_hw_mcast_receive(f, n, false, 0);
}

/* A client joins with IGMPv2, the bridge snoops it, the agent answers. */
static void join(uint32_t client, uint32_t group, int vid)
{
	uint8_t f[128];

	send(f, igmp(f, vid, 0x16u, client, group));
	snoop(group, 0, true);
	omci_mcast_service(g_m, g_mib, g_now);
}

/* Whether the bridge holds a group, and how many groups it holds. */
static size_t bridge(uint32_t group, uint32_t source, bool *held)
{
	struct omci_hw_mcast_group g[OMCI_HW_MCAST_MAX_GROUPS];
	uint8_t want[16], from[16] = { 0 };
	size_t n = 0;

	a4(want, group);
	if (source != 0u)
		a4(from, source);
	CHECK_EQ(omci_hw_mcast_groups(g, ARRAY_SIZE(g), &n), OMCI_OK);
	if (held != NULL) {
		*held = false;
		for (size_t i = 0; i < n; i++)
			*held = *held || (memcmp(g[i].group, want, 16) == 0 &&
					  memcmp(g[i].source, from, 16) == 0);
	}
	return n;
}

static bool held(uint32_t group)
{
	bool h;

	(void)bridge(group, 0, &h);
	return h;
}

static size_t listed(uint16_t inst, bool v6)
{
	return (v6 ? omci_mcast_ipv6_groups : omci_mcast_ipv4_groups)
		.count(inst);
}

static void test_without_a_policy_every_group_is_allowed(void)
{
	uint8_t row[64];

	up();
	side(false);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), 410);
	CHECK(held(IP4(239, 1, 1, 5)));
	CHECK_EQ(get32(311, PORT, 3), 1u);
	CHECK_EQ(listed(PORT, false), 1u);
	CHECK_EQ(listed(PORT, true), 0u);

	g_now += 7;
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK(omci_mcast_ipv4_groups.entry(PORT, 0, row, sizeof(row)));
	CHECK_EQ(row[0] << 8 | row[1], 410);
	CHECK_EQ(be32_at(&row[2]), 0u);
	CHECK_EQ(be32_at(&row[6]), IP4(239, 1, 1, 5));
	CHECK_EQ(be32_at(&row[10]), 0u);
	CHECK_EQ(be32_at(&row[14]), IP4(10, 0, 0, 2));
	CHECK_EQ(be32_at(&row[18]), 7u);
	CHECK(!omci_mcast_ipv4_groups.entry(PORT, 1, row, sizeof(row)));
	CHECK(!omci_mcast_ipv4_groups.entry(PORT, 0, row, 23));
	CHECK(!omci_mcast_ipv6_groups.entry(PORT, 0, row, sizeof(row)));

	/* An IPv6 group, in the IPv6 list. */
	static const uint8_t client6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
					     0,	   0,	 0, 0, 0, 0, 0, 2 };
	static const uint8_t group6[16] = {
		0xff, 0x3e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34
	};
	struct omci_hw_mcast_group g6 = { .vid = 0 };
	uint8_t f[160], zero[16] = { 0 };

	send(f, mld(f, 410, 143u, client6, group6, 4u));
	memcpy(g6.group, group6, 16);
	omci_hw_mcast_snoop(&g6, true);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, true), 1u);
	CHECK(omci_mcast_ipv6_groups.entry(PORT, 0, row, sizeof(row)));
	CHECK_EQ(row[0] << 8 | row[1], 410);
	CHECK_MEM(&row[2], zero, 16);
	CHECK_MEM(&row[18], group6, 16);
	CHECK_MEM(&row[38], client6, 16);
	CHECK(!omci_mcast_ipv6_groups.entry(PORT, 0, row, 57));

	/* A monitor of another port lists nothing. */
	me(47, 0x0202u);
	set_n(47, 0x0202u, 3, 5, 1);
	me(311, 0x0202u);
	CHECK_EQ(listed(0x0202u, false), 0u);
	CHECK_EQ(listed(0x0303u, false), 0u);
	down();
}

static void test_the_access_control_list_decides(void)
{
	up();
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);

	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	CHECK(held(IP4(239, 1, 1, 5)));
	join(IP4(10, 0, 0, 2), IP4(239, 2, 2, 2), -1);
	CHECK(!held(IP4(239, 2, 2, 2)));
	CHECK_EQ(get32(311, PORT, 3), 1u);
	CHECK_EQ(listed(PORT, false), 1u);

	/* A group the bridge holds without a message heard is judged too. */
	snoop(IP4(239, 1, 1, 9), 0, true);
	snoop(IP4(239, 3, 3, 3), 0, true);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK(held(IP4(239, 1, 1, 9)));
	CHECK(!held(IP4(239, 3, 3, 3)));
	CHECK_EQ(listed(PORT, false), 2u);

	/* The list loses the row: the groups end. */
	delete_row(309, PROFILE, 7, 1, 24);
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 1, 1, 5)));
	CHECK(!held(IP4(239, 1, 1, 9)));
	CHECK_EQ(listed(PORT, false), 0u);

	/* IPv6: refused, and noted. */
	static const uint8_t client6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
					     0,	   0,	 0, 0, 0, 0, 0, 2 };
	static const uint8_t group6[16] = {
		0xff, 0x3e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34
	};
	uint8_t f[160];

	send(f, mld(f, -1, 131u, client6, group6, 0));
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(get32(311, PORT, 3), 1u);
	down();
}

static void test_the_limits_of_groups_and_bandwidth(void)
{
	up();
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 1000, 0);
	set_n(310, PORT, 3, 2, 2);
	omci_mcast_program(g_m, g_mib, g_now);

	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 1), -1);
	join(IP4(10, 0, 0, 3), IP4(239, 1, 1, 1), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 2), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 3), -1);
	CHECK(held(IP4(239, 1, 1, 1)) && held(IP4(239, 1, 1, 2)));
	CHECK(!held(IP4(239, 1, 1, 3)));
	CHECK_EQ(get32(311, PORT, 3), 3u);
	CHECK_EQ(get32(311, PORT, 2), 2000u);
	CHECK_EQ(listed(PORT, false), 3u);

	/* Over the bandwidth: counted and allowed, then counted and refused. */
	set_n(310, PORT, 3, 0, 2);
	set_n(310, PORT, 4, 2500, 4);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 3), -1);
	CHECK(held(IP4(239, 1, 1, 3)));
	CHECK_EQ(get32(311, PORT, 4), 1u);
	CHECK_EQ(get32(311, PORT, 3), 4u);
	set_n(310, PORT, 5, 1, 1);
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 1, 1, 3)));
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 4), -1);
	CHECK(!held(IP4(239, 1, 1, 4)));
	CHECK_EQ(get32(311, PORT, 4), 2u);
	CHECK_EQ(get32(311, PORT, 3), 4u);

	/* The bandwidth reported stops at the most a counter holds. */
	set_n(310, PORT, 4, 0, 4);
	acl(PROFILE, 7, 2, 0, IP4(239, 7, 0, 0), IP4(239, 7, 0, 255),
	    0xffffffffu, 0);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 7, 0, 1), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 7, 0, 2), -1);
	CHECK_EQ(get32(311, PORT, 2), 0xffffffffu);
	down();
}

static void test_service_packages_go_by_the_vlan(void)
{
	uint8_t row[20];

	up();
	side(true);
	me(309, PROFILE_B);
	set_n(309, PROFILE_B, 1, 2, 1);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	acl(PROFILE_B, 7, 1, 0, IP4(239, 3, 3, 0), IP4(239, 3, 3, 255), 0, 0);
	/* VID 100 to the first profile, at most one group; untagged to the second. */
	memset(row, 0, sizeof(row));
	row[0] = 0x40u;
	row[1] = 1u;
	put16(&row[2], 100u);
	put16(&row[4], 1u);
	put16(&row[10], PROFILE);
	table_row(310, PORT, 6, row, sizeof(row));
	row[1] = 2u;
	put16(&row[2], OMCI_MCAST_VID_UNTAGGED);
	put16(&row[4], 0u);
	put16(&row[10], PROFILE_B);
	table_row(310, PORT, 6, row, sizeof(row));
	omci_mcast_program(g_m, g_mib, g_now);

	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 1), 100);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 2), 100);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 7), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 3, 3, 3), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 3, 3, 4), 200);
	CHECK(held(IP4(239, 1, 1, 1)));
	CHECK(!held(IP4(239, 1, 1, 2)));
	CHECK(!held(IP4(239, 1, 1, 7)));
	CHECK(held(IP4(239, 3, 3, 3)));
	CHECK(!held(IP4(239, 3, 3, 4)));

	/* The package's own bandwidth. */
	acl(PROFILE_B, 7, 2, 0, IP4(239, 3, 4, 0), IP4(239, 3, 4, 255), 900, 0);
	put32(&row[6], 1000u);
	table_row(310, PORT, 6, row, sizeof(row));
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 3, 4, 1), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 3, 4, 2), -1);
	CHECK(held(IP4(239, 3, 4, 1)) && held(IP4(239, 3, 4, 2)));
	CHECK_EQ(get32(311, PORT, 4), 1u);
	put32(&row[6], 0u);

	/* Any tagged, and any at all. */
	row[1] = 2u;
	put16(&row[2], OMCI_MCAST_VID_TAGGED);
	table_row(310, PORT, 6, row, sizeof(row));
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 3, 3, 3)));
	join(IP4(10, 0, 0, 2), IP4(239, 3, 3, 4), 200);
	CHECK(held(IP4(239, 3, 3, 4)));
	put16(&row[2], OMCI_MCAST_VID_ANY);
	table_row(310, PORT, 6, row, sizeof(row));
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 3, 3, 5), -1);
	CHECK(held(IP4(239, 3, 3, 5)));

	/* The bridge port takes the first package's profile. */
	struct omci_hw_mcast_port port;

	set_n(309, PROFILE, 3, 1, 1);
	set_n(309, PROFILE, 10, 3, 1);
	set_n(309, PROFILE, 12, 60, 4);
	set_n(309, PROFILE, 13, 50, 4);
	set_n(309, PROFILE, 14, 5, 4);
	const unsigned int sets = omci_hw_mcast_port_recorded(NULL);

	omci_mcast_program(g_m, g_mib, g_now);
	CHECK_EQ(omci_hw_mcast_port_recorded(&port), sets + 1u);
	CHECK(port.fast_leave);
	CHECK_EQ(port.robustness, 3u);
	CHECK_EQ(port.query_interval, 60u);
	CHECK_EQ(port.query_response, 50u);
	CHECK_EQ(port.last_member, 5u);
	/* Set again with each change of the policy, not each second. */
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK_EQ(omci_hw_mcast_port_recorded(NULL), sets + 2u);
	omci_mcast_tick(g_m, g_mib, g_now + 1);
	CHECK_EQ(omci_hw_mcast_port_recorded(NULL), sets + 2u);
	down();
}

static void test_previews(void)
{
	up();
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 5, 0, 0), IP4(239, 5, 0, 63), 0, 0);
	preview_of(PROFILE, 1, 30, 100, 2, 3);
	omci_mcast_program(g_m, g_mib, g_now);

	/* A preview of 30 s; not again for 100 s, twice at most. */
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 1), -1);
	CHECK(held(IP4(239, 5, 0, 1)));
	g_now += 29;
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK(held(IP4(239, 5, 0, 1)));
	g_now += 1;
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 5, 0, 1)));
	g_now += 10;
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 1), -1);
	CHECK(!held(IP4(239, 5, 0, 1)));
	g_now += 70;
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 1), -1);
	CHECK(held(IP4(239, 5, 0, 1)));
	/* A second client joins the preview under way, which goes on. */
	join(IP4(10, 0, 0, 3), IP4(239, 5, 0, 1), -1);
	CHECK_EQ(listed(PORT, false), 2u);
	g_now += 30;
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 5, 0, 1)));
	g_now += 200;
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 1), -1);
	CHECK(!held(IP4(239, 5, 0, 1)));

	/* A day after the first, the count starts again. */
	g_now += 86400;
	omci_mcast_tick(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 1), -1);
	CHECK(held(IP4(239, 5, 0, 1)));

	/* A change of the policy lets a preview under way go on. */
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK(held(IP4(239, 5, 0, 1)));

	/* More groups previewed than counts are kept: the oldest is forgotten. */
	for (unsigned int i = 2; i < 36u; i++) {
		g_now += 1;
		join(IP4(10, 0, 0, 2), IP4(239, 5, 0, i), -1);
	}
	g_now += 40;
	omci_mcast_tick(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 2), -1);
	CHECK(held(IP4(239, 5, 0, 2)));
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 35), -1);
	CHECK(!held(IP4(239, 5, 0, 35)));
	down();
}

/* An allowed preview group of the subscriber, both parts. */
static void allowed(unsigned int key, uint32_t group, unsigned int vlan_uni,
		    unsigned int duration)
{
	uint8_t row[22] = { 0 };

	row[0] = (uint8_t)(0x40u | key >> 8);
	row[1] = (uint8_t)key;
	put16(&row[20], vlan_uni);
	table_row(310, PORT, 7, row, sizeof(row));
	memset(row, 0, sizeof(row));
	row[0] = (uint8_t)(0x48u | key >> 8);
	row[1] = (uint8_t)key;
	put32(&row[14], group);
	put16(&row[18], duration);
	put16(&row[20], duration);
	table_row(310, PORT, 7, row, sizeof(row));
}

static void test_allowed_preview_groups(void)
{
	up();
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 5, 0, 0), IP4(239, 5, 0, 63), 700, 0);
	preview_of(PROFILE, 1, 30, 0, 0, 0);
	allowed(1, IP4(239, 5, 0, 1), 0, 2);
	allowed(2, IP4(239, 6, 0, 1), 410, 0);
	omci_mcast_program(g_m, g_mib, g_now);

	/* Allowed for good, not as a preview, with the row's bandwidth. */
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 1), -1);
	g_now += 40;
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK(held(IP4(239, 5, 0, 1)));
	CHECK_EQ(get32(311, PORT, 2), 700u);

	/* In no list, but allowed, for VID 410 only, with no bandwidth. */
	join(IP4(10, 0, 0, 2), IP4(239, 6, 0, 1), -1);
	CHECK(!held(IP4(239, 6, 0, 1)));
	join(IP4(10, 0, 0, 2), IP4(239, 6, 0, 1), 410);
	CHECK(held(IP4(239, 6, 0, 1)));
	CHECK_EQ(get32(311, PORT, 2), 700u);

	/* Two minutes, then the row ends and its group with it. */
	g_now += 20;
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK(held(IP4(239, 5, 0, 1)));
	g_now += 60;
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 5, 0, 1)));
	CHECK(held(IP4(239, 6, 0, 1)));
	CHECK_EQ(omci_mib_table_count(g_mib, 310, PORT, 7), 2u);
	down();
}

static void test_static_groups(void)
{
	bool h;

	up();
	side(true);
	acl(PROFILE, 8, 1, 0, IP4(239, 9, 9, 1), IP4(239, 9, 9, 3), 0, 100);
	acl(PROFILE, 8, 2, IP4(10, 7, 7, 7), IP4(232, 9, 9, 9),
	    IP4(232, 9, 9, 9), 0, 4097);
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK_EQ(bridge(0, 0, NULL), 4u);
	CHECK(bridge(IP4(232, 9, 9, 9), IP4(10, 7, 7, 7), &h) == 4u && h);

	/* A static group is not judged, listed or ended by a join. */
	CHECK_EQ(listed(PORT, false), 0u);
	snoop(IP4(239, 9, 9, 1), 0, false);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(bridge(0, 0, NULL), 4u);

	/* One that went missing comes back. */
	struct omci_hw_mcast_group g = { .vid = 100, .permanent = true };

	a4(g.group, IP4(239, 9, 9, 2));
	CHECK_EQ(omci_hw_mcast_group_del(&g), OMCI_OK);
	CHECK_EQ(bridge(0, 0, NULL), 3u);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(bridge(0, 0, NULL), 4u);

	/* A row that goes takes its groups; a wide range adds no more than room. */
	delete_row(309, PROFILE, 8, 1, 24);
	acl(PROFILE, 8, 3, 0, IP4(239, 8, 0, 0), IP4(239, 8, 0, 99), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK_EQ(bridge(0, 0, NULL), 32u);
	CHECK(bridge(IP4(232, 9, 9, 9), IP4(10, 7, 7, 7), &h) == 32u && h);
	CHECK(!held(IP4(239, 9, 9, 1)));

	/* A static group the bridge will not take is noted. */
	omci_hw_mcast_fail_next(false, true, false);
	CHECK_EQ(omci_hw_mcast_group_del(&g), OMCI_DECLINED);
	a4(g.group, IP4(239, 8, 0, 5));
	g.vid = 0;
	CHECK_EQ(omci_hw_mcast_group_del(&g), OMCI_OK);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(bridge(0, 0, NULL), 31u);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(bridge(0, 0, NULL), 32u);
	down();
}

static void test_leaves(void)
{
	uint8_t f[160];

	up();
	side(false);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 1), -1);
	join(IP4(10, 0, 0, 3), IP4(239, 1, 1, 1), -1);
	join(IP4(10, 0, 0, 3), IP4(239, 1, 1, 1), -1);
	CHECK_EQ(listed(PORT, false), 2u);
	CHECK_EQ(get32(311, PORT, 3), 3u);

	/* One client leaves; the last one leaves, but the bridge still holds the group. */
	send(f, igmp(f, -1, 0x17u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 1)));
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, false), 1u);
	send(f, igmp(f, -1, 0x17u, IP4(10, 0, 0, 3), IP4(239, 1, 1, 1)));
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, false), 1u);
	snoop(IP4(239, 1, 1, 1), 0, false);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, false), 0u);

	/* IGMPv3: a source joined and blocked; other groups are not touched. */
	send(f, igmp3(f, -1, IP4(10, 0, 0, 2), 5u, IP4(232, 1, 1, 1), 2,
		      IP4(10, 1, 1, 1)));
	snoop(IP4(232, 1, 1, 1), IP4(10, 1, 1, 1), true);
	snoop(IP4(232, 1, 1, 1), IP4(10, 1, 1, 2), true);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 2), -1);
	CHECK_EQ(listed(PORT, false), 3u);
	send(f, igmp3(f, -1, IP4(10, 0, 0, 2), 6u, IP4(232, 1, 1, 1), 1,
		      IP4(10, 1, 1, 1)));
	snoop(IP4(232, 1, 1, 1), IP4(10, 1, 1, 1), false);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, false), 2u);
	send(f, igmp3(f, -1, IP4(10, 0, 0, 2), 3u, IP4(232, 1, 1, 1), 0, 0));
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, false), 2u);

	/* A query from the subscriber side counts for nothing. */
	send(f, igmp(f, -1, 0x11u, IP4(10, 0, 0, 2), IP4(224, 0, 0, 1)));
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(get32(311, PORT, 3), 6u);
	down();
}

static void test_linking_to_the_subscriber_side(void)
{
	up();
	/* Through a mapper. */
	me(130, PORT);
	me(309, PROFILE);
	set_n(309, PROFILE, 1, 3, 1);
	me(310, PORT);
	set_n(310, PORT, 1, 1, 1);
	set_n(310, PORT, 2, PROFILE, 2);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 2, 2, 2), -1);
	CHECK(!held(IP4(239, 2, 2, 2)));
	/* A mapper that is not there links nothing: every group is allowed. */
	CHECK_EQ(omci_mib_delete(g_mib, 130, PORT), OMCI_OK);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 2, 2, 2), -1);
	CHECK(held(IP4(239, 2, 2, 2)));
	/* Nor does a bridge port of the network side, or one that is not there. */
	me(47, PORT);
	set_n(47, PORT, 3, 5, 1);
	set_n(310, PORT, 1, 0, 1);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 2, 2, 3), -1);
	CHECK(held(IP4(239, 2, 2, 3)));
	set_n(47, PORT, 3, 11, 1);
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 2, 2, 3)));
	down();
}

static enum omci_status port_fails(const struct omci_hw_mcast_port *cfg)
{
	(void)cfg;
	return OMCI_ERR_HW;
}

static void test_what_the_bridge_refuses(void)
{
	struct omci_hw_ops ops = omci_hw_host_ops;

	up();
	side(false);
	/* The port cannot be set: tried again each second until it is. */
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK_EQ(omci_hw_mcast_port_recorded(NULL), 1u);
	ops.mcast_port = port_fails;
	omci_hw_ops_set(&ops);
	omci_mcast_program(g_m, g_mib, g_now);
	omci_mcast_tick(g_m, g_mib, g_now);
	omci_hw_ops_set(NULL);
	CHECK_EQ(omci_hw_mcast_port_recorded(NULL), 1u);
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK_EQ(omci_hw_mcast_port_recorded(NULL), 2u);
	omci_mcast_tick(g_m, g_mib, g_now);
	CHECK_EQ(omci_hw_mcast_port_recorded(NULL), 2u);

	/* The groups cannot be read: what is known stays. */
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 1), -1);
	CHECK_EQ(listed(PORT, false), 1u);
	snoop(IP4(239, 1, 1, 1), 0, false);
	omci_hw_mcast_fail_next(true, false, false);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, false), 1u);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(listed(PORT, false), 0u);

	/* More active groups than are kept: the rest are not listed. */
	for (unsigned int i = 0; i < 66u; i++)
		join(IP4(10, 0, 1, i), IP4(239, 4, 4, 4), -1);
	CHECK_EQ(listed(PORT, false), 64u);

	/* A backend without multicast is left alone. */
	ops.mcast_port = NULL;
	omci_hw_ops_set(&ops);
	omci_mcast_program(g_m, g_mib, g_now);
	omci_mcast_service(g_m, g_mib, g_now);
	omci_mcast_tick(g_m, g_mib, g_now);
	omci_hw_ops_set(NULL);
	omci_mcast_program(NULL, g_mib, g_now);
	down();
}

/* ---- Messages upstream ---------------------------------------------- */

static void ctp(uint16_t inst, uint16_t port_id, uint16_t tcont, uint8_t dir)
{
	me(268, inst);
	set_n(268, inst, OMCI_GEM_ATTR_PORT_ID, port_id, 2);
	set_n(268, inst, OMCI_GEM_ATTR_TCONT, tcont, 2);
	set_n(268, inst, OMCI_GEM_ATTR_DIRECTION, dir, 1);
	me(266, inst);
	set_n(266, inst, OMCI_GEM_IW_ATTR_CTP, inst, 2);
}

static void net_port(uint16_t inst, uint8_t tp_type, uint16_t tp_ptr,
		     uint16_t vid)
{
	me(47, inst);
	set_n(47, inst, 3, tp_type, 1);
	set_n(47, inst, 4, tp_ptr, 2);
	if (vid != 0u) {
		uint8_t table[24] = { 0 };

		put16(table, vid);
		me(84, inst);
		CHECK_EQ(omci_mib_attr_set_by_olt(g_mib, 84, inst,
						  OMCI_BRIDGE_VLAN_ATTR_TABLE,
						  table, sizeof(table)),
			 OMCI_OK);
		set_n(84, inst, OMCI_BRIDGE_VLAN_ATTR_COUNT, 1, 1);
	}
}

/* The instances the ONU makes itself, the T-CONTs among them; first, as they reset the MIB. */
static void onu(void)
{
	struct omci_hw_caps caps;

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(g_mib, &caps), OMCI_OK);
}

/*
 * The network side, in the UNI's bridge: GEM port 256 both ways with @vid
 * (0 for every VLAN), and GEM port 4000 downstream only.
 */
static void network(uint16_t vid)
{
	ctp(0x0001, 256, 0x8000, 3);
	net_port(0x0201, 5, 0x0001, vid);
	ctp(0x0002, 4000, 0, 2);
	net_port(0x0202, 5, 0x0002, 0);
	(void)!omci_gem_program(g_mib, NULL);
	(void)!omci_bridge_program(g_mib, NULL);
}

/* The last message sent upstream but @back, its GEM port and length. */
static size_t sent(size_t back, uint16_t *port, uint8_t *f)
{
	*port = 0;
	return omci_hw_mcast_sent(back, port, f, 600);
}

static size_t sent_count(void)
{
	uint8_t f[600];
	uint16_t port;
	size_t n = 0;

	while (n < 8u && sent(n, &port, f) != 0u)
		n++;
	return n;
}

/* A message of the subscriber's, as the switch traps it, with no group to snoop. */
static void hear(const uint8_t *f, size_t n)
{
	send(f, n);
	omci_mcast_service(g_m, g_mib, g_now);
}

static void test_messages_go_upstream_under_a_policy(void)
{
	static const uint8_t client6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
					     0,	   0,	 0, 0, 0, 0, 0, 2 };
	static const uint8_t group6[16] = {
		0xff, 0x3e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34
	};
	struct omci_hw_mcast_port port;
	uint8_t f[256], out[600];
	uint16_t gem;
	size_t n;

	up();
	onu();
	side(true);
	network(0);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);
	(void)omci_hw_mcast_port_recorded(&port);
	CHECK(port.trap);

	/* An allowed join goes up GEM port 256 as it came. */
	n = igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	hear(f, n);
	CHECK_EQ(sent(0, &gem, out), n);
	CHECK_EQ(gem, 256u);
	CHECK_MEM(out, f, n);
	CHECK_EQ(sent_count(), 1u);

	/* A refused one does not, unless the profile forwards it. */
	hear(f, igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 2, 2, 2)));
	CHECK_EQ(sent_count(), 1u);
	set_n(309, PROFILE, 15, 1, 1);
	hear(f, igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 2, 2, 2)));
	CHECK_EQ(sent_count(), 2u);
	set_n(309, PROFILE, 15, 0, 1);

	/* A leave goes up; a query of the subscriber side does not. */
	hear(f, igmp(f, -1, 0x17u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5)));
	CHECK_EQ(sent_count(), 3u);
	hear(f, igmp(f, -1, 0x11u, IP4(10, 0, 0, 2), IP4(224, 0, 0, 1)));
	CHECK_EQ(sent_count(), 3u);

	/* IGMPv3 with the profile at version 3, then 2; MLD is not IGMP. */
	n = igmp3(f, -1, IP4(10, 0, 0, 2), 4u, IP4(239, 1, 1, 6), 0, 0);
	hear(f, n);
	CHECK_EQ(sent_count(), 4u);
	set_n(309, PROFILE, 1, 2, 1);
	hear(f, n);
	CHECK_EQ(sent_count(), 4u);
	hear(f, igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 6)));
	CHECK_EQ(sent_count(), 5u);
	hear(f, mld(f, -1, 143u, client6, group6, 3u));
	CHECK_EQ(sent_count(), 6u);
	/* At MLD version 1, an MLDv2 report is dropped, and IGMP is free. */
	set_n(309, PROFILE, 1, 16, 1);
	hear(f, mld(f, -1, 143u, client6, group6, 3u));
	CHECK_EQ(sent_count(), 6u);
	hear(f, igmp3(f, -1, IP4(10, 0, 0, 2), 4u, IP4(239, 1, 1, 6), 0, 0));
	CHECK_EQ(sent_count(), 7u);

	/* Without a policy the switch forwards them: no trap, nothing sent. */
	CHECK_EQ(omci_mib_delete(g_mib, 310, PORT), OMCI_OK);
	omci_mcast_program(g_m, g_mib, g_now);
	(void)omci_hw_mcast_port_recorded(&port);
	CHECK(!port.trap);
	hear(f, igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 7)));
	CHECK_EQ(sent_count(), 7u);
	down();
}

static void test_a_backend_that_cannot_send_leaves_the_messages_alone(void)
{
	struct omci_hw_ops ops = omci_hw_host_ops;
	struct omci_hw_mcast_port port;

	up();
	side(true);
	ops.mcast_send = NULL;
	omci_hw_ops_set(&ops);
	omci_mcast_program(g_m, g_mib, g_now);
	omci_hw_ops_set(NULL);
	(void)omci_hw_mcast_port_recorded(&port);
	CHECK(!port.trap);
	down();
}

/* ---- The proxy (IGMP function 1 and 2) ------------------------------ */

/* The Ethernet address the host backend gives the ONU. */
static const uint8_t PROXY_MAC[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 };

/* The subscriber side and the network side, as for the messages upstream, with the function. */
static void proxy_side(unsigned int function)
{
	up();
	onu();
	side(true);
	network(0);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	set_n(309, PROFILE, 2, function, 1);
}

/* A client leaves with IGMPv2; the bridge drops the group at once when @drops. */
static void leaves(uint32_t client, uint32_t group, bool drops)
{
	uint8_t f[160];

	send(f, igmp(f, -1, 0x17u, client, group));
	if (drops)
		snoop(group, 0, false);
	omci_mcast_service(g_m, g_mib, g_now);
}

/* A query from the OLT's side: general for a zero group, of IGMP version 1 in form. */
static void olt_asks(uint32_t group)
{
	uint8_t f[160];

	omci_hw_mcast_receive_pon(f,
				  igmp(f, -1, 0x11u, IP4(10, 0, 0, 1), group));
	omci_mcast_service(g_m, g_mib, g_now);
}

/* The message sent @back from the last, taken apart; its GEM port. */
static struct omci_mcast_report sent_report(size_t back, uint16_t *port)
{
	struct omci_mcast_report r;
	uint8_t f[600];
	const size_t n = sent(back, port, f);

	CHECK(n != 0u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	return r;
}

static void test_function_1_speaks_for_the_subscribers(void)
{
	struct omci_mcast_report r;
	uint8_t out[600];
	uint16_t gem;
	size_t n;

	proxy_side(1);
	omci_mcast_program(g_m, g_mib, g_now);

	/* A group of another subscriber, held all along. */
	join(IP4(10, 0, 0, 9), IP4(239, 1, 1, 9), -1);
	CHECK_EQ(sent_count(), 1u);
	(void)sent(0, &gem, out);

	/* The first subscriber of a group: one report of the ONU's own. */
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	CHECK_EQ(sent_count(), 2u);
	n = sent(0, &gem, out);
	CHECK_EQ(gem, 256u);
	CHECK_MEM(&out[6], PROXY_MAC, 6);
	CHECK(omci_mcast_report_parse(out, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_REPORT);
	CHECK_EQ(r.version, 3u);
	CHECK(r.n == 1u && r.rec[0].join);
	CHECK_EQ(be32_at(&r.rec[0].group[12]), IP4(239, 1, 1, 5));
	CHECK_EQ(be32_at(&r.client[12]), 0u);

	/* Another subscriber and a repeated report tell the OLT nothing. */
	join(IP4(10, 0, 0, 3), IP4(239, 1, 1, 5), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	CHECK_EQ(sent_count(), 2u);

	/*
	 * One leaves while another holds the group: nothing to tell, but the
	 * subscribers are asked, for the other to answer.
	 */
	leaves(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), false);
	CHECK_EQ(sent_count(), 3u);
	r = sent_report(0, &gem);
	CHECK_EQ(gem, OMCI_HW_MCAST_UNI);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	join(IP4(10, 0, 0, 3), IP4(239, 1, 1, 5), -1);
	for (unsigned int i = 0; i < 4u; i++) {
		g_now++;
		omci_mcast_service(g_m, g_mib, g_now);
	}
	CHECK_EQ(sent_count(), 3u);
	CHECK(held(IP4(239, 1, 1, 5)));

	/* The last leaves: the subscribers are asked, twice, a second apart. */
	leaves(IP4(10, 0, 0, 3), IP4(239, 1, 1, 5), false);
	CHECK_EQ(sent_count(), 4u);
	r = sent_report(0, &gem);
	CHECK_EQ(gem, OMCI_HW_MCAST_UNI);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(be32_at(&r.query_group[12]), IP4(239, 1, 1, 5));
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 4u);
	g_now++;
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 5u);
	CHECK(held(IP4(239, 1, 1, 5)));

	/* Nobody answered: the group goes from the bridge, and then the OLT hears of it. */
	g_now++;
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK(!held(IP4(239, 1, 1, 5)));
	CHECK_EQ(sent_count(), 5u);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 6u);
	r = sent_report(0, &gem);
	CHECK_EQ(gem, 256u);
	CHECK(r.n == 1u && !r.rec[0].join);
	CHECK_EQ(listed(PORT, false), 1u);

	down();
}

static void test_a_join_while_a_leave_is_checked_ends_the_check(void)
{
	proxy_side(1);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 6), -1);
	CHECK_EQ(sent_count(), 1u);
	leaves(IP4(10, 0, 0, 2), IP4(239, 1, 1, 6), false);
	CHECK_EQ(sent_count(), 2u);
	join(IP4(10, 0, 0, 4), IP4(239, 1, 1, 6), -1);
	for (unsigned int i = 0; i < 4u; i++) {
		g_now++;
		omci_mcast_service(g_m, g_mib, g_now);
	}
	/* The OLT hears nothing more, and the subscribers are asked no more. */
	CHECK_EQ(sent_count(), 2u);
	CHECK(held(IP4(239, 1, 1, 6)));
	down();
}

static void test_the_proxy_speaks_in_the_vlan_of_the_join(void)
{
	struct omci_mcast_report r;
	uint16_t gem;
	uint8_t f[160];

	proxy_side(2);
	set_n(309, PROFILE, 12, 60, 4);
	omci_mcast_program(g_m, g_mib, g_now);
	/* Untagged where the subscribers send no tag. */
	r = sent_report(1, &gem);
	CHECK(!r.tagged);
	g_pcp = 3;
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), 410);
	/* The report goes up with the tag of the join, VID and priority. */
	r = sent_report(0, &gem);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_REPORT);
	CHECK(r.tagged && r.vid == 410u && r.pcp == 3u);

	/* The query that checks its leave goes to the subscribers in the same tag. */
	send(f, igmp(f, 410, 0x17u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5)));
	omci_mcast_service(g_m, g_mib, g_now);
	r = sent_report(0, &gem);
	CHECK_EQ(gem, OMCI_HW_MCAST_UNI);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK(r.tagged && r.vid == 410u && r.pcp == 3u);

	/* A join again, and the general queries then go in that tag. */
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), 410);
	g_now += 60;
	omci_mcast_service(g_m, g_mib, g_now);
	r = sent_report(1, &gem);
	CHECK_EQ(gem, OMCI_HW_MCAST_UNI);
	CHECK(r.kind == OMCI_MCAST_KIND_QUERY && r.tagged && r.vid == 410u);
	g_pcp = 5;
	down();
}

static void test_a_source_specific_join_is_told_in_its_vlan(void)
{
	struct omci_mcast_report r;
	uint16_t gem;
	uint8_t f[160];

	proxy_side(1);
	omci_mcast_program(g_m, g_mib, g_now);
	/* The bridge holds the group alone, as a bridge does that snoops a join with a source. */
	g_pcp = 4;
	send(f, igmp3(f, 410, IP4(10, 0, 0, 2), 1u, IP4(239, 1, 1, 20), 1,
		      IP4(10, 1, 1, 1)));
	snoop(IP4(239, 1, 1, 20), 0, true);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 1u);
	r = sent_report(0, &gem);
	CHECK_EQ(gem, 256u);
	CHECK(r.tagged && r.vid == 410u && r.pcp == 4u);
	g_pcp = 5;
	down();
}

static void test_a_leave_with_fast_leave_is_told_at_once(void)
{
	struct omci_mcast_report r;
	uint16_t gem;

	proxy_side(1);
	set_n(309, PROFILE, 3, 1, 1);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	CHECK_EQ(sent_count(), 1u);
	leaves(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), true);
	/* The bridge dropped the group, so the OLT hears of the leave at once, and no query goes. */
	CHECK_EQ(sent_count(), 2u);
	r = sent_report(0, &gem);
	CHECK_EQ(gem, 256u);
	CHECK(r.n == 1u && !r.rec[0].join);
	down();
}

static void test_the_proxy_answers_the_olts_queries(void)
{
	struct omci_mcast_report r;
	uint16_t gem;
	unsigned int waited = 0;

	proxy_side(1);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	join(IP4(10, 0, 0, 3), IP4(239, 1, 1, 6), -1);
	CHECK_EQ(sent_count(), 2u);

	/* A general query is answered with the state of the groups, within its time. */
	olt_asks(0);
	while (sent_count() == 2u && waited < 11u) {
		g_now++;
		waited++;
		omci_mcast_service(g_m, g_mib, g_now);
	}
	CHECK(waited < 10u);
	CHECK_EQ(sent_count(), 3u);
	r = sent_report(0, &gem);
	CHECK_EQ(gem, 256u);
	CHECK_EQ(r.n, 2u);
	CHECK(r.rec[0].join && r.rec[1].join);

	/* A query of a group is answered at once if it is held, and not if it is not. */
	olt_asks(IP4(239, 1, 1, 6));
	CHECK_EQ(sent_count(), 4u);
	r = sent_report(0, &gem);
	CHECK(r.n == 1u && r.rec[0].join);
	CHECK_EQ(be32_at(&r.rec[0].group[12]), IP4(239, 1, 1, 6));
	olt_asks(IP4(239, 1, 1, 77));
	CHECK_EQ(sent_count(), 4u);

	/* Transparent snooping leaves the OLT's queries alone. */
	set_n(309, PROFILE, 2, 0, 1);
	omci_mcast_service(g_m, g_mib, g_now);
	olt_asks(IP4(239, 1, 1, 6));
	olt_asks(0);
	g_now += 20;
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 4u);
	down();
}

static void test_function_2_is_the_querier_of_the_subscribers(void)
{
	struct omci_mcast_report r;
	uint16_t gem;
	uint8_t out[600];

	proxy_side(2);
	set_n(309, PROFILE, 11, IP4(10, 0, 0, 77), 4);
	set_n(309, PROFILE, 12, 60, 4);
	set_n(309, PROFILE, 13, 50, 4);
	omci_mcast_program(g_m, g_mib, g_now);

	/* Queries of both protocols at once, to the subscriber side. */
	CHECK_EQ(sent_count(), 2u);
	r = sent_report(1, &gem);
	CHECK_EQ(gem, OMCI_HW_MCAST_UNI);
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(r.version, 3u);
	CHECK_EQ(r.max_resp, 50u);
	CHECK_EQ(be32_at(&r.client[12]), IP4(10, 0, 0, 77));
	r = sent_report(0, &gem);
	CHECK_EQ(gem, OMCI_HW_MCAST_UNI);
	CHECK_EQ(r.version, 17u);

	/* And again every interval of the profile. */
	g_now += 59;
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 2u);
	g_now++;
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 4u);

	/* A subscriber's own query is not for the proxy: nothing comes of it. */
	{
		uint8_t f[160];

		send(f, igmp(f, -1, 0x11u, IP4(10, 0, 0, 5), 0));
		omci_mcast_service(g_m, g_mib, g_now);
		CHECK_EQ(sent_count(), 4u);
		(void)sent(0, &gem, out);
	}
	down();
}

static void test_a_proxy_that_cannot_be_leaves_the_messages_as_they_are(void)
{
	struct omci_hw_ops ops = omci_hw_host_ops;
	uint8_t f[160], out[600];
	uint16_t gem;
	size_t n;

	/* No Ethernet address to send from. */
	proxy_side(2);
	ops.mcast_mac = NULL;
	omci_hw_ops_set(&ops);
	omci_mcast_program(g_m, g_mib, g_now);
	n = igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	send(f, n);
	snoop(IP4(239, 1, 1, 5), 0, true);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 1u);
	CHECK_EQ(sent(0, &gem, out), n);
	CHECK_MEM(out, f, n);
	CHECK_EQ(gem, 256u);
	omci_hw_ops_set(NULL);
	down();

	/* No policy, so the switch forwards the messages itself. */
	up();
	onu();
	side(false);
	network(0);
	set_n(309, PROFILE, 2, 1, 1);
	omci_mcast_program(g_m, g_mib, g_now);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	CHECK_EQ(sent_count(), 0u);
	down();

	/* A function that is not one is taken for the highest. */
	proxy_side(1);
	set_n(309, PROFILE, 2, 3, 1);
	omci_mcast_program(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 2u);
	down();
}

static void
test_a_refused_join_goes_up_as_it_came_where_the_profile_forwards_it(void)
{
	uint8_t f[160], out[600];
	uint16_t gem;
	size_t n;

	proxy_side(1);
	set_n(309, PROFILE, 15, 1, 1);
	omci_mcast_program(g_m, g_mib, g_now);
	n = igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 2, 2, 2));
	send(f, n);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 1u);
	CHECK_EQ(sent(0, &gem, out), n);
	CHECK_MEM(out, f, n);
	/* An allowed one is told by the ONU itself, and the refused group is not held. */
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	CHECK_EQ(sent_count(), 2u);
	CHECK(sent(0, &gem, out) != n);
	set_n(309, PROFILE, 15, 0, 1);
	send(f, n);
	omci_mcast_service(g_m, g_mib, g_now);
	CHECK_EQ(sent_count(), 2u);
	down();
}

static void test_the_rate_of_messages_upstream(void)
{
	uint8_t f[256];
	size_t n;

	up();
	onu();
	side(true);
	network(0);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	set_n(309, PROFILE, 6, 2, 4);
	omci_mcast_program(g_m, g_mib, g_now);
	n = igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));
	hear(f, n);
	hear(f, n);
	hear(f, n);
	CHECK_EQ(sent_count(), 2u);
	g_now += 1;
	hear(f, n);
	CHECK_EQ(sent_count(), 3u);
	down();
}

/* The outer tag of a sent frame: its TPID and TCI. */
static void tag_at(const uint8_t *f, size_t at, unsigned int *tpid,
		   unsigned int *tci)
{
	*tpid = (unsigned int)f[at] << 8 | f[at + 1u];
	*tci = (unsigned int)f[at + 2u] << 8 | f[at + 3u];
}

/* Join 239.1.1.5 in a frame tagged @vid (-1 untagged); the frame sent, its length. */
static size_t join_up(int vid, uint8_t *out)
{
	uint8_t f[256];
	uint16_t gem;

	hear(f, igmp(f, vid, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5)));
	return sent(0, &gem, out);
}

static void test_the_upstream_igmp_tag_control(void)
{
	unsigned int tpid, tci;
	uint8_t out[600];

	up();
	onu();
	side(true);
	network(0);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	set_n(309, PROFILE, 4, (3u << 13) | 420u, 2);
	omci_mcast_program(g_m, g_mib, g_now);

	/* 1: a tag added, in front of any other. */
	set_n(309, PROFILE, 5, 1, 1);
	CHECK_EQ(join_up(-1, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK(tpid == 0x8100u && tci == ((3u << 13) | 420u));
	CHECK_EQ(join_up(410, out), 54u);
	tag_at(out, 16, &tpid, &tci);
	CHECK(tpid == 0x8100u && tci == ((5u << 13) | 410u));

	/* 2: the outer tag replaced, or one added. */
	set_n(309, PROFILE, 5, 2, 1);
	CHECK_EQ(join_up(410, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK_EQ(tci, (3u << 13) | 420u);
	CHECK_EQ(join_up(-1, out), 50u);

	/* 3: the VID replaced, the priority kept; or a tag added. */
	set_n(309, PROFILE, 5, 3, 1);
	CHECK_EQ(join_up(410, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK_EQ(tci, (5u << 13) | 420u);
	CHECK_EQ(join_up(-1, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK_EQ(tci, (3u << 13) | 420u);

	/* 0, no rules: a tag the receive path took off is put back. */
	set_n(309, PROFILE, 5, 0, 1);
	{
		uint8_t f[256];
		uint16_t gem;
		const size_t n =
			igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5));

		omci_hw_mcast_receive(f, n, true, (2u << 13) | 77u);
		omci_mcast_service(g_m, g_mib, g_now);
		CHECK_EQ(sent(0, &gem, out), n + 4u);
		tag_at(out, 12, &tpid, &tci);
		CHECK(tpid == 0x8100u && tci == ((2u << 13) | 77u));
	}
	down();
}

/* Replace the rules of the extended VLAN tagging operation by @n rows. */
static void rules(const uint32_t (*rows)[4], size_t n)
{
	(void)!omci_mib_delete(g_mib, 171, 0x0101);
	me(171, 0x0101);
	for (size_t i = 0; i < n; i++) {
		uint8_t row[16];

		for (unsigned int w = 0; w < 4u; w++)
			put32(&row[4 * w], rows[i][w]);
		CHECK_EQ(omci_mib_table_put(g_mib, 171, 0x0101, 6, row, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
	}
	(void)!omci_vlan_program(g_mib, 171, 6, NULL);
}

static void test_the_vlan_rules_tag_the_messages(void)
{
	static const uint32_t xlate[][4] = {
		/* C-VID 410, any priority, to 1410, the priority kept */
		{ 0xf8000000u, (8u << 28) | (410u << 15), 0x400f0000u,
		  (8u << 16) | (1410u << 3) },
		/* C-VID 100, priority 6, to 200 at priority 6 */
		{ 0xf8000000u, (6u << 28) | (100u << 15), 0x400f0000u,
		  (6u << 16) | (200u << 3) },
	};
	static const uint32_t push[][4] = {
		/* untagged: C-VID 36 at priority 4 */
		{ 0xf8000000u, 0xf8000000u, 0x000f0000u,
		  (4u << 16) | (36u << 3) },
		/* C-VID 410: an S-tag of VID 100 that copies the priority */
		{ 0xf8000000u, (8u << 28) | (410u << 15),
		  (8u << 16) | (100u << 3), (8u << 16) | (410u << 3) },
		/* C-VID 37 at priority 6: an S-tag of VID 101 at priority 2 */
		{ 0xf8000000u, (6u << 28) | (37u << 15),
		  (2u << 16) | (101u << 3), (6u << 16) | (37u << 3) },
	};
	unsigned int tpid, tci;
	uint8_t out[600];

	up();
	onu();
	side(true);
	network(0);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);

	rules(xlate, ARRAY_SIZE(xlate));
	CHECK_EQ(join_up(410, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK_EQ(tci, (5u << 13) | 1410u);
	g_pcp = 6;
	CHECK_EQ(join_up(100, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK_EQ(tci, (6u << 13) | 200u);
	g_pcp = 5;
	CHECK_EQ(join_up(100, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK_EQ(tci, (5u << 13) | 100u);
	/* An outer S-tag is no C-tag the rules take. */
	g_tpid = 0x88a8u;
	CHECK_EQ(join_up(410, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK(tpid == 0x88a8u && tci == ((5u << 13) | 410u));
	g_tpid = 0x8100u;

	rules(push, ARRAY_SIZE(push));
	CHECK_EQ(join_up(-1, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK(tpid == 0x8100u && tci == ((4u << 13) | 36u));
	CHECK_EQ(join_up(410, out), 54u);
	tag_at(out, 12, &tpid, &tci);
	CHECK(tpid == 0x88a8u && tci == ((5u << 13) | 100u));
	tag_at(out, 16, &tpid, &tci);
	CHECK(tpid == 0x8100u && tci == ((5u << 13) | 410u));
	g_pcp = 6;
	CHECK_EQ(join_up(37, out), 54u);
	tag_at(out, 12, &tpid, &tci);
	CHECK(tpid == 0x88a8u && tci == ((2u << 13) | 101u));
	g_pcp = 5;
	CHECK_EQ(join_up(38, out), 50u);
	tag_at(out, 12, &tpid, &tci);
	CHECK_EQ(tci, (5u << 13) | 38u);
	rules(push, 0);
	down();
}

/*
 * Rules whose default discards the single-tagged frames no rule takes, as an
 * operator's OLT sets them: a message in a VLAN no rule takes from the
 * subscriber goes nowhere, even a VLAN of the network side; one a rule takes
 * goes up as the rule tags it.
 */
static void test_the_vlan_rules_discard_messages_they_do_not_take(void)
{
	static const uint32_t strict[][4] = {
		/* C-VID 410, any priority, to 1410, the priority kept */
		{ 0xf8000000u, (8u << 28) | (410u << 15), 0x400f0000u,
		  (8u << 16) | (1410u << 3) },
		/* the default of the single-tagged frames: discard */
		{ 0xf8000000u, 0xe8000000u, 0xc00f0000u, 0x000f8000u },
	};
	unsigned int tpid, tci;
	uint8_t f[256], out[600];

	up();
	onu();
	side(true);
	network(0);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);
	rules(strict, ARRAY_SIZE(strict));

	hear(f, igmp(f, 1410, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5)));
	CHECK_EQ(sent_count(), 0u);
	hear(f, igmp(f, 100, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5)));
	CHECK_EQ(sent_count(), 0u);

	CHECK_EQ(join_up(410, out), 50u);
	CHECK_EQ(sent_count(), 1u);
	tag_at(out, 12, &tpid, &tci);
	CHECK(tpid == 0x8100u && tci == ((5u << 13) | 1410u));
	rules(strict, 0);
	down();
}

static void test_the_gem_port_of_a_message(void)
{
	static const uint32_t xlate[][4] = {
		{ 0xf8000000u, (8u << 28) | (410u << 15), 0x400f0000u,
		  (8u << 16) | (1410u << 3) },
	};
	uint8_t out[600];
	uint16_t gem;

	up();
	onu();
	side(true);
	network(1410);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);

	/* GEM port 256 carries VLAN 1410 only. */
	CHECK_EQ(join_up(-1, out), 0u);
	rules(xlate, ARRAY_SIZE(xlate));
	CHECK_EQ(join_up(410, out), 50u);
	CHECK_EQ(sent(0, &gem, out), 50u);
	CHECK_EQ(gem, 256u);

	/* Behind a mapper: priorities 0 to 3 to GEM port 256, 4 to 7 to 300. */
	ctp(0x0011, 300, 0x8001, 3);
	me(130, 0x0301);
	for (unsigned int b = 0; b < 8u; b++)
		set_n(130, 0x0301, (uint8_t)(2u + b),
		      b < 4u ? 0x0001u : 0x0011u, 2);
	net_port(0x0203, 3, 0x0301, 1410);
	(void)!omci_gem_program(g_mib, NULL);
	(void)!omci_bridge_program(g_mib, NULL);
	(void)join_up(410, out);
	(void)sent(0, &gem, out);
	CHECK_EQ(gem, 300u);
	g_pcp = 1;
	(void)join_up(410, out);
	(void)sent(0, &gem, out);
	CHECK_EQ(gem, 256u);
	g_pcp = 5;

	/* A send that fails is noted. */
	omci_hw_mcast_send_fail_next();
	const size_t before = sent_count();

	(void)join_up(410, out);
	CHECK_EQ(sent_count(), before);

	/* Without the UNI in a bridge nothing goes up; with it last, it does. */
	CHECK_EQ(omci_mib_delete(g_mib, 47, PORT), OMCI_OK);
	(void)!omci_bridge_program(g_mib, NULL);
	(void)join_up(410, out);
	CHECK_EQ(sent_count(), before);
	me(47, PORT);
	set_n(47, PORT, 3, 1, 1);
	(void)!omci_bridge_program(g_mib, NULL);
	(void)join_up(410, out);
	(void)sent(0, &gem, out);
	CHECK_EQ(gem, 300u);
	rules(xlate, 0);
	down();
}

/* The downstream tag set for VLAN @vid, or NULL. */
static const struct omci_hw_mcast_ds *ds_of(uint16_t vid, size_t *n)
{
	const struct omci_hw_mcast_ds *r;

	*n = omci_hw_mcast_ds_recorded(&r);
	for (size_t i = 0; i < *n; i++)
		if (r[i].vid == vid)
			return &r[i];
	return NULL;
}

/* Set the profile's downstream IGMP and multicast TCI and program again. */
static void ds_control(uint16_t profile, unsigned int control, unsigned int tci)
{
	set_n(309, profile, 16, (uint32_t)(control << 16 | tci), 3);
	omci_mcast_program(g_m, g_mib, g_now);
}

static void test_the_downstream_tag_of_multicast(void)
{
	static const uint32_t xlate[][4] = {
		/* C-VID 410 to 420: VLAN 420 carries unicast too */
		{ 0xf8000000u, (8u << 28) | (410u << 15), 0x400f0000u,
		  (8u << 16) | (420u << 3) },
	};
	const struct omci_hw_mcast_ds *d;
	uint8_t row[20];
	size_t n;

	up();
	onu();
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 9), 0, 420);
	acl(PROFILE, 7, 2, 0, IP4(239, 1, 2, 0), IP4(239, 1, 2, 9), 0, 420);
	acl(PROFILE, 7, 3, 0, IP4(239, 1, 3, 0), IP4(239, 1, 3, 9), 0, 0);
	acl(PROFILE, 7, 4, 0, IP4(239, 1, 4, 0), IP4(239, 1, 4, 9), 0, 4097);
	acl(PROFILE, 8, 1, 0, IP4(239, 2, 1, 1), IP4(239, 2, 1, 1), 0, 421);
	omci_mcast_program(g_m, g_mib, g_now);
	(void)ds_of(420, &n);
	CHECK_EQ(n, 0u);

	/* 1: the tag comes off, in each VLAN once; untagged and any-VLAN rows have none. */
	ds_control(PROFILE, 1, 0);
	d = ds_of(420, &n);
	CHECK_EQ(n, 2u);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_STRIP);
	CHECK(ds_of(421, &n) != NULL);
	omci_mcast_program(g_m, g_mib, g_now);

	/* 2 to 4: added, written over, the VID written over. */
	ds_control(PROFILE, 2, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_ADD &&
	      d->tci == ((4u << 13) | 100u));
	ds_control(PROFILE, 3, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_REPLACE);
	ds_control(PROFILE, 4, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_REPLACE_VID);

	/* 5 to 7 without packages: the attribute's tag. */
	ds_control(PROFILE, 6, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_REPLACE &&
	      d->tci == ((4u << 13) | 100u));

	/* With a package of VID 200: its VID, the attribute's priority. */
	memset(row, 0, sizeof(row));
	row[0] = 0x40u;
	row[1] = 1u;
	put16(&row[2], 200u);
	put16(&row[10], PROFILE);
	table_row(310, PORT, 6, row, sizeof(row));
	ds_control(PROFILE, 5, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_ADD &&
	      d->tci == ((4u << 13) | 200u));
	/* Of untagged joins: 5 adds the attribute's tag, 6 and 7 strip. */
	put16(&row[2], OMCI_MCAST_VID_UNTAGGED);
	table_row(310, PORT, 6, row, sizeof(row));
	ds_control(PROFILE, 5, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_ADD &&
	      d->tci == ((4u << 13) | 100u));
	ds_control(PROFILE, 7, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_STRIP);
	/* Of any tagged, or unspecified: the attribute's tag. */
	put16(&row[2], OMCI_MCAST_VID_TAGGED);
	table_row(310, PORT, 6, row, sizeof(row));
	ds_control(PROFILE, 7, (4u << 13) | 100u);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_REPLACE_VID &&
	      d->tci == ((4u << 13) | 100u));

	/* A VLAN that unicast takes too keeps the VLAN rules' tag. */
	rules(xlate, ARRAY_SIZE(xlate));
	ds_control(PROFILE, 1, 0);
	CHECK(ds_of(420, &n) == NULL);
	CHECK_EQ(n, 1u);
	rules(xlate, 0);

	/* No more than eight VLANs. */
	for (unsigned int k = 10; k < 20u; k++)
		acl(PROFILE, 7, k, 0, IP4(239, 3, k, 0), IP4(239, 3, k, 9), 0,
		    900u + k);
	omci_mcast_program(g_m, g_mib, g_now);
	(void)ds_of(420, &n);
	CHECK_EQ(n, OMCI_HW_MCAST_DS_MAX);

	/* A change the backend refuses is tried again. */
	omci_hw_mcast_ds_fail_next();
	ds_control(PROFILE, 3, 0);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_STRIP);
	omci_mcast_program(g_m, g_mib, g_now);
	d = ds_of(420, &n);
	CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_REPLACE);

	/* A backend without downstream tags is left alone. */
	{
		struct omci_hw_ops ops = omci_hw_host_ops;

		ops.mcast_ds = NULL;
		omci_hw_ops_set(&ops);
		ds_control(PROFILE, 1, 0);
		omci_hw_ops_set(NULL);
		d = ds_of(420, &n);
		CHECK(d != NULL && d->action == OMCI_HW_MCAST_DS_REPLACE);
	}
	down();
}

/* The downstream frames of a group so far, as the backend counts them. */
static void flow(uint32_t group, uint64_t frames)
{
	uint8_t g[16];

	a4(g, group);
	omci_hw_mcast_flow(g, frames);
}

/* Whether the backend counts the frames of a group. */
static bool counted(uint32_t group)
{
	uint64_t frames = 0;
	uint8_t g[16];

	a4(g, group);
	return omci_hw_mcast_seen(g, &frames) == OMCI_OK;
}

/* Let @sec seconds go by, a tick each. */
static void pass(unsigned int sec)
{
	while (sec-- > 0)
		omci_mcast_tick(g_m, g_mib, ++g_now);
}

static size_t lost(uint16_t profile)
{
	return omci_mcast_lost_groups.count(profile);
}

static void test_lost_groups(void)
{
	uint16_t prof[4];
	uint8_t row[16];

	up();
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 420);
	omci_mcast_program(g_m, g_mib, g_now);

	/* A group the bridge forwards is counted from the next second. */
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 5), -1);
	CHECK(!counted(IP4(239, 1, 1, 5)));
	pass(1);
	CHECK(counted(IP4(239, 1, 1, 5)));

	/* A read that fails changes nothing; frames came, then stop for ten seconds. */
	omci_hw_mcast_watch_fail_next(false, true);
	flow(IP4(239, 1, 1, 5), 100);
	pass(1);
	pass(9);
	CHECK_EQ(lost(PROFILE), 0u);
	pass(2);
	CHECK_EQ(lost(PROFILE), 1u);
	CHECK(omci_mcast_lost_groups.entry(PROFILE, 0, row, sizeof(row)));
	CHECK_EQ(row[0] << 8 | row[1], 420);
	CHECK_EQ(be32_at(&row[2]), 0u);
	CHECK_EQ(be32_at(&row[6]), IP4(239, 1, 1, 5));
	CHECK(!omci_mcast_lost_groups.entry(PROFILE, 0, row, 9));
	CHECK(!omci_mcast_lost_groups.entry(PROFILE, 1, row, sizeof(row)));
	CHECK_EQ(lost(PROFILE_B), 0u);
	CHECK_EQ(omci_mcast_lost_profiles(g_m, prof, ARRAY_SIZE(prof)), 1u);
	CHECK_EQ(prof[0], PROFILE);
	CHECK_EQ(omci_mcast_lost_profiles(NULL, prof, ARRAY_SIZE(prof)), 0u);

	/* A second client of the group, a second group lost: a row each, the profile once. */
	join(IP4(10, 0, 0, 3), IP4(239, 1, 1, 5), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 6), -1);
	pass(11);
	CHECK_EQ(lost(PROFILE), 2u);
	CHECK_EQ(omci_mcast_lost_profiles(g_m, prof, ARRAY_SIZE(prof)), 1u);

	/* Frames again: the group flows. */
	flow(IP4(239, 1, 1, 5), 150);
	pass(1);
	CHECK_EQ(lost(PROFILE), 1u);

	/* The bridge lets a group go: it is no longer counted, once the backend agrees. */
	snoop(IP4(239, 1, 1, 6), 0, false);
	omci_hw_mcast_watch_fail_next(true, false);
	pass(1);
	CHECK(counted(IP4(239, 1, 1, 6)));
	CHECK_EQ(lost(PROFILE), 0u);
	pass(1);
	CHECK(!counted(IP4(239, 1, 1, 6)));

	/* A group the backend cannot count yet is counted when it can. */
	omci_hw_mcast_watch_fail_next(true, false);
	join(IP4(10, 0, 0, 2), IP4(239, 1, 1, 7), -1);
	pass(1);
	CHECK(!counted(IP4(239, 1, 1, 7)));
	pass(1);
	CHECK(counted(IP4(239, 1, 1, 7)));

	/* No more groups are counted than the backend has room for. */
	for (unsigned int i = 10; i < 25u; i++)
		join(IP4(10, 0, 0, 2), IP4(239, 1, 1, i), -1);
	pass(1);
	CHECK(counted(IP4(239, 1, 1, 23)));
	CHECK(!counted(IP4(239, 1, 1, 24)));
	snoop(IP4(239, 1, 1, 10), 0, false);
	pass(1);
	CHECK(counted(IP4(239, 1, 1, 24)));

	/* A backend that cannot count is left alone. */
	{
		struct omci_hw_ops ops = omci_hw_host_ops;

		ops.mcast_seen = NULL;
		omci_hw_ops_set(&ops);
		snoop(IP4(239, 1, 1, 11), 0, false);
		pass(1);
		omci_hw_ops_set(NULL);
		CHECK(counted(IP4(239, 1, 1, 11)));
	}

	/* Without a policy, no group is counted. */
	CHECK_EQ(omci_mib_delete(g_mib, 310, PORT), OMCI_OK);
	omci_mcast_program(g_m, g_mib, g_now);
	pass(1);
	CHECK(!counted(IP4(239, 1, 1, 5)));
	CHECK_EQ(lost(PROFILE), 0u);
	down();
	CHECK_EQ(lost(PROFILE), 0u);
}

static void test_what_is_counted_for_lost_groups(void)
{
	up();
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 5, 0, 0), IP4(239, 5, 0, 63), 0, 0);
	preview_of(PROFILE, 1, 300, 0, 0, 0);
	allowed(1, IP4(239, 6, 0, 1), 0, 0);
	omci_mcast_program(g_m, g_mib, g_now);

	/* A group for preview is in the profile's list; an allowed preview group in none is not. */
	join(IP4(10, 0, 0, 2), IP4(239, 5, 0, 1), -1);
	join(IP4(10, 0, 0, 2), IP4(239, 6, 0, 1), -1);
	CHECK(held(IP4(239, 6, 0, 1)));
	pass(1);
	CHECK(counted(IP4(239, 5, 0, 1)));
	CHECK(!counted(IP4(239, 6, 0, 1)));
	pass(10);
	CHECK_EQ(lost(PROFILE), 1u);
	down();
}

/* The lost multicast group alarms of the profile sent, and whether the last stood. */
static unsigned int lost_alarms(bool *stood)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++) {
		const uint8_t *f = omci_t_tx.frame[i];

		if ((f[2] & 0x1fu) != 16u || (f[4] << 8 | f[5]) != 309 ||
		    (f[6] << 8 | f[7]) != PROFILE)
			continue;
		n++;
		*stood = (f[8] & 0x80u) != 0u;
	}
	return n;
}

static void test_the_lost_group_alarm(void)
{
	struct omci_t_fixture fx;
	bool stood = false;
	uint8_t f[128];

	omci_hw_mcast_reset();
	CHECK(omci_t_up(&fx));
	g_mib = fx.mib;
	side(true);
	acl(PROFILE, 7, 1, 0, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 0, 0);
	omci_engine_reprogram(fx.eng);
	send(f, igmp(f, -1, 0x16u, IP4(10, 0, 0, 2), IP4(239, 1, 1, 5)));
	snoop(IP4(239, 1, 1, 5), 0, true);
	omci_engine_mcast_service(fx.eng);

	/* Counted from the first second, lost at the eleventh, notified the next. */
	for (uint64_t t = 2000; t < 2011u; t++)
		omci_engine_tick(fx.eng, t);
	CHECK_EQ(lost_alarms(&stood), 0u);
	omci_engine_tick(fx.eng, 2011);
	CHECK_EQ(lost_alarms(&stood), 1u);
	CHECK(stood);

	/* Frames again: the alarm clears. */
	flow(IP4(239, 1, 1, 5), 10);
	omci_engine_tick(fx.eng, 2012);
	omci_engine_tick(fx.eng, 2013);
	CHECK_EQ(lost_alarms(&stood), 2u);
	CHECK(!stood);
	omci_t_down(&fx);
	g_mib = NULL;
	omci_hw_mcast_reset();
}

static void test_one_agent_at_a_time(void)
{
	struct omci_mcast *m = NULL, *other = NULL;

	CHECK_EQ(omci_mcast_init(NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mcast_init(&m), OMCI_OK);
	CHECK_EQ(omci_mcast_init(&other), OMCI_ERR_BUSY);
	omci_mcast_fini(NULL);
	omci_mcast_fini((struct omci_mcast *)&other);
	CHECK_EQ(omci_mcast_init(&other), OMCI_ERR_BUSY);

	/* No engine while the agent is taken. */
	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL),
		 OMCI_ERR_BUSY);
	omci_mcast_fini(m);
	CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL), OMCI_OK);
	CHECK_EQ(omci_engine_mcast_fd(eng), -1);
	omci_engine_mcast_service(eng);
	omci_engine_mcast_service(NULL);
	omci_engine_fini(eng);
	omci_mib_fini(mib);

	/* Without an agent, no monitor lists anything. */
	CHECK_EQ(listed(PORT, false), 0u);
}

int main(void)
{
	RUN(test_messages_are_taken_apart);
	RUN(test_frames_that_are_not_messages_are_refused);
	RUN(test_without_a_policy_every_group_is_allowed);
	RUN(test_the_access_control_list_decides);
	RUN(test_the_limits_of_groups_and_bandwidth);
	RUN(test_service_packages_go_by_the_vlan);
	RUN(test_previews);
	RUN(test_allowed_preview_groups);
	RUN(test_static_groups);
	RUN(test_leaves);
	RUN(test_linking_to_the_subscriber_side);
	RUN(test_what_the_bridge_refuses);
	RUN(test_messages_go_upstream_under_a_policy);
	RUN(test_a_backend_that_cannot_send_leaves_the_messages_alone);
	RUN(test_function_1_speaks_for_the_subscribers);
	RUN(test_a_join_while_a_leave_is_checked_ends_the_check);
	RUN(test_the_proxy_speaks_in_the_vlan_of_the_join);
	RUN(test_a_source_specific_join_is_told_in_its_vlan);
	RUN(test_a_leave_with_fast_leave_is_told_at_once);
	RUN(test_the_proxy_answers_the_olts_queries);
	RUN(test_function_2_is_the_querier_of_the_subscribers);
	RUN(test_a_proxy_that_cannot_be_leaves_the_messages_as_they_are);
	RUN(test_a_refused_join_goes_up_as_it_came_where_the_profile_forwards_it);
	RUN(test_the_rate_of_messages_upstream);
	RUN(test_the_upstream_igmp_tag_control);
	RUN(test_the_vlan_rules_tag_the_messages);
	RUN(test_the_vlan_rules_discard_messages_they_do_not_take);
	RUN(test_the_gem_port_of_a_message);
	RUN(test_the_downstream_tag_of_multicast);
	RUN(test_lost_groups);
	RUN(test_what_is_counted_for_lost_groups);
	RUN(test_the_lost_group_alarm);
	RUN(test_one_agent_at_a_time);
	return OMCI_T_REPORT("test_mcast_agent");
}
