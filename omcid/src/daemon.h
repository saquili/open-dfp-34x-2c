/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The daemon: its main loop, shared by the programs that start it. */

#ifndef OMCID_DAEMON_H
#define OMCID_DAEMON_H

#include <stdbool.h>
#include <stdint.h>

/**
 * omcid_run() - Run the daemon until it is stopped or its input ends.
 * @argc: the number of arguments
 * @argv: the arguments
 * @rx_fd: where frames from the OLT come from
 * @tx_fd: where the answers go
 * @ctl_path: where to create the control socket
 *
 * Return: the exit status of the program.
 */
int omcid_run(int argc, char **argv, int rx_fd, int tx_fd,
	      const char *ctl_path);

/**
 * omcid_keep() - Keep the MIB in a file, and take it up again at the start.
 * @path: the file, or NULL to keep nothing
 *
 * The daemon replaces the file whole after each request of the OLT that may
 * change what it set up, and when it stops. A daemon that starts and finds
 * one it can go on from takes the MIB, the time of day and the performance
 * monitoring intervals from it and programs the data path again, and the
 * OLT sees no new ONU. A file it cannot go on from is left for the next
 * change to replace, and the daemon starts afresh. omcid_run() takes the
 * file as -m.
 *
 * Return: false, keeping nothing, for a path longer than the daemon takes.
 */
bool omcid_keep(const char *path);

struct omci_mib;
struct omci_ctl_counters;
struct omci_history;

/**
 * struct omcid_bus - Where the daemon publishes what it holds (ADR 0011):
 *	ubus on the board (target/bus_ubus.c).
 * @open: connect, and publish from then on @mib, @counters and the
 *	conversation with the OLT, @history; a bus that is not there yet is
 *	connected to later by @fd
 * @fd: the descriptor to wait on, or -1 while there is no connection
 * @handle: answer what waits on @fd
 * @close: go away
 */
struct omcid_bus {
	void (*open)(const struct omci_mib *mib,
		     const struct omci_ctl_counters *counters,
		     const struct omci_history *history);
	int (*fd)(void);
	void (*handle)(void);
	void (*close)(void);
};

/**
 * omcid_bus_set() - Publish on a bus from the next start of the loop.
 * @bus: the bus, or NULL for none
 */
void omcid_bus_set(const struct omcid_bus *bus);

struct omci_transport;

/**
 * omcid_serve() - Serve the OLT on a transport until stopped or the input ends.
 * @transport: where frames come from and answers go; it is closed on return
 * @ctl_path: where to create the control socket
 *
 * Return: the exit status of the program.
 */
int omcid_serve(struct omci_transport *transport, const char *ctl_path);

/**
 * omcid_monotonic_seconds() - The monotonic clock, in seconds.
 *
 * Return: the time, or 0 when the clock cannot be read.
 */
uint64_t omcid_monotonic_seconds(void);

#endif
