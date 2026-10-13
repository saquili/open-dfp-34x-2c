/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * GEM ports: the instances of the GEM port network CTP (class 268, G.988
 * 9.2.3) turned into the ports the PON MAC carries.
 */

#ifndef OMCI_ME_GEM_H
#define OMCI_ME_GEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw/hw.h"
#include "mib/store.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/*
 * The attributes of class 268 that define a port and its upstream traffic,
 * and those of the classes they point at: the T-CONT, the priority queue,
 * the traffic scheduler and the traffic descriptor (G.988 9.2.2, 9.2.10 to
 * 9.2.12), and the IEEE 802.1p mapper and GEM interworking termination
 * point that give a port its priorities (9.3.10, 9.2.4).
 */
enum {
	OMCI_GEM_CLASS_ID = 268,
	OMCI_GEM_ATTR_PORT_ID = 1,
	OMCI_GEM_ATTR_TCONT = 2,
	OMCI_GEM_ATTR_DIRECTION = 3,
	OMCI_GEM_ATTR_TM_US = 4,
	OMCI_GEM_ATTR_TD_US = 5,
	OMCI_GEM_ATTR_TD_DS = 9,

	OMCI_GEM_TCONT_CLASS_ID = 262,
	OMCI_GEM_TCONT_ATTR_ALLOC = 1,
	OMCI_GEM_TCONT_ATTR_POLICY = 3,

	OMCI_GEM_PRIQ_CLASS_ID = 277,
	OMCI_GEM_PRIQ_ATTR_RELATED_PORT = 6,
	OMCI_GEM_PRIQ_ATTR_SCHEDULER = 7,
	OMCI_GEM_PRIQ_ATTR_WEIGHT = 8,

	OMCI_GEM_SCHED_CLASS_ID = 278,
	OMCI_GEM_SCHED_ATTR_POLICY = 3,

	OMCI_GEM_TD_CLASS_ID = 280,
	OMCI_GEM_TD_ATTR_CIR = 1,
	OMCI_GEM_TD_ATTR_PIR = 2,
	OMCI_GEM_TD_ATTR_CBS = 3,
	OMCI_GEM_TD_ATTR_PBS = 4,

	OMCI_GEM_MAPPER_CLASS_ID = 130,
	OMCI_GEM_MAPPER_ATTR_PBIT0 = 2,
	OMCI_GEM_IW_CLASS_ID = 266,
	OMCI_GEM_IW_ATTR_CTP = 1,

	/* The ONU-G's traffic management option, and its rate-controlled value. */
	OMCI_GEM_ONUG_CLASS_ID = 256,
	OMCI_GEM_ONUG_ATTR_TM_OPTION = 4,
	OMCI_GEM_TM_RATE_CONTROLLED = 1,

	/* The policy of a T-CONT or a traffic scheduler. */
	OMCI_GEM_POLICY_WRR = 2,

	/* The multicast GEM interworking termination point and its tables. */
	OMCI_GEM_MCIW_CLASS_ID = 281,
	OMCI_GEM_MCIW_ATTR_CTP = 1,
	OMCI_GEM_MCIW_ATTR_IPV4 = 9,
	OMCI_GEM_MCIW_ATTR_IPV6 = 10,
};

/* The GEM ports the address tables of one multicast termination point name at most. */
#define OMCI_GEM_MCAST_PORTS 32u

/**
 * omci_gem_mcast_ports() - The GEM ports the address tables of a multicast
 *	GEM interworking termination point name.
 * @mib: the MIB
 * @instance: the instance of class 281
 * @ids: their port identifiers, each once, in the order the rows come
 * @cap: room in @ids
 *
 * Each row of the IPv4 and IPv6 multicast address tables names the GEM port
 * that carries its range of groups; a port other than the one of the
 * termination point's own CTP is a further multicast GEM port, which needs
 * no CTP of its own (G.988 9.2.5).
 *
 * Return: how many were written.
 */
size_t omci_gem_mcast_ports(const struct omci_mib *mib, uint16_t instance,
			    uint16_t *ids, size_t cap);

/**
 * omci_gem_program() - Program the PON MAC with every GEM port of the MIB.
 * @mib: the MIB
 * @refused: the number of instances left out; may be NULL
 *
 * The T-CONTs are mapped first, each to the Alloc-ID its T-CONT instance
 * names (G.984.3 5.5.4): none for the values that mark one the OLT has not
 * set, 0x00FF and 0xFFFF, or one above twelve bits. An Alloc-ID serves at
 * most one user traffic T-CONT and the ONU rejects the second mapping, so of
 * two instances that name one Alloc-ID, the one whose mapping is in force
 * keeps it, or else the lower, and the other is refused and maps nothing.
 *
 * An instance is left out when its port identifier does not fit twelve
 * bits, its direction is not one the standard defines, it carries traffic
 * upstream without pointing at a T-CONT of the ONU, or another instance
 * has the same port identifier.
 *
 * A port behind a multicast GEM interworking termination point that
 * carries traffic downstream only is a multicast port, and so is each port
 * that the termination point's address tables name besides (G.988 9.2.5).
 *
 * A port that carries traffic upstream takes its queue from the priority
 * queue its traffic management pointer names, the queue's priority within
 * the T-CONT, and a weight from that queue when the queue's scheduler, or
 * else its T-CONT, has the weighted round robin policy; its rates from the
 * traffic descriptor its descriptor pointer names; and its priorities from
 * the IEEE 802.1p mappers whose priorities lead to it, all eight when none
 * does. A port that carries traffic downstream is limited to the peak rate
 * of the traffic descriptor its downstream descriptor pointer names, at least
 * its committed rate, with the burst the descriptor names for the peak rate,
 * else for the committed rate; a descriptor with no peak rate limits nothing.
 *
 * Return: the status of the hardware.
 */
__must_check enum omci_status omci_gem_program(const struct omci_mib *mib,
					       unsigned int *refused);

/**
 * omci_gem_instance_rejected() - Whether the last programming left an instance out.
 * @instance: the instance of class 268
 *
 * Return: true when it did, or when the hardware refused its port.
 */
__must_check bool omci_gem_instance_rejected(uint16_t instance);

/**
 * omci_tcont_instance_rejected() - Whether the last programming refused a T-CONT.
 * @instance: the instance of class 262
 *
 * Return: true when its Alloc-ID was another instance's or the hardware
 * refused it.
 */
__must_check bool omci_tcont_instance_rejected(uint16_t instance);

/**
 * omci_gem_last() - The ports of the last programming.
 * @out: the ports
 *
 * Return: their number.
 */
size_t omci_gem_last(const struct omci_hw_gem_port **out);

#endif
