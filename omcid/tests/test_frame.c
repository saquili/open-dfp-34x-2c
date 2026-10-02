// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of parsing and building frames, and of the integrity check. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "omci/frame.h"
#include "omcid/status.h"

#include "harness.h"

static const uint8_t mibreset_req[OMCI_FRAME_LEN] = {
	0x00, 0x01, 0x4f, 0x0a, 0x00, 0x02, 0x00, 0x00,

	0,    0,    0,	  0,	0,    0,    0,	  0,	0, 0, 0, 0, 0, 0, 0, 0,
	0,    0,    0,	  0,	0,    0,    0,	  0,	0, 0, 0, 0, 0, 0, 0, 0,
	0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00,
};

static void test_parse_decodes_every_field(void)
{
	struct omci_frame f;

	CHECK_EQ(omci_frame_parse(mibreset_req, &f), OMCI_OK);
	CHECK_EQ(f.tci, 1);
	CHECK(!f.priority);
	CHECK_EQ(f.msg_type, 0x0f);
	CHECK(f.ar);
	CHECK(!f.ak);
	CHECK_EQ(f.device_id, OMCI_DEVICE_ID_BASELINE);
	CHECK_EQ(f.class_id, 2);
	CHECK_EQ(f.instance, 0);

	for (unsigned int i = 0; i < OMCI_CONTENTS_LEN; i++)
		CHECK_EQ(f.contents[i], 0);
}

static void test_the_priority_bit_is_not_part_of_the_tci(void)
{
	uint8_t wire[OMCI_FRAME_LEN];

	memcpy(wire, mibreset_req, sizeof(wire));
	wire[0] = 0x80;
	wire[1] = 0x01;

	struct omci_frame f;

	CHECK_EQ(omci_frame_parse(wire, &f), OMCI_OK);
	CHECK(f.priority);
	CHECK_EQ(f.tci, 1);
}

static void test_build_then_parse_round_trips(void)
{
	struct omci_frame in = {
		.tci = 0x1234,
		.priority = true,
		.msg_type = 0x09,
		.ar = true,
		.ak = false,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = 171,
		.instance = 0x0101,
	};
	for (unsigned int i = 0; i < OMCI_CONTENTS_LEN; i++)
		in.contents[i] = (uint8_t)(0xa0 + i);

	uint8_t wire[OMCI_FRAME_LEN];

	CHECK_EQ(omci_frame_build(&in, wire), OMCI_OK);

	struct omci_frame out;

	CHECK_EQ(omci_frame_parse(wire, &out), OMCI_OK);

	CHECK_EQ(out.tci, in.tci);
	CHECK_EQ(out.priority, in.priority);
	CHECK_EQ(out.msg_type, in.msg_type);
	CHECK_EQ(out.ar, in.ar);
	CHECK_EQ(out.ak, in.ak);
	CHECK_EQ(out.class_id, in.class_id);
	CHECK_EQ(out.instance, in.instance);
	CHECK_EQ(memcmp(out.contents, in.contents, OMCI_CONTENTS_LEN), 0);
}

static void test_multibyte_fields_are_big_endian_on_the_wire(void)
{
	struct omci_frame in = {
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = 171,
		.instance = 0x0102,
	};
	uint8_t wire[OMCI_FRAME_LEN];

	CHECK_EQ(omci_frame_build(&in, wire), OMCI_OK);

	CHECK_EQ(wire[4], 0x00);
	CHECK_EQ(wire[5], 0xab);
	CHECK_EQ(wire[6], 0x01);
	CHECK_EQ(wire[7], 0x02);
}

static void test_the_trailer_is_always_written(void)
{
	struct omci_frame in = { .device_id = OMCI_DEVICE_ID_BASELINE };
	uint8_t wire[OMCI_FRAME_LEN];

	CHECK_EQ(omci_frame_build(&in, wire), OMCI_OK);

	CHECK_EQ(wire[40], 0x00);
	CHECK_EQ(wire[41], 0x00);
	CHECK_EQ(wire[42], 0x00);
	CHECK_EQ(wire[43], 0x28);
}

static void test_a_built_frame_verifies_its_own_mic(void)
{
	struct omci_frame in = { .device_id = OMCI_DEVICE_ID_BASELINE,
				 .class_id = 2 };
	uint8_t wire[OMCI_FRAME_LEN];

	CHECK_EQ(omci_frame_build(&in, wire), OMCI_OK);
	CHECK(omci_frame_mic_ok(wire));

	wire[9] ^= 0x01;
	CHECK(!omci_frame_mic_ok(wire));
}

static void test_crc32_matches_the_known_answer(void)
{
	CHECK_EQ(omci_frame_mic((const uint8_t *)"123456789", 9), 0xfc891918u);
}

static void test_structural_errors_are_rejected(void)
{
	uint8_t wire[OMCI_FRAME_LEN];
	struct omci_frame f;

	memcpy(wire, mibreset_req, sizeof(wire));
	wire[3] = 0x0b;
	CHECK_EQ(omci_frame_parse(wire, &f), OMCI_ERR_PROTO);

	memcpy(wire, mibreset_req, sizeof(wire));
	wire[43] = 0x29;
	CHECK_EQ(omci_frame_parse(wire, &f), OMCI_ERR_PROTO);

	CHECK_EQ(omci_frame_parse(mibreset_req, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_frame_build(NULL, wire), OMCI_ERR_INVAL);
}

int main(void)
{
	RUN(test_parse_decodes_every_field);
	RUN(test_the_priority_bit_is_not_part_of_the_tci);
	RUN(test_build_then_parse_round_trips);
	RUN(test_multibyte_fields_are_big_endian_on_the_wire);
	RUN(test_the_trailer_is_always_written);
	RUN(test_a_built_frame_verifies_its_own_mic);
	RUN(test_crc32_matches_the_known_answer);
	RUN(test_structural_errors_are_rejected);

	printf("\n%d omci_t_checks, %d omci_t_failures\n", omci_t_checks,
	       omci_t_failures);
	return OMCI_T_REPORT("test_frame");
}
