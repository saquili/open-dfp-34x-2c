// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The pipe transport of transport_pipe.h. */

#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include "sys/log.h"
#include "sys/transport_pipe.h"

/* Read @len bytes; an end of file between two frames is the end of the stream. */
static enum omci_status read_exactly(int fd, uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		const ssize_t n = read(fd, &buf[done], len - done);

		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n == 0)
			return done == 0 ? OMCI_DECLINED : OMCI_ERR_TRUNCATED;
		if (errno == EINTR) {
			if (done == 0)
				return OMCI_ERR_AGAIN;
			continue;
		}
		OMCI_ERR("transport: read failed (errno %d)", errno);
		return OMCI_ERR_IO;
	}
	return OMCI_OK;
}

static enum omci_status pipe_recv(void *ctx,
				  uint8_t frame[static OMCI_FRAME_LEN])
{
	struct omci_transport_pipe *p = ctx;

	if (p->broken)
		return OMCI_ERR_IO;

	return read_exactly(p->rx_fd, frame, OMCI_FRAME_LEN);
}

static void pipe_send(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN])
{
	struct omci_transport_pipe *p = ctx;
	size_t done = 0;

	while (done < OMCI_FRAME_LEN) {
		const ssize_t n =
			write(p->tx_fd, &frame[done], OMCI_FRAME_LEN - done);
		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		OMCI_ERR(
			"transport: write stopped after %zu of %u bytes (errno %d) — the answer stream is now misaligned and this daemon will stop rather than corrupt it further",
			done, OMCI_FRAME_LEN, errno);
		p->broken = true;
		return;
	}
	p->sent++;
}

static void pipe_close(void *ctx)
{
	const struct omci_transport_pipe *p = ctx;

	OMCI_INFO("transport: %zu answers written%s", p->sent,
		  p->broken ? " before the stream broke" : "");
}

static int pipe_rx_fd(void *ctx)
{
	const struct omci_transport_pipe *p = ctx;

	return p->rx_fd;
}

static const struct omci_transport_ops pipe_ops = {
	.recv = pipe_recv,
	.rx_fd = pipe_rx_fd,
	.send = pipe_send,
	.close = pipe_close,
};

void omci_transport_pipe_init(struct omci_transport_pipe *pipe, int rx_fd,
			      int tx_fd, struct omci_transport *out)
{
	pipe->rx_fd = rx_fd;
	pipe->tx_fd = tx_fd;
	pipe->sent = 0;
	pipe->broken = false;

	out->ops = &pipe_ops;
	out->ctx = pipe;
}
