/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* applyhost.h: what applyhost.c gives the other parts of onu-apply. */

#ifndef APPLYHOST_H
#define APPLYHOST_H

#include "applyutil.h"

void lan_dhcp(int n);
void lans_up(void);
void pass_up(void);
void pass_down(void);

#endif
