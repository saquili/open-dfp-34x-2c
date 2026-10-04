// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * pon: show the PON devices of the kernel and their ONU through the "pon"
 * generic netlink family: the activation state, the identifiers the OLT
 * assigned, the alarms, the counters and the T-CONTs. "pon monitor" prints
 * every change the kernel announces, with the time it came; "pon gem" lists
 * the GEM interfaces, from the kernel's links, with the upstream priorities,
 * queue, weight and rates of those that carry traffic upstream.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>
#include <linux/rtnetlink.h>

#include "gemlink.h"
#include "genl.h"
#include "pondev.h"

#define MAX_DEVS 8
#define MAX_TCONTS 64

struct family {
	uint16_t id;
	uint32_t monitor;
};

struct dump {
	struct pon_dev dev[MAX_DEVS];
	size_t ndev;
	struct pon_tcont tcont[MAX_TCONTS];
	size_t ntcont;
};

static int dev_cb(const struct nlmsghdr *nlh, void *data)
{
	struct dump *d = data;

	if (d->ndev == MAX_DEVS)
		return MNL_CB_OK;
	if (pon_dev_parse(nlh, &d->dev[d->ndev]))
		return MNL_CB_ERROR;
	d->ndev++;
	return MNL_CB_OK;
}

static int tcont_cb(const struct nlmsghdr *nlh, void *data)
{
	struct dump *d = data;

	if (d->ntcont == MAX_TCONTS)
		return MNL_CB_OK;
	if (pon_tcont_parse(nlh, &d->tcont[d->ntcont]))
		return MNL_CB_ERROR;
	d->ntcont++;
	return MNL_CB_OK;
}

static int show(struct mnl_socket *nl, const struct family *fam, bool serial)
{
	static struct dump d;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	int err;

	err = genl_talk(nl,
			genl_request(buf, fam->id, PON_CMD_DEV_GET, NLM_F_DUMP, 2),
			dev_cb, &d);
	if (!err)
		err = genl_talk(nl,
				genl_request(buf, fam->id, PON_CMD_TCONT_GET,
					     NLM_F_DUMP, 3),
				tcont_cb, &d);
	if (err) {
		fprintf(stderr, "pon: the kernel did not answer: %s\n",
			strerror(-err));
		return 1;
	}
	if (!d.ndev) {
		fprintf(stderr, "pon: the kernel has no PON device\n");
		return 1;
	}

	for (size_t i = 0; i < d.ndev; i++) {
		char ifname[IF_NAMESIZE] = "";

		if (!if_indextoname(d.dev[i].ifindex, ifname))
			ifname[0] = '\0';
		pon_dev_print(stdout, &d.dev[i], ifname, serial);
		for (size_t t = 0; t < d.ntcont; t++)
			if (d.tcont[t].dev == d.dev[i].id)
				pon_tcont_print(stdout, &d.tcont[t]);
	}
	return 0;
}

static int monitor_cb(const struct nlmsghdr *nlh, void *data)
{
	const struct genlmsghdr *g = mnl_nlmsg_get_payload(nlh);
	struct timespec now;
	struct pon_dev d;
	char ifname[IF_NAMESIZE] = "";

	(void)data;
	if (g->cmd != PON_CMD_DEV_CHANGE_NTF || pon_dev_parse(nlh, &d))
		return MNL_CB_OK;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!if_indextoname(d.ifindex, ifname))
		ifname[0] = '\0';
	printf("%ld.%03ld ", (long)now.tv_sec, now.tv_nsec / 1000000);
	pon_dev_print(stdout, &d, ifname, false);
	fflush(stdout);
	return MNL_CB_OK;
}

static int watch_changes(struct mnl_socket *nl, const struct family *fam)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	unsigned int group = fam->monitor;

	if (!group || mnl_socket_setsockopt(nl, NETLINK_ADD_MEMBERSHIP, &group,
					    sizeof(group)) < 0) {
		fprintf(stderr, "pon: cannot join the monitor group: %s\n",
			group ? strerror(errno) : "the family has none");
		return 1;
	}
	for (;;) {
		ssize_t n = mnl_socket_recvfrom(nl, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				return 0;
			fprintf(stderr, "pon: %s\n", strerror(errno));
			return 1;
		}
		mnl_cb_run(buf, (size_t)n, 0, 0, monitor_cb, NULL);
	}
}

static int gem_cb(const struct nlmsghdr *nlh, void *data)
{
	struct gem_link g;
	int rc = gem_link_parse(nlh, &g);

	(void)data;
	if (rc < 0)
		return MNL_CB_ERROR;
	if (rc)
		gem_link_print(stdout, &g);
	return MNL_CB_OK;
}

/* Every GEM interface, one line each, from a dump of the links. */
static int show_gems(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifm;
	int err;

	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		fprintf(stderr, "pon: no route netlink: %s\n", strerror(errno));
		if (nl)
			mnl_socket_close(nl);
		return 1;
	}
	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 1;
	ifm = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifm));
	ifm->ifi_family = AF_UNSPEC;

	err = genl_talk(nl, nlh, gem_cb, NULL);
	mnl_socket_close(nl);
	if (err) {
		fprintf(stderr, "pon: the kernel did not list its links: %s\n",
			strerror(-err));
		return 1;
	}
	return 0;
}

static int print_usage(void)
{
	fprintf(stderr,
		"usage: pon [-s]       the PON devices, their ONU and T-CONTs;\n"
		"                      -s also prints the serial number\n"
		"       pon monitor    every change the kernel announces\n"
		"       pon gem        the GEM interfaces: Port-ID, direction,\n"
		"                      T-CONT, multicast, and upstream the\n"
		"                      priorities, queue, weight and rates\n");
	return 2;
}

int main(int argc, char **argv)
{
	bool serial = false, watch = false, gem = false;
	struct mnl_socket *nl;
	struct family fam;
	int i, err;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s"))
			serial = true;
		else if (!strcmp(argv[i], "monitor"))
			watch = true;
		else if (!strcmp(argv[i], "gem"))
			gem = true;
		else
			return print_usage();
	}

	if (gem)
		return show_gems();

	nl = mnl_socket_open(NETLINK_GENERIC);
	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		fprintf(stderr, "pon: no generic netlink: %s\n",
			strerror(errno));
		return 1;
	}
	memset(&fam, 0, sizeof(fam));
	err = genl_resolve(nl, PON_FAMILY_NAME, PON_MCGRP_MONITOR, &fam.id,
			   &fam.monitor);
	if (err == -ENOENT) {
		fprintf(stderr,
			"pon: the kernel has no \"%s\" family: is the PON class built in?\n",
			PON_FAMILY_NAME);
	} else if (err) {
		fprintf(stderr, "pon: the kernel did not answer: %s\n",
			strerror(-err));
	}
	if (err) {
		mnl_socket_close(nl);
		return 1;
	}

	err = watch ? watch_changes(nl, &fam) : show(nl, &fam, serial);
	mnl_socket_close(nl);
	return err;
}
