// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * How the stick introduces itself to the OLT: the "name=value" lines of
 * /run/onu-identity that onu-store keeps, and /run/onu-identity-source,
 * "store" or "factory". A read never gives the PLOAM password nor the
 * logical (LOID) password, only whether one is set. A change goes to onu-store (POND_STORE names another program),
 * then onu-mode reidentify (POND_REIDENTIFY) registers again with the OLT.
 */

#define _GNU_SOURCE

#include "prov.h"
#include "providers.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define IDENTITY "/run/onu-identity"
#define SOURCE "/run/onu-identity-source"

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

static pid_t g_reidentify;

void prov_identity_reap(void)
{
	if (g_reidentify > 0 && waitpid(g_reidentify, NULL, WNOHANG) == g_reidentify)
		g_reidentify = 0;
}

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

int prov_identity(struct prov_buf *out)
{
	char text[2048], v[256], source[32];
	size_t i;

	if (prov_slurp(IDENTITY, text, sizeof(text), NULL))
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
	field(text, "ploam_password", v, sizeof(v));
	pv_key(out, "ploam_set");
	pv_bool(out, *v != '\0');
	pv_comma(out);
	field(text, "loid_password", v, sizeof(v));
	pv_key(out, "loid_password_set");
	pv_bool(out, *v != '\0');
	explicit_bzero(v, sizeof(v));
	explicit_bzero(text, sizeof(text));
	pv_comma(out);
	pv_key(out, "source");
	if (!prov_read_line(SOURCE, source, sizeof(source)) &&
	    (!strcmp(source, "store") || !strcmp(source, "factory")))
		pv_str(out, source);
	else
		pv_null(out);
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

/* Run @prog with @arg, writing @input to it; 0 when it ended well. */
static int run_with_input(const char *prog, const char *arg, const char *input)
{
	int fd[2], status;
	pid_t pid;
	size_t left = strlen(input);

	if (pipe(fd))
		return -errno;
	pid = fork();
	if (pid < 0) {
		int e = errno;

		close(fd[0]);
		close(fd[1]);
		return -e;
	}
	if (pid == 0) {
		dup2(fd[0], 0);
		close(fd[0]);
		close(fd[1]);
		execlp(prog, prog, arg, (char *)NULL);
		_exit(127);
	}
	close(fd[0]);
	while (left) {
		ssize_t n = write(fd[1], input, left);

		if (n <= 0)
			break;
		input += n;
		left -= (size_t)n;
	}
	close(fd[1]);
	if (waitpid(pid, &status, 0) < 0)
		return -errno;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

int prov_identity_set(const char *val, char *err, size_t errcap)
{
	char text[1024], line[300];
	const char *p = val;
	size_t len = 0;
	int rc;

	prov_identity_reap();
	if (g_reidentify > 0) {
		snprintf(err, errcap, "the stick is still registering again");
		return -EBUSY;
	}

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

	rc = run_with_input(getenv("POND_STORE") ?: "onu-store", "identity", text);
	explicit_bzero(text, sizeof(text));
	explicit_bzero(line, sizeof(line));
	if (rc) {
		snprintf(err, errcap, "the identity was not stored");
		return rc;
	}

	/* Register again under the new identity, without waiting for it. */
	g_reidentify = fork();
	if (g_reidentify == 0) {
		const char *prog = getenv("POND_REIDENTIFY") ?: "onu-mode";

		signal(SIGCHLD, SIG_DFL);
		execlp(prog, prog, "reidentify", (char *)NULL);
		_exit(127);
	}
	return 0;
}
