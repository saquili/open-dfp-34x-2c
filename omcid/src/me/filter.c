// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The filters of filter.h. */

#include <stdbool.h>
#include <string.h>

#include "me/bridge.h"
#include "me/filter.h"
#include "omcid/array_size.h"
#include "sys/log.h"

#define ME_PPTP_ETH 11u
#define PPTP_PPPOE_FILTER 14u
#define ME_BRIDGE_PORT 47u
#define BRIDGE_PORT_TP_TYPE 3u
#define ME_MAC_FILTER 49u
#define MAC_FILTER_TABLE 1u
#define ME_PREASSIGN 79u

/*
 * A row of the MAC filter table (G.988 9.3.6): the entry, the filter byte and
 * the address. Bit 1 of the filter byte filters, else the entry forwards; bit
 * 2 names the source address, else the destination.
 */
#define ROW_LEN 8u
#define ROW_FILTER 0x01u
#define ROW_SOURCE 0x02u
#define ROW_MAC 2u

#define ETH_PPPOE_DISCOVERY 0x8863u
#define ETH_PPPOE_SESSION 0x8864u

#define ALL { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
#define BUT_LAST_OCTET { 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 }

/* An address of a group of the filter preassign table, by its attribute. */
struct group {
	uint8_t attr;
	uint8_t mac[6];
	uint8_t mask[6];
	uint16_t ethertype;
};

/* The groups of G.988 9.3.7, attributes 1 to 10. */
static const struct group g_groups[] = {
	{ 1,
	  { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x00 },
	  { 0xff, 0xff, 0xff, 0x80, 0x00, 0x00 },
	  0 },
	{ 2,
	  { 0x33, 0x33, 0x00, 0x00, 0x00, 0x00 },
	  { 0xff, 0xff, 0x00, 0x00, 0x00, 0x00 },
	  0 },
	{ 3, ALL, ALL, 0x0800 },
	{ 4, ALL, ALL, 0x8035 },
	{ 5, ALL, ALL, 0x8137 },
	{ 5, { 0x09, 0x00, 0x1b, 0xff, 0xff, 0xff }, ALL, 0 },
	{ 5, { 0x09, 0x00, 0x4e, 0x00, 0x00, 0x02 }, ALL, 0 },
	{ 6, { 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 }, ALL, 0 },
	{ 7, ALL, ALL, 0x809b },
	{ 7, ALL, ALL, 0x80f3 },
	{ 7, { 0x09, 0x00, 0x07, 0x00, 0x00, 0x00 }, BUT_LAST_OCTET, 0 },
	{ 7, { 0x09, 0x00, 0x07, 0xff, 0xff, 0xff }, ALL, 0 },
	{ 8, { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 }, BUT_LAST_OCTET, 0 },
	{ 9, ALL, ALL, 0x0806 },
	{ 10, ALL, ALL, ETH_PPPOE_DISCOVERY },
};

/* What the passes leave upstream: the individual and the group addresses. */
static const struct omci_hw_filter g_rest[] = {
	{ .upstream = true, .dmask = { 0x01 } },
	{ .upstream = true, .dmac = { 0x01 }, .dmask = { 0x01 } },
};

static const uint16_t g_pppoe[] = { ETH_PPPOE_DISCOVERY, ETH_PPPOE_SESSION };

/* The rules being made: written while they fit, counted all the same. */
struct rules {
	struct omci_hw_filter *out;
	size_t cap;
	size_t n;
};

/* What the last call of omci_filter_apply() gave the backend. */
static struct omci_hw_filter g_last[OMCI_HW_FILTER_MAX];
static size_t g_last_n;
static bool g_given;

/* What was said once: rules cut, no backend, an admitting UNI-side filter. */
static bool g_said_cut;
static bool g_said_none;
static bool g_said_uni;

static bool same(const struct omci_hw_filter *a, const struct omci_hw_filter *b)
{
	return a->upstream == b->upstream && a->pass == b->pass &&
	       a->ethertype == b->ethertype &&
	       memcmp(a->dmac, b->dmac, sizeof(a->dmac)) == 0 &&
	       memcmp(a->dmask, b->dmask, sizeof(a->dmask)) == 0 &&
	       memcmp(a->smac, b->smac, sizeof(a->smac)) == 0 &&
	       memcmp(a->smask, b->smask, sizeof(a->smask)) == 0;
}

static void put(struct rules *r, const struct omci_hw_filter *f)
{
	for (size_t i = 0; i < r->n && i < r->cap; i++)
		if (same(&r->out[i], f))
			return;
	if (r->n < r->cap)
		r->out[r->n] = *f;
	r->n++;
}

/* The side of the bridge a port is on: 1 the UNI's, 2 the PON's, 0 neither. */
static unsigned int side_of(const struct omci_mib *mib, uint16_t instance)
{
	uint8_t tp_type = 0;
	size_t len = 0;

	(void)!omci_mib_attr_get(mib, ME_BRIDGE_PORT, instance,
				 BRIDGE_PORT_TP_TYPE, &tp_type, 1u, &len);
	switch (tp_type) {
	case OMCI_BRIDGE_TP_PPTP_ETH:
	case OMCI_BRIDGE_TP_VEIP:
		return 1;
	case OMCI_BRIDGE_TP_MAPPER:
	case OMCI_BRIDGE_TP_GEM_IW:
	case OMCI_BRIDGE_TP_MCAST_GEM:
		return 2;
	default:
		return 0;
	}
}

/* A rule on the address of a row of the MAC filter table. */
static void put_address(struct rules *r, bool upstream, bool pass,
			const uint8_t *row, uint16_t ethertype)
{
	static const uint8_t all[6] = ALL;
	struct omci_hw_filter f = {
		.upstream = upstream,
		.pass = pass,
		.ethertype = ethertype,
	};

	if (row[1] & ROW_SOURCE) {
		memcpy(f.smac, &row[ROW_MAC], sizeof(f.smac));
		memcpy(f.smask, all, sizeof(f.smask));
	} else {
		memcpy(f.dmac, &row[ROW_MAC], sizeof(f.dmac));
		memcpy(f.dmask, all, sizeof(f.dmask));
	}
	put(r, &f);
}

/* The drops of a port: its groups that filter, its entries that filter. */
static void drops(const struct omci_mib *mib, uint16_t instance, bool upstream,
		  struct rules *r)
{
	uint8_t row[ROW_LEN];

	for (size_t i = 0; i < ARRAY_SIZE(g_groups); i++) {
		const struct group *g = &g_groups[i];
		struct omci_hw_filter f = {
			.upstream = upstream,
			.ethertype = g->ethertype,
		};
		uint8_t on = 0;
		size_t len = 0;

		if (omci_mib_attr_get(mib, ME_PREASSIGN, instance, g->attr, &on,
				      1u, &len) != OMCI_OK ||
		    on == 0)
			continue;
		memcpy(f.dmac, g->mac, sizeof(f.dmac));
		memcpy(f.dmask, g->mask, sizeof(f.dmask));
		put(r, &f);
	}
	for (size_t i = 0;
	     omci_mib_table_get(mib, ME_MAC_FILTER, instance, MAC_FILTER_TABLE,
				i, row, sizeof(row)) == OMCI_OK;
	     i++)
		if (row[1] & ROW_FILTER)
			put_address(r, upstream, false, row, 0);
}

/*
 * The passes upstream: each address that an entry of a PON side port forwards,
 * with the EtherTypes of PPPoE when its filter is on, or those EtherTypes
 * alone. Return: whether there are any.
 */
static bool passes(const struct omci_mib *mib, bool pppoe, struct rules *r)
{
	struct omci_mib_cursor c;
	uint8_t row[ROW_LEN];
	bool any = false;

	for (enum omci_status rc = omci_mib_walk_first(mib, &c); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &c)) {
		if (c.class_id != ME_BRIDGE_PORT)
			continue;
		const bool pon = side_of(mib, c.instance) == 2;

		for (size_t i = 0;
		     omci_mib_table_get(mib, ME_MAC_FILTER, c.instance,
					MAC_FILTER_TABLE, i, row,
					sizeof(row)) == OMCI_OK;
		     i++) {
			if (row[1] & ROW_FILTER)
				continue;
			if (!pon) {
				if (!g_said_uni)
					OMCI_INFO(
						"filter: bridge port %#06x admits only some addresses downstream; kept, not applied",
						c.instance);
				g_said_uni = true;
				continue;
			}
			any = true;
			if (!pppoe)
				put_address(r, true, true, row, 0);
			for (size_t k = 0; pppoe && k < ARRAY_SIZE(g_pppoe);
			     k++)
				put_address(r, true, true, row, g_pppoe[k]);
		}
	}
	for (size_t k = 0; !any && pppoe && k < ARRAY_SIZE(g_pppoe); k++) {
		const struct omci_hw_filter f = {
			.upstream = true,
			.pass = true,
			.ethertype = g_pppoe[k],
		};

		put(r, &f);
	}
	return any || pppoe;
}

size_t omci_filter_rules(const struct omci_mib *mib, struct omci_hw_filter *out,
			 size_t cap)
{
	struct rules r = { .out = out, .cap = cap };
	struct omci_mib_cursor c;
	bool pppoe = false;

	for (enum omci_status rc = omci_mib_walk_first(mib, &c); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &c)) {
		uint8_t on = 0;
		size_t len = 0;

		if (c.class_id == ME_PPTP_ETH &&
		    omci_mib_attr_get(mib, ME_PPTP_ETH, c.instance,
				      PPTP_PPPOE_FILTER, &on, 1u,
				      &len) == OMCI_OK)
			pppoe |= on != 0;
		if (c.class_id != ME_BRIDGE_PORT)
			continue;
		const unsigned int side = side_of(mib, c.instance);

		if (side != 0)
			drops(mib, c.instance, side == 2, &r);
	}
	if (passes(mib, pppoe, &r))
		for (size_t i = 0; i < ARRAY_SIZE(g_rest); i++)
			put(&r, &g_rest[i]);
	return r.n;
}

enum omci_status omci_filter_apply(const struct omci_mib *mib)
{
	struct omci_hw_filter now[OMCI_HW_FILTER_MAX];
	size_t n = omci_filter_rules(mib, now, OMCI_HW_FILTER_MAX);

	if (n > OMCI_HW_FILTER_MAX) {
		if (!g_said_cut)
			OMCI_WARN(
				"filter: the filters ask for %zu rules; the first %u are applied",
				n, OMCI_HW_FILTER_MAX);
		g_said_cut = true;
		n = OMCI_HW_FILTER_MAX;
	}

	bool changed = !g_given || n != g_last_n;

	if (omci_hw()->filter_apply == NULL) {
		if (n != 0 && !g_said_none)
			OMCI_INFO(
				"filter: this backend has no filters; they are kept, not applied");
		g_said_none |= n != 0;
		return OMCI_OK;
	}
	for (size_t i = 0; !changed && i < n; i++)
		changed = !same(&now[i], &g_last[i]);
	if (!changed)
		return OMCI_OK;

	const enum omci_status rc = omci_hw()->filter_apply(now, n);

	if (rc != OMCI_OK) {
		g_given = false;
		OMCI_WARN("filter: the rules were not applied: %s",
			  omci_status_str(rc));
		return rc;
	}
	memcpy(g_last, now, n * sizeof(now[0]));
	g_last_n = n;
	g_given = true;
	OMCI_INFO("filter: %zu rule(s) in force", n);
	return OMCI_OK;
}

void omci_filter_forget(void)
{
	g_given = false;
}
