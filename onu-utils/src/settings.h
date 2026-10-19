/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The settings of the stick's data path, as one table (ADR 0014): each key
 * of /run/onu.conf, the shape of its values and its default, the PPPoE
 * account of each session, and the rules that bind them together. onu-config
 * checks a whole set of settings against it ("onu-config check") and hands the
 * settings on with every default filled in ("onu-config settings"); the status
 * daemon asks it rather than knowing the rules.
 *
 * A set of settings is KEY=VALUE lines, a later line over an earlier one:
 * SESSIONn_GEM, SESSIONn_VLAN, SESSIONn_MAC, SESSIONn_FAMILY, SESSIONn_PASS
 * and SESSIONn_HOST_VLAN for the sessions 1 to 7; PPPOEn_USER and
 * PPPOEn_SECRET, the account of session n, an empty user removing it. The
 * keys of before are read and taken over: PPPOE_USER and PPPOE_SECRET, the
 * single account of before, and WAN_GEM, WAN_VLAN and WAN_MAC, its GEM
 * port, VLAN and MAC address, are the first session's; LAN_VLAN, the one
 * link to the host of before, is each session's that names none. The keys
 * of the bench's router of before, MODE, WAN_PROTO, WAN_ADDRESS,
 * WAN_GATEWAY, LAN_ADDRESS, LAN_DHCP, LAN_DHCP_START and LAN_DHCP_END, are
 * taken with any value and left out. Empty lines and lines that begin with
 * '#' are not settings.
 *
 * The connections 1 to 7 that take their address as a router does, IPoE, are
 * IPOEn_KIND (static, an address typed in, or dhcp, one from the operator's
 * DHCP server; empty for none, which removes it), IPOEn_GEM, IPOEn_VLAN and
 * IPOEn_MAC, as a session's, IPOEn_FAMILY (ipv4, ipv6 or both), and for a
 * static one IPOEn_ADDRESS (with its prefix length, 8 to 32 bits),
 * IPOEn_GATEWAY (another address of the network, or for a /32, and a /31
 * whose other address it is not, any host's on the link outside it),
 * IPOEn_DNS (the name servers, apart by blanks), IPOEn_ADDRESS6,
 * IPOEn_GATEWAY6 and IPOEn_PREFIX6 (the IPv6 prefix of the networks behind
 * the stick). Sessions with an account and these together are seven at most.
 *
 * The local networks 1 to 8, which the stick routes for the devices on the
 * host side, are LANn_CONN (the connection they go out by: pppoe:N, a session
 * that keeps its address on the stick, or ipoe:N), LANn_NAME (for people only),
 * LANn_VLAN (on the host link, nothing for untagged), LANn_ADDRESS (the
 * stick's address in the network, with its prefix length), LANn_DHCP (on or
 * off), LANn_POOL_FROM and LANn_POOL_TO (the addresses DHCP leases), LANn_LEASE
 * (seconds), LANn_IPV6 (on or off) and LANn_IPV6_SUBNET (the /64 of the
 * connection's delegated prefix, in hexadecimal); a network without a
 * connection is none. The fixed addresses 1 to 512 are FIXEDn, "L/MAC/IP",
 * the address IP that DHCP gives the device MAC in network L, and FIXEDn_NAME;
 * an empty FIXEDn removes one.
 *
 * The ports forwarded from the internet to a device of a local network, 1 to
 * 96, are FORWARDn, "CONN/PROTO/PORTS/L/MAC/PORT/ON": what comes in by the
 * connection CONN (pppoe:N or ipoe:N), of PROTO (tcp, udp or both), to the
 * port or the range of ports PORTS (443, or 8000-8010), goes to the device
 * MAC of network L, which goes out by CONN, at its fixed address, to the
 * port PORT, or for a range to as many ports from PORT, the same or others;
 * ON is on, or off for one kept but not in force. The ranges moved to other
 * ports of the forwards in force move 1024 ports at most, TCP's and UDP's
 * apart (SETTINGS_MOVED_MAX). FORWARDn_NAME is its name. An empty
 * FORWARDn removes one. DMZn, 1 to 7, "CONN/L/MAC", is a connection's DMZ:
 * the device that takes what comes in by it that no forward takes; an empty
 * DMZn removes one.
 *
 * The firewall's rules, 1 to 96, are each
 * RULEn, "ID_ACTION_FROM_TO_PROTO_PORTS_FAMILY_ON", in the order they are
 * judged in, n, the first that traffic matches deciding: ID, 1 to 96, which names the rule whatever place it
 * moves to; ACTION, allow or block; FROM and TO, where the traffic comes
 * from and goes to, each "internet", "internet:ADDR" for a part of it (an
 * IPv4 or IPv6 address, or a network ADDR/LEN), "lans" for every local
 * network, "lan:L" for network L or "dev:L:MAC" for a device of it; PROTO,
 * any, tcp, udp, both (TCP and UDP) or icmp; PORTS, the port or the range of
 * ports it goes to, A or A-B, for tcp, udp or both, empty for every one;
 * FAMILY, any, ipv4 or ipv6; ON, on, or off for one kept but not in force.
 * The fields are apart by '_', which no field holds: an end holds '/', ':'
 * and '.', and the ports '-'. A rule goes from somewhere to somewhere else,
 * never from the internet to the internet nor within one network; the
 * family of its address is its own; a network it names is set up, and a
 * device it goes to has a fixed address there, by which it is reached.
 * RULEn_NAME is its name, up to 40 characters; an empty RULEn removes one.
 * Where no rule decides, FW_OUT says what becomes of a new connection from
 * the networks to the internet, allow or block, and FW_BETWEEN of one
 * between two networks; one from the internet to them is blocked, but what a
 * forward or a DMZ takes.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_SESSIONS 7
/* The IPoE connections, and the connections of every kind together. */
#define SETTINGS_IPOE 7
#define SETTINGS_CONNS 7
/* A value, a name for people included: 32 characters of UTF-8 fit it. */
#define SETTINGS_VALUE_MAX 132
#define SETTINGS_USER_MAX 63
#define SETTINGS_SECRET_MAX 255
#define SETTINGS_LANS 8
#define SETTINGS_FIXED 512
#define SETTINGS_FORWARDS 96
/*
 * The most ports of ranges moved to other ports, a protocol's apart: the
 * NAT of nf_tables moves no range by an offset, and onu-fw's table maps
 * each port of one to its own, an element of a map of the table each, of
 * FW_MOVED_MAX at most (fwmsg.h, which says why so many).
 */
#define SETTINGS_MOVED_MAX 1024
/* A DMZ for each connection at most. */
#define SETTINGS_DMZS SETTINGS_CONNS
/*
 * The firewall's rules, as many as the switch's ACL has entries; a rule's
 * name, 40 characters of UTF-8, and its value.
 */
#define SETTINGS_RULES 96
#define SETTINGS_RULE_NAME_MAX 164
#define SETTINGS_RULE_MAX 128

/* The keys of before that are not a session's, read and never written. */
enum settings_key {
	SET_WAN_GEM,
	SET_WAN_VLAN,
	SET_WAN_MAC,
	SETTINGS_NKEYS
};

/* A session's keys, SESSIONn_<suffix>, in the order they are written. */
enum settings_skey {
	SET_S_GEM,
	SET_S_VLAN,
	SET_S_MAC,
	SET_S_FAMILY,
	SET_S_PASS,
	SET_S_HOST_VLAN,
	SETTINGS_NSKEYS
};

/* The settings of an IPoE connection, IPOEn_<suffix>. */
enum settings_ikey {
	SET_I_KIND,
	SET_I_GEM,
	SET_I_VLAN,
	SET_I_MAC,
	SET_I_FAMILY,
	SET_I_ADDRESS,
	SET_I_GATEWAY,
	SET_I_DNS,
	SET_I_ADDRESS6,
	SET_I_GATEWAY6,
	SET_I_PREFIX6,
	SETTINGS_NIKEYS
};

/* The settings of a local network, LANn_<suffix>. */
enum settings_lkey {
	SET_L_CONN,
	SET_L_NAME,
	SET_L_VLAN,
	SET_L_ADDRESS,
	SET_L_DHCP,
	SET_L_POOL_FROM,
	SET_L_POOL_TO,
	SET_L_LEASE,
	SET_L_IPV6,
	SET_L_IPV6_SUBNET,
	SETTINGS_NLKEYS
};

/* What the firewall does where no rule decides, FW_<suffix>: allow or block. */
enum settings_fkey {
	SET_F_OUT,
	SET_F_BETWEEN,
	SETTINGS_NFKEYS
};

/* A fixed address: FIXEDn, "L/MAC/IP", and FIXEDn_NAME; empty for none. */
struct settings_fixed {
	char spec[48];
	char name[SETTINGS_VALUE_MAX];
};

/* A forward: FORWARDn, "CONN/PROTO/PORTS/L/MAC/PORT/ON", and FORWARDn_NAME. */
struct settings_forward {
	char spec[64];
	char name[SETTINGS_VALUE_MAX];
};

/* A rule of the firewall: RULEn, "ID_ACTION_FROM_TO_PROTO_PORTS_FAMILY_ON", and RULEn_NAME. */
struct settings_rule {
	char spec[SETTINGS_RULE_MAX];
	char name[SETTINGS_RULE_NAME_MAX];
};

struct settings_account {
	char user[SETTINGS_USER_MAX + 1];
	char secret[SETTINGS_SECRET_MAX + 1];
};

/**
 * struct settings - A set of settings.
 * @val: the value of each key that is not a session's
 * @sval: the value of each key of each session
 * @acct: the account of each session, an empty user for none
 * @ival: the value of each key of each IPoE connection
 * @lval: the value of each key of each local network
 * @fixed: the fixed addresses
 * @fwd: the forwards
 * @dmz: the DMZs, DMZn: "CONN/L/MAC", empty for none
 * @rule: the firewall's rules, in their order
 * @fw: what the firewall does where no rule decides, allow or block
 * @legacy_lan: LAN_VLAN, the one link of before, when @had_legacy_lan
 * @had_legacy_lan: whether the set named LAN_VLAN
 */
struct settings {
	char val[SETTINGS_NKEYS][SETTINGS_VALUE_MAX];
	char sval[SETTINGS_SESSIONS][SETTINGS_NSKEYS][SETTINGS_VALUE_MAX];
	struct settings_account acct[SETTINGS_SESSIONS];
	char ival[SETTINGS_IPOE][SETTINGS_NIKEYS][SETTINGS_VALUE_MAX];
	char lval[SETTINGS_LANS][SETTINGS_NLKEYS][SETTINGS_VALUE_MAX];
	struct settings_fixed fixed[SETTINGS_FIXED];
	struct settings_forward fwd[SETTINGS_FORWARDS];
	char dmz[SETTINGS_DMZS][32];
	struct settings_rule rule[SETTINGS_RULES];
	char fw[SETTINGS_NFKEYS][8];
	char legacy_lan[SETTINGS_VALUE_MAX];
	bool had_legacy_lan;
	/*
	 * The management VLANs in use, the pair's and a pair's on trial, 0 for
	 * none: no setting of the data path, but what its VLANs must keep off,
	 * since the switch keeps their frames between the host side and the
	 * stick. The caller that knows them, onu-config change, sets them.
	 */
	unsigned int mgmt_vlan[2];
};

/**
 * settings_defaults() - Every key at its default, and no account.
 * @s: the settings
 */
void settings_defaults(struct settings *s);

/**
 * settings_line() - Take one line of a set of settings.
 * @s: the settings
 * @line: the line, without its end; it is not changed
 * @err: what is wrong with a line that is not taken, which names the key and
 *       never a value
 * @errcap: the size of @err
 *
 * A line that is not taken changes nothing: a check refuses the whole set
 * for it, a reader leaves it out and the value before it stays.
 *
 * Return: 0 when the line was taken or is no setting at all (empty, or a
 *         remark); -1 when it was not taken, with @err said.
 */
int settings_line(struct settings *s, const char *line, char *err,
		  size_t errcap);

/**
 * settings_settle() - Take over what the keys of before say.
 * @s: the settings, every line taken
 *
 * The first session takes the WAN's VLAN, MAC address and GEM port of
 * before where it names none of its own, and the WAN's go back to their
 * defaults. LAN_VLAN of before, or the host link itself when it is empty,
 * becomes the link of each session that names none.
 */
void settings_settle(struct settings *s);

/**
 * settings_coherent() - Whether the settings make sense together.
 * @s: the settings, settled
 * @err: why not, when they do not
 * @errcap: the size of @err
 * Return: true when they do.
 */
bool settings_coherent(const struct settings *s, char *err, size_t errcap);

/**
 * settings_format_defaults() - The default of every key as KEY=VALUE lines.
 * @out: where the lines go
 * @cap: the size of @out
 *
 * The keys of before that are not a session's, then the keys of each of the
 * sessions 1 to %SETTINGS_SESSIONS, and the firewall's: what the programs that apply the
 * settings start from. This table is the only place that says a default.
 *
 * Return: the length of the lines, which is @cap or more when they did not
 *         fit.
 */
size_t settings_format_defaults(char *out, size_t cap);

/**
 * settings_format() - The settings as KEY=VALUE lines.
 * @s: the settings, settled
 * @out: where the lines go
 * @cap: the size of @out
 *
 * The keys of each session that has an account, of each IPoE connection, of
 * each local network, the fixed addresses, the forwards, the DMZs, the
 * firewall's rules, and what it does where none decides when that is not
 * its default; never an account, nor a key of before.
 *
 * Return: the length of the lines, which is @cap or more when they did not
 *         fit.
 */
size_t settings_format(const struct settings *s, char *out, size_t cap);

#endif
