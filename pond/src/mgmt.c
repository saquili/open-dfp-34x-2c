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
#include "mgmt.h"
#include "prov.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

static void listen_on(struct mg_mgr *mgr, const char *cidr, unsigned int port,
		      mg_event_handler_t fn)
{
	char url[64];
	size_t n = strcspn(cidr, "/");

	snprintf(url, sizeof(url), "http://%.*s:%u", (int)n, cidr, port);
	if (mg_http_listen(mgr, url, fn, NULL))
		fprintf(stderr, "pond: listening on %s\n", url);
	else
		fprintf(stderr, "pond: cannot listen on %s\n", url);
}

static void reply(struct mg_connection *c, int code, const char *error, const char *detail)
{
	if (code == 200)
		mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
	else
		mg_http_reply(c, code, "Content-Type: application/json\r\n",
			      "{\"error\":\"%s\",\"detail\":\"%s\"}", error, detail);
}

void mgmt_set(struct mg_connection *c, struct mg_http_message *hm, mg_event_handler_t fn)
{
	char vlan[8], address[24], flag[4], text[512], pending[24], current[24], out[256];
	unsigned int port = mg_ntohs(c->loc.port);

	if (run("mgmt_status", text, sizeof(text))) {
		reply(c, 500, "mgmt", "the management pair cannot be read");
		return;
	}
	if (!status_value(text, "ADDRESS", current, sizeof(current)))
		current[0] = '\0';
	if (!status_value(text, "PENDING_ADDRESS", pending, sizeof(pending)))
		pending[0] = '\0';

	if (line_value(hm->body, "VLAN", vlan, sizeof(vlan)) &&
	    line_value(hm->body, "ADDRESS", address, sizeof(address))) {
		static const char *const names[] = { "vlan", "address" };
		const char *const values[] = { vlan, address };
		int rc = bus_act("mgmt_add", names, values, 2, NULL, out, sizeof(out));
		bool bad = rc == 2 || rc == -EINVAL;

		if (rc) {
			reply(c, bad ? 400 : 409, "mgmt",
			      bad ? "not a VLAN and an address with its prefix" :
				    "the new pair was not added: a change waits, or the VLAN is in use");
			return;
		}
		if (strcmp(address, current))
			listen_on(c->mgr, address, port, fn);
		reply(c, 200, NULL, NULL);
	} else if (line_value(hm->body, "COMMIT", flag, sizeof(flag))) {
		struct mg_addr a;

		if (!*pending) {
			reply(c, 409, "mgmt", "no change waits");
			return;
		}
		/* Only a panel opened at the new address shows that it answers. */
		if (!parse_ip(pending, &a) || c->loc.is_ip6 || memcmp(c->loc.ip, a.ip, 4)) {
			reply(c, 403, "mgmt", "confirm the change from a panel opened at the new address");
			return;
		}
		if (run("mgmt_commit", out, sizeof(out))) {
			reply(c, 500, "mgmt", "the change was not committed");
			return;
		}
		if (strcmp(current, pending) && *current)
			unlisten(c->mgr, current);
		reply(c, 200, NULL, NULL);
	} else if (line_value(hm->body, "CANCEL", flag, sizeof(flag))) {
		run("mgmt_cancel", out, sizeof(out));
		if (*pending && strcmp(current, pending))
			unlisten(c->mgr, pending);
		reply(c, 200, NULL, NULL);
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

void mgmt_resume(struct mg_mgr *mgr, const char *port, mg_event_handler_t fn)
{
	char text[512], pending[24];

	if (run("mgmt_status", text, sizeof(text)) ||
	    !status_value(text, "PENDING_ADDRESS", pending, sizeof(pending)))
		return;
	listen_on(mgr, pending, (unsigned int)atoi(port), fn);
}
