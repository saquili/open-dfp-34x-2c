// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of every message type with every combination of request and answer bits. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/action.h"
#include "me/table.h"

#include "harness.h"
#include "omcid/array_size.h"

enum expect {
	EXP_DROPPED,
	EXP_ANSWERED,
	EXP_ANY_ANSWER,
};

static const struct {
	uint8_t mt;
	bool implemented;
	uint16_t home_class;
	const char *why;
} g_types[] = {

	{ 0u, false, 257u, "unassigned in G.988" },
	{ 1u, false, 257u, "unassigned" },
	{ 2u, false, 257u, "unassigned" },
	{ 3u, false, 257u, "unassigned" },
	{ 4u, true, 171u,
	  "Create — ONU2-G cannot be created; ME 171 is a class the OLT really creates" },
	{ 5u, false, 257u, "unassigned" },
	{ 6u, true, 171u,
	  "Delete — same entity as Create, for the same reason" },
	{ 7u, false, 257u, "unassigned" },
	{ 8u, true, 257u, "Set" },
	{ 9u, true, 257u, "Get" },
	{ 10u, false, 257u, "unassigned" },
	{ 11u, true, 2u, "GetAllAlarms" },
	{ 12u, true, 2u, "GetAllAlarmsNext" },
	{ 13u, true, 2u, "MibUpload" },
	{ 14u, true, 2u, "MibUploadNext" },
	{ 15u, true, 2u, "MibReset" },
	{ 16u, false, 257u,
	  "AlarmNotification — ONU to OLT. A downstream one is the OLT sending us our own message type, and there is nothing to do with it but refuse" },
	{ 17u, false, 257u,
	  "AttributeValueChange — ONU to OLT, same reasoning as 16" },
	{ 18u, true, 263u, "Test" },
	{ 19u, false, 7u,
	  "StartSoftwareDownload — this ONU takes no image, whatever entity is named" },
	{ 20u, false, 7u,
	  "DownloadSection — the one request that legally clears AR; refused all the same" },
	{ 21u, false, 7u, "EndSoftwareDownload — not supported" },
	{ 22u, false, 7u, "ActivateImage — not supported" },
	{ 23u, false, 7u, "CommitImage — not supported" },
	{ 24u, true, 256u,
	  "SynchronizeTime — G.988 puts it on ONU-G, not on ONU-Data" },
	{ 25u, false, 256u,
	  "Reboot — ONU-G is where G.988 puts it; this ONU does not restart on request" },
	{ 26u, true, 171u,
	  "GetNext — ONU2-G carries no table; 6 of the 29 classes do" },
	{ 27u, false, 257u, "TestResult — ONU to OLT, same reasoning as 16" },
	{ 28u, true, 321u,
	  "GetCurrentData — only the 2 classes with PM counters accept it" },
	{ 29u, false, 257u,
	  "Set table: the standard defines it for the extended message set only, so a baseline frame is refused" },
	{ 30u, false, 257u, "unassigned" },
	{ 31u, false, 257u, "unassigned" },
};

#define N_TYPES (ARRAY_SIZE(g_types))

#define RESULT_OK 0u
#define RESULT_PROCESSING_ERROR 1u
#define RESULT_CMD_NOT_SUPPORTED 2u
#define RESULT_UNKNOWN_ME 4u

#define MT_DOWNLOAD_SECTION 20u

#define CLASS_UNKNOWN 3000u

enum class_kind { CK_HOME = 0, CK_FOREIGN, CK_UNKNOWN, CK_COUNT };

static bool resultless(uint8_t mt)
{
	return mt == 11u || mt == 12u || mt == 13u || mt == 14u;
}

static uint16_t foreign_class_for(uint8_t mt)
{
	for (size_t c = 0; c < omci_me_class_count; c++)
		if (!omci_me_action_allowed(omci_me_classes[c].class_id, mt))
			return omci_me_classes[c].class_id;
	return 0u;
}

static unsigned int g_cases;
static unsigned int g_refusals_asserted;
static unsigned int g_unknown_asserted;

static void one(struct omci_engine *eng, unsigned int mt_index,
		unsigned int flags, enum class_kind kind, bool priority)
{
	const uint8_t mt = g_types[mt_index].mt;

	uint16_t cls = CLASS_UNKNOWN;

	if (kind == CK_HOME) {
		cls = g_types[mt_index].home_class;
	} else if (kind == CK_FOREIGN) {
		cls = foreign_class_for(mt);
		if (cls == 0u)
			return;
	}
	const bool known_class = (kind != CK_UNKNOWN);
	const bool ar = (flags & 1u) != 0u;
	const bool ak = (flags & 2u) != 0u;

	enum expect want = EXP_ANY_ANSWER;
	uint8_t want_result = 0u;

	if (ak || (!ar && mt != MT_DOWNLOAD_SECTION)) {
		want = EXP_DROPPED;
	} else if (!g_types[mt_index].implemented) {
		want = EXP_ANSWERED;
		want_result = RESULT_CMD_NOT_SUPPORTED;
	} else if (kind == CK_UNKNOWN) {
		want = EXP_ANSWERED;
		want_result = RESULT_UNKNOWN_ME;
		g_unknown_asserted++;
	}

	const uint16_t tci = (uint16_t)(0x0100u + (g_cases++ & 0x3FFFu));

	uint8_t wire[OMCI_FRAME_LEN];
	struct omci_frame f = {
		.tci = tci,
		.priority = priority,
		.msg_type = mt,
		.ar = ar,
		.ak = ak,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = cls,
		.instance = 0u,
	};
	(void)!omci_frame_build(&f, wire);

	omci_t_tx_reset();
	const enum omci_status rc = omci_engine_rx(eng, wire);

	if (want == EXP_DROPPED) {
		if (omci_t_tx.n != 0u || rc == OMCI_OK)
			printf("    mt %u ar=%d ak=%d %s %s: answered with %u frame(s), rc=%d — %s\n",
			       mt, ar, ak, known_class ? "known" : "unknown",
			       priority ? "pri" : "ord", omci_t_tx.n, (int)rc,
			       g_types[mt_index].why);
		CHECK_EQ(omci_t_tx.n, 0u);
		CHECK(rc != OMCI_OK);
		return;
	}

	CHECK_EQ(rc, OMCI_OK);

	if (omci_t_tx.n == 0u) {
		printf("    mt %u ar=%d ak=%d %s %s: silence — %s\n", mt, ar,
		       ak, known_class ? "known" : "unknown",
		       priority ? "pri" : "ord", g_types[mt_index].why);
	}
	CHECK(omci_t_tx.n >= 1u);

	const uint8_t *rsp = NULL;

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++) {
		const uint8_t *f2 = omci_t_tx.frame[i];
		const uint16_t t2 = (uint16_t)(((uint16_t)f2[0] << 8) | f2[1]);
		const bool ak2 = (f2[2] & 0x20u) != 0u;

		if (ak2 && (t2 & 0x7FFFu) == tci) {
			CHECK(rsp == NULL);
			rsp = f2;
			continue;
		}

		if ((f2[2] & 0x1Fu) == 27u && (t2 & 0x7FFFu) == tci) {
			CHECK_EQ(f2[2] & 0x20u, 0u);
			CHECK_EQ(f2[2] & 0x40u, 0u);
			continue;
		}

		CHECK_EQ(t2 & 0x7FFFu, 0u);
		CHECK_EQ(f2[2] & 0x20u, 0u);
		CHECK_EQ(f2[2] & 0x40u, 0u);
	}

	if (rsp == NULL) {
		printf("    mt %u %s %s: %u frame(s), none answering transaction %u\n",
		       mt, known_class ? "known" : "unknown",
		       priority ? "pri" : "ord", omci_t_tx.n, tci);
	}
	CHECK(rsp != NULL);
	if (rsp == NULL)
		return;

	const uint16_t rsp_tci = (uint16_t)(((uint16_t)rsp[0] << 8) | rsp[1]);

	CHECK_EQ((rsp_tci & 0x8000u) != 0u, priority);

	CHECK_EQ(rsp[2] & 0x1Fu, mt);
	CHECK_EQ(rsp[2] & 0x40u, 0u);

	if (want == EXP_ANSWERED) {
		if (resultless(mt) && want_result != RESULT_OK) {
			for (unsigned int i = 8u; i < OMCI_FRAME_LEN - 8u; i++)
				CHECK_EQ(rsp[i], 0u);
			return;
		}
		if (rsp[8] != want_result) {
			printf("    mt %u %s %s: result %u, want %u — %s\n", mt,
			       known_class ? "known" : "unknown",
			       priority ? "pri" : "ord", rsp[8], want_result,
			       g_types[mt_index].why);
		}
		CHECK_EQ(rsp[8], want_result);
		return;
	}

	if (!omci_me_action_allowed(cls, mt)) {
		g_refusals_asserted++;

		if (resultless(mt)) {
			for (unsigned int i = 8u; i < OMCI_FRAME_LEN - 8u; i++)
				CHECK_EQ(rsp[i], 0u);
		} else {
			CHECK_EQ(rsp[8], RESULT_CMD_NOT_SUPPORTED);
		}
		return;
	}
	CHECK(rsp[8] != RESULT_CMD_NOT_SUPPORTED);
	if (known_class)
		CHECK(rsp[8] != RESULT_UNKNOWN_ME);
}

static void test_every_message_type_against_every_flag_and_class(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	g_refusals_asserted = 0;
	g_unknown_asserted = 0;

	for (unsigned int t = 0; t < N_TYPES; t++)
		for (unsigned int flags = 0; flags < 4u; flags++)
			for (unsigned int k = 0; k < (unsigned int)CK_COUNT;
			     k++)
				for (unsigned int p = 0; p < 2u; p++)
					one(fx.eng, t, flags,
					    (enum class_kind)k, p != 0u);

	printf("  the refusal arm fired %u times, the unknown-entity arm %u\n",
	       g_refusals_asserted, g_unknown_asserted);

	CHECK(g_refusals_asserted > 0u);
	CHECK(g_unknown_asserted > 0u);

	omci_t_down(&fx);
}

static void test_only_download_section_may_clear_the_answer_request(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	unsigned int accepted = 0;

	for (unsigned int mt = 0; mt < 32u; mt++) {
		uint8_t wire[OMCI_FRAME_LEN];
		struct omci_frame f = {
			.tci = (uint16_t)(0x7000u + mt),
			.priority = false,
			.msg_type = (uint8_t)mt,
			.ar = false,
			.ak = false,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = 257u,
			.instance = 0u,
		};
		(void)!omci_frame_build(&f, wire);

		omci_t_tx_reset();
		if (omci_engine_rx(fx.eng, wire) == OMCI_OK) {
			accepted++;
			if (mt != MT_DOWNLOAD_SECTION)
				printf("    mt %u accepted with AR clear\n",
				       mt);
			CHECK_EQ(mt, MT_DOWNLOAD_SECTION);
		}
	}
	CHECK_EQ(accepted, 1u);

	omci_t_down(&fx);
}

static void test_a_frame_marked_as_a_response_changes_nothing(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	const uint16_t cls = 322u, inst = 0x0505u;
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, 0x0301u, 4u, false, true, cls, inst);
	omci_t_tx_reset();
	CHECK(omci_t_rx(fx.eng, wire) != OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 0u);
	CHECK(!omci_mib_exists(fx.mib, cls, inst));

	omci_t_build(wire, 0x0302u, 4u, true, false, cls, inst);
	CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
	CHECK(omci_mib_exists(fx.mib, cls, inst));

	omci_t_build(wire, 0x0303u, 6u, false, true, cls, inst);
	omci_t_tx_reset();
	CHECK(omci_t_rx(fx.eng, wire) != OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 0u);
	CHECK(omci_mib_exists(fx.mib, cls, inst));

	omci_t_down(&fx);
}

static void test_the_onu_to_olt_types_are_refused_downstream(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	static const uint8_t upstream_only[] = { 16u, 17u, 27u };

	for (size_t i = 0; i < ARRAY_SIZE(upstream_only); i++) {
		uint8_t wire[OMCI_FRAME_LEN];

		omci_t_build(wire, (uint16_t)(0x0400u + i), upstream_only[i],
			     true, false, 257u, 0u);
		omci_t_tx_reset();
		CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
		CHECK_EQ(omci_t_tx.n, 1u);
		CHECK_EQ(omci_t_last_tx()[8], RESULT_CMD_NOT_SUPPORTED);
	}

	omci_t_down(&fx);
}

int main(void)
{
	RUN(test_every_message_type_against_every_flag_and_class);
	RUN(test_only_download_section_may_clear_the_answer_request);
	RUN(test_a_frame_marked_as_a_response_changes_nothing);
	RUN(test_the_onu_to_olt_types_are_refused_downstream);

	return OMCI_T_REPORT("msgtype-matrix");
}
