// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the answer to each message type, frame by frame. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"
#include "omcid/array_size.h"

static uint8_t last_tx[OMCI_FRAME_LEN];
static int tx_count;

static void on_send(void *ctx, const uint8_t f[static OMCI_FRAME_LEN])
{
	(void)ctx;
	memcpy(last_tx, f, OMCI_FRAME_LEN);
	tx_count++;
}
static const struct omci_tx_ops tx_ops = { .send = on_send };

/*
 * Five requests to the ONU data and the ONU-G entities, and the answer the
 * standard gives each: the request's identifier, class and instance, the
 * acknowledgement bit, and contents that are all zero but for the first
 * byte. That byte is a count of zero for Get all alarms (no alarm stands)
 * and a result code for the others: 0 for done, 2 for an action the class
 * does not accept (G.988 A.1.1).
 */
static const struct {
	uint8_t msg_type;
	uint16_t tci;
	uint16_t class_id;
	uint16_t instance;
	const char *what;
	uint8_t first;
} cases[] = {
	{ 11, 0x0015, 2, 0, "Get all alarms", 0 },
	{ 12, 0x0017, 2, 0, "Get all alarms next", 0 },
	{ 18, 0x001B, 256, 0, "Test of the ONU-G", 0 },
	{ 24, 0x0112, 2, 0, "Synchronize time on the ONU data", 2 },
	{ 28, 0x0115, 2, 0, "Get current data on the ONU data", 2 },
};

int main(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;

	if (omci_hw_caps(&caps) != OMCI_OK || omci_mib_init(&mib) != OMCI_OK ||
	    omci_me_defaults_apply(mib, &caps) != OMCI_OK ||
	    omci_engine_init(&eng, mib, &tx_ops, NULL) != OMCI_OK) {
		printf("test_msgtypes: setup failed\n");
		return 1;
	}

	printf("  five message types and the answers the standard gives:\n");
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct omci_frame f = {
			.tci = cases[i].tci,
			.msg_type = cases[i].msg_type,
			.ar = true,
			.ak = false,
			.device_id = 0x0a,
			.class_id = cases[i].class_id,
			.instance = cases[i].instance,
		};
		uint8_t req[OMCI_FRAME_LEN];
		(void)!omci_frame_build(&f, req);

		struct omci_frame a = f;
		uint8_t want[OMCI_FRAME_LEN];

		a.ar = false;
		a.ak = true;
		a.contents[0] = cases[i].first;
		(void)!omci_frame_build(&a, want);

		tx_count = 0;
		const enum omci_status rc = omci_engine_rx(eng, req);
		const bool answered = rc == OMCI_OK && tx_count == 1;
		const bool same = answered &&
				  memcmp(last_tx, want, OMCI_FRAME_LEN) == 0;

		const char *verdict = "NOT ANSWERED";

		if (same)
			verdict = "as the standard gives";
		else if (answered)
			verdict = "DIFFERS";
		printf("    MT %-2u %-34s %s\n", cases[i].msg_type,
		       cases[i].what, verdict);
		CHECK(same);
	}

	struct omci_frame unknown = {
		.tci = 0x7000,
		.msg_type = 29,
		.ar = true,
		.ak = false,
		.device_id = 0x0a,
		.class_id = 2,
		.instance = 0,
	};
	uint8_t req[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&unknown, req);
	tx_count = 0;
	(void)!omci_engine_rx(eng, req);
	CHECK(tx_count == 1);

	CHECK(last_tx[8] == 2u);

	CHECK(!omci_mib_exists(mib, 134, 0x7FFF));
	struct omci_frame test_ghost = {
		.tci = 0x7001,
		.msg_type = 18,
		.ar = true,
		.ak = false,
		.device_id = 0x0a,
		.class_id = 134,
		.instance = 0x7FFF,
	};
	(void)!omci_frame_build(&test_ghost, req);
	tx_count = 0;
	(void)!omci_engine_rx(eng, req);
	CHECK(tx_count == 1);
	CHECK(last_tx[8] == 5u);

	{
		const uint16_t ME_PM_DS = 321u;
		const uint16_t PM_INST = 0x0101u;

		CHECK(omci_mib_create(mib, ME_PM_DS, PM_INST) == OMCI_OK);

		uint8_t as_get[OMCI_FRAME_LEN];
		struct omci_frame g = {
			.tci = 0x7002,
			.msg_type = 9,
			.ar = true,
			.ak = false,
			.device_id = 0x0a,
			.class_id = ME_PM_DS,
			.instance = PM_INST,
		};
		g.contents[0] = 0xC0;
		(void)!omci_frame_build(&g, req);
		tx_count = 0;
		(void)!omci_engine_rx(eng, req);
		CHECK(tx_count == 1);
		memcpy(as_get, last_tx, OMCI_FRAME_LEN);

		struct omci_frame c = g;

		c.tci = 0x7003;
		c.msg_type = 28;
		(void)!omci_frame_build(&c, req);
		tx_count = 0;
		(void)!omci_engine_rx(eng, req);
		CHECK(tx_count == 1);

		CHECK(as_get[8] == 0u);
		CHECK(last_tx[8] == 0u);
		CHECK(memcmp(&as_get[9], &last_tx[9], 2) == 0);
	}

	omci_engine_fini(eng);
	omci_mib_fini(mib);

	printf("%s (%d omci_t_checks)\n",
	       omci_t_failures ? "test_msgtypes FAILED" : "test_msgtypes OK",
	       omci_t_checks);
	return OMCI_T_REPORT("test_msgtypes");
}
