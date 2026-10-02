/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The control socket: a local stream socket where tools read the MIB, the
 * counters of the engine and the fates of the VLAN rules, and follow the
 * frames as they come and go. The protocol is in omcid/ctl_proto.h.
 */

#ifndef OMCI_SYS_CTL_H
#define OMCI_SYS_CTL_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#include "omci/frame.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/* Clients at once, and the descriptors to poll for them and the listener. */
#define OMCI_CTL_MAX_CLIENTS 4u
#define OMCI_CTL_MAX_FDS (1u + OMCI_CTL_MAX_CLIENTS)

/* The frames kept for a client that follows them and falls behind. */
#define OMCI_CTL_TAP_RING 64u

struct omci_ctl;
struct omci_mib;

/**
 * struct omci_ctl_counters - The counters of the engine the socket reports.
 * @handled: frames received
 * @rejected: frames dropped as malformed
 * @answered: frames sent
 */
struct omci_ctl_counters {
	unsigned long handled;
	unsigned long rejected;
	unsigned long answered;
};

/**
 * omci_ctl_open() - Open the control socket; one per process.
 * @out: the socket
 * @path: where to create it; an old socket there is removed
 * @mib: the MIB it reads
 * @counters: the counters it reports; may be NULL
 *
 * Only the owner of the daemon may connect: the socket is created with
 * mode 0600.
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or %OMCI_ERR_IO.
 */
__must_check enum omci_status
omci_ctl_open(struct omci_ctl **out, const char *path,
	      const struct omci_mib *mib,
	      const struct omci_ctl_counters *counters);

/**
 * omci_ctl_pollfds() - The descriptors the main loop polls for the socket.
 * @ctl: the socket
 * @out: the descriptors
 * @cap: room in @out
 *
 * Return: their number.
 */
size_t omci_ctl_pollfds(struct omci_ctl *ctl, struct pollfd *out, size_t cap);

/**
 * omci_ctl_service() - Serve what the poll reported.
 * @ctl: the socket
 * @fds: the descriptors of omci_ctl_pollfds(), with their events
 * @n: their number
 */
void omci_ctl_service(struct omci_ctl *ctl, const struct pollfd *fds, size_t n);

/**
 * omci_ctl_tap_publish() - Pass a frame to the clients that follow the frames.
 * @ctl: the socket
 * @verdict: an &enum omci_ctl_tap_verdict
 * @detail: the message type
 * @frame: the frame
 */
void omci_ctl_tap_publish(struct omci_ctl *ctl, uint8_t verdict, uint8_t detail,
			  const uint8_t frame[static OMCI_FRAME_LEN]);

/**
 * omci_ctl_close() - Close the socket and its clients.
 * @ctl: the socket, or NULL
 */
void omci_ctl_close(struct omci_ctl *ctl);

#endif
