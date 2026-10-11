// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the traffic-control requests: each is built, then read back
 * attribute by attribute as the kernel reads it.
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/if_ether.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_gact.h>
#include <linux/tc_act/tc_mirred.h>

#include "tcrule.h"

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

#define INGRESS 0xfffffff2u

static char buf[8192];

static const struct tcmsg *tcm_of(const struct nlmsghdr *nlh)
{
	return mnl_nlmsg_get_payload(nlh);
}

/* The attribute @type of a message, after its tcmsg, or NULL. */
static const struct nlattr *top(const struct nlmsghdr *nlh, uint16_t type)
{
	const struct nlattr *a;

	mnl_attr_for_each(a, nlh, sizeof(struct tcmsg))
		if (mnl_attr_get_type(a) == type)
			return a;
	return NULL;
}

/* The attribute @type in the nest @nest, or NULL. */
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

static int count(const struct nlattr *nest)
{
	const struct nlattr *a;
	int n = 0;

	mnl_attr_for_each_nested(a, nest)
		n++;
	return n;
}

static bool is_str(const struct nlattr *a, const char *s)
{
	return a && mnl_attr_validate(a, MNL_TYPE_NUL_STRING) == 0 &&
	       !strcmp(mnl_attr_get_str(a), s);
}

/* The one action of the list @list: its kind and options. */
static const struct nlattr *action(const struct nlattr *list, const char *kind)
{
	const struct nlattr *first = in(list, 1);

	CHECK(list && count(list) == 1);
	CHECK(first && mnl_attr_get_type(first) == 1);
	CHECK(is_str(in(first, TCA_ACT_KIND), kind));
	return in(first, TCA_ACT_OPTIONS);
}

static uint32_t gact(const struct nlattr *opts)
{
	const struct nlattr *a = in(opts, TCA_GACT_PARMS);
	struct tc_gact g;

	CHECK(a && mnl_attr_get_payload_len(a) == sizeof(g));
	if (!a)
		return 0;
	memcpy(&g, mnl_attr_get_payload(a), sizeof(g));
	CHECK(g.index == 0 && g.capab == 0);
	return (uint32_t)g.action;
}

static void test_clsact(void)
{
	struct nlmsghdr *nlh = tc_clsact_msg(buf, 7, true, 11);

	CHECK(nlh->nlmsg_type == RTM_NEWQDISC);
	CHECK(nlh->nlmsg_flags ==
	      (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL));
	CHECK(nlh->nlmsg_seq == 11);
	CHECK(tcm_of(nlh)->tcm_family == AF_UNSPEC);
	CHECK(tcm_of(nlh)->tcm_ifindex == 7);
	CHECK(tcm_of(nlh)->tcm_parent == TC_H_CLSACT);
	CHECK(tcm_of(nlh)->tcm_handle == 0xffff0000u);
	CHECK(tcm_of(nlh)->tcm_info == 0);
	CHECK(is_str(top(nlh, TCA_KIND), "clsact"));
	CHECK(!top(nlh, TCA_OPTIONS));

	nlh = tc_clsact_msg(buf, 7, false, 12);
	CHECK(nlh->nlmsg_type == RTM_DELQDISC);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	CHECK(tcm_of(nlh)->tcm_parent == TC_H_CLSACT);
	CHECK(tcm_of(nlh)->tcm_handle == 0xffff0000u);
	CHECK(is_str(top(nlh, TCA_KIND), "clsact"));
}

/* tc filter add dev 3 ingress pref 1 protocol 802.1Q flower skip_sw
 * vlan_id 410 action trap: the boot's rule of the management VLAN. */
static void test_vlan_trap_in_the_switch(void)
{
	const struct tc_rule r = {
		.ifindex = 3, .pref = 1, .has_vlan = true, .vlan = 410,
		.hw = true, .action = TC_RULE_TRAP,
	};
	struct nlmsghdr *nlh = tc_rule_msg(buf, &r, 21);
	const struct nlattr *opts = top(nlh, TCA_OPTIONS), *a;

	CHECK(nlh->nlmsg_type == RTM_NEWTFILTER);
	CHECK(nlh->nlmsg_flags ==
	      (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL));
	CHECK(tcm_of(nlh)->tcm_ifindex == 3);
	CHECK(tcm_of(nlh)->tcm_parent == INGRESS);
	CHECK(tcm_of(nlh)->tcm_handle == 0);
	CHECK(tcm_of(nlh)->tcm_info == ((1u << 16) | htons(ETH_P_8021Q)));
	CHECK(is_str(top(nlh, TCA_KIND), "flower"));
	CHECK(opts && (opts->nla_type & NLA_F_NESTED));
	CHECK(opts && count(opts) == 4);
	a = in(opts, TCA_FLOWER_KEY_ETH_TYPE);
	CHECK(a && mnl_attr_get_payload_len(a) == 2 &&
	      mnl_attr_get_u16(a) == htons(ETH_P_8021Q));
	a = in(opts, TCA_FLOWER_KEY_VLAN_ID);
	CHECK(a && mnl_attr_get_payload_len(a) == 2 && mnl_attr_get_u16(a) == 410);
	a = in(opts, TCA_FLOWER_FLAGS);
	CHECK(a && mnl_attr_get_u32(a) == TCA_CLS_FLAGS_SKIP_SW);
	CHECK(gact(action(in(opts, TCA_FLOWER_ACT), "gact")) == TC_ACT_TRAP);
}

/* ... and on pon, with action drop; one without skip_sw names flags 0. */
static void test_vlan_drop(void)
{
	struct tc_rule r = {
		.ifindex = 4, .pref = 1, .has_vlan = true, .vlan = 4095,
		.hw = true, .action = TC_RULE_DROP,
	};
	struct nlmsghdr *nlh = tc_rule_msg(buf, &r, 22);
	const struct nlattr *opts = top(nlh, TCA_OPTIONS);

	CHECK(mnl_attr_get_u16(in(opts, TCA_FLOWER_KEY_VLAN_ID)) == 4095);
	CHECK(gact(action(in(opts, TCA_FLOWER_ACT), "gact")) == TC_ACT_SHOT);

	r.hw = false;
	r.vlan = 0;
	nlh = tc_rule_msg(buf, &r, 23);
	opts = top(nlh, TCA_OPTIONS);
	CHECK(in(opts, TCA_FLOWER_FLAGS) &&
	      mnl_attr_get_u32(in(opts, TCA_FLOWER_FLAGS)) == 0);
	CHECK(in(opts, TCA_FLOWER_KEY_VLAN_ID) &&
	      mnl_attr_get_u16(in(opts, TCA_FLOWER_KEY_VLAN_ID)) == 0);
}

/* tc filter add dev 6 ingress pref 2 protocol ip flower dst_ip
 * 192.0.2.10/32 action drop: a session's packets to an address of the
 * stick's host side; a prefix keeps only its own bits. */
static void test_ipv4_destination_dropped(void)
{
	struct tc_rule r = {
		.ifindex = 6, .pref = 2, .has_dst = true,
		.dst = htonl(0xc000020a), .dst_len = 32, .action = TC_RULE_DROP,
	};
	struct nlmsghdr *nlh = tc_rule_msg(buf, &r, 25);
	const struct nlattr *opts = top(nlh, TCA_OPTIONS), *a;

	CHECK(tcm_of(nlh)->tcm_info == ((2u << 16) | htons(ETH_P_IP)));
	CHECK(is_str(top(nlh, TCA_KIND), "flower"));
	CHECK(opts && count(opts) == 5);
	a = in(opts, TCA_FLOWER_KEY_ETH_TYPE);
	CHECK(a && mnl_attr_get_u16(a) == htons(ETH_P_IP));
	CHECK(!in(opts, TCA_FLOWER_KEY_VLAN_ID));
	a = in(opts, TCA_FLOWER_KEY_IPV4_DST);
	CHECK(a && mnl_attr_get_payload_len(a) == 4 &&
	      mnl_attr_get_u32(a) == htonl(0xc000020a));
	a = in(opts, TCA_FLOWER_KEY_IPV4_DST_MASK);
	CHECK(a && mnl_attr_get_u32(a) == 0xffffffffu);
	CHECK(gact(action(in(opts, TCA_FLOWER_ACT), "gact")) == TC_ACT_SHOT);

	r.dst_len = 24;
	nlh = tc_rule_msg(buf, &r, 26);
	opts = top(nlh, TCA_OPTIONS);
	CHECK(mnl_attr_get_u32(in(opts, TCA_FLOWER_KEY_IPV4_DST)) == htonl(0xc0000200));
	CHECK(mnl_attr_get_u32(in(opts, TCA_FLOWER_KEY_IPV4_DST_MASK)) == htonl(0xffffff00));
	r.dst_len = 0;
	nlh = tc_rule_msg(buf, &r, 27);
	opts = top(nlh, TCA_OPTIONS);
	CHECK(mnl_attr_get_u32(in(opts, TCA_FLOWER_KEY_IPV4_DST_MASK)) == 0);
}

/* tc filter add dev 5 ingress matchall action mirred ingress redirect
 * dev 9: a feed of onu-apply, with the priority the kernel picks. */
static void test_everything_redirected(void)
{
	const struct tc_rule r = {
		.ifindex = 5, .action = TC_RULE_REDIRECT, .to_ifindex = 9,
	};
	struct nlmsghdr *nlh = tc_rule_msg(buf, &r, 24);
	const struct nlattr *opts = top(nlh, TCA_OPTIONS), *a;
	struct tc_mirred m;

	CHECK(tcm_of(nlh)->tcm_info == htons(ETH_P_ALL));
	CHECK(is_str(top(nlh, TCA_KIND), "matchall"));
	CHECK(opts && count(opts) == 1);
	CHECK(!in(opts, TCA_MATCHALL_FLAGS));
	a = in(action(in(opts, TCA_MATCHALL_ACT), "mirred"), TCA_MIRRED_PARMS);
	CHECK(a && mnl_attr_get_payload_len(a) == sizeof(m));
	if (!a)
		return;
	memcpy(&m, mnl_attr_get_payload(a), sizeof(m));
	CHECK(m.action == TC_ACT_STOLEN);
	CHECK(m.eaction == TCA_INGRESS_REDIR);
	CHECK(m.ifindex == 9);
	CHECK(m.index == 0);
}

/* A VLAN redirected, and every frame trapped in the switch. */
static void test_more_kinds(void)
{
	struct tc_rule r = {
		.ifindex = 5, .pref = 1000, .has_vlan = true, .vlan = 7,
		.action = TC_RULE_REDIRECT, .to_ifindex = 6,
	};
	struct nlmsghdr *nlh = tc_rule_msg(buf, &r, 25);
	const struct nlattr *opts = top(nlh, TCA_OPTIONS), *a;
	struct tc_mirred m;

	CHECK(tcm_of(nlh)->tcm_info == ((1000u << 16) | htons(ETH_P_8021Q)));
	a = in(action(in(opts, TCA_FLOWER_ACT), "mirred"), TCA_MIRRED_PARMS);
	CHECK(a != NULL);
	if (a) {
		memcpy(&m, mnl_attr_get_payload(a), sizeof(m));
		CHECK(m.ifindex == 6 && m.eaction == TCA_INGRESS_REDIR);
	}

	r = (struct tc_rule){ .ifindex = 5, .pref = 65535, .hw = true,
			      .action = TC_RULE_TRAP };
	nlh = tc_rule_msg(buf, &r, 26);
	opts = top(nlh, TCA_OPTIONS);
	CHECK(tcm_of(nlh)->tcm_info == ((65535u << 16) | htons(ETH_P_ALL)));
	CHECK(in(opts, TCA_MATCHALL_FLAGS) &&
	      mnl_attr_get_u32(in(opts, TCA_MATCHALL_FLAGS)) == TCA_CLS_FLAGS_SKIP_SW);
	CHECK(gact(action(in(opts, TCA_MATCHALL_ACT), "gact")) == TC_ACT_TRAP);
}

static void test_del_and_dump(void)
{
	struct nlmsghdr *nlh = tc_rule_del_msg(buf, 8, 1000, 31);

	CHECK(nlh->nlmsg_type == RTM_DELTFILTER);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	CHECK(nlh->nlmsg_len == mnl_nlmsg_size(sizeof(struct tcmsg)));
	CHECK(tcm_of(nlh)->tcm_ifindex == 8);
	CHECK(tcm_of(nlh)->tcm_parent == INGRESS);
	CHECK(tcm_of(nlh)->tcm_info == 1000u << 16);

	nlh = tc_rule_dump_msg(buf, 8, 32);
	CHECK(nlh->nlmsg_type == RTM_GETTFILTER);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_DUMP));
	CHECK(nlh->nlmsg_len == mnl_nlmsg_size(sizeof(struct tcmsg)));
	CHECK(tcm_of(nlh)->tcm_parent == INGRESS);
	CHECK(tcm_of(nlh)->tcm_info == 0);
}

static void test_pref_of_a_dump_message(void)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct tcmsg *tcm;

	nlh->nlmsg_type = RTM_NEWTFILTER;
	tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
	tcm->tcm_info = (1000u << 16) | htons(ETH_P_8021Q);
	CHECK(tc_rule_pref(nlh) == 1000);
	tcm->tcm_info = (49152u << 16) | htons(ETH_P_ALL);
	CHECK(tc_rule_pref(nlh) == 49152);
	nlh->nlmsg_type = RTM_NEWQDISC;
	CHECK(tc_rule_pref(nlh) == 0);
	nlh->nlmsg_type = RTM_NEWTFILTER;
	nlh->nlmsg_len = mnl_nlmsg_size(sizeof(*tcm) - 1);
	CHECK(tc_rule_pref(nlh) == 0);
}

int main(void)
{
	test_clsact();
	test_vlan_trap_in_the_switch();
	test_vlan_drop();
	test_ipv4_destination_dropped();
	test_everything_redirected();
	test_more_kinds();
	test_del_and_dump();
	test_pref_of_a_dump_message();
	printf("test_tcrule: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
