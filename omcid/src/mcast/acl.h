/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The tables of the multicast managed entities, read from the MIB into whole
 * rows: the access control lists of a multicast operations profile (G.988
 * 9.3.27), and the service packages and the allowed preview groups of a
 * multicast subscriber config info (9.3.28). The row parts that share a row
 * key make one row.
 */

#ifndef OMCI_MCAST_ACL_H
#define OMCI_MCAST_ACL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mib/store.h"
#include "omcid/attrs.h"

/* The classes. */
#define OMCI_MCAST_PROFILE 309u
#define OMCI_MCAST_SUBSCRIBER 310u
#define OMCI_MCAST_MONITOR 311u

/* The attributes of the multicast operations profile. */
enum {
	OMCI_MCAST_PROFILE_IGMP_VERSION = 1,
	OMCI_MCAST_PROFILE_IGMP_FUNCTION = 2,
	OMCI_MCAST_PROFILE_IMMEDIATE_LEAVE = 3,
	OMCI_MCAST_PROFILE_US_TCI = 4,
	OMCI_MCAST_PROFILE_US_TAG_CONTROL = 5,
	OMCI_MCAST_PROFILE_US_RATE = 6,
	OMCI_MCAST_PROFILE_DYNAMIC_ACL = 7,
	OMCI_MCAST_PROFILE_STATIC_ACL = 8,
	OMCI_MCAST_PROFILE_LOST_GROUPS = 9,
	OMCI_MCAST_PROFILE_ROBUSTNESS = 10,
	OMCI_MCAST_PROFILE_QUERIER = 11,
	OMCI_MCAST_PROFILE_QUERY_INTERVAL = 12,
	OMCI_MCAST_PROFILE_QUERY_RESPONSE = 13,
	OMCI_MCAST_PROFILE_LAST_MEMBER = 14,
	OMCI_MCAST_PROFILE_UNAUTHORIZED = 15,
	OMCI_MCAST_PROFILE_DS_TCI = 16,
};

/* The attributes of the multicast subscriber config info. */
enum {
	OMCI_MCAST_SUBSCRIBER_ME_TYPE = 1,
	OMCI_MCAST_SUBSCRIBER_PROFILE = 2,
	OMCI_MCAST_SUBSCRIBER_MAX_GROUPS = 3,
	OMCI_MCAST_SUBSCRIBER_MAX_BANDWIDTH = 4,
	OMCI_MCAST_SUBSCRIBER_ENFORCE = 5,
	OMCI_MCAST_SUBSCRIBER_PACKAGES = 6,
	OMCI_MCAST_SUBSCRIBER_PREVIEW = 7,
};

/* The attributes of the multicast subscriber monitor. */
enum {
	OMCI_MCAST_MONITOR_ME_TYPE = 1,
	OMCI_MCAST_MONITOR_BANDWIDTH = 2,
	OMCI_MCAST_MONITOR_JOINS = 3,
	OMCI_MCAST_MONITOR_EXCEEDED = 4,
	OMCI_MCAST_MONITOR_IPV4_GROUPS = 5,
	OMCI_MCAST_MONITOR_IPV6_GROUPS = 6,
};

/*
 * An address: IPv6, or IPv4 in the IPv4-mapped form ::ffff:a.b.c.d (RFC
 * 4291), so that the two compare in one order and never overlap.
 */
#define OMCI_MCAST_ADDR_LEN 16u

/**
 * omci_mcast_addr_v4() - Whether an address is an IPv4 address.
 * @addr: the address
 *
 * Return: true for the IPv4-mapped form.
 */
__must_check bool
omci_mcast_addr_v4(const uint8_t addr[static OMCI_MCAST_ADDR_LEN]);

/**
 * omci_mcast_addr_set_v4() - Make an address of an IPv4 address.
 * @addr: the address
 * @v4: the four bytes of the IPv4 address
 */
void omci_mcast_addr_set_v4(uint8_t addr[static OMCI_MCAST_ADDR_LEN],
			    const uint8_t v4[static 4]);

/*
 * The most row parts a table holds (tables.c): an access control list, and
 * a table of the multicast subscriber config info.
 */
#define OMCI_MCAST_ACL_ROWS 24u
#define OMCI_MCAST_SUBSCRIBER_ROWS 16u

/**
 * struct omci_mcast_acl_row - A row of an access control list.
 * @key: its row key
 * @gem_port: the GEM port that carries the groups downstream
 * @vlan: the VLAN that carries them downstream: 0 untagged, 1 to 4095, 4097
 *	any tagged, 0xffff unspecified
 * @any_source: the source address is ignored
 * @source: the source address
 * @first: the first group of the range
 * @last: the last group of the range
 * @bandwidth: the imputed bandwidth of each group, in bytes per second
 * @preview_length: the longest preview, in seconds; 0 for groups that are
 *	authorized in full
 * @preview_repeat_time: the shortest time between two previews of a group,
 *	in seconds
 * @preview_repeat_count: the most previews of a group; 0 for no limit
 * @preview_reset_time: when the preview count starts again: 0 never, 1 to
 *	24 that hour of the day
 */
struct omci_mcast_acl_row {
	uint16_t key;
	uint16_t gem_port;
	uint16_t vlan;
	bool any_source;
	uint8_t source[OMCI_MCAST_ADDR_LEN];
	uint8_t first[OMCI_MCAST_ADDR_LEN];
	uint8_t last[OMCI_MCAST_ADDR_LEN];
	uint32_t bandwidth;
	uint16_t preview_length;
	uint16_t preview_repeat_time;
	uint16_t preview_repeat_count;
	uint16_t preview_reset_time;
};

/**
 * omci_mcast_acl_read() - The rows of an access control list.
 * @mib: the MIB
 * @profile: the instance of the multicast operations profile
 * @attr_no: %OMCI_MCAST_PROFILE_DYNAMIC_ACL or %OMCI_MCAST_PROFILE_STATIC_ACL
 * @out: the rows that have their part 0, in row key order
 * @cap: room in @out
 *
 * Return: how many were written.
 */
size_t omci_mcast_acl_read(const struct omci_mib *mib, uint16_t profile,
			   uint8_t attr_no, struct omci_mcast_acl_row *out,
			   size_t cap);

/**
 * omci_mcast_acl_matches() - Whether a row takes a group from a source.
 * @row: the row
 * @group: the group
 * @source: the source; all zero for any source
 *
 * Return: true when the group is in the row's range and the row ignores the
 * source or names this one.
 */
__must_check bool
omci_mcast_acl_matches(const struct omci_mcast_acl_row *row,
		       const uint8_t group[static OMCI_MCAST_ADDR_LEN],
		       const uint8_t source[static OMCI_MCAST_ADDR_LEN]);

/**
 * omci_mcast_acl_conflicts() - Whether a Set of a row part of a dynamic
 *	access control list is to be refused.
 * @mib: the MIB
 * @profile: the instance of the multicast operations profile
 * @row: the row part the Set writes, with its table control field
 * @len: its size
 *
 * The ONU refuses a Set that would make two rows overlap (G.988 9.3.27):
 * rows whose ranges share a group and whose sources can be the same, equal
 * or one of them ignored. A range that ends before it starts is refused as
 * well. Only a write is judged.
 *
 * Return: true when the Set is to be refused.
 */
__must_check bool omci_mcast_acl_conflicts(const struct omci_mib *mib,
					   uint16_t profile, const uint8_t *row,
					   size_t len);

/**
 * struct omci_mcast_package - A row of the multicast service package table.
 * @key: its row key
 * @vid: the VID of the joins it takes: 0 to 4095 that VID, 0 for
 *	priority-tagged joins; 4096 the untagged; 4097 any tagged; 0xffff
 *	unspecified
 * @max_groups: the most groups of the package at once; 0 for no limit
 * @max_bandwidth: the most imputed bandwidth of the package, in bytes per
 *	second; 0 for no limit
 * @profile: the multicast operations profile of the package
 */
struct omci_mcast_package {
	uint16_t key;
	uint16_t vid;
	uint16_t max_groups;
	uint32_t max_bandwidth;
	uint16_t profile;
};

/* The VIDs of a service package that are not one VID. */
#define OMCI_MCAST_VID_UNTAGGED 4096u
#define OMCI_MCAST_VID_TAGGED 4097u
#define OMCI_MCAST_VID_ANY 0xffffu

/**
 * omci_mcast_packages_read() - The service packages of a subscriber.
 * @mib: the MIB
 * @subscriber: the instance of the multicast subscriber config info
 * @out: the packages, in row key order
 * @cap: room in @out
 *
 * Return: how many were written.
 */
size_t omci_mcast_packages_read(const struct omci_mib *mib, uint16_t subscriber,
				struct omci_mcast_package *out, size_t cap);

/**
 * struct omci_mcast_preview - A row of the allowed preview groups table.
 * @key: its row key
 * @any_source: the row names no source
 * @source: the source address
 * @vlan_ani: the VID that carries the group downstream, 0 untagged
 * @vlan_uni: the VID that carries the joins upstream, 0 untagged
 * @group: the group
 * @duration: how long the group is authorized, in minutes; 0 for no end
 * @time_left: the minutes left of that
 */
struct omci_mcast_preview {
	uint16_t key;
	bool any_source;
	uint8_t source[OMCI_MCAST_ADDR_LEN];
	uint16_t vlan_ani;
	uint16_t vlan_uni;
	uint8_t group[OMCI_MCAST_ADDR_LEN];
	uint16_t duration;
	uint16_t time_left;
};

/* Where the duration and the time left are in a part 1. */
#define OMCI_MCAST_PREVIEW_DURATION 18u
#define OMCI_MCAST_PREVIEW_TIME_LEFT 20u

/**
 * omci_mcast_previews_read() - The allowed preview groups of a subscriber.
 * @mib: the MIB
 * @subscriber: the instance of the multicast subscriber config info
 * @out: the rows that have both parts, in row key order
 * @cap: room in @out
 *
 * Return: how many were written.
 */
size_t omci_mcast_previews_read(const struct omci_mib *mib, uint16_t subscriber,
				struct omci_mcast_preview *out, size_t cap);

/**
 * omci_mcast_preview_written() - Fill in the time left of a part 1 written
 *	by the OLT.
 * @mib: the MIB
 * @subscriber: the instance of the multicast subscriber config info
 * @row: the row part the Set writes, with its table control field; changed
 *	in place
 * @len: its size
 *
 * The ONU keeps the time left, which a Set does not change: a new row starts
 * with its whole duration, and a new duration of a row moves the time left
 * by the difference (G.988 9.3.28). A part 0, or a row that is not written,
 * is left as it is.
 *
 * Return: false when the time left of a row with a duration comes to
 * nothing, which ends the row.
 */
__must_check bool omci_mcast_preview_written(const struct omci_mib *mib,
					     uint16_t subscriber, uint8_t *row,
					     size_t len);

/**
 * omci_mcast_preview_count_down() - Let a minute of the allowed preview
 *	groups pass.
 * @mib: the MIB
 * @subscriber: the instance of the multicast subscriber config info
 *
 * Takes a minute off the time left of each row with a duration, and ends a
 * row whose time runs out (G.988 9.3.28).
 *
 * Return: true when a row ended.
 */
bool omci_mcast_preview_count_down(struct omci_mib *mib, uint16_t subscriber);

#endif
