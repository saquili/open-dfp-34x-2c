/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Log lines on the standard error, with a level that can be raised or lowered. */

#ifndef OMCI_LOG_H
#define OMCI_LOG_H

#include "omcid/attrs.h"

/**
 * enum omci_log_level - How much is logged; each level includes those above it.
 * @OMCI_LOG_ERR: failures
 * @OMCI_LOG_WARN: requests refused, or carried out in part
 * @OMCI_LOG_INFO: the course of the protocol (the default)
 * @OMCI_LOG_DEBUG: decisions of the engine
 * @OMCI_LOG_TRACE: every frame
 */
enum omci_log_level {
	OMCI_LOG_ERR = 0,
	OMCI_LOG_WARN = 1,
	OMCI_LOG_INFO = 2,
	OMCI_LOG_DEBUG = 3,
	OMCI_LOG_TRACE = 4,
};

/* Set and read the level. */
void omci_log_level_set(enum omci_log_level level);
enum omci_log_level omci_log_level_get(void);

/* Write one line; the OMCI_* macros below call it when the level allows. */
void omci_log__emit(enum omci_log_level level, const char *file, int line,
		    const char *fmt, ...) __printf(4, 5);

#define OMCI_LOG(lvl, ...)                                        \
	do {                                                      \
		if ((lvl) <= omci_log_level_get())                \
			omci_log__emit((lvl), __FILE__, __LINE__, \
				       __VA_ARGS__);              \
	} while (0)

#define OMCI_ERR(...) OMCI_LOG(OMCI_LOG_ERR, __VA_ARGS__)
#define OMCI_WARN(...) OMCI_LOG(OMCI_LOG_WARN, __VA_ARGS__)
#define OMCI_INFO(...) OMCI_LOG(OMCI_LOG_INFO, __VA_ARGS__)
#define OMCI_DEBUG(...) OMCI_LOG(OMCI_LOG_DEBUG, __VA_ARGS__)
#define OMCI_TRACE(...) OMCI_LOG(OMCI_LOG_TRACE, __VA_ARGS__)

#endif
