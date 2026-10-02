/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The instances the ONU makes itself, at start and after a MIB reset. */

#ifndef OMCI_ME_DEFAULTS_H
#define OMCI_ME_DEFAULTS_H

#include <stdint.h>

#include "hw/hw.h"
#include "mib/store.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/**
 * omci_me_defaults_apply() - Rebuild the MIB from the ONU's own instances.
 * @mib: the MIB; it is emptied first
 * @caps: what the hardware has
 *
 * Return: %OMCI_OK, or the first failure; the instances made before it stay.
 */
__must_check enum omci_status
omci_me_defaults_apply(struct omci_mib *mib, const struct omci_hw_caps *caps);

#endif
