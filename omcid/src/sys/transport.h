/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* How frames reach the daemon and leave it. */

#ifndef OMCI_SYS_TRANSPORT_H
#define OMCI_SYS_TRANSPORT_H

#include <stdint.h>

#include "omci/frame.h"
#include "omcid/status.h"

/**
 * struct omci_transport_ops - A transport of 48-byte frames.
 * @recv: read the next frame; %OMCI_DECLINED at the end of the stream,
 *	%OMCI_ERR_AGAIN when interrupted before any byte came
 * @rx_fd: the descriptor to poll for incoming frames
 * @send: write a frame
 * @close: release the transport
 */
struct omci_transport_ops {
	enum omci_status (*recv)(void *ctx,
				 uint8_t frame[static OMCI_FRAME_LEN]);

	int (*rx_fd)(void *ctx);

	void (*send)(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN]);

	void (*close)(void *ctx);
};

/**
 * struct omci_transport - A transport and its state.
 * @ops: its operations
 * @ctx: passed to them
 */
struct omci_transport {
	const struct omci_transport_ops *ops;
	void *ctx;
};

#endif
