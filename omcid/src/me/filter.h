/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The filters of the bridge ports (G.988 9.3.6 and 9.3.7) and the PPPoE
 * filter of the Ethernet UNI (9.5.1), as rules of the switch
 * (struct omci_hw_filter).
 *
 * A bridge port's filter acts on the frames that leave the bridge through the
 * port: on a port of the PON side on those that go upstream, on a port of the
 * UNI side on those that go downstream. The switch judges a frame where it
 * comes in, so the filters of every port of one side are one set of rules: the
 * PON side's for the frames that come in on the UNI, the UNI side's for those
 * that come in on the PON.
 *
 * Each group that the filter preassign table filters drops the frames to the
 * addresses G.988 lists for it, with the EtherType where it names one; the
 * AppleTalk range from 09:00:07:00:00:00 to 09:00:07:00:00:fc is taken with
 * the whole of its last octet. An entry of the MAC filter table that filters
 * drops the frames from or to its address. An entry that forwards admits only
 * the addresses that the port's entries forward, and the PPPoE filter only the
 * two EtherTypes of PPPoE: rules that pass them, then rules that drop every
 * other frame. Only upstream: rules that drop every frame that comes in on the
 * PON would take the OMCI channel too, so an admitting filter of a port of the
 * UNI side is kept and not applied.
 */

#ifndef OMCI_ME_FILTER_H
#define OMCI_ME_FILTER_H

#include <stddef.h>

#include "hw/hw.h"
#include "mib/store.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/**
 * omci_filter_rules() - The rules that the MIB's filters ask for.
 * @mib: the MIB
 * @out: the rules, in order: those that drop, those that pass, and those that
 *	drop what the passes leave
 * @cap: room in @out
 *
 * A rule that two filters ask for is written once.
 *
 * Return: how many rules the filters ask for; more than @cap when they do not
 * fit, of which the first @cap are written.
 */
__must_check size_t omci_filter_rules(const struct omci_mib *mib,
				      struct omci_hw_filter *out, size_t cap);

/**
 * omci_filter_apply() - Give the backend the rules of the MIB's filters,
 *	when they changed.
 * @mib: the MIB
 *
 * Rules beyond %OMCI_HW_FILTER_MAX are left out, and a backend without
 * filters takes none; either is said once.
 *
 * Return: %OMCI_OK, or the failure of the backend, which is given the rules
 * again at the next call.
 */
enum omci_status omci_filter_apply(const struct omci_mib *mib);

/**
 * omci_filter_forget() - Forget the rules last given, so that the next
 *	omci_filter_apply() gives them again.
 */
void omci_filter_forget(void);

#endif
