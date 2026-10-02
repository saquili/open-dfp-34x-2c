/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The management information base: the instances of the managed entities
 * and the values of their attributes, held in fixed memory.
 */

#ifndef OMCI_MIB_STORE_H
#define OMCI_MIB_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omcid/attrs.h"
#include "omcid/status.h"

struct omci_mib;

/**
 * struct omci_mib_notify_ops - Who hears of attribute changes.
 * @attr_changed: called when the ONU itself changes an attribute that the
 *	class says is announced; a change made by the OLT is not reported
 */
struct omci_mib_notify_ops {
	void (*attr_changed)(void *ctx, uint16_t class_id, uint16_t instance,
			     uint8_t attr_no, const void *value, size_t len);
};

/**
 * omci_mib_notify_set() - Set who hears of attribute changes.
 * @mib: the MIB
 * @ops: the listener, or NULL
 * @ctx: passed to @ops
 */
void omci_mib_notify_set(struct omci_mib *mib,
			 const struct omci_mib_notify_ops *ops, void *ctx);

/**
 * omci_mib_notify_suspend() - Hold back change reports, or let them through.
 * @mib: the MIB
 * @suspended: true to hold them back
 */
void omci_mib_notify_suspend(struct omci_mib *mib, bool suspended);

/**
 * omci_mib_notify_mute() - Silence change reports while the MIB is rebuilt.
 * @mib: the MIB
 * @muted: true to silence them
 *
 * Independent of omci_mib_notify_suspend(): a report goes out only when
 * neither holds it back.
 */
void omci_mib_notify_mute(struct omci_mib *mib, bool muted);

/**
 * omci_mib_announce_all() - Report every announced attribute of an instance.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 *
 * Return: %OMCI_OK, or the error of a class or instance that is not there.
 */
enum omci_status omci_mib_announce_all(struct omci_mib *mib, uint16_t class_id,
				       uint16_t instance);

/* The most instances, and the bytes their attributes can take together. */
#define OMCI_MIB_MAX_ROWS 512u
#define OMCI_MIB_ARENA_BYTES 24576u

/*
 * Table attributes: the largest row, the rows of all tables together, and
 * the most table attributes that can hold rows at once.
 */
#define OMCI_MIB_TABLE_ENTRY_MAX 64u
#define OMCI_MIB_TABLE_ENTRIES 256u
#define OMCI_MIB_MAX_TABLES 64u

/**
 * enum omci_mib_table_order - Where a new row of a table goes.
 * @OMCI_MIB_TABLE_FIRST: before the others
 * @OMCI_MIB_TABLE_LAST: after the others
 */
enum omci_mib_table_order {
	OMCI_MIB_TABLE_FIRST = 0,
	OMCI_MIB_TABLE_LAST,
};

/**
 * struct omci_mib_cursor - A place in a walk over every instance.
 * @class_id: the class of the instance
 * @instance: the instance
 * @slot: where the walk goes on from
 */
struct omci_mib_cursor {
	uint16_t class_id;
	uint16_t instance;
	size_t slot;
};

/**
 * omci_mib_init() - Set up an empty MIB; one per process.
 * @out: the MIB
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or %OMCI_ERR_BUSY while one exists.
 */
__must_check enum omci_status omci_mib_init(struct omci_mib **out);

/**
 * omci_mib_fini() - Empty the MIB and release it.
 * @mib: the MIB, or NULL
 */
void omci_mib_fini(struct omci_mib *mib);

/**
 * omci_mib_reset() - Remove every instance and every table row.
 * @mib: the MIB
 *
 * Return: %OMCI_OK, or %OMCI_ERR_INVAL without @mib.
 */
enum omci_status omci_mib_reset(struct omci_mib *mib);

/**
 * omci_mib_create() - Add an instance, every attribute zero.
 * @mib: the MIB
 * @class_id: its class
 * @instance: its instance
 *
 * Return: %OMCI_OK; %OMCI_ERR_ME_UNKNOWN for a class the device lacks;
 * %OMCI_ERR_EXISTS; %OMCI_ERR_NOMEM when the MIB is full.
 */
__must_check enum omci_status
omci_mib_create(struct omci_mib *mib, uint16_t class_id, uint16_t instance);

/**
 * omci_mib_delete() - Remove an instance and the rows of its tables.
 * @mib: the MIB
 * @class_id: its class
 * @instance: its instance
 *
 * Return: %OMCI_OK, or %OMCI_ERR_INST_UNKNOWN.
 */
__must_check enum omci_status
omci_mib_delete(struct omci_mib *mib, uint16_t class_id, uint16_t instance);

/**
 * omci_mib_exists() - Whether an instance exists.
 * @mib: the MIB
 * @class_id: its class
 * @instance: its instance
 *
 * Return: true when it does.
 */
__must_check bool omci_mib_exists(const struct omci_mib *mib, uint16_t class_id,
				  uint16_t instance);

/**
 * omci_mib_count() - The number of instances.
 * @mib: the MIB
 *
 * Return: the number.
 */
__must_check size_t omci_mib_count(const struct omci_mib *mib);

/**
 * omci_mib_attr_get() - Read an attribute.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute, from 1
 * @out: its value
 * @cap: room in @out
 * @out_len: the size of the value; may be NULL
 *
 * Return: %OMCI_OK; %OMCI_ERR_TRUNCATED when @cap is too small; the error
 * of a class, instance or attribute that is not there.
 */
__must_check enum omci_status omci_mib_attr_get(const struct omci_mib *mib,
						uint16_t class_id,
						uint16_t instance,
						uint8_t attr_no, void *out,
						size_t cap, size_t *out_len);

/**
 * omci_mib_attr_set() - Change an attribute, as the ONU itself does.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute
 * @value: the new value
 * @len: its size, which must be the attribute's
 *
 * A change of an attribute the class announces is reported to the
 * listener of omci_mib_notify_set().
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL for a wrong size, or the error of a
 * class, instance or attribute that is not there.
 */
__must_check enum omci_status
omci_mib_attr_set(struct omci_mib *mib, uint16_t class_id, uint16_t instance,
		  uint8_t attr_no, const void *value, size_t len);

/**
 * omci_mib_attr_set_by_olt() - Change an attribute at the OLT's request.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute
 * @value: the new value
 * @len: its size
 *
 * As omci_mib_attr_set(), but never reported: the OLT made the change.
 *
 * Return: as omci_mib_attr_set().
 */
__must_check enum omci_status
omci_mib_attr_set_by_olt(struct omci_mib *mib, uint16_t class_id,
			 uint16_t instance, uint8_t attr_no, const void *value,
			 size_t len);

/**
 * omci_mib_table_count() - The number of rows of a table attribute.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute
 *
 * Return: the number, 0 for a table with no rows.
 */
__must_check size_t omci_mib_table_count(const struct omci_mib *mib,
					 uint16_t class_id, uint16_t instance,
					 uint8_t attr_no);

/**
 * omci_mib_table_generation() - A number that changes with every table row change.
 * @mib: the MIB
 *
 * Return: the number.
 */
__must_check uint32_t omci_mib_table_generation(const struct omci_mib *mib);

/**
 * omci_mib_generation() - A number that changes with every change of the MIB.
 * @mib: the MIB
 *
 * Return: the number.
 */
__must_check uint32_t omci_mib_generation(const struct omci_mib *mib);

/**
 * omci_mib_table_capacity() - The table rows still free.
 * @mib: the MIB
 *
 * Return: the number.
 */
__must_check size_t omci_mib_table_capacity(const struct omci_mib *mib);

/**
 * omci_mib_table_get() - Read one row of a table attribute.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute
 * @index: the row, from 0
 * @out: the row
 * @cap: room in @out
 *
 * Return: %OMCI_OK; %OMCI_DECLINED past the last row; an error for a
 * class, instance or attribute that is not a table that exists.
 */
__must_check enum omci_status omci_mib_table_get(const struct omci_mib *mib,
						 uint16_t class_id,
						 uint16_t instance,
						 uint8_t attr_no, size_t index,
						 void *out, size_t cap);

/**
 * omci_mib_table_put() - Add a row to a table attribute, or replace one.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute
 * @entry: the row
 * @len: its size, which must be the attribute's
 * @key_len: how many leading bytes identify a row; a row with the same key
 *	is replaced in place, and 0 means that rows are never replaced
 * @where: where a new row goes
 *
 * Return: %OMCI_OK; %OMCI_ERR_NOMEM when the rows or the tables run out;
 * %OMCI_ERR_INVAL; or an error for what is not there.
 */
__must_check enum omci_status
omci_mib_table_put(struct omci_mib *mib, uint16_t class_id, uint16_t instance,
		   uint8_t attr_no, const void *entry, size_t len,
		   size_t key_len, enum omci_mib_table_order where);

/**
 * omci_mib_table_remove() - Remove the row with a key from a table attribute.
 * @mib: the MIB
 * @class_id: the class
 * @instance: the instance
 * @attr_no: the attribute
 * @key: the key
 * @key_len: its size
 *
 * Return: %OMCI_OK; %OMCI_DECLINED when no row has the key; an error for
 * a bad argument or what is not there.
 */
__must_check enum omci_status
omci_mib_table_remove(struct omci_mib *mib, uint16_t class_id,
		      uint16_t instance, uint8_t attr_no, const void *key,
		      size_t key_len);

/**
 * omci_mib_walk_first() - Start a walk over every instance.
 * @mib: the MIB
 * @cursor: the first instance
 *
 * The walk follows the order in which the instances were stored.
 *
 * Return: %OMCI_OK, or %OMCI_DECLINED when the MIB is empty.
 */
__must_check enum omci_status
omci_mib_walk_first(const struct omci_mib *mib, struct omci_mib_cursor *cursor);

/**
 * omci_mib_walk_next() - Go on to the next instance.
 * @mib: the MIB
 * @cursor: where the walk is; the next instance on return
 *
 * Return: %OMCI_OK, or %OMCI_DECLINED after the last one.
 */
__must_check enum omci_status
omci_mib_walk_next(const struct omci_mib *mib, struct omci_mib_cursor *cursor);

#endif
