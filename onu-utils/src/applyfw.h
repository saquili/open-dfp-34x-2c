/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* applyfw.h: what applyfw.c gives the other parts of onu-apply. */

#ifndef APPLYFW_H
#define APPLYFW_H

#include "applyutil.h"

void fw_clear(void);
void lans_fw(void);
void guard_fw(void);
void dos_switch(void);

#endif
