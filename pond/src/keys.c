// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The keys of the stick's two servers, changed from the panel:
 *
 *   POST /api/ssh/host-key     a new host key for the ssh server
 *   POST /api/ssh/keys         the public key of the body, for root's login
 *   POST /api/ssh/keys/remove  the key whose fingerprint is the body, away
 *   POST /api/ssh/keys-only    "on" or "off": the ssh server takes keys
 *                              alone, and no password
 *   POST /api/tls/key          a new key, which the panel's certificates are
 *                              made from
 *   POST /api/tls/certificate  an own certificate, {"certificate", "key"},
 *                              each in PEM, served in place of those
 *   POST /api/tls/self-signed  back to the certificates of the panel's key
 *
 * Each needs root's password in X-Pond-Password as well as a session or the
 * token: a key to log in with gives root, and the servers' keys are what a
 * person checks a connection by. onu-act has onu-config do the change (the
 * methods of onu-utils/src/act.c), which checks the password again and keeps
 * the change in the configuration store; pond does not wait for it, and
 * answers when it has ended (later.h). An own certificate is checked here
 * first (cert.h): its text, its key, P-256, and that the key is the
 * certificate's. Once a change of the panel's key or certificate has ended,
 * the daemon reads them again (tls.h): the connection that asked keeps its
 * own and gets the answer, and the connections after it get the new
 * certificate, which a browser warns about once. The ssh server is started
 * again by onu-config, and the sessions open go on.
 */

#include "auth.h"
#include "bus.h"
#include "cert.h"
#include "firmware.h"
#include "keys.h"
#include "later.h"
#include "prov.h"
#include "providers.h"
#include "tls.h"

#include "mongoose.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The letters of a fingerprint's base64. */
#define BASE64 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

/* The most a body takes: a chain of 8 KiB in PEM, its key and their JSON. */
#define KEYS_BODY_MAX (32u * 1024u)

enum keys_kind { K_HOST, K_ADD, K_REMOVE, K_ONLY, K_TLS };

static const struct {
	const char *path;
	const char *method;
	enum keys_kind kind;
} g_routes[] = {
	{ "/api/ssh/host-key", "ssh_host_key_new", K_HOST },
	{ "/api/ssh/keys", "ssh_key_add", K_ADD },
	{ "/api/ssh/keys/remove", "ssh_key_remove", K_REMOVE },
	{ "/api/ssh/keys-only", "ssh_keys_only", K_ONLY },
	{ "/api/tls/key", "tls_key_new", K_TLS },
	{ "/api/tls/certificate", "tls_own_store", K_TLS },
	{ "/api/tls/self-signed", "tls_own_remove", K_TLS },
};

/* A change under way: the connection that asked, by its id, and of what. */
struct pending {
	struct mg_mgr *mgr;
	unsigned long id;
	enum keys_kind kind;
};

static void reply(struct mg_connection *c, int code, const char *body)
{
	mg_http_reply(c, code, "Content-Type: application/json\r\nCache-Control: no-store\r\n",
		      "%s", body);
}

static void refuse(struct mg_connection *c, int code, const char *what, const char *why)
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
	reply(c, code, body);
}

/*
 * The answer to a change that did not end well, @code as bus.h gives it:
 * onu-act refused root's password (403), it could not be done (500), or
 * onu-config refused it, its exit status, and the first of its complaints
 * in @said is the cause (400).
 */
static void failed(struct mg_connection *c, int code, const char *said)
{
	const char *p = strstr(said, "onu-config: ");
	char why[256];

	p = p ? p + strlen("onu-config: ") : said;
	snprintf(why, sizeof(why), "%.*s", (int)strcspn(p, "\n"), p);
	if (code < 0 || !*why)
		snprintf(why, sizeof(why), "%s", code == -EACCES ?
			 "root's password is needed for this" :
			 code < 0 ? strerror(-code) : "onu-config refused it");
	refuse(c, code == -EACCES ? 403 : code < 0 ? 500 : 400,
	       code != -EACCES ? (code < 0 ? "not done" : "refused") : "wrong password", why);
}

/* The answer to a change that ended well: what it made. */
static void done_reply(struct mg_connection *c, enum keys_kind kind, const char *said)
{
	static char body[8192];
	struct prov_buf b = { .buf = body, .cap = sizeof(body) };
	char type[64] = "", fp[64] = "";
	const char *key = strstr(said, "KEY=");

	pv_open(&b, '{');
	pv_key(&b, "ok");
	pv_bool(&b, true);
	if (kind == K_HOST && fw_field(said, "TYPE", type, sizeof(type)) &&
	    fw_field(said, "FINGERPRINT", fp, sizeof(fp))) {
		pv_comma(&b);
		pv_key(&b, "host_key");
		pv_open(&b, '{');
		pv_key(&b, "type");
		pv_str(&b, type);
		pv_comma(&b);
		pv_key(&b, "fingerprint");
		pv_str(&b, fp);
		pv_close(&b, '}');
	} else if (kind == K_ADD && key) {
		pv_comma(&b);
		pv_key(&b, "key");
		ssh_key_json(&b, key + 4, "panel");
	} else if (kind == K_TLS) {
		pv_comma(&b);
		pv_key(&b, "tls");
		prov_tls(&b);
	}
	pv_close(&b, '}');
	reply(c, 200, b.overflow ? "{\"ok\":true}" : body);
}

static void ended(void *ctx, int code, const char *said)
{
	struct pending *p = ctx;
	struct mg_connection *c;

	/* The new key or certificate, for the connections from now on. */
	if (p->kind == K_TLS && !code)
		tls_reload();
	/* The connection that asked, unless it went meanwhile. */
	c = later_conn(p->mgr, p->id);
	if (c && code)
		failed(c, code, said);
	else if (c)
		done_reply(c, p->kind, said);
	free(p);
}

/* @s of @n bytes, all of them of @chars. */
static bool all_of(const char *s, size_t n, const char *chars)
{
	for (size_t i = 0; i < n; i++)
		if (!s[i] || !strchr(chars, s[i]))
			return false;
	return true;
}

/*
 * An own certificate of the JSON body @body as the input of onu-config tls
 * own, its lines KEY= and CHAIN=, in @input of @cap: 0, or the refusal sent.
 */
static int own_input(struct mg_connection *c, struct mg_str body, char *input, size_t cap)
{
	static struct cert_own own;
	char *cert = mg_json_get_str(body, "$.certificate");
	char *key = mg_json_get_str(body, "$.key");
	char err[160];
	size_t len;
	int rc = -EINVAL;

	if (!cert || !key)
		refuse(c, 400, "not a certificate", "give {\"certificate\", \"key\"}, each in PEM");
	else if (cert_own_pem(cert, key, &own, err, sizeof(err)))
		refuse(c, 400, "not a certificate", err);
	else
		rc = 0;
	free(cert);
	if (key) {
		explicit_bzero(key, strlen(key));
		free(key);
	}
	if (rc)
		return rc;
	len = (size_t)snprintf(input, cap, "KEY=");
	for (size_t i = 0; i < CERT_KEY_LEN; i++)
		len += (size_t)snprintf(input + len, cap - len, "%02x", own.key[i]);
	len += (size_t)snprintf(input + len, cap - len, "\nCHAIN=");
	len += mg_base64_encode(own.chain, own.len, input + len, cap - len);
	snprintf(input + len, cap - len, "\n");
	explicit_bzero(&own, sizeof(own));
	return 0;
}

/*
 * The input of the change of @kind, of the body of @hm, in @input of @cap,
 * and the value of keys alone in @state: 0, or the refusal sent.
 */
static int input_of(struct mg_connection *c, struct mg_http_message *hm, const char *path,
		    char *input, size_t cap, const char **state)
{
	struct mg_str body = hm->body;
	size_t n = body.len;

	while (n && strchr(" \t\r\n", body.buf[n - 1]))
		n--;
	if (!strcmp(path, "/api/tls/certificate"))
		return own_input(c, body, input, cap);
	if (!strcmp(path, "/api/ssh/keys")) {
		if (!n || n > 4000 || memchr(body.buf, '\0', n)) {
			refuse(c, 400, "not a key", "the body is a public key, a line TYPE BASE64 [COMMENT]");
			return -EINVAL;
		}
		snprintf(input, cap, "%.*s\n", (int)n, body.buf);
	} else if (!strcmp(path, "/api/ssh/keys/remove")) {
		if (!n || n > 64 || !all_of(body.buf, n, BASE64 ":=")) {
			refuse(c, 400, "not a fingerprint", "the body is a key's SHA256:... fingerprint");
			return -EINVAL;
		}
		snprintf(input, cap, "%.*s\n", (int)n, body.buf);
	} else if (!strcmp(path, "/api/ssh/keys-only")) {
		if (n == 2 && !memcmp(body.buf, "on", 2)) {
			*state = "on";
		} else if (n == 3 && !memcmp(body.buf, "off", 3)) {
			*state = "off";
		} else {
			refuse(c, 400, "not on or off", "the body is on or off");
			return -EINVAL;
		}
	}
	return 0;
}

bool keys_route(struct mg_connection *c, struct mg_http_message *hm)
{
	static char input[KEYS_BODY_MAX];
	const char *names[2] = { "password", "state" }, *values[2] = { NULL, NULL };
	char pw[AUTH_SECRET_MAX + 1];
	const char *why;
	struct pending *p;
	size_t i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(g_routes); i++)
		if (mg_match(hm->uri, mg_str(g_routes[i].path), NULL))
			break;
	if (i == ARRAY_SIZE(g_routes))
		return false;
	if (mg_strcmp(hm->method, mg_str("POST"))) {
		refuse(c, 405, "use POST", "a change of a key is a POST");
		return true;
	}
	if (hm->body.len > KEYS_BODY_MAX) {
		refuse(c, 413, "too large", "the body is 32 KiB at most");
		return true;
	}
	input[0] = '\0';
	if (input_of(c, hm, g_routes[i].path, input, sizeof(input), &values[1]))
		return true;
	if (!firmware_confirmed(c, hm, pw, sizeof(pw))) {
		explicit_bzero(input, sizeof(input));
		return true;
	}
	values[0] = pw;
	p = calloc(1, sizeof(*p));
	rc = p ? 0 : -ENOMEM;
	if (p) {
		p->mgr = c->mgr;
		p->id = c->id;
		p->kind = g_routes[i].kind;
		rc = bus_act_begin(g_routes[i].method, names, values, values[1] ? 2 : 1,
				   *input ? input : NULL, ended, p);
	}
	explicit_bzero(pw, sizeof(pw));
	explicit_bzero(input, sizeof(input));
	if (!rc)
		return true;
	free(p);
	why = rc == -EBUSY ? "the same change is under way" : strerror(-rc);
	refuse(c, rc == -EBUSY ? 409 : 500, rc == -EBUSY ? "busy" : "not started", why);
	return true;
}
