/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The priorities of the filters on the ingress of sfp and pon: the one
 * registry. The switch takes a frame through the filters of an interface in
 * the order of their priorities, lowest first, so the order of the ranges
 * below is the order in which the stick's rules look at a frame, and a
 * priority taken by two writers is a rule that replaces another. Each range
 * has its owner and its size.
 *
 *   range      owner  interface   what
 *   1-2        onu-mgmt  both     the management VLAN of the pair in use, and
 *                                 of the pair on trial; the boot's is the
 *                                 first, and the two take turns
 *   50-65      omcid  sfp         drops of the VLANs no rule takes
 *   100-107    omcid  sfp         traps of IGMP and MLD
 *   200-207    omcid  pon         downstream tags of multicast VLANs
 *   240-287    omcid  both        bridge port filters, upstream on sfp and
 *                                 downstream on pon
 *   300-315    omcid  pon         counting rules of multicast groups
 *   400-499    onu-fw both        the firewall's rules, an entry each
 *   1000-1015  omcid  both        VLAN rules, a priority each
 *
 * The session interfaces (ppp) have priorities of their own, which onu-apply
 * alone writes: the drops of what is addressed to the stick's networks, 1 to
 * 9. They are filters of another interface and take none of these.
 *
 * The firewall's rules come after omcid's rules of the bridge, so that a pass
 * of the firewall's keeps none of them from a frame, and before the driver's
 * own rules of IGMP and MLD, at 65535.
 *
 * onu-mgmt and onu-apply, shell scripts, write the numbers 1 and 2 and read
 * the boundary 1000 in their own text, and onu-fw has the firewall's range in
 * onu-utils/src/fwacl.h; a test of the image reads this file and finds them
 * the same.
 */

#ifndef OMCID_TCPRIO_H
#define OMCID_TCPRIO_H

#define TC_PRIO_MGMT 1u
#define TC_PRIO_MGMT_N 2u
#define TC_PRIO_DISCARD 50u
#define TC_PRIO_DISCARD_N 16u
#define TC_PRIO_TRAP 100u
#define TC_PRIO_TRAP_N 8u
#define TC_PRIO_DS 200u
#define TC_PRIO_DS_N 8u
#define TC_PRIO_FILTER 240u
#define TC_PRIO_FILTER_N 48u
#define TC_PRIO_WATCH 300u
#define TC_PRIO_WATCH_N 16u
#define TC_PRIO_FIREWALL 400u
#define TC_PRIO_FIREWALL_N 100u
#define TC_PRIO_VLAN 1000u
#define TC_PRIO_VLAN_N 16u

_Static_assert(TC_PRIO_MGMT + TC_PRIO_MGMT_N <= TC_PRIO_DISCARD,
	       "the management VLAN's priorities overlap the drops");
_Static_assert(TC_PRIO_DISCARD + TC_PRIO_DISCARD_N <= TC_PRIO_TRAP,
	       "the drops' priorities overlap the traps");
_Static_assert(TC_PRIO_TRAP + TC_PRIO_TRAP_N <= TC_PRIO_DS,
	       "the traps' priorities overlap the downstream tags");
_Static_assert(TC_PRIO_DS + TC_PRIO_DS_N <= TC_PRIO_FILTER,
	       "the downstream tags' priorities overlap the filters");
_Static_assert(TC_PRIO_FILTER + TC_PRIO_FILTER_N <= TC_PRIO_WATCH,
	       "the filters' priorities overlap the counting rules");
_Static_assert(TC_PRIO_WATCH + TC_PRIO_WATCH_N <= TC_PRIO_FIREWALL,
	       "the counting rules' priorities overlap the firewall's");
_Static_assert(TC_PRIO_FIREWALL + TC_PRIO_FIREWALL_N <= TC_PRIO_VLAN,
	       "the firewall's priorities overlap the VLAN rules");

#endif
