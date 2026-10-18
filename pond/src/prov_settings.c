// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The settings of the stick's data path: KEY=value lines in /run/onu.conf,
 * which onu-apply applies, and the account of each of the stick's PPPoE
 * sessions, 1 to 7, in /run/onu-pppoeN, as "user secret", readable by root
 * and by pppoed, which runs the session. What the settings mean, their defaults and the rules that bind them
 * together, is onu-config's (ADR 0014), and so are the files: pond reads
 * neither, and has onu-act run onu-config for it (bus.h). What came of them
 * is onu-apply's, and pond reads no file of it either: "onu-apply state"
 * gives it as lines "name=value" (settings_state).
 *
 * A read gives the settings as "onu-config current" gives them, each session
 * with its user and whether a secret is set (never the secret), and what
 * onu-apply and dhcp6d brought up for it, what it hands down among it, the
 * address handed on, and what became of the settings in the configuration
 * store: "loaded" at boot, "defaults" when the store held none, "saved" or
 * "failed" after a change, "none" without a store, and the changes on
 * trial, of the settings, the SFP lane or the management pair, with when
 * each goes back. A write takes KEY=value lines, has
 * "onu-config change" judge them over the settings in force and write
 * them, and starts "onu-apply try" without waiting for it: the change is on
 * trial, and goes back unless it is confirmed (ADR 0014, changes.c). Until
 * onu-apply ends, a write is refused.
 *
 * The local networks the stick routes, LANn_*, the fixed addresses of their
 * devices, FIXEDn, and the devices that have an address from a network's
 * DHCP server are read apart from the other settings, as the panel shows
 * them; the devices from udhcpd's lease file of each network,
 * /run/udhcpd-netN.leases, which onu-apply names so. The panel's writes of
 * a network and of a fixed address become the keys of the store here.
 *
 * So are the connections that take their address as a router does, IPOEn_*
 * (IPoE: an address typed in, or one from the operator's DHCP server), each
 * with what onu-apply says it brought up (ipoe.N.* of onu-apply state). A
 * connection or a session removed takes the networks that go out by it
 * with it, and their fixed addresses; so does a session that comes to hand
 * its address down.
 *
 * So are the ports forwarded from the internet to the networks' devices,
 * FORWARDn, and each connection's DMZ, DMZn. A forward's or a DMZ's device
 * keeps its address: the one it has from its network's DHCP server becomes
 * its fixed address in the same write when it has none, as the panel's
 * stand-in does it. A network removed takes its forwards and its DMZs with
 * it.
 */

#define _GNU_SOURCE

#include "bus.h"
#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define SESSIONS 7
/* The IPoE connections, and the connections of every kind together. */
#define IPOE 7
#define CONNS 7
/* A whole set of settings, as much as the configuration store holds. */
#define TEXT_MAX 65536
#define VALUE_MAX 64
/* The local networks, the fixed addresses, the forwards, the DMZs, and a name's bytes. */
#define LANS 8
#define FIXED_MAX 512
#define FORWARDS 96
#define DMZS CONNS
#define NAME_MAX 132
#define USER_MAX 63
#define SECRET_MAX 255

/* The account of a session as onu-config current gives it: its user, and whether a secret is set. */
struct account {
	char user[USER_MAX + 1];
	bool secret;
};

/* How the last onu-apply try ended, or -1. */
static int g_apply_status = -1;

/* Text after the @len bytes of @buf, which holds @cap; @len counts past it. */
static void append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*len >= cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf + *len, cap - *len, fmt, ap);
	va_end(ap);
	*len += n > 0 ? (size_t)n : 0;
}

/* Whether @line is KEY=VALUE, the key in capitals, digits and '_'. */
static bool setting_line(const char *line)
{
	size_t k = strspn(line, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");

	return k && line[k] == '=';
}

/*
 * The settings in force, in full, with each account's PPPOEn_USER and
 * PPPOEn_SECRET_SET, as onu-config current gives them through onu-act:
 * KEY=VALUE lines in @out, and no other.
 */
static int settings_current(char *out, size_t cap)
{
	static char got[TEXT_MAX];
	char *line, *save = NULL;
	size_t olen = 0;
	int rc = bus_act("settings_current", NULL, NULL, 0, NULL, got, sizeof(got));

	if (rc)
		return rc < 0 ? rc : -EIO;
	/* What it said of a line it left out is no setting. */
	out[0] = '\0';
	for (line = strtok_r(got, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (setting_line(line))
			append(out, cap, &olen, "%s\n", line);
	return olen >= cap ? -E2BIG : 0;
}

/* The account of each session, of the lines of onu-config current. */
static void accounts_of(const char *text, struct account acct[SESSIONS])
{
	memset(acct, 0, sizeof(*acct) * SESSIONS);
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		int n;

		if (strncmp(p, "PPPOE", 5) || p[5] < '1' || p[5] > '0' + SESSIONS)
			continue;
		n = p[5] - '1';
		if (!strncmp(p + 6, "_USER=", 6))
			snprintf(acct[n].user, sizeof(acct[n].user), "%.*s",
				 (int)strcspn(p + 12, "\n"), p + 12);
		else if (!strncmp(p + 6, "_SECRET_SET=1", 13))
			acct[n].secret = true;
	}
}

/* The first line of what onu-config said, without its name, as the reason. */
static void reason(const char *said, char *err, size_t errcap)
{
	static const char name[] = "onu-config: ";
	size_t n;

	if (!strncmp(said, name, sizeof(name) - 1))
		said += sizeof(name) - 1;
	n = strcspn(said, "\n");
	snprintf(err, errcap, "%.*s", (int)n, said);
}

void prov_settings_reap(void)
{
	bus_poll();
}

/* The end of onu-apply try. */
static void applied(void *ctx, int code, const char *output)
{
	(void)ctx;
	(void)output;
	g_apply_status = code < 0 ? 255 : code;
}

static const char *line_value(const char *text, const char *key, char *out,
			      size_t cap);

/*
 * The changes on trial of the state @st (trial.NAME), each with when it
 * goes back, in seconds of the uptime, and the new address the management
 * pair's may be confirmed from alone (trial.mgmt.address).
 */
static void pv_trials(struct prov_buf *out, const char *st)
{
	static const char *const names[] = { "settings", "lane", "mgmt" };
	bool first = true;

	pv_open(out, '[');
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		char key[32], v[32];

		snprintf(key, sizeof(key), "trial.%s", names[i]);
		if (!line_value(st, key, v, sizeof(v)))
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "name");
		pv_str(out, names[i]);
		pv_comma(out);
		pv_key(out, "until_s");
		pv_num(out, strtol(v, NULL, 10));
		snprintf(key, sizeof(key), "trial.%s.address", names[i]);
		if (!strcmp(names[i], "mgmt") && line_value(st, key, v, sizeof(v))) {
			pv_comma(out);
			pv_key(out, "address");
			pv_str(out, v);
		}
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/* The fact @key of the state @st, as a string or null. */
static void pv_fact(struct prov_buf *out, const char *st, const char *key)
{
	char line[128];

	if (line_value(st, key, line, sizeof(line)))
		pv_str(out, line);
	else
		pv_null(out);
}

/* A fact of session @n that onu-apply says, as a string or null. */
static void pv_session_fact(struct prov_buf *out, const char *st, int n,
			    const char *fact)
{
	char key[48];

	snprintf(key, sizeof(key), "session.%d.%s", n + 1, fact);
	pv_fact(out, st, key);
}

/* @value as a string, or null when empty. */
static void pv_str_or_null(struct prov_buf *out, const char *value)
{
	if (*value)
		pv_str(out, value);
	else
		pv_null(out);
}

/* Whether session @n hands @fam down, as onu-apply says in @st. */
static bool session_hands(const char *st, int n, const char *fam)
{
	char key[48], line[64];
	size_t len = strlen(fam);

	snprintf(key, sizeof(key), "session.%d.hands", n + 1);
	if (!line_value(st, key, line, sizeof(line)))
		return false;
	for (const char *p = line; (p = strstr(p, fam)); p += len)
		if ((p == line || p[-1] == ' ') && (!p[len] || p[len] == ' '))
			return true;
	return false;
}

/*
 * Each session with an account: its user, whether a secret is set, its
 * settings of @text (SESSIONn_<KEY> as "key"), and what onu-apply brought up,
 * which it says in @st.
 */
static void pv_sessions(struct prov_buf *out, const struct account acct[SESSIONS],
			const char *text, const char *st)
{
	static const char *const ses_paths[] = { "l3_device", "data.address", "dns-server" };
	static const char *const pd_paths[] = { "prefix", "host" };
	bool first = true;

	pv_open(out, '[');
	for (int n = 0; n < SESSIONS; n++) {
		char prefix[16], object[48], ses[3][BUS_VALUE], pd[2][BUS_VALUE];
		size_t plen;

		if (!*acct[n].user)
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "id");
		pv_num(out, n + 1);
		pv_comma(out);
		pv_key(out, "user");
		pv_str(out, acct[n].user);
		pv_comma(out);
		pv_key(out, "secret_set");
		pv_bool(out, acct[n].secret);
		plen = (size_t)snprintf(prefix, sizeof(prefix), "SESSION%d_", n + 1);
		for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
			const char *eq = strchr(p, '=');
			size_t vlen = strcspn(eq ? eq + 1 : p, "\n");
			char name[32];
			size_t klen;

			if (strncmp(p, prefix, plen) || !eq)
				continue;
			klen = (size_t)(eq - p) - plen;
			if (klen >= sizeof(name))
				continue;
			for (size_t i = 0; i < klen; i++) {
				char c = p[plen + i];

				name[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
			}
			name[klen] = '\0';
			pv_comma(out);
			pv_key(out, name);
			pv_strn(out, eq + 1, vlen);
		}
		pv_comma(out);
		pv_key(out, "hands_on");
		pv_bool(out, session_hands(st, n, "ipv4"));
		pv_comma(out);
		pv_key(out, "dev");
		pv_session_fact(out, st, n, "dev");
		pv_comma(out);
		pv_key(out, "gem_dev");
		pv_session_fact(out, st, n, "gem");
		/*
		 * What the session brought, its interface of netifd's (sesN):
		 * its link, its address, handed on or not, and its name servers;
		 * and what its dhcp6d took for it and the host it went to.
		 */
		snprintf(object, sizeof(object), "network.interface.ses%d", n + 1);
		bus_status(object, ses_paths, 3, ses);
		snprintf(object, sizeof(object), "dhcp6d.ses%d", n + 1);
		bus_status(object, pd_paths, 2, pd);
		pv_comma(out);
		pv_key(out, "ppp");
		pv_str_or_null(out, ses[0]);
		pv_comma(out);
		pv_key(out, "ipv4");
		pv_str_or_null(out, ses[1]);
		pv_comma(out);
		pv_key(out, "ipv6");
		pv_session_fact(out, st, n, "ipv6");
		pv_comma(out);
		pv_key(out, "prefix");
		pv_str_or_null(out, pd[0]);
		pv_comma(out);
		pv_key(out, "prefix_host");
		pv_str_or_null(out, pd[1]);
		/* Where it hands down, and the gateway the router is given there. */
		pv_comma(out);
		pv_key(out, "lan");
		pv_session_fact(out, st, n, "lan");
		pv_comma(out);
		pv_key(out, "gateway");
		pv_session_fact(out, st, n, "gateway");
		pv_comma(out);
		pv_key(out, "dns");
		pv_str_or_null(out, ses[2]);
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/*
 * What the data path came to, as onu-apply says it (onu-apply state): lines
 * "name=value", of which a fact that is not there has none. Without an
 * answer the page shows the settings and no state.
 */
static void state_of(char *out, size_t cap)
{
	if (bus_act("settings_state", NULL, NULL, 0, NULL, out, cap))
		out[0] = '\0';
}

/* Whether IPoE connection @n is set up in @text: it has a kind. */
static bool ipoe_on(const char *text, int n)
{
	char k[32], v[VALUE_MAX];

	snprintf(k, sizeof(k), "IPOE%d_KIND", n);
	return line_value(text, k, v, sizeof(v)) && *v;
}

/* The connections of @text, of every kind: sessions with an account and IPoE. */
static int conns_of(const char *text, const struct account acct[SESSIONS])
{
	int n = 0;

	for (int i = 0; i < SESSIONS; i++)
		n += *acct[i].user != '\0';
	for (int i = 1; i <= IPOE; i++)
		n += ipoe_on(text, i);
	return n;
}

/* A key of IPoE connection @n and its value, as a member of an object. */
static void pv_ipoe_field(struct prov_buf *out, const char *text, int n,
			  const char *key, const char *member)
{
	char k[32], v[NAME_MAX];

	snprintf(k, sizeof(k), "IPOE%d_%s", n, key);
	pv_comma(out);
	pv_key(out, member);
	pv_str(out, line_value(text, k, v, sizeof(v)) ? v : "");
}

/* A fact of IPoE connection @n that onu-apply says in @st, as a member. */
static void pv_ipoe_fact(struct prov_buf *out, const char *st, int n,
			 const char *fact, const char *member)
{
	char key[48];

	snprintf(key, sizeof(key), "ipoe.%d.%s", n, fact);
	pv_comma(out);
	pv_key(out, member);
	pv_fact(out, st, key);
}

/*
 * The IPoE connections, each with its settings of @text and what onu-apply
 * brought up for it, which it says in @st: its interfaces, the addresses in
 * effect, the gateway and the name servers, and the seconds left of a DHCP
 * lease. It is up while it has an address.
 */
static void pv_ipoe(struct prov_buf *out, const char *text, const char *st)
{
	char key[48], v[VALUE_MAX];
	bool first = true;

	pv_open(out, '[');
	for (int n = 1; n <= IPOE; n++) {
		bool up;

		if (!ipoe_on(text, n))
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "id");
		pv_num(out, n);
		pv_ipoe_field(out, text, n, "KIND", "kind");
		pv_ipoe_field(out, text, n, "VLAN", "vlan");
		pv_ipoe_field(out, text, n, "GEM", "gem");
		pv_ipoe_field(out, text, n, "MAC", "mac");
		pv_ipoe_field(out, text, n, "FAMILY", "family");
		pv_ipoe_field(out, text, n, "ADDRESS", "address");
		pv_ipoe_field(out, text, n, "GATEWAY", "gateway");
		pv_ipoe_field(out, text, n, "DNS", "dns");
		pv_ipoe_field(out, text, n, "ADDRESS6", "address6");
		pv_ipoe_field(out, text, n, "GATEWAY6", "gateway6");
		pv_ipoe_field(out, text, n, "PREFIX6", "prefix6");
		pv_ipoe_fact(out, st, n, "dev", "dev");
		pv_ipoe_fact(out, st, n, "gem", "gem_dev");
		snprintf(key, sizeof(key), "ipoe.%d.ipv4", n);
		up = line_value(st, key, v, sizeof(v));
		snprintf(key, sizeof(key), "ipoe.%d.ipv6", n);
		up = up || line_value(st, key, v, sizeof(v));
		pv_comma(out);
		pv_key(out, "up");
		pv_bool(out, up);
		pv_ipoe_fact(out, st, n, "ipv4", "ipv4");
		pv_ipoe_fact(out, st, n, "router", "router");
		pv_ipoe_fact(out, st, n, "dns", "dns_now");
		pv_ipoe_fact(out, st, n, "ipv6", "ipv6");
		pv_ipoe_fact(out, st, n, "prefix", "prefix");
		snprintf(key, sizeof(key), "ipoe.%d.lease", n);
		pv_comma(out);
		pv_key(out, "lease");
		if (line_value(st, key, v, sizeof(v)))
			pv_num(out, strtol(v, NULL, 10));
		else
			pv_null(out);
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/* A key of network @n and its value, as a member of an object. */
static void pv_lan_field(struct prov_buf *out, const char *text, int n,
			 const char *key, const char *member)
{
	char k[32], v[NAME_MAX];

	snprintf(k, sizeof(k), "LAN%d_%s", n, key);
	pv_comma(out);
	pv_key(out, member);
	pv_str(out, line_value(text, k, v, sizeof(v)) ? v : "");
}

/* The local networks, each with its settings. */
static void pv_lans(struct prov_buf *out, const char *text)
{
	char k[32], v[VALUE_MAX];
	bool first = true;

	pv_open(out, '[');
	for (int n = 1; n <= LANS; n++) {
		snprintf(k, sizeof(k), "LAN%d_CONN", n);
		if (!line_value(text, k, v, sizeof(v)) || !*v)
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "id");
		pv_num(out, n);
		pv_lan_field(out, text, n, "CONN", "conn");
		pv_lan_field(out, text, n, "NAME", "name");
		pv_lan_field(out, text, n, "VLAN", "vlan");
		pv_lan_field(out, text, n, "ADDRESS", "address");
		pv_lan_field(out, text, n, "DHCP", "dhcp");
		pv_lan_field(out, text, n, "POOL_FROM", "pool_from");
		pv_lan_field(out, text, n, "POOL_TO", "pool_to");
		snprintf(k, sizeof(k), "LAN%d_LEASE", n);
		pv_comma(out);
		pv_key(out, "lease_s");
		pv_num(out, line_value(text, k, v, sizeof(v)) ? atol(v) : 0);
		pv_lan_field(out, text, n, "IPV6", "ipv6");
		pv_lan_field(out, text, n, "IPV6_SUBNET", "ipv6_subnet");
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/*
 * A fixed address of the settings, FIXEDn: its number, its network, MAC
 * and address; false for a line that is none.
 */
static bool fixed_of(const char *line, int *n, int *lan, char mac[18],
		     char ip[16])
{
	const char *p = line + 5;
	size_t d = strspn(p, "0123456789");
	size_t len;

	if (strncmp(line, "FIXED", 5) || !d || p[d] != '=')
		return false;
	*n = atoi(p);
	p += d + 1;
	len = strcspn(p, "\n");
	if (len < 21 || len > 2 + 18 + 15 || p[1] != '/' || p[19] != '/')
		return false;
	*lan = p[0] - '0';
	memcpy(mac, p + 2, 17);
	mac[17] = '\0';
	snprintf(ip, 16, "%.*s", (int)(len - 20), p + 20);
	return true;
}

/* The fixed addresses, each with its name. */
static void pv_fixed_addrs(struct prov_buf *out, const char *text)
{
	char mac[18], ip[16], k[32], name[NAME_MAX];
	bool first = true;

	pv_open(out, '[');
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		int n, lan;

		if (!fixed_of(p, &n, &lan, mac, ip))
			continue;
		snprintf(k, sizeof(k), "FIXED%d_NAME", n);
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "lan");
		pv_num(out, lan);
		pv_comma(out);
		pv_key(out, "mac");
		pv_str(out, mac);
		pv_comma(out);
		pv_key(out, "ip");
		pv_str(out, ip);
		pv_comma(out);
		pv_key(out, "name");
		pv_str(out, line_value(text, k, name, sizeof(name)) ? name : "");
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/*
 * udhcpd's lease file: the time it was written, eight bytes, then a lease
 * after another: the seconds it had left then, its address, the device's
 * MAC, the name the device gave, NUL-padded, and two bytes of padding; all
 * numbers big-endian (BusyBox, networking/udhcp/dhcpd.c, write_leases()).
 * It holds max_leases of them, which onu-apply makes at most 512; the file
 * buffer has the room prov_slurp() needs past the last.
 */
#define LEASE_LEN 36
#define LEASES_MAX 512
#define LEASE_FILE_MAX (8 + LEASE_LEN * LEASES_MAX + 2)

static uint32_t be32(const unsigned char *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* The devices with a lease of network @n, after those of @first. */
static void pv_leases_of(struct prov_buf *out, int n, time_t now, bool *first)
{
	static char file[LEASE_FILE_MAX];
	const unsigned char *b = (const unsigned char *)file;
	char path[48], mac[18], ip[16], name[21];
	uint64_t written = 0;
	size_t len;

	snprintf(path, sizeof(path), "/run/udhcpd-net%d.leases", n);
	if (prov_slurp(path, file, sizeof(file), &len) || len < 8)
		return;
	for (int i = 0; i < 8; i++)
		written = written << 8 | b[i];
	for (size_t at = 8; at + LEASE_LEN <= len; at += LEASE_LEN) {
		const unsigned char *l = b + at;
		long long left = (long long)be32(l) - ((long long)now - (long long)written);

		if (left <= 0 || !be32(l + 4))
			continue;
		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", l[8], l[9],
			 l[10], l[11], l[12], l[13]);
		snprintf(ip, sizeof(ip), "%u.%u.%u.%u", l[4], l[5], l[6], l[7]);
		memcpy(name, l + 14, 20);
		name[20] = '\0';
		if (!*first)
			pv_comma(out);
		*first = false;
		pv_open(out, '{');
		pv_key(out, "lan");
		pv_num(out, n);
		pv_comma(out);
		pv_key(out, "mac");
		pv_str(out, mac);
		pv_comma(out);
		pv_key(out, "ip");
		pv_str(out, ip);
		pv_comma(out);
		pv_key(out, "name");
		if (*name)
			pv_str(out, name);
		else
			pv_null(out);
		pv_comma(out);
		pv_key(out, "left_s");
		pv_num(out, left);
		pv_close(out, '}');
	}
}

/* The devices that have an address from a network's DHCP server. */
static void pv_leases(struct prov_buf *out, const char *text)
{
	char k[32], v[VALUE_MAX];
	time_t now = time(NULL);
	bool first = true;

	pv_open(out, '[');
	for (int n = 1; n <= LANS; n++) {
		snprintf(k, sizeof(k), "LAN%d_CONN", n);
		if (line_value(text, k, v, sizeof(v)) && *v)
			pv_leases_of(out, n, now, &first);
	}
	pv_close(out, ']');
}

/**
 * struct fwd - A forward of the settings, FORWARDn,
 * "CONN/PROTO/PORTS/L/MAC/PORT/ON", which onu-config has judged.
 * @n: its number
 * @conn: the connection it comes in by
 * @proto: tcp, udp or both
 * @from: the first port it takes
 * @to: the last
 * @lan: the device's network
 * @mac: the device's MAC address
 * @port: the device's port, the first of a range
 * @on: whether it is in force
 */
struct fwd {
	int n;
	char conn[16];
	char proto[8];
	long from, to;
	int lan;
	char mac[18];
	long port;
	bool on;
};

/*
 * The @n fields apart by '/' of the value of the line @line, KEY=VALUE, in
 * @v of @cap bytes, each in @field: false for another number of them.
 */
static bool fields_of(const char *line, char *v, size_t cap, char **field, int n)
{
	const char *eq = strchr(line, '=');
	size_t len = eq ? strcspn(eq + 1, "\n") : 0;
	int i = 0;

	if (!eq || !len || len >= cap)
		return false;
	memcpy(v, eq + 1, len);
	v[len] = '\0';
	for (char *p = v; i < n; i++) {
		field[i] = p;
		p = strchr(p, '/');
		if (!p)
			break;
		*p++ = '\0';
	}
	return i == n - 1;
}

/* A forward of the settings, the line FORWARDn=...: false for one that is none. */
static bool fwd_of(const char *line, struct fwd *f)
{
	const char *p = line + 7;
	size_t d = strspn(p, "0123456789");
	char v[80], *field[7], *dash;

	if (strncmp(line, "FORWARD", 7) || !d || p[d] != '=' ||
	    !fields_of(line, v, sizeof(v), field, 7))
		return false;
	f->n = atoi(p);
	snprintf(f->conn, sizeof(f->conn), "%s", field[0]);
	snprintf(f->proto, sizeof(f->proto), "%s", field[1]);
	dash = strchr(field[2], '-');
	f->from = atol(field[2]);
	f->to = dash ? atol(dash + 1) : f->from;
	f->lan = atoi(field[3]);
	snprintf(f->mac, sizeof(f->mac), "%s", field[4]);
	f->port = atol(field[5]);
	f->on = !strcmp(field[6], "on");
	return true;
}

/* A DMZ of the settings, the line DMZn=CONN/L/MAC: false for one that is none. */
static bool dmz_of(const char *line, int *n, char conn[16], int *lan, char mac[18])
{
	const char *p = line + 3;
	size_t d = strspn(p, "0123456789");
	char v[48], *field[3];

	if (strncmp(line, "DMZ", 3) || !d || p[d] != '=' ||
	    !fields_of(line, v, sizeof(v), field, 3))
		return false;
	*n = atoi(p);
	snprintf(conn, 16, "%s", field[0]);
	*lan = atoi(field[1]);
	snprintf(mac, 18, "%s", field[2]);
	return true;
}

/* The ports forwarded from the internet, each with its name. */
static void pv_forwards(struct prov_buf *out, const char *text)
{
	char k[32], name[NAME_MAX];
	bool first = true;
	struct fwd f;

	pv_open(out, '[');
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		if (!fwd_of(p, &f))
			continue;
		snprintf(k, sizeof(k), "FORWARD%d_NAME", f.n);
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "id");
		pv_num(out, f.n);
		pv_comma(out);
		pv_key(out, "name");
		pv_str(out, line_value(text, k, name, sizeof(name)) ? name : "");
		pv_comma(out);
		pv_key(out, "conn");
		pv_str(out, f.conn);
		pv_comma(out);
		pv_key(out, "proto");
		pv_str(out, f.proto);
		pv_comma(out);
		pv_key(out, "ext_from");
		pv_num(out, f.from);
		pv_comma(out);
		pv_key(out, "ext_to");
		pv_num(out, f.to);
		pv_comma(out);
		pv_key(out, "lan");
		pv_num(out, f.lan);
		pv_comma(out);
		pv_key(out, "mac");
		pv_str(out, f.mac);
		pv_comma(out);
		pv_key(out, "int_port");
		pv_num(out, f.port);
		pv_comma(out);
		pv_key(out, "on");
		pv_bool(out, f.on);
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/* Each connection's DMZ: its device. */
static void pv_dmzs(struct prov_buf *out, const char *text)
{
	char conn[16], mac[18];
	bool first = true;
	int n, lan;

	pv_open(out, '[');
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		if (!dmz_of(p, &n, conn, &lan, mac))
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "conn");
		pv_str(out, conn);
		pv_comma(out);
		pv_key(out, "lan");
		pv_num(out, lan);
		pv_comma(out);
		pv_key(out, "mac");
		pv_str(out, mac);
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

/* The address the device @mac has now from network @lan's DHCP server, in @ip. */
static bool lease_ip(int lan, const char *mac, char ip[16])
{
	static char file[LEASE_FILE_MAX];
	const unsigned char *b = (const unsigned char *)file;
	time_t now = time(NULL);
	uint64_t written = 0;
	char path[48], m[18];
	size_t len;

	snprintf(path, sizeof(path), "/run/udhcpd-net%d.leases", lan);
	if (prov_slurp(path, file, sizeof(file), &len) || len < 8)
		return false;
	for (int i = 0; i < 8; i++)
		written = written << 8 | b[i];
	for (size_t at = 8; at + LEASE_LEN <= len; at += LEASE_LEN) {
		const unsigned char *l = b + at;
		long long left = (long long)be32(l) - ((long long)now - (long long)written);

		snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x", l[8], l[9], l[10],
			 l[11], l[12], l[13]);
		if (left <= 0 || !be32(l + 4) || strcasecmp(m, mac))
			continue;
		snprintf(ip, 16, "%u.%u.%u.%u", l[4], l[5], l[6], l[7]);
		return true;
	}
	return false;
}

int prov_settings(struct prov_buf *out)
{
	struct account acct[SESSIONS];
	static char text[TEXT_MAX], st[TEXT_MAX];
	char line[128];
	bool first = true;
	int rc;

	prov_settings_reap();
	rc = settings_current(text, sizeof(text));
	if (rc)
		return rc;
	accounts_of(text, acct);
	state_of(st, sizeof(st));

	pv_open(out, '{');
	pv_key(out, "settings");
	pv_open(out, '{');
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		const char *eq = strchr(p, '=');
		char name[32];
		size_t klen;

		if (!eq || !strncmp(p, "SESSION", 7) || !strncmp(p, "FIXED", 5) ||
		    !strncmp(p, "IPOE", 4) || !strncmp(p, "FORWARD", 7) ||
		    !strncmp(p, "DMZ", 3) ||
		    (!strncmp(p, "LAN", 3) && p[3] >= '1' && p[3] <= '9'))
			continue;
		klen = (size_t)(eq - p);
		if (klen >= sizeof(name))
			continue;
		memcpy(name, p, klen);
		name[klen] = '\0';
		if (!first)
			pv_comma(out);
		first = false;
		pv_key(out, name);
		pv_strn(out, eq + 1, strcspn(eq + 1, "\n"));
	}
	pv_close(out, '}');
	pv_comma(out);

	pv_key(out, "sessions");
	pv_sessions(out, acct, text, st);
	pv_comma(out);
	pv_key(out, "ipoe");
	pv_ipoe(out, text, st);
	pv_comma(out);

	pv_key(out, "lans");
	pv_lans(out, text);
	pv_comma(out);
	pv_key(out, "fixed");
	pv_fixed_addrs(out, text);
	pv_comma(out);
	pv_key(out, "leases");
	pv_leases(out, text);
	pv_comma(out);
	pv_key(out, "forwards");
	pv_forwards(out, text);
	pv_comma(out);
	pv_key(out, "dmz");
	pv_dmzs(out, text);
	pv_comma(out);
	explicit_bzero(acct, sizeof(acct));

	pv_key(out, "mode");
	pv_fact(out, st, "mode");
	pv_comma(out);

	pv_key(out, "dns");
	pv_fact(out, st, "dns");
	pv_comma(out);

	/* The address a session handed to the router, and where. */
	pv_key(out, "pass");
	if (line_value(st, "pass.address", line, sizeof(line))) {
		pv_open(out, '{');
		pv_key(out, "address");
		pv_str(out, line);
		pv_comma(out);
		pv_key(out, "lan");
		pv_fact(out, st, "pass.lan");
		/* The concentrator's address, when it is the router's gateway. */
		pv_comma(out);
		pv_key(out, "peer_gateway");
		pv_fact(out, st, "pass.peer_gateway");
		pv_close(out, '}');
	} else {
		pv_null(out);
	}
	pv_comma(out);

	pv_key(out, "store");
	pv_fact(out, st, "store");
	pv_comma(out);

	pv_key(out, "trials");
	pv_trials(out, st);
	pv_comma(out);

	pv_key(out, "applying");
	pv_bool(out, bus_act_running("settings_try"));
	pv_comma(out);
	pv_key(out, "last_apply_status");
	if (g_apply_status < 0)
		pv_null(out);
	else
		pv_num(out, g_apply_status);
	pv_close(out, '}');
	return 0;
}

/*
 * A change of the settings: onu-config judges it over the settings in force
 * and writes it, then onu-apply tries it, which is not waited for.
 */
static struct prov_change g_change;

static void settings_written(void *ctx, int code, const char *output)
{
	char err[256];
	int rc;

	(void)ctx;
	if (code == 1) {
		reason(output, err, sizeof(err));
		prov_change_end(&g_change, -EINVAL, err);
		return;
	}
	if (code) {
		snprintf(err, sizeof(err), "onu-config: %s",
			 code < 0 ? "it does not run" : "the settings were not written");
		prov_change_end(&g_change, -EIO, err);
		return;
	}
	rc = bus_act_start("settings_try", -1, NULL, applied, NULL);
	snprintf(err, sizeof(err), "onu-apply: %s", strerror(-rc));
	prov_change_end(&g_change, rc, err);
}

int prov_settings_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx)
{
	int rc;

	prov_settings_reap();
	if (bus_act_running("settings_try")) {
		snprintf(err, errcap, "the last change is still being applied");
		return -EBUSY;
	}
	if (strlen(val) >= TEXT_MAX) {
		snprintf(err, errcap, "the settings are too long");
		return -EINVAL;
	}
	rc = prov_change_take(&g_change, done, ctx, err, errcap);
	if (rc)
		return rc;
	rc = bus_act_begin("settings_change", NULL, NULL, 0, val, settings_written,
			   NULL);
	if (rc) {
		prov_change_drop(&g_change);
		snprintf(err, errcap, "onu-config: it does not run");
		return -EIO;
	}
	return -EINPROGRESS;
}

int prov_settings_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_settings_write(val, err, errcap, prov_waited, &w), &w);
}

/*
 * The panel's views of the settings. The panel names no mode: the stick
 * passes the OLT's services to the router in any case, and holds PPPoE
 * sessions itself when it has accounts.
 */

/* A KEY=VALUE line of @text, or NULL; @out holds the value. */
static const char *line_value(const char *text, const char *key, char *out,
			      size_t cap)
{
	size_t n = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, n) && p[n] == '=') {
			const char *v = p + n + 1;
			size_t len = strcspn(v, "\r\n");

			if (len >= cap)
				return NULL;
			memcpy(out, v, len);
			out[len] = '\0';
			return out;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

/*
 * Network @n removed in @change, which holds @len bytes of @cap: LANn_CONN
 * empty, and the fixed addresses in it, the forwards to its devices and the
 * DMZs among them gone with it, of the settings @text.
 */
static void lan_drop(const char *text, int n, char *change, size_t cap, size_t *len)
{
	char mac[18], ip[16], conn[16];
	struct fwd w;

	append(change, cap, len, "LAN%d_CONN=\n", n);
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		int f, lan;

		if (fixed_of(p, &f, &lan, mac, ip) && lan == n)
			append(change, cap, len, "FIXED%d=\n", f);
		else if (fwd_of(p, &w) && w.lan == n)
			append(change, cap, len, "FORWARD%d=\n", w.n);
		else if (dmz_of(p, &f, conn, &lan, mac) && lan == n)
			append(change, cap, len, "DMZ%d=\n", f);
	}
}

/* The networks that go out by the connection @conn, "pppoe:N" or "ipoe:N", removed. */
static void lans_drop(const char *text, const char *conn, char *change, size_t cap,
		      size_t *len)
{
	char k[32], v[VALUE_MAX];

	for (int n = 1; n <= LANS; n++) {
		snprintf(k, sizeof(k), "LAN%d_CONN", n);
		if (line_value(text, k, v, sizeof(v)) && !strcmp(v, conn))
			lan_drop(text, n, change, cap, len);
	}
}

/*
 * A session of the stick's own: ID, 1 to 7, or none for the first free one;
 * USER and SECRET, its account, an empty USER removing the session (an
 * absent SECRET keeps the one there is); VLAN, MAC, GEM, FAMILY, PASS and
 * HOST_VLAN. onu-config judges them, and takes over the WAN's settings of before,
 * which the first session took when it had none of its own. A session
 * removed takes the networks that go out by it with it, and so does one
 * that hands its address down, PASS=on. Sessions and the IPoE connections
 * are seven at most together.
 */
int prov_session_write(const char *val, char *err, size_t errcap,
		       prov_done done, void *ctx)
{
	static char text[TEXT_MAX], change[TEXT_MAX];
	char id[VALUE_MAX], user[USER_MAX + 2], secret[SECRET_MAX + 2],
		vlan[VALUE_MAX], mac[VALUE_MAX], gem[VALUE_MAX], fam[VALUE_MAX],
		pass[VALUE_MAX], host[VALUE_MAX];
	const char *i = line_value(val, "ID", id, sizeof(id));
	const char *u = line_value(val, "USER", user, sizeof(user));
	const char *sec = line_value(val, "SECRET", secret, sizeof(secret));
	const char *v = line_value(val, "VLAN", vlan, sizeof(vlan));
	const char *m = line_value(val, "MAC", mac, sizeof(mac));
	const char *g = line_value(val, "GEM", gem, sizeof(gem));
	const char *f = line_value(val, "FAMILY", fam, sizeof(fam));
	const char *p = line_value(val, "PASS", pass, sizeof(pass));
	const char *h = line_value(val, "HOST_VLAN", host, sizeof(host));
	struct account acct[SESSIONS];
	char conn[16], key[32];
	int n = 0;
	size_t len = 0;
	int rc;

	rc = settings_current(text, sizeof(text));
	if (rc) {
		snprintf(err, errcap, "onu-config: the settings in force cannot be read");
		return rc;
	}
	accounts_of(text, acct);
	if (i) {
		if (strlen(i) != 1 || *i < '1' || *i > '0' + SESSIONS) {
			snprintf(err, errcap, "ID: 1 to %d", SESSIONS);
			explicit_bzero(acct, sizeof(acct));
			return -EINVAL;
		}
		n = *i - '0';
	} else {
		for (int k = 0; k < SESSIONS && !n; k++)
			if (!*acct[k].user)
				n = k + 1;
		if (!n || !u || !*u) {
			snprintf(err, errcap, n ? "a new session needs USER" :
						  "all %d sessions are taken",
				 SESSIONS);
			explicit_bzero(acct, sizeof(acct));
			return -EINVAL;
		}
		if (conns_of(text, acct) >= CONNS) {
			snprintf(err, errcap, "all %d connections are taken", CONNS);
			explicit_bzero(acct, sizeof(acct));
			return -EINVAL;
		}
	}
	if (!*acct[n - 1].user && (!u || !*u)) {
		snprintf(err, errcap, "session %d has no account", n);
		explicit_bzero(acct, sizeof(acct));
		return -EINVAL;
	}
	explicit_bzero(acct, sizeof(acct));

	change[0] = '\0';
	snprintf(conn, sizeof(conn), "pppoe:%d", n);
	if (u && !*u) {
		append(change, sizeof(change), &len, "PPPOE%d_USER=\n", n);
		lans_drop(text, conn, change, sizeof(change), &len);
	} else {
		if (u)
			append(change, sizeof(change), &len, "PPPOE%d_USER=%s\n", n, u);
		if (sec && *sec)
			append(change, sizeof(change), &len, "PPPOE%d_SECRET=%s\n", n, sec);
		if (v)
			append(change, sizeof(change), &len, "SESSION%d_VLAN=%s\n", n, v);
		if (m)
			append(change, sizeof(change), &len, "SESSION%d_MAC=%s\n", n, m);
		if (g)
			append(change, sizeof(change), &len, "SESSION%d_GEM=%s\n", n, g);
		if (f)
			append(change, sizeof(change), &len, "SESSION%d_FAMILY=%s\n", n, f);
		if (p)
			append(change, sizeof(change), &len, "SESSION%d_PASS=%s\n", n, p);
		if (h)
			append(change, sizeof(change), &len, "SESSION%d_HOST_VLAN=%s\n", n, h);
		/*
		 * A session that hands its address down, PASS on as told or
		 * as it is (on, as a new one's, where the settings name
		 * none), keeps none on the stick for a network to go out
		 * behind, which onu-config refuses: its networks go with the
		 * change, as the panel's stand-in does it.
		 */
		if (!p) {
			snprintf(key, sizeof(key), "SESSION%d_PASS", n);
			p = line_value(text, key, pass, sizeof(pass));
		}
		if (!p || !strcmp(p, "on"))
			lans_drop(text, conn, change, sizeof(change), &len);
	}

	rc = prov_settings_write(change, err, errcap, done, ctx);
	explicit_bzero(secret, sizeof(secret));
	explicit_bzero(change, sizeof(change));
	return rc;
}

int prov_session_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_session_write(val, err, errcap, prov_waited, &w), &w);
}

/*
 * A local network: ID, 1 to 8, or none for the first free one, which needs
 * CONN; CONN, the connection it goes out by, empty to remove the network with
 * its fixed addresses; NAME, VLAN, ADDRESS, DHCP, POOL_FROM, POOL_TO, LEASE,
 * IPV6 and IPV6_SUBNET. onu-config judges them.
 */
int prov_lan_write(const char *val, char *err, size_t errcap, prov_done done,
		   void *ctx)
{
	static const char *const keys[] = {
		"NAME", "VLAN", "ADDRESS", "DHCP", "POOL_FROM", "POOL_TO",
		"LEASE", "IPV6", "IPV6_SUBNET",
	};
	static char text[TEXT_MAX], change[TEXT_MAX];
	char id[VALUE_MAX], c[VALUE_MAX], k[32], v[NAME_MAX];
	const char *i = line_value(val, "ID", id, sizeof(id));
	const char *conn = line_value(val, "CONN", c, sizeof(c));
	size_t len = 0;
	int n = 0, rc;

	rc = settings_current(text, sizeof(text));
	if (rc) {
		snprintf(err, errcap, "onu-config: the settings in force cannot be read");
		return rc;
	}
	if (i) {
		if (strlen(i) != 1 || *i < '1' || *i > '0' + LANS) {
			snprintf(err, errcap, "ID: 1 to %d", LANS);
			return -EINVAL;
		}
		n = *i - '0';
	} else {
		for (int m = 1; m <= LANS && !n; m++) {
			snprintf(k, sizeof(k), "LAN%d_CONN", m);
			if (!line_value(text, k, v, sizeof(v)) || !*v)
				n = m;
		}
		if (!n || !conn || !*conn) {
			snprintf(err, errcap, n ? "a new network needs CONN" :
						  "all %d local networks are taken",
				 LANS);
			return -EINVAL;
		}
	}
	change[0] = '\0';
	if (conn && !*conn) {
		/* The network goes, and the fixed addresses in it with it. */
		lan_drop(text, n, change, sizeof(change), &len);
	} else {
		if (conn)
			append(change, sizeof(change), &len, "LAN%d_CONN=%s\n", n, conn);
		for (size_t j = 0; j < ARRAY_SIZE(keys); j++)
			if (line_value(val, keys[j], v, sizeof(v)))
				append(change, sizeof(change), &len, "LAN%d_%s=%s\n", n,
				       keys[j], v);
	}
	return prov_settings_write(change, err, errcap, done, ctx);
}

int prov_lan_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_lan_write(val, err, errcap, prov_waited, &w), &w);
}

/*
 * A connection that takes its address as a router does: ID, 1 to 7, or none
 * for the first free one; KIND, static or dhcp, empty to remove it with the
 * networks that go out by it; VLAN, GEM, MAC and FAMILY; and for a static
 * one ADDRESS, GATEWAY, DNS (apart by blanks or commas), ADDRESS6, GATEWAY6
 * and PREFIX6, which a connection by DHCP has none of. Sessions and these
 * connections are seven at most together. onu-config judges them.
 */
int prov_ipoe_write(const char *val, char *err, size_t errcap, prov_done done,
		    void *ctx)
{
	static const char *const keys[] = { "VLAN", "GEM", "MAC", "FAMILY" };
	static const char *const typed[] = {
		"ADDRESS", "GATEWAY", "DNS", "ADDRESS6", "GATEWAY6", "PREFIX6",
	};
	static char text[TEXT_MAX], change[TEXT_MAX];
	char id[VALUE_MAX], kind[VALUE_MAX], v[NAME_MAX], dns[NAME_MAX];
	const char *i = line_value(val, "ID", id, sizeof(id));
	const char *k = line_value(val, "KIND", kind, sizeof(kind));
	bool dhcp = k && !strcmp(k, "dhcp");
	struct account acct[SESSIONS];
	size_t len = 0;
	int n = 0, conns, rc;

	rc = settings_current(text, sizeof(text));
	if (rc) {
		snprintf(err, errcap, "onu-config: the settings in force cannot be read");
		return rc;
	}
	if (i) {
		if (strlen(i) != 1 || *i < '1' || *i > '0' + IPOE) {
			snprintf(err, errcap, "ID: 1 to %d", IPOE);
			return -EINVAL;
		}
		n = *i - '0';
	}
	change[0] = '\0';
	if (n && k && !*k) {
		char conn[16];

		append(change, sizeof(change), &len, "IPOE%d_KIND=\n", n);
		snprintf(conn, sizeof(conn), "ipoe:%d", n);
		lans_drop(text, conn, change, sizeof(change), &len);
		return prov_settings_write(change, err, errcap, done, ctx);
	}
	if (!n || !ipoe_on(text, n)) {
		if (!k || !*k) {
			snprintf(err, errcap, "a new connection needs KIND");
			return -EINVAL;
		}
		accounts_of(text, acct);
		conns = conns_of(text, acct);
		explicit_bzero(acct, sizeof(acct));
		for (int m = 1; m <= IPOE && !n; m++)
			if (!ipoe_on(text, m))
				n = m;
		if (!n || conns >= CONNS) {
			snprintf(err, errcap, "all %d connections are taken", CONNS);
			return -EINVAL;
		}
	}
	if (k)
		append(change, sizeof(change), &len, "IPOE%d_KIND=%s\n", n, k);
	for (size_t j = 0; j < ARRAY_SIZE(keys); j++)
		if (line_value(val, keys[j], v, sizeof(v)))
			append(change, sizeof(change), &len, "IPOE%d_%s=%s\n", n, keys[j], v);
	for (size_t j = 0; j < ARRAY_SIZE(typed); j++) {
		if (dhcp)
			v[0] = '\0';
		else if (!line_value(val, typed[j], v, sizeof(v)))
			continue;
		/* The name servers one blank apart, as the settings keep them. */
		if (!strcmp(typed[j], "DNS")) {
			size_t w = 0;

			dns[0] = '\0';
			for (char *t = strtok(v, " ,\t"); t && w < sizeof(dns);
			     t = strtok(NULL, " ,\t"))
				w += (size_t)snprintf(dns + w, sizeof(dns) - w, "%s%s",
						      w ? " " : "", t);
			memcpy(v, dns, sizeof(v));
		}
		append(change, sizeof(change), &len, "IPOE%d_%s=%s\n", n, typed[j], v);
	}
	return prov_settings_write(change, err, errcap, done, ctx);
}

int prov_ipoe_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_ipoe_write(val, err, errcap, prov_waited, &w), &w);
}

/*
 * A device's fixed address: LAN, the network, MAC, the device, IP, the
 * address it gets, empty or absent to take it away, and NAME. A device has
 * one in a network: a new IP replaces the one it had.
 */
int prov_fixed_write(const char *val, char *err, size_t errcap,
		     prov_done done, void *ctx)
{
	static char text[TEXT_MAX], change[TEXT_MAX];
	static bool used[FIXED_MAX + 1];
	char lanv[VALUE_MAX], macv[VALUE_MAX], ipv[VALUE_MAX], name[NAME_MAX];
	char mac[18], ip[16];
	const char *l = line_value(val, "LAN", lanv, sizeof(lanv));
	const char *m = line_value(val, "MAC", macv, sizeof(macv));
	const char *a = line_value(val, "IP", ipv, sizeof(ipv));
	const char *nm = line_value(val, "NAME", name, sizeof(name));
	size_t len = 0;
	int n = 0, rc;

	if (!l || strlen(l) != 1 || *l < '1' || *l > '0' + LANS || !m) {
		snprintf(err, errcap, "a fixed address needs a network, LAN, and a device, MAC");
		return -EINVAL;
	}
	for (char *p = macv; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += 'a' - 'A';
	rc = settings_current(text, sizeof(text));
	if (rc) {
		snprintf(err, errcap, "onu-config: the settings in force cannot be read");
		return rc;
	}
	memset(used, 0, sizeof(used));
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		int f, lan;

		if (!fixed_of(p, &f, &lan, mac, ip) || f < 1 || f > FIXED_MAX)
			continue;
		used[f] = true;
		if (lan == *l - '0' && !strcasecmp(mac, macv))
			n = f;
	}
	change[0] = '\0';
	if (!a || !*a) {
		if (n)
			append(change, sizeof(change), &len, "FIXED%d=\n", n);
		return prov_settings_write(change, err, errcap, done, ctx);
	}
	for (int f = 1; f <= FIXED_MAX && !n; f++)
		if (!used[f])
			n = f;
	if (!n) {
		snprintf(err, errcap, "all %d fixed addresses are taken", FIXED_MAX);
		return -EINVAL;
	}
	append(change, sizeof(change), &len, "FIXED%d=%s/%s/%s\n", n, l, macv, a);
	if (nm)
		append(change, sizeof(change), &len, "FIXED%d_NAME=%s\n", n, nm);
	return prov_settings_write(change, err, errcap, done, ctx);
}

int prov_fixed_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_fixed_write(val, err, errcap, prov_waited, &w), &w);
}

/*
 * A device of a forward or a DMZ, LAN, 1 to 8, and MAC, xx:xx:xx:xx:xx:xx,
 * which goes into @mac in small letters: false, with why in @err, for one
 * that is none.
 */
static bool device_of(const char *lan, const char *mac, char m[18], char *err,
		      size_t errcap)
{
	if (!lan || strlen(lan) != 1 || *lan < '1' || *lan > '0' + LANS || !mac ||
	    strlen(mac) != 17 || strspn(mac, "0123456789abcdefABCDEF:") != 17) {
		snprintf(err, errcap, "a device is a network, LAN, and a MAC address, MAC");
		return false;
	}
	for (int i = 0; i < 18; i++)
		m[i] = mac[i] >= 'A' && mac[i] <= 'Z' ? (char)(mac[i] - 'A' + 'a') : mac[i];
	return true;
}

/*
 * The device @mac of network @lan keeps its address, which a forward or a
 * DMZ sends to: when it has no fixed address in the settings @text, the one
 * it has now from the network's DHCP server becomes one, a FIXEDk line of
 * @change, which holds @len bytes of @cap. 0, or -EINVAL with why in @err
 * when it has no address to keep, or no fixed address is free.
 */
static int pin(const char *text, int lan, const char *mac, char *change, size_t cap,
	       size_t *len, char *err, size_t errcap)
{
	static bool used[FIXED_MAX + 1];
	char m[18], ip[16];
	int k = 0;

	memset(used, 0, sizeof(used));
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		int f, l;

		if (!fixed_of(p, &f, &l, m, ip) || f < 1 || f > FIXED_MAX)
			continue;
		if (l == lan && !strcasecmp(m, mac))
			return 0;
		used[f] = true;
	}
	for (int f = 1; f <= FIXED_MAX && !k; f++)
		if (!used[f])
			k = f;
	if (!k) {
		snprintf(err, errcap, "all %d fixed addresses are taken", FIXED_MAX);
		return -EINVAL;
	}
	if (!lease_ip(lan, mac, ip)) {
		snprintf(err, errcap, "%s has no address in network %d to keep: give it a fixed one",
			 mac, lan);
		return -EINVAL;
	}
	append(change, cap, len, "FIXED%d=%d/%s/%s\n", k, lan, mac, ip);
	return 0;
}

/*
 * A port, or a range of ports, forwarded from the internet to a device of a
 * local network: ID, 1 to 96, or none for the first free one; CONN, the
 * connection it comes in by, pppoe:N or ipoe:N, empty to remove it; NAME;
 * PROTO, tcp (none), udp or both; EXT_FROM and EXT_TO, the ports it takes
 * (EXT_FROM alone when none); LAN and MAC, the device, which keeps its
 * address (pin()); INT_PORT, the device's port (EXT_FROM when none), for a
 * range its first; and ON, on (none) or off for one kept but not in force.
 * onu-config judges them: no two forwards of a connection take a port of
 * one protocol, and a range goes to the same ports.
 */
int prov_forward_write(const char *val, char *err, size_t errcap, prov_done done,
		       void *ctx)
{
	static char text[TEXT_MAX], change[TEXT_MAX];
	char id[VALUE_MAX], c[VALUE_MAX], name[NAME_MAX], proto[VALUE_MAX],
		from[VALUE_MAX], to[VALUE_MAX], lanv[VALUE_MAX], macv[VALUE_MAX],
		port[VALUE_MAX], on[VALUE_MAX], mac[18], ports[2 * VALUE_MAX];
	const char *i = line_value(val, "ID", id, sizeof(id));
	const char *conn = line_value(val, "CONN", c, sizeof(c));
	const char *nm = line_value(val, "NAME", name, sizeof(name));
	const char *pr = line_value(val, "PROTO", proto, sizeof(proto));
	const char *f = line_value(val, "EXT_FROM", from, sizeof(from));
	const char *t = line_value(val, "EXT_TO", to, sizeof(to));
	const char *l = line_value(val, "LAN", lanv, sizeof(lanv));
	const char *m = line_value(val, "MAC", macv, sizeof(macv));
	const char *ip = line_value(val, "INT_PORT", port, sizeof(port));
	const char *o = line_value(val, "ON", on, sizeof(on));
	bool used[FORWARDS + 1] = { false };
	struct fwd w;
	size_t len = 0;
	int n = 0, rc;

	if (i) {
		if (*i && strlen(i) <= 2 && strspn(i, "0123456789") == strlen(i))
			n = atoi(i);
		if (n < 1 || n > FORWARDS) {
			snprintf(err, errcap, "ID: 1 to %d", FORWARDS);
			return -EINVAL;
		}
	}
	rc = settings_current(text, sizeof(text));
	if (rc) {
		snprintf(err, errcap, "onu-config: the settings in force cannot be read");
		return rc;
	}
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
		if (fwd_of(p, &w) && w.n >= 1 && w.n <= FORWARDS)
			used[w.n] = true;
	change[0] = '\0';
	if (conn && !*conn) {
		if (n && used[n])
			append(change, sizeof(change), &len, "FORWARD%d=\n", n);
		return prov_settings_write(change, err, errcap, done, ctx);
	}
	if (!conn || !f || !*f) {
		snprintf(err, errcap, "a forward needs CONN and EXT_FROM");
		return -EINVAL;
	}
	if (!device_of(l, m, mac, err, errcap))
		return -EINVAL;
	for (int k = 1; k <= FORWARDS && !n; k++)
		if (!used[k])
			n = k;
	if (!n) {
		snprintf(err, errcap, "all %d forwards are taken", FORWARDS);
		return -EINVAL;
	}
	if (t && *t && strcmp(t, f))
		snprintf(ports, sizeof(ports), "%s-%s", f, t);
	else
		snprintf(ports, sizeof(ports), "%s", f);
	append(change, sizeof(change), &len, "FORWARD%d=%s/%s/%s/%s/%s/%s/%s\n", n, conn,
	       pr && *pr ? pr : "tcp", ports, l, mac, ip && *ip ? ip : f,
	       o && !strcmp(o, "off") ? "off" : "on");
	append(change, sizeof(change), &len, "FORWARD%d_NAME=%s\n", n, nm ? nm : "");
	rc = pin(text, *l - '0', mac, change, sizeof(change), &len, err, errcap);
	if (rc)
		return rc;
	return prov_settings_write(change, err, errcap, done, ctx);
}

int prov_forward_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_forward_write(val, err, errcap, prov_waited, &w), &w);
}

/*
 * A connection's DMZ, the device that takes what comes in by it that no
 * forward takes: CONN, the connection; LAN and MAC, the device, which keeps
 * its address (pin()), or MAC empty for none. A connection has one: a new
 * device replaces the one it had.
 */
int prov_dmz_write(const char *val, char *err, size_t errcap, prov_done done,
		   void *ctx)
{
	static char text[TEXT_MAX], change[TEXT_MAX];
	char c[VALUE_MAX], lanv[VALUE_MAX], macv[VALUE_MAX], mac[18], dc[16], dm[18];
	const char *conn = line_value(val, "CONN", c, sizeof(c));
	const char *l = line_value(val, "LAN", lanv, sizeof(lanv));
	const char *m = line_value(val, "MAC", macv, sizeof(macv));
	bool used[DMZS + 1] = { false };
	size_t len = 0;
	int n = 0, rc;

	if (!conn || !*conn) {
		snprintf(err, errcap, "a DMZ is a connection's, CONN");
		return -EINVAL;
	}
	rc = settings_current(text, sizeof(text));
	if (rc) {
		snprintf(err, errcap, "onu-config: the settings in force cannot be read");
		return rc;
	}
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		int d, dl;

		if (!dmz_of(p, &d, dc, &dl, dm) || d < 1 || d > DMZS)
			continue;
		used[d] = true;
		if (!strcmp(dc, conn))
			n = d;
	}
	change[0] = '\0';
	if (!m || !*m) {
		if (n)
			append(change, sizeof(change), &len, "DMZ%d=\n", n);
		return prov_settings_write(change, err, errcap, done, ctx);
	}
	if (!device_of(l, m, mac, err, errcap))
		return -EINVAL;
	for (int k = 1; k <= DMZS && !n; k++)
		if (!used[k])
			n = k;
	if (!n) {
		snprintf(err, errcap, "all %d DMZs are taken", DMZS);
		return -EINVAL;
	}
	append(change, sizeof(change), &len, "DMZ%d=%s/%s/%s\n", n, conn, l, mac);
	rc = pin(text, *l - '0', mac, change, sizeof(change), &len, err, errcap);
	if (rc)
		return rc;
	return prov_settings_write(change, err, errcap, done, ctx);
}

int prov_dmz_set(const char *val, char *err, size_t errcap)
{
	struct prov_wait w = { .err = err, .errcap = errcap };

	return prov_wait(prov_dmz_write(val, err, errcap, prov_waited, &w), &w);
}
