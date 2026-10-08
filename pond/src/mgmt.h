/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * A change of the management VLAN and address (mgmt.c): POST /api/set/mgmt
 * with VLAN and ADDRESS adds the new pair beside the old one and listens on
 * it; COMMIT=1, which only a request that came to the new address may send,
 * makes it the pair in use; CANCEL=1 takes it away.
 */

#ifndef POND_MGMT_H
#define POND_MGMT_H

#include "mongoose.h"

void mgmt_set(struct mg_connection *c, struct mg_http_message *hm, mg_event_handler_t fn);

/* After a restart in the middle of a change: listen on the new address too. */
void mgmt_resume(struct mg_mgr *mgr, const char *port, mg_event_handler_t fn);

#endif
