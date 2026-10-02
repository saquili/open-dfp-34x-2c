// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the MIB data sync attribute of the ONU data entity. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

static const char *const RESET_REQUEST =
	"06044f0a00020000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const RESET_RESPONSE =
	"06042f0a00020000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const GET_REQUEST =
	"0601490a00020000800000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const GET_ZERO_RESPONSE =
	"0601290a00020000008000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const SET_REQUEST =
	"0602480a00020000800007000000000000000000000000000000000000000000000000000000000000000028";
static const char *const SET_RESPONSE =
	"0602280a00020000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const GET_AGAIN_REQUEST =
	"0603490a00020000800000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const GET_SEVEN_RESPONSE =
	"0603290a00020000008000070000000000000000000000000000000000000000000000000000000000000028";
static const char *const RESET2_REQUEST =
	"06054f0a00020000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const RESET2_RESPONSE =
	"06052f0a00020000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const GET_AFTER_REQUEST =
	"0606490a00020000800000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const GET_AFTER_RESPONSE =
	"0606290a00020000008000000000000000000000000000000000000000000000000000000000000000000028";

static const char *const UPLOAD_ZERO_UNIT =
	"00012e0a00020000000200008000000000000000000000000000000000000000000000000000000000000028";
static const char *const UPLOAD_SEVEN_UNIT =
	"00012e0a00020000000200008000070000000000000000000000000000000000000000000000000000000028";

enum {
	ME_ONU_DATA = 2,
	ATTR_MDS = 1,
	MT_MIB_UPLOAD = 13,
	MT_MIB_UPLOAD_NEXT = 14
};

static uint8_t nib(char c)
{
	return (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10);
}

static void vec(const char *hex, uint8_t out[static OMCI_FRAME_LEN])
{
	for (size_t i = 0; i < 44u; i++)
		out[i] = (uint8_t)((nib(hex[2u * i]) << 4) |
				   nib(hex[2u * i + 1u]));
	const uint32_t mic = omci_frame_mic(out, 44u);

	out[44] = (uint8_t)(mic >> 24);
	out[45] = (uint8_t)(mic >> 16);
	out[46] = (uint8_t)(mic >> 8);
	out[47] = (uint8_t)mic;
}

static void exchange(struct omci_engine *eng, const char *request,
		     const char *response, const char *what)
{
	uint8_t req[OMCI_FRAME_LEN], want[OMCI_FRAME_LEN];

	vec(request, req);
	vec(response, want);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	if (omci_t_tx.n == 1u) {
		if (memcmp(omci_t_tx.frame[0], want, OMCI_FRAME_LEN) != 0)
			printf("    %s: the answer is not the expected one\n", what);
		CHECK_MEM(omci_t_tx.frame[0], want, OMCI_FRAME_LEN);
		CHECK(omci_frame_mic_ok(omci_t_tx.frame[0]));
	}
}

static unsigned int upload_unit_of_onu_data(struct omci_engine *eng,
					    uint8_t unit[static OMCI_FRAME_LEN])
{
	uint8_t wire[OMCI_FRAME_LEN];
	unsigned int found = 0;
	static uint16_t tid = 0x0900u;

	omci_t_build(wire, tid++, MT_MIB_UPLOAD, true, false, ME_ONU_DATA, 0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	const unsigned int units =
		omci_t_tx.n == 1u ?
			(((unsigned int)omci_t_tx.frame[0][8] << 8) |
			 omci_t_tx.frame[0][9]) :
			0u;
	CHECK(units > 30u);

	for (unsigned int seq = 0; seq < units; seq++) {
		omci_t_build(wire, tid++, MT_MIB_UPLOAD_NEXT, true, false,
			     ME_ONU_DATA, 0u);
		wire[8] = (uint8_t)(seq >> 8);
		wire[9] = (uint8_t)seq;
		omci_t_tx_reset();
		CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
		if (omci_t_tx.n != 1u)
			continue;
		const uint8_t *f = omci_t_tx.frame[0];

		if (f[8] == 0u && f[9] == ME_ONU_DATA) {
			if (found++ == 0u)
				memcpy(unit, f, OMCI_FRAME_LEN);
		}
	}
	return found;
}

static void check_unit(const uint8_t unit[static OMCI_FRAME_LEN],
		       const char *hex, const char *what)
{
	uint8_t want[OMCI_FRAME_LEN];

	vec(hex, want);
	if (memcmp(&unit[2], &want[2], 42u) != 0)
		printf("    %s: the upload unit is not the expected one\n", what);
	CHECK_MEM(&unit[2], &want[2], 42u);
	CHECK(omci_frame_mic_ok(unit));
}

static void
test_the_attribute_is_zero_after_a_reset_and_the_olt_can_write_it(void)
{
	struct omci_t_fixture fx;

	if (!omci_t_up(&fx))
		return;

	exchange(fx.eng, RESET_REQUEST, RESET_RESPONSE, "reset");
	exchange(fx.eng, GET_REQUEST, GET_ZERO_RESPONSE,
		 "get after the first reset");

	exchange(fx.eng, SET_REQUEST, SET_RESPONSE, "set to 7");
	exchange(fx.eng, GET_AGAIN_REQUEST, GET_SEVEN_RESPONSE, "get of 7");
	uint8_t v = 0xee;

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ONU_DATA, 0u, ATTR_MDS, &v, 1u,
				   NULL),
		 OMCI_OK);
	CHECK_EQ(v, 7u);

	exchange(fx.eng, RESET2_REQUEST, RESET2_RESPONSE, "second reset");
	exchange(fx.eng, GET_AFTER_REQUEST, GET_AFTER_RESPONSE,
		 "get after the second reset");
	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_ONU_DATA, 0u, ATTR_MDS, &v, 1u,
				   NULL),
		 OMCI_OK);
	CHECK_EQ(v, 0u);
	omci_t_down(&fx);
}

static void test_the_upload_carries_the_attribute(void)
{
	struct omci_t_fixture fx;

	if (!omci_t_up(&fx))
		return;
	uint8_t unit[OMCI_FRAME_LEN];

	exchange(fx.eng, RESET_REQUEST, RESET_RESPONSE, "reset");
	memset(unit, 0, sizeof(unit));
	CHECK_EQ(upload_unit_of_onu_data(fx.eng, unit), 1u);
	check_unit(unit, UPLOAD_ZERO_UNIT, "after a reset");

	exchange(fx.eng, SET_REQUEST, SET_RESPONSE, "set to 7");
	memset(unit, 0, sizeof(unit));
	CHECK_EQ(upload_unit_of_onu_data(fx.eng, unit), 1u);
	check_unit(unit, UPLOAD_SEVEN_UNIT, "after the OLT wrote 7");
	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_the_attribute_is_zero_after_a_reset_and_the_olt_can_write_it);
	RUN(test_the_upload_carries_the_attribute);
	return OMCI_T_REPORT("test_mib_data_sync");
}
