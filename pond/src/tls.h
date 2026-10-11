/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The panel over HTTPS (tls.c). The daemon listens with each scheme and port
 * of its -l addresses on each address it serves: https:// with Mongoose's
 * own TLS 1.3 and the certificate of cert.h for the address a connection
 * came to, http:// to send the browser on to https:// on the same address
 * with a 308, so that a password never crosses the link in the clear. No
 * Strict-Transport-Security is sent: with a certificate that no authority
 * signed, it would stop a browser from letting its user go on.
 */

#ifndef POND_TLS_H
#define POND_TLS_H

#include "mongoose.h"

#include <stdbool.h>

/* The address of each -l, up to POND_LISTEN_MAX; its host is a template. */
#define TLS_LISTEN_MAX 4

/*
 * Take the key of @keyfile, or none with NULL, and the -l addresses: an
 * https:// one without a key is refused. Returns 0, or a negative errno.
 */
int tls_setup(struct mg_mgr *mgr, const char *keyfile, const char *const *urls,
	      int n);

/* Whether the daemon serves HTTPS: a key and an https:// address. */
bool tls_on(void);

/*
 * Listen on @ip, an IPv4 address with or without its prefix, with each scheme
 * and port of the -l addresses, @fn handling each connection. Returns how
 * many listen.
 */
int tls_listen(struct mg_mgr *mgr, const char *ip, mg_event_handler_t fn);

/* Listen on the -l addresses themselves. Returns how many listen. */
int tls_listen_all(struct mg_mgr *mgr, mg_event_handler_t fn);

/* On MG_EV_ACCEPT: a connection to an https:// port starts TLS. */
void tls_accept(struct mg_connection *c);

/*
 * On a request that came over HTTP while the daemon serves HTTPS: answer it
 * with the address to go to, and return true.
 */
bool tls_redirect(struct mg_connection *c, struct mg_http_message *hm);

/*
 * The certificate for @ip at the time @now, its fingerprint and validity on
 * the standard output; for the boot script. Returns 0, or 1.
 */
int tls_print(const char *ip, time_t now);

#endif
