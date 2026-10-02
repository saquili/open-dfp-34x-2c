// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that sweep every attribute of every class with Get and Set. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/action.h"
#include "me/layout.h"
#include "me/table.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"
#include "model_tsv.h"

#define R_OK 0u
#define R_ATTR_FAILED 9u

#define SWEEP_INSTANCE 0x0abcu
#define CREATE_INSTANCE 0x0abdu

#define MT_CREATE 4u
#define MT_SET 8u
#define MT_GET 9u

#define GET_VALUE_BUDGET 25u

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

static const uint8_t *reply_for(uint16_t tci, unsigned int *avc_before)
{
	const uint8_t *rsp = NULL;
	unsigned int before = 0;

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++) {
		const uint8_t *f = omci_t_tx.frame[i];

		if ((f[2] & 0x20u) != 0u && (be16_at(f, 0) & 0x7FFFu) == tci) {
			rsp = f;
			break;
		}
		if ((f[2] & 0x1Fu) == 17u)
			before++;
	}
	if (avc_before != NULL)
		*avc_before = before;
	return rsp;
}

static const uint8_t *avc_frame(void)
{
	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x1Fu) == 17u)
			return omci_t_tx.frame[i];
	return NULL;
}

static const uint8_t *drive(struct omci_engine *eng, uint16_t tci, uint8_t mt,
			    uint16_t cls, uint16_t inst,
			    const uint8_t *contents, size_t n,
			    unsigned int *avc_before)
{
	struct omci_frame f = {
		.tci = tci,
		.priority = false,
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

	const uint8_t *rsp = reply_for(tci, avc_before);

	if (rsp != NULL) {
		CHECK_EQ(be16_at(rsp, 4), cls);
		CHECK_EQ(be16_at(rsp, 6), inst);
	}
	return rsp;
}

static void sweep_fixture(struct omci_t_fixture *fx)
{
	CHECK(omci_t_up(fx));

	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx->eng, wire), OMCI_OK);

	unsigned int made = 0;

	for (unsigned int c = 0; c < tsv_class_count; c++)
		if (omci_mib_create(fx->mib, tsv_classes[c].id,
				    SWEEP_INSTANCE) == OMCI_OK)
			made++;
	CHECK_EQ(made, tsv_class_count);
}

static bool lists(const struct tsv_class *c, uint8_t mt)
{
	return (c->actions & (1u << mt)) != 0u;
}

struct census {
	unsigned int get_defined, get_tables, get_undefined, no_get;
	unsigned int set_written, set_announceable, set_tables, no_set,
		set_refused;
	unsigned int creates, placed, no_create;
};

static struct census take_census(void)
{
	struct census k = { 0 };

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		if (lists(t, MT_GET)) {
			for (unsigned int n = 1; n <= 16u; n++) {
				if (n > t->attr_count)
					k.get_undefined++;
				else {
					k.get_defined++;
					k.get_tables +=
						t->attrs[n - 1].table ? 1u : 0u;
				}
			}
		} else {
			k.no_get++;
		}

		if (lists(t, MT_SET)) {
			for (unsigned int n = 1; n <= t->attr_count; n++) {
				const struct tsv_attr *a = &t->attrs[n - 1];

				if (!a->write)
					k.set_refused++;
				else if (a->table)
					k.set_tables++;
				else {
					k.set_written++;
					k.set_announceable += a->avc ? 1u : 0u;
				}
			}
		} else {
			k.no_set++;
		}

		if (lists(t, MT_CREATE)) {
			k.creates++;
			for (unsigned int n = 1; n <= t->attr_count; n++)
				k.placed += t->attrs[n - 1].create ? 1u : 0u;
		} else {
			k.no_create++;
		}
	}
	return k;
}

static uint16_t get_unsupported_of(const uint8_t *rsp)
{
	return be16_at(rsp, OMCI_FRAME_LEN - 12);
}
static uint16_t get_failed_of(const uint8_t *rsp)
{
	return be16_at(rsp, OMCI_FRAME_LEN - 10);
}
static uint16_t set_unsupported_of(const uint8_t *rsp)
{
	return be16_at(rsp, 9);
}
static uint16_t set_failed_of(const uint8_t *rsp)
{
	return be16_at(rsp, 11);
}

static void test_every_attribute_reads_back_what_the_mib_holds(void)
{
	struct omci_t_fixture fx;

	sweep_fixture(&fx);

	unsigned int defined = 0, values = 0, tables = 0, undefined = 0,
		     no_get = 0;
	const struct census need = take_census();

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		if (!lists(t, MT_GET)) {
			no_get++;
			continue;
		}

		for (unsigned int n = 1; n <= t->attr_count; n++) {
			const struct tsv_attr *a = &t->attrs[n - 1];
			uint8_t pat[64];

			if (a->table)
				continue;
			memset(pat, (int)(0x40u + n), a->size);
			CHECK_EQ(omci_mib_attr_set(fx.mib, t->id,
						   SWEEP_INSTANCE, (uint8_t)n,
						   pat, a->size),
				 OMCI_OK);
		}

		for (unsigned int n = 1; n <= 16u; n++) {
			const uint16_t bit = omci_attr_bit((uint8_t)n);
			const uint8_t body[2] = { (uint8_t)(bit >> 8),
						  (uint8_t)(bit & 0xffu) };
			const struct tsv_attr *a =
				n <= t->attr_count ? &t->attrs[n - 1] : NULL;

			const uint8_t *rsp = drive(fx.eng, next_tid(), MT_GET,
						   t->id, SWEEP_INSTANCE, body,
						   sizeof(body), NULL);
			CHECK(rsp != NULL);
			if (rsp == NULL)
				continue;
			const uint16_t got = be16_at(rsp, 9);

			if (a == NULL) {
				CHECK_EQ(rsp[8], R_ATTR_FAILED);
				CHECK_EQ(got, 0u);
				CHECK_EQ(get_unsupported_of(rsp), bit);
				CHECK_EQ(get_failed_of(rsp), 0u);
				undefined++;
				continue;
			}
			defined++;

			CHECK(a->read);
			CHECK(a->table || a->size <= GET_VALUE_BUDGET);
			CHECK_EQ(rsp[8], R_OK);
			if (got != bit)
				printf("    ME %u attr %u: mask %04x, asked %04x\n",
				       t->id, n, got, bit);
			CHECK_EQ(got, bit);

			if (a->table) {
				tables++;
				const uint32_t size =
					((uint32_t)rsp[11] << 24) |
					((uint32_t)rsp[12] << 16) |
					((uint32_t)rsp[13] << 8) | rsp[14];
				CHECK_EQ(size % a->size, 0u);
				continue;
			}

			uint8_t want[64];
			size_t got_len = 0;

			CHECK_EQ(omci_mib_attr_get(fx.mib, t->id,
						   SWEEP_INSTANCE, (uint8_t)n,
						   want, sizeof(want),
						   &got_len),
				 OMCI_OK);
			CHECK_EQ(got_len, (size_t)a->size);
			CHECK_MEM(&rsp[11], want, a->size);
			values++;
		}
	}

	printf("  axis 1: %u defined (%u value, %u table), %u undefined, %u classes list no Get\n",
	       defined, values, tables, undefined, no_get);

	CHECK_EQ(no_get, need.no_get);
	CHECK_EQ(defined, need.get_defined);
	CHECK_EQ(tables, need.get_tables);
	CHECK_EQ(undefined, need.get_undefined);
	CHECK_EQ(defined + undefined, (tsv_class_count - no_get) * 16u);
	CHECK_EQ(defined, 278u);
	CHECK_EQ(tables, 13u);
	CHECK_EQ(no_get, 0u);

	omci_t_down(&fx);
}

static void test_every_writable_attribute_is_written_and_not_announced(void)
{
	struct omci_t_fixture fx;

	sweep_fixture(&fx);

	unsigned int written = 0, withheld = 0, tables = 0, no_set = 0,
		     too_wide = 0;
	const struct census need = take_census();

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		if (!lists(t, MT_SET)) {
			no_set++;
			continue;
		}

		for (unsigned int n = 1; n <= t->attr_count; n++) {
			const struct tsv_attr *a = &t->attrs[n - 1];
			const uint16_t bit = omci_attr_bit((uint8_t)n);

			if (!a->write)
				continue;
			if (a->table) {
				tables++;
				continue;
			}
			if (2u + a->size > 32u) {
				too_wide++;
				continue;
			}

			uint8_t now[64];
			size_t len = 0;

			CHECK_EQ(omci_mib_attr_get(fx.mib, t->id,
						   SWEEP_INSTANCE, (uint8_t)n,
						   now, sizeof(now), &len),
				 OMCI_OK);

			uint8_t body[34];

			body[0] = (uint8_t)(bit >> 8);
			body[1] = (uint8_t)(bit & 0xffu);
			for (size_t k = 0; k < a->size; k++)
				body[2 + k] = (uint8_t)(now[k] ^ 0x5Au);

			unsigned int avc_before = 0;
			const uint8_t *rsp = drive(fx.eng, next_tid(), MT_SET,
						   t->id, SWEEP_INSTANCE, body,
						   2u + a->size, &avc_before);
			CHECK(rsp != NULL);
			if (rsp == NULL)
				continue;
			if (rsp[8] != R_OK)
				printf("    ME %u attr %u: Set result %u\n",
				       t->id, n, rsp[8]);
			CHECK_EQ(rsp[8], R_OK);

			uint8_t back[64];
			size_t bn = 0;

			CHECK_EQ(omci_mib_attr_get(fx.mib, t->id,
						   SWEEP_INSTANCE, (uint8_t)n,
						   back, sizeof(back), &bn),
				 OMCI_OK);
			CHECK_MEM(back, &body[2], a->size);
			written++;

			CHECK_EQ(avc_before, 0u);
			CHECK(avc_frame() == NULL);
			CHECK_EQ(omci_t_tx.n, 1u);
			withheld += a->avc ? 1u : 0u;
		}
	}

	printf("  axis 2: %u written (%u of them attributes that can be announced), %u tables, %u classes list no Set\n",
	       written, withheld, tables, no_set);
	CHECK_EQ(written, need.set_written);
	CHECK_EQ(withheld, need.set_announceable);
	CHECK_EQ(tables, need.set_tables);
	CHECK_EQ(no_set, need.no_set);
	CHECK_EQ(too_wide, 0u);

	omci_t_down(&fx);
}

static void
test_an_autonomous_write_that_changes_nothing_announces_nothing(void)
{
	struct omci_t_fixture fx;

	sweep_fixture(&fx);

	const uint8_t value = 0x77u;

	omci_t_tx_reset();
	CHECK_EQ(omci_mib_attr_set(fx.mib, 263u, SWEEP_INSTANCE, 8u, &value,
				   1u),
		 OMCI_OK);
	const unsigned int first = omci_t_tx.n;

	omci_t_tx_reset();
	CHECK_EQ(omci_mib_attr_set(fx.mib, 263u, SWEEP_INSTANCE, 8u, &value,
				   1u),
		 OMCI_OK);
	const unsigned int again = omci_t_tx.n;

	printf("  a changing write sends %u frames, the same write again sends %u\n",
	       first, again);
	CHECK_EQ(first, 1u);
	CHECK_EQ(again, 0u);

	const uint8_t body[3] = { 0x01u, 0x00u, 0x78u };
	(void)drive(fx.eng, next_tid(), MT_SET, 263u, SWEEP_INSTANCE, body,
		    sizeof(body), NULL);
	CHECK_EQ(omci_t_tx.n, 1u);

	omci_t_down(&fx);
}

static void test_every_read_only_attribute_refuses_and_says_which(void)
{
	struct omci_t_fixture fx;

	sweep_fixture(&fx);

	unsigned int refused = 0, no_set = 0;
	const struct census need = take_census();

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		if (!lists(t, MT_SET)) {
			no_set++;
			continue;
		}

		for (unsigned int n = 1; n <= t->attr_count; n++) {
			const struct tsv_attr *a = &t->attrs[n - 1];
			const uint16_t bit = omci_attr_bit((uint8_t)n);

			if (a->write)
				continue;

			uint8_t before[64] = { 0 };
			size_t bn = 0;

			if (!a->table)
				CHECK_EQ(omci_mib_attr_get(fx.mib, t->id,
							   SWEEP_INSTANCE,
							   (uint8_t)n, before,
							   sizeof(before), &bn),
					 OMCI_OK);

			uint8_t body[34] = { 0 };

			body[0] = (uint8_t)(bit >> 8);
			body[1] = (uint8_t)(bit & 0xffu);
			const size_t carry = a->size > 30u ? 30u : a->size;

			for (size_t k = 0; k < carry; k++)
				body[2 + k] = (uint8_t)(before[k] ^ 0xA5u);

			const uint8_t *rsp = drive(fx.eng, next_tid(), MT_SET,
						   t->id, SWEEP_INSTANCE, body,
						   2u + carry, NULL);
			CHECK(rsp != NULL);
			if (rsp == NULL)
				continue;

			if (rsp[8] != R_ATTR_FAILED)
				printf("    ME %u attr %u: read-only Set answered %u\n",
				       t->id, n, rsp[8]);
			CHECK_EQ(rsp[8], R_ATTR_FAILED);
			CHECK_EQ(set_unsupported_of(rsp), 0u);
			CHECK_EQ(set_failed_of(rsp), bit);
			refused++;

			if (!a->table) {
				uint8_t after[64];
				size_t an = 0;

				CHECK_EQ(omci_mib_attr_get(fx.mib, t->id,
							   SWEEP_INSTANCE,
							   (uint8_t)n, after,
							   sizeof(after), &an),
					 OMCI_OK);
				CHECK_MEM(after, before, a->size);
			}
		}
	}

	printf("  axis 3: %u refused with the failed mask, %u classes list no Set\n",
	       refused, no_set);
	CHECK_EQ(refused, need.set_refused);
	CHECK_EQ(no_set, need.no_set);

	omci_t_down(&fx);
}

static void test_set_by_create_values_land_where_the_table_says(void)
{
	struct omci_t_fixture fx;

	sweep_fixture(&fx);

	unsigned int creates = 0, placed = 0, no_create = 0;
	const struct census need = take_census();

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		if (!lists(t, MT_CREATE)) {
			no_create++;
			continue;
		}

		uint8_t body[OMCI_CONTENTS_LEN];

		for (size_t k = 0; k < sizeof(body); k++)
			body[k] = (uint8_t)(0x10u + k);

		unsigned int avc_before = 0;
		const uint8_t *rsp = drive(fx.eng, next_tid(), MT_CREATE, t->id,
					   CREATE_INSTANCE, body, sizeof(body),
					   &avc_before);
		CHECK(rsp != NULL);
		if (rsp == NULL)
			continue;
		CHECK_EQ(rsp[8], R_OK);
		creates++;

		CHECK_EQ(avc_before, 0u);
		CHECK(avc_frame() == NULL);
		CHECK_EQ(omci_t_tx.n, 1u);

		size_t at = 0;

		for (unsigned int n = 1; n <= t->attr_count; n++) {
			const struct tsv_attr *a = &t->attrs[n - 1];
			uint8_t got[64];
			size_t len = 0;

			if (!a->create)
				continue;
			CHECK(at + a->size <= OMCI_CONTENTS_LEN);
			CHECK_EQ(omci_mib_attr_get(fx.mib, t->id,
						   CREATE_INSTANCE, (uint8_t)n,
						   got, sizeof(got), &len),
				 OMCI_OK);
			CHECK_MEM(got, &body[at], a->size);
			at += a->size;
			placed++;
		}
	}

	printf("  axis 4: %u Creates, %u set-by-create values placed, %u classes list no Create\n",
	       creates, placed, no_create);
	CHECK_EQ(creates, need.creates);
	CHECK_EQ(placed, need.placed);
	CHECK_EQ(no_create, need.no_create);
	CHECK_EQ(creates, 15u);
	CHECK_EQ(no_create, 14u);

	omci_t_down(&fx);
}

static void test_the_census(void)
{
	unsigned int writable = 0, read_only = 0, set_by_create = 0, avc = 0,
		     tables = 0, attrs = 0;

	for (unsigned int c = 0; c < tsv_class_count; c++)
		for (unsigned int n = 1; n <= tsv_classes[c].attr_count; n++) {
			const struct tsv_attr *a = &tsv_classes[c].attrs[n - 1];

			attrs++;
			writable += a->write ? 1u : 0u;
			read_only += a->write ? 0u : 1u;
			set_by_create += a->create ? 1u : 0u;
			avc += a->avc ? 1u : 0u;
			tables += a->table ? 1u : 0u;
		}

	printf("  census: %u attributes: %u writable, %u read-only, %u set by create, %u announcing, %u tables\n",
	       attrs, writable, read_only, set_by_create, avc, tables);
	CHECK_EQ(attrs, 278u);
	CHECK_EQ(writable + read_only, attrs);
	CHECK_EQ(tables, 13u);
}

int main(int argc, char **argv)
{
	if (!tsv_open(argc, argv)) {
		printf("test_attr_sweep: no table to read\n");
		return 1;
	}

	RUN(test_the_census);
	RUN(test_every_attribute_reads_back_what_the_mib_holds);
	RUN(test_every_writable_attribute_is_written_and_not_announced);
	RUN(test_an_autonomous_write_that_changes_nothing_announces_nothing);
	RUN(test_every_read_only_attribute_refuses_and_says_which);
	RUN(test_set_by_create_values_land_where_the_table_says);

	return OMCI_T_REPORT("attr-sweep");
}
