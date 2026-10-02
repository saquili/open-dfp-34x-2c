// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the answer to MIB reset and of what follows it. */

#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

static const char *const RESET_REQUEST =
	"82014f0a00020000000000000000000000000000000000000000000000000000000000000000000000000028";
static const char *const RESET_RESPONSE =
	"82012f0a00020000000000000000000000000000000000000000000000000000000000000000000000000028";

static const char *const AVC_ARC =
	"0000110a01078001010001000000000000000000000000000000000000000000000000000000000000000028";

enum {
	ME_ANI_G = 263,
	ME_GEM_IW_TP = 266,
	ANI_INSTANCE = 0x8001,
	MARKER = 0x0555,
	ATTR_ARC = 8,
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

static size_t population_size(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;
	size_t n = 0;

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	n = omci_mib_count(mib);
	omci_mib_fini(mib);
	return n;
}

static void test_a_mib_reset_is_answered_with_its_response_alone(void)
{
	const size_t population = population_size();

	CHECK(population > 30u);

	struct omci_t_fixture fx;

	if (!omci_t_up(&fx))
		return;
	uint8_t req[OMCI_FRAME_LEN];

	CHECK_EQ(omci_mib_create(fx.mib, ME_GEM_IW_TP, MARKER), OMCI_OK);

	vec(RESET_REQUEST, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);

	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, RESET_RESPONSE);

	CHECK_EQ(omci_mib_count(fx.mib), population);
	CHECK(omci_mib_exists(fx.mib, 2u, 0u));
	CHECK(omci_mib_exists(fx.mib, ME_ANI_G, ANI_INSTANCE));
	CHECK(!omci_mib_exists(fx.mib, ME_GEM_IW_TP, MARKER));
	omci_t_down(&fx);
}

static void test_the_reset_response_is_replayed_alone(void)
{
	struct omci_t_fixture fx;

	if (!omci_t_up(&fx))
		return;
	uint8_t req[OMCI_FRAME_LEN];

	vec(RESET_REQUEST, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, RESET_RESPONSE);
	omci_t_down(&fx);
}

static void test_a_change_the_onu_makes_after_a_reset_is_announced(void)
{
	struct omci_t_fixture fx;

	if (!omci_t_up(&fx))
		return;
	uint8_t req[OMCI_FRAME_LEN];

	vec(RESET_REQUEST, req);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	const uint8_t one = 1;

	omci_t_tx_reset();
	CHECK_EQ(omci_mib_attr_set(fx.mib, ME_ANI_G, ANI_INSTANCE, ATTR_ARC,
				   &one, 1u),
		 OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	check_frame(0, AVC_ARC);
	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_a_mib_reset_is_answered_with_its_response_alone);
	RUN(test_the_reset_response_is_replayed_alone);
	RUN(test_a_change_the_onu_makes_after_a_reset_is_announced);
	return OMCI_T_REPORT("test_mibreset_reply");
}
