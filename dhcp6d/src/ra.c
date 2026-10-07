// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The Router Advertisement on the link to the host (RFC 4861, section 4.2):
 * the stick as the host's default router while it has a prefix to give, the
 * link's MTU, which the session's sets, and the name servers (RFC 8106). It
 * carries no prefix of its own: the host takes the delegated one by DHCPv6,
 * which the Other flag points at.
 */

#include <string.h>

#include "dhcp6d.h"

#define RA_FLAG_OTHER 0x40
#define ND_OPT_SOURCE_LINKADDR 1
#define ND_OPT_MTU 5
#define ND_OPT_RDNSS 25

size_t ra_build(const struct ra *r, uint8_t *out, size_t cap)
{
	size_t len = 16 + 8 + 8 + (r->ndns ? 8 + 16 * (size_t)r->ndns : 0);
	uint8_t *p = out;

	if (cap < len)
		return 0;
	memset(out, 0, len);

	p[0] = ICMP6_ROUTER_ADVERT;
	p[1] = 0;
	/* the checksum, p[2] and p[3], is the kernel's */
	p[4] = 64; /* the hop limit the host should use */
	p[5] = RA_FLAG_OTHER;
	put16(p + 6, r->lifetime);
	/* reachable time and retransmission timer: unspecified */
	p += 16;

	p[0] = ND_OPT_SOURCE_LINKADDR;
	p[1] = 1;
	memcpy(p + 2, r->mac, 6);
	p += 8;

	p[0] = ND_OPT_MTU;
	p[1] = 1;
	put32(p + 4, r->mtu);
	p += 8;

	if (r->ndns) {
		p[0] = ND_OPT_RDNSS;
		p[1] = (uint8_t)(1 + 2 * r->ndns);
		put32(p + 4, r->dns_lifetime);
		for (unsigned int i = 0; i < r->ndns; i++)
			memcpy(p + 8 + 16 * i, r->dns[i], 16);
	}
	return len;
}
