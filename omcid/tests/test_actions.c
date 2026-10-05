// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the action sets: every message type against every class. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/action.h"
#include "me/table.h"
#include "mib/store.h"
#include "omcid/array_size.h"
#include "omcid/ctl_proto.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"
#include "model_tsv.h"

#define INSTANCE 0x0abcu

static bool implemented(unsigned int mt)
{
	switch (mt) {
	case 4:
	case 6:
	case 8:
	case 9:
	case 11:
	case 12:
	case 13:
	case 14:
	case 15:
	case 18:
	case 24:
	case 26:
	case 28:
		return true;
	default:
		return false;
	}
}

static bool has_result_byte(unsigned int mt)
{
	return mt != 11u && mt != 12u && mt != 13u && mt != 14u;
}

static int g_first_rx;

static void on_tap(void *ctx, uint8_t verdict, uint8_t detail,
		   const uint8_t wire[static OMCI_FRAME_LEN])
{
	(void)ctx;
	(void)detail;
	(void)wire;
	if (verdict != OMCI_CTL_TAP_TX && g_first_rx < 0)
		g_first_rx = verdict;
}

static const struct omci_tap_ops tap_ops = { .frame = on_tap };

static unsigned int g_tid;
static uint16_t next_tid(void)
{
	g_tid = (g_tid % 0x7FFEu) + 1u;
	return (uint16_t)g_tid;
}

static const uint8_t *send(struct omci_engine *eng, unsigned int mt,
			   uint16_t cls, uint16_t inst)
{
	uint8_t wire[OMCI_FRAME_LEN];
	const uint16_t tci = next_tid();

	omci_t_build(wire, tci, (uint8_t)mt, true, false, cls, inst);
	g_first_rx = -1;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x20u) != 0u &&
		    (((omci_t_tx.frame[i][0] << 8) | omci_t_tx.frame[i][1]) &
		     0x7FFF) == tci)
			return omci_t_tx.frame[i];
	return NULL;
}

static void fixture(struct omci_t_fixture *fx)
{
	uint8_t wire[OMCI_FRAME_LEN];

	CHECK(omci_t_up(fx));
	omci_t_build(wire, next_tid(), 15u, true, false, 2u, 0u);
	CHECK_EQ(omci_t_rx(fx->eng, wire), OMCI_OK);
	omci_engine_tap_set(fx->eng, &tap_ops, NULL);
}

static void test_every_message_type_to_every_class(void)
{
	struct omci_t_fixture fx;
	unsigned int dispatched = 0, refused_by_class = 0, unhandled = 0;

	fixture(&fx);

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		for (unsigned int mt = 0; mt < 32u; mt++) {
			(void)!omci_mib_create(fx.mib, t->id, INSTANCE);

			const uint16_t inst = t->id == 2u ? 0u : INSTANCE;
			const uint8_t *rsp = send(fx.eng, mt, t->id, inst);
			const bool listed = (t->actions & (1u << mt)) != 0u;

			if (!implemented(mt)) {
				CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_UNHANDLED);
				unhandled++;
			} else if (!listed) {
				CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_ACTION);
				refused_by_class++;
			} else {
				CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_DISPATCH);
				dispatched++;
			}
			if (g_first_rx != OMCI_CTL_TAP_RX_DISPATCH) {
				CHECK(rsp != NULL);
				if (rsp != NULL) {
					if (has_result_byte(mt))
						CHECK_EQ(rsp[8], 2u);
					else
						for (unsigned int k = 8;
						     k < 40u; k++)
							CHECK_EQ(rsp[k], 0u);
				}
				CHECK_EQ(omci_t_tx.n, 1u);
			}
		}
	}

	printf("  %u dispatched, %u refused by the class, %u refused as not implemented\n",
	       dispatched, refused_by_class, unhandled);
	CHECK_EQ(dispatched + refused_by_class + unhandled,
		 tsv_class_count * 32u);
	CHECK(dispatched > 0u);
	CHECK(refused_by_class > 0u);
	CHECK(unhandled > 0u);

	omci_t_down(&fx);
}

static void test_classes_the_daemon_does_not_hold(void)
{
	static const uint16_t unknown[] = { 0,	    1,	    3,	   23,	 51,
					    158,    330,    335,   3000, 0x8000,
					    0xea60, 0xfffd, 0xffff };
	struct omci_t_fixture fx;

	fixture(&fx);
	for (size_t i = 0; i < ARRAY_SIZE(unknown); i++) {
		CHECK(tsv_class(unknown[i]) == NULL);
		for (unsigned int mt = 0; mt < 32u; mt++) {
			const uint8_t *rsp = send(fx.eng, mt, unknown[i], 0u);

			CHECK(rsp != NULL);
			CHECK_EQ(g_first_rx,
				 implemented(mt) ? OMCI_CTL_TAP_RX_UNKNOWN_ME :
						   OMCI_CTL_TAP_RX_UNHANDLED);
			if (rsp != NULL && has_result_byte(mt))
				CHECK_EQ(rsp[8], implemented(mt) ? 4u : 2u);
			if (rsp != NULL && !has_result_byte(mt))
				for (unsigned int k = 8; k < 40u; k++)
					CHECK_EQ(rsp[k], 0u);
		}
	}
	omci_t_down(&fx);
}

static void test_the_olt_session_is_served(void)
{
	static const struct {
		uint8_t mt;
		uint16_t cls;
	} session[] = {
		{ 4, 45 },  { 4, 47 },	{ 4, 84 },  { 4, 130 },	 { 4, 171 },
		{ 4, 266 }, { 4, 268 }, { 4, 272 }, { 4, 281 },	 { 4, 309 },
		{ 4, 310 }, { 4, 311 }, { 8, 6 },   { 8, 11 },	 { 8, 84 },
		{ 8, 130 }, { 8, 131 }, { 8, 171 }, { 8, 256 },	 { 8, 262 },
		{ 8, 263 }, { 8, 268 }, { 8, 281 }, { 8, 309 },	 { 8, 310 },
		{ 9, 2 },   { 9, 131 }, { 9, 287 }, { 9, 310 },	 { 11, 2 },
		{ 13, 2 },  { 14, 2 },	{ 15, 2 },  { 26, 287 },
	};
	struct omci_t_fixture fx;

	fixture(&fx);
	CHECK_EQ(ARRAY_SIZE(session), 34u);

	for (size_t i = 0; i < ARRAY_SIZE(session); i++) {
		const struct tsv_class *t = tsv_class(session[i].cls);

		CHECK(t != NULL);
		if (t == NULL)
			continue;
		CHECK((t->actions & (1u << session[i].mt)) != 0u);
		CHECK(implemented(session[i].mt));

		(void)!omci_mib_create(fx.mib, t->id, INSTANCE);
		(void)send(fx.eng, session[i].mt, t->id,
			   t->id == 2u || t->id == 131u || t->id == 256u ||
					   t->id == 287u ?
				   0u :
				   INSTANCE);
		CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_DISPATCH);
	}
	omci_t_down(&fx);
}

static void test_set_table_is_in_the_data_and_not_in_the_baseline_set(void)
{
	unsigned int classes = 0;

	for (unsigned int c = 0; c < tsv_class_count; c++)
		if ((tsv_classes[c].actions & (1u << 29)) != 0u)
			classes++;
	CHECK_EQ(classes, 5u);

	struct omci_t_fixture fx;

	fixture(&fx);
	for (unsigned int c = 0; c < tsv_class_count; c++)
		if ((tsv_classes[c].actions & (1u << 29)) != 0u) {
			(void)!omci_mib_create(fx.mib, tsv_classes[c].id,
					       INSTANCE);
			const uint8_t *rsp =
				send(fx.eng, 29u, tsv_classes[c].id, INSTANCE);

			CHECK(rsp != NULL);
			CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_UNHANDLED);
			if (rsp != NULL)
				CHECK_EQ(rsp[8], 2u);
		}
	omci_t_down(&fx);
}

static void test_each_refusal_before_a_handler_is_said(void)
{
	struct omci_t_fixture fx;
	char log[2048];

	fixture(&fx);
	CHECK(!implemented(5u));
	CHECK(!omci_me_action_allowed(2u, 4u));
	CHECK(omci_me_action_allowed(2u, 9u));

	const enum omci_log_level was = omci_log_level_get();

	omci_log_level_set(OMCI_LOG_DEBUG);

	OMCI_T_CAPTURE_LOG(log, { (void)send(fx.eng, 5u, 2u, 0u); });
	CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_UNHANDLED);
	CHECK_EQ(omci_t_log_count(log, "rx: message type 5 not implemented"),
		 1u);

	OMCI_T_CAPTURE_LOG(log, { (void)send(fx.eng, 9u, 3000u, 0u); });
	CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_UNKNOWN_ME);
	CHECK_EQ(omci_t_log_count(
			 log,
			 "rx: class 3000 is not implemented by this device"),
		 1u);

	OMCI_T_CAPTURE_LOG(log, { (void)send(fx.eng, 4u, 2u, 0u); });
	CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_ACTION);
	CHECK_EQ(
		omci_t_log_count(
			log,
			"rx: class 2 does not accept message type 4 — refused"),
		1u);

	OMCI_T_CAPTURE_LOG(log, { (void)send(fx.eng, 9u, 2u, 0u); });
	CHECK_EQ(g_first_rx, OMCI_CTL_TAP_RX_DISPATCH);
	CHECK(strstr(log, "rx: message type") == NULL);
	CHECK(strstr(log, "is not implemented by this device") == NULL);
	CHECK(strstr(log, "does not accept message type") == NULL);

	omci_log_level_set(was);
	omci_t_down(&fx);
}

static void test_the_predicates_off_the_wire(void)
{
	unsigned int answered = 0;

	for (unsigned int mt = 0; mt < 256u; mt++) {
		const bool want = mt < 32u && implemented(mt);

		CHECK_EQ(omci_me_msg_implemented((uint8_t)mt), want);
		answered += want ? 1u : 0u;
	}
	CHECK_EQ(answered, 13u);

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];

		CHECK_EQ(omci_me_action_mask(t->id), t->actions);
		for (unsigned int mt = 0; mt < 256u; mt++)
			CHECK_EQ(omci_me_action_allowed(t->id, (uint8_t)mt),
				 mt < 32u && (t->actions & (1u << mt)) != 0u);
	}

	CHECK(tsv_class(3000u) == NULL);
	CHECK_EQ(omci_me_action_mask(3000u), 0u);
	for (unsigned int mt = 0; mt < 256u; mt++)
		CHECK(omci_me_action_allowed(3000u, (uint8_t)mt));

	CHECK_EQ(omci_attr_bit(0), 0u);
	for (unsigned int a = 1; a <= 16u; a++) {
		CHECK_EQ(omci_attr_bit((uint8_t)a), 0x8000u >> (a - 1u));
		CHECK(omci_attr_in_mask((uint16_t)(0x8000u >> (a - 1u)),
					(uint8_t)a));
		CHECK(!omci_attr_in_mask((uint16_t)~(0x8000u >> (a - 1u)),
					 (uint8_t)a));
	}
	CHECK_EQ(omci_attr_bit(17), 0u);
	CHECK_EQ(omci_attr_bit(255), 0u);
	CHECK(!omci_attr_in_mask(0xFFFFu, 0));
	CHECK(!omci_attr_in_mask(0xFFFFu, 17));
	CHECK(!omci_attr_in_mask(0xFFFFu, 255));
}

int main(int argc, char **argv)
{
	if (!tsv_open(argc, argv)) {
		printf("test_actions: no table to read\n");
		return 1;
	}

	RUN(test_every_message_type_to_every_class);
	RUN(test_classes_the_daemon_does_not_hold);
	RUN(test_the_olt_session_is_served);
	RUN(test_set_table_is_in_the_data_and_not_in_the_baseline_set);
	RUN(test_each_refusal_before_a_handler_is_said);
	RUN(test_the_predicates_off_the_wire);

	return OMCI_T_REPORT("test_actions");
}
