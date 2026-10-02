// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The daemon on the board: frames through the OMCI port of the PON driver,
 * and the hardware through the kernel's interfaces.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "daemon.h"
#include "omcid/ctl_proto.h"
#include "sys/log.h"
#include "transport_port.h"

static void print_help(FILE *f, const char *prog)
{
	fprintf(f,
		"usage: %s [-d DEVICE] [-v]... [-h]\n"
		"Answer the OLT on the OMCI port of the ONU.\n"
		"\n"
		"  -d DEVICE  the OMCI port (default %s)\n"
		"  -v         log more; may be given more than once\n"
		"  -h         show this text\n",
		prog, OMCI_PORT_DEFAULT);
}

int main(int argc, char **argv)
{
	const char *device = OMCI_PORT_DEFAULT;
	int level = OMCI_LOG_INFO;
	int opt;

	while ((opt = getopt(argc, argv, "d:vh")) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'v':
			if (level < OMCI_LOG_TRACE)
				level++;
			break;
		case 'h':
			print_help(stdout, argv[0]);
			return EXIT_SUCCESS;
		default:
			print_help(stderr, argv[0]);
			return EXIT_FAILURE;
		}
	}
	omci_log_level_set((enum omci_log_level)level);

	struct omci_transport_port port;
	struct omci_transport transport;

	if (omci_transport_port_open(&port, device, &transport) != 0) {
		OMCI_ERR("%s: %s", device, strerror(errno));
		return EXIT_FAILURE;
	}
	OMCI_INFO("omcid: serving %s", device);
	return omcid_serve(&transport, OMCI_CTL_SOCKET_PATH);
}
