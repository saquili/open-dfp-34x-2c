// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The values an attribute takes, where it does not take them all, and the
 * attributes the ONU changes on its own.
 */

#include <stdbool.h>
#include <stddef.h>

#include "me/values.h"
#include "omcid/array_size.h"

/*
 * The attributes whose first byte has a range: of the ANI-G (G.988 9.2.1),
 * the signal fail threshold, 3 to 8, the signal degrade threshold, 4 to 10,
 * and alarm-reporting control, 0 or 1 (A.1.4.3); of the multicast operations
 * profile (9.3.27), the IGMP function, 0 to 2, immediate leave, 0 or 1, the
 * upstream IGMP tag control, 0 to 3, the unauthorized join request
 * behaviour, 0 or 1, and the control type of the downstream IGMP and
 * multicast TCI, 0 to 7; of the multicast subscriber config info (9.3.28),
 * the ME type, 0 or 1, and bandwidth enforcement, 0 or 1; of the multicast
 * subscriber monitor (9.3.29), the ME type.
 */
static const struct {
	uint16_t class_id;
	uint8_t attr_no;
	uint8_t min;
	uint8_t max;
} g_ranges[] = {
	{ 263u, 6u, 3u, 8u },  { 263u, 7u, 4u, 10u }, { 263u, 8u, 0u, 1u },
	{ 309u, 2u, 0u, 2u },  { 309u, 3u, 0u, 1u },  { 309u, 5u, 0u, 3u },
	{ 309u, 15u, 0u, 1u }, { 309u, 16u, 0u, 7u }, { 310u, 1u, 0u, 1u },
	{ 310u, 5u, 0u, 1u },  { 311u, 1u, 0u, 1u },
};

/*
 * The IGMP version of a multicast operations profile: IGMP 1, 2 and 3, MLD
 * 1 and 2 (16 and 17); the ONU refuses the others (G.988 9.3.27).
 */
static const uint8_t g_igmp_versions[] = { 1u, 2u, 3u, 16u, 17u };

bool omci_me_value_allowed(uint16_t class_id, uint8_t attr_no,
			   const uint8_t *value)
{
	if (class_id == 309u && attr_no == 1u) {
		for (size_t i = 0; i < ARRAY_SIZE(g_igmp_versions); i++)
			if (value[0] == g_igmp_versions[i])
				return true;
		return false;
	}
	for (size_t i = 0; i < ARRAY_SIZE(g_ranges); i++)
		if (g_ranges[i].class_id == class_id &&
		    g_ranges[i].attr_no == attr_no)
			return value[0] >= g_ranges[i].min &&
			       value[0] <= g_ranges[i].max;
	return true;
}

bool omci_me_value_kept(uint16_t class_id, uint8_t attr_no)
{
	return !(class_id == 263u && attr_no == 8u);
}
