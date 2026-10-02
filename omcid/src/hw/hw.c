// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The backend in use: the host backend in a host build, the kernel backend
 * in a build for the board, until another one is chosen.
 */

#include <stddef.h>

#include "hw/hw.h"

#if defined(OMCID_HW_STUB)
#define OMCI_HW_BUILT_IN omci_hw_host_ops
#else
#define OMCI_HW_BUILT_IN omci_hw_kernel_ops
#endif

static const struct omci_hw_ops *g_ops = &OMCI_HW_BUILT_IN;

const struct omci_hw_ops *omci_hw(void)
{
	return g_ops;
}

void omci_hw_ops_set(const struct omci_hw_ops *ops)
{
	g_ops = ops != NULL ? ops : &OMCI_HW_BUILT_IN;
}
