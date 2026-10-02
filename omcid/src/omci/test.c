// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests. The ONU runs one: the optical line supervision test of the ANI-G
 * (select test 7), whose result reports the supply voltage, the received
 * and transmitted optical power, the laser bias current and the
 * temperature (G.988 A.3.21, A.3.39).
 */

#include <stdbool.h>

#include "hw/hw.h"
#include "omci/frame.h"
#include "omci/test.h"
#include "sys/log.h"
#include "sys/wire.h"

#define OMCI_MT_TEST_RESULT 27u

#define ME_ANI_G 263u

#define SELECT_TEST_OPTICAL 7u

/*
 * The test result contents: each measurement is a type byte and a 16-bit
 * value; a measurement the transceiver did not give is left out.
 */
enum {
	RES_VOLTAGE_AT = 0u,
	RES_VOLTAGE_TYPE = 1u,
	RES_RX_AT = 3u,
	RES_RX_TYPE = 3u,
	RES_TX_AT = 6u,
	RES_TX_TYPE = 5u,
	RES_BIAS_AT = 9u,
	RES_BIAS_TYPE = 9u,
	RES_TEMP_AT = 12u,
	RES_TEMP_TYPE = 12u,
	RES_GP_BUF_AT = 15u,
	RES_LEN = 17u,
};

/* The base-2 logarithm of @x, in fixed point with 16 fraction bits. */
static uint32_t log2_q16(uint32_t x)
{
	unsigned int e = 0;

	for (uint32_t t = x; t > 1u; t >>= 1)
		e++;

	uint64_t m = ((uint64_t)x << 30) >> e;
	uint32_t frac = 0;

	for (unsigned int i = 0; i < 16u; i++) {
		m = (m * m) >> 30;
		frac <<= 1;
		if (m >= ((uint64_t)1u << 31)) {
			m >>= 1;
			frac |= 1u;
		}
	}
	return ((uint32_t)e << 16) | frac;
}

bool omci_test_power_dbu(uint16_t tenths_uw, int16_t *out)
{
	if (out == NULL)
		return false;
	*out = 0;

	if (tenths_uw == 0u)
		return false;

	/*
	 * 5000 * log10(2) with 8 fraction bits: times the logarithm, which has
	 * 16, it gives units of 0.002 dB with 24 fraction bits. Taking away
	 * 5000 of those units, 10 dB, turns 0.1 uW into 1 uW.
	 */
	enum { K_Q8 = 385318u };

	const uint64_t scaled = (uint64_t)log2_q16(tenths_uw) * K_Q8;
	const int32_t dbu500 = (int32_t)(scaled >> 24) - 5000;

	*out = (int16_t)dbu500;
	return true;
}

/* Write one measurement of the result, if there is one. */
static void put_result(uint8_t *body, unsigned int at, uint8_t type,
		       bool measured, uint16_t value)
{
	if (!measured)
		return;
	body[at] = type;
	body[at + 1] = (uint8_t)(value >> 8);
	body[at + 2] = (uint8_t)(value & 0xffu);
}

static bool got(const struct omci_hw_optics *o, enum omci_hw_optic which)
{
	return (o->valid & (uint8_t)(1u << (unsigned int)which)) != 0u;
}

static bool build_optical_body(uint8_t *body)
{
	struct omci_hw_optics o;

	if (omci_hw()->optics(&o) != OMCI_OK) {
		OMCI_WARN(
			"test: the transceiver answered nothing — no result to report");
		return false;
	}

	/* The voltage comes in units of 0.1 mV; the result takes 20 mV. */
	put_result(body, RES_VOLTAGE_AT, RES_VOLTAGE_TYPE,
		   got(&o, OMCI_HW_OPTIC_VOLTAGE),
		   (uint16_t)(o.voltage / 200u));

	int16_t rx_dbu = 0, tx_dbu = 0;
	const bool rx_ok = got(&o, OMCI_HW_OPTIC_RX_POWER) &&
			   omci_test_power_dbu(o.rx_power, &rx_dbu);
	const bool tx_ok = got(&o, OMCI_HW_OPTIC_TX_POWER) &&
			   omci_test_power_dbu(o.tx_power, &tx_dbu);

	put_result(body, RES_RX_AT, RES_RX_TYPE, rx_ok, (uint16_t)rx_dbu);
	put_result(body, RES_TX_AT, RES_TX_TYPE, tx_ok, (uint16_t)tx_dbu);

	put_result(body, RES_BIAS_AT, RES_BIAS_TYPE,
		   got(&o, OMCI_HW_OPTIC_BIAS), o.bias);
	put_result(body, RES_TEMP_AT, RES_TEMP_TYPE,
		   got(&o, OMCI_HW_OPTIC_TEMPERATURE), o.temperature);

	body[RES_GP_BUF_AT] = 0u;
	body[RES_GP_BUF_AT + 1] = 0u;

	return true;
}

bool omci_test_run(uint16_t class_id, uint16_t instance, uint8_t select_test,
		   uint16_t tci, bool priority,
		   uint8_t out[static OMCI_FRAME_LEN])
{
	if (class_id != ME_ANI_G)
		return false;

	struct omci_frame f = {
		.tci = tci,
		.priority = priority,
		.msg_type = OMCI_MT_TEST_RESULT,
		.ar = false,
		.ak = false,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = class_id,
		.instance = instance,
	};

	/* Any other test gets a result with no measurement in it. */
	if (select_test != SELECT_TEST_OPTICAL) {
		OMCI_DEBUG(
			"test: select %u on ME %u/%u is not a test this ONU runs — reporting no results",
			select_test, class_id, instance);
		(void)!omci_frame_build(&f, out);
		return true;
	}

	if (!build_optical_body(f.contents))
		return false;

	(void)!omci_frame_build(&f, out);
	return true;
}
