// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * pon: show the PON devices of the kernel and their ONU through the "pon"
 * generic netlink family: the activation state, the identifiers the OLT
 * assigned, the alarms, the counters and the T-CONTs. "pon monitor" prints
 * every change the kernel announces, with the time it came.
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

static int family_attr(const struct nlattr *a, void *data)
{
	struct family *fam = data;

	if (mnl_attr_get_type(a) == CTRL_ATTR_FAMILY_ID &&
	    mnl_attr_get_payload_len(a) == sizeof(uint16_t)) {
		fam->id = mnl_attr_get_u16(a);
	} else if (mnl_attr_get_type(a) == CTRL_ATTR_MCAST_GROUPS) {
		const struct nlattr *grp, *g;

		mnl_attr_for_each_nested(grp, a) {
			const char *name = NULL;
			uint32_t id = 0;

			mnl_attr_for_each_nested(g, grp) {
				if (mnl_attr_get_type(g) ==
				    CTRL_ATTR_MCAST_GRP_NAME)
					name = mnl_attr_get_str(g);
				else if (mnl_attr_get_type(g) ==
					 CTRL_ATTR_MCAST_GRP_ID)
					id = mnl_attr_get_u32(g);
			}
			if (name && !strcmp(name, PON_MCGRP_MONITOR))
				fam->monitor = id;
		}
	}
	return MNL_CB_OK;
}

static int family_cb(const struct nlmsghdr *nlh, void *data)
{
	return mnl_attr_parse(nlh, sizeof(struct genlmsghdr), family_attr,
			      data) < 0 ?
		       MNL_CB_ERROR :
		       MNL_CB_OK;
}

static struct nlmsghdr *request(char *buf, uint16_t type, uint8_t cmd,
				uint16_t flags, uint32_t seq)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct genlmsghdr *g;

	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST | flags;
	nlh->nlmsg_seq = seq;
	g = mnl_nlmsg_put_extra_header(nlh, sizeof(*g));
	g->cmd = cmd;
	g->version = 1;
	return nlh;
}

/* Send one request and run @cb on every message of the answer. */
static int talk(struct mnl_socket *nl, struct nlmsghdr *nlh, mnl_cb_t cb,
		void *data)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	const uint32_t port = mnl_socket_get_portid(nl);
	const uint32_t seq = nlh->nlmsg_seq;
	ssize_t n;
	int ret;

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		return -errno;
	do {
		n = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (n < 0)
			return -errno;
		ret = mnl_cb_run(buf, (size_t)n, seq, port, cb, data);
	} while (ret > MNL_CB_STOP);
	return ret < 0 ? -errno : 0;
}

static int resolve(struct mnl_socket *nl, struct family *fam)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh =
		request(buf, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, 1);

	mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, PON_FAMILY_NAME);
	memset(fam, 0, sizeof(*fam));
	return talk(nl, nlh, family_cb, fam);
}

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

	err = talk(nl, request(buf, fam->id, PON_CMD_DEV_GET, NLM_F_DUMP, 2),
		   dev_cb, &d);
	if (!err)
		err = talk(nl,
			   request(buf, fam->id, PON_CMD_TCONT_GET, NLM_F_DUMP,
				   3),
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

static int print_usage(void)
{
	fprintf(stderr,
		"usage: pon [-s]       the PON devices, their ONU and T-CONTs;\n"
		"                      -s also prints the serial number\n"
		"       pon monitor    every change the kernel announces\n");
	return 2;
}

int main(int argc, char **argv)
{
	bool serial = false, watch = false;
	struct mnl_socket *nl;
	struct family fam;
	int i, err;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s"))
			serial = true;
		else if (!strcmp(argv[i], "monitor"))
			watch = true;
		else
			return print_usage();
	}

	nl = mnl_socket_open(NETLINK_GENERIC);
	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		fprintf(stderr, "pon: no generic netlink: %s\n",
			strerror(errno));
		return 1;
	}
	err = resolve(nl, &fam);
	if (err || !fam.id) {
		fprintf(stderr,
			"pon: the kernel has no \"%s\" family: is the PON class built in?\n",
			PON_FAMILY_NAME);
		mnl_socket_close(nl);
		return 1;
	}

	err = watch ? watch_changes(nl, &fam) : show(nl, &fam, serial);
	mnl_socket_close(nl);
	return err;
}
