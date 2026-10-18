// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The values as replies, written into the connection's send buffer (value.h). */

#include "value.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A reply being written at the end of the send buffer of @c, from @start on:
 * @length is where the length of its body goes, once it is whole, and @body
 * where the body begins. @nomem tells a buffer that could not grow from one
 * that reached its most.
 */
struct reply {
	struct mg_connection *c;
	struct prov_buf b;
	size_t start;
	size_t length;
	size_t body;
	bool nomem;
};

/* The digits of a body's length, padded with blanks, as mongoose does it. */
#define LENGTH_DIGITS 10

/* The &prov_buf.grow of a reply: the send buffer, with what it holds. */
static bool send_grow(struct prov_buf *b, size_t cap)
{
	struct reply *r = b->ctx;
	struct mg_connection *c = r->c;

	c->send.len = b->len;
	if (!mg_iobuf_resize(&c->send, cap)) {
		r->nomem = true;
		return false;
	}
	b->buf = (char *)c->send.buf;
	b->cap = c->send.size;
	return true;
}

/*
 * Begin a reply 200 of at most @max bytes, the headers apart, to @c: whether
 * there was memory for its headers.
 */
static bool reply_begin(struct reply *r, struct mg_connection *c, size_t max)
{
	memset(r, 0, sizeof(*r));
	r->c = c;
	r->start = c->send.len;
	r->b.buf = (char *)c->send.buf;
	r->b.cap = c->send.size;
	r->b.len = c->send.len;
	r->b.max = r->start + 256 + max + 1;
	r->b.grow = send_grow;
	r->b.ctx = r;
	pv_raw(&r->b, "HTTP/1.1 200 OK\r\n"
		      "Content-Type: application/json\r\n"
		      "Cache-Control: no-store\r\n"
		      "Content-Length: ");
	r->length = r->b.len;
	pv_raw(&r->b, "          \r\n\r\n");
	r->body = r->b.len;
	r->b.max = r->body + max + 1;
	return !r->b.overflow;
}

/* Take the reply back: the send buffer as it was before it. */
static void reply_drop(struct reply *r)
{
	struct mg_connection *c = r->c;

	c->send.len = r->start;
	if (!c->send.len)
		mg_iobuf_free(&c->send);
}

/* End the reply: its length in, and it goes. */
static void reply_end(struct reply *r)
{
	char n[LENGTH_DIGITS + 1];

	snprintf(n, sizeof(n), "%-*lu", LENGTH_DIGITS,
		 (unsigned long)(r->b.len - r->body));
	memcpy(r->b.buf + r->length, n, LENGTH_DIGITS);
	r->c->send.len = r->b.len;
	r->c->is_resp = 0;
}

/* A reply of an error: {"error": @what, "detail": @why}. */
static void reply_err(struct mg_connection *c, int code, const char *what,
		      const char *why)
{
	char body[512];
	struct prov_buf b = { .buf = body, .cap = sizeof(body) };

	pv_open(&b, '{');
	pv_key(&b, "error");
	pv_str(&b, what);
	pv_comma(&b);
	pv_key(&b, "detail");
	pv_str(&b, why);
	pv_close(&b, '}');
	mg_http_reply(c, code,
		      "Content-Type: application/json\r\n"
		      "Cache-Control: no-store\r\n",
		      "%s", body);
}

/*
 * Write the value of @p into @r, from @since on with @from, as at most
 * VALUE_MAX bytes: 0, or a negative errno with why in @why, -E2BIG for a
 * value too large and -ENOMEM for one there was no memory for, which the
 * daemon's log has too. What was written of a value that failed is taken
 * back.
 */
static int value_of(struct reply *r, const struct prov *p,
		    const struct prov_from *from, unsigned long since,
		    char *why, size_t whycap)
{
	struct prov_buf *b = &r->b;
	size_t mark = b->len, max = b->max;
	int err;

	/* What overflowed before the value is the reply's, and stays so. */
	if (b->overflow) {
		snprintf(why, whycap, "the reply is too large");
		return -E2BIG;
	}
	if (mark + VALUE_MAX + 1 < max)
		b->max = mark + VALUE_MAX + 1;
	r->nomem = false;
	err = from ? from->read(b, since) : p->read(b);
	if (b->overflow && r->nomem) {
		err = -ENOMEM;
		snprintf(why, whycap, "no memory for the value");
	} else if (b->overflow && b->max == max) {
		err = -E2BIG;
		snprintf(why, whycap, "the reply would be larger than %u bytes",
			 VALUE_REPLY_MAX);
	} else if (b->overflow) {
		err = -E2BIG;
		snprintf(why, whycap, "the value is larger than %u bytes", VALUE_MAX);
	} else if (err) {
		snprintf(why, whycap, "%s", strerror(-err));
	}
	b->max = max;
	if (!err)
		return 0;
	/* A value cut short is never served, and never quietly. */
	if (err == -E2BIG || err == -ENOMEM)
		fprintf(stderr, "pond: %s: not served: %s\n", p->id, why);
	b->len = mark;
	b->overflow = false;
	if (b->cap)
		b->buf[mark] = '\0';
	return err;
}

void value_get(struct mg_connection *c, const struct prov *p,
	       const struct prov_from *from, unsigned long since)
{
	struct reply r;
	char why[128];

	if (!reply_begin(&r, c, VALUE_MAX + 256)) {
		reply_drop(&r);
		reply_err(c, 500, p->id, "no memory for the reply");
		return;
	}
	pv_open(&r.b, '{');
	pv_key(&r.b, "id");
	pv_str(&r.b, p->id);
	pv_comma(&r.b);
	pv_key(&r.b, "value");
	if (value_of(&r, p, from, since, why, sizeof(why))) {
		reply_drop(&r);
		reply_err(c, 500, p->id, why);
		return;
	}
	pv_close(&r.b, '}');
	if (r.b.overflow) {
		reply_drop(&r);
		reply_err(c, 500, p->id, r.nomem ? "no memory for the reply" :
						  "the reply is too large");
		return;
	}
	reply_end(&r);
}

void value_getm(struct mg_connection *c, struct mg_str ids)
{
	const char *s = ids.buf, *end = ids.buf + ids.len;
	struct reply r;
	bool first = true;

	if (!reply_begin(&r, c, VALUE_REPLY_MAX)) {
		reply_drop(&r);
		reply_err(c, 500, "reply too large", "no memory for the reply");
		return;
	}
	pv_open(&r.b, '{');
	while (s < end) {
		const char *comma = memchr(s, ',', (size_t)(end - s));
		size_t n = comma ? (size_t)(comma - s) : (size_t)(end - s);
		char name[64], why[128];
		const struct prov *p;
		int err;

		if (!n || n >= sizeof(name)) {
			s = comma ? comma + 1 : end;
			continue;
		}
		memcpy(name, s, n);
		name[n] = '\0';
		s = comma ? comma + 1 : end;
		p = prov_find(name);
		if (!first)
			pv_comma(&r.b);
		first = false;
		pv_key(&r.b, name);
		if (!prov_present(p) || !p->read) {
			pv_null(&r.b);
			continue;
		}
		if (p->flags & PROV_SLOW) {
			pv_raw(&r.b, "{\"error\":\"slow: ask for it alone\"}");
			continue;
		}
		err = value_of(&r, p, NULL, 0, why, sizeof(why));
		if (err) {
			pv_open(&r.b, '{');
			pv_key(&r.b, "error");
			pv_str(&r.b, err == -E2BIG ? "too large" : "unreadable");
			pv_comma(&r.b);
			pv_key(&r.b, "detail");
			pv_str(&r.b, why);
			pv_close(&r.b, '}');
		}
	}
	pv_close(&r.b, '}');
	if (r.b.overflow) {
		reply_drop(&r);
		reply_err(c, 500, "reply too large",
			  r.nomem ? "no memory for the reply" : "ask for fewer ids");
		return;
	}
	reply_end(&r);
}

void value_schema(struct mg_connection *c)
{
	struct reply r;
	bool first = true;

	/* A reply that has no memory for its headers overflows, which its end tells. */
	reply_begin(&r, c, VALUE_MAX);
	pv_open(&r.b, '{');
	pv_key(&r.b, "items");
	pv_open(&r.b, '[');
	for (size_t i = 0; i < pond_nprovs; i++) {
		const struct prov *p = &pond_provs[i];

		if (!prov_present(p))
			continue;
		if (!first)
			pv_comma(&r.b);
		first = false;
		pv_open(&r.b, '{');
		pv_key(&r.b, "id");
		pv_str(&r.b, p->id);
		pv_comma(&r.b);
		pv_key(&r.b, "cat");
		pv_str(&r.b, p->cat);
		pv_comma(&r.b);
		pv_key(&r.b, "label");
		pv_str(&r.b, p->label);
		pv_comma(&r.b);
		pv_key(&r.b, "readable");
		pv_bool(&r.b, p->read != NULL);
		pv_comma(&r.b);
		pv_key(&r.b, "writable");
		pv_bool(&r.b, p->write != NULL);
		pv_comma(&r.b);

		pv_key(&r.b, "volatile");
		pv_bool(&r.b, (p->flags & PROV_VOLATILE) != 0);
		pv_comma(&r.b);
		pv_key(&r.b, "slow");
		pv_bool(&r.b, (p->flags & PROV_SLOW) != 0);
		pv_close(&r.b, '}');
	}
	pv_close(&r.b, ']');
	pv_close(&r.b, '}');

	if (r.b.overflow) {
		reply_drop(&r);
		reply_err(c, 500, "schema too large",
			  "the reply buffer is smaller than the table");
		return;
	}
	reply_end(&r);
}

void value_sent(struct mg_connection *c)
{
	if (!c->send.len && c->send.size > VALUE_KEEP)
		mg_iobuf_free(&c->send);
}
