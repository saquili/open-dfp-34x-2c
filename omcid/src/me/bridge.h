/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Bridges: the MAC bridge ports of the MIB (class 47, G.988 9.3.4) turned
 * into the ports and VLAN lists the switch bridges.
 */

#ifndef OMCI_ME_BRIDGE_H
#define OMCI_ME_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw/hw.h"
#include "mib/store.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/* The classes that make up a bridge, and the attributes that link them. */
enum {
	OMCI_BRIDGE_CLASS_ID = 45,
	OMCI_BRIDGE_PORT_CLASS_ID = 47,
	OMCI_BRIDGE_VLAN_CLASS_ID = 84,
	OMCI_BRIDGE_MAPPER_CLASS_ID = 130,
	OMCI_BRIDGE_GEMIW_CLASS_ID = 266,
	OMCI_BRIDGE_MCIW_CLASS_ID = 281,
	OMCI_BRIDGE_GEM_CLASS_ID = 268,

	OMCI_BRIDGE_PORT_ATTR_BRIDGE = 1,
	OMCI_BRIDGE_PORT_ATTR_NUM = 2,
	OMCI_BRIDGE_PORT_ATTR_TPTYPE = 3,
	OMCI_BRIDGE_PORT_ATTR_TPPTR = 4,

	OMCI_BRIDGE_VLAN_ATTR_TABLE = 1,
	OMCI_BRIDGE_VLAN_ATTR_FWDOP = 2,
	OMCI_BRIDGE_VLAN_ATTR_COUNT = 3,

	OMCI_BRIDGE_MAPPER_ATTR_FIRST = 2,
	OMCI_BRIDGE_MAPPER_PBITS = 8,

	OMCI_BRIDGE_GEMIW_ATTR_CTP = 1,

	OMCI_BRIDGE_GEM_ATTR_PORT_ID = 1,
};

/**
 * enum omci_bridge_tp_type - What a bridge port leads to (attribute 3 of class 47).
 * @OMCI_BRIDGE_TP_PPTP_ETH: an Ethernet UNI
 * @OMCI_BRIDGE_TP_MAPPER: an IEEE 802.1p mapper, which leads to GEM ports
 * @OMCI_BRIDGE_TP_GEM_IW: a GEM interworking termination point
 * @OMCI_BRIDGE_TP_MCAST_GEM: a multicast GEM interworking termination point
 * @OMCI_BRIDGE_TP_VEIP: a virtual Ethernet interface point
 */
enum omci_bridge_tp_type {
	OMCI_BRIDGE_TP_PPTP_ETH = 1,
	OMCI_BRIDGE_TP_MAPPER = 3,
	OMCI_BRIDGE_TP_GEM_IW = 5,
	OMCI_BRIDGE_TP_MCAST_GEM = 6,
	OMCI_BRIDGE_TP_VEIP = 11,
};

/**
 * omci_bridge_program() - Program the switch with every bridge port of the MIB.
 * @mib: the MIB
 * @refused: the number of ports left out; may be NULL
 *
 * A port on the subscriber side becomes the UNI. A port on the network side
 * becomes the GEM ports it leads to, directly or through a mapper. Each
 * port carries the VIDs of its VLAN tagging filter data (class 84).
 *
 * Return: the status of the hardware.
 */
__must_check enum omci_status omci_bridge_program(const struct omci_mib *mib,
						  unsigned int *refused);

/**
 * omci_bridge_instance_rejected() - Whether the hardware refused a bridge port.
 * @instance: the instance of class 47
 *
 * Return: true when it did.
 */
__must_check bool omci_bridge_instance_rejected(uint16_t instance);

/**
 * omci_bridge_last() - The ports of the last programming.
 * @out: the ports
 *
 * Return: their number.
 */
size_t omci_bridge_last(const struct omci_hw_bridge_port **out);

#endif
