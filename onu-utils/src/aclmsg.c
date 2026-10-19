// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The attributes are those tc puts for the same rule (iproute2 6.15): the
 * EtherType, the PPP protocol of a session, each address and port with its
 * mask, the flags, 0 when there are none, and the one action, gact, the first
 * of the list.
 */

#include "aclmsg.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <libmnl/libmnl.h>
#include <linux/if_ether.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_gact.h>

#define TC_INGRESS TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS)
/* The PPP protocol of IPv4 (RFC 1332). */
#define PPP_IPV4 0x0021

static void put_port(struct nlmsghdr *nlh, uint16_t key, uint16_t mask_key, uint16_t v,
		     uint16_t m)
{
	mnl_attr_put_u16(nlh, key, htons(v));
	mnl_attr_put_u16(nlh, mask_key, htons(m));
}

struct nlmsghdr *acl_entry_msg(char *buf, int ifindex, const struct acl_entry *e, bool hw,
			       uint32_t seq)
{
	const uint16_t proto = e->pppoe ? ETH_P_PPP_SES : ETH_P_IP;
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct tc_gact g = { .action = e->pass ? TC_ACT_OK : TC_ACT_SHOT };
	struct nlattr *opts, *acts, *act, *aopts;
	struct tcmsg *tcm;

	nlh->nlmsg_type = RTM_NEWTFILTER;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	nlh->nlmsg_seq = seq;
	tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
	tcm->tcm_family = AF_UNSPEC;
	tcm->tcm_ifindex = ifindex;
	tcm->tcm_parent = TC_INGRESS;
	tcm->tcm_info = TC_H_MAKE((uint32_t)e->pref << 16, htons(proto));
	mnl_attr_put_strz(nlh, TCA_KIND, "flower");
	opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);
	mnl_attr_put_u16(nlh, TCA_FLOWER_KEY_ETH_TYPE, htons(proto));
	if (e->pppoe)
		mnl_attr_put_u16(nlh, TCA_FLOWER_KEY_PPP_PROTO, htons(PPP_IPV4));
	if (e->smac) {
		static const uint8_t all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

		mnl_attr_put(nlh, TCA_FLOWER_KEY_ETH_SRC, 6, e->mac);
		mnl_attr_put(nlh, TCA_FLOWER_KEY_ETH_SRC_MASK, 6, all);
	}
	if (e->proto)
		mnl_attr_put_u8(nlh, TCA_FLOWER_KEY_IP_PROTO, e->proto);
	if (e->smask) {
		mnl_attr_put_u32(nlh, TCA_FLOWER_KEY_IPV4_SRC, htonl(e->sip & e->smask));
		mnl_attr_put_u32(nlh, TCA_FLOWER_KEY_IPV4_SRC_MASK, htonl(e->smask));
	}
	if (e->dmask) {
		mnl_attr_put_u32(nlh, TCA_FLOWER_KEY_IPV4_DST, htonl(e->dip & e->dmask));
		mnl_attr_put_u32(nlh, TCA_FLOWER_KEY_IPV4_DST_MASK, htonl(e->dmask));
	}
	if (e->proto == IPPROTO_TCP) {
		if (e->spmask)
			put_port(nlh, TCA_FLOWER_KEY_TCP_SRC, TCA_FLOWER_KEY_TCP_SRC_MASK, e->sport,
				 e->spmask);
		if (e->dpmask)
			put_port(nlh, TCA_FLOWER_KEY_TCP_DST, TCA_FLOWER_KEY_TCP_DST_MASK, e->dport,
				 e->dpmask);
	} else if (e->proto == IPPROTO_UDP) {
		if (e->spmask)
			put_port(nlh, TCA_FLOWER_KEY_UDP_SRC, TCA_FLOWER_KEY_UDP_SRC_MASK, e->sport,
				 e->spmask);
		if (e->dpmask)
			put_port(nlh, TCA_FLOWER_KEY_UDP_DST, TCA_FLOWER_KEY_UDP_DST_MASK, e->dport,
				 e->dpmask);
	} else if (e->proto == IPPROTO_ICMP && e->icmp_type >= 0) {
		mnl_attr_put_u8(nlh, TCA_FLOWER_KEY_ICMPV4_TYPE, (uint8_t)e->icmp_type);
		mnl_attr_put_u8(nlh, TCA_FLOWER_KEY_ICMPV4_TYPE_MASK, 0xff);
	}
	mnl_attr_put_u32(nlh, TCA_FLOWER_FLAGS, hw ? TCA_CLS_FLAGS_SKIP_SW : 0);
	acts = mnl_attr_nest_start(nlh, TCA_FLOWER_ACT);
	act = mnl_attr_nest_start(nlh, 1);
	mnl_attr_put_strz(nlh, TCA_ACT_KIND, "gact");
	aopts = mnl_attr_nest_start(nlh, TCA_ACT_OPTIONS);
	mnl_attr_put(nlh, TCA_GACT_PARMS, sizeof(g), &g);
	mnl_attr_nest_end(nlh, aopts);
	mnl_attr_nest_end(nlh, act);
	mnl_attr_nest_end(nlh, acts);
	mnl_attr_nest_end(nlh, opts);
	return nlh;
}
