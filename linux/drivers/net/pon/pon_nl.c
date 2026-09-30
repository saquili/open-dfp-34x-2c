// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/pon.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "pon_nl.h"

#include <uapi/linux/pon.h>

/* PON_CMD_DEV_GET - do */
static const struct nla_policy pon_dev_get_nl_policy[PON_A_DEV_ID + 1] = {
	[PON_A_DEV_ID] = { .type = NLA_U32, },
};

/* PON_CMD_TCONT_GET - do */
static const struct nla_policy pon_tcont_get_nl_policy[PON_A_TCONT_INDEX + 1] = {
	[PON_A_TCONT_ID] = { .type = NLA_U32, },
	[PON_A_TCONT_INDEX] = { .type = NLA_U32, },
};

/* Ops table for pon */
static const struct genl_split_ops pon_nl_ops[] = {
	{
		.cmd		= PON_CMD_DEV_GET,
		.doit		= pon_nl_dev_get_doit,
		.policy		= pon_dev_get_nl_policy,
		.maxattr	= PON_A_DEV_ID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd	= PON_CMD_DEV_GET,
		.dumpit	= pon_nl_dev_get_dumpit,
		.flags	= GENL_ADMIN_PERM | GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= PON_CMD_TCONT_GET,
		.doit		= pon_nl_tcont_get_doit,
		.policy		= pon_tcont_get_nl_policy,
		.maxattr	= PON_A_TCONT_INDEX,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd	= PON_CMD_TCONT_GET,
		.dumpit	= pon_nl_tcont_get_dumpit,
		.flags	= GENL_ADMIN_PERM | GENL_CMD_CAP_DUMP,
	},
};

static const struct genl_multicast_group pon_nl_mcgrps[] = {
	[PON_NLGRP_MONITOR] = { "monitor", },
};

struct genl_family pon_nl_family __ro_after_init = {
	.name		= PON_FAMILY_NAME,
	.version	= PON_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= pon_nl_ops,
	.n_split_ops	= ARRAY_SIZE(pon_nl_ops),
	.mcgrps		= pon_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(pon_nl_mcgrps),
};
