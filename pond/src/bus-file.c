// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* bus.h from files, for the tests and a build without libubus. */

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

int bus_act(const char *method, const char *const *names,
	    const char *const *values, size_t n, const char *input, char *out,
	    size_t cap)
{
	const struct act_method *m = act_find(method);
	const char *given[ACT_PARAMS_MAX] = { NULL };
	const char *argv[ACT_ARGV_MAX + 1], *bad;

	if (cap)
		out[0] = '\0';
	if (!m)
		return -ENOENT;
	for (size_t p = 0; p < ACT_PARAMS_MAX && m->params[p].name; p++)
		for (size_t i = 0; i < n; i++)
			if (!strcmp(names[i], m->params[p].name))
				given[p] = values[i];
	if (act_argv(m, given, argv, &bad))
		return -EINVAL;
	argv[0] = program_of(m);
	return prov_exec(argv[0], argv + 1, m->input ? input : NULL, out, cap);
}

/* What bus_act_start() started: a program each, whom to tell, its output. */
#define SLOTS 4

static struct slot {
	const char *method;
	pid_t pid;
	int out;
	void (*done)(int code, const char *output);
} g_slots[SLOTS];

int bus_act_start(const char *method, int fd,
		  void (*done)(int code, const char *output))
{
	const struct act_method *m = act_find(method);
	const char *argv[ACT_ARGV_MAX + 1], *bad;
	struct slot *s = NULL;
	int out[2];
	pid_t pid;

	for (int i = 0; i < SLOTS && !s; i++)
		if (!g_slots[i].method)
			s = &g_slots[i];
	if (!m || bus_act_running(method) || !s || act_argv(m, NULL, argv, &bad) ||
	    pipe(out)) {
		if (fd >= 0)
			close(fd);
		return !m ? -ENOENT : !s || bus_act_running(method) ? -EBUSY : -EINVAL;
	}
	argv[0] = program_of(m);
	pid = fork();
	if (!pid) {
		int null = open("/dev/null", O_RDWR);

		dup2(m->fd_in && fd >= 0 ? fd : null, 0);
		dup2(m->fd_out && fd >= 0 ? fd : out[1], 1);
		dup2(out[1], 2);
		close(out[0]);
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(out[1]);
	if (fd >= 0)
		close(fd);
	if (pid < 0) {
		close(out[0]);
		return -EIO;
	}
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	s->method = method;
	s->pid = pid;
	s->out = out[0];
	s->done = done;
	return 0;
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
		ssize_t n;
		int st;

		if (!s->method || waitpid(s->pid, &st, WNOHANG) != s->pid)
			continue;
		n = read(s->out, output, sizeof(output) - 1);
		output[n > 0 ? n : 0] = '\0';
		close(s->out);
		s->method = NULL;
		if (s->done)
			s->done(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), output);
	}
}
