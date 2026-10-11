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
 * of @cap bytes. A method that needs root's password takes it as the value
 * "password". Returns its exit status, as a run of the program would, or
 * a negative errno when onu-act does not answer or refuses the values,
 * -EACCES for a password that is not root's. A build without libubus runs
 * the method's program itself, from $POND_<PROGRAM> when set (POND_ONU_LANE
 * for onu-lane), for the tests, and judges the password with onu-config
 * password-check ($POND_ONU_CONFIG) as onu-act does.
 */
int bus_act(const char *method, const char *const *names,
	    const char *const *values, size_t n, const char *input, char *out,
	    size_t cap);

/* What a run that was not waited for wrote, at most this much. */
#define BUS_OUTPUT_MAX 4096

/*
 * The end of a method a caller did not wait for: @ctx as given, the
 * program's exit status, or a negative errno as bus_act() returns, and what
 * it wrote. It is called from bus_poll(), never from inside another call of
 * this file.
 */
typedef void (*bus_done)(void *ctx, int code, const char *output);

/*
 * Have onu-act start @method, with no values but @password when not NULL,
 * and not wait for it. @fd, when not -1, goes to the program as its
 * standard input or output, as the method takes it, and is no longer the
 * caller's, whatever the result. @done is called with @ctx when it ends,
 * -EACCES when onu-act refused the password. A few methods run at once,
 * each once. Returns 0, -EBUSY while @method runs, or another negative
 * errno when it could not start; @done is called only after 0.
 */
int bus_act_start(const char *method, int fd, const char *password,
		  bus_done done, void *ctx);

/*
 * Have onu-act do @method as bus_act() does, with its values and input, and
 * not wait for it: @done is called with @ctx when it ends. The values and
 * the input are copied before this returns. Returns as bus_act_start().
 */
int bus_act_begin(const char *method, const char *const *names,
		  const char *const *values, size_t n, const char *input,
		  bus_done done, void *ctx);

/* Whether @method, started without waiting, has not ended yet. */
bool bus_act_running(const char *method);

/* Take what came of the runs not waited for: from the main loop, often. */
void bus_poll(void);

#endif
