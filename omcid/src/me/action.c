// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The message types of action.h. */

#include <stdbool.h>

#include "me/action.h"

#include "me/table.h"

bool omci_me_msg_implemented(uint8_t msg_type)
{
	return msg_type < 32u &&
	       (OMCI_ME_MSG_IMPLEMENTED & (1u << msg_type)) != 0u;
}

uint32_t omci_me_action_mask(uint16_t class_id)
{
	const struct omci_me_class *cls = omci_me_class_by_id(class_id);

	return cls != NULL ? cls->actions : 0u;
}

bool omci_me_action_allowed(uint16_t class_id, uint8_t msg_type)
{
	if (omci_me_class_by_id(class_id) == NULL)
		return true;

	if (msg_type >= 32u)
		return false;

	return (omci_me_action_mask(class_id) & (1u << msg_type)) != 0u;
}
