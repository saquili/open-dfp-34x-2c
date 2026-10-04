// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the multicast managed entities: the values their attributes take,
 * their tables with a table control field, and those tables read into rows.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mcast/acl.h"
#include "me/tables.h"
#include "me/values.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"

#include "harness.h"

#define MT_CREATE 4u
#define MT_SET 8u
#define MT_GET 9u
#define MT_GET_NEXT 26u

#define R_OK 0u
#define R_PARAM_ERROR 3u
#define R_UNKNOWN_INST 5u
#define R_DEVICE_BUSY 6u

#define SEGMENT 29u
#define PROFILE 0x0201u
#define SUBSCRIBER 0x0101u

/* The operations of a table control field. */
#define OP_RESERVED 0u
#define OP_WRITE 1u
#define OP_DELETE 2u
#define OP_CLEAR 3u

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

static void put16(uint8_t *p, unsigned int v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static const uint8_t *drive(struct omci_engine *eng, uint8_t mt, uint16_t cls,
			    uint16_t inst, const uint8_t *contents, size_t n)
{
	const uint16_t tci = next_tid();
	struct omci_frame f = {
		.tci = tci,
		.msg_type = mt,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = cls,
		.instance = inst,
	};
	uint8_t wire[OMCI_FRAME_LEN];

	if (contents != NULL && n > 0)
		memcpy(f.contents, contents, n);
	(void)!omci_frame_build(&f, wire);
	omci_t_tx_reset();
	(void)!omci_engine_rx(eng, wire);

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x20u) != 0u &&
		    (be16_at(omci_t_tx.frame[i], 0) & 0x7FFFu) == tci)
			return omci_t_tx.frame[i];
	return NULL;
}

static unsigned int result_of(const uint8_t *rsp)
{
	return rsp != NULL ? rsp[8] : 0xffu;
}

/* Set one attribute; the result. */
static unsigned int set_attr(struct omci_engine *eng, uint16_t cls,
			     uint16_t inst, uint8_t attr, const uint8_t *value,
			     size_t size)
{
	uint8_t body[OMCI_CONTENTS_LEN] = { 0 };
	const uint16_t bit = omci_attr_bit(attr);

	put16(body, bit);
	memcpy(&body[2], value, size);
	return result_of(drive(eng, MT_SET, cls, inst, body, 2u + size));
}

/* Read a table through Get and Get Next; its size, or -1. */
static long walk(struct omci_engine *eng, uint16_t cls, uint16_t inst,
		 uint8_t attr, uint8_t *out, size_t cap)
{
	const uint16_t bit = omci_attr_bit(attr);
	uint8_t body[4];
	const uint8_t *rsp;
	long size;
	size_t at = 0;

	put16(body, bit);
	rsp = drive(eng, MT_GET, cls, inst, body, 2);
	if (rsp == NULL || rsp[8] != R_OK)
		return -1;
	size = (long)be32_at(rsp, 11);
	CHECK((size_t)size <= cap);
	for (unsigned int seq = 0; at < (size_t)size; seq++) {
		const size_t take = (size_t)size - at < SEGMENT ?
					    (size_t)size - at :
					    SEGMENT;

		put16(&body[2], seq);
		rsp = drive(eng, MT_GET_NEXT, cls, inst, body, 4);
		CHECK_EQ(result_of(rsp), R_OK);
		if (rsp == NULL)
			return -1;
		memcpy(&out[at], &rsp[11], take);
		at += take;
	}
	return size;
}

/* A table control field. */
static void ctrl(uint8_t *row, unsigned int op, unsigned int part,
		 unsigned int key)
{
	row[0] = (uint8_t)(op << 6 | part << 3 | key >> 8);
	row[1] = (uint8_t)key;
}

static void v4(uint8_t *p, unsigned int a, unsigned int b, unsigned int c,
	       unsigned int d)
{
	p[0] = (uint8_t)a;
	p[1] = (uint8_t)b;
	p[2] = (uint8_t)c;
	p[3] = (uint8_t)d;
}

/* Part 0 of an access control list row, IPv4 addresses given as words. */
static void acl0(uint8_t row[24], unsigned int key, uint32_t source,
		 uint32_t first, uint32_t last, uint32_t bandwidth)
{
	memset(row, 0, 24);
	ctrl(row, OP_WRITE, 0, key);
	put16(&row[2], 4095u);
	put16(&row[4], 100u);
	put32(&row[6], source);
	put32(&row[10], first);
	put32(&row[14], last);
	put32(&row[18], bandwidth);
}

#define IP4(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

struct fixture {
	struct omci_t_fixture t;
};

static void up(struct fixture *fx)
{
	uint8_t reset[OMCI_FRAME_LEN];
	uint8_t profile[26] = { 2u }, subscriber[10] = { 0 },
		monitor[1] = { 0 };

	CHECK(omci_t_up(&fx->t));
	omci_t_build(reset, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx->t.eng, reset), OMCI_OK);
	CHECK_EQ(result_of(drive(fx->t.eng, MT_CREATE, OMCI_MCAST_PROFILE,
				 PROFILE, profile, sizeof(profile))),
		 R_OK);
	put16(&subscriber[1], PROFILE);
	CHECK_EQ(result_of(drive(fx->t.eng, MT_CREATE, OMCI_MCAST_SUBSCRIBER,
				 SUBSCRIBER, subscriber, sizeof(subscriber))),
		 R_OK);
	CHECK_EQ(result_of(drive(fx->t.eng, MT_CREATE, OMCI_MCAST_MONITOR,
				 SUBSCRIBER, monitor, sizeof(monitor))),
		 R_OK);
}

static void down(struct fixture *fx)
{
	omci_t_down(&fx->t);
}

static void test_create_refuses_values_the_attributes_do_not_take(void)
{
	static const struct {
		uint16_t cls;
		uint8_t len;
		uint8_t at;
		uint8_t value;
		uint8_t at2;
		uint8_t value2;
		uint16_t mask;
	} bad[] = {
		/* IGMP version 4; IGMP function 3 */
		{ 309u, 26u, 0u, 4u, 0u, 4u, 0x8000u },
		{ 309u, 26u, 1u, 3u, 0u, 2u, 0x4000u },
		/* immediate leave 2 and the downstream TCI's control type 8 */
		{ 309u, 26u, 2u, 2u, 23u, 8u, 0x2001u },
		/* upstream IGMP tag control 4 */
		{ 309u, 26u, 5u, 4u, 0u, 3u, 0x0800u },
		/* the ME type 2, bandwidth enforcement 2 */
		{ 310u, 10u, 0u, 2u, 9u, 2u, 0x8800u },
		{ 311u, 1u, 0u, 9u, 0u, 9u, 0x8000u },
	};
	struct fixture fx;

	up(&fx);
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		uint8_t body[32] = { 0 };
		const uint8_t get[2] = { 0x80u, 0x00u };
		const uint8_t *rsp;

		body[0] = 2u;
		body[bad[i].at] = bad[i].value;
		body[bad[i].at2] = bad[i].value2;
		rsp = drive(fx.t.eng, MT_CREATE, bad[i].cls, 0x0777u, body,
			    bad[i].len);
		CHECK_EQ(result_of(rsp), R_PARAM_ERROR);
		if (rsp != NULL)
			CHECK_EQ(be16_at(rsp, 9), bad[i].mask);
		CHECK(!omci_mib_exists(fx.t.mib, bad[i].cls, 0x0777u));
		CHECK_EQ(result_of(drive(fx.t.eng, MT_GET, bad[i].cls, 0x0777u,
					 get, sizeof(get))),
			 R_UNKNOWN_INST);
	}

	/* Every IGMP version the ONU has is taken. */
	static const uint8_t versions[] = { 1u, 2u, 3u, 16u, 17u };

	for (size_t i = 0; i < ARRAY_SIZE(versions); i++) {
		uint8_t body[26] = { versions[i] };

		CHECK_EQ(result_of(drive(fx.t.eng, MT_CREATE, 309u,
					 (uint16_t)(0x0900u + i), body,
					 sizeof(body))),
			 R_OK);
	}
	down(&fx);
}

static void test_sets_refuse_values_out_of_range(void)
{
	static const struct {
		uint16_t cls;
		uint16_t inst;
		uint8_t attr;
		uint8_t size;
		uint8_t bad;
		uint8_t good;
	} rules[] = {
		{ 309u, PROFILE, 1u, 1u, 0u, 3u },
		{ 309u, PROFILE, 1u, 1u, 4u, 16u },
		{ 309u, PROFILE, 1u, 1u, 18u, 17u },
		{ 309u, PROFILE, 2u, 1u, 3u, 2u },
		{ 309u, PROFILE, 3u, 1u, 2u, 1u },
		{ 309u, PROFILE, 5u, 1u, 4u, 3u },
		{ 309u, PROFILE, 15u, 1u, 2u, 1u },
		{ 309u, PROFILE, 16u, 3u, 8u, 7u },
		{ 310u, SUBSCRIBER, 1u, 1u, 2u, 1u },
		{ 310u, SUBSCRIBER, 5u, 1u, 2u, 1u },
		{ 311u, SUBSCRIBER, 1u, 1u, 2u, 1u },
	};
	struct fixture fx;

	up(&fx);
	for (size_t i = 0; i < ARRAY_SIZE(rules); i++) {
		uint8_t value[3] = { rules[i].bad, 0x12u, 0x34u };

		CHECK(!omci_me_value_allowed(rules[i].cls, rules[i].attr,
					     value));
		CHECK_EQ(set_attr(fx.t.eng, rules[i].cls, rules[i].inst,
				  rules[i].attr, value, rules[i].size),
			 R_PARAM_ERROR);
		value[0] = rules[i].good;
		CHECK_EQ(set_attr(fx.t.eng, rules[i].cls, rules[i].inst,
				  rules[i].attr, value, rules[i].size),
			 R_OK);
	}
	down(&fx);
}

/* The four tables with a table control field. */
static const struct {
	uint16_t cls;
	uint16_t inst;
	uint8_t attr;
	uint8_t size;
	uint8_t last_part;
	uint8_t rows;
} g_tables[] = {
	{ 309u, PROFILE, 7u, 24u, 2u, 24u },
	{ 309u, PROFILE, 8u, 24u, 2u, 24u },
	{ 310u, SUBSCRIBER, 6u, 20u, 0u, 16u },
	{ 310u, SUBSCRIBER, 7u, 22u, 1u, 16u },
};

/*
 * A row part for the generic checks: its key and part, the test bit set,
 * bytes that tell it apart, and for an access control list a range of its
 * own, so that no two rows overlap.
 */
static void generic_row(uint8_t *row, size_t size, unsigned int op,
			unsigned int part, unsigned int key, uint8_t fill)
{
	memset(row, fill, size);
	ctrl(row, op, part, key);
	row[0] |= 0x04u;
	if (size == 24u && part == 0u) {
		put32(&row[6], 0u);
		put32(&row[10], IP4(239, 2, key, fill));
		put32(&row[14], IP4(239, 2, key, fill));
	}
	if (size == 24u && part != 0u)
		memset(&row[2], 0, 12);
	if (size == 22u)
		memset(&row[18], 0, 4);
}

static void test_table_control_writes_deletes_and_clears(void)
{
	struct fixture fx;

	up(&fx);
	for (size_t t = 0; t < ARRAY_SIZE(g_tables); t++) {
		const uint16_t cls = g_tables[t].cls;
		const uint16_t inst = g_tables[t].inst;
		const uint8_t attr = g_tables[t].attr;
		const size_t size = g_tables[t].size;
		uint8_t row[24], flat[24 * 24];

		printf("  ME %u attribute %u\n", cls, attr);

		/* Two rows; a Get Next reads the operation and test bits as 0. */
		generic_row(row, size, OP_WRITE, 0, 0x101u, 0x31u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size), R_OK);
		generic_row(row, size, OP_WRITE, 0, 0x002u, 0x32u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size), R_OK);
		CHECK_EQ(walk(fx.t.eng, cls, inst, attr, flat, sizeof(flat)),
			 (long)(2 * size));
		CHECK_EQ(flat[0], 0x01u);
		CHECK_EQ(flat[1], 0x01u);
		CHECK_EQ(flat[size], 0x00u);
		CHECK_EQ(flat[size + 1], 0x02u);
		CHECK_EQ(flat[size - 1], size == 22u ? 0u : 0x31u);

		/* The same key and part replace the part. */
		generic_row(row, size, OP_WRITE, 0, 0x101u, 0x33u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size), R_OK);
		CHECK_EQ(walk(fx.t.eng, cls, inst, attr, flat, sizeof(flat)),
			 (long)(2 * size));
		CHECK_EQ(flat[size - 1], size == 22u ? 0u : 0x33u);

		/* The other parts of row 0x101, then a delete of all of them. */
		for (unsigned int p = 1; p <= g_tables[t].last_part; p++) {
			generic_row(row, size, OP_WRITE, p, 0x101u, 0x40u);
			CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size),
				 R_OK);
		}
		CHECK_EQ(walk(fx.t.eng, cls, inst, attr, flat, sizeof(flat)),
			 (long)((2u + g_tables[t].last_part) * size));
		generic_row(row, size, OP_DELETE, 0, 0x101u, 0x00u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size), R_OK);
		CHECK_EQ(walk(fx.t.eng, cls, inst, attr, flat, sizeof(flat)),
			 (long)size);
		CHECK_EQ(flat[1], 0x02u);
		/* A row that is not there deletes nothing, and is no fault. */
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size), R_OK);

		/* The reserved operation, and a part past the last. */
		generic_row(row, size, OP_RESERVED, 0, 0x005u, 0x50u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size),
			 R_PARAM_ERROR);
		if (g_tables[t].last_part != 0u) {
			generic_row(row, size, OP_WRITE,
				    g_tables[t].last_part + 1u, 0x005u, 0x50u);
			CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size),
				 R_PARAM_ERROR);
		}
		CHECK_EQ(walk(fx.t.eng, cls, inst, attr, flat, sizeof(flat)),
			 (long)size);

		/* Full: a new part is refused, an old one replaced. */
		for (unsigned int k = 10; k < 10u + g_tables[t].rows - 1u;
		     k++) {
			generic_row(row, size, OP_WRITE, 0, k, 0x60u);
			CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size),
				 R_OK);
		}
		CHECK_EQ(walk(fx.t.eng, cls, inst, attr, flat, sizeof(flat)),
			 (long)(g_tables[t].rows * size));
		generic_row(row, size, OP_WRITE, 0, 0x3ffu, 0x61u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size),
			 R_DEVICE_BUSY);
		generic_row(row, size, OP_WRITE, 0, 10u, 0x62u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size), R_OK);

		/* Clear all. */
		generic_row(row, size, OP_CLEAR, 0, 0x3ffu, 0x00u);
		CHECK_EQ(set_attr(fx.t.eng, cls, inst, attr, row, size), R_OK);
		CHECK_EQ(walk(fx.t.eng, cls, inst, attr, flat, sizeof(flat)),
			 0L);
	}
	down(&fx);
}

static void test_the_table_control_functions_refuse_what_cannot_be_asked(void)
{
	struct omci_me_table_policy acl, packages;
	struct omci_me_table_row_id id;
	struct omci_mib *mib = NULL;
	uint8_t row[24] = { 0 };

	CHECK(omci_me_table_policy(309u, 7u, &acl));
	CHECK(omci_me_table_policy(310u, 6u, &packages));
	CHECK_EQ(acl.ctrl, OMCI_ME_TABLE_CTRL_PARTS);
	CHECK_EQ(packages.ctrl, OMCI_ME_TABLE_CTRL_KEY);
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);

	CHECK_EQ(omci_me_table_ctrl_set(NULL, 309u, 1u, 7u, &acl, row, 24),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_me_table_ctrl_set(mib, 309u, 1u, 7u, NULL, row, 24),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_me_table_ctrl_set(mib, 309u, 1u, 7u, &acl, NULL, 24),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_me_table_ctrl_set(mib, 309u, 1u, 7u, &acl, row, 1),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_me_table_ctrl_set(mib, 309u, 1u, 7u, &acl, row, 65),
		 OMCI_ERR_INVAL);

	/* An instance that is not there: a delete and a write fail. */
	ctrl(row, OP_DELETE, 0, 7u);
	CHECK(omci_me_table_ctrl_set(mib, 309u, 1u, 7u, &acl, row, 24) !=
	      OMCI_OK);
	ctrl(row, OP_WRITE, 0, 7u);
	CHECK(omci_me_table_ctrl_set(mib, 309u, 1u, 7u, &acl, row, 24) !=
	      OMCI_OK);
	ctrl(row, OP_CLEAR, 0, 7u);
	CHECK_EQ(omci_me_table_ctrl_set(mib, 309u, 1u, 7u, &acl, row, 24),
		 OMCI_OK);

	/* A table without parts reads no part; the operation can be changed. */
	ctrl(row, OP_WRITE, 5u, 0x2a5u);
	omci_me_table_row_id(&packages, row, &id);
	CHECK_EQ(id.op, OMCI_ME_TABLE_OP_WRITE);
	CHECK_EQ(id.part, 0u);
	CHECK_EQ(id.key, 0x2a5u);
	omci_me_table_row_id(&acl, row, &id);
	CHECK_EQ(id.part, 5u);
	omci_me_table_op_set(row, OMCI_ME_TABLE_OP_CLEAR);
	omci_me_table_row_id(&acl, row, &id);
	CHECK_EQ(id.op, OMCI_ME_TABLE_OP_CLEAR);
	CHECK_EQ(id.part, 5u);
	CHECK_EQ(id.key, 0x2a5u);

	omci_mib_fini(mib);
}

/* Write a row part straight into the MIB, through the table control path. */
static void store(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		  uint8_t attr, const uint8_t *row, size_t size)
{
	struct omci_me_table_policy policy;

	CHECK(omci_me_table_policy(cls, attr, &policy));
	CHECK_EQ(omci_me_table_ctrl_set(mib, cls, inst, attr, &policy, row,
					size),
		 OMCI_OK);
}

static void test_acl_rows_are_put_together_from_their_parts(void)
{
	static const uint8_t lead_src[12] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0,
					      0,    0,	  0,	0,    0, 0 };
	static const uint8_t lead_dst[12] = { 0xff, 0x3e, 0, 0, 0, 0,
					      0,    0,	  0, 0, 0, 0 };
	static const uint8_t lead_v4[12] = { 0, 0, 0, 0, 0,    0,
					     0, 0, 0, 0, 0xff, 0xff };
	struct omci_mcast_acl_row rows[8];
	uint8_t row[24], g[16], s[16], zero[16] = { 0 };
	struct fixture fx;
	size_t n;

	up(&fx);
	/* Row 5: IPv4, any source. */
	acl0(row, 5u, 0u, IP4(239, 1, 1, 0), IP4(239, 1, 1, 255), 1000u);
	store(fx.t.mib, 309u, PROFILE, 7u, row, 24);
	/* Row 3: IPv6, with its source and preview in part 1. */
	acl0(row, 3u, 0x00000001u, 0x00000100u, 0x000001ffu, 2000u);
	store(fx.t.mib, 309u, PROFILE, 7u, row, 24);
	memset(row, 0, 24);
	ctrl(row, OP_WRITE, 1, 3u);
	memcpy(&row[2], lead_src, 12);
	put16(&row[14], 30u);
	put16(&row[16], 600u);
	put16(&row[18], 3u);
	put16(&row[20], 2u);
	store(fx.t.mib, 309u, PROFILE, 7u, row, 24);
	memset(row, 0, 24);
	ctrl(row, OP_WRITE, 2, 3u);
	memcpy(&row[2], lead_dst, 12);
	store(fx.t.mib, 309u, PROFILE, 7u, row, 24);
	/* Row 9: no part 0, so no row. */
	memset(row, 0, 24);
	ctrl(row, OP_WRITE, 1, 9u);
	store(fx.t.mib, 309u, PROFILE, 7u, row, 24);
	/* Row 7: leading bytes of the IPv4 form still make it IPv4. */
	acl0(row, 7u, IP4(10, 0, 0, 1), IP4(232, 1, 1, 1), IP4(232, 1, 1, 9),
	     0u);
	store(fx.t.mib, 309u, PROFILE, 7u, row, 24);
	memset(row, 0, 24);
	ctrl(row, OP_WRITE, 2, 7u);
	memcpy(&row[2], lead_v4, 12);
	store(fx.t.mib, 309u, PROFILE, 7u, row, 24);

	n = omci_mcast_acl_read(fx.t.mib, PROFILE, 7u, rows, ARRAY_SIZE(rows));
	CHECK_EQ(n, 3u);
	CHECK_EQ(omci_mcast_acl_read(fx.t.mib, PROFILE, 7u, rows, 2u), 2u);
	n = omci_mcast_acl_read(fx.t.mib, PROFILE, 7u, rows, ARRAY_SIZE(rows));
	CHECK_EQ(rows[0].key, 3u);
	CHECK_EQ(rows[1].key, 5u);
	CHECK_EQ(rows[2].key, 7u);

	CHECK(!omci_mcast_addr_v4(rows[0].first));
	CHECK_MEM(rows[0].first, lead_dst, 12);
	CHECK_EQ(be32_at(rows[0].first, 12), 0x100u);
	CHECK_EQ(be32_at(rows[0].last, 12), 0x1ffu);
	CHECK_MEM(rows[0].source, lead_src, 12);
	CHECK(!rows[0].any_source);
	CHECK_EQ(rows[0].bandwidth, 2000u);
	CHECK_EQ(rows[0].preview_length, 30u);
	CHECK_EQ(rows[0].preview_repeat_time, 600u);
	CHECK_EQ(rows[0].preview_repeat_count, 3u);
	CHECK_EQ(rows[0].preview_reset_time, 2u);

	CHECK(omci_mcast_addr_v4(rows[1].first));
	CHECK_EQ(be32_at(rows[1].first, 12), IP4(239, 1, 1, 0));
	CHECK(rows[1].any_source);
	CHECK_EQ(rows[1].gem_port, 4095u);
	CHECK_EQ(rows[1].vlan, 100u);
	CHECK_EQ(rows[1].preview_length, 0u);

	CHECK(omci_mcast_addr_v4(rows[2].first));
	CHECK(!rows[2].any_source);

	/* Matching: the range, and the source when the row names one. */
	v4(&g[12], 239, 1, 1, 7);
	omci_mcast_addr_set_v4(g, &g[12]);
	CHECK(omci_mcast_acl_matches(&rows[1], g, zero));
	v4(&s[12], 10, 0, 0, 1);
	omci_mcast_addr_set_v4(s, &s[12]);
	CHECK(omci_mcast_acl_matches(&rows[1], g, s));
	v4(&g[12], 239, 1, 2, 0);
	omci_mcast_addr_set_v4(g, &g[12]);
	CHECK(!omci_mcast_acl_matches(&rows[1], g, zero));
	v4(&g[12], 239, 1, 0, 255);
	omci_mcast_addr_set_v4(g, &g[12]);
	CHECK(!omci_mcast_acl_matches(&rows[1], g, zero));
	v4(&g[12], 232, 1, 1, 5);
	omci_mcast_addr_set_v4(g, &g[12]);
	CHECK(omci_mcast_acl_matches(&rows[2], g, s));
	CHECK(!omci_mcast_acl_matches(&rows[2], g, zero));
	down(&fx);
}

static void test_overlapping_rows_are_refused(void)
{
	static const uint8_t lead_dst[12] = { 0xff, 0x3e, 0, 0, 0, 0,
					      0,    0,	  0, 0, 0, 0 };
	uint8_t row[24], flat[24 * 8];
	struct fixture fx;

	up(&fx);
	acl0(row, 1u, 0u, IP4(239, 1, 0, 0), IP4(239, 1, 0, 255), 0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_OK);

	/* Shares groups with row 1, which takes any source. */
	acl0(row, 2u, 0u, IP4(239, 1, 0, 128), IP4(239, 1, 1, 0), 0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_PARAM_ERROR);
	acl0(row, 2u, IP4(10, 0, 0, 1), IP4(239, 1, 0, 128), IP4(239, 1, 1, 0),
	     0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_PARAM_ERROR);

	/* Row 1 rewritten to name a source: other sources may share. */
	acl0(row, 1u, IP4(10, 0, 0, 2), IP4(239, 1, 0, 0), IP4(239, 1, 0, 255),
	     0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_OK);
	acl0(row, 2u, IP4(10, 0, 0, 1), IP4(239, 1, 0, 128), IP4(239, 1, 1, 0),
	     0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_OK);
	acl0(row, 3u, IP4(10, 0, 0, 2), IP4(239, 1, 0, 255),
	     IP4(239, 1, 0, 255), 0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_PARAM_ERROR);

	/* A range that ends before it starts. */
	acl0(row, 4u, 0u, IP4(239, 9, 0, 2), IP4(239, 9, 0, 1), 0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_PARAM_ERROR);

	/* Row 4 made IPv6 first: the same four bytes no longer overlap. */
	memset(row, 0, 24);
	ctrl(row, OP_WRITE, 2, 4u);
	memcpy(&row[2], lead_dst, 12);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_OK);
	acl0(row, 4u, IP4(10, 0, 0, 2), IP4(239, 1, 0, 0), IP4(239, 1, 0, 255),
	     0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_OK);
	/* Turning row 4 back into IPv4 would overlap row 1. */
	memset(row, 0, 24);
	ctrl(row, OP_WRITE, 2, 4u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_PARAM_ERROR);

	/* Deletes and clears are not judged; part 3 is refused anyway. */
	CHECK(!omci_mcast_acl_conflicts(fx.t.mib, PROFILE, row, 23));
	ctrl(row, OP_WRITE, 3, 4u);
	CHECK(!omci_mcast_acl_conflicts(fx.t.mib, PROFILE, row, 24));
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_PARAM_ERROR);
	ctrl(row, OP_DELETE, 0, 4u);
	CHECK(!omci_mcast_acl_conflicts(fx.t.mib, PROFILE, row, 24));
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 7u, row, 24), R_OK);
	CHECK_EQ(walk(fx.t.eng, 309u, PROFILE, 7u, flat, sizeof(flat)), 48L);

	/* The static list may overlap. */
	acl0(row, 1u, 0u, IP4(239, 1, 0, 0), IP4(239, 1, 0, 255), 0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 8u, row, 24), R_OK);
	acl0(row, 2u, 0u, IP4(239, 1, 0, 0), IP4(239, 1, 0, 255), 0u);
	CHECK_EQ(set_attr(fx.t.eng, 309u, PROFILE, 8u, row, 24), R_OK);
	CHECK_EQ(walk(fx.t.eng, 309u, PROFILE, 8u, flat, sizeof(flat)), 48L);
	down(&fx);
}

static void test_service_packages_are_read_in_key_order(void)
{
	struct omci_mcast_package pkg[4];
	uint8_t row[20];
	struct fixture fx;

	up(&fx);
	memset(row, 0, sizeof(row));
	ctrl(row, OP_WRITE, 0, 4u);
	put16(&row[2], 100u);
	put16(&row[4], 8u);
	put32(&row[6], 1000000u);
	put16(&row[10], PROFILE);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 6u, row, 20), R_OK);
	ctrl(row, OP_WRITE, 0, 2u);
	put16(&row[2], OMCI_MCAST_VID_UNTAGGED);
	put16(&row[4], 0u);
	put32(&row[6], 0u);
	put16(&row[10], 0x0202u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 6u, row, 20), R_OK);

	CHECK_EQ(omci_mcast_packages_read(fx.t.mib, SUBSCRIBER, pkg, 1u), 1u);
	CHECK_EQ(omci_mcast_packages_read(fx.t.mib, SUBSCRIBER, pkg,
					  ARRAY_SIZE(pkg)),
		 2u);
	CHECK_EQ(pkg[0].key, 2u);
	CHECK_EQ(pkg[0].vid, OMCI_MCAST_VID_UNTAGGED);
	CHECK_EQ(pkg[0].profile, 0x0202u);
	CHECK_EQ(pkg[1].key, 4u);
	CHECK_EQ(pkg[1].vid, 100u);
	CHECK_EQ(pkg[1].max_groups, 8u);
	CHECK_EQ(pkg[1].max_bandwidth, 1000000u);
	CHECK_EQ(pkg[1].profile, PROFILE);
	down(&fx);
}

/* The two parts of an allowed preview group. */
static void preview0(uint8_t row[22], unsigned int key, const uint8_t *source,
		     unsigned int vlan_ani, unsigned int vlan_uni)
{
	memset(row, 0, 22);
	ctrl(row, OP_WRITE, 0, key);
	memcpy(&row[2], source, 16);
	put16(&row[18], vlan_ani);
	put16(&row[20], vlan_uni);
}

static void preview1(uint8_t row[22], unsigned int key, const uint8_t *group,
		     unsigned int duration, unsigned int time_left)
{
	memset(row, 0, 22);
	ctrl(row, OP_WRITE, 1, key);
	memcpy(&row[2], group, 16);
	put16(&row[18], duration);
	put16(&row[20], time_left);
}

static void test_preview_groups_are_read_with_both_parts(void)
{
	static const uint8_t none[16] = { 0 };
	static const uint8_t v6[16] = { 0xff, 0x3e, 0, 0, 0, 0, 0,    0,
					0,    0,    0, 0, 0, 0, 0x12, 0x34 };
	uint8_t g4[16] = { 0 }, s4[16] = { 0 }, row[22];
	struct omci_mcast_preview pv[4];
	struct fixture fx;

	up(&fx);
	v4(&g4[12], 239, 5, 5, 5);
	v4(&s4[12], 10, 1, 1, 1);
	preview0(row, 1u, none, 200u, 410u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	preview1(row, 1u, g4, 0u, 0u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	preview0(row, 2u, s4, 200u, 410u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	preview0(row, 3u, s4, 201u, 36u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	preview1(row, 3u, v6, 30u, 0u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);

	CHECK_EQ(omci_mcast_previews_read(fx.t.mib, SUBSCRIBER, pv, 1u), 1u);
	CHECK_EQ(omci_mcast_previews_read(fx.t.mib, SUBSCRIBER, pv,
					  ARRAY_SIZE(pv)),
		 2u);
	CHECK_EQ(pv[0].key, 1u);
	CHECK(pv[0].any_source);
	CHECK(omci_mcast_addr_v4(pv[0].group));
	CHECK_EQ(be32_at(pv[0].group, 12), IP4(239, 5, 5, 5));
	CHECK_EQ(pv[0].vlan_ani, 200u);
	CHECK_EQ(pv[0].vlan_uni, 410u);
	CHECK_EQ(pv[0].duration, 0u);
	CHECK_EQ(pv[1].key, 3u);
	CHECK(!pv[1].any_source);
	CHECK(omci_mcast_addr_v4(pv[1].source));
	CHECK_MEM(pv[1].group, v6, 16);
	CHECK_EQ(pv[1].duration, 30u);
	CHECK_EQ(pv[1].time_left, 30u);
	down(&fx);
}

/* The time left of the allowed preview group with @key, or -1 without one. */
static long time_left(struct omci_mib *mib, unsigned int key)
{
	struct omci_mcast_preview pv[8];
	const size_t n =
		omci_mcast_previews_read(mib, SUBSCRIBER, pv, ARRAY_SIZE(pv));

	for (size_t i = 0; i < n; i++)
		if (pv[i].key == key)
			return pv[i].time_left;
	return -1;
}

static void test_the_onu_keeps_the_time_left_of_a_preview_group(void)
{
	static const uint8_t none[16] = { 0 };
	uint8_t g[16] = { 0 }, row[22];
	struct fixture fx;

	up(&fx);
	v4(&g[12], 239, 5, 5, 6);
	preview0(row, 1u, none, 200u, 410u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	/* A new row starts with its duration, whatever the OLT sends. */
	preview1(row, 1u, g, 60u, 999u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	CHECK_EQ(time_left(fx.t.mib, 1u), 60L);

	/* Ten minutes on: a longer duration adds the difference. */
	preview1(row, 1u, g, 60u, 50u);
	store(fx.t.mib, 310u, SUBSCRIBER, 7u, row, 22);
	CHECK_EQ(time_left(fx.t.mib, 1u), 50L);
	preview1(row, 1u, g, 90u, 0u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	CHECK_EQ(time_left(fx.t.mib, 1u), 80L);
	/* A shorter one takes it away, and once nothing is left, the row. */
	preview1(row, 1u, g, 20u, 0u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	CHECK_EQ(time_left(fx.t.mib, 1u), 10L);
	preview1(row, 1u, g, 5u, 0u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	CHECK_EQ(time_left(fx.t.mib, 1u), -1L);
	CHECK_EQ(omci_mib_table_count(fx.t.mib, 310u, SUBSCRIBER, 7u), 0u);

	/* No duration: no time left; the most there can be is 0xffff. */
	preview0(row, 2u, none, 200u, 410u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	preview1(row, 2u, g, 0u, 77u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	CHECK_EQ(time_left(fx.t.mib, 2u), 0L);
	preview1(row, 2u, g, 0x10u, 0xfff0u);
	store(fx.t.mib, 310u, SUBSCRIBER, 7u, row, 22);
	preview1(row, 2u, g, 0xffffu, 0u);
	CHECK_EQ(set_attr(fx.t.eng, 310u, SUBSCRIBER, 7u, row, 22), R_OK);
	CHECK_EQ(time_left(fx.t.mib, 2u), 0xffffL);

	/* A part 0, a delete or a short row is left as it is. */
	preview0(row, 2u, none, 201u, 36u);
	CHECK(omci_mcast_preview_written(fx.t.mib, SUBSCRIBER, row, 22));
	CHECK_EQ(be16_at(row, 18), 201u);
	preview1(row, 2u, g, 1u, 0u);
	CHECK(omci_mcast_preview_written(fx.t.mib, SUBSCRIBER, row, 21));
	ctrl(row, OP_DELETE, 1, 2u);
	CHECK(omci_mcast_preview_written(fx.t.mib, SUBSCRIBER, row, 22));
	CHECK_EQ(be16_at(row, 20), 0u);
	down(&fx);
}

int main(void)
{
	RUN(test_create_refuses_values_the_attributes_do_not_take);
	RUN(test_sets_refuse_values_out_of_range);
	RUN(test_table_control_writes_deletes_and_clears);
	RUN(test_the_table_control_functions_refuse_what_cannot_be_asked);
	RUN(test_acl_rows_are_put_together_from_their_parts);
	RUN(test_overlapping_rows_are_refused);
	RUN(test_service_packages_are_read_in_key_order);
	RUN(test_preview_groups_are_read_with_both_parts);
	RUN(test_the_onu_keeps_the_time_left_of_a_preview_group);
	return OMCI_T_REPORT("test_mcast");
}
