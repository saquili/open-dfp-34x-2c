// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* pppoed's messages to netifd, as JSON text (nproto.h). */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nproto.h"

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

static int done(struct out *o)
{
	return o->full ? -1 : (int)o->len;
}

static bool start(struct out *o, char *buf, size_t cap)
{
	*o = (struct out){ .buf = buf, .cap = cap };
	if (!cap)
		return false;
	buf[0] = '\0';
	return true;
}

int nproto_dump_json(char *buf, size_t cap)
{
	struct out o;

	if (!start(&o, buf, cap))
		return -1;
	/* 5 is blobmsg's 32-bit integer. */
	/* 3 is a string, 7 a bool. */
	put(&o, "{\"name\":\"pppoed\",\"config\":[[\"session\",5],[\"family\",3],"
		"[\"pass\",7],[\"defaultroute\",7]],"
		"\"no-device\":false,\"available\":true,"
		"\"teardown-on-l3-link-down\":true}");
	return done(&o);
}

int nproto_run_json(char *buf, size_t cap, const char *const argv[])
{
	struct out o;

	if (!start(&o, buf, cap) || !argv[0])
		return -1;
	put(&o, "{\"action\":1,\"command\":[");
	for (int i = 0; argv[i]; i++) {
		put(&o, i ? "," : "");
		put_string(&o, argv[i]);
	}
	put(&o, "]}");
	return done(&o);
}

int nproto_kill_json(char *buf, size_t cap)
{
	struct out o;

	if (!start(&o, buf, cap))
		return -1;
	/* 15 is SIGTERM, on which pppoed ends the session as a peer expects. */
	put(&o, "{\"action\":2,\"signal\":15}");
	return done(&o);
}

int nproto_up_json(char *buf, size_t cap, const struct nproto_link *l)
{
	struct out o;
	bool first = true;

	if (!start(&o, buf, cap) || !l->ifname)
		return -1;
	put(&o, "{\"action\":0,\"link-up\":true,\"ifname\":");
	put_string(&o, l->ifname);
	if (l->local && !l->pass) {
		put(&o, ",\"ipaddr\":[{\"ipaddr\":");
		put_string(&o, l->local);
		put(&o, ",\"mask\":\"32\"");
		if (l->peer) {
			put(&o, ",\"ptp\":");
			put_string(&o, l->peer);
		}
		put(&o, "}]");
	}
	if (l->local && l->defaultroute)
		put(&o, ",\"routes\":[{\"target\":\"0.0.0.0\",\"netmask\":\"0\"}]");
	for (int i = 0; i < 2; i++) {
		if (!l->dns[i])
			continue;
		put(&o, first ? ",\"dns\":[" : ",");
		put_string(&o, l->dns[i]);
		first = false;
	}
	if (!first)
		put(&o, "]");
	put(&o, ",\"data\":{\"session\":%u", l->session);
	if (l->local) {
		put(&o, ",\"address\":");
		put_string(&o, l->local);
	}
	if (l->peer) {
		put(&o, ",\"peer\":");
		put_string(&o, l->peer);
	}
	put(&o, ",\"pass\":%s}}", l->pass ? "true" : "false");
	return done(&o);
}

/* Where the value of @key begins in @config, or NULL. */
static const char *value_of(const char *config, const char *key)
{
	char quoted[40];
	const char *p;

	if (!config || snprintf(quoted, sizeof(quoted), "\"%s\"", key) >= (int)sizeof(quoted))
		return NULL;
	p = strstr(config, quoted);
	if (!p)
		return NULL;
	p += strlen(quoted);
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p++ != ':')
		return NULL;
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

int nproto_bool(const char *config, const char *key)
{
	const char *p = value_of(config, key);

	if (!p)
		return -1;
	if (!strncmp(p, "true", 4) || !strncmp(p, "1", 1) || !strncmp(p, "\"1\"", 3))
		return 1;
	if (!strncmp(p, "false", 5) || !strncmp(p, "0", 1) || !strncmp(p, "\"0\"", 3))
		return 0;
	return -1;
}

int nproto_string(const char *config, const char *key, char *out, size_t cap)
{
	const char *p = value_of(config, key);
	size_t n;

	if (!p || *p++ != '"')
		return -1;
	n = strcspn(p, "\"\\");
	if (p[n] != '"' || n >= cap)
		return -1;
	memcpy(out, p, n);
	out[n] = '\0';
	return (int)n;
}

int nproto_session(const char *config)
{
	const char *p = value_of(config, "session");
	char *end;
	long n;

	if (!p)
		return -1;
	/* netifd gives it as a number; a string of one is taken too. */
	if (*p == '"')
		p++;
	if (*p < '1' || *p > '9')
		return -1;
	n = strtol(p, &end, 10);
	if (n < 1 || n > 7 || (*end != '"' && *end != ',' && *end != '}' && *end != ' '))
		return -1;
	return (int)n;
}
