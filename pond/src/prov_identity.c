// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * How the stick introduces itself to the OLT: the "name=value" lines of
 * /run/onu-identity that onu-config keeps, and where they came from, "store"
 * or "factory", as onu-config identity-current gives them through onu-act,
 * without the PLOAM password nor the logical (LOID) password, only whether
 * one is set: root alone reads the file. A change goes to
 * onu-config, then onu-apply reidentify registers again with the OLT, both
 * through onu-act (bus.h); onu-apply takes one registration after the other.
 */

#define _GNU_SOURCE

#include "bus.h"
#include "prov.h"
#include "providers.h"

#include "mongoose.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The fields the panel shows and may change: its key, the file's, the
 * longest value, and for a number the largest.
 */
static const struct {
	const char *key;
	const char *name;
	size_t max;
	unsigned long number;
} g_fields[] = {
	{ "SERIAL", "serial", 12, 0 },
	{ "VENDOR_ID", "vendor_id", 4, 0 },
	{ "EQUIPMENT_ID", "equipment_id", 20, 0 },
	{ "VERSION", "version", 14, 0 },
	{ "EQUIPMENT_SERIAL", "equipment_serial", 32, 0 },
	{ "MODEL", "model", 32, 0 },
	{ "OMCC_VERSION", "omcc_version", 8, 0xff },
	{ "VENDOR_PRODUCT_CODE", "vendor_product_code", 6, 0xffff },
	{ "TM_OPTION", "tm_option", 1, 2 },
	{ "LOID", "loid", 24, 0 },
	{ "SW_VERSION0", "sw_version0", 14, 0 },
	{ "SW_VERSION1", "sw_version1", 14, 0 },
	{ "PACK_VERSION", "pack_version", 14, 0 },
};

/* Shown as they are, never changed from the panel. */
static const char *const g_shown[] = { "ploam_format", "sw_version" };

/* The value of "name=" in the identity, or "". */
static void field(const char *text, const char *name, char *out, size_t cap)
{
	size_t n = strlen(name);
	const char *p = text;

	*out = '\0';
	while (p && *p) {
		if (!strncmp(p, name, n) && p[n] == '=') {
			size_t len = strcspn(p + n + 1, "\r\n");

			if (len >= cap)
				len = cap - 1;
			memcpy(out, p + n + 1, len);
			out[len] = '\0';
			return;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

/* A number of the identity of @lo to @hi, or @dflt for none or another. */
static unsigned long ranged(const char *text, const char *name, unsigned long lo,
			    unsigned long hi, unsigned long dflt)
{
	char v[8], *end;
	unsigned long n;

	field(text, name, v, sizeof(v));
	n = strtoul(v, &end, 10);
	return *v && !*end && n >= lo && n <= hi ? n : dflt;
}

/*
 * The subscriber ports the stick tells the OLT of (omcid's
 * hw_kernel.c, identity_ports()): uni_report, "both", "pptp" or "veip";
 * uni_ports, 1 to 8 Ethernet UNIs; pots_ports, 0 to 2 phone sockets.
 */
static void ports(const char *text, struct prov_buf *out)
{
	char v[8];

	field(text, "uni_report", v, sizeof(v));
	pv_key(out, "uni_report");
	pv_str(out, !strcmp(v, "pptp") || !strcmp(v, "veip") ? v : "both");
	pv_comma(out);
	pv_key(out, "uni_ports");
	pv_num(out, (long long)ranged(text, "uni_ports", 1, 8, 1));
	pv_comma(out);
	pv_key(out, "pots_ports");
	pv_num(out, (long long)ranged(text, "pots_ports", 0, 2, 0));
}

/*
 * The PPPoE login the OLT sent (omcid's method "login", class 148): its
 * user and whether it has a password, never the password, which
 * /api/identity/olt-login gives with root's password; null for none.
 */
static void olt_login(struct prov_buf *out)
{
	char text[512];
	int n = bus_call("omcid", "login", NULL, 0, text, sizeof(text));
	char *user = n > 0 ? mg_json_get_str(mg_str_n(text, (size_t)n), "$.user") : NULL;
	char *secret = n > 0 ? mg_json_get_str(mg_str_n(text, (size_t)n), "$.secret") : NULL;

	pv_key(out, "olt_login");
	if (user && *user) {
		pv_open(out, '{');
		pv_key(out, "user");
		pv_str(out, user);
		pv_comma(out);
		pv_key(out, "password_set");
		pv_bool(out, secret && *secret);
		pv_close(out, '}');
	} else {
		pv_null(out);
	}
	if (secret)
		explicit_bzero(secret, strlen(secret));
	free(user);
	free(secret);
	explicit_bzero(text, sizeof(text));
}

int prov_identity(struct prov_buf *out)
{
	char text[2048], v[256];
	size_t i;

	/* onu-config gives it without the passwords: root alone reads the file. */
	if (bus_act("identity_current", NULL, NULL, 0, NULL, text, sizeof(text)))
		*text = '\0';

	pv_open(out, '{');
	for (i = 0; i < ARRAY_SIZE(g_fields); i++) {
		field(text, g_fields[i].name, v, sizeof(v));
		pv_key(out, g_fields[i].name);
		pv_str(out, v);
		pv_comma(out);
	}
	for (i = 0; i < ARRAY_SIZE(g_shown); i++) {
		field(text, g_shown[i], v, sizeof(v));
		pv_key(out, g_shown[i]);
		pv_str(out, v);
		pv_comma(out);
	}
	field(text, "ploam_set", v, sizeof(v));
	pv_key(out, "ploam_set");
	pv_bool(out, !strcmp(v, "1"));
	pv_comma(out);
	field(text, "loid_password_set", v, sizeof(v));
	pv_key(out, "loid_password_set");
	pv_bool(out, !strcmp(v, "1"));
	pv_comma(out);
	field(text, "source", v, sizeof(v));
	pv_key(out, "source");
	if (!strcmp(v, "store") || !strcmp(v, "factory"))
		pv_str(out, v);
	else
		pv_null(out);
	pv_comma(out);
	ports(text, out);
	pv_comma(out);
	olt_login(out);
	pv_close(out, '}');
	return 0;
}

static bool printable(const char *v, size_t max)
{
	size_t n = strlen(v), i;

	if (n > max)
		return false;
	for (i = 0; i < n; i++)
		if (!isprint((unsigned char)v[i]) || v[i] == '=')
			return false;
	return true;
}

/* Four letters or digits, then eight hex digits (G.984.3 9.2.4.1). */
static bool serial(const char *v)
{
	size_t i;

	if (strlen(v) != 12)
		return false;
	for (i = 0; i < 4; i++)
		if (!isalnum((unsigned char)v[i]))
			return false;
	for (; i < 12; i++)
		if (!isxdigit((unsigned char)v[i]))
			return false;
	return true;
}

/* Ten characters, or twenty hex digits for its ten octets. */
static bool ploam(const char *v)
{
	size_t n = strlen(v), i;

	if (n == 20) {
		for (i = 0; i < n; i++)
			if (!isxdigit((unsigned char)v[i]))
				return false;
		return true;
	}
	return n >= 1 && n <= 10 && printable(v, 10);
}

/* A number up to @max, decimal or 0x and hexadecimal; empty is no number. */
static bool number(const char *v, unsigned long max)
{
	char *end;
	unsigned long n;

	if (!*v || *v == '-' || *v == '+')
		return false;
	errno = 0;
	n = strtoul(v, &end, 0);
	return !*end && !errno && n <= max;
}

/* A change of the identity: stored first, then registered under. */
static struct prov_change g_ident;

static void ident_stored(void *ctx, int code, const char *output)
{
	char out[256];

	(void)ctx;
	(void)output;
	if (code) {
		prov_change_end(&g_ident, code < 0 ? code : -EIO,
				"the identity was not stored");
		return;
	}
	/* Register again under the new identity; onu-act answers once it starts. */
	if (bus_act("reidentify", NULL, NULL, 0, NULL, out, sizeof(out)) < 0) {
		prov_change_end(&g_ident, -EIO,
				"the identity is stored, and taken at the next start");
		return;
	}
	prov_change_end(&g_ident, 0, NULL);
}

int prov_identity_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx)
{
	char text[1024], line[300];
	const char *p = val;
	size_t len = 0;
	int rc;

	while (*p) {
		size_t n = strcspn(p, "\r\n");
		const char *eq, *name = NULL;
		size_t i, max = 0;

		if (n && n < sizeof(line)) {
			memcpy(line, p, n);
			line[n] = '\0';
			eq = strchr(line, '=');
			if (!eq) {
				snprintf(err, errcap, "a line without '='");
				return -EINVAL;
			}
			line[eq - line] = '\0';
			if (!strcmp(line, "PLOAM")) {
				if (*(eq + 1) && !ploam(eq + 1)) {
					snprintf(err, errcap,
						 "PLOAM: ten characters, or twenty hex digits");
					return -EINVAL;
				}
				name = "PLOAM_PASSWORD";
			} else if (!strcmp(line, "UNI_REPORT")) {
				if (strcmp(eq + 1, "both") && strcmp(eq + 1, "pptp") &&
				    strcmp(eq + 1, "veip")) {
					snprintf(err, errcap, "UNI_REPORT: both, pptp or veip");
					return -EINVAL;
				}
				name = "UNI_REPORT";
			} else if (!strcmp(line, "UNI_PORTS") || !strcmp(line, "POTS_PORTS")) {
				const bool uni = line[0] == 'U';

				if (strlen(eq + 1) != 1 || eq[1] < (uni ? '1' : '0') ||
				    eq[1] > (uni ? '8' : '2')) {
					snprintf(err, errcap, uni ? "UNI_PORTS: 1 to 8" :
								    "POTS_PORTS: 0 to 2");
					return -EINVAL;
				}
				name = uni ? "UNI_PORTS" : "POTS_PORTS";
			} else if (!strcmp(line, "LOID_PASSWORD")) {
				if (!printable(eq + 1, 12)) {
					snprintf(err, errcap,
						 "LOID_PASSWORD: up to twelve characters");
					return -EINVAL;
				}
				name = "LOID_PASSWORD";
			} else {
				unsigned long limit = 0;

				for (i = 0; i < ARRAY_SIZE(g_fields); i++)
					if (!strcmp(line, g_fields[i].key)) {
						name = g_fields[i].key;
						max = g_fields[i].max;
						limit = g_fields[i].number;
					}
				if (!name) {
					snprintf(err, errcap, "%s: not a field of the identity", line);
					return -EINVAL;
				}
				if (!strcmp(name, "SERIAL") ? !serial(eq + 1) :
				    limit		    ? !number(eq + 1, limit) :
							      !printable(eq + 1, max)) {
					snprintf(err, errcap, "%s: not a value of it", name);
					return -EINVAL;
				}
			}
			/* An empty password keeps the one there is. */
			if ((strcmp(name, "PLOAM_PASSWORD") &&
			     strcmp(name, "LOID_PASSWORD")) || *(eq + 1))
				len += (size_t)snprintf(text + len, sizeof(text) - len,
							"%s=%s\n", name, eq + 1);
		} else if (n) {
			snprintf(err, errcap, "a line too long");
			return -EINVAL;
		}
		p += n;
		while (*p == '\r' || *p == '\n')
			p++;
	}
	if (!len) {
		snprintf(err, errcap, "nothing to change");
		return -EINVAL;
	}

	explicit_bzero(line, sizeof(line));
	rc = prov_change_take(&g_ident, done, ctx, err, errcap);
	if (rc) {
		explicit_bzero(text, sizeof(text));
		return rc;
	}
	rc = bus_act_begin("identity_store", NULL, NULL, 0, text, ident_stored, NULL);
	explicit_bzero(text, sizeof(text));
	if (rc) {
		prov_change_drop(&g_ident);
		snprintf(err, errcap, "the identity was not stored");
		return rc;
	}
	return -EINPROGRESS;
}

int prov_identity_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_identity_write(val, err, errcap, prov_waited, &w), &w);
}
