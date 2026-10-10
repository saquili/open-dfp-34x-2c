// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of onu-svc's messages to procd (svcmsg.h). */

#include <stdarg.h>
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

/*
 * The tests' writer: the message as JSON text, for its comparison with what
 * procd takes. A message is the root table's values, which text_end() closes.
 */
struct text {
	char buf[1024];
	size_t len;
	int depth;
	bool array[8];
	bool any[8];
	int room;
	int calls;
};

static struct text g_text;

static void put(struct text *t, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	t->len += (size_t)vsnprintf(t->buf + t->len, sizeof(t->buf) - t->len, fmt, ap);
	va_end(ap);
	if (t->len >= sizeof(t->buf))
		t->len = sizeof(t->buf) - 1;
}

static void put_string(struct text *t, const char *s)
{
	put(t, "\"");
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			put(t, "\\%c", *p);
		else if (*p < 0x20 || *p == 0x7f)
			put(t, "\\u%04x", *p);
		else
			put(t, "%c", *p);
	}
	put(t, "\"");
}

/* A value's place: room for one more call, a comma, and a name in a table. */
static int place(struct text *t, const char *name)
{
	t->calls++;
	if (!t->room)
		return -1;
	if (t->room > 0)
		t->room--;
	if (t->any[t->depth])
		put(t, ",");
	t->any[t->depth] = true;
	if (!t->array[t->depth]) {
		put_string(t, name);
		put(t, ":");
	}
	return 0;
}

static int text_open(void *ctx, const char *name, bool array)
{
	struct text *t = ctx;

	if (place(t, name))
		return -1;
	put(t, array ? "[" : "{");
	t->depth++;
	t->array[t->depth] = array;
	t->any[t->depth] = false;
	return 0;
}

static int text_close(void *ctx)
{
	struct text *t = ctx;

	t->calls++;
	if (!t->room)
		return -1;
	if (t->room > 0)
		t->room--;
	put(t, t->array[t->depth] ? "]" : "}");
	t->depth--;
	return 0;
}

static int text_string(void *ctx, const char *name, const char *value)
{
	struct text *t = ctx;

	if (place(t, name))
		return -1;
	put_string(t, value);
	return 0;
}

static int text_boolean(void *ctx, const char *name, bool value)
{
	struct text *t = ctx;

	if (place(t, name))
		return -1;
	put(t, value ? "true" : "false");
	return 0;
}

static const struct svc_writer g_writer = {
	.open = text_open, .close = text_close, .string = text_string,
	.boolean = text_boolean, .ctx = &g_text,
};

/* A message begins, with room for @room calls, or for any when -1. */
static const struct svc_writer *text_begin(int room)
{
	memset(&g_text, 0, sizeof(g_text));
	g_text.room = room;
	put(&g_text, "{");
	return &g_writer;
}

static const char *text_end(void)
{
	put(&g_text, "}");
	return g_text.buf;
}

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

	CHECK(svc_set_msg(text_begin(-1), "netifd", &r, false, NULL, NULL, argv) == 0);
	CHECK(!strcmp(text_end(), "{\"name\":\"netifd\",\"instances\":{\"netifd\":{\"command\":"
			   "[\"/usr/sbin/netifd\",\"-c\",\"/run/config\"],"
			   "\"respawn\":[\"60\",\"2\",\"0\"]}}}"));

	const char *one[] = { "pond", NULL };
	r = (struct svc_respawn){ 3600, 5, 3 };
	CHECK(svc_set_msg(text_begin(-1), "pond", &r, true, NULL, NULL, one) == 0);
	CHECK(!strcmp(text_end(), "{\"name\":\"pond\",\"instances\":{\"pond\":{\"command\":[\"pond\"],"
			   "\"respawn\":[\"3600\",\"5\",\"3\"],\"stdout\":true,\"stderr\":true}}}"));
}

static void test_escapes(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "sh", "-c", "echo \"a\\b\"\n\t", NULL };

	/* The words reach the writer as they are. */
	CHECK(svc_set_msg(text_begin(-1), "x", &r, false, NULL, NULL, argv) == 0);
	CHECK(strstr(text_end(), "[\"sh\",\"-c\",\"echo \\\"a\\\\b\\\"\\u000a\\u0009\"]") != NULL);
}

static void test_no_room(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "a-program", "-x", NULL };
	int calls;

	CHECK(svc_set_msg(text_begin(-1), "x", &r, true, NULL, "pond", argv) == 0);
	calls = g_text.calls;
	CHECK(calls > 10);
	/* A writer without room for one of its calls gives the message up there. */
	for (int room = 0; room < calls; room++) {
		CHECK(svc_set_msg(text_begin(room), "x", &r, true, NULL, "pond", argv) == -1);
		CHECK(g_text.calls == room + 1);
	}
	CHECK(svc_set_msg(text_begin(calls), "x", &r, true, NULL, "pond", argv) == 0);
}

static void test_to_file(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "dhcp6d", "-w", "ppp1", NULL };
	const char *buf;

	CHECK(svc_set_msg(text_begin(-1), "dhcp6d1", &r, false, "/run/dhcp6d.log", NULL, argv) == 0);
	buf = text_end();
	CHECK(strstr(buf, "\"command\":[\"/bin/sh\",\"-c\","
			  "\"f=$1; shift; exec \\\"$@\\\" >>\\\"$f\\\" 2>&1\",\"sh\","
			  "\"/run/dhcp6d.log\",\"dhcp6d\",\"-w\",\"ppp1\"]") != NULL);
	CHECK(strstr(buf, "stdout") == NULL);
}

static void test_user(void)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *argv[] = { "/usr/lib/onu/onu-run", "pond", NULL };
	struct svc_instance in;
	const char *words[SVC_WORDS_MAX];
	char line[128];

	CHECK(svc_set_msg(text_begin(-1), "pond", &r, false, NULL, "pond", argv) == 0);
	CHECK(strstr(text_end(), "\"respawn\":[\"60\",\"2\",\"0\"],\"user\":\"pond\"}") != NULL);
	CHECK(svc_set_msg(text_begin(-1), "pond", &r, false, NULL, NULL, argv) == 0);
	CHECK(strstr(text_end(), "user") == NULL);

	snprintf(line, sizeof(line), "-u pond -- /usr/lib/onu/onu-run pond");
	CHECK(svc_parse_instance(line, "pond", &in, words, SVC_WORDS_MAX) == 0);
	CHECK(in.user && !strcmp(in.user, "pond") && !strcmp(in.argv[1], "pond"));
	snprintf(line, sizeof(line), "-u -- onu-run pond");
	CHECK(svc_parse_instance(line, "pond", &in, words, SVC_WORDS_MAX) == -1);
	snprintf(line, sizeof(line), "-u po/nd -- onu-run pond");
	CHECK(svc_parse_instance(line, "pond", &in, words, SVC_WORDS_MAX) == -1);
}

static void test_name(void)
{
	CHECK(svc_name_msg(text_begin(-1), "omcid") == 0);
	CHECK(!strcmp(text_end(), "{\"name\":\"omcid\"}"));
	CHECK(svc_name_msg(text_begin(0), "omcid") == -1);
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
	const char *const none[] = { NULL };
	struct svc_instance empty = { .name = "x", .respawn = SVC_RESPAWN_DEFAULT, .argv = none };
	const char *buf;

	CHECK(svc_service_msg(text_begin(-1), "onu", in, 2) == 0);
	buf = text_end();
	static const char head[] = "{\"name\":\"onu\",\"instances\":{\"dhcp6d1\":{\"command\":"
				   "[\"/bin/sh\",";

	CHECK(!strncmp(buf, head, strlen(head)));
	CHECK(strstr(buf, "\"/run/dhcp6d.log\",\"dhcp6d\",\"-w\",\"ppp1\"]") != NULL);
	CHECK(strstr(buf, "},\"omcid\":{\"command\":[\"onu-run\",\"omcid\"],"
			  "\"respawn\":[\"60\",\"2\",\"0\"],\"stdout\":true,\"stderr\":true}}}") != NULL);
	/* With none, the service has no program left. */
	CHECK(svc_service_msg(text_begin(-1), "onu", in, 0) == 0 &&
	      !strcmp(text_end(), "{\"name\":\"onu\",\"instances\":{}}"));
	/* An instance without a program is refused before anything is written. */
	CHECK(svc_service_msg(text_begin(-1), "onu", &empty, 1) == -1 && !g_text.calls);
}

int main(void)
{
	test_names();
	test_respawn();
	test_set();
	test_escapes();
	test_no_room();
	test_to_file();
	test_user();
	test_name();
	test_parse_instance();
	test_service();
	printf("test_svcmsg: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
