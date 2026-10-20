// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick's own settings: its name, the name the panel shows for the
 * system, the setting of its SFP lane, and the management VLAN and address
 * it answers on. onu-config keeps them, and the
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
#include <time.h>

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

/* A change of the name: stored first, then given to the running system. */
static struct prov_change g_host;
static char g_host_name[64];

static void host_named(void *ctx, int code, const char *output)
{
	(void)ctx;
	(void)output;
	prov_change_end(&g_host, code ? -EIO : 0,
			"the name is stored, and taken at the next start");
}

static void host_stored(void *ctx, int code, const char *output)
{
	static const char *const names[] = { "name" };
	const char *const values[] = { g_host_name };

	(void)ctx;
	(void)output;
	if (code)
		prov_change_end(&g_host, -EIO, "the name was not stored");
	else if (bus_act_begin("hostname", names, values, 1, NULL, host_named, NULL))
		prov_change_end(&g_host, -EIO,
				"the name is stored, and taken at the next start");
}

int prov_hostname_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx)
{
	char name[64], input[96];
	int rc;

	if (!line_of(val, "HOSTNAME", name, sizeof(name)) || !host_name(name)) {
		snprintf(err, errcap,
			 "HOSTNAME: letters, digits and hyphens, not first or last, up to 63");
		return -EINVAL;
	}
	rc = prov_change_take(&g_host, done, ctx, err, errcap);
	if (rc)
		return rc;
	snprintf(g_host_name, sizeof(g_host_name), "%s", name);
	snprintf(input, sizeof(input), "HOSTNAME=%s\n", name);
	rc = bus_act_begin("system_store", NULL, NULL, 0, input, host_stored, NULL);
	if (rc) {
		prov_change_drop(&g_host);
		snprintf(err, errcap, "the name was not stored");
		return -EIO;
	}
	return -EINPROGRESS;
}

int prov_hostname_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_hostname_write(val, err, errcap, prov_waited, &w), &w);
}

static bool ascii_alnum(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool product_name_ok(const char *v)
{
	size_t n = strlen(v);

	if (!n || n > PRODUCT_NAME_MAX || !ascii_alnum(v[0]) || !ascii_alnum(v[n - 1]))
		return false;
	for (size_t i = 0; i < n; i++)
		if (!ascii_alnum(v[i]) && v[i] != ' ' && v[i] != '-' && v[i] != '.')
			return false;
	return true;
}

void product_name(char *out, size_t cap)
{
	char text[1024], v[64];

	if (!prov_slurp("/run/onu-system", text, sizeof(text), NULL) &&
	    line_of(text, "PRODUCT_NAME", v, sizeof(v)) && product_name_ok(v))
		snprintf(out, cap, "%s", v);
	else
		snprintf(out, cap, "%s", PRODUCT_NAME_DEFAULT);
}

void product_slug(char *out, size_t cap)
{
	char name[PRODUCT_NAME_MAX + 1];
	size_t n = 0;

	if (cap <= PRODUCT_NAME_MAX) {
		if (cap)
			out[0] = '\0';
		return;
	}
	product_name(name, sizeof(name));
	/*
	 * The name begins and ends with a letter or a digit, so the slug has
	 * no hyphen at either end, and is no longer than the name.
	 */
	for (const char *p = name; *p; p++) {
		if (ascii_alnum(*p))
			out[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
		else if (out[n - 1] != '-')
			out[n++] = '-';
	}
	out[n] = '\0';
}

int prov_product(struct prov_buf *out)
{
	char name[PRODUCT_NAME_MAX + 1];

	product_name(name, sizeof(name));
	pv_str(out, name);
	return 0;
}

/* A change of the product name: stored, which puts it in /run/onu-system too. */
static struct prov_change g_product;

static void product_stored(void *ctx, int code, const char *output)
{
	(void)ctx;
	(void)output;
	prov_change_end(&g_product, code ? -EIO : 0,
			code ? "the name was not stored" : "the name is stored");
}

int prov_product_write(const char *val, char *err, size_t errcap,
		       prov_done done, void *ctx)
{
	char name[64], input[96];
	int rc;

	/* An empty name takes the stick's away, for the default. */
	if (!line_of(val, "PRODUCT_NAME", name, sizeof(name)) ||
	    (*name && !product_name_ok(name))) {
		snprintf(err, errcap,
			 "PRODUCT_NAME: up to %d letters, digits, blanks, hyphens and dots, "
			 "a letter or a digit first and last; empty for %s",
			 PRODUCT_NAME_MAX, PRODUCT_NAME_DEFAULT);
		return -EINVAL;
	}
	rc = prov_change_take(&g_product, done, ctx, err, errcap);
	if (rc)
		return rc;
	snprintf(input, sizeof(input), "PRODUCT_NAME=%s\n", name);
	rc = bus_act_begin("system_store", NULL, NULL, 0, input, product_stored, NULL);
	if (rc) {
		prov_change_drop(&g_product);
		snprintf(err, errcap, "the name was not stored");
		return -EIO;
	}
	return -EINPROGRESS;
}

int prov_product_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_product_write(val, err, errcap, prov_waited, &w), &w);
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

/* A change of the lane's setting: a trial, a keep or a revert. */
static struct prov_change g_lane;

/* What the lane's program said of a change: 0, or why it was refused. */
static int lane_result(int code, const char *output, char *err, size_t errcap)
{
	if (code == 2 || code == -EINVAL) {
		snprintf(err, errcap, "not a setting of the lane");
		return -EINVAL;
	}
	if (code) {
		snprintf(err, errcap, "%s", *output ? output : "the lane was not changed");
		err[strcspn(err, "\n")] = '\0';
		return -EBUSY;
	}
	return 0;
}

static void lane_changed(void *ctx, int code, const char *output)
{
	char err[256];
	int rc = lane_result(code, output, err, sizeof(err));

	(void)ctx;
	prov_change_end(&g_lane, rc, err);
}

int prov_sfp_lane_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx)
{
	static const char *const names[] = { "mode" };
	char mode[32], v[8];
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
	rc = prov_change_take(&g_lane, done, ctx, err, errcap);
	if (rc)
		return rc;
	rc = bus_act_begin(method, names, values, 1, NULL, lane_changed, NULL);
	if (rc) {
		prov_change_drop(&g_lane);
		return lane_result(rc, "", err, errcap);
	}
	return -EINPROGRESS;
}

int prov_sfp_lane_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_sfp_lane_write(val, err, errcap, prov_waited, &w), &w);
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

/* A flag of KEY= in @text, 0 or 1, as JSON, or null. */
static void pv_line_bool(struct prov_buf *out, const char *text, const char *key)
{
	char v[8];

	if (line_of(text, key, v, sizeof(v)) && (!strcmp(v, "0") || !strcmp(v, "1")))
		pv_bool(out, v[0] == '1');
	else
		pv_null(out);
}

/* The keys of a source in the state file, by their name there and in JSON. */
struct clock_key {
	const char *line;
	const char *json;
	char kind;		/* 's' string, 'n' number, 'b' flag */
};

static const struct clock_key ntp_keys[] = {
	{ "ntp_server", "server", 's' },	{ "ntp_good", "good", 'b' },
	{ "ntp_offset_ns", "offset_ns", 'n' },	{ "ntp_delay_ns", "delay_ns", 'n' },
	{ "ntp_stratum", "stratum", 'n' },
};

/*
 * With the switch's hardware stamps PTP sets the switch's clock (the PHC),
 * and the system clock follows it: the stamps, the PHC's frequency, the
 * system clock's offset from it and the Delay_Req stamped as they left.
 */
static const struct clock_key ptp_keys[] = {
	{ "ptp_interface", "interface", 's' },	{ "ptp_good", "good", 'b' },
	{ "ptp_master", "master", 's' },	{ "ptp_via", "via", 's' },
	{ "ptp_delay_ns", "delay_ns", 'n' },	{ "ptp_offset_ns", "offset_ns", 'n' },
	{ "ptp_stamps", "stamps", 's' },	{ "ptp_phc_freq_ppb", "phc_freq_ppb", 'n' },
	{ "ptp_sys_phc_offset_ns", "sys_phc_offset_ns", 'n' },
	{ "ptp_tx_stamps", "tx_stamps", 'n' },	{ "ptp_tx_missed", "tx_missed", 'n' },
};

static const struct clock_key olt_keys[] = {
	{ "olt_good", "good", 'b' },
	{ "olt_utc", "utc_s", 'n' },
	{ "olt_offset_ns", "offset_ns", 'n' },
};

/* One source as an object, or null when timed does not ask it (its first key is missing). */
static void pv_source(struct prov_buf *out, const char *text, const struct clock_key *k, size_t n)
{
	char v[96];

	if (!line_of(text, k[0].line, v, sizeof(v))) {
		pv_null(out);
		return;
	}
	pv_open(out, '{');
	for (size_t i = 0; i < n; i++) {
		if (i)
			pv_comma(out);
		pv_key(out, k[i].json);
		if (k[i].kind == 's')
			pv_line_str(out, text, k[i].line);
		else if (k[i].kind == 'n')
			pv_line_num(out, text, k[i].line);
		else
			pv_line_bool(out, text, k[i].line);
	}
	pv_close(out, '}');
}

/*
 * The clock: the time it reads, and what timed says of it in its state file
 * (timed/README.rst), the source in use and how near the clock is to it, and
 * what each source it asks last gave. Without the file, nothing sets the
 * clock: the source and the state are null.
 */
int prov_clock(struct prov_buf *out)
{
	char text[2048];
	bool have = !prov_slurp("/run/timed/state", text, sizeof(text), NULL);

	if (!have)
		text[0] = '\0';
	pv_open(out, '{');
	pv_key(out, "now_s");
	pv_num(out, (long long)time(NULL));
	pv_comma(out);
	pv_key(out, "source");
	pv_line_str(out, text, "source");
	pv_comma(out);
	pv_key(out, "state");
	pv_line_str(out, text, "state");
	pv_comma(out);
	pv_key(out, "offset_ns");
	pv_line_num(out, text, "offset_ns");
	pv_comma(out);
	pv_key(out, "updated_s");
	pv_line_num(out, text, "updated");
	pv_comma(out);
	pv_key(out, "steps");
	pv_line_num(out, text, "steps");
	pv_comma(out);
	pv_key(out, "freq_ppb");
	pv_line_num(out, text, "freq_ppb");
	pv_comma(out);
	pv_key(out, "ntp");
	pv_source(out, text, ntp_keys, ARRAY_SIZE(ntp_keys));
	pv_comma(out);
	pv_key(out, "ptp");
	pv_source(out, text, ptp_keys, ARRAY_SIZE(ptp_keys));
	pv_comma(out);
	pv_key(out, "olt");
	pv_source(out, text, olt_keys, ARRAY_SIZE(olt_keys));
	pv_close(out, '}');
	return 0;
}
