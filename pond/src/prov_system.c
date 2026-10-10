// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick's own settings: its name, the setting of its SFP lane, and the
 * management VLAN and address it answers on. onu-config keeps them, and the
 * scripts onu-lane and onu-mgmt change the last two so that a change the
 * host does not take cannot lock anyone out (a trial that ends by itself, a
 * new address beside the old one). A change of the management pair needs
 * the connection it came by, and is mgmt.c's; the reading is here. onu-act
 * runs the programs (bus.h).
 */

#include "bus.h"
#include "prov.h"
#include "providers.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The value of KEY= in KEY=VALUE lines, or NULL. */
static const char *line_of(const char *text, const char *key, char *out, size_t cap)
{
	size_t klen = strlen(key);
	const char *p;

	for (p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			size_t len = strcspn(p + klen + 1, "\r\n");

			if (len >= cap)
				len = cap - 1;
			memcpy(out, p + klen + 1, len);
			out[len] = '\0';
			return out;
		}
	}
	return NULL;
}

/* A name of RFC 1123: letters, digits and hyphens, not at either end, 1 to 63. */
static bool host_name(const char *v)
{
	size_t n = strlen(v), i;

	if (!n || n > 63 || v[0] == '-' || v[n - 1] == '-')
		return false;
	for (i = 0; i < n; i++)
		if (!isalnum((unsigned char)v[i]) && v[i] != '-')
			return false;
	return true;
}

int prov_hostname_set(const char *val, char *err, size_t errcap)
{
	static const char *const names[] = { "name" };
	char name[64], input[96], out[256];
	const char *const values[] = { name };

	if (!line_of(val, "HOSTNAME", name, sizeof(name)) || !host_name(name)) {
		snprintf(err, errcap,
			 "HOSTNAME: letters, digits and hyphens, not first or last, up to 63");
		return -EINVAL;
	}
	snprintf(input, sizeof(input), "HOSTNAME=%s\n", name);
	if (bus_act("system_store", NULL, NULL, 0, input, out, sizeof(out))) {
		snprintf(err, errcap, "the name was not stored");
		return -EIO;
	}
	if (bus_act("hostname", names, values, 1, NULL, out, sizeof(out))) {
		snprintf(err, errcap, "the name is stored, and taken at the next start");
		return -EIO;
	}
	return 0;
}

/* A number of KEY= in @text, as JSON, or null. */
static void pv_line_num(struct prov_buf *out, const char *text, const char *key)
{
	char v[32], *end;
	long long n;

	if (line_of(text, key, v, sizeof(v)) && *v) {
		n = strtoll(v, &end, 10);
		if (!*end) {
			pv_num(out, n);
			return;
		}
	}
	pv_null(out);
}

/* A string of KEY= in @text, as JSON, or null when it is missing or empty. */
static void pv_line_str(struct prov_buf *out, const char *text, const char *key)
{
	char v[96];

	if (line_of(text, key, v, sizeof(v)) && *v)
		pv_str(out, v);
	else
		pv_null(out);
}

int prov_sfp_lane(struct prov_buf *out)
{
	char text[512], v[32];

	if (bus_act("lane_status", NULL, NULL, 0, NULL, text, sizeof(text)))
		return -EIO;
	pv_open(out, '{');
	pv_key(out, "choice");
	pv_line_str(out, text, "CHOICE");
	pv_comma(out);
	pv_key(out, "active");
	if (line_of(text, "NOW", v, sizeof(v)) && *v && strcmp(v, "searching"))
		pv_str(out, v);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "trial");
	if (line_of(text, "TRIAL_MODE", v, sizeof(v))) {
		pv_open(out, '{');
		pv_key(out, "mode");
		pv_line_str(out, text, "TRIAL_MODE");
		pv_comma(out);
		pv_key(out, "previous");
		pv_line_str(out, text, "TRIAL_PREVIOUS");
		pv_comma(out);
		pv_key(out, "until_s");
		pv_line_num(out, text, "TRIAL_UNTIL");
		pv_close(out, '}');
	} else {
		pv_null(out);
	}
	pv_close(out, '}');
	return 0;
}

int prov_sfp_lane_set(const char *val, char *err, size_t errcap)
{
	static const char *const names[] = { "mode" };
	char mode[32], v[8], out[256];
	const char *const values[] = { mode };
	const char *method;
	int rc;

	if (line_of(val, "MODE", mode, sizeof(mode))) {
		method = "lane_try";
	} else if (line_of(val, "KEEP", v, sizeof(v))) {
		method = "lane_keep";
	} else if (line_of(val, "REVERT", v, sizeof(v))) {
		method = "lane_revert";
	} else {
		snprintf(err, errcap, "MODE=setting, KEEP=1 or REVERT=1");
		return -EINVAL;
	}
	rc = bus_act(method, names, values, 1, NULL, out, sizeof(out));
	if (rc == 2 || rc == -EINVAL) {
		snprintf(err, errcap, "not a setting of the lane");
		return -EINVAL;
	}
	if (rc) {
		snprintf(err, errcap, "%s", *out ? out : "the lane was not changed");
		err[strcspn(err, "\n")] = '\0';
		return -EBUSY;
	}
	return 0;
}

int prov_mgmt(struct prov_buf *out)
{
	char text[512], v[32];

	if (bus_act("mgmt_status", NULL, NULL, 0, NULL, text, sizeof(text)))
		return -EIO;
	pv_open(out, '{');
	pv_key(out, "vlan");
	pv_line_num(out, text, "VLAN");
	pv_comma(out);
	pv_key(out, "address");
	pv_line_str(out, text, "ADDRESS");
	pv_comma(out);
	pv_key(out, "data_address");
	pv_line_str(out, text, "DATA_ADDRESS");
	pv_comma(out);
	pv_key(out, "pending");
	if (line_of(text, "PENDING_ADDRESS", v, sizeof(v))) {
		pv_open(out, '{');
		pv_key(out, "vlan");
		pv_line_num(out, text, "PENDING_VLAN");
		pv_comma(out);
		pv_key(out, "address");
		pv_line_str(out, text, "PENDING_ADDRESS");
		pv_comma(out);
		pv_key(out, "until_s");
		pv_line_num(out, text, "PENDING_UNTIL");
		pv_close(out, '}');
	} else {
		pv_null(out);
	}
	pv_close(out, '}');
	return 0;
}
