// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* dhcp6d's state as JSON text (state.h). */

#include <stdarg.h>
#include <stdio.h>

#include "state.h"

struct out {
	char *buf;
	size_t cap, len;
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

/* @s as a JSON string, or null; names and addresses need no escape but '"' and '\'. */
static void put_string(struct out *o, const char *s)
{
	if (!s) {
		put(o, "null");
		return;
	}
	put(o, "\"");
	for (; *s; s++)
		put(o, *s == '"' || *s == '\\' ? "\\%c" : (unsigned char)*s < 0x20 ? "?" : "%c", *s);
	put(o, "\"");
}

int dhcp6d_state_json(const struct dhcp6d_state *s, char *buf, size_t cap)
{
	struct out o = { .buf = buf, .cap = cap };

	if (!cap)
		return -1;
	buf[0] = '\0';
	put(&o, "{\"name\":");
	put_string(&o, s->name);
	put(&o, ",\"wan\":");
	put_string(&o, s->wan);
	put(&o, ",\"lan\":");
	put_string(&o, s->lan);
	put(&o, ",\"prefix\":");
	put_string(&o, s->prefix);
	put(&o, ",\"host\":");
	put_string(&o, s->host);
	put(&o, ",\"bound\":%s,\"routed\":%s}", s->bound ? "true" : "false",
	    s->routed ? "true" : "false");
	return o.full ? -1 : (int)o.len;
}
