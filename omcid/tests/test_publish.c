// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of what the daemon publishes of the MIB (sys/publish.h), through a
 * writer of JSON text such as pond makes of ubus's answer.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/svcrule.h"
#include "me/vlan.h"
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

/* The rules of the services as test_the_services() has them written. */
static const char *const g_rules[] = {
	"{\"fibre\":300,\"action\":\"move\",\"host\":301,\"auto\":false}",
	"{\"fibre\":200,\"action\":\"off\",\"auto\":false}",
	"{\"fibre\":500,\"action\":\"move\",\"host\":500,\"auto\":true}",
};

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

static void t_none(void *ctx, const char *name)
{
	struct text *t = ctx;

	t_name(t, name);
	t_put(t, "null");
}

static const struct omci_publish_ops g_text_ops = {
	.open = t_open,
	.close = t_close,
	.str = t_str,
	.num = t_num,
	.flag = t_flag,
	.none = t_none,
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

/*
 * The rules of the services: a person's, and a move the programming made
 * for the VEIP's VLAN 410, which the UNI's takes.
 */
static void test_the_services(struct omci_mib *mib)
{
	static const uint8_t uni = 2u, veip = 10u;
	static const uint8_t to100[16] = { 0xf8, 0,    0,    0,	   0x80, 0xcd,
					   0,	 0,    0x40, 0x0f, 0,	 0,
					   0,	 0x08, 0x03, 0x20 };
	static const uint8_t to500[16] = { 0xf8, 0,    0,    0,	   0x80, 0xcd,
					   0,	 0,    0x40, 0x0f, 0,	 0,
					   0,	 0x08, 0x0f, 0xa0 };
	const struct omci_svc_rule person[2] = {
		{ .fibre = 300u, .host = 301u, .action = OMCI_SVC_MOVE },
		{ .fibre = 200u, .action = OMCI_SVC_OFF },
	};
	struct text t = { .p = g_out, .cap = sizeof(g_out) };

	CHECK_EQ(omci_mib_create(mib, 171u, 0x101u), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, 171u, 0x601u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 171u, 0x101u, 1u, &uni, 1u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 171u, 0x601u, 1u, &veip, 1u), OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x101u, 6u, to100, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x601u, 6u, to500, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	omci_hw_service_rules_set(person, 2u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	t.first[0] = true;
	t_put(&t, "{");
	omci_publish_services(&g_text_ops, &t);
	t_put(&t, "}");
	/* The rules in their order, a person's first, and nothing more. */
	const char *at = g_out;

	CHECK_EQ(strncmp(at, "{\"rules\":[", 10), 0);
	for (size_t i = 0; i < 3u && at != NULL; i++) {
		at = strstr(at, g_rules[i]);
		CHECK(at != NULL);
		at = at != NULL ? at + strlen(g_rules[i]) + (i < 2u) : NULL;
	}
	CHECK(at != NULL && strcmp(at, "]}") == 0);
	omci_hw_service_rules_set(person, 0u);
}

/*
 * The PPPoE login of class 148: none, then of the first instance whose
 * username is not blank, its two usernames together, a byte that is no
 * printable ASCII ending a field.
 */
static void test_the_login(struct omci_mib *mib)
{
	uint8_t blank[25] = { 0 }, user1[25] = { 0 }, user2[25] = { 0 };
	uint8_t secret[25] = { 0 }, realm[25] = { 0 };
	struct text t = { .p = g_out, .cap = sizeof(g_out) };

	fill(mib);
	t.first[0] = true;
	t_put(&t, "{");
	omci_publish_login(mib, &g_text_ops, &t);
	t_put(&t, "}");
	CHECK_EQ(strcmp(g_out, "{}"), 0);

	memcpy(user1, "abcdefghijklmnopqrstuvwxy", 25);
	memcpy(user2, "z@isp.example\x01x", 15);
	memcpy(secret, "example-secret", 14);
	memcpy(realm, "isp.example", 11);
	CHECK_EQ(omci_mib_create(mib, 148u, 1u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 148u, 1u, 2u, blank, 25u), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, 148u, 2u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 148u, 2u, 2u, user1, 25u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 148u, 2u, 5u, user2, 25u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 148u, 2u, 3u, secret, 25u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 148u, 2u, 4u, realm, 25u), OMCI_OK);
	t = (struct text){ .p = g_out, .cap = sizeof(g_out) };
	t.first[0] = true;
	t_put(&t, "{");
	omci_publish_login(mib, &g_text_ops, &t);
	t_put(&t, "}");
	CHECK(strstr(g_out,
		     "\"user\":\"abcdefghijklmnopqrstuvwxyz@isp.example\"") !=
	      NULL);
	CHECK(strstr(g_out, "\"secret\":\"example-secret\"") != NULL);
	CHECK(strstr(g_out, "\"realm\":\"isp.example\"") != NULL);
}

/* A value of @n bytes, big-endian, into an attribute. */
static void set_num(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		    uint8_t attr, uint32_t v, size_t n)
{
	uint8_t b[4];

	for (size_t i = 0; i < n; i++)
		b[i] = (uint8_t)(v >> (8u * (n - 1u - i)));
	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, b, n), OMCI_OK);
}

/* A string attribute of 25 bytes. */
static void set_text(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		     uint8_t attr, const char *text)
{
	uint8_t b[25] = { 0 };

	memcpy(b, text, strlen(text));
	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, b, sizeof(b)),
		 OMCI_OK);
}

/* A large string (class 157) of @text, in parts of 25 bytes. */
static void large(struct omci_mib *mib, uint16_t inst, const char *text)
{
	const size_t len = strlen(text);
	const unsigned int parts = (unsigned int)((len + 24u) / 25u);

	CHECK_EQ(omci_mib_create(mib, 157u, inst), OMCI_OK);
	set_num(mib, 157u, inst, 1u, parts, 1u);
	for (unsigned int p = 0; p < parts; p++) {
		uint8_t b[25] = { 0 };
		const size_t n = len - 25u * p < 25u ? len - 25u * p : 25u;

		memcpy(b, text + 25u * p, n);
		CHECK_EQ(omci_mib_attr_set(mib, 157u, inst, (uint8_t)(2u + p),
					   b, sizeof(b)),
			 OMCI_OK);
	}
}

/* An instance with its pointers and numbers: @n pairs of attribute and value. */
static void make(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		 const unsigned int (*v)[3], size_t n)
{
	CHECK_EQ(omci_mib_create(mib, cls, inst), OMCI_OK);
	for (size_t i = 0; i < n; i++)
		set_num(mib, cls, inst, (uint8_t)v[i][0], v[i][1], v[i][2]);
}

/* What a publisher writes, as JSON text in g_out. */
static const char *written(void (*pub)(const struct omci_mib *,
				       const struct omci_publish_ops *, void *),
			   struct omci_mib *mib)
{
	struct text t = { .p = g_out, .cap = sizeof(g_out) };

	t.first[0] = true;
	g_out[0] = '\0';
	t_put(&t, "{");
	pub(mib, &g_text_ops, &t);
	t_put(&t, "}");
	return g_out;
}

/* Whether @what is in g_out; says what g_out holds when it is not. */
static bool holds(const char *what)
{
	const bool found = strstr(g_out, what) != NULL;

	if (!found)
		printf("  wanted %s\n  in %s\n", what, g_out);
	return found;
}

/* The @n strings of @parts one after another, in @out. */
static const char *joined(char *out, const char *const *parts, size_t n)
{
	out[0] = '\0';
	for (size_t i = 0; i < n; i++)
		strcat(out, parts[i]);
	return out;
}

/* Whether g_out holds the strings given, one after another. */
static bool holds_parts(const char *const *parts, size_t n)
{
	char want[1024];

	return holds(joined(want, parts, n));
}

/* Whether g_out is the strings given, one after another. */
static bool is_parts(const char *const *parts, size_t n)
{
	char want[1024];

	return strcmp(g_out, joined(want, parts, n)) == 0;
}

#define PARTS(...) ((const char *const[]){ __VA_ARGS__ })
#define NPARTS(...) (sizeof(PARTS(__VA_ARGS__)) / sizeof(const char *))
#define HOLDS(...) holds_parts(PARTS(__VA_ARGS__), NPARTS(__VA_ARGS__))
#define IS(...) is_parts(PARTS(__VA_ARGS__), NPARTS(__VA_ARGS__))

/* A rule of class 171 of four words. */
static void put_rule(uint8_t row[16], uint32_t w0, uint32_t w1, uint32_t w2,
		     uint32_t w3)
{
	const uint32_t w[4] = { w0, w1, w2, w3 };

	for (int i = 0; i < 16; i++)
		row[i] = (uint8_t)(w[i / 4] >> (24 - 8 * (i % 4)));
}

/*
 * A line of SIP user data found by its POTS UNI, a second by its VoIP
 * voice CTP alone and of no account; the SIP agent's servers through large
 * strings, a network address and TCP/UDP config data; the codecs of the
 * media profile, each once; DTMF as RTP events; the VLAN of an IP host's
 * bridge port. The PPPoE login is no SIP account.
 */
static void test_the_phone(struct omci_mib *mib)
{
	static const unsigned int agent[][3] = {
		{ 1, 0x8002, 2 }, { 2, 0x8003, 2 }, { 5, 1, 2 },
		{ 8, 0x8004, 2 }, { 10, 1, 2 },
	};
	static const unsigned int user1[][3] = {
		{ 1, 1, 2 },
		{ 2, 0x8001, 2 },
		{ 4, 0x10, 2 },
		{ 10, 0x0201, 2 },
	};
	static const unsigned int user3[][3] = {
		{ 1, 1, 2 },
		{ 2, 0xffff, 2 },
		{ 4, 0xffff, 2 },
		{ 10, 0xffff, 2 },
	};
	static const unsigned int ctp1[][3] = { { 1, 1, 2 },
						{ 2, 0x0201, 2 },
						{ 3, 1, 2 } };
	static const unsigned int ctp2[][3] = { { 1, 3, 2 },
						{ 2, 0x0202, 2 },
						{ 3, 1, 2 } };
	static const unsigned int media[][3] = {
		{ 3, 8, 1 },  { 6, 0, 1 },  { 9, 18, 1 },
		{ 12, 8, 1 }, { 15, 1, 1 }, { 16, 1, 2 },
	};
	static const unsigned int udp[][3] = { { 1, 5060, 2 }, { 2, 17, 1 } };
	uint8_t vids[24] = { 0x02, 0x58 };
	uint8_t row[16];

	written(omci_publish_phone, mib);
	CHECK(IS("{\"method\":\"none\",\"lines\":[],\"server\":null,",
		 "\"codecs\":[],\"dtmf\":null,\"vlan\":null}"));
	written(omci_publish_phone_secrets, mib);
	CHECK(IS("{\"lines\":[]}"));

	CHECK_EQ(omci_mib_create(mib, 53u, 0x0201u), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, 53u, 0x0202u), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, 138u, 0u), OMCI_OK);
	set_num(mib, 138u, 0u, 4u, 1u, 1u);
	CHECK_EQ(omci_mib_create(mib, 148u, 0x10u), OMCI_OK);
	set_text(mib, 148u, 0x10u, 2u, "48221234567");
	set_text(mib, 148u, 0x10u, 3u, "example-sip-secret");
	CHECK_EQ(omci_mib_create(mib, 148u, 0x20u), OMCI_OK);
	set_text(mib, 148u, 0x20u, 2u, "abc@isp.example");
	large(mib, 0x8001u, "48221234567");
	large(mib, 0x8002u, "sip.operator.example");
	large(mib, 0x8003u, "192.0.2.10");
	large(mib, 0x8004u, "operator.example");
	CHECK_EQ(omci_mib_create(mib, 137u, 1u), OMCI_OK);
	set_num(mib, 137u, 1u, 2u, 0x8002u, 2u);
	make(mib, 136u, 1u, udp, 2);
	make(mib, 150u, 1u, agent, 5);
	make(mib, 153u, 1u, user1, 4);
	set_text(mib, 153u, 1u, 3u, "Example");
	make(mib, 153u, 3u, user3, 4);
	make(mib, 139u, 1u, ctp1, 3);
	make(mib, 139u, 2u, ctp2, 3);
	make(mib, 142u, 1u, media, 6);
	CHECK_EQ(omci_mib_create(mib, 143u, 1u), OMCI_OK);
	set_num(mib, 143u, 1u, 6u, 1u, 1u);
	CHECK_EQ(omci_mib_create(mib, 47u, 0x0301u), OMCI_OK);
	set_num(mib, 47u, 0x0301u, 3u, 4u, 1u);
	CHECK_EQ(omci_mib_create(mib, 84u, 0x0301u), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 84u, 0x0301u, 1u, vids, sizeof(vids)),
		 OMCI_OK);

	written(omci_publish_phone, mib);
	CHECK(HOLDS("{\"method\":\"omci\",\"lines\":[{\"port\":1,",
		    "\"number\":\"48221234567\",\"display\":\"Example\",",
		    "\"user\":\"48221234567\",\"password_set\":true},",
		    "{\"port\":2,\"number\":null,\"display\":null,",
		    "\"user\":null,\"password_set\":false}],"));
	CHECK(HOLDS("\"server\":{\"proxy\":\"sip.operator.example\",",
		    "\"outbound_proxy\":\"192.0.2.10\",",
		    "\"registrar\":\"sip.operator.example\",",
		    "\"domain\":\"operator.example\",\"port\":5060,",
		    "\"transport\":\"udp\"},"));
	CHECK(HOLDS("\"codecs\":[\"G.711 A-law\",\"G.711 \xc2\xb5-law\",",
		    "\"G.729\"],\"dtmf\":\"rfc2833\",",
		    "\"vlan\":{\"fibre\":600,\"host\":600}}"));
	CHECK(strstr(g_out, "example-sip-secret") == NULL);
	written(omci_publish_phone_secrets, mib);
	CHECK(HOLDS(
		"{\"lines\":[{\"port\":1,\"secret\":\"example-sip-secret\"},",
		"{\"port\":2,\"secret\":\"\"}]}"));
	written(omci_publish_login, mib);
	CHECK(HOLDS("\"user\":\"abc@isp.example\""));

	/* TR-069; DTMF in the voice, or by SIP; TCP, and another protocol; no port. */
	set_num(mib, 138u, 0u, 4u, 3u, 1u);
	set_num(mib, 142u, 1u, 15u, 0u, 1u);
	set_num(mib, 142u, 1u, 3u, 99u, 1u);
	set_num(mib, 136u, 1u, 2u, 6u, 1u);
	set_num(mib, 157u, 0x8004u, 1u, 20u, 1u);
	written(omci_publish_phone, mib);
	CHECK(HOLDS("{\"method\":\"tr069\","));
	CHECK(HOLDS("\"dtmf\":\"inband\""));
	CHECK(HOLDS("\"codecs\":[\"payload type 99\","));
	CHECK(HOLDS("\"transport\":\"tcp\""));
	CHECK(HOLDS("\"domain\":\"operator.example\""));
	set_num(mib, 142u, 1u, 15u, 1u, 1u);
	set_num(mib, 143u, 1u, 6u, 0u, 1u);
	set_num(mib, 136u, 1u, 2u, 132u, 1u);
	written(omci_publish_phone, mib);
	CHECK(HOLDS("\"dtmf\":\"info\""));
	CHECK(HOLDS("\"transport\":null"));
	CHECK_EQ(omci_mib_delete(mib, 136u, 1u), OMCI_OK);
	written(omci_publish_phone, mib);
	CHECK(HOLDS("\"port\":null,"));

	/* No method chosen, but SIP accounts sent; a socket of none. */
	set_num(mib, 138u, 0u, 4u, 0u, 1u);
	CHECK_EQ(omci_mib_create(mib, 53u, 0x0203u), OMCI_OK);
	written(omci_publish_phone, mib);
	CHECK(HOLDS("{\"method\":\"omci\","));
	CHECK(HOLDS("{\"port\":3,\"number\":null,\"display\":null,",
		    "\"user\":null,\"password_set\":false}]"));

	/*
	 * The VLAN of an IP host's rules, voice tagged at 610 on the fibre;
	 * of one that tags it twice, no VLAN of its own towards the host.
	 */
	CHECK_EQ(omci_mib_create(mib, 171u, 0x0901u), OMCI_OK);
	set_num(mib, 171u, 0x0901u, 1u, 3u, 1u);
	put_rule(row, 0xf8000000u, 0xf8000000u, 0x000f0000u,
		 (5u << 16) | (610u << 3));
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x0901u, 6u, row, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	written(omci_publish_phone, mib);
	CHECK(HOLDS("\"vlan\":{\"fibre\":610,\"host\":610}"));
	CHECK_EQ(omci_mib_delete(mib, 171u, 0x0901u), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, 171u, 0x0902u), OMCI_OK);
	set_num(mib, 171u, 0x0902u, 1u, 3u, 1u);
	put_rule(row, 0xf8000000u, 0xf8000000u, (3u << 16) | (200u << 3),
		 (5u << 16) | (410u << 3));
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x0902u, 6u, row, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	written(omci_publish_phone, mib);
	CHECK(HOLDS("\"host\":null}"));
	CHECK_EQ(omci_mib_delete(mib, 171u, 0x0902u), OMCI_OK);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
}

int main(void)
{
	RUN_MIB(test_the_classes_asked_for_in_their_order);
	RUN_MIB(test_every_instance_without_classes);
	RUN_MIB(test_what_does_not_fit_is_left_out_whole);
	RUN_MIB(test_the_status);
	RUN_MIB(test_the_services);
	RUN_MIB(test_the_login);
	RUN_MIB(test_the_phone);
	return OMCI_T_REPORT("test_publish");
}
