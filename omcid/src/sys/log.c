// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The log lines of log.h: "[level] file:line: message". */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sys/log.h"

static enum omci_log_level g_level = OMCI_LOG_INFO;

static const char *const level_name[] = {
	[OMCI_LOG_ERR] = "ERR",	  [OMCI_LOG_WARN] = "WRN",
	[OMCI_LOG_INFO] = "INF",  [OMCI_LOG_DEBUG] = "DBG",
	[OMCI_LOG_TRACE] = "TRC",
};

void omci_log_level_set(enum omci_log_level level)
{
	g_level = level;
}

enum omci_log_level omci_log_level_get(void)
{
	return g_level;
}

void omci_log__emit(enum omci_log_level level, const char *file, int line,
		    const char *fmt, ...)
{
	const char *base = strrchr(file, '/');

	base = (base != NULL) ? base + 1 : file;

	fprintf(stderr, "[%s] %s:%d: ", level_name[level], base, line);

	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fputc('\n', stderr);
}
