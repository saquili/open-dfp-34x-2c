// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the main loop of the daemon. */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "daemon.h"
#include "hw/hw.h"
#include "me/defaults.h"
#include "omci/alarm.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "sys/history.h"
#include "sys/log.h"

#include "fault.h"
#include "harness.h"
#include "log_capture.h"

#define MT_GET 9u
#define MT_MIB_RESET 15u

#define N_FRAMES 7u

static uint8_t g_frames[N_FRAMES][OMCI_FRAME_LEN];
static char g_dir[64];
static char g_sock[128];
static char g_log[1u << 15];
static uint8_t g_out[64u * 1024u];

static void get_frame(uint8_t out[OMCI_FRAME_LEN], uint16_t tci, uint16_t cls,
		      uint16_t inst, uint16_t mask)
{
	struct omci_frame f = {
		.tci = tci,
		.msg_type = MT_GET,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = cls,
		.instance = inst,
	};
	f.contents[0] = (uint8_t)(mask >> 8);
	f.contents[1] = (uint8_t)mask;
	(void)!omci_frame_build(&f, out);
}

static void make_frames(void)
{
	get_frame(g_frames[0], 1, 256, 0, 0xffff);
	get_frame(g_frames[1], 2, 257, 0, 0xffff);
	get_frame(g_frames[2], 3, 9999, 0, 0xffff);
	omci_t_build(g_frames[3], 4, MT_MIB_RESET, true, false, 2, 0);
	get_frame(g_frames[4], 5, 263, 0x8001, 0xffff);
	get_frame(g_frames[5], 6, 256, 0, 0xffff);
	g_frames[5][OMCI_FRAME_LEN - 1] ^= 0x01;
	get_frame(g_frames[6], 7, 134, 0, 0xffff);
}

static uint8_t g_ref[64u * 1024u];
static size_t g_ref_n;
static size_t g_ref_startup;
static unsigned int g_ref_handled, g_ref_rejected;
static unsigned int g_ref_replies[N_FRAMES];

static void ref_append(void)
{
	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++) {
		memcpy(g_ref + g_ref_n, omci_t_tx.frame[i], OMCI_FRAME_LEN);
		g_ref_n += OMCI_FRAME_LEN;
	}
}

static void reference(unsigned int n)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;

	omci_log_level_set(OMCI_LOG_ERR);
	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);

	/*
	 * The daemon judges its alarms and reads the optical levels before
	 * it answers the first frame: so does the engine here.
	 */
	omci_engine_tick(fx.eng, 1u);
	g_ref_n = 0;
	ref_append();
	g_ref_startup = g_ref_n;
	g_ref_handled = 0;
	g_ref_rejected = 0;
	for (unsigned int i = 0; i < n; i++) {
		omci_t_tx_reset();
		if (omci_engine_rx(fx.eng, g_frames[i]) == OMCI_OK)
			g_ref_handled++;
		else
			g_ref_rejected++;
		g_ref_replies[i] = omci_t_tx.n;
		ref_append();
	}
	omci_t_down(&fx);
}

struct rig {
	int in_r, in_w, out_r, out_w;
};

static bool rig_up(struct rig *g)
{
	int in[2];
	int out[2];

	if (pipe(in) != 0 || pipe(out) != 0)
		return false;
	g->in_r = in[0];
	g->in_w = in[1];
	g->out_r = out[0];
	g->out_w = out[1];
	(void)fcntl(g->out_r, F_SETFL,
		    fcntl(g->out_r, F_GETFL, 0) | O_NONBLOCK);
	fault_reset();
	return true;
}

static void feed(struct rig *g, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		CHECK_EQ(write(g->in_w, g_frames[i], OMCI_FRAME_LEN),
			 (ssize_t)OMCI_FRAME_LEN);
}

static void end_input(struct rig *g)
{
	if (g->in_w >= 0)
		(void)close(g->in_w);
	g->in_w = -1;
}

static size_t collect(struct rig *g, uint8_t *buf, size_t cap)
{
	size_t n = 0;

	for (;;) {
		const ssize_t k = read(g->out_r, buf + n, cap - n);

		if (k <= 0)
			break;
		n += (size_t)k;
	}
	return n;
}

static void rig_down(struct rig *g)
{
	int *fds[] = { &g->in_r, &g->in_w, &g->out_r, &g->out_w };

	for (unsigned int i = 0; i < 4; i++) {
		if (*fds[i] >= 0)
			(void)close(*fds[i]);
		*fds[i] = -1;
	}
	fault_reset();
}

static int run(struct rig *g, int rx_fd, const char *sock, int argc,
	       char **argv)
{
	int status = -1;

	OMCI_T_CAPTURE_LOG(g_log, {
		status = omcid_run(argc, argv, rx_fd, g->out_w, sock);
	});
	return status;
}

static char *g_argv[] = { (char *)"omcid", (char *)"-p", NULL };

/*
 * The frames whose answers are compared with the reference: the daemon's
 * clock holds at the second the reference engine was given, so that no
 * second passing between two frames has it judge again what the reference
 * judged once.
 */
static char *g_argv_held[] = { (char *)"omcid", (char *)"-p", (char *)"-t",
			       (char *)"1", NULL };

static int run_frames(struct rig *g, unsigned int n, const char *sock)
{
	feed(g, n);
	end_input(g);
	return run(g, g->in_r, sock, 4, g_argv_held);
}

static void check_output(struct rig *g)
{
	const size_t len = collect(g, g_out, sizeof(g_out));

	CHECK_EQ(len, g_ref_n);
	CHECK_MEM(g_out, g_ref, len < g_ref_n ? len : g_ref_n);
}

/* A log appended to a file is cut once it passes 256 KiB. */
static void test_the_log_is_cut_when_full(void)
{
	char path[160];
	struct stat st;
	int fd, saved;

	snprintf(path, sizeof(path), "%s/log", g_dir);
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0600);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	fflush(stderr);
	saved = dup(2);
	(void)dup2(fd, 2);
	for (int i = 0; i < 4000; i++)
		OMCI_INFO(
			"a line of the log that is long enough to fill it soon, %d",
			i);
	fflush(stderr);
	(void)dup2(saved, 2);
	(void)close(saved);
	(void)close(fd);
	CHECK(stat(path, &st) == 0 && st.st_size > 0 &&
	      st.st_size <= 256 * 1024 + 256);
	(void)unlink(path);
}

static void test_end_of_input(void)
{
	struct rig g;

	CHECK(rig_up(&g));
	reference(N_FRAMES);
	CHECK(g_ref_handled > 0u && g_ref_rejected > 0u);

	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_SUCCESS);
	check_output(&g);

	CHECK(strstr(g_log, "omcid 0.1.0 starting") != NULL);
	CHECK(strstr(g_log, "transport: stdin/stdout") != NULL);
	CHECK(strstr(g_log, "hw: synthetic ONU") != NULL);
	CHECK(strstr(g_log, "mib ready: 38 instances") != NULL);
	char want[128 + sizeof(g_sock)];
	(void)snprintf(want, sizeof(want), "ctl: listening on %s", g_sock);
	CHECK(strstr(g_log, want) != NULL);
	(void)snprintf(want, sizeof(want),
		       "stopping: %u frames handled, %u rejected",
		       g_ref_handled, g_ref_rejected);
	CHECK(strstr(g_log, want) != NULL);
	(void)snprintf(want, sizeof(want), "transport: %zu answers written\n",
		       g_ref_n / OMCI_FRAME_LEN);
	CHECK(strstr(g_log, want) != NULL);
	CHECK(strstr(g_log, "[ERR]") == NULL);

	struct sigaction sa;

	CHECK_EQ(sigaction(SIGPIPE, NULL, &sa), 0);
	CHECK(sa.sa_handler == SIG_IGN);
	CHECK_EQ(sigaction(SIGTERM, NULL, &sa), 0);
	CHECK(sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN);
	CHECK_EQ(sigaction(SIGINT, NULL, &sa), 0);
	CHECK(sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN);
	rig_down(&g);
	(void)unlink(g_sock);
}

static void test_signal_stops_the_loop(void)
{
	static const int sigs[] = { SIGTERM, SIGINT };

	for (unsigned int i = 0; i < ARRAY_SIZE(sigs); i++) {
		struct rig g;

		CHECK(rig_up(&g));
		reference(0);

		fault_signal(FAULT_POLL, 1, sigs[i]);
		CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
		CHECK_EQ(fault_fired(FAULT_POLL), 1u);
		CHECK_EQ(fault_seen(FAULT_POLL), 1u);
		CHECK(strstr(g_log, "stopping: 0 frames handled, 0 rejected") !=
		      NULL);
		CHECK(strstr(g_log, "poll failed") == NULL);
		CHECK_EQ(collect(&g, g_out, sizeof(g_out)), g_ref_startup);
		rig_down(&g);
		(void)unlink(g_sock);

		CHECK(rig_up(&g));
		reference(N_FRAMES);
		CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_SUCCESS);
		check_output(&g);
		rig_down(&g);
		(void)unlink(g_sock);
	}
}

static unsigned int g_mode_calls;
static bool g_mode_router;
static unsigned int g_bridge_applies;
static unsigned int g_mode_dones;
static bool g_mode_done_router;
static unsigned int g_applies_at_done;

static void count_done(bool router)
{
	g_mode_dones++;
	g_mode_done_router = router;
	g_applies_at_done = g_bridge_applies;
}

static void count_mode(bool on)
{
	g_mode_calls++;
	g_mode_router = on;
}

static enum omci_status count_bridge(const struct omci_hw_bridge_port *ports,
				     size_t n, struct omci_hw_reject *rejected)
{
	g_bridge_applies++;
	return omci_hw_host_ops.bridge_apply(ports, n, rejected);
}

/*
 * SIGUSR1 and SIGUSR2 change the backend's mode and program the data path
 * again; a backend without a router mode is programmed again all the same.
 */
static void test_signals_change_the_mode(void)
{
	static const int sigs[] = { SIGUSR1, SIGUSR2, SIGUSR1 };
	struct omci_hw_ops ops = omci_hw_host_ops;

	ops.set_router = count_mode;
	ops.bridge_apply = count_bridge;
	ops.mode_done = count_done;
	for (unsigned int i = 0; i < ARRAY_SIZE(sigs); i++) {
		const bool host = i == 2;
		const char *want = sigs[i] == SIGUSR1 ? "mode: router" :
							"mode: bridge";
		struct rig g;

		CHECK(rig_up(&g));
		g_mode_calls = 0;
		g_bridge_applies = 0;
		g_mode_dones = 0;
		if (!host)
			omci_hw_ops_set(&ops);
		fault_signal(FAULT_POLL, 1, sigs[i]);
		end_input(&g);
		CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
		omci_hw_ops_set(NULL);

		CHECK_EQ(fault_fired(FAULT_POLL), 1u);
		CHECK(strstr(g_log, want) != NULL);
		CHECK_EQ(g_mode_calls, host ? 0u : 1u);
		CHECK(host || g_mode_router == (sigs[i] == SIGUSR1));
		CHECK_EQ(g_bridge_applies, host ? 0u : 1u);
		/* Told once the data path is programmed again, not before. */
		CHECK_EQ(g_mode_dones, host ? 0u : 1u);
		CHECK(host || g_mode_done_router == (sigs[i] == SIGUSR1));
		CHECK(host || g_applies_at_done == 1u);
		CHECK(strstr(g_log, "[ERR]") == NULL);
		rig_down(&g);
		(void)unlink(g_sock);
	}

	struct sigaction sa;

	CHECK_EQ(sigaction(SIGUSR1, NULL, &sa), 0);
	CHECK(sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN);
	CHECK_EQ(sigaction(SIGUSR2, NULL, &sa), 0);
	CHECK(sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN);
}

static unsigned int g_due_calls;

static bool due_once(void)
{
	return g_due_calls++ == 0;
}

/*
 * A backend that has changed what of the MIB it applies has the data path
 * programmed again, once for each time it says so.
 */
static void test_the_backend_asks_for_programming_again(void)
{
	struct omci_hw_ops ops = omci_hw_host_ops;
	struct rig g;

	ops.reprogram_due = due_once;
	ops.bridge_apply = count_bridge;
	CHECK(rig_up(&g));
	g_due_calls = 0;
	g_bridge_applies = 0;
	omci_hw_ops_set(&ops);
	end_input(&g);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
	omci_hw_ops_set(NULL);

	CHECK(g_due_calls >= 1u);
	CHECK_EQ(g_bridge_applies, 1u);
	CHECK(strstr(g_log, "the data path is programmed again from the MIB") !=
	      NULL);
	CHECK(strstr(g_log, "[ERR]") == NULL);
	rig_down(&g);
	(void)unlink(g_sock);
}

static int g_mcast_pipe[2] = { -1, -1 };
static unsigned int g_mcast_reads;

static int pipe_mcast_fd(void)
{
	return g_mcast_pipe[0];
}

static enum omci_status count_mcast_frame(struct omci_hw_mcast_frame *out)
{
	g_mcast_reads++;
	return omci_hw_mcast_frame(out);
}

/* The multicast agent's descriptor is waited on, and served when readable. */
static void test_the_multicast_descriptor_is_served(void)
{
	struct omci_hw_ops ops = omci_hw_host_ops;
	struct rig g;

	CHECK_EQ(pipe(g_mcast_pipe), 0);
	CHECK_EQ(write(g_mcast_pipe[1], "x", 1), 1);
	ops.mcast_fd = pipe_mcast_fd;
	ops.mcast_frame = count_mcast_frame;
	CHECK(rig_up(&g));
	g_mcast_reads = 0;
	omci_hw_ops_set(&ops);
	end_input(&g);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
	omci_hw_ops_set(NULL);
	CHECK(g_mcast_reads >= 2u);
	CHECK(strstr(g_log, "[ERR]") == NULL);
	rig_down(&g);
	(void)unlink(g_sock);
	(void)close(g_mcast_pipe[0]);
	(void)close(g_mcast_pipe[1]);
}

static int g_pon_pipe[2] = { -1, -1 };
static unsigned int g_pon_drains;

static int pipe_pon_fd(void)
{
	return g_pon_pipe[0];
}

static void read_pon_pipe(void)
{
	char c;

	g_pon_drains++;
	(void)!read(g_pon_pipe[0], &c, 1);
}

/* The PON device's descriptor is waited on, and served when readable. */
static void test_the_pon_descriptor_is_served(void)
{
	struct omci_hw_ops ops = omci_hw_host_ops;
	struct rig g;

	CHECK_EQ(pipe(g_pon_pipe), 0);
	CHECK_EQ(write(g_pon_pipe[1], "x", 1), 1);
	ops.pon_fd = pipe_pon_fd;
	ops.pon_drain = read_pon_pipe;
	CHECK(rig_up(&g));
	g_pon_drains = 0;
	omci_hw_ops_set(&ops);
	end_input(&g);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
	omci_hw_ops_set(NULL);
	CHECK_EQ(g_pon_drains, 1u);
	CHECK(strstr(g_log, "[ERR]") == NULL);
	rig_down(&g);
	(void)unlink(g_sock);
	(void)close(g_pon_pipe[0]);
	(void)close(g_pon_pipe[1]);
}

static void test_poll(void)
{
	struct rig g;

	CHECK(rig_up(&g));
	reference(N_FRAMES);
	fault_return(FAULT_POLL, 1, 0, 0);
	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_SUCCESS);
	CHECK_EQ(fault_fired(FAULT_POLL), 1u);
	CHECK(fault_seen(FAULT_POLL) > 1u);
	check_output(&g);
	CHECK(strstr(g_log, "[ERR]") == NULL);
	rig_down(&g);
	(void)unlink(g_sock);

	CHECK(rig_up(&g));
	reference(0);
	fault_fail(FAULT_POLL, 1, EIO);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_FAILURE);
	CHECK(strstr(g_log, "[ERR] daemon.c:") != NULL);
	CHECK(strstr(g_log, "poll failed (errno 5)") != NULL);
	CHECK(strstr(g_log, "stopping:") == NULL);
	char want[96];
	(void)snprintf(want, sizeof(want), "transport: %zu answers written\n",
		       g_ref_startup / OMCI_FRAME_LEN);
	CHECK(strstr(g_log, want) != NULL);
	rig_down(&g);
	(void)unlink(g_sock);
}

static void test_transport_errors(void)
{
	struct rig g;
	char want[128];

	CHECK(rig_up(&g));
	reference(N_FRAMES);
	feed(&g, N_FRAMES);
	end_input(&g);
	fault_fail(FAULT_READ, 1, EINTR);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
	CHECK_EQ(fault_fired(FAULT_READ), 1u);
	check_output(&g);
	rig_down(&g);
	(void)unlink(g_sock);

	CHECK(rig_up(&g));
	feed(&g, N_FRAMES);
	fault_fail(FAULT_READ, 1, EIO);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_FAILURE);
	CHECK(strstr(g_log, "transport: read failed (errno 5)") != NULL);
	CHECK(strstr(g_log, "] daemon.c:") != NULL &&
	      strstr(g_log, "transport: OMCI_ERR_IO") != NULL);
	CHECK(strstr(g_log, "stopping:") == NULL);
	rig_down(&g);
	(void)unlink(g_sock);

	CHECK(rig_up(&g));
	reference(0);
	feed(&g, N_FRAMES);
	end_input(&g);
	fault_fail(FAULT_WRITE, 1, EPIPE);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_FAILURE);
	CHECK(strstr(g_log, "write stopped after 0 of 48 bytes (errno 32)") !=
	      NULL);
	CHECK(strstr(g_log, "transport: OMCI_ERR_IO") != NULL);
	(void)snprintf(want, sizeof(want),
		       "transport: %zu answers written before the stream broke",
		       g_ref_startup / OMCI_FRAME_LEN - 1u);
	CHECK(strstr(g_log, want) != NULL);
	CHECK(strstr(g_log, "stopping:") == NULL);
	rig_down(&g);
	(void)unlink(g_sock);

	CHECK(rig_up(&g));
	CHECK_EQ(run(&g, -1, "/nonexistent-directory/omcid.sock", 2, g_argv),
		 EXIT_SUCCESS);
	CHECK(strstr(g_log,
		     "ctl: cannot listen on /nonexistent-directory/omcid.sock") !=
	      NULL);
	CHECK(strstr(g_log, "ctl: running without a control socket") != NULL);
	CHECK(strstr(g_log, "no descriptor left to wait on") != NULL);
	CHECK(strstr(g_log, "stopping: 0 frames handled, 0 rejected") != NULL);
	rig_down(&g);
}

static enum omci_status caps_fail(struct omci_hw_caps *out)
{
	(void)out;
	return OMCI_ERR_HW;
}

static enum omci_status caps_huge(struct omci_hw_caps *out)
{
	const enum omci_status rc = omci_hw_caps(out);

	out->num_tcont = 30;
	out->num_priq_per_tcont = 16;
	return rc;
}

static void check_clean_after(void)
{
	struct rig g;

	CHECK(rig_up(&g));
	reference(N_FRAMES);
	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_SUCCESS);
	check_output(&g);
	rig_down(&g);
	(void)unlink(g_sock);
}

static void test_setup_failures(void)
{
	struct rig g;

	struct omci_mib *held = NULL;

	CHECK_EQ(omci_mib_init(&held), OMCI_OK);
	CHECK(rig_up(&g));
	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_FAILURE);
	CHECK(strstr(g_log, "mib init failed: OMCI_ERR_BUSY") != NULL);
	CHECK(strstr(g_log, "hw:") == NULL);
	CHECK_EQ(collect(&g, g_out, sizeof(g_out)), 0u);
	CHECK(strstr(g_log, "transport: 0 answers written\n") != NULL);
	rig_down(&g);
	omci_mib_fini(held);
	check_clean_after();

	struct omci_alarm *al = NULL;

	CHECK_EQ(omci_alarm_init(&al), OMCI_OK);
	CHECK(rig_up(&g));
	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_FAILURE);
	CHECK(strstr(g_log, "engine init failed: OMCI_ERR_BUSY") != NULL);
	CHECK(strstr(g_log, "hw:") == NULL);
	rig_down(&g);
	omci_alarm_fini(al);
	check_clean_after();

	struct omci_hw_ops bad = *omci_hw();

	bad.caps = caps_fail;
	omci_hw_ops_set(&bad);
	CHECK(rig_up(&g));
	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_FAILURE);
	CHECK(strstr(g_log,
		     "hw: cannot determine this device's capabilities: OMCI_ERR_HW") !=
	      NULL);
	CHECK(strstr(g_log, "mib ready") == NULL);
	rig_down(&g);
	omci_hw_ops_set(NULL);
	check_clean_after();

	bad = *omci_hw();
	bad.caps = caps_huge;
	omci_hw_ops_set(&bad);
	CHECK(rig_up(&g));
	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_FAILURE);
	CHECK(strstr(g_log, "mib population failed: OMCI_ERR_NOMEM") != NULL);
	CHECK(strstr(g_log, "mib ready") == NULL);
	rig_down(&g);
	omci_hw_ops_set(NULL);
	check_clean_after();
}

struct client {
	struct rig *rig;
	int fd;
	bool connected;
	uint8_t rsp[2][OMCI_CTL_RSP_LEN + OMCI_CTL_STATS_LEN];
	ssize_t got[2];
};

static void client_ask(struct client *c)
{
	uint8_t req[OMCI_CTL_REQ_LEN] = { 0 };

	omci_be32_store(&req[OMCI_CTL_REQ_MAGIC], OMCI_CTL_MAGIC);
	omci_be16_store(&req[OMCI_CTL_REQ_OP], OMCI_CTL_OP_STATS);
	if (c->connected)
		(void)send(c->fd, req, sizeof(req), 0);
}

static void client_step(void *arg)
{
	struct client *c = arg;

	switch (fault_seen(FAULT_POLL)) {
	case 0: {
		c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un addr = { .sun_family = AF_UNIX };

		memcpy(addr.sun_path, g_sock, strlen(g_sock) + 1u);
		c->connected = c->fd >= 0 &&
			       connect(c->fd, (const struct sockaddr *)&addr,
				       sizeof(addr)) == 0;
		break;
	}
	case 1:
		client_ask(c);
		break;
	case 2:
		if (c->connected)
			c->got[0] =
				recv(c->fd, c->rsp[0], sizeof(c->rsp[0]), 0);
		feed(c->rig, N_FRAMES);
		end_input(c->rig);
		break;
	case 3:
		client_ask(c);
		break;
	case 4:
		if (c->connected)
			c->got[1] =
				recv(c->fd, c->rsp[1], sizeof(c->rsp[1]), 0);
		if (c->fd >= 0)
			(void)close(c->fd);
		break;
	default:
		break;
	}
}

static void check_stats(const struct client *c, unsigned int which,
			unsigned int handled, unsigned int rejected,
			unsigned int answered)
{
	const uint8_t *r = c->rsp[which];

	CHECK_EQ(c->got[which], (ssize_t)sizeof(c->rsp[which]));
	CHECK_EQ(omci_be32(&r[OMCI_CTL_RSP_MAGIC]), OMCI_CTL_MAGIC);
	CHECK_EQ(omci_be16(&r[OMCI_CTL_RSP_STATUS]), OMCI_CTL_OK);
	CHECK_EQ(omci_be16(&r[OMCI_CTL_RSP_PAYLEN]), OMCI_CTL_STATS_LEN);
	CHECK_EQ(omci_be32(&r[OMCI_CTL_RSP_LEN + 0]), handled);
	CHECK_EQ(omci_be32(&r[OMCI_CTL_RSP_LEN + 4]), rejected);
	CHECK_EQ(omci_be32(&r[OMCI_CTL_RSP_LEN + 8]), answered);
}

static void test_control_socket_in_the_loop(void)
{
	struct rig g;

	CHECK(rig_up(&g));
	reference(N_FRAMES);

	struct client c = { .rig = &g, .fd = -1, .got = { -1, -1 } };

	fault_hook(FAULT_POLL, client_step, &c);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
	check_output(&g);

	CHECK(c.connected);

	check_stats(&c, 0, 0, 0,
		    (unsigned int)(g_ref_startup / OMCI_FRAME_LEN));

	check_stats(&c, 1, 1, 0,
		    (unsigned int)(g_ref_startup / OMCI_FRAME_LEN) +
			    g_ref_replies[0]);
	rig_down(&g);
	(void)unlink(g_sock);
}

struct tapper {
	struct rig *rig;
	int fd;
	bool connected;
};

static void tapper_step(void *arg)
{
	struct tapper *t = arg;

	switch (fault_seen(FAULT_POLL)) {
	case 0: {
		t->fd = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un addr = { .sun_family = AF_UNIX };

		memcpy(addr.sun_path, g_sock, strlen(g_sock) + 1u);
		t->connected = t->fd >= 0 &&
			       connect(t->fd, (const struct sockaddr *)&addr,
				       sizeof(addr)) == 0;
		break;
	}
	case 1: {
		uint8_t req[OMCI_CTL_REQ_LEN] = { 0 };

		omci_be32_store(&req[OMCI_CTL_REQ_MAGIC], OMCI_CTL_MAGIC);
		omci_be16_store(&req[OMCI_CTL_REQ_OP], OMCI_CTL_OP_TAP);
		if (t->connected)
			(void)send(t->fd, req, sizeof(req), 0);
		break;
	}
	case 2:
		feed(t->rig, N_FRAMES);
		end_input(t->rig);
		break;
	default:
		break;
	}
}

static void test_the_tap_is_fed_in_the_loop(void)
{
	struct rig g;

	CHECK(rig_up(&g));
	reference(N_FRAMES);

	struct tapper t = { .rig = &g, .fd = -1 };

	fault_hook(FAULT_POLL, tapper_step, &t);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
	check_output(&g);
	CHECK(t.connected);

	uint8_t all[16u * 1024u];
	size_t have = 0;

	for (;;) {
		const ssize_t n = recv(t.fd, all + have, sizeof(all) - have,
				       MSG_DONTWAIT);
		if (n <= 0)
			break;
		have += (size_t)n;
	}
	CHECK(have >= OMCI_CTL_RSP_LEN);
	CHECK_EQ(omci_be32(&all[OMCI_CTL_RSP_MAGIC]), OMCI_CTL_MAGIC);
	CHECK_EQ(omci_be16(&all[OMCI_CTL_RSP_STATUS]), OMCI_CTL_OK);
	CHECK_EQ(omci_be16(&all[OMCI_CTL_RSP_PAYLEN]), 0u);

	unsigned int dispatched = 0, sent = 0, first_request_seen = 0;
	size_t at = OMCI_CTL_RSP_LEN;
	const size_t record = OMCI_CTL_RSP_LEN + OMCI_CTL_TAP_REC_LEN;

	while (at + record <= have) {
		const uint8_t *r = &all[at + OMCI_CTL_RSP_LEN];

		CHECK_EQ(omci_be16(&all[at + OMCI_CTL_RSP_PAYLEN]),
			 OMCI_CTL_TAP_REC_LEN);
		if (r[OMCI_CTL_TAP_VERDICT] == OMCI_CTL_TAP_RX_DISPATCH) {
			dispatched++;
			if (memcmp(&r[OMCI_CTL_TAP_FRAME], g_frames[0],
				   OMCI_FRAME_LEN) == 0)
				first_request_seen++;
		}
		sent += r[OMCI_CTL_TAP_VERDICT] == OMCI_CTL_TAP_TX ? 1u : 0u;
		at += record;
	}
	CHECK_EQ(at, have);
	CHECK(dispatched > 0u);
	CHECK_EQ(first_request_seen, 1u);
	CHECK(sent > 0u);

	if (t.fd >= 0)
		(void)close(t.fd);
	rig_down(&g);
	(void)unlink(g_sock);
}

static void test_clock(void)
{
	fault_reset();
	const uint64_t a = omcid_monotonic_seconds();
	const uint64_t b = omcid_monotonic_seconds();

	CHECK(a > 0u);
	CHECK(b >= a);
	CHECK(b - a < 5u);

	fault_fail(FAULT_CLOCK_GETTIME, 1, EINVAL);
	CHECK_EQ(omcid_monotonic_seconds(), 0u);
	CHECK_EQ(fault_fired(FAULT_CLOCK_GETTIME), 1u);
	CHECK(omcid_monotonic_seconds() > 0u);

	struct rig g;

	/* The loop reads the clock before each frame, and goes on without it. */
	CHECK(rig_up(&g));
	reference(N_FRAMES);
	fault_fail_from(FAULT_CLOCK_GETTIME, 1, EINVAL);
	feed(&g, N_FRAMES);
	end_input(&g);
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv), EXIT_SUCCESS);
	CHECK(fault_fired(FAULT_CLOCK_GETTIME) > N_FRAMES);
	check_output(&g);
	rig_down(&g);
	(void)unlink(g_sock);

	/* A clock held with -t is never read. */
	CHECK(rig_up(&g));
	fault_fail_from(FAULT_CLOCK_GETTIME, 1, EINVAL);
	CHECK_EQ(run_frames(&g, N_FRAMES, g_sock), EXIT_SUCCESS);
	CHECK_EQ(fault_fired(FAULT_CLOCK_GETTIME), 0u);
	check_output(&g);
	rig_down(&g);
	(void)unlink(g_sock);
}

/* The file the daemon keeps its MIB in. */
static char g_keep[160];

/* A Set of the MIB data sync, which the OLT reads back with a Get. */
static void mds_set(uint8_t out[OMCI_FRAME_LEN], uint8_t value)
{
	struct omci_frame f = {
		.tci = 0x51,
		.msg_type = 8,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = 2,
		.instance = 0,
	};
	f.contents[0] = 0x80;
	f.contents[2] = value;
	(void)!omci_frame_build(&f, out);
}

static char *g_argv_kept[] = {
	(char *)"omcid", (char *)"-p", (char *)"-t", (char *)"1",
	(char *)"-m",	 NULL,	       NULL
};
static char g_long_path[300];
static char *g_argv_bare[] = { (char *)"omcid", (char *)"-m", NULL };
static char *g_argv_long[] = { (char *)"omcid", (char *)"-m", g_long_path,
			       NULL };

/* Run the daemon on @frame, or on no frame, keeping its MIB in @path. */
static int run_kept(struct rig *g, const uint8_t *frame, const char *path)
{
	g_argv_kept[5] = (char *)path;
	if (frame != NULL)
		CHECK_EQ(write(g->in_w, frame, OMCI_FRAME_LEN),
			 (ssize_t)OMCI_FRAME_LEN);
	end_input(g);
	return run(g, g->in_r, g_sock, 6, g_argv_kept);
}

static void test_the_mib_is_kept_across_a_start_again(void)
{
	uint8_t frame[OMCI_FRAME_LEN];
	struct stat st;
	struct rig g;

	(void)unlink(g_keep);
	CHECK(rig_up(&g));
	mds_set(frame, 42);
	CHECK_EQ(run_kept(&g, frame, g_keep), EXIT_SUCCESS);
	CHECK(strstr(g_log, "mib: nothing kept in") != NULL);
	CHECK(strstr(g_log, "mib ready: 38 instances") != NULL);
	CHECK_EQ(stat(g_keep, &st), 0);
	CHECK(st.st_size > 36);
	rig_down(&g);
	(void)unlink(g_sock);

	/* Started again, the daemon answers as the OLT set it up. */
	CHECK(rig_up(&g));
	get_frame(frame, 0x52, 2, 0, 0x8000);
	CHECK_EQ(run_kept(&g, frame, g_keep), EXIT_SUCCESS);
	CHECK(strstr(g_log, "mib taken up again from") != NULL);
	CHECK(strstr(g_log, "mib ready") == NULL);

	const size_t n = collect(&g, g_out, sizeof(g_out));
	bool answered = false;

	for (size_t at = 0; at + OMCI_FRAME_LEN <= n; at += OMCI_FRAME_LEN)
		if (g_out[at + 1] == 0x52 && g_out[at + 2] == 0x29)
			answered = g_out[at + 11] == 42;
	CHECK(answered);
	CHECK(strstr(g_log, "[WARN]") == NULL);
	rig_down(&g);
	(void)unlink(g_sock);
	(void)unlink(g_keep);
}

static void test_a_kept_file_it_cannot_go_on_from(void)
{
	char path[sizeof(g_keep) + 16];
	char other[sizeof(g_keep) + 16];
	struct stat st;
	struct rig g;

	/* Bytes of another kind: the daemon starts afresh, and keeps anew. */
	FILE *f = fopen(g_keep, "w");

	CHECK(f != NULL);
	if (f != NULL) {
		fputs("not a MIB\n", f);
		fclose(f);
	}
	CHECK(rig_up(&g));
	CHECK_EQ(run_kept(&g, NULL, g_keep), EXIT_SUCCESS);
	CHECK(strstr(g_log, "is not one this daemon can go on from") != NULL);
	CHECK(strstr(g_log, "mib ready: 38 instances") != NULL);
	rig_down(&g);
	(void)unlink(g_sock);
	CHECK(rig_up(&g));
	CHECK_EQ(run_kept(&g, NULL, g_keep), EXIT_SUCCESS);
	CHECK(strstr(g_log, "mib taken up again from") != NULL);
	rig_down(&g);
	(void)unlink(g_sock);

	/* A directory cannot be read, nor replaced. */
	CHECK(rig_up(&g));
	CHECK_EQ(run_kept(&g, NULL, g_dir), EXIT_SUCCESS);
	CHECK(strstr(g_log, "mib: cannot read") != NULL);
	CHECK(strstr(g_log, "mib: cannot keep it in") != NULL);
	(void)snprintf(path, sizeof(path), "%s.new", g_dir);
	CHECK(stat(path, &st) != 0);
	rig_down(&g);
	(void)unlink(g_sock);

	/* Nor can a file under a file. */
	(void)snprintf(path, sizeof(path), "%s/x", g_keep);
	CHECK(rig_up(&g));
	CHECK_EQ(run_kept(&g, NULL, path), EXIT_SUCCESS);
	CHECK(strstr(g_log, "mib: cannot read") != NULL);
	CHECK(strstr(g_log, "x.new") != NULL);
	rig_down(&g);
	(void)unlink(g_sock);

	/* A device that is full: the new file goes, the old name is untouched. */
	(void)snprintf(path, sizeof(path), "%s/full", g_dir);
	(void)snprintf(other, sizeof(other), "%s/full.new", g_dir);
	CHECK_EQ(symlink("/dev/full", other), 0);
	CHECK(rig_up(&g));
	CHECK_EQ(run_kept(&g, NULL, path), EXIT_SUCCESS);
	CHECK(strstr(g_log, "No space left on device") != NULL);
	CHECK(lstat(other, &st) != 0);
	CHECK(stat(path, &st) != 0);
	rig_down(&g);
	(void)unlink(g_sock);
	(void)unlink(g_keep);

	/* -m wants a path, and one short enough. */
	memset(g_long_path, 'a', sizeof(g_long_path) - 1u);
	CHECK(rig_up(&g));
	CHECK_EQ(run(&g, g.in_r, g_sock, 2, g_argv_bare), EXIT_FAILURE);
	CHECK_EQ(run(&g, g.in_r, g_sock, 3, g_argv_long), EXIT_FAILURE);
	CHECK(!omcid_keep(g_long_path));
	CHECK(omcid_keep(NULL));
	rig_down(&g);
}

/* A bus that counts what the loop asks of it, its descriptor a pipe. */
static unsigned int g_bus_opens, g_bus_handles, g_bus_closes;
static const struct omci_mib *g_bus_mib;
static const struct omci_history *g_bus_history;
static struct omci_mcast *g_bus_mcast;
static int g_bus_pipe[2] = { -1, -1 };

static void fake_open(const struct omci_mib *mib,
		      const struct omci_ctl_counters *counters,
		      const struct omci_history *history,
		      struct omci_mcast *mcast)
{
	(void)counters;
	g_bus_opens++;
	g_bus_mib = mib;
	g_bus_history = history;
	g_bus_mcast = mcast;
}

static int fake_fd(void)
{
	return g_bus_pipe[0];
}

static void fake_handle(void)
{
	char c;

	(void)!read(g_bus_pipe[0], &c, 1);
	g_bus_handles++;
}

static void fake_close(void)
{
	g_bus_closes++;
}

static const struct omcid_bus g_fake_bus = {
	.open = fake_open,
	.fd = fake_fd,
	.handle = fake_handle,
	.close = fake_close,
};

static void test_the_bus_is_served_in_the_loop(void)
{
	uint8_t frame[OMCI_FRAME_LEN];
	struct rig g;

	CHECK(rig_up(&g));
	CHECK_EQ(pipe(g_bus_pipe), 0);
	CHECK_EQ(write(g_bus_pipe[1], "x", 1), 1);
	omcid_bus_set(&g_fake_bus);

	/* The loop serves the bus between the frames, and closes it at the end. */
	mds_set(frame, 5);
	CHECK_EQ(write(g.in_w, frame, OMCI_FRAME_LEN), (ssize_t)OMCI_FRAME_LEN);
	get_frame(frame, 0x53, 2, 0, 0x8000);
	CHECK_EQ(write(g.in_w, frame, OMCI_FRAME_LEN), (ssize_t)OMCI_FRAME_LEN);
	end_input(&g);
	CHECK_EQ(run(&g, g.in_r, g_sock, 4, g_argv_held), EXIT_SUCCESS);
	CHECK_EQ(g_bus_opens, 1u);
	CHECK(g_bus_mib != NULL);
	CHECK(g_bus_mcast != NULL);
	CHECK_EQ(g_bus_handles, 1u);

	/*
	 * The bus is given the conversation: both frames received and both
	 * answers, in their order, stamped with the second -t holds the
	 * clock at.
	 */
	CHECK(g_bus_history != NULL);
	CHECK_EQ(g_bus_history->head, 4u);
	CHECK_EQ(g_bus_history->rec[0].verdict, OMCI_CTL_TAP_RX_DISPATCH);
	CHECK_EQ(g_bus_history->rec[1].verdict, OMCI_CTL_TAP_TX);
	CHECK_EQ(memcmp(g_bus_history->rec[2].frame, frame, OMCI_FRAME_LEN), 0);
	CHECK_EQ(g_bus_history->rec[3].sec, 1u);
	CHECK_EQ(g_bus_closes, 1u);

	omcid_bus_set(NULL);
	(void)close(g_bus_pipe[0]);
	(void)close(g_bus_pipe[1]);
	rig_down(&g);
	(void)unlink(g_sock);
}

int main(void)
{
	(void)signal(SIGPIPE, SIG_IGN);
	snprintf(g_dir, sizeof(g_dir), "/tmp/omcid-test-daemon-%ld",
		 (long)getpid());
	if (mkdir(g_dir, 0700) != 0) {
		printf("test_daemon: cannot make %s\n", g_dir);
		return 1;
	}
	snprintf(g_sock, sizeof(g_sock), "%s/ctl.sock", g_dir);
	snprintf(g_keep, sizeof(g_keep), "%s/mib", g_dir);
	make_frames();

	RUN(test_the_log_is_cut_when_full);
	RUN(test_end_of_input);
	RUN(test_signal_stops_the_loop);
	RUN(test_signals_change_the_mode);
	RUN(test_the_backend_asks_for_programming_again);
	RUN(test_the_multicast_descriptor_is_served);
	RUN(test_the_pon_descriptor_is_served);
	RUN(test_poll);
	RUN(test_transport_errors);
	RUN(test_setup_failures);
	RUN(test_control_socket_in_the_loop);
	RUN(test_the_tap_is_fed_in_the_loop);
	RUN(test_clock);
	RUN(test_the_mib_is_kept_across_a_start_again);
	RUN(test_a_kept_file_it_cannot_go_on_from);
	RUN(test_the_bus_is_served_in_the_loop);

	(void)unlink(g_sock);
	(void)rmdir(g_dir);
	return OMCI_T_REPORT("test_daemon");
}
