/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The links the stick's scripts set, as route netlink requests: an interface
 * up or down, its MTU and its MAC address, a VLAN interface on another, and
 * the removal of an interface. The requests are those ip makes for
 *
 *   ip link set DEV [up|down] [mtu N] [address MAC]
 *   ip link add link DEV name NAME type vlan id VID
 *   ip link del DEV
 *
 * so that the kernel takes them as it takes ip's, with no iproute2 and no
 * BusyBox applet on the stick's image.
 */

#ifndef ONU_UTILS_LINKMSG_H
#define ONU_UTILS_LINKMSG_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/netlink.h>

/**
 * struct link_set - What a request sets of an interface; what is not named
 *	stays as it is.
 * @ifindex: the interface
 * @has_state: it is taken up or down
 * @up: up when true, down when false
 * @has_mtu: its MTU is set
 * @mtu: that MTU
 * @has_mac: its MAC address is set
 * @mac: that address
 */
struct link_set {
	int ifindex;
	bool has_state;
	bool up;
	bool has_mtu;
	uint32_t mtu;
	bool has_mac;
	uint8_t mac[6];
};

/*
 * Each request is built in @buf, of MNL_SOCKET_BUFFER_SIZE bytes, with
 * sequence number @seq, and asks for the kernel's acknowledgement.
 */
struct nlmsghdr *link_set_msg(char *buf, const struct link_set *s, uint32_t seq);

/* A VLAN interface @name of VLAN @vid on the interface @link_ifindex; it is
 * refused when the name is taken. */
struct nlmsghdr *link_vlan_add_msg(char *buf, int link_ifindex, const char *name,
				   uint16_t vid, uint32_t seq);

struct nlmsghdr *link_del_msg(char *buf, int ifindex, uint32_t seq);

#endif
