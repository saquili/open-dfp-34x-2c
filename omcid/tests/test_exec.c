// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that run the daemon as a program and talk to it through pipes. */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "omci/cases.h"
#include "omcid/array_size.h"
#include "omcid/ctl_proto.h"
#include "sys/log.h"

#include "harness.h"

#define FAILSAFE_MS 10000

struct child {
	pid_t pid;
	int in;
	int out;
	FILE *err;
};

static bool child_start(struct child *c, const char *bin, char *const argv[])
{
	int to_child[2], from_child[2];

	c->err = tmpfile();
	if (c->err == NULL || pipe(to_child) != 0)
		return false;
	if (pipe(from_child) != 0)
		return false;

	c->pid = fork();
	if (c->pid < 0)
		return false;
	if (c->pid == 0) {
		(void)dup2(to_child[0], 0);
		(void)dup2(from_child[1], 1);
		(void)dup2(fileno(c->err), 2);
		(void)close(to_child[0]);
		(void)close(to_child[1]);
		(void)close(from_child[0]);
		(void)close(from_child[1]);
		execv(bin, argv);
		_exit(127);
	}
	(void)close(to_child[0]);
	(void)close(from_child[1]);
	c->in = to_child[1];
	c->out = from_child[0];
	return true;
}

static bool child_read(struct child *c, uint8_t *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		struct pollfd p = { .fd = c->out, .events = POLLIN };

		if (poll(&p, 1, FAILSAFE_MS) <= 0)
			return false;
		const ssize_t k = read(c->out, buf + got, n - got);

		if (k <= 0)
			return false;
		got += (size_t)k;
	}
	return true;
}

static bool child_write(struct child *c, const uint8_t *buf, size_t n)
{
	return write(c->in, buf, n) == (ssize_t)n;
}

static int child_finish(struct child *c, uint8_t *rest, size_t cap,
			size_t *rest_n)
{
	(void)close(c->in);
	size_t got = 0;

	for (;;) {
		struct pollfd p = { .fd = c->out, .events = POLLIN };

		if (poll(&p, 1, FAILSAFE_MS) <= 0)
			break;
		const ssize_t k =
			read(c->out, rest + got, got < cap ? cap - got : 0);
		if (k <= 0)
			break;
		got += (size_t)k;
	}
	*rest_n = got;
	(void)close(c->out);
	int status = -1;
	(void)waitpid(c->pid, &status, 0);
	return status;
}

static void child_stderr(struct child *c, char *out, size_t cap)
{
	(void)fflush(c->err);
	(void)fseek(c->err, 0, SEEK_END);
	const long size = ftell(c->err);
	(void)fseek(c->err,
		    size > (long)(cap - 1u) ? size - (long)(cap - 1u) : 0L,
		    SEEK_SET);
	const size_t n = fread(out, 1, cap - 1u, c->err);

	out[n] = '\0';
	(void)fclose(c->err);
}

static int run_plain(const char *bin, char *const argv[], size_t *out_n,
		     char *err, size_t err_cap)
{
	struct child c;
	uint8_t rest[4096];

	if (!child_start(&c, bin, argv))
		return -1;
	const int status = child_finish(&c, rest, sizeof(rest), out_n);

	child_stderr(&c, err, err_cap);
	return status;
}

static bool g_made_socket;

static void remember_socket(void)
{
	struct stat st;

	g_made_socket = stat(OMCI_CTL_SOCKET_PATH, &st) != 0;
}
static void forget_socket(void)
{
	if (g_made_socket)
		(void)unlink(OMCI_CTL_SOCKET_PATH);
}

struct slice {
	enum omci_case_axis axis;
	uint32_t stride;
};

int main(int argc, char **argv)
{
	if (argc != 2) {
		printf("test_exec: the first argument is the path of the omcid binary\n");
		return 1;
	}
	const char *bin = argv[1];
	(void)signal(SIGPIPE, SIG_IGN);
	omci_log_level_set(OMCI_LOG_ERR);
	remember_socket();

	{
		char err[2048];
		size_t out_n = 99;

		char *const help[] = { (char *)bin, (char *)"-h", NULL };
		int status = run_plain(bin, help, &out_n, err, sizeof(err));

		CHECK(WIFEXITED(status));
		CHECK_EQ(WEXITSTATUS(status), 0);
		CHECK_EQ(out_n, 0u);
		CHECK(strstr(err, "usage: ") != NULL);
		char says[512];
		(void)snprintf(says, sizeof(says), "usage: %s ", bin);
		CHECK(strstr(err, says) != NULL);
		CHECK(strstr(err, "-p  frames come on the standard input") !=
		      NULL);
		CHECK(strstr(err, "starting") == NULL);

		char *const bad[] = { (char *)bin, (char *)"-p", (char *)"-z",
				      NULL };
		status = run_plain(bin, bad, &out_n, err, sizeof(err));
		CHECK(WIFEXITED(status));
		CHECK_EQ(WEXITSTATUS(status), 1);
		CHECK(strstr(err, "omcid: unknown argument '-z'") != NULL);
		CHECK(strstr(err, says) != NULL);
		CHECK(strstr(err, "starting") == NULL);

		char *const plain[] = { (char *)bin, (char *)"-p", NULL };

		status = run_plain(bin, plain, &out_n, err, sizeof(err));
		CHECK(WIFEXITED(status));
		CHECK_EQ(WEXITSTATUS(status), 0);
		CHECK(strstr(err, "omcid 0.1.0 starting") != NULL);
		CHECK(strstr(err, "transport: stdin/stdout") != NULL);
		CHECK(strstr(err, "mib ready: 33 instances") != NULL);
		CHECK(strstr(err, "stopping: 0 frames handled, 0 rejected") !=
		      NULL);
		CHECK(out_n > 0u && out_n % OMCI_FRAME_LEN == 0u);
		CHECK(strstr(err, "[DBG]") == NULL &&
		      strstr(err, "[TRC]") == NULL);

		char *const v1[] = { (char *)bin, (char *)"-v", (char *)"-p",
				     NULL };
		status = run_plain(bin, v1, &out_n, err, sizeof(err));
		CHECK_EQ(WEXITSTATUS(status), 0);
		CHECK(strstr(err, "[DBG]") != NULL);

		CHECK(strstr(err, "[TRC]") == NULL);
	}

	{
		char *const v3[] = { (char *)bin,  (char *)"-v", (char *)"-v",
				     (char *)"-v", (char *)"-p", NULL };
		struct child c;

		CHECK(child_start(&c, bin, v3));
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(OMCI_CASE_MSGTYPE, 2, wire);
		CHECK(child_write(&c, wire, sizeof(wire)));
		uint8_t rest[8192];
		size_t rest_n = 0;
		const int status =
			child_finish(&c, rest, sizeof(rest), &rest_n);
		char err[8192];

		child_stderr(&c, err, sizeof(err));
		CHECK(WIFEXITED(status));
		CHECK_EQ(WEXITSTATUS(status), 0);
		CHECK(strstr(err, "[DBG]") != NULL);
		CHECK(strstr(err, "[TRC] engine.c") != NULL);
		CHECK(strstr(err, "rx tid=") != NULL);
	}

	{
		struct omci_t_fixture fx;
		struct omci_hw_caps caps;

		CHECK(omci_t_up(&fx));
		CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);
		omci_case_prepare(OMCI_CASE_POPULATED, fx.mib);

		static uint8_t startup[OMCI_T_MAX_TX][OMCI_FRAME_LEN];
		const unsigned int n0 = omci_t_tx.n < OMCI_T_MAX_TX ?
						omci_t_tx.n :
						OMCI_T_MAX_TX;
		memcpy(startup, omci_t_tx.frame, (size_t)n0 * OMCI_FRAME_LEN);
		CHECK(n0 > 0u);

		char *const plain[] = { (char *)bin, (char *)"-p", NULL };
		struct child c;

		CHECK(child_start(&c, bin, plain));

		bool in_step = true;

		for (unsigned int i = 0; i < n0 && in_step; i++) {
			uint8_t got[OMCI_FRAME_LEN];

			in_step = child_read(&c, got, sizeof(got));
			CHECK(in_step);
			if (in_step)
				CHECK_MEM(got, startup[i], OMCI_FRAME_LEN);
		}

		static const struct slice slices[] = {
			{ OMCI_CASE_POPULATED, 1u }, { OMCI_CASE_MSGTYPE, 1u },
			{ OMCI_CASE_MTCLASS, 1u },   { OMCI_CASE_ENVELOPE, 2u },
			{ OMCI_CASE_MUTATE, 61u },
		};
		unsigned long sent = 0, answers = n0, handled = 0, rejected = 0,
			      mismatched = 0, silent_wrong = 0;
		for (size_t s = 0; s < ARRAY_SIZE(slices) && in_step; s++) {
			const uint32_t n = omci_case_count(slices[s].axis);

			for (uint32_t i = 0; i < n && in_step;
			     i += slices[s].stride) {
				uint8_t wire[OMCI_FRAME_LEN];
				(void)omci_case_build(slices[s].axis, i, wire);

				omci_t_tx_reset();
				const enum omci_status rc =
					omci_engine_rx(fx.eng, wire);
				if (rc == OMCI_OK)
					handled++;
				else
					rejected++;
				const unsigned int k =
					omci_t_tx.n < OMCI_T_MAX_TX ?
						omci_t_tx.n :
						OMCI_T_MAX_TX;

				if (!child_write(&c, wire, sizeof(wire))) {
					in_step = false;
					break;
				}
				sent++;
				for (unsigned int f = 0; f < k; f++) {
					uint8_t got[OMCI_FRAME_LEN];

					if (!child_read(&c, got, sizeof(got))) {
						in_step = false;
						break;
					}
					answers++;
					if (memcmp(got, omci_t_tx.frame[f],
						   OMCI_FRAME_LEN) != 0)
						mismatched++;
				}
			}
		}
		CHECK(in_step);
		CHECK(sent > 3000u);
		CHECK_EQ(mismatched, 0u);

		uint8_t rest[OMCI_FRAME_LEN];
		size_t rest_n = 0;
		const int status =
			child_finish(&c, rest, sizeof(rest), &rest_n);
		silent_wrong = rest_n;
		CHECK_EQ(silent_wrong, 0u);
		CHECK(WIFEXITED(status));
		CHECK_EQ(WEXITSTATUS(status), 0);

		char err[4096];

		child_stderr(&c, err, sizeof(err));
		char want[96];
		(void)snprintf(want, sizeof(want),
			       "stopping: %lu frames handled, %lu rejected",
			       handled, rejected);
		CHECK(strstr(err, want) != NULL);
		(void)snprintf(want, sizeof(want),
			       "transport: %lu answers written", answers);
		CHECK(strstr(err, want) != NULL);
		CHECK(strstr(err, "[ERR]") == NULL);

		CHECK(handled > 1000u);
		CHECK(rejected > 0u);
		printf("  conversation: %lu frames sent, %lu answers read (%u of them before the first frame), %lu handled, %lu rejected\n",
		       sent, answers, n0, handled, rejected);
		omci_t_down(&fx);
	}

	{
		char *const plain[] = { (char *)bin, (char *)"-p", NULL };
		struct child c;

		CHECK(child_start(&c, bin, plain));
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(OMCI_CASE_MSGTYPE, 0, wire);
		CHECK(child_write(&c, wire, 20));
		uint8_t rest[8192];
		size_t rest_n = 0;
		const int status =
			child_finish(&c, rest, sizeof(rest), &rest_n);
		char err[4096];

		child_stderr(&c, err, sizeof(err));
		CHECK(WIFEXITED(status));
		CHECK_EQ(WEXITSTATUS(status), 1);
		CHECK(strstr(err, "transport: OMCI_ERR_TRUNCATED") != NULL);
		CHECK(strstr(err, "stopping:") == NULL);
	}

	forget_socket();
	return OMCI_T_REPORT("test_exec");
}
