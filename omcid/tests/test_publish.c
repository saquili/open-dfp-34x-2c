// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of what the daemon publishes of the MIB (sys/publish.h), through a
 * writer of JSON text such as pond makes of ubus's answer.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mib/store.h"
#include "sys/ctl.h"
#include "sys/publish.h"

#include "harness.h"

#define ONU_G 256u
#define ANI_G 263u
#define EXT_VLAN 171u

/* What the instances and the end of the text are to hold. */
static const char g_rows[] =
	"\"6\":\"aa000000000000000000000000000000bb000000000000000000000000000000\"";
static const char g_onu_g[] =
	"{\"class\":256,\"instance\":0,\"attrs\":{\"1\":\"41424344\"";
static const char g_frames[] =
	"\"frames\":{\"handled\":7,\"rejected\":1,\"answered\":9}}";
static const char g_status[] =
	"{\"instances\":3,\"frames\":{\"handled\":7,\"rejected\":1,\"answered\":9}}";

static const struct omci_ctl_counters g_counters = { 7, 1, 9 };

/* An ONU-G with a logical password, an ANI-G, and a VLAN rule table of two rows. */
static void fill(struct omci_mib *mib)
{
	const uint8_t id[4] = { 'A', 'B', 'C', 'D' };
	const uint8_t password[12] = { 's', 'e', 'c', 'r', 'e', 't' };
	uint8_t row[16] = { 0 };

	CHECK_EQ(omci_mib_create(mib, ONU_G, 0), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, ONU_G, 0, 1, id, sizeof(id)), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, ONU_G, 0, 11, password,
				   sizeof(password)),
		 OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ANI_G, 0x8001), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, EXT_VLAN, 3), OMCI_OK);
	row[0] = 0xaa;
	CHECK_EQ(omci_mib_table_put(mib, EXT_VLAN, 3, 6, row, sizeof(row), 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	row[0] = 0xbb;
	CHECK_EQ(omci_mib_table_put(mib, EXT_VLAN, 3, 6, row, sizeof(row), 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
}

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

static char g_out[8192];

/* The MIB as text in g_out; the length the daemon reckoned it at in @len. */
static void publish(struct omci_mib *mib, const uint16_t *classes, size_t n,
		    size_t max, size_t *len)
{
	struct text t = { .p = g_out, .cap = sizeof(g_out) };

	t.first[0] = true;
	g_out[0] = '\0';
	t_put(&t, "{");
	*len = omci_publish_mib(mib, &g_counters, classes, n, max, &g_text_ops,
				&t);
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

static void test_the_classes_asked_for_in_their_order(struct omci_mib *mib)
{
	const uint16_t classes[] = { EXT_VLAN, ONU_G };
	size_t len;

	fill(mib);
	publish(mib, classes, 2, 0, &len);
	CHECK_EQ(len, strlen(g_out));
	CHECK(strncmp(g_out, "{\"mes\":[{\"class\":171,\"instance\":3,", 33) ==
	      0);
	CHECK(strstr(g_out, g_rows) != NULL);
	CHECK(strstr(g_out, "\"tables\":[6,10]}") != NULL);
	CHECK(strstr(g_out, g_onu_g) != NULL);
	CHECK(strstr(g_out, "\"11\"") == NULL);
	CHECK(strstr(g_out, "736563726574") == NULL);
	CHECK(strstr(g_out, "\"class\":263") == NULL);
	CHECK(strstr(g_out, "],\"truncated\":false,") != NULL);
	CHECK(strstr(g_out, ",\"logical_password_set\":true,") != NULL);
	CHECK(strstr(g_out, g_frames) != NULL);
}

static void test_every_instance_without_classes(struct omci_mib *mib)
{
	const uint8_t none[12] = { 0 };
	size_t len;

	fill(mib);
	CHECK_EQ(omci_mib_attr_set(mib, ONU_G, 0, 11, none, sizeof(none)),
		 OMCI_OK);
	publish(mib, NULL, 0, 0, &len);
	CHECK_EQ(len, strlen(g_out));
	CHECK_EQ(count(g_out, "\"class\":"), omci_mib_count(mib));
	CHECK(strstr(g_out, "\"logical_password_set\":false") != NULL);

	/* An empty MIB has an empty list. */
	CHECK_EQ(omci_mib_reset(mib), OMCI_OK);
	publish(mib, NULL, 0, 0, &len);
	CHECK_EQ(len, strlen(g_out));
	CHECK(strncmp(g_out, "{\"mes\":[],\"truncated\":false,", 28) == 0);
}

static void test_what_does_not_fit_is_left_out_whole(struct omci_mib *mib)
{
	/* The end at its longest. */
	const size_t end = strlen("],\"truncated\":false,") +
			   strlen("\"logical_password_set\":false,") +
			   strlen(g_frames);
	size_t len, second;

	fill(mib);
	publish(mib, NULL, 0, 1, &len);
	CHECK_EQ(len, strlen(g_out));
	CHECK(strncmp(g_out, "{\"mes\":[],\"truncated\":true,", 27) == 0);

	/* Room for the first instance and not the second. */
	publish(mib, NULL, 0, 0, &len);
	CHECK(strstr(g_out, ",{\"class\":") != NULL);
	second = (size_t)(strstr(g_out, ",{\"class\":") - g_out);
	publish(mib, NULL, 0, second + end, &len);
	CHECK_EQ(len, strlen(g_out));
	CHECK(len <= second + end);
	CHECK_EQ(count(g_out, "\"class\":"), 1u);
	CHECK(strstr(g_out, "\"truncated\":true") != NULL);
}

static void test_the_status(struct omci_mib *mib)
{
	struct text t = { .p = g_out, .cap = sizeof(g_out) };

	fill(mib);
	t.first[0] = true;
	t_put(&t, "{");
	omci_publish_status(mib, &g_counters, &g_text_ops, &t);
	t_put(&t, "}");
	CHECK_EQ(strcmp(g_out, g_status), 0);
}

int main(void)
{
	RUN_MIB(test_the_classes_asked_for_in_their_order);
	RUN_MIB(test_every_instance_without_classes);
	RUN_MIB(test_what_does_not_fit_is_left_out_whole);
	RUN_MIB(test_the_status);
	return OMCI_T_REPORT("test_publish");
}
