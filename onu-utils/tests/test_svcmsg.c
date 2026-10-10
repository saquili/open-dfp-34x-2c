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

	CHECK(svc_set_json(buf, sizeof(buf), "netifd", &r, false, NULL, argv) == (int)strlen(buf));
	CHECK(!strcmp(buf, "{\"name\":\"netifd\",\"instances\":{\"netifd\":{\"command\":"
			   "[\"/usr/sbin/netifd\",\"-c\",\"/run/config\"],"
			   "\"respawn\":[\"60\",\"2\",\"0\"]}}}"));

	const char *one[] = { "pond", NULL };
	r = (struct svc_respawn){ 3600, 5, 3 };
	CHECK(svc_set_json(buf, sizeof(buf), "pond", &r, true, NULL, one) > 0);
	CHECK(!strcmp(buf, "{\"name\":\"pond\",\"instances\":{\"pond\":{\"command\":[\"pond\"],"
			   "\"respawn\":[\"3600\",\"5\",\"3\"],\"stdout\":true,\"stderr\":true}}}"));
}

static void test_escapes(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "sh", "-c", "echo \"a\\b\"\n\t", NULL };
	char buf[512];

	CHECK(svc_set_json(buf, sizeof(buf), "x", &r, false, NULL, argv) > 0);
	CHECK(strstr(buf, "[\"sh\",\"-c\",\"echo \\\"a\\\\b\\\"\\u000a\\u0009\"]") != NULL);
}

static void test_too_long(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "a-program-with-a-long-name", NULL };
	char buf[512];
	int n;

	n = svc_set_json(buf, sizeof(buf), "x", &r, false, NULL, argv);
	CHECK(n > 0);
	/* Every size short of the whole fails, and never writes past it. */
	for (size_t cap = 0; cap <= (size_t)n; cap++) {
		char small[512];

		memset(small, 'Z', sizeof(small));
		CHECK(svc_set_json(small, cap, "x", &r, false, NULL, argv) == -1);
		CHECK(small[cap] == 'Z');
	}
	CHECK(svc_set_json(buf, (size_t)n + 1, "x", &r, false, NULL, argv) == n);
}

static void test_to_file(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "dhcp6d", "-w", "ppp1", NULL };
	char buf[512];

	CHECK(svc_set_json(buf, sizeof(buf), "dhcp6d1", &r, false, "/run/dhcp6d.log", argv) > 0);
	CHECK(strstr(buf, "\"command\":[\"/bin/sh\",\"-c\","
			  "\"f=$1; shift; exec \\\"$@\\\" >>\\\"$f\\\" 2>&1\",\"sh\","
			  "\"/run/dhcp6d.log\",\"dhcp6d\",\"-w\",\"ppp1\"]") != NULL);
	CHECK(strstr(buf, "stdout") == NULL);
}

static void test_name(void)
{
	char buf[64];

	CHECK(svc_name_json(buf, sizeof(buf), "omcid") == 16);
	CHECK(!strcmp(buf, "{\"name\":\"omcid\"}"));
	CHECK(svc_name_json(buf, 10, "omcid") == -1);
}

static void test_parse_instance(void)
{
	struct svc_instance in;
	const char *words[SVC_WORDS_MAX];
	char line[256];

	snprintf(line, sizeof(line), "-- onu-run omcid");
	CHECK(svc_parse_instance(line, "omcid", &in, words, SVC_WORDS_MAX) == 0);
	CHECK(!strcmp(in.name, "omcid") && in.respawn.threshold == 60 &&
	      in.respawn.timeout == 2 && in.respawn.retry == 0 && !in.log && !in.out);
	CHECK(!strcmp(in.argv[0], "onu-run") && !strcmp(in.argv[1], "omcid") && !in.argv[2]);

	snprintf(line, sizeof(line), "-r 3600,5,3\t-o /run/dhcp6d.log --  dhcp6d -w ppp1\n");
	CHECK(svc_parse_instance(line, "dhcp6d1", &in, words, SVC_WORDS_MAX) == 0);
	CHECK(in.respawn.threshold == 3600 && in.respawn.retry == 3 &&
	      !strcmp(in.out, "/run/dhcp6d.log") && !strcmp(in.argv[2], "ppp1") && !in.argv[3]);
	snprintf(line, sizeof(line), "-l -- udhcpd -f /run/udhcpd.conf");
	CHECK(svc_parse_instance(line, "udhcpd", &in, words, SVC_WORDS_MAX) == 0 && in.log);

	static const char *const bad[] = {
		"", "onu-run omcid", "--", "-l -o /run/x.log -- prog", "-r 60,2 -- prog",
		"-r", "-o", "-x -- prog", "-l prog -- prog",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		snprintf(line, sizeof(line), "%s", bad[i]);
		CHECK(svc_parse_instance(line, "x", &in, words, SVC_WORDS_MAX) == -1);
	}
	/* No more words than there is room for, the end included. */
	snprintf(line, sizeof(line), "-- a b c");
	CHECK(svc_parse_instance(line, "x", &in, words, 4) == 0);
	snprintf(line, sizeof(line), "-- a b c d");
	CHECK(svc_parse_instance(line, "x", &in, words, 4) == -1);
}

static void test_service(void)
{
	const char *omcid[] = { "onu-run", "omcid", NULL };
	const char *dhcp6d[] = { "dhcp6d", "-w", "ppp1", NULL };
	struct svc_instance in[2] = {
		{ .name = "dhcp6d1", .respawn = SVC_RESPAWN_DEFAULT, .out = "/run/dhcp6d.log",
		  .argv = dhcp6d },
		{ .name = "omcid", .respawn = SVC_RESPAWN_DEFAULT, .log = true, .argv = omcid },
	};
	char buf[1024];

	CHECK(svc_service_json(buf, sizeof(buf), "onu", in, 2) == (int)strlen(buf));
	static const char head[] = "{\"name\":\"onu\",\"instances\":{\"dhcp6d1\":{\"command\":"
				   "[\"/bin/sh\",";

	CHECK(!strncmp(buf, head, strlen(head)));
	CHECK(strstr(buf, "\"/run/dhcp6d.log\",\"dhcp6d\",\"-w\",\"ppp1\"]") != NULL);
	CHECK(strstr(buf, "},\"omcid\":{\"command\":[\"onu-run\",\"omcid\"],"
			  "\"respawn\":[\"60\",\"2\",\"0\"],\"stdout\":true,\"stderr\":true}}}") != NULL);
	/* With none, the service has no program left. */
	CHECK(svc_service_json(buf, sizeof(buf), "onu", in, 0) == (int)strlen(buf) &&
	      !strcmp(buf, "{\"name\":\"onu\",\"instances\":{}}"));
	CHECK(svc_service_json(buf, 20, "onu", in, 2) == -1);
}

int main(void)
{
	test_names();
	test_respawn();
	test_set();
	test_escapes();
	test_too_long();
	test_to_file();
	test_name();
	test_parse_instance();
	test_service();
	printf("test_svcmsg: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
