// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A change of the management pair, which cannot lock anyone out: onu-mgmt
 * adds the new VLAN and address beside the old ones, and this daemon listens
 * on the new address as well; a panel opened there confirms the change,
 * which shows that the new pair answers, and only then does the old one go.
 * onu-act runs onu-mgmt (bus.h).
 */

#include "bus.h"
#include "later.h"
#include "mgmt.h"
#include "prov.h"
#include "tls.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* onu-mgmt's @method through onu-act, with no values. */
static int run(const char *method, char *out, size_t cap)
{
	return bus_act(method, NULL, NULL, 0, NULL, out, cap);
}

static bool line_value(struct mg_str body, const char *key, char *out, size_t cap)
{
	size_t klen = strlen(key), i = 0;

	while (i < body.len) {
		size_t end = i;

		while (end < body.len && body.buf[end] != '\n' && body.buf[end] != '\r')
			end++;
		if (end - i > klen && !strncmp(body.buf + i, key, klen) &&
		    body.buf[i + klen] == '=') {
			size_t len = end - i - klen - 1;

			if (len >= cap)
				return false;
			memcpy(out, body.buf + i + klen + 1, len);
			out[len] = '\0';
			return true;
		}
		i = end + 1;
	}
	return false;
}

static bool status_value(const char *text, const char *key, char *out, size_t cap)
{
	return line_value(mg_str(text), key, out, cap);
}

/* The address of @cidr without its prefix, as mongoose holds addresses. */
static bool parse_ip(const char *cidr, struct mg_addr *a)
{
	char ip[32];
	size_t n = strcspn(cidr, "/");

	if (n >= sizeof(ip))
		return false;
	memcpy(ip, cidr, n);
	ip[n] = '\0';
	memset(a, 0, sizeof(*a));
	return mg_aton(mg_str(ip), a) && !a->is_ip6;
}

/* Stop listening on @cidr's address; the connections made to it stay. */
static void unlisten(struct mg_mgr *mgr, const char *cidr)
{
	struct mg_connection *l;
	struct mg_addr a;

	if (!parse_ip(cidr, &a))
		return;
	for (l = mgr->conns; l; l = l->next)
		if (l->is_listening && !memcmp(l->loc.ip, a.ip, 4)) {
			l->is_closing = 1;
			fprintf(stderr, "pond: no longer listening on %s\n", cidr);
		}
}

static void reply(struct mg_connection *c, int code, const char *error, const char *detail)
{
	if (code == 200)
		mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
	else
		mg_http_reply(c, code, "Content-Type: application/json\r\n",
			      "{\"error\":\"%s\",\"detail\":\"%s\"}", error, detail);
}

/*
 * The change under way, answered when onu-mgmt has done it (later.h): one at
 * a time. What follows it, listening on the new address or no longer on an
 * old one, is done whether or not the request is still there to answer.
 */
static struct {
	const char *method;
	struct mg_mgr *mgr;
	unsigned long id;
	mg_event_handler_t fn;
	char address[24];
	char current[24];
	char pending[24];
} g_op;

static void done(void *ctx, int code, const char *output)
{
	struct mg_connection *c = later_conn(g_op.mgr, g_op.id);
	bool bad = code == 2 || code == -EINVAL;
	int status = 200;
	const char *why = NULL;

	(void)ctx;
	(void)output;
	if (!strcmp(g_op.method, "mgmt_add")) {
		if (code) {
			status = bad ? 400 : 409;
			why = bad ? "not a VLAN and an address with its prefix" :
				    "the new pair was not added: a change waits, or the VLAN is in use";
		} else if (strcmp(g_op.address, g_op.current)) {
			tls_listen(g_op.mgr, g_op.address, g_op.fn);
		}
	} else if (!strcmp(g_op.method, "mgmt_commit")) {
		if (code) {
			status = 500;
			why = "the change was not committed";
		} else if (strcmp(g_op.current, g_op.pending) && *g_op.current) {
			unlisten(g_op.mgr, g_op.current);
		}
	} else if (*g_op.pending && strcmp(g_op.current, g_op.pending)) {
		unlisten(g_op.mgr, g_op.pending);
	}
	g_op.method = NULL;
	if (c)
		reply(c, status, "mgmt", why);
}

/* Have onu-mgmt do @method, with @n values, and answer @c when it is done. */
static void begin(struct mg_connection *c, const char *method,
		  const char *const *names, const char *const *values, size_t n)
{
	int rc;

	g_op.method = method;
	g_op.mgr = c->mgr;
	g_op.id = c->id;
	rc = bus_act_begin(method, names, values, n, NULL, done, NULL);
	if (rc) {
		g_op.method = NULL;
		reply(c, rc == -EINVAL ? 400 : rc == -EBUSY ? 409 : 500, "mgmt",
		      rc == -EINVAL ? "not a VLAN and an address with its prefix" :
		      rc == -EBUSY ? "a change of the pair is under way" :
				     "onu-mgmt does not run");
	}
}

void mgmt_set(struct mg_connection *c, struct mg_http_message *hm, mg_event_handler_t fn)
{
	static const char *const names[] = { "vlan", "address" };
	char vlan[8], address[24], flag[4], text[512];
	const char *const values[] = { vlan, address };
	struct mg_addr a;

	if (g_op.method) {
		reply(c, 409, "mgmt", "a change of the pair is under way");
		return;
	}
	if (run("mgmt_status", text, sizeof(text))) {
		reply(c, 500, "mgmt", "the management pair cannot be read");
		return;
	}
	if (!status_value(text, "ADDRESS", g_op.current, sizeof(g_op.current)))
		g_op.current[0] = '\0';
	if (!status_value(text, "PENDING_ADDRESS", g_op.pending, sizeof(g_op.pending)))
		g_op.pending[0] = '\0';
	g_op.fn = fn;

	if (line_value(hm->body, "VLAN", vlan, sizeof(vlan)) &&
	    line_value(hm->body, "ADDRESS", address, sizeof(address))) {
		snprintf(g_op.address, sizeof(g_op.address), "%s", address);
		begin(c, "mgmt_add", names, values, 2);
	} else if (line_value(hm->body, "COMMIT", flag, sizeof(flag))) {
		if (!*g_op.pending) {
			reply(c, 409, "mgmt", "no change waits");
			return;
		}
		/* Only a panel opened at the new address shows that it answers. */
		if (!parse_ip(g_op.pending, &a) || c->loc.is_ip6 ||
		    memcmp(c->loc.ip, a.ip, 4)) {
			reply(c, 403, "mgmt", "confirm the change from a panel opened at the new address");
			return;
		}
		begin(c, "mgmt_commit", NULL, NULL, 0);
	} else if (line_value(hm->body, "CANCEL", flag, sizeof(flag))) {
		begin(c, "mgmt_cancel", NULL, NULL, 0);
	} else {
		reply(c, 400, "mgmt", "VLAN and ADDRESS, COMMIT=1 or CANCEL=1");
	}
}

bool mgmt_addresses(char *current, char *pending, size_t cap)
{
	char text[512];

	*current = *pending = '\0';
	if (run("mgmt_status", text, sizeof(text)))
		return false;
	if (!status_value(text, "ADDRESS", current, cap))
		*current = '\0';
	if (!status_value(text, "PENDING_ADDRESS", pending, cap))
		*pending = '\0';
	return true;
}

void mgmt_settle(struct mg_mgr *mgr, const char *current, const char *pending)
{
	char now[24], waits[24];

	if (!*pending || !strcmp(current, pending) ||
	    !mgmt_addresses(now, waits, sizeof(now)) || *waits)
		return;
	if (!strcmp(now, pending))
		unlisten(mgr, current);
	else if (!strcmp(now, current))
		unlisten(mgr, pending);
}

void mgmt_resume(struct mg_mgr *mgr, mg_event_handler_t fn)
{
	char text[512], pending[24];

	if (run("mgmt_status", text, sizeof(text)) ||
	    !status_value(text, "PENDING_ADDRESS", pending, sizeof(pending)))
		return;
	tls_listen(mgr, pending, fn);
}
