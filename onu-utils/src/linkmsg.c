// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The attributes are those ip and bridge put (iproute2 6.14): a change of
 * state names the flag it changes in ifi_change and its value in ifi_flags;
 * a VLAN interface names its link, its name and, nested in the link's
 * information, its kind and its VLAN; a bridge its kind and, nested, its
 * VLAN filtering and default PVID. A VLAN of a bridge is a request of the
 * AF_BRIDGE family whose AF_SPEC holds the VLAN's information and, for the
 * bridge itself, the flag that names it.
 */

#include "linkmsg.h"

#include <string.h>
#include <sys/socket.h>

#include <libmnl/libmnl.h>
#include <linux/if.h>
#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

static struct nlmsghdr *link_header(char *buf, uint16_t type, uint16_t flags,
				    uint32_t seq, int ifindex)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct ifinfomsg *ifi;

	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
	nlh->nlmsg_seq = seq;
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifi));
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;
	return nlh;
}

struct nlmsghdr *link_set_msg(char *buf, const struct link_set *s, uint32_t seq)
{
	struct nlmsghdr *nlh = link_header(buf, RTM_NEWLINK, 0, seq, s->ifindex);
	struct ifinfomsg *ifi = mnl_nlmsg_get_payload(nlh);

	if (s->has_state) {
		ifi->ifi_change = IFF_UP;
		ifi->ifi_flags = s->up ? IFF_UP : 0;
	}
	if (s->has_mtu)
		mnl_attr_put_u32(nlh, IFLA_MTU, s->mtu);
	if (s->has_mac)
		mnl_attr_put(nlh, IFLA_ADDRESS, sizeof(s->mac), s->mac);
	if (s->master)
		mnl_attr_put_u32(nlh, IFLA_MASTER, (uint32_t)s->master);
	return nlh;
}

struct nlmsghdr *link_vlan_add_msg(char *buf, int link_ifindex, const char *name,
				   uint16_t vid, uint32_t seq)
{
	struct nlmsghdr *nlh = link_header(buf, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL,
					   seq, 0);
	struct nlattr *info, *data;

	mnl_attr_put_u32(nlh, IFLA_LINK, (uint32_t)link_ifindex);
	mnl_attr_put_strz(nlh, IFLA_IFNAME, name);
	info = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
	mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "vlan");
	data = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
	mnl_attr_put_u16(nlh, IFLA_VLAN_ID, vid);
	mnl_attr_nest_end(nlh, data);
	mnl_attr_nest_end(nlh, info);
	return nlh;
}

struct nlmsghdr *link_del_msg(char *buf, int ifindex, uint32_t seq)
{
	return link_header(buf, RTM_DELLINK, 0, seq, ifindex);
}

struct nlmsghdr *link_bridge_add_msg(char *buf, const char *name, uint32_t seq)
{
	struct nlmsghdr *nlh = link_header(buf, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL,
					   seq, 0);
	struct nlattr *info, *data;

	mnl_attr_put_strz(nlh, IFLA_IFNAME, name);
	info = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
	mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "bridge");
	data = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
	mnl_attr_put_u8(nlh, IFLA_BR_VLAN_FILTERING, 1);
	mnl_attr_put_u16(nlh, IFLA_BR_VLAN_DEFAULT_PVID, 0);
	mnl_attr_nest_end(nlh, data);
	mnl_attr_nest_end(nlh, info);
	return nlh;
}

struct nlmsghdr *bridge_vlan_msg(char *buf, const struct bridge_vlan *v, bool add,
				 uint32_t seq)
{
	struct nlmsghdr *nlh = link_header(buf, add ? RTM_SETLINK : RTM_DELLINK, 0, seq,
					   v->ifindex);
	struct ifinfomsg *ifi = mnl_nlmsg_get_payload(nlh);
	struct bridge_vlan_info info = { .vid = v->vid };
	struct nlattr *spec;

	ifi->ifi_family = AF_BRIDGE;
	if (v->pvid)
		info.flags |= BRIDGE_VLAN_INFO_PVID;
	if (v->untagged)
		info.flags |= BRIDGE_VLAN_INFO_UNTAGGED;
	spec = mnl_attr_nest_start(nlh, IFLA_AF_SPEC);
	if (v->self)
		mnl_attr_put_u16(nlh, IFLA_BRIDGE_FLAGS, BRIDGE_FLAGS_SELF);
	mnl_attr_put(nlh, IFLA_BRIDGE_VLAN_INFO, sizeof(info), &info);
	mnl_attr_nest_end(nlh, spec);
	return nlh;
}
