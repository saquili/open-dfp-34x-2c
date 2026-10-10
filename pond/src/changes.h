/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The changes on trial, all at once (changes.c): POST /api/set/changes with
 * CONFIRM=1 keeps every change on trial, the management pair's only when
 * the request came to its new address; REVERT=1 takes them all back.
 */

#ifndef POND_CHANGES_H
#define POND_CHANGES_H

#include "mongoose.h"

void changes_set(struct mg_connection *c, struct mg_http_message *hm);

#endif
