/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The traffic control the stick's scripts set, as route netlink requests
 * (ADR 0014, step 9g): the clsact qdisc of an interface, which filters hang
 * from, and filters on its ingress that take the frames of a VLAN, or every
 * frame, and trap them to the CPU, drop them or hand them to the ingress of
 * another interface. The requests are those tc makes for
 *
 *   tc qdisc add|del dev DEV clsact
 *   tc filter add dev DEV ingress [pref N] protocol 802.1Q flower [skip_sw]
 *	vlan_id VID action trap|drop|mirred ingress redirect dev TO
 *   tc filter add dev DEV ingress [pref N] matchall [skip_sw]
 *	action trap|drop|mirred ingress redirect dev TO
 *   tc filter del dev DEV ingress pref N
 *   tc filter show dev DEV ingress
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
};

/**
 * struct tc_rule - A filter on the ingress of an interface.
 * @ifindex: the interface
 * @pref: its priority, 1 to 65535; 0 has the kernel pick one, as tc does
 *	when it is given none
 * @has_vlan: it takes the frames whose outer tag, 802.1Q, has VID @vlan;
 *	without, every frame
 * @vlan: that VID, 0 to 4095
 * @hw: in the switch alone, never in software (tc's skip_sw)
 * @action: what it does with what it takes
 * @to_ifindex: for TC_RULE_REDIRECT, the interface whose ingress the frames
 *	go to
 */
struct tc_rule {
	int ifindex;
	uint16_t pref;
	bool has_vlan;
	uint16_t vlan;
	bool hw;
	enum tc_rule_action action;
	int to_ifindex;
};

/*
 * Each request is built in @buf, of MNL_SOCKET_BUFFER_SIZE bytes, with
 * sequence number @seq, and asks for the kernel's acknowledgement, but for
 * the dump, whose answer ends by itself.
 */
struct nlmsghdr *tc_clsact_msg(char *buf, int ifindex, bool add, uint32_t seq);
struct nlmsghdr *tc_rule_msg(char *buf, const struct tc_rule *r, uint32_t seq);
/* Remove the filters of priority @pref, whatever their protocol. */
struct nlmsghdr *tc_rule_del_msg(char *buf, int ifindex, uint16_t pref,
				 uint32_t seq);
/* A dump of the filters on the ingress of @ifindex. */
struct nlmsghdr *tc_rule_dump_msg(char *buf, int ifindex, uint32_t seq);

/* The priority of a filter in a message of the dump; 0 for another message. */
unsigned int tc_rule_pref(const struct nlmsghdr *nlh);

#endif
