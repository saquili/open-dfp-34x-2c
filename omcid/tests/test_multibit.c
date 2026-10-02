// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of requests that name several attributes at once. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/table.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "me/tables.h"

#include "harness.h"
#include "omcid/array_size.h"

static bool is_table_attr(uint16_t class_id, uint8_t attr)
{
	return omci_me_attr_is_table(
		omci_me_attr_get(omci_me_class_by_id(class_id), attr));
}

#define R_OK 0u
#define R_PARAM_ERROR 3u
#define R_ATTR_FAILED 9u

#define MT_SET 8u
#define MT_GET 9u

#define GET_VALUE_BUDGET 25u

#define GET_UNSUPPORTED_AT (OMCI_FRAME_LEN - 12u)
#define GET_FAILED_AT (OMCI_FRAME_LEN - 10u)
#define SET_UNSUPPORTED_AT 9u
#define SET_FAILED_AT 11u

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

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x20u) != 0u &&
		    (be16_at(omci_t_tx.frame[i], 0) & 0x7FFFu) == tci)
			return omci_t_tx.frame[i];
	return NULL;
}

static void get_mask(struct omci_engine *eng, uint16_t cls, uint16_t mask,
		     const uint8_t **out)
{
	const uint8_t body[2] = { (uint8_t)(mask >> 8),
				  (uint8_t)(mask & 0xffu) };
	*out = drive(eng, MT_GET, cls, INSTANCE, body, sizeof(body));
}

static void fixture(struct omci_t_fixture *fx)
{
	CHECK(omci_t_up(fx));
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx->eng, wire), OMCI_OK);
	for (size_t c = 0; c < omci_me_class_count; c++)
		(void)!omci_mib_create(fx->mib, omci_me_classes[c].class_id,
				       INSTANCE);
}

static void test_an_overflowing_get_stops_rather_than_shifting_the_rest(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	const uint16_t cls = 5u;
	static const struct {
		uint8_t attr, pat;
	} seed[] = {
		{ 4u, 0xd7u },
		{ 5u, 0xd8u },
		{ 6u, 0xd9u },
	};
	const struct omci_me_class *c = omci_me_class_by_id(cls);

	CHECK(c != NULL);

	for (size_t i = 0; i < ARRAY_SIZE(seed); i++) {
		const struct omci_me_attr *a =
			omci_me_attr_get(c, seed[i].attr);
		CHECK(a != NULL);
		if (a == NULL)
			continue;
		uint8_t pat[64];

		memset(pat, seed[i].pat, sizeof(pat));
		CHECK_EQ(omci_mib_attr_set(fx.mib, cls, INSTANCE, seed[i].attr,
					   pat, a->size),
			 OMCI_OK);
	}

	const uint8_t *rsp = NULL;

	get_mask(fx.eng, cls, 0x1C00u, &rsp);
	CHECK(rsp != NULL);
	if (rsp == NULL) {
		omci_t_down(&fx);
		return;
	}

	CHECK_EQ(rsp[8], R_ATTR_FAILED);
	CHECK_EQ(be16_at(rsp, 9), 0x1000u);
	CHECK_EQ(be16_at(rsp, GET_UNSUPPORTED_AT), 0u);
	CHECK_EQ(be16_at(rsp, GET_FAILED_AT), 0x0C00u);

	for (unsigned int k = 0; k < 20u; k++)
		CHECK_EQ(rsp[11 + k], 0xd7u);

	unsigned int strays = 0;

	for (unsigned int k = 11u; k < 11u + GET_VALUE_BUDGET; k++)
		if (rsp[k] == 0xd9u || rsp[k] == 0xd8u)
			strays++;
	if (strays != 0)
		printf("    %u byte(s) of a dropped attribute appear in the value area\n",
		       strays);
	CHECK_EQ(strays, 0u);

	omci_t_down(&fx);
}

static void
test_the_response_carries_twenty_five_bytes_of_values_not_twenty_nine(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	unsigned int discriminating = 0;

	for (size_t ci = 0; ci < omci_me_class_count; ci++) {
		const struct omci_me_class *c = &omci_me_classes[ci];

		size_t cum = 0;
		uint16_t want = 0, expect = 0;
		bool straddles = false;

		for (uint8_t attr = 1u; attr <= 16u; attr++) {
			const uint16_t bit = omci_attr_bit(attr);

			if (bit == 0u)
				continue;
			const struct omci_me_attr *a =
				omci_me_attr_get(c, attr);
			if (a == NULL || a->size == 0u)
				continue;

			const size_t cost =
				is_table_attr(c->class_id, attr) ? 4u : a->size;

			want |= bit;

			if ((a->access & OMCI_ME_ACC_READ) == 0u)
				continue;

			if (cum + cost <= GET_VALUE_BUDGET) {
				cum += cost;
				expect |= bit;
				continue;
			}

			straddles = (cum + cost) <= 29u;
			break;
		}

		if (!straddles)
			continue;
		discriminating++;

		const uint8_t *rsp = NULL;

		get_mask(fx.eng, c->class_id, want, &rsp);
		CHECK(rsp != NULL);
		if (rsp == NULL)
			continue;

		if (be16_at(rsp, 9) != expect)
			printf("    ME %u: mask %04x, want %04x (a 29-byte budget would give more)\n",
			       c->class_id, be16_at(rsp, 9), expect);
		CHECK_EQ(be16_at(rsp, 9), expect);
		CHECK_EQ(rsp[8], R_ATTR_FAILED);
	}

	printf("  %u class(es) straddle the 25/29 boundary and can tell the two apart\n",
	       discriminating);

	CHECK(discriminating > 0u);

	omci_t_down(&fx);
}

static void test_the_mask_and_the_bytes_always_agree(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	unsigned int checked = 0, truncated = 0;

	for (size_t ci = 0; ci < omci_me_class_count; ci++) {
		const struct omci_me_class *c = &omci_me_classes[ci];

		const uint8_t *rsp = NULL;

		get_mask(fx.eng, c->class_id, 0xFFFFu, &rsp);
		CHECK(rsp != NULL);
		if (rsp == NULL)
			continue;
		checked++;

		const uint16_t got = be16_at(rsp, 9);
		size_t claimed = 0;

		for (uint8_t attr = 1u; attr <= 16u; attr++) {
			const uint16_t bit = omci_attr_bit(attr);

			if (bit == 0u || (got & bit) == 0u)
				continue;
			const struct omci_me_attr *a =
				omci_me_attr_get(c, attr);
			CHECK(a != NULL);
			if (a == NULL)
				continue;

			claimed += is_table_attr(c->class_id, attr) ? 4u :
								      a->size;
		}

		if (claimed > GET_VALUE_BUDGET)
			printf("    ME %u: mask %04x claims %zu bytes, budget is %u\n",
			       c->class_id, got, claimed, GET_VALUE_BUDGET);
		CHECK(claimed <= GET_VALUE_BUDGET);

		if (rsp[8] == R_ATTR_FAILED)
			truncated++;

		const uint16_t uns = be16_at(rsp, GET_UNSUPPORTED_AT);
		const uint16_t fai = be16_at(rsp, GET_FAILED_AT);

		CHECK_EQ(rsp[8] == R_ATTR_FAILED, (uns | fai) != 0u);

		CHECK_EQ(got & uns, 0u);
		CHECK_EQ(got & fai, 0u);
		CHECK_EQ(uns & fai, 0u);
	}

	printf("  %u classes checked, %u answered with a truncation\n", checked,
	       truncated);
	CHECK_EQ(checked, omci_me_class_count);
	CHECK(truncated > 0u);

	omci_t_down(&fx);
}

static void test_a_set_that_fails_two_ways_names_both(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	const struct omci_me_class *c = omci_me_class_by_id(6u);

	CHECK(c != NULL);
	if (c == NULL) {
		omci_t_down(&fx);
		return;
	}

	const struct omci_me_attr *ro = omci_me_attr_get(c, 1u);

	CHECK(ro != NULL);
	CHECK(ro != NULL && (ro->access & OMCI_ME_ACC_WRITE) == 0u);
	CHECK(omci_me_attr_get(c, 15u) == NULL);

	uint8_t body[OMCI_CONTENTS_LEN];

	memset(body, 0x5Au, sizeof(body));
	const uint16_t mask =
		(uint16_t)(omci_attr_bit(1u) | omci_attr_bit(15u));
	body[0] = (uint8_t)(mask >> 8);
	body[1] = (uint8_t)(mask & 0xffu);

	const uint8_t *rsp =
		drive(fx.eng, MT_SET, 6u, INSTANCE, body, sizeof(body));
	CHECK(rsp != NULL);
	if (rsp == NULL) {
		omci_t_down(&fx);
		return;
	}

	CHECK_EQ(rsp[8], R_ATTR_FAILED);
	CHECK_EQ(be16_at(rsp, SET_UNSUPPORTED_AT), omci_attr_bit(15u));
	CHECK_EQ(be16_at(rsp, SET_FAILED_AT), omci_attr_bit(1u));

	omci_t_down(&fx);
}

static void test_a_partly_failing_set_applies_the_part_that_works(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	for (size_t ci = 0; ci < omci_me_class_count; ci++) {
		const struct omci_me_class *c = &omci_me_classes[ci];
		uint8_t w = 0, r = 0;

		for (uint8_t attr = 1u; attr <= 16u && (w == 0u || r == 0u);
		     attr++) {
			const struct omci_me_attr *a =
				omci_me_attr_get(c, attr);
			if (a == NULL || a->size == 0u || a->size > 4u ||
			    omci_attr_bit(attr) == 0u)
				continue;
			if (is_table_attr(c->class_id, attr))
				continue;
			if ((a->access & OMCI_ME_ACC_WRITE) != 0u) {
				if (w == 0u)
					w = attr;
			} else {
				if (r == 0u)
					r = attr;
			}
		}
		if (w == 0u || r == 0u || w > r)
			continue;

		const struct omci_me_attr *wa = omci_me_attr_get(c, w);
		uint8_t before[8];
		size_t n = 0;

		CHECK_EQ(omci_mib_attr_get(fx.mib, c->class_id, INSTANCE, w,
					   before, sizeof(before), &n),
			 OMCI_OK);

		uint8_t body[OMCI_CONTENTS_LEN];

		memset(body, 0, sizeof(body));
		const uint16_t mask =
			(uint16_t)(omci_attr_bit(w) | omci_attr_bit(r));
		body[0] = (uint8_t)(mask >> 8);
		body[1] = (uint8_t)(mask & 0xffu);
		for (size_t k = 0; k < wa->size; k++)
			body[2 + k] = (uint8_t)(before[k] ^ 0x3Cu);

		const uint8_t *rsp = drive(fx.eng, MT_SET, c->class_id,
					   INSTANCE, body, sizeof(body));
		CHECK(rsp != NULL);
		if (rsp == NULL) {
			omci_t_down(&fx);
			return;
		}

		CHECK_EQ(rsp[8], R_ATTR_FAILED);
		CHECK_EQ(be16_at(rsp, SET_FAILED_AT), omci_attr_bit(r));
		CHECK_EQ(be16_at(rsp, SET_UNSUPPORTED_AT), 0u);

		uint8_t after[8];
		size_t an = 0;

		CHECK_EQ(omci_mib_attr_get(fx.mib, c->class_id, INSTANCE, w,
					   after, sizeof(after), &an),
			 OMCI_OK);
		CHECK_MEM(after, &body[2], wa->size);

		printf("  ME %u: attribute %u written, attribute %u refused, in one Set\n",
		       c->class_id, w, r);
		omci_t_down(&fx);
		return;
	}

	printf("  no class offers a writable and a read-only attribute in that order\n");
	CHECK(false);
	omci_t_down(&fx);
}

static void test_an_undefined_attribute_stops_the_set(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	const struct omci_me_class *c = omci_me_class_by_id(6u);

	CHECK(c != NULL);
	if (c == NULL) {
		omci_t_down(&fx);
		return;
	}

	uint8_t snapshot[64][8];
	size_t lens[64];

	memset(lens, 0, sizeof(lens));
	for (uint8_t d = 1u; d <= c->attr_count && d < 64u; d++) {
		const struct omci_me_attr *a = omci_me_attr_get(c, d);

		if (a == NULL || a->size > 8u)
			continue;
		(void)!omci_mib_attr_get(fx.mib, 6u, INSTANCE, d, snapshot[d],
					 8u, &lens[d]);
	}

	uint8_t body[OMCI_CONTENTS_LEN];

	memset(body, 0xC3u, sizeof(body));
	const uint16_t mask =
		(uint16_t)(omci_attr_bit(15u) | omci_attr_bit(16u));
	body[0] = (uint8_t)(mask >> 8);
	body[1] = (uint8_t)(mask & 0xffu);

	const uint8_t *rsp =
		drive(fx.eng, MT_SET, 6u, INSTANCE, body, sizeof(body));
	CHECK(rsp != NULL);
	if (rsp == NULL) {
		omci_t_down(&fx);
		return;
	}

	CHECK_EQ(rsp[8], R_ATTR_FAILED);
	CHECK_EQ(be16_at(rsp, SET_UNSUPPORTED_AT), mask);
	CHECK_EQ(be16_at(rsp, SET_FAILED_AT), 0u);

	for (uint8_t d = 1u; d <= c->attr_count && d < 64u; d++) {
		if (lens[d] == 0)
			continue;
		uint8_t now[8];
		size_t n = 0;

		CHECK_EQ(omci_mib_attr_get(fx.mib, 6u, INSTANCE, d, now,
					   sizeof(now), &n),
			 OMCI_OK);
		CHECK_MEM(now, snapshot[d], lens[d]);
	}

	omci_t_down(&fx);
}

static void test_the_masks_appear_only_with_result_nine(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	unsigned int sets = 0, gets = 0;

	for (size_t ci = 0; ci < omci_me_class_count; ci++) {
		const struct omci_me_class *c = &omci_me_classes[ci];

		const uint8_t *g = NULL;

		get_mask(fx.eng, c->class_id, 0xFFFFu, &g);
		if (g != NULL) {
			gets++;
			if (g[8] != R_ATTR_FAILED) {
				CHECK_EQ(be16_at(g, GET_UNSUPPORTED_AT), 0u);
				CHECK_EQ(be16_at(g, GET_FAILED_AT), 0u);
			}
		}

		uint8_t body[OMCI_CONTENTS_LEN];

		memset(body, 0, sizeof(body));
		body[0] = 0xFFu;
		body[1] = 0xFFu;
		const uint8_t *s = drive(fx.eng, MT_SET, c->class_id, INSTANCE,
					 body, sizeof(body));
		if (s != NULL) {
			sets++;
			if (s[8] != R_ATTR_FAILED) {
				CHECK_EQ(be16_at(s, SET_UNSUPPORTED_AT), 0u);
				CHECK_EQ(be16_at(s, SET_FAILED_AT), 0u);
			}
		}
	}

	printf("  %u Gets and %u Sets checked for stray masks\n", gets, sets);
	CHECK_EQ(gets, omci_me_class_count);
	CHECK(sets > 0u);

	omci_t_down(&fx);
}

static void test_a_declined_value_does_not_drop_the_attributes_after_it(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	const uint16_t cls = 171u;
	const struct omci_me_attr *ptr =
		omci_me_attr_get(omci_me_class_by_id(cls), 7u);
	CHECK(ptr != NULL);
	if (ptr == NULL) {
		omci_t_down(&fx);
		return;
	}
	CHECK_EQ(ptr->size, 2u);

	uint8_t body[OMCI_CONTENTS_LEN];

	memset(body, 0, sizeof(body));
	const uint16_t mask = (uint16_t)(omci_attr_bit(6u) | omci_attr_bit(7u));

	body[0] = (uint8_t)(mask >> 8);
	body[1] = (uint8_t)(mask & 0xffu);

	body[2 + 4] = 0x02u;
	body[2 + 16] = 0xBEu;
	body[2 + 17] = 0xEFu;

	const uint8_t *rsp =
		drive(fx.eng, MT_SET, cls, INSTANCE, body, sizeof(body));
	CHECK(rsp != NULL);
	if (rsp == NULL) {
		omci_t_down(&fx);
		return;
	}

	CHECK_EQ(rsp[8], R_PARAM_ERROR);

	uint8_t got[4];
	size_t n = 0;

	CHECK_EQ(omci_mib_attr_get(fx.mib, cls, INSTANCE, 7u, got, sizeof(got),
				   &n),
		 OMCI_OK);
	if (n < 2u || got[0] != 0xBEu || got[1] != 0xEFu)
		printf("    attribute 7 reads %02x%02x after a declined attribute 6 — the write was lost\n",
		       n > 0 ? got[0] : 0, n > 1 ? got[1] : 0);
	CHECK_EQ(n, 2u);
	CHECK_EQ(got[0], 0xBEu);
	CHECK_EQ(got[1], 0xEFu);

	omci_t_down(&fx);
}

static void test_an_overlong_set_still_names_the_access_violation(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	for (size_t ci = 0; ci < omci_me_class_count; ci++) {
		const struct omci_me_class *c = &omci_me_classes[ci];
		uint8_t ro = 0;
		uint16_t mask = 0;
		size_t cum = 0;
		bool overflows = false;

		for (uint8_t attr = 1u; attr <= 16u; attr++) {
			const uint16_t bit = omci_attr_bit(attr);

			if (bit == 0u)
				continue;
			const struct omci_me_attr *a =
				omci_me_attr_get(c, attr);
			if (a == NULL || a->size == 0u)
				continue;
			if (ro == 0u && (a->access & OMCI_ME_ACC_WRITE) == 0u) {
				ro = attr;
				mask |= bit;
				cum += a->size;
				continue;
			}
			if (ro == 0u)
				continue;
			mask |= bit;
			cum += a->size;
			if (2u + cum > OMCI_CONTENTS_LEN) {
				overflows = true;
				break;
			}
		}
		if (ro == 0u || !overflows)
			continue;

		uint8_t body[OMCI_CONTENTS_LEN];

		memset(body, 0x77u, sizeof(body));
		body[0] = (uint8_t)(mask >> 8);
		body[1] = (uint8_t)(mask & 0xffu);

		const uint8_t *rsp = drive(fx.eng, MT_SET, c->class_id,
					   INSTANCE, body, sizeof(body));
		CHECK(rsp != NULL);
		if (rsp == NULL) {
			omci_t_down(&fx);
			return;
		}

		CHECK_EQ(rsp[8], R_ATTR_FAILED);
		const uint16_t exec = be16_at(rsp, SET_FAILED_AT);

		if ((exec & omci_attr_bit(ro)) == 0u)
			printf("    ME %u: the read-only attribute %u vanished from the mask (%04x)\n",
			       c->class_id, ro, exec);
		CHECK((exec & omci_attr_bit(ro)) != 0u);

		printf("  ME %u: an over-long Set still names attribute %u as refused\n",
		       c->class_id, ro);
		omci_t_down(&fx);
		return;
	}

	printf("  no class offers a read-only attribute followed by an overflowing mask\n");
	CHECK(false);
	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_an_overflowing_get_stops_rather_than_shifting_the_rest);
	RUN(test_the_response_carries_twenty_five_bytes_of_values_not_twenty_nine);
	RUN(test_the_mask_and_the_bytes_always_agree);
	RUN(test_a_set_that_fails_two_ways_names_both);
	RUN(test_a_partly_failing_set_applies_the_part_that_works);
	RUN(test_an_undefined_attribute_stops_the_set);
	RUN(test_the_masks_appear_only_with_result_nine);
	RUN(test_a_declined_value_does_not_drop_the_attributes_after_it);
	RUN(test_an_overlong_set_still_names_the_access_violation);

	return OMCI_T_REPORT("multibit");
}
