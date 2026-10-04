// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The providers read through netlink: the GEM ports, the bridge's
 * forwarding database and VLANs, the tc filters, and the ONU and its
 * T-CONTs from the generic netlink family "pon". Each decoder takes a
 * netlink dump as bytes, so that the tests can feed it dumps of their
 * own; on Linux the dumps come from the kernel through libmnl.
 */

#define _GNU_SOURCE

#include "prov.h"
#include "providers.h"

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <libmnl/libmnl.h>
#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/pkt_cls.h>
#include <linux/genetlink.h>
#include <linux/pkt_sched.h>
#include <linux/pon.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_vlan.h>
#include <net/if.h>
#include <sys/socket.h>
#else

struct nlmsghdr {
	uint32_t nlmsg_len;
	uint16_t nlmsg_type;
	uint16_t nlmsg_flags;
	uint32_t nlmsg_seq;
	uint32_t nlmsg_pid;
};

struct nlattr {
	uint16_t nla_len;
	uint16_t nla_type;
};

struct ndmsg {
	uint8_t ndm_family;
	uint8_t ndm_pad1;
	uint16_t ndm_pad2;
	int32_t ndm_ifindex;
	uint16_t ndm_state;
	uint8_t ndm_flags;
	uint8_t ndm_type;
};

struct ifinfomsg {
	unsigned char ifi_family;
	unsigned char __ifi_pad;
	unsigned short ifi_type;
	int ifi_index;
	unsigned int ifi_flags;
	unsigned int ifi_change;
};

struct tcmsg {
	unsigned char tcm_family;
	unsigned char tcm__pad1;
	unsigned short tcm__pad2;
	int tcm_ifindex;
	uint32_t tcm_handle;
	uint32_t tcm_parent;
	uint32_t tcm_info;
};

struct bridge_vlan_info {
	uint16_t flags;
	uint16_t vid;
};

struct tc_vlan {
	uint32_t index;
	uint32_t capab;
	int action;
	int refcnt;
	int bindcnt;
	int v_action;
};

#define NLMSG_ERROR 0x2
#define NLMSG_DONE 0x3
#define NLA_F_NESTED (1 << 15)
#define NLA_F_NET_BYTEORDER (1 << 14)
#define NLA_TYPE_MASK ~(NLA_F_NESTED | NLA_F_NET_BYTEORDER)

#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_NEWNEIGH 28
#define RTM_GETNEIGH 30
#define RTM_NEWTFILTER 44
#define RTM_GETTFILTER 46

#define IFLA_IFNAME 3
#define IFLA_MASTER 10
#define IFLA_LINKINFO 18
#define IFLA_AF_SPEC 26
#define IFLA_EXT_MASK 29

#define IFLA_INFO_KIND 1
#define IFLA_INFO_DATA 2

#define IFLA_GEM_PORT_ID 1
#define IFLA_GEM_DIRECTION 2
#define IFLA_GEM_TCONT 3
#define IFLA_GEM_MULTICAST 4

#define IFLA_GEM_DIR_UPSTREAM 1
#define IFLA_GEM_DIR_DOWNSTREAM 2

#define IFLA_BRIDGE_VLAN_INFO 2

#define BRIDGE_VLAN_INFO_PVID (1 << 1)
#define BRIDGE_VLAN_INFO_UNTAGGED (1 << 2)
#define BRIDGE_VLAN_INFO_RANGE_BEGIN (1 << 3)
#define BRIDGE_VLAN_INFO_RANGE_END (1 << 4)

#define NDA_LLADDR 2
#define NDA_VLAN 5
#define NDA_MASTER 9

#define NUD_REACHABLE 0x02
#define NUD_STALE 0x04
#define NUD_NOARP 0x40
#define NUD_PERMANENT 0x80

#define NTF_SELF (1 << 1)
#define NTF_EXT_LEARNED (1 << 4)
#define NTF_OFFLOADED (1 << 5)
#define NTF_STICKY (1 << 6)

#define TCA_KIND 1
#define TCA_OPTIONS 2
#define TCA_CHAIN 11

#define TCA_FLOWER_CLASSID 1
#define TCA_FLOWER_INDEV 2
#define TCA_FLOWER_ACT 3
#define TCA_FLOWER_KEY_ETH_DST 4
#define TCA_FLOWER_KEY_ETH_DST_MASK 5
#define TCA_FLOWER_KEY_ETH_SRC 6
#define TCA_FLOWER_KEY_ETH_SRC_MASK 7
#define TCA_FLOWER_KEY_ETH_TYPE 8
#define TCA_FLOWER_KEY_IP_PROTO 9
#define TCA_FLOWER_KEY_IPV4_SRC 10
#define TCA_FLOWER_KEY_IPV4_SRC_MASK 11
#define TCA_FLOWER_KEY_IPV4_DST 12
#define TCA_FLOWER_KEY_IPV4_DST_MASK 13
#define TCA_FLOWER_FLAGS 22
#define TCA_FLOWER_KEY_VLAN_ID 23
#define TCA_FLOWER_KEY_VLAN_PRIO 24
#define TCA_FLOWER_KEY_VLAN_ETH_TYPE 25

#define TCA_CLS_FLAGS_SKIP_HW (1 << 0)
#define TCA_CLS_FLAGS_SKIP_SW (1 << 1)
#define TCA_CLS_FLAGS_IN_HW (1 << 2)
#define TCA_CLS_FLAGS_NOT_IN_HW (1 << 3)

#define TCA_ACT_KIND 1
#define TCA_ACT_OPTIONS 2

#define TCA_VLAN_PARMS 2
#define TCA_VLAN_PUSH_VLAN_ID 3
#define TCA_VLAN_PUSH_VLAN_PROTOCOL 4
#define TCA_VLAN_PUSH_VLAN_PRIORITY 6

#define TCA_VLAN_ACT_POP 1
#define TCA_VLAN_ACT_PUSH 2
#define TCA_VLAN_ACT_MODIFY 3
#define TCA_VLAN_ACT_POP_ETH 4
#define TCA_VLAN_ACT_PUSH_ETH 5

#define TC_H_MAJ_MASK (0xFFFF0000U)
#define TC_H_MIN_MASK (0x0000FFFFU)
#define TC_H_MAJ(h) ((h) & TC_H_MAJ_MASK)
#define TC_H_MIN(h) ((h) & TC_H_MIN_MASK)
#define TC_H_MAKE(maj, min) (((maj) & TC_H_MAJ_MASK) | ((min) & TC_H_MIN_MASK))
#define TC_H_CLSACT (0xFFFFFFF1U)
#define TC_H_MIN_INGRESS 0xFFF2U

struct genlmsghdr {
	uint8_t cmd;
	uint8_t version;
	uint16_t reserved;
};

#define PON_A_DEV_ID 1
#define PON_A_DEV_IFINDEX 2
#define PON_A_DEV_TYPE 3
#define PON_A_DEV_ONU_STATE 4
#define PON_A_DEV_ONU_ID 5
#define PON_A_DEV_ALLOC_ID 6
#define PON_A_DEV_OMCC_PORT_ID 7
#define PON_A_DEV_ALARMS 8
#define PON_A_DEV_STATS 9
#define PON_A_DEV_SERIAL_NUMBER 10
#define PON_A_DEV_NUM_GEM_PORTS 11
#define PON_A_DEV_NUM_TCONTS 12

#define PON_A_TCONT_ID 1
#define PON_A_TCONT_INDEX 2
#define PON_A_TCONT_ALLOC_ID 3

#define PON_CMD_DEV_CHANGE_NTF 2
#define PON_CMD_TCONT_GET 3

#define PON_A_DEV_STATS_SN_REQUESTS 1
#define PON_A_DEV_STATS_RANGING_REQUESTS 2
#define PON_A_DEV_STATS_PLOAM_RX 3
#define PON_A_DEV_STATS_PLOAM_CRC_ERRORS 4
#define PON_A_DEV_STATS_PLOAM_OVERFLOWS 5
#define PON_A_DEV_STATS_BWMAP_RX 6
#define PON_A_DEV_STATS_BWMAP_CRC_ERRORS 7
#define PON_A_DEV_STATS_BWMAP_OVERFLOWS 8
#define PON_A_DEV_STATS_GEM_FRAMES 9
#define PON_A_DEV_STATS_BIP_ERRORS 10

#define PON_CMD_DEV_GET 1

#define PON_TYPE_GPON 1
#define PON_TYPE_XGPON 2
#define PON_TYPE_XGSPON 3
#define PON_TYPE_NGPON2 4

#define PON_ONU_STATE_O1 1
#define PON_ONU_STATE_O7 7

#define PON_ALARM_LOS 1
#define PON_ALARM_LOF 2
#define PON_ALARM_LOM 4
#define PON_ALARM_PEE 8
#endif

static_assert(sizeof(struct nlmsghdr) == 16,
	      "netlink message header is 16 bytes");
static_assert(sizeof(struct nlattr) == 4,
	      "netlink attribute header is 4 bytes");
static_assert(offsetof(struct ndmsg, ndm_ifindex) == 4, "ndmsg layout");
static_assert(offsetof(struct ndmsg, ndm_state) == 8, "ndmsg layout");
static_assert(offsetof(struct ndmsg, ndm_flags) == 10, "ndmsg layout");
static_assert(sizeof(struct ndmsg) == 12, "ndmsg is 12 bytes");
static_assert(offsetof(struct ifinfomsg, ifi_index) == 4, "ifinfomsg layout");
static_assert(sizeof(struct ifinfomsg) == 16, "ifinfomsg is 16 bytes");
static_assert(offsetof(struct tcmsg, tcm_ifindex) == 4, "tcmsg layout");
static_assert(offsetof(struct tcmsg, tcm_handle) == 8, "tcmsg layout");
static_assert(offsetof(struct tcmsg, tcm_parent) == 12, "tcmsg layout");
static_assert(offsetof(struct tcmsg, tcm_info) == 16, "tcmsg layout");
static_assert(sizeof(struct bridge_vlan_info) == 4,
	      "bridge_vlan_info is flags then vid");
static_assert(offsetof(struct bridge_vlan_info, vid) == 2,
	      "bridge_vlan_info layout");
static_assert(offsetof(struct tc_vlan, v_action) == 20,
	      "tc_vlan sits behind the tc_gen prefix");

static_assert(RTM_NEWLINK == 16 && RTM_GETLINK == 18, "rtnetlink link types");
static_assert(RTM_NEWNEIGH == 28 && RTM_GETNEIGH == 30,
	      "rtnetlink neigh types");
static_assert(RTM_NEWTFILTER == 44 && RTM_GETTFILTER == 46,
	      "rtnetlink tfilter types");
static_assert(IFLA_IFNAME == 3 && IFLA_MASTER == 10, "IFLA numbering");
static_assert(IFLA_LINKINFO == 18 && IFLA_AF_SPEC == 26 && IFLA_EXT_MASK == 29,
	      "IFLA numbering");
static_assert(IFLA_INFO_KIND == 1 && IFLA_INFO_DATA == 2,
	      "IFLA_INFO numbering");
static_assert(IFLA_GEM_PORT_ID == 1 && IFLA_GEM_DIRECTION == 2 &&
		      IFLA_GEM_TCONT == 3 && IFLA_GEM_MULTICAST == 4,
	      "our own gem link attributes");
static_assert(IFLA_GEM_DIR_UPSTREAM == 1 && IFLA_GEM_DIR_DOWNSTREAM == 2,
	      "G.988 direction bits");
static_assert(IFLA_BRIDGE_VLAN_INFO == 2, "IFLA_BRIDGE numbering");
static_assert(NDA_LLADDR == 2 && NDA_VLAN == 5 && NDA_MASTER == 9,
	      "NDA numbering");
static_assert(NUD_REACHABLE == 0x02 && NUD_NOARP == 0x40 &&
		      NUD_PERMANENT == 0x80,
	      "NUD bits");
static_assert(NTF_SELF == 0x02 && NTF_OFFLOADED == 0x20, "NTF bits");
static_assert(TCA_KIND == 1 && TCA_OPTIONS == 2 && TCA_CHAIN == 11,
	      "TCA numbering");
static_assert(TCA_FLOWER_ACT == 3 && TCA_FLOWER_KEY_ETH_TYPE == 8,
	      "flower numbering");
static_assert(TCA_FLOWER_FLAGS == 22 && TCA_FLOWER_KEY_VLAN_ID == 23 &&
		      TCA_FLOWER_KEY_VLAN_ETH_TYPE == 25,
	      "flower numbering");
static_assert(TCA_ACT_KIND == 1 && TCA_ACT_OPTIONS == 2, "tc action numbering");
static_assert(TCA_VLAN_PARMS == 2 && TCA_VLAN_PUSH_VLAN_ID == 3 &&
		      TCA_VLAN_PUSH_VLAN_PRIORITY == 6,
	      "act_vlan numbering");
static_assert(TCA_VLAN_ACT_MODIFY == 3, "act_vlan verdicts");
static_assert(TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS) == 0xFFFFFFF2U,
	      "the clsact ingress parent is ffff:fff2");
static_assert(sizeof(struct genlmsghdr) == 4, "the genl header is four bytes");
static_assert(PON_A_DEV_ID == 1 && PON_A_DEV_IFINDEX == 2 &&
		      PON_A_DEV_TYPE == 3 && PON_A_DEV_ONU_STATE == 4 &&
		      PON_A_DEV_ONU_ID == 5 && PON_A_DEV_ALLOC_ID == 6 &&
		      PON_A_DEV_OMCC_PORT_ID == 7 && PON_A_DEV_ALARMS == 8 &&
		      PON_A_DEV_STATS == 9 && PON_A_DEV_SERIAL_NUMBER == 10 &&
		      PON_A_DEV_NUM_GEM_PORTS == 11 &&
		      PON_A_DEV_NUM_TCONTS == 12,
	      "our own pon dev attributes");
static_assert(PON_A_TCONT_ID == 1 && PON_A_TCONT_INDEX == 2 &&
		      PON_A_TCONT_ALLOC_ID == 3,
	      "our own pon tcont attributes");
static_assert(PON_A_DEV_STATS_SN_REQUESTS == 1 &&
		      PON_A_DEV_STATS_PLOAM_RX == 3 &&
		      PON_A_DEV_STATS_BWMAP_RX == 6 &&
		      PON_A_DEV_STATS_BIP_ERRORS == 10,
	      "the pon stats nest");

static_assert(PON_CMD_DEV_GET == 1 && PON_CMD_DEV_CHANGE_NTF == 2 &&
		      PON_CMD_TCONT_GET == 3,
	      "pon commands");
static_assert(PON_TYPE_GPON == 1 && PON_ONU_STATE_O1 == 1 &&
		      PON_ONU_STATE_O7 == 7,
	      "pon enumerations start at one");
static_assert(PON_ALARM_LOS == 1 && PON_ALARM_LOF == 2 && PON_ALARM_LOM == 4 &&
		      PON_ALARM_PEE == 8,
	      "pon alarm bits");

#define NL_HDRLEN 16
#define NL_ALIGN(n) (((n) + 3U) & ~3U)
#define NL_ATTR_HDRLEN 4

#define NL_ATTR_MAX 48

struct nl_attrs {
	const char *val[NL_ATTR_MAX + 1];
	uint16_t len[NL_ATTR_MAX + 1];
};

static uint16_t nl_u16at(const char *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static uint32_t nl_u32at(const char *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static void nl_attrs_parse(struct nl_attrs *a, const char *p, size_t len)
{
	memset(a, 0, sizeof(*a));

	while (len >= NL_ATTR_HDRLEN) {
		uint16_t alen = nl_u16at(p);
		uint16_t type = (uint16_t)(nl_u16at(p + 2) & NLA_TYPE_MASK);
		size_t step;

		if (alen < NL_ATTR_HDRLEN || alen > len)
			return;
		if (type <= NL_ATTR_MAX) {
			a->val[type] = p + NL_ATTR_HDRLEN;
			a->len[type] = (uint16_t)(alen - NL_ATTR_HDRLEN);
		}
		step = NL_ALIGN(alen);
		if (step > len)
			return;
		len -= step;
		p += step;
	}
}

static bool nl_has(const struct nl_attrs *a, unsigned int type)
{
	return type <= NL_ATTR_MAX && a->val[type] != NULL;
}

static uint8_t nl_get_u8(const struct nl_attrs *a, unsigned int type,
			 uint8_t absent)
{
	if (!nl_has(a, type) || a->len[type] < 1)
		return absent;
	return (uint8_t)a->val[type][0];
}

static uint16_t nl_get_u16(const struct nl_attrs *a, unsigned int type,
			   uint16_t absent)
{
	if (!nl_has(a, type) || a->len[type] < 2)
		return absent;
	return nl_u16at(a->val[type]);
}

static uint32_t nl_get_u32(const struct nl_attrs *a, unsigned int type,
			   uint32_t absent)
{
	if (!nl_has(a, type) || a->len[type] < 4)
		return absent;
	return nl_u32at(a->val[type]);
}

static uint64_t nl_get_uint(const struct nl_attrs *a, unsigned int type,
			    uint64_t absent)
{
	if (!nl_has(a, type))
		return absent;
	if (a->len[type] == 8) {
		uint64_t v;

		memcpy(&v, a->val[type], sizeof(v));
		return v;
	}
	if (a->len[type] == 4)
		return nl_u32at(a->val[type]);
	return absent;
}

static uint16_t nl_get_be16(const struct nl_attrs *a, unsigned int type,
			    uint16_t absent)
{
	if (!nl_has(a, type) || a->len[type] < 2)
		return absent;
	return ntohs(nl_u16at(a->val[type]));
}

static const char *nl_get_str(const struct nl_attrs *a, unsigned int type,
			      char *buf, size_t cap)
{
	size_t n;

	buf[0] = '\0';
	if (!nl_has(a, type))
		return NULL;
	n = a->len[type];
	while (n && a->val[type][n - 1] == '\0')
		n--;
	if (n >= cap)
		n = cap - 1;
	memcpy(buf, a->val[type], n);
	buf[n] = '\0';
	return buf;
}

static void nl_pv_mac(struct prov_buf *b, const char *p, size_t len)
{
	const unsigned char *u = (const unsigned char *)p;
	char t[18];

	if (len < 6) {
		pv_null(b);
		return;
	}
	snprintf(t, sizeof(t), "%02x:%02x:%02x:%02x:%02x:%02x", u[0], u[1],
		 u[2], u[3], u[4], u[5]);
	pv_str(b, t);
}

static void nl_pv_ipv4(struct prov_buf *b, const char *p, size_t len)
{
	const unsigned char *u = (const unsigned char *)p;
	char t[16];

	if (len < 4) {
		pv_null(b);
		return;
	}
	snprintf(t, sizeof(t), "%u.%u.%u.%u", u[0], u[1], u[2], u[3]);
	pv_str(b, t);
}

static const char *nl_next_msg(const char *buf, size_t len, size_t *off,
			       uint16_t *type, const char **payload,
			       size_t *paylen)
{
	const char *p;
	uint32_t mlen;
	size_t step;

	if (*off + NL_HDRLEN > len)
		return NULL;
	p = buf + *off;
	mlen = nl_u32at(p);
	if (mlen < NL_HDRLEN || mlen > len - *off)
		return NULL;

	*type = nl_u16at(p + 4);
	*payload = p + NL_HDRLEN;
	*paylen = mlen - NL_HDRLEN;

	step = NL_ALIGN(mlen);
	if (step > len - *off)
		*off = len;
	else
		*off += step;
	return p;
}

#define NL_IFNAMSZ 16
#define NL_IFMAP_MAX 64

struct nl_ifmap {
	struct {
		int32_t index;
		char name[NL_IFNAMSZ];
	} ent[NL_IFMAP_MAX];
	bool truncated;
	unsigned int n;
};

static void nl_ifmap_build(struct nl_ifmap *map, const char *buf, size_t len)
{
	size_t off = 0;
	uint16_t type;
	const char *pay;
	size_t paylen;

	map->n = 0;
	while (nl_next_msg(buf, len, &off, &type, &pay, &paylen)) {
		struct ifinfomsg ifi;
		struct nl_attrs a;

		if (type != RTM_NEWLINK || paylen < sizeof(ifi))
			continue;
		if (map->n >= NL_IFMAP_MAX) {
			map->truncated = true;
			return;
		}
		memcpy(&ifi, pay, sizeof(ifi));
		nl_attrs_parse(&a, pay + sizeof(ifi), paylen - sizeof(ifi));
		map->ent[map->n].index = ifi.ifi_index;
		if (!nl_get_str(&a, IFLA_IFNAME, map->ent[map->n].name,
				sizeof(map->ent[map->n].name)))
			continue;
		map->n++;
	}
}

static const char *nl_ifmap_name(const struct nl_ifmap *map, int32_t index)
{
	unsigned int i;

	for (i = 0; i < map->n; i++)
		if (map->ent[i].index == index)
			return map->ent[i].name;
	return NULL;
}

static void nl_pv_ifname(struct prov_buf *out, const struct nl_ifmap *map,
			 int32_t index)
{
	const char *name = nl_ifmap_name(map, index);

	if (name)
		pv_str(out, name);
	else
		pv_null(out);
}

int prov_nl_gem_ports(const char *links, size_t len, struct prov_buf *out)
{
	struct nl_ifmap map;
	size_t off = 0;
	uint16_t type;
	const char *pay;
	size_t paylen;
	bool first = true;

	nl_ifmap_build(&map, links, len);

	pv_open(out, '[');
	while (nl_next_msg(links, len, &off, &type, &pay, &paylen)) {
		struct ifinfomsg ifi;
		struct nl_attrs a, info, data;
		char kind[24], name[NL_IFNAMSZ];
		uint8_t dir;

		if (type != RTM_NEWLINK || paylen < sizeof(ifi))
			continue;
		memcpy(&ifi, pay, sizeof(ifi));
		nl_attrs_parse(&a, pay + sizeof(ifi), paylen - sizeof(ifi));

		if (!nl_has(&a, IFLA_LINKINFO))
			continue;
		nl_attrs_parse(&info, a.val[IFLA_LINKINFO],
			       a.len[IFLA_LINKINFO]);
		if (!nl_get_str(&info, IFLA_INFO_KIND, kind, sizeof(kind)))
			continue;
		if (strcmp(kind, "gem"))
			continue;
		nl_attrs_parse(&data,
			       nl_has(&info, IFLA_INFO_DATA) ?
				       info.val[IFLA_INFO_DATA] :
				       "",
			       nl_has(&info, IFLA_INFO_DATA) ?
				       info.len[IFLA_INFO_DATA] :
				       0);

		if (!first)
			pv_comma(out);
		first = false;

		pv_open(out, '{');
		pv_key(out, "name");
		if (nl_get_str(&a, IFLA_IFNAME, name, sizeof(name)))
			pv_str(out, name);
		else
			pv_null(out);

		pv_comma(out);
		pv_key(out, "ifindex");
		pv_num(out, ifi.ifi_index);

		pv_comma(out);
		pv_key(out, "master");
		if (nl_has(&a, IFLA_MASTER))
			nl_pv_ifname(out, &map,
				     (int32_t)nl_get_u32(&a, IFLA_MASTER, 0));
		else
			pv_null(out);

		pv_comma(out);
		pv_key(out, "port_id");
		if (nl_has(&data, IFLA_GEM_PORT_ID))
			pv_num(out, nl_get_u16(&data, IFLA_GEM_PORT_ID, 0));
		else
			pv_null(out);

		dir = nl_get_u8(&data, IFLA_GEM_DIRECTION, 0);
		pv_comma(out);
		pv_key(out, "direction");
		if (!nl_has(&data, IFLA_GEM_DIRECTION))
			pv_null(out);
		else if (dir ==
			 (IFLA_GEM_DIR_UPSTREAM | IFLA_GEM_DIR_DOWNSTREAM))
			pv_str(out, "both");
		else if (dir == IFLA_GEM_DIR_UPSTREAM)
			pv_str(out, "upstream");
		else if (dir == IFLA_GEM_DIR_DOWNSTREAM)
			pv_str(out, "downstream");
		else
			pv_str(out, "unknown");
		pv_comma(out);
		pv_key(out, "direction_bits");
		if (nl_has(&data, IFLA_GEM_DIRECTION))
			pv_num(out, dir);
		else
			pv_null(out);

		pv_comma(out);
		pv_key(out, "tcont");
		if (nl_has(&data, IFLA_GEM_TCONT))
			pv_num(out, nl_get_u32(&data, IFLA_GEM_TCONT, 0));
		else
			pv_null(out);

		pv_comma(out);
		pv_key(out, "multicast");
		pv_bool(out, nl_has(&data, IFLA_GEM_MULTICAST));

		pv_close(out, '}');

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
	return 0;
}

static void nl_fdb_state(struct prov_buf *out, uint16_t state)
{
	if (state & NUD_PERMANENT)
		pv_str(out, "permanent");
	else if (state & NUD_NOARP)
		pv_str(out, "static");
	else if (state & NUD_STALE)
		pv_str(out, "stale");
	else if (state & NUD_REACHABLE)
		pv_str(out, "reachable");
	else
		pv_str(out, "none");
}

int prov_nl_fdb(const char *links, size_t llen, const char *fdb, size_t flen,
		struct prov_buf *out)
{
	struct nl_ifmap map;
	size_t off = 0;
	uint16_t type;
	const char *pay;
	size_t paylen;
	bool first = true;

	nl_ifmap_build(&map, links, llen);

	pv_open(out, '[');
	while (nl_next_msg(fdb, flen, &off, &type, &pay, &paylen)) {
		struct ndmsg nd;
		struct nl_attrs a;

		if (type != RTM_NEWNEIGH || paylen < sizeof(nd))
			continue;
		memcpy(&nd, pay, sizeof(nd));
		nl_attrs_parse(&a, pay + sizeof(nd), paylen - sizeof(nd));

		if (!first)
			pv_comma(out);
		first = false;

		pv_open(out, '{');
		pv_key(out, "dev");
		nl_pv_ifname(out, &map, nd.ndm_ifindex);

		pv_comma(out);
		pv_key(out, "ifindex");
		pv_num(out, nd.ndm_ifindex);

		pv_comma(out);
		pv_key(out, "addr");
		if (nl_has(&a, NDA_LLADDR))
			nl_pv_mac(out, a.val[NDA_LLADDR], a.len[NDA_LLADDR]);
		else
			pv_null(out);

		pv_comma(out);
		pv_key(out, "vlan");
		if (nl_has(&a, NDA_VLAN))
			pv_num(out, nl_get_u16(&a, NDA_VLAN, 0));
		else
			pv_null(out);

		pv_comma(out);
		pv_key(out, "master");
		if (nl_has(&a, NDA_MASTER))
			nl_pv_ifname(out, &map,
				     (int32_t)nl_get_u32(&a, NDA_MASTER, 0));
		else
			pv_null(out);

		pv_comma(out);
		pv_key(out, "state");
		nl_fdb_state(out, nd.ndm_state);

		pv_comma(out);
		pv_key(out, "learned");
		pv_bool(out, (nd.ndm_state & (NUD_REACHABLE | NUD_STALE)) != 0);

		pv_comma(out);
		pv_key(out, "self");
		pv_bool(out, (nd.ndm_flags & NTF_SELF) != 0);

		pv_comma(out);
		pv_key(out, "offloaded");
		pv_bool(out, (nd.ndm_flags & NTF_OFFLOADED) != 0);

		pv_comma(out);
		pv_key(out, "ext_learned");
		pv_bool(out, (nd.ndm_flags & NTF_EXT_LEARNED) != 0);

		pv_comma(out);
		pv_key(out, "sticky");
		pv_bool(out, (nd.ndm_flags & NTF_STICKY) != 0);

		pv_close(out, '}');

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
	return 0;
}

int prov_nl_vlans(const char *brlinks, size_t len, struct prov_buf *out)
{
	size_t off = 0;
	uint16_t type;
	const char *pay;
	size_t paylen;
	bool first = true;

	pv_open(out, '[');
	while (nl_next_msg(brlinks, len, &off, &type, &pay, &paylen)) {
		struct ifinfomsg ifi;
		struct nl_attrs a;
		char name[NL_IFNAMSZ];
		const char *p;
		size_t left;

		if (type != RTM_NEWLINK || paylen < sizeof(ifi))
			continue;
		memcpy(&ifi, pay, sizeof(ifi));
		nl_attrs_parse(&a, pay + sizeof(ifi), paylen - sizeof(ifi));
		if (!nl_has(&a, IFLA_AF_SPEC))
			continue;
		if (!nl_get_str(&a, IFLA_IFNAME, name, sizeof(name)))
			continue;

		p = a.val[IFLA_AF_SPEC];
		left = a.len[IFLA_AF_SPEC];
		while (left >= NL_ATTR_HDRLEN) {
			uint16_t alen = nl_u16at(p);
			uint16_t atype =
				(uint16_t)(nl_u16at(p + 2) & NLA_TYPE_MASK);
			struct bridge_vlan_info vi;
			size_t step;

			if (alen < NL_ATTR_HDRLEN || alen > left)
				break;
			if (atype == IFLA_BRIDGE_VLAN_INFO &&
			    alen >= NL_ATTR_HDRLEN + sizeof(vi)) {
				memcpy(&vi, p + NL_ATTR_HDRLEN, sizeof(vi));

				if (!first)
					pv_comma(out);
				first = false;

				pv_open(out, '{');
				pv_key(out, "dev");
				pv_str(out, name);
				pv_comma(out);
				pv_key(out, "vlan");
				pv_num(out, vi.vid);
				pv_comma(out);
				pv_key(out, "pvid");
				pv_bool(out, (vi.flags &
					      BRIDGE_VLAN_INFO_PVID) != 0);
				pv_comma(out);
				pv_key(out, "egress_untagged");
				pv_bool(out, (vi.flags &
					      BRIDGE_VLAN_INFO_UNTAGGED) != 0);
				pv_comma(out);
				pv_key(out, "range_begin");
				pv_bool(out,
					(vi.flags &
					 BRIDGE_VLAN_INFO_RANGE_BEGIN) != 0);
				pv_comma(out);
				pv_key(out, "range_end");
				pv_bool(out, (vi.flags &
					      BRIDGE_VLAN_INFO_RANGE_END) != 0);
				pv_close(out, '}');
			}
			step = NL_ALIGN(alen);
			if (step > left)
				break;
			left -= step;
			p += step;
		}

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
	return 0;
}

static void nl_tc_vlan_opts(struct prov_buf *out, const struct nl_attrs *o)
{
	struct tc_vlan v;

	pv_key(out, "vlan_action");
	if (nl_has(o, TCA_VLAN_PARMS) && o->len[TCA_VLAN_PARMS] >= sizeof(v)) {
		memcpy(&v, o->val[TCA_VLAN_PARMS], sizeof(v));
		switch (v.v_action) {
		case TCA_VLAN_ACT_POP:
			pv_str(out, "pop");
			break;
		case TCA_VLAN_ACT_PUSH:
			pv_str(out, "push");
			break;
		case TCA_VLAN_ACT_MODIFY:
			pv_str(out, "modify");
			break;
		case TCA_VLAN_ACT_POP_ETH:
			pv_str(out, "pop_eth");
			break;
		case TCA_VLAN_ACT_PUSH_ETH:
			pv_str(out, "push_eth");
			break;
		default:
			pv_str(out, "unknown");
			break;
		}
		pv_comma(out);
		pv_key(out, "index");
		pv_num(out, v.index);
	} else {
		pv_null(out);
		pv_comma(out);
		pv_key(out, "index");
		pv_null(out);
	}

	pv_comma(out);
	pv_key(out, "vlan_id");
	if (nl_has(o, TCA_VLAN_PUSH_VLAN_ID))
		pv_num(out, nl_get_u16(o, TCA_VLAN_PUSH_VLAN_ID, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "vlan_tpid");
	if (nl_has(o, TCA_VLAN_PUSH_VLAN_PROTOCOL))
		pv_num(out, nl_get_be16(o, TCA_VLAN_PUSH_VLAN_PROTOCOL, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "vlan_prio");
	if (nl_has(o, TCA_VLAN_PUSH_VLAN_PRIORITY))
		pv_num(out, nl_get_u8(o, TCA_VLAN_PUSH_VLAN_PRIORITY, 0));
	else
		pv_null(out);
}

static void nl_tc_actions(struct prov_buf *out, const char *p, size_t len)
{
	bool first = true;

	pv_open(out, '[');

	while (len >= NL_ATTR_HDRLEN) {
		uint16_t alen = nl_u16at(p);
		uint16_t order = (uint16_t)(nl_u16at(p + 2) & NLA_TYPE_MASK);
		struct nl_attrs act;
		char kind[24];
		size_t step;

		if (alen < NL_ATTR_HDRLEN || alen > len)
			break;
		nl_attrs_parse(&act, p + NL_ATTR_HDRLEN,
			       (size_t)(alen - NL_ATTR_HDRLEN));

		if (!first)
			pv_comma(out);
		first = false;

		pv_open(out, '{');
		pv_key(out, "order");
		pv_num(out, order);
		pv_comma(out);
		pv_key(out, "kind");
		if (nl_get_str(&act, TCA_ACT_KIND, kind, sizeof(kind)))
			pv_str(out, kind);
		else
			pv_null(out);

		if (!strcmp(kind, "vlan") && nl_has(&act, TCA_ACT_OPTIONS)) {
			struct nl_attrs opts;

			nl_attrs_parse(&opts, act.val[TCA_ACT_OPTIONS],
				       act.len[TCA_ACT_OPTIONS]);
			pv_comma(out);
			nl_tc_vlan_opts(out, &opts);
		} else {
			pv_comma(out);
			pv_key(out, "options_decoded");
			pv_bool(out, false);
		}
		pv_close(out, '}');

		step = NL_ALIGN(alen);
		if (step > len)
			break;
		len -= step;
		p += step;

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
}

static const uint16_t nl_flower_known[] = {
	TCA_FLOWER_CLASSID,
	TCA_FLOWER_INDEV,
	TCA_FLOWER_ACT,
	TCA_FLOWER_KEY_ETH_DST,
	TCA_FLOWER_KEY_ETH_DST_MASK,
	TCA_FLOWER_KEY_ETH_SRC,
	TCA_FLOWER_KEY_ETH_SRC_MASK,
	TCA_FLOWER_KEY_ETH_TYPE,
	TCA_FLOWER_KEY_IP_PROTO,
	TCA_FLOWER_KEY_IPV4_SRC,
	TCA_FLOWER_KEY_IPV4_SRC_MASK,
	TCA_FLOWER_KEY_IPV4_DST,
	TCA_FLOWER_KEY_IPV4_DST_MASK,
	TCA_FLOWER_FLAGS,
	TCA_FLOWER_KEY_VLAN_ID,
	TCA_FLOWER_KEY_VLAN_PRIO,
	TCA_FLOWER_KEY_VLAN_ETH_TYPE,
};

static void nl_tc_flower_unparsed(struct prov_buf *out, const char *p,
				  size_t len)
{
	bool first = true;

	pv_open(out, '[');
	while (len >= NL_ATTR_HDRLEN) {
		uint16_t alen = nl_u16at(p);
		uint16_t atype = (uint16_t)(nl_u16at(p + 2) & NLA_TYPE_MASK);
		size_t step, i;
		bool known = false;

		if (alen < NL_ATTR_HDRLEN || alen > len)
			break;
		for (i = 0; i < ARRAY_SIZE(nl_flower_known); i++)
			if (nl_flower_known[i] == atype)
				known = true;
		if (!known) {
			if (!first)
				pv_comma(out);
			first = false;
			pv_num(out, atype);
		}
		step = NL_ALIGN(alen);
		if (step > len)
			break;
		len -= step;
		p += step;
	}
	pv_close(out, ']');
}

static void nl_tc_flower_match(struct prov_buf *out, const struct nl_attrs *f)
{
	char indev[NL_IFNAMSZ];

	pv_open(out, '{');

	pv_key(out, "vlan_id");
	if (nl_has(f, TCA_FLOWER_KEY_VLAN_ID))
		pv_num(out, nl_get_u16(f, TCA_FLOWER_KEY_VLAN_ID, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "vlan_prio");
	if (nl_has(f, TCA_FLOWER_KEY_VLAN_PRIO))
		pv_num(out, nl_get_u8(f, TCA_FLOWER_KEY_VLAN_PRIO, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "vlan_ethtype");
	if (nl_has(f, TCA_FLOWER_KEY_VLAN_ETH_TYPE))
		pv_num(out, nl_get_be16(f, TCA_FLOWER_KEY_VLAN_ETH_TYPE, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "ethertype");
	if (nl_has(f, TCA_FLOWER_KEY_ETH_TYPE))
		pv_num(out, nl_get_be16(f, TCA_FLOWER_KEY_ETH_TYPE, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "ip_proto");
	if (nl_has(f, TCA_FLOWER_KEY_IP_PROTO))
		pv_num(out, nl_get_u8(f, TCA_FLOWER_KEY_IP_PROTO, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "eth_dst");
	if (nl_has(f, TCA_FLOWER_KEY_ETH_DST))
		nl_pv_mac(out, f->val[TCA_FLOWER_KEY_ETH_DST],
			  f->len[TCA_FLOWER_KEY_ETH_DST]);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "eth_dst_mask");
	if (nl_has(f, TCA_FLOWER_KEY_ETH_DST_MASK))
		nl_pv_mac(out, f->val[TCA_FLOWER_KEY_ETH_DST_MASK],
			  f->len[TCA_FLOWER_KEY_ETH_DST_MASK]);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "eth_src");
	if (nl_has(f, TCA_FLOWER_KEY_ETH_SRC))
		nl_pv_mac(out, f->val[TCA_FLOWER_KEY_ETH_SRC],
			  f->len[TCA_FLOWER_KEY_ETH_SRC]);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "eth_src_mask");
	if (nl_has(f, TCA_FLOWER_KEY_ETH_SRC_MASK))
		nl_pv_mac(out, f->val[TCA_FLOWER_KEY_ETH_SRC_MASK],
			  f->len[TCA_FLOWER_KEY_ETH_SRC_MASK]);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "ipv4_src");
	if (nl_has(f, TCA_FLOWER_KEY_IPV4_SRC))
		nl_pv_ipv4(out, f->val[TCA_FLOWER_KEY_IPV4_SRC],
			   f->len[TCA_FLOWER_KEY_IPV4_SRC]);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "ipv4_src_mask");
	if (nl_has(f, TCA_FLOWER_KEY_IPV4_SRC_MASK))
		nl_pv_ipv4(out, f->val[TCA_FLOWER_KEY_IPV4_SRC_MASK],
			   f->len[TCA_FLOWER_KEY_IPV4_SRC_MASK]);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "ipv4_dst");
	if (nl_has(f, TCA_FLOWER_KEY_IPV4_DST))
		nl_pv_ipv4(out, f->val[TCA_FLOWER_KEY_IPV4_DST],
			   f->len[TCA_FLOWER_KEY_IPV4_DST]);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "ipv4_dst_mask");
	if (nl_has(f, TCA_FLOWER_KEY_IPV4_DST_MASK))
		nl_pv_ipv4(out, f->val[TCA_FLOWER_KEY_IPV4_DST_MASK],
			   f->len[TCA_FLOWER_KEY_IPV4_DST_MASK]);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "indev");
	if (nl_get_str(f, TCA_FLOWER_INDEV, indev, sizeof(indev)) && indev[0])
		pv_str(out, indev);
	else
		pv_null(out);

	pv_close(out, '}');
}

int prov_nl_filters(const char *filters, size_t len, struct prov_buf *out)
{
	size_t off = 0;
	uint16_t type;
	const char *pay;
	size_t paylen;
	bool first = true;

	pv_open(out, '[');
	while (nl_next_msg(filters, len, &off, &type, &pay, &paylen)) {
		struct tcmsg tcm;
		struct nl_attrs a;
		char kind[24];
		uint32_t flags;

		if (type != RTM_NEWTFILTER || paylen < sizeof(tcm))
			continue;
		memcpy(&tcm, pay, sizeof(tcm));
		nl_attrs_parse(&a, pay + sizeof(tcm), paylen - sizeof(tcm));

		if (!first)
			pv_comma(out);
		first = false;

		pv_open(out, '{');
		pv_key(out, "handle");
		pv_num(out, tcm.tcm_handle);

		pv_comma(out);
		pv_key(out, "priority");
		pv_num(out, TC_H_MAJ(tcm.tcm_info) >> 16);
		pv_comma(out);
		pv_key(out, "ethertype");
		pv_num(out, ntohs((uint16_t)TC_H_MIN(tcm.tcm_info)));

		pv_comma(out);
		pv_key(out, "parent");
		pv_num(out, tcm.tcm_parent);

		pv_comma(out);
		pv_key(out, "chain");
		pv_num(out, nl_get_u32(&a, TCA_CHAIN, 0));

		pv_comma(out);
		pv_key(out, "kind");
		if (nl_get_str(&a, TCA_KIND, kind, sizeof(kind)))
			pv_str(out, kind);
		else
			pv_null(out);

		if (!strcmp(kind, "flower") && nl_has(&a, TCA_OPTIONS)) {
			struct nl_attrs f;

			nl_attrs_parse(&f, a.val[TCA_OPTIONS],
				       a.len[TCA_OPTIONS]);
			flags = nl_get_u32(&f, TCA_FLOWER_FLAGS, 0);

			pv_comma(out);
			pv_key(out, "offload");
			pv_open(out, '{');
			pv_key(out, "skip_sw");
			pv_bool(out, (flags & TCA_CLS_FLAGS_SKIP_SW) != 0);
			pv_comma(out);
			pv_key(out, "skip_hw");
			pv_bool(out, (flags & TCA_CLS_FLAGS_SKIP_HW) != 0);
			pv_comma(out);
			pv_key(out, "in_hw");
			pv_bool(out, (flags & TCA_CLS_FLAGS_IN_HW) != 0);
			pv_comma(out);
			pv_key(out, "not_in_hw");
			pv_bool(out, (flags & TCA_CLS_FLAGS_NOT_IN_HW) != 0);
			pv_close(out, '}');

			pv_comma(out);
			pv_key(out, "match");
			nl_tc_flower_match(out, &f);

			pv_comma(out);
			pv_key(out, "match_unparsed");
			nl_tc_flower_unparsed(out, a.val[TCA_OPTIONS],
					      a.len[TCA_OPTIONS]);

			pv_comma(out);
			pv_key(out, "actions");
			if (nl_has(&f, TCA_FLOWER_ACT))
				nl_tc_actions(out, f.val[TCA_FLOWER_ACT],
					      f.len[TCA_FLOWER_ACT]);
			else
				pv_raw(out, "[]");
		} else {
			pv_comma(out);
			pv_key(out, "options_decoded");
			pv_bool(out, false);
		}

		pv_close(out, '}');

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
	return 0;
}

static const char *pon_state_name(uint32_t state)
{
	static const char *const names[] = {
		"o1", "o2", "o3", "o4", "o5", "o6", "o7",
	};

	if (state < PON_ONU_STATE_O1 || state > PON_ONU_STATE_O7)
		return NULL;
	return names[state - PON_ONU_STATE_O1];
}

static const char *pon_type_name(uint32_t type)
{
	switch (type) {
	case PON_TYPE_GPON:
		return "gpon";
	case PON_TYPE_XGPON:
		return "xgpon";
	case PON_TYPE_XGSPON:
		return "xgs-pon";
	case PON_TYPE_NGPON2:
		return "ng-pon2";
	default:
		return NULL;
	}
}

static void pon_pv_serial(struct prov_buf *out, const char *p, size_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	char t[17];
	size_t i, n = 0;
	bool ascii = true;

	if (len != 8) {
		pv_null(out);
		return;
	}
	for (i = 0; i < 4; i++)
		if (p[i] < 0x20 || p[i] > 0x7e)
			ascii = false;

	for (i = 0; i < 8; i++) {
		unsigned char c = (unsigned char)p[i];

		if (ascii && i < 4) {
			t[n++] = (char)c;
			continue;
		}
		t[n++] = hex[c >> 4];
		t[n++] = hex[c & 0xf];
	}
	t[n] = '\0';
	pv_str(out, t);
}

static void pon_pv_alloc_ids(struct prov_buf *out, const char *p, size_t len)
{
	bool first = true;

	pv_open(out, '[');
	while (len >= NL_ATTR_HDRLEN) {
		uint16_t alen = nl_u16at(p);
		uint16_t type = (uint16_t)(nl_u16at(p + 2) & NLA_TYPE_MASK);
		size_t step;

		if (alen < NL_ATTR_HDRLEN || alen > len)
			break;
		if (type == PON_A_DEV_ALLOC_ID && alen >= NL_ATTR_HDRLEN + 4) {
			if (!first)
				pv_comma(out);
			pv_num(out, nl_u32at(p + NL_ATTR_HDRLEN));
			first = false;
		}
		step = NL_ALIGN(alen);
		if (step > len)
			break;
		len -= step;
		p += step;
	}
	pv_close(out, ']');
}

static void pon_pv_alarms(struct prov_buf *out, uint32_t alarms)
{
	static const struct {
		uint32_t bit;
		const char *name;
	} named[] = {
		{ PON_ALARM_LOS, "los" },
		{ PON_ALARM_LOF, "lof" },
		{ PON_ALARM_LOM, "lom" },
		{ PON_ALARM_PEE, "pee" },
	};
	bool first = true;
	unsigned int i;

	pv_open(out, '[');
	for (i = 0; i < ARRAY_SIZE(named); i++) {
		if (!(alarms & named[i].bit))
			continue;
		if (!first)
			pv_comma(out);
		pv_str(out, named[i].name);
		first = false;
		alarms &= ~named[i].bit;
	}

	if (alarms) {
		if (!first)
			pv_comma(out);
		pv_num(out, alarms);
	}
	pv_close(out, ']');
}

static void pon_pv_stats(struct prov_buf *out, const char *p, size_t len)
{
	static const struct {
		unsigned int attr;
		const char *key;
	} named[] = {
		{ PON_A_DEV_STATS_SN_REQUESTS, "sn_requests" },
		{ PON_A_DEV_STATS_RANGING_REQUESTS, "ranging_requests" },
		{ PON_A_DEV_STATS_PLOAM_RX, "ploam_rx" },
		{ PON_A_DEV_STATS_PLOAM_CRC_ERRORS, "ploam_crc_errors" },
		{ PON_A_DEV_STATS_PLOAM_OVERFLOWS, "ploam_overflows" },
		{ PON_A_DEV_STATS_BWMAP_RX, "bwmap_rx" },
		{ PON_A_DEV_STATS_BWMAP_CRC_ERRORS, "bwmap_crc_errors" },
		{ PON_A_DEV_STATS_BWMAP_OVERFLOWS, "bwmap_overflows" },
		{ PON_A_DEV_STATS_GEM_FRAMES, "gem_frames" },
		{ PON_A_DEV_STATS_BIP_ERRORS, "bip_errors" },
	};
	struct nl_attrs a;
	unsigned int i;

	nl_attrs_parse(&a, p, len);

	pv_open(out, '{');
	for (i = 0; i < ARRAY_SIZE(named); i++) {
		if (i)
			pv_comma(out);
		pv_key(out, named[i].key);
		if (nl_has(&a, named[i].attr))
			pv_num(out,
			       (long long)nl_get_uint(&a, named[i].attr, 0));
		else
			pv_null(out);
	}
	pv_close(out, '}');
}

int prov_nl_tconts(const char *dump, size_t dlen, struct prov_buf *out)
{
	const size_t genl_hdr = sizeof(struct genlmsghdr);
	const char *pay = NULL;
	size_t off = 0, paylen = 0;
	bool first = true;
	uint16_t type;

	pv_open(out, '[');
	while (nl_next_msg(dump, dlen, &off, &type, &pay, &paylen)) {
		struct nl_attrs a;

		if (paylen < genl_hdr)
			continue;
		nl_attrs_parse(&a, pay + genl_hdr, paylen - genl_hdr);

		if (!first)
			pv_comma(out);
		first = false;

		pv_open(out, '{');
		pv_key(out, "onu");
		pv_num(out, nl_get_u32(&a, PON_A_TCONT_ID, 0));
		pv_comma(out);
		pv_key(out, "index");
		pv_num(out, nl_get_u32(&a, PON_A_TCONT_INDEX, 0));
		pv_comma(out);
		pv_key(out, "alloc_id");
		if (nl_has(&a, PON_A_TCONT_ALLOC_ID))
			pv_num(out, nl_get_u32(&a, PON_A_TCONT_ALLOC_ID, 0));
		else
			pv_null(out);
		pv_close(out, '}');
	}
	pv_close(out, ']');
	return 0;
}

int prov_nl_onu(const char *links, size_t llen, const char *dev, size_t dlen,
		struct prov_buf *out)
{
	const size_t genl_hdr = sizeof(struct genlmsghdr);
	struct nl_ifmap map;
	struct nl_attrs a;
	const char *pay = NULL;
	size_t off = 0, paylen = 0;
	const char *name;
	uint16_t type;

	if (!nl_next_msg(dev, dlen, &off, &type, &pay, &paylen) ||
	    paylen < genl_hdr) {
		pv_null(out);
		return 0;
	}

	nl_attrs_parse(&a, pay + genl_hdr, paylen - genl_hdr);
	nl_ifmap_build(&map, links, llen);

	pv_open(out, '{');
	pv_key(out, "id");
	pv_num(out, nl_get_u32(&a, PON_A_DEV_ID, 0));

	pv_comma(out);
	pv_key(out, "num_gem_ports");
	if (nl_has(&a, PON_A_DEV_NUM_GEM_PORTS))
		pv_num(out, nl_get_u32(&a, PON_A_DEV_NUM_GEM_PORTS, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "num_tconts");
	if (nl_has(&a, PON_A_DEV_NUM_TCONTS))
		pv_num(out, nl_get_u32(&a, PON_A_DEV_NUM_TCONTS, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "interface");
	if (nl_has(&a, PON_A_DEV_IFINDEX))
		nl_pv_ifname(out, &map,
			     (int32_t)nl_get_u32(&a, PON_A_DEV_IFINDEX, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "type");
	name = pon_type_name(nl_get_u32(&a, PON_A_DEV_TYPE, 0));
	if (name)
		pv_str(out, name);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "state");
	name = pon_state_name(nl_get_u32(&a, PON_A_DEV_ONU_STATE, 0));
	if (name)
		pv_str(out, name);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "onu_id");
	if (nl_has(&a, PON_A_DEV_ONU_ID))
		pv_num(out, nl_get_u32(&a, PON_A_DEV_ONU_ID, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "alloc_ids");
	pon_pv_alloc_ids(out, pay + genl_hdr, paylen - genl_hdr);

	pv_comma(out);
	pv_key(out, "omcc_port_id");
	if (nl_has(&a, PON_A_DEV_OMCC_PORT_ID))
		pv_num(out, nl_get_u32(&a, PON_A_DEV_OMCC_PORT_ID, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "alarms");
	if (nl_has(&a, PON_A_DEV_ALARMS))
		pon_pv_alarms(out, nl_get_u32(&a, PON_A_DEV_ALARMS, 0));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "counters");
	if (nl_has(&a, PON_A_DEV_STATS))
		pon_pv_stats(out, a.val[PON_A_DEV_STATS],
			     a.len[PON_A_DEV_STATS]);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "serial");
	if (nl_has(&a, PON_A_DEV_SERIAL_NUMBER))
		pon_pv_serial(out, a.val[PON_A_DEV_SERIAL_NUMBER],
			      a.len[PON_A_DEV_SERIAL_NUMBER]);
	else
		pv_null(out);

	pv_close(out, '}');
	return 0;
}

#ifndef __linux__

int prov_bridge(struct prov_buf *out)
{
	(void)out;
	return -EOPNOTSUPP;
}

int prov_gem_ports(struct prov_buf *out)
{
	(void)out;
	return -EOPNOTSUPP;
}

int prov_tconts(struct prov_buf *out)
{
	(void)out;
	return -EOPNOTSUPP;
}

int prov_tc_rules(struct prov_buf *out)
{
	(void)out;
	return -EOPNOTSUPP;
}

int prov_onu(struct prov_buf *out)
{
	(void)out;
	return -EOPNOTSUPP;
}
#else

#define NL_DUMP_CAP_LINKS (64 * 1024)
#define NL_DUMP_CAP_OTHER (32 * 1024)

static char nl_links_buf[NL_DUMP_CAP_LINKS];
static char nl_other_buf[NL_DUMP_CAP_OTHER];

static int nl_dump_bus(int bus, struct nlmsghdr *req, char *out, size_t cap,
		       size_t *outlen)
{
	uint32_t rbuf[2048];
	struct mnl_socket *nl;
	unsigned int portid;
	uint32_t seq = req->nlmsg_seq;
	size_t got = 0;
	int err = 0;
	bool done = false;

	nl = mnl_socket_open(bus);
	if (!nl)
		return -errno;
	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		err = -errno;
		goto out;
	}
	portid = mnl_socket_get_portid(nl);

	if (mnl_socket_sendto(nl, req, req->nlmsg_len) < 0) {
		err = -errno;
		goto out;
	}

	while (!done) {
		ssize_t n = mnl_socket_recvfrom(nl, rbuf, sizeof(rbuf));
		const char *p = (const char *)rbuf;
		size_t left;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			goto out;
		}
		if (n == 0)
			break;
		left = (size_t)n;

		while (left >= NL_HDRLEN) {
			uint32_t mlen = nl_u32at(p);
			uint16_t mtype = nl_u16at(p + 4);
			size_t step;

			if (mlen < NL_HDRLEN || mlen > left) {
				err = -EPROTO;
				goto out;
			}

			if (nl_u32at(p + 8) != seq ||
			    nl_u32at(p + 12) != portid) {
				err = -EPROTO;
				goto out;
			}
			if (mtype == NLMSG_ERROR) {
				int kerr;

				if (mlen < NL_HDRLEN + sizeof(kerr)) {
					err = -EPROTO;
					goto out;
				}
				memcpy(&kerr, p + NL_HDRLEN, sizeof(kerr));
				err = kerr ? kerr : -EPROTO;
				goto out;
			}

			if (nl_u16at(p + 6) & NLM_F_DUMP_INTR) {
				err = -EINTR;
				goto out;
			}
			if (mtype == NLMSG_DONE) {
				done = true;
				break;
			}

			step = NL_ALIGN(mlen);
			if (got + step > cap) {
				err = -EFBIG;
				goto out;
			}
			memcpy(out + got, p, mlen);
			if (step > mlen)
				memset(out + got + mlen, 0, step - mlen);
			got += step;

			if (step > left)
				break;
			left -= step;
			p += step;
		}
	}

	*outlen = got;
out:
	mnl_socket_close(nl);
	return err;
}

static int nl_dump(struct nlmsghdr *req, char *out, size_t cap, size_t *outlen)
{
	return nl_dump_bus(NETLINK_ROUTE, req, out, cap, outlen);
}

#define NL_REQ_WORDS 64

static int nl_dump_links(char *out, size_t cap, size_t *outlen)
{
	uint32_t req[NL_REQ_WORDS];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(req);
	struct ifinfomsg *ifi;

	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 1;
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifi));
	ifi->ifi_family = AF_UNSPEC;

	return nl_dump(nlh, out, cap, outlen);
}

static int nl_dump_bridge_links(char *out, size_t cap, size_t *outlen)
{
	uint32_t req[NL_REQ_WORDS];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(req);
	struct ifinfomsg *ifi;

	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 2;
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifi));
	ifi->ifi_family = AF_BRIDGE;
	mnl_attr_put_u32(nlh, IFLA_EXT_MASK, RTEXT_FILTER_BRVLAN);

	return nl_dump(nlh, out, cap, outlen);
}

static int nl_dump_fdb(char *out, size_t cap, size_t *outlen)
{
	uint32_t req[NL_REQ_WORDS];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(req);
	struct ndmsg *ndm;

	nlh->nlmsg_type = RTM_GETNEIGH;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 3;
	ndm = mnl_nlmsg_put_extra_header(nlh, sizeof(*ndm));
	ndm->ndm_family = AF_BRIDGE;

	return nl_dump(nlh, out, cap, outlen);
}

static int nl_dump_filters(unsigned int ifindex, char *out, size_t cap,
			   size_t *outlen)
{
	uint32_t req[NL_REQ_WORDS];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(req);
	struct tcmsg *tcm;

	nlh->nlmsg_type = RTM_GETTFILTER;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 4;
	tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
	tcm->tcm_family = AF_UNSPEC;
	tcm->tcm_ifindex = (int)ifindex;
	tcm->tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS);

	return nl_dump(nlh, out, cap, outlen);
}

int prov_bridge(struct prov_buf *out)
{
	size_t llen = 0, flen = 0;
	size_t mark = out->len;
	int err;

	err = nl_dump_links(nl_links_buf, sizeof(nl_links_buf), &llen);
	if (err)
		return err;
	err = nl_dump_fdb(nl_other_buf, sizeof(nl_other_buf), &flen);
	if (err)
		return err;

	pv_open(out, '{');
	pv_key(out, "fdb");
	prov_nl_fdb(nl_links_buf, llen, nl_other_buf, flen, out);

	err = nl_dump_bridge_links(nl_links_buf, sizeof(nl_links_buf), &llen);
	if (err) {
		out->len = mark;
		out->buf[mark] = '\0';
		out->overflow = false;
		return err;
	}
	pv_comma(out);
	pv_key(out, "vlans");
	prov_nl_vlans(nl_links_buf, llen, out);
	pv_close(out, '}');
	return 0;
}

int prov_gem_ports(struct prov_buf *out)
{
	size_t llen = 0;
	int err = nl_dump_links(nl_links_buf, sizeof(nl_links_buf), &llen);

	if (err)
		return err;
	return prov_nl_gem_ports(nl_links_buf, llen, out);
}

int prov_tc_rules(struct prov_buf *out)
{
	static const char *const ports[] = { "sfp", "pon", NULL };
	unsigned int i;

	pv_open(out, '{');
	for (i = 0; ports[i]; i++) {
		unsigned int idx = if_nametoindex(ports[i]);
		size_t flen = 0;
		int err;

		if (i)
			pv_comma(out);
		pv_key(out, ports[i]);

		if (!idx) {
			char why[96];

			snprintf(why, sizeof(why), "{\"error\":\"%s\"}",
				 strerror(errno));
			pv_raw(out, why);
			continue;
		}

		err = nl_dump_filters(idx, nl_other_buf, sizeof(nl_other_buf),
				      &flen);
		if (err) {
			char why[96];

			snprintf(why, sizeof(why), "{\"error\":\"%s\"}",
				 strerror(-err));
			pv_raw(out, why);
			continue;
		}

		pv_open(out, '{');
		pv_key(out, "ingress");
		prov_nl_filters(nl_other_buf, flen, out);
		pv_close(out, '}');
	}
	pv_close(out, '}');
	return 0;
}

static int nl_genl_family(const char *name, uint16_t *id)
{
	uint32_t req[NL_REQ_WORDS];
	uint32_t rbuf[2048];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(req);
	struct genlmsghdr *gh;
	struct mnl_socket *nl;
	struct nl_attrs a;
	unsigned int portid;
	uint32_t seq = 5;
	const char *p = (const char *)rbuf;
	uint32_t mlen;
	ssize_t n;
	int err = 0;

	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = seq;
	gh = mnl_nlmsg_put_extra_header(nlh, sizeof(*gh));
	gh->cmd = CTRL_CMD_GETFAMILY;
	gh->version = 1;
	mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, name);

	nl = mnl_socket_open(NETLINK_GENERIC);
	if (!nl)
		return -errno;
	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		err = -errno;
		goto out;
	}
	portid = mnl_socket_get_portid(nl);

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		err = -errno;
		goto out;
	}

	do {
		n = mnl_socket_recvfrom(nl, rbuf, sizeof(rbuf));
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		err = -errno;
		goto out;
	}
	if ((size_t)n < NL_HDRLEN + sizeof(*gh)) {
		err = -EPROTO;
		goto out;
	}

	mlen = nl_u32at(p);
	if (mlen < NL_HDRLEN || mlen > (size_t)n || nl_u32at(p + 8) != seq ||
	    nl_u32at(p + 12) != portid) {
		err = -EPROTO;
		goto out;
	}
	if (nl_u16at(p + 4) == NLMSG_ERROR) {
		int kerr = 0;

		if (mlen >= NL_HDRLEN + sizeof(kerr))
			memcpy(&kerr, p + NL_HDRLEN, sizeof(kerr));

		err = kerr ? kerr : -EPROTO;
		goto out;
	}

	nl_attrs_parse(&a, p + NL_HDRLEN + sizeof(*gh),
		       mlen - NL_HDRLEN - sizeof(*gh));
	if (!nl_has(&a, CTRL_ATTR_FAMILY_ID)) {
		err = -EPROTO;
		goto out;
	}
	*id = nl_get_u16(&a, CTRL_ATTR_FAMILY_ID, 0);
out:
	mnl_socket_close(nl);
	return err;
}

static int nl_dump_onu(char *out, size_t cap, size_t *outlen)
{
	uint32_t req[NL_REQ_WORDS];
	struct nlmsghdr *nlh;
	struct genlmsghdr *gh;
	uint16_t family = 0;
	int err = nl_genl_family(PON_FAMILY_NAME, &family);

	if (err)
		return err;

	nlh = mnl_nlmsg_put_header(req);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 6;
	gh = mnl_nlmsg_put_extra_header(nlh, sizeof(*gh));
	gh->cmd = PON_CMD_DEV_GET;
	gh->version = PON_FAMILY_VERSION;

	return nl_dump_bus(NETLINK_GENERIC, nlh, out, cap, outlen);
}

static int nl_dump_tconts(char *out, size_t cap, size_t *outlen)
{
	uint32_t req[NL_REQ_WORDS];
	struct nlmsghdr *nlh;
	struct genlmsghdr *gh;
	uint16_t family = 0;
	int err = nl_genl_family(PON_FAMILY_NAME, &family);

	if (err)
		return err;

	nlh = mnl_nlmsg_put_header(req);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 7;
	gh = mnl_nlmsg_put_extra_header(nlh, sizeof(*gh));
	gh->cmd = PON_CMD_TCONT_GET;
	gh->version = PON_FAMILY_VERSION;

	return nl_dump_bus(NETLINK_GENERIC, nlh, out, cap, outlen);
}

int prov_tconts(struct prov_buf *out)
{
	size_t dlen = 0;
	int err = nl_dump_tconts(nl_other_buf, sizeof(nl_other_buf), &dlen);

	if (err)
		return err;

	return prov_nl_tconts(nl_other_buf, dlen, out);
}

int prov_onu(struct prov_buf *out)
{
	size_t llen = 0, dlen = 0;
	int err;

	err = nl_dump_links(nl_links_buf, sizeof(nl_links_buf), &llen);
	if (err)
		return err;
	err = nl_dump_onu(nl_other_buf, sizeof(nl_other_buf), &dlen);
	if (err)
		return err;

	return prov_nl_onu(nl_links_buf, llen, nl_other_buf, dlen, out);
}
#endif
