// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The IGMP and MLD messages of message.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mcast/message.h"
#include "omcid/byteorder.h"

enum {
	ETH_LEN = 14,
	ETH_IPV4 = 0x0800,
	ETH_IPV6 = 0x86dd,

	/* IPv4 with the router alert option (RFC 2113), and IGMP. */
	IP4_LEN = 24,
	IP4_TOS_CONTROL = 0xc0,
	PROTO_IGMP = 2,
	IGMP_QUERY = 0x11,
	IGMP_V1_REPORT = 0x12,
	IGMP_V2_REPORT = 0x16,
	IGMP_V2_LEAVE = 0x17,
	IGMP_V3_REPORT = 0x22,
	IGMP_HEAD = 8,
	IGMP_V3_QUERY = 12,

	/* IPv6 with a hop-by-hop header holding the router alert (RFC 2711), and MLD. */
	IP6_LEN = 40,
	HBH_LEN = 8,
	PROTO_HOPOPTS = 0,
	PROTO_ICMPV6 = 58,
	MLD_QUERY = 130,
	MLD_V1_REPORT = 131,
	MLD_V1_DONE = 132,
	MLD_V2_REPORT = 143,
	MLD_HEAD = 8,
	MLD_V1_QUERY = 24,
	MLD_V2_QUERY = 28,

	/* The group records of a version 3 or MLDv2 report. */
	REPORT_HEAD = 8,
	RECORD_HEAD = 4,
	IS_INCLUDE = 1,
	IS_EXCLUDE = 2,
	TO_INCLUDE = 3,
	TO_EXCLUDE = 4,
};

static bool is_v4(const uint8_t *a)
{
	return omci_mcast_addr_v4(a);
}

/* The one's complement sum of @len bytes, an even number, added to @sum. */
static uint32_t sum_add(uint32_t sum, const uint8_t *p, size_t len)
{
	for (size_t i = 0; i + 1u < len; i += 2u)
		sum += omci_be16(&p[i]);
	return sum;
}

static uint16_t sum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return (uint16_t)~sum;
}

/* The code of a value that a floating point format holds (RFC 3376 4.1.1). */
static unsigned int float_code(uint32_t v, uint32_t threshold,
			       unsigned int mant_bits, unsigned int exp_shift)
{
	const uint32_t mant = (1u << mant_bits) - 1u;

	if (v < threshold)
		return v;
	for (unsigned int exp = 0; exp < 8u; exp++) {
		const uint32_t m = v >> (exp + 3u);

		if (m < (2u << mant_bits))
			return threshold | exp << exp_shift | (m & mant);
	}
	return threshold | 7u << exp_shift | mant;
}

/* An IPv4 multicast address' Ethernet address, and an IPv6 one's. */
static void mac4(uint8_t *mac, const uint8_t *ip)
{
	mac[0] = 0x01u;
	mac[1] = 0x00u;
	mac[2] = 0x5eu;
	mac[3] = ip[1] & 0x7fu;
	mac[4] = ip[2];
	mac[5] = ip[3];
}

static void mac6(uint8_t *mac, const uint8_t *ip)
{
	mac[0] = 0x33u;
	mac[1] = 0x33u;
	memcpy(&mac[2], &ip[12], 4);
}

/*
 * The Ethernet and IPv4 headers for @len bytes of IGMP to @dst, four bytes;
 * the offset of the IGMP message.
 */
static size_t headers4(uint8_t *f, const struct omci_mcast_env *env,
		       const uint8_t *dst, size_t len)
{
	uint8_t *ip = &f[ETH_LEN];

	mac4(f, dst);
	memcpy(&f[6], env->mac, OMCI_MCAST_MAC_LEN);
	omci_be16_store(&f[12], ETH_IPV4);
	memset(ip, 0, IP4_LEN);
	ip[0] = 0x46u;
	ip[1] = IP4_TOS_CONTROL;
	omci_be16_store(&ip[2], IP4_LEN + len);
	ip[8] = 1u;
	ip[9] = PROTO_IGMP;
	memcpy(&ip[12], &env->source[12], 4);
	memcpy(&ip[16], dst, 4);
	ip[20] = 0x94u;
	ip[21] = 0x04u;
	omci_be16_store(&ip[10], sum_fold(sum_add(0, ip, IP4_LEN)));
	return ETH_LEN + IP4_LEN;
}

/*
 * The Ethernet, IPv6 and hop-by-hop headers for @len bytes of MLD to @dst;
 * the offset of the MLD message.
 */
static size_t headers6(uint8_t *f, const struct omci_mcast_env *env,
		       const uint8_t *dst, size_t len)
{
	uint8_t *ip = &f[ETH_LEN];

	mac6(f, dst);
	memcpy(&f[6], env->mac, OMCI_MCAST_MAC_LEN);
	omci_be16_store(&f[12], ETH_IPV6);
	memset(ip, 0, IP6_LEN + HBH_LEN);
	ip[0] = 0x60u;
	omci_be16_store(&ip[4], HBH_LEN + len);
	ip[6] = PROTO_HOPOPTS;
	ip[7] = 1u;
	memcpy(&ip[8], env->source, OMCI_MCAST_ADDR_LEN);
	memcpy(&ip[24], dst, OMCI_MCAST_ADDR_LEN);
	ip[40] = PROTO_ICMPV6;
	ip[42] = 0x05u;
	ip[43] = 0x02u;
	ip[46] = 0x01u;
	return ETH_LEN + IP6_LEN + HBH_LEN;
}

/* The checksum of the IGMP message at @at, @len bytes, or of its header. */
static void igmp_sum(uint8_t *f, size_t at, size_t len)
{
	f[at + 2] = 0;
	f[at + 3] = 0;
	omci_be16_store(&f[at + 2], sum_fold(sum_add(0, &f[at], len)));
}

/* The checksum of the MLD message at @at, @len bytes (RFC 8200 8.1). */
static void mld_sum(uint8_t *f, size_t at, size_t len)
{
	const uint8_t *ip = &f[ETH_LEN];
	uint8_t tail[8] = { 0 };
	uint32_t sum = sum_add(0, &ip[8], 2u * OMCI_MCAST_ADDR_LEN);

	omci_be32_store(tail, (unsigned int)len);
	tail[7] = PROTO_ICMPV6;
	sum = sum_add(sum, tail, sizeof(tail));
	f[at + 2] = 0;
	f[at + 3] = 0;
	omci_be16_store(&f[at + 2], sum_fold(sum_add(sum, &f[at], len)));
}

static const uint8_t g_igmp_all_hosts[4] = { 224, 0, 0, 1 };
static const uint8_t g_igmp_routers[4] = { 224, 0, 0, 2 };
static const uint8_t g_igmp_v3_routers[4] = { 224, 0, 0, 22 };
static const uint8_t g_mld_all_nodes[16] = { 0xff, 0x02, [15] = 1 };
static const uint8_t g_mld_routers[16] = { 0xff, 0x02, [15] = 2 };
static const uint8_t g_mld_v2_routers[16] = { 0xff, 0x02, [15] = 0x16 };

/* Whether the item holds the group from anyone. */
static bool held(const struct omci_mcast_item *it)
{
	return it->any || it->n != 0u;
}

/* The message of IGMP version 1 or 2, or MLD version 1, about one group. */
static size_t single(uint8_t *out, size_t cap, const struct omci_mcast_env *env,
		     bool change, const struct omci_mcast_item *it)
{
	const bool v6 = env->version >= 16u;
	const size_t body = v6 ? MLD_V1_QUERY : IGMP_HEAD;
	const size_t head = v6 ? ETH_LEN + IP6_LEN + HBH_LEN :
				 ETH_LEN + IP4_LEN;
	const bool report = held(it);
	size_t at;

	if (head + body > cap || (!report && (!change || env->version == 1u)))
		return 0;
	if (v6) {
		at = headers6(out, env, report ? it->group : g_mld_routers,
			      body);
		memset(&out[at], 0, body);
		out[at] = report ? MLD_V1_REPORT : MLD_V1_DONE;
		memcpy(&out[at + 8], it->group, OMCI_MCAST_ADDR_LEN);
		mld_sum(out, at, body);
		return at + body;
	}
	at = headers4(out, env, report ? &it->group[12] : g_igmp_routers, body);
	memset(&out[at], 0, body);
	out[at] = !report	     ? IGMP_V2_LEAVE :
		  env->version == 1u ? IGMP_V1_REPORT :
				       IGMP_V2_REPORT;
	memcpy(&out[at + 4], &it->group[12], 4);
	igmp_sum(out, at, body);
	return at + body;
}

/* The group record of an item: its type, and its length with the sources. */
static unsigned int record_type(const struct omci_mcast_item *it, bool change)
{
	if (it->any)
		return change ? TO_EXCLUDE : IS_EXCLUDE;
	return change ? TO_INCLUDE : IS_INCLUDE;
}

static size_t record_len(const struct omci_mcast_item *it, size_t alen)
{
	return RECORD_HEAD + alen * (1u + (it->any ? 0u : it->n));
}

/* The message of IGMP version 3 or MLD version 2, with as many groups as fit. */
static size_t multi(uint8_t *out, size_t cap, const struct omci_mcast_env *env,
		    bool change, const struct omci_mcast_item *items, size_t n,
		    size_t *used)
{
	const bool v6 = env->version >= 16u;
	const size_t alen = v6 ? OMCI_MCAST_ADDR_LEN : 4u;
	const size_t head = v6 ? ETH_LEN + IP6_LEN + HBH_LEN :
				 ETH_LEN + IP4_LEN;
	size_t len = REPORT_HEAD, records = 0, at, k;
	uint8_t *p;

	/* Which items go in: those of the family, held, or all on a change. */
	for (k = 0; k < n; k++) {
		if (is_v4(items[k].group) == v6 ||
		    (!change && !held(&items[k])))
			continue;
		if (head + len + record_len(&items[k], alen) > cap)
			break;
		len += record_len(&items[k], alen);
		records++;
	}
	*used = k;
	if (records == 0u)
		return 0;
	at = v6 ? headers6(out, env, g_mld_v2_routers, len) :
		  headers4(out, env, g_igmp_v3_routers, len);
	p = &out[at];
	memset(p, 0, len);
	p[0] = v6 ? MLD_V2_REPORT : IGMP_V3_REPORT;
	omci_be16_store(&p[6], (unsigned int)records);
	p += REPORT_HEAD;
	for (size_t i = 0; i < k; i++) {
		const struct omci_mcast_item *it = &items[i];
		const unsigned int nsrc = it->any ? 0u : it->n;

		if (is_v4(it->group) == v6 || (!change && !held(it)))
			continue;
		p[0] = (uint8_t)record_type(it, change);
		omci_be16_store(&p[2], nsrc);
		memcpy(&p[RECORD_HEAD], v6 ? it->group : &it->group[12], alen);
		for (unsigned int s = 0; s < nsrc; s++)
			memcpy(&p[RECORD_HEAD + alen * (1u + s)],
			       v6 ? it->source[s] : &it->source[s][12], alen);
		p += record_len(it, alen);
	}
	if (v6)
		mld_sum(out, at, len);
	else
		igmp_sum(out, at, len);
	return at + len;
}

size_t omci_mcast_build_report(uint8_t *out, size_t cap,
			       const struct omci_mcast_env *env, bool change,
			       const struct omci_mcast_item *items, size_t n,
			       size_t *used)
{
	const bool v6 = env->version >= 16u;

	*used = 0;
	if (n == 0u)
		return 0;
	if (env->version == 3u || env->version == 17u)
		return multi(out, cap, env, change, items, n, used);
	*used = 1;
	if (is_v4(items[0].group) == v6)
		return 0;
	return single(out, cap, env, change, &items[0]);
}

size_t omci_mcast_build_query(uint8_t *out, size_t cap,
			      const struct omci_mcast_env *env,
			      const uint8_t *group, unsigned int max_resp,
			      unsigned int robustness, unsigned int interval)
{
	static const uint8_t none[OMCI_MCAST_ADDR_LEN];
	const bool v6 = env->version >= 16u;
	const bool general = group == NULL ||
			     memcmp(group, none, sizeof(none)) == 0;
	const bool v3 = env->version == 3u || env->version == 17u;
	const size_t body = v6 ? (v3 ? MLD_V2_QUERY : MLD_V1_QUERY) :
				 (v3 ? IGMP_V3_QUERY : IGMP_HEAD);
	const size_t head = v6 ? ETH_LEN + IP6_LEN + HBH_LEN :
				 ETH_LEN + IP4_LEN;
	const uint8_t qrv = robustness <= 7u ? (uint8_t)robustness : 0u;
	const uint8_t *dst;
	size_t at;

	if (head + body > cap || (!general && is_v4(group) == v6))
		return 0;
	if (v6)
		dst = general ? g_mld_all_nodes : group;
	else
		dst = general ? g_igmp_all_hosts : &group[12];
	if (v6) {
		at = headers6(out, env, dst, body);
		memset(&out[at], 0, body);
		out[at] = MLD_QUERY;
		if (v3)
			omci_be16_store(&out[at + 4],
					float_code(max_resp * 100u, 32768u, 12u,
						   12u));
		else
			omci_be16_store(&out[at + 4],
					max_resp * 100u > 0xffffu ?
						0xffffu :
						max_resp * 100u);
		if (!general)
			memcpy(&out[at + 8], group, OMCI_MCAST_ADDR_LEN);
		if (v3) {
			out[at + 24] = qrv;
			out[at + 25] =
				(uint8_t)float_code(interval, 128u, 4u, 4u);
		}
		mld_sum(out, at, body);
		return at + body;
	}
	at = headers4(out, env, dst, body);
	memset(&out[at], 0, body);
	out[at] = IGMP_QUERY;
	if (v3)
		out[at + 1] = (uint8_t)float_code(max_resp, 128u, 4u, 4u);
	else if (env->version == 2u)
		out[at + 1] = (uint8_t)(max_resp > 255u ? 255u : max_resp);
	if (!general)
		memcpy(&out[at + 4], &group[12], 4);
	if (v3) {
		out[at + 8] = qrv;
		out[at + 9] = (uint8_t)float_code(interval, 128u, 4u, 4u);
	}
	igmp_sum(out, at, body);
	return at + body;
}
