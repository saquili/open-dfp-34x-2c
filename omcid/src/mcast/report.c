// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The IGMP and MLD messages of report.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mcast/report.h"
#include "omcid/byteorder.h"

enum {
	/* Ethernet, and the two kinds of VLAN tag. */
	ETH_TYPE = 12,
	TAG_LEN = 4,
	ETH_IPV4 = 0x0800,
	ETH_IPV6 = 0x86dd,
	ETH_CTAG = 0x8100,
	ETH_STAG = 0x88a8,

	/* IPv4 and IGMP. */
	IP4_MIN = 20,
	IP4_TOTAL = 2,
	IP4_PROTO = 9,
	IP4_SRC = 12,
	PROTO_IGMP = 2,
	IGMP_V1_REPORT = 0x12,
	IGMP_V2_REPORT = 0x16,
	IGMP_V2_LEAVE = 0x17,
	IGMP_V3_REPORT = 0x22,
	IGMP_GROUP = 4,

	/* IPv6, its hop-by-hop options, and MLD. */
	IP6_LEN = 40,
	IP6_PAYLOAD = 4,
	IP6_NEXT = 6,
	IP6_SRC = 8,
	PROTO_HOPOPTS = 0,
	PROTO_ICMPV6 = 58,
	MLD_V1_REPORT = 131,
	MLD_V1_DONE = 132,
	MLD_V2_REPORT = 143,
	MLD_GROUP = 8,

	/* The records of a version 3 or MLDv2 report (RFC 3376 4.2, 3810 5.2). */
	REPORT_RECORDS = 6,
	REPORT_FIRST = 8,
	RECORD_HEAD = 4,
	IS_INCLUDE = 1,
	IS_EXCLUDE = 2,
	TO_INCLUDE = 3,
	TO_EXCLUDE = 4,
	ALLOW = 5,
	BLOCK = 6,
};

/* An address of @len bytes, IPv4 or IPv6, as acl.h keeps it. */
static void addr(uint8_t out[static OMCI_MCAST_ADDR_LEN], const uint8_t *in,
		 size_t len)
{
	if (len == 4u)
		omci_mcast_addr_set_v4(out, in);
	else
		memcpy(out, in, OMCI_MCAST_ADDR_LEN);
}

/* Keep a record: @source of @alen bytes, or NULL for every source. */
static void add(struct omci_mcast_report *r, bool join, const uint8_t *group,
		const uint8_t *source, size_t alen)
{
	struct omci_mcast_record *rec;

	if (r->n == OMCI_MCAST_REPORT_RECORDS) {
		if (r->cut < UINT8_MAX)
			r->cut++;
		return;
	}
	rec = &r->rec[r->n++];
	rec->join = join;
	addr(rec->group, group, alen);
	if (source != NULL)
		addr(rec->source, source, alen);
	else
		memset(rec->source, 0, sizeof(rec->source));
}

/*
 * The records of an IGMPv3 or MLDv2 report, its addresses @alen bytes long,
 * from @p to @end. Return: false when one runs past the end.
 */
static bool records(struct omci_mcast_report *r, const uint8_t *p,
		    const uint8_t *end, size_t alen)
{
	const unsigned int n = omci_be16(&p[REPORT_RECORDS]);

	p += REPORT_FIRST;
	for (unsigned int i = 0; i < n; i++) {
		size_t size;
		unsigned int nsrc;
		const uint8_t *group, *src;

		if ((size_t)(end - p) < RECORD_HEAD + alen)
			return false;
		nsrc = omci_be16(&p[2]);
		size = RECORD_HEAD + alen * (1u + nsrc) + 4u * p[1];
		if ((size_t)(end - p) < size)
			return false;
		group = &p[RECORD_HEAD];
		src = group + alen;

		switch (p[0]) {
		case IS_EXCLUDE:
		case TO_EXCLUDE:
			add(r, true, group, NULL, alen);
			break;
		case TO_INCLUDE:
		case IS_INCLUDE:
			if (nsrc == 0u || p[0] == TO_INCLUDE)
				add(r, false, group, NULL, alen);
			for (unsigned int s = 0; s < nsrc; s++)
				add(r, true, group, src + s * alen, alen);
			break;
		case ALLOW:
		case BLOCK:
			for (unsigned int s = 0; s < nsrc; s++)
				add(r, p[0] == ALLOW, group, src + s * alen,
				    alen);
			break;
		default:
			break;
		}
		p += size;
	}
	return true;
}

static bool igmp(const uint8_t *ip, size_t len, struct omci_mcast_report *r)
{
	size_t hl, total;
	const uint8_t *m;

	if (len < IP4_MIN || ip[0] >> 4 != 4u)
		return false;
	hl = (ip[0] & 0xfu) * 4u;
	total = omci_be16(&ip[IP4_TOTAL]);
	if (hl < IP4_MIN || total < hl + 8u || total > len ||
	    ip[IP4_PROTO] != PROTO_IGMP)
		return false;

	omci_mcast_addr_set_v4(r->client, &ip[IP4_SRC]);
	m = &ip[hl];
	switch (m[0]) {
	case IGMP_V1_REPORT:
	case IGMP_V2_REPORT:
		r->kind = OMCI_MCAST_KIND_REPORT;
		r->version = m[0] == IGMP_V1_REPORT ? 1u : 2u;
		add(r, true, &m[IGMP_GROUP], NULL, 4u);
		return true;
	case IGMP_V2_LEAVE:
		r->kind = OMCI_MCAST_KIND_LEAVE;
		r->version = 2u;
		add(r, false, &m[IGMP_GROUP], NULL, 4u);
		return true;
	case IGMP_V3_REPORT:
		r->kind = OMCI_MCAST_KIND_REPORT;
		r->version = 3u;
		return records(r, m, &ip[total], 4u);
	default:
		return true;
	}
}

static bool mld(const uint8_t *ip, size_t len, struct omci_mcast_report *r)
{
	const uint8_t *p, *end;
	unsigned int next;

	if (len < IP6_LEN || ip[0] >> 4 != 6u ||
	    IP6_LEN + omci_be16(&ip[IP6_PAYLOAD]) > len)
		return false;
	end = &ip[IP6_LEN + omci_be16(&ip[IP6_PAYLOAD])];
	p = &ip[IP6_LEN];
	next = ip[IP6_NEXT];
	if (next == PROTO_HOPOPTS) {
		if (end - p < 8 || (size_t)(end - p) < (p[1] + 1u) * 8u)
			return false;
		next = p[0];
		p += (p[1] + 1u) * 8u;
	}
	if (next != PROTO_ICMPV6 || end - p < 8)
		return false;

	memcpy(r->client, &ip[IP6_SRC], OMCI_MCAST_ADDR_LEN);
	switch (p[0]) {
	case MLD_V1_REPORT:
	case MLD_V1_DONE:
		if ((size_t)(end - p) < MLD_GROUP + OMCI_MCAST_ADDR_LEN)
			return false;
		r->kind = p[0] == MLD_V1_REPORT ? OMCI_MCAST_KIND_REPORT :
						  OMCI_MCAST_KIND_LEAVE;
		r->version = 16u;
		add(r, p[0] == MLD_V1_REPORT, &p[MLD_GROUP], NULL,
		    OMCI_MCAST_ADDR_LEN);
		return true;
	case MLD_V2_REPORT:
		r->kind = OMCI_MCAST_KIND_REPORT;
		r->version = 17u;
		return records(r, p, end, OMCI_MCAST_ADDR_LEN);
	default:
		return true;
	}
}

bool omci_mcast_report_parse(const uint8_t *frame, size_t len, uint16_t tci,
			     bool tagged, struct omci_mcast_report *out)
{
	size_t at = ETH_TYPE;
	unsigned int type;

	memset(out, 0, sizeof(*out));
	if (tagged) {
		out->tagged = true;
		out->vid = tci & 0x0fffu;
		out->pcp = (uint8_t)(tci >> 13);
	}
	if (frame == NULL || len < ETH_TYPE + 2u)
		return false;

	/* The tags still in the frame: the outer one, then an inner one. */
	type = omci_be16(&frame[at]);
	for (unsigned int i = 0;
	     i < 2u && (type == ETH_CTAG || type == ETH_STAG); i++) {
		if (len < at + TAG_LEN + 2u)
			return false;
		if (!out->tagged) {
			const unsigned int t = omci_be16(&frame[at + 2u]);

			out->tagged = true;
			out->vid = (uint16_t)(t & 0x0fffu);
			out->pcp = (uint8_t)(t >> 13);
		}
		at += TAG_LEN;
		type = omci_be16(&frame[at]);
	}
	at += 2u;

	if (type == ETH_IPV4)
		return igmp(&frame[at], len - at, out);
	if (type == ETH_IPV6)
		return mld(&frame[at], len - at, out);
	return false;
}
