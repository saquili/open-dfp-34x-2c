/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The multicast agent: the OLT's multicast policy (G.988 9.3.27 to 9.3.29)
 * over the groups the bridge snoops on the subscriber side. It reads what
 * the subscriber sends and which groups the bridge forwards, takes back each
 * group the policy does not allow, sets the bridge's leave and timers, keeps
 * the static groups, and keeps the counts and the active groups that the
 * OLT reads from the multicast subscriber monitor.
 */

#ifndef OMCI_MCAST_AGENT_H
#define OMCI_MCAST_AGENT_H

#include <stdint.h>

#include "me/tables.h"
#include "mib/store.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

struct omci_mcast;

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
 * time, and services.
 */
void omci_mcast_tick(struct omci_mcast *m, struct omci_mib *mib,
		     uint64_t now_sec);

/*
 * The active group lists of a multicast subscriber monitor (G.988 9.3.29),
 * IPv4 and IPv6: the subscriber side's, in an instance linked to it.
 */
extern const struct omci_me_table_source omci_mcast_ipv4_groups;
extern const struct omci_me_table_source omci_mcast_ipv6_groups;

#endif
