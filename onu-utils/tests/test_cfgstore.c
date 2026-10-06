// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the configuration store format, on regions built here. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfgstore.h"

static int failures;
static int checks;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

#define REGION 256u /* two 128-byte slots, 104 bytes of payload each */

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* A fresh region reads as empty: all 0xff, as an erased flash is. */
static void erase(uint8_t *region)
{
	memset(region, 0xff, REGION);
}

/* cfg_store a payload and copy only its reported bytes into the region, as a
 * caller carrying one slot to the flash would. */
static int save(uint8_t *region, int slot, uint32_t seq, const char *payload)
{
	uint8_t one[REGION];
	size_t off, len;
	int err;

	memcpy(one, region, REGION);
	err = cfg_store(one, REGION, slot, seq, payload, strlen(payload), &off,
			&len);
	if (err)
		return err;
	memcpy(region + off, one + off, len);
	return 0;
}

static void test_geometry(void)
{
	CHECK(cfg_payload_cap(REGION) == REGION / 2 - CFG_HEADER);
	CHECK(cfg_payload_cap(2 * CFG_HEADER) == 0);
	CHECK(cfg_payload_cap(0) == 0);
}

static void test_empty(void)
{
	uint8_t region[REGION];
	char payload[128];

	erase(region);
	CHECK(cfg_load(region, REGION, payload, sizeof(payload), NULL, NULL,
		       NULL) == -ENOENT);
}

static void test_round_trip(void)
{
	uint8_t region[REGION];
	char payload[128];
	uint32_t seq = 0;
	int slot = -1;

	erase(region);
	CHECK(save(region, -1, 0, "MODE=passthrough\n") == 0);
	CHECK(cfg_load(region, REGION, payload, sizeof(payload), NULL, &seq,
		       &slot) == 0);
	CHECK(slot == 0 && seq == 1);
	CHECK(strcmp(payload, "MODE=passthrough\n") == 0);

	/* The first write is the magic, big-endian, so the stick reads it. */
	CHECK(get32(region) == CFG_MAGIC);
}

static void test_get_set_unset(void)
{
	char payload[128] = "";
	char val[64];

	CHECK(cfg_set(payload, sizeof(payload), "MODE", "router") == 0);
	CHECK(cfg_set(payload, sizeof(payload), "WAN_VLAN", "410") == 0);
	CHECK(cfg_get(payload, "MODE", val, sizeof(val)) == 0 &&
	      strcmp(val, "router") == 0);
	CHECK(cfg_get(payload, "WAN_VLAN", val, sizeof(val)) == 0 &&
	      strcmp(val, "410") == 0);

	/* A second set of the same key replaces its line, not adds one. */
	CHECK(cfg_set(payload, sizeof(payload), "MODE", "passthrough") == 0);
	CHECK(cfg_get(payload, "MODE", val, sizeof(val)) == 0 &&
	      strcmp(val, "passthrough") == 0);
	CHECK(cfg_get(payload, "WAN_VLAN", val, sizeof(val)) == 0 &&
	      strcmp(val, "410") == 0);
	CHECK(strcmp(payload, "MODE=passthrough\nWAN_VLAN=410\n") == 0);

	CHECK(cfg_unset(payload, "MODE") == 0);
	CHECK(cfg_get(payload, "MODE", val, sizeof(val)) == -ENOENT);
	CHECK(cfg_get(payload, "WAN_VLAN", val, sizeof(val)) == 0);
	CHECK(strcmp(payload, "WAN_VLAN=410\n") == 0);

	/* Unset of an absent key is a success that changes nothing. */
	CHECK(cfg_unset(payload, "MODE") == 0);
	CHECK(strcmp(payload, "WAN_VLAN=410\n") == 0);

	/* An empty value round-trips. */
	CHECK(cfg_set(payload, sizeof(payload), "WAN_MAC", "") == 0);
	CHECK(cfg_get(payload, "WAN_MAC", val, sizeof(val)) == 0 &&
	      val[0] == '\0');
}

static void test_bad_input(void)
{
	char payload[128] = "";
	char val[8];

	CHECK(cfg_set(payload, sizeof(payload), "lower", "x") == -EINVAL);
	CHECK(cfg_set(payload, sizeof(payload), "9START", "x") == -EINVAL);
	CHECK(cfg_set(payload, sizeof(payload), "A B", "x") == -EINVAL);
	CHECK(cfg_set(payload, sizeof(payload), "K", "two\nlines") == -EINVAL);
	CHECK(payload[0] == '\0');
	CHECK(!cfg_valid_key(""));
	CHECK(cfg_valid_key("A"));
	CHECK(cfg_valid_key("WAN_VLAN_2"));

	/* A line that does not fit the payload buffer is refused, not cut. */
	CHECK(cfg_set(payload, 8, "KEY", "value-too-long") == -EFBIG);
	CHECK(payload[0] == '\0');

	/* A get into too small a buffer is refused, not cut. */
	CHECK(cfg_set(payload, sizeof(payload), "K", "abcdefgh") == 0);
	CHECK(cfg_get(payload, "K", val, sizeof(val)) == -EFBIG);
}

static void test_too_big_for_a_slot(void)
{
	uint8_t region[REGION];
	char big[200];

	erase(region);
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	CHECK(save(region, -1, 0, big) == -EFBIG);
}

static void test_slots_alternate(void)
{
	uint8_t region[REGION];
	char payload[128];
	uint32_t seq;
	int slot;

	erase(region);
	CHECK(save(region, -1, 0, "N=1\n") == 0);
	cfg_load(region, REGION, payload, sizeof(payload), NULL, &seq, &slot);
	CHECK(slot == 0 && seq == 1);

	CHECK(save(region, slot, seq, "N=2\n") == 0);
	cfg_load(region, REGION, payload, sizeof(payload), NULL, &seq, &slot);
	CHECK(slot == 1 && seq == 2 && strcmp(payload, "N=2\n") == 0);

	CHECK(save(region, slot, seq, "N=3\n") == 0);
	cfg_load(region, REGION, payload, sizeof(payload), NULL, &seq, &slot);
	CHECK(slot == 0 && seq == 3 && strcmp(payload, "N=3\n") == 0);
}

static void test_higher_seq_wins(void)
{
	uint8_t region[REGION];
	char payload[128];
	uint32_t seq;
	int slot;

	/* Slot 1 is newer than slot 0, whatever their order on the flash. */
	erase(region);
	save(region, -1, 0, "N=old\n");     /* slot 0, seq 1 */
	save(region, 0, 1, "N=new\n");      /* slot 1, seq 2 */
	save(region, 1, 2, "N=newer\n");    /* slot 0, seq 3 */
	cfg_load(region, REGION, payload, sizeof(payload), NULL, &seq, &slot);
	CHECK(slot == 0 && seq == 3 && strcmp(payload, "N=newer\n") == 0);
}

static void test_torn_write_keeps_the_old(void)
{
	uint8_t region[REGION];
	char payload[128];
	uint32_t seq;
	int slot;

	/* Slot 0 holds the good configuration. */
	erase(region);
	save(region, -1, 0, "N=good\n");
	cfg_load(region, REGION, payload, sizeof(payload), NULL, &seq, &slot);
	CHECK(slot == 0 && seq == 1);

	/* A write to slot 1 that a power loss cut: its payload is not whole,
	 * so its checksum fails and the reader keeps slot 0. */
	save(region, slot, seq, "N=half\n");
	region[REGION / 2 + CFG_HEADER + 1] ^= 0xff;
	CHECK(cfg_load(region, REGION, payload, sizeof(payload), NULL, &seq,
		       &slot) == 0);
	CHECK(slot == 0 && seq == 1 && strcmp(payload, "N=good\n") == 0);
}

static void test_checksum_rejects_corruption(void)
{
	uint8_t region[REGION];
	char payload[128];

	erase(region);
	save(region, -1, 0, "N=good\n");
	region[CFG_HEADER] ^= 0xff; /* a flipped payload byte */
	CHECK(cfg_load(region, REGION, payload, sizeof(payload), NULL, NULL,
		       NULL) == -ENOENT);
}

static void test_length_guard(void)
{
	uint8_t region[REGION];
	char payload[128];

	erase(region);
	save(region, -1, 0, "N=good\n");
	/* A length past the slot makes the slot invalid, not an over-read. */
	put32(region + 12, REGION);
	CHECK(cfg_load(region, REGION, payload, sizeof(payload), NULL, NULL,
		       NULL) == -ENOENT);
}

static void test_other_slot_untouched(void)
{
	uint8_t region[REGION];
	uint8_t before[REGION / 2];

	erase(region);
	save(region, -1, 0, "N=1\n");         /* slot 0 */
	memcpy(before, region, REGION / 2);
	save(region, 0, 1, "N=2\n");          /* slot 1 */
	CHECK(memcmp(region, before, REGION / 2) == 0);
}

int main(void)
{
	test_geometry();
	test_empty();
	test_round_trip();
	test_get_set_unset();
	test_bad_input();
	test_too_big_for_a_slot();
	test_slots_alternate();
	test_higher_seq_wins();
	test_torn_write_keeps_the_old();
	test_checksum_rejects_corruption();
	test_length_guard();
	test_other_slot_untouched();

	printf("test_cfgstore: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
