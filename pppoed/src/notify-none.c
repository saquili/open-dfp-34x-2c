// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * pppoed's messages to netifd (nproto.h), where there is no ubus: a build on
 * a host without libubus, for the tests, prints them instead.
 */

#include <stdio.h>

#include "nproto.h"

int nproto_send(const char *iface, const char *json)
{
	printf("notify %s %s\n", iface, json);
	fflush(stdout);
	return 0;
}
