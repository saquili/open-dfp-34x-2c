// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that what a Create gives, a Get reads back. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/table.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "me/values.h"

#include "harness.h"
#include "model_tsv.h"

#define MT_CREATE 4u
#define MT_DELETE 6u
#define MT_SET 8u
#define MT_GET 9u

#define R_OK 0u
#define R_NOT_SUPPORTED 2u
#define R_UNKNOWN_INST 5u
#define R_INSTANCE_EXISTS 7u
#define R_ATTR_FAILED 9u

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

static void pattern(uint8_t *dst, const struct tsv_class *t, unsigned int n,
		    unsigned int salt)
{
	const struct tsv_attr *a = &t->attrs[n - 1];

	for (unsigned int k = 0; k < a->size; k++)
		dst[k] = (uint8_t)(1u + ((t->id * 31u + n * 17u + k * 3u +
					  salt * 59u) %
					 200u));
	/* An attribute with a range takes the next value it allows. */
	for (unsigned int step = 1;
	     step < 256u && !omci_me_value_allowed(t->id, (uint8_t)n, dst);
	     step++)
		dst[0] = (uint8_t)(dst[0] + 1u);
}

static size_t create_payload(const struct tsv_class *t, unsigned int salt,
			     uint8_t *out)
{
	size_t at = 0;

	for (unsigned int n = 1; n <= t->attr_count; n++)
		if (t->attrs[n - 1].create) {
			pattern(&out[at], t, n, salt);
			at += t->attrs[n - 1].size;
		}
	return at;
}

static void expect_attr(struct omci_engine *eng, const struct tsv_class *t,
			uint16_t inst, unsigned int n, const uint8_t *want,
			const char *when)
{
	const uint16_t bit = omci_attr_bit((uint8_t)n);
	const uint8_t body[2] = { (uint8_t)(bit >> 8), (uint8_t)(bit & 0xffu) };
	const uint8_t *rsp =
		drive(eng, MT_GET, t->id, inst, body, sizeof(body));

	CHECK(rsp != NULL);
	if (rsp == NULL)
		return;
	if (rsp[8] != R_OK)
		printf("    ME %u attr %u (%s): Get answered %u\n", t->id, n,
		       when, rsp[8]);
	CHECK_EQ(rsp[8], R_OK);
	CHECK_EQ(be16_at(rsp, 9), bit);
	if (memcmp(&rsp[11], want, t->attrs[n - 1].size) != 0)
		printf("    ME %u attr %u (%s): the value read is not the value written\n",
		       t->id, n, when);
	CHECK_MEM(&rsp[11], want, t->attrs[n - 1].size);
}

static bool performance_monitoring(const struct tsv_class *t)
{
	return t->id == 321u || t->id == 322u;
}

static void round_trip(struct omci_engine *eng, const struct tsv_class *t)
{
	uint8_t payload[OMCI_CONTENTS_LEN];
	const size_t len = create_payload(t, 1u, payload);
	const uint8_t zero[64] = { 0 };

	const uint8_t *rsp =
		drive(eng, MT_CREATE, t->id, INSTANCE, payload, len);
	CHECK(rsp != NULL);
	if (rsp == NULL)
		return;
	if (rsp[8] != R_OK)
		printf("    ME %u: Create answered %u\n", t->id, rsp[8]);
	CHECK_EQ(rsp[8], R_OK);
	rsp = drive(eng, MT_CREATE, t->id, INSTANCE, payload, len);
	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ(rsp[8], R_INSTANCE_EXISTS);

	for (unsigned int n = 1; n <= t->attr_count; n++) {
		const struct tsv_attr *a = &t->attrs[n - 1];
		uint8_t want[64];

		if (a->table || (performance_monitoring(t) && !a->create))
			continue;
		if (a->create)
			pattern(want, t, n, 1u);
		else
			memcpy(want, zero, sizeof(want));
		expect_attr(eng, t, INSTANCE, n, want, "created");
	}

	uint8_t body[OMCI_CONTENTS_LEN];
	uint16_t mask = 0;
	size_t at = 2;

	memset(body, 0, sizeof(body));
	for (unsigned int n = 1; n <= t->attr_count; n++) {
		const struct tsv_attr *a = &t->attrs[n - 1];

		if (!a->write || a->table)
			continue;
		if (at + a->size > OMCI_CONTENTS_LEN)
			break;
		mask = (uint16_t)(mask | omci_attr_bit((uint8_t)n));
		pattern(&body[at], t, n, 2u);
		at += a->size;
	}
	body[0] = (uint8_t)(mask >> 8);
	body[1] = (uint8_t)(mask & 0xffu);
	if (mask != 0u) {
		rsp = drive(eng, MT_SET, t->id, INSTANCE, body, at);
		CHECK(rsp != NULL);
		if (rsp != NULL) {
			if (rsp[8] != R_OK)
				printf("    ME %u: Set of %04x answered %u\n",
				       t->id, mask, rsp[8]);
			CHECK_EQ(rsp[8], R_OK);
		}
	}
	for (unsigned int n = 1; n <= t->attr_count; n++) {
		const struct tsv_attr *a = &t->attrs[n - 1];
		uint8_t want[64];

		if (a->table || !a->write)
			continue;
		if ((mask & omci_attr_bit((uint8_t)n)) == 0u) {
			uint8_t one[34] = { 0 };
			const uint16_t bit = omci_attr_bit((uint8_t)n);

			one[0] = (uint8_t)(bit >> 8);
			one[1] = (uint8_t)(bit & 0xffu);
			pattern(&one[2], t, n, 2u);
			rsp = drive(eng, MT_SET, t->id, INSTANCE, one,
				    2u + a->size);
			CHECK(rsp != NULL);
			if (rsp != NULL)
				CHECK_EQ(rsp[8], R_OK);
		}
		pattern(want, t, n, 2u);
		expect_attr(eng, t, INSTANCE, n, want, "set");
	}

	for (unsigned int n = 1; n <= t->attr_count; n++) {
		const struct tsv_attr *a = &t->attrs[n - 1];
		uint8_t refuse[34] = { 0 };
		uint8_t want[64];

		if (a->write || a->table || performance_monitoring(t))
			continue;
		const uint16_t bit = omci_attr_bit((uint8_t)n);

		refuse[0] = (uint8_t)(bit >> 8);
		refuse[1] = (uint8_t)(bit & 0xffu);
		memset(&refuse[2], 0xEE, a->size);
		rsp = drive(eng, MT_SET, t->id, INSTANCE, refuse, 2u + a->size);
		CHECK(rsp != NULL);
		if (rsp != NULL) {
			CHECK_EQ(rsp[8], R_ATTR_FAILED);
			CHECK_EQ(be16_at(rsp, 11), bit);
		}
		if (a->create)
			pattern(want, t, n, 1u);
		else
			memcpy(want, zero, sizeof(want));
		expect_attr(eng, t, INSTANCE, n, want, "refused");
		break;
	}

	rsp = drive(eng, MT_DELETE, t->id, INSTANCE, NULL, 0);
	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ(rsp[8], R_OK);

	{
		const uint16_t bit = omci_attr_bit(1u);
		const uint8_t one[2] = { (uint8_t)(bit >> 8),
					 (uint8_t)(bit & 0xffu) };
		rsp = drive(eng, MT_GET, t->id, INSTANCE, one, sizeof(one));
		CHECK(rsp != NULL);
		if (rsp != NULL)
			CHECK_EQ(rsp[8], R_UNKNOWN_INST);
	}
	rsp = drive(eng, MT_DELETE, t->id, INSTANCE, NULL, 0);
	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ(rsp[8], R_UNKNOWN_INST);

	uint8_t again[OMCI_CONTENTS_LEN];
	const size_t again_len = create_payload(t, 3u, again);

	rsp = drive(eng, MT_CREATE, t->id, INSTANCE, again, again_len);
	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ(rsp[8], R_OK);
	for (unsigned int n = 1; n <= t->attr_count; n++) {
		const struct tsv_attr *a = &t->attrs[n - 1];
		uint8_t want[64];

		if (a->table || (performance_monitoring(t) && !a->create))
			continue;
		if (a->create)
			pattern(want, t, n, 3u);
		else
			memcpy(want, zero, sizeof(want));
		expect_attr(eng, t, INSTANCE, n, want, "recreated");
	}
	rsp = drive(eng, MT_DELETE, t->id, INSTANCE, NULL, 0);
	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ(rsp[8], R_OK);
}

static void test_every_class_that_lists_create_round_trips(void)
{
	struct omci_t_fixture fx;
	unsigned int classes = 0;

	CHECK(omci_t_up(&fx));
	uint8_t reset[OMCI_FRAME_LEN];

	omci_t_build(reset, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx.eng, reset), OMCI_OK);

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		if ((t->actions & (1u << MT_CREATE)) == 0u)
			continue;
		round_trip(fx.eng, t);
		classes++;
	}
	printf("  %u classes round-tripped\n", classes);
	CHECK_EQ(classes, 16u);

	omci_t_down(&fx);
}

static void test_the_classes_the_onu_creates_refuse_create_and_delete(void)
{
	struct omci_t_fixture fx;
	unsigned int classes = 0;

	CHECK(omci_t_up(&fx));
	uint8_t reset[OMCI_FRAME_LEN];

	omci_t_build(reset, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx.eng, reset), OMCI_OK);

	const size_t before = omci_mib_count(fx.mib);

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		if ((t->actions & (1u << MT_CREATE)) != 0u)
			continue;
		classes++;
		uint8_t payload[OMCI_CONTENTS_LEN];
		const size_t len = create_payload(t, 1u, payload);
		const uint8_t *rsp =
			drive(fx.eng, MT_CREATE, t->id, INSTANCE, payload, len);

		CHECK(rsp != NULL);
		if (rsp != NULL)
			CHECK_EQ(rsp[8], R_NOT_SUPPORTED);
		CHECK(!omci_mib_exists(fx.mib, t->id, INSTANCE));

		rsp = drive(fx.eng, MT_DELETE, t->id, 0u, NULL, 0);
		CHECK(rsp != NULL);
		if (rsp != NULL)
			CHECK_EQ(rsp[8], R_NOT_SUPPORTED);
	}
	CHECK_EQ(classes, 14u);
	CHECK_EQ(omci_mib_count(fx.mib), before);

	omci_t_down(&fx);
}

int main(int argc, char **argv)
{
	if (!tsv_open(argc, argv)) {
		printf("test_roundtrip: no table to read\n");
		return 1;
	}

	RUN(test_every_class_that_lists_create_round_trips);
	RUN(test_the_classes_the_onu_creates_refuse_create_and_delete);

	return OMCI_T_REPORT("test_roundtrip");
}
