// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of reading table attributes with Get and Get next. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/table.h"
#include "mib/store.h"
#include "me/tables.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

#define ME171 171u
#define RULE_ATTR 6u
#define RULE_DESC 6u
#define RULE_BYTES 16u
#define INST 0x0102u

#define ME287 287u
#define ME287_ATTR 1u
#define ME287_DESC 1u

#define MT_GET 9u
#define MT_GET_NEXT 26u
#define MT_SET 8u

#define RES_DONE 0u
#define R_PARAM_ERROR 3u

#define SEGMENT 29u

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
			    uint16_t inst, bool priority,
			    const uint8_t *contents, size_t n)
{
	const uint16_t tci = next_tid();
	struct omci_frame f = {
		.tci = tci,
		.priority = priority,
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

static const uint8_t *get_table(struct omci_engine *eng, uint16_t cls,
				uint16_t inst, uint8_t attr, bool priority)
{
	const uint16_t bit = omci_attr_bit(attr);
	const uint8_t body[2] = { (uint8_t)(bit >> 8), (uint8_t)(bit & 0xffu) };

	return drive(eng, MT_GET, cls, inst, priority, body, sizeof(body));
}

static const uint8_t *get_next(struct omci_engine *eng, uint16_t cls,
			       uint16_t inst, uint8_t attr, uint16_t seq,
			       bool priority)
{
	const uint16_t bit = omci_attr_bit(attr);
	const uint8_t body[4] = {
		(uint8_t)(bit >> 8),
		(uint8_t)(bit & 0xffu),
		(uint8_t)(seq >> 8),
		(uint8_t)(seq & 0xffu),
	};
	return drive(eng, MT_GET_NEXT, cls, inst, priority, body, sizeof(body));
}

static void fill_rules(struct omci_mib *mib, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		uint8_t e[RULE_BYTES];

		memset(e, (int)(0x20u + (i & 0x3Fu)), sizeof(e));

		e[0] = (uint8_t)(i >> 8);
		e[1] = (uint8_t)(i & 0xffu);
		e[2] = 0x5Au;
		e[3] = (uint8_t)(0xC0u + (i & 0x1Fu));
		CHECK_EQ(omci_mib_table_put(mib, ME171, INST, RULE_DESC, e,
					    sizeof(e), 4u, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
	}
	CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), n);
}

static void fixture(struct omci_t_fixture *fx)
{
	CHECK(omci_t_up(fx));
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx->eng, wire), OMCI_OK);
	(void)!omci_mib_create(fx->mib, ME171, INST);
	(void)!omci_mib_create(fx->mib, ME287, INST);
}

static size_t walk(struct omci_engine *eng, uint32_t promised,
		   unsigned int segments, uint8_t *out, size_t cap,
		   bool priority)
{
	size_t at = 0;

	for (unsigned int s = 0; s < segments; s++) {
		const uint8_t *rsp = get_next(eng, ME171, INST, RULE_ATTR,
					      (uint16_t)s, priority);
		CHECK(rsp != NULL);
		if (rsp == NULL)
			return at;

		CHECK_EQ(rsp[8], RES_DONE);
		CHECK_EQ(be16_at(rsp, 9), omci_attr_bit(RULE_ATTR));

		const size_t want = (s + 1u == segments) ?
					    (size_t)(promised - (uint32_t)at) :
					    SEGMENT;
		CHECK(want <= SEGMENT);
		if (at + want > cap)
			return at;

		memcpy(&out[at], &rsp[11], want);
		at += want;
	}
	return at;
}

static void test_the_promised_size_is_what_the_walk_delivers(void)
{
	static const struct {
		unsigned int entries;
		unsigned int segments;
		const char *why;
	} k[] = {
		{ 1u, 1u, "one entry, well under a segment" },
		{ 2u, 2u, "32 bytes: one full segment and a 3-byte remainder" },
		{ 3u, 2u, "48 bytes: 29 + 19" },
		{ 29u, 16u,
		  "464 bytes = 16 EXACT segments — the last one full, no remainder" },
		{ 30u, 17u,
		  "480 bytes: 16 full segments and a 16-byte remainder" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(k); i++) {
		struct omci_t_fixture fx;

		fixture(&fx);
		fill_rules(fx.mib, k[i].entries);

		const uint8_t *g =
			get_table(fx.eng, ME171, INST, RULE_ATTR, false);
		CHECK(g != NULL);
		if (g == NULL) {
			omci_t_down(&fx);
			continue;
		}

		CHECK_EQ(g[8], RES_DONE);
		const uint32_t promised = be32_at(g, 11);
		const uint32_t expect = (uint32_t)(k[i].entries * RULE_BYTES);

		if (promised != expect)
			printf("    %u entries: promised %u bytes, want %u (%s)\n",
			       k[i].entries, promised, expect, k[i].why);
		CHECK_EQ(promised, expect);

		char log[512];

		omci_log_level_set(OMCI_LOG_DEBUG);
		const uint8_t *over = NULL;

		OMCI_T_CAPTURE_LOG(log, {
			over = get_next(fx.eng, ME171, INST, RULE_ATTR,
					(uint16_t)k[i].segments, false);
		});
		omci_log_level_set(OMCI_LOG_ERR);
		CHECK(over != NULL);
		if (over != NULL)
			CHECK_EQ(over[8], R_PARAM_ERROR);
		char says[96];
		(void)snprintf(
			says, sizeof(says),
			"get next: sequence %u past the %u segments promised",
			k[i].segments, k[i].segments);
		CHECK_EQ(omci_t_log_count(log, says), 1u);

		uint8_t got[1024];
		const size_t n = walk(fx.eng, promised, k[i].segments, got,
				      sizeof(got), false);
		if (n != promised)
			printf("    %u entries: walk delivered %zu of %u bytes (%s)\n",
			       k[i].entries, n, promised, k[i].why);
		CHECK_EQ(n, (size_t)promised);

		for (unsigned int e = 0; e < k[i].entries; e++) {
			uint8_t want[RULE_BYTES];
			size_t wn = 0;

			CHECK_EQ(omci_mib_table_get(fx.mib, ME171, INST,
						    RULE_DESC, e, want,
						    sizeof(want)),
				 OMCI_OK);
			(void)wn;
			CHECK_MEM(&got[e * RULE_BYTES], want, RULE_BYTES);
		}

		const uint8_t *past = get_next(fx.eng, ME171, INST, RULE_ATTR,
					       (uint16_t)k[i].segments, false);
		CHECK(past != NULL);
		if (past != NULL)
			CHECK_EQ(past[8], R_PARAM_ERROR);

		const uint8_t *stale =
			get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);
		CHECK(stale != NULL);
		if (stale != NULL)
			CHECK_EQ(stale[8], R_PARAM_ERROR);

		omci_t_down(&fx);
	}
}

static void test_an_empty_table_opens_no_walk(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	const uint8_t *g = get_table(fx.eng, ME171, INST, RULE_ATTR, false);

	CHECK(g != NULL);
	if (g == NULL) {
		omci_t_down(&fx);
		return;
	}
	CHECK_EQ(g[8], RES_DONE);
	CHECK_EQ(be32_at(g, 11), 0u);

	const uint8_t *n = get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);

	CHECK(n != NULL);
	if (n != NULL)
		CHECK_EQ(n[8], R_PARAM_ERROR);

	omci_t_down(&fx);
}

static void test_a_table_that_moves_mid_walk_is_refused(void)
{
	static const struct {
		bool grow;
		const char *why;
	} dir[] = {
		{ false, "an entry deleted under the walk" },
		{ true, "an entry added under the walk" },
	};

	for (size_t d = 0; d < ARRAY_SIZE(dir); d++) {
		struct omci_t_fixture fx;

		fixture(&fx);
		fill_rules(fx.mib, 4u);

		const uint8_t *g =
			get_table(fx.eng, ME171, INST, RULE_ATTR, false);
		CHECK(g != NULL);
		if (g == NULL) {
			omci_t_down(&fx);
			continue;
		}
		CHECK_EQ(be32_at(g, 11), 64u);

		const uint8_t *s0 =
			get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);
		CHECK(s0 != NULL);
		if (s0 != NULL)
			CHECK_EQ(s0[8], RES_DONE);

		if (dir[d].grow) {
			uint8_t e[RULE_BYTES];

			memset(e, 0x99, sizeof(e));
			e[0] = 0xEE;
			e[1] = 0xEE;
			e[2] = 0x5A;
			e[3] = 0xEE;
			CHECK_EQ(omci_mib_table_put(fx.mib, ME171, INST,
						    RULE_DESC, e, sizeof(e), 4u,
						    OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		} else {
			uint8_t key[4] = { 0u, 0u, 0x5Au, 0xC0u };

			CHECK_EQ(omci_mib_table_remove(fx.mib, ME171, INST,
						       RULE_DESC, key,
						       sizeof(key)),
				 OMCI_OK);
		}

		const uint8_t *s1 =
			get_next(fx.eng, ME171, INST, RULE_ATTR, 1u, false);
		CHECK(s1 != NULL);
		if (s1 == NULL) {
			omci_t_down(&fx);
			continue;
		}

		if (s1[8] != R_PARAM_ERROR)
			printf("    %s: segment 1 answered %u, want %u\n",
			       dir[d].why, s1[8], R_PARAM_ERROR);
		CHECK_EQ(s1[8], R_PARAM_ERROR);

		const uint8_t *again =
			get_next(fx.eng, ME171, INST, RULE_ATTR, 1u, false);
		CHECK(again != NULL);
		if (again != NULL)
			CHECK_EQ(again[8], R_PARAM_ERROR);

		omci_t_down(&fx);
	}
}

static void test_a_mib_reset_closes_the_walk(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);
	fill_rules(fx.mib, 4u);

	const uint8_t *g = get_table(fx.eng, ME171, INST, RULE_ATTR, false);

	CHECK(g != NULL);
	const uint8_t *s0 = get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);

	CHECK(s0 != NULL);
	if (s0 != NULL)
		CHECK_EQ(s0[8], RES_DONE);

	const uint8_t *reset = drive(fx.eng, 15u, 2u, 0u, false, NULL, 0);

	CHECK(reset != NULL);
	CHECK_EQ(omci_mib_create(fx.mib, ME171, INST), OMCI_OK);

	char log[1024];

	omci_log_level_set(OMCI_LOG_DEBUG);
	OMCI_T_CAPTURE_LOG(log, {
		const uint8_t *s1 =
			get_next(fx.eng, ME171, INST, RULE_ATTR, 1u, false);
		CHECK(s1 != NULL);
		if (s1 != NULL)
			CHECK_EQ(s1[8], R_PARAM_ERROR);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK_EQ(
		omci_t_log_count(
			log,
			"get next: 171/258 attr 6 does not match the read in progress"),
		1u);
	CHECK(strstr(log, "changed mid-read") == NULL);
	omci_t_down(&fx);
}

static void test_two_tables_in_one_mask_open_no_walk(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);

	const uint16_t both = (uint16_t)(omci_attr_bit(1u) | omci_attr_bit(2u));
	const uint8_t body[2] = { (uint8_t)(both >> 8),
				  (uint8_t)(both & 0xffu) };
	const uint8_t *rsp =
		drive(fx.eng, MT_GET, ME287, 0u, false, body, sizeof(body));

	CHECK(rsp != NULL);
	if (rsp != NULL) {
		CHECK_EQ(rsp[8], RES_DONE);
		CHECK_EQ(be16_at(rsp, 9), both);
		CHECK_EQ(be32_at(rsp, 11), 2u * omci_me_class_count);
		CHECK(be32_at(rsp, 15) > 0u);
	}

	for (uint8_t attr = 1u; attr <= 2u; attr++) {
		const uint8_t *refused =
			get_next(fx.eng, ME287, 0u, attr, 0u, false);
		CHECK(refused != NULL);
		if (refused != NULL)
			CHECK_EQ(refused[8], R_PARAM_ERROR);
	}

	const uint8_t *one = get_table(fx.eng, ME287, 0u, 2u, false);

	CHECK(one != NULL);
	const uint32_t size = one != NULL ? be32_at(one, 11) : 0u;

	CHECK(size > 0u);
	const uint8_t *seg = get_next(fx.eng, ME287, 0u, 2u, 0u, false);

	CHECK(seg != NULL);
	if (seg != NULL)
		CHECK_EQ(seg[8], RES_DONE);
	const uint8_t *wrong = get_next(fx.eng, ME287, 0u, 1u, 0u, false);

	CHECK(wrong != NULL);
	if (wrong != NULL)
		CHECK_EQ(wrong[8], R_PARAM_ERROR);

	omci_t_down(&fx);
}

static void test_a_get_of_two_tables_closes_its_own_channels_walk(void)
{
	for (unsigned int channel = 0; channel < 2u; channel++) {
		const bool mine = channel == 1u;
		struct omci_t_fixture fx;

		fixture(&fx);
		fill_rules(fx.mib, 4u);

		/*
		 * The other channel reads another table: a read of the same
		 * one would go on serving this channel after its own closed.
		 */
		CHECK(get_table(fx.eng, ME171, INST, RULE_ATTR, mine) != NULL);
		CHECK(get_table(fx.eng, ME287, 0u, ME287_ATTR, !mine) != NULL);

		const uint16_t both =
			(uint16_t)(omci_attr_bit(1u) | omci_attr_bit(2u));
		const uint8_t body[2] = { (uint8_t)(both >> 8),
					  (uint8_t)(both & 0xffu) };
		const uint8_t *two = drive(fx.eng, MT_GET, ME287, INST, mine,
					   body, sizeof(body));
		CHECK(two != NULL);
		if (two != NULL)
			CHECK_EQ(two[8], RES_DONE);

		const uint8_t *closed =
			get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, mine);
		CHECK(closed != NULL);
		if (closed != NULL)
			CHECK_EQ(closed[8], R_PARAM_ERROR);
		const uint8_t *open =
			get_next(fx.eng, ME287, 0u, ME287_ATTR, 0u, !mine);
		CHECK(open != NULL);
		if (open != NULL)
			CHECK_EQ(open[8], RES_DONE);
		omci_t_down(&fx);
	}
}

static void test_the_two_channels_walk_independently(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);
	fill_rules(fx.mib, 4u);

	const uint8_t *lo = get_table(fx.eng, ME171, INST, RULE_ATTR, false);
	const uint8_t *hi = get_table(fx.eng, ME171, INST, RULE_ATTR, true);

	CHECK(lo != NULL);
	CHECK(hi != NULL);
	if (lo == NULL || hi == NULL) {
		omci_t_down(&fx);
		return;
	}
	CHECK_EQ(be32_at(lo, 11), 64u);
	CHECK_EQ(be32_at(hi, 11), 64u);

	uint8_t got_lo[64], got_hi[64];
	size_t at_lo = 0, at_hi = 0;

	for (unsigned int s = 0; s < 3u; s++) {
		const size_t want = (s == 2u) ? 6u : SEGMENT;

		const uint8_t *a = get_next(fx.eng, ME171, INST, RULE_ATTR,
					    (uint16_t)s, false);
		CHECK(a != NULL);
		if (a != NULL) {
			CHECK_EQ(a[8], RES_DONE);
			CHECK_EQ(be16_at(a, 0) & 0x8000u, 0u);
			memcpy(&got_lo[at_lo], &a[11], want);
			at_lo += want;
		}

		const uint8_t *b = get_next(fx.eng, ME171, INST, RULE_ATTR,
					    (uint16_t)s, true);
		CHECK(b != NULL);
		if (b != NULL) {
			CHECK_EQ(b[8], RES_DONE);
			CHECK_EQ(be16_at(b, 0) & 0x8000u, 0x8000u);
			memcpy(&got_hi[at_hi], &b[11], want);
			at_hi += want;
		}
	}

	CHECK_EQ(at_lo, 64u);
	CHECK_EQ(at_hi, 64u);
	CHECK_MEM(got_lo, got_hi, 64u);

	omci_t_down(&fx);
}

static void test_the_malformed_walks(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);
	fill_rules(fx.mib, 4u);

	const uint8_t *cold =
		get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);
	CHECK(cold != NULL);
	if (cold != NULL)
		CHECK_EQ(cold[8], R_PARAM_ERROR);

	const uint8_t *g = get_table(fx.eng, ME171, INST, RULE_ATTR, false);

	CHECK(g != NULL);
	if (g == NULL) {
		omci_t_down(&fx);
		return;
	}

	const uint8_t none[4] = { 0u, 0u, 0u, 0u };
	const uint8_t *n0 = drive(fx.eng, MT_GET_NEXT, ME171, INST, false, none,
				  sizeof(none));
	CHECK(n0 != NULL);
	if (n0 != NULL)
		CHECK_EQ(n0[8], R_PARAM_ERROR);

	const uint8_t *wrong = get_next(fx.eng, ME171, INST, 1u, 0u, false);

	CHECK(wrong != NULL);
	if (wrong != NULL)
		CHECK_EQ(wrong[8], R_PARAM_ERROR);

	const uint8_t *elsewhere =
		get_next(fx.eng, ME171, 0x0999u, RULE_ATTR, 0u, false);
	CHECK(elsewhere != NULL);
	if (elsewhere != NULL)
		CHECK(elsewhere[8] != RES_DONE);

	const uint8_t *otherclass =
		get_next(fx.eng, ME287, INST, RULE_ATTR, 0u, false);
	CHECK(otherclass != NULL);
	if (otherclass != NULL) {
		if (otherclass[8] == RES_DONE)
			printf("    a Get-Next naming ME %u was served from ME %u's walk\n",
			       ME287, ME171);
		CHECK(otherclass[8] != RES_DONE);
	}

	const uint16_t two =
		(uint16_t)(omci_attr_bit(1u) | omci_attr_bit(RULE_ATTR));
	const uint8_t twobody[4] = {
		(uint8_t)(two >> 8),
		(uint8_t)(two & 0xffu),
		0u,
		0u,
	};
	const uint8_t *multi = drive(fx.eng, MT_GET_NEXT, ME171, INST, false,
				     twobody, sizeof(twobody));
	CHECK(multi != NULL);
	if (multi != NULL) {
		if (multi[8] == RES_DONE)
			printf("    a two-attribute Get-Next was served, echoing %04x\n",
			       be16_at(multi, 9));
		CHECK_EQ(multi[8], R_PARAM_ERROR);
	}

	const uint16_t pair =
		(uint16_t)(omci_attr_bit(RULE_ATTR) | omci_attr_bit(10u));
	const uint8_t pairbody[4] = {
		(uint8_t)(pair >> 8),
		(uint8_t)(pair & 0xffu),
		0u,
		0u,
	};
	const uint8_t *both = drive(fx.eng, MT_GET_NEXT, ME171, INST, false,
				    pairbody, sizeof(pairbody));
	CHECK(both != NULL);
	if (both != NULL) {
		if (both[8] == RES_DONE)
			printf("    Get-Next{%u,10} served with echo %04x — the OLT is told 10 is present\n",
			       RULE_ATTR, be16_at(both, 9));
		CHECK_EQ(both[8], R_PARAM_ERROR);
	}

	const uint8_t *first =
		get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);
	CHECK(first != NULL);
	uint8_t keep[SEGMENT];

	if (first != NULL) {
		CHECK_EQ(first[8], RES_DONE);
		memcpy(keep, &first[11], SEGMENT);
	}
	const uint8_t *second =
		get_next(fx.eng, ME171, INST, RULE_ATTR, 1u, false);
	CHECK(second != NULL);
	if (second != NULL)
		CHECK_EQ(second[8], RES_DONE);
	const uint8_t *again =
		get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);
	CHECK(again != NULL);
	if (again != NULL && first != NULL) {
		CHECK_EQ(again[8], RES_DONE);
		CHECK_MEM(&again[11], keep, SEGMENT);
	}

	const uint8_t *past =
		get_next(fx.eng, ME171, INST, RULE_ATTR, 99u, false);
	CHECK(past != NULL);
	if (past != NULL)
		CHECK_EQ(past[8], R_PARAM_ERROR);

	omci_t_down(&fx);
}

static void test_a_segment_carries_twenty_nine_bytes(void)
{
	struct omci_t_fixture fx;

	fixture(&fx);
	fill_rules(fx.mib, 4u);

	const uint8_t *g = get_table(fx.eng, ME171, INST, RULE_ATTR, false);

	CHECK(g != NULL);
	if (g == NULL) {
		omci_t_down(&fx);
		return;
	}

	const uint8_t *s0 = get_next(fx.eng, ME171, INST, RULE_ATTR, 0u, false);

	CHECK(s0 != NULL);
	if (s0 == NULL) {
		omci_t_down(&fx);
		return;
	}

	uint8_t want[64];

	for (unsigned int e = 0; e < 4u; e++)
		CHECK_EQ(omci_mib_table_get(fx.mib, ME171, INST, RULE_DESC, e,
					    &want[e * RULE_BYTES], RULE_BYTES),
			 OMCI_OK);

	CHECK_MEM(&s0[11], want, SEGMENT);

	const uint8_t *s2 = get_next(fx.eng, ME171, INST, RULE_ATTR, 2u, false);

	CHECK(s2 != NULL);
	if (s2 != NULL) {
		CHECK_EQ(s2[8], RES_DONE);
		CHECK_MEM(&s2[11], &want[2u * SEGMENT], 64u - 2u * SEGMENT);
	}

	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_the_promised_size_is_what_the_walk_delivers);
	RUN(test_an_empty_table_opens_no_walk);
	RUN(test_a_table_that_moves_mid_walk_is_refused);
	RUN(test_a_mib_reset_closes_the_walk);
	RUN(test_a_get_of_two_tables_closes_its_own_channels_walk);
	RUN(test_two_tables_in_one_mask_open_no_walk);
	RUN(test_the_two_channels_walk_independently);
	RUN(test_the_malformed_walks);
	RUN(test_a_segment_carries_twenty_nine_bytes);

	return OMCI_T_REPORT("getnext");
}
