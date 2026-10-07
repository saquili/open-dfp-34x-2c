// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The server of the link to the host: it delegates the prefix the client
 * holds to the one host behind the stick (RFC 8415, section 18.3). A new
 * host takes the binding over, as a new router behind the stick would. The
 * prefix goes out with what is left of its lifetimes, and T1 and T2 short
 * enough that a new prefix from the operator reaches the host within ten
 * minutes; a prefix of before goes with the new one at a lifetime of 0, so
 * that the host stops using it.
 */

#include <string.h>

#include "dhcp6d.h"

/* At most this long between the host's renewals, in seconds. */
#define SERVER_T1_MAX 600
#define SERVER_T2_MAX 960

void server_init(struct server *s, const uint8_t *duid, size_t duid_len)
{
	memset(s, 0, sizeof(*s));
	if (duid_len > sizeof(s->duid))
		duid_len = sizeof(s->duid);
	memcpy(s->duid, duid, duid_len);
	s->duid_len = duid_len;
}

void server_offer(struct server *s, const struct prefix *p, int64_t now)
{
	(void)now;
	if (s->have && (!p || p->len != s->prefix.len ||
			memcmp(p->addr, s->prefix.addr, 16))) {
		s->old = s->prefix;
		s->have_old = true;
	}
	s->have = p != NULL;
	if (p)
		s->prefix = *p;
}

static uint32_t left(int64_t until, int64_t now)
{
	int64_t s;

	if (until >= INT64_MAX / 8)
		return DHCP6_INFINITY;
	s = (until - now) / 1000;
	if (s <= 0)
		return 0;
	return s >= (int64_t)DHCP6_INFINITY ? DHCP6_INFINITY - 1 : (uint32_t)s;
}

static void put_prefix(struct msg *m, const struct prefix *p, uint32_t preferred,
		       uint32_t valid)
{
	uint8_t v[25];

	put32(v, preferred);
	put32(v + 4, valid);
	v[8] = p->len;
	memcpy(v + 9, p->addr, 16);
	msg_opt(m, DHCP6_OPT_IAPREFIX, v, sizeof(v));
}

/* The IA_PD of the answer: the prefix, the one of before, or a status. */
static void put_ia(struct server *s, struct msg *m, uint32_t iaid, int64_t now,
		   uint16_t status)
{
	uint8_t ia[12];
	uint32_t preferred = left(s->prefix.preferred_until, now);
	uint32_t valid = left(s->prefix.valid_until, now);
	uint32_t t1 = 0, t2 = 0;

	if (!status && s->have && valid) {
		t1 = preferred / 2;
		t2 = preferred / 5 * 4;
		if (t1 > SERVER_T1_MAX)
			t1 = SERVER_T1_MAX;
		if (t2 > SERVER_T2_MAX)
			t2 = SERVER_T2_MAX;
		if (t2 < t1)
			t2 = t1;
	}
	put32(ia, iaid);
	put32(ia + 4, t1);
	put32(ia + 8, t2);
	msg_open(m, DHCP6_OPT_IA_PD);
	msg_raw(m, ia, sizeof(ia));
	if (status || !s->have || !valid) {
		uint8_t st[2];

		put16(st, status ? status : DHCP6_NO_PREFIX_AVAIL);
		msg_opt(m, DHCP6_OPT_STATUS_CODE, st, sizeof(st));
	} else {
		put_prefix(m, &s->prefix, preferred, valid);
		if (s->have_old)
			put_prefix(m, &s->old, 0, 0);
	}
	msg_close(m);
}

static void put_dns(const struct server *s, struct msg *m)
{
	if (s->ndns)
		msg_opt(m, DHCP6_OPT_DNS_SERVERS, s->dns, 16 * (size_t)s->ndns);
}

static void put_status(struct msg *m, uint16_t status)
{
	uint8_t st[2];

	put16(st, status);
	msg_opt(m, DHCP6_OPT_STATUS_CODE, st, sizeof(st));
}

/* Whether @duid with @iaid is the host of the binding. */
static bool is_host(const struct server *s, const uint8_t *duid, size_t len,
		    uint32_t iaid)
{
	return s->bound && len == s->host_len && !memcmp(duid, s->host, len) &&
	       iaid == s->host_iaid;
}

/* Bind @duid's IA @iaid at @src, until the prefix's valid lifetime ends. */
static unsigned int bind_host(struct server *s, const uint8_t *duid, size_t len,
			      uint32_t iaid, const uint8_t src[16])
{
	bool same = is_host(s, duid, len, iaid) && !memcmp(src, s->host_addr, 16);

	s->bound = true;
	memcpy(s->host, duid, len);
	s->host_len = len;
	s->host_iaid = iaid;
	memcpy(s->host_addr, src, 16);
	s->bound_until = s->prefix.valid_until;
	return same ? 0 : SERVER_EV_ROUTE;
}

size_t server_answer(struct server *s, const uint8_t *in, size_t inlen,
		     const uint8_t src[16], int64_t now, uint8_t *out,
		     size_t cap, unsigned int *events)
{
	const uint8_t *opts = in + 4, *cid, *sid, *ia;
	size_t olen, clen, slen, ialen, rlen;
	uint32_t xid, iaid = 0;
	uint8_t type;
	struct msg m;

	*events = 0;
	if (inlen < 4)
		return 0;
	olen = inlen - 4;
	type = in[0];
	xid = (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
	cid = opt_find(opts, olen, DHCP6_OPT_CLIENTID, &clen);
	sid = opt_find(opts, olen, DHCP6_OPT_SERVERID, &slen);
	ia = opt_find(opts, olen, DHCP6_OPT_IA_PD, &ialen);
	if (ia && ialen >= 12)
		iaid = get32(ia);
	else
		ia = NULL;

	/* A message for another server is not ours (section 16). */
	if (sid && (slen != s->duid_len || memcmp(sid, s->duid, slen)))
		return 0;
	if (!cid || !clen || clen > DHCP6_DUID_MAX)
		return 0;

	switch (type) {
	case DHCP6_SOLICIT:
		if (sid || !ia)
			return 0;
		if (opt_find(opts, olen, DHCP6_OPT_RAPID_COMMIT, &rlen) && s->have) {
			msg_start(&m, out, cap, DHCP6_REPLY, xid);
			msg_opt(&m, DHCP6_OPT_RAPID_COMMIT, NULL, 0);
			*events |= bind_host(s, cid, clen, iaid, src);
		} else {
			msg_start(&m, out, cap, DHCP6_ADVERTISE, xid);
		}
		break;
	case DHCP6_REQUEST:
	case DHCP6_RENEW:
		if (!sid || !ia)
			return 0;
		msg_start(&m, out, cap, DHCP6_REPLY, xid);
		/* A Renew of a binding the server does not hold says so. */
		if (type == DHCP6_RENEW && !is_host(s, cid, clen, iaid)) {
			msg_opt(&m, DHCP6_OPT_SERVERID, s->duid, s->duid_len);
			msg_opt(&m, DHCP6_OPT_CLIENTID, cid, clen);
			put_ia(s, &m, iaid, now, DHCP6_NO_BINDING);
			return m.overflow ? 0 : m.len;
		}
		if (s->have)
			*events |= bind_host(s, cid, clen, iaid, src);
		break;
	case DHCP6_REBIND:
		if (sid || !ia)
			return 0;
		/* Any host may rebind to the prefix there is: it takes it. */
		msg_start(&m, out, cap, DHCP6_REPLY, xid);
		if (s->have)
			*events |= bind_host(s, cid, clen, iaid, src);
		break;
	case DHCP6_RELEASE:
		if (!sid)
			return 0;
		msg_start(&m, out, cap, DHCP6_REPLY, xid);
		msg_opt(&m, DHCP6_OPT_SERVERID, s->duid, s->duid_len);
		msg_opt(&m, DHCP6_OPT_CLIENTID, cid, clen);
		if (ia && is_host(s, cid, clen, iaid)) {
			s->bound = false;
			*events |= SERVER_EV_UNROUTE;
		}
		put_status(&m, DHCP6_SUCCESS);
		return m.overflow ? 0 : m.len;
	case DHCP6_INFORMATION_REQUEST:
		if (sid || ia)
			return 0;
		msg_start(&m, out, cap, DHCP6_REPLY, xid);
		msg_opt(&m, DHCP6_OPT_SERVERID, s->duid, s->duid_len);
		msg_opt(&m, DHCP6_OPT_CLIENTID, cid, clen);
		put_dns(s, &m);
		return m.overflow ? 0 : m.len;
	default:
		return 0;
	}

	msg_opt(&m, DHCP6_OPT_SERVERID, s->duid, s->duid_len);
	msg_opt(&m, DHCP6_OPT_CLIENTID, cid, clen);
	put_ia(s, &m, iaid, now, 0);
	put_dns(s, &m);
	if (m.overflow) {
		*events = 0;
		return 0;
	}
	/* The host of the binding now knows the prefix of before is gone. */
	if (type != DHCP6_SOLICIT && is_host(s, cid, clen, iaid))
		s->have_old = false;
	if (type == DHCP6_SOLICIT && (*events & SERVER_EV_ROUTE))
		s->have_old = false;
	return m.len;
}

bool server_expire(struct server *s, int64_t now)
{
	if (!s->bound || now < s->bound_until)
		return false;
	s->bound = false;
	return true;
}
