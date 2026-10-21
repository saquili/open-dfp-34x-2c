// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The test of a group of television of iptv.h. */

#include "iptv.h"

#include "bus.h"
#include "later.h"
#include "prov.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How often the count is read, and the seconds of a test. */
#define READ_MS 50
#define SECONDS_MAX 30u
#define SECONDS_DEFAULT 10u

/*
 * The test under way: the connection that asked, its group as text and as a
 * number, its VLAN and seconds, when it began and when the count is read
 * next, by mg_millis(), and the milliseconds to the first bytes, -1 before
 * any came.
 */
static struct {
	bool on;
	struct mg_mgr *mgr;
	unsigned long id;
	char group[INET_ADDRSTRLEN];
	uint32_t addr;
	uint32_t vlan;
	uint32_t seconds;
	uint64_t start;
	uint64_t next;
	long first_ms;
} g_test;

static void reply(struct mg_connection *c, int code, const char *error,
		  const char *detail)
{
	char body[512];
	struct prov_buf b = { .buf = body, .cap = sizeof(body) };

	pv_open(&b, '{');
	pv_key(&b, "error");
	pv_str(&b, error);
	pv_comma(&b);
	pv_key(&b, "detail");
	pv_str(&b, detail);
	pv_close(&b, '}');
	mg_http_reply(c, code,
		      "Content-Type: application/json\r\nCache-Control: no-store\r\n",
		      "%s", body);
}

/* The value of the line KEY=VALUE of @body, into @out; false without one. */
static bool body_value(struct mg_str body, const char *key, char *out,
		       size_t cap)
{
	const size_t klen = strlen(key);
	size_t at = 0;

	while (at < body.len) {
		const char *line = body.buf + at;
		const char *nl = memchr(line, '\n', body.len - at);
		size_t len = nl ? (size_t)(nl - line) : body.len - at;

		at += len + 1;
		if (len && line[len - 1] == '\r')
			len--;
		if (len <= klen || memcmp(line, key, klen) || line[klen] != '=')
			continue;
		len -= klen + 1;
		if (len >= cap)
			return false;
		memcpy(out, line + klen + 1, len);
		out[len] = '\0';
		return true;
	}
	return false;
}

/* A whole number of @text from @lo to @hi, into @out; false for another text. */
static bool number(const char *text, uint32_t lo, uint32_t hi, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (*text < '0' || *text > '9')
		return false;
	v = strtoul(text, &end, 10);
	if (*end || v < lo || v > hi)
		return false;
	*out = (uint32_t)v;
	return true;
}

/* What omcid's answer says of the test, {"running":B,"bytes":N}: false when it says none. */
static bool count_of(const char *text, int n, bool *running, double *bytes)
{
	const struct mg_str json = mg_str_n(text, n > 0 ? (size_t)n : 0);

	*running = false;
	*bytes = 0;
	if (n <= 0 || !mg_json_get_num(json, "$.bytes", bytes))
		return false;
	(void)mg_json_get_bool(json, "$.running", running);
	return true;
}

/* omcid's answer to the test's start as a reply: false when it started. */
static bool refused(struct mg_connection *c, const char *text, int n)
{
	static const struct {
		const char *error;
		int code;
		const char *detail;
	} why[] = {
		{ "invalid", 400, "GROUP: an IPv4 multicast group; VLAN: 1 to 4094" },
		{ "busy", 409, "a test runs; try again when it has ended" },
		{ "refused", 403, "the operator's policy refuses the group" },
		{ "unable", 501, "the stick cannot join or count a group here" },
		{ "no_counter", 503, "no counter of the switch is free" },
		{ "not_sent", 502, "the join could not be sent to the OLT" },
	};
	const struct mg_str json = mg_str_n(text, n > 0 ? (size_t)n : 0);
	char *error, *reason;
	bool ok = false;

	if (n < 0) {
		reply(c, 503, "no omcid", "omcid does not answer");
		return true;
	}
	if (mg_json_get_bool(json, "$.ok", &ok) && ok)
		return false;
	error = mg_json_get_str(json, "$.error");
	reason = mg_json_get_str(json, "$.reason");
	for (size_t i = 0; i < ARRAY_SIZE(why); i++) {
		if (!error || strcmp(error, why[i].error))
			continue;
		reply(c, why[i].code, error,
		      reason && *reason ? reason : why[i].detail);
		free(error);
		free(reason);
		return true;
	}
	reply(c, 502, "iptv", "omcid gave no answer it knows");
	free(error);
	free(reason);
	return true;
}

void iptv_test(struct mg_connection *c, struct mg_http_message *hm)
{
	char group[INET_ADDRSTRLEN] = "", vlan[8] = "", seconds[8] = "",
	     text[256];
	struct bus_arg args[] = {
		{ .name = "group", .values = &g_test.addr, .n = 1 },
		{ .name = "vlan", .values = &g_test.vlan, .n = 1 },
		{ .name = "seconds", .values = &g_test.seconds, .n = 1 },
	};
	struct in_addr a;
	int n;

	if (g_test.on) {
		reply(c, 409, "busy", "a test runs; try again when it has ended");
		return;
	}
	if (!body_value(hm->body, "GROUP", group, sizeof(group)) ||
	    inet_pton(AF_INET, group, &a) != 1 ||
	    (ntohl(a.s_addr) >> 28) != 0xeu) {
		reply(c, 400, "iptv",
		      "GROUP: a multicast address, 224.0.0.0 to 239.255.255.255");
		return;
	}
	g_test.addr = ntohl(a.s_addr);
	g_test.vlan = 0;
	if (body_value(hm->body, "VLAN", vlan, sizeof(vlan)) && *vlan &&
	    !number(vlan, 1, 4094, &g_test.vlan)) {
		reply(c, 400, "iptv", "VLAN: 1 to 4094, or none for untagged");
		return;
	}
	g_test.seconds = SECONDS_DEFAULT;
	if (body_value(hm->body, "SECONDS", seconds, sizeof(seconds)) &&
	    !number(seconds, 1, SECONDS_MAX, &g_test.seconds)) {
		reply(c, 400, "iptv", "SECONDS: 1 to 30");
		return;
	}
	n = bus_call("omcid", "tv_test", args, ARRAY_SIZE(args), text,
		     sizeof(text));
	if (refused(c, text, n))
		return;
	snprintf(g_test.group, sizeof(g_test.group), "%s", group);
	g_test.mgr = c->mgr;
	g_test.id = c->id;
	g_test.start = mg_millis();
	g_test.next = g_test.start + READ_MS;
	g_test.first_ms = -1;
	g_test.on = true;
}

/* The end: omcid leaves the group, and the request gets what it counted. */
static void finish(void)
{
	const uint32_t one = 1;
	const struct bus_arg stop = { .name = "stop", .values = &one, .n = 1 };
	struct mg_connection *c = later_conn(g_test.mgr, g_test.id);
	char text[128], body[384];
	struct prov_buf b = { .buf = body, .cap = sizeof(body) };
	double bytes = 0;
	bool running;
	int n;

	g_test.on = false;
	n = bus_call("omcid", "tv_test", &stop, 1, text, sizeof(text));
	if (!c)
		return;
	if (!count_of(text, n, &running, &bytes)) {
		reply(c, 502, "iptv", "omcid lost the test");
		return;
	}
	if (bytes > 0 && g_test.first_ms < 0)
		g_test.first_ms = (long)(mg_millis() - g_test.start);
	pv_open(&b, '{');
	pv_key(&b, "group");
	pv_str(&b, g_test.group);
	pv_comma(&b);
	pv_key(&b, "vlan");
	if (g_test.vlan)
		pv_num(&b, g_test.vlan);
	else
		pv_null(&b);
	pv_comma(&b);
	pv_key(&b, "seconds");
	pv_num(&b, g_test.seconds);
	pv_comma(&b);
	pv_key(&b, "bytes");
	pv_num(&b, (long long)bytes);
	pv_comma(&b);
	pv_key(&b, "rate_bps");
	pv_num(&b, (long long)(bytes * 8 / g_test.seconds));
	pv_comma(&b);
	pv_key(&b, "first_ms");
	if (g_test.first_ms >= 0)
		pv_num(&b, g_test.first_ms);
	else
		pv_null(&b);
	pv_close(&b, '}');
	mg_http_reply(c, 200,
		      "Content-Type: application/json\r\nCache-Control: no-store\r\n",
		      "%s", body);
}

void iptv_poll(void)
{
	const uint64_t now = mg_millis();
	char text[128];
	double bytes;
	bool running;
	int n;

	if (!g_test.on || now < g_test.next)
		return;
	g_test.next = now + READ_MS;
	if (now - g_test.start >= (uint64_t)g_test.seconds * 1000u) {
		finish();
		return;
	}
	if (g_test.first_ms >= 0)
		return;
	n = bus_call("omcid", "tv_test", NULL, 0, text, sizeof(text));
	if (count_of(text, n, &running, &bytes) && bytes > 0)
		g_test.first_ms = (long)(now - g_test.start);
}

bool iptv_running(void)
{
	return g_test.on;
}
