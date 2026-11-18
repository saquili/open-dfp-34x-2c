/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* applyppp.h: what applyppp.c gives the other parts of onu-apply. */

#ifndef APPLYPPP_H
#define APPLYPPP_H

#include "applyutil.h"

void iface_event(void);
void accounts_to_pppoed(int only);
void sessions_down(void);
void sessions_sync(void);

#endif
