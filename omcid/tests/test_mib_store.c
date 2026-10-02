// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the MIB store. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/layout.h"
#include "me/table.h"
#include "mib/store.h"
#include "omcid/array_size.h"
#include "omcid/status.h"

#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

#define ANIG 263

static void test_a_fresh_mib_is_empty(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_count(mib), 0);
	CHECK(!omci_mib_exists(mib, ANIG, 0));

	struct omci_mib_cursor c;

	CHECK_EQ(omci_mib_walk_first(mib, &c), OMCI_DECLINED);
}

static void test_create_and_delete(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_create(mib, ANIG, 0x8001), OMCI_OK);
	CHECK(omci_mib_exists(mib, ANIG, 0x8001));
	CHECK_EQ(omci_mib_count(mib), 1);

	CHECK_EQ(omci_mib_create(mib, ANIG, 0x8001), OMCI_ERR_EXISTS);
	CHECK_EQ(omci_mib_create(mib, 0xfffd, 1), OMCI_ERR_ME_UNKNOWN);

	CHECK_EQ(omci_mib_delete(mib, ANIG, 0x8001), OMCI_OK);
	CHECK(!omci_mib_exists(mib, ANIG, 0x8001));
	CHECK_EQ(omci_mib_count(mib), 0);
	CHECK_EQ(omci_mib_delete(mib, ANIG, 0x8001), OMCI_ERR_INST_UNKNOWN);
}

static void test_a_new_row_is_zeroed(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_create(mib, ANIG, 1), OMCI_OK);

	uint8_t v[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	size_t n = 0;

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 1, 2, v, sizeof(v), &n), OMCI_OK);
	CHECK_EQ(n, 2);
	CHECK_EQ(v[0], 0);
	CHECK_EQ(v[1], 0);
}

static void test_attributes_round_trip(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_create(mib, ANIG, 1), OMCI_OK);

	const uint8_t key[2] = { 0x80, 0x01 };

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 1, 2, key, sizeof(key)), OMCI_OK);

	uint8_t got[2] = { 0 };
	size_t n = 0;

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 1, 2, got, sizeof(got), &n),
		 OMCI_OK);
	CHECK_EQ(n, 2);
	CHECK_EQ(memcmp(got, key, 2), 0);

	const uint8_t one = 0x5a;

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 1, 1, &one, 1), OMCI_OK);

	uint8_t back = 0;

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 1, 1, &back, 1, NULL), OMCI_OK);
	CHECK_EQ(back, 0x5a);

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 1, 2, got, sizeof(got), &n),
		 OMCI_OK);
	CHECK_EQ(memcmp(got, key, 2), 0);
}

static void test_attribute_errors_are_distinguishable(struct omci_mib *mib)
{
	const uint8_t v[2] = { 0, 1 };

	CHECK(omci_mib_attr_set(mib, ANIG, 0x7777u, 1u, v, 1) != OMCI_OK);

	uint8_t buf[8];

	CHECK_EQ(omci_mib_attr_get(mib, 0xfffd, 1, 1, buf, sizeof(buf), NULL),
		 OMCI_ERR_ME_UNKNOWN);
	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 9, 1, buf, sizeof(buf), NULL),
		 OMCI_ERR_INST_UNKNOWN);

	CHECK_EQ(omci_mib_create(mib, ANIG, 9), OMCI_OK);
	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 9, 99, buf, sizeof(buf), NULL),
		 OMCI_ERR_ATTR_UNKNOWN);
	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 9, 0, buf, sizeof(buf), NULL),
		 OMCI_ERR_ATTR_UNKNOWN);

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 9, 2, buf, 1, NULL),
		 OMCI_ERR_TRUNCATED);

	const uint8_t three[3] = { 1, 2, 3 };

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 9, 2, three, 3), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 9, 2, three, 1), OMCI_ERR_INVAL);
}

static void test_delete_then_create_reuses_and_zeroes(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_create(mib, ANIG, 1), OMCI_OK);

	const uint8_t dirty[2] = { 0xde, 0xad };

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 1, 2, dirty, 2), OMCI_OK);
	CHECK_EQ(omci_mib_delete(mib, ANIG, 1), OMCI_OK);

	CHECK_EQ(omci_mib_create(mib, ANIG, 2), OMCI_OK);

	uint8_t v[2] = { 0xff, 0xff };

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 2, 2, v, sizeof(v), NULL),
		 OMCI_OK);
	CHECK_EQ(v[0], 0);
	CHECK_EQ(v[1], 0);
}

static void test_churn_does_not_exhaust_the_arena(struct omci_mib *mib)
{
	for (unsigned int i = 0; i < 4000; i++) {
		CHECK_EQ(omci_mib_create(mib, ANIG, (uint16_t)(i & 0xffff)),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, ANIG, (uint16_t)(i & 0xffff)),
			 OMCI_OK);
	}
	CHECK_EQ(omci_mib_count(mib), 0);

	CHECK_EQ(omci_mib_create(mib, ANIG, 1), OMCI_OK);
	CHECK_EQ(omci_mib_count(mib), 1);
}

static void test_both_capacity_limits_refuse_honestly(struct omci_mib *mib)
{
	unsigned int made = 0;

	for (unsigned int i = 0; i < OMCI_MIB_MAX_ROWS + 32u; i++) {
		if (omci_mib_create(mib, ANIG, (uint16_t)i) == OMCI_OK)
			made++;
		else
			break;
	}
	CHECK_EQ(made, OMCI_MIB_MAX_ROWS);
	CHECK_EQ(omci_mib_count(mib), OMCI_MIB_MAX_ROWS);

	CHECK_EQ(omci_mib_create(mib, ANIG, (uint16_t)(OMCI_MIB_MAX_ROWS + 1u)),
		 OMCI_ERR_NOMEM);

	const uint8_t v[2] = { 0x5a, 0xa5 };

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 0, 2, v, sizeof(v)), OMCI_OK);
	uint8_t got[2] = { 0, 0 };

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 0, 2, got, sizeof(got), NULL),
		 OMCI_OK);
	CHECK_EQ(got[0], 0x5au);
	CHECK_EQ(got[1], 0xa5u);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_count(mib), 0);

	const uint16_t WIDE = 322;

	CHECK(omci_me_class_by_id(WIDE) != NULL);

	made = 0;
	for (unsigned int i = 0; i < OMCI_MIB_MAX_ROWS + 32u; i++) {
		if (omci_mib_create(mib, WIDE, (uint16_t)i) == OMCI_OK)
			made++;
		else
			break;
	}
	CHECK(made > 0);
	CHECK(made < OMCI_MIB_MAX_ROWS);
	CHECK_EQ(omci_mib_count(mib), made);

	char log[512];
	char says[128];

	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(omci_mib_create(mib, WIDE, (uint16_t)(made + 1u)),
			 OMCI_ERR_NOMEM);
	});
	CHECK(strstr(log, "mib arena full: ") != NULL);
	(void)snprintf(says, sizeof(says), "/%u bytes, class %u wants ",
		       (unsigned int)OMCI_MIB_ARENA_BYTES, (unsigned int)WIDE);
	CHECK(strstr(log, says) != NULL);
	CHECK(strstr(log, " — raise OMCI_MIB_ARENA_BYTES") != NULL);
	CHECK(strstr(log, "mib full: ") == NULL);

	CHECK(omci_mib_exists(mib, WIDE, (uint16_t)(made - 1u)));
}

static void test_walk_visits_every_live_row_once(struct omci_mib *mib)
{
	const uint16_t insts[] = { 1, 2, 3, 4, 5 };

	for (size_t i = 0; i < ARRAY_SIZE(insts); i++)
		CHECK_EQ(omci_mib_create(mib, ANIG, insts[i]), OMCI_OK);

	CHECK_EQ(omci_mib_delete(mib, ANIG, 3), OMCI_OK);

	unsigned int seen = 0;
	bool saw_deleted = false;
	struct omci_mib_cursor c;

	for (enum omci_status rc = omci_mib_walk_first(mib, &c); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &c)) {
		seen++;
		if (c.instance == 3)
			saw_deleted = true;
		CHECK_EQ(c.class_id, ANIG);
	}

	CHECK_EQ(seen, 4);
	CHECK(!saw_deleted);
	CHECK_EQ(seen, omci_mib_count(mib));
}

static void test_reset_forgets_everything(struct omci_mib *mib)
{
	for (uint16_t i = 1; i <= 20; i++)
		CHECK_EQ(omci_mib_create(mib, ANIG, i), OMCI_OK);
	CHECK_EQ(omci_mib_count(mib), 20);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_count(mib), 0);
	CHECK(!omci_mib_exists(mib, ANIG, 1));

	struct omci_mib_cursor c;

	CHECK_EQ(omci_mib_walk_first(mib, &c), OMCI_DECLINED);

	for (uint16_t i = 1; i <= 20; i++)
		CHECK_EQ(omci_mib_create(mib, ANIG, i), OMCI_OK);
	CHECK_EQ(omci_mib_count(mib), 20);
}

static void test_many_classes_coexist(struct omci_mib *mib)
{
	unsigned int created = 0;

	for (size_t i = 0; i < omci_me_class_count; i++) {
		if (omci_mib_create(mib, omci_me_classes[i].class_id, 1) ==
		    OMCI_OK)
			created++;
	}
	CHECK_EQ(created, omci_me_class_count);
	CHECK_EQ(omci_mib_count(mib), omci_me_class_count);
}

#define T_CLASS 171u
#define T_ATTR 6u
#define T_SIZE 16u
#define T_KEY 8u

static void rule(uint8_t out[T_SIZE], uint8_t k, uint8_t v)
{
	memset(out, 0, T_SIZE);
	out[0] = k;
	out[7] = k;
	out[8] = v;
	out[15] = v;
}

static uint8_t first_byte(struct omci_mib *mib, size_t index)
{
	uint8_t e[T_SIZE];

	return omci_mib_table_get(mib, T_CLASS, 1, T_ATTR, index, e,
				  sizeof(e)) == OMCI_OK ?
		       e[0] :
		       0xff;
}

static void test_a_table_holds_a_list_where_an_attribute_held_one_value(
	struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);

	uint8_t a[T_SIZE], b[T_SIZE], c[T_SIZE];

	rule(a, 0x11, 0xA0);
	rule(b, 0x22, 0xB0);
	rule(c, 0x33, 0xC0);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, a, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, b, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, c, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 3u);

	CHECK_EQ(first_byte(mib, 0), 0x11u);
	CHECK_EQ(first_byte(mib, 1), 0x22u);
	CHECK_EQ(first_byte(mib, 2), 0x33u);

	uint8_t sink[T_SIZE];

	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 1, T_ATTR, 3, sink,
				    sizeof(sink)),
		 OMCI_DECLINED);
}

static void test_order_is_the_callers_choice(struct omci_mib *mib)
{
	uint8_t a[T_SIZE], b[T_SIZE];

	rule(a, 0x11, 0xA0);
	rule(b, 0x22, 0xB0);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, a, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, b, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(first_byte(mib, 0), 0x11u);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, a, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_FIRST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, b, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_FIRST),
		 OMCI_OK);
	CHECK_EQ(first_byte(mib, 0), 0x22u);
}

static void test_a_key_match_replaces_in_place(struct omci_mib *mib)
{
	uint8_t a[T_SIZE], b[T_SIZE], a2[T_SIZE], got[T_SIZE];

	rule(a, 0x11, 0xA0);
	rule(b, 0x22, 0xB0);
	rule(a2, 0x11, 0xEE);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, a, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	const uint32_t tg_one = omci_mib_table_generation(mib);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, b, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_generation(mib), tg_one + 1u);

	const uint32_t tg_two = omci_mib_table_generation(mib);
	const uint32_t g_two = omci_mib_generation(mib);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, a2, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_FIRST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_generation(mib), tg_two + 1u);
	CHECK_EQ(omci_mib_generation(mib), g_two + 1u);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 2u);
	CHECK_EQ(first_byte(mib, 0), 0x11u);
	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 1, T_ATTR, 0, got,
				    sizeof(got)),
		 OMCI_OK);
	CHECK_EQ(got[8], 0xEEu);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, a2, T_SIZE, 0,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 3u);
}

static void test_remove_by_key(struct omci_mib *mib)
{
	uint8_t a[T_SIZE], b[T_SIZE], c[T_SIZE];

	rule(a, 0x11, 0xA0);
	rule(b, 0x22, 0xB0);
	rule(c, 0x33, 0xC0);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	for (const uint8_t *r = a; r != NULL; r = (r == a) ? b :
						  (r == b) ? c :
							     NULL)
		CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE,
					    T_KEY, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 3u);

	const uint32_t tg_before = omci_mib_table_generation(mib);

	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, b, T_KEY),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_generation(mib), tg_before + 1u);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 2u);
	CHECK_EQ(first_byte(mib, 0), 0x11u);
	CHECK_EQ(first_byte(mib, 1), 0x33u);

	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, a, T_KEY),
		 OMCI_OK);
	CHECK_EQ(first_byte(mib, 0), 0x33u);

	const uint32_t tg_gone = omci_mib_table_generation(mib);

	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, a, T_KEY),
		 OMCI_DECLINED);
	CHECK_EQ(omci_mib_table_generation(mib), tg_gone);
}

static void test_entries_come_back_to_the_pool(struct omci_mib *mib)
{
	uint8_t r[T_SIZE];

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	const size_t empty = omci_mib_table_capacity(mib);

	CHECK_EQ(empty, OMCI_MIB_TABLE_ENTRIES);

	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	for (unsigned int i = 0; i < 10; i++) {
		rule(r, (uint8_t)(i + 1), 0x10);
		CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE,
					    T_KEY, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
	}
	CHECK_EQ(omci_mib_table_capacity(mib), empty - 10u);

	CHECK_EQ(omci_mib_delete(mib, T_CLASS, 1), OMCI_OK);
	CHECK_EQ(omci_mib_table_capacity(mib), empty);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 0u);

	CHECK_EQ(omci_mib_create(mib, T_CLASS, 2), OMCI_OK);
	rule(r, 0x77, 0x88);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 2, T_ATTR, r, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_table_capacity(mib), empty);
}

static void test_the_edges_are_refused_distinguishably(struct omci_mib *mib)
{
	uint8_t r[T_SIZE], sink[T_SIZE];

	rule(r, 0x11, 0xA0);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_INST_UNKNOWN);

	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE - 1u,
				    T_KEY, OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_INVAL);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE,
				    T_SIZE + 1u, OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_INVAL);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, 99, r, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_ATTR_UNKNOWN);

	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 1, T_ATTR, 0, sink,
				    T_SIZE - 1u),
		 OMCI_ERR_TRUNCATED);
}

static void test_the_pool_runs_out_honestly(struct omci_mib *mib)
{
	uint8_t r[T_SIZE];

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);

	unsigned int put = 0;

	for (unsigned int i = 0; i < OMCI_MIB_TABLE_ENTRIES + 8u; i++) {
		rule(r, 0, 0);
		r[0] = (uint8_t)(i & 0xffu);
		r[1] = (uint8_t)(i >> 8);
		if (omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE,
				       T_KEY, OMCI_MIB_TABLE_LAST) == OMCI_OK)
			put++;
	}
	CHECK_EQ(put, OMCI_MIB_TABLE_ENTRIES);
	CHECK_EQ(omci_mib_table_capacity(mib), 0u);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR),
		 OMCI_MIB_TABLE_ENTRIES);
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
}

static void test_table_slots_run_out_honestly(struct omci_mib *mib)
{
	uint8_t r[T_SIZE];

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);

	unsigned int tables_made = 0;

	for (uint16_t inst = 1; inst <= (uint16_t)(OMCI_MIB_MAX_TABLES + 1u);
	     inst++) {
		CHECK_EQ(omci_mib_create(mib, T_CLASS, inst), OMCI_OK);
		rule(r, (uint8_t)inst, 0xA0);
		const enum omci_status rc =
			omci_mib_table_put(mib, T_CLASS, inst, T_ATTR, r,
					   T_SIZE, T_KEY, OMCI_MIB_TABLE_LAST);
		if (rc == OMCI_OK)
			tables_made++;
		else
			CHECK_EQ(rc, OMCI_ERR_NOMEM);
	}
	CHECK_EQ(tables_made, OMCI_MIB_MAX_TABLES);

	const uint16_t loser = (uint16_t)(OMCI_MIB_MAX_TABLES + 1u);

	CHECK(omci_mib_exists(mib, T_CLASS, loser));
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, loser, T_ATTR), 0u);

	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 1u);
	CHECK_EQ(first_byte(mib, 0), 1u);

	CHECK_EQ(omci_mib_table_capacity(mib),
		 OMCI_MIB_TABLE_ENTRIES - tables_made);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
}

static void test_every_table_entry_fits_a_node(void)
{
	unsigned int widest = 0;
	uint16_t widest_class = 0;

	for (size_t i = 0; i < omci_me_class_count; i++) {
		const struct omci_me_class *cls = &omci_me_classes[i];

		for (uint8_t n = 1; n <= cls->attr_count; n++) {
			const struct omci_me_attr *a = omci_me_attr_get(cls, n);

			if (!omci_me_attr_is_table(a))
				continue;

			CHECK(a->size <= OMCI_MIB_TABLE_ENTRY_MAX);
			if (a->size > widest) {
				widest = a->size;
				widest_class = cls->class_id;
			}
		}
	}
	printf("  widest table row: %u bytes (ME %u), a node holds %u\n",
	       widest, widest_class, (unsigned int)OMCI_MIB_TABLE_ENTRY_MAX);
	CHECK(widest > 0);
}

static uint32_t store_digest(const struct omci_mib *mib)
{
	uint32_t h = 2166136261u;

	struct omci_mib_cursor cur;

	for (enum omci_status w = omci_mib_walk_first(mib, &cur); w == OMCI_OK;
	     w = omci_mib_walk_next(mib, &cur)) {
		const struct omci_me_class *cls =
			omci_me_class_by_id(cur.class_id);
		h ^= cur.class_id;
		h *= 16777619u;
		h ^= cur.instance;
		h *= 16777619u;
		if (cls == NULL)
			continue;

		for (uint8_t d = 1; d <= cls->attr_count; d++) {
			uint8_t attr[64];
			size_t len = 0;

			if (omci_mib_attr_get(mib, cur.class_id, cur.instance,
					      d, attr, sizeof(attr),
					      &len) == OMCI_OK)
				for (size_t b = 0; b < len; b++) {
					h ^= attr[b];
					h *= 16777619u;
				}

			const size_t rows = omci_mib_table_count(
				mib, cur.class_id, cur.instance, d);
			for (size_t r = 0; r < rows; r++) {
				uint8_t row[32] = { 0 };

				if (omci_mib_table_get(mib, cur.class_id,
						       cur.instance, d, r, row,
						       sizeof(row)) != OMCI_OK)
					continue;
				for (size_t b = 0; b < sizeof(row); b++) {
					h ^= row[b];
					h *= 16777619u;
				}
			}
		}
	}
	return h;
}

static void test_the_generation_counter_moves_exactly_when_the_store_does(
	struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);

	uint32_t g = omci_mib_generation(mib);
	uint32_t d = store_digest(mib);

#define MOVED_TOGETHER()                              \
	do {                                          \
		CHECK(omci_mib_generation(mib) != g); \
		CHECK(store_digest(mib) != d);        \
		g = omci_mib_generation(mib);         \
		d = store_digest(mib);                \
	} while (0)

	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	MOVED_TOGETHER();

	const uint8_t assoc[1] = { 2 };

	CHECK_EQ(omci_mib_attr_set(mib, T_CLASS, 1, 1, assoc, sizeof(assoc)),
		 OMCI_OK);
	MOVED_TOGETHER();

	CHECK_EQ(omci_mib_attr_set(mib, T_CLASS, 1, 1, assoc, sizeof(assoc)),
		 OMCI_OK);
	CHECK(omci_mib_generation(mib) != g);
	CHECK_EQ(store_digest(mib), d);
	g = omci_mib_generation(mib);

	uint8_t e[T_SIZE];

	rule(e, 1, 10);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, e, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	MOVED_TOGETHER();

	rule(e, 1, 20);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, e, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	MOVED_TOGETHER();

	uint8_t out[T_SIZE];
	uint8_t attr[1];

	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 1, T_ATTR, 0, out,
				    sizeof(out)),
		 OMCI_OK);
	size_t attr_len = 0;

	CHECK_EQ(omci_mib_attr_get(mib, T_CLASS, 1, 1, attr, sizeof(attr),
				   &attr_len),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 1, T_ATTR), 1u);
	CHECK(omci_mib_exists(mib, T_CLASS, 1));
	CHECK_EQ(omci_mib_count(mib), 1u);
	struct omci_mib_cursor cur;

	CHECK_EQ(omci_mib_walk_first(mib, &cur), OMCI_OK);
	CHECK_EQ(omci_mib_generation(mib), g);
	CHECK_EQ(store_digest(mib), d);

	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_ERR_EXISTS);
	CHECK_EQ(omci_mib_attr_set(mib, T_CLASS, 1, 1, assoc, 2),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_delete(mib, T_CLASS, 99), OMCI_ERR_INST_UNKNOWN);
	CHECK_EQ(omci_mib_generation(mib), g);
	CHECK_EQ(store_digest(mib), d);

	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, e, T_KEY),
		 OMCI_OK);
	MOVED_TOGETHER();

	CHECK_EQ(omci_mib_delete(mib, T_CLASS, 1), OMCI_OK);
	MOVED_TOGETHER();

	CHECK_EQ(omci_mib_create(mib, T_CLASS, 2), OMCI_OK);
	MOVED_TOGETHER();

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK(omci_mib_generation(mib) != g);
}

static unsigned int g_sink_calls;

static void count_announcement(void *ctx, uint16_t class_id, uint16_t instance,
			       uint8_t attr_no, const void *value, size_t len)
{
	(void)ctx;
	(void)class_id;
	(void)instance;
	(void)attr_no;
	(void)value;
	(void)len;
	g_sink_calls++;
}

static const struct omci_mib_notify_ops g_counter_sink = {
	.attr_changed = count_announcement
};

static void
test_every_entry_point_refuses_what_it_cannot_use(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ANIG, 0x8001), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	uint8_t e[T_SIZE];

	rule(e, 1, 10);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, e, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);

	const uint32_t g = omci_mib_generation(mib);
	const uint32_t d = store_digest(mib);
	uint8_t buf[32] = { 0 };
	size_t n = 77;
	struct omci_mib_cursor cur = { 0 };

	struct omci_mib *none = NULL;

	CHECK_EQ(omci_mib_init(NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_reset(NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_create(NULL, ANIG, 1), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_delete(NULL, ANIG, 1), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_attr_get(NULL, ANIG, 1, 1, buf, sizeof(buf), &n),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_attr_set(NULL, ANIG, 1, 1, buf, 1), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_attr_set_by_olt(NULL, ANIG, 1, 1, buf, 1),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_announce_all(NULL, ANIG, 1), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_walk_first(NULL, &cur), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_walk_next(NULL, &cur), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_table_get(NULL, T_CLASS, 1, T_ATTR, 0, buf,
				    sizeof(buf)),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_table_put(NULL, T_CLASS, 1, T_ATTR, e, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_table_remove(NULL, T_CLASS, 1, T_ATTR, e, T_KEY),
		 OMCI_ERR_INVAL);
	CHECK(!omci_mib_exists(NULL, ANIG, 0x8001));
	CHECK_EQ(omci_mib_count(NULL), 0u);
	CHECK_EQ(omci_mib_generation(NULL), 0u);
	CHECK_EQ(omci_mib_table_generation(NULL), 0u);
	CHECK_EQ(omci_mib_table_count(NULL, T_CLASS, 1, T_ATTR), 0u);
	CHECK_EQ(omci_mib_table_capacity(NULL), 0u);
	omci_mib_fini(NULL);
	CHECK_EQ(n, 77u);
	CHECK(none == NULL);

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 0x8001, 1, NULL, sizeof(buf), &n),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 0x8001, 1, NULL, 1),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_attr_set_by_olt(mib, ANIG, 0x8001, 1, NULL, 1),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_walk_first(mib, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_walk_next(mib, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 1, T_ATTR, 0, NULL,
				    sizeof(buf)),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, NULL, T_SIZE,
				    T_KEY, OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, NULL, T_KEY),
		 OMCI_ERR_INVAL);

	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, e, 0),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, e, T_SIZE + 1u),
		 OMCI_ERR_INVAL);

	CHECK_EQ(omci_mib_announce_all(mib, 0xfffd, 1), OMCI_ERR_ME_UNKNOWN);
	CHECK_EQ(omci_mib_announce_all(mib, ANIG, 0x1234),
		 OMCI_ERR_INST_UNKNOWN);
	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 2, T_ATTR, 0, buf,
				    sizeof(buf)),
		 OMCI_ERR_INST_UNKNOWN);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 2, T_ATTR, e, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_INST_UNKNOWN);
	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 2, T_ATTR, e, T_KEY),
		 OMCI_ERR_INST_UNKNOWN);

	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 1, 1, 0, buf, sizeof(buf)),
		 OMCI_ERR_ATTR_UNKNOWN);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, 1, e, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_ERR_ATTR_UNKNOWN);
	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 1, 99, 0, buf, sizeof(buf)),
		 OMCI_ERR_ATTR_UNKNOWN);
	CHECK_EQ(omci_mib_table_get(mib, 0xfffd, 1, 1, 0, buf, sizeof(buf)),
		 OMCI_ERR_INST_UNKNOWN);

	CHECK_EQ(omci_mib_create(mib, T_CLASS, 3), OMCI_OK);
	const uint32_t g3 = omci_mib_generation(mib);

	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 3, T_ATTR, e, T_KEY),
		 OMCI_DECLINED);
	CHECK_EQ(omci_mib_table_get(mib, T_CLASS, 3, T_ATTR, 0, buf,
				    sizeof(buf)),
		 OMCI_DECLINED);
	CHECK_EQ(omci_mib_generation(mib), g3);
	CHECK_EQ(omci_mib_delete(mib, T_CLASS, 3), OMCI_OK);

	CHECK_EQ(omci_mib_generation(mib), g3 + 1u);
	CHECK(g3 == g + 1u);
	CHECK_EQ(omci_mib_count(mib), 2u);
	CHECK_EQ(store_digest(mib), d);
}

static void test_a_null_mib_does_not_unregister_the_sink(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ANIG, 0x8001), OMCI_OK);
	g_sink_calls = 0;
	omci_mib_notify_set(mib, &g_counter_sink, NULL);

	const uint8_t arc = 0x5a;

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 0x8001, 8, &arc, 1), OMCI_OK);
	CHECK_EQ(g_sink_calls, 1u);

	omci_mib_notify_set(NULL, NULL, NULL);
	const uint8_t arc2 = 0x5b;

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 0x8001, 8, &arc2, 1), OMCI_OK);
	CHECK_EQ(g_sink_calls, 2u);

	omci_mib_notify_set(mib, NULL, NULL);
	const uint8_t arc3 = 0x5c;

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 0x8001, 8, &arc3, 1), OMCI_OK);
	CHECK_EQ(g_sink_calls, 2u);
}

static void
test_only_the_attributes_that_announce_are_announced(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ANIG, 0x8001), OMCI_OK);
	g_sink_calls = 0;
	omci_mib_notify_set(mib, &g_counter_sink, NULL);

	const struct omci_me_class *cls = omci_me_class_by_id(ANIG);
	uint8_t value[32];
	unsigned int announcing = 0, silent = 0;

	memset(value, 0xa5, sizeof(value));
	for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);

		if (omci_me_attr_is_table(a) || a->size > sizeof(value))
			continue;
		const unsigned int before = g_sink_calls;

		CHECK_EQ(omci_mib_attr_set(mib, ANIG, 0x8001, attr, value,
					   a->size),
			 OMCI_OK);
		if ((a->flags & OMCI_ME_ATTR_AVC) != 0u) {
			CHECK_EQ(g_sink_calls, before + 1u);
			announcing++;
		} else {
			CHECK_EQ(g_sink_calls, before);
			silent++;
		}
	}
	CHECK(announcing > 0u && silent > 0u);

	g_sink_calls = 0;
	CHECK_EQ(omci_mib_announce_all(mib, ANIG, 0x8001), OMCI_OK);
	CHECK_EQ(g_sink_calls, announcing);
	omci_mib_notify_set(mib, NULL, NULL);
}

static void
test_a_freed_slot_behind_another_class_is_reused_in_place(struct omci_mib *mib)
{
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ANIG, 1), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, 6, 1), OMCI_OK);
	const uint8_t v = 0xee;

	CHECK_EQ(omci_mib_attr_set(mib, ANIG, 1, 1, &v, 1), OMCI_OK);

	CHECK_EQ(omci_mib_delete(mib, ANIG, 1), OMCI_OK);
	CHECK_EQ(omci_mib_delete(mib, T_CLASS, 1), OMCI_OK);

	CHECK_EQ(omci_mib_create(mib, ANIG, 2), OMCI_OK);
	uint8_t back = 0xff;

	CHECK_EQ(omci_mib_attr_get(mib, ANIG, 2, 1, &back, 1, NULL), OMCI_OK);
	CHECK_EQ(back, 0u);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 2), OMCI_OK);

	struct omci_mib_cursor cur;

	CHECK_EQ(omci_mib_walk_first(mib, &cur), OMCI_OK);
	CHECK(cur.class_id == ANIG && cur.instance == 2u && cur.slot == 0u);
	CHECK_EQ(omci_mib_walk_next(mib, &cur), OMCI_OK);
	CHECK(cur.class_id == T_CLASS && cur.instance == 2u && cur.slot == 1u);
	CHECK_EQ(omci_mib_walk_next(mib, &cur), OMCI_OK);
	CHECK(cur.class_id == 6u && cur.instance == 1u && cur.slot == 2u);
	CHECK_EQ(omci_mib_walk_next(mib, &cur), OMCI_DECLINED);
	CHECK_EQ(omci_mib_count(mib), 3u);
}

static void test_the_arena_fills_to_its_last_byte(struct omci_mib *mib)
{
	const size_t wide = omci_me_row_size(omci_me_class_by_id(134));
	const size_t one = omci_me_row_size(omci_me_class_by_id(2));
	const size_t none = omci_me_row_size(omci_me_class_by_id(287));

	CHECK(wide > 100u);
	CHECK_EQ(one, 1u);
	CHECK_EQ(none, 0u);

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	size_t used = 0;
	unsigned int rows = 0;

	for (uint16_t inst = 0; used + wide <= OMCI_MIB_ARENA_BYTES;
	     inst++, rows++, used += wide)
		CHECK_EQ(omci_mib_create(mib, 134, inst), OMCI_OK);
	for (uint16_t inst = 0; used < OMCI_MIB_ARENA_BYTES;
	     inst++, rows++, used += one)
		CHECK_EQ(omci_mib_create(mib, 2, inst), OMCI_OK);
	CHECK_EQ(used, (size_t)OMCI_MIB_ARENA_BYTES);
	CHECK(rows < OMCI_MIB_MAX_ROWS);

	char log[256];

	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(omci_mib_create(mib, 2, 0x7777), OMCI_ERR_NOMEM);
	});
	CHECK(strstr(log, "mib arena full: ") != NULL);
	CHECK_EQ(omci_mib_create(mib, 287, 0), OMCI_OK);
	CHECK_EQ(omci_mib_count(mib), (size_t)rows + 1u);
}

static void
test_a_deleted_instance_gives_its_table_slot_back(struct omci_mib *mib)
{
	uint8_t r[T_SIZE];

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	for (unsigned int k = 0; k < 3u * OMCI_MIB_MAX_TABLES; k++) {
		const uint16_t inst = (uint16_t)(1u + k);

		CHECK_EQ(omci_mib_create(mib, T_CLASS, inst), OMCI_OK);
		rule(r, (uint8_t)k, 0xA0);
		CHECK_EQ(omci_mib_table_put(mib, T_CLASS, inst, T_ATTR, r,
					    T_SIZE, T_KEY, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_count(mib, T_CLASS, inst, T_ATTR), 1u);
		CHECK_EQ(omci_mib_delete(mib, T_CLASS, inst), OMCI_OK);
		CHECK_EQ(omci_mib_table_count(mib, T_CLASS, inst, T_ATTR), 0u);
	}
	CHECK_EQ(omci_mib_table_capacity(mib), OMCI_MIB_TABLE_ENTRIES);
}

static void
test_the_store_traces_what_it_makes_and_deletes(struct omci_mib *mib)
{
	char log[512];

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	omci_log_level_set(OMCI_LOG_TRACE);
	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(omci_mib_create(mib, ANIG, 0x8001), OMCI_OK);
	});
	CHECK_EQ(omci_t_log_count(log, "create 263/32769 ("), 1u);
	CHECK_EQ(omci_t_log_count(log, " B)"), 1u);
	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(omci_mib_delete(mib, ANIG, 0x8001), OMCI_OK);
	});
	CHECK_EQ(omci_t_log_count(log, "delete 263/32769"), 1u);
	omci_log_level_set(OMCI_LOG_INFO);
}

static void
test_a_refused_first_entry_leaves_no_table_behind(struct omci_mib *mib)
{
	char log[16384];

	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, T_CLASS, 1), OMCI_OK);
	uint8_t r[T_SIZE];

	for (unsigned int i = 0; i < OMCI_MIB_TABLE_ENTRIES; i++) {
		rule(r, 0, 0);
		r[0] = (uint8_t)(i & 0xffu);
		r[1] = (uint8_t)(i >> 8);
		CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 1, T_ATTR, r, T_SIZE,
					    T_KEY, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
	}
	CHECK_EQ(omci_mib_table_capacity(mib), 0u);

	const unsigned int many = OMCI_MIB_MAX_TABLES + 6u;

	rule(r, 0x77, 0x88);
	for (uint16_t inst = 2; inst < 2u + many; inst++)
		CHECK_EQ(omci_mib_create(mib, T_CLASS, inst), OMCI_OK);
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(log, {
		for (uint16_t inst = 2; inst < 2u + many; inst++)
			CHECK_EQ(omci_mib_table_put(mib, T_CLASS, inst, T_ATTR,
						    r, T_SIZE, T_KEY,
						    OMCI_MIB_TABLE_LAST),
				 OMCI_ERR_NOMEM);
	});
	omci_log_level_set(OMCI_LOG_INFO);
	CHECK_EQ(omci_t_log_count(
			 log, "mib: table entry pool is full (256 entries)"),
		 many);
	CHECK_EQ(omci_t_log_count(log, "no table slot left"), 0u);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 2, T_ATTR), 0u);

	uint8_t first[T_SIZE];

	rule(first, 0, 0);
	CHECK_EQ(omci_mib_table_remove(mib, T_CLASS, 1, T_ATTR, first, T_KEY),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, T_CLASS, 2, T_ATTR, r, T_SIZE, T_KEY,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_count(mib, T_CLASS, 2, T_ATTR), 1u);
}

int main(void)
{
	test_every_table_entry_fits_a_node();
	RUN_MIB(test_a_fresh_mib_is_empty);
	RUN_MIB(test_create_and_delete);
	RUN_MIB(test_a_new_row_is_zeroed);
	RUN_MIB(test_attributes_round_trip);
	RUN_MIB(test_attribute_errors_are_distinguishable);
	RUN_MIB(test_delete_then_create_reuses_and_zeroes);
	RUN_MIB(test_churn_does_not_exhaust_the_arena);
	RUN_MIB(test_both_capacity_limits_refuse_honestly);
	RUN_MIB(test_walk_visits_every_live_row_once);
	RUN_MIB(test_reset_forgets_everything);
	RUN_MIB(test_many_classes_coexist);

	RUN_MIB(test_a_table_holds_a_list_where_an_attribute_held_one_value);
	RUN_MIB(test_order_is_the_callers_choice);
	RUN_MIB(test_a_key_match_replaces_in_place);
	RUN_MIB(test_remove_by_key);
	RUN_MIB(test_entries_come_back_to_the_pool);
	RUN_MIB(test_the_edges_are_refused_distinguishably);
	RUN_MIB(test_the_pool_runs_out_honestly);
	RUN_MIB(test_table_slots_run_out_honestly);
	RUN_MIB(test_the_generation_counter_moves_exactly_when_the_store_does);
	RUN_MIB(test_every_entry_point_refuses_what_it_cannot_use);
	RUN_MIB(test_a_null_mib_does_not_unregister_the_sink);
	RUN_MIB(test_only_the_attributes_that_announce_are_announced);
	RUN_MIB(test_a_freed_slot_behind_another_class_is_reused_in_place);
	RUN_MIB(test_a_refused_first_entry_leaves_no_table_behind);
	RUN_MIB(test_the_arena_fills_to_its_last_byte);
	RUN_MIB(test_a_deleted_instance_gives_its_table_slot_back);
	RUN_MIB(test_the_store_traces_what_it_makes_and_deletes);

	printf("\n%d omci_t_checks, %d omci_t_failures\n", omci_t_checks,
	       omci_t_failures);
	return OMCI_T_REPORT("test_mib_store");
}
