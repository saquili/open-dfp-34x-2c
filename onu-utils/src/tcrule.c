// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The attributes are those tc puts (iproute2 6.14): a flower filter always
 * names its flags, 0 when it has none, a matchall filter only flags that
 * are set, and the filter's one action is the first of its list.
 */

#include "tcrule.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <libmnl/libmnl.h>
#include <linux/if_ether.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_gact.h>
#include <linux/tc_act/tc_mirred.h>

#define TC_INGRESS TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS)

static struct nlmsghdr *tc_header(char *buf, uint16_t type, uint16_t flags,
				  uint32_t seq, int ifindex, uint32_t parent,
				  uint32_t info)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct tcmsg *tcm;

	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST | flags;
	nlh->nlmsg_seq = seq;
	tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
	tcm->tcm_family = AF_UNSPEC;
	tcm->tcm_ifindex = ifindex;
	tcm->tcm_parent = parent;
	tcm->tcm_info = info;
	return nlh;
}

struct nlmsghdr *tc_clsact_msg(char *buf, int ifindex, bool add, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct tcmsg *tcm;

	nlh = tc_header(buf, add ? RTM_NEWQDISC : RTM_DELQDISC,
			add ? NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL : NLM_F_ACK,
			seq, ifindex, TC_H_CLSACT, 0);
	tcm = mnl_nlmsg_get_payload(nlh);
	tcm->tcm_handle = TC_H_MAKE(TC_H_CLSACT, 0);
	mnl_attr_put_strz(nlh, TCA_KIND, "clsact");
	return nlh;
}

/* The filter's one action, the first of the list @list. */
static void put_action(struct nlmsghdr *nlh, uint16_t list,
		       const struct tc_rule *r)
{
	struct nlattr *acts, *act, *opts;

	acts = mnl_attr_nest_start(nlh, list);
	act = mnl_attr_nest_start(nlh, 1);
	if (r->action == TC_RULE_REDIRECT) {
		struct tc_mirred m = {
			.action = TC_ACT_STOLEN,
			.eaction = TCA_INGRESS_REDIR,
			.ifindex = (uint32_t)r->to_ifindex,
		};

		mnl_attr_put_strz(nlh, TCA_ACT_KIND, "mirred");
		opts = mnl_attr_nest_start(nlh, TCA_ACT_OPTIONS);
		mnl_attr_put(nlh, TCA_MIRRED_PARMS, sizeof(m), &m);
	} else {
		struct tc_gact g = {
			.action = r->action == TC_RULE_TRAP ? TC_ACT_TRAP :
							      TC_ACT_SHOT,
		};

		mnl_attr_put_strz(nlh, TCA_ACT_KIND, "gact");
		opts = mnl_attr_nest_start(nlh, TCA_ACT_OPTIONS);
		mnl_attr_put(nlh, TCA_GACT_PARMS, sizeof(g), &g);
	}
	mnl_attr_nest_end(nlh, opts);
	mnl_attr_nest_end(nlh, act);
	mnl_attr_nest_end(nlh, acts);
}

struct nlmsghdr *tc_rule_msg(char *buf, const struct tc_rule *r, uint32_t seq)
{
	const uint16_t proto = r->has_vlan ? ETH_P_8021Q :
			       r->has_dst ? ETH_P_IP : ETH_P_ALL;
	const uint32_t flags = r->hw ? TCA_CLS_FLAGS_SKIP_SW : 0;
	const bool flower = r->has_vlan || r->has_dst;
	struct nlmsghdr *nlh;
	struct nlattr *opts;

	nlh = tc_header(buf, RTM_NEWTFILTER,
			NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL, seq, r->ifindex,
			TC_INGRESS,
			TC_H_MAKE((uint32_t)r->pref << 16, htons(proto)));
	mnl_attr_put_strz(nlh, TCA_KIND, flower ? "flower" : "matchall");
	opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);
	if (flower) {
		mnl_attr_put_u16(nlh, TCA_FLOWER_KEY_ETH_TYPE, htons(proto));
		if (r->has_vlan) {
			mnl_attr_put_u16(nlh, TCA_FLOWER_KEY_VLAN_ID, r->vlan);
		} else {
			uint32_t mask = r->dst_len ?
				htonl(~0u << (32 - r->dst_len)) : 0;

			mnl_attr_put_u32(nlh, TCA_FLOWER_KEY_IPV4_DST, r->dst & mask);
			mnl_attr_put_u32(nlh, TCA_FLOWER_KEY_IPV4_DST_MASK, mask);
		}
		mnl_attr_put_u32(nlh, TCA_FLOWER_FLAGS, flags);
		put_action(nlh, TCA_FLOWER_ACT, r);
	} else {
		if (flags)
			mnl_attr_put_u32(nlh, TCA_MATCHALL_FLAGS, flags);
		put_action(nlh, TCA_MATCHALL_ACT, r);
	}
	mnl_attr_nest_end(nlh, opts);
	return nlh;
}

struct nlmsghdr *tc_rule_del_msg(char *buf, int ifindex, uint16_t pref,
				 uint32_t seq)
{
	return tc_header(buf, RTM_DELTFILTER, NLM_F_ACK, seq, ifindex,
			 TC_INGRESS, TC_H_MAKE((uint32_t)pref << 16, 0));
}

struct nlmsghdr *tc_rule_dump_msg(char *buf, int ifindex, uint32_t seq)
{
	return tc_header(buf, RTM_GETTFILTER, NLM_F_DUMP, seq, ifindex,
			 TC_INGRESS, 0);
}

unsigned int tc_rule_pref(const struct nlmsghdr *nlh)
{
	const struct tcmsg *tcm;

	if (nlh->nlmsg_type != RTM_NEWTFILTER ||
	    nlh->nlmsg_len < mnl_nlmsg_size(sizeof(*tcm)))
		return 0;
	tcm = mnl_nlmsg_get_payload(nlh);
	return TC_H_MAJ(tcm->tcm_info) >> 16;
}
