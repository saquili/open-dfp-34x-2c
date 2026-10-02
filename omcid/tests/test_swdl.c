// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that the software download messages are refused. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "omcid/ctl_proto.h"

#include "harness.h"

static const struct {
	const char *name;
	const char *request;
	const char *response;
} g_family[] = {
	{ "StartSoftwareDownload",
	  "0501530a0007000107000000c801000100000000000000000000000000000000000000000000000000000028",
	  "0501330a00070001020000000000000000000000000000000000000000000000000000000000000000000028" },
	{ "DownloadSection",
	  "0502540a0007000107030a11181f262d343b424950575e656c737a81888f969da4abb2b9c0c7ced500000028",
	  "0502340a00070001020000000000000000000000000000000000000000000000000000000000000000000028" },
	{ "EndSoftwareDownload",
	  "0503550a00070001deadbeef000000c801000100000000000000000000000000000000000000000000000028",
	  "0503350a00070001020000000000000000000000000000000000000000000000000000000000000000000028" },
	{ "ActivateSoftware",
	  "0504560a00070001000000000000000000000000000000000000000000000000000000000000000000000028",
	  "0504360a00070001020000000000000000000000000000000000000000000000000000000000000000000028" },
	{ "CommitSoftware",
	  "0505570a00070001000000000000000000000000000000000000000000000000000000000000000000000028",
	  "0505370a00070001020000000000000000000000000000000000000000000000000000000000000000000028" },
	{ "Reboot",
	  "0506590a01000000000000000000000000000000000000000000000000000000000000000000000000000028",
	  "0506390a01000000020000000000000000000000000000000000000000000000000000000000000000000028" },
};

enum {
	ME_SOFTWARE_IMAGE = 7,
	ME_ONU_G = 256,
	MT_START = 19,
	MT_SECTION = 20,
	MT_END = 21,
	MT_ACTIVATE = 22,
	MT_COMMIT = 23,
	RESULT_NOT_SUPPORTED = 2,
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

static unsigned int g_verdict_seen[16];
static void on_tap(void *ctx, uint8_t verdict, uint8_t detail,
		   const uint8_t w[static OMCI_FRAME_LEN])
{
	(void)ctx;
	(void)detail;
	(void)w;
	if (verdict < 16u)
		g_verdict_seen[verdict]++;
}
static const struct omci_tap_ops tap_ops = { .frame = on_tap };

static bool fixture(struct omci_t_fixture *fx)
{
	struct omci_hw_caps caps;

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	if (!omci_t_up(fx))
		return false;
	CHECK_EQ(omci_me_defaults_apply(fx->mib, &caps), OMCI_OK);
	omci_engine_tap_set(fx->eng, &tap_ops, NULL);
	omci_t_tx_reset();
	memset(g_verdict_seen, 0, sizeof(g_verdict_seen));
	return true;
}

struct image_state {
	uint8_t bytes[2][16][32];
	uint8_t have[2][16];
};

static void snapshot(struct omci_mib *mib, struct image_state *st)
{
	memset(st, 0, sizeof(*st));
	for (unsigned int inst = 0; inst < 2u; inst++)
		for (uint8_t attr = 1; attr <= 16u; attr++) {
			size_t n = 0;

			if (omci_mib_attr_get(
				    mib, ME_SOFTWARE_IMAGE, (uint16_t)inst,
				    attr, st->bytes[inst][attr - 1u],
				    sizeof(st->bytes[0][0]), &n) == OMCI_OK)
				st->have[inst][attr - 1u] = 1;
		}
}

static void refused(struct omci_t_fixture *fx, const char *request,
		    const char *response, const char *name)
{
	uint8_t req[OMCI_FRAME_LEN], want[OMCI_FRAME_LEN];
	struct image_state before, after;

	vec(request, req);
	vec(response, want);

	snapshot(fx->mib, &before);
	const uint32_t gen = omci_mib_generation(fx->mib);

	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx->eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	if (omci_t_tx.n == 1u) {
		if (memcmp(omci_t_tx.frame[0], want, OMCI_FRAME_LEN) != 0)
			printf("    %s: the answer is not the expected one\n", name);
		CHECK_MEM(omci_t_tx.frame[0], want, OMCI_FRAME_LEN);
		CHECK(omci_frame_mic_ok(omci_t_tx.frame[0]));
	}

	snapshot(fx->mib, &after);
	CHECK_EQ(omci_mib_generation(fx->mib), gen);
	CHECK_MEM(&after, &before, sizeof(after));
}

static void test_every_message_is_refused_as_expected(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_family); i++) {
		struct omci_t_fixture fx;

		if (!fixture(&fx))
			return;
		refused(&fx, g_family[i].request, g_family[i].response,
			g_family[i].name);
		omci_t_down(&fx);
	}
}

static void
test_a_whole_download_is_refused_at_every_step_and_changes_nothing(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;

	struct image_state before, after;

	snapshot(fx.mib, &before);
	const uint32_t gen = omci_mib_generation(fx.mib);

	unsigned int tid = 0x0700;
	unsigned int answers = 0;
	uint8_t wire[OMCI_FRAME_LEN];
	static const uint8_t steps[] = { MT_START,   MT_SECTION, MT_SECTION,
					 MT_SECTION, MT_SECTION, MT_SECTION,
					 MT_SECTION, MT_SECTION, MT_SECTION,
					 MT_END,     MT_END,	 MT_ACTIVATE,
					 MT_COMMIT };
	for (size_t i = 0; i < sizeof(steps); i++) {
		const bool last_section = steps[i] == MT_SECTION && i == 8u;
		const bool ar = steps[i] != MT_SECTION || last_section;

		omci_t_build(wire, (uint16_t)tid++, steps[i], ar, false,
			     ME_SOFTWARE_IMAGE, 1u);
		omci_t_tx_reset();
		CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
		CHECK_EQ(omci_t_tx.n, 1u);
		if (omci_t_tx.n == 1u) {
			CHECK_EQ(omci_t_tx.frame[0][8],
				 (uint8_t)RESULT_NOT_SUPPORTED);
			CHECK_EQ(omci_t_tx.frame[0][2] & 0x1Fu, steps[i]);
			answers++;
		}
	}
	CHECK_EQ(answers, (unsigned int)sizeof(steps));

	snapshot(fx.mib, &after);
	CHECK_EQ(omci_mib_generation(fx.mib), gen);
	CHECK_MEM(&after, &before, sizeof(after));
	omci_t_down(&fx);
}

static void test_the_refusal_does_not_depend_on_the_entity(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;
	uint8_t wire[OMCI_FRAME_LEN];
	unsigned int tid = 0x0780;

	static const struct {
		uint16_t cls, inst;
	} named[] = {
		{ ME_SOFTWARE_IMAGE, 0x0005 },
		{ 3000, 0 },
		{ 257, 0 },
		{ ME_ONU_G, 0 },
		{ 263, 0x8001 },
	};
	for (size_t k = 0; k < ARRAY_SIZE(named); k++)
		for (uint8_t mt = MT_START; mt <= MT_COMMIT + 2u; mt++) {
			if (mt == 24u)
				continue;
			omci_t_build(wire, (uint16_t)tid++, mt, true, false,
				     named[k].cls, named[k].inst);
			omci_t_tx_reset();
			CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
			CHECK_EQ(omci_t_tx.n, 1u);
			if (omci_t_tx.n == 1u)
				CHECK_EQ(omci_t_tx.frame[0][8],
					 (uint8_t)RESULT_NOT_SUPPORTED);
		}
	omci_t_down(&fx);
}

static void test_the_engine_reports_no_handler_for_them(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;
	uint8_t req[OMCI_FRAME_LEN];

	vec(g_family[0].request, req);
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(g_verdict_seen[OMCI_CTL_TAP_RX_UNHANDLED], 1u);
	CHECK_EQ(g_verdict_seen[OMCI_CTL_TAP_RX_DISPATCH], 0u);
	omci_t_down(&fx);
}

static void
test_a_section_that_asks_for_no_answer_is_answered_all_the_same(void)
{
	struct omci_t_fixture fx;

	if (!fixture(&fx))
		return;
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, 0x0790u, MT_SECTION, false, false, ME_SOFTWARE_IMAGE,
		     1u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	if (omci_t_tx.n == 1u)
		CHECK_EQ(omci_t_tx.frame[0][8], (uint8_t)RESULT_NOT_SUPPORTED);
	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_every_message_is_refused_as_expected);
	RUN(test_a_whole_download_is_refused_at_every_step_and_changes_nothing);
	RUN(test_the_refusal_does_not_depend_on_the_entity);
	RUN(test_the_engine_reports_no_handler_for_them);
	RUN(test_a_section_that_asks_for_no_answer_is_answered_all_the_same);
	return OMCI_T_REPORT("test_swdl");
}
