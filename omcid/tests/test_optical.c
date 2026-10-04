// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the optical alarms and levels of the ANI-G. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "omci/optical.h"
#include "omcid/array_size.h"

#include "harness.h"

/* 0.5 dB and 3 dB, in the units of the levels. */
#define HALF_DB 250
#define THREE_DB 1500

/* The thresholds that leave every limit to the ONU. */
static const struct omci_optical_thresholds POLICY = {
	OMCI_OPTICAL_RX_POLICY,
	OMCI_OPTICAL_RX_POLICY,
	OMCI_OPTICAL_TX_POLICY,
	OMCI_OPTICAL_TX_POLICY,
};

/* A transceiver's own: 100 mA, 891 to 3981 uW out, 1.0 to 199.5 uW in. */
static const struct omci_hw_optics_limits OWN = {
	.value = { 50000u, 8912u, 39810u, 10u, 1995u },
	.valid = 0x1Fu,
};

static bool near(int32_t got, int32_t want)
{
	return got >= want - 2 && got <= want + 2;
}

static void test_levels_are_in_dbm(void)
{
	CHECK_EQ(omci_optical_level(0u), OMCI_OPTICAL_LEVEL_NONE);
	CHECK(near(omci_optical_level(10000u), 0));
	CHECK(near(omci_optical_level(10u), -15000));
	CHECK(near(omci_optical_level(20000u), 1505));
	CHECK(near(omci_optical_level(1u), -20000));
}

static void test_the_onu_policy_is_the_transceivers(void)
{
	struct omci_optical_limits lim;

	omci_optical_limits(&POLICY, &OWN, NULL, &lim);
	CHECK_EQ(lim.set, 0x1Fu);
	CHECK_EQ(lim.bias_high, 50000u);
	CHECK(near(lim.level[OMCI_HW_LIMIT_TX_LOW], -250));
	CHECK(near(lim.level[OMCI_HW_LIMIT_TX_HIGH], 3000));
	CHECK(near(lim.level[OMCI_HW_LIMIT_RX_LOW], -15000));
	CHECK(near(lim.level[OMCI_HW_LIMIT_RX_HIGH], -3500));

	/* A threshold the transceiver did not give does not apply. */
	struct omci_hw_optics_limits some = OWN;

	some.valid = (uint8_t)(1u << OMCI_HW_LIMIT_RX_LOW);
	omci_optical_limits(&POLICY, &some, NULL, &lim);
	CHECK_EQ(lim.set, (uint8_t)(1u << OMCI_HW_LIMIT_RX_LOW));

	omci_optical_limits(&POLICY, NULL, NULL, &lim);
	CHECK_EQ(lim.set, 0u);
}

static void test_the_transmitter_limits_follow_the_power_level(void)
{
	struct omci_optical_limits high, lim;
	struct omci_hw_pon pon = { .power_level_valid = true };

	omci_optical_limits(&POLICY, &OWN, NULL, &high);

	static const struct {
		uint8_t level;
		int32_t shift;
	} levels[] = {
		{ 2u, 0 },
		{ 1u, THREE_DB },
		{ 0u, 2 * THREE_DB },
		{ 3u, 0 },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(levels); i++) {
		pon.power_level = levels[i].level;
		omci_optical_limits(&POLICY, &OWN, &pon, &lim);
		CHECK_EQ(lim.level[OMCI_HW_LIMIT_TX_LOW],
			 high.level[OMCI_HW_LIMIT_TX_LOW] - levels[i].shift);
		CHECK_EQ(lim.level[OMCI_HW_LIMIT_TX_HIGH],
			 high.level[OMCI_HW_LIMIT_TX_HIGH] - levels[i].shift);
		CHECK_EQ(lim.level[OMCI_HW_LIMIT_RX_LOW],
			 high.level[OMCI_HW_LIMIT_RX_LOW]);
	}

	/* A level that was not read moves nothing. */
	pon.power_level = 0u;
	pon.power_level_valid = false;
	omci_optical_limits(&POLICY, &OWN, &pon, &lim);
	CHECK_EQ(lim.level[OMCI_HW_LIMIT_TX_LOW],
		 high.level[OMCI_HW_LIMIT_TX_LOW]);
}

static void test_the_olt_thresholds_win(void)
{
	const struct omci_optical_thresholds t = { 40u, 16u, 0xFEu, 10u };
	struct omci_optical_limits lim;

	omci_optical_limits(&t, &OWN, NULL, &lim);
	CHECK_EQ(lim.level[OMCI_HW_LIMIT_RX_LOW], -40 * HALF_DB);
	CHECK_EQ(lim.level[OMCI_HW_LIMIT_RX_HIGH], -16 * HALF_DB);
	CHECK_EQ(lim.level[OMCI_HW_LIMIT_TX_LOW], -2 * HALF_DB);
	CHECK_EQ(lim.level[OMCI_HW_LIMIT_TX_HIGH], 10 * HALF_DB);
	CHECK_EQ(lim.set, 0x1Fu);

	/* Without the transceiver's, the OLT's still apply. */
	omci_optical_limits(&t, NULL, NULL, &lim);
	CHECK_EQ(lim.set, 0x1Eu);
}

/* Readings: all five present, the powers in 0.1 uW, the bias in 2 uA. */
static struct omci_hw_optics readings(uint16_t rx, uint16_t tx, uint16_t bias)
{
	struct omci_hw_optics o = {
		.bias = bias,
		.tx_power = tx,
		.rx_power = rx,
		.valid = 0x1Fu,
	};

	return o;
}

static void test_received_power_out_of_range(void)
{
	struct omci_optical_limits lim;
	struct omci_optical v = { 0 };
	struct omci_hw_optics o;

	omci_optical_limits(&POLICY, &OWN, NULL, &lim);

	/* 0.9 uW: under 1.0 uW. */
	o = readings(9u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.rx_low);
	CHECK(!v.rx_high);

	/* 1.1 uW is back, but not by 0.5 dB; 1.2 uW is. */
	o = readings(11u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.rx_low);
	o = readings(12u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(!v.rx_low);

	/* No light at all. */
	o = readings(0u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, false);
	CHECK(v.rx_low);

	/* 210 uW: over 199.5 uW; 185 uW is not 0.5 dB under it, 175 uW is. */
	o = readings(2100u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.rx_high);
	CHECK(!v.rx_low);
	o = readings(1850u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.rx_high);
	o = readings(1750u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(!v.rx_high);

	/* A missing reading leaves the alarms as they were. */
	o = readings(2100u, 16412u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	o.valid = 0u;
	o.rx_power = 30u;
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.rx_high);
}

static void test_the_transmitter_is_judged_in_operation(void)
{
	struct omci_optical_limits lim;
	struct omci_optical v = { 0 };
	struct omci_hw_optics o;

	omci_optical_limits(&POLICY, &OWN, NULL, &lim);

	/* 500 uW is under 891 uW, 120 mA over 100 mA. */
	o = readings(34u, 5000u, 60000u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.tx_low);
	CHECK(!v.tx_high);
	CHECK(v.bias_high);

	/* 96 mA is over 100 mA less a sixteenth; 93 mA is not. */
	o = readings(34u, 5000u, 48000u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.bias_high);
	o = readings(34u, 5000u, 46500u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(!v.bias_high);

	/* Out of operation the transmitter's alarms clear. */
	o = readings(34u, 5000u, 60000u);
	omci_optical_judge(&v, &o, &lim, true);
	omci_optical_judge(&v, &o, &lim, false);
	CHECK(!v.tx_low);
	CHECK(!v.bias_high);

	/* 4500 uW is over 3981 uW. */
	o = readings(34u, 45000u, 6950u);
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.tx_high);
	CHECK(!v.tx_low);

	/* Readings that are missing leave the alarms as they were. */
	o.valid = (uint8_t)(1u << OMCI_HW_OPTIC_RX_POWER);
	o.tx_power = 16412u;
	omci_optical_judge(&v, &o, &lim, true);
	CHECK(v.tx_high);

	/* Without limits nothing is declared. */
	struct omci_optical_limits none;

	omci_optical_limits(&POLICY, NULL, NULL, &none);
	v = (struct omci_optical){ 0 };
	o = readings(0u, 0u, 65535u);
	omci_optical_judge(&v, &o, &none, true);
	CHECK(!v.rx_low && !v.tx_low && !v.bias_high);
}

int main(void)
{
	RUN(test_levels_are_in_dbm);
	RUN(test_the_onu_policy_is_the_transceivers);
	RUN(test_the_transmitter_limits_follow_the_power_level);
	RUN(test_the_olt_thresholds_win);
	RUN(test_received_power_out_of_range);
	RUN(test_the_transmitter_is_judged_in_operation);
	return OMCI_T_REPORT("test_optical");
}
