// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the alarms: hardware alarms, bitmaps, Get all alarms and notifications. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/alarm.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

#define ME_ANI_G 263u
#define ANI_G_INSTANCE 0x8001u

#define ME_ONU_DATA 2u

static void hw_all_clear(void)
{
	for (unsigned int a = 0; a < (unsigned int)OMCI_HW_ALARM_COUNT; a++)
		omci_hw_alarm_force((enum omci_hw_alarm)a, false);
}

static bool bit_of(const uint8_t *map, unsigned int n)
{
	return (map[n / 8u] & (uint8_t)(0x80u >> (n % 8u))) != 0u;
}

static void test_a_raised_alarm_reaches_the_bitmap(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);

	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);

	bool changed = false;

	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_OK);
	CHECK(changed);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	struct omci_alarm_entry e;

	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK_EQ(e.class_id, ME_ANI_G);
	CHECK_EQ(e.instance, ANI_G_INSTANCE);
	CHECK(bit_of(e.bitmap, 0));
	CHECK(!bit_of(e.bitmap, 2));
	CHECK(!bit_of(e.bitmap, 3));

	omci_alarm_fini(al);
}

static void test_an_alarm_that_clears_is_a_change_too(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);

	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_SF, true);
	bool changed = false;

	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_OK);
	CHECK(changed);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_OK);
	CHECK(!changed);

	omci_hw_alarm_force(OMCI_HW_ALARM_SF, false);
	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_OK);
	CHECK(changed);
	CHECK_EQ(omci_alarm_snapshot(al, false), 0u);

	omci_alarm_fini(al);
}

static void test_each_mapped_alarm_lands_on_its_own_bit(void)
{
	static const struct {
		enum omci_hw_alarm hw;
		unsigned int bit;
	} mapped[] = {
		{ OMCI_HW_ALARM_LOS, 0u },
		{ OMCI_HW_ALARM_SF, 2u },
		{ OMCI_HW_ALARM_SD, 3u },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(mapped); i++) {
		struct omci_alarm *al = NULL;

		CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
		hw_all_clear();
		omci_hw_alarm_force(mapped[i].hw, true);
		CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
		CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

		struct omci_alarm_entry e;

		CHECK(omci_alarm_snapshot_get(al, 0, &e));
		for (unsigned int b = 0; b < 8u; b++)
			CHECK_EQ(bit_of(e.bitmap, b), b == mapped[i].bit);
		omci_alarm_fini(al);
	}
}

static void test_unmapped_conditions_raise_nothing(void)
{
	static const enum omci_hw_alarm unmapped[] = {
		OMCI_HW_ALARM_LOF,
		OMCI_HW_ALARM_LOM,
		OMCI_HW_ALARM_TX_TOO_LONG,
		OMCI_HW_ALARM_TX_MISMATCH,
	};

	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();
	for (unsigned int i = 0; i < ARRAY_SIZE(unmapped); i++)
		omci_hw_alarm_force(unmapped[i], true);

	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 0u);
	omci_alarm_fini(al);
}

static void test_every_condition_at_once(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	for (unsigned int a = 0; a < (unsigned int)OMCI_HW_ALARM_COUNT; a++)
		omci_hw_alarm_force((enum omci_hw_alarm)a, true);

	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	struct omci_alarm_entry e;

	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK(bit_of(e.bitmap, 0));
	CHECK(bit_of(e.bitmap, 2));
	CHECK(bit_of(e.bitmap, 3));
	CHECK(!bit_of(e.bitmap, 1));
	CHECK(!bit_of(e.bitmap, 4));

	for (unsigned int i = 1; i < OMCI_ALARM_BITMAP_LEN; i++)
		CHECK_EQ(e.bitmap[i], 0u);

	hw_all_clear();
	omci_alarm_fini(al);
}

static void test_the_notification_frame(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_SD, true);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);

	struct omci_alarm_entry changed_list[4];

	CHECK_EQ(omci_alarm_take_changes(al, changed_list, 4), 1u);

	uint8_t wire[OMCI_FRAME_LEN];

	CHECK_EQ(omci_alarm_sequence(al), 1u);
	omci_alarm_build_notification(al, &changed_list[0], wire);

	struct omci_frame f;

	CHECK_EQ(omci_frame_parse(wire, &f), OMCI_OK);
	CHECK_EQ(f.msg_type, 16u);
	CHECK_EQ(f.tci, 0u);
	CHECK(!f.ar);
	CHECK(!f.ak);
	CHECK_EQ(f.class_id, ME_ANI_G);
	CHECK_EQ(f.instance, ANI_G_INSTANCE);
	CHECK(bit_of(f.contents, 3));

	CHECK_EQ(f.contents[28], 0u);
	CHECK_EQ(f.contents[29], 0u);
	CHECK_EQ(f.contents[30], 0u);
	CHECK_EQ(f.contents[31], 1u);
	CHECK_EQ(omci_alarm_sequence(al), 2u);

	for (unsigned int i = 0; i < 300u; i++) {
		omci_alarm_build_notification(al, &changed_list[0], wire);
		CHECK(omci_alarm_sequence(al) != 0u);
	}

	omci_alarm_reset_sequence(al);
	CHECK_EQ(omci_alarm_sequence(al), 1u);

	hw_all_clear();
	omci_alarm_fini(al);
}

static void test_the_snapshot_is_frozen(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, false);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);

	struct omci_alarm_entry e;

	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK(bit_of(e.bitmap, 0));

	CHECK_EQ(omci_alarm_snapshot(al, false), 0u);

	omci_alarm_fini(al);
}

static void test_reading_past_the_end(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_SF, true);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	struct omci_alarm_entry e;

	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK(!omci_alarm_snapshot_get(al, 1, &e));
	CHECK(!omci_alarm_snapshot_get(al, 0xFFFFu, &e));

	hw_all_clear();
	omci_alarm_fini(al);
}

static void test_through_the_engine(void)
{
	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_hw_alarm_force(OMCI_HW_ALARM_SD, true);

	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x0501u, 11u, true, false, ME_ONU_DATA, 0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);

	CHECK_EQ(((unsigned int)rsp[8] << 8) | rsp[9], 1u);

	omci_t_build(req, 0x0502u, 12u, true, false, ME_ONU_DATA, 0u);
	req[8] = 0;
	req[9] = 0;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	rsp = omci_t_last_tx();
	CHECK(rsp != NULL);
	CHECK_EQ(((unsigned int)rsp[8] << 8) | rsp[9], ME_ANI_G);
	CHECK_EQ(((unsigned int)rsp[10] << 8) | rsp[11], ANI_G_INSTANCE);
	CHECK(bit_of(&rsp[12], 0));
	CHECK(bit_of(&rsp[12], 3));

	CHECK_EQ(rsp[39], 0u);

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_the_seam_refuses_bad_arguments(void)
{
	CHECK_EQ(omci_hw_alarms(NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_hw_pon(NULL), OMCI_ERR_INVAL);

	uint64_t v = 12345u;

	CHECK_EQ(omci_hw_counter(0, 0, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_hw_counter(999u, 0, &v), OMCI_ERR_INVAL);
	CHECK_EQ(v, 0u);
	CHECK_EQ(omci_hw_counter(0, 9999u, &v), OMCI_ERR_INVAL);

	omci_hw_counter_force(2u, 5u, 0x1122334455667788ull);
	CHECK_EQ(omci_hw_counter(2u, 5u, &v), OMCI_OK);
	CHECK(v == 0x1122334455667788ull);
	CHECK_EQ(omci_hw_counter(2u, 6u, &v), OMCI_OK);
	CHECK_EQ(v, 0u);
	omci_hw_counter_force(2u, 5u, 0u);

	omci_hw_alarm_force((enum omci_hw_alarm)99, true);
	uint32_t mask = 0xFFu;

	CHECK_EQ(omci_hw_alarms(&mask), OMCI_OK);
	CHECK_EQ(mask & ~0x7Fu, 0u);
	omci_hw_counter_force(999u, 0u, 1u);

	CHECK_EQ(omci_alarm_poll(NULL, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_alarm_snapshot(NULL, false), 0u);
	CHECK_EQ(omci_alarm_sequence(NULL), 0u);
	struct omci_alarm_entry e;

	CHECK(!omci_alarm_snapshot_get(NULL, 0, &e));
	CHECK_EQ(omci_alarm_take_changes(NULL, &e, 1), 0u);
	omci_alarm_reset_sequence(NULL);
	CHECK_EQ(omci_alarm_init(NULL), OMCI_ERR_INVAL);
	omci_alarm_fini(NULL);
}

static void test_the_engine_answers_past_the_end(void)
{
	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_SF, true);

	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x0601u, 11u, true, false, ME_ONU_DATA, 0u);
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);

	static const unsigned int past[] = { 1u, 0x0100u, 0x0101u, 0xFF00u,
					     0xFFFFu };
	for (size_t k = 0; k < ARRAY_SIZE(past); k++) {
		omci_t_build(req, (uint16_t)(0x0602u + k), 12u, true, false,
			     ME_ONU_DATA, 0u);
		req[8] = (uint8_t)(past[k] >> 8);
		req[9] = (uint8_t)past[k];
		omci_t_tx_reset();
		CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
		CHECK_EQ(omci_t_tx.n, 1u);

		const uint8_t *rsp = omci_t_last_tx();

		CHECK(rsp != NULL);
		for (unsigned int i = 8; rsp != NULL && i < 40u; i++)
			CHECK_EQ(rsp[i], 0u);
	}

	omci_t_build(req, 0x0610u, 12u, true, false, ME_ONU_DATA, 0u);
	req[8] = 0;
	req[9] = 0;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	const uint8_t *first = omci_t_last_tx();

	CHECK(first != NULL);
	if (first != NULL) {
		CHECK_EQ((unsigned int)(first[8] << 8 | first[9]), 263u);
		CHECK_EQ((unsigned int)(first[10] << 8 | first[11]), 0x8001u);
		CHECK(first[12] != 0u);
	}

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_only_one_alarm_plane(void)
{
	struct omci_alarm *a = NULL, *b = NULL;

	CHECK_EQ(omci_alarm_init(&a), OMCI_OK);
	CHECK_EQ(omci_alarm_init(&b), OMCI_ERR_BUSY);
	omci_alarm_fini(a);
	CHECK_EQ(omci_alarm_init(&b), OMCI_OK);
	omci_alarm_fini(b);
}

static void test_a_failed_read_keeps_the_last_known_state(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);

	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	omci_hw_alarm_fail_next(true);
	bool changed = true;

	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_ERR_HW);
	CHECK(!changed);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	struct omci_alarm_entry e;

	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK(bit_of(e.bitmap, 0));

	omci_hw_alarm_fail_next(false);
	hw_all_clear();
	omci_alarm_fini(al);
}

static void test_the_engine_answers_when_the_driver_is_silent(void)
{
	static const char says[] =
		"get-all-alarms: hardware read failed; answering from the last known state";
	char log[512];

	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_SD, true);

	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	const enum omci_log_level was = omci_log_level_get();

	omci_log_level_set(OMCI_LOG_WARN);

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x0701u, 11u, true, false, ME_ONU_DATA, 0u);
	OMCI_T_CAPTURE_LOG(log, { CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK); });
	CHECK(strstr(log, says) == NULL);

	omci_hw_alarm_fail_next(true);
	omci_t_build(req, 0x0702u, 11u, true, false, ME_ONU_DATA, 0u);
	omci_t_tx_reset();
	OMCI_T_CAPTURE_LOG(log, { CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK); });
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK_EQ(omci_t_log_count(log, says), 1u);

	const uint8_t *rsp = omci_t_last_tx();

	CHECK(rsp != NULL);
	CHECK_EQ(((unsigned int)rsp[8] << 8) | rsp[9], 1u);

	omci_log_level_set(was);
	omci_hw_alarm_fail_next(false);
	omci_t_down(&fx);
	hw_all_clear();
}

/* The ANI-G attributes the alarms are judged by. */
#define ANI_G_SF 6u
#define ANI_G_SD 7u
#define ANI_G_ARC 8u
#define ANI_G_ARC_INTERVAL 9u

#define MT_SET 8u
#define MT_GET_ALL_ALARMS 11u
#define MT_GET_ALL_ALARMS_N 12u
#define MT_ALARM 16u
#define MT_AVC 17u

#define RESULT_OK 0u
#define RESULT_PARAM_ERROR 3u

static bool fixture_with_defaults(struct omci_t_fixture *fx)
{
	struct omci_hw_caps caps;

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	if (!omci_t_up(fx))
		return false;
	CHECK_EQ(omci_me_defaults_apply(fx->mib, &caps), OMCI_OK);
	omci_t_tx_reset();
	return true;
}

/* The frames sent of message type @mt, and the last of them. */
static unsigned int sent_of(uint8_t mt, const uint8_t **last)
{
	unsigned int n = 0;

	if (last != NULL)
		*last = NULL;
	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++) {
		if ((omci_t_tx.frame[i][2] & 0x1Fu) != mt)
			continue;
		n++;
		if (last != NULL)
			*last = omci_t_tx.frame[i];
	}
	return n;
}

/* Set a one-byte attribute as the OLT does: the result of the answer. */
static unsigned int olt_set_u8(struct omci_engine *eng, uint16_t tci,
			       uint16_t cls, uint16_t inst, uint8_t attr,
			       uint8_t v)
{
	uint8_t req[OMCI_FRAME_LEN];
	const uint16_t mask = (uint16_t)(0x8000u >> (attr - 1u));

	omci_t_build(req, tci, MT_SET, true, false, cls, inst);
	req[8] = (uint8_t)(mask >> 8);
	req[9] = (uint8_t)mask;
	req[10] = v;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, req), OMCI_OK);

	const uint8_t *rsp = omci_t_last_tx();

	return rsp != NULL ? rsp[8] : 0xFFu;
}

static uint8_t mib_u8(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		      uint8_t attr)
{
	uint8_t v = 0xEEu;

	CHECK_EQ(omci_mib_attr_get(mib, cls, inst, attr, &v, 1u, NULL),
		 OMCI_OK);
	return v;
}

/* Get all alarms, with its retrieval mode: the count it answers. */
static unsigned int get_all_alarms(struct omci_engine *eng, uint16_t tci,
				   uint8_t mode)
{
	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, tci, MT_GET_ALL_ALARMS, true, false, ME_ONU_DATA, 0u);
	req[8] = mode;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, req), OMCI_OK);

	const uint8_t *rsp = omci_t_last_tx();

	return rsp != NULL ? ((unsigned int)rsp[8] << 8 | rsp[9]) : 0xFFFFu;
}

/* Get all alarms next: the class of the entity it answers, 0 for none. */
static unsigned int get_all_alarms_next(struct omci_engine *eng, uint16_t tci,
					uint16_t seq)
{
	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, tci, MT_GET_ALL_ALARMS_N, true, false, ME_ONU_DATA,
		     0u);
	req[8] = (uint8_t)(seq >> 8);
	req[9] = (uint8_t)seq;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, req), OMCI_OK);

	const uint8_t *rsp = omci_t_last_tx();

	return rsp != NULL ? ((unsigned int)rsp[8] << 8 | rsp[9]) : 0xFFFFu;
}

/* The thresholds of the ANI-G as the ONU starts with them. */
static const struct omci_alarm_thresholds DEFAULTS = {
	.sf = 5u,
	.sd = 9u,
	.optical = { 0xFFu, 0xFFu, 0x81u, 0x81u },
};

static void test_the_rate_is_judged_from_the_receiver(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();
	CHECK_EQ(omci_alarm_judge(NULL, 0u, &DEFAULTS), OMCI_ERR_INVAL);
	CHECK_EQ(omci_alarm_judge(al, 0u, NULL), OMCI_ERR_INVAL);

	/* Eleven quiet seconds, then 25 errors: 10^-9 over 10 s. */
	omci_hw_pon_force(true, true, 0u);
	for (unsigned int s = 0; s <= 10u; s++)
		CHECK_EQ(omci_alarm_judge(al, s, &DEFAULTS), OMCI_OK);
	omci_hw_pon_force(true, true, 25u);
	CHECK_EQ(omci_alarm_judge(al, 11u, &DEFAULTS), OMCI_OK);

	struct omci_alarm_entry e;

	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);
	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK(!bit_of(e.bitmap, 2));
	CHECK(bit_of(e.bitmap, 3));

	/* A read that fails leaves no rate to judge: both clear. */
	omci_hw_pon_fail_next(true);
	CHECK_EQ(omci_alarm_judge(al, 12u, &DEFAULTS), OMCI_ERR_HW);
	omci_hw_pon_fail_next(false);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 0u);

	/* So does a receiver that has no count, or is not in operation. */
	omci_hw_pon_force(true, true, 0u);
	for (unsigned int s = 13u; s <= 24u; s++)
		CHECK_EQ(omci_alarm_judge(al, s, &DEFAULTS), OMCI_OK);
	omci_hw_pon_force(true, true, 30000u);
	CHECK_EQ(omci_alarm_judge(al, 25u, &DEFAULTS), OMCI_OK);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK(omci_alarm_standing(al, ME_ANI_G, ANI_G_INSTANCE));
	omci_hw_pon_force(true, false, 30000u);
	CHECK_EQ(omci_alarm_judge(al, 26u, &DEFAULTS), OMCI_OK);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK(!omci_alarm_standing(al, ME_ANI_G, ANI_G_INSTANCE));
	omci_hw_pon_force(false, true, 30000u);
	CHECK_EQ(omci_alarm_judge(al, 27u, &DEFAULTS), OMCI_OK);

	/* A backend that cannot tell the state of the receiver. */
	struct omci_hw_ops blind = omci_hw_host_ops;

	blind.pon = NULL;
	omci_hw_ops_set(&blind);
	CHECK_EQ(omci_alarm_judge(al, 28u, &DEFAULTS), OMCI_ERR_HW);
	omci_hw_ops_set(NULL);

	omci_hw_pon_force(false, false, 0u);
	omci_alarm_fini(al);
}

static void test_suppressed_alarms_are_kept_but_not_told(void)
{
	struct omci_alarm *al = NULL;
	struct omci_alarm_entry e[4];

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();

	CHECK(!omci_alarm_standing(al, ME_ANI_G, ANI_G_INSTANCE));
	CHECK(!omci_alarm_standing(NULL, ME_ANI_G, ANI_G_INSTANCE));
	omci_alarm_suppress(NULL, ME_ANI_G, ANI_G_INSTANCE, true);
	omci_alarm_suppress(al, ME_ANI_G, ANI_G_INSTANCE, false);
	omci_alarm_suppress(al, ME_ANI_G, ANI_G_INSTANCE, true);

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK(omci_alarm_standing(al, ME_ANI_G, ANI_G_INSTANCE));
	CHECK(!omci_alarm_standing(al, 11u, 0x0101u));
	CHECK_EQ(omci_alarm_take_changes(al, e, 4), 0u);

	/* Get all alarms has it, unless asked for the others only. */
	CHECK_EQ(omci_alarm_snapshot(al, true), 0u);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);

	/* What the snapshot gave, the OLT knows: nothing more to tell. */
	omci_alarm_suppress(al, ME_ANI_G, ANI_G_INSTANCE, false);
	CHECK_EQ(omci_alarm_take_changes(al, e, 4), 0u);
	CHECK_EQ(omci_alarm_snapshot(al, true), 1u);

	/* Left out of a snapshot, it is clear to the OLT, and told once free. */
	omci_alarm_suppress(al, ME_ANI_G, ANI_G_INSTANCE, true);
	CHECK_EQ(omci_alarm_snapshot(al, true), 0u);
	omci_alarm_suppress(al, ME_ANI_G, ANI_G_INSTANCE, false);
	CHECK_EQ(omci_alarm_take_changes(al, e, 0), 0u);
	CHECK_EQ(omci_alarm_take_changes(al, e, 4), 1u);
	CHECK(bit_of(e[0].bitmap, 0));
	CHECK_EQ(omci_alarm_take_changes(al, e, 4), 0u);
	CHECK_EQ(omci_alarm_take_changes(NULL, e, 4), 0u);
	CHECK_EQ(omci_alarm_take_changes(al, NULL, 4), 0u);

	omci_alarm_snapshot_drop(al);
	CHECK(!omci_alarm_snapshot_get(al, 0, &e[0]));
	omci_alarm_snapshot_drop(NULL);

	hw_all_clear();
	omci_alarm_fini(al);
}

static void test_no_room_for_another_entity(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();

	for (unsigned int i = 0; i < OMCI_ALARM_MAX_ENTITIES; i++)
		omci_alarm_suppress(al, (uint16_t)(1000u + i), 1u, true);
	omci_alarm_suppress(al, 2000u, 1u, true);

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK(!omci_alarm_standing(al, ME_ANI_G, ANI_G_INSTANCE));
	CHECK_EQ(omci_alarm_snapshot(al, false), 0u);

	hw_all_clear();
	omci_alarm_fini(al);
}

static void test_the_tick_notifies_each_change(void)
{
	hw_all_clear();
	omci_hw_pon_force(false, false, 0u);

	struct omci_t_fixture fx;
	const uint8_t *f = NULL;

	if (!fixture_with_defaults(&fx))
		return;

	omci_engine_tick(fx.eng, 100u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);

	/* Read once a second: not again within the same one. */
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 100u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);

	omci_engine_tick(fx.eng, 101u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	CHECK(f != NULL);
	if (f != NULL) {
		CHECK_EQ(((unsigned int)f[4] << 8) | f[5], ME_ANI_G);
		CHECK_EQ(((unsigned int)f[6] << 8) | f[7], ANI_G_INSTANCE);
		CHECK(bit_of(&f[8], 0));
		CHECK_EQ(f[8 + 31], 1u);
	}

	omci_engine_tick(fx.eng, 102u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 1u);

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, false);
	omci_engine_tick(fx.eng, 103u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 2u);
	if (f != NULL) {
		CHECK(!bit_of(&f[8], 0));
		CHECK_EQ(f[8 + 31], 2u);
	}

	/* A hardware that does not answer leaves the alarms as they were. */
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_hw_alarm_fail_next(true);
	omci_engine_tick(fx.eng, 104u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 2u);
	omci_hw_alarm_fail_next(false);

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_an_entity_the_mib_lacks_is_not_notified(void)
{
	hw_all_clear();

	struct omci_t_fixture fx;

	if (!omci_t_up(&fx))
		return;
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 5u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);
	omci_t_down(&fx);
	hw_all_clear();
}

static void test_the_tick_judges_the_bit_error_rate(void)
{
	hw_all_clear();
	omci_hw_pon_force(true, true, 0u);

	struct omci_t_fixture fx;
	const uint8_t *f = NULL;

	if (!fixture_with_defaults(&fx))
		return;

	for (unsigned int s = 0; s <= 10u; s++)
		omci_engine_tick(fx.eng, 1000u + s);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);

	/* 30000 errors in a second: past 10^-5 and 10^-9. */
	omci_hw_pon_force(true, true, 30000u);
	omci_engine_tick(fx.eng, 1011u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL) {
		CHECK(!bit_of(&f[8], 0));
		CHECK(bit_of(&f[8], 2));
		CHECK(bit_of(&f[8], 3));
	}

	/*
	 * The OLT raises the signal fail threshold to 10^-3: 30000 errors a
	 * second are below a tenth of it, and signal fail clears.
	 */
	CHECK_EQ(olt_set_u8(fx.eng, 0x0901u, ME_ANI_G, ANI_G_INSTANCE, ANI_G_SF,
			    3u),
		 RESULT_OK);
	omci_t_tx_reset();
	omci_hw_pon_force(true, true, 60000u);
	omci_engine_tick(fx.eng, 1012u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL) {
		CHECK(!bit_of(&f[8], 2));
		CHECK(bit_of(&f[8], 3));
	}

	/* Out of operation there is no rate: signal degrade clears too. */
	omci_t_tx_reset();
	omci_hw_pon_force(false, false, 0u);
	omci_engine_tick(fx.eng, 1013u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL)
		CHECK(!bit_of(&f[8], 3));

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_alarm_reporting_control_ends_by_itself(void)
{
	hw_all_clear();
	omci_hw_pon_force(false, false, 0u);

	struct omci_t_fixture fx;
	const uint8_t *f = NULL;

	if (!fixture_with_defaults(&fx))
		return;

	CHECK_EQ(olt_set_u8(fx.eng, 0x0A01u, ME_ANI_G, ANI_G_INSTANCE,
			    ANI_G_ARC_INTERVAL, 1u),
		 RESULT_OK);
	CHECK_EQ(olt_set_u8(fx.eng, 0x0A02u, ME_ANI_G, ANI_G_INSTANCE,
			    ANI_G_ARC, 1u),
		 RESULT_OK);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 500u);

	/* Under control: kept, not notified. */
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 501u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);

	/* Get all alarms reports it, unless asked for the others only. */
	CHECK_EQ(get_all_alarms(fx.eng, 0x0A03u, 0u), 1u);
	CHECK_EQ(get_all_alarms(fx.eng, 0x0A04u, 1u), 0u);

	/* A minute after the alarm last stood, the control ends. */
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, false);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 502u);
	omci_engine_tick(fx.eng, 560u);
	CHECK_EQ(sent_of(MT_AVC, NULL), 0u);
	CHECK_EQ(mib_u8(fx.mib, ME_ANI_G, ANI_G_INSTANCE, ANI_G_ARC), 1u);

	omci_engine_tick(fx.eng, 561u);
	CHECK_EQ(sent_of(MT_AVC, &f), 1u);
	if (f != NULL) {
		CHECK_EQ(f[8], 0x01u);
		CHECK_EQ(f[9], 0x00u);
		CHECK_EQ(f[10], 0u);
	}
	CHECK_EQ(mib_u8(fx.mib, ME_ANI_G, ANI_G_INSTANCE, ANI_G_ARC), 0u);

	/* The OLT holds it clear, as it is: nothing to notify. */
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);

	/* Reporting has resumed. */
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 562u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 1u);

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_alarm_reporting_control_the_olt_ends(void)
{
	hw_all_clear();
	omci_hw_pon_force(false, false, 0u);

	struct omci_t_fixture fx;
	const uint8_t *f = NULL;

	if (!fixture_with_defaults(&fx))
		return;

	/* An interval of 255 never ends. */
	CHECK_EQ(olt_set_u8(fx.eng, 0x0B01u, ME_ANI_G, ANI_G_INSTANCE,
			    ANI_G_ARC_INTERVAL, 255u),
		 RESULT_OK);
	CHECK_EQ(olt_set_u8(fx.eng, 0x0B02u, ME_ANI_G, ANI_G_INSTANCE,
			    ANI_G_ARC, 1u),
		 RESULT_OK);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 700u);
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 701u);
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, false);
	omci_engine_tick(fx.eng, 702u);
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 702u + 255u * 60u + 100u);
	CHECK_EQ(sent_of(MT_AVC, NULL), 0u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);

	/* The OLT ends it: the alarm it never heard of is notified. */
	CHECK_EQ(olt_set_u8(fx.eng, 0x0B03u, ME_ANI_G, ANI_G_INSTANCE,
			    ANI_G_ARC, 0u),
		 RESULT_OK);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 702u + 255u * 60u + 101u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL)
		CHECK(bit_of(&f[8], 0));

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_get_all_alarms_restarts_the_sequence(void)
{
	hw_all_clear();
	omci_hw_pon_force(false, false, 0u);

	struct omci_t_fixture fx;
	const uint8_t *f = NULL;

	if (!fixture_with_defaults(&fx))
		return;

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 10u);
	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, false);
	omci_engine_tick(fx.eng, 11u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 2u);
	if (f != NULL)
		CHECK_EQ(f[8 + 31], 2u);

	CHECK_EQ(get_all_alarms(fx.eng, 0x0C01u, 0u), 0u);

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 12u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL)
		CHECK_EQ(f[8 + 31], 1u);

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_an_idle_alarm_upload_ends(void)
{
	hw_all_clear();
	omci_hw_pon_force(false, false, 0u);

	struct omci_t_fixture fx;

	if (!fixture_with_defaults(&fx))
		return;

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, true);
	omci_engine_tick(fx.eng, 100u);
	CHECK_EQ(get_all_alarms(fx.eng, 0x0D01u, 0u), 1u);

	/* A minute between commands is allowed ... */
	omci_engine_tick(fx.eng, 160u);
	CHECK_EQ(get_all_alarms_next(fx.eng, 0x0D02u, 0u), ME_ANI_G);
	omci_engine_tick(fx.eng, 220u);
	CHECK_EQ(get_all_alarms_next(fx.eng, 0x0D03u, 0u), ME_ANI_G);

	/* ... more ends the upload. */
	omci_engine_tick(fx.eng, 281u);
	CHECK_EQ(get_all_alarms_next(fx.eng, 0x0D04u, 0u), 0u);

	omci_t_down(&fx);
	hw_all_clear();
}

static void test_a_set_out_of_range_is_refused(void)
{
	struct omci_t_fixture fx;

	if (!fixture_with_defaults(&fx))
		return;

	static const struct {
		uint8_t attr;
		uint8_t value;
		uint8_t result;
	} sets[] = {
		{ ANI_G_SF, 2u, RESULT_PARAM_ERROR },
		{ ANI_G_SF, 9u, RESULT_PARAM_ERROR },
		{ ANI_G_SF, 3u, RESULT_OK },
		{ ANI_G_SF, 8u, RESULT_OK },
		{ ANI_G_SD, 3u, RESULT_PARAM_ERROR },
		{ ANI_G_SD, 11u, RESULT_PARAM_ERROR },
		{ ANI_G_SD, 4u, RESULT_OK },
		{ ANI_G_SD, 10u, RESULT_OK },
		{ ANI_G_ARC, 2u, RESULT_PARAM_ERROR },
		{ ANI_G_ARC, 1u, RESULT_OK },
		{ ANI_G_ARC, 0u, RESULT_OK },
		{ ANI_G_ARC_INTERVAL, 200u, RESULT_OK },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(sets); i++) {
		const uint8_t before =
			mib_u8(fx.mib, ME_ANI_G, ANI_G_INSTANCE, sets[i].attr);

		CHECK_EQ(olt_set_u8(fx.eng, (uint16_t)(0x0E01u + i), ME_ANI_G,
				    ANI_G_INSTANCE, sets[i].attr,
				    sets[i].value),
			 sets[i].result);
		CHECK_EQ(mib_u8(fx.mib, ME_ANI_G, ANI_G_INSTANCE, sets[i].attr),
			 sets[i].result == RESULT_OK ? sets[i].value : before);
	}

	omci_t_down(&fx);
}

/* An optical level attribute of the ANI-G. */
static int16_t ani_g_level(struct omci_mib *mib, uint8_t attr)
{
	uint8_t v[2] = { 0xEEu, 0xEEu };

	CHECK_EQ(omci_mib_attr_get(mib, ME_ANI_G, ANI_G_INSTANCE, attr, v,
				   sizeof(v), NULL),
		 OMCI_OK);
	return (int16_t)(uint16_t)((unsigned int)v[0] << 8 | v[1]);
}

#define ANI_G_RX_LEVEL 10u
#define ANI_G_RX_LOW 11u
#define ANI_G_TX_LEVEL 14u

static void test_the_ani_g_reports_its_optical_levels(void)
{
	hw_all_clear();
	omci_hw_optics_reset();
	omci_hw_optics_limits_reset();
	omci_hw_pon_force(true, false, 0u);

	struct omci_t_fixture fx;

	if (!fixture_with_defaults(&fx))
		return;

	omci_engine_tick(fx.eng, 300u);
	CHECK_EQ(ani_g_level(fx.mib, ANI_G_RX_LEVEL), omci_optical_level(34u));
	CHECK_EQ(ani_g_level(fx.mib, ANI_G_TX_LEVEL),
		 omci_optical_level(16412u));

	/* No light gives the lowest level. */
	omci_hw_optics_force(OMCI_HW_OPTIC_RX_POWER, true, 0u);
	omci_engine_tick(fx.eng, 301u);
	CHECK_EQ(ani_g_level(fx.mib, ANI_G_RX_LEVEL), OMCI_OPTICAL_LEVEL_NONE);

	/* A reading that is missing leaves the level as it was. */
	omci_hw_optics_force(OMCI_HW_OPTIC_RX_POWER, false, 50u);
	omci_engine_tick(fx.eng, 302u);
	CHECK_EQ(ani_g_level(fx.mib, ANI_G_RX_LEVEL), OMCI_OPTICAL_LEVEL_NONE);

	omci_t_down(&fx);
	omci_hw_optics_reset();
	omci_hw_pon_force(false, false, 0u);
	hw_all_clear();
}

static void test_optical_alarms_are_notified(void)
{
	hw_all_clear();
	omci_hw_optics_reset();
	omci_hw_optics_limits_reset();
	omci_hw_pon_force(true, false, 0u);
	omci_hw_pon_level_force(true, 2u);

	struct omci_t_fixture fx;
	const uint8_t *f = NULL;

	if (!fixture_with_defaults(&fx))
		return;

	omci_engine_tick(fx.eng, 400u);
	CHECK_EQ(sent_of(MT_ALARM, NULL), 0u);

	/* 500 uW, under the transceiver's 891 uW at the highest level. */
	omci_hw_optics_force(OMCI_HW_OPTIC_TX_POWER, true, 5000u);
	omci_engine_tick(fx.eng, 401u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL)
		CHECK(bit_of(&f[8], 4));

	/* A level lower, the limit is 3 dB lower: 520 uW is inside it. */
	omci_hw_pon_level_force(true, 1u);
	omci_hw_optics_force(OMCI_HW_OPTIC_TX_POWER, true, 5200u);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 402u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL)
		CHECK(!bit_of(&f[8], 4));

	/* The OLT's lower optical threshold of -20 dBm: 3.4 uW is under it. */
	CHECK_EQ(olt_set_u8(fx.eng, 0x0F01u, ME_ANI_G, ANI_G_INSTANCE,
			    ANI_G_RX_LOW, 40u),
		 RESULT_OK);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 403u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL)
		CHECK(bit_of(&f[8], 0));

	/* 120 mA of bias, over the transceiver's 100 mA. */
	omci_hw_optics_force(OMCI_HW_OPTIC_BIAS, true, 60000u);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 404u);
	CHECK_EQ(sent_of(MT_ALARM, &f), 1u);
	if (f != NULL) {
		CHECK(bit_of(&f[8], 0));
		CHECK(bit_of(&f[8], 6));
	}

	omci_t_down(&fx);
	omci_hw_optics_reset();
	omci_hw_pon_level_force(false, 0u);
	omci_hw_pon_force(false, false, 0u);
	hw_all_clear();
}

static void test_the_transceivers_thresholds_are_read_once_had(void)
{
	struct omci_alarm *al = NULL;
	struct omci_alarm_entry e;
	int16_t rx = 7, tx = 7;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();
	omci_hw_optics_reset();
	CHECK_EQ(omci_alarm_levels(al, &rx, &tx), 0u);
	CHECK_EQ(rx, 7);
	CHECK_EQ(omci_alarm_levels(NULL, &rx, &tx), 0u);
	CHECK_EQ(omci_alarm_levels(al, NULL, &tx), 0u);
	CHECK_EQ(omci_alarm_levels(al, &rx, NULL), 0u);

	/* 0.5 uW in: under 1 uW, once the transceiver gives its thresholds. */
	omci_hw_optics_force(OMCI_HW_OPTIC_RX_POWER, true, 5u);
	for (unsigned int i = 0; i < (unsigned int)OMCI_HW_LIMIT_COUNT; i++)
		omci_hw_optics_limit_force((enum omci_hw_limit)i, false, 0u);
	CHECK_EQ(omci_alarm_judge(al, 1u, &DEFAULTS), OMCI_OK);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 0u);

	/* A backend without them. */
	struct omci_hw_ops plain = omci_hw_host_ops;

	plain.optics_limits = NULL;
	omci_hw_ops_set(&plain);
	CHECK_EQ(omci_alarm_judge(al, 2u, &DEFAULTS), OMCI_OK);
	omci_hw_ops_set(NULL);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 0u);

	omci_hw_optics_limits_reset();
	CHECK_EQ(omci_alarm_judge(al, 3u, &DEFAULTS), OMCI_OK);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al, false), 1u);
	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK(bit_of(e.bitmap, 0));
	CHECK_EQ(omci_alarm_levels(al, &rx, &tx),
		 OMCI_ALARM_LEVEL_RX | OMCI_ALARM_LEVEL_TX);
	CHECK_EQ(rx, omci_optical_level(5u));

	/* Kept once had: thresholds that go missing later change nothing. */
	omci_hw_optics_limit_force(OMCI_HW_LIMIT_RX_LOW, false, 0u);
	CHECK_EQ(omci_alarm_judge(al, 4u, &DEFAULTS), OMCI_OK);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK(omci_alarm_standing(al, ME_ANI_G, ANI_G_INSTANCE));

	/* A transceiver that answers nothing leaves the alarms as they were. */
	for (unsigned int i = 0; i < (unsigned int)OMCI_HW_OPTIC_COUNT; i++)
		omci_hw_optics_force((enum omci_hw_optic)i, false, 0u);
	CHECK_EQ(omci_alarm_judge(al, 5u, &DEFAULTS), OMCI_OK);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK(omci_alarm_standing(al, ME_ANI_G, ANI_G_INSTANCE));

	omci_hw_optics_reset();
	omci_hw_optics_limits_reset();
	hw_all_clear();
	omci_alarm_fini(al);
}

int main(void)
{
	RUN(test_a_raised_alarm_reaches_the_bitmap);
	RUN(test_an_alarm_that_clears_is_a_change_too);
	RUN(test_each_mapped_alarm_lands_on_its_own_bit);
	RUN(test_unmapped_conditions_raise_nothing);
	RUN(test_every_condition_at_once);
	RUN(test_the_notification_frame);
	RUN(test_the_snapshot_is_frozen);
	RUN(test_reading_past_the_end);
	RUN(test_through_the_engine);
	RUN(test_the_seam_refuses_bad_arguments);
	RUN(test_the_engine_answers_past_the_end);
	RUN(test_only_one_alarm_plane);
	RUN(test_a_failed_read_keeps_the_last_known_state);
	RUN(test_the_engine_answers_when_the_driver_is_silent);
	RUN(test_the_rate_is_judged_from_the_receiver);
	RUN(test_suppressed_alarms_are_kept_but_not_told);
	RUN(test_no_room_for_another_entity);
	RUN(test_the_tick_notifies_each_change);
	RUN(test_an_entity_the_mib_lacks_is_not_notified);
	RUN(test_the_tick_judges_the_bit_error_rate);
	RUN(test_alarm_reporting_control_ends_by_itself);
	RUN(test_alarm_reporting_control_the_olt_ends);
	RUN(test_get_all_alarms_restarts_the_sequence);
	RUN(test_an_idle_alarm_upload_ends);
	RUN(test_a_set_out_of_range_is_refused);
	RUN(test_the_ani_g_reports_its_optical_levels);
	RUN(test_optical_alarms_are_notified);
	RUN(test_the_transceivers_thresholds_are_read_once_had);
	return OMCI_T_REPORT("test_alarm");
}
