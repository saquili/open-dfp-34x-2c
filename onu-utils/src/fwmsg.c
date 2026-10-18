// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The stick's own table of nf_tables, as a batch of requests (fwmsg.h). */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>
#include <netinet/in.h>

#include "fwmsg.h"

#define TABLE "onu"
#define FLOWTABLE "ft"
#define FORWARD "forward"
#define POSTROUTING "postrouting"

/* The interfaces of the PPPoE sessions, the networks' way out. */
#define WAN_PREFIX "ppp"

/* The priority of the source NAT, nft's "srcnat". */
#define SRCNAT_PRIORITY 100

/* The kind of TCP's option of the maximum segment size (RFC 9293). */
#define TCP_OPT_MSS 2

void fw_spec_init(struct fw_spec *s)
{
	memset(s, 0, sizeof(*s));
	s->offload = true;
}

static bool ifname_ok(const char *v)
{
	size_t n = strlen(v);

	return n && n < FW_IFNAMSIZ &&
	       strspn(v, "abcdefghijklmnopqrstuvwxyz"
			 "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == n;
}

/* a.b.c.d/len, the first address of a network: false for anything else. */
static bool net_of(const char *v, uint32_t *net, uint8_t *len)
{
	char a[16];
	const char *slash = strchr(v, '/');
	struct in_addr in;
	char *end;
	unsigned long l;

	if (!slash || (size_t)(slash - v) >= sizeof(a))
		return false;
	memcpy(a, v, (size_t)(slash - v));
	a[slash - v] = '\0';
	l = strtoul(slash + 1, &end, 10);
	if (inet_pton(AF_INET, a, &in) != 1 || end == slash + 1 || *end ||
	    l < 8 || l > 32)
		return false;
	*net = ntohl(in.s_addr);
	*len = (uint8_t)l;
	return !(*net & ~(l == 32 ? ~0u : ~(~0u >> l)));
}

int fw_spec_line(struct fw_spec *s, const char *line, char *err, size_t errcap)
{
	char word[16], a[64], b[64];
	int n;

	if (!*line || *line == '#')
		return 0;
	n = sscanf(line, "%15s %63s %63s", word, a, b);
	if (n == 3 && !strcmp(word, "lan")) {
		struct fw_lan *l = &s->lan[s->nlans];

		if (s->nlans == FW_LANS_MAX || !ifname_ok(a) ||
		    !net_of(b, &l->net, &l->len)) {
			snprintf(err, errcap, "lan %s %s: no network of the table", a, b);
			return -1;
		}
		/* ifname_ok() took no more than the field holds. */
		memcpy(l->ifname, a, strlen(a) + 1);
		s->nlans++;
		return 0;
	}
	if (n == 2 && !strcmp(word, "wan")) {
		if (s->nwans == FW_WANS_MAX || !ifname_ok(a)) {
			snprintf(err, errcap, "wan %s: no interface of the table", a);
			return -1;
		}
		/* Two sessions on one GEM interface give it once. */
		for (int i = 0; i < s->nwans; i++)
			if (!strcmp(s->wan[i], a))
				return 0;
		memcpy(s->wan[s->nwans++], a, strlen(a) + 1);
		return 0;
	}
	if (n == 2 && !strcmp(word, "offload") &&
	    (!strcmp(a, "on") || !strcmp(a, "off"))) {
		s->offload = !strcmp(a, "on");
		return 0;
	}
	snprintf(err, errcap, "%.40s: no line of the table", line);
	return -1;
}

/* The batch being made, and the number of its next request. */
struct batch {
	struct mnl_nlmsg_batch *b;
	uint32_t seq;
	bool full;
};

static struct nlmsghdr *request(struct batch *bt, uint16_t type, uint16_t flags)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(bt->b));
	struct nfgenmsg *nfg;

	nlh->nlmsg_type = NFNL_SUBSYS_NFTABLES << 8 | type;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
	nlh->nlmsg_seq = bt->seq++;
	nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
	nfg->nfgen_family = NFPROTO_INET;
	nfg->version = NFNETLINK_V0;
	nfg->res_id = 0;
	return nlh;
}

/* The batch's begin or end, which asks for no acknowledgement. */
static void edge(struct batch *bt, uint16_t type)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(bt->b));
	struct nfgenmsg *nfg;

	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = bt->seq++;
	nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
	nfg->nfgen_family = AF_UNSPEC;
	nfg->version = NFNETLINK_V0;
	nfg->res_id = htons(NFNL_SUBSYS_NFTABLES);
	if (!mnl_nlmsg_batch_next(bt->b))
		bt->full = true;
}

static void done(struct batch *bt)
{
	if (!mnl_nlmsg_batch_next(bt->b))
		bt->full = true;
}

/* An expression of a rule: its name, and its data nested in it. */
struct expr {
	struct nlattr *elem;
	struct nlattr *data;
};

static struct expr expr_begin(struct nlmsghdr *nlh, const char *name)
{
	struct expr e;

	e.elem = mnl_attr_nest_start(nlh, NFTA_LIST_ELEM);
	mnl_attr_put_strz(nlh, NFTA_EXPR_NAME, name);
	e.data = mnl_attr_nest_start(nlh, NFTA_EXPR_DATA);
	return e;
}

static void expr_end(struct nlmsghdr *nlh, struct expr e)
{
	mnl_attr_nest_end(nlh, e.data);
	mnl_attr_nest_end(nlh, e.elem);
}

/* A meta key into the first register. */
static void meta(struct nlmsghdr *nlh, uint32_t key)
{
	struct expr e = expr_begin(nlh, "meta");

	mnl_attr_put_u32(nlh, NFTA_META_KEY, htonl(key));
	mnl_attr_put_u32(nlh, NFTA_META_DREG, htonl(NFT_REG_1));
	expr_end(nlh, e);
}

/* The first register against @len bytes of @data. */
static void cmp(struct nlmsghdr *nlh, uint32_t op, const void *data, uint16_t len)
{
	struct expr e = expr_begin(nlh, "cmp");
	struct nlattr *d;

	mnl_attr_put_u32(nlh, NFTA_CMP_SREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_CMP_OP, htonl(op));
	d = mnl_attr_nest_start(nlh, NFTA_CMP_DATA);
	mnl_attr_put(nlh, NFTA_DATA_VALUE, len, data);
	mnl_attr_nest_end(nlh, d);
	expr_end(nlh, e);
}

/* @len bytes at @offset of the header @base into the first register. */
static void payload(struct nlmsghdr *nlh, uint32_t base, uint32_t offset, uint32_t len)
{
	struct expr e = expr_begin(nlh, "payload");

	mnl_attr_put_u32(nlh, NFTA_PAYLOAD_DREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_PAYLOAD_BASE, htonl(base));
	mnl_attr_put_u32(nlh, NFTA_PAYLOAD_OFFSET, htonl(offset));
	mnl_attr_put_u32(nlh, NFTA_PAYLOAD_LEN, htonl(len));
	expr_end(nlh, e);
}

/* The first register as itself AND @mask, of @len bytes. */
static void bitwise(struct nlmsghdr *nlh, const void *mask, uint16_t len)
{
	static const uint8_t zero[16];
	struct expr e = expr_begin(nlh, "bitwise");
	struct nlattr *d;

	mnl_attr_put_u32(nlh, NFTA_BITWISE_SREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_BITWISE_DREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_BITWISE_LEN, htonl(len));
	d = mnl_attr_nest_start(nlh, NFTA_BITWISE_MASK);
	mnl_attr_put(nlh, NFTA_DATA_VALUE, len, mask);
	mnl_attr_nest_end(nlh, d);
	d = mnl_attr_nest_start(nlh, NFTA_BITWISE_XOR);
	mnl_attr_put(nlh, NFTA_DATA_VALUE, len, zero);
	mnl_attr_nest_end(nlh, d);
	expr_end(nlh, e);
}

/* An interface's name, @key of meta, equal to @name, or to its prefix. */
static void ifname_is(struct nlmsghdr *nlh, uint32_t key, const char *name, bool prefix)
{
	char v[FW_IFNAMSIZ] = { 0 };

	memcpy(v, name, strlen(name));
	meta(nlh, key);
	cmp(nlh, NFT_CMP_EQ, v, prefix ? (uint16_t)strlen(name) : FW_IFNAMSIZ);
}

/* The connection's state, in the kernel's order, has one of @bits. */
static void ct_state_any(struct nlmsghdr *nlh, uint32_t bits)
{
	static const uint32_t none;
	struct expr e = expr_begin(nlh, "ct");

	mnl_attr_put_u32(nlh, NFTA_CT_KEY, htonl(NFT_CT_STATE));
	mnl_attr_put_u32(nlh, NFTA_CT_DREG, htonl(NFT_REG_1));
	expr_end(nlh, e);
	bitwise(nlh, &bits, sizeof(bits));
	cmp(nlh, NFT_CMP_NEQ, &none, sizeof(none));
}

static void verdict(struct nlmsghdr *nlh, int code)
{
	struct expr e = expr_begin(nlh, "immediate");
	struct nlattr *d, *v;

	mnl_attr_put_u32(nlh, NFTA_IMMEDIATE_DREG, htonl(NFT_REG_VERDICT));
	d = mnl_attr_nest_start(nlh, NFTA_IMMEDIATE_DATA);
	v = mnl_attr_nest_start(nlh, NFTA_DATA_VERDICT);
	mnl_attr_put_u32(nlh, NFTA_VERDICT_CODE, htonl((uint32_t)code));
	mnl_attr_nest_end(nlh, v);
	mnl_attr_nest_end(nlh, d);
	expr_end(nlh, e);
}

/* A rule of @chain, begun: its expressions follow, then rule_end(). */
static struct nlmsghdr *rule_begin(struct batch *bt, const char *chain,
				   struct nlattr **exprs)
{
	struct nlmsghdr *nlh = request(bt, NFT_MSG_NEWRULE, NLM_F_CREATE | NLM_F_APPEND);

	mnl_attr_put_strz(nlh, NFTA_RULE_TABLE, TABLE);
	mnl_attr_put_strz(nlh, NFTA_RULE_CHAIN, chain);
	*exprs = mnl_attr_nest_start(nlh, NFTA_RULE_EXPRESSIONS);
	return nlh;
}

static void rule_end(struct batch *bt, struct nlmsghdr *nlh, struct nlattr *exprs)
{
	mnl_attr_nest_end(nlh, exprs);
	done(bt);
}

static void chain(struct batch *bt, const char *name, const char *type,
		  uint32_t hook, int32_t priority)
{
	struct nlmsghdr *nlh = request(bt, NFT_MSG_NEWCHAIN, NLM_F_CREATE);
	struct nlattr *h;

	mnl_attr_put_strz(nlh, NFTA_CHAIN_TABLE, TABLE);
	mnl_attr_put_strz(nlh, NFTA_CHAIN_NAME, name);
	h = mnl_attr_nest_start(nlh, NFTA_CHAIN_HOOK);
	mnl_attr_put_u32(nlh, NFTA_HOOK_HOOKNUM, htonl(hook));
	mnl_attr_put_u32(nlh, NFTA_HOOK_PRIORITY, htonl((uint32_t)priority));
	mnl_attr_nest_end(nlh, h);
	mnl_attr_put_u32(nlh, NFTA_CHAIN_POLICY, htonl(NF_ACCEPT));
	mnl_attr_put_strz(nlh, NFTA_CHAIN_TYPE, type);
	done(bt);
}

static void flowtable(struct batch *bt, const struct fw_spec *s)
{
	struct nlmsghdr *nlh = request(bt, NFT_MSG_NEWFLOWTABLE, NLM_F_CREATE);
	struct nlattr *h, *devs;

	mnl_attr_put_strz(nlh, NFTA_FLOWTABLE_TABLE, TABLE);
	mnl_attr_put_strz(nlh, NFTA_FLOWTABLE_NAME, FLOWTABLE);
	h = mnl_attr_nest_start(nlh, NFTA_FLOWTABLE_HOOK);
	mnl_attr_put_u32(nlh, NFTA_FLOWTABLE_HOOK_NUM, htonl(NF_NETDEV_INGRESS));
	mnl_attr_put_u32(nlh, NFTA_FLOWTABLE_HOOK_PRIORITY, htonl(0));
	devs = mnl_attr_nest_start(nlh, NFTA_FLOWTABLE_HOOK_DEVS);
	mnl_attr_put_strz(nlh, NFTA_DEVICE_NAME, "sfp");
	for (int i = 0; i < s->nwans; i++)
		mnl_attr_put_strz(nlh, NFTA_DEVICE_NAME, s->wan[i]);
	mnl_attr_nest_end(nlh, devs);
	mnl_attr_nest_end(nlh, h);
	mnl_attr_put_u32(nlh, NFTA_FLOWTABLE_FLAGS, htonl(NFT_FLOWTABLE_HW_OFFLOAD));
	done(bt);
}

/* The table there is taken away, whether there is one or not. */
static void destroy(struct batch *bt)
{
	struct nlmsghdr *nlh = request(bt, NFT_MSG_DESTROYTABLE, 0);

	mnl_attr_put_strz(nlh, NFTA_TABLE_NAME, TABLE);
	done(bt);
}

/*
 * The SYN of a connection that goes out by a session has its maximum segment
 * size cut to what the route out carries, as nft's "tcp option maxseg size
 * set rt mtu", which never raises it: the far end then sends no packet larger
 * than the session takes. A SYN goes through the CPU before its flow is handed
 * to the switch.
 */
static void clamp_mss(struct batch *bt)
{
	static const uint8_t tcp = IPPROTO_TCP, syn = 0x02, none;
	struct nlattr *exprs;
	struct nlmsghdr *nlh = rule_begin(bt, FORWARD, &exprs);
	struct expr e;

	ifname_is(nlh, NFT_META_OIFNAME, WAN_PREFIX, true);
	meta(nlh, NFT_META_L4PROTO);
	cmp(nlh, NFT_CMP_EQ, &tcp, sizeof(tcp));
	payload(nlh, NFT_PAYLOAD_TRANSPORT_HEADER, 13, 1);
	bitwise(nlh, &syn, sizeof(syn));
	cmp(nlh, NFT_CMP_NEQ, &none, sizeof(none));
	e = expr_begin(nlh, "rt");
	mnl_attr_put_u32(nlh, NFTA_RT_KEY, htonl(NFT_RT_TCPMSS));
	mnl_attr_put_u32(nlh, NFTA_RT_DREG, htonl(NFT_REG_1));
	expr_end(nlh, e);
	e = expr_begin(nlh, "byteorder");
	mnl_attr_put_u32(nlh, NFTA_BYTEORDER_SREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_BYTEORDER_DREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_BYTEORDER_OP, htonl(NFT_BYTEORDER_HTON));
	mnl_attr_put_u32(nlh, NFTA_BYTEORDER_LEN, htonl(2));
	mnl_attr_put_u32(nlh, NFTA_BYTEORDER_SIZE, htonl(2));
	expr_end(nlh, e);
	e = expr_begin(nlh, "exthdr");
	mnl_attr_put_u8(nlh, NFTA_EXTHDR_TYPE, TCP_OPT_MSS);
	mnl_attr_put_u32(nlh, NFTA_EXTHDR_OFFSET, htonl(2));
	mnl_attr_put_u32(nlh, NFTA_EXTHDR_LEN, htonl(2));
	mnl_attr_put_u32(nlh, NFTA_EXTHDR_SREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_EXTHDR_OP, htonl(NFT_EXTHDR_OP_TCPOPT));
	expr_end(nlh, e);
	rule_end(bt, nlh, exprs);
}

/*
 * A TCP or UDP flow of network @l handed to the flowtable, by a packet that
 * comes from the network (@key NFT_META_IIFNAME) or goes into it
 * (NFT_META_OIFNAME): a stream that flows one way only, into the network, is
 * handed over by its own packets.
 */
static void flow_add(struct batch *bt, const struct fw_lan *l, uint32_t key, uint8_t proto)
{
	struct nlattr *exprs;
	struct nlmsghdr *nlh = rule_begin(bt, FORWARD, &exprs);
	struct expr e;

	ifname_is(nlh, key, l->ifname, false);
	meta(nlh, NFT_META_L4PROTO);
	cmp(nlh, NFT_CMP_EQ, &proto, sizeof(proto));
	e = expr_begin(nlh, "flow_offload");
	mnl_attr_put_strz(nlh, NFTA_FLOW_TABLE_NAME, FLOWTABLE);
	expr_end(nlh, e);
	rule_end(bt, nlh, exprs);
}

static void accept_answers(struct batch *bt)
{
	struct nlattr *exprs;
	struct nlmsghdr *nlh = rule_begin(bt, FORWARD, &exprs);

	ct_state_any(nlh, NF_CT_STATE_BIT(IP_CT_ESTABLISHED) | NF_CT_STATE_BIT(IP_CT_RELATED));
	verdict(nlh, NF_ACCEPT);
	rule_end(bt, nlh, exprs);
}

/* A new connection from a session into network @l, dropped. */
static void keep_out(struct batch *bt, const struct fw_lan *l)
{
	struct nlattr *exprs;
	struct nlmsghdr *nlh = rule_begin(bt, FORWARD, &exprs);

	ifname_is(nlh, NFT_META_IIFNAME, WAN_PREFIX, true);
	ifname_is(nlh, NFT_META_OIFNAME, l->ifname, false);
	ct_state_any(nlh, NF_CT_STATE_BIT(IP_CT_NEW));
	verdict(nlh, NF_DROP);
	rule_end(bt, nlh, exprs);
}

/* What network @l sends into a session, behind the session's address. */
static void masquerade(struct batch *bt, const struct fw_lan *l)
{
	static const uint8_t ipv4 = NFPROTO_IPV4;
	struct nlattr *exprs;
	struct nlmsghdr *nlh = rule_begin(bt, POSTROUTING, &exprs);
	uint32_t mask = htonl(l->len == 32 ? ~0u : ~(~0u >> l->len));
	uint32_t net = htonl(l->net);
	struct expr e;

	ifname_is(nlh, NFT_META_OIFNAME, WAN_PREFIX, true);
	meta(nlh, NFT_META_NFPROTO);
	cmp(nlh, NFT_CMP_EQ, &ipv4, sizeof(ipv4));
	payload(nlh, NFT_PAYLOAD_NETWORK_HEADER, 12, 4);
	if (l->len < 32)
		bitwise(nlh, &mask, sizeof(mask));
	cmp(nlh, NFT_CMP_EQ, &net, sizeof(net));
	e = expr_begin(nlh, "masq");
	expr_end(nlh, e);
	rule_end(bt, nlh, exprs);
}

static struct batch begin(char *buf, size_t cap, uint32_t seq)
{
	struct batch bt = { .b = mnl_nlmsg_batch_start(buf, cap / 2), .seq = seq };

	edge(&bt, NFNL_MSG_BATCH_BEGIN);
	return bt;
}

static size_t end(struct batch *bt, uint32_t *last)
{
	size_t len;

	*last = bt->seq - 1;
	edge(bt, NFNL_MSG_BATCH_END);
	len = bt->full ? 0 : mnl_nlmsg_batch_size(bt->b);
	mnl_nlmsg_batch_stop(bt->b);
	return len;
}

size_t fw_batch(const struct fw_spec *s, char *buf, size_t cap, uint32_t seq,
		uint32_t *last)
{
	struct batch bt = begin(buf, cap, seq);
	struct nlmsghdr *nlh;

	destroy(&bt);
	nlh = request(&bt, NFT_MSG_NEWTABLE, NLM_F_CREATE);
	mnl_attr_put_strz(nlh, NFTA_TABLE_NAME, TABLE);
	done(&bt);
	if (s->offload)
		flowtable(&bt, s);
	chain(&bt, FORWARD, "filter", NF_INET_FORWARD, 0);
	chain(&bt, POSTROUTING, "nat", NF_INET_POST_ROUTING, SRCNAT_PRIORITY);
	clamp_mss(&bt);
	for (int i = 0; s->offload && i < s->nlans; i++) {
		flow_add(&bt, &s->lan[i], NFT_META_IIFNAME, IPPROTO_TCP);
		flow_add(&bt, &s->lan[i], NFT_META_IIFNAME, IPPROTO_UDP);
		flow_add(&bt, &s->lan[i], NFT_META_OIFNAME, IPPROTO_TCP);
		flow_add(&bt, &s->lan[i], NFT_META_OIFNAME, IPPROTO_UDP);
	}
	accept_answers(&bt);
	for (int i = 0; i < s->nlans; i++)
		keep_out(&bt, &s->lan[i]);
	for (int i = 0; i < s->nlans; i++)
		masquerade(&bt, &s->lan[i]);
	return end(&bt, last);
}

size_t fw_batch_clear(char *buf, size_t cap, uint32_t seq, uint32_t *last)
{
	struct batch bt = begin(buf, cap, seq);

	destroy(&bt);
	return end(&bt, last);
}
