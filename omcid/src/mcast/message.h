/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The IGMP and MLD messages the ONU makes itself, as a proxy (G.988 9.3.27,
 * IGMP function 1 and 2): the reports and leaves it sends the OLT for the
 * groups its subscribers hold, and the queries it sends the subscribers.
 * IGMP versions 1 to 3 (RFC 1112, 2236, 3376) and MLD versions 1 and 2
 * (RFC 2710, 3810), each as an Ethernet frame without a VLAN tag, the
 * checksums in place; the tag is the agent's to put on.
 */

#ifndef OMCI_MCAST_MESSAGE_H
#define OMCI_MCAST_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcast/acl.h"
#include "omcid/attrs.h"

/* The sources one group of a report names, at most. */
#define OMCI_MCAST_ITEM_SOURCES 8u

/* The length of an Ethernet address. */
#define OMCI_MCAST_MAC_LEN 6u

/**
 * struct omci_mcast_env - Who a message is from, and in which version.
 * @mac: the source Ethernet address
 * @source: the source IP address in the form acl.h keeps addresses, IPv4 in
 *	the mapped form; all zero for 0.0.0.0 and ::
 * @version: 1 to 3 for IGMP, 16 and 17 for MLD 1 and 2, as the multicast
 *	operations profile numbers them (G.988 9.3.27)
 */
struct omci_mcast_env {
	uint8_t mac[OMCI_MCAST_MAC_LEN];
	uint8_t source[OMCI_MCAST_ADDR_LEN];
	uint8_t version;
};

/**
 * struct omci_mcast_item - What the subscribers hold of one group.
 * @group: the group
 * @any: from every source
 * @n: the sources it is held from, when not from every source
 * @source: those sources
 *
 * Neither @any nor a source is no member at all.
 */
struct omci_mcast_item {
	uint8_t group[OMCI_MCAST_ADDR_LEN];
	bool any;
	uint8_t n;
	uint8_t source[OMCI_MCAST_ITEM_SOURCES][OMCI_MCAST_ADDR_LEN];
};

/**
 * omci_mcast_build_report() - Make a report or a leave.
 * @out: the frame
 * @cap: room in @out
 * @env: who it is from
 * @change: the groups changed (a join or a leave); otherwise they answer a
 *	query, with what is held
 * @items: the groups
 * @n: how many
 * @used: how many of them the frame took
 *
 * Version 3 and MLD version 2 put as many groups in one frame as fit, a
 * group of every source as an exclude record and any other as an include
 * record, which with no source is the leave. The versions before take one
 * group a frame, a report while it is held and a leave (IGMPv2) or done
 * (MLDv1) when it is not; version 1 has no leave, and an answer holds no
 * leave. The groups must be of the family of the version.
 *
 * Return: the length of the frame, 0 when there is nothing to send or the
 *	frame does not fit.
 */
size_t omci_mcast_build_report(uint8_t *out, size_t cap,
			       const struct omci_mcast_env *env, bool change,
			       const struct omci_mcast_item *items, size_t n,
			       size_t *used);

/**
 * omci_mcast_build_query() - Make a query.
 * @out: the frame
 * @cap: room in @out
 * @env: who it is from
 * @group: the group it asks about, or NULL (or all zero) for a general query
 * @max_resp: the most time to answer, in units of 0.1 s
 * @robustness: the querier's robustness variable, in versions 3 and MLD 2
 * @interval: the time between general queries, in seconds, for the same
 *
 * Return: the length of the frame, 0 when it does not fit or the group is
 *	not of the family of the version.
 */
size_t omci_mcast_build_query(uint8_t *out, size_t cap,
			      const struct omci_mcast_env *env,
			      const uint8_t *group, unsigned int max_resp,
			      unsigned int robustness, unsigned int interval);

#endif
