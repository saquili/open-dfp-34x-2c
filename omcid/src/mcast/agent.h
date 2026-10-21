/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The multicast agent: the OLT's multicast policy (G.988 9.3.27 to 9.3.29)
 * over the groups the bridge snoops on the subscriber side. It reads what
 * the subscriber sends and which groups the bridge forwards, takes back each
 * group the policy does not allow, sets the bridge's leave and timers, keeps
 * the static groups, and keeps the counts and the active groups that the
 * OLT reads from the multicast subscriber monitor. It counts the downstream
 * bytes of the groups the bridge forwards and lists those whose stream
 * stopped in the lost groups of their profile. It gives the subscriber's
 * messages upstream a priority the operator's rules take where the profile
 * leaves the priority to the subscriber, keeps what the stick's panel shows,
 * and tests a group from the stick itself.
 */

#ifndef OMCI_MCAST_AGENT_H
#define OMCI_MCAST_AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcast/acl.h"
#include "me/tables.h"
#include "mib/store.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

struct omci_mcast;

/* The lost multicast group alarm of the multicast operations profile. */
#define OMCI_MCAST_ALARM_LOST 0u

/**
 * omci_mcast_init() - Set up the agent; one per process.
 * @out: the agent
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or %OMCI_ERR_BUSY while one exists.
 */
__must_check enum omci_status omci_mcast_init(struct omci_mcast **out);

/**
 * omci_mcast_fini() - Release the agent.
 * @m: the agent, or NULL
 */
void omci_mcast_fini(struct omci_mcast *m);

/**
 * omci_mcast_program() - Take the policy from the MIB again.
 * @m: the agent, or NULL
 * @mib: the MIB
 * @now_sec: the time, in seconds of a clock that never goes back
 *
 * For any change of the multicast classes or of the bridge ports: sets the
 * bridge's leave and timers from the profile, adds and removes the static
 * groups, and judges every group again, so that one the policy no longer
 * allows ends. A backend without multicast is left alone.
 */
void omci_mcast_program(struct omci_mcast *m, struct omci_mib *mib,
			uint64_t now_sec);

/**
 * omci_mcast_service() - Answer what the subscriber side did.
 * @m: the agent, or NULL
 * @mib: the MIB
 * @now_sec: the time
 *
 * Reads the messages, counting the joins accepted and those over the
 * bandwidth and noting who joined, then the groups of the bridge: a group
 * the policy does not allow is removed, and a static group that went
 * missing comes back.
 */
void omci_mcast_service(struct omci_mcast *m, struct omci_mib *mib,
			uint64_t now_sec);

/**
 * omci_mcast_tick() - Let time pass; once a second.
 * @m: the agent, or NULL
 * @mib: the MIB
 * @now_sec: the time
 *
 * Ends the previews whose time is up, starts the preview counts again a day
 * after they began, counts the allowed preview groups down a minute at a
 * time, and services; ends a test whose time is up; then reads the
 * downstream bytes of each group the bridge forwards, a group that had none
 * for a while being lost.
 */
void omci_mcast_tick(struct omci_mcast *m, struct omci_mib *mib,
		     uint64_t now_sec);

/*
 * The active group lists of a multicast subscriber monitor (G.988 9.3.29),
 * IPv4 and IPv6: the subscriber side's, in an instance linked to it.
 */
extern const struct omci_me_table_source omci_mcast_ipv4_groups;
extern const struct omci_me_table_source omci_mcast_ipv6_groups;

/*
 * The lost groups list of a multicast operations profile (G.988 9.3.27):
 * the groups of its dynamic access control list that the bridge forwards
 * but whose stream stopped.
 */
extern const struct omci_me_table_source omci_mcast_lost_groups;

/**
 * omci_mcast_lost_profiles() - The multicast operations profiles with a lost
 *	group, whose lost multicast group alarm stands.
 * @m: the agent, or NULL
 * @out: their instances
 * @cap: room in @out
 *
 * Return: how many were written.
 */
size_t omci_mcast_lost_profiles(const struct omci_mcast *m, uint16_t *out,
				size_t cap);

struct omci_publish_ops;

/**
 * omci_mcast_publish() - Write what the agent holds of television, for the
 *	stick's panel (sys/publish.h).
 * @m: the agent, or NULL, which holds nothing
 * @ops: the writer
 * @ctx: passed to @ops
 *
 * As JSON: {"now_s":N,"policy":P,"groups":[...],"rejected":[...],
 * "log":[...],"lost":[...],"counters":{...},"querier":Q,"priority":R}, the
 * shape of pond's value "iptv" (pond/openapi.json): the OLT's policy, null
 * without one; the groups the bridge forwards; the joins the policy refused
 * and the events, the latest first; the groups whose stream stopped; the
 * counts since the start; the OLT's latest query, null before one; who
 * gives the subscriber's IGMP its priority upstream, null without a
 * profile. Times are the agent's clock, in seconds.
 */
void omci_mcast_publish(const struct omci_mcast *m,
			const struct omci_publish_ops *ops, void *ctx);

/**
 * omci_mcast_test_start() - Join a group from the stick and count its stream.
 * @m: the agent
 * @group: an IPv4 multicast group, in the form acl.h keeps addresses
 * @vid: the VLAN a subscriber would join it in, 1 to 4094, or 0 untagged
 * @seconds: how long the test lasts, 1 to 30; the agent ends it itself a few
 *	seconds after, when nobody did
 * @why: of a group the policy refuses, the word of the refusal, as
 *	omci_mcast_publish() gives it; "" otherwise
 *
 * The agent sends the OLT a join of the group as a subscriber's would go,
 * judged by the policy, and counts the group's downstream bytes, which the
 * bridge does not forward to the subscriber side for the test.
 *
 * Return: %OMCI_OK; %OMCI_ERR_INVAL for another group or VLAN or time;
 * %OMCI_ERR_BUSY while a test runs; %OMCI_DECLINED for a group the policy
 * refuses; %OMCI_ERR_IO in a backend that cannot send or count;
 * %OMCI_ERR_NOMEM when no counter is free; %OMCI_ERR_HW when the join could
 * not be sent.
 */
__must_check enum omci_status
omci_mcast_test_start(struct omci_mcast *m,
		      const uint8_t group[static OMCI_MCAST_ADDR_LEN],
		      uint16_t vid, unsigned int seconds, const char **why);

/**
 * omci_mcast_test_read() - The bytes of the group under test so far.
 * @m: the agent, or NULL
 * @bytes: the bytes since the test began, or of the whole test once it ended
 * @running: whether the test still runs
 *
 * Return: %OMCI_OK, %OMCI_DECLINED before any test, or %OMCI_ERR_HW when the
 * count could not be read.
 */
__must_check enum omci_status
omci_mcast_test_read(struct omci_mcast *m, uint64_t *bytes, bool *running);

/**
 * omci_mcast_test_stop() - End the test: the group is left, and its count kept.
 * @m: the agent, or NULL
 */
void omci_mcast_test_stop(struct omci_mcast *m);

#endif
