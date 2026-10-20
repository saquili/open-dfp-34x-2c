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
 * @have_upstream: the message gave the port's upstream attributes, as the
 *	kernel does for a port that carries traffic upstream
 * @pbits: the IEEE 802.1p priorities of its VLANs' frames it takes upstream
 * @queue: its upstream queue in the T-CONT, 0 the highest priority
 * @weight: the queue's weight, 0 for strict priority
 * @cir: the committed rate of its upstream traffic, bytes per second
 * @pir: the peak rate, bytes per second; 0 for no limit
 * @encrypted: the OLT encrypts the port's downstream, as the PON device
 *	reports it (encrypted-port-id of the "pon" family): not a link
 *	attribute, so the parse leaves it false and the caller sets it
 */
struct gem_link {
	char name[IF_NAMESIZE];
	uint16_t port_id;
	uint8_t direction;
	bool have_tcont;
	uint32_t tcont;
	bool multicast;
	bool have_upstream;
	uint8_t pbits;
	uint8_t queue;
	uint8_t weight;
	uint64_t cir;
	uint64_t pir;
	bool encrypted;
};

/* Read a link message: 1 for a GEM interface, 0 for another, -EINVAL when malformed. */
int gem_link_parse(const struct nlmsghdr *nlh, struct gem_link *g);

/*
 * One line: "gem4095 port 4095 down multicast", "gem256 port 256 both tcont 0
 * queue 0 strict encrypted"; the priorities when not all eight, the rates
 * when set, and "encrypted" when the OLT encrypts the port.
 */
void gem_link_print(FILE *f, const struct gem_link *g);

#endif
