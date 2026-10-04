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
 * The one-byte attributes with a range: of the ANI-G (G.988 9.2.1), the
 * signal fail threshold, 3 to 8, the signal degrade threshold, 4 to 10,
 * and alarm-reporting control, 0 or 1 (A.1.4.3).
 */
static const struct {
	uint16_t class_id;
	uint8_t attr_no;
	uint8_t min;
	uint8_t max;
} g_ranges[] = {
	{ 263u, 6u, 3u, 8u },
	{ 263u, 7u, 4u, 10u },
	{ 263u, 8u, 0u, 1u },
};

bool omci_me_value_allowed(uint16_t class_id, uint8_t attr_no,
			   const uint8_t *value)
{
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
