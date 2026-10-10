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
 * MODE, WAN_GEM, WAN_VLAN, WAN_PROTO, WAN_ADDRESS, WAN_GATEWAY, WAN_MAC,
 * LAN_ADDRESS, LAN_DHCP, LAN_DHCP_START, LAN_DHCP_END; SESSIONn_GEM,
 * SESSIONn_VLAN, SESSIONn_MAC, SESSIONn_FAMILY, SESSIONn_PASS and
 * SESSIONn_HOST_VLAN for the sessions 1 to 7; PPPOEn_USER and PPPOEn_SECRET,
 * the account of session n, an empty user removing it (PPPOE_USER and
 * PPPOE_SECRET, the single account of before, are the first session's); and
 * LAN_VLAN, the one link to the host of before. Empty lines and lines that
 * begin with '#' are not settings.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_SESSIONS 7
#define SETTINGS_VALUE_MAX 64
#define SETTINGS_USER_MAX 63
#define SETTINGS_SECRET_MAX 255

/* The keys that are not a session's, in the order they are written. */
enum settings_key {
	SET_MODE,
	SET_WAN_GEM,
	SET_WAN_VLAN,
	SET_WAN_PROTO,
	SET_WAN_ADDRESS,
	SET_WAN_GATEWAY,
	SET_WAN_MAC,
	SET_LAN_ADDRESS,
	SET_LAN_DHCP,
	SET_LAN_DHCP_START,
	SET_LAN_DHCP_END,
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
	char legacy_lan[SETTINGS_VALUE_MAX];
	bool had_legacy_lan;
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
 * MODE's words of before, bridge and passthrough, name no shape: the
 * accounts give it, and they become empty. The first session takes the
 * WAN's VLAN, MAC address and GEM port of before where it names none of its
 * own; with an account and outside the bench's router mode, the WAN's go
 * back to their defaults, as they were the session's. LAN_VLAN of before,
 * or the host link itself when it is empty, becomes the link of each
 * session that names none.
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
 * settings_format() - The settings as KEY=VALUE lines.
 * @s: the settings, settled
 * @out: where the lines go
 * @cap: the size of @out
 *
 * Every key that is not a session's, then the keys of each session that has
 * an account; never an account.
 *
 * Return: the length of the lines, which is @cap or more when they did not
 *         fit.
 */
size_t settings_format(const struct settings *s, char *out, size_t cap);

#endif
