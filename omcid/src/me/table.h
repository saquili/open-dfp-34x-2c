/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */
/*
 * The managed entity classes the daemon serves.
 *
 * Made by tools/gen_me_table.py from data/g988-me-min.tsv: change the
 * table, not this file, then run python3 tools/gen_me_table.py --write.
 *
 * The table follows ITU-T G.988 (11/2022). sha256 of the table:
 * c8be1de266eeea602177155a42576ec18a8024b3c941f5ddff1da636d9394397
 * 63 classes, 570 attributes.
 *
 * An attribute is known by its number alone: attribute n of a class is
 * element n - 1 of its array, and bit n of an attribute mask, counted from
 * the most significant bit. The instance identifier comes before attribute
 * 1 and is not an attribute.
 */

#ifndef OMCI_ME_TABLE_H
#define OMCI_ME_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * enum omci_me_creator - Who makes the instances of a class.
 * @OMCI_ME_CREATED_BY_ONU: the ONU, on its own; a Create is refused
 * @OMCI_ME_CREATED_BY_OLT: the OLT, with Create and Delete
 */
enum omci_me_creator {
	OMCI_ME_CREATED_BY_ONU = 0,
	OMCI_ME_CREATED_BY_OLT = 1,
};

/**
 * enum omci_me_access - How the OLT may use an attribute.
 * @OMCI_ME_ACC_READ: read it with Get
 * @OMCI_ME_ACC_WRITE: write it with Set
 * @OMCI_ME_ACC_SET_BY_CREATE: give it in the Create
 */
enum omci_me_access {
	OMCI_ME_ACC_READ = 1u << 0,
	OMCI_ME_ACC_WRITE = 1u << 1,
	OMCI_ME_ACC_SET_BY_CREATE = 1u << 2,
};

/**
 * enum omci_me_attr_flag - What else is true of an attribute.
 * @OMCI_ME_ATTR_MANDATORY: the standard requires it
 * @OMCI_ME_ATTR_TABLE: it is a table: a Get gives its size and Get next
 *	its rows
 * @OMCI_ME_ATTR_AVC: a change the ONU makes to it is announced
 */
enum omci_me_attr_flag {
	OMCI_ME_ATTR_MANDATORY = 1u << 0,
	OMCI_ME_ATTR_TABLE = 1u << 1,
	OMCI_ME_ATTR_AVC = 1u << 2,
};

/**
 * struct omci_me_attr - An attribute of a class.
 * @name: its name in the standard
 * @size: its size in bytes; for a table, the size of a row
 * @access: an &enum omci_me_access mask
 * @flags: an &enum omci_me_attr_flag mask
 */
struct omci_me_attr {
	const char *name;
	uint16_t size;
	uint8_t access;
	uint8_t flags;
};

/**
 * struct omci_me_class - A managed entity class.
 * @actions: bit N set when the class accepts message type N
 * @alarms: bit N set when the class defines alarm N
 * @class_id: its identifier
 * @creator: an &enum omci_me_creator
 * @attr_count: its attributes, 1 to 16
 * @name: its name in the standard
 * @attrs: its attributes, attribute n at @attrs[n - 1]
 */
struct omci_me_class {
	uint32_t actions;
	uint32_t alarms;
	uint16_t class_id;
	uint8_t creator;
	uint8_t attr_count;
	const char *name;
	const struct omci_me_attr *attrs;
};

/* The classes, in ascending order of identifier, and their number. */
extern const struct omci_me_class omci_me_classes[];
extern const size_t omci_me_class_count;

/**
 * omci_me_class_by_id() - Find a class.
 * @class_id: its identifier
 *
 * Return: the class, or NULL when the daemon does not serve it.
 */
const struct omci_me_class *omci_me_class_by_id(uint16_t class_id);

/**
 * omci_me_attr_get() - Find an attribute of a class.
 * @cls: the class, or NULL
 * @attr_no: the attribute, from 1
 *
 * Return: the attribute, or NULL when the class has none with that number.
 */
const struct omci_me_attr *omci_me_attr_get(const struct omci_me_class *cls,
					    uint8_t attr_no);

/**
 * omci_me_attr_is_table() - Whether an attribute is a table.
 * @a: the attribute, or NULL
 *
 * Return: true for a table attribute.
 */
static inline bool omci_me_attr_is_table(const struct omci_me_attr *a)
{
	return a != NULL && (a->flags & OMCI_ME_ATTR_TABLE) != 0u;
}

#endif /* OMCI_ME_TABLE_H */
