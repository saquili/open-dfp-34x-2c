// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the bridge port filters: the rules that the filter preassign
 * table, the MAC filter table and the PPPoE filter ask for, and how they
 * reach the backend.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/filter.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"

#include "harness.h"

#define ME_PPTP_ETH 11u
#define ME_BRIDGE 45u
#define ME_BRIDGE_PORT 47u
#define ME_MAC_FILTER 49u
#define ME_PREASSIGN 79u

#define MT_CREATE 4u
#define MT_DELETE 6u
#define MT_SET 8u
#define R_OK 0u

/* The TP types of a bridge port. */
#define TP_PPTP 1u
#define TP_IP_HOST 2u
#define TP_MAPPER 3u
#define TP_GEM_IW 5u
#define TP_MCAST_GEM 6u
#define TP_VEIP 11u

/* The filter byte of a row: add, and filter or source. */
#define ADD 0x40u
#define FILTER 0x01u
#define SOURCE 0x02u

static const uint8_t g_all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void port(struct omci_mib *mib, uint16_t inst, uint8_t tp_type,
		 bool preassign)
{
	CHECK_EQ(omci_mib_create(mib, ME_BRIDGE_PORT, inst), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set_by_olt(mib, ME_BRIDGE_PORT, inst, 3u,
					  &tp_type, 1u),
		 OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ME_MAC_FILTER, inst), OMCI_OK);
	if (preassign)
		CHECK_EQ(omci_mib_create(mib, ME_PREASSIGN, inst), OMCI_OK);
}

static void group(struct omci_mib *mib, uint16_t inst, uint8_t attr)
{
	const uint8_t on = 1u;

	CHECK_EQ(omci_mib_attr_set_by_olt(mib, ME_PREASSIGN, inst, attr, &on,
					  1u),
		 OMCI_OK);
}

static void row(struct omci_mib *mib, uint16_t inst, uint8_t entry,
		uint8_t filter_byte, uint8_t last)
{
	const uint8_t r[8] = { entry, filter_byte, 0x00u, 0x00u,
			       0x5eu, 0x00u,	   0x53u, last };

	CHECK_EQ(omci_mib_table_put(mib, ME_MAC_FILTER, inst, 1u, r, sizeof(r),
				    1u, OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
}

static void pppoe(struct omci_mib *mib, uint8_t on)
{
	if (!omci_mib_exists(mib, ME_PPTP_ETH, 0x0101u))
		CHECK_EQ(omci_mib_create(mib, ME_PPTP_ETH, 0x0101u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set_by_olt(mib, ME_PPTP_ETH, 0x0101u, 14u, &on,
					  1u),
		 OMCI_OK);
}

static bool is_drop_to(const struct omci_hw_filter *f, bool upstream,
		       const uint8_t *mac, const uint8_t *mask,
		       uint16_t ethertype)
{
	static const uint8_t none[6];

	return f->upstream == upstream && !f->pass &&
	       f->ethertype == ethertype && memcmp(f->dmac, mac, 6) == 0 &&
	       memcmp(f->dmask, mask, 6) == 0 && memcmp(f->smask, none, 6) == 0;
}

static void test_no_filter_asks_for_no_rule(struct omci_mib *mib)
{
	struct omci_hw_filter out[4];

	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 0u);
	port(mib, 0x0001u, TP_PPTP, true);
	port(mib, 0x0002u, TP_GEM_IW, false);
	pppoe(mib, 0u);
	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 0u);
}

static void test_a_group_drops_on_the_side_of_its_port(struct omci_mib *mib)
{
	static const uint8_t ipv4_mc[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x00 };
	static const uint8_t ipv4_mc_mask[6] = { 0xff, 0xff, 0xff,
						 0x80, 0x00, 0x00 };
	struct omci_hw_filter out[8];

	/* ARP downstream for a UNI-side port, and again for the VEIP: once. */
	port(mib, 0x0001u, TP_PPTP, true);
	group(mib, 0x0001u, 9u);
	port(mib, 0x0002u, TP_GEM_IW, true);
	group(mib, 0x0002u, 1u);
	group(mib, 0x0002u, 9u);
	port(mib, 0x0003u, TP_VEIP, true);
	group(mib, 0x0003u, 9u);
	/* A port on neither side filters nothing. */
	port(mib, 0x0004u, TP_IP_HOST, true);
	group(mib, 0x0004u, 2u);

	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 3u);
	CHECK(is_drop_to(&out[0], false, g_all, g_all, 0x0806u));
	CHECK(is_drop_to(&out[1], true, ipv4_mc, ipv4_mc_mask, 0u));
	CHECK(is_drop_to(&out[2], true, g_all, g_all, 0x0806u));
}

static void test_every_group_has_its_addresses(struct omci_mib *mib)
{
	static const uint8_t netbeui[6] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 };
	static const uint8_t bpdu[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 };
	static const uint8_t bpdu_mask[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0x00
	};
	struct omci_hw_filter out[16];

	port(mib, 0x0001u, TP_MAPPER, true);
	port(mib, 0x0002u, TP_MCAST_GEM, true);
	for (uint8_t attr = 1u; attr <= 10u; attr++)
		group(mib, 0x0001u, attr);
	group(mib, 0x0002u, 6u);

	/* Ten groups take fifteen addresses; the second port repeats one. */
	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 15u);
	CHECK(is_drop_to(&out[7], true, netbeui, g_all, 0u));
	CHECK(is_drop_to(&out[12], true, bpdu, bpdu_mask, 0u));
	CHECK(is_drop_to(&out[14], true, g_all, g_all, 0x8863u));
}

static void test_the_mac_table_drops_and_admits(struct omci_mib *mib)
{
	struct omci_hw_filter out[8];

	/* On the PON side: drop to .10 and from .11, admit to .12, from .13. */
	port(mib, 0x0002u, TP_GEM_IW, false);
	row(mib, 0x0002u, 0u, ADD | FILTER, 0x10u);
	row(mib, 0x0002u, 1u, ADD | FILTER | SOURCE, 0x11u);
	row(mib, 0x0002u, 2u, ADD, 0x12u);
	row(mib, 0x0002u, 3u, ADD | SOURCE, 0x13u);
	/* On the UNI side a drop applies and an admitting entry does not. */
	port(mib, 0x0001u, TP_PPTP, false);
	row(mib, 0x0001u, 0u, ADD | FILTER, 0x20u);
	row(mib, 0x0001u, 1u, ADD, 0x21u);

	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 7u);
	CHECK(out[0].upstream && !out[0].pass && out[0].dmac[5] == 0x10u &&
	      memcmp(out[0].dmask, g_all, 6) == 0);
	CHECK(out[1].upstream && !out[1].pass && out[1].smac[5] == 0x11u &&
	      memcmp(out[1].smask, g_all, 6) == 0 && out[1].dmask[0] == 0u);
	CHECK(!out[2].upstream && !out[2].pass && out[2].dmac[5] == 0x20u);
	CHECK(out[3].upstream && out[3].pass && out[3].dmac[5] == 0x12u);
	CHECK(out[4].upstream && out[4].pass && out[4].smac[5] == 0x13u);
	/* Then every other frame upstream: individual, then group addresses. */
	CHECK(out[5].upstream && !out[5].pass && out[5].dmask[0] == 0x01u &&
	      out[5].dmac[0] == 0u);
	CHECK(out[6].upstream && !out[6].pass && out[6].dmac[0] == 0x01u);
	/* A second look says nothing new and asks for the same. */
	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 7u);
}

static void test_the_pppoe_filter_admits_pppoe_alone(struct omci_mib *mib)
{
	struct omci_hw_filter out[8];

	pppoe(mib, 1u);
	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 4u);
	CHECK(out[0].upstream && out[0].pass && out[0].ethertype == 0x8863u &&
	      out[0].dmask[0] == 0u);
	CHECK(out[1].pass && out[1].ethertype == 0x8864u);
	CHECK(!out[2].pass && !out[3].pass);

	/* With an admitted address, PPPoE of that address alone. */
	port(mib, 0x0002u, TP_GEM_IW, false);
	row(mib, 0x0002u, 0u, ADD, 0x12u);
	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 4u);
	CHECK(out[0].pass && out[0].ethertype == 0x8863u &&
	      out[0].dmac[5] == 0x12u);
	CHECK(out[1].pass && out[1].ethertype == 0x8864u &&
	      out[1].dmac[5] == 0x12u);
}

static void test_rules_beyond_the_room_are_counted(struct omci_mib *mib)
{
	struct omci_hw_filter out[2];

	port(mib, 0x0002u, TP_GEM_IW, true);
	group(mib, 0x0002u, 1u);
	group(mib, 0x0002u, 2u);
	group(mib, 0x0002u, 9u);
	CHECK_EQ(omci_filter_rules(mib, out, ARRAY_SIZE(out)), 3u);
	CHECK_EQ(out[1].dmac[0], 0x33u);
}

static void test_the_backend_takes_changes_only(struct omci_mib *mib)
{
	const struct omci_hw_filter *got = NULL;

	omci_filter_forget();
	port(mib, 0x0002u, TP_GEM_IW, true);
	group(mib, 0x0002u, 9u);
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	CHECK_EQ(omci_hw_filter_recorded(&got), 1u);
	CHECK(is_drop_to(&got[0], true, g_all, g_all, 0x0806u));

	/* Unchanged: the backend is not called, so its failure waits. */
	omci_hw_filter_fail_next();
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	group(mib, 0x0002u, 2u);
	CHECK_EQ(omci_filter_apply(mib), OMCI_ERR_HW);
	CHECK_EQ(omci_hw_filter_recorded(&got), 1u);
	/* The rules that failed are given again. */
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	CHECK_EQ(omci_hw_filter_recorded(&got), 2u);

	/* Changed in place, same count. */
	const uint8_t off = 0u;

	CHECK_EQ(omci_mib_attr_set_by_olt(mib, ME_PREASSIGN, 0x0002u, 9u, &off,
					  1u),
		 OMCI_OK);
	group(mib, 0x0002u, 3u);
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	CHECK_EQ(omci_hw_filter_recorded(&got), 2u);
	CHECK(is_drop_to(&got[1], true, g_all, g_all, 0x0800u));

	/* The host backend holds no more than a backend may. */
	CHECK_EQ(omci_hw_filter_apply(got, OMCI_HW_FILTER_MAX + 1u),
		 OMCI_ERR_NOMEM);
}

static void test_too_many_rules_are_cut(struct omci_mib *mib)
{
	const struct omci_hw_filter *got = NULL;

	omci_filter_forget();
	for (uint16_t p = 1u; p <= 4u; p++) {
		port(mib, p, TP_GEM_IW, false);
		for (uint8_t e = 0u; e < 16u; e++)
			row(mib, p, e, ADD | FILTER, (uint8_t)(p * 16u + e));
	}
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	CHECK_EQ(omci_hw_filter_recorded(&got), OMCI_HW_FILTER_MAX);
	/* Said once; the same cut set is not given twice. */
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	CHECK_EQ(omci_hw_filter_recorded(&got), OMCI_HW_FILTER_MAX);
}

static void test_a_backend_without_filters_keeps_them(struct omci_mib *mib)
{
	struct omci_hw_ops plain = omci_hw_host_ops;

	plain.filter_apply = NULL;
	omci_hw_ops_set(&plain);
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	port(mib, 0x0002u, TP_GEM_IW, true);
	group(mib, 0x0002u, 9u);
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	CHECK_EQ(omci_filter_apply(mib), OMCI_OK);
	omci_hw_ops_set(NULL);
}

/* Send a request and return the result byte of its answer, or 0xff. */
static uint8_t request(struct omci_engine *eng, uint16_t tci, uint8_t mt,
		       uint16_t cls, uint16_t inst, const uint8_t *contents,
		       size_t len)
{
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tci, mt, true, false, cls, inst);
	if (contents != NULL)
		memcpy(&wire[8], contents, len);
	omci_t_tx_reset();
	if (omci_t_rx(eng, wire) != OMCI_OK)
		return 0xffu;
	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x20u) != 0u)
			return omci_t_tx.frame[i][8];
	return 0xffu;
}

static void test_the_engine_applies_the_filters(void)
{
	const uint8_t bridge_port[10] = { 0x00, 0x01, 0x02, 0x01, 0x01, 0x01 };
	const uint8_t arp[3] = { 0x00, 0x80, 0x01 };
	const struct omci_hw_filter *got = NULL;
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);
	CHECK_EQ(request(fx.eng, 0x0100u, MT_CREATE, ME_BRIDGE, 1u, NULL, 0),
		 R_OK);
	CHECK_EQ(request(fx.eng, 0x0101u, MT_CREATE, ME_BRIDGE_PORT, 0x0201u,
			 bridge_port, sizeof(bridge_port)),
		 R_OK);
	/* ARP filtering on the UNI side's port: a drop downstream. */
	CHECK_EQ(request(fx.eng, 0x0102u, MT_SET, ME_PREASSIGN, 0x0201u, arp,
			 sizeof(arp)),
		 R_OK);
	CHECK_EQ(omci_hw_filter_recorded(&got), 1u);
	CHECK(is_drop_to(&got[0], false, g_all, g_all, 0x0806u));

	/* The port goes, and its filters with it. */
	CHECK_EQ(request(fx.eng, 0x0103u, MT_DELETE, ME_BRIDGE_PORT, 0x0201u,
			 NULL, 0),
		 R_OK);
	CHECK_EQ(omci_hw_filter_recorded(&got), 0u);
	omci_t_down(&fx);
}

int main(void)
{
	RUN_MIB(test_no_filter_asks_for_no_rule);
	RUN_MIB(test_a_group_drops_on_the_side_of_its_port);
	RUN_MIB(test_every_group_has_its_addresses);
	RUN_MIB(test_the_mac_table_drops_and_admits);
	RUN_MIB(test_the_pppoe_filter_admits_pppoe_alone);
	RUN_MIB(test_rules_beyond_the_room_are_counted);
	RUN_MIB(test_the_backend_takes_changes_only);
	RUN_MIB(test_too_many_rules_are_cut);
	RUN_MIB(test_a_backend_without_filters_keeps_them);
	RUN(test_the_engine_applies_the_filters);
	return OMCI_T_REPORT("test_filter");
}
