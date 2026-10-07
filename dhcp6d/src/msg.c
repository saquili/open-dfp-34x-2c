// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * DHCPv6 messages: a type, a 24-bit transaction id and options, each a code,
 * a length and a value, which may hold options of its own (RFC 8415, section
 * 8 and 21).
 */

#include <string.h>

#include "dhcp6d.h"

void msg_start(struct msg *m, uint8_t *buf, size_t cap, uint8_t type,
	       uint32_t xid)
{
	memset(m, 0, sizeof(*m));
	m->buf = buf;
	m->cap = cap;
	if (cap < 4) {
		m->overflow = true;
		return;
	}
	buf[0] = type;
	buf[1] = (uint8_t)(xid >> 16);
	buf[2] = (uint8_t)(xid >> 8);
	buf[3] = (uint8_t)xid;
	m->len = 4;
}

void msg_raw(struct msg *m, const void *val, size_t len)
{
	if (m->overflow || m->len + len > m->cap) {
		m->overflow = true;
		return;
	}
	if (len)
		memcpy(m->buf + m->len, val, len);
	m->len += len;
}

void msg_opt(struct msg *m, uint16_t code, const void *val, size_t len)
{
	uint8_t head[4];

	put16(head, code);
	put16(head + 2, (uint16_t)len);
	msg_raw(m, head, sizeof(head));
	msg_raw(m, val, len);
}

void msg_opt16(struct msg *m, uint16_t code, uint16_t v)
{
	uint8_t b[2];

	put16(b, v);
	msg_opt(m, code, b, sizeof(b));
}

void msg_open(struct msg *m, uint16_t code)
{
	uint8_t head[4] = { 0 };

	put16(head, code);
	if (m->depth >= ARRAY_SIZE(m->nest)) {
		m->overflow = true;
		return;
	}
	m->nest[m->depth++] = m->len;
	msg_raw(m, head, sizeof(head));
}

void msg_close(struct msg *m)
{
	size_t at;

	if (!m->depth) {
		m->overflow = true;
		return;
	}
	at = m->nest[--m->depth];
	if (m->overflow)
		return;
	put16(m->buf + at + 2, (uint16_t)(m->len - at - 4));
}

const uint8_t *opt_find(const uint8_t *opts, size_t olen, uint16_t code,
			size_t *len)
{
	size_t off = 0;

	while (off + 4 <= olen) {
		uint16_t c = get16(opts + off), l = get16(opts + off + 2);

		if (off + 4 + l > olen)
			return NULL;
		if (c == code) {
			*len = l;
			return opts + off + 4;
		}
		off += 4 + (size_t)l;
	}
	return NULL;
}

uint16_t opt_status(const uint8_t *opts, size_t olen)
{
	size_t len;
	const uint8_t *s = opt_find(opts, olen, DHCP6_OPT_STATUS_CODE, &len);

	return s && len >= 2 ? get16(s) : DHCP6_SUCCESS;
}

size_t duid_ll(uint8_t *out, const uint8_t mac[6])
{
	put16(out, 3); /* DUID-LL */
	put16(out + 2, 1); /* Ethernet */
	memcpy(out + 4, mac, 6);
	return 10;
}
