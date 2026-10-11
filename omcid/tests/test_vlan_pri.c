// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of a translation that keeps any priority, into a VLAN whose GEM
 * ports divide the priorities: it takes each priority apart, and follows a
 * mapper that starts or stops dividing them.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/bridge.h"
#include "me/defaults.h"
#include "me/gem.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

#define ME171 171u
#define RULE_ATTR 6u
#define RULE_BYTES 16u
#define XLATE_INST 0x0101u
#define MAPPER 0x1102u

static uint8_t tx[4][OMCI_FRAME_LEN];
static size_t tx_count;

static void on_send(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN])
{
	(void)ctx;
	if (tx_count < 4)
		memcpy(tx[tx_count++], frame, OMCI_FRAME_LEN);
}

static const struct omci_tx_ops tx_ops = { .send = on_send };

static void set_u16(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		    uint8_t attr, uint16_t v)
{
	const uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, b, sizeof(b)),
		 OMCI_OK);
}

static void set_u8(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		   uint8_t attr, uint8_t v)
{
	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, &v, sizeof(v)),
		 OMCI_OK);
}

/* A GEM port both ways on T-CONT 0, with its interworking point. */
static void gem(struct omci_mib *mib, uint16_t inst, uint16_t port_id)
{
	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_CLASS_ID, inst), OMCI_OK);
	set_u16(mib, OMCI_GEM_CLASS_ID, inst, OMCI_GEM_ATTR_PORT_ID, port_id);
	set_u16(mib, OMCI_GEM_CLASS_ID, inst, OMCI_GEM_ATTR_TCONT, 0x8000u);
	set_u8(mib, OMCI_GEM_CLASS_ID, inst, OMCI_GEM_ATTR_DIRECTION, 3u);
	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_IW_CLASS_ID, inst), OMCI_OK);
	set_u16(mib, OMCI_GEM_IW_CLASS_ID, inst, OMCI_GEM_IW_ATTR_CTP, inst);
}

static void bridge_port(struct omci_mib *mib, uint16_t inst, uint8_t num,
			uint8_t tp_type, uint16_t tp_ptr)
{
	CHECK_EQ(omci_mib_create(mib, 47u, inst), OMCI_OK);
	set_u16(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_BRIDGE, 0x0001u);
	set_u8(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_NUM, num);
	set_u8(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_TPTYPE, tp_type);
	set_u16(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_TPPTR, tp_ptr);
}

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/*
 * A single-tagged frame of VID @match and priority @pri, 8 for any, given
 * VID @assign and priority @to, 8 for the frame's own.
 */
static void make_xlate(uint8_t out[RULE_BYTES], unsigned int pri,
		       unsigned int match, unsigned int to, unsigned int assign)
{
	be32(&out[0], (15u << 28) | (4096u << 15));
	be32(&out[4], (pri << 28) | (match << 15));
	be32(&out[8], (1u << 30) | (15u << 16));
	be32(&out[12], (to << 16) | (assign << 3));
}

/* Send a Set of @class/@inst with the attributes of @mask, @value in order. */
static uint8_t set(struct omci_engine *eng, uint16_t tid, uint16_t class_id,
		   uint16_t inst, uint16_t mask, const uint8_t *value,
		   size_t len)
{
	struct omci_frame f = {
		.tci = tid,
		.msg_type = 8,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = class_id,
		.instance = inst,
	};
	uint8_t wire[OMCI_FRAME_LEN];

	f.contents[0] = (uint8_t)(mask >> 8);
	f.contents[1] = (uint8_t)mask;
	memcpy(&f.contents[2], value, len);
	(void)!omci_frame_build(&f, wire);

	tx_count = 0;
	if (omci_engine_rx(eng, wire) != OMCI_OK || tx_count == 0)
		return 0xff;
	return tx[0][8];
}

static uint8_t set_rule(struct omci_engine *eng, uint16_t tid,
			const uint8_t rule[RULE_BYTES])
{
	return set(eng, tid, ME171, XLATE_INST, omci_attr_bit(RULE_ATTR), rule,
		   RULE_BYTES);
}

/* Point the mapper's priorities 4 to 7 at the interworking point @iw. */
static uint8_t map_high(struct omci_engine *eng, uint16_t tid, uint16_t iw)
{
	uint8_t v[8];
	uint16_t mask = 0;

	for (unsigned int b = 0; b < 4u; b++) {
		mask |= omci_attr_bit(OMCI_GEM_MAPPER_ATTR_PBIT0 + 4u + b);
		v[2u * b] = (uint8_t)(iw >> 8);
		v[2u * b + 1u] = (uint8_t)iw;
	}
	return set(eng, tid, OMCI_GEM_MAPPER_CLASS_ID, MAPPER, mask, v,
		   sizeof(v));
}

/* The rule programmed that takes VID @match, or NULL. */
static const struct omci_hw_vlan_xlate *rule_of(uint16_t match)
{
	const struct omci_hw_vlan_xlate *r;
	const size_t n = omci_hw_vlan_recorded(&r);

	for (size_t i = 0; i < n; i++)
		if (r[i].match_cvid == match)
			return &r[i];
	return NULL;
}

int main(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;
	const struct omci_hw_vlan_xlate *r;
	const uint16_t vid = 410u;
	uint8_t table[24] = { 0 };
	uint8_t rule[RULE_BYTES];

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &tx_ops, NULL), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);

	/* Two GEM ports behind a mapper that sends them every priority. */
	gem(mib, 0x0116u, 278u);
	gem(mib, 0x0117u, 279u);
	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_MAPPER_CLASS_ID, MAPPER),
		 OMCI_OK);
	for (uint8_t b = 0; b < 8u; b++)
		set_u16(mib, OMCI_GEM_MAPPER_CLASS_ID, MAPPER,
			(uint8_t)(OMCI_GEM_MAPPER_ATTR_PBIT0 + b), 0x0116u);
	CHECK_EQ(omci_mib_create(mib, 45u, 0x0001u), OMCI_OK);
	bridge_port(mib, 0x0101u, 1u, OMCI_BRIDGE_TP_PPTP_ETH, 0x0101u);
	bridge_port(mib, MAPPER, 2u, OMCI_BRIDGE_TP_MAPPER, MAPPER);
	CHECK_EQ(omci_mib_create(mib, 84u, MAPPER), OMCI_OK);
	table[0] = (uint8_t)(vid >> 8);
	table[1] = (uint8_t)vid;
	CHECK_EQ(omci_mib_attr_set(mib, 84u, MAPPER,
				   OMCI_BRIDGE_VLAN_ATTR_TABLE, table,
				   sizeof(table)),
		 OMCI_OK);
	set_u8(mib, 84u, MAPPER, OMCI_BRIDGE_VLAN_ATTR_COUNT, 1u);
	CHECK_EQ(omci_mib_create(mib, ME171, XLATE_INST), OMCI_OK);
	omci_engine_reprogram(eng);

	/* VID 10 into 410, keeping any priority; 11 of priority 5; 12 given 3. */
	make_xlate(rule, 8u, 10u, 8u, vid);
	CHECK_EQ(set_rule(eng, 1, rule), 0u);
	make_xlate(rule, 5u, 11u, 8u, vid);
	CHECK_EQ(set_rule(eng, 2, rule), 0u);
	make_xlate(rule, 8u, 12u, 3u, vid);
	CHECK_EQ(set_rule(eng, 3, rule), 0u);

	r = rule_of(10u);
	CHECK(r != NULL && r->kind == OMCI_HW_VLAN_CVID_XLATE &&
	      r->assign_pri == OMCI_HW_VLAN_PRI_KEEP && !r->match_has_pri &&
	      !r->per_pri);
	CHECK(!omci_vlan_per_pri_stale());
	printf("    one GEM port takes every priority of VLAN 410: the rule that keeps any priority stays whole\n");

	/* The mapper sends priorities 4 to 7 to the second port. */
	CHECK_EQ(map_high(eng, 4, 0x0117u), 0u);
	{
		const struct omci_hw_gem_port *g;
		const size_t n = omci_gem_last(&g);
		uint8_t low = 0, high = 0;

		for (size_t i = 0; i < n; i++) {
			if (g[i].port_id == 278u)
				low = g[i].pbits;
			if (g[i].port_id == 279u)
				high = g[i].pbits;
		}
		CHECK_EQ(low, 0x0fu);
		CHECK_EQ(high, 0xf0u);
	}
	r = rule_of(10u);
	CHECK(r != NULL && r->per_pri && !r->match_has_pri);
	r = rule_of(11u);
	CHECK(r != NULL && !r->per_pri && r->match_has_pri &&
	      r->match_pri == 5u);
	r = rule_of(12u);
	CHECK(r != NULL && !r->per_pri && r->assign_pri == 3u);
	CHECK(!omci_vlan_per_pri_stale());
	printf("    the mapper divides VLAN 410: the rule that keeps any priority takes each apart, the one that matches or writes a priority does not\n");

	/* Back to one port for every priority. */
	CHECK_EQ(map_high(eng, 5, 0x0116u), 0u);
	r = rule_of(10u);
	CHECK(r != NULL && !r->per_pri);
	CHECK(!omci_vlan_per_pri_stale());
	printf("    the mapper gives VLAN 410 back to one GEM port: the rule is whole again\n");

	/* A rule into a VLAN the divided GEM ports do not carry stays whole. */
	CHECK_EQ(map_high(eng, 6, 0x0117u), 0u);
	make_xlate(rule, 8u, 20u, 8u, 40u);
	CHECK_EQ(set_rule(eng, 7, rule), 0u);
	r = rule_of(20u);
	CHECK(r != NULL && !r->per_pri);
	CHECK(rule_of(10u) != NULL && rule_of(10u)->per_pri);
	printf("    a rule into a VLAN no divided GEM port carries stays whole\n");

	omci_engine_fini(eng);
	omci_mib_fini(mib);

	printf("\n%d omci_t_checks, %d omci_t_failures\n", omci_t_checks,
	       omci_t_failures);
	return OMCI_T_REPORT("test_vlan_pri");
}
