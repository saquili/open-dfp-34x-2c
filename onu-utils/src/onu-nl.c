// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-nl: the traffic control and the switch's parameters that the stick's
 * scripts set, through netlink, in place of tc and devlink of iproute2,
 * which only the RAM image carries (ADR 0014, step 9g).
 *
 *   onu-nl clsact add|del DEV
 *   onu-nl filter add DEV [pref N] [vlan VID | dst ADDRESS[/LEN]] [hw]
 *                         trap|drop|redirect TO
 *   onu-nl filter del DEV pref N
 *   onu-nl filter prefs DEV
 *   onu-nl param NAME [VALUE]
 *   onu-nl addr-wait DEV ADDRESS/LEN [SECONDS]
 *
 * "clsact add" leaves a clsact qdisc that is there already. A filter is on
 * the ingress of DEV; "vlan" takes the frames of a VLAN, "dst" the IPv4
 * packets to an address of a prefix (/32 without LEN), and without either it
 * takes every frame; "hw" keeps it in
 * the switch alone; "redirect" hands what it takes to the ingress of TO.
 * "filter prefs" prints the priorities of the filters on the ingress of
 * DEV, one a line, the lowest first. "param" prints the value of a
 * parameter of the stick's devlink instance, the first the kernel lists,
 * or sets it (src/dlparam.h). "addr-wait" returns when DEV has the IPv4
 * address with that prefix length, at once if it has it, else as the
 * kernel says it got it, or fails after SECONDS (10 without): it listens
 * for the kernel's message and does not look in a loop. When the kernel
 * refuses, its own message follows the error.
 *
 * Exit status: 0 done, 1 refused or failed, 2 a wrong command line.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libmnl/libmnl.h>
#include <linux/devlink.h>
#include <linux/genetlink.h>
#include <linux/rtnetlink.h>

#include "dlparam.h"
#include "genl.h"
#include "tcrule.h"

/**
 * struct talk - A request under way.
 * @cb: what reads each message of the answer, or NULL
 * @data: its data
 * @err: the error that ended the answer
 * @why: the kernel's message about it
 */
struct talk {
	mnl_cb_t cb;
	void *data;
	int err;
	char why[160];
};

static uint32_t g_seq;

static int data_cb(const struct nlmsghdr *nlh, void *data)
{
	struct talk *t = data;

	return t->cb ? t->cb(nlh, t->data) : MNL_CB_OK;
}

static int why_attr(const struct nlattr *a, void *data)
{
	struct talk *t = data;

	if (mnl_attr_get_type(a) == NLMSGERR_ATTR_MSG &&
	    mnl_attr_validate(a, MNL_TYPE_NUL_STRING) == 0)
		snprintf(t->why, sizeof(t->why), "%s", mnl_attr_get_str(a));
	return MNL_CB_OK;
}

/*
 * The acknowledgement, or the error and the kernel's message about it,
 * which follows the request it answers unless the kernel capped that.
 */
static int error_cb(const struct nlmsghdr *nlh, void *data)
{
	struct talk *t = data;
	const struct nlmsgerr *e = mnl_nlmsg_get_payload(nlh);
	size_t off = sizeof(*e);

	if (nlh->nlmsg_len < mnl_nlmsg_size(sizeof(*e))) {
		t->err = -EBADMSG;
		return MNL_CB_ERROR;
	}
	if (!(nlh->nlmsg_flags & NLM_F_CAPPED) && e->msg.nlmsg_len > sizeof(e->msg))
		off += e->msg.nlmsg_len - sizeof(e->msg);
	if (nlh->nlmsg_flags & NLM_F_ACK_TLVS &&
	    off < mnl_nlmsg_get_payload_len(nlh))
		mnl_attr_parse(nlh, (unsigned int)off, why_attr, t);
	if (!e->error)
		return MNL_CB_STOP;
	t->err = e->error;
	return MNL_CB_ERROR;
}

/* The end of a dump, which carries the error that cut it short. */
static int done_cb(const struct nlmsghdr *nlh, void *data)
{
	struct talk *t = data;
	int err = 0;

	if (mnl_nlmsg_get_payload_len(nlh) >= sizeof(err))
		memcpy(&err, mnl_nlmsg_get_payload(nlh), sizeof(err));
	if (err < 0) {
		t->err = err;
		return MNL_CB_ERROR;
	}
	return MNL_CB_STOP;
}

/*
 * Send @nlh and read its answer to the end, @cb reading each message of it.
 * The error @done, a negative errno or 0, counts as done; another is
 * printed as what @what met.
 */
static int talk(struct mnl_socket *nl, struct nlmsghdr *nlh, mnl_cb_t cb,
		void *data, int done, const char *what)
{
	static const mnl_cb_t ctl[NLMSG_MIN_TYPE] = {
		[NLMSG_ERROR] = error_cb,
		[NLMSG_DONE] = done_cb,
	};
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct talk t = { .cb = cb, .data = data };
	const uint32_t port = mnl_socket_get_portid(nl);
	const uint32_t seq = nlh->nlmsg_seq;
	ssize_t n;
	int ret = MNL_CB_OK;

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		t.err = -errno;
	} else {
		do {
			n = mnl_socket_recvfrom(nl, buf, sizeof(buf));
			if (n < 0) {
				t.err = -errno;
				break;
			}
			errno = 0;
			ret = mnl_cb_run2(buf, (size_t)n, seq, port, data_cb, &t,
					  ctl, NLMSG_MIN_TYPE);
			if (ret < 0 && !t.err)
				t.err = errno ? -errno : -EPROTO;
		} while (ret > MNL_CB_STOP);
	}
	if (!t.err || t.err == done)
		return 0;
	fprintf(stderr, "onu-nl: %s: %s%s%s\n", what, strerror(-t.err),
		t.why[0] ? ": " : "", t.why);
	return t.err;
}

static struct mnl_socket *open_bus(int bus)
{
	struct mnl_socket *nl = mnl_socket_open(bus);
	int one = 1;

	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		fprintf(stderr, "onu-nl: no netlink socket: %s\n",
			strerror(errno));
		if (nl)
			mnl_socket_close(nl);
		return NULL;
	}
	/* Errors without the request, and with the kernel's message. */
	mnl_socket_setsockopt(nl, NETLINK_CAP_ACK, &one, sizeof(one));
	mnl_socket_setsockopt(nl, NETLINK_EXT_ACK, &one, sizeof(one));
	return nl;
}

static int usage(void)
{
	fprintf(stderr,
		"usage: onu-nl clsact add|del DEV\n"
		"       onu-nl filter add DEV [pref N] [vlan VID | dst ADDRESS[/LEN]] [hw]\n"
		"                         trap|drop|redirect TO\n"
		"       onu-nl filter del DEV pref N\n"
		"       onu-nl filter prefs DEV\n"
		"       onu-nl param NAME [VALUE]\n"
		"       onu-nl addr-wait DEV ADDRESS/LEN [SECONDS]\n");
	return 2;
}

/* A decimal number of @s from @min to @max, digits alone. */
static bool decimal(const char *s, unsigned long min, unsigned long max,
		    unsigned long *v)
{
	if (!*s || s[strspn(s, "0123456789")] || strlen(s) > 9)
		return false;
	*v = strtoul(s, NULL, 10);
	return *v >= min && *v <= max;
}

static int interface(const char *name)
{
	int ifindex = (int)if_nametoindex(name);

	if (!ifindex)
		fprintf(stderr, "onu-nl: no interface %s\n", name);
	return ifindex;
}

static int clsact(int argc, char **argv)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct mnl_socket *nl;
	bool add;
	int ifindex, err;

	if (argc != 2 || (strcmp(argv[0], "add") && strcmp(argv[0], "del")))
		return usage();
	add = !strcmp(argv[0], "add");
	ifindex = interface(argv[1]);
	if (!ifindex)
		return 1;
	nl = open_bus(NETLINK_ROUTE);
	if (!nl)
		return 1;
	err = talk(nl, tc_clsact_msg(buf, ifindex, add, ++g_seq), NULL, NULL,
		   add ? -EEXIST : 0, add ? "clsact not added" : "clsact not removed");
	mnl_socket_close(nl);
	return err ? 1 : 0;
}

/* DEV [pref N] [vlan VID | dst ADDRESS[/LEN]] [hw] trap|drop|redirect TO, into @r. */
static bool rule_args(int argc, char **argv, struct tc_rule *r)
{
	bool pref = false;
	unsigned long v;
	int i;

	memset(r, 0, sizeof(*r));
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "pref") && !pref && i + 1 < argc) {
			if (!decimal(argv[++i], 1, 65535, &v))
				return false;
			r->pref = (uint16_t)v;
			pref = true;
		} else if (!strcmp(argv[i], "vlan") && !r->has_vlan &&
			   !r->has_dst && i + 1 < argc) {
			if (!decimal(argv[++i], 0, 4095, &v))
				return false;
			r->vlan = (uint16_t)v;
			r->has_vlan = true;
		} else if (!strcmp(argv[i], "dst") && !r->has_vlan &&
			   !r->has_dst && i + 1 < argc) {
			char addr[INET_ADDRSTRLEN];
			const char *slash = strchr(argv[++i], '/');
			size_t n = slash ? (size_t)(slash - argv[i]) : strlen(argv[i]);
			struct in_addr in;

			if (n >= sizeof(addr))
				return false;
			memcpy(addr, argv[i], n);
			addr[n] = '\0';
			if (inet_pton(AF_INET, addr, &in) != 1)
				return false;
			v = 32;
			if (slash && !decimal(slash + 1, 0, 32, &v))
				return false;
			r->dst = in.s_addr;
			r->dst_len = (uint8_t)v;
			r->has_dst = true;
		} else if (!strcmp(argv[i], "hw") && !r->hw) {
			r->hw = true;
		} else {
			break;
		}
	}
	if (i == argc - 1 && !strcmp(argv[i], "trap")) {
		r->action = TC_RULE_TRAP;
	} else if (i == argc - 1 && !strcmp(argv[i], "drop")) {
		r->action = TC_RULE_DROP;
	} else if (i == argc - 2 && !strcmp(argv[i], "redirect")) {
		r->action = TC_RULE_REDIRECT;
	} else {
		return false;
	}
	return true;
}

static int pref_cb(const struct nlmsghdr *nlh, void *data)
{
	uint8_t *seen = data;
	unsigned int pref = tc_rule_pref(nlh);

	if (pref)
		seen[pref / 8] |= (uint8_t)(1u << (pref % 8));
	return MNL_CB_OK;
}

static int filter(int argc, char **argv)
{
	static uint8_t seen[65536 / 8];
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct mnl_socket *nl;
	struct tc_rule r;
	unsigned long pref = 0;
	int ifindex, err;

	if (argc < 2)
		return usage();
	if (!strcmp(argv[0], "add")) {
		if (!rule_args(argc - 1, argv + 1, &r))
			return usage();
	} else if (!strcmp(argv[0], "del")) {
		if (argc != 4 || strcmp(argv[2], "pref") ||
		    !decimal(argv[3], 1, 65535, &pref))
			return usage();
	} else if (strcmp(argv[0], "prefs") || argc != 2) {
		return usage();
	}
	ifindex = interface(argv[1]);
	if (!ifindex)
		return 1;
	if (!strcmp(argv[0], "add")) {
		r.ifindex = ifindex;
		if (r.action == TC_RULE_REDIRECT) {
			r.to_ifindex = interface(argv[argc - 1]);
			if (!r.to_ifindex)
				return 1;
		}
	}

	nl = open_bus(NETLINK_ROUTE);
	if (!nl)
		return 1;
	if (!strcmp(argv[0], "add")) {
		err = talk(nl, tc_rule_msg(buf, &r, ++g_seq), NULL, NULL, 0,
			   "filter not added");
	} else if (!strcmp(argv[0], "del")) {
		err = talk(nl, tc_rule_del_msg(buf, ifindex, (uint16_t)pref, ++g_seq),
			   NULL, NULL, 0, "filter not removed");
	} else {
		err = talk(nl, tc_rule_dump_msg(buf, ifindex, ++g_seq), pref_cb,
			   seen, 0, "filters not listed");
		for (unsigned int p = 1; !err && p < 65536; p++)
			if (seen[p / 8] & (1u << (p % 8)))
				printf("%u\n", p);
	}
	mnl_socket_close(nl);
	return err ? 1 : 0;
}

static int dev_cb(const struct nlmsghdr *nlh, void *data)
{
	struct dl_dev *d = data;

	if (!d->bus[0] && dl_dev_parse(nlh, d))
		memset(d, 0, sizeof(*d));
	return MNL_CB_OK;
}

static int param_cb(const struct nlmsghdr *nlh, void *data)
{
	if (dl_param_parse(nlh, data)) {
		errno = EBADMSG;
		return MNL_CB_ERROR;
	}
	return MNL_CB_OK;
}

static int param(int argc, char **argv)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	const char *name = argv[0];
	struct mnl_socket *nl;
	struct nlmsghdr *nlh;
	struct dl_dev d = { .bus = "" };
	struct dl_param p = { 0 };
	uint16_t family;
	int err;

	if (argc < 1 || argc > 2 || !*name || strlen(name) >= DL_NAME_MAX)
		return usage();
	nl = open_bus(NETLINK_GENERIC);
	if (!nl)
		return 1;
	err = genl_resolve(nl, DEVLINK_GENL_NAME, NULL, &family, NULL);
	if (err) {
		fprintf(stderr, "onu-nl: %s\n",
			err == -ENOENT ? "the kernel has no devlink" : strerror(-err));
		goto out;
	}
	err = talk(nl, dl_dev_dump_msg(buf, family, ++g_seq), dev_cb, &d, 0,
		   "devlink instances not listed");
	if (!err && !d.bus[0]) {
		fprintf(stderr, "onu-nl: the kernel has no devlink instance\n");
		err = -ENODEV;
	}
	if (!err)
		err = talk(nl, dl_param_get_msg(buf, family, ++g_seq, &d, name),
			   param_cb, &p, 0, name);
	if (!err && !p.have) {
		fprintf(stderr, "onu-nl: %s has no runtime value\n", name);
		err = -ENODATA;
	}
	if (err)
		goto out;
	if (argc == 1) {
		printf("%s\n", p.value);
		goto out;
	}
	err = dl_param_set_msg(buf, family, ++g_seq, &d, name, p.type, argv[1],
			       &nlh);
	if (err) {
		fprintf(stderr, "onu-nl: %s: %s is %s\n", name, argv[1],
			err == -EOPNOTSUPP ? "of a type this does not set" :
					     "no value of its type");
		goto out;
	}
	err = talk(nl, nlh, NULL, NULL, 0, name);
out:
	mnl_socket_close(nl);
	return err ? 1 : 0;
}

/* The address wanted, and whether it is on the interface. */
struct addr_want {
	const char *dev;
	struct in_addr addr;
	uint8_t plen;
	bool found;
};

static int addr_attr(const struct nlattr *a, void *data)
{
	struct in_addr *got = data;

	if (mnl_attr_get_type(a) == IFA_LOCAL &&
	    mnl_attr_get_payload_len(a) == sizeof(*got))
		memcpy(got, mnl_attr_get_payload(a), sizeof(*got));
	return MNL_CB_OK;
}

/* An address that arrived, or one the dump lists: is it the one wanted. */
static int addr_cb(const struct nlmsghdr *nlh, void *data)
{
	struct addr_want *w = data;
	const struct ifaddrmsg *ifa = mnl_nlmsg_get_payload(nlh);
	struct in_addr got = { 0 };

	if (nlh->nlmsg_type != RTM_NEWADDR ||
	    nlh->nlmsg_len < mnl_nlmsg_size(sizeof(*ifa)) ||
	    ifa->ifa_family != AF_INET || ifa->ifa_prefixlen != w->plen ||
	    ifa->ifa_index != if_nametoindex(w->dev))
		return MNL_CB_OK;
	mnl_attr_parse(nlh, sizeof(*ifa), addr_attr, &got);
	if (got.s_addr == w->addr.s_addr)
		w->found = true;
	return MNL_CB_OK;
}

/* The IPv4 addresses the kernel lists now, into @w. */
static int addr_dump(struct addr_want *w)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct mnl_socket *nl = open_bus(NETLINK_ROUTE);
	struct nlmsghdr *nlh;
	struct ifaddrmsg *ifa;
	int err;

	if (!nl)
		return -ENOBUFS;
	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_GETADDR;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = ++g_seq;
	ifa = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifa));
	ifa->ifa_family = AF_INET;
	err = talk(nl, nlh, addr_cb, w, 0, "addresses not listed");
	mnl_socket_close(nl);
	return err;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int addr_wait(int argc, char **argv)
{
	struct addr_want w = { .dev = argv[0] };
	char text[INET_ADDRSTRLEN], buf[MNL_SOCKET_BUFFER_SIZE];
	const char *slash = argc > 1 ? strchr(argv[1], '/') : NULL;
	unsigned long plen, seconds = 10;
	struct mnl_socket *nl;
	long end;

	if (argc < 2 || argc > 3 || !slash ||
	    (size_t)(slash - argv[1]) >= sizeof(text) ||
	    !decimal(slash + 1, 1, 32, &plen) ||
	    (argc == 3 && !decimal(argv[2], 0, 3600, &seconds)))
		return usage();
	memcpy(text, argv[1], (size_t)(slash - argv[1]));
	text[slash - argv[1]] = '\0';
	if (inet_pton(AF_INET, text, &w.addr) != 1)
		return usage();
	w.plen = (uint8_t)plen;

	/* Listen first, so that no address that comes during the dump is lost. */
	nl = mnl_socket_open(NETLINK_ROUTE);
	if (!nl || mnl_socket_bind(nl, RTMGRP_IPV4_IFADDR, MNL_SOCKET_AUTOPID) < 0) {
		fprintf(stderr, "onu-nl: no netlink socket: %s\n", strerror(errno));
		if (nl)
			mnl_socket_close(nl);
		return 1;
	}
	end = now_ms() + (long)seconds * 1000L;
	for (bool dump = true;;) {
		struct pollfd pfd = { .fd = mnl_socket_get_fd(nl), .events = POLLIN };
		long left;
		ssize_t n;

		if (dump) {
			dump = false;
			if (addr_dump(&w) < 0 || w.found)
				break;
		}
		left = end - now_ms();
		if (left <= 0 || poll(&pfd, 1, (int)left) <= 0)
			break;
		n = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (n < 0) {
			/* Messages lost: look at the addresses again. */
			dump = errno == ENOBUFS || errno == EINTR;
			if (!dump)
				break;
			continue;
		}
		mnl_cb_run(buf, (size_t)n, 0, 0, addr_cb, &w);
		if (w.found)
			break;
	}
	mnl_socket_close(nl);
	return w.found ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return usage();
	if (!strcmp(argv[1], "addr-wait"))
		return addr_wait(argc - 2, argv + 2);
	if (!strcmp(argv[1], "clsact"))
		return clsact(argc - 2, argv + 2);
	if (!strcmp(argv[1], "filter"))
		return filter(argc - 2, argv + 2);
	if (!strcmp(argv[1], "param"))
		return param(argc - 2, argv + 2);
	return usage();
}
