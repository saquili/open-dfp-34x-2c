// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The settings of the stick's data path: KEY=value lines in /run/onu.conf,
 * which onu-apply applies, and the account of each of the stick's PPPoE
 * sessions, 1 to 7, in /run/onu-pppoeN, as "user secret", readable by root
 * only. What the settings mean, their defaults and the rules that bind them
 * together, is onu-config's (ADR 0014), and so are the files: pond reads
 * neither, and has onu-act run onu-config for it (bus.h). What came of them
 * is onu-apply's, and pond reads no file of it either: "onu-apply state"
 * gives it as lines "name=value" (settings_state).
 *
 * A read gives the settings as "onu-config current" gives them, each session
 * with its user and whether a secret is set (never the secret), and what
 * onu-apply and dhcp6d brought up for it, what it hands down among it, the
 * address handed on, and what became of the settings in the configuration
 * store: "loaded" at boot, "defaults" when the store held none, "saved" or
 * "failed" after a change, "none" without a store, and the changes on
 * trial, of the settings, the SFP lane or the management pair, with when
 * each goes back. A write takes KEY=value lines, has
 * "onu-config change" judge them over the settings in force and write
 * them, and starts "onu-apply try" without waiting for it: the change is on
 * trial, and goes back unless it is confirmed (ADR 0014, changes.c). Until
 * onu-apply ends, a write is refused.
 */

#define _GNU_SOURCE

#include "bus.h"
#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SESSIONS 7
#define TEXT_MAX 8192
#define VALUE_MAX 64
#define USER_MAX 63
#define SECRET_MAX 255

/* The account of a session as onu-config current gives it: its user, and whether a secret is set. */
struct account {
	char user[USER_MAX + 1];
	bool secret;
};

/* How the last onu-apply try ended, or -1. */
static int g_apply_status = -1;

/* Text after the @len bytes of @buf, which holds @cap; @len counts past it. */
static void append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*len >= cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf + *len, cap - *len, fmt, ap);
	va_end(ap);
	*len += n > 0 ? (size_t)n : 0;
}

/* Whether @line is KEY=VALUE, the key in capitals, digits and '_'. */
static bool setting_line(const char *line)
{
	size_t k = strspn(line, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");

	return k && line[k] == '=';
}

/*
 * The settings in force, in full, with each account's PPPOEn_USER and
 * PPPOEn_SECRET_SET, as onu-config current gives them through onu-act:
 * KEY=VALUE lines in @out, and no other.
 */
static int settings_current(char *out, size_t cap)
{
	char got[TEXT_MAX], *line, *save = NULL;
	size_t olen = 0;
	int rc = bus_act("settings_current", NULL, NULL, 0, NULL, got, sizeof(got));

	if (rc)
		return rc < 0 ? rc : -EIO;
	/* What it said of a line it left out is no setting. */
	out[0] = '\0';
	for (line = strtok_r(got, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (setting_line(line))
			append(out, cap, &olen, "%s\n", line);
	return olen >= cap ? -E2BIG : 0;
}

/* The account of each session, of the lines of onu-config current. */
static void accounts_of(const char *text, struct account acct[SESSIONS])
{
	memset(acct, 0, sizeof(*acct) * SESSIONS);
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		int n;

		if (strncmp(p, "PPPOE", 5) || p[5] < '1' || p[5] > '0' + SESSIONS)
			continue;
		n = p[5] - '1';
		if (!strncmp(p + 6, "_USER=", 6))
			snprintf(acct[n].user, sizeof(acct[n].user), "%.*s",
				 (int)strcspn(p + 12, "\n"), p + 12);
		else if (!strncmp(p + 6, "_SECRET_SET=1", 13))
			acct[n].secret = true;
	}
}

/* The first line of what onu-config said, without its name, as the reason. */
static void reason(const char *said, char *err, size_t errcap)
{
	static const char name[] = "onu-config: ";
	size_t n;

	if (!strncmp(said, name, sizeof(name) - 1))
		said += sizeof(name) - 1;
	n = strcspn(said, "\n");
	snprintf(err, errcap, "%.*s", (int)n, said);
}

void prov_settings_reap(void)
{
	bus_poll();
}

/* The end of onu-apply try. */
static void applied(void *ctx, int code, const char *output)
{
	(void)ctx;
	(void)output;
	g_apply_status = code < 0 ? 255 : code;
}

static const char *line_value(const char *text, const char *key, char *out,
			      size_t cap);

/*
 * The changes on trial of the state @st (trial.NAME), each with when it
 * goes back, in seconds of the uptime, and the new address the management
 * pair's may be confirmed from alone (trial.mgmt.address).
 */
static void pv_trials(struct prov_buf *out, const char *st)
{
	static const char *const names[] = { "settings", "lane", "mgmt" };
	bool first = true;

	pv_open(out, '[');
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char key[32], v[32];

		snprintf(key, sizeof(key), "trial.%s", names[i]);
		if (!line_value(st, key, v, sizeof(v)))
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "name");
		pv_str(out, names[i]);
		pv_comma(out);
		pv_key(out, "until_s");
		pv_num(out, strtol(v, NULL, 10));
		snprintf(key, sizeof(key), "trial.%s.address", names[i]);
		if (!strcmp(names[i], "mgmt") && line_value(st, key, v, sizeof(v))) {
			pv_comma(out);
			pv_key(out, "address");
			pv_str(out, v);
		}
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/* The fact @key of the state @st, as a string or null. */
static void pv_fact(struct prov_buf *out, const char *st, const char *key)
{
	char line[128];

	if (line_value(st, key, line, sizeof(line)))
		pv_str(out, line);
	else
		pv_null(out);
}

/* A fact of session @n that onu-apply says, as a string or null. */
static void pv_session_fact(struct prov_buf *out, const char *st, int n,
			    const char *fact)
{
	char key[48];

	snprintf(key, sizeof(key), "session.%d.%s", n + 1, fact);
	pv_fact(out, st, key);
}

/* @value as a string, or null when empty. */
static void pv_str_or_null(struct prov_buf *out, const char *value)
{
	if (*value)
		pv_str(out, value);
	else
		pv_null(out);
}

/* Whether session @n hands @fam down, as onu-apply says in @st. */
static bool hands(const char *st, int n, const char *fam)
{
	char key[48], line[64];
	size_t len = strlen(fam);

	snprintf(key, sizeof(key), "session.%d.hands", n + 1);
	if (!line_value(st, key, line, sizeof(line)))
		return false;
	for (const char *p = line; (p = strstr(p, fam)); p += len)
		if ((p == line || p[-1] == ' ') && (!p[len] || p[len] == ' '))
			return true;
	return false;
}

/*
 * Each session with an account: its user, whether a secret is set, its
 * settings of @text (SESSIONn_<KEY> as "key"), and what onu-apply brought up,
 * which it says in @st.
 */
static void pv_sessions(struct prov_buf *out, const struct account acct[SESSIONS],
			const char *text, const char *st)
{
	static const char *const ses_paths[] = { "l3_device", "data.address", "dns-server" };
	static const char *const pd_paths[] = { "prefix", "host" };
	bool first = true;

	pv_open(out, '[');
	for (int n = 0; n < SESSIONS; n++) {
		char prefix[16], object[48], ses[3][BUS_VALUE], pd[2][BUS_VALUE];
		size_t plen;

		if (!*acct[n].user)
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "id");
		pv_num(out, n + 1);
		pv_comma(out);
		pv_key(out, "user");
		pv_str(out, acct[n].user);
		pv_comma(out);
		pv_key(out, "secret_set");
		pv_bool(out, acct[n].secret);
		plen = (size_t)snprintf(prefix, sizeof(prefix), "SESSION%d_", n + 1);
		for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
			const char *eq = strchr(p, '=');
			size_t vlen = strcspn(eq ? eq + 1 : p, "\n");
			char name[32];
			size_t klen;

			if (strncmp(p, prefix, plen) || !eq)
				continue;
			klen = (size_t)(eq - p) - plen;
			if (klen >= sizeof(name))
				continue;
			for (size_t i = 0; i < klen; i++) {
				char c = p[plen + i];

				name[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
			}
			name[klen] = '\0';
			pv_comma(out);
			pv_key(out, name);
			pv_strn(out, eq + 1, vlen);
		}
		pv_comma(out);
		pv_key(out, "hands_on");
		pv_bool(out, hands(st, n, "ipv4"));
		pv_comma(out);
		pv_key(out, "dev");
		pv_session_fact(out, st, n, "dev");
		pv_comma(out);
		pv_key(out, "gem_dev");
		pv_session_fact(out, st, n, "gem");
		/*
		 * What the session brought, its interface of netifd's (sesN):
		 * its link, its address, handed on or not, and its name servers;
		 * and what its dhcp6d took for it and the host it went to.
		 */
		snprintf(object, sizeof(object), "network.interface.ses%d", n + 1);
		bus_status(object, ses_paths, 3, ses);
		snprintf(object, sizeof(object), "dhcp6d.ses%d", n + 1);
		bus_status(object, pd_paths, 2, pd);
		pv_comma(out);
		pv_key(out, "ppp");
		pv_str_or_null(out, ses[0]);
		pv_comma(out);
		pv_key(out, "ipv4");
		pv_str_or_null(out, ses[1]);
		pv_comma(out);
		pv_key(out, "ipv6");
		pv_session_fact(out, st, n, "ipv6");
		pv_comma(out);
		pv_key(out, "prefix");
		pv_str_or_null(out, pd[0]);
		pv_comma(out);
		pv_key(out, "prefix_host");
		pv_str_or_null(out, pd[1]);
		/* Where it hands down, and the gateway the router is given there. */
		pv_comma(out);
		pv_key(out, "lan");
		pv_session_fact(out, st, n, "lan");
		pv_comma(out);
		pv_key(out, "gateway");
		pv_session_fact(out, st, n, "gateway");
		pv_comma(out);
		pv_key(out, "dns");
		pv_str_or_null(out, ses[2]);
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/*
 * What the data path came to, as onu-apply says it (onu-apply state): lines
 * "name=value", of which a fact that is not there has none. Without an
 * answer the page shows the settings and no state.
 */
static void state_of(char *out, size_t cap)
{
	if (bus_act("settings_state", NULL, NULL, 0, NULL, out, cap))
		out[0] = '\0';
}

int prov_settings(struct prov_buf *out)
{
	struct account acct[SESSIONS];
	char text[TEXT_MAX], st[TEXT_MAX], line[128];
	bool first = true;
	int rc;

	prov_settings_reap();
	rc = settings_current(text, sizeof(text));
	if (rc)
		return rc;
	accounts_of(text, acct);
	state_of(st, sizeof(st));

	pv_open(out, '{');
	pv_key(out, "settings");
	pv_open(out, '{');
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		const char *eq = strchr(p, '=');
		char name[32];
		size_t klen;

		if (!eq || !strncmp(p, "SESSION", 7))
			continue;
		klen = (size_t)(eq - p);
		if (klen >= sizeof(name))
			continue;
		memcpy(name, p, klen);
		name[klen] = '\0';
		if (!first)
			pv_comma(out);
		first = false;
		pv_key(out, name);
		pv_strn(out, eq + 1, strcspn(eq + 1, "\n"));
	}
	pv_close(out, '}');
	pv_comma(out);

	pv_key(out, "sessions");
	pv_sessions(out, acct, text, st);
	pv_comma(out);
	explicit_bzero(acct, sizeof(acct));

	pv_key(out, "mode");
	pv_fact(out, st, "mode");
	pv_comma(out);

	pv_key(out, "dns");
	pv_fact(out, st, "dns");
	pv_comma(out);

	/* The address a session handed to the router, and where. */
	pv_key(out, "pass");
	if (line_value(st, "pass.address", line, sizeof(line))) {
		pv_open(out, '{');
		pv_key(out, "address");
		pv_str(out, line);
		pv_comma(out);
		pv_key(out, "lan");
		pv_fact(out, st, "pass.lan");
		/* The concentrator's address, when it is the router's gateway. */
		pv_comma(out);
		pv_key(out, "peer_gateway");
		pv_fact(out, st, "pass.peer_gateway");
		pv_close(out, '}');
	} else {
		pv_null(out);
	}
	pv_comma(out);

	pv_key(out, "store");
	pv_fact(out, st, "store");
	pv_comma(out);

	pv_key(out, "trials");
	pv_trials(out, st);
	pv_comma(out);

	pv_key(out, "applying");
	pv_bool(out, bus_act_running("settings_try"));
	pv_comma(out);
	pv_key(out, "last_apply_status");
	if (g_apply_status < 0)
		pv_null(out);
	else
		pv_num(out, g_apply_status);
	pv_close(out, '}');
	return 0;
}

/*
 * A change of the settings: onu-config judges it over the settings in force
 * and writes it, then onu-apply tries it, which is not waited for.
 */
static struct prov_change g_change;

static void settings_written(void *ctx, int code, const char *output)
{
	char err[256];
	int rc;

	(void)ctx;
	if (code == 1) {
		reason(output, err, sizeof(err));
		prov_change_end(&g_change, -EINVAL, err);
		return;
	}
	if (code) {
		snprintf(err, sizeof(err), "onu-config: %s",
			 code < 0 ? "it does not run" : "the settings were not written");
		prov_change_end(&g_change, -EIO, err);
		return;
	}
	rc = bus_act_start("settings_try", -1, NULL, applied, NULL);
	snprintf(err, sizeof(err), "onu-apply: %s", strerror(-rc));
	prov_change_end(&g_change, rc, err);
}

int prov_settings_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx)
{
	int rc;

	prov_settings_reap();
	if (bus_act_running("settings_try")) {
		snprintf(err, errcap, "the last change is still being applied");
		return -EBUSY;
	}
	if (strlen(val) >= TEXT_MAX) {
		snprintf(err, errcap, "the settings are too long");
		return -EINVAL;
	}
	rc = prov_change_take(&g_change, done, ctx, err, errcap);
	if (rc)
		return rc;
	rc = bus_act_begin("settings_change", NULL, NULL, 0, val, settings_written,
			   NULL);
	if (rc) {
		prov_change_drop(&g_change);
		snprintf(err, errcap, "onu-config: it does not run");
		return -EIO;
	}
	return -EINPROGRESS;
}

int prov_settings_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_settings_write(val, err, errcap, prov_waited, &w), &w);
}

/*
 * The panel's views of the settings. The panel names no mode: the stick
 * passes the OLT's services to the router in any case, and holds PPPoE
 * sessions itself when it has accounts.
 */

/* A KEY=VALUE line of @text, or NULL; @out holds the value. */
static const char *line_value(const char *text, const char *key, char *out,
			      size_t cap)
{
	size_t n = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, n) && p[n] == '=') {
			const char *v = p + n + 1;
			size_t len = strcspn(v, "\r\n");

			if (len >= cap)
				return NULL;
			memcpy(out, v, len);
			out[len] = '\0';
			return out;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

/*
 * A session of the stick's own: ID, 1 to 7, or none for the first free one;
 * USER and SECRET, its account, an empty USER removing the session (an
 * absent SECRET keeps the one there is); VLAN, MAC, GEM, FAMILY, PASS and
 * HOST_VLAN. onu-config judges them, and takes over the WAN's settings of before,
 * which the first session took when it had none of its own.
 */
int prov_session_write(const char *val, char *err, size_t errcap,
		       prov_done done, void *ctx)
{
	char id[VALUE_MAX], user[USER_MAX + 2], secret[SECRET_MAX + 2],
		vlan[VALUE_MAX], mac[VALUE_MAX], gem[VALUE_MAX], fam[VALUE_MAX],
		pass[VALUE_MAX], host[VALUE_MAX], text[TEXT_MAX];
	const char *i = line_value(val, "ID", id, sizeof(id));
	const char *u = line_value(val, "USER", user, sizeof(user));
	const char *sec = line_value(val, "SECRET", secret, sizeof(secret));
	const char *v = line_value(val, "VLAN", vlan, sizeof(vlan));
	const char *m = line_value(val, "MAC", mac, sizeof(mac));
	const char *g = line_value(val, "GEM", gem, sizeof(gem));
	const char *f = line_value(val, "FAMILY", fam, sizeof(fam));
	const char *p = line_value(val, "PASS", pass, sizeof(pass));
	const char *h = line_value(val, "HOST_VLAN", host, sizeof(host));
	struct account acct[SESSIONS];
	int n = 0;
	size_t len = 0;
	int rc;

	rc = settings_current(text, sizeof(text));
	if (rc) {
		snprintf(err, errcap, "onu-config: the settings in force cannot be read");
		return rc;
	}
	accounts_of(text, acct);
	if (i) {
		if (strlen(i) != 1 || *i < '1' || *i > '0' + SESSIONS) {
			snprintf(err, errcap, "ID: 1 to %d", SESSIONS);
			explicit_bzero(acct, sizeof(acct));
			return -EINVAL;
		}
		n = *i - '0';
	} else {
		for (int k = 0; k < SESSIONS && !n; k++)
			if (!*acct[k].user)
				n = k + 1;
		if (!n || !u || !*u) {
			snprintf(err, errcap, n ? "a new session needs USER" :
						  "all %d sessions are taken",
				 SESSIONS);
			explicit_bzero(acct, sizeof(acct));
			return -EINVAL;
		}
	}
	if (!*acct[n - 1].user && (!u || !*u)) {
		snprintf(err, errcap, "session %d has no account", n);
		explicit_bzero(acct, sizeof(acct));
		return -EINVAL;
	}
	explicit_bzero(acct, sizeof(acct));

	if (u && !*u) {
		append(text, sizeof(text), &len, "PPPOE%d_USER=\n", n);
	} else {
		if (u)
			append(text, sizeof(text), &len, "PPPOE%d_USER=%s\n", n, u);
		if (sec && *sec)
			append(text, sizeof(text), &len, "PPPOE%d_SECRET=%s\n", n, sec);
		if (v)
			append(text, sizeof(text), &len, "SESSION%d_VLAN=%s\n", n, v);
		if (m)
			append(text, sizeof(text), &len, "SESSION%d_MAC=%s\n", n, m);
		if (g)
			append(text, sizeof(text), &len, "SESSION%d_GEM=%s\n", n, g);
		if (f)
			append(text, sizeof(text), &len, "SESSION%d_FAMILY=%s\n", n, f);
		if (p)
			append(text, sizeof(text), &len, "SESSION%d_PASS=%s\n", n, p);
		if (h)
			append(text, sizeof(text), &len, "SESSION%d_HOST_VLAN=%s\n", n, h);
	}
	if (!len)
		text[0] = '\0';

	rc = prov_settings_write(text, err, errcap, done, ctx);
	explicit_bzero(secret, sizeof(secret));
	explicit_bzero(text, sizeof(text));
	return rc;
}

int prov_session_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_session_write(val, err, errcap, prov_waited, &w), &w);
}
