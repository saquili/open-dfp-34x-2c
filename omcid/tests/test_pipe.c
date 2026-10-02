// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the pipe transport. */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sys/log.h"
#include "sys/transport_pipe.h"

#include "fault.h"
#include "harness.h"
#include "log_capture.h"

struct rig {
	int in_r, in_w;
	int out_r, out_w;
	struct omci_transport_pipe pipe;
	struct omci_transport t;
};

static bool rig_up(struct rig *r)
{
	int in[2];
	int out[2];

	if (pipe(in) != 0 || pipe(out) != 0)
		return false;
	r->in_r = in[0];
	r->in_w = in[1];
	r->out_r = out[0];
	r->out_w = out[1];

	(void)fcntl(r->out_r, F_SETFL,
		    fcntl(r->out_r, F_GETFL, 0) | O_NONBLOCK);
	omci_transport_pipe_init(&r->pipe, r->in_r, r->out_w, &r->t);
	fault_reset();
	return true;
}

static void rig_down(struct rig *r)
{
	int *fds[] = { &r->in_r, &r->in_w, &r->out_r, &r->out_w };

	for (unsigned int i = 0; i < 4; i++) {
		if (*fds[i] >= 0)
			(void)close(*fds[i]);
		*fds[i] = -1;
	}
	fault_reset();
}

static void fill(uint8_t f[OMCI_FRAME_LEN], uint8_t seed)
{
	for (unsigned int i = 0; i < OMCI_FRAME_LEN; i++)
		f[i] = (uint8_t)(seed + i);
}

static size_t drain(struct rig *r, uint8_t *out, size_t cap)
{
	const ssize_t n = read(r->out_r, out, cap);

	return n < 0 ? 0u : (size_t)n;
}

static void test_init(void)
{
	struct rig r;

	CHECK(rig_up(&r));

	r.pipe.sent = 9;
	r.pipe.broken = true;
	struct omci_transport t2;

	omci_transport_pipe_init(&r.pipe, r.in_r, r.out_w, &t2);

	CHECK_EQ(r.pipe.rx_fd, r.in_r);
	CHECK_EQ(r.pipe.tx_fd, r.out_w);
	CHECK_EQ(r.pipe.sent, 0u);
	CHECK(!r.pipe.broken);
	CHECK(t2.ops != NULL && t2.ctx == &r.pipe);
	CHECK(t2.ops->recv != NULL && t2.ops->send != NULL &&
	      t2.ops->close != NULL);
	CHECK_EQ(t2.ops->rx_fd(t2.ctx), r.in_r);
	rig_down(&r);
}

static void test_recv(void)
{
	struct rig r;

	CHECK(rig_up(&r));
	uint8_t a[OMCI_FRAME_LEN], b[OMCI_FRAME_LEN], got[OMCI_FRAME_LEN];

	fill(a, 1);
	fill(b, 100);

	CHECK_EQ(write(r.in_w, a, sizeof(a)), (ssize_t)sizeof(a));
	CHECK_EQ(write(r.in_w, b, sizeof(b)), (ssize_t)sizeof(b));
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_OK);
	CHECK_MEM(got, a, OMCI_FRAME_LEN);
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_OK);
	CHECK_MEM(got, b, OMCI_FRAME_LEN);

	(void)close(r.in_w);
	r.in_w = -1;
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_DECLINED);
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_DECLINED);
	rig_down(&r);

	CHECK(rig_up(&r));
	CHECK_EQ(write(r.in_w, a, 20), 20);
	(void)close(r.in_w);
	r.in_w = -1;
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_ERR_TRUNCATED);
	rig_down(&r);
}

static void test_recv_signal(void)
{
	struct rig r;

	CHECK(rig_up(&r));
	uint8_t a[OMCI_FRAME_LEN], got[OMCI_FRAME_LEN];

	fill(a, 7);
	CHECK_EQ(write(r.in_w, a, sizeof(a)), (ssize_t)sizeof(a));

	fault_fail(FAULT_READ, 1, EINTR);
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_ERR_AGAIN);
	CHECK_EQ(fault_fired(FAULT_READ), 1u);
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_OK);
	CHECK_MEM(got, a, OMCI_FRAME_LEN);
	rig_down(&r);

	CHECK(rig_up(&r));
	CHECK_EQ(write(r.in_w, a, sizeof(a)), (ssize_t)sizeof(a));
	fault_short(FAULT_READ, 1, 20);
	fault_fail(FAULT_READ, 2, EINTR);
	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_OK);
	CHECK_MEM(got, a, OMCI_FRAME_LEN);
	CHECK_EQ(fault_fired(FAULT_READ), 2u);
	CHECK_EQ(fault_seen(FAULT_READ), 3u);
	rig_down(&r);
}

static void test_recv_error(void)
{
	struct rig r;

	CHECK(rig_up(&r));
	uint8_t got[OMCI_FRAME_LEN];
	char log[512];

	fault_fail(FAULT_READ, 1, EIO);
	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_ERR_IO);
	});
	CHECK_EQ(fault_fired(FAULT_READ), 1u);
	CHECK(strstr(log, "[ERR] transport_pipe.c:") != NULL);
	CHECK(strstr(log, "transport: read failed (errno 5)") != NULL);
	rig_down(&r);

	CHECK(rig_up(&r));
	uint8_t a[OMCI_FRAME_LEN];

	fill(a, 3);
	CHECK_EQ(write(r.in_w, a, sizeof(a)), (ssize_t)sizeof(a));
	fault_short(FAULT_READ, 1, 20);
	fault_fail(FAULT_READ, 2, ENOTCONN);
	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_ERR_IO);
	});
	CHECK(strstr(log, "transport: read failed (errno 107)") != NULL);
	rig_down(&r);
}

static void test_send(void)
{
	struct rig r;

	CHECK(rig_up(&r));
	uint8_t a[OMCI_FRAME_LEN], b[OMCI_FRAME_LEN], got[2 * OMCI_FRAME_LEN];

	fill(a, 11);
	fill(b, 200);

	r.t.ops->send(r.t.ctx, a);
	r.t.ops->send(r.t.ctx, b);
	CHECK_EQ(r.pipe.sent, 2u);
	CHECK(!r.pipe.broken);
	CHECK_EQ(drain(&r, got, sizeof(got)), sizeof(got));
	CHECK_MEM(got, a, OMCI_FRAME_LEN);
	CHECK_MEM(got + OMCI_FRAME_LEN, b, OMCI_FRAME_LEN);

	const unsigned int writes = fault_seen(FAULT_WRITE);

	fault_short(FAULT_WRITE, 1, 20);
	r.t.ops->send(r.t.ctx, a);
	CHECK_EQ(r.pipe.sent, 3u);
	CHECK(!r.pipe.broken);
	CHECK_EQ(fault_seen(FAULT_WRITE) - writes, 2u);
	CHECK_EQ(drain(&r, got, sizeof(got)), OMCI_FRAME_LEN);
	CHECK_MEM(got, a, OMCI_FRAME_LEN);

	fault_fail(FAULT_WRITE, 1, EINTR);
	fault_fail(FAULT_WRITE, 2, EAGAIN);
	r.t.ops->send(r.t.ctx, b);
	CHECK_EQ(r.pipe.sent, 4u);
	CHECK(!r.pipe.broken);
	CHECK_EQ(fault_fired(FAULT_WRITE), 3u);
	CHECK_EQ(drain(&r, got, sizeof(got)), OMCI_FRAME_LEN);
	CHECK_MEM(got, b, OMCI_FRAME_LEN);
	rig_down(&r);
}

static void test_send_failure(void)
{
	struct rig r;

	CHECK(rig_up(&r));
	uint8_t a[OMCI_FRAME_LEN], got[OMCI_FRAME_LEN];
	char log[1024];

	fill(a, 21);

	fault_fail(FAULT_WRITE, 1, EPIPE);
	OMCI_T_CAPTURE_LOG(log, { r.t.ops->send(r.t.ctx, a); });
	CHECK(r.pipe.broken);
	CHECK_EQ(r.pipe.sent, 0u);
	CHECK(strstr(log,
		     "transport: write stopped after 0 of 48 bytes (errno 32)") !=
	      NULL);
	CHECK(strstr(log, "now misaligned") != NULL);
	CHECK_EQ(drain(&r, got, sizeof(got)), 0u);

	uint8_t b[OMCI_FRAME_LEN];

	fill(b, 40);
	CHECK_EQ(write(r.in_w, b, sizeof(b)), (ssize_t)sizeof(b));
	const unsigned int reads = fault_seen(FAULT_READ);

	CHECK_EQ(r.t.ops->recv(r.t.ctx, got), OMCI_ERR_IO);
	CHECK_EQ(fault_seen(FAULT_READ), reads);

	OMCI_T_CAPTURE_LOG(log, { r.t.ops->close(r.t.ctx); });
	CHECK(strstr(log,
		     "transport: 0 answers written before the stream broke") !=
	      NULL);
	rig_down(&r);

	CHECK(rig_up(&r));
	fault_short(FAULT_WRITE, 1, 20);
	fault_fail(FAULT_WRITE, 2, EIO);
	OMCI_T_CAPTURE_LOG(log, { r.t.ops->send(r.t.ctx, a); });
	CHECK(r.pipe.broken);
	CHECK_EQ(r.pipe.sent, 0u);
	CHECK(strstr(log, "write stopped after 20 of 48 bytes (errno 5)") !=
	      NULL);
	CHECK_EQ(drain(&r, got, sizeof(got)), 20u);
	CHECK_MEM(got, a, 20);

	rig_down(&r);
	CHECK(rig_up(&r));
	fault_return(FAULT_WRITE, 1, 0, 0);
	errno = EINTR;
	OMCI_T_CAPTURE_LOG(log, { r.t.ops->send(r.t.ctx, a); });
	CHECK_EQ(fault_fired(FAULT_WRITE), 1u);
	CHECK(r.pipe.broken);
	CHECK_EQ(r.pipe.sent, 0u);
	CHECK(strstr(log, "transport: write stopped after 0 of 48 bytes") !=
	      NULL);
	CHECK_EQ(drain(&r, got, sizeof(got)), 0u);

	rig_down(&r);
	CHECK(rig_up(&r));
	(void)close(r.out_r);
	r.out_r = -1;
	OMCI_T_CAPTURE_LOG(log, { r.t.ops->send(r.t.ctx, a); });
	CHECK(r.pipe.broken);
	CHECK(strstr(log, "(errno 32)") != NULL);
	rig_down(&r);
}

static void test_close(void)
{
	struct rig r;

	CHECK(rig_up(&r));
	uint8_t a[OMCI_FRAME_LEN];
	char log[512];

	fill(a, 5);

	r.t.ops->send(r.t.ctx, a);
	r.t.ops->send(r.t.ctx, a);
	r.t.ops->send(r.t.ctx, a);
	OMCI_T_CAPTURE_LOG(log, { r.t.ops->close(r.t.ctx); });
	CHECK(strstr(log, "[INF] transport_pipe.c:") != NULL);
	CHECK(strstr(log, "transport: 3 answers written\n") != NULL);
	CHECK(strstr(log, "broke") == NULL);

	CHECK_EQ(write(r.in_w, a, 1), 1);
	CHECK(fcntl(r.in_r, F_GETFL) >= 0);
	CHECK(fcntl(r.out_w, F_GETFL) >= 0);
	rig_down(&r);
}

int main(void)
{
	(void)signal(SIGPIPE, SIG_IGN);
	RUN(test_init);
	RUN(test_recv);
	RUN(test_recv_signal);
	RUN(test_recv_error);
	RUN(test_send);
	RUN(test_send_failure);
	RUN(test_close);
	return OMCI_T_REPORT("test_pipe");
}
