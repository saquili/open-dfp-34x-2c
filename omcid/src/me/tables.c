// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The table policies and the tables made on demand, of tables.h. */

#include <stdbool.h>

#include "hw/hw.h"
#include "mcast/acl.h"
#include "mcast/agent.h"
#include "me/action.h"
#include "me/fdb.h"
#include "me/table.h"
#include "me/tables.h"
#include "omcid/array_size.h"
#include "sys/wire.h"

enum {
	ME_BRIDGE_PORT_FILTER = 49,
	ME_BRIDGE_PORT_TABLE = 50,
	ME_VOICE_SERVICE_PROFILE = 58,
	ME_DIAL_PLAN = 145,
	ME_SIP_AGENT = 150,
	ME_EXT_VLAN_TAG_OPER = 171,
	ME_MCAST_GEM_IW_TP = 281,
	ME_OMCI = 287,
	ME_MCAST_OPS_PROFILE = 309,
	ME_MCAST_SUBSCRIBER = 310,
	ME_MCAST_MONITOR = 311,
	ME_IPV6_HOST = 347,
};

/* A table whose rows are known by all their bytes, kept in arrival order. */
#define WHOLE_ROW(size, rows)           \
	{ .key_len = (size),            \
	  .order = OMCI_MIB_TABLE_LAST, \
	  .max_entries = (rows) }

/* A table whose rows start with their key of @len bytes, zeros clearing it. */
#define INDEX_ROWS(len, rows)           \
	{ .key_len = (len),             \
	  .order = OMCI_MIB_TABLE_LAST, \
	  .max_entries = (rows),        \
	  .ctrl = OMCI_ME_TABLE_CTRL_INDEX }

/*
 * A table whose rows start with a table control field, kept in arrival
 * order: up to @rows row parts, the last part @last.
 */
#define CTRL_ROWS(kind, last, rows)          \
	{ .key_len = OMCI_ME_TABLE_CTRL_LEN, \
	  .order = OMCI_MIB_TABLE_LAST,      \
	  .max_entries = (rows),             \
	  .ctrl = (kind),                    \
	  .max_part = (last) }

/*
 * The policies. A rule of the extended VLAN tagging operation table (class
 * 171, attribute 6) is known by its eight filter bytes and removed by a row
 * whose eight treatment bytes are all ones; it may hold as many rules as
 * the hardware classifier can. The multicast address tables of class 281
 * are known by their first four bytes and remove a row whose address
 * fields are all zero. The access control lists of the multicast operations
 * profile (class 309) have rows of three parts, and the allowed preview
 * groups of the multicast subscriber config info (class 310) rows of two;
 * its service packages have one; a table control field leads each part. The
 * MAC filter table of a bridge port (class 49) is known by its entry number,
 * and holds as many addresses as the port is given rules for. The tone and
 * ringing tables of the voice service profile (class 58) are known by their
 * first byte, an index or an event, and the SIP response table of the SIP
 * agent (class 150) by its first two, a response code; a row of a zero key
 * clears its table. A dial plan (class 145) is known by its number, the
 * first byte, and removed by a row whose action, the second, is 0. The
 * others keep whole rows, among them the tables of the IPv6 host (class 347),
 * which the ONU keeps of what its stack holds: the addresses with their
 * lifetimes, the routers, the DNS servers and the on-link prefixes.
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

	{ ME_MCAST_OPS_PROFILE, 7,
	  CTRL_ROWS(OMCI_ME_TABLE_CTRL_PARTS, 2u, OMCI_MCAST_ACL_ROWS) },
	{ ME_MCAST_OPS_PROFILE, 8,
	  CTRL_ROWS(OMCI_ME_TABLE_CTRL_PARTS, 2u, OMCI_MCAST_ACL_ROWS) },
	{ ME_MCAST_SUBSCRIBER, 6,
	  CTRL_ROWS(OMCI_ME_TABLE_CTRL_KEY, 0u, OMCI_MCAST_SUBSCRIBER_ROWS) },
	{ ME_MCAST_SUBSCRIBER, 7,
	  CTRL_ROWS(OMCI_ME_TABLE_CTRL_PARTS, 1u, OMCI_MCAST_SUBSCRIBER_ROWS) },

	{ ME_BRIDGE_PORT_FILTER,
	  1,
	  { .key_len = 1,
	    .order = OMCI_MIB_TABLE_LAST,
	    .max_entries = OMCI_ME_MAC_FILTER_ROWS,
	    .ctrl = OMCI_ME_TABLE_CTRL_ENTRY } },

	{ ME_VOICE_SERVICE_PROFILE, 10, INDEX_ROWS(1, 32) },
	{ ME_VOICE_SERVICE_PROFILE, 11, INDEX_ROWS(1, 40) },
	{ ME_VOICE_SERVICE_PROFILE, 12, INDEX_ROWS(1, 32) },
	{ ME_VOICE_SERVICE_PROFILE, 13, INDEX_ROWS(1, 40) },
	{ ME_DIAL_PLAN,
	  6,
	  { .key_len = 1,
	    .del_offset = 1,
	    .del_len = 1,
	    .del_value = 0x00u,
	    .order = OMCI_MIB_TABLE_LAST,
	    .max_entries = 32 } },
	{ ME_SIP_AGENT, 12, INDEX_ROWS(2, 32) },

	{ ME_IPV6_HOST, 9, WHOLE_ROW(24, 8) },
	{ ME_IPV6_HOST, 10, WHOLE_ROW(16, 4) },
	{ ME_IPV6_HOST, 11, WHOLE_ROW(16, 4) },
	{ ME_IPV6_HOST, 14, WHOLE_ROW(26, 8) },
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

/*
 * A table control field: in its first byte the operation (bits 7 and 6), the
 * row part (bits 5 to 3, in a table with parts) and the two high bits of the
 * row key (bits 1 and 0); the second byte holds the rest of the key. The bits
 * between are reserved, or the test bit of the extended format.
 */
enum {
	CTRL_OP_SHIFT = 6,
	CTRL_PART_SHIFT = 3,
	CTRL_PART_MASK = 0x7,
	CTRL_KEY_HIGH = 0x3,
};

void omci_me_table_op_set(uint8_t *row, enum omci_me_table_op op)
{
	row[0] = (uint8_t)((row[0] & ~(0x3u << CTRL_OP_SHIFT)) |
			   (unsigned int)op << CTRL_OP_SHIFT);
}

void omci_me_table_row_id(const struct omci_me_table_policy *policy,
			  const uint8_t *row, struct omci_me_table_row_id *out)
{
	out->op = (enum omci_me_table_op)(row[0] >> CTRL_OP_SHIFT);
	out->part = policy->ctrl == OMCI_ME_TABLE_CTRL_PARTS ?
			    (uint8_t)((row[0] >> CTRL_PART_SHIFT) &
				      CTRL_PART_MASK) :
			    0u;
	out->key = (uint16_t)((row[0] & CTRL_KEY_HIGH) << 8 | row[1]);
}

/*
 * The key a row part is stored under: its row part and row key, the other
 * bits clear, so that a Get Next reads the operation and the test bit as 0.
 */
static void stored_key(uint8_t part, uint16_t key,
		       uint8_t out[static OMCI_ME_TABLE_CTRL_LEN])
{
	out[0] = (uint8_t)(part << CTRL_PART_SHIFT | key >> 8);
	out[1] = (uint8_t)key;
}

static bool holds_key(const struct omci_mib *mib, uint16_t class_id,
		      uint16_t instance, uint8_t attr_no, const uint8_t *key,
		      size_t key_len)
{
	const size_t n = omci_mib_table_count(mib, class_id, instance, attr_no);

	for (size_t i = 0; i < n; i++) {
		uint8_t have[OMCI_MIB_TABLE_ENTRY_MAX] = { 0 };

		(void)!omci_mib_table_get(mib, class_id, instance, attr_no, i,
					  have, sizeof(have));
		if (omci_wire_equal(have, key, key_len))
			return true;
	}
	return false;
}

/* Remove every part of the row with @key; a part that is not there is no fault. */
static enum omci_status delete_row(struct omci_mib *mib, uint16_t class_id,
				   uint16_t instance, uint8_t attr_no,
				   const struct omci_me_table_policy *policy,
				   uint16_t key)
{
	for (uint8_t part = 0; part <= policy->max_part; part++) {
		uint8_t k[OMCI_ME_TABLE_CTRL_LEN];

		stored_key(part, key, k);
		const enum omci_status rc = omci_mib_table_remove(
			mib, class_id, instance, attr_no, k, sizeof(k));

		if (rc != OMCI_OK && rc != OMCI_DECLINED)
			return rc;
	}
	return OMCI_OK;
}

/* Remove every row, the first each time, as many times as there are rows. */
static enum omci_status clear_rows(struct omci_mib *mib, uint16_t class_id,
				   uint16_t instance, uint8_t attr_no)
{
	for (size_t n = omci_mib_table_count(mib, class_id, instance, attr_no);
	     n > 0; n--) {
		uint8_t first[OMCI_MIB_TABLE_ENTRY_MAX] = { 0 };

		(void)!omci_mib_table_get(mib, class_id, instance, attr_no, 0,
					  first, sizeof(first));
		(void)!omci_mib_table_remove(mib, class_id, instance, attr_no,
					     first, OMCI_ME_TABLE_CTRL_LEN);
	}
	return OMCI_OK;
}

/* The two high bits of the second byte of a row known by its entry number. */
enum {
	ENTRY_OP_SHIFT = 6,
	ENTRY_REMOVE = 0,
	ENTRY_ADD = 1,
	ENTRY_CLEAR = 2,
};

/*
 * A Set of a row known by its entry number (OMCI_ME_TABLE_CTRL_ENTRY): it is
 * added, or replaces the row of its number; removed, which is no fault when
 * the row is not there; or the whole table is cleared.
 */
static enum omci_status entry_set(struct omci_mib *mib, uint16_t class_id,
				  uint16_t instance, uint8_t attr_no,
				  const struct omci_me_table_policy *policy,
				  const uint8_t *row, size_t len)
{
	const unsigned int op = row[1] >> ENTRY_OP_SHIFT;

	if (op == ENTRY_CLEAR)
		return clear_rows(mib, class_id, instance, attr_no);
	if (op == ENTRY_REMOVE) {
		const enum omci_status rc = omci_mib_table_remove(
			mib, class_id, instance, attr_no, row, 1u);

		return rc == OMCI_DECLINED ? OMCI_OK : rc;
	}
	if (op != ENTRY_ADD)
		return OMCI_DECLINED;
	if (omci_mib_table_count(mib, class_id, instance, attr_no) >=
		    policy->max_entries &&
	    !holds_key(mib, class_id, instance, attr_no, row, 1u))
		return OMCI_ERR_NOMEM;
	return omci_mib_table_put(mib, class_id, instance, attr_no, row, len,
				  1u, policy->order);
}

/*
 * A Set of a row known by its leading key (OMCI_ME_TABLE_CTRL_INDEX): it is
 * added, or replaces the row of its key; a key of zeros clears the table.
 */
static enum omci_status index_set(struct omci_mib *mib, uint16_t class_id,
				  uint16_t instance, uint8_t attr_no,
				  const struct omci_me_table_policy *policy,
				  const uint8_t *row, size_t len)
{
	bool zero = true;

	for (uint8_t i = 0; i < policy->key_len; i++)
		zero = zero && row[i] == 0u;
	if (zero)
		return clear_rows(mib, class_id, instance, attr_no);
	if (omci_mib_table_count(mib, class_id, instance, attr_no) >=
		    policy->max_entries &&
	    !holds_key(mib, class_id, instance, attr_no, row, policy->key_len))
		return OMCI_ERR_NOMEM;
	return omci_mib_table_put(mib, class_id, instance, attr_no, row, len,
				  policy->key_len, policy->order);
}

enum omci_status
omci_me_table_ctrl_set(struct omci_mib *mib, uint16_t class_id,
		       uint16_t instance, uint8_t attr_no,
		       const struct omci_me_table_policy *policy,
		       const uint8_t *row, size_t len)
{
	uint8_t stored[OMCI_MIB_TABLE_ENTRY_MAX];
	struct omci_me_table_row_id id;

	if (mib == NULL || policy == NULL || row == NULL ||
	    len < OMCI_ME_TABLE_CTRL_LEN || len > sizeof(stored))
		return OMCI_ERR_INVAL;

	if (policy->ctrl == OMCI_ME_TABLE_CTRL_ENTRY)
		return entry_set(mib, class_id, instance, attr_no, policy, row,
				 len);
	if (policy->ctrl == OMCI_ME_TABLE_CTRL_INDEX)
		return index_set(mib, class_id, instance, attr_no, policy, row,
				 len);

	omci_me_table_row_id(policy, row, &id);
	if (id.op == OMCI_ME_TABLE_OP_DELETE)
		return delete_row(mib, class_id, instance, attr_no, policy,
				  id.key);
	if (id.op == OMCI_ME_TABLE_OP_CLEAR)
		return clear_rows(mib, class_id, instance, attr_no);
	if (id.op != OMCI_ME_TABLE_OP_WRITE || id.part > policy->max_part)
		return OMCI_DECLINED;

	omci_wire_copy(stored, row, len);
	stored_key(id.part, id.key, stored);

	if (policy->max_entries != 0 &&
	    omci_mib_table_count(mib, class_id, instance, attr_no) >=
		    policy->max_entries &&
	    !holds_key(mib, class_id, instance, attr_no, stored,
		       OMCI_ME_TABLE_CTRL_LEN))
		return OMCI_ERR_NOMEM;

	return omci_mib_table_put(mib, class_id, instance, attr_no, stored, len,
				  OMCI_ME_TABLE_CTRL_LEN, policy->order);
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

static const struct omci_me_table_source me_list_source = {
	.count = me_list_count,
	.entry = me_list_entry
};
static const struct omci_me_table_source msg_list_source = {
	.count = msg_list_count,
	.entry = msg_list_entry
};

/*
 * The tables made on demand: the two of the OMCI entity, the active group
 * lists of the multicast subscriber monitor, the lost groups list of the
 * multicast operations profile, and the bridge table of a bridge port.
 */
static const struct {
	uint16_t class_id;
	uint8_t attr_no;
	const struct omci_me_table_source *source;
} rendered[] = {
	{ ME_OMCI, ATTR_ME_TYPES, &me_list_source },
	{ ME_OMCI, ATTR_MSG_TYPES, &msg_list_source },
	{ ME_MCAST_OPS_PROFILE, 9, &omci_mcast_lost_groups },
	{ ME_MCAST_MONITOR, 5, &omci_mcast_ipv4_groups },
	{ ME_MCAST_MONITOR, 6, &omci_mcast_ipv6_groups },
	{ ME_BRIDGE_PORT_TABLE, 1, &omci_fdb_bridge_table },
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
