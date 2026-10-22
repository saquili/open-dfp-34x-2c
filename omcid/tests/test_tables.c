// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the table attributes, stored and made on demand. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/table.h"
#include "me/tables.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"
#include "model_tsv.h"
#include "omcid/array_size.h"

#define MT_SET 8u
#define MT_GET 9u
#define MT_GET_NEXT 26u

#define R_OK 0u
#define R_PARAM_ERROR 3u
#define R_DEVICE_BUSY 6u

#define SEGMENT 29u
#define INSTANCE 0x0abcu

static unsigned int g_tid;
static uint16_t next_tid(void)
{
	g_tid = (g_tid % 0x7FFEu) + 1u;
	return (uint16_t)g_tid;
}

static uint16_t be16_at(const uint8_t *f, unsigned int at)
{
	return (uint16_t)(((uint16_t)f[at] << 8) | f[at + 1u]);
}

static uint32_t be32_at(const uint8_t *f, unsigned int at)
{
	return ((uint32_t)f[at] << 24) | ((uint32_t)f[at + 1u] << 16) |
	       ((uint32_t)f[at + 2u] << 8) | (uint32_t)f[at + 3u];
}

static const uint8_t *drive(struct omci_engine *eng, uint8_t mt, uint16_t cls,
			    uint16_t inst, const uint8_t *contents, size_t n)
{
	const uint16_t tci = next_tid();
	struct omci_frame f = {
		.tci = tci,
		.msg_type = mt,
		.ar = true,
		.ak = false,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = cls,
		.instance = inst,
	};
	if (contents != NULL && n > 0)
		memcpy(f.contents, contents,
		       n > OMCI_CONTENTS_LEN ? OMCI_CONTENTS_LEN : n);

	uint8_t wire[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&f, wire);
	omci_t_tx_reset();
	(void)!omci_engine_rx(eng, wire);

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x20u) != 0u &&
		    (be16_at(omci_t_tx.frame[i], 0) & 0x7FFFu) == tci)
			return omci_t_tx.frame[i];
	return NULL;
}

static long get_size(struct omci_engine *eng, uint16_t cls, uint16_t inst,
		     unsigned int attr)
{
	const uint16_t bit = omci_attr_bit((uint8_t)attr);
	const uint8_t body[2] = { (uint8_t)(bit >> 8), (uint8_t)(bit & 0xffu) };
	const uint8_t *rsp = drive(eng, MT_GET, cls, inst, body, sizeof(body));

	CHECK(rsp != NULL);
	if (rsp == NULL || rsp[8] != R_OK)
		return -1;
	CHECK_EQ(be16_at(rsp, 9), bit);
	return (long)be32_at(rsp, 11);
}

static long walk(struct omci_engine *eng, uint16_t cls, uint16_t inst,
		 unsigned int attr, uint8_t *out, size_t cap)
{
	const long size = get_size(eng, cls, inst, attr);

	if (size < 0)
		return -1;
	if (size == 0)
		return 0;
	CHECK((size_t)size <= cap);
	if ((size_t)size > cap)
		return -1;

	const unsigned int segments =
		(unsigned int)((size + SEGMENT - 1) / SEGMENT);
	const uint16_t bit = omci_attr_bit((uint8_t)attr);
	size_t at = 0;

	for (unsigned int seq = 0; seq < segments; seq++) {
		const uint8_t body[4] = { (uint8_t)(bit >> 8),
					  (uint8_t)(bit & 0xffu),
					  (uint8_t)(seq >> 8), (uint8_t)seq };
		const uint8_t *rsp =
			drive(eng, MT_GET_NEXT, cls, inst, body, sizeof(body));

		CHECK(rsp != NULL);
		if (rsp == NULL)
			return -1;
		CHECK_EQ(rsp[8], R_OK);
		CHECK_EQ(be16_at(rsp, 9), bit);

		const size_t take = seq + 1u == segments ? (size_t)size - at :
							   SEGMENT;
		memcpy(&out[at], &rsp[11], take);
		at += take;
	}
	CHECK_EQ(at, (size_t)size);

	const uint8_t past[4] = { (uint8_t)(bit >> 8), (uint8_t)(bit & 0xffu),
				  (uint8_t)(segments >> 8), (uint8_t)segments };
	const uint8_t *refused =
		drive(eng, MT_GET_NEXT, cls, inst, past, sizeof(past));
	CHECK(refused != NULL);
	if (refused != NULL)
		CHECK_EQ(refused[8], R_PARAM_ERROR);
	return (long)at;
}

static void fixture(struct omci_t_fixture *fx)
{
	uint8_t reset[OMCI_FRAME_LEN];

	CHECK(omci_t_up(fx));
	omci_t_build(reset, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx->eng, reset), OMCI_OK);
	for (unsigned int c = 0; c < tsv_class_count; c++)
		(void)!omci_mib_create(fx->mib, tsv_classes[c].id, INSTANCE);
}

static void test_every_table_is_stored_or_rendered(void)
{
	unsigned int stored = 0, rendered = 0, other = 0;

	for (unsigned int c = 0; c < tsv_class_count; c++)
		for (unsigned int n = 1; n <= 16u; n++) {
			const struct tsv_class *t = &tsv_classes[c];
			const struct omci_me_table_source *src = NULL;
			struct omci_me_table_policy policy;
			const bool is_rendered =
				omci_me_table_rendered(t->id, (uint8_t)n, &src);
			const bool is_stored = omci_me_table_policy(
				t->id, (uint8_t)n, &policy);
			const bool table = n <= t->attr_count &&
					   t->attrs[n - 1].table;

			CHECK(!(is_rendered && is_stored));
			CHECK_EQ(is_rendered || is_stored, table);
			if (is_rendered) {
				CHECK(src != NULL && src->count != NULL &&
				      src->entry != NULL);
				rendered++;
			}
			if (is_stored) {
				const struct tsv_attr *a = &t->attrs[n - 1];

				CHECK(policy.key_len > 0u &&
				      policy.key_len <= a->size);
				CHECK(policy.max_entries > 0u);
				CHECK((size_t)a->size <=
				      OMCI_MIB_TABLE_ENTRY_MAX);
				if (policy.del_len != 0u)
					CHECK(policy.del_offset +
						      policy.del_len <=
					      a->size);
				stored++;
			}
			other += !table && (is_rendered || is_stored) ? 1u : 0u;
		}
	printf("  %u stored, %u rendered\n", stored, rendered);
	CHECK_EQ(stored, 19u);
	CHECK_EQ(rendered, 6u);
	CHECK_EQ(other, 0u);

	CHECK(!omci_me_table_rendered(287u, 1u, NULL));
	CHECK(!omci_me_table_policy(171u, 6u, NULL));
}

static void test_the_omci_entity_lists_what_the_daemon_holds(void)
{
	struct omci_t_fixture fx;
	uint8_t bytes[256];

	fixture(&fx);

	long n = walk(fx.eng, 287u, 0u, 1u, bytes, sizeof(bytes));

	CHECK_EQ(n, 2L * tsv_class_count);
	unsigned int prev = 0;

	for (unsigned int i = 0;
	     i < tsv_class_count && n == 2L * tsv_class_count; i++) {
		const unsigned int id = be16_at(bytes, 2u * i);

		CHECK_EQ(id, tsv_classes[i].id);
		CHECK(id > prev);
		prev = id;
	}

	uint32_t listed = 0;

	for (unsigned int c = 0; c < tsv_class_count; c++)
		listed |= tsv_classes[c].actions;
	static const unsigned int answered[] = { 4,  6,	 8,  9,	 11, 12, 13,
						 14, 15, 18, 24, 26, 28 };
	unsigned int want[32], nwant = 0;

	for (unsigned int i = 0; i < ARRAY_SIZE(answered); i++)
		if ((listed & (1u << answered[i])) != 0u)
			want[nwant++] = answered[i];

	n = walk(fx.eng, 287u, 0u, 2u, bytes, sizeof(bytes));
	CHECK_EQ(n, (long)nwant);
	for (unsigned int i = 0; i < nwant && n == (long)nwant; i++)
		CHECK_EQ(bytes[i], want[i]);
	CHECK_EQ(nwant, 13u);

	const uint8_t row[2 + 2] = { 0x80u, 0x00u, 0x00u, 0x2au };
	const uint8_t *rsp = drive(fx.eng, MT_SET, 287u, 0u, row, sizeof(row));

	CHECK(rsp == NULL || rsp[8] != R_OK);

	omci_t_down(&fx);
}

static void test_the_group_lists_are_empty(void)
{
	static const struct {
		uint16_t cls;
		uint8_t attr;
	} empty[] = { { 309, 9 }, { 311, 5 }, { 311, 6 } };
	struct omci_t_fixture fx;

	fixture(&fx);
	for (size_t i = 0; i < ARRAY_SIZE(empty); i++) {
		CHECK_EQ(get_size(fx.eng, empty[i].cls, INSTANCE,
				  empty[i].attr),
			 0L);

		const uint16_t bit = omci_attr_bit(empty[i].attr);
		const uint8_t body[4] = { (uint8_t)(bit >> 8),
					  (uint8_t)(bit & 0xffu), 0, 0 };
		const uint8_t *rsp = drive(fx.eng, MT_GET_NEXT, empty[i].cls,
					   INSTANCE, body, sizeof(body));

		CHECK(rsp != NULL);
		if (rsp != NULL)
			CHECK_EQ(rsp[8], R_PARAM_ERROR);
	}
	omci_t_down(&fx);
}

static void make_row(uint8_t *row, size_t size, unsigned int key,
		     unsigned int fill)
{
	memset(row, (int)fill, size);
	row[0] = (uint8_t)(key >> 8);
	row[1] = (uint8_t)key;
	row[2] = 0x5au;
	row[3] = (uint8_t)(0xc0u + (key & 0x3fu));
}

static unsigned int set_row(struct omci_engine *eng, uint16_t cls,
			    unsigned int attr, const uint8_t *row, size_t size)
{
	uint8_t body[OMCI_CONTENTS_LEN] = { 0 };
	const uint16_t bit = omci_attr_bit((uint8_t)attr);

	body[0] = (uint8_t)(bit >> 8);
	body[1] = (uint8_t)(bit & 0xffu);
	memcpy(&body[2], row, size);
	const uint8_t *rsp = drive(eng, MT_SET, cls, INSTANCE, body, 2u + size);

	return rsp != NULL ? rsp[8] : 0xffu;
}

static void test_the_stored_tables_fold_rows_by_their_key(void)
{
	struct omci_t_fixture fx;
	unsigned int tested = 0;

	fixture(&fx);

	for (unsigned int c = 0; c < tsv_class_count; c++)
		for (unsigned int n = 1; n <= tsv_classes[c].attr_count; n++) {
			const struct tsv_class *t = &tsv_classes[c];
			const struct tsv_attr *a = &t->attrs[n - 1];
			struct omci_me_table_policy policy;

			if (!a->table || !a->write ||
			    !omci_me_table_policy(t->id, (uint8_t)n, &policy))
				continue;
			/* A dial plan is known by one byte: test_the_voice_tables(). */
			if ((t->id == 171u && n == 6u) || t->id == 145u ||
			    policy.ctrl != OMCI_ME_TABLE_CTRL_NONE)
				continue;
			tested++;

			uint8_t rows[8][64], flat[8 * 64], row[64];
			const size_t size = a->size;

			make_row(rows[0], size, 1, 0x31);
			make_row(rows[1], size, 2, 0x32);
			CHECK_EQ(set_row(fx.eng, t->id, n, rows[0], size),
				 R_OK);
			CHECK_EQ(set_row(fx.eng, t->id, n, rows[1], size),
				 R_OK);
			CHECK_EQ(get_size(fx.eng, t->id, INSTANCE, n),
				 (long)(2 * size));
			CHECK_EQ(walk(fx.eng, t->id, INSTANCE, n, flat,
				      sizeof(flat)),
				 (long)(2 * size));
			CHECK_MEM(&flat[0], rows[0], size);
			CHECK_MEM(&flat[size], rows[1], size);

			CHECK_EQ(set_row(fx.eng, t->id, n, rows[0], size),
				 R_OK);
			CHECK_EQ(get_size(fx.eng, t->id, INSTANCE, n),
				 (long)(2 * size));
			memcpy(row, rows[0], size);
			row[size - 1u] ^= 0xffu;
			CHECK_EQ(set_row(fx.eng, t->id, n, row, size), R_OK);
			const long after = get_size(fx.eng, t->id, INSTANCE, n);

			if (policy.key_len == size)
				CHECK_EQ(after, (long)(3 * size));
			else
				CHECK_EQ(after, (long)(2 * size));
			if (policy.key_len < size) {
				CHECK_EQ(walk(fx.eng, t->id, INSTANCE, n, flat,
					      sizeof(flat)),
					 (long)(2 * size));
				CHECK_MEM(&flat[0], row, size);
			}

			if (policy.del_len != 0u) {
				memcpy(row, rows[1], size);
				memset(&row[policy.del_offset],
				       policy.del_value, policy.del_len);
				CHECK_EQ(set_row(fx.eng, t->id, n, row, size),
					 R_OK);
				CHECK_EQ(get_size(fx.eng, t->id, INSTANCE, n),
					 (long)size);

				CHECK_EQ(set_row(fx.eng, t->id, n, row, size),
					 R_OK);
				CHECK_EQ(get_size(fx.eng, t->id, INSTANCE, n),
					 (long)size);
			}

			for (unsigned int k = 10;
			     get_size(fx.eng, t->id, INSTANCE, n) <
				     (long)(policy.max_entries * size) &&
			     k < 10u + policy.max_entries + 4u;
			     k++) {
				make_row(row, size, k, 0x40);
				CHECK_EQ(set_row(fx.eng, t->id, n, row, size),
					 R_OK);
			}
			CHECK_EQ(get_size(fx.eng, t->id, INSTANCE, n),
				 (long)(policy.max_entries * size));
			make_row(row, size, 999, 0x41);
			CHECK_EQ(set_row(fx.eng, t->id, n, row, size),
				 R_DEVICE_BUSY);
			CHECK_EQ(get_size(fx.eng, t->id, INSTANCE, n),
				 (long)(policy.max_entries * size));

			uint8_t big[16 * 64];

			CHECK_EQ(walk(fx.eng, t->id, INSTANCE, n, big,
				      sizeof(big)),
				 (long)(policy.max_entries * size));

			printf("  ME %u attribute %u: %zu-byte rows, key %u, delete %s, up to %u rows\n",
			       t->id, n, size, policy.key_len,
			       policy.del_len != 0u ? "by marker" : "none",
			       policy.max_entries);
		}
	CHECK_EQ(tested, 3u);

	omci_t_down(&fx);
}

static void test_the_vlan_rule_table_walks(void)
{
	struct omci_t_fixture fx;
	uint8_t flat[8 * 16], rows[5][16];

	fixture(&fx);
	for (unsigned int i = 0; i < 5u; i++) {
		make_row(rows[i], 16, i + 1u, 0x20 + i);
		CHECK_EQ(omci_mib_table_put(fx.mib, 171u, INSTANCE, 6u, rows[i],
					    16, 8, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
	}
	CHECK_EQ(get_size(fx.eng, 171u, INSTANCE, 6u), 5L * 16);
	CHECK_EQ(walk(fx.eng, 171u, INSTANCE, 6u, flat, sizeof(flat)), 5L * 16);
	for (unsigned int i = 0; i < 5u; i++)
		CHECK_MEM(&flat[16u * i], rows[i], 16);

	omci_t_down(&fx);
}

static void test_the_table_functions_refuse_what_cannot_be_asked(void)
{
	struct omci_me_table_policy vlan, mcast, whole;
	uint8_t row[28];

	CHECK(omci_me_table_policy(171u, 6u, &vlan));
	CHECK(omci_me_table_policy(281u, 9u, &mcast));
	CHECK(omci_me_table_policy(171u, 10u, &whole));
	CHECK_EQ(whole.del_len, 0u);
	CHECK(!omci_me_table_policy(171u, 7u, &whole));
	CHECK(!omci_me_table_policy(3000u, 6u, &whole));

	memset(row, 0xff, sizeof(row));
	CHECK(omci_me_table_is_delete(&vlan, row, 16));
	CHECK(!omci_me_table_is_delete(NULL, row, 16));
	CHECK(!omci_me_table_is_delete(&vlan, NULL, 16));
	CHECK(!omci_me_table_is_delete(&vlan, row, 15));
	CHECK(!omci_me_table_is_delete(&whole, row, sizeof(row)));
	row[15] = 0xfeu;
	CHECK(!omci_me_table_is_delete(&vlan, row, 16));

	memset(row, 0, sizeof(row));
	row[0] = 0x12u;
	CHECK(omci_me_table_is_delete(&mcast, row, 12));
	row[11] = 0x01u;
	CHECK(!omci_me_table_is_delete(&mcast, row, 12));

	for (unsigned int k = 0; k < 2u; k++) {
		const struct omci_me_table_policy *p = k == 0u ? &vlan : &mcast;
		const size_t len = (size_t)p->del_offset + p->del_len;

		for (size_t b = 0; b < len; b++) {
			memset(row, p->del_value, sizeof(row));
			CHECK(omci_me_table_is_delete(p, row, len));
			row[b] = (uint8_t)~p->del_value;
			CHECK_EQ(omci_me_table_is_delete(p, row, len),
				 b < p->del_offset);
		}
	}

	const struct omci_me_table_source *classes = NULL, *types = NULL,
					  *groups = NULL;
	uint8_t two[2] = { 0xaau, 0xbbu }, one[1] = { 0xaau };

	CHECK(omci_me_table_rendered(287u, 1u, &classes));
	CHECK(omci_me_table_rendered(287u, 2u, &types));
	CHECK(omci_me_table_rendered(309u, 9u, &groups));
	if (classes == NULL || types == NULL || groups == NULL)
		return;

	CHECK_EQ(classes->count(0u), tsv_class_count);
	CHECK_EQ(types->count(0u), 13u);
	CHECK_EQ(groups->count(0u), 0u);

	CHECK(classes->entry(0u, 0u, two, sizeof(two)));
	CHECK_EQ(be16_at(two, 0), tsv_classes[0].id);
	two[0] = 0xaau;
	two[1] = 0xbbu;
	CHECK(!classes->entry(0u, 0u, NULL, sizeof(two)));
	CHECK(!classes->entry(0u, 0u, one, sizeof(one)));
	CHECK(!classes->entry(0u, tsv_class_count, two, sizeof(two)));
	CHECK_EQ(two[0], 0xaau);
	CHECK_EQ(two[1], 0xbbu);

	CHECK(types->entry(0u, 12u, one, sizeof(one)));
	CHECK_EQ(one[0], 28u);
	one[0] = 0xaau;
	CHECK(!types->entry(0u, 0u, NULL, 1u));
	CHECK(!types->entry(0u, 0u, one, 0u));
	CHECK(!types->entry(0u, 13u, one, sizeof(one)));
	CHECK_EQ(one[0], 0xaau);

	CHECK(!groups->entry(0u, 0u, two, sizeof(two)));
	CHECK_EQ(two[0], 0xaau);
}

/*
 * The voice tables known by a leading index (G.988 9.9.3, 9.9.6): a row is
 * added, or replaces the row of its index, and a row of index 0 clears the
 * table, which takes no new index once full. A dial plan (9.9.10) is known
 * by its number, and a row whose action is 0 removes it.
 */
static void test_the_voice_tables(void)
{
	struct omci_t_fixture fx;
	uint8_t row[32] = { 0 }, flat[40 * 20];

	fixture(&fx);

	row[0] = 1;
	row[1] = 1;
	CHECK_EQ(set_row(fx.eng, 58u, 10u, row, 20u), R_OK);
	row[0] = 2;
	CHECK_EQ(set_row(fx.eng, 58u, 10u, row, 20u), R_OK);
	row[0] = 1;
	row[1] = 0;
	CHECK_EQ(set_row(fx.eng, 58u, 10u, row, 20u), R_OK);
	CHECK_EQ(walk(fx.eng, 58u, INSTANCE, 10u, flat, sizeof(flat)), 40);
	CHECK(flat[0] == 1u && flat[1] == 0u && flat[20] == 2u);
	row[0] = 0;
	CHECK_EQ(set_row(fx.eng, 58u, 10u, row, 20u), R_OK);
	CHECK_EQ(get_size(fx.eng, 58u, INSTANCE, 10u), 0);

	for (unsigned int k = 1; k <= 32u; k++) {
		row[0] = (uint8_t)k;
		CHECK_EQ(set_row(fx.eng, 58u, 12u, row, 5u), R_OK);
	}
	row[0] = 33;
	CHECK_EQ(set_row(fx.eng, 58u, 12u, row, 5u), R_DEVICE_BUSY);
	row[0] = 5;
	CHECK_EQ(set_row(fx.eng, 58u, 12u, row, 5u), R_OK);
	CHECK_EQ(get_size(fx.eng, 58u, INSTANCE, 12u), 32 * 5);

	row[0] = 0x01;
	row[1] = 0x94;
	CHECK_EQ(set_row(fx.eng, 150u, 12u, row, 5u), R_OK);
	CHECK_EQ(get_size(fx.eng, 150u, INSTANCE, 12u), 5);
	row[0] = 0;
	row[1] = 0;
	CHECK_EQ(set_row(fx.eng, 150u, 12u, row, 5u), R_OK);
	CHECK_EQ(get_size(fx.eng, 150u, INSTANCE, 12u), 0);

	memset(row, 0, sizeof(row));
	row[0] = 1;
	row[1] = 1;
	memcpy(&row[2], "0T", 2);
	CHECK_EQ(set_row(fx.eng, 145u, 6u, row, 30u), R_OK);
	row[0] = 2;
	CHECK_EQ(set_row(fx.eng, 145u, 6u, row, 30u), R_OK);
	CHECK_EQ(get_size(fx.eng, 145u, INSTANCE, 6u), 60);
	row[0] = 1;
	row[1] = 0;
	CHECK_EQ(set_row(fx.eng, 145u, 6u, row, 30u), R_OK);
	CHECK_EQ(set_row(fx.eng, 145u, 6u, row, 30u), R_OK);
	CHECK_EQ(walk(fx.eng, 145u, INSTANCE, 6u, flat, sizeof(flat)), 30);
	CHECK_EQ(flat[0], 2u);

	omci_t_down(&fx);
}

int main(int argc, char **argv)
{
	if (!tsv_open(argc, argv)) {
		printf("test_tables: no table to read\n");
		return 1;
	}

	RUN(test_every_table_is_stored_or_rendered);
	RUN(test_the_omci_entity_lists_what_the_daemon_holds);
	RUN(test_the_group_lists_are_empty);
	RUN(test_the_stored_tables_fold_rows_by_their_key);
	RUN(test_the_vlan_rule_table_walks);
	RUN(test_the_table_functions_refuse_what_cannot_be_asked);
	RUN(test_the_voice_tables);

	return OMCI_T_REPORT("test_tables");
}
