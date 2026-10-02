// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of MD5 and of the CHAP response that is made with it. */

#include <stdio.h>
#include <string.h>

#include "pppoed.h"

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

static void hex(const uint8_t d[16], char out[33])
{
	for (int i = 0; i < 16; i++)
		snprintf(out + 2 * i, 3, "%02x", d[i]);
}

int main(void)
{
	uint8_t d[16];
	char h[33];

	CHECK(md5_selftest());

	md5("", 0, d);
	hex(d, h);
	CHECK(strcmp(h, "d41d8cd98f00b204e9800998ecf8427e") == 0);

	md5("The quick brown fox jumps over the lazy dog", 43, d);
	hex(d, h);
	CHECK(strcmp(h, "9e107d9d372bb6826bd81d3542a419d6") == 0);

	/* The CHAP response: MD5 of the identifier, the secret, the challenge. */
	const uint8_t id = 7;
	const uint8_t challenge[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t joined[1 + 6 + sizeof(challenge)], whole[16];

	joined[0] = id;
	memcpy(joined + 1, "secret", 6);
	memcpy(joined + 7, challenge, sizeof(challenge));
	md5(joined, sizeof(joined), whole);
	md5_three(&id, 1, "secret", 6, challenge, sizeof(challenge), d);
	CHECK(memcmp(d, whole, sizeof(d)) == 0);

	printf("test_md5: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
