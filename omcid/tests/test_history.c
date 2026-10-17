// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the conversation with the OLT (sys/history.h): the frames kept,
 * what a reader is given from a number on, and the logical password, which
 * no frame kept holds.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "sys/history.h"
#include "sys/publish.h"

#include "harness.h"

#define ONU_G 256u
#define ONU_DATA 2u

/* The message types, with the acknowledgement bit where an answer has it. */
#define SET 0x48u
#define GET_RESPONSE 0x29u
#define GET 0x49u
#define AVC 0x11u
#define UPLOAD_NEXT_RESPONSE 0x2eu

/* The bits of attributes 1, 10 and 11 in a mask. */
#define ATTR_1 0x8000u
#define ATTR_10 0x0040u
#define ATTR_11 0x0020u

static struct omci_history g_h;

/* A writer of JSON text: a comma before all but the first of each level. */
struct text {
	char *p;
	size_t len;
	size_t cap;
	bool first[8];
	unsigned int depth;
};

static void t_put(struct text *t, const char *s)
{
	const size_t n = strlen(s);

	if (t->len + n >= t->cap)
		return;
	memcpy(t->p + t->len, s, n + 1);
	t->len += n;
}

static void t_name(struct text *t, const char *name)
{
	if (!t->first[t->depth])
		t_put(t, ",");
	t->first[t->depth] = false;
	if (name == NULL)
		return;
	t_put(t, "\"");
	t_put(t, name);
	t_put(t, "\":");
}

static void t_open(void *ctx, const char *name, bool array)
{
	struct text *t = ctx;

	t_name(t, name);
	t_put(t, array ? "[" : "{");
	t->first[++t->depth] = true;
}

static void t_close(void *ctx, bool array)
{
	struct text *t = ctx;

	t->depth--;
	t_put(t, array ? "]" : "}");
}

static void t_str(void *ctx, const char *name, const char *value)
{
	struct text *t = ctx;

	t_name(t, name);
	t_put(t, "\"");
	t_put(t, value);
	t_put(t, "\"");
}

static void t_num(void *ctx, const char *name, uint32_t value)
{
	struct text *t = ctx;
	char v[16];

	t_name(t, name);
	snprintf(v, sizeof(v), "%u", value);
	t_put(t, v);
}

static void t_flag(void *ctx, const char *name, bool value)
{
	struct text *t = ctx;

	t_name(t, name);
	t_put(t, value ? "true" : "false");
}

static const struct omci_publish_ops g_text_ops = {
	.open = t_open,
	.close = t_close,
	.str = t_str,
	.num = t_num,
	.flag = t_flag,
};

static char g_out[64 * 1024];

/* The frames from @since, at most @max, as text in g_out. */
static void publish(uint32_t since, uint32_t max)
{
	struct text t = { .p = g_out, .cap = sizeof(g_out) };

	t.first[0] = true;
	g_out[0] = '\0';
	t_put(&t, "{");
	omci_publish_frames(&g_h, since, max, &g_text_ops, &t);
	t_put(&t, "}");
}

static size_t count(const char *text, const char *what)
{
	size_t n = 0;

	for (const char *p = strstr(text, what); p != NULL;
	     p = strstr(p + 1, what))
		n++;
	return n;
}

/* A frame of @type to @class: its TCI and contents left to the caller. */
static void frame(uint8_t f[OMCI_FRAME_LEN], uint8_t type, uint16_t class_id)
{
	memset(f, 0, OMCI_FRAME_LEN);
	omci_be16_store(&f[0], 0x1234u);
	f[2] = type;
	f[3] = 0x0au;
	omci_be16_store(&f[4], class_id);
	f[41] = 0x28u;
}

/* Fill @n bytes of a frame from @at with 0xee, a stand-in for a password. */
static void secret(uint8_t f[OMCI_FRAME_LEN], size_t at, size_t n)
{
	memset(&f[at], 0xee, n);
}

/* Keep @f and say whether the copy kept was masked, and how many 0xee are left in it. */
static bool keep(const uint8_t f[OMCI_FRAME_LEN], size_t *left)
{
	const struct omci_history_rec *r;

	omci_history_add(&g_h, 100u, 7u, OMCI_CTL_TAP_RX_DISPATCH, f[2] & 0x1fu,
			 f);
	r = &g_h.rec[(g_h.head - 1u) % OMCI_HISTORY_LEN];
	*left = 0;
	for (size_t i = 0; i < OMCI_FRAME_LEN; i++)
		*left += r->frame[i] == 0xeeu;
	return r->masked;
}

static void test_a_frame_is_kept_with_its_time_and_fate(void)
{
	static const char digit[] = "0123456789abcdef";
	char hex[2u * OMCI_FRAME_LEN + 16u] = "\"frame\":\"";
	uint8_t f[OMCI_FRAME_LEN];
	size_t at = strlen(hex);

	memset(&g_h, 0, sizeof(g_h));
	frame(f, GET, ONU_DATA);
	omci_history_add(&g_h, 1792270000u, 123u, OMCI_CTL_TAP_TX, 9u, f);
	publish(0, 0);
	CHECK(strstr(g_out,
		     "{\"head\":1,\"first\":0,\"frames\":[{\"seq\":0,") ==
	      g_out);
	CHECK(strstr(g_out, "\"sec\":1792270000,\"ms\":123,\"verdict\":5,") !=
	      NULL);
	CHECK(strstr(g_out, "\"type\":9,\"masked\":false,") != NULL);

	/* The frame as it passed, in hexadecimal. */
	for (size_t i = 0; i < OMCI_FRAME_LEN; i++) {
		hex[at++] = digit[f[i] >> 4];
		hex[at++] = digit[f[i] & 15u];
	}
	memcpy(&hex[at], "\"}]}", 5);
	CHECK(strstr(g_out, hex) != NULL);
}

static void test_a_reader_is_given_the_frames_from_its_number_on(void)
{
	uint8_t f[OMCI_FRAME_LEN];

	memset(&g_h, 0, sizeof(g_h));
	frame(f, GET, ONU_DATA);
	for (unsigned int i = 0; i < OMCI_HISTORY_LEN + 5u; i++)
		omci_history_add(&g_h, i, 0, OMCI_CTL_TAP_RX_DISPATCH, 9u, f);

	/* The oldest five are gone: a reader that asks for them gets the oldest kept. */
	publish(2, 3);
	CHECK(strstr(g_out, "\"head\":1029,\"first\":5,") != NULL);
	CHECK_EQ(count(g_out, "\"seq\":"), 3u);
	CHECK(strstr(g_out, "{\"seq\":5,\"sec\":5,") != NULL);
	CHECK(strstr(g_out, "{\"seq\":7,\"sec\":7,") != NULL);

	/* From its number on, at most a page at a time. */
	publish(600, 0);
	CHECK_EQ(count(g_out, "\"seq\":"), OMCI_HISTORY_PAGE);
	CHECK(strstr(g_out, "{\"seq\":600,") != NULL);
	publish(600, OMCI_HISTORY_PAGE + 1u);
	CHECK_EQ(count(g_out, "\"seq\":"), OMCI_HISTORY_PAGE);

	/* Up to date: nothing more. */
	publish(1029, 0);
	CHECK(strstr(g_out, "\"frames\":[]") != NULL);

	/* A number past the head is of a daemon before this one: from the oldest. */
	publish(5000, 1);
	CHECK(strstr(g_out, "{\"seq\":5,") != NULL);
}

static void test_the_logical_password_is_never_kept(void)
{
	uint8_t f[OMCI_FRAME_LEN];
	size_t left;

	memset(&g_h, 0, sizeof(g_h));

	/* A Set of attribute 1 and the password: the password after its four bytes. */
	frame(f, SET, ONU_G);
	omci_be16_store(&f[8], ATTR_1 | ATTR_11);
	secret(f, 10 + 4, 12);
	CHECK(keep(f, &left));
	CHECK_EQ(left, 0u);

	/* A Set without it is kept as it came. */
	omci_be16_store(&f[8], ATTR_1);
	CHECK(!keep(f, &left));
	CHECK_EQ(left, 12u);

	/* The answer to a Get of it, after the result byte. */
	frame(f, GET_RESPONSE, ONU_G);
	omci_be16_store(&f[9], ATTR_11);
	secret(f, 11, 12);
	CHECK(keep(f, &left));
	CHECK_EQ(left, 0u);

	/* An AVC of it. */
	frame(f, AVC, ONU_G);
	omci_be16_store(&f[8], ATTR_11);
	secret(f, 10, 12);
	CHECK(keep(f, &left));
	CHECK_EQ(left, 0u);

	/* An ONU-G in the MIB upload, after the class, instance and mask it carries. */
	frame(f, UPLOAD_NEXT_RESPONSE, ONU_DATA);
	omci_be16_store(&f[8], ONU_G);
	omci_be16_store(&f[12], ATTR_11);
	secret(f, 14, 12);
	CHECK(keep(f, &left));
	CHECK_EQ(left, 0u);

	/* Another class in the upload is kept as it came. */
	omci_be16_store(&f[8], 257u);
	CHECK(!keep(f, &left));
	CHECK_EQ(left, 12u);

	/* So are another class's Set, and a Get, which carries no values. */
	frame(f, SET, 257u);
	omci_be16_store(&f[8], ATTR_11);
	secret(f, 10, 12);
	CHECK(!keep(f, &left));
	frame(f, GET, ONU_G);
	omci_be16_store(&f[8], ATTR_11);
	secret(f, 10, 12);
	CHECK(!keep(f, &left));
	CHECK_EQ(left, 12u);

	/* A password that the values before it push past the contents is not there. */
	frame(f, SET, ONU_G);
	omci_be16_store(&f[8], 0x4000u | ATTR_10 | ATTR_11);
	CHECK(!keep(f, &left));

	/* One that runs past the contents is zeroed as far as they go. */
	omci_be16_store(&f[8], ATTR_10 | ATTR_11);
	secret(f, 10 + 24, 6);
	CHECK(keep(f, &left));
	CHECK_EQ(left, 0u);
	CHECK_EQ(g_h.rec[(g_h.head - 1u) % OMCI_HISTORY_LEN].frame[40], 0u);

	/* What a reader is told of it. */
	publish(g_h.head - 1u, 1);
	CHECK(strstr(g_out, "\"masked\":true") != NULL);
}

int main(void)
{
	RUN(test_a_frame_is_kept_with_its_time_and_fate);
	RUN(test_a_reader_is_given_the_frames_from_its_number_on);
	RUN(test_the_logical_password_is_never_kept);
	return OMCI_T_REPORT("test_history");
}
