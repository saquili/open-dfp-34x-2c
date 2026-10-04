// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the extended VLAN tagging operation, class 171, from the frame to the hardware. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/table.h"
#include "me/tables.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"
#include "omcid/array_size.h"

static struct omci_hw_caps hw_caps;
#define ME171 171u
#define RULE_DESC 6u
#define RULE_ATTR 6u
#define RULE_BYTES 16u
#define INST 0x0101u

static uint8_t tx[16][OMCI_FRAME_LEN];
static size_t tx_count;

static void on_send(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN])
{
	(void)ctx;
	if (tx_count < 16)
		memcpy(tx[tx_count++], frame, OMCI_FRAME_LEN);
}

static const struct omci_tx_ops tx_ops = { .send = on_send };

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void make_rule(uint8_t out[RULE_BYTES], uint32_t f0, uint32_t f1,
		      uint32_t t0, uint32_t t1)
{
	be32(&out[0], f0);
	be32(&out[4], f1);
	be32(&out[8], t0);
	be32(&out[12], t1);
}

static void make_xlate(uint8_t out[RULE_BYTES], unsigned int pri,
		       unsigned int match_vid, unsigned int assign_vid)
{
	make_rule(out, (15u << 28) | (4096u << 15),
		  (pri << 28) | (match_vid << 15), (1u << 30) | (15u << 16),
		  (pri << 16) | (assign_vid << 3));
}

static uint8_t set_rule_on(struct omci_engine *eng, uint16_t inst, uint16_t tid,
			   const uint8_t rule[RULE_BYTES])
{
	struct omci_frame f = {
		.tci = tid,
		.msg_type = 8,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = ME171,
		.instance = inst,
	};
	const uint16_t mask = omci_attr_bit(RULE_ATTR);

	f.contents[0] = (uint8_t)(mask >> 8);
	f.contents[1] = (uint8_t)mask;
	memcpy(&f.contents[2], rule, RULE_BYTES);

	uint8_t wire[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&f, wire);

	tx_count = 0;
	if (omci_engine_rx(eng, wire) != OMCI_OK || tx_count == 0)
		return 0xff;
	return tx[0][8];
}

static uint8_t delete_instance(struct omci_engine *eng, uint16_t inst,
			       uint16_t tid)
{
	struct omci_frame f = {
		.tci = tid,
		.msg_type = 6,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = ME171,
		.instance = inst,
	};
	uint8_t wire[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&f, wire);

	tx_count = 0;
	if (omci_engine_rx(eng, wire) != OMCI_OK || tx_count == 0)
		return 0xff;
	return tx[0][8];
}

static uint8_t set_rule(struct omci_engine *eng, uint16_t tid,
			const uint8_t rule[RULE_BYTES])
{
	struct omci_frame f = {
		.tci = tid,
		.msg_type = 8,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = ME171,
		.instance = INST,
	};
	const uint16_t mask = omci_attr_bit(RULE_ATTR);

	f.contents[0] = (uint8_t)(mask >> 8);
	f.contents[1] = (uint8_t)mask;
	memcpy(&f.contents[2], rule, RULE_BYTES);

	uint8_t wire[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&f, wire);

	tx_count = 0;
	if (omci_engine_rx(eng, wire) != OMCI_OK || tx_count == 0)
		return 0xff;
	return tx[0][8];
}

static void make_live_xlate(uint8_t out[RULE_BYTES], unsigned int pri,
			    unsigned int match_vid, unsigned int assign_vid)
{
	make_rule(out, (15u << 28) | (4096u << 15),
		  (pri << 28) | (match_vid << 15) | (5u << 12),
		  (1u << 30) | (15u << 16),
		  (pri << 16) | (assign_vid << 3) | 2u);
}

static void make_xlate_delete_tpid(uint8_t out[RULE_BYTES], unsigned int pri,
				   unsigned int match_vid, unsigned int tpid)
{
	make_rule(out, (15u << 28) | (4096u << 15),
		  (pri << 28) | (match_vid << 15) | (tpid << 12), 0xFFFFFFFFu,
		  0xFFFFFFFFu);
}

static void make_xlate_delete(uint8_t out[RULE_BYTES], unsigned int pri,
			      unsigned int match_vid)
{
	make_xlate_delete_tpid(out, pri, match_vid, 5u);
}

static void make_plain_delete(uint8_t out[RULE_BYTES], unsigned int pri,
			      unsigned int match_vid)
{
	make_xlate_delete_tpid(out, pri, match_vid, 0u);
}

/*
 * Whether the hardware holds exactly the translations of VID 410 that @to
 * gives: for each priority p, @to[p] the VID its frames go to, 0 for none.
 * A rule that names a priority takes the frames of that priority alone.
 */
static bool xlates_410(const uint16_t to[8])
{
	const struct omci_hw_vlan_xlate *applied = NULL;
	const size_t n = omci_hw_vlan_recorded(&applied);
	size_t want = 0;
	unsigned int seen = 0;

	for (unsigned int p = 0; p < 8u; p++)
		want += to[p] != 0u;
	if (n != want || applied == NULL)
		return n == want;
	for (size_t i = 0; i < n; i++) {
		const struct omci_hw_vlan_xlate *r = &applied[i];

		if (r->kind != OMCI_HW_VLAN_CVID_XLATE ||
		    r->match_cvid != 410u || !r->match_has_pri ||
		    r->match_pri > 7u || to[r->match_pri] != r->assign_cvid ||
		    (seen & (1u << r->match_pri)) != 0u)
			return false;
		seen |= 1u << r->match_pri;
	}
	return true;
}

static void reset_instance(struct omci_engine *eng, struct omci_mib *mib,
			   uint16_t tid)
{
	CHECK_EQ(delete_instance(eng, INST, tid), 0u);
	CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);
	CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 0u);
}

static uint32_t treatment_of(struct omci_mib *mib, size_t index)
{
	uint8_t e[RULE_BYTES];

	if (omci_mib_table_get(mib, ME171, INST, RULE_DESC, index, e,
			       sizeof(e)) != OMCI_OK)
		return 0;
	return ((uint32_t)e[8] << 24) | ((uint32_t)e[9] << 16) |
	       ((uint32_t)e[10] << 8) | (uint32_t)e[11];
}

static uint32_t inner_filter_of(struct omci_mib *mib, size_t index)
{
	uint8_t e[RULE_BYTES];

	if (omci_mib_table_get(mib, ME171, INST, RULE_DESC, index, e,
			       sizeof(e)) != OMCI_OK)
		return 0;
	return ((uint32_t)e[4] << 24) | ((uint32_t)e[5] << 16) |
	       ((uint32_t)e[6] << 8) | (uint32_t)e[7];
}

static uint32_t filter_of(struct omci_mib *mib, size_t index)
{
	uint8_t e[RULE_BYTES];

	if (omci_mib_table_get(mib, ME171, INST, RULE_DESC, index, e,
			       sizeof(e)) != OMCI_OK)
		return 0;
	return ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) |
	       ((uint32_t)e[2] << 8) | (uint32_t)e[3];
}

int main(void)
{
	if (omci_hw_caps(&hw_caps) != OMCI_OK) {
		printf("hw: cannot read capabilities\n");
		return 1;
	}

	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &tx_ops, NULL), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &hw_caps), OMCI_OK);

	CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);

	struct omci_me_table_policy policy;

	CHECK(omci_me_table_policy(ME171, RULE_DESC, &policy));
	CHECK_EQ(policy.key_len, 8u);
	CHECK_EQ(policy.del_offset, 8u);
	CHECK_EQ(policy.del_len, 8u);

	CHECK(policy.max_entries <= OMCI_HW_VLAN_MAX_RULES);

	printf("  three rules over one attribute:\n");

	uint8_t xlate[RULE_BYTES], passthru[RULE_BYTES], other[RULE_BYTES];

	make_xlate(xlate, 6u, 410u, 1410u);
	make_xlate(passthru, 6u, 4096u, 1410u);
	make_xlate(other, 6u, 10u, 210u);

	CHECK_EQ(set_rule(eng, 1, xlate), 0u);
	CHECK_EQ(set_rule(eng, 2, passthru), 0u);
	CHECK_EQ(set_rule(eng, 3, other), 0u);

	CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
	printf("    %zu rules stored\n",
	       omci_mib_table_count(mib, ME171, INST, RULE_DESC));

	CHECK_EQ(filter_of(mib, 0), ((15u << 28) | (4096u << 15)));
	CHECK_EQ(treatment_of(mib, 0), (1u << 30) | (15u << 16));
	CHECK_EQ(inner_filter_of(mib, 0), (6u << 28) | (410u << 15));
	CHECK_EQ(inner_filter_of(mib, 1), (6u << 28) | (4096u << 15));
	printf("    order is insertion order: the translation rule is first, not the pass-through\n");

	uint8_t xlate2[RULE_BYTES];

	make_xlate(xlate2, 6u, 410u, 1502u);
	CHECK_EQ(set_rule(eng, 4, xlate2), 0u);
	CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
	CHECK_EQ(treatment_of(mib, 0), (1u << 30) | (15u << 16));
	CHECK_EQ(filter_of(mib, 0), (15u << 28) | (4096u << 15));
	printf("    a repeated filter rewrites its rule in place\n");

	uint8_t del[RULE_BYTES];

	make_rule(del, (15u << 28) | (4096u << 15), (6u << 28) | (4096u << 15),
		  0xFFFFFFFFu, 0xFFFFFFFFu);
	CHECK(omci_me_table_is_delete(&policy, del, RULE_BYTES));
	CHECK_EQ(set_rule(eng, 5, del), 0u);
	CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 2u);
	CHECK_EQ(inner_filter_of(mib, 0), (6u << 28) | (410u << 15));
	CHECK_EQ(inner_filter_of(mib, 1), (6u << 28) | (10u << 15));
	printf("    an all-ones treatment half deletes the rule its filter names\n");

	CHECK_EQ(set_rule(eng, 6, del), 0u);
	CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 2u);

	{
		struct omci_frame g = {
			.tci = 7,
			.msg_type = 9,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = ME171,
			.instance = INST,
		};
		const uint16_t mask = omci_attr_bit(RULE_ATTR);

		g.contents[0] = (uint8_t)(mask >> 8);
		g.contents[1] = (uint8_t)mask;

		uint8_t wire[OMCI_FRAME_LEN];
		(void)!omci_frame_build(&g, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		CHECK_EQ(tx_count, 1u);

		const uint8_t *r = tx[0];

		CHECK_EQ(r[8], 0u);
		CHECK_EQ((r[9] << 8) | r[10], mask);
		const uint32_t size = ((uint32_t)r[11] << 24) |
				      ((uint32_t)r[12] << 16) |
				      ((uint32_t)r[13] << 8) | (uint32_t)r[14];
		CHECK_EQ(size, 2u * RULE_BYTES);
		printf("    a Get answers %u bytes — the size of the whole table\n",
		       (unsigned int)size);
	}

	{
		make_xlate(other, 6u, 10u, 210u);
		CHECK_EQ(set_rule(eng, 10, passthru), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);

		uint8_t flat[3u * RULE_BYTES];

		for (size_t i = 0; i < 3; i++)
			CHECK_EQ(omci_mib_table_get(mib, ME171, INST, RULE_DESC,
						    i, &flat[i * RULE_BYTES],
						    RULE_BYTES),
				 OMCI_OK);

		struct omci_frame g = {
			.tci = 20,
			.msg_type = 9,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = ME171,
			.instance = INST,
		};
		const uint16_t mask = omci_attr_bit(RULE_ATTR);

		g.contents[0] = (uint8_t)(mask >> 8);
		g.contents[1] = (uint8_t)mask;
		uint8_t wire[OMCI_FRAME_LEN];
		(void)!omci_frame_build(&g, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		const uint32_t size = ((uint32_t)tx[0][11] << 24) |
				      ((uint32_t)tx[0][12] << 16) |
				      ((uint32_t)tx[0][13] << 8) |
				      (uint32_t)tx[0][14];
		CHECK_EQ(size, 48u);

		uint8_t walked[64];
		size_t at = 0;
		const uint16_t segments = 2;

		for (uint16_t seq = 0; seq < segments; seq++) {
			struct omci_frame n = {
				.tci = (uint16_t)(30 + seq),
				.msg_type = 26,
				.ar = true,
				.device_id = OMCI_DEVICE_ID_BASELINE,
				.class_id = ME171,
				.instance = INST,
			};
			n.contents[0] = (uint8_t)(mask >> 8);
			n.contents[1] = (uint8_t)mask;
			n.contents[2] = (uint8_t)(seq >> 8);
			n.contents[3] = (uint8_t)seq;
			(void)!omci_frame_build(&n, wire);
			tx_count = 0;
			CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
			CHECK_EQ(tx_count, 1u);
			CHECK_EQ(tx[0][8], 0u);
			CHECK_EQ((tx[0][9] << 8) | tx[0][10], mask);

			const size_t take =
				(seq + 1u == segments) ? size - 29u * seq : 29u;
			memcpy(&walked[at], &tx[0][11], take);
			at += take;
		}
		CHECK_EQ(at, 48u);
		CHECK(memcmp(walked, flat, 48u) == 0);
		printf("    walked %zu bytes back in %u segments, byte-identical to the stored rules\n",
		       at, segments);

		struct omci_frame oob = {
			.tci = 40,
			.msg_type = 26,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = ME171,
			.instance = INST,
		};
		oob.contents[0] = (uint8_t)(mask >> 8);
		oob.contents[1] = (uint8_t)mask;
		oob.contents[3] = 9;
		(void)!omci_frame_build(&oob, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		CHECK_EQ(tx[0][8], 3u);

		struct omci_frame wrong = {
			.tci = 41,
			.msg_type = 26,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = ME171,
			.instance = 0x0601,
		};
		wrong.contents[0] = (uint8_t)(mask >> 8);
		wrong.contents[1] = (uint8_t)mask;
		(void)!omci_frame_build(&wrong, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		CHECK_EQ(tx[0][8], 3u);
		printf("    an out-of-range sequence and a mismatched instance are both refused\n");
	}

	{
		uint8_t r[RULE_BYTES];

		CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(mib, &hw_caps), OMCI_OK);
		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);
		for (unsigned int i = 0; i < 5; i++) {
			make_xlate(r, 6u, 100u + (unsigned int)i,
				   1000u + (unsigned int)i);
			CHECK_EQ(set_rule(eng, (uint16_t)(50 + i), r), 0u);
		}
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 5u);

		const uint16_t mask = omci_attr_bit(RULE_ATTR);
		uint8_t wire[OMCI_FRAME_LEN];
		struct omci_frame g = {
			.tci = 60,
			.msg_type = 9,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = ME171,
			.instance = INST,
		};
		g.contents[0] = (uint8_t)(mask >> 8);
		g.contents[1] = (uint8_t)mask;
		(void)!omci_frame_build(&g, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);

		struct omci_frame n0 = {
			.tci = 61,
			.msg_type = 26,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = ME171,
			.instance = INST,
		};
		n0.contents[0] = (uint8_t)(mask >> 8);
		n0.contents[1] = (uint8_t)mask;
		(void)!omci_frame_build(&n0, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		CHECK_EQ(tx[0][8], 0u);

		make_rule(r, (15u << 28) | (4096u << 15),
			  (6u << 28) | (100u << 15), 0xFFFFFFFFu, 0xFFFFFFFFu);
		CHECK_EQ(set_rule(eng, 62, r), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 4u);

		struct omci_frame n1 = {
			.tci = 63,
			.msg_type = 26,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = ME171,
			.instance = INST,
		};
		n1.contents[0] = (uint8_t)(mask >> 8);
		n1.contents[1] = (uint8_t)mask;
		n1.contents[3] = 1;
		(void)!omci_frame_build(&n1, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		CHECK_EQ(tx_count, 1u);
		CHECK_EQ(tx[0][8], 3u);
		printf("    a Set between two segments closes the walk instead of reshaping it\n");

		(void)!omci_frame_build(&g, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);

		struct omci_frame reset = {
			.tci = 70,
			.msg_type = 15,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = 2,
			.instance = 0,
		};
		(void)!omci_frame_build(&reset, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);

		(void)!omci_frame_build(&n1, wire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, wire), OMCI_OK);
		CHECK_EQ(tx[0][8], 3u);
		printf("    a MibReset closes the walk even though the identity still matches\n");
	}

	{
		uint8_t r[RULE_BYTES];

		CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(mib, &hw_caps), OMCI_OK);
		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);

		unsigned int accepted = 0;

		for (unsigned int i = 0; i < 200; i++) {
			make_xlate(r, 6u, 100u + (unsigned int)i,
				   1000u + (unsigned int)i);
			if (set_rule(eng, (uint16_t)(0x300 + i), r) == 0u)
				accepted++;
		}
		struct omci_me_table_policy p2;

		CHECK(omci_me_table_policy(ME171, RULE_DESC, &p2));
		CHECK_EQ(accepted, p2.max_entries);
		CHECK(omci_mib_table_capacity(mib) > 0u);
		printf("    a Set flood stops at %u rules and leaves %zu pool entries for everyone else\n",
		       p2.max_entries, omci_mib_table_capacity(mib));

		CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(mib, &hw_caps), OMCI_OK);
		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);
	}

	CHECK_EQ(omci_mib_delete(mib, ME171, INST), OMCI_OK);
	CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 0u);

	(void)!omci_mib_delete(mib, ME171, INST);
	CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);

	uint8_t ethertype_rule[RULE_BYTES];

	make_rule(ethertype_rule, (15u << 28) | (4096u << 15),
		  (6u << 28) | (77u << 15) | 3u, (1u << 30) | (15u << 16),
		  (6u << 16) | (1410u << 3));
	CHECK_EQ(set_rule(eng, 6, ethertype_rule), 3u);

	uint8_t good[RULE_BYTES];

	make_xlate(good, 6u, 88u, 1888u);
	CHECK_EQ(set_rule(eng, 7, good), 0u);
	printf("    an unprogrammable rule is refused, and does not poison the next Set\n");

	{
		const struct omci_hw_vlan_xlate *applied = NULL;
		const uint16_t INST_B = 0x0102u;

		CHECK_EQ(delete_instance(eng, INST, 199), 0u);
		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 0u);

		CHECK_EQ(omci_mib_create(mib, ME171, INST_B), OMCI_OK);

		unsigned int accepted = 0, refused_full = 0;

		for (unsigned int i = 0; i < 9u; i++) {
			uint8_t r[RULE_BYTES];

			make_xlate(r, 0u, 300u + i, 3000u + i);
			(set_rule_on(eng, INST, (uint16_t)(200u + i), r) ==
			 0u) ?
				accepted++ :
				refused_full++;
		}
		for (unsigned int i = 0; i < 8u; i++) {
			uint8_t r[RULE_BYTES];

			make_xlate(r, 0u, 400u + i, 4000u + i);
			(set_rule_on(eng, INST_B, (uint16_t)(220u + i), r) ==
			 0u) ?
				accepted++ :
				refused_full++;
		}

		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);
		CHECK_EQ(accepted, (unsigned int)OMCI_HW_VLAN_MAX_RULES);
		CHECK_EQ(refused_full, 1u);
		printf("    %u accepted, %u refused for budget, %u in the classifier\n",
		       accepted, refused_full,
		       (unsigned int)omci_hw_vlan_recorded(&applied));

		uint8_t dup[RULE_BYTES];

		make_live_xlate(dup, 0u, 300u, 3000u);
		CHECK_EQ(set_rule_on(eng, INST, 240, dup), 0u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		uint8_t none_row[RULE_BYTES];

		make_rule(none_row, (15u << 28) | (4096u << 15),
			  (14u << 28) | (4096u << 15), (1u << 30) | (15u << 16),
			  (0u << 16) | (1u << 3));
		CHECK_EQ(set_rule_on(eng, INST, 241, none_row), 0u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		uint8_t ether[RULE_BYTES];

		make_rule(ether, (15u << 28) | (4096u << 15),
			  (6u << 28) | (555u << 15) | 3u,
			  (1u << 30) | (15u << 16), (6u << 16) | (1555u << 3));
		CHECK_EQ(set_rule_on(eng, INST, 242, ether), 3u);

		uint8_t qinq[RULE_BYTES];

		make_rule(qinq, (15u << 28) | (4096u << 15),
			  (6u << 28) | (666u << 15),
			  (0u << 30) | (6u << 16) | (777u << 3),
			  (6u << 16) | (666u << 3));
		CHECK_EQ(set_rule_on(eng, INST, 243, qinq), 6u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		/*
		 * An S-tag of the VID that a translation gives a C-tag is a
		 * rule of its own, the TPIDs telling the two apart, and finds
		 * no room.
		 */
		uint8_t qinq_beside[RULE_BYTES];

		make_rule(qinq_beside, (15u << 28) | (4096u << 15),
			  (6u << 28) | (500u << 15),
			  (0u << 30) | (6u << 16) | (3000u << 3),

			  (6u << 16) | (500u << 3));
		CHECK_EQ(set_rule_on(eng, INST, 244, qinq_beside), 6u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		CHECK_EQ(delete_instance(eng, INST_B, 250), 0u);
		CHECK_EQ(delete_instance(eng, INST, 251), 0u);
		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 0u);
	}

	{
		uint8_t r6[RULE_BYTES], r5[RULE_BYTES], r0[RULE_BYTES];
		uint8_t n6[RULE_BYTES], n5[RULE_BYTES], n0[RULE_BYTES];
		uint8_t d6[RULE_BYTES], d5[RULE_BYTES], d0[RULE_BYTES];

		make_live_xlate(r6, 6u, 410u, 1410u);
		make_live_xlate(n6, 6u, 410u, 1601u);
		make_live_xlate(r5, 5u, 410u, 1410u);
		make_live_xlate(n5, 5u, 410u, 1601u);
		make_live_xlate(r0, 0u, 410u, 1410u);
		make_live_xlate(n0, 0u, 410u, 1601u);
		make_xlate_delete(d6, 6u, 410u);
		make_xlate_delete(d5, 5u, 410u);
		make_xlate_delete(d0, 0u, 410u);

		static const uint16_t old_all[8] = {
			[0] = 1410u, [5] = 1410u, [6] = 1410u
		};
		static const uint16_t new_all[8] = {
			[0] = 1601u, [5] = 1601u, [6] = 1601u
		};

		reset_instance(eng, mib, 300);
		CHECK_EQ(set_rule(eng, 301, r6), 0u);
		CHECK_EQ(set_rule(eng, 302, r5), 0u);
		CHECK_EQ(set_rule(eng, 303, r0), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
		CHECK(xlates_410(old_all));
		printf("    three p-bit rows are three translations, one per priority\n");

		static const uint16_t r1_6[8] = {
			[0] = 1410u, [5] = 1410u, [6] = 1601u
		};
		static const uint16_t r1_5[8] = {
			[0] = 1410u, [5] = 1601u, [6] = 1601u
		};

		CHECK_EQ(set_rule(eng, 311, n6), 0u);
		CHECK(xlates_410(r1_6));
		CHECK_EQ(set_rule(eng, 312, n5), 0u);
		CHECK(xlates_410(r1_5));
		CHECK_EQ(set_rule(eng, 313, n0), 0u);
		CHECK(xlates_410(new_all));
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
		printf("    route 1, rewrite in walk order: each priority follows its own row\n");

		static const uint16_t r2_0[8] = {
			[0] = 1601u, [5] = 1410u, [6] = 1410u
		};
		static const uint16_t r2_5[8] = {
			[0] = 1601u, [5] = 1601u, [6] = 1410u
		};

		reset_instance(eng, mib, 320);
		CHECK_EQ(set_rule(eng, 321, r6), 0u);
		CHECK_EQ(set_rule(eng, 322, r5), 0u);
		CHECK_EQ(set_rule(eng, 323, r0), 0u);
		CHECK_EQ(set_rule(eng, 324, n0), 0u);
		CHECK(xlates_410(r2_0));
		CHECK_EQ(set_rule(eng, 325, n5), 0u);
		CHECK(xlates_410(r2_5));
		CHECK_EQ(set_rule(eng, 326, n6), 0u);
		CHECK(xlates_410(new_all));
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
		printf("    route 2, rewrite in reverse: each priority follows its own row\n");

		reset_instance(eng, mib, 330);
		CHECK_EQ(set_rule(eng, 331, r6), 0u);
		CHECK_EQ(set_rule(eng, 332, r5), 0u);
		CHECK_EQ(set_rule(eng, 333, r0), 0u);

		uint8_t n3[RULE_BYTES];

		static const uint16_t r3_add[8] = {
			[0] = 1410u, [3] = 1601u, [5] = 1410u, [6] = 1410u
		};
		static const uint16_t r3_end[8] = { [3] = 1601u };

		make_live_xlate(n3, 3u, 410u, 1601u);
		CHECK_EQ(set_rule(eng, 334, n3), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 4u);
		CHECK(xlates_410(r3_add));

		CHECK_EQ(set_rule(eng, 335, d6), 0u);
		CHECK_EQ(set_rule(eng, 336, d5), 0u);
		CHECK_EQ(set_rule(eng, 337, d0), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 1u);
		CHECK(xlates_410(r3_end));
		printf("    route 3, add-new-then-delete-old: ends on the new translation, not empty\n");

		reset_instance(eng, mib, 340);
		CHECK_EQ(set_rule(eng, 341, r6), 0u);
		CHECK_EQ(set_rule(eng, 342, r5), 0u);
		CHECK_EQ(set_rule(eng, 343, r0), 0u);

		static const uint16_t r4_d6[8] = { [0] = 1410u, [5] = 1410u };
		static const uint16_t r4_d5[8] = { [0] = 1410u, [6] = 1601u };
		static const uint16_t r4_d0[8] = { [5] = 1601u, [6] = 1601u };

		CHECK_EQ(set_rule(eng, 344, d6), 0u);
		CHECK(xlates_410(r4_d6));
		CHECK_EQ(set_rule(eng, 345, n6), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
		CHECK(xlates_410(r1_6));

		CHECK_EQ(set_rule(eng, 346, d5), 0u);
		CHECK(xlates_410(r4_d5));
		CHECK_EQ(set_rule(eng, 347, n5), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
		CHECK(xlates_410(r1_5));

		CHECK_EQ(set_rule(eng, 348, d0), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 2u);
		CHECK(xlates_410(r4_d0));
		CHECK_EQ(set_rule(eng, 349, n0), 0u);
		CHECK_EQ(omci_mib_table_count(mib, ME171, INST, RULE_DESC), 3u);
		CHECK(xlates_410(new_all));
		printf("    route 4, delete-and-re-add per p-bit: each delete takes its priority, ends with 3 rows\n");

		reset_instance(eng, mib, 350);
	}

	{
		const uint16_t INST_C = 0x0103u;
		const struct omci_vlan_fate_rec *f = NULL;
		const struct omci_hw_vlan_xlate *applied = NULL;
		uint8_t r[RULE_BYTES];
		unsigned int over = 0;

		reset_instance(eng, mib, 400);
		CHECK_EQ(omci_mib_create(mib, ME171, INST_C), OMCI_OK);

		for (unsigned int k = 0; k < 15u; k++) {
			make_xlate(r, 0u, 301u + k, 1301u + k);
			CHECK_EQ(set_rule(eng, (uint16_t)(401u + k), r), 0u);
		}
		make_xlate(r, 0u, 999u, 1999u);
		CHECK_EQ(set_rule_on(eng, INST_C, 420, r), 0u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		/*
		 * A rule that gives the VID another rule gives is a many-to-one
		 * mapping, which needs an entry of its own upstream: a full
		 * classifier refuses it at the Set.
		 */
		make_xlate(r, 0u, 777u, 1301u);
		CHECK_EQ(set_rule(eng, 421, r), 6u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		/*
		 * In the MIB all the same, as more rules than room can be after
		 * a restart: the newcomer finds no room, not the rule in force
		 * of the other instance.
		 */
		CHECK_EQ(omci_mib_table_put(mib, ME171, INST, RULE_DESC, r,
					    RULE_BYTES, 8, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		(void)!omci_vlan_program(mib, ME171, RULE_DESC, NULL);

		unsigned int victim_instance = 0, victim_match = 0,
			     c_programmed = 0, over_budget = 0;
		size_t nf = omci_vlan_fates(&f, &over);

		for (size_t i = 0; i < nf; i++) {
			if (f[i].fate == (uint8_t)OMCI_VLAN_FATE_OVER_BUDGET) {
				over_budget++;
				victim_instance = f[i].instance;
				victim_match = f[i].match_cvid;
			}
			if (f[i].instance == INST_C &&
			    f[i].fate == (uint8_t)OMCI_VLAN_FATE_PROGRAMMED)
				c_programmed++;
		}
		CHECK_EQ(over, 0u);
		CHECK_EQ(over_budget, 1u);
		CHECK_EQ(victim_instance, INST);
		CHECK_EQ(victim_match, 777u);
		CHECK_EQ(c_programmed, 1u);

		const size_t napplied = omci_hw_vlan_recorded(&applied);

		CHECK_EQ(napplied, (size_t)OMCI_HW_VLAN_MAX_RULES);
		if (napplied == (size_t)OMCI_HW_VLAN_MAX_RULES) {
			unsigned int found = 0, unexpected = 0;

			for (size_t i = 0; i < napplied; i++) {
				const uint16_t m = applied[i].match_cvid,
					       v = applied[i].assign_cvid;
				if (m >= 301u && m <= 315u && v == m + 1000u)
					found++;
				else if (m == 999u && v == 1999u)
					found++;
				else
					unexpected++;
			}
			CHECK_EQ(found, 16u);
			CHECK_EQ(unexpected, 0u);
		}
		printf("    shape 2: the newcomer is dropped, not the rule in force of the other instance\n");

		omci_vlan_forget_in_force();
		(void)!omci_vlan_program(mib, ME171, RULE_DESC, NULL);

		unsigned int after_reboot_victim = 0;

		nf = omci_vlan_fates(&f, &over);
		for (size_t i = 0; i < nf; i++)
			if (f[i].fate == (uint8_t)OMCI_VLAN_FATE_OVER_BUDGET)
				after_reboot_victim = f[i].instance;
		CHECK_EQ(after_reboot_victim, INST_C);
		printf("    and with nothing remembered it falls back to walk order, as at boot\n");

		CHECK_EQ(delete_instance(eng, INST_C, 430), 0u);
		reset_instance(eng, mib, 431);
	}

	{
		const uint16_t INST_B3 = 0x0102u;
		const struct omci_hw_vlan_xlate *applied = NULL;
		uint8_t r[RULE_BYTES];
		uint16_t tid = 900u;

		reset_instance(eng, mib, 899);
		if (!omci_mib_exists(mib, ME171, INST_B3))
			CHECK_EQ(omci_mib_create(mib, ME171, INST_B3), OMCI_OK);

		make_rule(r, (15u << 28) | (4096u << 15),
			  (2u << 28) | (44u << 15), 0xc00f0000u,
			  (2u << 16) | (55u << 3));
		CHECK_EQ(set_rule(eng, tid++, r), 3u);

		for (unsigned int k = 0; k < 5u; k++) {
			make_xlate(r, 0u, 500u + k, 1500u + k);
			CHECK_EQ(set_rule_on(eng, INST_B3, tid++, r), 0u);
		}
		make_xlate(r, 0u, 400u, 1400u);
		CHECK_EQ(set_rule(eng, tid++, r), 0u);

		const size_t napplied = omci_hw_vlan_recorded(&applied);

		CHECK_EQ(napplied, 6u);
		if (napplied == 6u) {
			CHECK_EQ(applied[0].match_cvid, 400u);
			CHECK_EQ(applied[0].assign_cvid, 1400u);
			unsigned int ok = 0;

			for (size_t i = 1u; i < 6u; i++)
				if (applied[i].match_cvid ==
				    500u + (unsigned int)(i - 1u))
					ok++;
			CHECK_EQ(ok, 5u);
		}

		unsigned int yielded = 0;

		CHECK_EQ(omci_vlan_last_reconcile(&yielded, NULL), 6u);
		printf("    an unencodable row does not arm the preference on a table that fits\n");

		CHECK_EQ(delete_instance(eng, INST_B3, tid++), 0u);
		reset_instance(eng, mib, tid++);
	}

	{
		const uint16_t INST_B2 = 0x0102u;
		const uint16_t INST_C2 = 0x0103u;
		const struct omci_vlan_fate_rec *f = NULL;
		const struct omci_hw_vlan_xlate *applied = NULL;
		uint8_t r[RULE_BYTES];
		unsigned int over = 0;
		uint16_t tid = 800u;

		reset_instance(eng, mib, 799);
		if (!omci_mib_exists(mib, ME171, INST_B2))
			CHECK_EQ(omci_mib_create(mib, ME171, INST_B2), OMCI_OK);
		if (!omci_mib_exists(mib, ME171, INST_C2))
			CHECK_EQ(omci_mib_create(mib, ME171, INST_C2), OMCI_OK);

		make_xlate(r, 0u, 50u, 1050u);
		CHECK_EQ(set_rule(eng, tid++, r), 0u);
		make_xlate(r, 0u, 60u, 1060u);
		CHECK_EQ(set_rule(eng, tid++, r), 0u);
		for (unsigned int k = 0; k < 6u; k++) {
			make_xlate(r, 0u, 201u + k, 1201u + k);
			CHECK_EQ(set_rule(eng, tid++, r), 0u);
		}
		for (unsigned int k = 0; k < 8u; k++) {
			make_xlate(r, 0u, 301u + k, 1301u + k);
			CHECK_EQ(set_rule_on(eng, INST_B2, tid++, r), 0u);
		}
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		/*
		 * 40 -> 1050 is a many-to-one mapping beside 50 -> 1050, which a
		 * full classifier refuses at the Set; it is put in the MIB as a
		 * restart could leave it. 50 -> 1090 is shadowed by 50 -> 1050.
		 */
		make_xlate(r, 0u, 40u, 1050u);
		CHECK_EQ(set_rule_on(eng, INST_C2, tid++, r), 6u);
		CHECK_EQ(omci_mib_table_put(mib, ME171, INST_C2, RULE_DESC, r,
					    RULE_BYTES, 8, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		make_xlate(r, 0u, 50u, 1090u);
		CHECK_EQ(set_rule_on(eng, INST_C2, tid++, r), 0u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		make_plain_delete(r, 0u, 50u);
		CHECK_EQ(set_rule(eng, tid++, r), 0u);

		unsigned int live_40 = 0, live_50_1090 = 0, over_40 = 0,
			     over_50_1090 = 0;
		const size_t napplied = omci_hw_vlan_recorded(&applied);

		for (size_t i = 0; i < napplied; i++) {
			if (applied[i].match_cvid == 40u &&
			    applied[i].assign_cvid == 1050u)
				live_40++;
			if (applied[i].match_cvid == 50u &&
			    applied[i].assign_cvid == 1090u)
				live_50_1090++;
		}
		const size_t nf = omci_vlan_fates(&f, &over);

		for (size_t i = 0; i < nf; i++) {
			if (f[i].fate != (uint8_t)OMCI_VLAN_FATE_OVER_BUDGET)
				continue;
			if (f[i].match_cvid == 40u && f[i].net_vid == 1050u)
				over_40++;
			if (f[i].match_cvid == 50u && f[i].net_vid == 1090u)
				over_50_1090++;
		}

		CHECK_EQ(napplied, (size_t)OMCI_HW_VLAN_MAX_RULES);
		CHECK_EQ(live_40, 1u);
		CHECK_EQ(live_50_1090, 0u);
		CHECK_EQ(over_40, 0u);
		CHECK_EQ(over_50_1090, 1u);
		printf("    a deleted rule's match VID does not protect a translation that never ran\n");

		CHECK_EQ(delete_instance(eng, INST_B2, tid++), 0u);
		CHECK_EQ(delete_instance(eng, INST_C2, tid++), 0u);
		reset_instance(eng, mib, tid++);
	}

	{
		const uint16_t inst[4] = { INST, 0x0102u, 0x0103u, 0x0104u };

		static const uint32_t seeds[] = {
			0x5EEDu, 1u,	    2u,	     3u,      7u,      11u,
			13u,	 17u,	    19u,     23u,     29u,     31u,
			37u,	 41u,	    43u,     47u,     0xABCDu, 0xDEADu,
			0xBEEFu, 0xC0FFEEu, 99991u,  12345u,  424242u, 8675309u,
			0x1234u, 0x9999u,   0xFACEu, 0x0BADu, 271828u, 314159u,
			161803u, 577215u,
		};
		unsigned int over_budget_steps = 0, checked_incumbents = 0,
			     yield_steps = 0;

		reset_instance(eng, mib, 600);
		for (unsigned int k = 1; k < 4u; k++)
			if (!omci_mib_exists(mib, ME171, inst[k]))
				CHECK_EQ(omci_mib_create(mib, ME171, inst[k]),
					 OMCI_OK);

		struct omci_hw_vlan_xlate before[OMCI_HW_VLAN_MAX_RULES];
		size_t before_n = 0;

		for (unsigned int si = 0; si < ARRAY_SIZE(seeds); si++) {
			uint32_t seed = seeds[si];

			for (unsigned int step = 0; step < 1200u; step++) {
				seed = seed * 1664525u + 1013904223u;
				const unsigned int pick = (seed >> 16) & 3u;
				const unsigned int slot = (seed >> 8) & 7u;
				const unsigned int base = 100u + pick * 100u;
				const unsigned int mvid = base + slot;

				const bool borrow = ((seed >> 24) & 7u) == 0u;
				const unsigned int nvid =
					1000u + ((seed >> 20) & 7u) +
					(borrow ? ((pick + 1u) & 3u) : pick) *
						100u;
				const bool kill = ((seed >> 4) & 7u) == 0u;

				const unsigned int kind = (seed >> 12) & 15u;
				unsigned int yielded_before = 0;
				(void)omci_vlan_last_reconcile(&yielded_before,
							       NULL);

				uint8_t r[RULE_BYTES];

				if (kill)
					make_xlate_delete_tpid(
						r, 0u, mvid,
						((seed >> 18) & 1u) ? 5u : 0u);
				else if (kind == 0u)
					/*
					 * S-VIDs from 3000 on, apart from the
					 * C-VIDs the translations give, so that
					 * a VID pair names one rule in the
					 * fates, which do not say S or C.
					 */
					make_rule(
						r, (15u << 28) | (4096u << 15),
						(0u << 28) | (mvid << 15) |
							(5u << 12),
						(0u << 30) | (0u << 16) |
							((nvid + 2000u) << 3),
						(0u << 16) | (mvid << 3) | 2u);
				else if (kind == 1u)
					make_rule(
						r, (15u << 28) | (4096u << 15),
						(0u << 28) | (mvid << 15) |
							(5u << 12),
						0xc00f0000u,
						(0u << 16) | (nvid << 3) | 2u);
				else
					make_xlate(r, 0u, mvid, nvid);

				/*
				 * A quarter of the rows go into the MIB without
				 * the Set's check for room, as a restart can
				 * leave more rules than room: the reconcile then
				 * has to choose.
				 */
				if (!kill && ((seed >> 27) & 3u) == 0u) {
					(void)!omci_mib_table_put(
						mib, ME171, inst[pick],
						RULE_DESC, r, RULE_BYTES, 8,
						OMCI_MIB_TABLE_LAST);
					(void)!omci_vlan_program(
						mib, ME171, RULE_DESC, NULL);
				} else {
					(void)set_rule_on(
						eng, inst[pick],
						(uint16_t)(600u +
							   (step % 900u)),
						r);
				}

				const struct omci_vlan_fate_rec *f = NULL;
				unsigned int over = 0;
				const size_t nf = omci_vlan_fates(&f, &over);

				unsigned int dropped = 0;

				for (size_t i = 0; i < nf; i++)
					if (f[i].fate ==
					    (uint8_t)OMCI_VLAN_FATE_OVER_BUDGET)
						dropped++;

				const struct omci_hw_vlan_xlate *now = NULL;
				const size_t now_n =
					omci_hw_vlan_recorded(&now);

				if ((step & 31u) == 0u)
					for (size_t a = 0; a + 1u < nf; a++)
						for (size_t b = a + 1u; b < nf;
						     b++)
							CHECK(!(f[a].instance ==
									f[b].instance &&
								f[a].row ==
									f[b].row));

				unsigned int yielded_now = 0;
				size_t preferred_n = 0;
				const size_t plain_n = omci_vlan_last_reconcile(
					&yielded_now, &preferred_n);
				CHECK(now_n >= plain_n);
				if (yielded_now != yielded_before) {
					yield_steps++;

					CHECK(preferred_n < plain_n);
					CHECK_EQ(now_n, plain_n);
				}

				if (dropped > 0u)
					over_budget_steps++;

				if (yielded_now == yielded_before) {
					for (size_t b = 0; b < before_n; b++) {
						const uint16_t bnet =
							(before[b].kind ==
							 OMCI_HW_VLAN_SVID_PUSH) ?
								before[b]
									.assign_svid :
								before[b]
									.assign_cvid;
						for (size_t i = 0; i < nf;
						     i++) {
							if (f[i].match_cvid !=
								    before[b].match_cvid ||
							    f[i].net_vid !=
								    bnet)
								continue;
							checked_incumbents++;
							CHECK(f[i].fate !=
							      (uint8_t)
								      OMCI_VLAN_FATE_OVER_BUDGET);
							break;
						}
					}
				}

				before_n = now_n;
				for (size_t i = 0; i < now_n; i++)
					before[i] = now[i];
			}
		}

		CHECK(over_budget_steps > 500u);
		CHECK(checked_incumbents > 10000u);
		printf("    sweep: %u seeds, %u over-budget reconciles, %u incumbents checked, %u steps where the preference yielded\n",
		       (unsigned int)(ARRAY_SIZE(seeds)), over_budget_steps,
		       checked_incumbents, yield_steps);

		reset_instance(eng, mib, 700);
		for (unsigned int k = 1; k < 4u; k++)
			(void)delete_instance(eng, inst[k],
					      (uint16_t)(710u + k));
	}

	{
		const struct omci_hw_vlan_xlate *applied = NULL;

		uint8_t live[RULE_BYTES];

		make_xlate(live, 6u, 91u, 1911u);
		CHECK_EQ(set_rule(eng, 8, live), 0u);
		CHECK(omci_hw_vlan_recorded(&applied) > 0u);

		CHECK_EQ(delete_instance(eng, INST, 9), 0u);
		CHECK(!omci_mib_exists(mib, ME171, INST));
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 0u);
		printf("    Delete takes the instance's rules out of the classifier too\n");

		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);
		CHECK_EQ(set_rule(eng, 10, live), 0u);
		CHECK(omci_hw_vlan_recorded(&applied) > 0u);

		struct omci_frame r = {
			.tci = 11,
			.msg_type = 15,
			.ar = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = 2,
			.instance = 0,
		};
		uint8_t rwire[OMCI_FRAME_LEN];
		(void)!omci_frame_build(&r, rwire);
		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, rwire), OMCI_OK);
		CHECK(!omci_mib_exists(mib, ME171, INST));
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 0u);
		printf("    MibReset clears the classifier, not just the MIB\n");
	}

	{
		uint8_t a[RULE_BYTES], b[RULE_BYTES];
		const struct omci_vlan_fate_rec *fates = NULL;
		unsigned int over = 0;
		bool saw_refused = false, saw_programmed = false;

		CHECK_EQ(omci_mib_create(mib, ME171, INST), OMCI_OK);
		make_xlate(a, 0, 410u, 1410u);
		make_xlate(b, 0, 420u, 1430u);

		CHECK_EQ(set_rule(eng, 60, a), 0u);
		CHECK_EQ(set_rule(eng, 61, b), 0u);

		omci_hw_vlan_reject_next(410u);
		CHECK_EQ(set_rule(eng, 62, a), 3u);
		printf("    a rule the switch refused comes back as a parameter error, not OK\n");

		for (size_t i = 0, nf = omci_vlan_fates(&fates, &over); i < nf;
		     i++) {
			if (fates[i].match_cvid == 410u &&
			    fates[i].fate == OMCI_VLAN_FATE_REFUSED)
				saw_refused = true;
			if (fates[i].match_cvid == 420u &&
			    fates[i].fate == OMCI_VLAN_FATE_PROGRAMMED)
				saw_programmed = true;
		}
		CHECK(saw_refused);
		CHECK(saw_programmed);
		printf("    the refused row reports `refused`, and its neighbour still reports `programmed`\n");

		omci_hw_vlan_reject_next(410u);
		CHECK_EQ(set_rule(eng, 63, b), 0u);
		printf("    a rule still refused does not make the NEXT rule's Set fail\n");

		CHECK_EQ(delete_instance(eng, INST, 64), 0u);
	}

	omci_engine_fini(eng);
	omci_mib_fini(mib);

	printf("\n%d omci_t_checks, %d omci_t_failures\n", omci_t_checks,
	       omci_t_failures);
	return OMCI_T_REPORT("test_me171");
}
