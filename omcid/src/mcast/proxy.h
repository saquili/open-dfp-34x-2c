/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The ONU as a proxy for the multicast groups of its subscribers (G.988
 * 9.3.27, IGMP function 1 and 2; RFC 4605 is the model).
 *
 * With function 1, snooping with proxy reporting, the OLT hears of a group
 * from the ONU, not from the subscriber: a join once for the first
 * subscriber to hold it, a leave once for the last to let it go, and an
 * answer to each of its queries from what the ONU holds. With function 2 the
 * ONU is also the querier of the subscribers, with the profile's address
 * and timers. Either asks the subscribers whether anyone still holds a group
 * after a leave, before it lets the group go.
 *
 * The proxy knows nothing of the bridge or the wire. It is told which groups
 * the subscribers hold, the leaves and the queries it hears, and the time,
 * and sends through the operations it is given.
 */

#ifndef OMCI_MCAST_PROXY_H
#define OMCI_MCAST_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcast/acl.h"
#include "mcast/message.h"
#include "mcast/report.h"

/* The groups the OLT is told about, the leaves checked, the queries asked, at most. */
#define OMCI_MCAST_PROXY_GROUPS 64u
#define OMCI_MCAST_PROXY_CHECKS 16u
#define OMCI_MCAST_PROXY_ASKED 8u

/* The tags a general query is sent in, at most. */
#define OMCI_MCAST_PROXY_TAGS 8u

/* The IGMP function of the multicast operations profile. */
enum {
	OMCI_MCAST_FUNCTION_SNOOPING = 0,
	OMCI_MCAST_FUNCTION_REPORTING = 1,
	OMCI_MCAST_FUNCTION_PROXY = 2,
};

/**
 * struct omci_mcast_tag - The VLAN tag a group is joined in.
 * @tagged: whether the joins carried a tag
 * @vid: its VID
 * @pcp: its priority
 *
 * What the proxy makes for a group carries the tag of the subscribers' joins
 * of it, so that the VLAN rules and the GEM ports take it as they take
 * those. Two tags are the same when both are untagged or both have the same
 * VID.
 */
struct omci_mcast_tag {
	bool tagged;
	uint16_t vid;
	uint8_t pcp;
};

/**
 * struct omci_mcast_chan - A group and source the subscribers hold.
 * @group: the group
 * @source: the source, all zero for every source
 * @tag: the tag of the joins
 */
struct omci_mcast_chan {
	uint8_t group[OMCI_MCAST_ADDR_LEN];
	uint8_t source[OMCI_MCAST_ADDR_LEN];
	struct omci_mcast_tag tag;
};

/**
 * struct omci_mcast_proxy_params - What the profile says.
 * @function: one of the OMCI_MCAST_FUNCTION_ values
 * @version: the IGMP version of the profile (G.988 9.3.27): 1 to 3, or 16
 *	and 17 for MLD; the other protocol takes the highest version
 * @querier: the IPv4 address the IGMP messages are from, all zero for none
 * @robustness: the robustness variable; 0 for 2
 * @query_interval: seconds between general queries; 0 for 125
 * @query_response: the most time to answer a general query, in units of
 *	0.1 s; 0 for 100
 * @last_member: the same for a group's query; 0 for 10
 * @fast_leave: a leave ends the group without a query
 */
struct omci_mcast_proxy_params {
	uint8_t function;
	uint8_t version;
	uint8_t querier[4];
	uint8_t robustness;
	uint32_t query_interval;
	uint32_t query_response;
	uint32_t last_member;
	bool fast_leave;
};

/**
 * struct omci_mcast_proxy_ops - What the proxy sends through.
 * @up: send a frame, which has no VLAN tag, to the OLT in @tag; false when it
 *	could not be sent
 * @down: send a frame to the subscribers in @tag; false when it could not be
 * @drop: no subscriber holds this group any longer: end it
 * @mac: the Ethernet address the messages are from; false for none
 * @ctx: the argument of each of these
 */
struct omci_mcast_proxy_ops {
	bool (*up)(void *ctx, const uint8_t *frame, size_t len,
		   const struct omci_mcast_tag *tag);
	bool (*down)(void *ctx, const uint8_t *frame, size_t len,
		     const struct omci_mcast_tag *tag);
	void (*drop)(void *ctx,
		     const uint8_t group[static OMCI_MCAST_ADDR_LEN]);
	bool (*mac)(void *ctx, uint8_t mac[static OMCI_MCAST_MAC_LEN]);
	void *ctx;
};

/* A group the OLT was told about, and the tag it was told in. */
struct omci_mcast_told {
	struct omci_mcast_item item;
	struct omci_mcast_tag tag;
};

/* A leave being checked: queries still to send, and when the next goes. */
struct omci_mcast_check {
	uint8_t group[OMCI_MCAST_ADDR_LEN];
	struct omci_mcast_tag tag;
	unsigned int left;
	uint64_t next;
};

/* What the proxy knows: what the OLT was told, the leaves being checked, the queries to answer. */
struct omci_mcast_proxy {
	struct omci_mcast_told told[OMCI_MCAST_PROXY_GROUPS];
	size_t n_told;
	struct omci_mcast_check check[OMCI_MCAST_PROXY_CHECKS];
	size_t n_check;
	uint8_t asked[OMCI_MCAST_PROXY_ASKED][OMCI_MCAST_ADDR_LEN];
	size_t n_asked;
	/* Whether a general query of the OLT is to be answered, IPv4 then IPv6, and when. */
	bool answering[2];
	uint64_t answer_at[2];
	/* When the next general queries go to the subscribers; 0 before the first. */
	uint64_t query_at;
	/* The jitter of the answers: its state, and whether the Ethernet address seeded it. */
	uint32_t rng;
	bool seeded;
};

/**
 * omci_mcast_proxy_reset() - Forget everything the OLT was told.
 * @p: the proxy
 */
void omci_mcast_proxy_reset(struct omci_mcast_proxy *p);

/**
 * omci_mcast_proxy_left() - A subscriber left a group.
 * @p: the proxy
 * @pr: the profile's settings
 * @group: the group
 * @tag: the tag of the leave
 * @now: the time, in seconds of a clock that never goes back
 *
 * Without fast leave, the proxy then asks the subscribers, in the tag, whether
 * anyone still holds the group, and lets it go when nobody answers.
 */
void omci_mcast_proxy_left(struct omci_mcast_proxy *p,
			   const struct omci_mcast_proxy_params *pr,
			   const uint8_t group[static OMCI_MCAST_ADDR_LEN],
			   const struct omci_mcast_tag *tag, uint64_t now);

/**
 * omci_mcast_proxy_joined() - A subscriber joined a group.
 * @p: the proxy
 * @group: the group
 *
 * Ends the check of its leave.
 */
void omci_mcast_proxy_joined(struct omci_mcast_proxy *p,
			     const uint8_t group[static OMCI_MCAST_ADDR_LEN]);

/**
 * omci_mcast_proxy_query() - The OLT sent a query.
 * @p: the proxy
 * @q: the query, as the parser took it apart
 * @now: the time
 *
 * A general query is answered within its response time, at a time the
 * proxy picks; a query of a group, at the next run.
 */
void omci_mcast_proxy_query(struct omci_mcast_proxy *p,
			    const struct omci_mcast_report *q, uint64_t now);

/**
 * omci_mcast_proxy_run() - Tell the OLT what changed, and let time pass.
 * @p: the proxy
 * @pr: the profile's settings
 * @ops: what to send through
 * @chans: the groups and sources the subscribers hold
 * @n: how many
 * @now: the time
 *
 * Sends the OLT a report for each group newly held, a leave for each
 * group no longer held, and the answers it owes; sends the queries of the
 * checks of leaves and, with function 2, the general queries; and lets go
 * of a group nobody answered for. A message that could not be sent is
 * made again at the next run. With function 0 the proxy forgets all.
 */
void omci_mcast_proxy_run(struct omci_mcast_proxy *p,
			  const struct omci_mcast_proxy_params *pr,
			  const struct omci_mcast_proxy_ops *ops,
			  const struct omci_mcast_chan *chans, size_t n,
			  uint64_t now);

#endif
