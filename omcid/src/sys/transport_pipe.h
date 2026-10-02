/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* A transport over a pair of descriptors, for tests and replays. */

#ifndef OMCI_SYS_TRANSPORT_PIPE_H
#define OMCI_SYS_TRANSPORT_PIPE_H

#include <stdbool.h>

#include "sys/transport.h"

/**
 * struct omci_transport_pipe - The state of a pipe transport.
 * @rx_fd: where frames come from
 * @tx_fd: where answers go
 * @sent: the frames written
 * @broken: set when a frame could be written only in part; the stream of
 *	answers is out of step from then on, so nothing more is read
 */
struct omci_transport_pipe {
	int rx_fd;
	int tx_fd;
	size_t sent;
	bool broken;
};

/**
 * omci_transport_pipe_init() - Make a transport over two descriptors.
 * @pipe: its state
 * @rx_fd: where frames come from
 * @tx_fd: where answers go
 * @out: the transport
 */
void omci_transport_pipe_init(struct omci_transport_pipe *pipe, int rx_fd,
			      int tx_fd, struct omci_transport *out);

#endif
