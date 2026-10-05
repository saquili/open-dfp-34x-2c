// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * pond: the HTTP status daemon of the ONU. It serves, as JSON:
 *
 *   /api/health      that it runs, without a token
 *   /api/schema      the list of values
 *   /api/get/ID      one value
 *   /api/getm?ids=A,B several values at once, slow ones left out
 *   /api/set/ID      a POST: change a value to the body of the request
 *
 * Every route but the first needs the bearer token of /etc/pond.token;
 * without the file, the API stays locked. At / it serves the status page,
 * which asks for the token, reads the API and changes the settings.
 */

#include "prov.h"
#include "providers.h"

#include "mongoose.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POND_VALUE_CAP (64 * 1024)
#define POND_REPLY_CAP (96 * 1024)
#define POND_SET_CAP 4096

/*
 * The addresses the daemon listens on, one per -l; with none, every address
 * of the stick.
 */
#define POND_LISTEN_MAX 4
static const char *g_listen[POND_LISTEN_MAX];
static int g_nlisten;
static const char *g_tokenf = "/etc/pond.token";
static const char *g_page = "/usr/share/pond/index.html";
static char g_token[128];
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void reply_json(struct mg_connection *c, int code, const char *body)
{
	mg_http_reply(c, code,
		      "Content-Type: application/json\r\n"
		      "Cache-Control: no-store\r\n",
		      "%s", body);
}

static void reply_err(struct mg_connection *c, int code, const char *what,
		      const char *why)
{
	char body[512];
	struct prov_buf b = { body, sizeof(body), 0, false };

	pv_open(&b, '{');
	pv_key(&b, "error");
	pv_str(&b, what);
	pv_comma(&b);
	pv_key(&b, "detail");
	pv_str(&b, why);
	pv_close(&b, '}');
	reply_json(c, code, body);
}

static void api_schema(struct mg_connection *c)
{
	static char body[POND_REPLY_CAP];
	struct prov_buf b = { body, sizeof(body), 0, false };
	size_t i;
	bool first = true;

	pv_open(&b, '{');
	pv_key(&b, "items");
	pv_open(&b, '[');
	for (i = 0; i < pond_nprovs; i++) {
		const struct prov *p = &pond_provs[i];

		if (!prov_present(p))
			continue;
		if (!first)
			pv_comma(&b);
		first = false;
		pv_open(&b, '{');
		pv_key(&b, "id");
		pv_str(&b, p->id);
		pv_comma(&b);
		pv_key(&b, "cat");
		pv_str(&b, p->cat);
		pv_comma(&b);
		pv_key(&b, "label");
		pv_str(&b, p->label);
		pv_comma(&b);
		pv_key(&b, "readable");
		pv_bool(&b, p->read != NULL);
		pv_comma(&b);
		pv_key(&b, "writable");
		pv_bool(&b, p->write != NULL);
		pv_comma(&b);

		pv_key(&b, "volatile");
		pv_bool(&b, (p->flags & PROV_VOLATILE) != 0);
		pv_comma(&b);
		pv_key(&b, "slow");
		pv_bool(&b, (p->flags & PROV_SLOW) != 0);
		pv_close(&b, '}');
	}
	pv_close(&b, ']');
	pv_close(&b, '}');

	if (b.overflow) {
		reply_err(c, 500, "schema too large",
			  "the reply buffer is smaller than the table");
		return;
	}
	reply_json(c, 200, body);
}

static void api_get(struct mg_connection *c, struct mg_str id)
{
	static char body[POND_VALUE_CAP + 256];
	static char val[POND_VALUE_CAP];
	char name[64];
	const struct prov *p;
	struct prov_buf v = { val, sizeof(val), 0, false };
	struct prov_buf b = { body, sizeof(body), 0, false };
	int err;

	if (id.len >= sizeof(name)) {
		reply_err(c, 404, "no such item",
			  "the name is too long to be one");
		return;
	}
	memcpy(name, id.buf, id.len);
	name[id.len] = '\0';

	p = prov_find(name);
	if (!prov_present(p) || !p->read) {
		reply_err(c, 404, "no such item", name);
		return;
	}

	err = p->read(&v);
	if (err) {
		char why[128];

		snprintf(why, sizeof(why), "%s", strerror(-err));
		reply_err(c, 500, name, why);
		return;
	}
	if (v.overflow) {
		reply_err(c, 500, name, "the value did not fit in the buffer");
		return;
	}

	pv_open(&b, '{');
	pv_key(&b, "id");
	pv_str(&b, name);
	pv_comma(&b);
	pv_key(&b, "value");
	pv_raw(&b, val);
	pv_close(&b, '}');
	reply_json(c, b.overflow ? 500 : 200,
		   b.overflow ? "{\"error\":\"reply too large\"}" : body);
}

static void api_getm(struct mg_connection *c, struct mg_str query)
{
	static char body[POND_REPLY_CAP];
	static char val[POND_VALUE_CAP];
	struct prov_buf b = { body, sizeof(body), 0, false };
	struct mg_str ids = mg_http_var(query, mg_str("ids"));
	const char *s, *end;
	bool first = true;

	if (!ids.len) {
		reply_err(c, 400, "no ids", "ask for ids=a,b,c");
		return;
	}

	pv_open(&b, '{');
	s = ids.buf;
	end = ids.buf + ids.len;
	while (s < end) {
		const char *comma = memchr(s, ',', (size_t)(end - s));
		size_t n = comma ? (size_t)(comma - s) : (size_t)(end - s);
		char name[64];
		const struct prov *p;
		struct prov_buf v = { val, sizeof(val), 0, false };

		if (n && n < sizeof(name)) {
			memcpy(name, s, n);
			name[n] = '\0';

			p = prov_find(name);
			if (!first)
				pv_comma(&b);
			first = false;
			pv_key(&b, name);

			if (!prov_present(p) || !p->read)
				pv_null(&b);
			else if (p->flags & PROV_SLOW)
				pv_raw(&b,
				       "{\"error\":\"slow: ask for it alone\"}");
			else if (p->read(&v) || v.overflow)
				pv_raw(&b, "{\"error\":\"unreadable\"}");
			else
				pv_raw(&b, val);
		}
		s = comma ? comma + 1 : end;
	}
	pv_close(&b, '}');

	if (b.overflow) {
		reply_err(c, 500, "reply too large", "ask for fewer ids");
		return;
	}
	reply_json(c, 200, body);
}

static void api_set(struct mg_connection *c, struct mg_http_message *hm,
		    struct mg_str id)
{
	static char val[POND_SET_CAP];
	char name[64], err[256] = "";
	const struct prov *p;
	int rc;

	if (mg_strcmp(hm->method, mg_str("POST"))) {
		reply_err(c, 405, "use POST", "a change is a POST of the new value");
		return;
	}
	if (id.len >= sizeof(name)) {
		reply_err(c, 404, "no such item",
			  "the name is too long to be one");
		return;
	}
	memcpy(name, id.buf, id.len);
	name[id.len] = '\0';

	p = prov_find(name);
	if (!prov_present(p) || !p->write) {
		reply_err(c, 404, "no such setting", name);
		return;
	}
	if (hm->body.len >= sizeof(val) ||
	    (hm->body.len && memchr(hm->body.buf, '\0', hm->body.len))) {
		reply_err(c, 400, name, "the value is too long, or not text");
		return;
	}
	memcpy(val, hm->body.buf, hm->body.len);
	val[hm->body.len] = '\0';

	rc = p->write(val, err, sizeof(err));
	explicit_bzero(val, sizeof(val));
	if (rc) {
		reply_err(c, rc == -EBUSY ? 409 : rc == -EINVAL ? 400 : 500, name,
			  *err ? err : strerror(-rc));
		return;
	}
	reply_json(c, 200, "{\"ok\":true}");
}

/* Whether the request carries the token, compared in constant time. */
static bool authorised(struct mg_http_message *hm)
{
	const struct mg_str *h = mg_http_get_header(hm, "Authorization");
	size_t n = strlen(g_token);

	if (!n)
		return false;
	if (!h || h->len != n + 7 || strncmp(h->buf, "Bearer ", 7))
		return false;

	{
		size_t i;
		unsigned char diff = 0;

		for (i = 0; i < n; i++)
			diff |= (unsigned char)(h->buf[7 + i] ^ g_token[i]);
		return diff == 0;
	}
}

static void handler(struct mg_connection *c, int ev, void *ev_data)
{
	struct mg_http_message *hm = (struct mg_http_message *)ev_data;
	struct mg_str caps[2];

	if (ev != MG_EV_HTTP_MSG)
		return;

	if (mg_match(hm->uri, mg_str("/api/health"), NULL)) {
		reply_json(c, 200, "{\"ok\":true}");
		return;
	}

	if (mg_match(hm->uri, mg_str("/"), NULL) ||
	    mg_match(hm->uri, mg_str("/index.html"), NULL)) {
		struct mg_http_serve_opts opts = {
			.mime_types = "html=text/html; charset=utf-8",
			.extra_headers =
				"Cache-Control: no-store\r\n"
				"X-Content-Type-Options: nosniff\r\n"
				"Content-Security-Policy: default-src 'none'; "
				"script-src 'unsafe-inline'; "
				"style-src 'unsafe-inline'; connect-src 'self'\r\n",
		};

		mg_http_serve_file(c, hm, g_page, &opts);
		return;
	}

	if (!mg_match(hm->uri, mg_str("/api/#"), NULL)) {
		reply_err(c, 404, "no such route",
			  "this daemon serves / and /api");
		return;
	}

	if (!authorised(hm)) {
		reply_err(
			c, 401, "unauthorised",
			strlen(g_token) ?
				"present a bearer token" :
				"this daemon has no token file, so the API is locked");
		return;
	}

	if (mg_match(hm->uri, mg_str("/api/schema"), NULL))
		api_schema(c);
	else if (mg_match(hm->uri, mg_str("/api/getm"), NULL))
		api_getm(c, hm->query);
	else if (mg_match(hm->uri, mg_str("/api/get/*"), caps))
		api_get(c, caps[0]);
	else if (mg_match(hm->uri, mg_str("/api/set/*"), caps))
		api_set(c, hm, caps[0]);
	else
		reply_err(c, 404, "no such route", "try /api/schema");
}

static void load_token(void)
{
	size_t i;

	g_token[0] = '\0';
	if (prov_read_line(g_tokenf, g_token, sizeof(g_token)))
		return;

	for (i = 0; g_token[i]; i++)
		if (g_token[i] == '\n' || g_token[i] == '\r' ||
		    g_token[i] == ' ') {
			g_token[i] = '\0';
			break;
		}
}

int main(int argc, char **argv)
{
	struct mg_mgr mgr;
	int listening = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-l") && i + 1 < argc &&
		    g_nlisten < POND_LISTEN_MAX)
			g_listen[g_nlisten++] = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			g_tokenf = argv[++i];
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			g_page = argv[++i];
		else {
			fprintf(stderr,
				"usage: %s [-l http://addr:port]... [-t tokenfile] [-p page]\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (!g_nlisten)
		g_listen[g_nlisten++] = "http://0.0.0.0:8080";

	load_token();
	mg_mgr_init(&mgr);
	/*
	 * An address that is missing costs only its own listener: the others
	 * still serve, and only when none does is there no daemon.
	 */
	for (i = 0; i < g_nlisten; i++) {
		if (!mg_http_listen(&mgr, g_listen[i], handler, NULL)) {
			fprintf(stderr, "pond: cannot listen on %s\n",
				g_listen[i]);
			continue;
		}
		fprintf(stderr, "pond: listening on %s\n", g_listen[i]);
		listening++;
	}
	if (!listening)
		return 1;
	fprintf(stderr, "pond: %s\n",
		strlen(g_token) ? "token loaded" :
				  "NO TOKEN -- the API is locked");

	while (!g_stop) {
		mg_mgr_poll(&mgr, 200);
		prov_settings_reap();
	}

	mg_mgr_free(&mgr);
	return 0;
}
