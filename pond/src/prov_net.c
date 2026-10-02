// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The providers of the interfaces: link state and counters, from sysfs and ethtool. */

#define _GNU_SOURCE

#include "prov.h"
#include "providers.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/ethtool.h>
#include <linux/sockios.h>
#endif

static int net_attr(const char *ifname, const char *what, char *out, size_t cap)
{
	char path[256];

	if (snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ifname,
		     what) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	return prov_read_line(path, out, cap);
}

static long long net_attr_num(const char *ifname, const char *what,
			      long long absent)
{
	char t[64];

	if (net_attr(ifname, what, t, sizeof(t)))
		return absent;
	return strtoll(t, NULL, 10);
}

static long long net_attr_hex(const char *ifname, const char *what,
			      long long absent)
{
	char t[64];

	if (net_attr(ifname, what, t, sizeof(t)))
		return absent;
	return strtoll(t, NULL, 16);
}

int prov_ports(struct prov_buf *out)
{
	static const char *const skip[] = { ".", "..", "lo", NULL };
	struct dirent *de;
	char rooted[512];
	DIR *d = opendir(prov_path("/sys/class/net", rooted, sizeof(rooted)));
	bool first = true;
	unsigned int i;

	if (!d)
		return -errno;

	pv_open(out, '[');
	while ((de = readdir(d))) {
		char t[128];
		bool skipit = false;

		for (i = 0; skip[i]; i++)
			if (!strcmp(de->d_name, skip[i]))
				skipit = true;
		if (skipit)
			continue;

		if (!first)
			pv_comma(out);
		first = false;

		pv_open(out, '{');
		pv_key(out, "name");
		pv_str(out, de->d_name);

		pv_comma(out);
		pv_key(out, "admin_up");
		pv_bool(out,
			(net_attr_hex(de->d_name, "flags", 0) & IFF_UP) != 0);

		pv_comma(out);
		pv_key(out, "carrier");
		pv_num(out, net_attr_num(de->d_name, "carrier", -1));

		pv_comma(out);
		pv_key(out, "operstate");
		if (net_attr(de->d_name, "operstate", t, sizeof(t)))
			pv_null(out);
		else
			pv_str(out, t);

		pv_comma(out);
		pv_key(out, "speed_mbit");

		pv_num(out, net_attr_num(de->d_name, "speed", -1));

		pv_comma(out);
		pv_key(out, "mtu");
		pv_num(out, net_attr_num(de->d_name, "mtu", -1));

		pv_comma(out);
		pv_key(out, "mac");
		if (net_attr(de->d_name, "address", t, sizeof(t)))
			pv_null(out);
		else
			pv_str(out, t);

		pv_comma(out);
		pv_key(out, "cpu_path");
		pv_open(out, '{');
		pv_key(out, "rx_packets");
		pv_num(out,
		       net_attr_num(de->d_name, "statistics/rx_packets", -1));
		pv_comma(out);
		pv_key(out, "tx_packets");
		pv_num(out,
		       net_attr_num(de->d_name, "statistics/tx_packets", -1));
		pv_comma(out);
		pv_key(out, "rx_bytes");
		pv_num(out,
		       net_attr_num(de->d_name, "statistics/rx_bytes", -1));
		pv_comma(out);
		pv_key(out, "tx_bytes");
		pv_num(out,
		       net_attr_num(de->d_name, "statistics/tx_bytes", -1));
		pv_comma(out);

		pv_key(out, "rx_missed");
		pv_num(out, net_attr_num(de->d_name,
					 "statistics/rx_missed_errors", -1));
		pv_close(out, '}');

		pv_close(out, '}');

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
	closedir(d);
	return 0;
}

#ifndef __linux__

static int ethtool_stats(int fd, const char *ifname, struct prov_buf *out)
{
	(void)fd;
	(void)ifname;
	(void)out;
	return -EOPNOTSUPP;
}
#else
static int ethtool_stats(int fd, const char *ifname, struct prov_buf *out)
{
	struct ifreq ifr;
	struct ethtool_drvinfo drv = { .cmd = ETHTOOL_GDRVINFO };
	struct ethtool_gstrings *strings = NULL;
	struct ethtool_stats *stats = NULL;
	unsigned int n, i;
	int err = 0;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	ifr.ifr_data = (void *)&drv;
	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0)
		return -errno;
	n = drv.n_stats;
	if (!n)
		return -ENODATA;

	strings = calloc(1, sizeof(*strings) + n * ETH_GSTRING_LEN);
	stats = calloc(1, sizeof(*stats) + n * sizeof(uint64_t));
	if (!strings || !stats) {
		err = -ENOMEM;
		goto out;
	}

	strings->cmd = ETHTOOL_GSTRINGS;
	strings->string_set = ETH_SS_STATS;
	strings->len = n;
	ifr.ifr_data = (void *)strings;
	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) {
		err = -errno;
		goto out;
	}

	stats->cmd = ETHTOOL_GSTATS;
	stats->n_stats = n;
	ifr.ifr_data = (void *)stats;
	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) {
		err = -errno;
		goto out;
	}

	pv_open(out, '{');
	for (i = 0; i < n; i++) {
		char name[ETH_GSTRING_LEN + 1];

		memcpy(name, &strings->data[i * ETH_GSTRING_LEN],
		       ETH_GSTRING_LEN);
		name[ETH_GSTRING_LEN] = '\0';
		if (!name[0])
			continue;
		if (i)
			pv_comma(out);
		pv_key(out, name);
		pv_num(out, (long long)stats->data[i]);
	}
	pv_close(out, '}');
out:
	free(strings);
	free(stats);
	return err;
}
#endif

int prov_port_counters(struct prov_buf *out)
{
	static const char *const ports[] = { "sfp", "pon", NULL };
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	unsigned int i;

	if (fd < 0)
		return -errno;

	pv_open(out, '{');
	for (i = 0; ports[i]; i++) {
		int err;

		if (i)
			pv_comma(out);
		pv_key(out, ports[i]);
		err = ethtool_stats(fd, ports[i], out);
		if (err) {
			char why[96];

			snprintf(why, sizeof(why), "{\"error\":\"%s\"}",
				 strerror(-err));
			pv_raw(out, why);
		}
	}
	pv_close(out, '}');
	close(fd);
	return 0;
}

int prov_gem_counters(struct prov_buf *out)
{
	struct dirent *de;
	char rooted[512];
	DIR *d;
	int fd;
	bool first = true;

	d = opendir(prov_path("/sys/class/net", rooted, sizeof(rooted)));
	if (!d)
		return -errno;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		closedir(d);
		return -errno;
	}

	pv_open(out, '{');
	while ((de = readdir(d))) {
		int err;

		if (strncmp(de->d_name, "gem", 3))
			continue;

		if (!first)
			pv_comma(out);
		first = false;
		pv_key(out, de->d_name);
		err = ethtool_stats(fd, de->d_name, out);
		if (err) {
			char why[96];

			snprintf(why, sizeof(why), "{\"error\":\"%s\"}",
				 strerror(-err));
			pv_raw(out, why);
		}
	}
	pv_close(out, '}');

	close(fd);
	closedir(d);
	return 0;
}
