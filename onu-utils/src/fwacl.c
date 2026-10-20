// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The firewall's rules as the switch's ACL (fwacl.h). */

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fwacl.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define IPPROTO_ICMP_ 1
#define IPPROTO_TCP_ 6
#define IPPROTO_UDP_ 17
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8

void acl_spec_init(struct acl_spec *s)
{
	memset(s, 0, sizeof(*s));
}

static uint32_t len_mask(unsigned int len)
{
	return len ? 0xffffffffu << (32 - len) : 0;
}

/* a.b.c.d: true with the address in @a, host order. */
static bool addr_of(const char *v, uint32_t *a)
{
	struct in_addr in;

	if (inet_pton(AF_INET, v, &in) != 1)
		return false;
	*a = ntohl(in.s_addr);
	return true;
}

/* a.b.c.d[/len]: the network, its bits past the length cleared. */
static bool net_of(const char *v, uint32_t *net, uint32_t *mask)
{
	char a[16];
	const char *slash = strchr(v, '/');
	size_t n = slash ? (size_t)(slash - v) : strlen(v);
	unsigned long len = 32;
	char *end;

	if (!n || n >= sizeof(a))
		return false;
	memcpy(a, v, n);
	a[n] = '\0';
	if (slash) {
		len = strtoul(slash + 1, &end, 10);
		if (end == slash + 1 || *end || len > 32)
			return false;
	}
	if (!addr_of(a, net))
		return false;
	*mask = len_mask((unsigned int)len);
	*net &= *mask;
	return true;
}

static bool mac_of(const char *v, uint8_t mac[6])
{
	unsigned int b[6];
	char x;

	if (strlen(v) != 17 ||
	    sscanf(v, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5],
		   &x) != 6)
		return false;
	for (int i = 0; i < 6; i++)
		mac[i] = (uint8_t)b[i];
	return true;
}

/* A port, 1 to 65535, or the first and the last of a range apart by '-'. */
static bool ports_of(const char *v, uint16_t *first, uint16_t *last)
{
	unsigned long a, b;
	char *end;

	a = strtoul(v, &end, 10);
	if (end == v || a < 1 || a > 65535)
		return false;
	b = a;
	if (*end == '-') {
		const char *p = end + 1;

		b = strtoul(p, &end, 10);
		if (end == p || b < a || b > 65535)
			return false;
	}
	if (*end)
		return false;
	*first = (uint16_t)a;
	*last = (uint16_t)b;
	return true;
}

/* internet, internet:ADDR[/LEN], lans, lan:L or dev:L:MAC. */
static bool end_of(const char *v, struct acl_end *e)
{
	memset(e, 0, sizeof(*e));
	if (!strcmp(v, "internet")) {
		e->kind = ACL_END_INTERNET;
		return true;
	}
	if (!strncmp(v, "internet:", 9)) {
		e->kind = ACL_END_INTERNET;
		if (net_of(v + 9, &e->net, &e->mask))
			return true;
		/* An address of IPv6, which the ACL does not compare. */
		e->v6 = strchr(v + 9, ':') != NULL;
		return e->v6;
	}
	if (!strcmp(v, "lans")) {
		e->kind = ACL_END_LANS;
		return true;
	}
	if ((strncmp(v, "lan:", 4) && strncmp(v, "dev:", 4)) || v[4] < '1' ||
	    v[4] > '0' + ACL_LANS)
		return false;
	e->lan = v[4] - '1';
	if (v[0] == 'l') {
		e->kind = ACL_END_LAN;
		return !v[5];
	}
	e->kind = ACL_END_DEV;
	return v[5] == ':' && mac_of(v + 6, e->mac);
}

/* ID_ACTION_FROM_TO_PROTO_PORTS_FAMILY_ON, as RULEn has it. */
static bool rule_of(const char *v, struct acl_rule *r)
{
	char f[8][64];
	unsigned long id;
	char *end;

	memset(r, 0, sizeof(*r));
	for (int i = 0; i < 8; i++) {
		size_t len = strcspn(v, "_");

		if (len >= sizeof(f[i]) || (i < 7) != (v[len] == '_'))
			return false;
		memcpy(f[i], v, len);
		f[i][len] = '\0';
		v += len + (i < 7);
	}
	id = strtoul(f[0], &end, 10);
	if (end == f[0] || *end || id < 1 || id > ACL_RULES)
		return false;
	r->id = (unsigned int)id;
	if (strcmp(f[1], "allow") && strcmp(f[1], "block"))
		return false;
	r->allow = !strcmp(f[1], "allow");
	if (!end_of(f[2], &r->from) || !end_of(f[3], &r->to))
		return false;
	if (!strcmp(f[4], "tcp"))
		r->protos = ACL_TCP;
	else if (!strcmp(f[4], "udp"))
		r->protos = ACL_UDP;
	else if (!strcmp(f[4], "both"))
		r->protos = ACL_TCP | ACL_UDP;
	else if (!strcmp(f[4], "icmp"))
		r->protos = ACL_ICMP;
	else if (!strcmp(f[4], "any"))
		r->protos = ACL_ANY;
	else
		return false;
	if (*f[5]) {
		if (!(r->protos & (ACL_TCP | ACL_UDP)) || !ports_of(f[5], &r->first, &r->last))
			return false;
		r->ports = true;
	}
	if (!strcmp(f[6], "ipv4"))
		r->family = 4;
	else if (!strcmp(f[6], "ipv6"))
		r->family = 6;
	else if (strcmp(f[6], "any"))
		return false;
	if (strcmp(f[7], "on") && strcmp(f[7], "off"))
		return false;
	r->on = !strcmp(f[7], "on");
	return true;
}

static int wan_by_ifname(const struct acl_spec *s, const char *ifname, uint32_t addr)
{
	for (int i = 0; i < s->nwans; i++)
		if (!strcmp(s->wan[i].ifname, ifname) && s->wan[i].addr == addr)
			return i;
	return -1;
}

static int bad(char *err, size_t errcap, const char *line)
{
	snprintf(err, errcap, "%.48s: no line of the ACL's", line);
	return -1;
}

int acl_spec_line(struct acl_spec *s, const char *line, char *err, size_t errcap)
{
	char word[8], a[80], b[64], c[64], d[64], e[64], f[64], x[2];
	uint32_t addr, mask;
	unsigned long n;
	char *end;
	int k;

	if (!*line || *line == '#')
		return 0;
	k = sscanf(line, "%7s %79s %63s %63s %63s %63s %63s %1s", word, a, b, c, d, e, f, x);
	if (k == 2 && !strcmp(word, "rule")) {
		if (s->nrules == ACL_RULES || !rule_of(a, &s->rule[s->nrules]))
			return bad(err, errcap, line);
		s->nrules++;
		return 0;
	}
	if ((k == 3 || k == 4) && !strcmp(word, "lan")) {
		n = strtoul(a, &end, 10);
		if (end == a || *end || n < 1 || n > ACL_LANS || !net_of(b, &addr, &mask) ||
		    (k == 4 && strlen(c) >= sizeof(s->lan_conn[0])))
			return bad(err, errcap, line);
		s->lan_on[n - 1] = true;
		s->lan_net[n - 1] = addr;
		s->lan_mask[n - 1] = mask;
		snprintf(s->lan_conn[n - 1], sizeof(s->lan_conn[0]), "%s", k == 4 ? c : "");
		return 0;
	}
	if (k == 4 && !strcmp(word, "wan")) {
		struct acl_wan *w = &s->wan[s->nwans];

		if (s->nwans == ACL_WANS || strlen(a) >= sizeof(w->conn) ||
		    strlen(b) >= sizeof(w->ifname) || !addr_of(c, &w->addr))
			return bad(err, errcap, line);
		memcpy(w->conn, a, strlen(a) + 1);
		memcpy(w->ifname, b, strlen(b) + 1);
		w->pppoe = !strncmp(a, "pppoe:", 6);
		s->nwans++;
		return 0;
	}
	if (k == 4 && !strcmp(word, "dev")) {
		int i = s->ndevs;

		n = strtoul(a, &end, 10);
		if (i == ACL_DEVS || end == a || *end || n < 1 || n > ACL_LANS ||
		    !mac_of(b, s->dev_mac[i]) || !addr_of(c, &s->dev_ip[i]))
			return bad(err, errcap, line);
		s->dev_lan[i] = (int)n - 1;
		s->ndevs++;
		return 0;
	}
	if (k == 7 && !strcmp(word, "fwd")) {
		int i = s->nfwds;
		uint16_t first, last;
		unsigned long port;

		port = strtoul(f, &end, 10);
		if (i == ACL_FWDS || !addr_of(b, &addr) || (strcmp(c, "tcp") && strcmp(c, "udp")) ||
		    !ports_of(d, &first, &last) || !addr_of(e, &s->fwd_dev[i]) || end == f || *end ||
		    port < 1 || port + (last - first) > 65535)
			return bad(err, errcap, line);
		s->fwd_wan[i] = wan_by_ifname(s, a, addr);
		s->fwd_proto[i] = !strcmp(c, "tcp") ? IPPROTO_TCP_ : IPPROTO_UDP_;
		s->fwd_from[i] = first;
		s->fwd_last[i] = last;
		s->fwd_port[i] = (uint16_t)port;
		s->nfwds++;
		return 0;
	}
	if (k == 4 && !strcmp(word, "dmz")) {
		int i = s->ndmzs;

		if (i == ACL_DMZS || !addr_of(b, &addr) || !addr_of(c, &s->dmz_dev[i]))
			return bad(err, errcap, line);
		s->dmz_wan[i] = wan_by_ifname(s, a, addr);
		s->ndmzs++;
		return 0;
	}
	return bad(err, errcap, line);
}

/* The entries of the rule being compiled. */
struct ctx {
	const struct acl_spec *s;
	struct acl_entry *e;
	int n;
	bool over;
	bool internet;
};

static void add(struct ctx *c, const struct acl_entry *t)
{
	if (c->n == ACL_RULE_ENTRIES) {
		c->over = true;
		return;
	}
	c->e[c->n++] = *t;
	if (t->port == ACL_SFP && t->phase == ACL_INTERNET)
		c->internet = true;
}

/*
 * @t of each protocol of @protos, and of each masked prefix of the ports
 * @lo to @hi, as its source ports when @src, else its destination ports; of
 * every port without @ports.
 */
static void put_l4(struct ctx *c, struct acl_entry t, unsigned int protos, bool ports,
		   uint16_t lo, uint16_t hi, bool src)
{
	static const struct {
		unsigned int bit;
		uint8_t proto;
	} p[] = {
		{ ACL_TCP, IPPROTO_TCP_ },
		{ ACL_UDP, IPPROTO_UDP_ },
		{ ACL_ICMP, IPPROTO_ICMP_ },
		{ ACL_ANY, 0 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(p); i++) {
		uint32_t a = lo;

		if (!(protos & p[i].bit))
			continue;
		t.proto = p[i].proto;
		if (!ports || !(p[i].bit & (ACL_TCP | ACL_UDP))) {
			add(c, &t);
			continue;
		}
		/* The largest block aligned at a that ends no later than hi, each time. */
		while (a <= hi) {
			uint32_t size = a ? a & -a : 0x10000;

			while (a + size - 1 > hi)
				size >>= 1;
			if (src) {
				t.sport = (uint16_t)a;
				t.spmask = (uint16_t)~(size - 1);
			} else {
				t.dport = (uint16_t)a;
				t.dpmask = (uint16_t)~(size - 1);
			}
			add(c, &t);
			a += size;
		}
	}
}

/* How an end of a rule is seen on sfp: a network's addresses, a device's address or its MAC. */
struct side {
	int lan;
	bool mac;
	uint8_t m[6];
	uint32_t net, mask;
};

static int fixed_of(const struct acl_spec *s, int lan, const uint8_t mac[6])
{
	for (int i = 0; i < s->ndevs; i++)
		if (s->dev_lan[i] == lan && !memcmp(s->dev_mac[i], mac, 6))
			return i;
	return -1;
}

/*
 * The ways @e is seen on sfp, in @out: every network, one, or a device, by
 * its MAC address when @by_mac, by its fixed address else, and by its
 * network when it has none and @net_else.
 */
static int sides_of(const struct acl_spec *s, const struct acl_end *e, bool by_mac, bool net_else,
		    struct side *out)
{
	int n = 0, d;

	switch (e->kind) {
	case ACL_END_LANS:
		for (int i = 0; i < ACL_LANS; i++)
			if (s->lan_on[i])
				out[n++] = (struct side){ .lan = i, .net = s->lan_net[i],
							  .mask = s->lan_mask[i] };
		break;
	case ACL_END_LAN:
		if (s->lan_on[e->lan])
			out[n++] = (struct side){ .lan = e->lan, .net = s->lan_net[e->lan],
						  .mask = s->lan_mask[e->lan] };
		break;
	case ACL_END_DEV:
		if (!s->lan_on[e->lan])
			break;
		out[n] = (struct side){ .lan = e->lan };
		if (by_mac) {
			out[n].mac = true;
			memcpy(out[n++].m, e->mac, 6);
			break;
		}
		d = fixed_of(s, e->lan, e->mac);
		if (d >= 0) {
			out[n].net = s->dev_ip[d];
			out[n++].mask = 0xffffffffu;
		} else if (net_else) {
			out[n].net = s->lan_net[e->lan];
			out[n++].mask = s->lan_mask[e->lan];
		}
		break;
	}
	return n;
}

static void from_side(struct acl_entry *t, const struct side *x)
{
	t->smac = x->mac;
	memcpy(t->mac, x->m, 6);
	t->sip = x->net;
	t->smask = x->mask;
}

static void to_side(struct acl_entry *t, const struct side *x)
{
	t->dip = x->net;
	t->dmask = x->mask;
}

/* Whether the device of address @ip lies in @e. */
static bool reaches(const struct acl_spec *s, const struct acl_end *e, uint32_t ip)
{
	int d;

	switch (e->kind) {
	case ACL_END_LANS:
		for (int i = 0; i < ACL_LANS; i++)
			if (s->lan_on[i] && (ip & s->lan_mask[i]) == s->lan_net[i])
				return true;
		return false;
	case ACL_END_LAN:
		return s->lan_on[e->lan] && (ip & s->lan_mask[e->lan]) == s->lan_net[e->lan];
	case ACL_END_DEV:
		d = fixed_of(s, e->lan, e->mac);
		return d >= 0 && s->dev_ip[d] == ip;
	}
	return false;
}

/* Between the networks: what enters sfp from one network to another. */
static void between(struct ctx *c, const struct acl_rule *r, struct acl_entry t)
{
	struct side a[ACL_LANS], b[ACL_LANS];
	int na = sides_of(c->s, &r->from, true, false, a);
	int nb = sides_of(c->s, &r->to, false, false, b);
	int first = c->n;

	t.port = ACL_SFP;
	t.phase = ACL_LOCAL;
	for (int i = 0; i < na; i++)
		for (int j = 0; j < nb; j++) {
			if (a[i].lan == b[j].lan)
				continue;
			from_side(&t, &a[i]);
			to_side(&t, &b[j]);
			put_l4(c, t, r->protos, r->ports, r->first, r->last, false);
		}
	if (!r->allow || c->n == first)
		return;
	/* The answers: from B by its addresses to A by its own, or by its network. */
	na = sides_of(c->s, &r->to, false, false, a);
	nb = sides_of(c->s, &r->from, false, true, b);
	for (int i = 0; i < na; i++)
		for (int j = 0; j < nb; j++) {
			if (a[i].lan == b[j].lan)
				continue;
			from_side(&t, &a[i]);
			to_side(&t, &b[j]);
			put_l4(c, t, r->protos, r->ports, r->first, r->last, true);
		}
}

/* Whether connection @conn is one a network of @e goes out by. */
static bool goes_out_by(const struct acl_spec *s, const struct acl_end *e, const char *conn)
{
	for (int i = 0; i < ACL_LANS; i++) {
		bool of = e->kind == ACL_END_LANS || e->lan == i;

		if (of && s->lan_on[i] && !strcmp(s->lan_conn[i], conn))
			return true;
	}
	return false;
}

/* The connection's address, by IPoE or inside its PPPoE session, on pon. */
static void on_wan(struct acl_entry *t, const struct acl_wan *w)
{
	t->port = ACL_PON;
	t->phase = ACL_LOCAL;
	t->pppoe = w->pppoe;
	t->dip = w->addr;
	t->dmask = 0xffffffffu;
}

/* To the internet: what enters sfp from a network to no local address. */
static void out(struct ctx *c, const struct acl_rule *r, struct acl_entry t)
{
	struct side a[ACL_LANS];
	int na = sides_of(c->s, &r->from, true, false, a);
	int first = c->n;

	t.port = ACL_SFP;
	t.phase = ACL_INTERNET;
	t.dip = r->to.net;
	t.dmask = r->to.mask;
	for (int i = 0; i < na; i++) {
		from_side(&t, &a[i]);
		put_l4(c, t, r->protos, r->ports, r->first, r->last, false);
	}
	/*
	 * The answers, which come in by each connection the networks go out
	 * by, to its address, when the rule names ports.
	 */
	if (!r->allow || !r->ports || c->n == first)
		return;
	for (int w = 0; w < c->s->nwans; w++) {
		struct acl_entry u = t;

		if (!goes_out_by(c->s, &r->from, c->s->wan[w].conn))
			continue;
		u.smac = false;
		u.sip = r->to.net;
		u.smask = r->to.mask;
		on_wan(&u, &c->s->wan[w]);
		put_l4(c, u, r->protos & (ACL_TCP | ACL_UDP), true, r->first, r->last, true);
	}
}

static unsigned int proto_bit(uint8_t proto)
{
	return proto == IPPROTO_TCP_ ? ACL_TCP : ACL_UDP;
}

/* The answers of a device of address @dev, from the ports @lo to @hi, to the internet. */
static void answers(struct ctx *c, const struct acl_rule *r, struct acl_entry t, uint32_t dev,
		    unsigned int protos, uint16_t lo, uint16_t hi)
{
	t.port = ACL_SFP;
	t.phase = ACL_INTERNET;
	t.pppoe = false;
	t.pass = true;
	t.sip = dev;
	t.smask = 0xffffffffu;
	t.dip = r->from.net;
	t.dmask = r->from.mask;
	put_l4(c, t, protos, true, lo, hi, true);
}

/* A range of ports, inclusive. */
struct range {
	uint32_t lo, hi;
};

static int range_cmp(const void *a, const void *b)
{
	const struct range *x = a, *y = b;

	return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/*
 * The ports @lo to @hi that no forward of connection @w of protocol @proto
 * takes, as ranges in @out: as many as there are, ACL_FWDS + 1 at most.
 */
static int not_forwarded(const struct acl_spec *s, int w, uint8_t proto, uint32_t lo, uint32_t hi,
			 struct range *out)
{
	struct range taken[ACL_FWDS];
	int nt = 0, n = 0;
	uint32_t at = lo;

	for (int i = 0; i < s->nfwds; i++)
		if (s->fwd_wan[i] == w && s->fwd_proto[i] == proto)
			taken[nt++] = (struct range){ s->fwd_from[i], s->fwd_last[i] };
	qsort(taken, (size_t)nt, sizeof(taken[0]), range_cmp);
	for (int i = 0; i < nt && at <= hi; i++) {
		if (taken[i].hi < at)
			continue;
		if (taken[i].lo > hi)
			break;
		if (taken[i].lo > at)
			out[n++] = (struct range){ at, taken[i].lo - 1 };
		at = taken[i].hi + 1;
	}
	if (at <= hi)
		out[n++] = (struct range){ at, hi };
	return n;
}

/*
 * From the internet: what enters pon to a connection's address, through
 * the forwards and the DMZs that reach the rule's destination. Returns
 * whether a DMZ's part names every port, and is left out.
 */
static bool in(struct ctx *c, const struct acl_rule *r, struct acl_entry t)
{
	const struct acl_spec *s = c->s;
	unsigned int tcpudp = r->protos & ACL_ANY ? ACL_TCP | ACL_UDP :
						     r->protos & (ACL_TCP | ACL_UDP);
	bool left = false;

	t.sip = r->from.net;
	t.smask = r->from.mask;
	for (int i = 0; i < s->nfwds; i++) {
		uint32_t ilo = s->fwd_port[i], ihi = ilo + (s->fwd_last[i] - s->fwd_from[i]);
		int w = s->fwd_wan[i];
		struct acl_entry u = t;

		if (w < 0 || !(tcpudp & proto_bit(s->fwd_proto[i])) ||
		    !reaches(s, &r->to, s->fwd_dev[i]))
			continue;
		/* The rule's ports are the device's: those the forward takes, moved. */
		if (r->ports) {
			ilo = ilo > r->first ? ilo : r->first;
			ihi = ihi < r->last ? ihi : r->last;
			if (ilo > ihi)
				continue;
		}
		on_wan(&u, &s->wan[w]);
		put_l4(c, u, proto_bit(s->fwd_proto[i]), true,
		       (uint16_t)(s->fwd_from[i] + (ilo - s->fwd_port[i])),
		       (uint16_t)(s->fwd_from[i] + (ihi - s->fwd_port[i])), false);
		if (r->allow)
			answers(c, r, t, s->fwd_dev[i], proto_bit(s->fwd_proto[i]),
				(uint16_t)ilo, (uint16_t)ihi);
	}
	for (int i = 0; i < s->ndmzs; i++) {
		int w = s->dmz_wan[i];
		struct acl_entry u = t;

		if (w < 0 || !reaches(s, &r->to, s->dmz_dev[i]))
			continue;
		on_wan(&u, &s->wan[w]);
		if (r->protos & (ACL_ICMP | ACL_ANY)) {
			struct acl_entry a = t;

			u.proto = IPPROTO_ICMP_;
			u.icmp_type = ICMP_ECHO_REQUEST;
			add(c, &u);
			if (r->allow) {
				a.port = ACL_SFP;
				a.phase = ACL_INTERNET;
				a.pass = true;
				a.sip = s->dmz_dev[i];
				a.smask = 0xffffffffu;
				a.dip = r->from.net;
				a.dmask = r->from.mask;
				a.proto = IPPROTO_ICMP_;
				a.icmp_type = ICMP_ECHO_REPLY;
				add(c, &a);
			}
		}
		if (!tcpudp)
			continue;
		if (!r->ports) {
			left = true;
			continue;
		}
		for (unsigned int bit = ACL_TCP; bit <= ACL_UDP; bit <<= 1) {
			uint8_t proto = bit == ACL_TCP ? IPPROTO_TCP_ : IPPROTO_UDP_;
			struct range free[ACL_FWDS + 1];
			int nf;

			if (!(tcpudp & bit))
				continue;
			nf = not_forwarded(s, w, proto, r->first, r->last, free);
			for (int f = 0; f < nf; f++) {
				put_l4(c, u, bit, true, (uint16_t)free[f].lo, (uint16_t)free[f].hi,
				       false);
				if (r->allow)
					answers(c, r, t, s->dmz_dev[i], bit, (uint16_t)free[f].lo,
						(uint16_t)free[f].hi);
			}
		}
	}
	return left;
}

static void rule_entries(struct ctx *c, const struct acl_rule *r, int ri, struct acl_status *st)
{
	struct acl_entry t = { .rule = (int16_t)ri, .pass = r->allow, .icmp_type = -1 };
	bool left = false;

	st->id = r->id;
	st->state = ACL_OUT;
	if (!r->on) {
		st->why = "off";
		return;
	}
	if (r->family == 6 || r->from.v6 || r->to.v6) {
		st->why = "ipv6";
		return;
	}
	if (r->from.kind == ACL_END_INTERNET)
		left = in(c, r, t);
	else if (r->to.kind == ACL_END_INTERNET)
		out(c, r, t);
	else
		between(c, r, t);
	if (c->over) {
		st->why = "room";
		return;
	}
	if (!c->n) {
		st->why = left ? "dmz" : "nothing";
		return;
	}
	st->state = left ? ACL_PART : ACL_IN;
	st->why = left ? "dmz" : NULL;
	st->entries = c->n;
	st->internet = c->internet;
}

/* The passes of the local destinations, ahead of the rules to the internet on sfp. */
static int local_passes(const struct acl_spec *s, struct acl_entry *out)
{
	struct acl_entry t = { .port = ACL_SFP, .phase = ACL_PASSES, .rule = -1, .pass = true,
			       .icmp_type = -1 };
	int n = 0;

	for (int i = 0; i < ACL_LANS; i++) {
		if (!s->lan_on[i])
			continue;
		t.dip = s->lan_net[i];
		t.dmask = s->lan_mask[i];
		out[n++] = t;
	}
	t.dip = 0xffffffffu;
	t.dmask = 0xffffffffu;
	out[n++] = t;
	t.dip = 0xe0000000u;
	t.dmask = 0xf0000000u;
	t.proto = IPPROTO_UDP_;
	out[n++] = t;
	return n;
}

void acl_compile(const struct acl_spec *s, int room, struct acl_set *out)
{
	static struct acl_entry scratch[ACL_RULE_ENTRIES];
	struct acl_entry passes[ACL_LANS + 2];
	int np = local_passes(s, passes);
	bool passed = false, full = false;
	uint16_t pref[2] = { ACL_PREF, ACL_PREF };

	memset(out, 0, sizeof(*out));
	if (room > ACL_SET_MAX)
		room = ACL_SET_MAX;
	for (int i = 0; i < s->nrules; i++) {
		struct acl_status *st = &out->st[i];
		struct ctx c = { .s = s, .e = scratch };
		int need;

		out->nst++;
		if (full) {
			st->id = s->rule[i].id;
			st->state = ACL_OUT;
			st->why = "room";
			continue;
		}
		rule_entries(&c, &s->rule[i], i, st);
		if (st->state == ACL_OUT) {
			full = c.over;
			continue;
		}
		need = c.n + (st->internet && !passed ? np : 0);
		if (out->n + need > room) {
			/* This rule and every one after it, which may count on it. */
			st->state = ACL_OUT;
			st->why = "room";
			st->entries = 0;
			full = true;
			continue;
		}
		if (st->internet && !passed) {
			memcpy(&out->e[out->n], passes, (size_t)np * sizeof(passes[0]));
			out->n += np;
			out->passes = np;
			passed = true;
		}
		memcpy(&out->e[out->n], scratch, (size_t)c.n * sizeof(scratch[0]));
		out->n += c.n;
	}
	/*
	 * In the order the switch judges them: by port, then by place among
	 * the port's, then as they came, the rules' in their order. A stable
	 * insertion sort, of ACL_ROOM entries at most.
	 */
	for (int i = 1; i < out->n; i++) {
		struct acl_entry x = out->e[i];
		int j = i;

		while (j > 0 && (out->e[j - 1].port > x.port ||
				 (out->e[j - 1].port == x.port && out->e[j - 1].phase > x.phase))) {
			out->e[j] = out->e[j - 1];
			j--;
		}
		out->e[j] = x;
	}
	for (int i = 0; i < out->n; i++)
		out->e[i].pref = pref[out->e[i].port]++;
}

static int put(char *buf, size_t cap, int len, const char *fmt, ...)
	__attribute((format(printf, 4, 5)));

static int put(char *buf, size_t cap, int len, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf + ((size_t)len < cap ? (size_t)len : cap),
		      (size_t)len < cap ? cap - (size_t)len : 0, fmt, ap);
	va_end(ap);
	return len + n;
}

static int mask_len(uint32_t mask)
{
	int n = 0;

	while (mask & 0x80000000u) {
		n++;
		mask <<= 1;
	}
	return n;
}

static int put_net(char *buf, size_t cap, int len, const char *key, uint32_t a, uint32_t mask)
{
	return put(buf, cap, len, " %s %u.%u.%u.%u/%d", key, a >> 24, (a >> 16) & 255,
		   (a >> 8) & 255, a & 255, mask_len(mask));
}

static int put_port(char *buf, size_t cap, int len, const char *key, uint16_t v, uint16_t m)
{
	if (m == 0xffff)
		return put(buf, cap, len, " %s %u", key, v);
	return put(buf, cap, len, " %s %u/0x%04x", key, v, m);
}

int acl_entry_format(const struct acl_entry *e, bool hw, char *buf, size_t cap)
{
	int len = 0;

	len = put(buf, cap, len, "tc filter add dev %s ingress pref %u protocol %s flower%s%s",
		  e->port == ACL_SFP ? "sfp" : "pon", e->pref, e->pppoe ? "ppp_ses" : "ip",
		  hw ? " skip_sw" : "", e->pppoe ? " ppp_proto ip" : "");
	if (e->smac)
		len = put(buf, cap, len, " src_mac %02x:%02x:%02x:%02x:%02x:%02x", e->mac[0],
			  e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
	if (e->smask)
		len = put_net(buf, cap, len, "src_ip", e->sip, e->smask);
	if (e->dmask)
		len = put_net(buf, cap, len, "dst_ip", e->dip, e->dmask);
	if (e->proto)
		len = put(buf, cap, len, " ip_proto %s",
			  e->proto == IPPROTO_TCP_ ? "tcp" : e->proto == IPPROTO_UDP_ ? "udp" : "icmp");
	if (e->spmask)
		len = put_port(buf, cap, len, "src_port", e->sport, e->spmask);
	if (e->dpmask)
		len = put_port(buf, cap, len, "dst_port", e->dport, e->dpmask);
	if (e->icmp_type >= 0)
		len = put(buf, cap, len, " type %d", e->icmp_type);
	return put(buf, cap, len, " action %s", e->pass ? "pass" : "drop");
}

int acl_status_format(const struct acl_status *st, char *buf, size_t cap)
{
	static const char *const state[] = { "in", "part", "out" };

	if (st->state == ACL_IN)
		return snprintf(buf, cap, "%u in %d", st->id, st->entries);
	return snprintf(buf, cap, "%u %s %d %s", st->id, state[st->state], st->entries,
			st->why ? st->why : "");
}
