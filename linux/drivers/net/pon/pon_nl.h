/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/pon.yaml */
/* YNL-GEN kernel header */

#ifndef _LINUX_PON_GEN_H
#define _LINUX_PON_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/pon.h>

int pon_nl_dev_get_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_dev_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int pon_nl_tcont_get_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_tcont_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb);
int pon_nl_tcont_set_doit(struct sk_buff *skb, struct genl_info *info);
int pon_nl_tod_set_doit(struct sk_buff *skb, struct genl_info *info);

enum {
	PON_NLGRP_MONITOR,
};

extern struct genl_family pon_nl_family;

#endif /* _LINUX_PON_GEN_H */
