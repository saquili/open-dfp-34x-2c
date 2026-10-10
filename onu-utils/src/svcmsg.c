// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The messages of onu-svc to procd, put through a writer (svcmsg.h). */

#include <errno.h>
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

/*
 * The shell that runs a program with its output appended to a file: the file
 * and then the program come as its arguments, so that neither is ever read
 * as shell code. exec keeps the program in the shell's process, which procd
 * watches.
 */
static const char *const to_file[] = {
	"/bin/sh", "-c", "f=$1; shift; exec \"$@\" >>\"$f\" 2>&1", "sh", NULL
};

/* The strings of @argv, NULL-terminated, into the array open in @w. */
static int put_words(const struct svc_writer *w, const char *const *argv)
{
	for (int i = 0; argv[i]; i++)
		if (w->string(w->ctx, NULL, argv[i]))
			return -1;
	return 0;
}

/* One instance, a table named as it is, as procd's "set" takes it. */
static int put_instance(const struct svc_writer *w, const struct svc_instance *in)
{
	char n[3][12];

	if (w->open(w->ctx, in->name, false) || w->open(w->ctx, "command", true))
		return -1;
	if (in->out && (put_words(w, to_file) || w->string(w->ctx, NULL, in->out)))
		return -1;
	if (put_words(w, in->argv) || w->close(w->ctx))
		return -1;
	/* procd reads the three numbers of "respawn" as strings. */
	snprintf(n[0], sizeof(n[0]), "%u", in->respawn.threshold);
	snprintf(n[1], sizeof(n[1]), "%u", in->respawn.timeout);
	snprintf(n[2], sizeof(n[2]), "%u", in->respawn.retry);
	if (w->open(w->ctx, "respawn", true) || w->string(w->ctx, NULL, n[0]) ||
	    w->string(w->ctx, NULL, n[1]) || w->string(w->ctx, NULL, n[2]) || w->close(w->ctx))
		return -1;
	if (in->log && (w->boolean(w->ctx, "stdout", true) || w->boolean(w->ctx, "stderr", true)))
		return -1;
	if (in->user && w->string(w->ctx, "user", in->user))
		return -1;
	return w->close(w->ctx) ? -1 : 0;
}

int svc_service_msg(const struct svc_writer *w, const char *name,
		    const struct svc_instance *inst, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!inst[i].argv || !inst[i].argv[0])
			return -1;
	if (w->string(w->ctx, "name", name) || w->open(w->ctx, "instances", false))
		return -1;
	for (size_t i = 0; i < n; i++)
		if (put_instance(w, &inst[i]))
			return -1;
	return w->close(w->ctx) ? -1 : 0;
}

int svc_set_msg(const struct svc_writer *w, const char *name, const struct svc_respawn *r,
		bool log, const char *out, const char *user, const char *const argv[])
{
	struct svc_instance one = {
		.name = name, .respawn = *r, .log = log, .out = out, .user = user,
		.argv = argv,
	};

	return svc_service_msg(w, name, &one, 1);
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
		} else if (!strcmp(w, "-u")) {
			w = strtok_r(NULL, " \t\r\n", &save);
			if (!w || !svc_name_ok(w))
				return -1;
			inst->user = w;
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

int svc_name_msg(const struct svc_writer *w, const char *name)
{
	return w->string(w->ctx, "name", name) ? -1 : 0;
}
