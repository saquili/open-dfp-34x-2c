/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The daemon: its main loop, shared by the programs that start it. */

#ifndef OMCID_DAEMON_H
#define OMCID_DAEMON_H

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
