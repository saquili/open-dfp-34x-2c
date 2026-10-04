// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The tables of the multicast managed entities, of acl.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mcast/acl.h"
#include "me/tables.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"

/*
 * The row parts: of an access control list, 24 bytes, in three formats
 * (G.988 9.3.27); of the service package table, 20; of the allowed preview
 * groups table, 22, in two formats (9.3.28).
 */
enum {
	PART_LEN = 24,
	MAX_PARTS = 3,

	/* Part 0 of an access control list row. */
	ACL_GEM_PORT = 2,
	ACL_VLAN = 4,
	ACL_SOURCE = 6,
	ACL_FIRST = 10,
	ACL_LAST = 14,
	ACL_BANDWIDTH = 18,
	/* Part 1, and the leading bytes of part 2. */
	ACL_LEAD = 2,
	ACL_PREVIEW_LENGTH = 14,
	ACL_PREVIEW_REPEAT_TIME = 16,
	ACL_PREVIEW_REPEAT_COUNT = 18,
	ACL_PREVIEW_RESET_TIME = 20,

	/* A service package. */
	PKG_VID = 2,
	PKG_MAX_GROUPS = 4,
	PKG_MAX_BANDWIDTH = 6,
	PKG_PROFILE = 10,

	/* The two parts of an allowed preview group. */
	PREVIEW_ADDR = 2,
	PREVIEW_VLAN_ANI = 18,
	PREVIEW_VLAN_UNI = 20,

	/* An address: the leading bytes before the IPv4 ones. */
	V4_ZEROS = 10,
	V4_AT = 12,
};

/* The row parts that share a row key; bit n of @have for part n. */
struct parts {
	uint16_t key;
	uint8_t have;
	uint8_t part[MAX_PARTS][PART_LEN];
};

bool omci_mcast_addr_v4(const uint8_t addr[static OMCI_MCAST_ADDR_LEN])
{
	for (unsigned int i = 0; i < V4_ZEROS; i++)
		if (addr[i] != 0u)
			return false;
	return addr[V4_ZEROS] == 0xffu && addr[V4_ZEROS + 1] == 0xffu;
}

void omci_mcast_addr_set_v4(uint8_t addr[static OMCI_MCAST_ADDR_LEN],
			    const uint8_t v4[static 4])
{
	memset(addr, 0, V4_ZEROS);
	addr[V4_ZEROS] = 0xffu;
	addr[V4_ZEROS + 1] = 0xffu;
	memcpy(&addr[V4_AT], v4, 4);
}

/*
 * The row parts of a table, by row key in key order; @extra, a part a Set
 * is about to write, in place of the stored part with its key and part. A
 * table holds no more parts than its policy lets in, which @out has room for
 * with one more.
 */
static size_t gather(const struct omci_mib *mib, uint16_t class_id,
		     uint16_t instance, uint8_t attr_no, const uint8_t *extra,
		     size_t extra_len, struct parts *out)
{
	struct omci_me_table_policy policy;
	const size_t rows =
		omci_mib_table_count(mib, class_id, instance, attr_no);
	size_t n = 0;

	(void)!omci_me_table_policy(class_id, attr_no, &policy);
	for (size_t i = 0; i <= rows; i++) {
		uint8_t row[OMCI_MIB_TABLE_ENTRY_MAX] = { 0 };
		struct omci_me_table_row_id id;
		size_t at = 0;

		if (i < rows)
			(void)!omci_mib_table_get(mib, class_id, instance,
						  attr_no, i, row, sizeof(row));
		else if (extra != NULL)
			memcpy(row, extra, extra_len);
		else
			break;

		omci_me_table_row_id(&policy, row, &id);
		while (at < n && out[at].key < id.key)
			at++;
		if (at == n || out[at].key != id.key) {
			memmove(&out[at + 1], &out[at],
				(n - at) * sizeof(out[0]));
			memset(&out[at], 0, sizeof(out[0]));
			out[at].key = id.key;
			n++;
		}
		memcpy(out[at].part[id.part], row, PART_LEN);
		out[at].have |= (uint8_t)(1u << id.part);
	}
	return n;
}

/* Leading bytes that make the address an IPv4 one: ten zeros, then 0 or 0xffff. */
static bool v4_lead(const uint8_t *lead)
{
	for (unsigned int i = 0; i < V4_ZEROS; i++)
		if (lead[i] != 0u)
			return false;
	return lead[V4_ZEROS] == lead[V4_ZEROS + 1] &&
	       (lead[V4_ZEROS] == 0u || lead[V4_ZEROS] == 0xffu);
}

/*
 * An address of an access control list row: its four bytes in part 0, after
 * the twelve leading bytes of part 1 or 2 when the row has that part
 * (G.988 9.3.27).
 */
static void acl_addr(const uint8_t *lead, const uint8_t *low,
		     uint8_t out[static OMCI_MCAST_ADDR_LEN])
{
	if (lead != NULL && !v4_lead(lead)) {
		memcpy(out, lead, V4_AT);
		memcpy(&out[V4_AT], low, 4);
		return;
	}
	omci_mcast_addr_set_v4(out, low);
}

/* An access control list row from its parts; false without part 0. */
static bool acl_row(const struct parts *p, struct omci_mcast_acl_row *out)
{
	const uint8_t *p0 = p->part[0], *p1 = p->part[1], *p2 = p->part[2];
	const uint8_t *src_lead = (p->have & 0x2u) ? &p1[ACL_LEAD] : NULL;
	const uint8_t *dst_lead = (p->have & 0x4u) ? &p2[ACL_LEAD] : NULL;

	if (!(p->have & 0x1u))
		return false;

	memset(out, 0, sizeof(*out));
	out->key = p->key;
	out->gem_port = (uint16_t)omci_be16(&p0[ACL_GEM_PORT]);
	out->vlan = (uint16_t)omci_be16(&p0[ACL_VLAN]);
	acl_addr(src_lead, &p0[ACL_SOURCE], out->source);
	out->any_source = omci_mcast_addr_v4(out->source) &&
			  omci_be32(&out->source[V4_AT]) == 0u;
	acl_addr(dst_lead, &p0[ACL_FIRST], out->first);
	acl_addr(dst_lead, &p0[ACL_LAST], out->last);
	out->bandwidth = omci_be32(&p0[ACL_BANDWIDTH]);
	if (src_lead != NULL) {
		out->preview_length =
			(uint16_t)omci_be16(&p1[ACL_PREVIEW_LENGTH]);
		out->preview_repeat_time =
			(uint16_t)omci_be16(&p1[ACL_PREVIEW_REPEAT_TIME]);
		out->preview_repeat_count =
			(uint16_t)omci_be16(&p1[ACL_PREVIEW_REPEAT_COUNT]);
		out->preview_reset_time =
			(uint16_t)omci_be16(&p1[ACL_PREVIEW_RESET_TIME]);
	}
	return true;
}

size_t omci_mcast_acl_read(const struct omci_mib *mib, uint16_t profile,
			   uint8_t attr_no, struct omci_mcast_acl_row *out,
			   size_t cap)
{
	struct parts parts[OMCI_MCAST_ACL_ROWS + 1];
	const size_t n = gather(mib, OMCI_MCAST_PROFILE, profile, attr_no, NULL,
				0, parts);
	size_t got = 0;

	for (size_t i = 0; i < n && got < cap; i++)
		if (acl_row(&parts[i], &out[got]))
			got++;
	return got;
}

bool omci_mcast_acl_matches(const struct omci_mcast_acl_row *row,
			    const uint8_t group[static OMCI_MCAST_ADDR_LEN],
			    const uint8_t source[static OMCI_MCAST_ADDR_LEN])
{
	if (memcmp(group, row->first, OMCI_MCAST_ADDR_LEN) < 0 ||
	    memcmp(group, row->last, OMCI_MCAST_ADDR_LEN) > 0)
		return false;
	return row->any_source ||
	       memcmp(source, row->source, OMCI_MCAST_ADDR_LEN) == 0;
}

/* Two rows that a join could find both. */
static bool overlap(const struct omci_mcast_acl_row *a,
		    const struct omci_mcast_acl_row *b)
{
	if (memcmp(a->first, b->last, OMCI_MCAST_ADDR_LEN) > 0 ||
	    memcmp(b->first, a->last, OMCI_MCAST_ADDR_LEN) > 0)
		return false;
	return a->any_source || b->any_source ||
	       memcmp(a->source, b->source, OMCI_MCAST_ADDR_LEN) == 0;
}

bool omci_mcast_acl_conflicts(const struct omci_mib *mib, uint16_t profile,
			      const uint8_t *row, size_t len)
{
	struct omci_me_table_policy policy;
	struct omci_me_table_row_id id;
	struct parts parts[OMCI_MCAST_ACL_ROWS + 1];
	struct omci_mcast_acl_row mine, other;
	size_t n, at = 0;

	(void)!omci_me_table_policy(OMCI_MCAST_PROFILE,
				    OMCI_MCAST_PROFILE_DYNAMIC_ACL, &policy);
	omci_me_table_row_id(&policy, row, &id);
	if (id.op != OMCI_ME_TABLE_OP_WRITE || id.part >= MAX_PARTS ||
	    len != PART_LEN)
		return false;
	if (id.part == 0u &&
	    omci_be32(&row[ACL_LAST]) < omci_be32(&row[ACL_FIRST]))
		return true;

	n = gather(mib, OMCI_MCAST_PROFILE, profile,
		   OMCI_MCAST_PROFILE_DYNAMIC_ACL, row, len, parts);
	while (parts[at].key != id.key)
		at++;
	if (!acl_row(&parts[at], &mine))
		return false;
	for (size_t i = 0; i < n; i++)
		if (i != at && acl_row(&parts[i], &other) &&
		    overlap(&mine, &other))
			return true;
	return false;
}

size_t omci_mcast_packages_read(const struct omci_mib *mib, uint16_t subscriber,
				struct omci_mcast_package *out, size_t cap)
{
	struct parts parts[OMCI_MCAST_ACL_ROWS + 1];
	const size_t n = gather(mib, OMCI_MCAST_SUBSCRIBER, subscriber,
				OMCI_MCAST_SUBSCRIBER_PACKAGES, NULL, 0, parts);
	size_t got = 0;

	for (size_t i = 0; i < n && got < cap; i++) {
		const uint8_t *p = parts[i].part[0];

		out[got].key = parts[i].key;
		out[got].vid = (uint16_t)omci_be16(&p[PKG_VID]);
		out[got].max_groups = (uint16_t)omci_be16(&p[PKG_MAX_GROUPS]);
		out[got].max_bandwidth = omci_be32(&p[PKG_MAX_BANDWIDTH]);
		out[got].profile = (uint16_t)omci_be16(&p[PKG_PROFILE]);
		got++;
	}
	return got;
}

/*
 * An address of an allowed preview group: IPv4 when its first twelve bytes
 * are zero (G.988 9.3.28).
 */
static void preview_addr(const uint8_t *in,
			 uint8_t out[static OMCI_MCAST_ADDR_LEN])
{
	static const uint8_t zero[V4_AT];

	if (memcmp(in, zero, V4_AT) == 0) {
		omci_mcast_addr_set_v4(out, &in[V4_AT]);
		return;
	}
	memcpy(out, in, OMCI_MCAST_ADDR_LEN);
}

size_t omci_mcast_previews_read(const struct omci_mib *mib, uint16_t subscriber,
				struct omci_mcast_preview *out, size_t cap)
{
	struct parts parts[OMCI_MCAST_ACL_ROWS + 1];
	const size_t n = gather(mib, OMCI_MCAST_SUBSCRIBER, subscriber,
				OMCI_MCAST_SUBSCRIBER_PREVIEW, NULL, 0, parts);
	size_t got = 0;

	for (size_t i = 0; i < n && got < cap; i++) {
		const uint8_t *p0 = parts[i].part[0], *p1 = parts[i].part[1];
		struct omci_mcast_preview *r = &out[got];

		if (parts[i].have != 0x3u)
			continue;
		r->key = parts[i].key;
		preview_addr(&p0[PREVIEW_ADDR], r->source);
		r->any_source = omci_mcast_addr_v4(r->source) &&
				omci_be32(&r->source[V4_AT]) == 0u;
		r->vlan_ani = (uint16_t)omci_be16(&p0[PREVIEW_VLAN_ANI]);
		r->vlan_uni = (uint16_t)omci_be16(&p0[PREVIEW_VLAN_UNI]);
		preview_addr(&p1[PREVIEW_ADDR], r->group);
		r->duration =
			(uint16_t)omci_be16(&p1[OMCI_MCAST_PREVIEW_DURATION]);
		r->time_left =
			(uint16_t)omci_be16(&p1[OMCI_MCAST_PREVIEW_TIME_LEFT]);
		got++;
	}
	return got;
}

bool omci_mcast_preview_written(const struct omci_mib *mib, uint16_t subscriber,
				uint8_t *row, size_t len)
{
	struct omci_me_table_policy policy;
	struct omci_me_table_row_id id;
	struct parts parts[OMCI_MCAST_ACL_ROWS + 1];
	const unsigned int duration =
		omci_be16(&row[OMCI_MCAST_PREVIEW_DURATION]);
	long left = (long)duration;
	size_t n;

	(void)!omci_me_table_policy(OMCI_MCAST_SUBSCRIBER,
				    OMCI_MCAST_SUBSCRIBER_PREVIEW, &policy);
	omci_me_table_row_id(&policy, row, &id);
	if (id.op != OMCI_ME_TABLE_OP_WRITE || id.part != 1u ||
	    len != PART_LEN - 2u)
		return true;

	n = gather(mib, OMCI_MCAST_SUBSCRIBER, subscriber,
		   OMCI_MCAST_SUBSCRIBER_PREVIEW, NULL, 0, parts);
	for (size_t i = 0; i < n; i++) {
		const uint8_t *was = parts[i].part[1];

		if (parts[i].key != id.key || !(parts[i].have & 0x2u))
			continue;
		left = (long)omci_be16(&was[OMCI_MCAST_PREVIEW_TIME_LEFT]) +
		       (long)duration -
		       (long)omci_be16(&was[OMCI_MCAST_PREVIEW_DURATION]);
	}
	if (duration == 0u)
		left = 0;
	else if (left <= 0)
		return false;
	if (left > 0xffffL)
		left = 0xffffL;
	omci_be16_store(&row[OMCI_MCAST_PREVIEW_TIME_LEFT], (unsigned int)left);
	return true;
}

bool omci_mcast_preview_count_down(struct omci_mib *mib, uint16_t subscriber)
{
	struct omci_me_table_policy policy;
	struct parts parts[OMCI_MCAST_ACL_ROWS + 1];
	const size_t n = gather(mib, OMCI_MCAST_SUBSCRIBER, subscriber,
				OMCI_MCAST_SUBSCRIBER_PREVIEW, NULL, 0, parts);
	bool ended = false;

	(void)!omci_me_table_policy(OMCI_MCAST_SUBSCRIBER,
				    OMCI_MCAST_SUBSCRIBER_PREVIEW, &policy);
	for (size_t i = 0; i < n; i++) {
		uint8_t *p1 = parts[i].part[1];
		unsigned int left;

		if (!(parts[i].have & 0x2u) ||
		    omci_be16(&p1[OMCI_MCAST_PREVIEW_DURATION]) == 0u)
			continue;
		left = omci_be16(&p1[OMCI_MCAST_PREVIEW_TIME_LEFT]);
		if (left <= 1u) {
			omci_me_table_op_set(p1, OMCI_ME_TABLE_OP_DELETE);
			ended = true;
		} else {
			omci_be16_store(&p1[OMCI_MCAST_PREVIEW_TIME_LEFT],
					left - 1u);
			omci_me_table_op_set(p1, OMCI_ME_TABLE_OP_WRITE);
		}
		(void)!omci_me_table_ctrl_set(mib, OMCI_MCAST_SUBSCRIBER,
					      subscriber,
					      OMCI_MCAST_SUBSCRIBER_PREVIEW,
					      &policy, p1, PART_LEN - 2u);
	}
	return ended;
}
