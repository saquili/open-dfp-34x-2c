/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * An entry of the firewall's ACL (fwacl.h) as the route netlink request tc
 * makes for
 *
 *   tc filter add dev DEV ingress pref N protocol ip|ppp_ses flower [skip_sw]
 *	[ppp_proto ip] [src_mac MAC] [src_ip NET] [dst_ip NET] [ip_proto P]
 *	[src_port V/M] [dst_port V/M] [type T] action pass|drop
 *
 * (acl_entry_format() prints it), so that the kernel and the switch driver
 * below it take it as they take tc's. The clsact qdisc, a filter's removal
 * and the dump of the priorities are tcrule.h's.
 */

#ifndef ONU_UTILS_ACLMSG_H
#define ONU_UTILS_ACLMSG_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/netlink.h>

#include "fwacl.h"

/*
 * The request that adds @e on the ingress of @ifindex, in the switch alone
 * when @hw, built in @buf of MNL_SOCKET_BUFFER_SIZE bytes with sequence
 * number @seq, asking for the kernel's acknowledgement.
 */
struct nlmsghdr *acl_entry_msg(char *buf, int ifindex, const struct acl_entry *e, bool hw,
			       uint32_t seq);

#endif /* ONU_UTILS_ACLMSG_H */
