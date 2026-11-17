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

int main(void)
{
	test_up_and_down();
	test_mtu_and_address();
	test_vlan_add();
	test_del();
	printf("test_linkmsg: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
