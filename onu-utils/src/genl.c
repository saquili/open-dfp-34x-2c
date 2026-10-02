// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The answer to a request ends with the end of a dump or with the kernel's
 * acknowledgement. A request that is neither a dump nor asks for the
 * acknowledgement is answered by its one message and nothing after it, and
 * a reader that waits for the end then waits forever: so every request that
 * is not a dump asks for it.
 */

#include "genl.h"

#include <errno.h>
#include <string.h>

#include <linux/genetlink.h>

struct found {
	const char *group;
	uint16_t id;
	uint32_t group_id;
};

static void group_attr(const struct nlattr *grp, struct found *f)
{
	const struct nlattr *g;
	const char *name = NULL;
	uint32_t id = 0;

	mnl_attr_for_each_nested(g, grp) {
		if (mnl_attr_get_type(g) == CTRL_ATTR_MCAST_GRP_NAME)
			name = mnl_attr_get_str(g);
		else if (mnl_attr_get_type(g) == CTRL_ATTR_MCAST_GRP_ID)
			id = mnl_attr_get_u32(g);
	}
	if (name && !strcmp(name, f->group))
		f->group_id = id;
}

static int family_attr(const struct nlattr *a, void *data)
{
	struct found *f = data;
	const struct nlattr *grp;

	if (mnl_attr_get_type(a) == CTRL_ATTR_FAMILY_ID &&
	    mnl_attr_get_payload_len(a) == sizeof(uint16_t)) {
		f->id = mnl_attr_get_u16(a);
	} else if (mnl_attr_get_type(a) == CTRL_ATTR_MCAST_GROUPS && f->group) {
		mnl_attr_for_each_nested(grp, a)
			group_attr(grp, f);
	}
	return MNL_CB_OK;
}

static int family_cb(const struct nlmsghdr *nlh, void *data)
{
	if (mnl_attr_parse(nlh, sizeof(struct genlmsghdr), family_attr, data) < 0)
		return MNL_CB_ERROR;
	return MNL_CB_OK;
}

struct nlmsghdr *genl_request(char *buf, uint16_t type, uint8_t cmd,
			      uint16_t flags, uint32_t seq)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct genlmsghdr *g;

	if (!(flags & NLM_F_DUMP))
		flags |= NLM_F_ACK;
	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST | flags;
	nlh->nlmsg_seq = seq;
	g = mnl_nlmsg_put_extra_header(nlh, sizeof(*g));
	g->cmd = cmd;
	g->version = 1;
	return nlh;
}

int genl_talk(struct mnl_socket *nl, struct nlmsghdr *nlh, mnl_cb_t cb,
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
		errno = 0;
		ret = mnl_cb_run(buf, (size_t)n, seq, port, cb, data);
	} while (ret > MNL_CB_STOP);
	if (ret < 0)
		return errno ? -errno : -EPROTO;
	return 0;
}

int genl_resolve(struct mnl_socket *nl, const char *name, const char *group,
		 uint16_t *id, uint32_t *group_id)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct found f = { .group = group };
	struct nlmsghdr *nlh;
	int err;

	nlh = genl_request(buf, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, 1);
	mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, name);
	err = genl_talk(nl, nlh, family_cb, &f);
	if (err)
		return err;
	if (!f.id)
		return -ENOENT;
	*id = f.id;
	if (group_id)
		*group_id = f.group_id;
	return 0;
}
