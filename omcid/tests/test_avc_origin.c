// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that only a change the ONU makes is announced, never one the OLT makes. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omci/notify.h"

#include "harness.h"

static const char *const SET_ARC_REQUEST =
	"0101480a01078001010001000000000000000000000000000000000000000000000000000000000000000028";
static const char *const SET_ARC_RESPONSE =
	"0101280a01078001000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const SET_ARC_AGAIN_REQ =
	"0102480a01078001010001000000000000000000000000000000000000000000000000000000000000000028";
static const char *const SET_ARC_AGAIN_RSP =
	"0102280a01078001000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const AVC_ARC =
	"0000110a01078001010001000000000000000000000000000000000000000000000000000000000000000028";
static const char *const CREATE_REQUEST =
	"0103440a010a01010102050103ffff0104000000000000000000000000000000000000000000000000000028";
static const char *const CREATE_RESPONSE =
	"0103240a010a0101000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const AVC_OPER_STATE =
	"0000110a010a0101040001000000000000000000000000000000000000000000000000000000000000000028";

enum {
	ME_ANI_G = 263,
	ME_GEM_IW_TP = 266,
	ANI_INSTANCE = 0x8001,
	GEM_INSTANCE = 0x0101,
	ATTR_ARC = 8,
	ATTR_OPER = 6,
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

static void check_frame(unsigned int n, const char *hex)
{
	uint8_t want[OMCI_FRAME_LEN];

	vec(hex, want);
	CHECK(n < omci_t_tx.n);
	if (n < omci_t_tx.n) {
		CHECK_MEM(omci_t_tx.frame[n], want, OMCI_FRAME_LEN);
		CHECK(omci_frame_mic_ok(omci_t_tx.frame[n]));
	}
}

static unsigned int avc_sent(void)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if (omci_t_tx.frame[i][2] == 0x11u)
			n++;
	return n;
}

static uint8_t byte_of(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		       uint8_t attr)
{
	uint8_t v = 0xee;

	CHECK_EQ(omci_mib_attr_get(mib, cls, inst, attr, &v, 1u, NULL),
		 OMCI_OK);
	return v;
}

static bool fixture(struct omci_t_fixture *fx)
{
	struct omci_hw_caps caps;

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	if (!omci_t_up(fx))
		return false;
	CHECK_EQ(omci_me_defaults_apply(fx->mib, &caps), OMCI_OK);
	omci_t_tx_reset();
	return true;
}

static void test_a_set_the_olt_sends_is_not_announced(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;
	uint8_t req[OMCI_FRAME_LEN];

	CHECK_EQ(byte_of(fx.mib, ME_ANI_G, ANI_INSTANCE, ATTR_ARC), 0u);

	vec(SET_ARC_REQUEST, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK_EQ(avc_sent(), 0u);
	check_frame(0, SET_ARC_RESPONSE);
	CHECK_EQ(byte_of(fx.mib, ME_ANI_G, ANI_INSTANCE, ATTR_ARC), 1u);

	vec(SET_ARC_AGAIN_REQ, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK_EQ(avc_sent(), 0u);
	check_frame(0, SET_ARC_AGAIN_RSP);
	omci_t_down(&fx);
}

static void test_a_create_the_olt_sends_is_not_announced(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;
	uint8_t req[OMCI_FRAME_LEN];

	CHECK(!omci_mib_exists(fx.mib, ME_GEM_IW_TP, GEM_INSTANCE));
	vec(CREATE_REQUEST, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK_EQ(avc_sent(), 0u);
	check_frame(0, CREATE_RESPONSE);

	CHECK(omci_mib_exists(fx.mib, ME_GEM_IW_TP, GEM_INSTANCE));
	uint8_t ptr[2] = { 0, 0 };

	CHECK_EQ(omci_mib_attr_get(fx.mib, ME_GEM_IW_TP, GEM_INSTANCE, 1, ptr,
				   2u, NULL),
		 OMCI_OK);
	CHECK_EQ(ptr[0], 0x01u);
	CHECK_EQ(ptr[1], 0x02u);
	omci_t_down(&fx);
}

static void test_a_change_the_onu_makes_is_announced(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;
	const uint8_t one = 1;

	omci_t_tx_reset();
	CHECK_EQ(omci_mib_attr_set(fx.mib, ME_ANI_G, ANI_INSTANCE, ATTR_ARC,
				   &one, 1u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, AVC_ARC);

	omci_t_tx_reset();
	CHECK_EQ(omci_mib_attr_set(fx.mib, ME_ANI_G, ANI_INSTANCE, ATTR_ARC,
				   &one, 1u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 0u);

	uint8_t req[OMCI_FRAME_LEN];

	vec(CREATE_REQUEST, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	omci_t_tx_reset();
	CHECK_EQ(omci_mib_attr_set(fx.mib, ME_GEM_IW_TP, GEM_INSTANCE,
				   ATTR_OPER, &one, 1u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, AVC_OPER_STATE);
	omci_t_down(&fx);
}

static void test_the_announcement_names_each_of_the_sixteen_attributes(void)
{
	for (unsigned int attr = 1; attr <= 16u; attr++) {
		uint8_t wire[OMCI_FRAME_LEN];
		const uint8_t value[3] = { 0xa1, 0xb2, 0xc3 };
		const unsigned int bit = 0x8000u >> (attr - 1u);

		memset(wire, 0x5a, sizeof(wire));
		omci_notify_build_avc(263u, 0x8001u, (uint8_t)attr, value,
				      sizeof(value), wire);

		CHECK_EQ(wire[0], 0u);
		CHECK_EQ(wire[1], 0u);
		CHECK_EQ(wire[2], 17u);
		CHECK_EQ(wire[3], OMCI_DEVICE_ID_BASELINE);
		CHECK_EQ((unsigned int)(wire[4] << 8 | wire[5]), 263u);
		CHECK_EQ((unsigned int)(wire[6] << 8 | wire[7]), 0x8001u);
		CHECK_EQ((unsigned int)(wire[8] << 8 | wire[9]), bit);
		CHECK_MEM(&wire[10], value, sizeof(value));
		for (unsigned int i = 10u + (unsigned int)sizeof(value);
		     i < 40u; i++)
			CHECK_EQ(wire[i], 0u);
		CHECK(omci_frame_mic_ok(wire));
	}

	uint8_t wire[OMCI_FRAME_LEN];

	omci_notify_build_avc(263u, 0x8001u, 3u, NULL, 0, wire);
	CHECK_EQ((unsigned int)(wire[8] << 8 | wire[9]), 0x2000u);
	CHECK_EQ(wire[10], 0u);
	omci_notify_build_avc(263u, 0x8001u, 17u, NULL, 0, wire);
	CHECK_EQ((unsigned int)(wire[8] << 8 | wire[9]), 0u);
}

int main(void)
{
	RUN(test_a_set_the_olt_sends_is_not_announced);
	RUN(test_a_create_the_olt_sends_is_not_announced);
	RUN(test_a_change_the_onu_makes_is_announced);
	RUN(test_the_announcement_names_each_of_the_sixteen_attributes);
	return OMCI_T_REPORT("test_avc_origin");
}
