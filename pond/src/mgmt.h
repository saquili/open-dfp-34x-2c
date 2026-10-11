/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * A change of the management VLAN and address (mgmt.c): POST /api/set/mgmt
 * with VLAN and ADDRESS adds the new pair beside the old one and listens on
 * it, with each scheme and port of the address in use (tls.h); COMMIT=1,
 * which only a request that came to the new address may send, makes it the
 * pair in use; CANCEL=1 takes it away.
 */

#ifndef POND_MGMT_H
#define POND_MGMT_H

#include "mongoose.h"

void mgmt_set(struct mg_connection *c, struct mg_http_message *hm, mg_event_handler_t fn);

/*
 * The management address in use and the new one that waits to be
 * confirmed, empty without one, as onu-mgmt tells them (@cap bytes each).
 */
bool mgmt_addresses(char *current, char *pending, size_t cap);

/*
 * After a change was kept or taken back by other means (changes.c): listen
 * on the address in use alone, where @current and @pending were the ones
 * of before.
 */
void mgmt_settle(struct mg_mgr *mgr, const char *current, const char *pending);

/*
 * After a restart in the middle of a change: listen on the new address too,
 * with each scheme and port of the others (tls.h).
 */
void mgmt_resume(struct mg_mgr *mgr, mg_event_handler_t fn);

#endif
