// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* omcitap: follow the frames the daemon receives and sends, through its control socket. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "omcid/ctl_proto.h"
#include "omcid/libomcid.h"

static const char *verdict_name(uint8_t v)
{
	switch (v) {
	case OMCI_CTL_TAP_RX_PARSE:
		return "RX-REJECT-PARSE";
	case OMCI_CTL_TAP_RX_MIC:
		return "RX-REJECT-MIC";
	case OMCI_CTL_TAP_RX_ARAK:
		return "RX-REJECT-ARAK";
	case OMCI_CTL_TAP_RX_REPLAY:
		return "RX-REPLAY";
	case OMCI_CTL_TAP_RX_DISPATCH:
		return "RX-DISPATCH";
	case OMCI_CTL_TAP_TX:
		return "TX";
	case OMCI_CTL_TAP_RX_ACTION:
		return "RX-REJECT-ACTION";
	case OMCI_CTL_TAP_RX_UNKNOWN_ME:
		return "RX-REJECT-UNKNOWN-ME";
	case OMCI_CTL_TAP_RX_UNHANDLED:
		return "RX-REJECT-UNHANDLED";
	default:
		return "?";
	}
}

int main(int argc, char **argv)
{
	const char *sock = OMCI_CTL_SOCKET_PATH;
	long count = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			count = strtol(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			sock = argv[++i];
		else {
			fprintf(stderr, "usage: %s [-n COUNT] [-s SOCKET]\n",
				argv[0]);
			return 2;
		}
	}

	struct omcid_conn *c = omcid_connect(sock);

	if (c == NULL) {
		fprintf(stderr, "omcitap: cannot reach omcid at %s\n", sock);
		return 1;
	}

	const int rc = omcid_tap_subscribe(c);

	if (rc != 0) {
		fprintf(stderr,
			"omcitap: the daemon refused the subscription (status %d)\n",
			rc);
		omcid_disconnect(c);
		return 1;
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	long seen = 0;

	while (count == 0 || seen < count) {
		struct omcid_tap_record r;
		const int got = omcid_tap_next(c, &r);

		if (got < 0) {
			fprintf(stderr, "omcitap: the connection ended\n");
			break;
		}
		if (got == 0)
			continue;

		printf("%-16s mt=%-3u ", verdict_name(r.verdict), r.detail);
		if (r.dropped != 0)
			printf("drop=%u ", r.dropped);
		for (unsigned int i = 0; i < sizeof(r.frame); i++)
			printf("%02x", r.frame[i]);
		printf("\n");
		seen++;
	}

	omcid_disconnect(c);
	return 0;
}
