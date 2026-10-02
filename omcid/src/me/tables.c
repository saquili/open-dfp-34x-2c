// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The table policies and the tables made on demand, of tables.h. */

#include <stdbool.h>

#include "hw/hw.h"
#include "me/action.h"
#include "me/table.h"
#include "me/tables.h"
#include "omcid/array_size.h"
#include "sys/wire.h"

enum {
	ME_EXT_VLAN_TAG_OPER = 171,
	ME_MCAST_GEM_IW_TP = 281,
	ME_OMCI = 287,
	ME_MCAST_OPS_PROFILE = 309,
	ME_MCAST_SUBSCRIBER = 310,
	ME_MCAST_MONITOR = 311,
};

/* A table whose rows are known by all their bytes, kept in arrival order. */
#define WHOLE_ROW(size, rows)           \
	{ .key_len = (size),            \
	  .order = OMCI_MIB_TABLE_LAST, \
	  .max_entries = (rows) }

/*
 * The policies. A rule of the extended VLAN tagging operation table (class
 * 171, attribute 6) is known by its eight filter bytes and removed by a row
 * whose eight treatment bytes are all ones; it may hold as many rules as
 * the hardware classifier can. The multicast address tables of class 281
 * are known by their first four bytes and remove a row whose address
 * fields are all zero. The others keep whole rows.
 */
static const struct {
	uint16_t class_id;
	uint8_t attr_no;
	struct omci_me_table_policy policy;
} policies[] = {
	{ ME_EXT_VLAN_TAG_OPER,
	  6,
	  { .key_len = 8,
	    .del_offset = 8,
	    .del_len = 8,
	    .del_value = 0xffu,
	    .order = OMCI_MIB_TABLE_LAST,
	    .max_entries = OMCI_HW_VLAN_MAX_RULES } },

	{ ME_EXT_VLAN_TAG_OPER, 10, WHOLE_ROW(28, 16) },

	{ ME_MCAST_GEM_IW_TP,
	  9,
	  { .key_len = 4,
	    .del_offset = 4,
	    .del_len = 8,
	    .del_value = 0x00u,
	    .order = OMCI_MIB_TABLE_LAST,
	    .max_entries = 16 } },
	{ ME_MCAST_GEM_IW_TP,
	  10,
	  { .key_len = 4,
	    .del_offset = 4,
	    .del_len = 20,
	    .del_value = 0x00u,
	    .order = OMCI_MIB_TABLE_LAST,
	    .max_entries = 16 } },

	{ ME_MCAST_OPS_PROFILE, 7, WHOLE_ROW(24, 16) },
	{ ME_MCAST_OPS_PROFILE, 8, WHOLE_ROW(24, 16) },
	{ ME_MCAST_SUBSCRIBER, 6, WHOLE_ROW(20, 16) },
	{ ME_MCAST_SUBSCRIBER, 7, WHOLE_ROW(22, 16) },
};

bool omci_me_table_policy(uint16_t class_id, uint8_t attr_no,
			  struct omci_me_table_policy *out)
{
	if (out == NULL)
		return false;

	for (size_t i = 0; i < ARRAY_SIZE(policies); i++) {
		if (policies[i].class_id != class_id ||
		    policies[i].attr_no != attr_no)
			continue;
		*out = policies[i].policy;
		return true;
	}
	return false;
}

bool omci_me_table_is_delete(const struct omci_me_table_policy *policy,
			     const void *row, size_t len)
{
	if (policy == NULL || row == NULL || policy->del_len == 0)
		return false;
	if ((size_t)policy->del_offset + policy->del_len > len)
		return false;

	const uint8_t *e = row;

	for (uint8_t i = 0; i < policy->del_len; i++)
		if (e[policy->del_offset + i] != policy->del_value)
			return false;
	return true;
}

/* The two tables of the OMCI entity, class 287 (G.988 9.12.8). */
enum {
	ME_ID_BYTES = 2,
	ATTR_ME_TYPES = 1,
	ATTR_MSG_TYPES = 2,
};

/* The ME type table: the class identifier of every class the device has. */
static size_t me_list_count(uint16_t instance)
{
	(void)instance;
	return omci_me_class_count;
}

static bool me_list_entry(uint16_t instance, size_t index, void *out,
			  size_t cap)
{
	(void)instance;
	if (out == NULL || cap < ME_ID_BYTES || index >= omci_me_class_count)
		return false;

	uint8_t *p = out;

	p[0] = (uint8_t)(omci_me_classes[index].class_id >> 8);
	p[1] = (uint8_t)omci_me_classes[index].class_id;
	return true;
}

/* The message type table: the types some class accepts and the engine executes. */
static uint32_t msg_type_set(void)
{
	uint32_t listed = 0;

	for (size_t i = 0; i < omci_me_class_count; i++)
		listed |= omci_me_classes[i].actions;
	return listed & OMCI_ME_MSG_IMPLEMENTED;
}

static size_t msg_list_count(uint16_t instance)
{
	(void)instance;

	size_t n = 0;

	for (uint32_t set = msg_type_set(); set != 0u; set &= set - 1u)
		n++;
	return n;
}

static bool msg_list_entry(uint16_t instance, size_t index, void *out,
			   size_t cap)
{
	(void)instance;
	if (out == NULL || cap < 1u)
		return false;

	const uint32_t set = msg_type_set();

	for (uint8_t mt = 0; mt < 32u; mt++) {
		if ((set & (1u << mt)) == 0u)
			continue;
		if (index-- == 0u) {
			*(uint8_t *)out = mt;
			return true;
		}
	}
	return false;
}

/* A table that stays empty: the device does not track what it would list. */
static size_t no_rows(uint16_t instance)
{
	(void)instance;
	return 0;
}

static bool no_row(uint16_t instance, size_t index, void *out, size_t cap)
{
	(void)instance;
	(void)index;
	(void)out;
	(void)cap;
	return false;
}

static const struct omci_me_table_source me_list_source = {
	.count = me_list_count,
	.entry = me_list_entry
};
static const struct omci_me_table_source msg_list_source = {
	.count = msg_list_count,
	.entry = msg_list_entry
};
static const struct omci_me_table_source empty_source = { .count = no_rows,
							  .entry = no_row };

/*
 * The tables made on demand: the two of the OMCI entity, and the lists of
 * multicast groups, lost and active, which stay empty.
 */
static const struct {
	uint16_t class_id;
	uint8_t attr_no;
	const struct omci_me_table_source *source;
} rendered[] = {
	{ ME_OMCI, ATTR_ME_TYPES, &me_list_source },
	{ ME_OMCI, ATTR_MSG_TYPES, &msg_list_source },
	{ ME_MCAST_OPS_PROFILE, 9, &empty_source },
	{ ME_MCAST_MONITOR, 5, &empty_source },
	{ ME_MCAST_MONITOR, 6, &empty_source },
};

bool omci_me_table_rendered(uint16_t class_id, uint8_t attr_no,
			    const struct omci_me_table_source **out)
{
	if (out == NULL)
		return false;

	for (size_t i = 0; i < ARRAY_SIZE(rendered); i++) {
		if (rendered[i].class_id != class_id ||
		    rendered[i].attr_no != attr_no)
			continue;
		*out = rendered[i].source;
		return true;
	}
	return false;
}
