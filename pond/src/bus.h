/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The state of the stick's programs on ubus (ADR 0014): netifd's of each
 * interface, a PPPoE session's among them, dhcp6d's and omcid's. bus-ubus.c
 * asks ubusd; bus-file.c, for the tests and a build without libubus, reads
 * run/ubus/OBJECT, a "PATH=VALUE" line for each value of "status", and
 * run/ubus/OBJECT.METHOD, the JSON text a method answers.
 */

#ifndef POND_BUS_H
#define POND_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* An argument of a method: a number, or with @array an array of @n. */
struct bus_arg {
	const char *name;
	const uint32_t *values;
	size_t n;
	bool array;
};

/*
 * The answer of the method @method of @object to the @nargs @args, as JSON
 * text in @out, of @cap bytes: the arguments go as blobmsg, and the answer
 * is made text once, here, for the panel. Returns its length, or a negative
 * errno when the object does not answer or the answer does not fit.
 */
int bus_call(const char *object, const char *method,
	     const struct bus_arg *args, size_t nargs, char *out, size_t cap);

#endif
