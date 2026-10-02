// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The OMCI port transport of transport_port.h. The port is non-blocking: a
 * read with nothing waiting is not an error, and a write while the link is
 * down is dropped, since the OLT sends its request again.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "sys/log.h"
#include "transport_port.h"

/* The longest OMCI message, an extended one (G.988 11.2.9). */
#define OMCI_PORT_MSG_MAX 1980u

static enum omci_status port_recv(void *ctx,
				  uint8_t frame[static OMCI_FRAME_LEN])
{
	struct omci_transport_port *p = ctx;
	uint8_t msg[OMCI_PORT_MSG_MAX];

	for (;;) {
		const ssize_t n = read(p->fd, msg, sizeof(msg));

		if (n == (ssize_t)OMCI_FRAME_LEN) {
			memcpy(frame, msg, OMCI_FRAME_LEN);
			p->rx++;
			return OMCI_OK;
		}
		if (n > 0) {
			p->ignored++;
			OMCI_DEBUG(
				"port: a message of %zd bytes is not a baseline frame; ignored",
				n);
			continue;
		}
		if (n == 0 || errno == EAGAIN || errno == EINTR)
			return OMCI_ERR_AGAIN;
		OMCI_ERR("port: read failed: %s", strerror(errno));
		return OMCI_ERR_IO;
	}
}

static int port_rx_fd(void *ctx)
{
	const struct omci_transport_port *p = ctx;

	return p->fd;
}

static void port_send(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN])
{
	struct omci_transport_port *p = ctx;
	ssize_t n;

	do
		n = write(p->fd, frame, OMCI_FRAME_LEN);
	while (n < 0 && errno == EINTR);

	if (n == (ssize_t)OMCI_FRAME_LEN) {
		p->tx++;
		return;
	}
	if (n < 0 &&
	    (errno == ENOLINK || errno == ENETDOWN || errno == EAGAIN)) {
		p->tx_dropped++;
		return;
	}
	OMCI_ERR("port: write failed: %s",
		 n < 0 ? strerror(errno) : "short write");
}

static void port_close(void *ctx)
{
	struct omci_transport_port *p = ctx;

	OMCI_INFO("port: %lu frames in, %lu ignored, %lu out, %lu dropped",
		  p->rx, p->ignored, p->tx, p->tx_dropped);
	if (p->fd >= 0)
		(void)close(p->fd);
	p->fd = -1;
}

static const struct omci_transport_ops port_ops = {
	.recv = port_recv,
	.rx_fd = port_rx_fd,
	.send = port_send,
	.close = port_close,
};

int omci_transport_port_open(struct omci_transport_port *port, const char *path,
			     struct omci_transport *out)
{
	*port = (struct omci_transport_port){ .fd = -1 };

	port->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (port->fd < 0)
		return -1;

	out->ops = &port_ops;
	out->ctx = port;
	return 0;
}
