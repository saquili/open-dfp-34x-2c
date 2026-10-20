// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * pon: show the PON devices of the kernel and their ONU through the "pon"
 * generic netlink family: the activation state, the identifiers the OLT
 * assigned, the Port-IDs it encrypts, the alarms, the counters and the
 * T-CONTs. "pon monitor" prints
 * every change the kernel announces, with the time it came; "pon gem" lists
 * the GEM interfaces, from the kernel's links, with the upstream priorities,
 * queue, weight and rates of those that carry traffic upstream, and whether
 * the OLT encrypts each, from the PON devices' reports.
 *
 * An ONU that the OLT disabled with Disable_Serial_Number stays disabled, in
 * O7 with its laser off, even across a power-off, until the OLT enables it
 * (ITU-T G.984.3 clause 10.2.4, Table 10-1, and clause 11.1.2). "pon keep",
 * a service of procd, follows the state the kernel announces and keeps the
 * disable in the configuration store, PON_DISABLED, which "onu-apply
 * identity" gives the PON driver at boot, before the PON comes up. "pon
 * enable" clears the disable by hand, in the driver and in the store.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/wait.h>

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>
#include <linux/rtnetlink.h>

#include "gemlink.h"
#include "genl.h"
#include "pondev.h"

#define MAX_DEVS 8
#define MAX_TCONTS 64

/* The key of the configuration store that keeps the OLT's disable. */
#define KEEP_KEY "PON_DISABLED"

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

/* Run @argv, a program of PATH, and wait for it: its exit status, or -1. */
static int run(const char *const argv[])
{
	int status;
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * Keep the disable in the configuration store, or take it out; onu-config
 * writes nothing when the store holds that already. 0 when it took it.
 */
static int store_disabled(bool disabled)
{
	static const char *const set[] = { "onu-config", "set", KEEP_KEY, "1", NULL };
	static const char *const unset[] = { "onu-config", "unset", KEEP_KEY, NULL };

	return run(disabled ? set : unset) == 0 ? 0 : -1;
}

/* What "pon keep" last told the store, an &enum pon_keep. */
static int kept = PON_KEEP_UNKNOWN;

/*
 * Follow one report: when it says something of the disable that the store
 * was not told yet, tell the store. A store that failed is told again at the
 * next report that says the same.
 */
static void keep_follow(const struct pon_dev *d)
{
	int now = pon_keep_of(d);

	if (now == PON_KEEP_UNKNOWN || now == kept)
		return;
	if (store_disabled(now == PON_KEEP_DISABLED)) {
		if (now == PON_KEEP_DISABLED)
			syslog(LOG_ERR,
			       "the OLT disabled this ONU, and the configuration store did not keep it: a restart enables it");
		else
			syslog(LOG_ERR,
			       "the OLT enabled this ONU, and the configuration store did not forget the disable: a restart brings it up in O7");
		return;
	}
	if (now == PON_KEEP_DISABLED)
		syslog(LOG_WARNING,
		       "the OLT disabled this ONU (O7, emergency stop, laser off): kept across a restart until the OLT enables it; \"pon enable\" clears it by hand");
	else if (kept == PON_KEEP_DISABLED)
		syslog(LOG_NOTICE, "the OLT enabled this ONU again: no longer kept disabled");
	kept = now;
}

static int keep_cb(const struct nlmsghdr *nlh, void *data)
{
	const struct genlmsghdr *g = mnl_nlmsg_get_payload(nlh);
	struct pon_dev d;

	(void)data;
	if ((g->cmd == PON_CMD_DEV_GET || g->cmd == PON_CMD_DEV_CHANGE_NTF) &&
	    !pon_dev_parse(nlh, &d))
		keep_follow(&d);
	return MNL_CB_OK;
}

/* The state of every PON device now, asked on a socket of its own. */
static int keep_dump(const struct family *fam)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct mnl_socket *nl = mnl_socket_open(NETLINK_GENERIC);
	int err;

	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		err = -errno;
		if (nl)
			mnl_socket_close(nl);
		return err;
	}
	err = genl_talk(nl, genl_request(buf, fam->id, PON_CMD_DEV_GET,
					 NLM_F_DUMP, 1),
			keep_cb, NULL);
	mnl_socket_close(nl);
	return err;
}

/*
 * pon keep: the monitor group first, then the state now, so that no change
 * falls between the two; then every change. Changes the socket had no room
 * for are made up for by asking for the state again.
 */
static int keep(struct mnl_socket *nl, const struct family *fam)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	unsigned int group = fam->monitor;
	int err;

	openlog("pon", LOG_PID, LOG_DAEMON);
	if (!group || mnl_socket_setsockopt(nl, NETLINK_ADD_MEMBERSHIP, &group,
					    sizeof(group)) < 0) {
		fprintf(stderr, "pon: cannot join the monitor group: %s\n",
			group ? strerror(errno) : "the family has none");
		return 1;
	}
	err = keep_dump(fam);
	if (err)
		fprintf(stderr, "pon: the kernel did not answer: %s\n",
			strerror(-err));
	for (;;) {
		ssize_t n = mnl_socket_recvfrom(nl, buf, sizeof(buf));

		if (n < 0 && errno == ENOBUFS) {
			keep_dump(fam);
			continue;
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "pon: %s\n", strerror(errno));
			return 1;
		}
		mnl_cb_run(buf, (size_t)n, 0, 0, keep_cb, NULL);
	}
}

/*
 * pon enable: forget the OLT's disable by hand. The driver takes an ONU in
 * O7 to O2 at once, or brings the PON up enabled the next time, and the
 * store no longer gives the disable back at boot.
 */
static int enable_by_hand(void)
{
	static const char *const param[] = {
		"onu-nl", "param", "gpon_disabled", "false", NULL
	};
	int rc = 0;

	openlog("pon", LOG_PID, LOG_USER);
	if (run(param)) {
		fprintf(stderr, "pon: the PON driver did not forget the disable\n");
		rc = 1;
	}
	if (store_disabled(false)) {
		fprintf(stderr, "pon: the configuration store did not forget the disable\n");
		rc = 1;
	}
	if (rc)
		syslog(LOG_ERR, "enabled by hand, not by the OLT, in part: see the errors above");
	else
		syslog(LOG_WARNING,
		       "enabled by hand, not by the OLT: the ONU leaves O7 and is no longer kept disabled");
	return rc;
}

/* A GEM interface, marked encrypted when a PON device of @data says so. */
static int gem_cb(const struct nlmsghdr *nlh, void *data)
{
	const struct dump *d = data;
	struct gem_link g;
	int rc = gem_link_parse(nlh, &g);

	if (rc < 0)
		return MNL_CB_ERROR;
	if (!rc)
		return MNL_CB_OK;
	for (size_t i = 0; i < d->ndev; i++)
		if (pon_dev_encrypted(&d->dev[i], g.port_id))
			g.encrypted = true;
	gem_link_print(stdout, &g);
	return MNL_CB_OK;
}

/*
 * The PON devices, for the Port-IDs the OLT encrypts; none when the kernel
 * has no "pon" family or does not answer, and the GEM interfaces are then
 * listed without them.
 */
static void gem_encryption(struct dump *d)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct mnl_socket *nl = mnl_socket_open(NETLINK_GENERIC);
	struct family fam;

	memset(&fam, 0, sizeof(fam));
	if (nl && mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) == 0 &&
	    genl_resolve(nl, PON_FAMILY_NAME, PON_MCGRP_MONITOR, &fam.id,
			 &fam.monitor) == 0)
		(void)genl_talk(nl, genl_request(buf, fam.id, PON_CMD_DEV_GET,
						 NLM_F_DUMP, 2),
				dev_cb, d);
	if (nl)
		mnl_socket_close(nl);
}

/*
 * Every GEM interface, one line each, from a dump of the links, with whether
 * the OLT encrypts it.
 */
static int show_gems(void)
{
	static struct dump d;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct mnl_socket *nl;
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifm;
	int err;

	gem_encryption(&d);
	nl = mnl_socket_open(NETLINK_ROUTE);

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

	err = genl_talk(nl, nlh, gem_cb, &d);
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
		"       pon keep       keep the OLT's disable of the ONU across\n"
		"                      a restart, in the configuration store\n"
		"       pon enable     forget the OLT's disable, by hand\n"
		"       pon gem        the GEM interfaces: Port-ID, direction,\n"
		"                      T-CONT, multicast, upstream the\n"
		"                      priorities, queue, weight and rates,\n"
		"                      and whether the OLT encrypts them\n");
	return 2;
}

int main(int argc, char **argv)
{
	bool serial = false, watch = false, gem = false, keeper = false;
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
		else if (!strcmp(argv[i], "keep") && argc == 2)
			keeper = true;
		else if (!strcmp(argv[i], "enable") && argc == 2)
			return enable_by_hand();
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

	if (keeper)
		err = keep(nl, &fam);
	else
		err = watch ? watch_changes(nl, &fam) : show(nl, &fam, serial);
	mnl_socket_close(nl);
	return err;
}
