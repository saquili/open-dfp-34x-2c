// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of onu-svc's messages to procd (svcmsg.h). */

#include <stdio.h>
#include <string.h>

#include "svcmsg.h"

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

static void test_names(void)
{
	CHECK(svc_name_ok("netifd"));
	CHECK(svc_name_ok("pppoed-1"));
	CHECK(svc_name_ok("Ssh_2"));
	CHECK(!svc_name_ok(""));
	CHECK(!svc_name_ok(NULL));
	CHECK(!svc_name_ok("a b"));
	CHECK(!svc_name_ok("a/b"));
	CHECK(!svc_name_ok("\"x\""));
}

static void test_respawn(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;

	CHECK(r.threshold == 60 && r.timeout == 2 && r.retry == 0);
	CHECK(svc_parse_respawn("3600,5,3", &r) == 0);
	CHECK(r.threshold == 3600 && r.timeout == 5 && r.retry == 3);
	CHECK(svc_parse_respawn("0,0,0", &r) == 0 && !r.threshold && !r.timeout && !r.retry);
	CHECK(svc_parse_respawn("60,2", &r) == -1);
	CHECK(svc_parse_respawn("60,2,0,1", &r) == -1);
	CHECK(svc_parse_respawn("60,,0", &r) == -1);
	CHECK(svc_parse_respawn("-1,2,0", &r) == -1);
	CHECK(svc_parse_respawn("60,2,x", &r) == -1);
	CHECK(svc_parse_respawn("60,2,0 ", &r) == -1);
	CHECK(svc_parse_respawn("9999999999,2,0", &r) == -1);
	CHECK(svc_parse_respawn("", &r) == -1);
}

static void test_set(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "/usr/sbin/netifd", "-c", "/run/config", NULL };
	char buf[512];

	CHECK(svc_set_json(buf, sizeof(buf), "netifd", &r, false, argv) == (int)strlen(buf));
	CHECK(!strcmp(buf, "{\"name\":\"netifd\",\"instances\":{\"netifd\":{\"command\":"
			   "[\"/usr/sbin/netifd\",\"-c\",\"/run/config\"],"
			   "\"respawn\":[\"60\",\"2\",\"0\"]}}}"));

	const char *one[] = { "pond", NULL };
	r = (struct svc_respawn){ 3600, 5, 3 };
	CHECK(svc_set_json(buf, sizeof(buf), "pond", &r, true, one) > 0);
	CHECK(!strcmp(buf, "{\"name\":\"pond\",\"instances\":{\"pond\":{\"command\":[\"pond\"],"
			   "\"respawn\":[\"3600\",\"5\",\"3\"],\"stdout\":true,\"stderr\":true}}}"));
}

static void test_escapes(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "sh", "-c", "echo \"a\\b\"\n\t", NULL };
	char buf[512];

	CHECK(svc_set_json(buf, sizeof(buf), "x", &r, false, argv) > 0);
	CHECK(strstr(buf, "[\"sh\",\"-c\",\"echo \\\"a\\\\b\\\"\\u000a\\u0009\"]") != NULL);
}

static void test_too_long(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "a-program-with-a-long-name", NULL };
	char buf[512];
	int n;

	n = svc_set_json(buf, sizeof(buf), "x", &r, false, argv);
	CHECK(n > 0);
	/* Every size short of the whole fails, and never writes past it. */
	for (size_t cap = 0; cap <= (size_t)n; cap++) {
		char small[512];

		memset(small, 'Z', sizeof(small));
		CHECK(svc_set_json(small, cap, "x", &r, false, argv) == -1);
		CHECK(small[cap] == 'Z');
	}
	CHECK(svc_set_json(buf, (size_t)n + 1, "x", &r, false, argv) == n);
}

static void test_name(void)
{
	char buf[64];

	CHECK(svc_name_json(buf, sizeof(buf), "omcid") == 16);
	CHECK(!strcmp(buf, "{\"name\":\"omcid\"}"));
	CHECK(svc_name_json(buf, 10, "omcid") == -1);
}

int main(void)
{
	test_names();
	test_respawn();
	test_set();
	test_escapes();
	test_too_long();
	test_name();
	printf("test_svcmsg: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
