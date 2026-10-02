// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the alarms: hardware alarms, bitmaps, Get all alarms and notifications. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
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
	CHECK_EQ(omci_alarm_snapshot(al), 1u);

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
	CHECK_EQ(omci_alarm_snapshot(al), 1u);

	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_OK);
	CHECK(!changed);

	omci_hw_alarm_force(OMCI_HW_ALARM_SF, false);
	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_OK);
	CHECK(changed);
	CHECK_EQ(omci_alarm_snapshot(al), 0u);

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
		CHECK_EQ(omci_alarm_snapshot(al), 1u);

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
	CHECK_EQ(omci_alarm_snapshot(al), 0u);
	omci_alarm_fini(al);
}

static void test_every_condition_at_once(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	for (unsigned int a = 0; a < (unsigned int)OMCI_HW_ALARM_COUNT; a++)
		omci_hw_alarm_force((enum omci_hw_alarm)a, true);

	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al), 1u);

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

	CHECK_EQ(omci_alarm_changed(al, changed_list, 4), 1u);

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
	CHECK_EQ(omci_alarm_snapshot(al), 1u);

	omci_hw_alarm_force(OMCI_HW_ALARM_LOS, false);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);

	struct omci_alarm_entry e;

	CHECK(omci_alarm_snapshot_get(al, 0, &e));
	CHECK(bit_of(e.bitmap, 0));

	CHECK_EQ(omci_alarm_snapshot(al), 0u);

	omci_alarm_fini(al);
}

static void test_reading_past_the_end(void)
{
	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	hw_all_clear();
	omci_hw_alarm_force(OMCI_HW_ALARM_SF, true);
	CHECK_EQ(omci_alarm_poll(al, NULL), OMCI_OK);
	CHECK_EQ(omci_alarm_snapshot(al), 1u);

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
	CHECK_EQ(omci_alarm_snapshot(NULL), 0u);
	CHECK_EQ(omci_alarm_sequence(NULL), 0u);
	struct omci_alarm_entry e;

	CHECK(!omci_alarm_snapshot_get(NULL, 0, &e));
	CHECK_EQ(omci_alarm_changed(NULL, &e, 1), 0u);
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
	CHECK_EQ(omci_alarm_snapshot(al), 1u);

	omci_hw_alarm_fail_next(true);
	bool changed = true;

	CHECK_EQ(omci_alarm_poll(al, &changed), OMCI_ERR_HW);
	CHECK(!changed);
	CHECK_EQ(omci_alarm_snapshot(al), 1u);

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
	return OMCI_T_REPORT("test_alarm");
}
