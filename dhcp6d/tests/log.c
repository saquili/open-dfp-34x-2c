// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The log function of dhcp6d, for tests that do not link its main. */

#include <stdarg.h>
#include <stdio.h>

#include "dhcp6d.h"

void dhcp6d_log(int level, const char *fmt, ...)
{
	va_list ap;

	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
