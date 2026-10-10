/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The state of the stick's programs on ubus (ADR 0014): netifd's of each
 * interface, a PPPoE session's among them, and dhcp6d's. bus-ubus.c asks
 * ubusd; bus-file.c, for the tests and a build without libubus, reads
 * run/ubus/OBJECT, a "PATH=VALUE" line for each value.
 */

#ifndef POND_BUS_H
#define POND_BUS_H

#include <stddef.h>

#define BUS_VALUE 64

/*
 * The values of the method "status" of @object at @paths, each in out[i]:
 * a path names a value through the tables, "data.address", and an array of
 * strings, "dns-server", gives them apart by blanks; a value that is not
 * there is empty. Returns 0, or a negative errno when the object does not
 * answer.
 */
int bus_status(const char *object, const char *const *paths, size_t n,
	       char (*out)[BUS_VALUE]);

#endif
