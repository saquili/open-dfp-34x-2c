// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The MIB in fixed memory. Each instance is a row whose attribute values
 * lie end to end in one arena; a deleted row stays reserved for its class
 * and is reused by the next instance of that class. The rows of table
 * attributes come from a shared pool, linked per table.
 */

#include <stdbool.h>

#include "me/layout.h"
#include "me/table.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omcid/byteorder.h"
#include "sys/log.h"
#include "sys/wire.h"

/* An instance: its attributes, in the arena, laid out by me/layout.c. */
struct row {
	uint16_t class_id;
	uint16_t instance;
	uint16_t size;
	bool used;
	uint8_t *data;
	size_t next_free;
};

#define NO_SLOT SIZE_MAX

/* A row of a table attribute, and the next row of the same table. */
struct table_entry {
	uint8_t bytes[OMCI_MIB_TABLE_ENTRY_MAX];
	size_t next;
};

/* A table attribute that holds rows. */
struct table {
	uint16_t class_id;
	uint16_t instance;
	uint8_t attr_no;
	uint8_t entry_size;
	bool used;
	size_t head;
	size_t count;
};

static void table_drain(struct omci_mib *mib, struct table *t);

struct omci_mib {
	struct row rows[OMCI_MIB_MAX_ROWS];
	size_t row_count;
	size_t live;
	size_t free_head;

	uint8_t arena[OMCI_MIB_ARENA_BYTES];
	size_t arena_used;

	struct table tables[OMCI_MIB_MAX_TABLES];
	struct table_entry entries[OMCI_MIB_TABLE_ENTRIES];
	size_t entry_free;
	size_t entries_used;
	/* changes with every table row change */
	uint32_t table_gen;

	/* changes with every change of the MIB */
	uint32_t gen;

	const struct omci_mib_notify_ops *notify;
	void *notify_ctx;
	bool notify_suspended;
	bool notify_muted;
};

static struct omci_mib g_mib;
static bool g_mib_taken;

void omci_mib_notify_set(struct omci_mib *mib,
			 const struct omci_mib_notify_ops *ops, void *ctx)
{
	if (mib == NULL)
		return;
	mib->notify = ops;
	mib->notify_ctx = ctx;
}

void omci_mib_notify_suspend(struct omci_mib *mib, bool suspended)
{
	if (mib != NULL)
		mib->notify_suspended = suspended;
}

void omci_mib_notify_mute(struct omci_mib *mib, bool muted)
{
	if (mib != NULL)
		mib->notify_muted = muted;
}

static void announce(struct omci_mib *mib, uint16_t class_id, uint16_t instance,
		     uint8_t wire_attr, const void *value, size_t len)
{
	if (mib->notify == NULL || mib->notify->attr_changed == NULL ||
	    mib->notify_suspended || mib->notify_muted)
		return;
	mib->notify->attr_changed(mib->notify_ctx, class_id, instance,
				  wire_attr, value, len);
}

/* The row of an instance, or NO_SLOT. */
static size_t find_slot(const struct omci_mib *mib, uint16_t class_id,
			uint16_t instance)
{
	for (size_t i = 0; i < mib->row_count; i++)
		if (mib->rows[i].used && mib->rows[i].class_id == class_id &&
		    mib->rows[i].instance == instance)
			return i;
	return NO_SLOT;
}

enum omci_status omci_mib_init(struct omci_mib **out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	if (g_mib_taken)
		return OMCI_ERR_BUSY;

	g_mib_taken = true;
	*out = &g_mib;
	return omci_mib_reset(&g_mib);
}

void omci_mib_fini(struct omci_mib *mib)
{
	if (mib == NULL)
		return;
	(void)omci_mib_reset(mib);
	g_mib_taken = false;
}

enum omci_status omci_mib_reset(struct omci_mib *mib)
{
	if (mib == NULL)
		return OMCI_ERR_INVAL;

	mib->row_count = 0;
	mib->live = 0;
	mib->free_head = NO_SLOT;
	mib->arena_used = 0;

	for (size_t i = 0; i < OMCI_MIB_MAX_TABLES; i++)
		mib->tables[i].used = false;
	mib->entry_free = NO_SLOT;
	mib->entries_used = 0;
	mib->table_gen++;
	mib->gen++;

	OMCI_DEBUG("mib reset");
	return OMCI_OK;
}

enum omci_status omci_mib_create(struct omci_mib *mib, uint16_t class_id,
				 uint16_t instance)
{
	if (mib == NULL)
		return OMCI_ERR_INVAL;

	const struct omci_me_class *cls = omci_me_class_by_id(class_id);

	if (cls == NULL) {
		OMCI_DEBUG("create: no class %u", class_id);
		return OMCI_ERR_ME_UNKNOWN;
	}
	if (find_slot(mib, class_id, instance) != NO_SLOT)
		return OMCI_ERR_EXISTS;

	const size_t size = omci_me_row_size(cls);

	size_t prev = NO_SLOT;

	/* Reuse a free row of the same class: it has the right size. */
	for (size_t i = mib->free_head; i != NO_SLOT;
	     prev = i, i = mib->rows[i].next_free) {
		if (mib->rows[i].class_id != class_id)
			continue;
		if (prev == NO_SLOT)
			mib->free_head = mib->rows[i].next_free;
		else
			mib->rows[prev].next_free = mib->rows[i].next_free;

		mib->rows[i].instance = instance;
		mib->rows[i].used = true;
		mib->rows[i].next_free = NO_SLOT;
		omci_wire_fill(mib->rows[i].data, 0, size);
		mib->live++;
		mib->gen++;
		return OMCI_OK;
	}

	if (mib->row_count >= OMCI_MIB_MAX_ROWS) {
		OMCI_ERR("mib full: %u rows — raise OMCI_MIB_MAX_ROWS",
			 (unsigned int)OMCI_MIB_MAX_ROWS);
		return OMCI_ERR_NOMEM;
	}
	if (mib->arena_used + size > OMCI_MIB_ARENA_BYTES) {
		OMCI_ERR(
			"mib arena full: %zu/%u bytes, class %u wants %zu — raise OMCI_MIB_ARENA_BYTES",
			mib->arena_used, (unsigned int)OMCI_MIB_ARENA_BYTES,
			class_id, size);
		return OMCI_ERR_NOMEM;
	}

	struct row *r = &mib->rows[mib->row_count];

	r->class_id = class_id;
	r->instance = instance;
	r->size = (uint16_t)size;
	r->used = true;
	r->data = &mib->arena[mib->arena_used];
	r->next_free = NO_SLOT;

	omci_wire_fill(r->data, 0, size);
	mib->arena_used += size;
	mib->row_count++;
	mib->live++;
	mib->gen++;

	OMCI_TRACE("create %u/%u (%zu B)", class_id, instance, size);
	return OMCI_OK;
}

enum omci_status omci_mib_delete(struct omci_mib *mib, uint16_t class_id,
				 uint16_t instance)
{
	if (mib == NULL)
		return OMCI_ERR_INVAL;

	const size_t slot = find_slot(mib, class_id, instance);

	if (slot == NO_SLOT)
		return OMCI_ERR_INST_UNKNOWN;

	for (size_t i = 0; i < OMCI_MIB_MAX_TABLES; i++)
		if (mib->tables[i].used &&
		    mib->tables[i].class_id == class_id &&
		    mib->tables[i].instance == instance)
			table_drain(mib, &mib->tables[i]);

	mib->rows[slot].used = false;
	mib->rows[slot].next_free = mib->free_head;
	mib->free_head = slot;
	mib->live--;
	mib->gen++;

	OMCI_TRACE("delete %u/%u", class_id, instance);
	return OMCI_OK;
}

bool omci_mib_exists(const struct omci_mib *mib, uint16_t class_id,
		     uint16_t instance)
{
	return mib != NULL && find_slot(mib, class_id, instance) != NO_SLOT;
}

size_t omci_mib_count(const struct omci_mib *mib)
{
	return mib != NULL ? mib->live : 0;
}

/* Where an attribute's value lies, and its size. */
static enum omci_status locate(const struct omci_mib *mib, uint16_t class_id,
			       uint16_t instance, uint8_t attr_no,
			       uint8_t **out_bytes, size_t *out_size)
{
	const struct omci_me_class *cls = omci_me_class_by_id(class_id);

	if (cls == NULL)
		return OMCI_ERR_ME_UNKNOWN;

	const size_t slot = find_slot(mib, class_id, instance);

	if (slot == NO_SLOT)
		return OMCI_ERR_INST_UNKNOWN;

	const struct omci_me_attr *a = omci_me_attr_get(cls, attr_no);

	if (a == NULL)
		return OMCI_ERR_ATTR_UNKNOWN;

	const size_t off = omci_me_attr_offset(cls, attr_no);

	if (off == OMCI_ME_NO_OFFSET || off + a->size > mib->rows[slot].size)
		return OMCI_ERR_ATTR_UNKNOWN;

	*out_bytes = mib->rows[slot].data + off;
	*out_size = a->size;
	return OMCI_OK;
}

enum omci_status omci_mib_attr_get(const struct omci_mib *mib,
				   uint16_t class_id, uint16_t instance,
				   uint8_t attr_no, void *out, size_t cap,
				   size_t *out_len)
{
	if (mib == NULL || out == NULL)
		return OMCI_ERR_INVAL;

	uint8_t *bytes;
	size_t size;
	enum omci_status rc =
		locate(mib, class_id, instance, attr_no, &bytes, &size);
	if (rc != OMCI_OK)
		return rc;

	if (cap < size)
		return OMCI_ERR_TRUNCATED;

	omci_wire_copy(out, bytes, size);
	if (out_len != NULL)
		*out_len = size;
	return OMCI_OK;
}

/* Store a value; report it when it changed, @announce_a_change allows it and the class announces the attribute. */
static enum omci_status attr_set(struct omci_mib *mib, uint16_t class_id,
				 uint16_t instance, uint8_t attr_no,
				 const void *value, size_t len,
				 bool announce_a_change)
{
	if (mib == NULL || value == NULL)
		return OMCI_ERR_INVAL;

	uint8_t *bytes;
	size_t size;
	enum omci_status rc =
		locate(mib, class_id, instance, attr_no, &bytes, &size);
	if (rc != OMCI_OK)
		return rc;

	if (len != size)
		return OMCI_ERR_INVAL;

	bool changed = false;
	const uint8_t *src = value;

	for (size_t i = 0; i < size && !changed; i++)
		changed = bytes[i] != src[i];

	omci_wire_copy(bytes, value, size);
	mib->gen++;

	if (changed && announce_a_change) {
		const struct omci_me_class *cls = omci_me_class_by_id(class_id);
		const struct omci_me_attr *a = omci_me_attr_get(cls, attr_no);

		if (a != NULL && (a->flags & OMCI_ME_ATTR_AVC) != 0)
			announce(mib, class_id, instance, attr_no, bytes, size);
	}
	return OMCI_OK;
}

enum omci_status omci_mib_attr_set(struct omci_mib *mib, uint16_t class_id,
				   uint16_t instance, uint8_t attr_no,
				   const void *value, size_t len)
{
	return attr_set(mib, class_id, instance, attr_no, value, len, true);
}

enum omci_status omci_mib_attr_reading(struct omci_mib *mib, uint16_t class_id,
				       uint16_t instance, uint8_t attr_no,
				       const void *value, size_t len)
{
	if (mib == NULL || value == NULL)
		return OMCI_ERR_INVAL;

	uint8_t *bytes;
	size_t size;
	const enum omci_status rc =
		locate(mib, class_id, instance, attr_no, &bytes, &size);

	if (rc != OMCI_OK)
		return rc;
	if (len != size)
		return OMCI_ERR_INVAL;

	omci_wire_copy(bytes, value, size);
	return OMCI_OK;
}

enum omci_status omci_mib_attr_set_by_olt(struct omci_mib *mib,
					  uint16_t class_id, uint16_t instance,
					  uint8_t attr_no, const void *value,
					  size_t len)
{
	return attr_set(mib, class_id, instance, attr_no, value, len, false);
}

enum omci_status omci_mib_announce_all(struct omci_mib *mib, uint16_t class_id,
				       uint16_t instance)
{
	if (mib == NULL)
		return OMCI_ERR_INVAL;

	const struct omci_me_class *cls = omci_me_class_by_id(class_id);

	if (cls == NULL)
		return OMCI_ERR_ME_UNKNOWN;
	const size_t slot = find_slot(mib, class_id, instance);

	if (slot == NO_SLOT)
		return OMCI_ERR_INST_UNKNOWN;

	for (uint8_t n = 1; n <= cls->attr_count; n++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, n);

		if (a == NULL || (a->flags & OMCI_ME_ATTR_AVC) == 0)
			continue;

		/* The notification of a table names it and holds no rows. */
		if (omci_me_attr_is_table(a))
			announce(mib, class_id, instance, n, NULL, 0);
		else
			announce(mib, class_id, instance, n,
				 mib->rows[slot].data +
					 omci_me_attr_offset(cls, n),
				 a->size);
	}
	return OMCI_OK;
}

enum omci_status omci_mib_table_changed(struct omci_mib *mib, uint16_t class_id,
					uint16_t instance, uint8_t attr_no)
{
	if (mib == NULL)
		return OMCI_ERR_INVAL;

	const struct omci_me_class *cls = omci_me_class_by_id(class_id);

	if (cls == NULL)
		return OMCI_ERR_ME_UNKNOWN;
	if (find_slot(mib, class_id, instance) == NO_SLOT)
		return OMCI_ERR_INST_UNKNOWN;

	const struct omci_me_attr *a = omci_me_attr_get(cls, attr_no);

	if (a == NULL)
		return OMCI_ERR_ATTR_UNKNOWN;
	if (!omci_me_attr_is_table(a))
		return OMCI_ERR_INVAL;

	if ((a->flags & OMCI_ME_ATTR_AVC) != 0)
		announce(mib, class_id, instance, attr_no, NULL, 0);
	return OMCI_OK;
}

static enum omci_status walk_from(const struct omci_mib *mib, size_t from,
				  struct omci_mib_cursor *cursor)
{
	for (size_t i = from; i < mib->row_count; i++) {
		if (!mib->rows[i].used)
			continue;
		cursor->class_id = mib->rows[i].class_id;
		cursor->instance = mib->rows[i].instance;
		cursor->slot = i;
		return OMCI_OK;
	}
	return OMCI_DECLINED;
}

enum omci_status omci_mib_walk_first(const struct omci_mib *mib,
				     struct omci_mib_cursor *cursor)
{
	if (mib == NULL || cursor == NULL)
		return OMCI_ERR_INVAL;
	return walk_from(mib, 0, cursor);
}

enum omci_status omci_mib_walk_next(const struct omci_mib *mib,
				    struct omci_mib_cursor *cursor)
{
	if (mib == NULL || cursor == NULL)
		return OMCI_ERR_INVAL;
	return walk_from(mib, cursor->slot + 1, cursor);
}

static struct table *table_find(const struct omci_mib *mib, uint16_t class_id,
				uint16_t instance, uint8_t attr_no)
{
	for (size_t i = 0; i < OMCI_MIB_MAX_TABLES; i++) {
		const struct table *t = &mib->tables[i];

		if (t->used && t->class_id == class_id &&
		    t->instance == instance && t->attr_no == attr_no)
			return (struct table *)(uintptr_t)t;
	}
	return NULL;
}

static size_t entry_alloc(struct omci_mib *mib)
{
	if (mib->entry_free != NO_SLOT) {
		const size_t n = mib->entry_free;

		mib->entry_free = mib->entries[n].next;
		mib->entries[n].next = NO_SLOT;
		return n;
	}
	if (mib->entries_used < OMCI_MIB_TABLE_ENTRIES) {
		const size_t n = mib->entries_used++;

		mib->entries[n].next = NO_SLOT;
		return n;
	}
	return NO_SLOT;
}

static void entry_free(struct omci_mib *mib, size_t n)
{
	mib->entries[n].next = mib->entry_free;
	mib->entry_free = n;
}

static void table_drain(struct omci_mib *mib, struct table *t)
{
	size_t n = t->head;

	while (n != NO_SLOT) {
		const size_t next = mib->entries[n].next;

		entry_free(mib, n);
		n = next;
	}
	t->head = NO_SLOT;
	t->count = 0;
	t->used = false;
	mib->table_gen++;
}

uint32_t omci_mib_generation(const struct omci_mib *mib)
{
	return mib != NULL ? mib->gen : 0u;
}

uint32_t omci_mib_table_generation(const struct omci_mib *mib)
{
	return mib != NULL ? mib->table_gen : 0;
}

/* The row size of a table attribute, or 0 when the attribute is not a table this MIB can hold. */
static uint8_t table_entry_size(uint16_t class_id, uint8_t attr_no)
{
	const struct omci_me_class *cls = omci_me_class_by_id(class_id);
	const struct omci_me_attr *a = omci_me_attr_get(cls, attr_no);

	if (!omci_me_attr_is_table(a) || a->size == 0 ||
	    a->size > OMCI_MIB_TABLE_ENTRY_MAX)
		return 0;
	return (uint8_t)a->size;
}

size_t omci_mib_table_count(const struct omci_mib *mib, uint16_t class_id,
			    uint16_t instance, uint8_t attr_no)
{
	if (mib == NULL)
		return 0;

	const struct table *t = table_find(mib, class_id, instance, attr_no);

	return t != NULL ? t->count : 0;
}

size_t omci_mib_table_capacity(const struct omci_mib *mib)
{
	if (mib == NULL)
		return 0;

	size_t free_listed = 0;

	for (size_t n = mib->entry_free; n != NO_SLOT; n = mib->entries[n].next)
		free_listed++;

	return (OMCI_MIB_TABLE_ENTRIES - mib->entries_used) + free_listed;
}

enum omci_status omci_mib_table_get(const struct omci_mib *mib,
				    uint16_t class_id, uint16_t instance,
				    uint8_t attr_no, size_t index, void *out,
				    size_t cap)
{
	if (mib == NULL || out == NULL)
		return OMCI_ERR_INVAL;
	if (find_slot(mib, class_id, instance) == NO_SLOT)
		return OMCI_ERR_INST_UNKNOWN;

	const uint8_t size = table_entry_size(class_id, attr_no);

	if (size == 0)
		return OMCI_ERR_ATTR_UNKNOWN;
	if (cap < size)
		return OMCI_ERR_TRUNCATED;

	const struct table *t = table_find(mib, class_id, instance, attr_no);

	if (t == NULL || index >= t->count)
		return OMCI_DECLINED;

	size_t n = t->head;

	for (size_t i = 0; i < index; i++)
		n = mib->entries[n].next;

	omci_wire_copy(out, mib->entries[n].bytes, size);
	return OMCI_OK;
}

enum omci_status omci_mib_table_put(struct omci_mib *mib, uint16_t class_id,
				    uint16_t instance, uint8_t attr_no,
				    const void *entry, size_t len,
				    size_t key_len,
				    enum omci_mib_table_order where)
{
	if (mib == NULL || entry == NULL)
		return OMCI_ERR_INVAL;
	if (find_slot(mib, class_id, instance) == NO_SLOT)
		return OMCI_ERR_INST_UNKNOWN;

	const uint8_t size = table_entry_size(class_id, attr_no);

	if (size == 0)
		return OMCI_ERR_ATTR_UNKNOWN;
	if (len != size || key_len > size)
		return OMCI_ERR_INVAL;

	const uint8_t *want = entry;

	struct table *t = table_find(mib, class_id, instance, attr_no);

	if (t != NULL && key_len > 0) {
		for (size_t n = t->head; n != NO_SLOT;
		     n = mib->entries[n].next) {
			if (!omci_wire_equal(mib->entries[n].bytes, want,
					     key_len))
				continue;
			omci_wire_copy(mib->entries[n].bytes, want, size);
			mib->table_gen++;
			mib->gen++;
			return OMCI_OK;
		}
	}

	if (t == NULL) {
		for (size_t i = 0; i < OMCI_MIB_MAX_TABLES && t == NULL; i++)
			if (!mib->tables[i].used)
				t = &mib->tables[i];
		if (t == NULL) {
			OMCI_WARN(
				"mib: no table slot left for %u/%u attribute %u",
				class_id, instance, attr_no);
			return OMCI_ERR_NOMEM;
		}
		*t = (struct table){
			.class_id = class_id,
			.instance = instance,
			.attr_no = attr_no,
			.entry_size = size,
			.used = true,
			.head = NO_SLOT,
			.count = 0,
		};
	}

	const size_t n = entry_alloc(mib);

	if (n == NO_SLOT) {
		OMCI_WARN("mib: table entry pool is full (%u entries)",
			  OMCI_MIB_TABLE_ENTRIES);
		if (t->count == 0)
			t->used = false;
		return OMCI_ERR_NOMEM;
	}
	omci_wire_copy(mib->entries[n].bytes, want, size);

	if (where == OMCI_MIB_TABLE_FIRST || t->head == NO_SLOT) {
		mib->entries[n].next = t->head;
		t->head = n;
	} else {
		size_t tail = t->head;

		while (mib->entries[tail].next != NO_SLOT)
			tail = mib->entries[tail].next;
		mib->entries[tail].next = n;
		mib->entries[n].next = NO_SLOT;
	}
	t->count++;
	mib->table_gen++;
	mib->gen++;
	return OMCI_OK;
}

enum omci_status omci_mib_table_remove(struct omci_mib *mib, uint16_t class_id,
				       uint16_t instance, uint8_t attr_no,
				       const void *key, size_t key_len)
{
	if (mib == NULL || key == NULL || key_len == 0)
		return OMCI_ERR_INVAL;
	if (find_slot(mib, class_id, instance) == NO_SLOT)
		return OMCI_ERR_INST_UNKNOWN;

	struct table *t = table_find(mib, class_id, instance, attr_no);

	if (t == NULL)
		return OMCI_DECLINED;
	if (key_len > t->entry_size)
		return OMCI_ERR_INVAL;

	size_t prev = NO_SLOT;

	for (size_t n = t->head; n != NO_SLOT;
	     prev = n, n = mib->entries[n].next) {
		if (!omci_wire_equal(mib->entries[n].bytes, key, key_len))
			continue;

		if (prev == NO_SLOT)
			t->head = mib->entries[n].next;
		else
			mib->entries[prev].next = mib->entries[n].next;
		entry_free(mib, n);

		if (--t->count == 0)
			t->used = false;
		mib->table_gen++;
		mib->gen++;
		return OMCI_OK;
	}
	return OMCI_DECLINED;
}

/*
 * The MIB as bytes: a header, then each instance with its values as they
 * lie in its row, then each table attribute with its rows in order.
 *
 *   header      "OMIB", version (2), instances (2), tables (2), 0 (2)
 *   instance    class (2), instance (2), size of the values (2), values
 *   table       class (2), instance (2), attribute (1), size of a row (1),
 *               rows (2), the rows
 */
#define SAVE_MAGIC 0x4f4d4942u
#define SAVE_VERSION 1u
#define SAVE_HEADER 12u

/* A place in a buffer of bytes, and whether a write or read went past its end. */
struct bytes {
	uint8_t *out;
	const uint8_t *in;
	size_t len;
	size_t at;
	bool short_of_room;
};

static uint8_t *put(struct bytes *b, size_t n)
{
	if (b->short_of_room || b->len - b->at < n) {
		b->short_of_room = true;
		return NULL;
	}
	b->at += n;
	return b->out + b->at - n;
}

static void put16(struct bytes *b, unsigned int v)
{
	uint8_t *p = put(b, 2);

	if (p != NULL)
		omci_be16_store(p, v);
}

static void put_bytes(struct bytes *b, const void *v, size_t n)
{
	uint8_t *p = put(b, n);

	if (p != NULL)
		omci_wire_copy(p, v, n);
}

static const uint8_t *take(struct bytes *b, size_t n)
{
	if (b->short_of_room || b->len - b->at < n) {
		b->short_of_room = true;
		return NULL;
	}
	b->at += n;
	return b->in + b->at - n;
}

static unsigned int take16(struct bytes *b)
{
	const uint8_t *p = take(b, 2);

	return p != NULL ? omci_be16(p) : 0u;
}

static unsigned int take8(struct bytes *b)
{
	const uint8_t *p = take(b, 1);

	return p != NULL ? *p : 0u;
}

enum omci_status omci_mib_save(const struct omci_mib *mib, uint8_t *out,
			       size_t cap, size_t *out_len)
{
	if (mib == NULL || out == NULL || out_len == NULL)
		return OMCI_ERR_INVAL;

	struct bytes b = { .out = out, .len = cap };
	size_t tables = 0;

	for (size_t i = 0; i < OMCI_MIB_MAX_TABLES; i++)
		tables += mib->tables[i].used;

	uint8_t *magic = put(&b, 4);

	if (magic != NULL)
		omci_be32_store(magic, SAVE_MAGIC);
	put16(&b, SAVE_VERSION);
	put16(&b, (unsigned int)mib->live);
	put16(&b, (unsigned int)tables);
	put16(&b, 0);

	for (size_t i = 0; i < mib->row_count; i++) {
		const struct row *r = &mib->rows[i];

		if (!r->used)
			continue;
		put16(&b, r->class_id);
		put16(&b, r->instance);
		put16(&b, r->size);
		put_bytes(&b, r->data, r->size);
	}

	for (size_t i = 0; i < OMCI_MIB_MAX_TABLES; i++) {
		const struct table *t = &mib->tables[i];

		if (!t->used)
			continue;
		put16(&b, t->class_id);
		put16(&b, t->instance);
		put_bytes(&b, &t->attr_no, 1);
		put_bytes(&b, &t->entry_size, 1);
		put16(&b, (unsigned int)t->count);
		for (size_t n = t->head; n != NO_SLOT; n = mib->entries[n].next)
			put_bytes(&b, mib->entries[n].bytes, t->entry_size);
	}

	if (b.short_of_room)
		return OMCI_ERR_TRUNCATED;
	*out_len = b.at;
	return OMCI_OK;
}

static enum omci_status load(struct omci_mib *mib, struct bytes *b)
{
	const uint8_t *magic = take(b, 4);

	if (magic == NULL)
		return OMCI_ERR_TRUNCATED;
	const unsigned int version = take16(b);
	const unsigned int rows = take16(b);
	const unsigned int tables = take16(b);

	(void)take16(b);
	if (b->short_of_room)
		return OMCI_ERR_TRUNCATED;
	if (omci_be32(magic) != SAVE_MAGIC || version != SAVE_VERSION)
		return OMCI_ERR_PROTO;

	for (unsigned int i = 0; i < rows; i++) {
		const uint16_t class_id = (uint16_t)take16(b);
		const uint16_t instance = (uint16_t)take16(b);
		const size_t size = take16(b);
		const uint8_t *values = take(b, size);

		if (values == NULL)
			return OMCI_ERR_TRUNCATED;

		const struct omci_me_class *cls = omci_me_class_by_id(class_id);

		if (cls == NULL || omci_me_row_size(cls) != size)
			return OMCI_ERR_PROTO;

		const enum omci_status rc =
			omci_mib_create(mib, class_id, instance);

		if (rc != OMCI_OK)
			return rc;
		/* A MIB just reset has no free rows: the new one is the last. */
		omci_wire_copy(mib->rows[mib->row_count - 1u].data, values,
			       size);
	}

	for (unsigned int i = 0; i < tables; i++) {
		const uint16_t class_id = (uint16_t)take16(b);
		const uint16_t instance = (uint16_t)take16(b);
		const uint8_t attr_no = (uint8_t)take8(b);
		const size_t size = take8(b);
		const unsigned int count = take16(b);
		const uint8_t *entries = take(b, (size_t)count * size);

		if (entries == NULL)
			return OMCI_ERR_TRUNCATED;
		if (table_entry_size(class_id, attr_no) != size)
			return OMCI_ERR_PROTO;
		for (unsigned int n = 0; n < count; n++) {
			const enum omci_status rc = omci_mib_table_put(
				mib, class_id, instance, attr_no,
				entries + (size_t)n * size, size, 0,
				OMCI_MIB_TABLE_LAST);

			if (rc != OMCI_OK)
				return rc;
		}
	}

	return b->at == b->len ? OMCI_OK : OMCI_ERR_PROTO;
}

enum omci_status omci_mib_load(struct omci_mib *mib, const uint8_t *in,
			       size_t len)
{
	if (mib == NULL || in == NULL)
		return OMCI_ERR_INVAL;

	struct bytes b = { .in = in, .len = len };

	(void)omci_mib_reset(mib);

	const enum omci_status rc = load(mib, &b);

	if (rc != OMCI_OK)
		(void)omci_mib_reset(mib);
	return rc;
}
