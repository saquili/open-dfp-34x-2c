/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The firewall's rules as the switch's ACL (ADR 0023): the compiler of the
 * rules in their order, with the networks, the devices' fixed addresses, the
 * forwards, the DMZs and the connections' addresses, to an ordered list of
 * tc flower rules on the ingress of sfp, the host link, and of pon, the
 * PON's port, all in the switch alone (skip_sw), each a pass or a drop, at
 * the priorities from ACL_PREF on. The switch judges a frame as it enters a
 * port, by the entry of the lowest priority that matches it, and keeps no
 * state (linux/Documentation/networking/dsa/rtl9602c-acl-qos.rst, "The ACL").
 * IPv4 alone: the driver compares no IPv6 address, and the stick routes no
 * IPv6 to the networks' devices yet (stage 5); a rule of IPv6 alone is none
 * here, and one of any family is its IPv4.
 *
 * From a network, what enters sfp: a network is its addresses (src_ip),
 * "lans" one entry for each network, a device it comes from its MAC address
 * (src_mac). To a network, its addresses (dst_ip), to a device its fixed
 * address, to a part of the internet its address (dst_ip). The ACL sees no
 * route, so "the internet" is what goes to no local address. A rule to a
 * local address and one to the internet never match the same frame, so on
 * sfp the rules to a local address come first, in their order, then a pass
 * of each local destination: each network's addresses, 255.255.255.255 and
 * the groups, 224.0.0.0/4, of UDP (a DHCP request, mDNS and SSDP go to no
 * rule to the internet; IGMP goes on to them, as a pass of it would keep
 * the driver's own rule from copying it to the bridge's snooping); and then
 * the rules to the internet, in their order, which need name no
 * destination.
 *
 * From the internet, what enters pon: a frame to a network's device comes to
 * a connection's own address, and reaches the device through a forward or
 * the DMZ (NAT). A rule to a device or a network is the external ports of
 * the forwards that go there, the rule's ports, which are the device's,
 * moved through the forward's mapping, and, for a DMZ that goes there, the
 * rule's ports that no forward of the connection takes, of TCP and UDP, and
 * an echo request of ICMP (type 8). A DMZ takes every port no forward takes,
 * and so the answers to the networks' own traffic, which the ACL cannot tell
 * from a new connection: a rule through the DMZ that names no port is left
 * out there (status part, "dmz"). Every entry on pon names the connection's
 * address (dst_ip), so that what the OLT bridges to the host side for other
 * services (television, a router's own sessions) meets none; a connection
 * by PPPoE is matched inside its session (protocol ppp_ses, ppp_proto ip),
 * an IPoE connection as IPv4 on its GEM port. The session's number is not
 * named: its address tells it apart, and with a part of the internet named
 * too an entry would need a ninth slot.
 *
 * A rule that blocks is a drop. A rule that allows is a pass, with a pass of
 * the answers placed with it, B to A with the rule's ports as their source
 * ports, as a later rule that blocks would drop them else: the ACL cannot
 * tell an answer from a new packet, so the far end may start from those
 * ports too. Between the networks the answers of a rule without ports are
 * all of its protocol. The answers of a rule from the internet are those of
 * the devices its forwards and DMZ reach, from the ports they take there. The
 * answers of a rule to the internet come by pon to the connection's address,
 * where only rules of forwards' and DMZ's ports judge them; they are passed
 * when the rule names ports, and not when it names none, as a pass of every
 * frame to the connection's address would undo every later rule from the
 * internet.
 *
 * A protocol of both is an entry of TCP and one of UDP; icmp is ip_proto
 * icmp; any names no protocol. A range of ports is the masked prefixes that
 * cover it (dst_port V/M).
 *
 * The rules go into the ACL in their order up to the last one that fits
 * whole, with the passes of the local destinations ahead of the first that
 * has an entry to the internet on sfp; the rest are not in hardware (room).
 *
 * The compiler reads lines, which onu-apply writes:
 *
 *   lan N NET/LEN [CONN]     network N, 1 to 8: its addresses, and the
 *                            connection it goes out by, pppoe:S or ipoe:K
 *   wan CONN IFNAME ADDR     a connection with its IPv4 address, now, and
 *                            its interface
 *   dev N MAC ADDR           the fixed address of a device of network N
 *   fwd IFNAME ADDR PROTO PORTS DEV PORT
 *   dmz IFNAME ADDR DEV      a forward in force and a DMZ, as onu-fw's
 *                            table has them (fwmsg.h)
 *   rule ID_ACTION_FROM_TO_PROTO_PORTS_FAMILY_ON
 *                            a rule, as RULEn has it, in their order
 *
 * A forward and a DMZ come after the line of their connection; one of a
 * connection without a line is no one's.
 */

#ifndef ONU_UTILS_FWACL_H
#define ONU_UTILS_FWACL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The priorities of the firewall's entries on sfp and pon, one each, in
 * the order the switch judges them: the registry's range
 * (omcid/target/tcprio.h, TC_PRIO_FIREWALL), after omcid's rules of the
 * bridge and before its VLAN rules.
 */
#define ACL_PREF 400
#define ACL_PREFS 100
/*
 * The most entries the rules take: the ACL's 96, less the driver's own four
 * (IGMP and MLD) and the four of the management VLAN's two pairs, which
 * onu-mgmt holds at once while it changes the pair (a trap on sfp and a drop
 * on pon each): the firewall never keeps the management VLAN from a change.
 * omcid's rules of the bridge share what is left, and the kernel says when
 * no entry is: the rules from there on are left out.
 */
#define ACL_ROOM 88

#define ACL_LANS 8
#define ACL_WANS 14
#define ACL_DEVS 192
#define ACL_FWDS (2 * 96)
#define ACL_DMZS 7
#define ACL_RULES 96
/* The entries of one rule, at most, before the room is counted. */
#define ACL_RULE_ENTRIES 256

enum acl_port {
	ACL_SFP,
	ACL_PON,
};

/*
 * Where an entry stands among those of its port: on sfp, the rules to a
 * local address, the passes of the local destinations, then the rules to
 * the internet; on pon, one place.
 */
enum acl_phase {
	ACL_LOCAL,
	ACL_PASSES,
	ACL_INTERNET,
};

/**
 * struct acl_entry - A flower rule of the ACL, as tc would add it.
 * @port: %ACL_SFP or %ACL_PON
 * @phase: its place among its port's (enum acl_phase)
 * @rule: the rule it is of, its place in the spec, or -1 for the passes of
 *	the local destinations
 * @pref: its priority on its port
 * @pass: a pass, else a drop
 * @pppoe: of an IPv4 packet in a PPPoE session (protocol ppp_ses, ppp_proto
 *	ip), else of IPv4 (protocol ip)
 * @smac: from the MAC address @mac
 * @mac: that address
 * @sip, @smask: from the addresses @sip under @smask, host order; @smask 0
 *	for any
 * @dip, @dmask: to those addresses, likewise
 * @proto: of that IP protocol, 0 for any
 * @sport, @spmask: from the ports @sport under @spmask, @spmask 0 for any
 * @dport, @dpmask: to those ports, likewise
 * @icmp_type: of that type of ICMP, -1 for any
 */
struct acl_entry {
	uint8_t port;
	uint8_t phase;
	int16_t rule;
	uint16_t pref;
	bool pass;
	bool pppoe;
	bool smac;
	uint8_t mac[6];
	uint32_t sip, smask;
	uint32_t dip, dmask;
	uint8_t proto;
	uint16_t sport, spmask;
	uint16_t dport, dpmask;
	int16_t icmp_type;
};

/* Where a rule's end is. */
enum acl_end_kind {
	ACL_END_INTERNET,
	ACL_END_LANS,
	ACL_END_LAN,
	ACL_END_DEV,
};

/**
 * struct acl_end - An end of a rule.
 * @kind: the internet or a part of it, every network, one, or a device
 * @v6: a part of the internet of IPv6
 * @net, @mask: a part of the internet of IPv4, host order; @mask 0 for all
 * @lan: the network of %ACL_END_LAN and %ACL_END_DEV, from 0
 * @mac: the device's MAC address
 */
struct acl_end {
	uint8_t kind;
	bool v6;
	uint32_t net, mask;
	int lan;
	uint8_t mac[6];
};

/* The protocols of a rule. */
#define ACL_TCP 1u
#define ACL_UDP 2u
#define ACL_ICMP 4u
#define ACL_ANY 8u

/**
 * struct acl_rule - A rule, as RULEn says it.
 * @id: its ID
 * @allow: allows, else blocks
 * @from, @to: its ends
 * @protos: %ACL_TCP and %ACL_UDP, %ACL_ICMP or %ACL_ANY
 * @ports: names ports, @first to @last
 * @family: 0 any, 4 or 6
 * @on: in force
 */
struct acl_rule {
	unsigned int id;
	bool allow;
	struct acl_end from, to;
	unsigned int protos;
	bool ports;
	uint16_t first, last;
	int family;
	bool on;
};

/**
 * struct acl_wan - A connection with an address.
 * @conn: pppoe:S or ipoe:K
 * @ifname: its interface
 * @addr: its address, host order
 * @pppoe: a PPPoE session
 */
struct acl_wan {
	char conn[16];
	char ifname[16];
	uint32_t addr;
	bool pppoe;
};

/**
 * struct acl_spec - What the rules are compiled with.
 * @lan_on, @lan_net, @lan_mask, @lan_conn: network n's addresses and the
 *	connection it goes out by, "" for none
 * @wan: the connections that have an address, @nwans of them
 * @dev_lan, @dev_mac, @dev_ip: the fixed addresses, @ndevs of them
 * @fwd_wan, @fwd_proto, @fwd_from, @fwd_last, @fwd_dev, @fwd_port: the
 *	forwards: the connection, TCP or UDP, the external ports, the device's
 *	address and its first port; @nfwds of them
 * @dmz_wan, @dmz_dev: the DMZs, @ndmzs of them
 * @rule: the rules, in their order, @nrules of them
 */
struct acl_spec {
	bool lan_on[ACL_LANS];
	uint32_t lan_net[ACL_LANS], lan_mask[ACL_LANS];
	char lan_conn[ACL_LANS][16];
	struct acl_wan wan[ACL_WANS];
	int nwans;
	int dev_lan[ACL_DEVS];
	uint8_t dev_mac[ACL_DEVS][6];
	uint32_t dev_ip[ACL_DEVS];
	int ndevs;
	int fwd_wan[ACL_FWDS];
	uint8_t fwd_proto[ACL_FWDS];
	uint16_t fwd_from[ACL_FWDS], fwd_last[ACL_FWDS];
	uint32_t fwd_dev[ACL_FWDS];
	uint16_t fwd_port[ACL_FWDS];
	int nfwds;
	int dmz_wan[ACL_DMZS];
	uint32_t dmz_dev[ACL_DMZS];
	int ndmzs;
	struct acl_rule rule[ACL_RULES];
	int nrules;
};

/* Whether a rule is in the ACL: whole, in part, or not. */
enum acl_state {
	ACL_IN,
	ACL_PART,
	ACL_OUT,
};

/**
 * struct acl_status - What became of a rule.
 * @id: its ID
 * @state: %ACL_IN, %ACL_PART or %ACL_OUT
 * @why: for %ACL_PART and %ACL_OUT, why: "off", "ipv6" (of IPv6 alone),
 *	"nothing" (nothing the ACL sees matches it, as a rule from the internet
 *	to a device that no forward and no DMZ reaches), "room" (no room left
 *	for it whole), "refused" (the kernel refused an entry of it), "after"
 *	(after a rule that was refused), "dmz" (its part through a DMZ names
 *	every port); NULL for %ACL_IN
 * @entries: the entries it has in the list
 * @internet: whether it has an entry to the internet on sfp, which needs
 *	the passes of the local destinations ahead of it
 */
struct acl_status {
	unsigned int id;
	uint8_t state;
	const char *why;
	int entries;
	bool internet;
};

/*
 * The entries: the rules' as many as fit (ACL_ROOM), and the passes of the
 * local destinations, which they count.
 */
#define ACL_SET_MAX ACL_ROOM

/**
 * struct acl_set - The compiled rules.
 * @e: the entries, those of sfp first, each port's in the order of their
 *	priorities, @n of them
 * @st: what became of each rule of the spec, in its order
 * @nst: the rules
 * @passes: the passes of the local destinations among @e
 */
struct acl_set {
	struct acl_entry e[ACL_SET_MAX];
	int n;
	struct acl_status st[ACL_RULES];
	int nst;
	int passes;
};

void acl_spec_init(struct acl_spec *s);

/*
 * Take a line of the file over @s: 0, or -1 with why in @err for a line
 * that is none of it. An empty line and a line that begins with '#' are
 * none, and are taken.
 */
int acl_spec_line(struct acl_spec *s, const char *line, char *err, size_t errcap);

/*
 * Compile the rules of @s into @out, with @room entries at most (ACL_ROOM,
 * or fewer).
 */
void acl_compile(const struct acl_spec *s, int room, struct acl_set *out);

/*
 * The tc command that adds @e, with skip_sw when @hw, into @buf of @cap
 * bytes: its length, as snprintf().
 */
int acl_entry_format(const struct acl_entry *e, bool hw, char *buf, size_t cap);

/*
 * What became of @st, as a line of the status file: "ID in ENTRIES",
 * "ID part ENTRIES WHY" or "ID out 0 WHY".
 */
int acl_status_format(const struct acl_status *st, char *buf, size_t cap);

#endif /* ONU_UTILS_FWACL_H */
