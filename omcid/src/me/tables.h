/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Table attributes: how a Set changes the rows of each, and which tables
 * are made on demand instead of being stored.
 */

#ifndef OMCI_ME_TABLES_H
#define OMCI_ME_TABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mib/store.h"
#include "omcid/attrs.h"

/**
 * enum omci_me_table_ctrl - What the first two bytes of a row say.
 * @OMCI_ME_TABLE_CTRL_NONE: nothing of their own: the policy's key and
 *	removal marker steer a Set
 * @OMCI_ME_TABLE_CTRL_KEY: they are a table control field (G.988 9.3.28):
 *	the two high bits write the row, delete it or clear the table, and the
 *	low ten bits are the row key
 * @OMCI_ME_TABLE_CTRL_PARTS: the same, with the row part in the three bits
 *	below the two high ones: a row is up to @max_part + 1 parts with one
 *	row key (G.988 9.3.27)
 * @OMCI_ME_TABLE_CTRL_ENTRY: the first byte is the row's entry number, its
 *	key, and the two high bits of the second add the row (01), remove it
 *	(00) or clear the table (10) (G.988 9.3.6)
 */
enum omci_me_table_ctrl {
	OMCI_ME_TABLE_CTRL_NONE = 0,
	OMCI_ME_TABLE_CTRL_KEY,
	OMCI_ME_TABLE_CTRL_PARTS,
	OMCI_ME_TABLE_CTRL_ENTRY,
};

/**
 * struct omci_me_table_policy - How a Set of a table attribute is applied.
 * @key_len: the leading bytes that identify a row; a row with a known key
 *	replaces the row that has it
 * @del_offset: where, in a row, the bytes that mark a removal start
 * @del_len: how many of them there are; 0 when the table has no removal
 * @del_value: the value all of them hold in a row that asks for a removal
 * @order: where a new row goes
 * @max_entries: the most rows the table may hold; 0 for no limit
 * @ctrl: whether each row starts with a table control field, which then
 *	is the key and steers the Set (omci_me_table_ctrl_set())
 * @max_part: the last row part the table takes, for
 *	%OMCI_ME_TABLE_CTRL_PARTS
 */
struct omci_me_table_policy {
	uint8_t key_len;

	uint8_t del_offset;
	uint8_t del_len;
	uint8_t del_value;

	enum omci_mib_table_order order;

	uint16_t max_entries;

	enum omci_me_table_ctrl ctrl;
	uint8_t max_part;
};

/* The size of a table control field, which is the key of its table. */
#define OMCI_ME_TABLE_CTRL_LEN 2u

/* The most addresses the MAC filter table of a bridge port holds. */
#define OMCI_ME_MAC_FILTER_ROWS 16u

/**
 * enum omci_me_table_op - What a row with a table control field asks for.
 * @OMCI_ME_TABLE_OP_RESERVED: nothing: the value is reserved
 * @OMCI_ME_TABLE_OP_WRITE: write the row part, in place of the one with the
 *	same row key and part
 * @OMCI_ME_TABLE_OP_DELETE: delete the row, every part of it
 * @OMCI_ME_TABLE_OP_CLEAR: delete every row
 */
enum omci_me_table_op {
	OMCI_ME_TABLE_OP_RESERVED = 0,
	OMCI_ME_TABLE_OP_WRITE,
	OMCI_ME_TABLE_OP_DELETE,
	OMCI_ME_TABLE_OP_CLEAR,
};

/**
 * struct omci_me_table_row_id - A table control field, taken apart.
 * @op: what the row asks for
 * @part: its row part; 0 in a table without parts
 * @key: its row key, 0 to 1023
 */
struct omci_me_table_row_id {
	enum omci_me_table_op op;
	uint8_t part;
	uint16_t key;
};

/**
 * omci_me_table_op_set() - Change what a row with a table control field asks for.
 * @row: the row
 * @op: the operation
 */
void omci_me_table_op_set(uint8_t *row, enum omci_me_table_op op);

/**
 * omci_me_table_row_id() - Take a table control field apart.
 * @policy: the table's policy, of a table with the field
 * @row: the row
 * @out: the field's parts
 */
void omci_me_table_row_id(const struct omci_me_table_policy *policy,
			  const uint8_t *row, struct omci_me_table_row_id *out);

/**
 * omci_me_table_ctrl_set() - Apply a Set of a row that has a table control
 *	field.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute
 * @policy: the table's policy
 * @row: the row, as the Set carries it
 * @len: its size, the attribute's
 *
 * A write stores the row, with every bit of the field but the row part and
 * the row key cleared, in place of the part with the same row key and part:
 * a Get Next reads the operation as 0, and the test bit of the extended
 * format (G.988 9.3.27) too, which tells the OLT that the ONU has the
 * format. A delete removes every part with the row key; a clear empties the
 * table.
 *
 * Return: %OMCI_OK; %OMCI_DECLINED for the reserved operation or a row part
 * the table does not take; %OMCI_ERR_NOMEM when a new part finds the table
 * or the MIB full; an error for what is not there.
 */
__must_check enum omci_status
omci_me_table_ctrl_set(struct omci_mib *mib, uint16_t class_id,
		       uint16_t instance, uint8_t attr_no,
		       const struct omci_me_table_policy *policy,
		       const uint8_t *row, size_t len);

/**
 * struct omci_me_table_source - A table attribute made on demand.
 * @count: the number of rows for an instance
 * @entry: one row; false when there is none at @index
 */
struct omci_me_table_source {
	size_t (*count)(uint16_t instance);

	bool (*entry)(uint16_t instance, size_t index, void *out, size_t cap);
};

/**
 * omci_me_table_rendered() - Whether a table attribute is made on demand.
 * @class_id: the class
 * @attr_no: the attribute
 * @out: where its rows come from
 *
 * Return: true for such a table; its rows are not in the MIB.
 */
__must_check bool
omci_me_table_rendered(uint16_t class_id, uint8_t attr_no,
		       const struct omci_me_table_source **out);

/**
 * omci_me_table_policy() - The policy of a table attribute.
 * @class_id: the class
 * @attr_no: the attribute
 * @out: the policy
 *
 * Return: false for an attribute without one: a Set then stores the value
 * as it would for any attribute.
 */
__must_check bool omci_me_table_policy(uint16_t class_id, uint8_t attr_no,
				       struct omci_me_table_policy *out);

/**
 * omci_me_table_is_delete() - Whether a row asks for a removal.
 * @policy: the table's policy
 * @row: the row
 * @len: its size
 *
 * Return: true when the marking bytes all hold the removal value.
 */
__must_check bool
omci_me_table_is_delete(const struct omci_me_table_policy *policy,
			const void *row, size_t len);

#endif
