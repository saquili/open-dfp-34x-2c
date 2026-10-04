// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

#include "gemlink.h"

#include <errno.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The link's kind is "gem": the attribute holds the name with its NUL. */
static bool is_gem(const struct nlattr *kind)
{
	return mnl_attr_get_payload_len(kind) == sizeof("gem") &&
	       !memcmp(mnl_attr_get_payload(kind), "gem", sizeof("gem"));
}

static int get_u8(const struct nlattr *a, uint8_t *v)
{
	if (mnl_attr_get_payload_len(a) != sizeof(uint8_t))
		return -EINVAL;
	*v = mnl_attr_get_u8(a);
	return 0;
}

/* An NLA_UINT: four bytes, or eight for a value that needs them. */
static int get_uint(const struct nlattr *a, uint64_t *v)
{
	switch (mnl_attr_get_payload_len(a)) {
	case sizeof(uint32_t):
		*v = mnl_attr_get_u32(a);
		return 0;
	case sizeof(uint64_t):
		*v = mnl_attr_get_u64(a);
		return 0;
	}
	return -EINVAL;
}

static int gem_data(const struct nlattr *data, struct gem_link *g)
{
	const struct nlattr *a;

	mnl_attr_for_each_nested(a, data) {
		const uint16_t len = mnl_attr_get_payload_len(a);

		switch (mnl_attr_get_type(a)) {
		case IFLA_GEM_PORT_ID:
			if (len != sizeof(uint16_t))
				return -EINVAL;
			g->port_id = mnl_attr_get_u16(a);
			break;
		case IFLA_GEM_DIRECTION:
			if (len != sizeof(uint8_t))
				return -EINVAL;
			g->direction = mnl_attr_get_u8(a);
			break;
		case IFLA_GEM_TCONT:
			if (len != sizeof(uint32_t))
				return -EINVAL;
			g->tcont = mnl_attr_get_u32(a);
			g->have_tcont = true;
			break;
		case IFLA_GEM_MULTICAST:
			g->multicast = true;
			break;
		case IFLA_GEM_PBITS:
			if (get_u8(a, &g->pbits))
				return -EINVAL;
			g->have_upstream = true;
			break;
		case IFLA_GEM_QUEUE:
			if (get_u8(a, &g->queue))
				return -EINVAL;
			g->have_upstream = true;
			break;
		case IFLA_GEM_WEIGHT:
			if (get_u8(a, &g->weight))
				return -EINVAL;
			g->have_upstream = true;
			break;
		case IFLA_GEM_CIR:
			if (get_uint(a, &g->cir))
				return -EINVAL;
			g->have_upstream = true;
			break;
		case IFLA_GEM_PIR:
			if (get_uint(a, &g->pir))
				return -EINVAL;
			g->have_upstream = true;
			break;
		}
	}
	return 0;
}

int gem_link_parse(const struct nlmsghdr *nlh, struct gem_link *g)
{
	const struct nlattr *a, *name = NULL, *info = NULL, *kind = NULL;
	const struct nlattr *data = NULL;

	memset(g, 0, sizeof(*g));
	g->direction = IFLA_GEM_DIR_BOTH;
	g->pbits = 0xff;
	if (nlh->nlmsg_type != RTM_NEWLINK)
		return 0;

	mnl_attr_for_each(a, nlh, sizeof(struct ifinfomsg)) {
		if (mnl_attr_get_type(a) == IFLA_IFNAME)
			name = a;
		else if (mnl_attr_get_type(a) == IFLA_LINKINFO)
			info = a;
	}
	if (!info)
		return 0;
	mnl_attr_for_each_nested(a, info) {
		if (mnl_attr_get_type(a) == IFLA_INFO_KIND)
			kind = a;
		else if (mnl_attr_get_type(a) == IFLA_INFO_DATA)
			data = a;
	}
	if (!kind || !is_gem(kind))
		return 0;

	if (!name || mnl_attr_validate(name, MNL_TYPE_NUL_STRING) < 0 ||
	    strlen(mnl_attr_get_str(name)) >= sizeof(g->name))
		return -EINVAL;
	snprintf(g->name, sizeof(g->name), "%s", mnl_attr_get_str(name));
	if (data && gem_data(data, g))
		return -EINVAL;
	return 1;
}

void gem_link_print(FILE *f, const struct gem_link *g)
{
	static const char *const dir[] = {
		[IFLA_GEM_DIR_UPSTREAM] = "up",
		[IFLA_GEM_DIR_DOWNSTREAM] = "down",
		[IFLA_GEM_DIR_BOTH] = "both",
	};
	const char *d = g->direction < ARRAY_SIZE(dir) ? dir[g->direction] : NULL;

	fprintf(f, "%s port %u %s", g->name, g->port_id, d ? d : "unknown");
	if (g->have_tcont)
		fprintf(f, " tcont %u", g->tcont);
	if (g->multicast)
		fputs(" multicast", f);
	if (g->have_upstream) {
		if (g->pbits != 0xff)
			fprintf(f, " pbits 0x%02x", g->pbits);
		fprintf(f, " queue %u", g->queue);
		if (g->weight)
			fprintf(f, " weight %u", g->weight);
		else
			fputs(" strict", f);
		if (g->cir)
			fprintf(f, " cir %llu", (unsigned long long)g->cir);
		if (g->pir)
			fprintf(f, " pir %llu", (unsigned long long)g->pir);
	}
	fputc('\n', f);
}
