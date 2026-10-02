/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Attribute masks: sixteen bits, one per attribute, attribute 1 in the
 * most significant bit. A class has at most sixteen attributes.
 */

#ifndef OMCI_ATTRMASK_H
#define OMCI_ATTRMASK_H

#include <stdbool.h>
#include <stdint.h>

#define OMCI_ATTR_MAX 16u

/* The mask bit of attribute @attr, or 0 for a number outside 1 to 16. */
static inline uint16_t omci_attr_bit(uint8_t attr)
{
	if (attr == 0 || attr > OMCI_ATTR_MAX)
		return 0;
	return (uint16_t)(0x8000u >> (attr - 1u));
}

/* True when @mask names attribute @attr. */
static inline bool omci_attr_in_mask(uint16_t mask, uint8_t attr)
{
	const uint16_t bit = omci_attr_bit(attr);

	return bit != 0 && (mask & bit) != 0;
}

#endif
