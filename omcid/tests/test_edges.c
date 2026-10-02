// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of edge cases across the small modules. */

#include <stdio.h>
#include <string.h>

#include "omci/cases.h"
#include "omci/clock.h"
#include "omci/frame.h"
#include "omcid/status.h"
#include "sys/log.h"

#include "harness.h"

static void test_every_status_has_a_name(void)
{
	CHECK_EQ(strcmp(omci_status_str(OMCI_OK), "OMCI_OK"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_DECLINED), "OMCI_DECLINED"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_INVAL), "OMCI_ERR_INVAL"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_NOMEM), "OMCI_ERR_NOMEM"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_TRUNCATED),
			"OMCI_ERR_TRUNCATED"),
		 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_PROTO), "OMCI_ERR_PROTO"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_MIC), "OMCI_ERR_MIC"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_ME_UNKNOWN),
			"OMCI_ERR_ME_UNKNOWN"),
		 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_INST_UNKNOWN),
			"OMCI_ERR_INST_UNKNOWN"),
		 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_ATTR_UNKNOWN),
			"OMCI_ERR_ATTR_UNKNOWN"),
		 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_EXISTS), "OMCI_ERR_EXISTS"),
		 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_BUSY), "OMCI_ERR_BUSY"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_HW), "OMCI_ERR_HW"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_IO), "OMCI_ERR_IO"), 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_ERR_AGAIN),
			"interrupted, try again"),
		 0);

	CHECK_EQ(strcmp(omci_status_str((enum omci_status)99), "OMCI_ERR_?"),
		 0);
	CHECK_EQ(strcmp(omci_status_str(OMCI_STATUS_SIGNED), "OMCI_ERR_?"), 0);
	CHECK_EQ(strcmp(omci_status_str((enum omci_status)(-1)), "OMCI_ERR_?"),
		 0);
}

static void test_the_mic_of_nothing_is_zero_and_of_a_frame_is_not(void)
{
	uint8_t wire[OMCI_FRAME_LEN] = { 0 };

	CHECK_EQ(omci_frame_mic(NULL, 44), 0u);
	CHECK_EQ(omci_frame_mic(wire, 0), 0u);
	CHECK(omci_frame_mic(wire, 44) != 0u);
	wire[0] = 1;
	CHECK(omci_frame_mic(wire, 44) != omci_frame_mic(&wire[1], 43));
}

static void test_a_clock_that_is_forgotten_says_nothing(void)
{
	struct omci_clock c = { 0 };

	CHECK(omci_clock_sync(&c, 2024, 5, 17, 12, 30, 15, 1000u));
	CHECK(omci_clock_is_set(&c));
	CHECK(omci_clock_now(&c, 1000u) != 0u);

	omci_clock_forget(NULL);
	CHECK(omci_clock_is_set(&c));

	omci_clock_forget(&c);
	CHECK(!omci_clock_is_set(&c));
	CHECK_EQ(omci_clock_now(&c, 5000u), 0u);
	CHECK_EQ(c.wall_at_sync, 0u);
	CHECK_EQ(c.mono_at_sync, 0u);
}

static void test_an_axis_that_is_not_there_has_no_cases(void)
{
	uint8_t wire[OMCI_FRAME_LEN];
	char label[32] = "stale";

	CHECK_EQ(omci_case_count(OMCI_CASE_AXIS_COUNT), 0u);
	CHECK_EQ(omci_case_count((enum omci_case_axis)99), 0u);
	CHECK(!omci_case_build(OMCI_CASE_AXIS_COUNT, 0, wire));
	CHECK(!omci_case_build((enum omci_case_axis)99, 0, wire));
	CHECK(omci_frame_mic_ok(wire));
	omci_case_label(OMCI_CASE_AXIS_COUNT, 0, label, sizeof(label));
	CHECK_EQ(label[0], '\0');
	CHECK(omci_case_axis_name(OMCI_CASE_AXIS_COUNT) == NULL);
	CHECK(omci_case_axis_name((enum omci_case_axis)99) == NULL);
	CHECK_EQ(omci_case_axis_by_name(NULL), OMCI_CASE_AXIS_COUNT);
	CHECK_EQ(omci_case_axis_by_name("no-such-axis"), OMCI_CASE_AXIS_COUNT);

	omci_case_label(OMCI_CASE_MUTATE, 0, NULL, 0);
	label[0] = 'x';
	omci_case_label(OMCI_CASE_MUTATE, 0, label, 0);
	CHECK_EQ(label[0], 'x');
}

static void test_the_log_level_is_what_was_set(void)
{
	const enum omci_log_level before = omci_log_level_get();

	for (int l = OMCI_LOG_ERR; l <= OMCI_LOG_TRACE; l++) {
		omci_log_level_set((enum omci_log_level)l);
		CHECK_EQ(omci_log_level_get(), (enum omci_log_level)l);
	}
	omci_log_level_set(before);
}

int main(void)
{
	RUN(test_every_status_has_a_name);
	RUN(test_the_mic_of_nothing_is_zero_and_of_a_frame_is_not);
	RUN(test_a_clock_that_is_forgotten_says_nothing);
	RUN(test_an_axis_that_is_not_there_has_no_cases);
	RUN(test_the_log_level_is_what_was_set);
	return OMCI_T_REPORT("test_edges");
}
