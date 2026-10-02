// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of Test on instances that exist and that do not. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

static const char *const IPHOST_GHOST_REQUEST =
	"0301520a00867fff000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const IPHOST_GHOST_RESPONSE =
	"0301320a00867fff050000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const ANI_GHOST_REQUEST =
	"0302520a01077777070000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const ANI_GHOST_RESPONSE =
	"0302320a01077777050000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const ONUG_REQUEST =
	"0303520a01000000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const ONUG_RESPONSE =
	"0303320a01000000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const ANI_REQUEST =
	"0304520a01078001070000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const ANI_RESPONSE =
	"0304320a01078001000000000000000000000000000000000000000000000000000000000000000000000028";

enum {
	ME_IP_HOST = 134,
	ME_ONU_G = 256,
	ME_ANI_G = 263,
	ANI_INSTANCE = 0x8001,
	MT_TEST_RESULT = 27
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

static void send(struct omci_t_fixture *fx, const char *hex)
{
	uint8_t req[OMCI_FRAME_LEN];

	vec(hex, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx->eng, req), OMCI_OK);
}

static void test_a_test_of_an_absent_instance_is_answered_with_result_5(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;

	CHECK(!omci_mib_exists(fx.mib, ME_IP_HOST, 0x7fff));
	send(&fx, IPHOST_GHOST_REQUEST);
	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, IPHOST_GHOST_RESPONSE);
	omci_t_down(&fx);
}

static void test_no_result_follows_a_test_of_an_absent_instance(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;

	CHECK(!omci_mib_exists(fx.mib, ME_ANI_G, 0x7777));
	send(&fx, ANI_GHOST_REQUEST);
	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, ANI_GHOST_RESPONSE);
	omci_t_down(&fx);
}

static void test_a_test_of_a_present_instance_is_answered_with_result_0(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;

	CHECK(omci_mib_exists(fx.mib, ME_ONU_G, 0u));
	send(&fx, ONUG_REQUEST);
	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, ONUG_RESPONSE);

	CHECK(omci_mib_exists(fx.mib, ME_ANI_G, ANI_INSTANCE));
	send(&fx, ANI_REQUEST);
	CHECK_EQ(omci_t_tx.n, 2u);
	check_frame(0, ANI_RESPONSE);
	if (omci_t_tx.n == 2u) {
		const uint8_t *r = omci_t_tx.frame[1];

		CHECK_EQ(r[2], (uint8_t)MT_TEST_RESULT);
		CHECK_EQ((((unsigned int)r[0] << 8) | r[1]), 0x0304u);
		CHECK_EQ((((unsigned int)r[4] << 8) | r[5]),
			 (unsigned int)ME_ANI_G);
		CHECK_EQ((((unsigned int)r[6] << 8) | r[7]),
			 (unsigned int)ANI_INSTANCE);
		CHECK(omci_frame_mic_ok(r));
	}
	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_a_test_of_an_absent_instance_is_answered_with_result_5);
	RUN(test_no_result_follows_a_test_of_an_absent_instance);
	RUN(test_a_test_of_a_present_instance_is_answered_with_result_0);
	return OMCI_T_REPORT("test_test_instance");
}
