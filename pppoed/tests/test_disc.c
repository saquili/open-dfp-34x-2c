// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the discovery's PPP-Max-Payload (RFC 4638): what is asked for, what
 * a concentrator's answer leaves the session, and the tag read from a list.
 * The discovery's helpers are static, so its source is compiled in here.
 */

#include "../src/disc.c"

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static uint16_t wanted_at(uint16_t ifmtu)
{
	struct pppoe_session s = { .ifmtu = ifmtu };

	return max_payload_wanted(&s);
}

int main(void)
{
	uint8_t list[32];
	struct disc_tags t;
	uint8_t v[2];
	size_t off;

	printf("the PPP-Max-Payload asked for follows the interface's MTU\n");
	CHECK(wanted_at(1500) == 0);
	CHECK(wanted_at(1508) == 1500);
	CHECK(wanted_at(1501) == 1493);
	CHECK(wanted_at(1400) == 0);

	printf("the session keeps to 1492 unless the concentrator agrees\n");
	CHECK(max_payload_agreed(0, 1500) == PPPOE_MTU);
	CHECK(max_payload_agreed(1500, 0) == PPPOE_MTU);
	CHECK(max_payload_agreed(1500, 1500) == 1500);
	CHECK(max_payload_agreed(1500, 1496) == 1496);
	CHECK(max_payload_agreed(1500, 1600) == 1500);
	CHECK(max_payload_agreed(1500, 1400) == PPPOE_MTU);
	CHECK(max_payload_agreed(1500, PPPOE_MTU) == PPPOE_MTU);

	printf("the tag is read from a PADO or PADS, two bytes or nothing\n");
	pppoe_put16(v, 1500);
	off = tag_put(list, 0, PPPOE_TAG_AC_NAME, "bng", 3);
	off = tag_put(list, off, PPPOE_TAG_PPP_MAX_PAYLOAD, v, sizeof(v));
	CHECK(tags_parse(list, off, &t));
	CHECK(t.max_payload == 1500);
	CHECK(strcmp(t.ac_name, "bng") == 0);
	off = tag_put(list, 0, PPPOE_TAG_PPP_MAX_PAYLOAD, "\x05\xdc\x00", 3);
	CHECK(tags_parse(list, off, &t));
	CHECK(t.max_payload == 0);
	CHECK(tags_parse(list, 0, &t));
	CHECK(t.max_payload == 0);

	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
