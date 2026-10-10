// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * pond: the HTTP daemon of the ONU. It serves, as JSON:
 *
 *   /api/health        that it runs, to anyone
 *   /api/auth/login    a POST of {user, secret}: a session, in a cookie
 *   /api/auth/session  who is signed in
 *   /api/auth/logout   a POST: end the session
 *   /api/auth/password a POST of {current, next}: a new password
 *   /api/schema        the list of values
 *   /api/get/ID        one value
 *   /api/getm?ids=A,B  several values at once, slow ones left out
 *   /api/set/ID        a POST: change a value to the body of the request
 *   /api/set/changes   a POST: keep the changes on trial, or take them back
 *                      (changes.c)
 *   /api/system/restart a POST: restart the stick
 *   /api/firmware/upload   a POST of an update file: into the other bank
 *   /api/firmware/download the running bank as an update file
 *   /api/firmware/try      a POST: start a bank once, by a restart
 *                          (firmware.c)
 *
 * Every other route of /api needs a session (auth.h) or the bearer token of
 * /etc/pond.token, for scripts. A change made in a session must carry the
 * header X-Pond, which a page of another site cannot add, and while the
 * default password is in force a session may only change it. Every other
 * path is the panel: the static files of the web directory, the hashed
 * assets cached for good, and index.html for any path of the app.
 */

#include "auth.h"
#include "bus.h"
#include "changes.h"
#include "firmware.h"
#include "mgmt.h"
#include "prov.h"
#include "providers.h"

#include "mongoose.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
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
static const char *g_web = "/usr/share/pond/www";
static char g_token[128];
static volatile sig_atomic_t g_stop;
static time_t g_restart_at;

#define SECURITY_HEADERS                                                   \
	"X-Content-Type-Options: nosniff\r\n"                               \
	"X-Frame-Options: DENY\r\n"                                         \
	"Referrer-Policy: no-referrer\r\n"

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

/* The session id of the request's cookie, or an empty string. */
static void cookie_session(struct mg_http_message *hm, char id[AUTH_ID_LEN + 1])
{
	const struct mg_str *h = mg_http_get_header(hm, "Cookie");
	struct mg_str v;

	id[0] = '\0';
	if (!h)
		return;
	v = mg_http_get_header_var(*h, mg_str("pond_sid"));
	if (v.len == AUTH_ID_LEN) {
		memcpy(id, v.buf, AUTH_ID_LEN);
		id[AUTH_ID_LEN] = '\0';
	}
}

/* Secure when the router's proxy says the browser came in over HTTPS. */
static const char *cookie_secure(struct mg_http_message *hm)
{
	const struct mg_str *p = mg_http_get_header(hm, "X-Forwarded-Proto");

	return p && !mg_strcmp(*p, mg_str("https")) ? "; Secure" : "";
}

static void reply_session(struct mg_connection *c, const char *cookie)
{
	char body[160], headers[256];

	snprintf(body, sizeof(body),
		 "{\"user\":\"%s\",\"must_change\":%s,\"idle_timeout_s\":%d}",
		 AUTH_USER, auth_default_password() ? "true" : "false",
		 AUTH_IDLE_S);
	/* mg_http_reply() takes the headers as they are, not as a format. */
	snprintf(headers, sizeof(headers),
		 "Content-Type: application/json\r\nCache-Control: no-store\r\n%s",
		 cookie ? cookie : "");
	mg_http_reply(c, 200, headers, "%s", body);
}

static void reply_retry(struct mg_connection *c, int seconds)
{
	char body[128];

	char headers[128];

	snprintf(body, sizeof(body),
		 "{\"error\":\"too many attempts\",\"retry_after\":%d}",
		 seconds);
	snprintf(headers, sizeof(headers),
		 "Content-Type: application/json\r\nCache-Control: no-store\r\n"
		 "Retry-After: %d\r\n",
		 seconds);
	mg_http_reply(c, 429, headers, "%s", body);
}

/* A string member of a JSON body, or NULL; the caller frees it. */
static char *json_str(struct mg_http_message *hm, const char *path)
{
	return mg_json_get_str(hm->body, path);
}

static void free_secret(char *s)
{
	if (s) {
		explicit_bzero(s, strlen(s));
		free(s);
	}
}

static void api_login(struct mg_connection *c, struct mg_http_message *hm)
{
	char *user = json_str(hm, "$.user");
	char *secret = json_str(hm, "$.secret");
	char id[AUTH_ID_LEN + 1], cookie[160];
	int retry = 0;
	int rc = auth_sign_in(user, secret, time(NULL), id, &retry);

	free(user);
	free_secret(secret);
	if (rc == -EAGAIN) {
		reply_retry(c, retry);
		return;
	}
	if (rc == -EACCES) {
		reply_err(c, 401, "wrong user or password", "try again");
		return;
	}
	if (rc) {
		reply_err(c, 500, "no session", strerror(-rc));
		return;
	}
	snprintf(cookie, sizeof(cookie),
		 "Set-Cookie: pond_sid=%s; Path=/; HttpOnly; SameSite=Strict%s\r\n",
		 id, cookie_secure(hm));
	reply_session(c, cookie);
}

static void api_password(struct mg_connection *c, struct mg_http_message *hm,
			 const char *sid)
{
	char *current = json_str(hm, "$.current");
	char *next = json_str(hm, "$.next");
	int retry = 0;
	int rc = auth_change(current, next, sid, time(NULL), &retry);

	free_secret(current);
	free_secret(next);
	if (rc == -EAGAIN)
		reply_retry(c, retry);
	else if (rc == -EACCES)
		reply_err(c, 403, "wrong password", "the current password is another");
	else if (rc == -EINVAL)
		reply_err(c, 400, "not a password",
			  "8 to 128 printable characters, other than the current one");
	else if (rc)
		reply_err(c, 500, "not stored", strerror(-rc));
	else
		reply_session(c, NULL);
}

/* Restart the stick a moment after the reply has gone. */
static void api_restart(struct mg_connection *c)
{
	g_restart_at = time(NULL) + 1;
	reply_json(c, 200, "{\"ok\":true}");
}

static void restart_when_due(void)
{
	char out[64];

	if (!g_restart_at || time(NULL) < g_restart_at)
		return;
	g_restart_at = 0;
	sync();
	if (bus_act("restart", NULL, NULL, 0, NULL, out, sizeof(out)) < 0)
		fprintf(stderr, "pond: onu-act did not restart the stick\n");
}

/* Whether a path climbs out of its directory. */
static bool dotdot(struct mg_str s)
{
	size_t i;

	for (i = 0; i + 1 < s.len; i++)
		if (s.buf[i] == '.' && s.buf[i + 1] == '.')
			return true;
	return false;
}

/*
 * The panel. A hashed asset is cached for good; any other path is the app,
 * index.html, which the browser must ask for again every time.
 */
static void serve_web(struct mg_connection *c, struct mg_http_message *hm)
{
	char path[512];
	struct mg_http_serve_opts opts = {
		.mime_types = "html=text/html; charset=utf-8,js=text/javascript,"
			      "css=text/css,svg=image/svg+xml",
	};

	if (mg_match(hm->uri, mg_str("/assets/*"), NULL) &&
	    !memchr(hm->uri.buf, '%', hm->uri.len) && !dotdot(hm->uri) &&
	    hm->uri.len < 200) {
		snprintf(path, sizeof(path), "%s%.*s", g_web, (int)hm->uri.len,
			 hm->uri.buf);
		opts.extra_headers = "Cache-Control: public, max-age=31536000, immutable\r\n"
			SECURITY_HEADERS;
		mg_http_serve_file(c, hm, path, &opts);
		return;
	}
	snprintf(path, sizeof(path), "%s/index.html", g_web);
	opts.extra_headers =
		"Cache-Control: no-store\r\n" SECURITY_HEADERS
		"Content-Security-Policy: default-src 'self'; script-src 'self'; "
		"style-src 'self'; img-src 'self' data:; connect-src 'self'; "
		"frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n";
	mg_http_serve_file(c, hm, path, &opts);
}

/*
 * Whether an upload whose headers are in may go on: a session or the token,
 * X-Pond on a session's change, and a password of the person's own. Otherwise
 * it answers, and the connection closes rather than take the body.
 */
static bool upload_admitted(struct mg_connection *c, struct mg_http_message *hm)
{
	char sid[AUTH_ID_LEN + 1];
	bool session;

	cookie_session(hm, sid);
	session = auth_session(sid, time(NULL));
	if (!session && !authorised(hm))
		reply_err(c, 401, "unauthorised", "sign in, or present a bearer token");
	else if (session && !mg_http_get_header(hm, "X-Pond"))
		reply_err(c, 403, "refused", "a change needs the X-Pond header");
	else if (session && auth_default_password())
		reply_err(c, 403, "must_change", "set a password of your own first");
	else
		return true;
	c->is_draining = 1;
	return false;
}

static void handler(struct mg_connection *c, int ev, void *ev_data)
{
	struct mg_http_message *hm = (struct mg_http_message *)ev_data;
	struct mg_str caps[2];

	/* An upload is taken as its headers come, before its body. */
	if (ev == MG_EV_HTTP_HDRS) {
		if (mg_match(hm->uri, mg_str("/api/firmware/upload"), NULL) &&
		    !mg_strcmp(hm->method, mg_str("POST")) && upload_admitted(c, hm))
			firmware_upload(c, hm);
		return;
	}
	if (ev != MG_EV_HTTP_MSG)
		return;

	if (mg_match(hm->uri, mg_str("/api/health"), NULL)) {
		reply_json(c, 200, "{\"ok\":true}");
		return;
	}

	if (!mg_match(hm->uri, mg_str("/api/#"), NULL)) {
		serve_web(c, hm);
		return;
	}

	bool post = !mg_strcmp(hm->method, mg_str("POST"));
	char sid[AUTH_ID_LEN + 1];

	if (mg_match(hm->uri, mg_str("/api/auth/login"), NULL)) {
		if (post)
			api_login(c, hm);
		else
			reply_err(c, 405, "use POST", "sign in with a POST");
		return;
	}

	cookie_session(hm, sid);
	bool session = auth_session(sid, time(NULL));
	bool token = !session && authorised(hm);

	if (!session && !token) {
		reply_err(c, 401, "unauthorised", "sign in, or present a bearer token");
		return;
	}
	/* A change made from a browser comes from this panel, not another site. */
	if (session && post && !mg_http_get_header(hm, "X-Pond")) {
		reply_err(c, 403, "refused", "a change needs the X-Pond header");
		return;
	}

	if (mg_match(hm->uri, mg_str("/api/auth/session"), NULL)) {
		reply_session(c, NULL);
		return;
	}
	if (mg_match(hm->uri, mg_str("/api/auth/logout"), NULL)) {
		auth_sign_out(sid);
		mg_http_reply(c, 200,
			      "Content-Type: application/json\r\n"
			      "Set-Cookie: pond_sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n",
			      "{}");
		return;
	}
	if (mg_match(hm->uri, mg_str("/api/auth/password"), NULL)) {
		if (post)
			api_password(c, hm, sid);
		else
			reply_err(c, 405, "use POST", "change the password with a POST");
		return;
	}
	if (session && auth_default_password()) {
		reply_err(c, 403, "must_change", "set a password of your own first");
		return;
	}

	if (mg_match(hm->uri, mg_str("/api/system/restart"), NULL)) {
		if (post)
			api_restart(c);
		else
			reply_err(c, 405, "use POST", "restart with a POST");
	} else if (mg_match(hm->uri, mg_str("/api/firmware/upload"), NULL)) {
		reply_err(c, 405, "use POST", "upload an update file with a POST");
	} else if (mg_match(hm->uri, mg_str("/api/firmware/download"), NULL)) {
		firmware_download(c);
	} else if (mg_match(hm->uri, mg_str("/api/firmware/try"), NULL)) {
		if (!post)
			reply_err(c, 405, "use POST", "start a bank with a POST");
		else if (firmware_try(c, hm))
			g_restart_at = time(NULL) + 1;
	} else if (mg_match(hm->uri, mg_str("/api/schema"), NULL))
		api_schema(c);
	else if (mg_match(hm->uri, mg_str("/api/getm"), NULL))
		api_getm(c, hm->query);
	else if (mg_match(hm->uri, mg_str("/api/get/*"), caps))
		api_get(c, caps[0]);
	else if (mg_match(hm->uri, mg_str("/api/set/mgmt"), NULL) && post)
		mgmt_set(c, hm, handler);
	else if (mg_match(hm->uri, mg_str("/api/set/changes"), NULL) && post)
		changes_set(c, hm);
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
		else if (!strcmp(argv[i], "-w") && i + 1 < argc)
			g_web = argv[++i];
		else {
			fprintf(stderr,
				"usage: %s [-l http://addr:port]... [-t tokenfile] [-w webdir]\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (!g_nlisten)
		g_listen[g_nlisten++] = "http://0.0.0.0:8080";

	/*
	 * The panel waits; the stick's own work does not. A slow page is
	 * better than a late answer to the OLT.
	 */
	errno = 0;
	if (nice(10) == -1 && errno)
		fprintf(stderr, "pond: cannot lower its priority: %s\n", strerror(errno));

	load_token();
	auth_init(getenv("POND_AUTH_FILE"), getenv("POND_STORE"));
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
	/* A change of the management pair that waits: its address answers too. */
	{
		const char *colon = strrchr(g_listen[0], ':');

		mgmt_resume(&mgr, colon ? colon + 1 : "8080", handler);
	}
	fprintf(stderr, "pond: %s; %s\n",
		strlen(g_token) ? "token loaded" : "no token for scripts",
		auth_default_password() ? "the default password is in force" :
					  "a password is set");

	while (!g_stop) {
		mg_mgr_poll(&mgr, 200);
		prov_settings_reap();
		firmware_poll();
		restart_when_due();
	}

	mg_mgr_free(&mgr);
	return 0;
}
