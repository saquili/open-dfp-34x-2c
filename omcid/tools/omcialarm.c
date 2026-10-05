// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * omcialarm: print the alarms the hardware reports, and the alarm
 * notifications the daemon would send for them.
 */

#include <stdio.h>

#include "hw/hw.h"
#include "me/pm.h"
#include "omci/alarm.h"
#include "omcid/array_size.h"

static const char *const NAME[OMCI_HW_ALARM_COUNT] = {
	"LOS", "LOF", "LOM", "SF", "SD", "TX Too Long", "TX Mismatch",
};

int main(void)
{
	uint32_t mask = 0;
	const enum omci_status rc = omci_hw()->alarms(&mask);

	if (rc != OMCI_OK) {
		fprintf(stderr,
			"omcialarm: the MAC would not answer (status %d)\n",
			(int)rc);
		return 1;
	}

	printf("hardware alarms, mask 0x%02lx:\n", (unsigned long)mask);
	for (unsigned int a = 0; a < (unsigned int)OMCI_HW_ALARM_COUNT; a++)
		printf("  %-12s %s\n", NAME[a],
		       (mask & (1u << a)) ? "occur" : "clear");

	struct omci_alarm *al = NULL;

	if (omci_alarm_init(&al) != OMCI_OK) {
		fprintf(stderr, "omcialarm: cannot bring up the alarm plane\n");
		return 1;
	}
	if (omci_alarm_poll(al, NULL) != OMCI_OK) {
		fprintf(stderr, "omcialarm: poll failed\n");
		return 1;
	}

	const uint16_t n = omci_alarm_snapshot(al, false);

	printf("\nOMCI: %u alarmed entit%s (= the GetAllAlarms count)\n", n,
	       n == 1 ? "y" : "ies");
	for (uint16_t s = 0; s < n; s++) {
		struct omci_alarm_entry e;

		if (!omci_alarm_snapshot_get(al, s, &e))
			break;
		printf("  seq %u: class %u instance 0x%04x  bitmap ", s,
		       e.class_id, e.instance);
		for (unsigned int i = 0; i < OMCI_ALARM_BITMAP_LEN; i++)
			printf("%02x", e.bitmap[i]);
		printf("\n    bits set:");
		for (unsigned int b = 0; b < OMCI_ALARM_BITMAP_LEN * 8u; b++)
			if (e.bitmap[b / 8u] & (0x80u >> (b % 8u)))
				printf(" %u", b);
		printf("\n");
	}

	omci_alarm_fini(al);

	static uint64_t c[OMCI_HW_PORT_COUNTERS];

	printf("\nwhich switch ports answer at all (ifInOctets, idx 0):\n");
	for (unsigned int port = 0; port < 8u; port++) {
		const enum omci_status pr = omci_hw()->port_counters(port, c);

		printf("  port %u: %s", port, pr == OMCI_OK ? "ok" : "REFUSED");
		if (pr == OMCI_OK)
			printf("  ifInOctets=%llu", (unsigned long long)c[0]);
		printf("\n");
	}

	const unsigned int uni = 0u;

	printf("\ncounters on the UNI port (%u):\n", uni);
	static const struct {
		const char *name;
		unsigned int idx;
	} probe[] = {
		{ "ifInOctets", 0 },
		{ "ifInUcastPkts", 1 },
		{ "ifOutOctets", 5 },
		{ "ifOutUcastPkts", 7 },
		{ "etherStatsDropEvents", 26 },
		{ "etherStatsCRCAlignErrors", 35 },
		{ "etherStatsRxPkts64Octets", 60 },
		{ "etherStatsTxPkts64Octets", 45 },
	};
	unsigned int read_ok = 0;
	const bool ok = omci_hw()->port_counters(uni, c) == OMCI_OK;

	for (unsigned int i = 0; i < ARRAY_SIZE(probe); i++) {
		if (!ok) {
			printf("  %-26s READ FAILED\n", probe[i].name);
			continue;
		}
		read_ok++;
		printf("  %-26s %llu\n", probe[i].name,
		       (unsigned long long)c[probe[i].idx]);
	}
	printf("  %u/%u counters read\n", read_ok,
	       (unsigned int)(ARRAY_SIZE(probe)));

	unsigned int accounted = 0;

	for (unsigned int i = 0; i < 70u; i++)
		if (omci_pm_counter_accounted(i, NULL))
			accounted++;
	printf("  %u counter indices accounted for by the PM mapping\n",
	       accounted);

	return 0;
}
