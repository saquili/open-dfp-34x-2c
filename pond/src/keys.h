/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The keys of the stick's two servers, changed from the panel (keys.c): the
 * ssh server's host key, the public keys root logs in with and whether the
 * server takes them alone, the panel's key and an own certificate.
 */

#ifndef POND_KEYS_H
#define POND_KEYS_H

#include <stdbool.h>

struct mg_connection;
struct mg_http_message;

/*
 * The request @hm, when its path is one of keys.c's: answered, now or once
 * onu-act has done the change, and true; false for any other path.
 */
bool keys_route(struct mg_connection *c, struct mg_http_message *hm);

#endif
