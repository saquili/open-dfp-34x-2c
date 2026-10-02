/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * A GEM interface as the kernel's link message describes it: its name, and
 * the attributes of its kind, "gem" (IFLA_GEM_* in linux/if_link.h).
 */

#ifndef ONU_UTILS_GEMLINK_H
#define ONU_UTILS_GEMLINK_H

#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <linux/netlink.h>

/**
 * struct gem_link - A GEM interface.
 * @name: the interface's name
 * @port_id: the GEM Port-ID
 * @direction: an &enum ifla_gem_direction; both ways when the message has none
 * @have_tcont: the message named a T-CONT
 * @tcont: the index of the T-CONT that carries the port upstream
 * @multicast: a multicast GEM port
 */
struct gem_link {
	char name[IF_NAMESIZE];
	uint16_t port_id;
	uint8_t direction;
	bool have_tcont;
	uint32_t tcont;
	bool multicast;
};

/* Read a link message: 1 for a GEM interface, 0 for another, -EINVAL when malformed. */
int gem_link_parse(const struct nlmsghdr *nlh, struct gem_link *g);

/* One line: "gem4095 port 4095 down multicast", "gem256 port 256 both tcont 0". */
void gem_link_print(FILE *f, const struct gem_link *g);

#endif
