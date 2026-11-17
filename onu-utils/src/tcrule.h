/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The traffic control the stick's scripts set, as route netlink requests
 * (ADR 0017): the clsact qdisc of an interface, which filters hang
 * from, and filters on its ingress or its egress that take the frames of a
 * VLAN, those without a tag, or every frame, and trap them to the CPU, drop
 * them, hand them to the ingress of another interface, or push, change or
 * pop their 802.1Q tag. The requests are those tc makes for
 *
 *   tc qdisc add|del dev DEV clsact
 *   tc filter add dev DEV ingress|egress [pref N] protocol 802.1Q flower
 *	[skip_sw|skip_hw] vlan_id VID action ACTION
 *   tc filter add dev DEV ingress|egress [pref N] protocol all flower
 *	[skip_sw|skip_hw] num_of_vlans 0 action ACTION
 *   tc filter add dev DEV ingress|egress [pref N] protocol ip flower
 *	[skip_sw|skip_hw] dst_ip ADDRESS/LEN action ACTION
 *   tc filter add dev DEV ingress|egress [pref N] matchall [skip_sw|skip_hw]
 *	action ACTION
 *   tc filter del dev DEV ingress|egress pref N
 *   tc filter show dev DEV ingress
 *
 * with ACTION one of trap, drop, mirred ingress redirect dev TO, and vlan
 * push id VID, vlan modify id VID or vlan pop, each of which passes the frame
 * on (pipe)
 *
 * so that the kernel, and the switch driver below it, take them as they
 * take tc's.
 */

#ifndef ONU_UTILS_TCRULE_H
#define ONU_UTILS_TCRULE_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/netlink.h>

enum tc_rule_action {
	TC_RULE_TRAP,
	TC_RULE_DROP,
	TC_RULE_REDIRECT,
	TC_RULE_PUSH,
	TC_RULE_MODIFY,
	TC_RULE_POP,
};

/**
 * struct tc_rule - A filter on the ingress or the egress of an interface.
 * @ifindex: the interface
 * @egress: on its egress; without, on its ingress
 * @pref: its priority, 1 to 65535; 0 has the kernel pick one, as tc does
 *	when it is given none
 * @has_vlan: it takes the frames whose outer tag, 802.1Q, has VID @vlan;
 *	without, every frame
 * @vlan: that VID, 0 to 4095
 * @untagged: it takes the frames without a tag (num_of_vlans 0); not with
 *	@has_vlan
 * @has_dst: it takes the IPv4 packets to an address of @dst/@dst_len; not
 *	with @has_vlan or @untagged
 * @dst: that address, in network order
 * @dst_len: the length of its prefix, 0 to 32
 * @hw: in the switch alone, never in software (tc's skip_sw)
 * @sw: in software alone, never in the switch (tc's skip_hw); not with @hw
 * @action: what it does with what it takes
 * @to_ifindex: for TC_RULE_REDIRECT, the interface whose ingress the frames
 *	go to
 * @act_vlan: for TC_RULE_PUSH and TC_RULE_MODIFY, the VID of the tag, 1 to
 *	4094
 */
struct tc_rule {
	int ifindex;
	bool egress;
	uint16_t pref;
	bool has_vlan;
	uint16_t vlan;
	bool untagged;
	bool has_dst;
	uint32_t dst;
	uint8_t dst_len;
	bool hw;
	bool sw;
	enum tc_rule_action action;
	int to_ifindex;
	uint16_t act_vlan;
};

/*
 * Each request is built in @buf, of MNL_SOCKET_BUFFER_SIZE bytes, with
 * sequence number @seq, and asks for the kernel's acknowledgement, but for
 * the dump, whose answer ends by itself.
 */
struct nlmsghdr *tc_clsact_msg(char *buf, int ifindex, bool add, uint32_t seq);
struct nlmsghdr *tc_rule_msg(char *buf, const struct tc_rule *r, uint32_t seq);
/*
 * Remove the filters of priority @pref, whatever their protocol, on the
 * ingress of @ifindex or, with @egress, on its egress.
 */
struct nlmsghdr *tc_rule_del_msg(char *buf, int ifindex, bool egress,
				 uint16_t pref, uint32_t seq);
/* A dump of the filters on the ingress of @ifindex. */
struct nlmsghdr *tc_rule_dump_msg(char *buf, int ifindex, uint32_t seq);

/* The priority of a filter in a message of the dump; 0 for another message. */
unsigned int tc_rule_pref(const struct nlmsghdr *nlh);

#endif
