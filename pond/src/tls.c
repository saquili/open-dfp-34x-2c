// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The panel over HTTPS (tls.h). The certificates are made as they are first
 * needed, one for each address and period, and kept: a handful of addresses
 * at most, and a signature takes a moment on the stick. An own certificate,
 * of the file of -c, takes their place on every address while it is there
 * and its key is its own.
 */

#include "tls.h"
#include "cert.h"
#include "providers.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* A -l address: its scheme and port, which each address served gets. */
struct listen_addr {
	const char *url;
	bool tls;
	unsigned int port;
};

static struct listen_addr g_addrs[TLS_LISTEN_MAX];
static int g_naddrs;
static struct mg_mgr *g_mgr;
static bool g_tls;

/* The certificates made so far, the oldest replaced first. */
#define TLS_CACHE 4
struct cert_entry {
	uint8_t ip[4];
	time_t from;
	size_t len;
	uint8_t der[CERT_MAX];
};

static struct cert_entry g_cache[TLS_CACHE];
static unsigned int g_next;

/* The files of -k and -c, and the own certificate, when there is one. */
static const char *g_keyf, *g_ownf;
static struct cert_own g_own;
static bool g_has_own;

/* The own certificate of the file of -c, if any; said when it is refused. */
static void own_load(void)
{
	char err[160];
	int rc;

	g_has_own = false;
	if (!g_ownf)
		return;
	rc = cert_own_load(g_ownf, &g_own, err, sizeof(err));
	if (rc && rc != -ENOENT)
		fprintf(stderr, "pond: the own certificate of %s is not served: %s\n", g_ownf, err);
	g_has_own = !rc;
}

int tls_setup(struct mg_mgr *mgr, const char *keyfile, const char *ownfile,
	      const char *const *urls, int n)
{
	int i, rc;

	g_mgr = mgr;
	g_keyf = keyfile;
	g_ownf = ownfile;
	if (keyfile) {
		rc = cert_load_key(keyfile);
		if (rc) {
			fprintf(stderr, "pond: no key for HTTPS in %s: %s\n", keyfile,
				rc == -EINVAL ? "not a key of P-256" : strerror(-rc));
			return rc;
		}
	}
	own_load();
	for (i = 0; i < n && g_naddrs < TLS_LISTEN_MAX; i++) {
		struct listen_addr *a = &g_addrs[g_naddrs];

		a->url = urls[i];
		a->tls = mg_url_is_ssl(urls[i]);
		a->port = mg_url_port(urls[i]);
		if (a->tls && !cert_have_key()) {
			fprintf(stderr, "pond: %s needs a key (-k)\n", urls[i]);
			return -EINVAL;
		}
		g_tls |= a->tls;
		g_naddrs++;
	}
	return 0;
}

bool tls_on(void)
{
	return g_tls;
}

static bool tls_port(unsigned int port)
{
	int i;

	for (i = 0; i < g_naddrs; i++)
		if (g_addrs[i].tls && g_addrs[i].port == port)
			return true;
	return false;
}

/* The port that a request over HTTP is sent on to. */
static unsigned int https_port(void)
{
	int i;

	for (i = 0; i < g_naddrs; i++)
		if (g_addrs[i].tls)
			return g_addrs[i].port;
	return 443;
}

int tls_listen(struct mg_mgr *mgr, const char *ip, mg_event_handler_t fn)
{
	size_t n = strcspn(ip, "/");
	int i, j, count = 0;
	char url[80];

	for (i = 0; i < g_naddrs; i++) {
		/* Each scheme and port once. */
		for (j = 0; j < i; j++)
			if (g_addrs[j].tls == g_addrs[i].tls &&
			    g_addrs[j].port == g_addrs[i].port)
				break;
		if (j < i)
			continue;
		snprintf(url, sizeof(url), "%s://%.*s:%u", g_addrs[i].tls ? "https" : "http",
			 (int)n, ip, g_addrs[i].port);
		if (mg_http_listen(mgr, url, fn, NULL)) {
			fprintf(stderr, "pond: listening on %s\n", url);
			count++;
		} else {
			fprintf(stderr, "pond: cannot listen on %s\n", url);
		}
	}
	return count;
}

int tls_listen_all(struct mg_mgr *mgr, mg_event_handler_t fn)
{
	int i, count = 0;

	for (i = 0; i < g_naddrs; i++) {
		if (!mg_http_listen(mgr, g_addrs[i].url, fn, NULL)) {
			fprintf(stderr, "pond: cannot listen on %s\n", g_addrs[i].url);
			continue;
		}
		fprintf(stderr, "pond: listening on %s\n", g_addrs[i].url);
		count++;
	}
	return count;
}

/* The certificate for @ip at @now, made or kept; NULL when none can be. */
static const struct cert_entry *certificate(const uint8_t ip[4], time_t now)
{
	struct cert_entry *e;
	time_t from, until;
	unsigned int i;

	cert_validity(now, &from, &until);
	for (i = 0; i < TLS_CACHE; i++) {
		e = &g_cache[i];
		if (e->len && e->from == from && !memcmp(e->ip, ip, 4))
			return e;
	}
	e = &g_cache[g_next++ % TLS_CACHE];
	e->len = cert_make(ip, now, e->der, sizeof(e->der));
	if (!e->len)
		return NULL;
	memcpy(e->ip, ip, 4);
	e->from = from;
	return e;
}

void tls_accept(struct mg_connection *c)
{
	uint8_t key[CERT_KEY_DER_LEN];
	const struct cert_entry *e;
	struct mg_tls_opts opts;

	if (!tls_port(mg_ntohs(c->loc.port)))
		return;
	/* The own certificate on every address, the server's and its chain. */
	if (g_has_own) {
		cert_own_key_der(&g_own, key);
		memset(&opts, 0, sizeof(opts));
		opts.cert = mg_str_n((const char *)g_own.chain, g_own.len);
		opts.key = mg_str_n((const char *)key, sizeof(key));
		mg_tls_init(c, &opts);
		memset(key, 0, sizeof(key));
		return;
	}
	e = c->loc.is_ip6 ? NULL : certificate(c->loc.ip, time(NULL));
	if (!e) {
		mg_error(c, "no certificate for this address");
		return;
	}
	cert_key_der(key);
	memset(&opts, 0, sizeof(opts));
	opts.cert = mg_str_n((const char *)e->der, e->len);
	opts.key = mg_str_n((const char *)key, sizeof(key));
	mg_tls_init(c, &opts);
	memset(key, 0, sizeof(key));
}

/* A host of the Host header without its port: a name or an address. */
static bool host_of(struct mg_str h, struct mg_str *host)
{
	size_t i, n = h.len;

	if (n && h.buf[0] == '[') {
		const char *end = memchr(h.buf, ']', n);

		if (!end)
			return false;
		n = (size_t)(end - h.buf) + 1;
	} else {
		const char *colon = memchr(h.buf, ':', n);

		if (colon)
			n = (size_t)(colon - h.buf);
	}
	if (!n || n > 253)
		return false;
	for (i = 0; i < n; i++) {
		char ch = h.buf[i];

		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == ':' ||
		      ch == '[' || ch == ']'))
			return false;
	}
	*host = mg_str_n(h.buf, n);
	return true;
}

bool tls_redirect(struct mg_connection *c, struct mg_http_message *hm)
{
	const struct mg_str *h;
	char here[16], port[8] = "", headers[600];
	struct mg_str host;
	unsigned int p = https_port();

	if (!g_tls || c->is_tls)
		return false;
	h = mg_http_get_header(hm, "Host");
	if (!h || !host_of(*h, &host)) {
		snprintf(here, sizeof(here), "%u.%u.%u.%u", c->loc.ip[0], c->loc.ip[1],
			 c->loc.ip[2], c->loc.ip[3]);
		host = mg_str(here);
	}
	if (p != 443)
		snprintf(port, sizeof(port), ":%u", p);
	if (snprintf(headers, sizeof(headers), "Location: https://%.*s%s%.*s%s%.*s\r\n",
		     (int)host.len, host.buf, port, (int)hm->uri.len, hm->uri.buf,
		     hm->query.len ? "?" : "", (int)hm->query.len, hm->query.buf) >=
	    (int)sizeof(headers)) {
		mg_http_reply(c, 414, "", "");
		return true;
	}
	mg_http_reply(c, 308, headers, "");
	return true;
}

static void iso(time_t t, char out[24])
{
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int tls_print(const char *ip, time_t now)
{
	struct mg_addr a;
	const struct cert_entry *e;
	char fp[CERT_FP_LEN], until[24];
	time_t from, to;

	memset(&a, 0, sizeof(a));
	if (!mg_aton(mg_str_n(ip, strcspn(ip, "/")), &a) || a.is_ip6) {
		fprintf(stderr, "pond: %s is not an IPv4 address\n", ip);
		return 1;
	}
	if (g_has_own) {
		cert_fingerprint(g_own.chain, g_own.leaf, fp);
		cert_iso(g_own.not_after, until);
		printf("%.*s SHA-256 %s until %s\n", (int)strcspn(ip, "/"), ip, fp, until);
		return 0;
	}
	e = certificate(a.ip, now);
	if (!e) {
		fprintf(stderr, "pond: no certificate for %s\n", ip);
		return 1;
	}
	cert_fingerprint(e->der, e->len, fp);
	cert_validity(now, &from, &to);
	iso(to, until);
	printf("%.*s SHA-256 %s until %s\n", (int)strcspn(ip, "/"), ip, fp, until);
	return 0;
}

static void pv_member(struct prov_buf *out, const char *key, const char *value)
{
	pv_comma(out);
	pv_key(out, key);
	pv_str(out, value);
}

/* The own certificate: who it names, who signed it, its validity and its names. */
static void own_value(struct prov_buf *out)
{
	char fp[CERT_FP_LEN], t[24], *name, *save = NULL;
	char names[CERT_NAMES_MAX];

	if (!g_has_own) {
		pv_null(out);
		return;
	}
	pv_open(out, '{');
	pv_key(out, "subject");
	pv_str(out, g_own.subject);
	pv_member(out, "issuer", g_own.issuer);
	cert_fingerprint(g_own.chain, g_own.leaf, fp);
	pv_member(out, "sha256", fp);
	cert_iso(g_own.not_before, t);
	pv_member(out, "not_before", t);
	cert_iso(g_own.not_after, t);
	pv_member(out, "not_after", t);
	pv_comma(out);
	pv_key(out, "chain");
	pv_num(out, g_own.count);
	pv_comma(out);
	pv_key(out, "names");
	pv_open(out, '[');
	snprintf(names, sizeof(names), "%s", g_own.names);
	for (name = strtok_r(names, " ", &save); name; name = strtok_r(NULL, " ", &save)) {
		if (name != names)
			pv_comma(out);
		pv_str(out, name);
	}
	pv_close(out, ']');
	pv_close(out, '}');
}

/*
 * The provider "tls": whether the daemon serves HTTPS, the own certificate
 * or null, and for each address served over HTTPS, the certificate it
 * gets, and whether that certificate names it.
 */
int prov_tls(struct prov_buf *out)
{
	struct mg_connection *l;
	time_t now = time(NULL), from, to;
	char fp[CERT_FP_LEN], t[24], ip[16];
	bool first = true;

	pv_open(out, '{');
	pv_key(out, "https");
	pv_bool(out, g_tls);
	pv_comma(out);
	pv_key(out, "own");
	own_value(out);
	pv_comma(out);
	pv_key(out, "certificates");
	pv_open(out, '[');
	for (l = g_mgr ? g_mgr->conns : NULL; g_tls && l; l = l->next) {
		const struct cert_entry *e;

		if (!l->is_listening || l->is_closing || l->loc.is_ip6 ||
		    !tls_port(mg_ntohs(l->loc.port)))
			continue;
		e = g_has_own ? NULL : certificate(l->loc.ip, now);
		if (!e && !g_has_own)
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		snprintf(ip, sizeof(ip), "%u.%u.%u.%u", l->loc.ip[0], l->loc.ip[1],
			 l->loc.ip[2], l->loc.ip[3]);
		pv_open(out, '{');
		pv_key(out, "address");
		pv_str(out, ip);
		pv_comma(out);
		pv_key(out, "port");
		pv_num(out, mg_ntohs(l->loc.port));
		if (e) {
			cert_fingerprint(e->der, e->len, fp);
			cert_validity(now, &from, &to);
			pv_member(out, "sha256", fp);
			iso(from, t);
			pv_member(out, "not_before", t);
			iso(to, t);
			pv_member(out, "not_after", t);
		} else {
			cert_fingerprint(g_own.chain, g_own.leaf, fp);
			pv_member(out, "sha256", fp);
			cert_iso(g_own.not_before, t);
			pv_member(out, "not_before", t);
			cert_iso(g_own.not_after, t);
			pv_member(out, "not_after", t);
		}
		pv_comma(out);
		pv_key(out, "named");
		pv_bool(out, !g_has_own || cert_own_names(&g_own, l->loc.ip));
		pv_close(out, '}');
	}
	pv_close(out, ']');
	pv_close(out, '}');
	return 0;
}
