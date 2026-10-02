/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Which message types the daemon executes, and which ones each class accepts. */

#ifndef OMCI_ME_ACTION_H
#define OMCI_ME_ACTION_H

#include <stdbool.h>
#include <stdint.h>

#include "omcid/attrs.h"

/*
 * The message types the engine executes: Create, Delete, Set, Get, Get all
 * alarms and its next, MIB upload and its next, MIB reset, Test, Synchronize
 * time, Get next and Get current data.
 */
#define OMCI_ME_MSG_IMPLEMENTED                                           \
	((1u << 4) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 11) |     \
	 (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 18) | \
	 (1u << 24) | (1u << 26) | (1u << 28))

/**
 * omci_me_msg_implemented() - Whether the engine executes a message type.
 * @msg_type: the type
 *
 * Return: true when it does.
 */
__must_check bool omci_me_msg_implemented(uint8_t msg_type);

/**
 * omci_me_action_mask() - The message types a class accepts.
 * @class_id: the class
 *
 * Return: bit N set for message type N; 0 for a class the device lacks.
 */
__must_check uint32_t omci_me_action_mask(uint16_t class_id);

/**
 * omci_me_action_allowed() - Whether a class accepts a message type.
 * @class_id: the class
 * @msg_type: the type
 *
 * A class the device lacks passes, so that the request is answered as one
 * for an unknown entity rather than as an unsupported action.
 *
 * Return: true when the class accepts the type, or is unknown.
 */
__must_check bool omci_me_action_allowed(uint16_t class_id, uint8_t msg_type);

#endif
