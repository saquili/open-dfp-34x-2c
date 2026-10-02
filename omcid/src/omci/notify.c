// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The attribute value change notification: transaction identifier 0, no
 * answer asked, the attribute mask, then the new value (G.988 A.3.20).
 */

#include <stdbool.h>

#include "omci/attrmask.h"
#include "omci/frame.h"
#include "omci/notify.h"
#include "sys/wire.h"

#define OMCI_MT_AVC 17u

void omci_notify_build_avc(uint16_t class_id, uint16_t instance, uint8_t attr,
			   const void *value, size_t len,
			   uint8_t out[static OMCI_FRAME_LEN])
{
	struct omci_frame f = {
		.tci = 0,
		.priority = false,
		.msg_type = OMCI_MT_AVC,
		.ar = false,
		.ak = false,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = class_id,
		.instance = instance,
	};

	const uint16_t mask = omci_attr_bit(attr);

	f.contents[0] = (uint8_t)(mask >> 8);
	f.contents[1] = (uint8_t)(mask & 0xffu);

	if (len > 0)
		omci_wire_copy(&f.contents[2], value, len);

	(void)!omci_frame_build(&f, out);
}
