// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The settings of the stick's data path, as one table (settings.h). */

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "settings.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

enum kind {
	K_GEM,
	K_VLAN,
	K_ONOFF,
	K_MAC_OPT,
	K_FAMILY,
	K_HOST_VLAN,
	K_CONN,
	K_NAME,
	K_CIDR,
	K_IPV4_OPT,
	K_LEASE,
	K_SUBNET6,
	K_IPOE_KIND,
	K_ADDRS,
	K_CIDR6,
	K_IPV6_OPT,
	K_PREFIX6,
};

/*
 * The keys of before that are not a session's: the WAN's GEM port, VLAN and
 * MAC address of the single session of before, which the first session
 * takes over (settings_settle()), and their defaults. They are read, and
 * never written.
 */
static const struct {
	const char *name;
	enum kind kind;
	const char *dflt;
} g_keys[SETTINGS_NKEYS] = {
	[SET_WAN_GEM] = { "WAN_GEM", K_GEM, "auto" },
	[SET_WAN_VLAN] = { "WAN_VLAN", K_VLAN, "" },
	[SET_WAN_MAC] = { "WAN_MAC", K_MAC_OPT, "" },
};

/*
 * The keys of the bench's router of before, which the stick no longer has:
 * taken with any value, and left out.
 */
static const char *const g_gone[] = {
	"MODE", "WAN_PROTO", "WAN_ADDRESS", "WAN_GATEWAY", "LAN_ADDRESS",
	"LAN_DHCP", "LAN_DHCP_START", "LAN_DHCP_END",
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

/*
 * The settings of each IPoE connection and their defaults: KIND, static or
 * dhcp, none for no connection; the family IPv4, as a router's WAN has it
 * where nothing else is said. The addresses are a static connection's.
 */
static const struct {
	const char *suffix;
	enum kind kind;
	const char *dflt;
} g_ikeys[SETTINGS_NIKEYS] = {
	[SET_I_KIND] = { "KIND", K_IPOE_KIND, "" },
	[SET_I_GEM] = { "GEM", K_GEM, "auto" },
	[SET_I_VLAN] = { "VLAN", K_VLAN, "" },
	[SET_I_MAC] = { "MAC", K_MAC_OPT, "" },
	[SET_I_FAMILY] = { "FAMILY", K_FAMILY, "ipv4" },
	[SET_I_ADDRESS] = { "ADDRESS", K_CIDR, "" },
	[SET_I_GATEWAY] = { "GATEWAY", K_IPV4_OPT, "" },
	[SET_I_DNS] = { "DNS", K_ADDRS, "" },
	[SET_I_ADDRESS6] = { "ADDRESS6", K_CIDR6, "" },
	[SET_I_GATEWAY6] = { "GATEWAY6", K_IPV6_OPT, "" },
	[SET_I_PREFIX6] = { "PREFIX6", K_PREFIX6, "" },
};

/*
 * The settings of each local network and their defaults: CONN, the
 * connection it goes out by, none for no network; DHCP on, a day's lease.
 */
static const struct {
	const char *suffix;
	enum kind kind;
	const char *dflt;
} g_lkeys[SETTINGS_NLKEYS] = {
	[SET_L_CONN] = { "CONN", K_CONN, "" },
	[SET_L_NAME] = { "NAME", K_NAME, "" },
	[SET_L_VLAN] = { "VLAN", K_VLAN, "" },
	[SET_L_ADDRESS] = { "ADDRESS", K_CIDR, "" },
	[SET_L_DHCP] = { "DHCP", K_ONOFF, "on" },
	[SET_L_POOL_FROM] = { "POOL_FROM", K_IPV4_OPT, "" },
	[SET_L_POOL_TO] = { "POOL_TO", K_IPV4_OPT, "" },
	[SET_L_LEASE] = { "LEASE", K_LEASE, "86400" },
	[SET_L_IPV6] = { "IPV6", K_ONOFF, "off" },
	[SET_L_IPV6_SUBNET] = { "IPV6_SUBNET", K_SUBNET6, "" },
};

/* A lease runs for a minute to a week. */
#define LEASE_MIN 60ul
#define LEASE_MAX 604800ul

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

/* a.b.c.d, each part a decimal number up to 255 without a leading zero. */
static bool ipv4(const char *s, uint32_t *out)
{
	uint32_t a = 0;

	for (int part = 0; part < 4; part++) {
		size_t n = strspn(s, "0123456789");
		unsigned long v;

		if (!n || n > 3 || (n > 1 && *s == '0'))
			return false;
		v = strtoul(s, NULL, 10);
		if (v > 255)
			return false;
		a = a << 8 | (uint32_t)v;
		s += n;
		if (part < 3 && *s++ != '.')
			return false;
	}
	if (*s)
		return false;
	if (out)
		*out = a;
	return true;
}

static uint32_t mask_of(unsigned long len)
{
	return len ? ~0u << (32 - len) : 0;
}

/*
 * a.b.c.d/len: an address of a network of 8 to 30 bits of prefix, neither
 * its first address nor its last.
 */
static bool cidr(const char *s, uint32_t *addr, unsigned long *len)
{
	char a[16];
	const char *slash = strchr(s, '/');
	uint32_t ip, m;
	unsigned long l;

	if (!slash || (size_t)(slash - s) >= sizeof(a))
		return false;
	memcpy(a, s, (size_t)(slash - s));
	a[slash - s] = '\0';
	if (!ipv4(a, &ip) || !number(slash + 1, 8, 30, &l))
		return false;
	m = mask_of(l);
	if ((ip & ~m) == 0 || (ip & ~m) == ~m)
		return false;
	if (addr)
		*addr = ip;
	if (len)
		*len = l;
	return true;
}

/* An IPv6 address, as inet_pton(3) takes it: no zone, no length. */
static bool ipv6(const char *s, struct in6_addr *out)
{
	struct in6_addr a;

	if (strlen(s) >= INET6_ADDRSTRLEN || inet_pton(AF_INET6, s, &a) != 1)
		return false;
	if (out)
		*out = a;
	return true;
}

/* addr/len, an IPv6 address with a prefix length from @lo to @hi. */
static bool cidr6(const char *s, unsigned long lo, unsigned long hi,
		  struct in6_addr *addr, unsigned long *len)
{
	char a[INET6_ADDRSTRLEN];
	const char *slash = strchr(s, '/');
	unsigned long l;

	if (!slash || (size_t)(slash - s) >= sizeof(a))
		return false;
	memcpy(a, s, (size_t)(slash - s));
	a[slash - s] = '\0';
	if (!ipv6(a, addr) || !number(slash + 1, lo, hi, &l))
		return false;
	if (len)
		*len = l;
	return true;
}

/*
 * prefix/len, an IPv6 prefix of 16 to 64 bits for the networks behind the
 * stick, each a /64 of it: its first address, nothing after the prefix.
 */
static bool prefix6(const char *s)
{
	struct in6_addr a;
	unsigned long len;

	if (!cidr6(s, 16, 64, &a, &len))
		return false;
	for (unsigned long bit = len; bit < 128; bit++)
		if (a.s6_addr[bit / 8] & (0x80 >> (bit % 8)))
			return false;
	return true;
}

/* Addresses, IPv4 or IPv6, one blank between two: name servers. */
static bool addrs(const char *v)
{
	char a[INET6_ADDRSTRLEN];

	while (*v) {
		size_t n = strcspn(v, " ");

		if (!n || n >= sizeof(a))
			return false;
		memcpy(a, v, n);
		a[n] = '\0';
		if (!ipv4(a, NULL) && !ipv6(a, NULL))
			return false;
		v += n;
		if (*v && !*++v)
			return false;
	}
	return true;
}

/* A name for people: printable, UTF-8 too, up to 32 characters. */
#define NAME_CHARS 32

static bool name(const char *v)
{
	size_t chars = 0;

	for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
		if (*p < ' ' || *p == 0x7f)
			return false;
		/* A character is a byte that continues none. */
		chars += (*p & 0xc0) != 0x80;
	}
	return chars <= NAME_CHARS && strlen(v) < SETTINGS_VALUE_MAX;
}

/*
 * pppoe:N, the PPPoE session N, or ipoe:N, the IPoE connection N: whether it
 * is the latter in @ipoe, N from 0 in @n.
 */
static bool conn(const char *v, bool *ipoe, int *n)
{
	bool i = !strncmp(v, "ipoe:", 5);
	const char *d = v + (i ? 5 : 6);

	if ((!i && strncmp(v, "pppoe:", 6)) || d[0] < '1' ||
	    d[0] > '0' + (i ? SETTINGS_IPOE : SETTINGS_SESSIONS) || d[1])
		return false;
	if (ipoe)
		*ipoe = i;
	if (n)
		*n = d[0] - '1';
	return true;
}

static bool valid(enum kind kind, const char *v)
{
	switch (kind) {
	case K_FAMILY:
		return !strcmp(v, "ipv4") || !strcmp(v, "ipv6") ||
		       !strcmp(v, "both");
	case K_GEM:
		return !strcmp(v, "auto") || number(v, 0, 4095, NULL);
	case K_VLAN:
		return !*v || number(v, 1, 4094, NULL);
	case K_HOST_VLAN:
		return !*v || !strcmp(v, "untagged") || number(v, 1, 4094, NULL);
	case K_ONOFF:
		return !strcmp(v, "on") || !strcmp(v, "off");
	case K_MAC_OPT:
		return !*v || mac(v);
	case K_CONN:
		return !*v || conn(v, NULL, NULL);
	case K_NAME:
		return name(v);
	case K_CIDR:
		return !*v || cidr(v, NULL, NULL);
	case K_IPV4_OPT:
		return !*v || ipv4(v, NULL);
	case K_LEASE: {
		size_t n = strlen(v);
		unsigned long l;

		if (!n || n > 6 || strspn(v, "0123456789") != n || *v == '0')
			return false;
		l = strtoul(v, NULL, 10);
		return l >= LEASE_MIN && l <= LEASE_MAX;
	}
	case K_SUBNET6: {
		size_t n = strlen(v);

		return !n || (n <= 4 && strspn(v, "0123456789abcdef") == n);
	}
	case K_IPOE_KIND:
		return !*v || !strcmp(v, "static") || !strcmp(v, "dhcp");
	case K_ADDRS:
		return addrs(v);
	case K_CIDR6:
		return !*v || cidr6(v, 1, 128, NULL, NULL);
	case K_IPV6_OPT:
		return !*v || ipv6(v, NULL);
	case K_PREFIX6:
		return !*v || prefix6(v);
	}
	return false;
}

/*
 * L/MAC/IP: the network L, 1 to 8, the device's MAC address and the
 * address it gets.
 */
static bool fixed_spec(const char *v, int *lan, char *macv, uint32_t *ip)
{
	char m[18];

	if (v[0] < '1' || v[0] > '0' + SETTINGS_LANS || v[1] != '/' ||
	    strlen(v) < 2 + 17 + 1 || v[19] != '/')
		return false;
	memcpy(m, v + 2, 17);
	m[17] = '\0';
	if (!mac(m) || !ipv4(v + 20, ip))
		return false;
	if (lan)
		*lan = v[0] - '1';
	if (macv)
		memcpy(macv, m, sizeof(m));
	return true;
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

/* IPOEn_<suffix>: the connection, from 0, and the setting; or false. */
static bool ikey_index(const char *name, int *n, int *k)
{
	if (strncmp(name, "IPOE", 4) || name[4] < '1' ||
	    name[4] > '0' + SETTINGS_IPOE || name[5] != '_')
		return false;
	for (int i = 0; i < SETTINGS_NIKEYS; i++) {
		if (!strcmp(name + 6, g_ikeys[i].suffix)) {
			*n = name[4] - '1';
			*k = i;
			return true;
		}
	}
	return false;
}

/* LANn_<suffix>: the network, from 0, and the setting; or false. */
static bool lkey_index(const char *name, int *n, int *k)
{
	if (strncmp(name, "LAN", 3) || name[3] < '1' ||
	    name[3] > '0' + SETTINGS_LANS || name[4] != '_')
		return false;
	for (int i = 0; i < SETTINGS_NLKEYS; i++) {
		if (!strcmp(name + 5, g_lkeys[i].suffix)) {
			*n = name[3] - '1';
			*k = i;
			return true;
		}
	}
	return false;
}

/* FIXEDn or FIXEDn_NAME: the fixed address, from 0; or false. */
static bool fixed_key(const char *name, int *n, bool *isname)
{
	const char *p = name + 5;
	size_t d = strspn(p, "0123456789");
	unsigned long v;

	if (strncmp(name, "FIXED", 5) || !d || d > 3 || *p == '0')
		return false;
	v = strtoul(p, NULL, 10);
	if (v < 1 || v > SETTINGS_FIXED)
		return false;
	if (!p[d])
		*isname = false;
	else if (!strcmp(p + d, "_NAME"))
		*isname = true;
	else
		return false;
	*n = (int)v - 1;
	return true;
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
	for (int n = 0; n < SETTINGS_IPOE; n++)
		for (int k = 0; k < SETTINGS_NIKEYS; k++)
			snprintf(s->ival[n][k], SETTINGS_VALUE_MAX, "%s",
				 g_ikeys[k].dflt);
	for (int n = 0; n < SETTINGS_LANS; n++)
		for (int k = 0; k < SETTINGS_NLKEYS; k++)
			snprintf(s->lval[n][k], SETTINGS_VALUE_MAX, "%s",
				 g_lkeys[k].dflt);
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

	for (size_t g = 0; g < ARRAY_SIZE(g_gone); g++)
		if (!strcmp(key, g_gone[g]))
			return 0;
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
	if (ikey_index(key, &n, &k)) {
		if (!valid(g_ikeys[k].kind, v)) {
			snprintf(err, errcap, "%s: not a value of it", key);
			return -1;
		}
		snprintf(s->ival[n][k], SETTINGS_VALUE_MAX, "%s", v);
		return 0;
	}
	if (lkey_index(key, &n, &k)) {
		if (!valid(g_lkeys[k].kind, v)) {
			snprintf(err, errcap, "%s: not a value of it", key);
			return -1;
		}
		snprintf(s->lval[n][k], SETTINGS_VALUE_MAX, "%s", v);
		return 0;
	}
	if (fixed_key(key, &n, &user)) {
		struct settings_fixed *f = &s->fixed[n];

		if (user ? !name(v) : *v && !fixed_spec(v, NULL, NULL, NULL)) {
			snprintf(err, errcap, "%s: not a value of it", key);
			return -1;
		}
		if (user)
			snprintf(f->name, sizeof(f->name), "%s", v);
		else
			snprintf(f->spec, sizeof(f->spec), "%s", v);
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
	char (*one)[SETTINGS_VALUE_MAX] = s->sval[0];

	/* The single session of before: its settings were the WAN's. */
	if (!*one[SET_S_VLAN])
		memcpy(one[SET_S_VLAN], s->val[SET_WAN_VLAN], SETTINGS_VALUE_MAX);
	if (!*one[SET_S_MAC])
		memcpy(one[SET_S_MAC], s->val[SET_WAN_MAC], SETTINGS_VALUE_MAX);
	if (!strcmp(one[SET_S_GEM], "auto"))
		memcpy(one[SET_S_GEM], s->val[SET_WAN_GEM], SETTINGS_VALUE_MAX);
	for (int k = 0; k < SETTINGS_NKEYS; k++)
		snprintf(s->val[k], SETTINGS_VALUE_MAX, "%s", g_keys[k].dflt);

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

/* Whether the VLAN @v, a setting's value, is a management VLAN of @s. */
static bool mgmt(const struct settings *s, const char *v)
{
	unsigned long vid = strtoul(v, NULL, 10);

	return *v && vid && (vid == s->mgmt_vlan[0] || vid == s->mgmt_vlan[1]);
}

/* Whether IPoE connection @n is set up: it has a kind. */
static bool ipoe_on(const struct settings *s, int n)
{
	return *s->ival[n][SET_I_KIND];
}

/* Whether IPoE connection @n carries the family @fam, "ipv4" or "ipv6". */
static bool ipoe_carries(const struct settings *s, int n, const char *fam)
{
	const char *f = s->ival[n][SET_I_FAMILY];

	return !strcmp(f, "both") || !strcmp(f, fam);
}

/* Whether network @n is set up: it has a connection. */
static bool lan_on(const struct settings *s, int n)
{
	return *s->lval[n][SET_L_CONN];
}

/*
 * A local network goes out by a session that keeps its address on the
 * stick, in a VLAN of its own on the host link, which no session hands an
 * address down in and which is no management VLAN, with an address of its
 * own; the addresses DHCP leases are in it, and do not hold the stick's.
 * No two networks share a VLAN, or addresses.
 */
static bool lans_coherent(const struct settings *s, char *err, size_t errcap)
{
	for (int a = 0; a < SETTINGS_LANS; a++) {
		const char(*la)[SETTINGS_VALUE_MAX] = s->lval[a];
		const char *vlan = la[SET_L_VLAN];
		uint32_t ip, m, from = 0, to = 0;
		unsigned long len;
		bool by_ipoe = false;
		int sn = 0;

		if (!lan_on(s, a))
			continue;
		if (!cidr(la[SET_L_ADDRESS], &ip, &len)) {
			snprintf(err, errcap,
				 "network %d: it needs an address, LAN%d_ADDRESS",
				 a + 1, a + 1);
			return false;
		}
		(void)conn(la[SET_L_CONN], &by_ipoe, &sn);
		if (by_ipoe && !ipoe_on(s, sn)) {
			snprintf(err, errcap,
				 "network %d: connection %d is not set up",
				 a + 1, sn + 1);
			return false;
		}
		/* Its devices go out behind the connection's IPv4 address. */
		if (by_ipoe && !ipoe_carries(s, sn, "ipv4")) {
			snprintf(err, errcap,
				 "network %d: connection %d carries no IPv4",
				 a + 1, sn + 1);
			return false;
		}
		if (!by_ipoe && !*s->acct[sn].user) {
			snprintf(err, errcap,
				 "network %d: session %d has no account", a + 1,
				 sn + 1);
			return false;
		}
		if (!by_ipoe && strcmp(s->sval[sn][SET_S_PASS], "off")) {
			snprintf(err, errcap,
				 "network %d: session %d hands its address down; a network goes out by a session that keeps it",
				 a + 1, sn + 1);
			return false;
		}
		if (mgmt(s, vlan)) {
			snprintf(err, errcap,
				 "network %d: VLAN %s is the management VLAN",
				 a + 1, vlan);
			return false;
		}
		for (int n = 0; n < SETTINGS_SESSIONS; n++) {
			if (!hands_down(s, n) || strcmp(down_vlan(s, n), vlan))
				continue;
			snprintf(err, errcap,
				 "network %d: session %d hands its address down in %s",
				 a + 1, n + 1, *vlan ? vlan : "the untagged link");
			return false;
		}
		/* A route to the one would take what goes to the other. */
		for (int c = 0; c < SETTINGS_IPOE; c++) {
			uint32_t ipc;
			unsigned long lenc;

			if (!ipoe_on(s, c) ||
			    !cidr(s->ival[c][SET_I_ADDRESS], &ipc, &lenc))
				continue;
			m = mask_of(len < lenc ? len : lenc);
			if ((ip & m) == (ipc & m)) {
				snprintf(err, errcap,
					 "network %d and connection %d share addresses",
					 a + 1, c + 1);
				return false;
			}
		}
		m = mask_of(len);
		if (*la[SET_L_POOL_FROM] || *la[SET_L_POOL_TO]) {
			if (!ipv4(la[SET_L_POOL_FROM], &from) ||
			    !ipv4(la[SET_L_POOL_TO], &to) || from > to ||
			    (from & m) != (ip & m) || (to & m) != (ip & m) ||
			    (from & ~m) == 0 || (to & ~m) == ~m ||
			    (ip >= from && ip <= to)) {
				snprintf(err, errcap,
					 "network %d: the pool is not a range of its addresses without the stick's",
					 a + 1);
				return false;
			}
		}
		for (int b = a + 1; b < SETTINGS_LANS; b++) {
			uint32_t ipb, mb;
			unsigned long lenb;

			if (!lan_on(s, b) ||
			    !cidr(s->lval[b][SET_L_ADDRESS], &ipb, &lenb))
				continue;
			if (!strcmp(vlan, s->lval[b][SET_L_VLAN])) {
				snprintf(err, errcap,
					 "networks %d and %d are in one VLAN",
					 a + 1, b + 1);
				return false;
			}
			mb = mask_of(len < lenb ? len : lenb);
			if ((ip & mb) == (ipb & mb)) {
				snprintf(err, errcap,
					 "networks %d and %d share addresses",
					 a + 1, b + 1);
				return false;
			}
		}
	}
	return true;
}

/*
 * An IPoE connection runs in a VLAN of the PON, one at most in a VLAN, which
 * is no management VLAN; a static one has an address and a gateway of each
 * family it carries, the IPv4 gateway another address of the address's
 * network. Sessions with an account and these connections are seven at most.
 */
static bool ipoe_coherent(const struct settings *s, char *err, size_t errcap)
{
	int conns = 0;

	for (int n = 0; n < SETTINGS_SESSIONS; n++)
		conns += *s->acct[n].user != '\0';
	for (int n = 0; n < SETTINGS_IPOE; n++)
		conns += ipoe_on(s, n);
	if (conns > SETTINGS_CONNS) {
		snprintf(err, errcap,
			 "%d connections: sessions and the others together are %d at most",
			 conns, SETTINGS_CONNS);
		return false;
	}
	for (int a = 0; a < SETTINGS_IPOE; a++) {
		const char(*ia)[SETTINGS_VALUE_MAX] = s->ival[a];
		uint32_t ip, gw, m;
		unsigned long len;

		if (!ipoe_on(s, a))
			continue;
		if (!*ia[SET_I_VLAN]) {
			snprintf(err, errcap,
				 "connection %d: it needs a VLAN, IPOE%d_VLAN", a + 1,
				 a + 1);
			return false;
		}
		if (mgmt(s, ia[SET_I_VLAN])) {
			snprintf(err, errcap,
				 "connection %d: VLAN %s is the management VLAN",
				 a + 1, ia[SET_I_VLAN]);
			return false;
		}
		for (int b = a + 1; b < SETTINGS_IPOE; b++) {
			if (ipoe_on(s, b) &&
			    !strcmp(ia[SET_I_VLAN], s->ival[b][SET_I_VLAN])) {
				snprintf(err, errcap,
					 "connections %d and %d are in one VLAN",
					 a + 1, b + 1);
				return false;
			}
		}
		if (strcmp(ia[SET_I_KIND], "static"))
			continue;
		if (ipoe_carries(s, a, "ipv4")) {
			if (!cidr(ia[SET_I_ADDRESS], &ip, &len) ||
			    !ipv4(ia[SET_I_GATEWAY], &gw)) {
				snprintf(err, errcap,
					 "connection %d: a static one needs IPOE%d_ADDRESS and IPOE%d_GATEWAY",
					 a + 1, a + 1, a + 1);
				return false;
			}
			m = mask_of(len);
			if ((gw & m) != (ip & m) || gw == ip || (gw & ~m) == 0 ||
			    (gw & ~m) == ~m) {
				snprintf(err, errcap,
					 "connection %d: the gateway is no other address of its network",
					 a + 1);
				return false;
			}
		}
		if (ipoe_carries(s, a, "ipv6") &&
		    (!*ia[SET_I_ADDRESS6] || !*ia[SET_I_GATEWAY6])) {
			snprintf(err, errcap,
				 "connection %d: a static one with IPv6 needs IPOE%d_ADDRESS6 and IPOE%d_GATEWAY6",
				 a + 1, a + 1, a + 1);
			return false;
		}
	}

	/* A connection in a session's VLAN shares its interface and MAC address. */
	for (int a = 0; a < SETTINGS_IPOE; a++) {
		for (int b = 0; b < SETTINGS_SESSIONS; b++) {
			const char *ma = s->ival[a][SET_I_MAC];
			const char *mb = s->sval[b][SET_S_MAC];

			if (!ipoe_on(s, a) || !*s->acct[b].user ||
			    strcmp(s->ival[a][SET_I_GEM], s->sval[b][SET_S_GEM]) ||
			    strcmp(s->ival[a][SET_I_VLAN], s->sval[b][SET_S_VLAN]))
				continue;
			if (*ma && *mb && strcasecmp(ma, mb)) {
				snprintf(err, errcap,
					 "connection %d and session %d are in one VLAN and send from one MAC address",
					 a + 1, b + 1);
				return false;
			}
		}
	}
	return true;
}

/*
 * A fixed address is in a network that is set up, one of its addresses but
 * the stick's; a device has one in a network, and no address is two
 * devices'.
 */
static bool fixed_coherent(const struct settings *s, char *err, size_t errcap)
{
	for (int i = 0; i < SETTINGS_FIXED; i++) {
		char ma[18], mb[18];
		uint32_t ip, ipb, net, m;
		unsigned long len;
		int lan, lanb;

		if (!*s->fixed[i].spec)
			continue;
		(void)fixed_spec(s->fixed[i].spec, &lan, ma, &ip);
		if (!lan_on(s, lan) ||
		    !cidr(s->lval[lan][SET_L_ADDRESS], &net, &len)) {
			snprintf(err, errcap,
				 "fixed address %d: network %d is not set up",
				 i + 1, lan + 1);
			return false;
		}
		m = mask_of(len);
		if ((ip & m) != (net & m) || (ip & ~m) == 0 ||
		    (ip & ~m) == ~m || ip == net) {
			snprintf(err, errcap,
				 "fixed address %d: not an address of network %d for a device",
				 i + 1, lan + 1);
			return false;
		}
		for (int j = i + 1; j < SETTINGS_FIXED; j++) {
			if (!*s->fixed[j].spec)
				continue;
			(void)fixed_spec(s->fixed[j].spec, &lanb, mb, &ipb);
			if (lan == lanb && !strcasecmp(ma, mb)) {
				snprintf(err, errcap,
					 "fixed addresses %d and %d are of one device in network %d",
					 i + 1, j + 1, lan + 1);
				return false;
			}
			if (ip == ipb) {
				snprintf(err, errcap,
					 "fixed addresses %d and %d give one address",
					 i + 1, j + 1);
				return false;
			}
		}
	}
	return true;
}

bool settings_coherent(const struct settings *s, char *err, size_t errcap)
{
	for (int n = 0; n < SETTINGS_SESSIONS; n++) {
		if (!*s->acct[n].user != !*s->acct[n].secret) {
			snprintf(err, errcap,
				 "session %d: an account needs PPPOE%d_USER and PPPOE%d_SECRET",
				 n + 1, n + 1, n + 1);
			return false;
		}
	}
	/*
	 * The switch drops a management VLAN's frames on the PON's ingress and
	 * keeps them from the PON on the host link's: a session or a link in
	 * it would carry nothing.
	 */
	for (int n = 0; n < SETTINGS_SESSIONS; n++) {
		if (!*s->acct[n].user)
			continue;
		if (mgmt(s, s->sval[n][SET_S_VLAN]) || mgmt(s, down_vlan(s, n))) {
			snprintf(err, errcap,
				 "session %d: VLAN %s is the management VLAN", n + 1,
				 mgmt(s, s->sval[n][SET_S_VLAN]) ? s->sval[n][SET_S_VLAN] :
								  down_vlan(s, n));
			return false;
		}
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
	return ipoe_coherent(s, err, errcap) && lans_coherent(s, err, errcap) &&
	       fixed_coherent(s, err, errcap);
}

size_t settings_format_defaults(char *out, size_t cap)
{
	size_t len = 0;

	if (cap)
		out[0] = '\0';
	for (int k = 0; k < SETTINGS_NKEYS; k++)
		len += (size_t)snprintf(out + (len < cap ? len : cap),
					len < cap ? cap - len : 0, "%s=%s\n",
					g_keys[k].name, g_keys[k].dflt);
	for (int n = 0; n < SETTINGS_SESSIONS; n++)
		for (int k = 0; k < SETTINGS_NSKEYS; k++)
			len += (size_t)snprintf(out + (len < cap ? len : cap),
						len < cap ? cap - len : 0,
						"SESSION%d_%s=%s\n", n + 1,
						g_skeys[k].suffix, g_skeys[k].dflt);
	return len;
}

size_t settings_format(const struct settings *s, char *out, size_t cap)
{
	size_t len = 0;

	/* Without a session, a connection or a network there is nothing to write. */
	if (cap)
		out[0] = '\0';

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

	/* A connection's settings are kept while it has a kind. */
	for (int n = 0; n < SETTINGS_IPOE; n++) {
		if (!ipoe_on(s, n))
			continue;
		for (int k = 0; k < SETTINGS_NIKEYS; k++)
			len += (size_t)snprintf(out + (len < cap ? len : cap),
						len < cap ? cap - len : 0,
						"IPOE%d_%s=%s\n", n + 1,
						g_ikeys[k].suffix, s->ival[n][k]);
	}

	/* A network's settings are kept while it has a connection. */
	for (int n = 0; n < SETTINGS_LANS; n++) {
		if (!lan_on(s, n))
			continue;
		for (int k = 0; k < SETTINGS_NLKEYS; k++)
			len += (size_t)snprintf(out + (len < cap ? len : cap),
						len < cap ? cap - len : 0,
						"LAN%d_%s=%s\n", n + 1,
						g_lkeys[k].suffix, s->lval[n][k]);
	}
	for (int i = 0; i < SETTINGS_FIXED; i++) {
		const struct settings_fixed *f = &s->fixed[i];

		if (!*f->spec)
			continue;
		len += (size_t)snprintf(out + (len < cap ? len : cap),
					len < cap ? cap - len : 0,
					"FIXED%d=%s\n", i + 1, f->spec);
		if (*f->name)
			len += (size_t)snprintf(out + (len < cap ? len : cap),
						len < cap ? cap - len : 0,
						"FIXED%d_NAME=%s\n", i + 1, f->name);
	}
	return len;
}
