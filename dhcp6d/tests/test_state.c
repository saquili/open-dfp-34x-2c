// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of dhcp6d's state as it publishes it (state.h). */

#include <stdio.h>
#include <string.h>

#include "state.h"

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

int main(void)
{
	struct dhcp6d_state s = {
		.name = "ses2", .wan = "ppp1", .lan = "lan410",
		.prefix = "2001:db8:1200:400::/56", .host = "fe80::2",
		.bound = true, .routed = true,
	};
	char buf[512];

	CHECK(dhcp6d_state_json(&s, buf, sizeof(buf)) == (int)strlen(buf));
	CHECK(!strcmp(buf, "{\"name\":\"ses2\",\"wan\":\"ppp1\",\"lan\":\"lan410\","
			   "\"prefix\":\"2001:db8:1200:400::/56\","
			   "\"host\":\"fe80::2\",\"bound\":true,\"routed\":true}"));

	/* Nothing delegated yet, and a name that needs an escape. */
	s = (struct dhcp6d_state){ .name = "a\"b", .wan = "ppp1", .lan = "lan410" };
	CHECK(dhcp6d_state_json(&s, buf, sizeof(buf)) > 0);
	CHECK(!strcmp(buf, "{\"name\":\"a\\\"b\",\"wan\":\"ppp1\",\"lan\":\"lan410\","
			   "\"prefix\":null,\"host\":null,\"bound\":false,\"routed\":false}"));
	CHECK(dhcp6d_state_json(&s, buf, 20) == -1);
	CHECK(dhcp6d_state_json(&s, buf, 0) == -1);

	printf("test_state: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
