/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * What onu-act does for the programs that may not do it themselves (ADR
 * 0014, step 8): a fixed table of methods, each a program of the stick run
 * with fixed arguments, the caller's values in their places and, for some,
 * the caller's text, KEY=VALUE lines, on its standard input. A value is
 * taken only when it is short enough and of the characters its place
 * allows, so that nothing a caller sends becomes an option, a path or a
 * second argument. ubusd's access lists say who may call which method.
 */

#ifndef ONU_ACT_H
#define ONU_ACT_H

#include <stdbool.h>
#include <stddef.h>

/* The most values of a method, fixed arguments, and the longest value. */
#define ACT_PARAMS_MAX 2
#define ACT_FIXED_MAX 2
#define ACT_VALUE_MAX 64
#define ACT_ARGV_MAX (2 + ACT_FIXED_MAX + ACT_PARAMS_MAX)

/* The most text a method takes on its standard input. */
#define ACT_INPUT_MAX 8192

/* The longest password a call carries (onu-config password: 128). */
#define ACT_PASSWORD_MAX 128

/**
 * struct act_param - A value a caller gives, in its place after the fixed
 *	arguments.
 * @name: its key in the call
 * @chars: the characters it may hold
 * @max: its longest length
 * @optional: it may be left out, and then nothing takes its place
 */
struct act_param {
	const char *name;
	const char *chars;
	unsigned int max;
	bool optional;
};

/**
 * struct act_method - What a method runs.
 * @name: its name on ubus
 * @program: the program, by its whole path
 * @fixed: the arguments before the values, NULL after the last
 * @params: the values, NULL-named after the last
 * @input: whether it takes the caller's text on its standard input
 * @fd_in: whether the descriptor the caller passes is its standard input
 * @fd_out: whether the descriptor the caller passes is its standard output
 * @background: whether it is answered once started, and not waited for
 * @needs_password: whether it runs only when the call carries root's password in
 *	force, as "password", which onu-config password-check judges: what
 *	would give a caller root (a new password, a new system), so that the
 *	status daemon alone, taken over, cannot
 * @timeout_s: how long it may run before it is killed
 */
struct act_method {
	const char *name;
	const char *program;
	const char *fixed[ACT_FIXED_MAX + 1];
	struct act_param params[ACT_PARAMS_MAX + 1];
	bool input;
	bool fd_in;
	bool fd_out;
	bool background;
	bool needs_password;
	unsigned int timeout_s;
};

extern const struct act_method act_methods[];
extern const size_t act_method_count;

/* The method named @name, or NULL. */
const struct act_method *act_find(const char *name);

/**
 * act_argv() - The arguments of a run of @m.
 * @m: the method
 * @values: a value for each of @m's params, NULL for one not given
 * @argv: the program and its arguments, NULL-terminated, ACT_ARGV_MAX + 1
 *	of them at most; the values are the caller's strings
 * @bad: the name of the param refused, on an error
 *
 * Return: 0, or -EINVAL for a value missing, too long or of a character
 * its place does not allow.
 */
int act_argv(const struct act_method *m, const char *const *values,
	     const char **argv, const char **bad);

#endif
