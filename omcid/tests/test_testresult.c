// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the test result of the optical line supervision test. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/action.h"

#include "harness.h"
#include "hw/hw.h"
#include "omci/test.h"
#include "omcid/array_size.h"

enum {
	C_VOLT_TYPE = 8u + 0u,
	C_VOLT_VAL = 8u + 1u,
	C_RX_TYPE = 8u + 3u,
	C_RX_VAL = 8u + 4u,
	C_TX_TYPE = 8u + 6u,
	C_TX_VAL = 8u + 7u,
	C_BIAS_TYPE = 8u + 9u,
	C_BIAS_VAL = 8u + 10u,
	C_TEMP_TYPE = 8u + 12u,
	C_TEMP_VAL = 8u + 13u,
	C_GP_BUF = 8u + 15u,
};

#define ME_ANI_G 263u
#define ANI_INSTANCE 0x8001u
#define MT_TEST 18u
#define MT_TEST_RESULT 27u
#define SELECT_OPTICAL 7u

static bool fixture(struct omci_t_fixture *fx)
{
	if (!omci_t_up(fx))
		return false;
	CHECK_EQ(omci_mib_create(fx->mib, ME_ANI_G, ANI_INSTANCE), OMCI_OK);
	CHECK_EQ(omci_mib_create(fx->mib, 134u, 0x0101u), OMCI_OK);
	CHECK_EQ(omci_mib_create(fx->mib, 256u, 0x0101u), OMCI_OK);
	return true;
}

static uint16_t be16_at(const uint8_t *f, unsigned int at)
{
	return (uint16_t)(((uint16_t)f[at] << 8) | f[at + 1u]);
}

static enum omci_status send_test(struct omci_engine *eng, uint16_t tci,
				  uint16_t class_id, uint16_t instance,
				  uint8_t select_test, bool priority)
{
	uint8_t wire[OMCI_FRAME_LEN];

	if (priority)
		omci_t_build_pri(wire, tci, MT_TEST, true, false, class_id,
				 instance);
	else
		omci_t_build(wire, tci, MT_TEST, true, false, class_id,
			     instance);

	wire[8] = select_test;
	return omci_t_rx(eng, wire);
}

static void test_a_test_is_answered_and_then_reported(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();
	omci_t_tx_reset();

	CHECK_EQ(send_test(fx.eng, 0x1234u, ME_ANI_G, ANI_INSTANCE,
			   SELECT_OPTICAL, false),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 2u);

	const uint8_t *rsp = omci_t_tx.frame[0];
	const uint8_t *res = omci_t_tx.frame[1];

	CHECK_EQ(rsp[2] & 0x1Fu, MT_TEST);
	CHECK((rsp[2] & 0x20u) != 0u);
	CHECK_EQ(rsp[8], 0u);

	CHECK_EQ(res[2] & 0x1Fu, MT_TEST_RESULT);
	CHECK_EQ(res[2] & 0x20u, 0u);
	CHECK_EQ(res[2] & 0x40u, 0u);
	CHECK_EQ(res[3], 0x0Au);

	omci_t_down(&fx);
}

static void test_the_result_carries_the_tests_transaction(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();

	for (uint16_t tci = 1u; tci <= 0x0100u; tci = (uint16_t)(tci * 16u)) {
		omci_t_tx_reset();
		CHECK_EQ(send_test(fx.eng, tci, ME_ANI_G, ANI_INSTANCE,
				   SELECT_OPTICAL, false),
			 OMCI_OK);
		CHECK_EQ(omci_t_tx.n, 2u);

		const uint16_t got = be16_at(omci_t_tx.frame[1], 0);

		CHECK_EQ(got & 0x7FFFu, tci);
		CHECK_EQ(got & 0x8000u, 0u);

		CHECK_EQ(be16_at(omci_t_tx.frame[1], 4), ME_ANI_G);
		CHECK_EQ(be16_at(omci_t_tx.frame[1], 6), ANI_INSTANCE);
	}

	omci_t_down(&fx);
}

static void test_the_five_measurements_are_converted_and_placed(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();
	omci_t_tx_reset();

	CHECK_EQ(send_test(fx.eng, 0x0055u, ME_ANI_G, ANI_INSTANCE,
			   SELECT_OPTICAL, false),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 2u);
	const uint8_t *r = omci_t_tx.frame[1];

	CHECK_EQ(r[C_VOLT_TYPE], 1u);
	CHECK_EQ(be16_at(r, C_VOLT_VAL), 161u);
	CHECK_EQ(r[C_RX_TYPE], 3u);
	CHECK_EQ(be16_at(r, C_RX_VAL), 2657u);
	CHECK_EQ(r[C_TX_TYPE], 5u);
	CHECK_EQ(be16_at(r, C_TX_VAL), 16075u);
	CHECK_EQ(r[C_BIAS_TYPE], 9u);
	CHECK_EQ(be16_at(r, C_BIAS_VAL), 6950u);
	CHECK_EQ(r[C_TEMP_TYPE], 12u);
	CHECK_EQ(be16_at(r, C_TEMP_VAL), 12405u);

	CHECK_EQ(be16_at(r, C_GP_BUF), 0u);

	for (unsigned int i = C_GP_BUF + 2u; i < OMCI_FRAME_LEN - 8u; i++)
		CHECK_EQ(r[i], 0u);

	omci_t_down(&fx);
}

static void test_an_instrument_that_did_not_answer_is_not_reported_as_zero(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();
	omci_hw_optics_force(OMCI_HW_OPTIC_BIAS, false, 0u);
	omci_t_tx_reset();

	CHECK_EQ(send_test(fx.eng, 0x0056u, ME_ANI_G, ANI_INSTANCE,
			   SELECT_OPTICAL, false),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 2u);
	const uint8_t *r = omci_t_tx.frame[1];

	CHECK_EQ(r[C_BIAS_TYPE], 0u);
	CHECK_EQ(be16_at(r, C_BIAS_VAL), 0u);

	CHECK_EQ(r[C_VOLT_TYPE], 1u);
	CHECK_EQ(be16_at(r, C_VOLT_VAL), 161u);
	CHECK_EQ(r[C_TEMP_TYPE], 12u);
	CHECK_EQ(be16_at(r, C_TEMP_VAL), 12405u);

	omci_hw_optics_reset();

	omci_t_down(&fx);
}

static void
test_a_dark_receiver_reports_no_measurement_rather_than_one_microwatt(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();
	omci_hw_optics_force(OMCI_HW_OPTIC_RX_POWER, true, 0u);
	omci_t_tx_reset();

	CHECK_EQ(send_test(fx.eng, 0x0057u, ME_ANI_G, ANI_INSTANCE,
			   SELECT_OPTICAL, false),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 2u);
	const uint8_t *r = omci_t_tx.frame[1];

	CHECK_EQ(r[C_RX_TYPE], 0u);
	CHECK_EQ(be16_at(r, C_RX_VAL), 0u);
	CHECK_EQ(r[C_TX_TYPE], 5u);

	omci_hw_optics_reset();

	omci_t_down(&fx);
}

static void
test_an_unrunnable_test_reports_no_results_rather_than_echoing_the_request(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();

	for (uint8_t sel = 0u; sel < 12u; sel++) {
		if (sel == SELECT_OPTICAL)
			continue;

		omci_t_tx_reset();
		CHECK_EQ(send_test(fx.eng, (uint16_t)(0x0200u + sel), ME_ANI_G,
				   ANI_INSTANCE, sel, false),
			 OMCI_OK);
		CHECK_EQ(omci_t_tx.n, 2u);

		const uint8_t *r = omci_t_tx.frame[1];

		CHECK_EQ(r[2] & 0x1Fu, MT_TEST_RESULT);

		for (unsigned int i = 8u; i < 8u + 17u; i++)
			CHECK_EQ(r[i], 0u);
	}

	omci_t_down(&fx);
}

static void test_a_class_with_no_instrument_gets_no_result(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();

	const uint16_t classes[] = { 134u, 256u };

	for (size_t i = 0; i < ARRAY_SIZE(classes); i++) {
		omci_t_tx_reset();
		CHECK_EQ(send_test(fx.eng, (uint16_t)(0x0300u + i), classes[i],
				   0x0101u, SELECT_OPTICAL, false),
			 OMCI_OK);
		CHECK_EQ(omci_t_tx.n, 1u);
		CHECK_EQ(omci_t_tx.frame[0][2] & 0x1Fu, MT_TEST);
		CHECK_EQ(omci_t_tx.frame[0][8], 0u);
	}

	omci_t_down(&fx);
}

static void test_a_class_that_accepts_no_test_is_refused(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();

	const uint16_t classes[] = { 11u, 257u, 5u, 171u };

	for (size_t i = 0; i < ARRAY_SIZE(classes); i++) {
		CHECK(!omci_me_action_allowed(classes[i], MT_TEST));

		omci_t_tx_reset();
		CHECK_EQ(send_test(fx.eng, (uint16_t)(0x0380u + i), classes[i],
				   0x0101u, SELECT_OPTICAL, false),
			 OMCI_OK);
		CHECK_EQ(omci_t_tx.n, 1u);
		CHECK_EQ(omci_t_tx.frame[0][2] & 0x1Fu, MT_TEST);
		CHECK_EQ(omci_t_tx.frame[0][8], 2u);
	}

	omci_t_down(&fx);
}

static void test_the_result_stays_on_the_channel_that_asked(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();
	omci_t_tx_reset();

	CHECK_EQ(send_test(fx.eng, 0x0044u, ME_ANI_G, ANI_INSTANCE,
			   SELECT_OPTICAL, true),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 2u);

	CHECK_EQ(be16_at(omci_t_tx.frame[0], 0) & 0x8000u, 0x8000u);
	CHECK_EQ(be16_at(omci_t_tx.frame[1], 0) & 0x8000u, 0x8000u);
	CHECK_EQ(be16_at(omci_t_tx.frame[1], 0) & 0x7FFFu, 0x0044u);

	omci_t_down(&fx);
}

static void test_a_silent_transceiver_produces_no_result(void)
{
	struct omci_t_fixture fx;

	CHECK(fixture(&fx));

	omci_hw_optics_reset();
	for (unsigned int i = 0; i < (unsigned int)OMCI_HW_OPTIC_COUNT; i++)
		omci_hw_optics_force((enum omci_hw_optic)i, false, 0u);

	omci_t_tx_reset();
	CHECK_EQ(send_test(fx.eng, 0x0077u, ME_ANI_G, ANI_INSTANCE,
			   SELECT_OPTICAL, false),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	omci_hw_optics_reset();

	omci_t_down(&fx);
}

static void test_the_optical_power_conversion(void)
{
	static const struct {
		uint16_t raw;
		int16_t want;
		const char *why;
	} k[] = {
		{ 1u, -5000,
		  "0.1 uW = -10.000 dBu, the smallest reading that has a logarithm" },
		{ 3u, -2615, "0.3 uW = -5.229 dBu" },
		{ 10u, -1,
		  "1.0 uW = 0.000 dBu, one count low: the truncation, and its whole size" },
		{ 34u, 2657,
		  "3.4 uW = 5.314 dBu — this stick's own receive level on the live line" },
		{ 100u, 4999, "10 uW = 10.000 dBu" },
		{ 16412u, 16075,
		  "1641.2 uW = 32.150 dBu — its transmit level in the same instant" },
		{ 65535u, 19082,
		  "6553.5 uW = 38.164 dBu, the largest reading the field can carry" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(k); i++) {
		int16_t got = 0;

		CHECK(omci_test_power_dbu(k[i].raw, &got));
		if (got != k[i].want)
			printf("    raw %u: got %d, want %d (%s)\n", k[i].raw,
			       got, k[i].want, k[i].why);
		CHECK_EQ(got, k[i].want);
	}

	int16_t out = 0x5A5A;

	CHECK(!omci_test_power_dbu(0u, &out));
	CHECK_EQ(out, 0);

	CHECK(!omci_test_power_dbu(34u, NULL));
}

static void test_the_conversion_never_goes_backwards(void)
{
	int16_t prev = INT16_MIN;
	unsigned int steps_back = 0;

	for (uint32_t raw = 1u; raw <= 0xFFFFu; raw++) {
		int16_t got = 0;

		CHECK(omci_test_power_dbu((uint16_t)raw, &got));
		if (got < prev) {
			if (steps_back < 4u)
				printf("    raw %lu: %d after %d\n",
				       (unsigned long)raw, got, prev);
			steps_back++;
		}
		prev = got;
	}
	CHECK_EQ(steps_back, 0u);
}

static void test_the_optics_stub_refuses_what_it_cannot_serve(void)
{
	omci_hw_optics_reset();

	CHECK(omci_hw_optics(NULL) != OMCI_OK);

	omci_hw_optics_force((enum omci_hw_optic)OMCI_HW_OPTIC_COUNT, false,
			     0xFFFFu);
	omci_hw_optics_force((enum omci_hw_optic)99, true, 0xFFFFu);

	struct omci_hw_optics o;

	CHECK_EQ(omci_hw_optics(&o), OMCI_OK);
	CHECK_EQ(o.rx_power, 34u);
	CHECK_EQ(o.valid,
		 (uint8_t)((1u << (unsigned int)OMCI_HW_OPTIC_COUNT) - 1u));
}

int main(void)
{
	RUN(test_the_optical_power_conversion);
	RUN(test_the_conversion_never_goes_backwards);

	RUN(test_a_test_is_answered_and_then_reported);
	RUN(test_the_result_carries_the_tests_transaction);
	RUN(test_the_five_measurements_are_converted_and_placed);
	RUN(test_an_instrument_that_did_not_answer_is_not_reported_as_zero);
	RUN(test_a_dark_receiver_reports_no_measurement_rather_than_one_microwatt);
	RUN(test_an_unrunnable_test_reports_no_results_rather_than_echoing_the_request);
	RUN(test_a_class_with_no_instrument_gets_no_result);
	RUN(test_a_class_that_accepts_no_test_is_refused);
	RUN(test_the_result_stays_on_the_channel_that_asked);
	RUN(test_a_silent_transceiver_produces_no_result);
	RUN(test_the_optics_stub_refuses_what_it_cannot_serve);

	return OMCI_T_REPORT("testresult");
}
