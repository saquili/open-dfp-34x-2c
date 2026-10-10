// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The messages of onu-svc to procd, as JSON text (svcmsg.h). */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svcmsg.h"

bool svc_name_ok(const char *name)
{
	if (!name || !*name)
		return false;
	for (const char *p = name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
			return false;
	return true;
}

/* A decimal number of at most 9 digits, all of @s. */
static int number(const char *s, unsigned int *v)
{
	char *end;

	if (*s < '0' || *s > '9' || strlen(s) > 9)
		return -1;
	errno = 0;
	*v = (unsigned int)strtoul(s, &end, 10);
	return errno || *end ? -1 : 0;
}

int svc_parse_respawn(const char *s, struct svc_respawn *r)
{
	char copy[32], *threshold, *timeout, *retry;

	if (strlen(s) >= sizeof(copy))
		return -1;
	strcpy(copy, s);
	threshold = copy;
	timeout = strchr(threshold, ',');
	if (!timeout)
		return -1;
	*timeout++ = '\0';
	retry = strchr(timeout, ',');
	if (!retry)
		return -1;
	*retry++ = '\0';
	if (number(threshold, &r->threshold) || number(timeout, &r->timeout) ||
	    number(retry, &r->retry))
		return -1;
	return 0;
}

/* Text appended to a buffer that knows when it overflowed. */
struct out {
	char *buf;
	size_t cap;
	size_t len;
	bool full;
};

static void put(struct out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (o->full)
		return;
	va_start(ap, fmt);
	n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= o->cap - o->len)
		o->full = true;
	else
		o->len += (size_t)n;
}

/* @s as a JSON string: '"' and '\' escaped, and every control character. */
static void put_string(struct out *o, const char *s)
{
	put(o, "\"");
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			put(o, "\\%c", *p);
		else if (*p < 0x20 || *p == 0x7f)
			put(o, "\\u%04x", *p);
		else
			put(o, "%c", *p);
	}
	put(o, "\"");
}

/*
 * The shell that runs a program with its output appended to a file: the file
 * and then the program come as its arguments, so that neither is ever read
 * as shell code. exec keeps the program in the shell's process, which procd
 * watches.
 */
static const char *const to_file[] = {
	"/bin/sh", "-c", "f=$1; shift; exec \"$@\" >>\"$f\" 2>&1", "sh", NULL
};

int svc_set_json(char *buf, size_t cap, const char *name, const struct svc_respawn *r,
		 bool log, const char *out, const char *const argv[])
{
	struct out o = { .buf = buf, .cap = cap };
	bool first = true;

	if (!cap)
		return -1;
	buf[0] = '\0';
	put(&o, "{\"name\":");
	put_string(&o, name);
	put(&o, ",\"instances\":{");
	put_string(&o, name);
	put(&o, ":{\"command\":[");
	if (out) {
		for (int i = 0; to_file[i]; i++) {
			put(&o, first ? "" : ",");
			put_string(&o, to_file[i]);
			first = false;
		}
		put(&o, ",");
		put_string(&o, out);
	}
	for (int i = 0; argv[i]; i++) {
		put(&o, first ? "" : ",");
		put_string(&o, argv[i]);
		first = false;
	}
	/* procd reads the three numbers of "respawn" as strings. */
	put(&o, "],\"respawn\":[\"%u\",\"%u\",\"%u\"]", r->threshold, r->timeout, r->retry);
	if (log)
		put(&o, ",\"stdout\":true,\"stderr\":true");
	put(&o, "}}}");
	return o.full ? -1 : (int)o.len;
}

int svc_name_json(char *buf, size_t cap, const char *name)
{
	struct out o = { .buf = buf, .cap = cap };

	if (!cap)
		return -1;
	buf[0] = '\0';
	put(&o, "{\"name\":");
	put_string(&o, name);
	put(&o, "}");
	return o.full ? -1 : (int)o.len;
}
