// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* omcivlan: list what became of each VLAN rule, through the control socket. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omcid/array_size.h"
#include "omcid/ctl_proto.h"
#include "omcid/libomcid.h"

static const char *fate_name(uint8_t f)
{
	switch (f) {
	case 0:
		return "programmed";
	case 1:
		return "collapsed";
	case 2:
		return "shadowed";
	case 3:
		return "over-budget";
	case 4:
		return "unsupported";
	case 5:
		return "inert";
	default:
		return "?";
	}
}

static void note(const struct omcid_vlan_fate_entry *e, char *out, size_t cap)
{
	switch (e->fate) {
	case 1:
		(void)snprintf(out, cap, "same entry as %u->%u", e->rival_match,
			       e->rival_net);
		break;
	case 2:
		(void)snprintf(out, cap, "lost to %u->%u", e->rival_match,
			       e->rival_net);
		break;
	case 3:
		(void)snprintf(out, cap, "classifier full");
		break;
	case 4:
		(void)snprintf(out, cap, "this dataplane cannot express it");
		break;
	case 5:
		(void)snprintf(out, cap, "nothing to program");
		break;
	default:
		out[0] = '\0';
		break;
	}
}

int main(int argc, char **argv)
{
	const char *sock = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc)
			sock = argv[++i];
		else {
			(void)fprintf(stderr, "usage: %s [-s SOCKET]\n",
				      argv[0]);
			return 2;
		}
	}

	struct omcid_conn *conn = omcid_connect(sock);

	if (conn == NULL) {
		perror("omcivlan: cannot reach omcid on its control socket");
		return 1;
	}

	static struct omcid_vlan_fate_entry rec[128];
	size_t n = 0, total = 0;
	unsigned int over = 0;
	const int rc = omcid_vlan_fate_list(conn, rec, ARRAY_SIZE(rec), &n,
					    &total, &over);
	omcid_disconnect(conn);

	if (rc != 0) {
		(void)fprintf(stderr,
			      "omcivlan: the daemon refused the request (%d)\n",
			      rc);
		return 1;
	}
	if (total == 0u) {
		printf("no ME 171 rows\n");
		return 0;
	}

	printf("inst    row  fate         match   ->  network   note\n");
	unsigned int lost = 0;

	for (size_t i = 0; i < n; i++) {
		char why[64];

		note(&rec[i], why, sizeof(why));
		printf("0x%04x  %3u  %-11s %5u   ->  %7u   %s\n",
		       rec[i].instance, rec[i].row, fate_name(rec[i].fate),
		       rec[i].match_cvid, rec[i].net_vid, why);
		if (rec[i].fate == 2u || rec[i].fate == 3u)
			lost++;
	}

	if (n < total)
		printf("\n%zu of %zu records shown\n", n, total);
	if (over > 0u)
		printf("%u row(s) did not fit the daemon's snapshot and are not listed\n",
		       over);
	printf("\n%u row(s) the OLT believes are in force and are not\n", lost);
	return 0;
}
