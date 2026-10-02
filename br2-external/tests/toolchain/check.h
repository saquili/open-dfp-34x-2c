/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * What every test program of this directory uses. CHECK counts a failure and
 * says where it was; DONE prints "ok <name>" when no check failed and returns
 * from main with the status.
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

static int failures __attribute__((unused));

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;					\
		}							\
	} while (0)

#define DONE(name)							\
	do {								\
		if (failures) {						\
			printf("FAILED %s: %d check(s)\n", name, failures); \
			return 1;					\
		}							\
		printf("ok %s\n", name);				\
		return 0;						\
	} while (0)

#endif
