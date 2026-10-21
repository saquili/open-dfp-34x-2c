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
 *   /api/auth/sessions the sessions open, without their ids
 *   /api/auth/sessions/others  a POST, with root's password: end all but
 *                      the one that asks
 *   /api/token         whether a token of scripts is in force
 *   /api/schema        the list of values
 *   /api/get/ID        one value; ?since=N from a point on, for a log
 *   /api/getm?ids=A,B  several values at once, slow ones left out
 *   /api/set/ID        a POST: change a value to the body of the request
 *   /api/set/changes   a POST: keep the changes on trial, or take them back
 *                      (changes.c)
 *   /api/identity/olt-login a POST, with root's password: the PPPoE login
 *                      the OLT sent, its password with it
 *   /api/iptv/test     a POST: a group of television joined from the stick
 *                      for a few seconds, and what came of it (iptv.h)
 *   /api/system/restart a POST: restart the stick
 *   /api/firmware/upload   a POST of an update file: into the other bank
 *   /api/firmware/download the running bank as an update file
 *   /api/firmware/try      a POST: start a bank once, by a restart
 *   /api/firmware/return   a POST: start the shipped firmware from now on,
 *                          by a restart
 *   /api/firmware/backup   the whole flash (firmware.c)
 *   /api/ssh/...           POSTs: the ssh server's host key, the keys root
 *                          logs in with, and whether it takes them alone
 *   /api/tls/...           POSTs: the panel's key and an own certificate
 *   /api/token/new, /off   POSTs: a new token of scripts, or none (keys.c)
 *
 * Over HTTPS (tls.h), a request that comes over HTTP is sent on to the same
 * path over HTTPS, but for /api/health. Every other route of /api needs a
 * session (auth.h) or the bearer token of the file of -t, for scripts: the
 * token that onu-config keeps in the stick's store. A change made in a session
 * must carry the header X-Pond, which a page of another site cannot add, and
 * while the default password is in force a session may only change it. Every
 * other path is the panel: the static files of the web directory, the hashed
 * assets cached for good, and index.html for any path of the app.
 */

#include "auth.h"
#include "bus.h"
#include "changes.h"
#include "firmware.h"
#include "iptv.h"
#include "keys.h"
#include "later.h"
#include "mgmt.h"
#include "prov.h"
#include "providers.h"
#include "tls.h"
#include "value.h"

#include "mongoose.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <grp.h>
#include <linux/capability.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#define POND_SET_CAP 4096

/*
 * The addresses the daemon listens on, one per -l; with none, every address
 * of the stick.
 */
#define POND_LISTEN_MAX 4
static const char *g_listen[POND_LISTEN_MAX];
static int g_nlisten;
static const char *g_tokenf;
static const char *g_keyf;
static const char *g_ownf;
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
	struct prov_buf b = { .buf = body, .cap = sizeof(body) };

	pv_open(&b, '{');
	pv_key(&b, "error");
	pv_str(&b, what);
	pv_comma(&b);
	pv_key(&b, "detail");
	pv_str(&b, why);
	pv_close(&b, '}');
	reply_json(c, code, body);
}

static void api_get(struct mg_connection *c, struct mg_str id,
		    struct mg_str query)
{
	char name[64];
	const struct prov *p;
	struct mg_str since = mg_http_var(query, mg_str("since"));
	const struct prov_from *from = NULL;
	unsigned long point = 0;

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

	if (prov_find_from(name) && since.len) {
		char num[16];

		snprintf(num, sizeof(num), "%.*s", (int)since.len, since.buf);
		from = prov_find_from(name);
		point = strtoul(num, NULL, 10);
	}
	value_get(c, p, from, point);
}

static void api_getm(struct mg_connection *c, struct mg_str query)
{
	struct mg_str ids = mg_http_var(query, mg_str("ids"));

	if (!ids.len) {
		reply_err(c, 400, "no ids", "ask for ids=a,b,c");
		return;
	}
	value_getm(c, ids);
}

/*
 * A request answered once its change has ended (later.h): the id of its
 * connection, and what the answer names.
 */
struct later {
	unsigned long id;
	char name[64];
	char secure[16];
};

static struct mg_mgr *g_mgr;

static struct later *later_new(struct mg_connection *c)
{
	struct later *l = calloc(1, sizeof(*l));

	if (l)
		l->id = c->id;
	return l;
}

/* The answer to a change of @name that ended with @rc. */
static void set_reply(struct mg_connection *c, const char *name, int rc,
		      const char *err)
{
	if (rc)
		reply_err(c, rc == -EBUSY ? 409 : rc == -EINVAL ? 400 : 500, name,
			  err && *err ? err : strerror(-rc));
	else
		reply_json(c, 200, "{\"ok\":true}");
}

static void set_done(void *ctx, int rc, const char *err)
{
	struct later *l = ctx;
	struct mg_connection *c = later_conn(g_mgr, l->id);

	if (c)
		set_reply(c, l->name, rc, err);
	free(l);
}

static void api_set(struct mg_connection *c, struct mg_http_message *hm,
		    struct mg_str id)
{
	static char val[POND_SET_CAP];
	char name[64], err[256] = "";
	const struct prov *p;
	struct later *l;
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

	l = later_new(c);
	if (!l) {
		explicit_bzero(val, sizeof(val));
		reply_err(c, 500, name, "no memory for the change");
		return;
	}
	snprintf(l->name, sizeof(l->name), "%s", name);
	rc = p->write(val, err, sizeof(err), set_done, l);
	explicit_bzero(val, sizeof(val));
	if (rc == -EINPROGRESS)
		return;
	free(l);
	set_reply(c, name, rc, err);
}

/*
 * The token of scripts, out of the file of -t, which onu-config makes, replaces
 * and removes while the daemon runs. The file is read at every request that
 * does not carry a session (a few bytes of tmpfs), so that a new token holds
 * at once and a removed one stops holding, with no restart that would end the
 * sessions.
 */
static void load_token(void)
{
	size_t i;

	g_token[0] = '\0';
	if (!g_tokenf || prov_read_line(g_tokenf, g_token, sizeof(g_token))) {
		g_token[0] = '\0';
		return;
	}
	for (i = 0; g_token[i]; i++)
		if (g_token[i] == '\n' || g_token[i] == '\r' ||
		    g_token[i] == ' ') {
			g_token[i] = '\0';
			break;
		}
}

/* Whether the request carries the token, compared in constant time. */
static bool authorised(struct mg_http_message *hm)
{
	const struct mg_str *h = mg_http_get_header(hm, "Authorization");
	size_t n;

	if (!h || h->len < 7 || strncmp(h->buf, "Bearer ", 7))
		return false;
	load_token();
	n = strlen(g_token);
	if (!n || h->len != n + 7)
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

/* Secure over HTTPS, the daemon's own or a proxy's that says so. */
static const char *cookie_secure(struct mg_connection *c, struct mg_http_message *hm)
{
	const struct mg_str *p = mg_http_get_header(hm, "X-Forwarded-Proto");

	if (c->is_tls)
		return "; Secure";
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

/* The answer to a sign-in that ended with @rc, opening session @id. */
static void login_reply(struct mg_connection *c, int rc, const char *id,
			int retry, const char *secure)
{
	char cookie[160];

	if (rc == -EAGAIN) {
		reply_retry(c, retry);
		return;
	}
	if (rc == -EACCES) {
		reply_err(c, 401, "wrong user or password", "try again");
		return;
	}
	if (rc == -ENOKEY) {
		reply_err(c, 423, "no store",
			  "the settings store cannot be read; sign in on the console");
		return;
	}
	if (rc) {
		reply_err(c, 500, "no session", strerror(-rc));
		return;
	}
	snprintf(cookie, sizeof(cookie),
		 "Set-Cookie: pond_sid=%s; Path=/; HttpOnly; SameSite=Strict%s\r\n",
		 id, secure);
	reply_session(c, cookie);
}

static void login_done(void *ctx, int rc, const char *id)
{
	struct later *l = ctx;
	struct mg_connection *c = later_conn(g_mgr, l->id);

	if (c)
		login_reply(c, rc, id, 0, l->secure);
	free(l);
}

static void api_login(struct mg_connection *c, struct mg_http_message *hm)
{
	char *user = json_str(hm, "$.user");
	char *secret = json_str(hm, "$.secret");
	struct later *l = later_new(c);
	char from[48];
	int retry = 0, rc = -ENOMEM;

	/* The address the wrong passwords are counted by (auth.h). */
	mg_snprintf(from, sizeof(from), "%M", mg_print_ip, &c->rem);
	if (l) {
		snprintf(l->secure, sizeof(l->secure), "%s", cookie_secure(c, hm));
		rc = auth_sign_in_begin(user, secret, from, time(NULL), &retry,
					login_done, l);
	}
	free(user);
	free_secret(secret);
	if (rc == -EINPROGRESS)
		return;
	login_reply(c, rc, "", retry, l ? l->secure : "");
	free(l);
}

/* The answer to a change of the password that ended with @rc. */
static void password_reply(struct mg_connection *c, int rc, int retry)
{
	if (rc == -EAGAIN)
		reply_retry(c, retry);
	else if (rc == -EACCES)
		reply_err(c, 403, "wrong password", "the current password is another");
	else if (rc == -ENOKEY)
		reply_err(c, 423, "no store",
			  "the settings store cannot be read; sign in on the console");
	else if (rc == -EINVAL)
		reply_err(c, 400, "not a password",
			  "8 to 128 printable characters, other than the current one");
	else if (rc)
		reply_err(c, 500, "not stored", strerror(-rc));
	else
		reply_session(c, NULL);
}

static void password_done(void *ctx, int rc, const char *id)
{
	struct later *l = ctx;
	struct mg_connection *c = later_conn(g_mgr, l->id);

	(void)id;
	if (c)
		password_reply(c, rc, 0);
	free(l);
}

static void api_password(struct mg_connection *c, struct mg_http_message *hm,
			 const char *sid)
{
	char *current = json_str(hm, "$.current");
	char *next = json_str(hm, "$.next");
	struct later *l = later_new(c);
	char from[48];
	int retry = 0, rc = -ENOMEM;

	mg_snprintf(from, sizeof(from), "%M", mg_print_ip, &c->rem);
	if (l)
		rc = auth_change_begin(current, next, sid, from, time(NULL), &retry,
				       password_done, l);
	free_secret(current);
	free_secret(next);
	if (rc == -EINPROGRESS)
		return;
	password_reply(c, rc, retry);
	free(l);
}

/*
 * The sessions open, the oldest first, the one that asks marked, none with
 * its id: when each was opened, its last request and the address it came
 * from; and how many there may be and how long one lasts.
 */
static void api_sessions(struct mg_connection *c, const char *sid)
{
	struct auth_open open[AUTH_SESSIONS];
	time_t now = time(NULL);
	int n = auth_list(sid, now, open);
	char body[1024];
	struct prov_buf b = { .buf = body, .cap = sizeof(body) };

	pv_open(&b, '{');
	pv_key(&b, "now_s");
	pv_num(&b, (long long)now);
	pv_comma(&b);
	pv_key(&b, "max");
	pv_num(&b, AUTH_SESSIONS);
	pv_comma(&b);
	pv_key(&b, "idle_timeout_s");
	pv_num(&b, AUTH_IDLE_S);
	pv_comma(&b);
	pv_key(&b, "max_age_s");
	pv_num(&b, AUTH_MAX_AGE_S);
	pv_comma(&b);
	pv_key(&b, "sessions");
	pv_open(&b, '[');
	for (int i = 0; i < n; i++) {
		if (i)
			pv_comma(&b);
		pv_open(&b, '{');
		pv_key(&b, "since_s");
		pv_num(&b, (long long)open[i].created);
		pv_comma(&b);
		pv_key(&b, "seen_s");
		pv_num(&b, (long long)open[i].seen);
		pv_comma(&b);
		pv_key(&b, "from");
		pv_str(&b, open[i].from);
		pv_comma(&b);
		pv_key(&b, "this");
		pv_bool(&b, open[i].self);
		pv_close(&b, '}');
	}
	pv_close(&b, ']');
	pv_close(&b, '}');
	reply_json(c, 200, body);
}

/*
 * End every session but the one that asks, with root's password again, as
 * a change of the keys takes it: whoever holds a session, a browser left
 * open, cannot shut the owner out without it.
 */
static void api_end_others(struct mg_connection *c, struct mg_http_message *hm,
			   const char *sid)
{
	char pw[AUTH_SECRET_MAX + 1], body[48];

	if (!firmware_confirmed(c, hm, pw, sizeof(pw)))
		return;
	explicit_bzero(pw, sizeof(pw));
	snprintf(body, sizeof(body), "{\"ok\":true,\"ended\":%d}",
		 auth_end_others(sid, time(NULL)));
	reply_json(c, 200, body);
}

/*
 * The PPPoE login the OLT sent, its password with it, with root's password
 * (the X-Pond-Password of the updates, firmware.h): omcid's answer as it is,
 * {"user","secret","realm"}, or 404 when the OLT sent none.
 */
static void api_olt_login(struct mg_connection *c, struct mg_http_message *hm)
{
	char pw[AUTH_SECRET_MAX + 1], text[512];
	int n;

	if (!firmware_confirmed(c, hm, pw, sizeof(pw)))
		return;
	explicit_bzero(pw, sizeof(pw));
	n = bus_call("omcid", "login", NULL, 0, text, sizeof(text));
	if (n < 0)
		reply_err(c, 503, "no omcid", "omcid does not answer");
	else if (!strstr(text, "\"user\""))
		reply_err(c, 404, "no login", "the OLT sent no PPPoE login");
	else
		reply_json(c, 200, text);
	explicit_bzero(text, sizeof(text));
}

/* Whether a token of scripts is in force; never the token. */
static void api_token(struct mg_connection *c)
{
	load_token();
	reply_json(c, 200, g_token[0] ? "{\"set\":true}" : "{\"set\":false}");
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

	if (ev == MG_EV_ACCEPT) {
		tls_accept(c);
		return;
	}
	if (ev == MG_EV_WRITE) {
		value_sent(c);
		return;
	}
	/*
	 * An upload is taken as its headers come, before its body; a request
	 * over HTTP, while there is HTTPS, is sent on before its body too.
	 */
	if (ev == MG_EV_HTTP_HDRS) {
		if (!mg_match(hm->uri, mg_str("/api/health"), NULL) && tls_redirect(c, hm))
			c->is_draining = 1;
		else if (mg_match(hm->uri, mg_str("/api/firmware/upload"), NULL) &&
			 !mg_strcmp(hm->method, mg_str("POST")) && upload_admitted(c, hm))
			firmware_upload(c, hm);
		return;
	}
	if (ev != MG_EV_HTTP_MSG || c->is_draining)
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
		/*
		 * A sign-in comes from this panel too: a form of another site
		 * can post a text that reads as JSON, which would sign the
		 * browser in, or lock the sign-ins of its address.
		 */
		if (!post)
			reply_err(c, 405, "use POST", "sign in with a POST");
		else if (!mg_http_get_header(hm, "X-Pond"))
			reply_err(c, 403, "refused", "a sign-in needs the X-Pond header");
		else
			api_login(c, hm);
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
	if (mg_match(hm->uri, mg_str("/api/auth/sessions"), NULL)) {
		api_sessions(c, sid);
		return;
	}
	if (mg_match(hm->uri, mg_str("/api/auth/sessions/others"), NULL)) {
		if (post)
			api_end_others(c, hm, sid);
		else
			reply_err(c, 405, "use POST", "end the other sessions with a POST");
		return;
	}
	if (mg_match(hm->uri, mg_str("/api/token"), NULL)) {
		api_token(c);
		return;
	}
	/* The servers' keys: answered now, or once onu-act has done the change. */
	if (keys_route(c, hm))
		return;

	if (mg_match(hm->uri, mg_str("/api/system/restart"), NULL)) {
		if (post)
			api_restart(c);
		else
			reply_err(c, 405, "use POST", "restart with a POST");
	} else if (mg_match(hm->uri, mg_str("/api/identity/olt-login"), NULL)) {
		if (post)
			api_olt_login(c, hm);
		else
			reply_err(c, 405, "use POST", "ask for the OLT's login with a POST");
	} else if (mg_match(hm->uri, mg_str("/api/iptv/test"), NULL)) {
		if (post)
			iptv_test(c, hm);
		else
			reply_err(c, 405, "use POST", "test a group with a POST");
	} else if (mg_match(hm->uri, mg_str("/api/firmware/upload"), NULL)) {
		reply_err(c, 405, "use POST", "upload an update file with a POST");
	} else if (mg_match(hm->uri, mg_str("/api/firmware/download"), NULL)) {
		firmware_download(c);
	} else if (mg_match(hm->uri, mg_str("/api/firmware/backup"), NULL)) {
		firmware_backup(c, hm);
	} else if (mg_match(hm->uri, mg_str("/api/firmware/try"), NULL)) {
		if (!post)
			reply_err(c, 405, "use POST", "start a bank with a POST");
		else if (firmware_try(c, hm))
			g_restart_at = time(NULL) + 1;
	} else if (mg_match(hm->uri, mg_str("/api/firmware/return"), NULL)) {
		if (!post)
			reply_err(c, 405, "use POST", "go back to the shipped firmware with a POST");
		else if (firmware_return(c, hm))
			g_restart_at = time(NULL) + 1;
	} else if (mg_match(hm->uri, mg_str("/api/schema"), NULL))
		value_schema(c);
	else if (mg_match(hm->uri, mg_str("/api/getm"), NULL))
		api_getm(c, hm->query);
	else if (mg_match(hm->uri, mg_str("/api/get/*"), caps))
		api_get(c, caps[0], hm->query);
	else if (mg_match(hm->uri, mg_str("/api/set/mgmt"), NULL) && post)
		mgmt_set(c, hm, handler);
	else if (mg_match(hm->uri, mg_str("/api/set/changes"), NULL) && post)
		changes_set(c, hm);
	else if (mg_match(hm->uri, mg_str("/api/set/*"), caps))
		api_set(c, hm, caps[0]);
	else
		reply_err(c, 404, "no such route", "try /api/schema");
}

#ifdef __linux__
/*
 * Run as the account @user, with its groups, from here on, and keep of
 * root's powers only CAP_NET_BIND_SERVICE: the listeners on ports 80 and
 * 443, here and when a change of the management pair brings another address
 * later. A process that runs as @user already keeps what it has. 0, or -1.
 */
static int run_as(const char *user)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct caps[_LINUX_CAPABILITY_U32S_3] = { };
	struct passwd *pw = getpwnam(user);

	if (!pw) {
		fprintf(stderr, "pond: no account %s\n", user);
		return -1;
	}
	if (getuid() == pw->pw_uid)
		return 0;
	caps[CAP_TO_INDEX(CAP_NET_BIND_SERVICE)].permitted =
		CAP_TO_MASK(CAP_NET_BIND_SERVICE);
	caps[CAP_TO_INDEX(CAP_NET_BIND_SERVICE)].effective =
		CAP_TO_MASK(CAP_NET_BIND_SERVICE);
	if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) ||
	    initgroups(user, pw->pw_gid) || setgid(pw->pw_gid) ||
	    setuid(pw->pw_uid) || syscall(SYS_capset, &hdr, caps) ||
	    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) ||
	    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		fprintf(stderr, "pond: cannot run as %s: %s\n", user,
			strerror(errno));
		return -1;
	}
	return 0;
}
#endif

int main(int argc, char **argv)
{
	struct mg_mgr mgr;
	const char *print_for = NULL;
	const char *user = NULL;
	int listening;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-l") && i + 1 < argc &&
		    g_nlisten < POND_LISTEN_MAX)
			g_listen[g_nlisten++] = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			g_tokenf = argv[++i];
		else if (!strcmp(argv[i], "-k") && i + 1 < argc)
			g_keyf = argv[++i];
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			g_ownf = argv[++i];
		else if (!strcmp(argv[i], "-F") && i + 1 < argc)
			print_for = argv[++i];
		else if (!strcmp(argv[i], "-w") && i + 1 < argc)
			g_web = argv[++i];
		else if (!strcmp(argv[i], "-u") && i + 1 < argc)
			user = argv[++i];
		else {
			fprintf(stderr,
				"usage: %s [-l http[s]://addr:port]... [-k keyfile] [-c ownfile] [-t tokenfile]\n"
				"          [-w webdir] [-u user]\n"
				"       %s -k keyfile [-c ownfile] -F address\n",
				argv[0], argv[0]);
			return 2;
		}
	}

	if (user) {
#ifdef __linux__
		if (run_as(user))
			return 1;
#else
		fprintf(stderr, "pond: -u needs Linux\n");
		return 2;
#endif
	}

	/* The boot script's line for the console: the certificate of an address. */
	if (print_for) {
		if (!g_keyf || tls_setup(NULL, g_keyf, g_ownf, NULL, 0))
			return 1;
		return tls_print(print_for, time(NULL));
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
	auth_init();
	mg_mgr_init(&mgr);
	g_mgr = &mgr;
	if (tls_setup(&mgr, g_keyf, g_ownf, g_listen, g_nlisten))
		return 1;
	/*
	 * An address that is missing costs only its own listener: the others
	 * still serve, and only when none does is there no daemon.
	 */
	listening = tls_listen_all(&mgr, handler);
	if (!listening)
		return 1;
	/* A change of the management pair that waits: its address answers too. */
	mgmt_resume(&mgr, handler);
	fprintf(stderr, "pond: %s; %s\n",
		strlen(g_token) ? "token for scripts" : "no token for scripts",
		auth_default_password() ? "the default password is in force" :
					  "a password is set");

	while (!g_stop) {
		/* A test of a group reads its count every 50 ms (iptv.h). */
		mg_mgr_poll(&mgr, iptv_running() ? 10 : 200);
		prov_settings_reap();
		firmware_poll();
		iptv_poll();
		restart_when_due();
	}

	mg_mgr_free(&mgr);
	return 0;
}
