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
 * The local networks 1 to 8, which the stick routes for the devices on the
 * host side, are LANn_CONN (the connection they go out by, pppoe:N, a session
 * that keeps its address on the stick), LANn_NAME (for people only),
 * LANn_VLAN (on the host link, nothing for untagged), LANn_ADDRESS (the
 * stick's address in the network, with its prefix length), LANn_DHCP (on or
 * off), LANn_POOL_FROM and LANn_POOL_TO (the addresses DHCP leases), LANn_LEASE
 * (seconds), LANn_IPV6 (on or off) and LANn_IPV6_SUBNET (the /64 of the
 * connection's delegated prefix, in hexadecimal); a network without a
 * connection is none. The fixed addresses 1 to 512 are FIXEDn, "L/MAC/IP",
 * the address IP that DHCP gives the device MAC in network L, and FIXEDn_NAME;
 * an empty FIXEDn removes one.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_SESSIONS 7
/* A value, a name for people included: 32 characters of UTF-8 fit it. */
#define SETTINGS_VALUE_MAX 132
#define SETTINGS_USER_MAX 63
#define SETTINGS_SECRET_MAX 255
#define SETTINGS_LANS 8
#define SETTINGS_FIXED 512

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

/* A fixed address: FIXEDn, "L/MAC/IP", and FIXEDn_NAME; empty for none. */
struct settings_fixed {
	char spec[48];
	char name[SETTINGS_VALUE_MAX];
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
 * @legacy_lan: LAN_VLAN, the one link of before, when @had_legacy_lan
 * @had_legacy_lan: whether the set named LAN_VLAN
 */
struct settings {
	char val[SETTINGS_NKEYS][SETTINGS_VALUE_MAX];
	char sval[SETTINGS_SESSIONS][SETTINGS_NSKEYS][SETTINGS_VALUE_MAX];
	struct settings_account acct[SETTINGS_SESSIONS];
	char lval[SETTINGS_LANS][SETTINGS_NLKEYS][SETTINGS_VALUE_MAX];
	struct settings_fixed fixed[SETTINGS_FIXED];
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
 * sessions 1 to %SETTINGS_SESSIONS: what the programs that apply the
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
 * The keys of each session that has an account; never an account, nor a
 * key of before.
 *
 * Return: the length of the lines, which is @cap or more when they did not
 *         fit.
 */
size_t settings_format(const struct settings *s, char *out, size_t cap);

#endif
