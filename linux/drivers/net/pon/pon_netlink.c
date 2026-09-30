// SPDX-License-Identifier: GPL-2.0-only
/*
 * The pon generic netlink family: a dev object for each ONU, a tcont object
 * for each of its T-CONTs, and dev-change-ntf on the monitor group. The
 * policies and the operation table (pon_nl.c) are generated from
 * Documentation/netlink/specs/pon.yaml.
 *
 * The objects are registered by the MAC drivers, as in the dpll family:
 * userspace reads them and is told of changes, but can neither create nor
 * change any, because the kernel runs the transmission convergence layer.
 * The family carries activation, ranging, identifiers and allocations, which
 * a network interface has no field for; the ONU's traffic stays on its GEM
 * interfaces. The handlers run without the genetlink mutex, each holding a
 * reference on the PON device it reports. Requests are answered in every
 * network namespace, and notifications go to the initial one.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pon.h>
#include <linux/skbuff.h>
#include <net/genetlink.h>
#include <net/netlink.h>

#include "pon_core.h"
#include "pon_nl.h"

/*
 * "Nothing known": ONU-ID and OMCC Port-ID -1, since 0 is a valid value of
 * both, and everything else 0.
 */
static void pon_nl_status_init(struct pon_dev_status *st)
{
	*st = (struct pon_dev_status){
		.onu_id = -1,
		.omcc_port_id = -1,
	};
}

/*
 * A MAC without dev_status, or one whose call fails, gives a report of the id
 * and the two capacities only: a half-filled status would mix measurements
 * and zeros.
 */
static void pon_nl_status_get(struct pon_device *pon, struct pon_dev_status *st)
{
	pon_nl_status_init(st);
	if (pon_device_status(pon, st))
		pon_nl_status_init(st);
}

/*
 * Every counter is sent, zeros included, since a zero is a measurement. As
 * uint attributes they take 4 bytes while the value fits in 32 bits and 8
 * bytes after, so a counter can pass 2^32 without a change of the ABI.
 */
static int pon_nl_stats_fill(struct sk_buff *msg,
			     const struct pon_dev_stats *s)
{
	static const struct {
		unsigned int	attr;
		size_t		off;
	} counters[] = {
		{ PON_A_DEV_STATS_SN_REQUESTS,
		  offsetof(struct pon_dev_stats, sn_requests) },
		{ PON_A_DEV_STATS_RANGING_REQUESTS,
		  offsetof(struct pon_dev_stats, ranging_requests) },
		{ PON_A_DEV_STATS_PLOAM_RX,
		  offsetof(struct pon_dev_stats, ploam_rx) },
		{ PON_A_DEV_STATS_PLOAM_CRC_ERRORS,
		  offsetof(struct pon_dev_stats, ploam_crc_errors) },
		{ PON_A_DEV_STATS_PLOAM_OVERFLOWS,
		  offsetof(struct pon_dev_stats, ploam_overflows) },
		{ PON_A_DEV_STATS_BWMAP_RX,
		  offsetof(struct pon_dev_stats, bwmap_rx) },
		{ PON_A_DEV_STATS_BWMAP_CRC_ERRORS,
		  offsetof(struct pon_dev_stats, bwmap_crc_errors) },
		{ PON_A_DEV_STATS_BWMAP_OVERFLOWS,
		  offsetof(struct pon_dev_stats, bwmap_overflows) },
		{ PON_A_DEV_STATS_GEM_FRAMES,
		  offsetof(struct pon_dev_stats, gem_frames) },
		{ PON_A_DEV_STATS_BIP_ERRORS,
		  offsetof(struct pon_dev_stats, bip_errors) },
	};
	struct nlattr *nest;
	unsigned int i;

	nest = nla_nest_start(msg, PON_A_DEV_STATS);
	if (!nest)
		return -EMSGSIZE;

	for (i = 0; i < ARRAY_SIZE(counters); i++) {
		u64 v = *(const u64 *)((const char *)s + counters[i].off);

		if (nla_put_uint(msg, counters[i].attr, v)) {
			nla_nest_cancel(msg, nest);
			return -EMSGSIZE;
		}
	}

	nla_nest_end(msg, nest);
	return 0;
}

/*
 * What the MAC did not set is left out, not sent as 0: 0 is a valid ONU-ID,
 * Port-ID and Alloc-ID, and "no alarms" is a claim a MAC in reset cannot
 * make.
 */
static int pon_nl_dev_fill(struct sk_buff *msg, struct pon_device *pon,
			   const struct pon_dev_status *st, u32 portid,
			   u32 seq, int flags, u8 cmd)
{
	unsigned int i;
	void *hdr;

	hdr = genlmsg_put(msg, portid, seq, &pon_nl_family, flags, cmd);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(msg, PON_A_DEV_ID, pon->id))
		goto cancel;
	if (st->ifindex &&
	    nla_put_u32(msg, PON_A_DEV_IFINDEX, st->ifindex))
		goto cancel;
	if (st->type && nla_put_u32(msg, PON_A_DEV_TYPE, st->type))
		goto cancel;
	if (st->onu_state &&
	    nla_put_u32(msg, PON_A_DEV_ONU_STATE, st->onu_state))
		goto cancel;
	if (st->onu_id >= 0 &&
	    nla_put_u32(msg, PON_A_DEV_ONU_ID, st->onu_id))
		goto cancel;
	for (i = 0; i < st->num_alloc_ids; i++)
		if (nla_put_u32(msg, PON_A_DEV_ALLOC_ID, st->alloc_id[i]))
			goto cancel;
	if (st->omcc_port_id >= 0 &&
	    nla_put_u32(msg, PON_A_DEV_OMCC_PORT_ID, st->omcc_port_id))
		goto cancel;
	if (st->have_alarms &&
	    nla_put_u32(msg, PON_A_DEV_ALARMS, st->alarms))
		goto cancel;
	if (st->have_stats && pon_nl_stats_fill(msg, &st->stats))
		goto cancel;
	if (st->have_serial &&
	    nla_put(msg, PON_A_DEV_SERIAL_NUMBER, sizeof(st->serial_number),
		    st->serial_number))
		goto cancel;

	/*
	 * The two capacities are always sent, as the data path registered
	 * them. num-gem-ports is num_flows, the size of the MAC's flow space,
	 * which may include flows the MAC never gives to a GEM interface. Both
	 * are 0 until a data path registers; once it has unregistered,
	 * num_flows is 0 again and num_tconts keeps its value.
	 */
	if (nla_put_u32(msg, PON_A_DEV_NUM_GEM_PORTS, pon->num_flows) ||
	    nla_put_u32(msg, PON_A_DEV_NUM_TCONTS, pon->num_tconts))
		goto cancel;

	genlmsg_end(msg, hdr);
	return 0;

cancel:
	genlmsg_cancel(msg, hdr);
	return -EMSGSIZE;
}

/*
 * alloc-id is left out while the T-CONT is idle (-ENOENT). Any other error
 * from tcont_get cancels the message: a get then fails with -EMSGSIZE, and a
 * dump ends at that T-CONT.
 */
static int pon_nl_tcont_fill(struct sk_buff *msg, struct pon_device *pon,
			     unsigned int index, u32 portid, u32 seq, int flags)
{
	u16 alloc_id;
	void *hdr;
	int err;

	hdr = genlmsg_put(msg, portid, seq, &pon_nl_family, flags,
			  PON_CMD_TCONT_GET);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(msg, PON_A_TCONT_ID, pon->id) ||
	    nla_put_u32(msg, PON_A_TCONT_INDEX, index))
		goto cancel;

	err = pon->ops->tcont_get(pon, index, &alloc_id);
	if (!err && nla_put_u32(msg, PON_A_TCONT_ALLOC_ID, alloc_id))
		goto cancel;
	if (err && err != -ENOENT)
		goto cancel;

	genlmsg_end(msg, hdr);
	return 0;

cancel:
	genlmsg_cancel(msg, hdr);
	return -EMSGSIZE;
}

/**
 * pon_nl_tcont_get_doit() - answer a tcont-get request for one T-CONT
 * @skb: the request
 * @info: the request's attributes
 *
 * The MAC's operations are used without the registration lock, and without
 * a check that the ONU has a registered data path: on an ONU that has its
 * OMCI port and no data path, pon->ops is NULL.
 *
 * Return: 0; -EINVAL without id or index; -ENODEV for an unknown ONU;
 * -ENOENT when the MAC has no tcont_get or the index is not below
 * num_tconts; or another negative error code.
 */
int pon_nl_tcont_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_device *pon;
	struct sk_buff *msg;
	unsigned int index;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_TCONT_ID) ||
	    GENL_REQ_ATTR_CHECK(info, PON_A_TCONT_INDEX))
		return -EINVAL;

	pon = pon_device_get_by_id(nla_get_u32(info->attrs[PON_A_TCONT_ID]));
	if (!pon) {
		NL_SET_ERR_MSG_ATTR(info->extack, info->attrs[PON_A_TCONT_ID],
				    "no ONU with that id");
		return -ENODEV;
	}

	index = nla_get_u32(info->attrs[PON_A_TCONT_INDEX]);
	if (!pon->ops->tcont_get || index >= pon->num_tconts) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[PON_A_TCONT_INDEX],
				    "no such T-CONT on this ONU");
		pon_device_put(pon);
		return -ENOENT;
	}

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg) {
		pon_device_put(pon);
		return -ENOMEM;
	}

	err = pon_nl_tcont_fill(msg, pon, index, info->snd_portid,
				info->snd_seq, 0);
	pon_device_put(pon);
	if (err) {
		nlmsg_free(msg);
		return err;
	}

	return genlmsg_reply(msg, info);
}

/**
 * pon_nl_tcont_get_dumpit() - dump every T-CONT of every ONU
 * @skb: the part of the dump to fill
 * @cb: the dump, whose args[0] and args[1] hold the id of the ONU and the
 *	index of the T-CONT to report next
 *
 * ONUs whose MAC has no tcont_get are skipped. As in a get, the MAC's
 * operations are used without the registration lock, and without a check
 * that the ONU has a registered data path.
 *
 * Return: the length of @skb; 0 ends the dump.
 */
int pon_nl_tcont_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct pon_device *pon;
	u32 id = cb->args[0];
	unsigned int index = cb->args[1];

	while ((pon = pon_device_get_next(id))) {
		if (!pon->ops->tcont_get) {
			id = pon->id + 1;
			index = 0;
			pon_device_put(pon);
			continue;
		}
		for (; index < pon->num_tconts; index++) {
			if (pon_nl_tcont_fill(skb, pon, index,
					      NETLINK_CB(cb->skb).portid,
					      cb->nlh->nlmsg_seq, NLM_F_MULTI)) {
				cb->args[0] = pon->id;
				cb->args[1] = index;
				pon_device_put(pon);
				return skb->len;
			}
		}
		id = pon->id + 1;
		index = 0;
		pon_device_put(pon);
	}

	cb->args[0] = id;
	cb->args[1] = 0;
	return skb->len;
}

/**
 * pon_nl_dev_get_doit() - answer a dev-get request for one ONU
 * @skb: the request
 * @info: the request's attributes
 *
 * Return: 0; -EINVAL without id; -ENODEV for an unknown ONU; or another
 * negative error code.
 */
int pon_nl_dev_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev_status st;
	struct pon_device *pon;
	struct sk_buff *msg;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_DEV_ID))
		return -EINVAL;

	pon = pon_device_get_by_id(nla_get_u32(info->attrs[PON_A_DEV_ID]));
	if (!pon) {
		NL_SET_ERR_MSG_ATTR(info->extack, info->attrs[PON_A_DEV_ID],
				    "no ONU with that id");
		return -ENODEV;
	}

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg) {
		pon_device_put(pon);
		return -ENOMEM;
	}

	pon_nl_status_get(pon, &st);
	err = pon_nl_dev_fill(msg, pon, &st, info->snd_portid, info->snd_seq, 0,
			      PON_CMD_DEV_GET);
	pon_device_put(pon);
	if (err) {
		nlmsg_free(msg);
		return err;
	}

	return genlmsg_reply(msg, info);
}

/**
 * pon_nl_dev_get_dumpit() - dump every ONU
 * @skb: the part of the dump to fill
 * @cb: the dump, whose args[0] holds the lowest id not yet visited
 *
 * The cursor moves past an ONU before its message is built. So a dump never
 * repeats an ONU, and an ONU registered during the dump appears if its id is
 * still ahead; an ONU whose message does not fit in @skb is not carried over
 * to the next part.
 *
 * Return: the length of @skb while the dump goes on, 0 when it is complete,
 * or a negative error code.
 */
int pon_nl_dev_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct pon_dev_status st;
	struct pon_device *pon;
	int err = 0;

	while ((pon = pon_device_get_next(cb->args[0]))) {
		cb->args[0] = pon->id + 1;
		pon_nl_status_get(pon, &st);
		err = pon_nl_dev_fill(skb, pon, &st,
				      NETLINK_CB(cb->skb).portid,
				      cb->nlh->nlmsg_seq, NLM_F_MULTI,
				      PON_CMD_DEV_GET);
		pon_device_put(pon);
		if (err)
			break;
	}

	return err == -EMSGSIZE ? skb->len : err;
}

void pon_device_notify(struct pon_device *pon, const struct pon_dev_status *st)
{
	struct sk_buff *msg;

	/*
	 * To the monitor group of the initial network namespace, and only when
	 * it has listeners. The group has no permission flag: any process may
	 * join it and receive what a dev-get reply holds, serial number
	 * included, while dev-get itself needs CAP_NET_ADMIN.
	 */
	if (!genl_has_listeners(&pon_nl_family, &init_net, PON_NLGRP_MONITOR))
		return;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return;

	if (pon_nl_dev_fill(msg, pon, st, 0, 0, 0, PON_CMD_DEV_CHANGE_NTF)) {
		nlmsg_free(msg);
		return;
	}

	genlmsg_multicast(&pon_nl_family, msg, 0, PON_NLGRP_MONITOR,
			  GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(pon_device_notify);

/**
 * pon_netlink_init() - register the pon generic netlink family
 *
 * Return: 0 or a negative error code.
 */
int pon_netlink_init(void)
{
	return genl_register_family(&pon_nl_family);
}

/**
 * pon_netlink_exit() - unregister the pon generic netlink family
 */
void pon_netlink_exit(void)
{
	genl_unregister_family(&pon_nl_family);
}
