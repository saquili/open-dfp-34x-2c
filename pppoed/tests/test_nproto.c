// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of pppoed's messages to netifd (nproto.h). */

#include <stdio.h>
#include <string.h>

#include "nproto.h"

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

static void test_dump(void)
{
	char buf[512];

	static const char head[] = "{\"name\":\"pppoed\",\"config\":[[\"session\",5],"
				   "[\"family\",3],[\"pass\",7],[\"defaultroute\",7]],";

	CHECK(nproto_dump_json(buf, sizeof(buf)) == (int)strlen(buf));
	CHECK(!strncmp(buf, head, strlen(head)));
	CHECK(strstr(buf, "\"teardown-on-l3-link-down\":true") != NULL);
	CHECK(nproto_dump_json(buf, 10) == -1);
}

static void test_run_and_kill(void)
{
	const char *argv[] = { "/usr/sbin/pppoed", "up", "-i", "wan1410", "-N", "ses\"1", NULL };
	const char *none[] = { NULL };
	char buf[512];

	CHECK(nproto_run_json(buf, sizeof(buf), argv) > 0);
	CHECK(!strcmp(buf, "{\"action\":1,\"command\":[\"/usr/sbin/pppoed\",\"up\",\"-i\","
			   "\"wan1410\",\"-N\",\"ses\\\"1\"]}"));
	CHECK(nproto_run_json(buf, sizeof(buf), none) == -1);
	CHECK(nproto_run_json(buf, 20, argv) == -1);
	CHECK(nproto_kill_json(buf, sizeof(buf)) > 0 &&
	      !strcmp(buf, "{\"action\":2,\"signal\":15}"));
}

static void test_up(void)
{
	struct nproto_link l = {
		.ifname = "ppp0", .local = "203.0.113.24", .peer = "203.0.113.1",
		.dns = { "198.51.100.53", "198.51.100.54" }, .session = 17,
		.defaultroute = true,
	};
	char buf[1024];

	CHECK(nproto_up_json(buf, sizeof(buf), &l) == (int)strlen(buf));
	CHECK(!strcmp(buf, "{\"action\":0,\"link-up\":true,\"ifname\":\"ppp0\","
			   "\"ipaddr\":[{\"ipaddr\":\"203.0.113.24\",\"mask\":\"32\","
			   "\"ptp\":\"203.0.113.1\"}],"
			   "\"routes\":[{\"target\":\"0.0.0.0\",\"netmask\":\"0\"}],"
			   "\"dns\":[\"198.51.100.53\",\"198.51.100.54\"],"
			   "\"data\":{\"session\":17,\"address\":\"203.0.113.24\","
			   "\"peer\":\"203.0.113.1\",\"pass\":false}}"));

	/* Handed on: no address here, the default route all the same. */
	l.pass = true;
	CHECK(nproto_up_json(buf, sizeof(buf), &l) > 0);
	CHECK(!strstr(buf, "\"ipaddr\":[") && strstr(buf, "\"routes\":[") &&
	      strstr(buf, "\"pass\":true}") && strstr(buf, "\"address\":\"203.0.113.24\""));

	/* IPv6 alone: no address, no route, no name server of IPv4. */
	l = (struct nproto_link){ .ifname = "ppp1", .session = 18, .defaultroute = true };
	CHECK(nproto_up_json(buf, sizeof(buf), &l) > 0);
	CHECK(!strcmp(buf, "{\"action\":0,\"link-up\":true,\"ifname\":\"ppp1\","
			   "\"data\":{\"session\":18,\"pass\":false}}"));

	/* One name server; none without an interface; and too short a buffer. */
	l = (struct nproto_link){ .ifname = "ppp0", .local = "203.0.113.24",
				  .dns = { NULL, "198.51.100.54" } };
	CHECK(nproto_up_json(buf, sizeof(buf), &l) > 0 &&
	      strstr(buf, "\"dns\":[\"198.51.100.54\"]") && !strstr(buf, "routes"));
	l.ifname = NULL;
	CHECK(nproto_up_json(buf, sizeof(buf), &l) == -1);
	l.ifname = "ppp0";
	CHECK(nproto_up_json(buf, 40, &l) == -1);
}

static void test_config(void)
{
	/* As netifd formats the interface's configuration (blobmsg_format_json). */
	const char *config = "{ \"session\": 3, \"family\": \"ipv6\", \"pass\": true, "
			     "\"defaultroute\": false }";
	char s[16];

	CHECK(nproto_session(config) == 3);
	CHECK(nproto_string(config, "family", s, sizeof(s)) == 4 && !strcmp(s, "ipv6"));
	CHECK(nproto_bool(config, "pass") == 1 && nproto_bool(config, "defaultroute") == 0);
	CHECK(nproto_bool(config, "missing") == -1);
	CHECK(nproto_string(config, "family", s, 4) == -1);
	CHECK(nproto_string(config, "session", s, sizeof(s)) == -1);
	CHECK(nproto_session("{\"session\":\"7\"}") == 7);
	CHECK(nproto_session("{\"session\": 8}") == -1);
	CHECK(nproto_session("{\"session\": 0}") == -1);
	CHECK(nproto_session("{\"session\": 12}") == -1);
	CHECK(nproto_session("{\"family\": \"ipv4\"}") == -1);
	CHECK(nproto_session(NULL) == -1);
	CHECK(nproto_bool("{\"pass\": \"1\"}", "pass") == 1);
}

int main(void)
{
	test_dump();
	test_run_and_kill();
	test_up();
	test_config();
	printf("test_nproto: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
