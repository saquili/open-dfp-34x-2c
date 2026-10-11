// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* bus.h from files, for the tests and a build without libubus. */

#define _GNU_SOURCE

#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "act.h"
#include "bus.h"
#include "prov.h"

int bus_status(const char *object, const char *const *paths, size_t n,
	       char (*out)[BUS_VALUE])
{
	char path[160], text[2048];

	snprintf(path, sizeof(path), "/run/ubus/%s", object);
	for (size_t i = 0; i < n; i++)
		out[i][0] = '\0';
	if (prov_slurp(path, text, sizeof(text), NULL))
		return -ENOENT;
	for (const char *line = text; *line; line += strcspn(line, "\n") + (line[strcspn(line, "\n")] != 0)) {
		size_t len = strcspn(line, "\n"), k;

		for (size_t i = 0; i < n; i++) {
			k = strlen(paths[i]);
			if (len > k && !strncmp(line, paths[i], k) && line[k] == '=')
				snprintf(out[i], BUS_VALUE, "%.*s", (int)(len - k - 1), line + k + 1);
		}
	}
	return 0;
}

int bus_call(const char *object, const char *method,
	     const struct bus_arg *args, size_t nargs, char *out, size_t cap)
{
	char path[160];
	size_t len = 0;
	int err;

	(void)args;
	(void)nargs;
	snprintf(path, sizeof(path), "/run/ubus/%s.%s", object, method);
	err = prov_slurp(path, out, cap, &len);
	if (err)
		return err;
	while (len > 0 && out[len - 1] == '\n')
		out[--len] = '\0';
	return (int)len;
}

/* The program of @m, from $POND_<ITS NAME> when set, for the tests. */
static const char *program_of(const struct act_method *m)
{
	const char *base = strrchr(m->program, '/') + 1, *prog;
	char env[64];
	size_t k = (size_t)snprintf(env, sizeof(env), "POND_");

	for (; *base && k + 1 < sizeof(env); base++)
		env[k++] = *base == '-' ? '_' : (char)toupper((unsigned char)*base);
	env[k] = '\0';
	prog = getenv(env);
	return prog && *prog ? prog : m->program;
}

/* Whether @secret is root's password in force, as onu-act judges it. */
static bool password_ok(const char *secret)
{
	static const struct act_method check = { .program = "/usr/sbin/onu-config" };
	const char *argv[] = { "password-check", NULL };
	char line[ACT_PASSWORD_MAX + 2], out[64];
	int rc;

	if (!secret || strlen(secret) > ACT_PASSWORD_MAX || strchr(secret, '\n'))
		return false;
	snprintf(line, sizeof(line), "%s\n", secret);
	rc = prov_exec(program_of(&check), argv, line, out, sizeof(out));
	explicit_bzero(line, sizeof(line));
	return rc == 0 || rc == 3 || rc == 4;
}

/*
 * The arguments of a run of @method, from the @n values @values of @names,
 * in @argv, and the password among them in @password: 0, or a negative
 * errno as bus_act() returns.
 */
static int args_of(const struct act_method *m, const char *const *names,
		   const char *const *values, size_t n, const char **argv,
		   const char **password)
{
	const char *given[ACT_PARAMS_MAX] = { NULL };
	const char *bad;

	*password = NULL;
	for (size_t i = 0; i < n; i++) {
		for (size_t p = 0; p < ACT_PARAMS_MAX && m->params[p].name; p++)
			if (!strcmp(names[i], m->params[p].name))
				given[p] = values[i];
		if (!strcmp(names[i], "password"))
			*password = values[i];
	}
	if (act_argv(m, given, argv, &bad))
		return -EINVAL;
	if (m->needs_password && !password_ok(*password))
		return -EACCES;
	argv[0] = program_of(m);
	return 0;
}

int bus_act(const char *method, const char *const *names,
	    const char *const *values, size_t n, const char *input, char *out,
	    size_t cap)
{
	const struct act_method *m = act_find(method);
	const char *argv[ACT_ARGV_MAX + 1], *password;
	int err;

	if (cap)
		out[0] = '\0';
	if (!m)
		return -ENOENT;
	err = args_of(m, names, values, n, argv, &password);
	if (err)
		return err;
	return prov_exec(argv[0], argv + 1, m->input ? input : NULL, out, cap);
}

/* What was started without waiting: a program each, whom to tell, its output. */
#define SLOTS 8

static struct slot {
	const char *method;
	pid_t pid;
	int out;
	bus_done done;
	void *ctx;
} g_slots[SLOTS];

/*
 * Start @argv for @m without waiting: @fd as its standard input or output
 * as @m takes it, else @input on its standard input when not NULL. @fd is no
 * longer the caller's, whatever the result.
 */
static int start(const struct act_method *m, const char *const *argv, int fd,
		 const char *input, bus_done done, void *ctx)
{
	struct slot *s = NULL;
	int out[2], in[2] = { -1, -1 };
	pid_t pid;

	for (int i = 0; i < SLOTS && !s; i++)
		if (!g_slots[i].method)
			s = &g_slots[i];
	if (bus_act_running(m->name) || !s) {
		if (fd >= 0)
			close(fd);
		return -EBUSY;
	}
	if (pipe(out)) {
		if (fd >= 0)
			close(fd);
		return -EIO;
	}
	/* The input fits the pipe (ACT_INPUT_MAX), so it is written at once. */
	if (input && m->input && (pipe(in) ||
	    write(in[1], input, strlen(input)) != (ssize_t)strlen(input))) {
		close(out[0]);
		close(out[1]);
		if (in[0] >= 0) {
			close(in[0]);
			close(in[1]);
		}
		if (fd >= 0)
			close(fd);
		return -EIO;
	}
	if (in[1] >= 0)
		close(in[1]);
	pid = fork();
	if (!pid) {
		int null = open("/dev/null", O_RDWR);

		dup2(m->fd_in && fd >= 0 ? fd : in[0] >= 0 ? in[0] : null, 0);
		dup2(m->fd_out && fd >= 0 ? fd : out[1], 1);
		dup2(out[1], 2);
		close(out[0]);
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(out[1]);
	if (in[0] >= 0)
		close(in[0]);
	if (fd >= 0)
		close(fd);
	if (pid < 0) {
		close(out[0]);
		return -EIO;
	}
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	s->method = m->name;
	s->pid = pid;
	s->out = out[0];
	s->done = done;
	s->ctx = ctx;
	return 0;
}

int bus_act_start(const char *method, int fd, const char *password,
		  bus_done done, void *ctx)
{
	const struct act_method *m = act_find(method);
	const char *argv[ACT_ARGV_MAX + 1], *bad;

	if (!m || act_argv(m, NULL, argv, &bad)) {
		if (fd >= 0)
			close(fd);
		return !m ? -ENOENT : -EINVAL;
	}
	if (m->needs_password && !password_ok(password)) {
		if (fd >= 0)
			close(fd);
		return -EACCES;
	}
	argv[0] = program_of(m);
	return start(m, argv, fd, NULL, done, ctx);
}

int bus_act_begin(const char *method, const char *const *names,
		  const char *const *values, size_t n, const char *input,
		  bus_done done, void *ctx)
{
	const struct act_method *m = act_find(method);
	const char *argv[ACT_ARGV_MAX + 1], *password;
	int err;

	if (!m)
		return -ENOENT;
	err = args_of(m, names, values, n, argv, &password);
	if (err)
		return err;
	return start(m, argv, -1, input, done, ctx);
}

bool bus_act_running(const char *method)
{
	for (int i = 0; i < SLOTS; i++)
		if (g_slots[i].method && !strcmp(g_slots[i].method, method))
			return true;
	return false;
}

void bus_poll(void)
{
	for (int i = 0; i < SLOTS; i++) {
		struct slot *s = &g_slots[i];
		char output[BUS_OUTPUT_MAX];
		bus_done done = s->done;
		void *ctx = s->ctx;
		ssize_t n;
		int st;

		if (!s->method || waitpid(s->pid, &st, WNOHANG) != s->pid)
			continue;
		n = read(s->out, output, sizeof(output) - 1);
		output[n > 0 ? n : 0] = '\0';
		close(s->out);
		s->method = NULL;
		s->done = NULL;
		if (done)
			done(ctx, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), output);
	}
}
