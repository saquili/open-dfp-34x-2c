/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* What the board's kernel backend offers beyond its operations table. */

#ifndef OMCID_TARGET_HW_KERNEL_H
#define OMCID_TARGET_HW_KERNEL_H

#include <stdbool.h>

/**
 * omci_hw_kernel_router() - route instead of bridging
 * @on: true when the stick routes the subscriber's traffic itself
 *
 * In router mode the bridge and the VLAN rules that the OLT provisions are
 * answered as usual but are not applied to the hardware: the GEM interfaces
 * stay out of any bridge, and the router of the stick uses them.
 */
void omci_hw_kernel_router(bool on);

#endif
