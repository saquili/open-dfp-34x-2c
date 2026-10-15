// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * sstats: the driver statistics of an interface, as "ethtool -S" prints them,
 * for a unit whose image has no ethtool.
 *
 *   sstats IFACE [PATTERN]
 *
 * Prints name and value of each statistic, and with a PATTERN only those whose
 * name contains it.
 */

#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/ethtool.h>
#include <linux/sockios.h>

int main(int argc, char **argv)
{
	struct ifreq ifr;
	struct ethtool_sset_info *si;
	struct ethtool_gstrings *gs;
	struct ethtool_stats *st;
	unsigned int n;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: sstats IFACE [PATTERN]\n");
		return 2;
	}
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("sstats");
		return 1;
	}
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", argv[1]);

	si = calloc(1, sizeof(*si) + sizeof(uint32_t));
	if (!si) {
		perror("sstats");
		return 1;
	}
	si->cmd = ETHTOOL_GSSET_INFO;
	si->sset_mask = 1ULL << ETH_SS_STATS;
	ifr.ifr_data = (void *)si;
	if (ioctl(fd, SIOCETHTOOL, &ifr) || !si->sset_mask) {
		fprintf(stderr, "sstats: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	n = si->data[0];
	gs = calloc(1, sizeof(*gs) + (size_t)n * ETH_GSTRING_LEN);
	st = calloc(1, sizeof(*st) + (size_t)n * sizeof(uint64_t));
	if (!gs || !st) {
		perror("sstats");
		return 1;
	}
	gs->cmd = ETHTOOL_GSTRINGS;
	gs->string_set = ETH_SS_STATS;
	gs->len = n;
	ifr.ifr_data = (void *)gs;
	if (ioctl(fd, SIOCETHTOOL, &ifr)) {
		fprintf(stderr, "sstats: strings: %s\n", strerror(errno));
		return 1;
	}
	st->cmd = ETHTOOL_GSTATS;
	st->n_stats = n;
	ifr.ifr_data = (void *)st;
	if (ioctl(fd, SIOCETHTOOL, &ifr)) {
		fprintf(stderr, "sstats: stats: %s\n", strerror(errno));
		return 1;
	}
	for (unsigned int i = 0; i < n; i++) {
		const char *name = (const char *)gs->data + (size_t)i * ETH_GSTRING_LEN;

		if (argc > 2 && !strstr(name, argv[2]))
			continue;
		printf("%-32s %llu\n", name, (unsigned long long)st->data[i]);
	}
	return 0;
}
