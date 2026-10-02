// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the checks the engine makes before it executes a request. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

#define R_OK 0u
#define R_INSTANCE_EXISTS 7u

#define MT_CREATE 4u
#define MT_SET 8u
#define MT_GET 9u
#define MT_MIB_RESET 15u
#define MT_DOWNLOAD_SECTION 20u

#define MT_FIELD(f) ((unsigned int)((f)[2] & 0x1Fu))
#define MT_AK(f) (((f)[2] & 0x20u) != 0u)

static uint8_t last_tx[OMCI_FRAME_LEN];
static int tx_count;

static void on_send(void *ctx, const uint8_t f[static OMCI_FRAME_LEN])
{
	(void)ctx;
	memcpy(last_tx, f, OMCI_FRAME_LEN);
	tx_count++;
}
static const struct omci_tx_ops tx_ops = { .send = on_send };

static void build(uint8_t out[OMCI_FRAME_LEN], uint16_t tci, uint8_t mt,
		  bool ar, bool ak, uint16_t class_id, uint16_t inst)
{
	struct omci_frame f = {
		.tci = tci,
		.msg_type = mt,
		.ar = ar,
		.ak = ak,
		.device_id = 0x0a,
		.class_id = class_id,
		.instance = inst,
	};
	(void)!omci_frame_build(&f, out);
}

int main(void)
{
	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &tx_ops, NULL), OMCI_OK);

	uint8_t f[OMCI_FRAME_LEN];

	build(f, 10, MT_CREATE, true, false, 171, 0x0102);
	f[20] ^= 0xFFu;
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_ERR_MIC);
	CHECK_EQ(tx_count, 0);
	CHECK(!omci_mib_exists(mib, 171, 0x0102));

	build(f, 11, MT_SET, false, false, 171, 0x0102);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_ERR_PROTO);
	CHECK_EQ(tx_count, 0);
	build(f, 14, MT_SET, true, true, 171, 0x0102);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_ERR_PROTO);
	CHECK_EQ(tx_count, 0);
	build(f, 12, MT_DOWNLOAD_SECTION, false, false, 7, 0);
	CHECK(omci_engine_rx(eng, f) != OMCI_ERR_PROTO);

	build(f, 100, MT_CREATE, true, false, 171, 0x0103);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);
	CHECK_EQ(last_tx[8], R_OK);
	uint8_t first[OMCI_FRAME_LEN];

	memcpy(first, last_tx, OMCI_FRAME_LEN);

	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);
	CHECK(memcmp(first, last_tx, OMCI_FRAME_LEN) == 0);
	CHECK_EQ(last_tx[8], R_OK);

	build(f, 101, MT_CREATE, true, false, 171, 0x0103);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);
	CHECK_EQ(last_tx[8], R_INSTANCE_EXISTS);

	for (uint16_t k = 0; k < 13; k++) {
		build(f, (uint16_t)(1000u + k), MT_GET, true, false, 171,
		      0x0103);
		CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	}

	build(f, 1013, MT_CREATE, true, false, 171, 0x0104);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);
	CHECK_EQ(last_tx[8], R_OK);

	build(f, 100, MT_CREATE, true, false, 171, 0x0103);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);
	CHECK_EQ(last_tx[8], R_OK);

	build(f, (uint16_t)(1000u + 14), MT_GET, true, false, 171, 0x0103);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);

	build(f, 100, MT_CREATE, true, false, 171, 0x0103);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);
	CHECK_EQ(last_tx[8], R_INSTANCE_EXISTS);

	build(f, 200, MT_MIB_RESET, true, false, 2, 0);
	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);

	tx_count = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(tx_count, 1);
	CHECK_EQ(MT_FIELD(last_tx), MT_MIB_RESET);
	CHECK(MT_AK(last_tx));
	CHECK(MT_FIELD(last_tx) != 17u);

	CHECK(omci_mib_exists(mib, 5, 0x0101));
	{
		struct omci_frame s = {
			.tci = 300,
			.msg_type = MT_SET,
			.ar = true,
			.ak = false,
			.device_id = 0x0a,
			.class_id = 5,
			.instance = 0x0101,
		};
		s.contents[0] = 0xC0;
		s.contents[1] = 0x00;
		s.contents[2] = 0xAA;
		s.contents[3] = 0x5B;
		(void)!omci_frame_build(&s, f);

		tx_count = 0;
		CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
		CHECK_EQ(tx_count, 1);
		CHECK_EQ(last_tx[8], 9u);
		CHECK_EQ(last_tx[9], 0u);
		CHECK_EQ(last_tx[10], 0u);
		CHECK_EQ(last_tx[11], 0x80u);
		CHECK_EQ(last_tx[12], 0u);

		uint8_t got = 0;
		size_t n = 0;

		CHECK_EQ(omci_mib_attr_get(mib, 5, 0x0101, 1, &got, sizeof(got),
					   &n),
			 OMCI_OK);
		CHECK(got != 0xAAu);

		CHECK_EQ(omci_mib_attr_get(mib, 5, 0x0101, 2, &got, sizeof(got),
					   &n),
			 OMCI_OK);
		CHECK_EQ(got, 0x5Bu);
	}

	omci_engine_fini(eng);
	omci_mib_fini(mib);

	printf("%s (%d omci_t_checks)\n",
	       omci_t_failures ? "test_checks FAILED" : "test_checks OK",
	       omci_t_checks);
	return OMCI_T_REPORT("test_checks");
}
