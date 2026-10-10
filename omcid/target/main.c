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
#ifdef OMCID_UBUS
#include "bus_ubus.h"
#endif
#include "hw_kernel.h"
#include "omcid/ctl_proto.h"
#include "sys/log.h"
#include "transport_port.h"

static void print_help(FILE *f, const char *prog)
{
	fprintf(f,
		"usage: %s [-d DEVICE] [-m FILE] [-r] [-x PROGRAM] [-v]... [-h]\n"
		"Answer the OLT on the OMCI port of the ONU.\n"
		"\n"
		"  -d DEVICE  the OMCI port (default %s)\n"
		"  -m FILE    keep the MIB in FILE, and take it up again at the\n"
		"             start, so that a daemon started again goes on as\n"
		"             the OLT set it up\n"
		"  -r         router mode: the OLT's bridge and VLAN rules are not\n"
		"             applied, and the GEM interfaces stay out of any bridge;\n"
		"             SIGUSR1 puts a running daemon in router mode, SIGUSR2\n"
		"             back in bridge mode\n"
		"  -x PROGRAM run PROGRAM with the argument omcid after each\n"
		"             change of what the daemon publishes in /run, and\n"
		"             each GEM interface made or taken away\n"
		"  -v         log more; may be given more than once\n"
		"  -h         show this text\n",
		prog, OMCI_PORT_DEFAULT);
}

int main(int argc, char **argv)
{
	const char *device = OMCI_PORT_DEFAULT;
	int level = OMCI_LOG_INFO;
	int opt;

	while ((opt = getopt(argc, argv, "d:m:rx:vh")) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'm':
			if (!omcid_keep(optarg)) {
				fprintf(stderr,
					"%s: -m %s: the path is too long\n",
					argv[0], optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'r':
			omci_hw_kernel_router(true);
			break;
		case 'x':
			omci_hw_kernel_notify(optarg);
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
#ifdef OMCID_UBUS
	omcid_bus_set(&omcid_bus_ubus);
#endif
	return omcid_serve(&transport, OMCI_CTL_SOCKET_PATH);
}
