/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The links the stick's scripts set, as route netlink requests: an interface
 * up or down, its MTU, its MAC address and its bridge, a VLAN interface on
 * another, a bridge that filters VLANs, the VLANs of a bridge's port or of
 * the bridge itself, and the removal of an interface. The requests are those
 * ip and bridge make for
 *
 *   ip link set DEV [up|down] [mtu N] [address MAC] [master BRIDGE]
 *   ip link add link DEV name NAME type vlan id VID
 *   ip link add name NAME type bridge vlan_filtering 1 vlan_default_pvid 0
 *   bridge vlan add|del dev DEV vid VID [pvid] [untagged] [self]
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
 * @master: the bridge it becomes a port of, by interface index; 0 for none
 */
struct link_set {
	int ifindex;
	bool has_state;
	bool up;
	bool has_mtu;
	uint32_t mtu;
	bool has_mac;
	uint8_t mac[6];
	int master;
};

/**
 * struct bridge_vlan - A VLAN of a bridge's port, or of the bridge itself.
 * @ifindex: the port, or the bridge with @self
 * @vid: the VLAN, 1 to 4094
 * @pvid: the VLAN the port gives the untagged frames it receives
 * @untagged: the port sends the VLAN's frames untagged
 * @self: @ifindex is the bridge, which is the member: the CPU's side of it
 */
struct bridge_vlan {
	int ifindex;
	uint16_t vid;
	bool pvid;
	bool untagged;
	bool self;
};

/*
 * Each request is built in @buf, of MNL_SOCKET_BUFFER_SIZE bytes, with
 * sequence number @seq, and asks for the kernel's acknowledgement.
 */
struct nlmsghdr *link_set_msg(char *buf, const struct link_set *s, uint32_t seq);

/* A VLAN interface @name of VLAN @vid on the interface @link_ifindex; it is
 * refused when the name is taken.
 */
struct nlmsghdr *link_vlan_add_msg(char *buf, int link_ifindex, const char *name,
				   uint16_t vid, uint32_t seq);

struct nlmsghdr *link_del_msg(char *buf, int ifindex, uint32_t seq);

/*
 * A bridge @name that filters VLANs from the start and gives a new port no
 * VLAN of its own (a default PVID of 0); it is refused when the name is
 * taken.
 */
struct nlmsghdr *link_bridge_add_msg(char *buf, const char *name, uint32_t seq);

/* The VLAN @v added to its port or bridge, or with @add false removed. */
struct nlmsghdr *bridge_vlan_msg(char *buf, const struct bridge_vlan *v, bool add,
				 uint32_t seq);

#endif
