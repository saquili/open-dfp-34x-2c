/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The system clock against a PTP hardware clock (PHC). The kernel reads the
 * two in turn (PTP_SYS_OFFSET): the system clock, the PHC, the system clock,
 * and so on, ending with the system clock. Each PHC reading lies between the
 * two system readings around it; the pair closest together says best where it
 * lies, at their middle, as phc2sys of linuxptp takes it.
 */

#ifndef TIMED_PHC_H
#define TIMED_PHC_H

#include "timed.h"

/* The PHC readings asked for at a time; the kernel takes up to 25. */
#define PHC_SAMPLES 5

struct phc_reading {
	ns_t sys[PHC_SAMPLES + 1];	/* system clock: before, between and after */
	ns_t phc[PHC_SAMPLES];
	unsigned int n;			/* PHC readings */
};

/*
 * The system clock less the PHC, from the reading between the two system
 * readings closest together, whose distance goes to @window. False when no
 * pair is in order.
 */
bool phc_sys_offset(const struct phc_reading *r, ns_t *offset, ns_t *window);

#endif
