// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * mdbadd: add or remove one permanent IPv4 group in a bridge's multicast
 * database, for a bench or a unit whose image has no bridge tool.
 *
 *   mdbadd add|del BRIDGE PORT GROUP VID
 *
 * VID 0 is the bridge's untagged entry. Prints what the kernel answered.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_bridge.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

int main(int argc, char **argv)
{
	struct {
		struct nlmsghdr h;
		struct br_port_msg m;
		char attr[128];
	} req;
	struct br_mdb_entry e;
	struct nlattr *a;
	struct sockaddr_nl nl = { .nl_family = AF_NETLINK };
	struct in_addr g;
	bool add;
	int fd;
	char buf[1024];
	ssize_t n;

	if (argc != 6 || (strcmp(argv[1], "add") && strcmp(argv[1], "del"))) {
		fprintf(stderr, "usage: mdbadd add|del BRIDGE PORT GROUP VID\n");
		return 2;
	}
	add = !strcmp(argv[1], "add");
	if (!inet_aton(argv[4], &g)) {
		fprintf(stderr, "mdbadd: bad group\n");
		return 2;
	}
	memset(&req, 0, sizeof(req));
	memset(&e, 0, sizeof(e));
	e.ifindex = if_nametoindex(argv[3]);
	e.state = MDB_PERMANENT;
	e.vid = (uint16_t)atoi(argv[5]);
	e.addr.proto = htons(0x0800);
	e.addr.u.ip4 = g.s_addr;
	req.m.family = AF_BRIDGE;
	req.m.ifindex = (int)if_nametoindex(argv[2]);
	if (!e.ifindex || !req.m.ifindex) {
		fprintf(stderr, "mdbadd: no such interface\n");
		return 1;
	}
	req.h.nlmsg_type = add ? RTM_NEWMDB : RTM_DELMDB;
	req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
	req.h.nlmsg_seq = 1;
	a = (struct nlattr *)req.attr;
	a->nla_type = MDBA_SET_ENTRY;
	a->nla_len = NLA_HDRLEN + sizeof(e);
	memcpy((char *)a + NLA_HDRLEN, &e, sizeof(e));
	req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.m)) + NLA_ALIGN(a->nla_len);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0 || sendto(fd, &req, req.h.nlmsg_len, 0, (struct sockaddr *)&nl, sizeof(nl)) < 0) {
		perror("mdbadd");
		return 1;
	}
	n = recv(fd, buf, sizeof(buf), 0);
	if (n >= (ssize_t)sizeof(struct nlmsghdr)) {
		struct nlmsghdr *h = (struct nlmsghdr *)buf;

		if (h->nlmsg_type == NLMSG_ERROR) {
			int err = ((struct nlmsgerr *)NLMSG_DATA(h))->error;

			printf("%s %s vid %s: %s\n", argv[1], argv[4], argv[5],
			       err ? strerror(-err) : "ok");
			return err ? 1 : 0;
		}
	}
	printf("no answer\n");
	return 1;
}
