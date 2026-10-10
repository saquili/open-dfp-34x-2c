// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The settings of the stick's data path, as one table (settings.h). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "settings.h"

enum kind {
	K_MODE,
	K_PROTO,
	K_GEM,
	K_VLAN,
	K_PREFIX,
	K_PREFIX_OPT,
	K_ADDR,
	K_ADDR_OPT,
	K_ONOFF,
	K_MAC_OPT,
	K_FAMILY,
	K_HOST_VLAN,
};

/* The keys of /run/onu.conf, as onu-mode reads them, and their defaults. */
static const struct {
	const char *name;
	enum kind kind;
	const char *dflt;
} g_keys[SETTINGS_NKEYS] = {
	[SET_MODE] = { "MODE", K_MODE, "" },
	[SET_WAN_GEM] = { "WAN_GEM", K_GEM, "auto" },
	[SET_WAN_VLAN] = { "WAN_VLAN", K_VLAN, "" },
	[SET_WAN_PROTO] = { "WAN_PROTO", K_PROTO, "dhcp" },
	[SET_WAN_ADDRESS] = { "WAN_ADDRESS", K_PREFIX_OPT, "" },
	[SET_WAN_GATEWAY] = { "WAN_GATEWAY", K_ADDR_OPT, "" },
	[SET_WAN_MAC] = { "WAN_MAC", K_MAC_OPT, "" },
	[SET_LAN_ADDRESS] = { "LAN_ADDRESS", K_PREFIX, "192.168.1.1/24" },
	[SET_LAN_DHCP] = { "LAN_DHCP", K_ONOFF, "on" },
	[SET_LAN_DHCP_START] = { "LAN_DHCP_START", K_ADDR, "192.168.1.100" },
	[SET_LAN_DHCP_END] = { "LAN_DHCP_END", K_ADDR, "192.168.1.199" },
};

/*
 * The settings of each session and their defaults: PASS, whether it hands
 * its address down to the router or keeps it, and HOST_VLAN, the VLAN of the
 * host link it hands it down in, none for its own VLAN or untagged for none.
 */
static const struct {
	const char *suffix;
	enum kind kind;
	const char *dflt;
} g_skeys[SETTINGS_NSKEYS] = {
	[SET_S_GEM] = { "GEM", K_GEM, "auto" },
	[SET_S_VLAN] = { "VLAN", K_VLAN, "" },
	[SET_S_MAC] = { "MAC", K_MAC_OPT, "" },
	[SET_S_FAMILY] = { "FAMILY", K_FAMILY, "both" },
	[SET_S_PASS] = { "PASS", K_ONOFF, "on" },
	[SET_S_HOST_VLAN] = { "HOST_VLAN", K_HOST_VLAN, "" },
};

/* The one link to the router of before: LAN_VLAN, or the host link itself. */
#define LEGACY_LAN "LAN_VLAN"

static bool number(const char *s, unsigned long lo, unsigned long hi,
		   unsigned long *out)
{
	size_t n = strlen(s);
	unsigned long v;

	if (!n || n > 5 || strspn(s, "0123456789") != n || (n > 1 && *s == '0'))
		return false;
	v = strtoul(s, NULL, 10);
	if (out)
		*out = v;
	return v >= lo && v <= hi;
}

/* a.b.c.d, in decimal without leading zeros. */
static bool ipv4(const char *s, uint32_t *ip)
{
	uint32_t v = 0;

	for (int part = 0; part < 4; part++) {
		size_t n = strspn(s, "0123456789");
		char num[4];
		unsigned long octet;

		if (!n || n > 3)
			return false;
		memcpy(num, s, n);
		num[n] = '\0';
		if (!number(num, 0, 255, &octet))
			return false;
		v = v << 8 | (uint32_t)octet;
		s += n;
		if (part < 3 && *s++ != '.')
			return false;
	}
	if (ip)
		*ip = v;
	return *s == '\0';
}

/* a.b.c.d/len, with a length from 1 to 32. */
static bool prefix(const char *s, uint32_t *ip, uint32_t *mask)
{
	char addr[SETTINGS_VALUE_MAX];
	const char *slash = strchr(s, '/');
	unsigned long len;

	if (!slash || (size_t)(slash - s) >= sizeof(addr))
		return false;
	memcpy(addr, s, (size_t)(slash - s));
	addr[slash - s] = '\0';
	if (!ipv4(addr, ip) || !number(slash + 1, 1, 32, &len))
		return false;
	if (mask)
		*mask = len == 32 ? 0xffffffffu : ~(0xffffffffu >> len);
	return true;
}

/*
 * xx:xx:xx:xx:xx:xx, in hexadecimal, an address one station may send from:
 * neither a group address nor all zeros.
 */
static bool mac(const char *s)
{
	unsigned long first;
	bool zero = true;

	if (strlen(s) != 17)
		return false;
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':')
				return false;
		} else if (!strchr("0123456789abcdefABCDEF", s[i])) {
			return false;
		} else if (s[i] != '0') {
			zero = false;
		}
	}
	first = strtoul(s, NULL, 16);
	return !zero && !(first & 1);
}

static bool valid(enum kind kind, const char *v)
{
	switch (kind) {
	case K_MODE:
		/* Only router is a shape; the others are words of before. */
		return !*v || !strcmp(v, "bridge") || !strcmp(v, "router") ||
		       !strcmp(v, "passthrough");
	case K_FAMILY:
		return !strcmp(v, "ipv4") || !strcmp(v, "ipv6") ||
		       !strcmp(v, "both");
	case K_PROTO:
		return !strcmp(v, "pppoe") || !strcmp(v, "dhcp") ||
		       !strcmp(v, "static");
	case K_GEM:
		return !strcmp(v, "auto") || number(v, 0, 4095, NULL);
	case K_VLAN:
		return !*v || number(v, 1, 4094, NULL);
	case K_HOST_VLAN:
		return !*v || !strcmp(v, "untagged") || number(v, 1, 4094, NULL);
	case K_PREFIX:
		return prefix(v, NULL, NULL);
	case K_PREFIX_OPT:
		return !*v || prefix(v, NULL, NULL);
	case K_ADDR:
		return ipv4(v, NULL);
	case K_ADDR_OPT:
		return !*v || ipv4(v, NULL);
	case K_ONOFF:
		return !strcmp(v, "on") || !strcmp(v, "off");
	case K_MAC_OPT:
		return !*v || mac(v);
	}
	return false;
}

/* A user or a secret: printable, without blanks, which separate the two. */
static bool word(const char *v, size_t max)
{
	size_t n = strlen(v);

	if (!n || n > max)
		return false;
	for (size_t i = 0; i < n; i++)
		if (v[i] <= ' ' || v[i] > '~')
			return false;
	return true;
}

static int key_index(const char *name)
{
	for (int k = 0; k < SETTINGS_NKEYS; k++)
		if (!strcmp(name, g_keys[k].name))
			return k;
	return -1;
}

/* SESSIONn_<suffix>: the session, from 0, and the setting; or false. */
static bool skey_index(const char *name, int *n, int *k)
{
	if (strncmp(name, "SESSION", 7) || name[7] < '1' ||
	    name[7] > '0' + SETTINGS_SESSIONS || name[8] != '_')
		return false;
	for (int i = 0; i < SETTINGS_NSKEYS; i++) {
		if (!strcmp(name + 9, g_skeys[i].suffix)) {
			*n = name[7] - '1';
			*k = i;
			return true;
		}
	}
	return false;
}

/* PPPOEn_USER or PPPOEn_SECRET, and PPPOE_USER of before for the first. */
static bool account_key(const char *name, int *n, bool *user)
{
	const char *rest;

	if (strncmp(name, "PPPOE", 5))
		return false;
	if (name[5] == '_') {
		*n = 0;
		rest = name + 6;
	} else if (name[5] >= '1' && name[5] <= '0' + SETTINGS_SESSIONS &&
		   name[6] == '_') {
		*n = name[5] - '1';
		rest = name + 7;
	} else {
		return false;
	}
	if (!strcmp(rest, "USER"))
		*user = true;
	else if (!strcmp(rest, "SECRET"))
		*user = false;
	else
		return false;
	return true;
}

void settings_defaults(struct settings *s)
{
	memset(s, 0, sizeof(*s));
	for (int k = 0; k < SETTINGS_NKEYS; k++)
		snprintf(s->val[k], SETTINGS_VALUE_MAX, "%s", g_keys[k].dflt);
	for (int n = 0; n < SETTINGS_SESSIONS; n++)
		for (int k = 0; k < SETTINGS_NSKEYS; k++)
			snprintf(s->sval[n][k], SETTINGS_VALUE_MAX, "%s",
				 g_skeys[k].dflt);
}

int settings_line(struct settings *s, const char *line, char *err,
		  size_t errcap)
{
	char key[32];
	const char *eq, *v;
	size_t klen;
	bool user;
	int k, n;

	if (!*line || *line == '#')
		return 0;
	eq = strchr(line, '=');
	if (!eq) {
		snprintf(err, errcap, "a line without '='");
		return -1;
	}
	klen = (size_t)(eq - line);
	if (klen >= sizeof(key)) {
		snprintf(err, errcap, "a line of no setting");
		return -1;
	}
	memcpy(key, line, klen);
	key[klen] = '\0';
	v = eq + 1;

	if (account_key(key, &n, &user)) {
		struct settings_account *a = &s->acct[n];
		size_t max = user ? SETTINGS_USER_MAX : SETTINGS_SECRET_MAX;

		/* An empty user removes the session's account. */
		if (user && !*v) {
			memset(a, 0, sizeof(*a));
			return 0;
		}
		if (!word(v, max)) {
			snprintf(err, errcap,
				 "%s: printable, without blanks, up to %zu characters",
				 key, max);
			return -1;
		}
		memcpy(user ? a->user : a->secret, v, strlen(v) + 1);
		return 0;
	}

	k = key_index(key);
	if (k >= 0) {
		if (!valid(g_keys[k].kind, v)) {
			snprintf(err, errcap, "%s: not a value of it", key);
			return -1;
		}
		snprintf(s->val[k], SETTINGS_VALUE_MAX, "%s", v);
		return 0;
	}
	if (skey_index(key, &n, &k)) {
		if (!valid(g_skeys[k].kind, v)) {
			snprintf(err, errcap, "%s: not a value of it", key);
			return -1;
		}
		snprintf(s->sval[n][k], SETTINGS_VALUE_MAX, "%s", v);
		return 0;
	}
	if (!strcmp(key, LEGACY_LAN)) {
		if (!valid(K_VLAN, v)) {
			snprintf(err, errcap, "%s: not a value of it", key);
			return -1;
		}
		snprintf(s->legacy_lan, SETTINGS_VALUE_MAX, "%s", v);
		s->had_legacy_lan = true;
		return 0;
	}
	snprintf(err, errcap, "%s: no such setting", key);
	return -1;
}

void settings_settle(struct settings *s)
{
	struct settings_account *first = &s->acct[0];
	char (*one)[SETTINGS_VALUE_MAX] = s->sval[0];

	if (!strcmp(s->val[SET_MODE], "bridge") ||
	    !strcmp(s->val[SET_MODE], "passthrough"))
		s->val[SET_MODE][0] = '\0';

	/* The single session of before: its settings were the WAN's. */
	if (!*one[SET_S_VLAN])
		memcpy(one[SET_S_VLAN], s->val[SET_WAN_VLAN], SETTINGS_VALUE_MAX);
	if (!*one[SET_S_MAC])
		memcpy(one[SET_S_MAC], s->val[SET_WAN_MAC], SETTINGS_VALUE_MAX);
	if (!strcmp(one[SET_S_GEM], "auto"))
		memcpy(one[SET_S_GEM], s->val[SET_WAN_GEM], SETTINGS_VALUE_MAX);
	if (*first->user && strcmp(s->val[SET_MODE], "router")) {
		snprintf(s->val[SET_WAN_VLAN], SETTINGS_VALUE_MAX, "%s",
			 g_keys[SET_WAN_VLAN].dflt);
		snprintf(s->val[SET_WAN_MAC], SETTINGS_VALUE_MAX, "%s",
			 g_keys[SET_WAN_MAC].dflt);
		snprintf(s->val[SET_WAN_GEM], SETTINGS_VALUE_MAX, "%s",
			 g_keys[SET_WAN_GEM].dflt);
	}

	/* The link of before is the link of each session that names none. */
	if (s->had_legacy_lan)
		for (int n = 0; n < SETTINGS_SESSIONS; n++)
			if (!*s->sval[n][SET_S_HOST_VLAN])
				snprintf(s->sval[n][SET_S_HOST_VLAN],
					 SETTINGS_VALUE_MAX, "%s",
					 *s->legacy_lan ? s->legacy_lan :
							  "untagged");
	s->had_legacy_lan = false;
	s->legacy_lan[0] = '\0';
}

/*
 * The VLAN of the host link session @n hands its address down in: its
 * HOST_VLAN, else its own VLAN; "" for none.
 */
static const char *down_vlan(const struct settings *s, int n)
{
	const char *h = s->sval[n][SET_S_HOST_VLAN];

	if (!strcmp(h, "untagged"))
		return "";
	return *h ? h : s->sval[n][SET_S_VLAN];
}

/* Whether session @n has an account and is told to hand its address down. */
static bool hands_down(const struct settings *s, int n)
{
	return *s->acct[n].user && strcmp(s->sval[n][SET_S_PASS], "off");
}

/* Whether session @n carries the family @fam, "ipv4" or "ipv6". */
static bool carries(const struct settings *s, int n, const char *fam)
{
	const char *f = s->sval[n][SET_S_FAMILY];

	return !strcmp(f, "both") || !strcmp(f, fam);
}

bool settings_coherent(const struct settings *s, char *err, size_t errcap)
{
	uint32_t ip, mask, start, end, gw;

	if (!strcmp(s->val[SET_WAN_PROTO], "static")) {
		if (!prefix(s->val[SET_WAN_ADDRESS], &ip, &mask) ||
		    !ipv4(s->val[SET_WAN_GATEWAY], &gw)) {
			snprintf(err, errcap,
				 "a static WAN needs WAN_ADDRESS and WAN_GATEWAY");
			return false;
		}
		if ((gw & mask) != (ip & mask) || gw == ip) {
			snprintf(err, errcap,
				 "WAN_GATEWAY is not another address of WAN_ADDRESS's network");
			return false;
		}
	}
	for (int n = 0; n < SETTINGS_SESSIONS; n++) {
		if (!*s->acct[n].user != !*s->acct[n].secret) {
			snprintf(err, errcap,
				 "session %d: an account needs PPPOE%d_USER and PPPOE%d_SECRET",
				 n + 1, n + 1, n + 1);
			return false;
		}
	}
	if (!strcmp(s->val[SET_MODE], "router") &&
	    !strcmp(s->val[SET_WAN_PROTO], "pppoe") && !*s->acct[0].user) {
		snprintf(err, errcap,
			 "a PPPoE WAN takes session 1's account: PPPOE1_USER and PPPOE1_SECRET");
		return false;
	}
	/* Sessions in one VLAN share its interface, and so its MAC address. */
	for (int a = 0; a < SETTINGS_SESSIONS; a++) {
		for (int b = a + 1; b < SETTINGS_SESSIONS; b++) {
			const char *ma = s->sval[a][SET_S_MAC];
			const char *mb = s->sval[b][SET_S_MAC];

			if (!*s->acct[a].user || !*s->acct[b].user ||
			    strcmp(s->sval[a][SET_S_GEM], s->sval[b][SET_S_GEM]) ||
			    strcmp(s->sval[a][SET_S_VLAN], s->sval[b][SET_S_VLAN]))
				continue;
			if (*ma && *mb && strcasecmp(ma, mb)) {
				snprintf(err, errcap,
					 "sessions %d and %d are in one VLAN and send from one MAC address",
					 a + 1, b + 1);
				return false;
			}
		}
	}

	/* In one VLAN to the host one session hands IPv4 down, and one IPv6. */
	for (int a = 0; a < SETTINGS_SESSIONS; a++) {
		for (int b = a + 1; b < SETTINGS_SESSIONS; b++) {
			const char *both = "";

			if (!hands_down(s, a) || !hands_down(s, b) ||
			    strcmp(down_vlan(s, a), down_vlan(s, b)))
				continue;
			if (carries(s, a, "ipv4") && carries(s, b, "ipv4"))
				both = "an IPv4 address";
			else if (carries(s, a, "ipv6") && carries(s, b, "ipv6"))
				both = "an IPv6 prefix";
			if (*both) {
				snprintf(err, errcap,
					 "sessions %d and %d both hand %s down in one VLAN to the host",
					 a + 1, b + 1, both);
				return false;
			}
		}
	}

	if (!prefix(s->val[SET_LAN_ADDRESS], &ip, &mask) ||
	    !ipv4(s->val[SET_LAN_DHCP_START], &start) ||
	    !ipv4(s->val[SET_LAN_DHCP_END], &end) ||
	    (start & mask) != (ip & mask) || (end & mask) != (ip & mask) ||
	    start > end) {
		snprintf(err, errcap,
			 "LAN_DHCP_START to LAN_DHCP_END is not a range of LAN_ADDRESS's network");
		return false;
	}
	return true;
}

size_t settings_format(const struct settings *s, char *out, size_t cap)
{
	size_t len = 0;

	for (int k = 0; k < SETTINGS_NKEYS; k++)
		len += (size_t)snprintf(out + (len < cap ? len : cap),
					len < cap ? cap - len : 0, "%s=%s\n",
					g_keys[k].name, s->val[k]);
	/* A session's settings are kept while it has an account. */
	for (int n = 0; n < SETTINGS_SESSIONS; n++) {
		if (!*s->acct[n].user)
			continue;
		for (int k = 0; k < SETTINGS_NSKEYS; k++)
			len += (size_t)snprintf(out + (len < cap ? len : cap),
						len < cap ? cap - len : 0,
						"SESSION%d_%s=%s\n", n + 1,
						g_skeys[k].suffix, s->sval[n][k]);
	}
	return len;
}
