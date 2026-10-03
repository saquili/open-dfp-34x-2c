// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the two priority channels: what each keeps of its own, and what
 * an OLT that counts its identifiers into the priority bit carries from one
 * to the other.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ME_ONU_DATA 2u

#include "me/vlan.h"
#include "mib/store.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

#define ME171 171u
#define INST 0x0101u
#define RULE_ATTR 6u
#define RULE_DESC 6u
#define MT_GET 9u
#define MT_GETNEXT 26u

#define ATTR6_MASK 0x0400u

static void rule_row(uint8_t out[16], unsigned int pri, unsigned int match,
		     unsigned int assign)
{
	omci_be32_store(&out[0], (15u << 28) | (4096u << 15));
	omci_be32_store(&out[4], (pri << 28) | (match << 15));
	omci_be32_store(&out[8], (1u << 30) | (15u << 16));
	omci_be32_store(&out[12], (pri << 16) | (assign << 3));
}

static uint8_t get_next(struct omci_engine *eng, uint16_t tid, bool high,
			uint16_t seq)
{
	uint8_t req[OMCI_FRAME_LEN];

	if (high)
		omci_t_build_pri(req, tid, MT_GETNEXT, true, false, ME171,
				 INST);
	else
		omci_t_build(req, tid, MT_GETNEXT, true, false, ME171, INST);

	struct omci_frame f;
	(void)!omci_frame_parse(req, &f);
	omci_be16_store(&f.contents[0], ATTR6_MASK);
	omci_be16_store(&f.contents[2], seq);
	(void)!omci_frame_build(&f, req);

	omci_t_tx_reset();
	(void)!omci_engine_rx(eng, req);
	const uint8_t *rsp = omci_t_last_tx();
	return rsp == NULL ? 0xFFu : rsp[8];
}

static void open_walk(struct omci_engine *eng, uint16_t tid, bool high)
{
	uint8_t req[OMCI_FRAME_LEN];

	if (high)
		omci_t_build_pri(req, tid, MT_GET, true, false, ME171, INST);
	else
		omci_t_build(req, tid, MT_GET, true, false, ME171, INST);

	struct omci_frame f;
	(void)!omci_frame_parse(req, &f);
	omci_be16_store(&f.contents[0], ATTR6_MASK);
	(void)!omci_frame_build(&f, req);

	omci_t_tx_reset();
	(void)!omci_engine_rx(eng, req);
}

static void fill_table(struct omci_mib *mib, unsigned int n)
{
	(void)!omci_mib_create(mib, ME171, INST);
	for (unsigned int i = 0; i < n; i++) {
		uint8_t row[16];

		rule_row(row, 6u, 100u + i, 1000u + i);
		(void)!omci_mib_table_put(mib, ME171, INST, RULE_DESC, row, 16,
					  8, OMCI_MIB_TABLE_LAST);
	}
}

static void test_the_high_channel_is_answered(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	fill_table(fx.mib, 4);

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build_pri(req, 0x0101u, MT_GET, true, false, ME171, INST);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	struct omci_frame f;

	CHECK_EQ(omci_frame_parse(rsp, &f), OMCI_OK);
	CHECK(f.priority);
	CHECK_EQ(f.tci, 0x0101u);
	CHECK(f.ak);

	omci_t_down(&fx);
}

static void test_two_walks_do_not_see_each_other(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	fill_table(fx.mib, 6);

	open_walk(fx.eng, 0x0201u, false);
	open_walk(fx.eng, 0x0202u, true);

	for (uint16_t seq = 0; seq < 3u; seq++) {
		CHECK_EQ(get_next(fx.eng, (uint16_t)(0x0300u + seq), false,
				  seq),
			 0u);
		CHECK_EQ(get_next(fx.eng, (uint16_t)(0x0400u + seq), true, seq),
			 0u);
	}

	CHECK_EQ(get_next(fx.eng, 0x0500u, false, 0u), 0u);

	CHECK_EQ(get_next(fx.eng, 0x0501u, false, 9999u), 3u);
	CHECK_EQ(get_next(fx.eng, 0x0502u, true, 3u), 0u);
	CHECK_EQ(get_next(fx.eng, 0x0503u, true, 9999u), 3u);
	CHECK_EQ(get_next(fx.eng, 0x0504u, false, 1u), 0u);

	omci_t_down(&fx);
}

/*
 * A read opened on the low channel is continued on the high one, as by an
 * OLT whose identifier passed 0x7fff, and ends after its last segment there
 * too.
 */
static void test_a_walk_continues_on_the_other_channel(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	fill_table(fx.mib, 4);

	uint8_t want[3][32];

	open_walk(fx.eng, 0x0601u, false);
	for (uint16_t seq = 0; seq < 3u; seq++) {
		CHECK_EQ(get_next(fx.eng, (uint16_t)(0x0602u + seq), false,
				  seq),
			 0u);
		memcpy(want[seq], &omci_t_last_tx()[8], 32);
	}

	open_walk(fx.eng, 0x7FFEu, false);
	CHECK_EQ(get_next(fx.eng, 0x7FFFu, false, 0u), 0u);
	CHECK_MEM(&omci_t_last_tx()[8], want[0], 32);
	CHECK_EQ(get_next(fx.eng, 0x8000u, true, 1u), 0u);
	CHECK_MEM(&omci_t_last_tx()[8], want[1], 32);
	CHECK_EQ(get_next(fx.eng, 0x8001u, true, 2u), 0u);
	CHECK_MEM(&omci_t_last_tx()[8], want[2], 32);

	CHECK_EQ(get_next(fx.eng, 0x8002u, true, 0u), 3u);
	CHECK_EQ(get_next(fx.eng, 0x0003u, false, 0u), 3u);

	omci_t_down(&fx);
}

static void test_the_caches_do_not_answer_for_each_other(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_mib_create(fx.mib, ME171, INST), OMCI_OK);
	fill_table(fx.mib, 2);

	uint8_t lo[OMCI_FRAME_LEN], hi[OMCI_FRAME_LEN];

	omci_t_build(lo, 0x0777u, MT_GET, true, false, ME171, INST);
	omci_t_build_pri(hi, 0x0777u, MT_GET, true, false, ME171, INST);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, lo), OMCI_OK);
	uint8_t lo_first[OMCI_FRAME_LEN];

	CHECK(omci_t_last_tx() != NULL);
	memcpy(lo_first, omci_t_last_tx(), OMCI_FRAME_LEN);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, hi), OMCI_OK);
	CHECK(omci_t_last_tx() != NULL);
	uint8_t hi_first[OMCI_FRAME_LEN];

	memcpy(hi_first, omci_t_last_tx(), OMCI_FRAME_LEN);

	struct omci_frame f;

	CHECK_EQ(omci_frame_parse(hi_first, &f), OMCI_OK);
	CHECK(f.priority);
	CHECK(memcmp(hi_first, lo_first, OMCI_FRAME_LEN) != 0);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, lo), OMCI_OK);
	if (omci_t_last_tx() != NULL)
		CHECK_MEM(omci_t_last_tx(), lo_first, OMCI_FRAME_LEN);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, hi), OMCI_OK);
	if (omci_t_last_tx() != NULL)
		CHECK_MEM(omci_t_last_tx(), hi_first, OMCI_FRAME_LEN);

	omci_t_down(&fx);
}

static void test_filling_one_cache_leaves_the_other(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_mib_create(fx.mib, ME171, INST), OMCI_OK);
	fill_table(fx.mib, 3);

	uint8_t up[OMCI_FRAME_LEN], next[OMCI_FRAME_LEN];

	omci_t_build_pri(up, 0x0050u, 13u, true, false, ME_ONU_DATA, 0u);
	(void)!omci_t_rx(fx.eng, up);

	omci_t_build_pri(next, 0x0055u, 14u, true, false, ME_ONU_DATA, 0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, next), OMCI_OK);
	CHECK(omci_t_last_tx() != NULL);
	uint8_t hi_first[OMCI_FRAME_LEN];

	memcpy(hi_first, omci_t_last_tx(), OMCI_FRAME_LEN);

	for (uint16_t t = 0; t < 20u; t++) {
		uint8_t lo[OMCI_FRAME_LEN];

		omci_t_build(lo, (uint16_t)(0x1000u + t), MT_GET, true, false,
			     ME171, INST);
		(void)!omci_t_rx(fx.eng, lo);
	}

	CHECK_EQ(omci_mib_create(fx.mib, ME171, 0x0199u), OMCI_OK);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, next), OMCI_OK);
	CHECK(omci_t_last_tx() != NULL);
	if (omci_t_last_tx() != NULL)
		CHECK_MEM(omci_t_last_tx(), hi_first, OMCI_FRAME_LEN);

	omci_t_down(&fx);
}

static void test_every_message_type_answers_on_its_own_channel(void)
{
	static const uint8_t mt[] = { 9u, 11u, 12u, 13u, 14u, 24u, 26u, 28u };

	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	fill_table(fx.mib, 2);

	for (unsigned int i = 0; i < ARRAY_SIZE(mt); i++) {
		for (unsigned int high = 0; high < 2u; high++) {
			uint8_t req[OMCI_FRAME_LEN];
			const uint16_t tid =
				(uint16_t)(0x2000u + (i << 4) + high);
			if (high)
				omci_t_build_pri(req, tid, mt[i], true, false,
						 ME171, INST);
			else
				omci_t_build(req, tid, mt[i], true, false,
					     ME171, INST);

			omci_t_tx_reset();
			(void)!omci_engine_rx(fx.eng, req);
			if (omci_t_tx.n == 0)
				continue;

			struct omci_frame f;

			CHECK_EQ(omci_frame_parse(omci_t_last_tx(), &f),
				 OMCI_OK);
			CHECK_EQ(f.priority, high != 0u);
			CHECK_EQ(f.tci, (uint16_t)(tid & 0x7FFFu));
		}
	}

	omci_t_down(&fx);
}

static uint16_t mib_upload(struct omci_engine *eng, uint16_t tid, bool high)
{
	uint8_t req[OMCI_FRAME_LEN];

	if (high)
		omci_t_build_pri(req, tid, 13u, true, false, ME_ONU_DATA, 0u);
	else
		omci_t_build(req, tid, 13u, true, false, ME_ONU_DATA, 0u);

	omci_t_tx_reset();
	(void)!omci_engine_rx(eng, req);
	const uint8_t *rsp = omci_t_last_tx();
	return rsp == NULL ? 0u : (uint16_t)omci_be16(&rsp[8]);
}

/* Upload next @seq; @out gets the answer's contents. */
static void upload_next(struct omci_engine *eng, uint16_t tid, bool high,
			uint16_t seq, uint8_t out[32])
{
	uint8_t req[OMCI_FRAME_LEN];

	if (high)
		omci_t_build_pri(req, tid, 14u, true, false, ME_ONU_DATA, 0u);
	else
		omci_t_build(req, tid, 14u, true, false, ME_ONU_DATA, 0u);

	struct omci_frame f;
	(void)!omci_frame_parse(req, &f);
	omci_be16_store(&f.contents[0], seq);
	(void)!omci_frame_build(&f, req);

	omci_t_tx_reset();
	(void)!omci_engine_rx(eng, req);
	const uint8_t *rsp = omci_t_last_tx();

	memset(out, 0xEE, 32);
	if (rsp != NULL)
		memcpy(out, &rsp[8], 32);
}

static bool all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != 0u)
			return false;
	return true;
}

/*
 * An upload started on the low channel is continued on the high one, as by
 * an OLT whose identifier passed 0x7fff: each unit is the one it was before.
 */
static void test_an_upload_continues_on_the_other_channel(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	fill_table(fx.mib, 2);
	for (uint16_t i = 0; i < 3u; i++)
		CHECK_EQ(omci_mib_create(fx.mib, ME171,
					 (uint16_t)(0x9000u + i)),
			 OMCI_OK);

	const uint16_t units = mib_upload(fx.eng, 0x0100u, false);
	uint8_t want[4][32];

	CHECK(units > 4u);
	for (uint16_t seq = 0; seq < 4u; seq++) {
		upload_next(fx.eng, (uint16_t)(0x0101u + seq), false, seq,
			    want[seq]);
		CHECK(!all_zero(want[seq], 32));
	}

	uint8_t got[32];

	CHECK_EQ(mib_upload(fx.eng, 0x7FFDu, false), units);
	upload_next(fx.eng, 0x7FFEu, false, 0u, got);
	CHECK_MEM(got, want[0], 32);
	upload_next(fx.eng, 0x7FFFu, false, 1u, got);
	CHECK_MEM(got, want[1], 32);
	upload_next(fx.eng, 0x8000u, true, 2u, got);
	CHECK_MEM(got, want[2], 32);
	upload_next(fx.eng, 0x8001u, true, 3u, got);
	CHECK_MEM(got, want[3], 32);

	upload_next(fx.eng, 0x8002u, true, units, got);
	CHECK(all_zero(got, 32));

	omci_t_down(&fx);
}

static void test_mib_reset_invalidates_both_channels(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x0B01u, 4u, true, false, ME171, 0x0501u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK(omci_t_last_tx() != NULL);
	CHECK_EQ(omci_t_last_tx()[8], 0u);

	uint8_t hi[OMCI_FRAME_LEN];

	omci_t_build_pri(hi, 0x0B01u, 4u, true, false, ME171, 0x0502u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, hi), OMCI_OK);
	CHECK(omci_t_last_tx() != NULL);
	CHECK_EQ(omci_t_last_tx()[8], 0u);

	fill_table(fx.mib, 3);
	open_walk(fx.eng, 0x0B02u, false);
	uint8_t up[OMCI_FRAME_LEN];

	omci_t_build(up, 0x0B03u, 13u, true, false, ME_ONU_DATA, 0u);
	(void)!omci_t_rx(fx.eng, up);

	uint8_t rst[OMCI_FRAME_LEN];

	omci_t_build(rst, 0x0B10u, 15u, true, false, ME_ONU_DATA, 0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, rst), OMCI_OK);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK(omci_mib_exists(fx.mib, ME171, 0x0501u));

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, hi), OMCI_OK);
	CHECK(omci_mib_exists(fx.mib, ME171, 0x0502u));

	CHECK_EQ(get_next(fx.eng, 0x0B20u, false, 0u), 3u);

	omci_t_build(up, 0x0B21u, 14u, true, false, ME_ONU_DATA, 0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, up), OMCI_OK);
	CHECK(omci_t_last_tx() != NULL);
	if (omci_t_last_tx() != NULL) {
		bool all_zero = true;

		for (unsigned int i = 8; i < 40u; i++)
			if (omci_t_last_tx()[i] != 0u)
				all_zero = false;
		CHECK(all_zero);
	}

	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_the_high_channel_is_answered);
	RUN(test_two_walks_do_not_see_each_other);
	RUN(test_a_walk_continues_on_the_other_channel);
	RUN(test_the_caches_do_not_answer_for_each_other);
	RUN(test_filling_one_cache_leaves_the_other);
	RUN(test_every_message_type_answers_on_its_own_channel);
	RUN(test_an_upload_continues_on_the_other_channel);
	RUN(test_mib_reset_invalidates_both_channels);
	return OMCI_T_REPORT("test_priority");
}
