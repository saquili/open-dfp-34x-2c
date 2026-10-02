/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Table attributes: how a Set changes the rows of each, and which tables
 * are made on demand instead of being stored.
 */

#ifndef OMCI_ME_TABLES_H
#define OMCI_ME_TABLES_H

#include <stdbool.h>
#include <stdint.h>

#include "mib/store.h"
#include "omcid/attrs.h"

/**
 * struct omci_me_table_policy - How a Set of a table attribute is applied.
 * @key_len: the leading bytes that identify a row; a row with a known key
 *	replaces the row that has it
 * @del_offset: where, in a row, the bytes that mark a removal start
 * @del_len: how many of them there are; 0 when the table has no removal
 * @del_value: the value all of them hold in a row that asks for a removal
 * @order: where a new row goes
 * @max_entries: the most rows the table may hold; 0 for no limit
 */
struct omci_me_table_policy {
	uint8_t key_len;

	uint8_t del_offset;
	uint8_t del_len;
	uint8_t del_value;

	enum omci_mib_table_order order;

	uint16_t max_entries;
};

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
