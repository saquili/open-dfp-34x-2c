/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The IGMP and MLD messages a subscriber sends, taken apart: which groups,
 * from which sources, it joins or leaves. IGMP versions 1 to 3 (RFC 1112,
 * 2236, 3376) and MLD versions 1 and 2 (RFC 2710, 3810).
 */

#ifndef OMCI_MCAST_REPORT_H
#define OMCI_MCAST_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcast/acl.h"
#include "omcid/attrs.h"

/* The most records of one message that are kept. */
#define OMCI_MCAST_REPORT_RECORDS 16u

/**
 * enum omci_mcast_kind - What a message is.
 * @OMCI_MCAST_KIND_OTHER: not a report or a leave: a query, or a message
 *	that is not taken apart
 * @OMCI_MCAST_KIND_REPORT: a membership report, of any version
 * @OMCI_MCAST_KIND_LEAVE: an IGMPv2 leave or an MLDv1 done
 */
enum omci_mcast_kind {
	OMCI_MCAST_KIND_OTHER = 0,
	OMCI_MCAST_KIND_REPORT,
	OMCI_MCAST_KIND_LEAVE,
};

/**
 * struct omci_mcast_record - One group of a message.
 * @join: the subscriber joins (true) or leaves (false)
 * @group: the group
 * @source: the source, all zero for every source
 */
struct omci_mcast_record {
	bool join;
	uint8_t group[OMCI_MCAST_ADDR_LEN];
	uint8_t source[OMCI_MCAST_ADDR_LEN];
};

/**
 * struct omci_mcast_report - A message taken apart.
 * @kind: what it is
 * @version: the IGMP version, 1 to 3, or 16 and 17 for MLD 1 and 2, as the
 *	multicast operations profile numbers them (G.988 9.3.27)
 * @tagged: the frame had a VLAN tag
 * @vid: the VID of its outer tag
 * @pcp: the priority of its outer tag
 * @client: the address it came from
 * @n: the records kept
 * @cut: records that did not fit
 * @rec: the records: one per group and source, a group joined from every
 *	source with an all-zero source
 */
struct omci_mcast_report {
	enum omci_mcast_kind kind;
	uint8_t version;
	bool tagged;
	uint16_t vid;
	uint8_t pcp;
	uint8_t client[OMCI_MCAST_ADDR_LEN];
	uint8_t n;
	uint8_t cut;
	struct omci_mcast_record rec[OMCI_MCAST_REPORT_RECORDS];
};

/**
 * omci_mcast_report_parse() - Take a message apart.
 * @frame: the Ethernet frame, from its destination address; up to two VLAN
 *	tags may be in it
 * @len: its length
 * @tci: the tag the receive path took off the frame, when @tagged
 * @tagged: whether it took one off
 * @out: the message
 *
 * An IGMPv3 or MLDv2 record joins the group from every source in exclude
 * mode, and from each source it names in include mode or when it allows
 * new sources; it leaves the group when it changes to include mode with no
 * source, and leaves each source it blocks.
 *
 * Return: false for a frame that is not IGMP or MLD, or is cut short.
 */
__must_check bool omci_mcast_report_parse(const uint8_t *frame, size_t len,
					  uint16_t tci, bool tagged,
					  struct omci_mcast_report *out);

#endif
