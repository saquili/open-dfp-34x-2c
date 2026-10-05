// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The main loop: wait for a frame or a client of the control socket, at
 * most a second at a time so that the performance monitoring intervals
 * advance, and hand each frame to the engine.
 */

#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "daemon.h"
#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/ctl_proto.h"
#include "omcid/status.h"
#include "sys/ctl.h"
#include "sys/log.h"
#include "sys/transport_pipe.h"

#ifndef OMCID_VERSION
#define OMCID_VERSION "0.1.0"
#endif

#ifdef OMCID_HW_STUB
#define OMCID_PROFILE "host (hardware stubbed)"
#else
#define OMCID_PROFILE "target"
#endif

static volatile sig_atomic_t g_stop;

/* A change of mode that a signal asked for, not yet made. */
enum { MODE_KEPT, MODE_ROUTER, MODE_BRIDGE };

static volatile sig_atomic_t g_mode;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void on_mode(int sig)
{
	g_mode = sig == SIGUSR1 ? MODE_ROUTER : MODE_BRIDGE;
}

/*
 * SIGUSR1 puts the backend in router mode, SIGUSR2 back in bridge mode. The
 * MIB stays as the OLT made it, and the data path is programmed again from
 * it: in router mode the bridges and the VLAN rules stay out of the
 * hardware, in bridge mode they are applied again.
 */
static void change_mode(struct omci_engine *eng)
{
	const bool router = g_mode == MODE_ROUTER;

	g_mode = MODE_KEPT;
	OMCI_INFO("mode: %s", router ? "router" : "bridge");
	if (omci_hw()->set_router != NULL)
		omci_hw()->set_router(router);
	omci_engine_reprogram(eng);
}

static void print_usage(const char *argv0)
{
	fprintf(stderr,
		"omcid " OMCID_VERSION " - the OMCI daemon of the ONU\n"
		"\n"
		"usage: %s [-v] [-p] [-t N] [-h]\n"
		"  -v  log more; may be given more than once\n"
		"  -p  frames come on the standard input and answers go to the\n"
		"      standard output (the default of this build)\n"
		"  -t  hold the clock at second N, so that the same frames give\n"
		"      the same answers however fast they come\n"
		"  -h  show this text\n"
		"\n"
		"Log lines go to the standard error, so recorded frames can be\n"
		"played through the daemon:\n"
		"  omcid -p < frames.bin > answers.bin\n",
		argv0);
}

/* The engine sends through the transport, and the counters see each frame. */
struct tx_bridge {
	const struct omci_transport *transport;
	struct omci_ctl_counters *counters;
};

static void on_send(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN])
{
	struct tx_bridge *b = ctx;

	b->counters->answered++;
	b->transport->ops->send(b->transport->ctx, frame);
}

static void on_tap(void *ctx, uint8_t verdict, uint8_t detail,
		   const uint8_t frame[static OMCI_FRAME_LEN])
{
	omci_ctl_tap_publish(ctx, verdict, detail, frame);
}

uint64_t omcid_monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec;
}

/*
 * The time the engine is given: the monotonic clock, or the second that -t
 * holds it at. A replay reads the clock between its frames, and a second
 * that passes there has the alarms judged again, the counters sampled and
 * the timers advanced; a held clock leaves the answers to the frames alone.
 */
static bool g_clock_held;
static uint64_t g_clock_at;

static uint64_t clock_now(void)
{
	return g_clock_held ? g_clock_at : omcid_monotonic_seconds();
}

/* A number of seconds in decimal digits, and nothing else. */
static bool parse_seconds(const char *s, uint64_t *out)
{
	char *end;

	if (s[0] < '0' || s[0] > '9')
		return false;
	errno = 0;
	const unsigned long long v = strtoull(s, &end, 10);

	if (errno != 0 || *end != '\0')
		return false;
	*out = (uint64_t)v;
	return true;
}

int omcid_run(int argc, char **argv, int rx_fd, int tx_fd, const char *ctl_path)
{
	int verbosity = OMCI_LOG_INFO;

	g_stop = 0;
	g_clock_held = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		}
		if (strcmp(argv[i], "-p") == 0)
			continue;
		if (strcmp(argv[i], "-t") == 0) {
			if (i + 1 == argc ||
			    !parse_seconds(argv[i + 1], &g_clock_at)) {
				fprintf(stderr,
					"omcid: -t takes a number of seconds\n");
				print_usage(argv[0]);
				return EXIT_FAILURE;
			}
			g_clock_held = true;
			i++;
			continue;
		}
		if (strcmp(argv[i], "-v") == 0) {
			if (verbosity < OMCI_LOG_TRACE)
				verbosity++;
			continue;
		}
		fprintf(stderr, "omcid: unknown argument '%s'\n", argv[i]);
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	omci_log_level_set((enum omci_log_level)verbosity);

	OMCI_INFO("omcid %s starting — %s", OMCID_VERSION, OMCID_PROFILE);

	struct omci_transport_pipe pipe;
	struct omci_transport transport;

	omci_transport_pipe_init(&pipe, rx_fd, tx_fd, &transport);
	OMCI_INFO("transport: stdin/stdout");
	return omcid_serve(&transport, ctl_path);
}

int omcid_serve(struct omci_transport *transport, const char *ctl_path)
{
	g_stop = 0;
	g_mode = MODE_KEPT;

	/*
	 * SIGINT and SIGTERM stop the loop, SIGUSR1 and SIGUSR2 change the mode;
	 * a client that goes away raises no SIGPIPE.
	 */
	struct sigaction sa = { .sa_handler = on_signal };

	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGTERM, &sa, NULL);

	struct sigaction mode = { .sa_handler = on_mode };

	sigemptyset(&mode.sa_mask);
	(void)sigaction(SIGUSR1, &mode, NULL);
	(void)sigaction(SIGUSR2, &mode, NULL);

	struct sigaction ign = { .sa_handler = SIG_IGN };

	sigemptyset(&ign.sa_mask);
	(void)sigaction(SIGPIPE, &ign, NULL);

	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;
	struct omci_ctl *ctl = NULL;

	int status = EXIT_FAILURE;

	struct omci_ctl_counters counters = { 0, 0, 0 };
	struct tx_bridge bridge = { .transport = transport,
				    .counters = &counters };

	enum omci_status rc = omci_mib_init(&mib);

	if (rc != OMCI_OK) {
		OMCI_ERR("mib init failed: %s", omci_status_str(rc));
		goto out;
	}

	static const struct omci_tx_ops tx_ops = { .send = on_send };

	rc = omci_engine_init(&eng, mib, &tx_ops, &bridge);
	if (rc != OMCI_OK) {
		OMCI_ERR("engine init failed: %s", omci_status_str(rc));
		goto out_mib;
	}

	struct omci_hw_caps caps;

	rc = omci_hw()->caps(&caps);
	if (rc != OMCI_OK) {
		OMCI_ERR("hw: cannot determine this device's capabilities: %s",
			 omci_status_str(rc));
		goto out_engine;
	}
	OMCI_INFO("hw: %s", omci_hw()->source());

	/* The MIB starts with the ONU's own instances. */
	rc = omci_me_defaults_apply(mib, &caps);
	if (rc != OMCI_OK) {
		OMCI_ERR("mib population failed: %s", omci_status_str(rc));
		goto out_engine;
	}
	OMCI_INFO("mib ready: %zu instances", omci_mib_count(mib));

	/* The control socket is a convenience: the OLT is served without it. */
	if (omci_ctl_open(&ctl, ctl_path, mib, &counters) != OMCI_OK)
		OMCI_WARN(
			"ctl: running without a control socket — the OLT is still served");

	static const struct omci_tap_ops tap_ops = { .frame = on_tap };

	if (ctl != NULL)
		omci_engine_tap_set(eng, &tap_ops, ctl);

	size_t handled = 0;
	size_t rejected = 0;

	const int frame_fd = transport->ops->rx_fd != NULL ?
				     transport->ops->rx_fd(transport->ctx) :
				     -1;

	while (!g_stop) {
		struct pollfd fds[2 + OMCI_CTL_MAX_FDS];
		nfds_t n = 0;

		if (g_mode != MODE_KEPT)
			change_mode(eng);

		if (frame_fd >= 0) {
			fds[n].fd = frame_fd;
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
		}

		/* The multicast agent's descriptor, when the backend has one. */
		const int mcast_fd = omci_engine_mcast_fd(eng);
		const nfds_t mcast_at = n;

		if (mcast_fd >= 0) {
			fds[n].fd = mcast_fd;
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
		}
		const nfds_t ctl_first = n;

		n += omci_ctl_pollfds(ctl, &fds[n], OMCI_CTL_MAX_FDS);

		if (n == 0) {
			OMCI_ERR("no descriptor left to wait on — stopping");
			break;
		}

		const int rc_poll = poll(fds, n, 1000);

		if (rc_poll < 0) {
			if (errno == EINTR)
				continue;
			OMCI_ERR("poll failed (errno %d)", errno);
			goto out_engine;
		}

		omci_engine_tick(eng, clock_now());

		if (rc_poll == 0)
			continue;

		if (mcast_fd >= 0 && (fds[mcast_at].revents & POLLIN))
			omci_engine_mcast_service(eng);

		omci_ctl_service(ctl, &fds[ctl_first], n - ctl_first);

		if (frame_fd < 0 ||
		    !(fds[0].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;

		uint8_t frame[OMCI_FRAME_LEN];

		rc = transport->ops->recv(transport->ctx, frame);
		if (rc == OMCI_ERR_AGAIN)
			continue;
		/* The input has ended. */
		if (rc == OMCI_DECLINED)
			break;
		if (rc != OMCI_OK) {
			OMCI_ERR("transport: %s", omci_status_str(rc));
			goto out_engine;
		}

		rc = omci_engine_rx(eng, frame);
		if (rc == OMCI_OK)
			handled++;
		else
			rejected++;
		counters.handled = handled;
		counters.rejected = rejected;
	}

	OMCI_INFO("stopping: %zu frames handled, %zu rejected", handled,
		  rejected);
	status = EXIT_SUCCESS;

out_engine:
	omci_ctl_close(ctl);
	omci_engine_fini(eng);
out_mib:
	omci_mib_fini(mib);
out:
	transport->ops->close(transport->ctx);
	return status;
}
