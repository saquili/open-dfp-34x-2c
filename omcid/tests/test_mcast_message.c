// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the messages the ONU makes as a proxy: each is checked for its
 * checksums, then taken apart again by the parser that reads the subscribers'.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mcast/message.h"
#include "mcast/report.h"
#include "omcid/byteorder.h"

#include "harness.h"

#define IP4(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

static const uint8_t MAC[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x09 };
static const uint8_t SRC6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
				  0,	0,    0, 0, 0, 0, 0, 9 };
static const uint8_t GROUP6[16] = { 0xff, 0x3e, 0, 0, 0, 0, 0,	  0,
				    0,	  0,	0, 0, 0, 0, 0x12, 0x34 };
static const uint8_t SRC6B[16] = { 0x20, 0x01, 0xd, 0xb8, 0, 0, 0, 0,
				   0,	 0,    0,   0,	  0, 0, 0, 7 };

static uint32_t be_ip(const uint8_t *p)
{
	return omci_be32(p);
}

static void a4(uint8_t out[16], uint32_t v)
{
	uint8_t b[4];

	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
	omci_mcast_addr_set_v4(out, b);
}

/* The environment of IPv4 version @version from an address of a subscriber, or from the unspecified one. */
static struct omci_mcast_env env4(uint8_t version, bool with_source)
{
	struct omci_mcast_env e = { .version = version };

	memcpy(e.mac, MAC, sizeof(MAC));
	if (with_source)
		a4(e.source, IP4(10, 0, 0, 9));
	return e;
}

static struct omci_mcast_env env6(uint8_t version)
{
	struct omci_mcast_env e = { .version = version };

	memcpy(e.mac, MAC, sizeof(MAC));
	memcpy(e.source, SRC6, sizeof(SRC6));
	return e;
}

/* An item of a group held from every source. */
static struct omci_mcast_item any4(uint32_t group)
{
	struct omci_mcast_item it = { .any = true };

	a4(it.group, group);
	return it;
}

/* An item of a group held from the @n sources counting from @source. */
static struct omci_mcast_item some4(uint32_t group, unsigned int n,
				    uint32_t source)
{
	struct omci_mcast_item it = { .n = (uint8_t)n };

	a4(it.group, group);
	for (unsigned int i = 0; i < n; i++)
		a4(it.source[i], source + i);
	return it;
}

/* The sum of @len bytes with @sum added, which for a message with its checksum is zero. */
static uint16_t fold(const uint8_t *p, size_t len, uint32_t sum)
{
	for (size_t i = 0; i + 1u < len; i += 2u)
		sum += omci_be16(&p[i]);
	if (len % 2u != 0u)
		sum += (uint32_t)p[len - 1u] << 8;
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return (uint16_t)~sum;
}

/* An IPv4 frame is well made: its header, its length, its checksums. */
static void check4(const uint8_t *f, size_t n, const struct omci_mcast_env *e)
{
	const uint8_t *ip = &f[14];

	CHECK_EQ(omci_be16(&f[12]), 0x0800u);
	CHECK_MEM(&f[6], e->mac, 6);
	CHECK_EQ(ip[0], 0x46u);
	CHECK_EQ(ip[8], 1u);
	CHECK_EQ(ip[9], 2u);
	CHECK_EQ(omci_be16(&ip[2]), n - 14u);
	CHECK_EQ(ip[20], 0x94u);
	CHECK_EQ(ip[21], 0x04u);
	CHECK_EQ(fold(ip, 24, 0), 0u);
	CHECK_EQ(fold(&ip[24], n - 14u - 24u, 0), 0u);
	CHECK_MEM(&ip[12], &e->source[12], 4);
}

/* An IPv6 frame is well made, with the checksum over the pseudo header. */
static void check6(const uint8_t *f, size_t n, const struct omci_mcast_env *e)
{
	const uint8_t *ip = &f[14];
	const size_t len = n - 14u - 48u;
	uint8_t tail[8] = { 0 };
	uint32_t sum = 0;

	CHECK_EQ(omci_be16(&f[12]), 0x86ddu);
	CHECK_MEM(&f[6], e->mac, 6);
	CHECK_EQ(ip[0], 0x60u);
	CHECK_EQ(omci_be16(&ip[4]), 8u + len);
	CHECK_EQ(ip[6], 0u);
	CHECK_EQ(ip[7], 1u);
	CHECK_EQ(ip[40], 58u);
	CHECK_EQ(ip[41], 0u);
	CHECK_EQ(ip[42], 0x05u);
	CHECK_EQ(ip[43], 0x02u);
	CHECK_MEM(&ip[8], e->source, 16);
	for (size_t i = 8; i < 40; i += 2u)
		sum += omci_be16(&ip[i]);
	tail[2] = (uint8_t)(len >> 8);
	tail[3] = (uint8_t)len;
	tail[7] = 58u;
	for (size_t i = 0; i < 8; i += 2u)
		sum += omci_be16(&tail[i]);
	CHECK_EQ(fold(&ip[48], len, sum), 0u);
}

static void test_igmp_version_2_reports_and_leaves(void)
{
	static const uint8_t mac_group[6] = {
		0x01, 0x00, 0x5e, 0x01, 0x01, 0x05
	};
	struct omci_mcast_env e = env4(2, true);
	struct omci_mcast_item it = any4(IP4(239, 1, 1, 5));
	struct omci_mcast_report r;
	uint8_t f[128], want[16];
	size_t n, used;

	/* A join is a report to the group. */
	n = omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used);
	CHECK_EQ(n, 14u + 24u + 8u);
	CHECK_EQ(used, 1u);
	check4(f, n, &e);
	CHECK_EQ(f[38], 0x16u);
	CHECK_EQ(be_ip(&f[30]), IP4(239, 1, 1, 5));
	CHECK_MEM(f, mac_group, 6);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_REPORT);
	CHECK_EQ(r.version, 2u);
	CHECK(r.n == 1u && r.rec[0].join);
	a4(want, IP4(239, 1, 1, 5));
	CHECK_MEM(r.rec[0].group, want, 16);
	a4(want, IP4(10, 0, 0, 9));
	CHECK_MEM(r.client, want, 16);

	/* The sources of a group are not told by this version: it is held. */
	it = some4(IP4(239, 1, 1, 5), 2, IP4(10, 1, 1, 1));
	n = omci_mcast_build_report(f, sizeof(f), &e, false, &it, 1, &used);
	CHECK_EQ(n, 14u + 24u + 8u);
	CHECK_EQ(f[38], 0x16u);

	/* A leave goes to the routers; an answer holds none. */
	memset(it.source, 0, sizeof(it.source));
	it.n = 0;
	n = omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used);
	CHECK_EQ(n, 14u + 24u + 8u);
	check4(f, n, &e);
	CHECK_EQ(f[38], 0x17u);
	CHECK_EQ(be_ip(&f[30]), IP4(224, 0, 0, 2));
	CHECK_EQ(f[0], 0x01u);
	CHECK_EQ(f[5], 0x02u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_LEAVE);
	CHECK(r.n == 1u && !r.rec[0].join);
	CHECK_EQ(omci_mcast_build_report(f, sizeof(f), &e, false, &it, 1,
					 &used),
		 0u);
	CHECK_EQ(used, 1u);

	/* No room, no groups, a group of the other family, an unspecified source. */
	it = any4(IP4(239, 1, 1, 5));
	CHECK_EQ(omci_mcast_build_report(f, 45, &e, true, &it, 1, &used), 0u);
	CHECK_EQ(omci_mcast_build_report(f, sizeof(f), &e, true, &it, 0, &used),
		 0u);
	CHECK_EQ(used, 0u);
	memcpy(it.group, GROUP6, 16);
	CHECK_EQ(omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used),
		 0u);
	e = env4(2, false);
	it = any4(IP4(239, 1, 1, 5));
	n = omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used);
	check4(f, n, &e);
	CHECK_EQ(be_ip(&f[26]), 0u);
}

static void test_igmp_version_1_has_no_leave(void)
{
	struct omci_mcast_env e = env4(1, true);
	struct omci_mcast_item it = any4(IP4(239, 1, 1, 5));
	struct omci_mcast_report r;
	uint8_t f[128];
	size_t n, used;

	n = omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used);
	check4(f, n, &e);
	CHECK_EQ(f[38], 0x12u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 1u);
	it.any = false;
	CHECK_EQ(omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used),
		 0u);
}

static void test_igmp_version_3_records(void)
{
	struct omci_mcast_env e = env4(3, true);
	struct omci_mcast_item it[3] = {
		any4(IP4(239, 1, 1, 5)),
		some4(IP4(232, 1, 1, 1), 2, IP4(10, 1, 1, 1)),
		some4(IP4(239, 1, 1, 6), 0, 0),
	};
	struct omci_mcast_report r;
	uint8_t f[256], want[16];
	size_t n, used;

	/* A change: exclude with no source, include with sources, include with none. */
	n = omci_mcast_build_report(f, sizeof(f), &e, true, it, 3, &used);
	CHECK_EQ(used, 3u);
	CHECK_EQ(n, 14u + 24u + 8u + 8u + 16u + 8u);
	check4(f, n, &e);
	CHECK_EQ(f[38], 0x22u);
	CHECK_EQ(omci_be16(&f[44]), 3u);
	CHECK_EQ(f[46], 4u);
	CHECK_EQ(f[54], 3u);
	CHECK_EQ(omci_be16(&f[56]), 2u);
	CHECK_EQ(be_ip(&f[66]), IP4(10, 1, 1, 2));
	CHECK_EQ(f[70], 3u);
	CHECK_EQ(omci_be16(&f[72]), 0u);
	CHECK_EQ(f[15], 0xc0u);
	CHECK_EQ(be_ip(&f[30]), IP4(224, 0, 0, 22));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 3u);
	/* The group, the group's leave before its sources, and the last group's leave. */
	CHECK_EQ(r.n, 5u);
	CHECK(r.rec[0].join);
	CHECK(!r.rec[1].join && r.rec[2].join && r.rec[3].join);
	CHECK(!r.rec[4].join);
	a4(want, IP4(10, 1, 1, 2));
	CHECK_MEM(r.rec[3].source, want, 16);

	/* An answer: the states, and none for a group nobody holds. */
	n = omci_mcast_build_report(f, sizeof(f), &e, false, it, 3, &used);
	CHECK_EQ(used, 3u);
	CHECK_EQ(omci_be16(&f[44]), 2u);
	CHECK_EQ(f[46], 2u);
	CHECK_EQ(f[54], 1u);
	check4(f, n, &e);

	/* Only so many records fit; the rest go in the next frame. */
	{
		struct omci_mcast_item many[40];

		for (unsigned int i = 0; i < 40; i++)
			many[i] = any4(IP4(239, 2, 0, i));
		n = omci_mcast_build_report(f, 100, &e, true, many, 40, &used);
		CHECK_EQ(used, 6u);
		CHECK_EQ(omci_be16(&f[44]), 6u);
		check4(f, n, &e);
		CHECK(n <= 100u);
		n = omci_mcast_build_report(f, 100, &e, true, &many[used],
					    40u - used, &used);
		CHECK_EQ(used, 6u);
		CHECK_EQ(be_ip(&f[50]), IP4(239, 2, 0, 6));
	}

	/* A group of the other family is passed over; a frame with none to send is empty. */
	it[0] = any4(IP4(239, 1, 1, 5));
	memcpy(it[1].group, GROUP6, 16);
	n = omci_mcast_build_report(f, sizeof(f), &e, true, it, 2, &used);
	CHECK_EQ(used, 2u);
	CHECK_EQ(omci_be16(&f[44]), 1u);
	CHECK_EQ(omci_mcast_build_report(f, sizeof(f), &e, true, &it[1], 1,
					 &used),
		 0u);
	CHECK_EQ(used, 1u);
	CHECK_EQ(omci_mcast_build_report(f, 50, &e, true, it, 1, &used), 0u);
	CHECK_EQ(used, 0u);
}

static void test_mld_reports(void)
{
	struct omci_mcast_env e = env6(16);
	struct omci_mcast_item it = { .any = true };
	struct omci_mcast_report r;
	uint8_t f[256];
	size_t n, used;

	memcpy(it.group, GROUP6, 16);
	/* Version 1: a report to the group, a done to the routers. */
	n = omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used);
	CHECK_EQ(n, 14u + 48u + 24u);
	check6(f, n, &e);
	CHECK_EQ(f[62], 131u);
	CHECK_MEM(&f[38], GROUP6, 16);
	CHECK_EQ(f[0], 0x33u);
	CHECK_EQ(f[2], 0x00u);
	CHECK_EQ(f[4], 0x12u);
	CHECK_EQ(f[5], 0x34u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 16u);
	CHECK(r.n == 1u && r.rec[0].join);
	CHECK_MEM(r.client, SRC6, 16);
	it.any = false;
	n = omci_mcast_build_report(f, sizeof(f), &e, true, &it, 1, &used);
	check6(f, n, &e);
	CHECK_EQ(f[62], 132u);
	CHECK_EQ(f[38], 0xffu);
	CHECK_EQ(f[53], 2u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_LEAVE);
	CHECK_EQ(omci_mcast_build_report(f, sizeof(f), &e, false, &it, 1,
					 &used),
		 0u);
	CHECK_EQ(omci_mcast_build_report(f, 60, &e, true, &it, 1, &used), 0u);
	{
		struct omci_mcast_item v4 = any4(IP4(239, 1, 1, 5));

		CHECK_EQ(omci_mcast_build_report(f, sizeof(f), &e, true, &v4, 1,
						 &used),
			 0u);
	}

	/* Version 2: records to ff02::16. */
	e = env6(17);
	it.any = true;
	{
		struct omci_mcast_item two[2];

		two[0] = it;
		memset(&two[1], 0, sizeof(two[1]));
		memcpy(two[1].group, GROUP6, 16);
		two[1].group[15] = 0x99u;
		two[1].n = 1;
		memcpy(two[1].source[0], SRC6B, 16);
		n = omci_mcast_build_report(f, sizeof(f), &e, true, two, 2,
					    &used);
		CHECK_EQ(used, 2u);
		check6(f, n, &e);
		CHECK_EQ(f[62], 143u);
		CHECK_EQ(omci_be16(&f[68]), 2u);
		CHECK_EQ(f[70], 4u);
		CHECK_EQ(f[70 + 20], 3u);
		CHECK_MEM(&f[70 + 20 + 4 + 16], SRC6B, 16);
		CHECK_EQ(f[53], 0x16u);
		CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
		CHECK_EQ(r.version, 17u);
		CHECK_EQ(r.n, 3u);
	}
}

/* The value a parsed query's response time may be when @sent was asked for. */
static void check_resp(uint32_t got, uint32_t sent)
{
	CHECK(got <= sent);
	CHECK(got + sent / 16u + 1u >= sent);
}

static void test_igmp_queries(void)
{
	struct omci_mcast_env e = env4(2, true);
	struct omci_mcast_report r;
	uint8_t f[128], group[16], want[16] = { 0 };
	size_t n;

	/* A general query of version 2. */
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 100, 2, 125);
	CHECK_EQ(n, 14u + 24u + 8u);
	check4(f, n, &e);
	CHECK_EQ(f[38], 0x11u);
	CHECK_EQ(f[39], 100u);
	CHECK_EQ(be_ip(&f[30]), IP4(224, 0, 0, 1));
	CHECK_EQ(f[5], 0x01u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(r.version, 2u);
	CHECK_EQ(r.max_resp, 100u);
	CHECK_MEM(r.query_group, want, 16);

	/* Specific to a group, all-zero meaning general; the time over what the byte holds. */
	a4(group, IP4(239, 1, 1, 5));
	n = omci_mcast_build_query(f, sizeof(f), &e, group, 1000, 2, 125);
	check4(f, n, &e);
	CHECK_EQ(f[39], 255u);
	CHECK_EQ(be_ip(&f[30]), IP4(239, 1, 1, 5));
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_MEM(r.query_group, group, 16);
	n = omci_mcast_build_query(f, sizeof(f), &e, want, 10, 2, 125);
	CHECK_EQ(be_ip(&f[30]), IP4(224, 0, 0, 1));

	/* Version 1 has no response time. */
	e = env4(1, true);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 100, 2, 125);
	check4(f, n, &e);
	CHECK_EQ(f[39], 0u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 1u);

	/* Version 3: the response time and the interval as floating point numbers, the robustness. */
	e = env4(3, false);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 100, 2, 125);
	CHECK_EQ(n, 14u + 24u + 12u);
	check4(f, n, &e);
	CHECK_EQ(f[39], 100u);
	CHECK_EQ(f[46], 2u);
	CHECK_EQ(f[47], 125u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 3u);
	CHECK_EQ(r.max_resp, 100u);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 600, 9, 200);
	check4(f, n, &e);
	CHECK_EQ(f[46], 0u);
	CHECK_EQ(f[47], 0x89u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.max_resp, 576u);
	check_resp(r.max_resp, 600u);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 100000, 2, 125);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.max_resp, 31u << 10);

	/* No room; a group of the other family. */
	CHECK_EQ(omci_mcast_build_query(f, 40, &e, NULL, 100, 2, 125), 0u);
	CHECK_EQ(omci_mcast_build_query(f, sizeof(f), &e, GROUP6, 100, 2, 125),
		 0u);
}

static void test_mld_queries(void)
{
	struct omci_mcast_env e = env6(16);
	struct omci_mcast_report r;
	uint8_t f[128], none[16] = { 0 };
	size_t n;

	/* Version 1: milliseconds in a short message. */
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 100, 2, 125);
	CHECK_EQ(n, 14u + 48u + 24u);
	check6(f, n, &e);
	CHECK_EQ(f[62], 130u);
	CHECK_EQ(omci_be16(&f[66]), 10000u);
	CHECK_EQ(f[38], 0xffu);
	CHECK_EQ(f[39], 0x02u);
	CHECK_EQ(f[53], 0x01u);
	CHECK_EQ(f[0], 0x33u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.kind, OMCI_MCAST_KIND_QUERY);
	CHECK_EQ(r.version, 16u);
	CHECK_EQ(r.max_resp, 100u);
	CHECK_MEM(r.query_group, none, 16);
	n = omci_mcast_build_query(f, sizeof(f), &e, GROUP6, 1000000, 2, 125);
	check6(f, n, &e);
	CHECK_EQ(omci_be16(&f[66]), 0xffffu);
	CHECK_MEM(&f[38], GROUP6, 16);
	CHECK_MEM(&f[70], GROUP6, 16);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_MEM(r.query_group, GROUP6, 16);
	CHECK_EQ(omci_mcast_build_query(f, sizeof(f), &e, none, 100, 2, 125),
		 14u + 48u + 24u);

	/* Version 2: a floating point response time, the robustness and the interval. */
	e = env6(17);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 100, 3, 125);
	CHECK_EQ(n, 14u + 48u + 28u);
	check6(f, n, &e);
	CHECK_EQ(omci_be16(&f[66]), 10000u);
	CHECK_EQ(f[86], 3u);
	CHECK_EQ(f[87], 125u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.version, 17u);
	CHECK_EQ(r.max_resp, 100u);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 4000, 9, 125);
	check6(f, n, &e);
	CHECK_EQ(f[86], 0u);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	check_resp(r.max_resp, 4000u);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 4001, 9, 125);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	check_resp(r.max_resp, 4001u);
	n = omci_mcast_build_query(f, sizeof(f), &e, NULL, 4000000, 2, 125);
	CHECK(omci_mcast_report_parse(f, n, 0, false, &r));
	CHECK_EQ(r.max_resp, ((0xfffu | 0x1000u) << 10) / 100u + 1u);

	CHECK_EQ(omci_mcast_build_query(f, 70, &e, NULL, 100, 2, 125), 0u);
	{
		uint8_t g4[16];

		a4(g4, IP4(239, 1, 1, 5));
		CHECK_EQ(omci_mcast_build_query(f, sizeof(f), &e, g4, 100, 2,
						125),
			 0u);
	}
}

int main(void)
{
	RUN(test_igmp_version_2_reports_and_leaves);
	RUN(test_igmp_version_1_has_no_leave);
	RUN(test_igmp_version_3_records);
	RUN(test_mld_reports);
	RUN(test_igmp_queries);
	RUN(test_mld_queries);
	return OMCI_T_REPORT("test_mcast_message");
}
