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

/*
 * Have onu-act do @method (onu-utils/src/act.h): the @n values @values of the
 * params @names, NULL for one not given, and @input on the program's
 * standard input when it is not NULL. What the program wrote goes in @out,
 * of @cap bytes. Returns its exit status, as a run of the program would, or
 * a negative errno when onu-act does not answer or refuses the values. A
 * build without libubus runs the method's program itself, from
 * $POND_<PROGRAM> when set (POND_ONU_LANE for onu-lane), for the tests.
 */
int bus_act(const char *method, const char *const *names,
	    const char *const *values, size_t n, const char *input, char *out,
	    size_t cap);

/* What a run that bus_act_start() started wrote, at most this much. */
#define BUS_OUTPUT_MAX 4096

/*
 * Have onu-act start @method, with no values, and not wait for it. @fd, when
 * not -1, goes to the program as its standard input or output, as the method
 * takes it, and is no longer the caller's, whatever the result. @done is
 * called from bus_poll() with the program's exit status, or a negative errno
 * when onu-act did not answer, and what it wrote. A few methods run at once,
 * each once. Returns 0, -EBUSY while @method runs, or another negative errno
 * when it could not start.
 */
int bus_act_start(const char *method, int fd,
		  void (*done)(int code, const char *output));

/* Whether @method, started by bus_act_start(), has not ended yet. */
bool bus_act_running(const char *method);

/* Take what came of bus_act_start(): from the main loop, often. */
void bus_poll(void);

#endif
