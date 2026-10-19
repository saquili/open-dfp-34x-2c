// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The changes on trial (ADR 0011), whichever program made them: the
 * settings, the setting of the SFP lane and the management pair. onu-change
 * keeps them all, the management pair only when the confirmation came to
 * its new address, which this daemon gives it as the address the request
 * came to, or takes them all back. The panel confirms them as soon as it
 * reaches the stick again. The daemon then listens on the management
 * address in use alone. onu-act runs onu-change (bus.h).
 */

#include "bus.h"
#include "changes.h"
#include "later.h"
#include "mgmt.h"
#include "prov.h"

#include <stdlib.h>
#include <string.h>

/* Whether @body is "@key=1", with a line end or without. */
static bool flag(struct mg_str body, const char *key)
{
	char want[16];

	snprintf(want, sizeof(want), "%s=1", key);
	return mg_strcmp(body, mg_str(want)) == 0 ||
	       (body.len == strlen(want) + 1 && !strncmp(body.buf, want, body.len - 1) &&
		(body.buf[body.len - 1] == '\n' || body.buf[body.len - 1] == '\r'));
}

static void reply(struct mg_connection *c, int code, const char *detail)
{
	if (code == 200)
		mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
	else
		mg_http_reply(c, code, "Content-Type: application/json\r\n",
			      "{\"error\":\"changes\",\"detail\":\"%s\"}", detail);
}

/*
 * The confirmation or revert under way, answered when onu-change has done it
 * (later.h): one at a time. The daemon settles its addresses either way.
 */
static struct {
	bool busy;
	bool confirm;
	struct mg_mgr *mgr;
	unsigned long id;
	char current[24];
	char pending[24];
} g_op;

static void done(void *ctx, int code, const char *output)
{
	struct mg_connection *c = later_conn(g_op.mgr, g_op.id);

	(void)ctx;
	(void)output;
	g_op.busy = false;
	mgmt_settle(g_op.mgr, g_op.current, g_op.pending);
	if (!c)
		return;
	if (code == 3 && g_op.confirm)
		reply(c, 409, "nothing was on trial: the changes went back already");
	else if (code)
		reply(c, 500, g_op.confirm ? "not every change was kept" :
					     "not every change was taken back");
	else
		reply(c, 200, NULL);
}

void changes_set(struct mg_connection *c, struct mg_http_message *hm)
{
	static const char *const names[] = { "address" };
	const char *values[1] = { NULL };
	char here[48];
	const char *method;
	int rc;

	if (g_op.busy) {
		reply(c, 409, "a confirmation or a revert is under way");
		return;
	}
	if (flag(hm->body, "CONFIRM")) {
		mg_snprintf(here, sizeof(here), "%M", mg_print_ip, &c->loc);
		/* An IPv6 address is the bare address. */
		values[0] = here + (here[0] == '[');
		here[strcspn(here, "]")] = '\0';
		method = "changes_confirm";
	} else if (flag(hm->body, "REVERT")) {
		method = "changes_revert";
	} else {
		reply(c, 400, "CONFIRM=1 or REVERT=1");
		return;
	}
	mgmt_addresses(g_op.current, g_op.pending, sizeof(g_op.current));
	g_op.confirm = values[0] != NULL;
	g_op.mgr = c->mgr;
	g_op.id = c->id;
	rc = bus_act_begin(method, names, values, 1, NULL, done, NULL);
	if (rc) {
		reply(c, 500, g_op.confirm ? "not every change was kept" :
					     "not every change was taken back");
		return;
	}
	g_op.busy = true;
}
