/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * A request answered later, once what it started through onu-act has ended
 * (bus.h): the daemon serves the other requests meanwhile. The request is
 * found again by the id mongoose gave its connection; a connection closed by
 * then is answered by nobody, and what was started is not undone.
 */

#ifndef POND_LATER_H
#define POND_LATER_H

#include "mongoose.h"

/* The connection of @mgr whose id is @id, or NULL once it has closed. */
static inline struct mg_connection *later_conn(struct mg_mgr *mgr,
					       unsigned long id)
{
	for (struct mg_connection *c = mgr->conns; c; c = c->next)
		if (c->id == id && !c->is_closing && !c->is_draining)
			return c;
	return NULL;
}

#endif
