/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* What the board's kernel backend offers beyond its operations table. */

#ifndef OMCID_TARGET_HW_KERNEL_H
#define OMCID_TARGET_HW_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * omci_hw_kernel_router() - route instead of bridging
 * @on: true when the stick routes the subscriber's traffic itself
 *
 * In router mode the bridge and the VLAN rules that the OLT provisions are
 * answered as usual but are not applied to the hardware: the GEM interfaces
 * stay out of any bridge, and the router of the stick uses them. It is also
 * the backend's set_router, which the daemon calls on SIGUSR1 and SIGUSR2.
 */
void omci_hw_kernel_router(bool on);

/**
 * omci_hw_kernel_notify() - a program to tell of each change published
 * @program: its path, or NULL for none
 *
 * After each change of what the backend publishes in /run (the VLANs of the
 * GEM ports, the OLT's translations) and each GEM interface made or taken
 * away, the backend runs @program with the argument "omcid", and does not
 * wait for it.
 */
void omci_hw_kernel_notify(const char *program);

/**
 * omci_hw_kernel_stats_bytes() - the bytes an action has counted
 * @stats: the attributes of its TCA_ACT_STATS
 * @len: their length
 * @bytes: the bytes
 *
 * The kernel keeps the count of the frames that went through an action in
 * software (TCA_STATS_BASIC) apart from the count the switch reports of the
 * frames it handled itself (TCA_STATS_BASIC_HW). A rule offloaded to the
 * switch has nearly all of its frames in the second, which a software
 * count alone shows as a thousandth of the traffic; the sum is what has
 * passed, and goes on rising with either.
 *
 * Return: 0, or -EPROTO when there is no software count to read.
 */
int omci_hw_kernel_stats_bytes(const uint8_t *stats, size_t len,
			       uint64_t *bytes);

#endif
