/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* applyconn.h: what applyconn.c gives the other parts of onu-apply. */

#ifndef APPLYCONN_H
#define APPLYCONN_H

#include "applyutil.h"

char *pon_mac(char *out);
char *conn_gem(const char *g, const char *v, char *out);
void guard(const char *dev);
void feeds_down(void);
bool dev_shared(const char *dir, const char *dev);
void feeds_all(void);

#endif
