// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of decoding and programming VLAN rules. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/vlan.h"
#include "mib/store.h"

#include "harness.h"
#include "log_capture.h"
#include "omcid/array_size.h"

static void pack(uint8_t row[16], uint32_t w0, uint32_t w1, uint32_t w2,
		 uint32_t w3)
{
	const uint32_t w[4] = { w0, w1, w2, w3 };

	for (int i = 0; i < 4; i++) {
		row[i * 4 + 0] = (uint8_t)(w[i] >> 24);
		row[i * 4 + 1] = (uint8_t)(w[i] >> 16);
		row[i * 4 + 2] = (uint8_t)(w[i] >> 8);
		row[i * 4 + 3] = (uint8_t)w[i];
	}
}

/*
 * A translation of the frames of one VID and any priority, which keeps the
 * priority. @tag goes into the filter's TPID field, which the hardware does
 * not match: rows that differ only in it are different rows of one rule.
 */
static void xlate_row(uint8_t row[16], unsigned int tag, unsigned int match,
		      unsigned int assign)
{
	pack(row, 0xf8000000u, (8u << 28) | (match << 15) | ((tag & 7u) << 12),
	     0x400f0000u, (8u << 16) | (assign << 3));
}

/* A translation of the frames of one VID and priority, which keeps it. */
static void pri_xlate_row(uint8_t row[16], unsigned int pri, unsigned int match,
			  unsigned int assign)
{
	pack(row, 0xf8000000u, (pri << 28) | (match << 15), 0x400f0000u,
	     (8u << 16) | (assign << 3));
}

/* A discard of the frames of one VID, of priority @pri or of any (8). */
static void discard_row(uint8_t row[16], unsigned int tag, unsigned int pri,
			unsigned int match)
{
	pack(row, 0xf8000000u, (pri << 28) | (match << 15) | ((tag & 7u) << 12),
	     0xc00f0000u, 0x000f0000u);
}

static void passthru_row(uint8_t row[16], unsigned int pri, unsigned int match)
{
	pack(row, 0xf8000000u, (pri << 28) | (match << 15), 0x000f0000u,
	     0x00000000u);
}

static void clear_rows(struct omci_mib *mib)
{
	for (;;) {
		const size_t before =
			omci_mib_table_count(mib, 171u, 0x101u, 6u);
		if (before == 0)
			return;
		uint8_t row[16];

		if (omci_mib_table_get(mib, 171u, 0x101u, 6u, 0, row,
				       sizeof(row)) != OMCI_OK)
			return;
		if (omci_mib_table_remove(mib, 171u, 0x101u, 6u, row, 8) !=
		    OMCI_OK)
			return;
	}
}

int main(void)
{
	struct omci_vlan_rule r;
	uint8_t row[16];

	pack(row, 0xf8000000u, 0x60cd5000u, 0x400f0000u, 0x00062c12u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.match_cvid, 410u);
	CHECK_EQ(r.assign_cvid, 1410u);

	pack(row, 0xf8000000u, 0x50cd5000u, 0x400f0000u, 0x00052c12u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.match_cvid, 410u);
	CHECK_EQ(r.assign_cvid, 1410u);

	pack(row, 0xf8000000u, 0x00cd5000u, 0x400f0000u, 0x00002c12u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.match_cvid, 410u);
	CHECK_EQ(r.assign_cvid, 1410u);

	pack(row, 0xf8000000u, 0xf0005000u, 0xc00f0000u, 0x000f0002u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);

	/*
	 * A discard of the single-tagged frames of a VID, of the priority the
	 * filter names or of any; of the double-tagged frames of a VID it is
	 * refused, and so is one with an Ethertype or a VID out of range.
	 */
	pack(row, 0xf8000000u, 0x60cd5000u, 0xc00f0000u, 0x00062c12u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_DISCARD_VID);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_NONE);
	CHECK_EQ(r.match_cvid, 410u);
	CHECK(r.match_has_pri);
	CHECK_EQ(r.match_pri, 6u);
	pack(row, 0xf8000000u, (8u << 28) | (410u << 15), 0xc00f0000u,
	     0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_DISCARD_VID);
	CHECK_EQ(r.match_cvid, 410u);
	CHECK(!r.match_has_pri);
	pack(row, (8u << 28) | (200u << 15), (8u << 28) | (410u << 15),
	     0xc00f0000u, 0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_DISCARD);
	pack(row, (8u << 28) | (4096u << 15), (8u << 28) | (410u << 15),
	     0xc00f0000u, 0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_DISCARD);
	pack(row, 0xf8000000u, (8u << 28) | (410u << 15) | 2u, 0xc00f0000u,
	     0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_ETHERTYPE);
	pack(row, 0xf8000000u, (8u << 28) | (4095u << 15), 0xc00f0000u,
	     0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);
	pack(row, 0xf8000000u, (8u << 28) | (0u << 15), 0xc00f0000u,
	     0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);

	pack(row, 0xf8000000u, 0x60000000u | (4096u << 15), 0x400f0000u,
	     0x00002c12u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);

	pack(row, 0xf8000000u, 0x60000000u | (100u << 15) | (5u << 12),
	     0x400f0000u, (6u << 16) | (200u << 3) | 2u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.match_cvid, 100u);
	CHECK_EQ(r.assign_cvid, 200u);

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (6u << 16) | (100u << 3), (6u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SVID_PUSH);
	CHECK_EQ(r.match_cvid, 410u);
	CHECK_EQ(r.assign_svid, 100u);
	CHECK_EQ(r.assign_pri, 6u);

	/* An S-VID push copying the inner priority: the filter's, or any. */
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (8u << 16) | (100u << 3), (6u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SVID_PUSH);
	CHECK_EQ(r.assign_pri, 6u);
	pack(row, 0xf8000000u, (8u << 28) | (410u << 15),
	     (0u << 30) | (8u << 16) | (100u << 3), (8u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SVID_PUSH);
	CHECK_EQ(r.assign_pri, OMCI_HW_VLAN_PRI_COPY);
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (9u << 16) | (100u << 3), (6u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_PCP_REMARK);

	/* An untagged frame given a C-tag, by the inner or the outer treatment. */
	pack(row, 0xf8000000u, 0xf8000000u, 0x000f0000u,
	     (5u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_PUSH);
	CHECK_EQ(r.match_cvid, 0u);
	CHECK_EQ(r.assign_cvid, 410u);
	CHECK_EQ(r.assign_pri, 5u);
	pack(row, 0xf8000000u, 0xf8000000u, (3u << 16) | (36u << 3),
	     0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_PUSH);
	CHECK_EQ(r.assign_cvid, 36u);
	CHECK_EQ(r.assign_pri, 3u);
	pack(row, 0xf8000000u, 0xf8000002u, 0x000f0000u,
	     (5u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_PUSH);
	CHECK_EQ(r.filter_ethertype, OMCI_HW_ETHERTYPE_PPPOE);
	CHECK_EQ(r.assign_cvid, 410u);
	pack(row, 0xf8000000u, 0xf8000000u, 0x000f0000u,
	     (8u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_PCP_REMARK);
	pack(row, 0xf8000000u, 0xf8000000u, 0x000f0000u,
	     (5u << 16) | (5000u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);
	/*
	 * An untagged frame given two tags: the inner treatment's C-tag behind
	 * the outer treatment's S-tag, each with its own priority; copies of a
	 * priority, the DSCP and VIDs copied from a tag are refused, as an
	 * untagged frame has neither.
	 */
	pack(row, 0xf8000000u, 0xf8000000u, (3u << 16) | (36u << 3),
	     (5u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SCVID_PUSH);
	CHECK_EQ(r.match_cvid, 0u);
	CHECK_EQ(r.assign_svid, 36u);
	CHECK_EQ(r.assign_pri, 3u);
	CHECK_EQ(r.assign_cvid, 410u);
	CHECK(r.inner_has_pri);
	CHECK_EQ(r.inner_pri, 5u);
	pack(row, 0xf8000000u, 0xf8000000u, (3u << 16) | (36u << 3),
	     (0u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SCVID_PUSH);
	CHECK_EQ(r.inner_pri, 0u);
	pack(row, 0xf8000000u, 0xf8000000u, (3u << 16) | (36u << 3),
	     (8u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_PCP_REMARK);
	pack(row, 0xf8000000u, 0xf8000000u, (10u << 16) | (36u << 3),
	     (5u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_PCP_REMARK);
	pack(row, 0xf8000000u, 0xf8000000u, (3u << 16) | (36u << 3),
	     (5u << 16) | (4096u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);
	pack(row, 0xf8000000u, 0xf8000004u, (3u << 16) | (36u << 3),
	     (5u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SCVID_PUSH);
	CHECK_EQ(r.filter_ethertype, OMCI_HW_ETHERTYPE_IPV6);

	/* A translation that gives the tag a new priority. */
	pack(row, 0xf8000000u, (8u << 28) | (410u << 15), 0x400f0000u,
	     (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.assign_pri, 6u);
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15), 0x400f0000u,
	     (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.assign_pri, OMCI_HW_VLAN_PRI_KEEP);

	/*
	 * The filter on the EtherType: of a single-tagged frame it is taken;
	 * of a double-tagged one, or with a code G.988 reserves, it is
	 * refused. The codes name the EtherTypes of G.988 (11/2022) 9.3.13,
	 * PPPoE's discovery and session both.
	 */
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15) | 3u, 0x400f0000u,
	     (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_NONE);
	CHECK_EQ(r.filter_ethertype, 3u);
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15) | 6u, 0x400f0000u,
	     (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_ETHERTYPE_CODE);
	pack(row, 0xf8000000u, 0xf800000fu, 0x000f0000u,
	     (5u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_ETHERTYPE_CODE);
	pack(row, (6u << 28) | (10u << 15), (6u << 28) | (410u << 15) | 1u,
	     0x400f0000u, (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_ETHERTYPE);
	{
		static const struct {
			uint8_t code;
			size_t n;
			uint16_t type[2];
		} codes[] = {
			{ 0u, 0u, { 0u } },
			{ 1u, 1u, { 0x0800u } },
			{ 2u, 2u, { 0x8863u, 0x8864u } },
			{ 3u, 1u, { 0x0806u } },
			{ 4u, 1u, { 0x86ddu } },
			{ 5u, 1u, { 0x888eu } },
			{ 6u, 0u, { 0u } },
		};

		for (size_t i = 0; i < ARRAY_SIZE(codes); i++) {
			uint16_t et[OMCI_HW_ETHERTYPES_MAX] = { 0 };

			CHECK_EQ(omci_hw_ethertypes(codes[i].code, et),
				 codes[i].n);
			for (size_t k = 0; k < codes[i].n; k++)
				CHECK_EQ(et[k], codes[i].type[k]);
		}
	}

	/* A translation that gives the tag priority 0, of one priority or any. */
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15), 0x400f0000u,
	     (0u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.assign_cvid, 1410u);
	CHECK_EQ(r.assign_pri, 0u);
	CHECK(r.match_has_pri);
	CHECK_EQ(r.match_pri, 6u);
	pack(row, 0xf8000000u, (8u << 28) | (410u << 15), 0x400f0000u,
	     (0u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);
	CHECK_EQ(r.assign_pri, 0u);
	CHECK(!r.match_has_pri);

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15), 0x400f0000u,
	     (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_CVID_XLATE);

	pack(row, (6u << 28) | (10u << 15), (6u << 28) | (410u << 15),
	     0x400f0000u, (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_SHAPE);

	pack(row, (7u << 28) | (11u << 15) | (3u << 12),
	     (6u << 28) | (410u << 15) | (5u << 12) | 2u,
	     (1u << 30) | (4u << 16) | (12u << 3) | 1u,
	     (6u << 16) | (1410u << 3) | 2u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.filter_outer_pri, 7u);
	CHECK_EQ(r.filter_outer_vid, 11u);
	CHECK_EQ(r.filter_outer_tpid, 3u);
	CHECK_EQ(r.filter_inner_pri, 6u);
	CHECK_EQ(r.filter_inner_vid, 410u);
	CHECK_EQ(r.filter_inner_tpid, 5u);
	CHECK_EQ(r.filter_ethertype, 2u);
	CHECK_EQ(r.tags_to_remove, 1u);
	CHECK_EQ(r.treat_outer_pri, 4u);
	CHECK_EQ(r.treat_outer_vid, 12u);
	CHECK_EQ(r.treat_outer_tpid, 1u);
	CHECK_EQ(r.treat_inner_pri, 6u);
	CHECK_EQ(r.treat_inner_vid, 1410u);
	CHECK_EQ(r.treat_inner_tpid, 2u);

	pack(row, 0xf8000000u, 0xf8000000u, (3u << 30) | (15u << 16),
	     (15u << 16));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);
	CHECK_EQ(r.tags_to_remove, 3u);

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15), 0x400f0000u,
	     (6u << 16) | (4096u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);

	pack(row, 0xf8000000u, (6u << 28) | (5000u << 15), 0x400f0000u,
	     (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15), 0x400f0000u,
	     (6u << 16) | (5000u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);

	pack(row, 0xf8000000u, (14u << 28) | (410u << 15), 0x400f0000u,
	     (6u << 16) | (1410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);

	pack(row, 0xf8000000u, (15u << 28) | (410u << 15),
	     (1u << 30) | (15u << 16), (15u << 16));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);

	/* An S-VID push that gives the C-tag behind it priority 0, or 2. */
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (6u << 16) | (100u << 3), (0u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SVID_PUSH);
	CHECK_EQ(r.assign_svid, 100u);
	CHECK_EQ(r.assign_pri, 6u);
	CHECK(r.inner_has_pri);
	CHECK_EQ(r.inner_pri, 0u);
	pack(row, 0xf8000000u, (8u << 28) | (410u << 15),
	     (0u << 30) | (8u << 16) | (100u << 3), (2u << 16) | (4096u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SVID_PUSH);
	CHECK_EQ(r.assign_pri, OMCI_HW_VLAN_PRI_COPY);
	CHECK(r.inner_has_pri);
	CHECK_EQ(r.inner_pri, 2u);
	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (6u << 16) | (100u << 3), (6u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK(!r.inner_has_pri);

	CHECK(omci_vlan_reject_name((enum omci_vlan_reject)99)[0] != '\0');

	CHECK(!omci_vlan_decode(row, 16, NULL));
	CHECK(!omci_vlan_decode(NULL, 16, &r));

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (6u << 16) | (100u << 3), (6u << 16) | (99u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_SHAPE);

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (6u << 16) | (100u << 3), (6u << 16) | (4096u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_SVID_PUSH);
	CHECK_EQ(r.assign_svid, 100u);

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15),
	     (0u << 30) | (6u << 16) | (5000u << 3), (6u << 16) | (410u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);

	pack(row, 0xf8000000u, (6u << 28) | (410u << 15), 0x400f0000u,
	     (6u << 16) | (4095u << 3));
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_UNSUPPORTED);
	CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);

	/*
	 * The default rules that discard, as an operator's OLT sets all three:
	 * that of the single-tagged frames drops what no other rule takes; those
	 * of the double-tagged and the untagged frames leave nothing to program.
	 */
	pack(row, 0xf8000000u, 0xe8000000u, 0xc00f0000u, 0x000f8000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_DISCARD_TAGGED);
	pack(row, 0xe8000000u, 0xe8000000u, 0xc00f0000u, 0x000f8000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);
	pack(row, 0xf8000000u, 0xf8000000u, 0xc00f0000u, 0x000f0000u);
	CHECK(omci_vlan_decode(row, 16, &r));
	CHECK_EQ(r.op, OMCI_VLAN_OP_NONE);

	{
		static const struct {
			unsigned int match, assign;
			bool ok;
		} ends[] = {
			{ 410u, 1u, true },	 { 410u, 4094u, true },
			{ 1u, 1410u, true },	 { 4094u, 1410u, true },
			{ 410u, 0u, false },	 { 410u, 4095u, false },
			{ 4095u, 1410u, false },
		};

		for (size_t i = 0; i < ARRAY_SIZE(ends); i++) {
			xlate_row(row, 6u, ends[i].match, ends[i].assign);
			CHECK(omci_vlan_decode(row, 16, &r));
			CHECK_EQ(r.op, ends[i].ok ? OMCI_VLAN_OP_CVID_XLATE :
						    OMCI_VLAN_OP_UNSUPPORTED);
			if (ends[i].ok) {
				CHECK_EQ(r.match_cvid, ends[i].match);
				CHECK_EQ(r.assign_cvid, ends[i].assign);
			} else {
				CHECK_EQ(r.reject, OMCI_VLAN_REJECT_VID_RANGE);
			}
		}
	}

	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_ETHERTYPE)[0] != '\0');
	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_PCP_REMARK)[0] != '\0');
	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_SHAPE)[0] != '\0');
	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_VID_RANGE)[0] != '\0');
	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_DISCARD)[0] != '\0');
	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_ETHERTYPE_CODE)[0] !=
	      '\0');
	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_ETHERTYPE_PASS)[0] !=
	      '\0');
	CHECK(omci_vlan_reject_name(OMCI_VLAN_REJECT_NONE)[0] != '\0');

	CHECK(!omci_vlan_decode(row, 15, &r));

	struct omci_mib *mib = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, 171u, 0x101u), OMCI_OK);

	uint8_t r2[16], r3[16], r4[16];

	pack(r2, 0xf8000000u, 0x60cd5000u, 0x400f0000u, 0x00062c12u);
	pack(r3, 0xf8000000u, 0x50cd5000u, 0x400f0000u, 0x00052c12u);
	pack(r4, 0xf8000000u, 0x00cd5000u, 0x400f0000u, 0x00002c12u);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, r2, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, r3, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, r4, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);

	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);

	/*
	 * The three rows translate the frames of priorities 6, 5 and 0 alone,
	 * in list order 0, 5, 6; the first gives the way back for all three.
	 */
	const struct omci_hw_vlan_xlate *applied = NULL;
	const size_t napplied = omci_hw_vlan_recorded(&applied);
	static const uint8_t prios[3] = { 0u, 5u, 6u };

	CHECK_EQ(napplied, 3u);
	for (size_t i = 0; i < napplied && i < 3u; i++) {
		CHECK_EQ(applied[i].match_cvid, 410u);
		CHECK_EQ(applied[i].assign_cvid, 1410u);
		CHECK(applied[i].match_has_pri);
		CHECK_EQ(applied[i].match_pri, prios[i]);
		CHECK_EQ(applied[i].assign_pri, OMCI_HW_VLAN_PRI_KEEP);
		CHECK_EQ(applied[i].up_only, i > 0u);
	}

	CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x101u, 6u, r2, 8), OMCI_OK);
	CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x101u, 6u, r3, 8), OMCI_OK);
	CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x101u, 6u, r4, 8), OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(&applied), 0u);

	uint8_t xl[16], pt[16], xl2[16];

	/*
	 * A default rule that discards the single-tagged frames no rule takes
	 * is programmed in its place in list order. It matches no VID, and a
	 * push onto untagged frames, which matches none either, neither takes
	 * its place nor gives it up; of two such defaults, one is programmed.
	 */
	{
		uint8_t dflt[16], dflt2[16], push[16];

		clear_rows(mib);
		xlate_row(xl, 0u, 410u, 1410u);
		pack(dflt, 0xf8000000u, 0xe8000000u, 0xc00f0000u, 0x000f8000u);
		pack(dflt2, 0xf8000000u, 0xe8001000u, 0xc00f0000u, 0x000f8000u);
		pack(push, 0xf8000000u, 0xf8000000u, 0x000f0000u,
		     (5u << 16) | (36u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, push, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, dflt, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 3u);
		if (omci_hw_vlan_recorded(&applied) == 3u) {
			CHECK_EQ(applied[0].kind, OMCI_HW_VLAN_CVID_XLATE);
			CHECK_EQ(applied[1].kind, OMCI_HW_VLAN_DISCARD_TAGGED);
			CHECK_EQ(applied[1].match_cvid, 0u);
			CHECK_EQ(applied[2].kind, OMCI_HW_VLAN_CVID_PUSH);
			CHECK_EQ(applied[2].assign_cvid, 36u);
		}

		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, dflt2, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 3u);
		clear_rows(mib);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	}

	/*
	 * Rules on the EtherType of untagged frames come ahead of the default
	 * rule of untagged frames, which comes last in list order whatever its
	 * bytes (G.988 9.3.13); of two rules that push one VID, the later works
	 * upstream only. The switch refuses a rule by its VID and EtherType, so
	 * the other rules of untagged frames stay in force. A rule that leaves
	 * the untagged frames of an EtherType as they are, ahead of one that
	 * tags them, is left out; with no such rule after it, it needs nothing.
	 */
	{
		static const char pass_out[] =
			"untagged ethertype left as is ahead of a rule that tags it";
		uint8_t dflt[16], ppp[16], v6[16], pass[16];
		const struct omci_hw_vlan_xlate *in = NULL;
		char log[4096];

		clear_rows(mib);
		pack(dflt, 0xf8000000u, 0xf8000000u, 0x000f0000u,
		     (0u << 16) | (10u << 3));
		pack(ppp, 0xf8000000u, 0xf8000002u, 0x000f0000u,
		     (0u << 16) | (35u << 3));
		pack(v6, 0xf8000000u, 0xf8000004u, 0x000f0000u,
		     (0u << 16) | (10u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, dflt, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, ppp, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, v6, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 3u);
		if (omci_hw_vlan_recorded(&applied) == 3u) {
			CHECK_EQ(applied[0].kind, OMCI_HW_VLAN_CVID_PUSH);
			CHECK_EQ(applied[0].ethertype, OMCI_HW_ETHERTYPE_PPPOE);
			CHECK_EQ(applied[0].assign_cvid, 35u);
			CHECK(!applied[0].up_only);
			CHECK_EQ(applied[1].ethertype, OMCI_HW_ETHERTYPE_IPV6);
			CHECK_EQ(applied[1].assign_cvid, 10u);
			CHECK(!applied[1].up_only);
			CHECK_EQ(applied[2].ethertype, OMCI_HW_ETHERTYPE_ANY);
			CHECK_EQ(applied[2].assign_cvid, 10u);
			CHECK(applied[2].up_only);
		}

		omci_hw_vlan_reject_next(
			(uint16_t)(OMCI_HW_ETHERTYPE_PPPOE << 12));
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_DECLINED);
		CHECK(omci_vlan_row_refused(ppp, sizeof(ppp)));
		CHECK(!omci_vlan_row_refused(dflt, sizeof(dflt)));
		CHECK(!omci_vlan_row_refused(v6, sizeof(v6)));
		CHECK_EQ(omci_vlan_in_force(&in), 2u);

		pack(pass, 0xf8000000u, 0xf8000005u, 0x000f0000u, 0x000f0000u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, pass, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});
		CHECK(strstr(log, pass_out) != NULL);
		CHECK(omci_vlan_row_refused(pass, sizeof(pass)));
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 3u);

		CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x101u, 6u, dflt, 8),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK(!omci_vlan_row_refused(pass, sizeof(pass)));
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 2u);
		clear_rows(mib);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	}

	/*
	 * A rule that leaves the untagged frames of an EtherType as they are
	 * is judged by each rule after it that tags untagged frames: a push of
	 * a C-tag on another EtherType takes none of its frames, and it needs
	 * nothing; a push of a C-tag and an S-tag of any EtherType, here the
	 * default rule of untagged frames, takes them, and it is left out.
	 */
	{
		static const char pass_out[] =
			"untagged ethertype left as is ahead of a rule that tags it";
		uint8_t pass[16], v6[16], two[16];
		char log[4096];

		clear_rows(mib);
		/* untagged ARP: left as is */
		pack(pass, 0xf8000000u, 0xf8000003u, 0x000f0000u, 0x000f0000u);
		/* untagged IPv6: C-VID 36 */
		pack(v6, 0xf8000000u, 0xf8000004u, 0x000f0000u,
		     (0u << 16) | (36u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, pass, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, v6, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});
		CHECK(strstr(log, pass_out) == NULL);
		CHECK(!omci_vlan_row_refused(pass, sizeof(pass)));
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
		if (omci_hw_vlan_recorded(&applied) == 1u) {
			CHECK_EQ(applied[0].kind, OMCI_HW_VLAN_CVID_PUSH);
			CHECK_EQ(applied[0].ethertype, OMCI_HW_ETHERTYPE_IPV6);
		}

		/* untagged, the default: C-VID 410 at 5 behind S-VID 200 at 3 */
		pack(two, 0xf8000000u, 0xf8000000u, (3u << 16) | (200u << 3),
		     (5u << 16) | (410u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, two, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});
		CHECK(strstr(log, pass_out) != NULL);
		CHECK(omci_vlan_row_refused(pass, sizeof(pass)));
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 2u);
		if (omci_hw_vlan_recorded(&applied) == 2u) {
			CHECK_EQ(applied[0].ethertype, OMCI_HW_ETHERTYPE_IPV6);
			CHECK_EQ(applied[1].kind, OMCI_HW_VLAN_SCVID_PUSH);
			CHECK_EQ(applied[1].ethertype, OMCI_HW_ETHERTYPE_ANY);
		}
		clear_rows(mib);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	}

	/*
	 * The default rule of the double-tagged frames is one of the three
	 * default rules too: it comes last in list order, after a rule of
	 * single-tagged frames whose bytes sort after its own, and needs
	 * nothing from the hardware.
	 */
	{
		uint8_t dbl[16], t[16];
		const struct omci_vlan_fate_rec *f = NULL;

		clear_rows(mib);
		/* double-tagged, the default: left as they are */
		pack(dbl, 0xe8000000u, 0xe8000000u, 0x000f0000u, 0x000f0000u);
		xlate_row(t, 0u, 410u, 1410u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, dbl, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, t, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
		CHECK_EQ(omci_vlan_fates(&f, NULL), 2u);
		if (f != NULL && omci_vlan_fates(NULL, NULL) == 2u) {
			CHECK_EQ(f[0].fate, (uint8_t)OMCI_VLAN_FATE_PROGRAMMED);
			CHECK_EQ(f[0].match_cvid, 410u);
			CHECK_EQ(f[0].row, 1u);
			CHECK_EQ(f[1].fate, (uint8_t)OMCI_VLAN_FATE_INERT);
			CHECK_EQ(f[1].row, 0u);
		}
		clear_rows(mib);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	}

	/*
	 * A discard of the frames of a VID is programmed in its place in list
	 * order. Of any priority, it leaves the priorities that earlier rules
	 * take from its VID to them, and takes no frame when they take all
	 * eight. A rule after it on its VID is shadowed, and so is a discard
	 * after a rule that takes its frames. Two discards of different VIDs
	 * are no many-to-one mapping.
	 */
	{
		static const char after_discard[] =
			"me171: C-VID 410<->2000 comes after the discard of C-VID 410";
		static const char discard_after[] =
			"me171: the discard of C-VID 430 comes after the 430<->1430 rule";
		static const char takes_none[] =
			"me171: the discard of C-VID 440 takes no frame";
		uint8_t d[16], d2[16], t[16];
		char log[4096];

		clear_rows(mib);
		pri_xlate_row(t, 5u, 410u, 1410u);
		discard_row(d, 0u, 8u, 410u);
		discard_row(d2, 0u, 8u, 420u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, d, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, d2, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, t, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 3u);
		if (omci_hw_vlan_recorded(&applied) == 3u) {
			CHECK_EQ(applied[0].kind, OMCI_HW_VLAN_CVID_XLATE);
			CHECK_EQ(applied[0].match_pri, 5u);
			CHECK_EQ(applied[1].kind, OMCI_HW_VLAN_DISCARD_VID);
			CHECK_EQ(applied[1].match_cvid, 410u);
			CHECK(!applied[1].match_has_pri);
			CHECK_EQ(applied[1].keep_pri, 1u << 5);
			CHECK(!applied[1].up_only);
			CHECK_EQ(applied[2].kind, OMCI_HW_VLAN_DISCARD_VID);
			CHECK_EQ(applied[2].match_cvid, 420u);
			CHECK_EQ(applied[2].keep_pri, 0u);
			CHECK(!applied[2].up_only);
		}
		CHECK(omci_vlan_has_room(mib, 171u, 6u, d, sizeof(d)));

		xlate_row(t, 6u, 410u, 2000u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, t, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});
		CHECK(strstr(log, after_discard) != NULL);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 3u);

		clear_rows(mib);
		xlate_row(t, 0u, 430u, 1430u);
		discard_row(d, 5u, 8u, 430u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, t, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, d, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});
		CHECK(strstr(log, discard_after) != NULL);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);

		clear_rows(mib);
		for (unsigned int p = 0; p < 8u; p++) {
			pri_xlate_row(t, p, 440u, 1440u);
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, t,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		discard_row(d, 0u, 8u, 440u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, d, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});
		CHECK(strstr(log, takes_none) != NULL);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 8u);
		{
			const struct omci_vlan_fate_rec *f = NULL;
			const size_t nf = omci_vlan_fates(&f, NULL);

			CHECK_EQ(nf, 9u);
			if (nf == 9u)
				CHECK_EQ(f[8].fate,
					 (uint8_t)OMCI_VLAN_FATE_SHADOWED);
		}
		clear_rows(mib);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	}

	/*
	 * A push of a C-tag and an S-tag takes the untagged frames, as a push of
	 * a C-tag does, which after it is shadowed. After an S-VID push into the
	 * same S-VID it works upstream only, the way back being the earlier
	 * rule's.
	 */
	{
		uint8_t two[16], one[16], spush[16];
		const struct omci_vlan_fate_rec *f = NULL;

		clear_rows(mib);
		pack(two, 0xf8000000u, 0xf8000000u, (3u << 16) | (200u << 3),
		     (5u << 16) | (410u << 3));
		pack(one, 0xf8000000u, 0xf8001000u, 0x000f0000u,
		     (4u << 16) | (36u << 3));
		pack(spush, 0xf8000000u, (8u << 28) | (100u << 15),
		     (0u << 30) | (2u << 16) | (200u << 3),
		     (8u << 16) | (4096u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, two, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, one, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, spush, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 2u);
		if (omci_hw_vlan_recorded(&applied) == 2u) {
			CHECK_EQ(applied[0].kind, OMCI_HW_VLAN_SVID_PUSH);
			CHECK_EQ(applied[0].match_cvid, 100u);
			CHECK(!applied[0].up_only);
			CHECK_EQ(applied[1].kind, OMCI_HW_VLAN_SCVID_PUSH);
			CHECK_EQ(applied[1].match_cvid, 0u);
			CHECK_EQ(applied[1].assign_cvid, 410u);
			CHECK(applied[1].inner_has_pri);
			CHECK_EQ(applied[1].inner_pri, 5u);
			CHECK_EQ(applied[1].assign_svid, 200u);
			CHECK_EQ(applied[1].assign_pri, 3u);
			CHECK(applied[1].up_only);
		}
		CHECK_EQ(omci_vlan_fates(&f, NULL), 3u);
		if (f != NULL && omci_vlan_fates(NULL, NULL) == 3u) {
			CHECK_EQ(f[1].net_vid, 200u);
			CHECK_EQ(f[2].fate, (uint8_t)OMCI_VLAN_FATE_SHADOWED);
			CHECK_EQ(f[2].rival_net, 200u);
		}
		clear_rows(mib);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	}

	clear_rows(mib);
	xlate_row(xl, 0u, 410u, 1410u);
	passthru_row(pt, 1u, 410u);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, pt, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
	if (omci_hw_vlan_recorded(&applied) == 1u) {
		CHECK_EQ(applied[0].match_cvid, 410u);
		CHECK_EQ(applied[0].assign_cvid, 1410u);
	}

	clear_rows(mib);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, pt, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
	if (omci_hw_vlan_recorded(&applied) == 1u) {
		CHECK_EQ(applied[0].match_cvid, 410u);
		CHECK_EQ(applied[0].assign_cvid, 1410u);
	}

	clear_rows(mib);
	xlate_row(xl, 0u, 410u, 1410u);
	xlate_row(xl2, 1u, 410u, 2000u);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl2, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
	if (omci_hw_vlan_recorded(&applied) == 1u)
		CHECK_EQ(applied[0].assign_cvid, 1410u);

	clear_rows(mib);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl2, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	/* List order decides, sorted on the filter, not the order of the Sets. */
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
	if (omci_hw_vlan_recorded(&applied) == 1u)
		CHECK_EQ(applied[0].assign_cvid, 1410u);

	clear_rows(mib);
	xlate_row(xl, 0u, 410u, 1410u);
	xlate_row(xl2, 0u, 411u, 1410u);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl2, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	/*
	 * Two VIDs to one: both are translated upstream, and the way back is
	 * the first rule's (G.988 9.3.13, many-to-one mappings).
	 */
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(&applied), 2u);
	if (omci_hw_vlan_recorded(&applied) == 2u) {
		CHECK_EQ(applied[0].match_cvid, 410u);
		CHECK_EQ(applied[0].assign_cvid, 1410u);
		CHECK(!applied[0].up_only);
		CHECK_EQ(applied[1].match_cvid, 411u);
		CHECK_EQ(applied[1].assign_cvid, 1410u);
		CHECK(applied[1].up_only);
	}

	clear_rows(mib);
	{
		uint8_t qinq[16];

		pack(qinq, 0xf8000000u, (6u << 28) | (410u << 15),
		     (0u << 30) | (6u << 16) | (100u << 3),
		     (6u << 16) | (410u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, qinq, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		unsigned int refused = 99u;

		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, &refused), OMCI_OK);
		CHECK_EQ(refused, 0u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
		if (omci_hw_vlan_recorded(&applied) == 1u) {
			CHECK_EQ(applied[0].kind, OMCI_HW_VLAN_SVID_PUSH);
			CHECK_EQ(applied[0].match_cvid, 410u);
			CHECK_EQ(applied[0].assign_svid, 100u);

			CHECK_EQ(applied[0].assign_cvid, 0u);
		}
	}

	/*
	 * Two S-tags of one S-VID in front of different C-tags share it: the
	 * C-tag behind tells them apart, and one pop undoes both. An S-tag and
	 * a C-tag of one VID do not clash either; two C-tags of one VID do.
	 */
	clear_rows(mib);
	{
		uint8_t s410[16], s36[16], xl40[16];

		pack(s410, 0xf8000000u, (6u << 28) | (410u << 15),
		     (6u << 16) | (100u << 3), (6u << 16) | (410u << 3));
		pack(s36, 0xf8000000u, (6u << 28) | (36u << 15),
		     (6u << 16) | (100u << 3), (6u << 16) | (36u << 3));
		xlate_row(xl40, 0u, 40u, 100u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, s410, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, s36, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl40, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 3u);
		if (omci_hw_vlan_recorded(&applied) == 3u) {
			unsigned int s_pushes = 0, xlates = 0;

			for (unsigned int i = 0; i < 3u; i++) {
				if (applied[i].kind == OMCI_HW_VLAN_SVID_PUSH &&
				    applied[i].assign_svid == 100u)
					s_pushes++;
				if (applied[i].kind ==
					    OMCI_HW_VLAN_CVID_XLATE &&
				    applied[i].assign_cvid == 100u)
					xlates++;
			}
			CHECK_EQ(s_pushes, 2u);
			CHECK_EQ(xlates, 1u);
		}

		uint8_t xl41[16];

		/* A second C-VID to C-VID 100 works upstream only. */
		xlate_row(xl41, 0u, 41u, 100u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl41, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 4u);
		if (omci_hw_vlan_recorded(&applied) == 4u) {
			CHECK_EQ(applied[3].match_cvid, 41u);
			CHECK(applied[3].up_only);
		}
	}

	clear_rows(mib);
	{
		uint8_t push[16];

		pack(push, 0xf8000000u, 0xf8000000u, 0x000f0000u,
		     (5u << 16) | (410u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, push, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
		if (omci_hw_vlan_recorded(&applied) == 1u) {
			CHECK_EQ(applied[0].kind, OMCI_HW_VLAN_CVID_PUSH);
			CHECK_EQ(applied[0].match_cvid, 0u);
			CHECK_EQ(applied[0].assign_cvid, 410u);
			CHECK_EQ(applied[0].assign_pri, 5u);
		}
	}

	clear_rows(mib);
	{
		uint8_t bad[16];

		pack(bad, 0xf8000000u, (6u << 28) | (410u << 15) | 7u,
		     0x400f0000u, (6u << 16) | (1410u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, bad, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		unsigned int refused = 0;

		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 0u);
	}
	clear_rows(mib);

	clear_rows(mib);
	xlate_row(xl, 0u, 410u, 1410u);
	xlate_row(xl2, 0u, 100u, 200u);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, xl2, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(&applied), 2u);

	clear_rows(mib);
	{
		const unsigned int CAP = OMCI_HW_VLAN_MAX_RULES;
		const unsigned int WANT = CAP + 8u;

		for (unsigned int i = 0; i < WANT; i++) {
			uint8_t many[16];

			xlate_row(many, 0u, 100u + i, 2000u + i);
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, many,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		CHECK_EQ(omci_mib_table_count(mib, 171u, 0x101u, 6u), WANT);

		unsigned int refused = 0;
		char log[512];

		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, &refused),
				 OMCI_OK);
		});

		CHECK_EQ(omci_hw_vlan_recorded(&applied), CAP);

		CHECK_EQ(refused, WANT - CAP);

		CHECK_EQ(omci_t_log_count(log, "NOT programmed"), 1u);
	}
	clear_rows(mib);

	{
		CHECK_EQ(omci_mib_create(mib, 171u, 0x102u), OMCI_OK);
		CHECK_EQ(omci_mib_create(mib, 264u, 0x101u), OMCI_OK);

		uint8_t a[16], b[16];

		xlate_row(a, 0u, 410u, 1410u);
		xlate_row(b, 0u, 40u, 1601u);

		uint8_t unprog[16];

		pack(unprog, 0xf8000000u, (6u << 28) | (36u << 15) | 7u,
		     0x400f0000u, (6u << 16) | (1502u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, unprog, 16,
					    8, OMCI_MIB_TABLE_LAST),
			 OMCI_OK);

		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, a, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		unsigned int once = 0;

		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, &once), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
		CHECK_EQ(once, 1u);

		CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x101u, 6u, unprog,
					       8),
			 OMCI_OK);

		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x102u, 6u, b, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);

		CHECK_EQ(omci_hw_vlan_recorded(&applied), 2u);
		bool saw_a = false, saw_b = false;

		for (size_t i = 0; i < 2u; i++) {
			if (applied[i].match_cvid == 410u &&
			    applied[i].assign_cvid == 1410u)
				saw_a = true;
			if (applied[i].match_cvid == 40u &&
			    applied[i].assign_cvid == 1601u)
				saw_b = true;
		}
		CHECK(saw_a);
		CHECK(saw_b);

		CHECK_EQ(omci_mib_delete(mib, 264u, 0x101u), OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 2u);

		CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x101u, 6u, a, 8),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&applied), 1u);
		CHECK_EQ(applied[0].match_cvid, 40u);
		CHECK_EQ(applied[0].assign_cvid, 1601u);

		CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x102u, 6u, b, 8),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, 171u, 0x102u), OMCI_OK);
	}
	clear_rows(mib);

	{
		uint8_t stub[4] = { 0, 0, 0, 0 };

		CHECK(omci_vlan_has_room(mib, 171u, 6u, stub, sizeof(stub)));
		CHECK(omci_vlan_has_room(mib, 171u, 6u, NULL, 0));
	}

	clear_rows(mib);
	{
		const struct omci_vlan_fate_rec *f = NULL;
		unsigned int over = 99u;
		char log[1024];
		uint8_t fr[16];

		xlate_row(fr, 6u, 410u, 1410u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		xlate_row(fr, 5u, 410u, 1410u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		xlate_row(fr, 0u, 410u, 1410u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		xlate_row(fr, 4u, 410u, 1900u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);

		pack(fr, 0xf8000000u, 0xf8000000u, (3u << 30) | (15u << 16),
		     (15u << 16));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);

		/* A discard of the double-tagged frames of VID 44: unsupported. */
		pack(fr, (2u << 28) | (44u << 15), (2u << 28) | (44u << 15),
		     0xc00f0000u, (2u << 16) | (55u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);

		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});

		static const char conflict[] =
			"me171: C-VID 410<->1900 conflicts with the existing 410<->1410 rule";

		CHECK(strstr(log, conflict) != NULL);
		{
			unsigned int said = 0;
			const char *at = log;

			while ((at = strstr(at,
					    "conflicts with the existing")) !=
			       NULL) {
				said++;
				at++;
			}
			CHECK_EQ(said, 1u);
		}

		/*
		 * In list order, sorted on the filter: the unsupported row, whose
		 * filter names an outer tag, first, then the four rows of VID 410
		 * by their TPID field, 0, 4, 5, 6, and last the row that names no
		 * tag.
		 */
		static const uint8_t listed[6] = { 5u, 2u, 3u, 1u, 0u, 4u };

		CHECK_EQ(omci_vlan_fates(&f, &over), 6u);
		CHECK_EQ(over, 0u);
		CHECK(f != NULL);
		if (f != NULL && omci_vlan_fates(NULL, NULL) == 6u) {
			CHECK_EQ(f[0].fate,
				 (uint8_t)OMCI_VLAN_FATE_UNSUPPORTED);

			CHECK_EQ(f[1].fate, (uint8_t)OMCI_VLAN_FATE_PROGRAMMED);
			CHECK_EQ(f[1].match_cvid, 410u);
			CHECK_EQ(f[1].net_vid, 1410u);

			CHECK_EQ(f[2].fate, (uint8_t)OMCI_VLAN_FATE_SHADOWED);
			CHECK_EQ(f[2].net_vid, 1900u);
			CHECK_EQ(f[2].rival_match, 410u);
			CHECK_EQ(f[2].rival_net, 1410u);

			CHECK_EQ(f[3].fate, (uint8_t)OMCI_VLAN_FATE_COLLAPSED);
			CHECK_EQ(f[4].fate, (uint8_t)OMCI_VLAN_FATE_COLLAPSED);
			CHECK_EQ(f[5].fate, (uint8_t)OMCI_VLAN_FATE_INERT);

			for (size_t i = 0; i < 6u; i += 5u) {
				CHECK_EQ(f[i].net_vid, 0u);
				CHECK_EQ(f[i].rival_match, 0u);
				CHECK_EQ(f[i].rival_net, 0u);
			}

			for (size_t i = 0; i < 6u; i++) {
				CHECK_EQ(f[i].instance, 0x101u);
				CHECK_EQ(f[i].row, listed[i]);
			}
		}
		CHECK(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_SHADOWED),
			     "shadowed") == 0);

		pack(fr, 0xf8000000u, (7u << 28) | (88u << 15), 0x400f0000u,
		     (7u << 16) | (1888u << 3));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_mib_table_count(mib, 171u, 0x101u, 6u), 7u);

		xlate_row(fr, 1u, 99u, 1999u);
		CHECK(omci_vlan_has_room(mib, 171u, 6u, fr, 16));
		CHECK_EQ(omci_vlan_fates(&f, &over), 6u);
		CHECK_EQ(over, 0u);
		if (f != NULL && omci_vlan_fates(NULL, NULL) == 6u)
			CHECK_EQ(f[2].fate, (uint8_t)OMCI_VLAN_FATE_SHADOWED);

		printf("    every ME 171 row's fate is retrievable, and a dry run does not disturb it\n");
	}

	clear_rows(mib);
	{
		const struct omci_vlan_fate_rec *f = NULL;
		unsigned int over = 0;
		char log[1024];
		uint8_t rr[16];

		xlate_row(rr, 0u, 10u, 1010u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, rr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		xlate_row(rr, 0u, 20u, 1020u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, rr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		xlate_row(rr, 0u, 30u, 1030u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, rr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		xlate_row(rr, 1u, 30u, 1999u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, rr, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);

		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});

		CHECK_EQ(omci_vlan_fates(&f, &over), 4u);
		CHECK_EQ(over, 0u);
		if (f != NULL && omci_vlan_fates(NULL, NULL) == 4u) {
			CHECK_EQ(f[3].fate, (uint8_t)OMCI_VLAN_FATE_SHADOWED);
			CHECK_EQ(f[3].match_cvid, 30u);
			CHECK_EQ(f[3].net_vid, 1999u);
			CHECK_EQ(f[3].rival_match, 30u);
			CHECK_EQ(f[3].rival_net, 1030u);
		}
		printf("    a shadowed row names the entry that actually beat it, not the first one\n");
	}

	clear_rows(mib);
	{
		const struct omci_vlan_fate_rec *f = NULL;
		unsigned int over = 99u, budget = 0;
		char log[1024];

		for (unsigned int k = 0; k < OMCI_HW_VLAN_MAX_RULES + 1u; k++) {
			uint8_t br[16];

			xlate_row(br, 0u, 100u + k, 2000u + k);
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, br,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});

		const size_t nf = omci_vlan_fates(&f, &over);

		CHECK_EQ(nf, (size_t)OMCI_HW_VLAN_MAX_RULES + 1u);
		CHECK_EQ(over, 0u);
		for (size_t i = 0; i < nf && f != NULL; i++)
			if (f[i].fate == (uint8_t)OMCI_VLAN_FATE_OVER_BUDGET)
				budget++;
		CHECK_EQ(budget, 1u);
		if (f != NULL && nf == (size_t)OMCI_HW_VLAN_MAX_RULES + 1u) {
			CHECK_EQ(f[OMCI_HW_VLAN_MAX_RULES].fate,
				 (uint8_t)OMCI_VLAN_FATE_OVER_BUDGET);
			CHECK_EQ(f[OMCI_HW_VLAN_MAX_RULES].match_cvid,
				 100u + OMCI_HW_VLAN_MAX_RULES);
		}
		printf("    the one row the classifier had no room for is named, not just counted\n");
	}

	clear_rows(mib);
	{
		const struct omci_vlan_fate_rec *f = NULL;
		unsigned int over = 0;
		char log[1024];
		const unsigned int rows_per_inst = OMCI_HW_VLAN_MAX_RULES;
		const unsigned int instances = OMCI_VLAN_FATE_INSTANCES + 1u;

		for (unsigned int inst = 0; inst < instances; inst++) {
			const uint16_t id = (uint16_t)(0x0201u + inst);

			CHECK_EQ(omci_mib_create(mib, 171u, id), OMCI_OK);
			for (unsigned int k = 0; k < rows_per_inst; k++) {
				uint8_t or_[16];

				xlate_row(or_, 0u,
					  1u + inst * rows_per_inst + k,
					  3000u + inst * rows_per_inst + k);
				CHECK_EQ(omci_mib_table_put(
						 mib, 171u, id, 6u, or_, 16, 8,
						 OMCI_MIB_TABLE_LAST),
					 OMCI_OK);
			}
		}
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL),
				 OMCI_OK);
		});

		CHECK_EQ(omci_vlan_fates(&f, &over),
			 (size_t)OMCI_VLAN_FATE_MAX);
		CHECK_EQ(over, instances * rows_per_inst - OMCI_VLAN_FATE_MAX);
		printf("    %u rows did not fit the fate snapshot, and the count says so\n",
		       over);

		for (unsigned int inst = 0; inst < instances; inst++) {
			const uint16_t id = (uint16_t)(0x0201u + inst);

			while (omci_mib_table_count(mib, 171u, id, 6u) > 0) {
				uint8_t gone[16];

				CHECK_EQ(omci_mib_table_get(mib, 171u, id, 6u,
							    0, gone,
							    sizeof(gone)),
					 OMCI_OK);
				CHECK_EQ(omci_mib_table_remove(mib, 171u, id,
							       6u, gone, 8),
					 OMCI_OK);
			}
			CHECK_EQ(omci_mib_delete(mib, 171u, id), OMCI_OK);
		}
	}

	{
		static const struct {
			enum omci_vlan_fate f;
			const char *name;
		} names[] = {
			{ OMCI_VLAN_FATE_PROGRAMMED, "programmed" },
			{ OMCI_VLAN_FATE_COLLAPSED, "collapsed" },
			{ OMCI_VLAN_FATE_SHADOWED, "shadowed" },
			{ OMCI_VLAN_FATE_OVER_BUDGET, "over-budget" },
			{ OMCI_VLAN_FATE_UNSUPPORTED, "unsupported" },
			{ OMCI_VLAN_FATE_INERT, "inert" },
		};
		for (size_t i = 0; i < ARRAY_SIZE(names); i++)
			CHECK(strcmp(omci_vlan_fate_name(names[i].f),
				     names[i].name) == 0);
		CHECK(strcmp(omci_vlan_fate_name((enum omci_vlan_fate)99),
			     "?") == 0);
	}

	clear_rows(mib);
	{
		uint8_t fr2[16];

		xlate_row(fr2, 0u, 61u, 1610u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr2, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);

		const struct omci_hw_vlan_xlate *ap2 = NULL;

		CHECK_EQ(omci_hw_vlan_recorded(&ap2), 1u);

		xlate_row(fr2, 0u, 62u, 1620u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, fr2, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		omci_hw_vlan_fail_next();
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_ERR_HW);

		CHECK_EQ(omci_hw_vlan_recorded(&ap2), 1u);
		if (omci_hw_vlan_recorded(&ap2) == 1u)
			CHECK_EQ(ap2[0].match_cvid, 61u);

		{
			const struct omci_vlan_fate_rec *kept = NULL;
			unsigned int over = 99u;

			CHECK_EQ(omci_vlan_fates(&kept, &over), 1u);
			CHECK_EQ(over, 0u);
			if (kept != NULL && omci_vlan_fates(NULL, NULL) == 1u) {
				CHECK_EQ(kept[0].match_cvid, 61u);
				CHECK_EQ(kept[0].fate,
					 (uint8_t)OMCI_VLAN_FATE_PROGRAMMED);
			}
		}

		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&ap2), 2u);
		CHECK_EQ(omci_vlan_fates(NULL, NULL), 2u);

		CHECK_EQ(omci_vlan_last_reconcile(NULL, NULL), 2u);
		printf("    a failed apply is reported, and nothing is remembered as programmed\n");
	}

	clear_rows(mib);
	{
		const struct omci_hw_vlan_xlate *ap3 = NULL;
		uint8_t rr3[16];

		if (!omci_mib_exists(mib, 171u, 0x102u))
			CHECK_EQ(omci_mib_create(mib, 171u, 0x102u), OMCI_OK);

		for (unsigned int k = 0; k < OMCI_HW_VLAN_MAX_RULES; k++) {
			xlate_row(rr3, 0u, 700u + k, 1700u + k);
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x102u, 6u, rr3,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&ap3),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		for (unsigned int k = 0; k < OMCI_HW_VLAN_MAX_RULES; k++) {
			xlate_row(rr3, 0u, 700u + k, 1900u + k);
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x102u, 6u, rr3,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		omci_hw_vlan_fail_next();
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_ERR_HW);

		{
			const struct omci_vlan_fate_rec *fnow = NULL;
			unsigned int overnow = 0;
			const size_t nnow = omci_vlan_fates(&fnow, &overnow);
			unsigned int phantom = 0;

			for (size_t i = 0; i < nnow; i++)
				if (fnow[i].net_vid >= 1900u &&
				    fnow[i].net_vid < 1916u &&
				    fnow[i].fate ==
					    (uint8_t)OMCI_VLAN_FATE_PROGRAMMED)
					phantom++;
			CHECK_EQ(phantom, 0u);
		}

		xlate_row(rr3, 0u, 301u, 1301u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, rr3, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);

		unsigned int found_301 = 0;
		const size_t n3 = omci_hw_vlan_recorded(&ap3);

		for (size_t i = 0; i < n3; i++)
			if (ap3[i].match_cvid == 301u &&
			    ap3[i].assign_cvid == 1301u)
				found_301++;
		CHECK_EQ(found_301, 1u);

		const struct omci_vlan_fate_rec *ff = NULL;
		unsigned int ffover = 0;
		const size_t ffn = omci_vlan_fates(&ff, &ffover);
		unsigned int claimed_1900 = 0;

		for (size_t i = 0; i < ffn; i++)
			if (ff[i].net_vid >= 1900u && ff[i].net_vid < 1916u &&
			    ff[i].fate == (uint8_t)OMCI_VLAN_FATE_PROGRAMMED)
				claimed_1900++;
		CHECK(claimed_1900 <= n3);
		printf("    a failed apply does not put its rules into the in-force set\n");

		while (omci_mib_table_count(mib, 171u, 0x102u, 6u) > 0) {
			uint8_t gone[16];

			CHECK_EQ(omci_mib_table_get(mib, 171u, 0x102u, 6u, 0,
						    gone, sizeof(gone)),
				 OMCI_OK);
			CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x102u, 6u,
						       gone, 8),
				 OMCI_OK);
		}
		CHECK_EQ(omci_mib_delete(mib, 171u, 0x102u), OMCI_OK);
	}

	clear_rows(mib);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	{
		const struct omci_hw_vlan_xlate *ap4 = NULL;
		const struct omci_vlan_fate_rec *f = NULL;
		unsigned int over = 99u;
		uint8_t r16[16];

		CHECK_EQ(omci_mib_create(mib, 171u, 0x102u), OMCI_OK);
		for (unsigned int k = 0; k < OMCI_HW_VLAN_MAX_RULES; k++) {
			xlate_row(r16, 0u, 700u + k, 1700u + k);
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x102u, 6u, r16,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		CHECK_EQ(omci_hw_vlan_recorded(&ap4),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);

		pack(r16, 0xf8000000u, 0xf8000000u, (3u << 30) | (15u << 16),
		     (15u << 16));
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, r16, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		xlate_row(r16, 0u, 410u, 1410u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, r16, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);

		const size_t nf = omci_vlan_fates(&f, &over);

		CHECK_EQ(nf, (size_t)OMCI_HW_VLAN_MAX_RULES + 2u);
		CHECK_EQ(over, 0u);
		if (f != NULL && nf == (size_t)OMCI_HW_VLAN_MAX_RULES + 2u) {
			for (size_t i = 0; i < OMCI_HW_VLAN_MAX_RULES; i++) {
				CHECK_EQ(f[i].instance, 0x102u);
				CHECK_EQ(f[i].fate,
					 (uint8_t)OMCI_VLAN_FATE_PROGRAMMED);
			}
			/* The translation sorts before the row that names no tag. */
			CHECK_EQ(f[OMCI_HW_VLAN_MAX_RULES].instance, 0x101u);
			CHECK_EQ(f[OMCI_HW_VLAN_MAX_RULES].fate,
				 (uint8_t)OMCI_VLAN_FATE_OVER_BUDGET);
			CHECK_EQ(f[OMCI_HW_VLAN_MAX_RULES].match_cvid, 410u);
			CHECK_EQ(f[OMCI_HW_VLAN_MAX_RULES + 1u].instance,
				 0x101u);
			CHECK_EQ(f[OMCI_HW_VLAN_MAX_RULES + 1u].fate,
				 (uint8_t)OMCI_VLAN_FATE_INERT);
		}
		printf("    a row that translates nothing is listed after the translations that were in force\n");

		/*
		 * Taking the rule in force first can cost rules: a translation
		 * of VID 5 for any priority, in force, shadows those of VID 5
		 * for priorities 0 and 1, which list order takes before it.
		 * When that programs fewer rules than list order would, list
		 * order wins.
		 */
		clear_rows(mib);
		CHECK_EQ(omci_mib_delete(mib, 171u, 0x102u), OMCI_OK);

		uint8_t y[16];
		unsigned int yielded_before = 0, yielded_after = 0;

		xlate_row(y, 0u, 5u, 100u);
		CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, y, 16, 8,
					    OMCI_MIB_TABLE_LAST),
			 OMCI_OK);
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
		(void)omci_vlan_last_reconcile(&yielded_before, NULL);

		for (unsigned int p = 0; p < 2u; p++) {
			pack(y, 0xf8000000u, (p << 28) | (5u << 15),
			     0x400f0000u, (8u << 16) | ((200u + p) << 3));
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, y,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		for (unsigned int k = 0; k < 14u; k++) {
			xlate_row(y, 0u, 20u + k, 1020u + k);
			CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, y,
						    16, 8, OMCI_MIB_TABLE_LAST),
				 OMCI_OK);
		}
		CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);

		size_t preferred = 0;
		const size_t plain =
			omci_vlan_last_reconcile(&yielded_after, &preferred);

		CHECK_EQ(yielded_after, yielded_before + 1u);
		CHECK_EQ(plain, (size_t)OMCI_HW_VLAN_MAX_RULES);
		CHECK_EQ(preferred, (size_t)OMCI_HW_VLAN_MAX_RULES - 1u);
		CHECK_EQ(omci_hw_vlan_recorded(&ap4),
			 (size_t)OMCI_HW_VLAN_MAX_RULES);
		if (omci_hw_vlan_recorded(&ap4) == OMCI_HW_VLAN_MAX_RULES) {
			CHECK(ap4[0].match_has_pri && ap4[0].match_pri == 0u);
			CHECK(ap4[1].match_has_pri && ap4[1].match_pri == 1u);
			CHECK_EQ(ap4[2].match_cvid, 5u);
			CHECK(!ap4[2].match_has_pri);
		}
		printf("    in-force rules that would shadow more than they keep give way to list order\n");
		CHECK_EQ(omci_mib_create(mib, 171u, 0x102u), OMCI_OK);

		clear_rows(mib);
		while (omci_mib_table_count(mib, 171u, 0x102u, 6u) > 0) {
			uint8_t gone[16];

			CHECK_EQ(omci_mib_table_get(mib, 171u, 0x102u, 6u, 0,
						    gone, sizeof(gone)),
				 OMCI_OK);
			CHECK_EQ(omci_mib_table_remove(mib, 171u, 0x102u, 6u,
						       gone, 8),
				 OMCI_OK);
		}
		CHECK_EQ(omci_mib_delete(mib, 171u, 0x102u), OMCI_OK);
	}

	omci_mib_fini(mib);

	return OMCI_T_REPORT("test_vlan");
}
