// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-act: does, as root, what the programs that run as an account of their
 * own may ask of the stick (ADR 0014, step 8), and nothing else. It is the
 * object "onu-act" on ubus, with a method for each entry of the table of
 * act.h; ubusd's access lists say who may call which. A method runs its
 * program, without a shell, with the caller's values in their places once
 * they pass the table's rules, and the caller's text, "input", on its
 * standard input. The answer comes when the program ends: {"code": its
 * exit status, or 128 and the signal, "output": what it wrote, at most
 * OUTPUT_MAX bytes}. A method run in the background is answered as soon as
 * it starts, {"code": 0}. A program that runs past its method's time is
 * killed.
 *
 * procd keeps it (onu-svc, rcS). Without ubusd it ends, and procd starts it
 * again.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "act.h"

#define OUTPUT_MAX 16384
#define TIMEOUT_S 30

/* A program run for a caller who waits for it. */
struct run {
	struct ubus_request_data req;
	struct uloop_process proc;
	struct uloop_fd out;
	struct uloop_timeout timeout;
	char text[OUTPUT_MAX + 1];
	size_t len;
};

static struct ubus_context *g_ctx;
static struct blob_buf g_buf;

/* Each method's policy: its values, then "input" when it takes text. */
static struct blobmsg_policy g_policy[64][ACT_PARAMS_MAX + 1];
static struct ubus_method g_methods[64];

static size_t params_of(const struct act_method *m)
{
	size_t n = 0;

	while (m->params[n].name)
		n++;
	return n;
}

/* Read what the program wrote, keeping the first OUTPUT_MAX bytes. */
static void drain(struct run *r)
{
	char sink[512];

	for (;;) {
		ssize_t n = r->len < OUTPUT_MAX ?
				    read(r->out.fd, r->text + r->len, OUTPUT_MAX - r->len) :
				    read(r->out.fd, sink, sizeof(sink));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return;
		if (r->len < OUTPUT_MAX)
			r->len += (size_t)n;
	}
}

static void on_output(struct uloop_fd *u, unsigned int events)
{
	(void)events;
	drain(container_of(u, struct run, out));
}

static void on_timeout(struct uloop_timeout *t)
{
	struct run *r = container_of(t, struct run, timeout);

	fprintf(stderr, "onu-act: %d runs too long, and is killed\n", r->proc.pid);
	kill(r->proc.pid, SIGKILL);
}

/*
 * The program has ended: what it wrote is in the pipe, or was read. A
 * program it left behind may hold the pipe open; it is not waited for.
 */
static void ended(struct uloop_process *p, int status)
{
	struct run *r = container_of(p, struct run, proc);

	drain(r);
	r->text[r->len] = '\0';
	uloop_fd_delete(&r->out);
	close(r->out.fd);
	uloop_timeout_cancel(&r->timeout);

	blob_buf_init(&g_buf, 0);
	blobmsg_add_u32(&g_buf, "code",
			WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
	blobmsg_add_string(&g_buf, "output", r->text);
	ubus_send_reply(g_ctx, &r->req, g_buf.head);
	ubus_complete_deferred_request(g_ctx, &r->req, UBUS_STATUS_OK);
	free(r);
}

/* A program run in the background, reaped when it ends. */
static void on_background_exit(struct uloop_process *p, int status)
{
	(void)status;
	free(p);
}

/* Start @argv, with @input on its standard input and its output in @out[1]. */
static pid_t spawn(const char *const *argv, const char *input, int out)
{
	int in[2] = { -1, -1 };
	pid_t pid;

	if (input && pipe2(in, O_CLOEXEC))
		return -1;
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		sigset_t none;

		signal(SIGPIPE, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		dup2(input ? in[0] : null, 0);
		dup2(out >= 0 ? out : null, 1);
		dup2(out >= 0 ? out : null, 2);
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (input) {
		size_t left = strlen(input);

		close(in[0]);
		/* The text is shorter than a pipe holds: it is written at once. */
		while (pid > 0 && left) {
			ssize_t w = write(in[1], input, left);

			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
				break;
			input += w;
			left -= (size_t)w;
		}
		close(in[1]);
	}
	return pid;
}

static int call(struct ubus_context *ctx, struct ubus_object *obj,
		struct ubus_request_data *req, const char *method,
		struct blob_attr *msg)
{
	const struct act_method *m = act_find(method);
	struct blob_attr *tb[ACT_PARAMS_MAX + 1];
	const char *values[ACT_PARAMS_MAX] = { NULL };
	const char *argv[ACT_ARGV_MAX + 1], *bad = NULL, *input = NULL;
	size_t np, i;
	struct run *r;
	int out[2];

	(void)obj;
	if (!m)
		return UBUS_STATUS_METHOD_NOT_FOUND;
	np = params_of(m);
	blobmsg_parse(g_policy[m - act_methods], np + m->input, tb,
		      blob_data(msg), blob_len(msg));
	for (i = 0; i < np; i++)
		values[i] = tb[i] ? blobmsg_get_string(tb[i]) : NULL;
	if (m->input && tb[np])
		input = blobmsg_get_string(tb[np]);
	if (input && strlen(input) > ACT_INPUT_MAX)
		bad = "input";
	if (bad || act_argv(m, values, argv, &bad)) {
		fprintf(stderr, "onu-act: %s: %s refused\n", method, bad);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (m->background) {
		struct uloop_process *p = calloc(1, sizeof(*p));

		if (!p)
			return UBUS_STATUS_UNKNOWN_ERROR;
		p->pid = spawn(argv, input, -1);
		if (p->pid < 0) {
			free(p);
			return UBUS_STATUS_UNKNOWN_ERROR;
		}
		p->cb = on_background_exit;
		uloop_process_add(p);
		blob_buf_init(&g_buf, 0);
		blobmsg_add_u32(&g_buf, "code", 0);
		ubus_send_reply(ctx, req, g_buf.head);
		return UBUS_STATUS_OK;
	}

	r = calloc(1, sizeof(*r));
	if (!r || pipe2(out, O_CLOEXEC)) {
		free(r);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	r->proc.pid = spawn(argv, input, out[1]);
	close(out[1]);
	if (r->proc.pid < 0) {
		close(out[0]);
		free(r);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
	r->out.fd = out[0];
	r->out.cb = on_output;
	uloop_fd_add(&r->out, ULOOP_READ);
	r->proc.cb = ended;
	uloop_process_add(&r->proc);
	r->timeout.cb = on_timeout;
	uloop_timeout_set(&r->timeout, 1000 * (m->timeout_s ? m->timeout_s : TIMEOUT_S));
	ubus_defer_request(ctx, req, &r->req);
	return UBUS_STATUS_OK;
}

static struct ubus_object_type g_type = { .name = "onu-act" };

static struct ubus_object g_object = {
	.name = "onu-act",
	.type = &g_type,
	.methods = g_methods,
};

static void lost(struct ubus_context *ctx)
{
	(void)ctx;
	fprintf(stderr, "onu-act: ubusd went away\n");
	uloop_end();
}

int main(void)
{
	int err;

	if (act_method_count > sizeof(g_methods) / sizeof(g_methods[0]))
		return 1;
	for (size_t i = 0; i < act_method_count; i++) {
		const struct act_method *m = &act_methods[i];
		size_t np = params_of(m);

		for (size_t k = 0; k < np; k++)
			g_policy[i][k] = (struct blobmsg_policy){
				.name = m->params[k].name, .type = BLOBMSG_TYPE_STRING
			};
		if (m->input)
			g_policy[i][np] = (struct blobmsg_policy){
				.name = "input", .type = BLOBMSG_TYPE_STRING
			};
		g_methods[i] = (struct ubus_method){
			.name = m->name, .handler = call,
			.policy = g_policy[i], .n_policy = (int)(np + m->input),
		};
	}
	g_object.n_methods = (int)act_method_count;
	g_type.methods = g_methods;
	g_type.n_methods = (int)act_method_count;

	signal(SIGPIPE, SIG_IGN);
	uloop_init();
	g_ctx = ubus_connect(NULL);
	if (!g_ctx) {
		fprintf(stderr, "onu-act: no ubusd\n");
		return 1;
	}
	g_ctx->connection_lost = lost;
	ubus_add_uloop(g_ctx);
	err = ubus_add_object(g_ctx, &g_object);
	if (err) {
		fprintf(stderr, "onu-act: not on ubus: %s\n", ubus_strerror(err));
		return 1;
	}
	uloop_run();
	ubus_free(g_ctx);
	uloop_done();
	return 0;
}
