// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The client of the session's link: it asks for a delegated prefix and the
 * name servers, and keeps the prefix (RFC 8415, sections 18.2 and 18.3). A
 * Solicit offers Rapid Commit, so that a server that takes it answers with
 * the binding at once; otherwise the best Advertise of the first wait is
 * taken with a Request. The binding is renewed with its server at T1, with
 * any server at T2, and lost at the end of its valid lifetime, when the
 * client starts again.
 */

#include <string.h>

#include "dhcp6d.h"

#define FAR_FUTURE (INT64_MAX / 4)

static uint32_t rnd(struct client *c)
{
	uint32_t x = c->rand ? c->rand : 0x9e3779b9u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	c->rand = x;
	return x;
}

/* A random tenth of @t either way, or upward only (RFC 8415, section 15). */
static int64_t jitter(struct client *c, int64_t t, bool upward)
{
	int64_t tenth = t / 10;

	if (!tenth)
		return 0;
	if (upward)
		return (int64_t)(rnd(c) % (uint32_t)(tenth + 1));
	return (int64_t)(rnd(c) % (uint32_t)(2 * tenth + 1)) - tenth;
}

static int64_t max_rt(enum client_state st)
{
	switch (st) {
	case CLIENT_SOLICIT:
		return DHCP6_SOL_MAX_RT;
	case CLIENT_REQUEST:
		return DHCP6_REQ_MAX_RT;
	case CLIENT_RENEW:
		return DHCP6_REN_MAX_RT;
	case CLIENT_REBIND:
		return DHCP6_REB_MAX_RT;
	default:
		return 0;
	}
}

/* Start an exchange in @st, whose first message goes out at once. */
static void begin(struct client *c, enum client_state st, int64_t now,
		  int64_t irt)
{
	c->state = st;
	c->xid = rnd(c) & 0xffffff;
	c->started = now;
	c->next = now;
	c->count = 0;
	c->rt = irt + jitter(c, irt, st == CLIENT_SOLICIT);
	c->best_len = 0;
	c->best_pref = -1;
}

void client_init(struct client *c, const uint8_t *duid, size_t duid_len,
		 uint32_t iaid, uint32_t seed, int64_t now)
{
	memset(c, 0, sizeof(*c));
	if (duid_len > sizeof(c->duid))
		duid_len = sizeof(c->duid);
	memcpy(c->duid, duid, duid_len);
	c->duid_len = duid_len;
	c->iaid = iaid;
	c->rand = seed;
	begin(c, CLIENT_SOLICIT, now, DHCP6_SOL_TIMEOUT);
}

static int64_t until(int64_t now, uint32_t seconds)
{
	return seconds == DHCP6_INFINITY ? FAR_FUTURE : now + (int64_t)seconds * 1000;
}

/* The message of the exchange in progress. */
static size_t build(struct client *c, int64_t now, uint8_t type, uint8_t *out,
		    size_t cap)
{
	static const uint8_t oro[] = { 0, DHCP6_OPT_DNS_SERVERS };
	int64_t elapsed = c->count ? (now - c->started) / 10 : 0;
	uint8_t ia[12];
	struct msg m;

	msg_start(&m, out, cap, type, c->xid);
	msg_opt(&m, DHCP6_OPT_CLIENTID, c->duid, c->duid_len);
	msg_opt16(&m, DHCP6_OPT_ELAPSED_TIME,
		  (uint16_t)(elapsed > 0xffff ? 0xffff : elapsed));
	msg_opt(&m, DHCP6_OPT_ORO, oro, sizeof(oro));
	if (type == DHCP6_SOLICIT)
		msg_opt(&m, DHCP6_OPT_RAPID_COMMIT, NULL, 0);
	if (type == DHCP6_REQUEST || type == DHCP6_RENEW || type == DHCP6_RELEASE)
		msg_opt(&m, DHCP6_OPT_SERVERID, c->server, c->server_len);

	put32(ia, c->iaid);
	put32(ia + 4, 0);
	put32(ia + 8, 0);
	msg_open(&m, DHCP6_OPT_IA_PD);
	msg_raw(&m, ia, sizeof(ia));
	/* Renew, Rebind and Release name the prefix the client holds. */
	if (c->bound && (type == DHCP6_RENEW || type == DHCP6_REBIND ||
			 type == DHCP6_RELEASE)) {
		uint8_t p[25] = { 0 };

		p[8] = c->prefix.len;
		memcpy(p + 9, c->prefix.addr, 16);
		msg_opt(&m, DHCP6_OPT_IAPREFIX, p, sizeof(p));
	}
	msg_close(&m);
	return m.overflow ? 0 : m.len;
}

static uint8_t type_of(enum client_state st)
{
	switch (st) {
	case CLIENT_SOLICIT:
		return DHCP6_SOLICIT;
	case CLIENT_REQUEST:
		return DHCP6_REQUEST;
	case CLIENT_RENEW:
		return DHCP6_RENEW;
	case CLIENT_REBIND:
		return DHCP6_REBIND;
	default:
		return 0;
	}
}

/*
 * The first prefix of the IA_PD of @opts that the client can use, with the
 * times from T1 and T2, and the server's status for the IA.
 */
static bool take_prefix(const struct client *c, const uint8_t *opts,
			size_t olen, int64_t now, struct prefix *p,
			int64_t *t1_at, int64_t *t2_at, uint16_t *status)
{
	size_t ialen, off;
	const uint8_t *ia = opt_find(opts, olen, DHCP6_OPT_IA_PD, &ialen);
	uint32_t t1, t2;

	*status = DHCP6_NO_PREFIX_AVAIL;
	if (!ia || ialen < 12 || get32(ia) != c->iaid)
		return false;
	*status = opt_status(ia + 12, ialen - 12);
	if (*status != DHCP6_SUCCESS)
		return false;
	t1 = get32(ia + 4);
	t2 = get32(ia + 8);

	for (off = 12; off + 4 <= ialen;) {
		uint16_t code = get16(ia + off), len = get16(ia + off + 2);
		const uint8_t *v = ia + off + 4;

		if (off + 4 + len > ialen)
			break;
		off += 4 + (size_t)len;
		if (code != DHCP6_OPT_IAPREFIX || len < 25)
			continue;

		uint32_t preferred = get32(v), valid = get32(v + 4);

		if (!valid || preferred > valid || !v[8] || v[8] > 128)
			continue;
		if (opt_status(v + 25, len - 25) != DHCP6_SUCCESS)
			continue;
		memset(p, 0, sizeof(*p));
		p->len = v[8];
		memcpy(p->addr, v + 9, 16);
		p->preferred_until = until(now, preferred);
		p->valid_until = until(now, valid);
		/* T1 and T2 of 0 leave the times to the client: half and 0.8. */
		if (!t1 || !t2 || t1 > t2) {
			t1 = preferred == DHCP6_INFINITY ? DHCP6_INFINITY : preferred / 2;
			t2 = preferred == DHCP6_INFINITY ? DHCP6_INFINITY :
							   preferred / 5 * 4;
		}
		*t1_at = until(now, t1);
		*t2_at = until(now, t2);
		return true;
	}
	*status = DHCP6_NO_PREFIX_AVAIL;
	return false;
}

static unsigned int take_dns(struct client *c, const uint8_t *opts, size_t olen)
{
	uint8_t dns[DHCP6_DNS_MAX][16];
	unsigned int n = 0;
	size_t len;
	const uint8_t *v = opt_find(opts, olen, DHCP6_OPT_DNS_SERVERS, &len);

	memset(dns, 0, sizeof(dns));
	for (size_t off = 0; v && off + 16 <= len && n < DHCP6_DNS_MAX; off += 16)
		memcpy(dns[n++], v + off, 16);
	if (n == c->ndns && !memcmp(dns, c->dns, sizeof(dns)))
		return 0;
	memcpy(c->dns, dns, sizeof(dns));
	c->ndns = n;
	return CLIENT_EV_DNS;
}

/* A Reply that binds a prefix; the events it brings. */
static unsigned int take_reply(struct client *c, const uint8_t *opts,
			       size_t olen, int64_t now)
{
	struct prefix p;
	int64_t t1_at, t2_at;
	uint16_t status;
	size_t slen;
	const uint8_t *sid = opt_find(opts, olen, DHCP6_OPT_SERVERID, &slen);
	unsigned int ev = 0;

	if (!sid || !slen || slen > sizeof(c->server))
		return 0;
	if (!take_prefix(c, opts, olen, now, &p, &t1_at, &t2_at, &status)) {
		/*
		 * No prefix: a Request starts over with a Solicit; a Renew or a
		 * Rebind that the server no longer knows does too, keeping the
		 * prefix until it runs out.
		 */
		DHCP6D_WARN("the server gives no prefix (status %u)", status);
		begin(c, CLIENT_SOLICIT, now, DHCP6_SOL_TIMEOUT);
		c->next = now + c->rt;
		return 0;
	}

	if (!c->bound || p.len != c->prefix.len ||
	    memcmp(p.addr, c->prefix.addr, 16))
		ev |= CLIENT_EV_BOUND;
	c->bound = true;
	c->prefix = p;
	c->t1_at = t1_at;
	c->t2_at = t2_at;
	memcpy(c->server, sid, slen);
	c->server_len = slen;
	ev |= take_dns(c, opts, olen);
	c->state = CLIENT_BOUND;
	return ev;
}

/* Take the best Advertise of a Solicit on to a Request. */
static void request_best(struct client *c, int64_t now)
{
	size_t slen;
	const uint8_t *sid = opt_find(c->best + 4, c->best_len - 4,
				      DHCP6_OPT_SERVERID, &slen);

	memcpy(c->server, sid, slen);
	c->server_len = slen;
	begin(c, CLIENT_REQUEST, now, DHCP6_REQ_TIMEOUT);
}

static unsigned int input(struct client *c, const uint8_t *in, size_t inlen,
			  int64_t now)
{
	const uint8_t *opts = in + 4, *cid, *sid;
	size_t olen = inlen - 4, clen, slen, plen;
	uint32_t xid;

	if (inlen < 4)
		return 0;
	xid = (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
	if (xid != c->xid || c->state == CLIENT_BOUND)
		return 0;
	cid = opt_find(opts, olen, DHCP6_OPT_CLIENTID, &clen);
	sid = opt_find(opts, olen, DHCP6_OPT_SERVERID, &slen);
	if (!cid || clen != c->duid_len || memcmp(cid, c->duid, clen) || !sid ||
	    !slen || slen > sizeof(c->server))
		return 0;

	if (in[0] == DHCP6_ADVERTISE && c->state == CLIENT_SOLICIT) {
		struct prefix p;
		int64_t t1, t2;
		uint16_t status;
		const uint8_t *pref = opt_find(opts, olen, DHCP6_OPT_PREFERENCE,
					       &plen);
		int value = pref && plen == 1 ? pref[0] : 0;

		if (opt_status(opts, olen) != DHCP6_SUCCESS ||
		    !take_prefix(c, opts, olen, now, &p, &t1, &t2, &status) ||
		    value <= c->best_pref || inlen > sizeof(c->best))
			return 0;
		memcpy(c->best, in, inlen);
		c->best_len = inlen;
		c->best_pref = value;
		/* The highest preference is taken at once (section 18.2.1). */
		if (value == 255)
			request_best(c, now);
		return 0;
	}
	if (in[0] != DHCP6_REPLY)
		return 0;
	/* A Reply to a Solicit binds only with Rapid Commit. */
	if (c->state == CLIENT_SOLICIT &&
	    !opt_find(opts, olen, DHCP6_OPT_RAPID_COMMIT, &plen))
		return 0;
	if (c->state == CLIENT_RENEW && (slen != c->server_len ||
					 memcmp(sid, c->server, slen)))
		return 0;
	return take_reply(c, opts, olen, now);
}

/* The timers: what is due at @now. */
static unsigned int timers(struct client *c, int64_t now)
{
	if (c->bound && now >= c->prefix.valid_until) {
		DHCP6D_WARN("the delegated prefix ran out");
		c->bound = false;
		begin(c, CLIENT_SOLICIT, now, DHCP6_SOL_TIMEOUT);
		return CLIENT_EV_LOST;
	}
	switch (c->state) {
	case CLIENT_BOUND:
		if (now >= c->t1_at)
			begin(c, CLIENT_RENEW, now, DHCP6_REN_TIMEOUT);
		break;
	case CLIENT_RENEW:
		if (now >= c->t2_at)
			begin(c, CLIENT_REBIND, now, DHCP6_REB_TIMEOUT);
		break;
	case CLIENT_SOLICIT:
		/* The first wait is over: the best Advertise is taken. */
		if (c->count && now >= c->next && c->best_len)
			request_best(c, now);
		break;
	case CLIENT_REQUEST:
		if (c->count >= DHCP6_REQ_MAX_RC && now >= c->next)
			begin(c, CLIENT_SOLICIT, now, DHCP6_SOL_TIMEOUT);
		break;
	default:
		break;
	}
	return 0;
}

size_t client_step(struct client *c, int64_t now, const uint8_t *in,
		   size_t inlen, uint8_t *out, size_t cap, unsigned int *events)
{
	int64_t mrt;
	size_t n;

	*events = 0;
	if (in)
		*events |= input(c, in, inlen, now);
	*events |= timers(c, now);

	if (c->state == CLIENT_BOUND || now < c->next)
		return 0;
	n = build(c, now, type_of(c->state), out, cap);
	if (c->count)
		c->rt = 2 * c->rt + jitter(c, c->rt, false);
	mrt = max_rt(c->state);
	if (mrt && c->rt > mrt)
		c->rt = mrt + jitter(c, mrt, false);
	c->count++;
	c->next = now + c->rt;
	return n;
}

int64_t client_deadline(const struct client *c)
{
	int64_t d = FAR_FUTURE;

	if (c->state != CLIENT_BOUND)
		d = c->next;
	if (c->state == CLIENT_BOUND && c->t1_at < d)
		d = c->t1_at;
	if (c->state == CLIENT_RENEW && c->t2_at < d)
		d = c->t2_at;
	if (c->bound && c->prefix.valid_until < d)
		d = c->prefix.valid_until;
	return d;
}

size_t client_release(struct client *c, int64_t now, uint8_t *out, size_t cap)
{
	size_t n;

	if (!c->bound)
		return 0;
	c->xid = rnd(c) & 0xffffff;
	c->count = 0;
	c->started = now;
	n = build(c, now, DHCP6_RELEASE, out, cap);
	c->bound = false;
	return n;
}
