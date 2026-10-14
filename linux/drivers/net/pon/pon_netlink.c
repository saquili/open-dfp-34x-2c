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
		{ PON_A_DEV_STATS_FEC_CORRECTED_BYTES,
		  offsetof(struct pon_dev_stats, fec_corrected_bytes) },
		{ PON_A_DEV_STATS_FEC_CORRECTED_CODEWORDS,
		  offsetof(struct pon_dev_stats, fec_corrected_codewords) },
		{ PON_A_DEV_STATS_FEC_UNCORRECTABLE_CODEWORDS,
		  offsetof(struct pon_dev_stats, fec_uncorrectable_codewords) },
		{ PON_A_DEV_STATS_FEC_CODEWORDS,
		  offsetof(struct pon_dev_stats, fec_codewords) },
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
	if (st->have_power_level &&
	    nla_put_u32(msg, PON_A_DEV_POWER_LEVEL, st->power_level))
		goto cancel;

	/*
	 * The capacities are always sent, as the data path registered them.
	 * num-gem-ports is num_flows, the size of the MAC's flow space, which
	 * may include flows the MAC never gives to a GEM interface. All are 0
	 * until a data path registers; once it has unregistered, num_flows is
	 * 0 again and the other two keep their values.
	 */
	if (nla_put_u32(msg, PON_A_DEV_NUM_GEM_PORTS, pon->num_flows) ||
	    nla_put_u32(msg, PON_A_DEV_NUM_TCONTS, pon->num_tconts) ||
	    nla_put_u32(msg, PON_A_DEV_NUM_TCONT_QUEUES, pon->num_queues))
		goto cancel;

	genlmsg_end(msg, hdr);
	return 0;

cancel:
	genlmsg_cancel(msg, hdr);
	return -EMSGSIZE;
}

/*
 * alloc-id is left out while the T-CONT is idle (-ENOENT). The MAC is asked
 * first: -ENODEV when the ONU has no such T-CONT now, or the MAC's error,
 * comes back without a message; -EMSGSIZE when @msg has no room.
 */
static int pon_nl_tcont_fill(struct sk_buff *msg, struct pon_device *pon,
			     unsigned int index, u32 portid, u32 seq, int flags)
{
	u16 alloc_id = 0;
	void *hdr;
	int err;

	err = pon_device_tcont(pon, index, &alloc_id);
	if (err && err != -ENOENT)
		return err;

	hdr = genlmsg_put(msg, portid, seq, &pon_nl_family, flags,
			  PON_CMD_TCONT_GET);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(msg, PON_A_TCONT_ID, pon->id) ||
	    nla_put_u32(msg, PON_A_TCONT_INDEX, index) ||
	    (!err && nla_put_u32(msg, PON_A_TCONT_ALLOC_ID, alloc_id))) {
		genlmsg_cancel(msg, hdr);
		return -EMSGSIZE;
	}

	genlmsg_end(msg, hdr);
	return 0;
}

/**
 * pon_nl_tcont_get_doit() - answer a tcont-get request for one T-CONT
 * @skb: the request
 * @info: the request's attributes
 *
 * Return: 0; -EINVAL without id or index; -ENODEV for an unknown ONU;
 * -ENOENT when the ONU has no data path, the MAC has no tcont_get or the
 * index is not below num_tconts; or another negative error code.
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

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg) {
		pon_device_put(pon);
		return -ENOMEM;
	}

	index = nla_get_u32(info->attrs[PON_A_TCONT_INDEX]);
	err = pon_nl_tcont_fill(msg, pon, index, info->snd_portid,
				info->snd_seq, 0);
	pon_device_put(pon);
	if (err) {
		nlmsg_free(msg);
		if (err != -ENODEV)
			return err;
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[PON_A_TCONT_INDEX],
				    "no such T-CONT on this ONU");
		return -ENOENT;
	}

	return genlmsg_reply(msg, info);
}

/**
 * pon_nl_tcont_set_doit() - map one T-CONT to an Alloc-ID, or unmap it
 * @skb: the request
 * @info: the request's attributes
 *
 * Return: 0; -EINVAL without id or index; -ENODEV for an unknown ONU, or one
 * without a data path; -EOPNOTSUPP when the MAC does not map T-CONTs;
 * -ENOENT for an index not below num_tconts; -EBUSY when another T-CONT is
 * mapped to the Alloc-ID; or another negative error code.
 */
int pon_nl_tcont_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *alloc = info->attrs[PON_A_TCONT_ALLOC_ID];
	struct pon_device *pon;
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

	err = pon_device_tcont_set(pon,
				   nla_get_u32(info->attrs[PON_A_TCONT_INDEX]),
				   alloc ? (int)nla_get_u32(alloc) : -1,
				   info->extack);
	pon_device_put(pon);
	return err;
}

/**
 * pon_nl_tod_set_doit() - set the ONU's clock at a downstream frame
 * @skb: the request
 * @info: the request's attributes
 *
 * Return: 0; -EINVAL without id, superframe, sec or nsec; -ENODEV for an
 * unknown ONU, or one without a data path; -EOPNOTSUPP when the MAC cannot
 * set a clock at a frame; -EAGAIN while the ONU has no equalization delay;
 * -ERANGE when the frame has passed or lies too far ahead; or another
 * negative error code.
 */
int pon_nl_tod_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct timespec64 tstamp;
	struct pon_device *pon;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_TOD_ID) ||
	    GENL_REQ_ATTR_CHECK(info, PON_A_TOD_SUPERFRAME) ||
	    GENL_REQ_ATTR_CHECK(info, PON_A_TOD_SEC) ||
	    GENL_REQ_ATTR_CHECK(info, PON_A_TOD_NSEC))
		return -EINVAL;

	pon = pon_device_get_by_id(nla_get_u32(info->attrs[PON_A_TOD_ID]));
	if (!pon) {
		NL_SET_ERR_MSG_ATTR(info->extack, info->attrs[PON_A_TOD_ID],
				    "no ONU with that id");
		return -ENODEV;
	}

	tstamp.tv_sec = nla_get_u64(info->attrs[PON_A_TOD_SEC]);
	tstamp.tv_nsec = nla_get_u32(info->attrs[PON_A_TOD_NSEC]);
	err = pon_device_tod_set(pon,
				 nla_get_u32(info->attrs[PON_A_TOD_SUPERFRAME]),
				 &tstamp, info->extack);
	pon_device_put(pon);
	return err;
}

/**
 * pon_nl_tcont_get_dumpit() - dump every T-CONT of every ONU
 * @skb: the part of the dump to fill
 * @cb: the dump, whose args[0] and args[1] hold the id of the ONU and the
 *	index of the T-CONT to report next
 *
 * An ONU without a data path, or whose MAC has no tcont_get or fails it, is
 * left out from that T-CONT on.
 *
 * Return: the length of @skb; 0 ends the dump.
 */
int pon_nl_tcont_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct pon_device *pon;
	u32 id = cb->args[0];
	unsigned int index = cb->args[1];
	int err;

	while ((pon = pon_device_get_next(id))) {
		for (; index < pon->num_tconts; index++) {
			err = pon_nl_tcont_fill(skb, pon, index,
						NETLINK_CB(cb->skb).portid,
						cb->nlh->nlmsg_seq,
						NLM_F_MULTI);
			if (err == -EMSGSIZE) {
				cb->args[0] = pon->id;
				cb->args[1] = index;
				pon_device_put(pon);
				return skb->len;
			}
			if (err)
				break;
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
 * The cursor moves past an ONU once its message is in @skb. So a dump never
 * repeats an ONU, an ONU whose message does not fit in @skb is the first of
 * the next part, and an ONU registered during the dump appears if its id is
 * still ahead.
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
		pon_nl_status_get(pon, &st);
		err = pon_nl_dev_fill(skb, pon, &st,
				      NETLINK_CB(cb->skb).portid,
				      cb->nlh->nlmsg_seq, NLM_F_MULTI,
				      PON_CMD_DEV_GET);
		if (!err)
			cb->args[0] = pon->id + 1;
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
	 * it has listeners. Joining the group needs CAP_NET_ADMIN, as a change
	 * does; dev-get, whose reply holds what a notification holds, needs
	 * none.
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
