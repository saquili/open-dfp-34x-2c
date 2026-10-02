// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * omci_ctl_probe: check, on the board, that the daemon answers on its
 * control socket, and print its counters.
 */

#include <stdio.h>
#include <string.h>

#include "omcid/libomcid.h"

int main(void)
{
	struct omcid_conn *c = omcid_connect(NULL);

	if (c == NULL) {
		printf("omci_ctl_probe: cannot reach omcid on its control socket\n");
		return 1;
	}

	struct omcid_conn_stats st = { 0, 0, 0 };

	if (omcid_stats(c, &st) == 0)
		printf("stats: handled=%u rejected=%u answered=%u\n",
		       st.handled, st.rejected, st.answered);
	else
		printf("stats: FAILED\n");

	uint16_t inst[16];
	size_t n = 0;

	if (omcid_me_list(c, 256, inst, 16, &n) == 0) {
		printf("me 256 (ONU-G): %zu instance(s):", n);
		for (size_t i = 0; i < n; i++)
			printf(" 0x%04x", inst[i]);
		printf("\n");

		if (n > 0) {
			uint8_t row[512];
			size_t got = 0;

			if (omcid_me_get(c, 256, inst[0], row, sizeof(row),
					 &got) == 0) {
				printf("me 256/0x%04x: %zu attribute bytes:",
				       inst[0], got);
				for (size_t i = 0; i < got && i < 16; i++)
					printf(" %02x", row[i]);
				printf("%s\n", got > 16 ? " ..." : "");
			} else {
				printf("me 256 get: FAILED\n");
			}
		}
	} else {
		printf("me 256 list: FAILED\n");
	}

	omcid_disconnect(c);
	return 0;
}
