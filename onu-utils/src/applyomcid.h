/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* applyomcid.h: what applyomcid.c gives the other parts of onu-apply. */

#ifndef APPLYOMCID_H
#define APPLYOMCID_H

#include "applyutil.h"

void omcid_rules(void);
void omcid_igmp(void);
bool omcid_runs(void);
void omcid_run(const char *mode);
bool held_vlans(struct words *out);
void held_want(struct buf *out);
void omcid_want(struct buf *out);
void omcid_sync(const char *want);

#endif
