// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The main loop: wait for a frame or a client of the control socket, at
 * most a second at a time so that the performance monitoring intervals
 * advance, and hand each frame to the engine.
 */

#include <poll.h>
#include <errno.h>
#include <fcntl.h>
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
#include "sys/history.h"
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
	if (omci_hw()->mode_done != NULL)
		omci_hw()->mode_done(router);
}

/* Where the daemon publishes what it holds, or NULL. */
static const struct omcid_bus *g_bus;

void omcid_bus_set(const struct omcid_bus *bus)
{
	g_bus = bus;
}

/*
 * The file the MIB is kept in, or NULL, and room for what is kept and the
 * name of the new file that replaces it.
 */
static const char *g_keep;
static uint8_t g_kept[OMCI_ENGINE_SAVE_MAX + 1u];
static char g_keep_new[256];

bool omcid_keep(const char *path)
{
	if (path != NULL &&
	    strlen(path) + sizeof(".new") > sizeof(g_keep_new)) {
		g_keep = NULL;
		return false;
	}
	g_keep = path;
	if (path != NULL)
		(void)snprintf(g_keep_new, sizeof(g_keep_new), "%s.new", path);
	return true;
}

/*
 * Take up the MIB the daemon kept. Return: true when it did, false when
 * nothing was kept or what was cannot be gone on from.
 */
static bool take_up(struct omci_engine *eng, const struct omci_mib *mib)
{
	if (g_keep == NULL)
		return false;

	const int fd = open(g_keep, O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		if (errno == ENOENT)
			OMCI_INFO("mib: nothing kept in %s yet", g_keep);
		else
			OMCI_WARN("mib: cannot read %s: %s", g_keep,
				  strerror(errno));
		return false;
	}

	size_t len = 0;
	ssize_t n;

	do {
		n = read(fd, g_kept + len, sizeof(g_kept) - len);
		if (n > 0)
			len += (size_t)n;
	} while ((n > 0 && len < sizeof(g_kept)) || (n < 0 && errno == EINTR));

	const int err = errno;

	(void)close(fd);
	if (n < 0) {
		OMCI_WARN("mib: cannot read %s: %s", g_keep, strerror(err));
		return false;
	}

	const enum omci_status rc = omci_engine_load(eng, g_kept, len);

	if (rc != OMCI_OK) {
		OMCI_WARN("mib: %s is not one this daemon can go on from (%s)",
			  g_keep, omci_status_str(rc));
		return false;
	}
	OMCI_INFO("mib taken up again from %s: %zu instances", g_keep,
		  omci_mib_count(mib));
	return true;
}

/* Keep the MIB: a new file, written whole, takes the old one's name. */
static void keep_mib(const struct omci_engine *eng)
{
	size_t len = 0;

	if (g_keep == NULL)
		return;
	(void)!omci_engine_save(eng, g_kept, sizeof(g_kept), &len);

	const int fd = open(g_keep_new,
			    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0) {
		OMCI_WARN("mib: cannot keep it in %s: %s", g_keep_new,
			  strerror(errno));
		return;
	}

	bool whole = true;

	for (size_t at = 0; at < len && whole;) {
		const ssize_t n = write(fd, g_kept + at, len - at);

		if (n > 0)
			at += (size_t)n;
		else
			whole = n < 0 && errno == EINTR;
	}

	const int err = errno;

	(void)close(fd);
	if (!whole || rename(g_keep_new, g_keep) != 0) {
		OMCI_WARN("mib: cannot keep it in %s: %s", g_keep,
			  strerror(whole ? errno : err));
		(void)unlink(g_keep_new);
		return;
	}
	OMCI_DEBUG("mib kept in %s: %zu bytes", g_keep, len);
}

static void print_usage(const char *argv0)
{
	fprintf(stderr,
		"omcid " OMCID_VERSION " - the OMCI daemon of the ONU\n"
		"\n"
		"usage: %s [-v] [-p] [-t N] [-m FILE] [-h]\n"
		"  -v  log more; may be given more than once\n"
		"  -p  frames come on the standard input and answers go to the\n"
		"      standard output (the default of this build)\n"
		"  -t  hold the clock at second N, so that the same frames give\n"
		"      the same answers however fast they come\n"
		"  -m  keep the MIB in FILE, and take it up again at the start\n"
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

/*
 * The conversation with the OLT, kept for the bus, and the control socket's
 * followers, which may be none. A frame is stamped with the wall clock, or
 * with the second -t holds the clock at, which reads no clock.
 */
static struct omci_history g_history;

static void on_tap(void *ctx, uint8_t verdict, uint8_t detail,
		   const uint8_t frame[static OMCI_FRAME_LEN])
{
	struct timespec ts = { 0, 0 };

	if (g_clock_held)
		ts.tv_sec = (time_t)g_clock_at;
	else
		(void)clock_gettime(CLOCK_REALTIME, &ts);
	omci_history_add(&g_history, (uint32_t)ts.tv_sec,
			 (uint16_t)(ts.tv_nsec / 1000000), verdict, detail,
			 frame);
	omci_ctl_tap_publish(ctx, verdict, detail, frame);
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
	(void)omcid_keep(NULL);

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
		if (strcmp(argv[i], "-m") == 0) {
			if (i + 1 == argc || !omcid_keep(argv[i + 1])) {
				fprintf(stderr,
					"omcid: -m takes the path of a file, of at most %zu characters\n",
					sizeof(g_keep_new) - sizeof(".new"));
				print_usage(argv[0]);
				return EXIT_FAILURE;
			}
			i++;
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

	/*
	 * The MIB starts as the daemon kept it, which the OLT set up, or with
	 * the ONU's own instances.
	 */
	if (!take_up(eng, mib)) {
		rc = omci_me_defaults_apply(mib, &caps);
		if (rc != OMCI_OK) {
			OMCI_ERR("mib population failed: %s",
				 omci_status_str(rc));
			goto out_engine;
		}
		OMCI_INFO("mib ready: %zu instances", omci_mib_count(mib));
	}

	uint32_t kept = omci_engine_changes(eng);

	/* The control socket is a convenience: the OLT is served without it. */
	if (omci_ctl_open(&ctl, ctl_path, mib, &counters) != OMCI_OK)
		OMCI_WARN(
			"ctl: running without a control socket — the OLT is still served");

	static const struct omci_tap_ops tap_ops = { .frame = on_tap };

	g_history.head = 0;
	omci_engine_tap_set(eng, &tap_ops, ctl);
	if (g_bus != NULL)
		g_bus->open(mib, &counters, &g_history);

	size_t handled = 0;
	size_t rejected = 0;

	const int frame_fd = transport->ops->rx_fd != NULL ?
				     transport->ops->rx_fd(transport->ctx) :
				     -1;

	while (!g_stop) {
		struct pollfd fds[3 + OMCI_CTL_MAX_FDS];
		nfds_t n = 0;

		if (g_mode != MODE_KEPT)
			change_mode(eng);
		if (omci_hw()->reprogram_due != NULL &&
		    omci_hw()->reprogram_due()) {
			OMCI_INFO(
				"the data path is programmed again from the MIB");
			omci_engine_reprogram(eng);
		}

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
		/* The bus's, while it is connected. */
		const int bus_fd = g_bus != NULL ? g_bus->fd() : -1;
		const nfds_t bus_at = n;

		if (bus_fd >= 0) {
			fds[n].fd = bus_fd;
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
		if (bus_fd >= 0 && (fds[bus_at].revents & (POLLIN | POLLHUP)))
			g_bus->handle();

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
		if (omci_engine_changes(eng) != kept) {
			kept = omci_engine_changes(eng);
			keep_mib(eng);
		}
	}

	OMCI_INFO("stopping: %zu frames handled, %zu rejected", handled,
		  rejected);
	keep_mib(eng);
	status = EXIT_SUCCESS;

out_engine:
	if (g_bus != NULL)
		g_bus->close();
	omci_ctl_close(ctl);
	omci_engine_fini(eng);
out_mib:
	omci_mib_fini(mib);
out:
	transport->ops->close(transport->ctx);
	return status;
}
