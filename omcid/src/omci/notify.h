/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Notifications the ONU sends on its own. */

#ifndef OMCI_NOTIFY_H
#define OMCI_NOTIFY_H

#include <stddef.h>
#include <stdint.h>

#include "omci/frame.h"

/**
 * omci_notify_build_avc() - Build an attribute value change notification.
 * @class_id: the class
 * @instance: the instance
 * @attr: the attribute that changed
 * @value: its new value
 * @len: the size of @value
 * @out: the frame (G.988 A.3.20)
 */
void omci_notify_build_avc(uint16_t class_id, uint16_t instance, uint8_t attr,
			   const void *value, size_t len,
			   uint8_t out[static OMCI_FRAME_LEN]);

#endif
