/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The transport of the board: the OMCI port of the PON driver, a character
 * device that carries one OMCI message per read and per write.
 */

#ifndef OMCID_TRANSPORT_PORT_H
#define OMCID_TRANSPORT_PORT_H

#include "sys/transport.h"

/* The OMCI port of the first PON device. */
#define OMCI_PORT_DEFAULT "/dev/pon0omci0"

/**
 * struct omci_transport_port - The state of the OMCI port transport.
 * @fd: the open port
 * @rx: baseline frames read
 * @ignored: messages read that are not baseline frames
 * @tx: frames written
 * @tx_dropped: frames the port could not send because the link was down
 */
struct omci_transport_port {
	int fd;
	unsigned long rx;
	unsigned long ignored;
	unsigned long tx;
	unsigned long tx_dropped;
};

/**
 * omci_transport_port_open() - Open the OMCI port as a transport.
 * @port: its state
 * @path: the character device
 * @out: the transport
 *
 * Return: 0, or -1 with errno set.
 */
int omci_transport_port_open(struct omci_transport_port *port, const char *path,
			     struct omci_transport *out);

#endif
