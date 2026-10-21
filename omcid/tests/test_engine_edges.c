// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the edge cases of the engine: full MIB, refused hardware, odd requests. */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/pm.h"
#include "me/table.h"
#include "me/tables.h"
#include "mib/store.h"
#include "omci/alarm.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

enum {
	MT_CREATE = 4,
	MT_DELETE = 6,
	MT_SET = 8,
	MT_GET = 9,
	MT_MIB_UPLOAD = 13,
	MT_MIB_UPLOAD_NEXT = 14,
	MT_MIB_RESET = 15,
};

enum {
	RES_OK = 0,
	RES_PROCESSING = 1,
	RES_NOT_SUPPORTED = 2,
	RES_PARAM = 3,
	RES_UNKNOWN_ME = 4,
	RES_UNKNOWN_INST = 5,
	RES_BUSY = 6,
	RES_EXISTS = 7,
	RES_ATTR_FAILED = 9,
};

static char g_log[1u << 15];

static void frame(uint8_t out[OMCI_FRAME_LEN], uint16_t tci, uint8_t mt,
		  uint16_t cls, uint16_t inst, const uint8_t *contents,
		  size_t n)
{
	struct omci_frame f = {
		.tci = tci,
		.msg_type = mt,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = cls,
		.instance = inst,
	};
	if (n > 0u)
		memcpy(f.contents, contents, n);
	(void)!omci_frame_build(&f, out);
}

static uint16_t g_tci;

static uint8_t send_rx(struct omci_t_fixture *fx, uint8_t mt, uint16_t cls,
		       uint16_t inst, const uint8_t *contents, size_t n)
{
	uint8_t wire[OMCI_FRAME_LEN];

	frame(wire, ++g_tci, mt, cls, inst, contents, n);
	omci_t_tx_reset();
	(void)!omci_engine_rx(fx->eng, wire);
	const uint8_t *last = omci_t_last_tx();

	return last != NULL ? last[8] : 0xffu;
}

static bool fixture(struct omci_t_fixture *fx)
{
	struct omci_hw_caps caps;

	if (!omci_t_up(fx) || omci_hw_caps(&caps) != OMCI_OK ||
	    omci_me_defaults_apply(fx->mib, &caps) != OMCI_OK)
		return false;
	omci_log_level_set(OMCI_LOG_ERR);
	return true;
}

static enum omci_status g_vlan_says, g_gem_says, g_bridge_says, g_caps_says;
static uint16_t g_gem_rejects;

static enum omci_status spy_caps(struct omci_hw_caps *out)
{
	if (g_caps_says != OMCI_OK)
		return g_caps_says;
	return omci_hw_caps(out);
}

static enum omci_status spy_vlan(const struct omci_hw_vlan_xlate *r, size_t n,
				 struct omci_hw_reject *rej)
{
	(void)r;
	(void)n;
	if (rej != NULL)
		*rej = (struct omci_hw_reject){ 0 };
	return g_vlan_says;
}

static enum omci_status spy_gem(const struct omci_hw_gem_port *p, size_t n,
				struct omci_hw_reject *rej)
{
	(void)p;
	(void)n;
	if (rej != NULL) {
		*rej = (struct omci_hw_reject){ 0 };
		if (g_gem_rejects != 0u)
			omci_hw_reject_add(rej, g_gem_rejects);
	}
	return g_gem_says;
}

static unsigned int g_bridge_calls;

static enum omci_status spy_bridge(const struct omci_hw_bridge_port *p,
				   size_t n, struct omci_hw_reject *rej)
{
	(void)p;
	(void)n;
	g_bridge_calls++;
	if (rej != NULL)
		*rej = (struct omci_hw_reject){ 0 };
	return g_bridge_says;
}

static struct omci_hw_ops g_spy;

static void spy_on(void)
{
	g_spy = *omci_hw();
	g_spy.caps = spy_caps;
	g_spy.vlan_apply = spy_vlan;
	g_spy.gem_apply = spy_gem;
	g_spy.bridge_apply = spy_bridge;
	g_vlan_says = g_gem_says = g_bridge_says = g_caps_says = OMCI_OK;
	g_gem_rejects = 0;
	omci_hw_ops_set(&g_spy);
}

static void spy_off(void)
{
	omci_hw_ops_set(NULL);
}

static uint8_t create_gem(struct omci_t_fixture *fx, uint16_t inst,
			  uint16_t port_id)
{
	const uint8_t c[5] = { (uint8_t)(port_id >> 8), (uint8_t)port_id, 0x80,
			       0x00, 3 };
	return send_rx(fx, MT_CREATE, 268, inst, c, sizeof(c));
}

static void test_every_answer_of_the_device_has_a_result_code(void)
{
	static const struct {
		int32_t status;
		uint8_t result;
		const char *why;
	} want[] = {
		{ OMCI_OK, RES_OK, "done" },
		{ OMCI_ERR_ME_UNKNOWN, RES_UNKNOWN_ME, "no such class" },
		{ OMCI_ERR_INST_UNKNOWN, RES_UNKNOWN_INST, "no such instance" },
		{ OMCI_ERR_EXISTS, RES_EXISTS, "already there" },
		{ OMCI_ERR_ATTR_UNKNOWN, RES_PARAM, "no such attribute" },
		{ OMCI_ERR_INVAL, RES_PARAM, "impossible argument" },
		{ OMCI_ERR_NOMEM, RES_BUSY, "out of room" },
		{ OMCI_ERR_BUSY, RES_BUSY, "busy" },
		{ OMCI_ERR_HW, RES_PROCESSING,
		  "the device could not be reached" },
		{ OMCI_ERR_IO, RES_PROCESSING, "a call failed" },
		{ OMCI_ERR_AGAIN, RES_PARAM,
		  "not a status the engine knows what to say for" },
		{ OMCI_ERR_TRUNCATED, RES_PARAM, "likewise" },
		{ OMCI_ERR_PROTO, RES_PARAM, "likewise" },
		{ OMCI_ERR_MIC, RES_PARAM, "likewise" },
		{ (enum omci_status)99, RES_PARAM, "a status that is not one" },
	};

	static_assert(sizeof(want[0]) == 8 + sizeof(const char *),
		      "the status keeps the width its fixed type gave it");
	for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
		struct omci_t_fixture fx;

		CHECK(fixture(&fx));
		spy_on();
		g_gem_says = want[i].status;
		const uint8_t got = create_gem(&fx, 0x0210, 300);

		if (got != want[i].result)
			printf("    %s (status %d): result %u, expected %u\n",
			       want[i].why, (int)want[i].status, got,
			       want[i].result);
		CHECK_EQ(got, want[i].result);
		spy_off();
		omci_t_down(&fx);
	}

	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	spy_on();
	g_gem_says = OMCI_DECLINED;
	g_gem_rejects = 300;
	CHECK_EQ(create_gem(&fx, 0x0210, 300), RES_PARAM);
	g_gem_rejects = 999;
	CHECK_EQ(create_gem(&fx, 0x0211, 301), RES_OK);
	spy_off();
	omci_t_down(&fx);
}

static void test_a_delete_that_cannot_reach_the_device_says_so(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	static const struct {
		uint16_t cls;
		uint16_t inst;
		const char *name;
	} cls[] = {
		{ 171, 0x0301, "extended VLAN tagging" },
		{ 268, 0x0302, "GEM port" },
		{ 45, 0x0303, "MAC bridge" },
	};
	for (unsigned int k = 0; k < 3u; k++) {
		for (unsigned int how = 0; how < 2u; how++) {
			const uint8_t made =
				cls[k].cls == 268 ?
					create_gem(&fx, cls[k].inst,
						   (uint16_t)(400u + k)) :
					send_rx(&fx, MT_CREATE, cls[k].cls,
						cls[k].inst, NULL, 0);
			CHECK_EQ(made, RES_OK);
			spy_on();
			const enum omci_status says = how == 0u ? OMCI_ERR_HW :
								  OMCI_ERR_IO;
			g_vlan_says = g_gem_says = g_bridge_says = says;
			const uint8_t got = send_rx(&fx, MT_DELETE, cls[k].cls,
						    cls[k].inst, NULL, 0);
			if (got != RES_PROCESSING)
				printf("    %s deleted with the device saying %d: result %u\n",
				       cls[k].name, (int)says, got);
			CHECK_EQ(got, RES_PROCESSING);
			CHECK(!omci_mib_exists(fx.mib, cls[k].cls,
					       cls[k].inst));
			spy_off();
		}
	}

	CHECK_EQ(send_rx(&fx, MT_CREATE, 268, 0x0310,
			 (const uint8_t[5]){ 0x01, 0x2c, 0x80, 0, 3 }, 5),
		 RES_OK);
	spy_on();
	g_gem_says = OMCI_DECLINED;
	g_gem_rejects = 999;
	CHECK_EQ(send_rx(&fx, MT_DELETE, 268, 0x0310, NULL, 0), RES_OK);
	spy_off();
	omci_t_down(&fx);
}

static void row_words(uint8_t out[16], unsigned int pri, unsigned int match,
		      unsigned int assign)
{
	const uint32_t w[4] = {
		0xf8000000u,
		(pri << 28) | (match << 15) | (5u << 12),
		0x400f0000u,
		(pri << 16) | (assign << 3) | 2u,
	};
	for (int k = 0; k < 4; k++) {
		out[k * 4 + 0] = (uint8_t)(w[k] >> 24);
		out[k * 4 + 1] = (uint8_t)(w[k] >> 16);
		out[k * 4 + 2] = (uint8_t)(w[k] >> 8);
		out[k * 4 + 3] = (uint8_t)w[k];
	}
}

static void
test_a_table_row_that_cannot_reach_the_switch_fails_the_attribute(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	CHECK_EQ(send_rx(&fx, MT_CREATE, 171, 0x0401, NULL, 0), RES_OK);

	uint8_t set[18] = { 0x04, 0x00 };

	row_words(&set[2], 6, 410, 1410);
	CHECK_EQ(send_rx(&fx, MT_SET, 171, 0x0401, set, sizeof(set)), RES_OK);

	row_words(&set[2], 5, 36, 1502);
	omci_hw_vlan_fail_next();
	CHECK_EQ(send_rx(&fx, MT_SET, 171, 0x0401, set, sizeof(set)),
		 RES_ATTR_FAILED);
	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	if (rsp != NULL) {
		CHECK_EQ((unsigned int)(rsp[9] << 8 | rsp[10]), 0u);
		CHECK_EQ((unsigned int)(rsp[11] << 8 | rsp[12]), 0x0400u);
	}

	uint8_t del[18] = { 0x04, 0x00 };

	row_words(&del[2], 6, 410, 1410);
	memset(&del[2 + 8], 0xff, 8);
	omci_hw_vlan_fail_next();
	CHECK_EQ(send_rx(&fx, MT_SET, 171, 0x0401, del, sizeof(del)),
		 RES_ATTR_FAILED);
	rsp = omci_t_last_tx();
	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ((unsigned int)(rsp[11] << 8 | rsp[12]), 0x0400u);

	CHECK_EQ(send_rx(&fx, MT_SET, 171, 0x0401, del, sizeof(del)), RES_OK);
	omci_t_down(&fx);
}

static void test_a_create_into_a_full_mib_says_busy(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	const size_t room = OMCI_MIB_MAX_ROWS - omci_mib_count(fx.mib);

	for (size_t i = 0; i < room; i++)
		CHECK_EQ(send_rx(&fx, MT_CREATE, 84, (uint16_t)(0x1000u + i),
				 NULL, 0),
			 RES_OK);
	CHECK_EQ(omci_mib_count(fx.mib), (size_t)OMCI_MIB_MAX_ROWS);

	char log[2048];

	omci_log_level_set(OMCI_LOG_ERR);
	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(send_rx(&fx, MT_CREATE, 84, 0x7000, NULL, 0),
			 RES_BUSY);
	});
	CHECK(strstr(log, "mib full: 512 rows") != NULL);
	CHECK(!omci_mib_exists(fx.mib, 84, 0x7000));
	CHECK_EQ(send_rx(&fx, MT_CREATE, 84, 0x1000, NULL, 0), RES_EXISTS);
	omci_t_down(&fx);
}

static void test_an_upload_reads_what_is_there_when_it_asks(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	(void)send_rx(&fx, MT_MIB_UPLOAD, 2, 0, NULL, 0);
	CHECK(omci_t_last_tx() != NULL);

	CHECK_EQ(omci_mib_delete(fx.mib, 2, 0), OMCI_OK);
	uint8_t want[2] = { 0, 0 };

	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		(void)send_rx(&fx, MT_MIB_UPLOAD_NEXT, 2, 0, want,
			      sizeof(want));
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK(omci_t_log_count(g_log, "mib upload next: 2/0 attribute ") > 0u);
	CHECK(strstr(g_log, " is gone mid-upload") != NULL);
	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	if (rsp != NULL) {
		CHECK_EQ((unsigned int)(rsp[8] << 8 | rsp[9]), 2u);
		CHECK_EQ((unsigned int)(rsp[10] << 8 | rsp[11]), 0u);
		CHECK((rsp[12] << 8 | rsp[13]) != 0);
	}
	omci_t_down(&fx);
}

static void test_a_mib_reset_that_cannot_read_the_device(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	CHECK_EQ(send_rx(&fx, MT_CREATE, 84, 0x0501, NULL, 0), RES_OK);
	CHECK(omci_mib_count(fx.mib) > 30u);

	spy_on();
	g_caps_says = OMCI_ERR_HW;
	omci_log_level_set(OMCI_LOG_ERR);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(send_rx(&fx, MT_MIB_RESET, 2, 0, NULL, 0), RES_OK);
	});
	CHECK(strstr(g_log, "mib reset: repopulation failed: OMCI_ERR_HW") !=
	      NULL);
	CHECK_EQ(omci_t_tx.n, 1u);
	spy_off();
	omci_t_down(&fx);
}

static void test_the_log_says_what_the_engine_refused_and_what_it_dropped(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_log_level_set(OMCI_LOG_INFO);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(send_rx(&fx, MT_GET, 5, 0xbeef,
				 (const uint8_t[2]){ 0x80, 0x00 }, 2),
			 RES_UNKNOWN_INST);
	});
	CHECK_EQ(omci_t_log_count(g_log, "tx: 5/0xbeef mt=9 answered result 5"),
		 1u);
	CHECK_EQ(omci_t_log_count(g_log, "answered result"), 1u);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(send_rx(&fx, MT_GET, 2, 0,
				 (const uint8_t[2]){ 0x80, 0x00 }, 2),
			 RES_OK);
	});
	CHECK(strstr(g_log, "answered result") == NULL);

	omci_log_level_set(OMCI_LOG_WARN);
	static const struct {
		bool ar, ak;
		const char *says;
	} bad[] = {
		{ true, true,
		  "rx: malformed request tid=17185 mt=9 (ar=1 ak=1)" },
		{ false, false,
		  "rx: malformed request tid=17185 mt=9 (ar=0 ak=0)" },
		{ false, true,
		  "rx: malformed request tid=17185 mt=9 (ar=0 ak=1)" },
	};
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		uint8_t wire[OMCI_FRAME_LEN];
		struct omci_frame f = {
			.tci = 0x4321,
			.msg_type = MT_GET,
			.ar = bad[i].ar,
			.ak = bad[i].ak,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = 2,
			.instance = 0,
		};
		(void)!omci_frame_build(&f, wire);
		omci_t_tx_reset();
		OMCI_T_CAPTURE_LOG(g_log, {
			CHECK_EQ(omci_engine_rx(fx.eng, wire), OMCI_ERR_PROTO);
		});
		CHECK_EQ(omci_t_log_count(g_log, bad[i].says), 1u);
		CHECK_EQ(omci_t_tx.n, 0u);
	}
	omci_log_level_set(OMCI_LOG_ERR);
	omci_t_down(&fx);
}

static void
test_an_upload_next_past_the_units_it_promised_is_answered_empty(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	(void)send_rx(&fx, MT_MIB_UPLOAD, 2, 0, NULL, 0);
	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	if (rsp == NULL) {
		omci_t_down(&fx);
		return;
	}
	const unsigned int units = (unsigned int)(rsp[8] << 8 | rsp[9]);

	CHECK(units > 1u);

	static const unsigned int past[] = { 0u, 1u, 100u };

	for (unsigned int k = 0; k < 4u; k++) {
		const unsigned int seq = k == 0u ? units - 1u :
						   units + past[k - 1u];
		const uint8_t ask[2] = { (uint8_t)(seq >> 8), (uint8_t)seq };

		omci_log_level_set(OMCI_LOG_WARN);
		OMCI_T_CAPTURE_LOG(g_log, {
			(void)send_rx(&fx, MT_MIB_UPLOAD_NEXT, 2, 0, ask,
				      sizeof(ask));
		});
		omci_log_level_set(OMCI_LOG_ERR);
		rsp = omci_t_last_tx();
		CHECK(rsp != NULL);
		if (rsp == NULL)
			continue;
		const unsigned int cls = (unsigned int)(rsp[8] << 8 | rsp[9]);
		const unsigned int inst =
			(unsigned int)(rsp[10] << 8 | rsp[11]);
		const unsigned int mask =
			(unsigned int)(rsp[12] << 8 | rsp[13]);
		char says[96];
		(void)snprintf(
			says, sizeof(says),
			"mib upload next: sequence %u past the %u promised",
			seq, units);
		if (k == 0u) {
			CHECK(cls != 0u && mask != 0u);
			CHECK(strstr(g_log, "past the") == NULL);
		} else {
			CHECK_EQ(cls, 0u);
			CHECK_EQ(inst, 0u);
			CHECK_EQ(mask, 0u);
			CHECK_EQ(omci_t_log_count(g_log, says), 1u);
		}
	}
	omci_t_down(&fx);
}

static void test_an_upload_of_one_unit_and_of_none(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	(void)send_rx(&fx, MT_MIB_UPLOAD, 2, 0, NULL, 0);
	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ((unsigned int)(rsp[8] << 8 | rsp[9]), 0u);
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		(void)send_rx(&fx, MT_MIB_UPLOAD_NEXT, 2, 0,
			      (const uint8_t[2]){ 0, 0 }, 2);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK_EQ(
		omci_t_log_count(
			g_log,
			"mib upload next: sequence 0 with no upload in progress"),
		1u);

	CHECK_EQ(omci_mib_create(fx.mib, 2, 0), OMCI_OK);
	(void)send_rx(&fx, MT_MIB_UPLOAD, 2, 0, NULL, 0);
	rsp = omci_t_last_tx();
	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK_EQ((unsigned int)(rsp[8] << 8 | rsp[9]), 1u);
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		(void)send_rx(&fx, MT_MIB_UPLOAD_NEXT, 2, 0,
			      (const uint8_t[2]){ 0, 0 }, 2);
	});
	CHECK(strstr(g_log, "mib upload next") == NULL);
	rsp = omci_t_last_tx();
	CHECK(rsp != NULL);
	if (rsp != NULL) {
		CHECK_EQ((unsigned int)(rsp[8] << 8 | rsp[9]), 2u);
		CHECK_EQ((unsigned int)(rsp[10] << 8 | rsp[11]), 0u);
		CHECK((rsp[12] << 8 | rsp[13]) != 0);
	}
	OMCI_T_CAPTURE_LOG(g_log, {
		(void)send_rx(&fx, MT_MIB_UPLOAD_NEXT, 2, 0,
			      (const uint8_t[2]){ 0, 1 }, 2);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK_EQ(omci_t_log_count(
			 g_log,
			 "mib upload next: sequence 1 past the 1 promised"),
		 1u);
	omci_t_down(&fx);
}

static void test_a_mib_reset_closes_the_upload(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	(void)send_rx(&fx, MT_MIB_UPLOAD, 2, 0, NULL, 0);
	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	if (rsp != NULL)
		CHECK((rsp[8] << 8 | rsp[9]) > 1);

	CHECK_EQ(send_rx(&fx, MT_MIB_RESET, 2, 0, NULL, 0), RES_OK);
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		(void)send_rx(&fx, MT_MIB_UPLOAD_NEXT, 2, 0,
			      (const uint8_t[2]){ 0, 0 }, 2);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK_EQ(
		omci_t_log_count(
			g_log,
			"mib upload next: sequence 0 with no upload in progress"),
		1u);
	rsp = omci_t_last_tx();
	CHECK(rsp != NULL);
	for (unsigned int i = 8; rsp != NULL && i < 40u; i++)
		CHECK_EQ(rsp[i], 0u);
	omci_t_down(&fx);
}

static void test_the_upload_walks_in_ascending_order(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	omci_log_level_set(OMCI_LOG_ERR);

	static const struct {
		uint16_t cls, inst;
	} made[] = {
		{ 263, 0x8001 }, { 256, 0 },	{ 6, 0x0180 }, { 6, 0x0101 },
		{ 5, 0x0180 },	 { 5, 0x0101 }, { 2, 0 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(made); i++)
		CHECK_EQ(omci_mib_create(fx.mib, made[i].cls, made[i].inst),
			 OMCI_OK);

	(void)send_rx(&fx, MT_MIB_UPLOAD, 2, 0, NULL, 0);
	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	const unsigned int units =
		rsp != NULL ? (unsigned int)(rsp[8] << 8 | rsp[9]) : 0u;
	CHECK(units >= ARRAY_SIZE(made));

	unsigned int prev_cls = 0, prev_inst = 0, distinct = 0, backwards = 0;

	for (unsigned int seq = 0; seq < units; seq++) {
		const uint8_t ask[2] = { (uint8_t)(seq >> 8), (uint8_t)seq };
		(void)send_rx(&fx, MT_MIB_UPLOAD_NEXT, 2, 0, ask, sizeof(ask));
		rsp = omci_t_last_tx();
		CHECK(rsp != NULL);
		if (rsp == NULL)
			continue;
		const unsigned int cls = (unsigned int)(rsp[8] << 8 | rsp[9]);
		const unsigned int inst =
			(unsigned int)(rsp[10] << 8 | rsp[11]);
		if (cls < prev_cls || (cls == prev_cls && inst < prev_inst))
			backwards++;
		if (cls != prev_cls || inst != prev_inst)
			distinct++;
		prev_cls = cls;
		prev_inst = inst;
	}
	CHECK_EQ(backwards, 0u);
	CHECK_EQ(distinct, (unsigned int)(ARRAY_SIZE(made)));
	omci_t_down(&fx);
}

static void test_a_get_of_every_attribute_is_answered_by_the_rule(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	unsigned int classes = 0, with_failed = 0, with_unsupported = 0,
		     whole = 0;
	for (size_t k = 0; k < omci_me_class_count; k++) {
		const struct omci_me_class *cls = &omci_me_classes[k];
		const uint16_t inst = (uint16_t)(0x6100u + k);

		CHECK_EQ(omci_mib_create(fx.mib, cls->class_id, inst), OMCI_OK);

		uint16_t got = 0, unsupported = 0, failed = 0;
		unsigned int used = 0;
		bool stopped = false;

		for (uint8_t attr = 1; attr <= 16u; attr++) {
			const uint16_t bit = (uint16_t)(0x8000u >> (attr - 1u));
			const struct omci_me_attr *a =
				omci_me_attr_get(cls, attr);

			if (a == NULL)
				unsupported = (uint16_t)(unsupported | bit);
			else if (stopped)
				failed = (uint16_t)(failed | bit);
			else if (used + (omci_me_attr_is_table(a) ? 4u :
								    a->size) >
				 25u) {
				failed = (uint16_t)(failed | bit);
				stopped = true;
			} else {
				got = (uint16_t)(got | bit);
				used += omci_me_attr_is_table(a) ? 4u : a->size;
			}
		}

		const uint8_t result = send_rx(&fx, MT_GET, cls->class_id, inst,
					       (const uint8_t[2]){ 0xff, 0xff },
					       2);
		const uint8_t *rsp = omci_t_last_tx();

		CHECK(rsp != NULL);
		if (rsp == NULL)
			continue;
		classes++;
		if (unsupported != 0u || failed != 0u) {
			CHECK_EQ(result, RES_ATTR_FAILED);
			with_failed += failed != 0u ? 1u : 0u;
			with_unsupported += unsupported != 0u ? 1u : 0u;
			CHECK_EQ((unsigned int)(rsp[36] << 8 | rsp[37]),
				 (unsigned int)unsupported);
			CHECK_EQ((unsigned int)(rsp[38] << 8 | rsp[39]),
				 (unsigned int)failed);
		} else {
			CHECK_EQ(result, RES_OK);
			whole++;
		}
		CHECK_EQ((unsigned int)(rsp[9] << 8 | rsp[10]),
			 (unsigned int)got);

		const unsigned int end = 11u + used;
		const unsigned int last =
			(unsigned int)(unsupported != 0u || failed != 0u ? 36u :
									   40u);
		unsigned int stray = 0;

		for (unsigned int i = end; i < last; i++)
			stray += rsp[i] != 0u ? 1u : 0u;
		CHECK_EQ(stray, 0u);
	}
	printf("    %u classes: %u answered whole, %u with attributes it could not deliver, %u with attributes it does not define\n",
	       classes, whole, with_failed, with_unsupported);
	CHECK_EQ(classes, (unsigned int)omci_me_class_count);
	CHECK(with_failed > 5u);
	CHECK(with_unsupported > 5u);
	CHECK(whole > 0u);
	omci_t_down(&fx);
}

static bool builds_bridge(uint16_t class_id)
{
	static const uint16_t classes[] = { 45, 47, 84, 130, 266, 268, 281 };

	for (size_t i = 0; i < ARRAY_SIZE(classes); i++)
		if (classes[i] == class_id)
			return true;
	return false;
}

static void test_only_the_entities_a_bridge_is_made_of_reach_the_bridge(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));
	spy_on();

	unsigned int creatable = 0, bridged = 0;

	for (size_t k = 0; k < omci_me_class_count; k++) {
		const struct omci_me_class *cls = &omci_me_classes[k];

		if ((cls->actions & (1u << MT_CREATE)) == 0u)
			continue;
		creatable++;
		g_bridge_calls = 0;
		const uint8_t made =
			send_rx(&fx, MT_CREATE, cls->class_id, 0x7001, NULL, 0);
		const unsigned int calls = g_bridge_calls;

		if (builds_bridge(cls->class_id)) {
			CHECK_EQ(made, RES_OK);
			CHECK_EQ(calls, 1u);
			bridged++;
		} else {
			CHECK_EQ(calls, 0u);
		}
		if (made == RES_OK &&
		    (cls->actions & (1u << MT_DELETE)) != 0u) {
			g_bridge_calls = 0;
			CHECK_EQ(send_rx(&fx, MT_DELETE, cls->class_id, 0x7001,
					 NULL, 0),
				 RES_OK);
			CHECK_EQ(g_bridge_calls,
				 builds_bridge(cls->class_id) ? 1u : 0u);
		}
	}
	CHECK_EQ(bridged, 7u);
	CHECK(creatable > 7u);

	CHECK_EQ(send_rx(&fx, MT_CREATE, 47, 0x7002, NULL, 0), RES_OK);
	CHECK_EQ(send_rx(&fx, MT_CREATE, 171, 0x7003, NULL, 0), RES_OK);
	g_bridge_calls = 0;
	CHECK_EQ(send_rx(&fx, MT_SET, 47, 0x7002,
			 (const uint8_t[4]){ 0x40, 0x00, 0x12, 0x34 }, 4),
		 RES_OK);
	CHECK_EQ(g_bridge_calls, 1u);
	g_bridge_calls = 0;
	CHECK_EQ(send_rx(&fx, MT_SET, 171, 0x7003,
			 (const uint8_t[3]){ 0x80, 0x00, 0x01 }, 3),
		 RES_OK);
	CHECK_EQ(g_bridge_calls, 0u);

	spy_off();
	omci_t_down(&fx);
}

static void test_the_engine_starts_once_and_says_why_it_cannot(void)
{
	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);

	CHECK_EQ(omci_engine_init(NULL, mib, &omci_t_tx_ops, NULL),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_engine_init(&eng, NULL, &omci_t_tx_ops, NULL),
		 OMCI_ERR_INVAL);
	CHECK(eng == NULL);

	CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL), OMCI_OK);
	struct omci_engine *again = NULL;

	CHECK_EQ(omci_engine_init(&again, mib, &omci_t_tx_ops, NULL),
		 OMCI_ERR_BUSY);
	CHECK(again == NULL);
	omci_engine_fini(eng);

	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL),
		 OMCI_ERR_BUSY);
	omci_alarm_fini(al);

	struct omci_pm *pm = NULL;

	CHECK_EQ(omci_pm_init(&pm, mib), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL),
		 OMCI_ERR_BUSY);
	omci_pm_fini(pm);
	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	omci_alarm_fini(al);

	CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL), OMCI_OK);

	uint8_t wire[OMCI_FRAME_LEN];

	frame(wire, 1, MT_GET, 256, 0, NULL, 0);
	CHECK_EQ(omci_engine_rx(NULL, wire), OMCI_ERR_INVAL);
	omci_engine_tap_set(NULL, NULL, NULL);
	omci_engine_tick(NULL, 5);
	omci_engine_fini(NULL);
	omci_engine_fini(eng);
	omci_mib_fini(mib);
}

static void test_the_model_keeps_what_the_engine_relies_on(void)
{
	for (size_t k = 0; k < omci_me_class_count; k++) {
		const struct omci_me_class *cls = &omci_me_classes[k];
		size_t by_create = 0;

		for (uint8_t n = 1; n <= cls->attr_count; n++) {
			const struct omci_me_attr *a = omci_me_attr_get(cls, n);

			CHECK(a != NULL);
			if (a == NULL)
				continue;

			CHECK((a->access & OMCI_ME_ACC_READ) != 0u);

			if ((a->access & OMCI_ME_ACC_SET_BY_CREATE) != 0u)
				by_create += a->size;

			struct omci_me_table_policy policy;

			if (omci_me_attr_is_table(a) &&
			    (a->access & OMCI_ME_ACC_WRITE) != 0u) {
				CHECK(omci_me_table_policy(cls->class_id, n,
							   &policy));
				CHECK(policy.key_len > 0u &&
				      policy.key_len <= a->size);
			}

			if (omci_me_attr_is_table(a))
				CHECK(a->size >= 2u ||
				      (cls->class_id == 287u && n == 2u &&
				       a->size == 1u));
		}
		CHECK(by_create <= OMCI_CONTENTS_LEN);
	}
}

int main(void)
{
	RUN(test_every_answer_of_the_device_has_a_result_code);
	RUN(test_a_delete_that_cannot_reach_the_device_says_so);
	RUN(test_a_table_row_that_cannot_reach_the_switch_fails_the_attribute);
	RUN(test_a_create_into_a_full_mib_says_busy);
	RUN(test_an_upload_reads_what_is_there_when_it_asks);
	RUN(test_an_upload_next_past_the_units_it_promised_is_answered_empty);
	RUN(test_an_upload_of_one_unit_and_of_none);
	RUN(test_a_mib_reset_closes_the_upload);
	RUN(test_the_upload_walks_in_ascending_order);
	RUN(test_a_get_of_every_attribute_is_answered_by_the_rule);
	RUN(test_the_log_says_what_the_engine_refused_and_what_it_dropped);
	RUN(test_only_the_entities_a_bridge_is_made_of_reach_the_bridge);
	RUN(test_a_mib_reset_that_cannot_read_the_device);
	RUN(test_the_engine_starts_once_and_says_why_it_cannot);
	RUN(test_the_model_keeps_what_the_engine_relies_on);
	return OMCI_T_REPORT("test_engine_edges");
}
