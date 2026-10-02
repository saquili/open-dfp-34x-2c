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

/* The attributes of class 268 that define a port, and the T-CONT class. */
enum {
	OMCI_GEM_CLASS_ID = 268,
	OMCI_GEM_ATTR_PORT_ID = 1,
	OMCI_GEM_ATTR_TCONT = 2,
	OMCI_GEM_ATTR_DIRECTION = 3,

	OMCI_GEM_TCONT_CLASS_ID = 262,
};

/**
 * omci_gem_program() - Program the PON MAC with every GEM port of the MIB.
 * @mib: the MIB
 * @refused: the number of instances left out; may be NULL
 *
 * An instance is left out when its port identifier does not fit twelve
 * bits, its direction is not one the standard defines, it carries traffic
 * upstream without pointing at a T-CONT of the ONU, or another instance
 * has the same port identifier.
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
 * omci_gem_last() - The ports of the last programming.
 * @out: the ports
 *
 * Return: their number.
 */
size_t omci_gem_last(const struct omci_hw_gem_port **out);

#endif
