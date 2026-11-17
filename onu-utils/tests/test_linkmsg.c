// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the link requests: each is built, then read back attribute by
 * attribute as the kernel reads it.
 */

#include <stdio.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/if.h>
#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

#include "linkmsg.h"

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static char buf[8192];

static const struct ifinfomsg *ifi_of(const struct nlmsghdr *nlh)
{
	return mnl_nlmsg_get_payload(nlh);
}

/* The attribute @type of a message, after its ifinfomsg, or NULL. */
static const struct nlattr *top(const struct nlmsghdr *nlh, uint16_t type)
{
	const struct nlattr *a;

	mnl_attr_for_each(a, nlh, sizeof(struct ifinfomsg))
		if (mnl_attr_get_type(a) == type)
			return a;
	return NULL;
}

static const struct nlattr *in(const struct nlattr *nest, uint16_t type)
{
	const struct nlattr *a;

	if (!nest)
		return NULL;
	mnl_attr_for_each_nested(a, nest)
		if (mnl_attr_get_type(a) == type)
			return a;
	return NULL;
}

static int count_top(const struct nlmsghdr *nlh)
{
	const struct nlattr *a;
	int n = 0;

	mnl_attr_for_each(a, nlh, sizeof(struct ifinfomsg))
		n++;
	return n;
}

/* ip link set pon up */
static void test_up_and_down(void)
{
	struct link_set s = { .ifindex = 4, .has_state = true, .up = true };
	struct nlmsghdr *nlh = link_set_msg(buf, &s, 21);

	CHECK(nlh->nlmsg_type == RTM_NEWLINK);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	CHECK(nlh->nlmsg_seq == 21);
	CHECK(ifi_of(nlh)->ifi_family == AF_UNSPEC);
	CHECK(ifi_of(nlh)->ifi_index == 4);
	CHECK(ifi_of(nlh)->ifi_change == IFF_UP && ifi_of(nlh)->ifi_flags == IFF_UP);
	CHECK(count_top(nlh) == 0);

	s.up = false;
	nlh = link_set_msg(buf, &s, 22);
	CHECK(ifi_of(nlh)->ifi_change == IFF_UP && ifi_of(nlh)->ifi_flags == 0);
}

/* ip link set wan410 mtu 1508 address 00:00:5e:00:53:01: no flag is changed. */
static void test_mtu_and_address(void)
{
	static const uint8_t mac[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 };
	struct link_set s = { .ifindex = 9, .has_mtu = true, .mtu = 1508, .has_mac = true };
	struct nlmsghdr *nlh;
	const struct nlattr *a;

	memcpy(s.mac, mac, sizeof(mac));
	nlh = link_set_msg(buf, &s, 23);
	CHECK(ifi_of(nlh)->ifi_index == 9);
	CHECK(ifi_of(nlh)->ifi_change == 0 && ifi_of(nlh)->ifi_flags == 0);
	a = top(nlh, IFLA_MTU);
	CHECK(a && mnl_attr_validate(a, MNL_TYPE_U32) == 0 && mnl_attr_get_u32(a) == 1508);
	a = top(nlh, IFLA_ADDRESS);
	CHECK(a && mnl_attr_get_payload_len(a) == 6 &&
	      !memcmp(mnl_attr_get_payload(a), mac, 6));
	CHECK(count_top(nlh) == 2);

	/* What is not named is not in the request. */
	s.has_mtu = false;
	s.has_mac = false;
	nlh = link_set_msg(buf, &s, 24);
	CHECK(count_top(nlh) == 0);
}

/* ip link add link gem256 name wan410 type vlan id 410 */
static void test_vlan_add(void)
{
	struct nlmsghdr *nlh = link_vlan_add_msg(buf, 6, "wan410", 410, 25);
	const struct nlattr *info, *data, *a;

	CHECK(nlh->nlmsg_type == RTM_NEWLINK);
	CHECK(nlh->nlmsg_flags ==
	      (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL));
	CHECK(ifi_of(nlh)->ifi_family == AF_UNSPEC && ifi_of(nlh)->ifi_index == 0);
	a = top(nlh, IFLA_LINK);
	CHECK(a && mnl_attr_get_u32(a) == 6);
	a = top(nlh, IFLA_IFNAME);
	CHECK(a && mnl_attr_validate(a, MNL_TYPE_NUL_STRING) == 0 &&
	      !strcmp(mnl_attr_get_str(a), "wan410"));
	info = top(nlh, IFLA_LINKINFO);
	CHECK(info != NULL);
	a = in(info, IFLA_INFO_KIND);
	CHECK(a && !strcmp(mnl_attr_get_str(a), "vlan"));
	data = in(info, IFLA_INFO_DATA);
	CHECK(data != NULL);
	a = in(data, IFLA_VLAN_ID);
	CHECK(a && mnl_attr_validate(a, MNL_TYPE_U16) == 0 && mnl_attr_get_u16(a) == 410);
	CHECK(count_top(nlh) == 3);
}

/* ip link del wan410 */
static void test_del(void)
{
	struct nlmsghdr *nlh = link_del_msg(buf, 12, 26);

	CHECK(nlh->nlmsg_type == RTM_DELLINK);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	CHECK(nlh->nlmsg_seq == 26);
	CHECK(ifi_of(nlh)->ifi_index == 12 && ifi_of(nlh)->ifi_family == AF_UNSPEC);
	CHECK(count_top(nlh) == 0);
}

/* ip link set sfp master omcibr0 */
static void test_master(void)
{
	struct link_set s = { .ifindex = 3, .master = 7 };
	struct nlmsghdr *nlh = link_set_msg(buf, &s, 27);
	const struct nlattr *a;

	CHECK(nlh->nlmsg_type == RTM_NEWLINK);
	CHECK(ifi_of(nlh)->ifi_index == 3 && ifi_of(nlh)->ifi_change == 0);
	a = top(nlh, IFLA_MASTER);
	CHECK(a && mnl_attr_validate(a, MNL_TYPE_U32) == 0 && mnl_attr_get_u32(a) == 7);
	CHECK(count_top(nlh) == 1);
}

/* ip link add name omcibr0 type bridge vlan_filtering 1 vlan_default_pvid 0 */
static void test_bridge_add(void)
{
	struct nlmsghdr *nlh = link_bridge_add_msg(buf, "omcibr0", 28);
	const struct nlattr *info, *data, *a;

	CHECK(nlh->nlmsg_type == RTM_NEWLINK);
	CHECK(nlh->nlmsg_flags ==
	      (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL));
	CHECK(ifi_of(nlh)->ifi_family == AF_UNSPEC && ifi_of(nlh)->ifi_index == 0);
	a = top(nlh, IFLA_IFNAME);
	CHECK(a && !strcmp(mnl_attr_get_str(a), "omcibr0"));
	info = top(nlh, IFLA_LINKINFO);
	a = in(info, IFLA_INFO_KIND);
	CHECK(a && !strcmp(mnl_attr_get_str(a), "bridge"));
	data = in(info, IFLA_INFO_DATA);
	a = in(data, IFLA_BR_VLAN_FILTERING);
	CHECK(a && mnl_attr_validate(a, MNL_TYPE_U8) == 0 && mnl_attr_get_u8(a) == 1);
	a = in(data, IFLA_BR_VLAN_DEFAULT_PVID);
	CHECK(a && mnl_attr_validate(a, MNL_TYPE_U16) == 0 && mnl_attr_get_u16(a) == 0);
	CHECK(count_top(nlh) == 2);
}

/* The VLAN information of a bridge VLAN request, and its flags attribute. */
static const struct bridge_vlan_info *vinfo(const struct nlmsghdr *nlh,
					    const struct nlattr **flags)
{
	const struct nlattr *spec = top(nlh, IFLA_AF_SPEC), *a;

	*flags = in(spec, IFLA_BRIDGE_FLAGS);
	a = in(spec, IFLA_BRIDGE_VLAN_INFO);
	if (!a || mnl_attr_get_payload_len(a) != sizeof(struct bridge_vlan_info))
		return NULL;
	return mnl_attr_get_payload(a);
}

/* bridge vlan add dev sfp vid 4094 pvid untagged; ... del; ... add dev omcibr0 vid 4094 self */
static void test_bridge_vlan(void)
{
	struct bridge_vlan v = { .ifindex = 3, .vid = 4094, .pvid = true, .untagged = true };
	const struct bridge_vlan_info *i;
	const struct nlattr *flags;
	struct nlmsghdr *nlh;

	nlh = bridge_vlan_msg(buf, &v, true, 29);
	CHECK(nlh->nlmsg_type == RTM_SETLINK);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	CHECK(ifi_of(nlh)->ifi_family == AF_BRIDGE && ifi_of(nlh)->ifi_index == 3);
	i = vinfo(nlh, &flags);
	CHECK(i && i->vid == 4094 &&
	      i->flags == (BRIDGE_VLAN_INFO_PVID | BRIDGE_VLAN_INFO_UNTAGGED));
	CHECK(flags == NULL);
	CHECK(count_top(nlh) == 1);

	v.pvid = v.untagged = false;
	nlh = bridge_vlan_msg(buf, &v, false, 30);
	CHECK(nlh->nlmsg_type == RTM_DELLINK);
	i = vinfo(nlh, &flags);
	CHECK(i && i->vid == 4094 && i->flags == 0 && flags == NULL);

	v.ifindex = 7;
	v.self = true;
	nlh = bridge_vlan_msg(buf, &v, true, 31);
	CHECK(ifi_of(nlh)->ifi_index == 7);
	i = vinfo(nlh, &flags);
	CHECK(i && i->vid == 4094 && i->flags == 0);
	CHECK(flags && mnl_attr_validate(flags, MNL_TYPE_U16) == 0 &&
	      mnl_attr_get_u16(flags) == BRIDGE_FLAGS_SELF);
}

int main(void)
{
	test_up_and_down();
	test_mtu_and_address();
	test_vlan_add();
	test_del();
	test_master();
	test_bridge_add();
	test_bridge_vlan();
	printf("test_linkmsg: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
