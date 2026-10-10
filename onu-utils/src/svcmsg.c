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

/* One instance, "NAME":{...}, as procd's "set" takes it. */
static void put_instance(struct out *o, const struct svc_instance *in)
{
	bool first = true;

	put_string(o, in->name);
	put(o, ":{\"command\":[");
	if (in->out) {
		for (int i = 0; to_file[i]; i++) {
			put(o, first ? "" : ",");
			put_string(o, to_file[i]);
			first = false;
		}
		put(o, ",");
		put_string(o, in->out);
	}
	for (int i = 0; in->argv[i]; i++) {
		put(o, first ? "" : ",");
		put_string(o, in->argv[i]);
		first = false;
	}
	/* procd reads the three numbers of "respawn" as strings. */
	put(o, "],\"respawn\":[\"%u\",\"%u\",\"%u\"]", in->respawn.threshold,
	    in->respawn.timeout, in->respawn.retry);
	if (in->log)
		put(o, ",\"stdout\":true,\"stderr\":true");
	put(o, "}");
}

int svc_service_json(char *buf, size_t cap, const char *name,
		     const struct svc_instance *inst, size_t n)
{
	struct out o = { .buf = buf, .cap = cap };

	if (!cap)
		return -1;
	buf[0] = '\0';
	put(&o, "{\"name\":");
	put_string(&o, name);
	put(&o, ",\"instances\":{");
	for (size_t i = 0; i < n; i++) {
		if (!inst[i].argv || !inst[i].argv[0])
			return -1;
		put(&o, i ? "," : "");
		put_instance(&o, &inst[i]);
	}
	put(&o, "}}");
	return o.full ? -1 : (int)o.len;
}

int svc_set_json(char *buf, size_t cap, const char *name, const struct svc_respawn *r,
		 bool log, const char *out, const char *const argv[])
{
	struct svc_instance one = {
		.name = name, .respawn = *r, .log = log, .out = out, .argv = argv,
	};

	return svc_service_json(buf, cap, name, &one, 1);
}

int svc_parse_instance(char *line, const char *name, struct svc_instance *inst,
		       const char **words, size_t cap)
{
	char *save = NULL, *w;
	size_t n = 0;

	memset(inst, 0, sizeof(*inst));
	inst->name = name;
	inst->respawn = (struct svc_respawn)SVC_RESPAWN_DEFAULT;
	for (w = strtok_r(line, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
		if (!strcmp(w, "--"))
			break;
		if (!strcmp(w, "-l")) {
			inst->log = true;
		} else if (!strcmp(w, "-r")) {
			w = strtok_r(NULL, " \t\r\n", &save);
			if (!w || svc_parse_respawn(w, &inst->respawn))
				return -1;
		} else if (!strcmp(w, "-o")) {
			w = strtok_r(NULL, " \t\r\n", &save);
			if (!w)
				return -1;
			inst->out = w;
		} else {
			return -1;
		}
	}
	if (!w || (inst->log && inst->out))
		return -1;
	while ((w = strtok_r(NULL, " \t\r\n", &save))) {
		if (n + 1 >= cap)
			return -1;
		words[n++] = w;
	}
	if (!n)
		return -1;
	words[n] = NULL;
	inst->argv = words;
	return 0;
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
