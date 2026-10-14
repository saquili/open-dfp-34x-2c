/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The OLT's seconds. The switch's driver reports the start of each second of
 * the OLT's downstream frames, 8000 frames of 125 us, as an external timestamp
 * of its PHC, channel 0: the PHC's time when the second began, as near as an
 * interrupt reads it. The OLT keeps its frames at its clock's rate, so the
 * PHC can follow that rate: its offset from the OLT is its time at a second
 * less the time a whole number of seconds after the second it counts from,
 * its anchor. The time of day the OLT may give (ITU-T G.984.3) sets the PHC in
 * hardware, and a jump that large starts the count again from the next second.
 */

#ifndef TIMED_PON_H
#define TIMED_PON_H

#include "timed.h"

/* An offset beyond this is the PHC set anew, not its rate: the count starts again. */
#define PON_JUMP NS_PER_MS

struct pon {
	bool anchored;
	ns_t anchor;	/* the PHC's time at the second counted from */
};

enum pon_second {
	PON_ANCHOR,	/* the first second: counted from, no offset yet */
	PON_JUMPED,	/* the PHC jumped: counted from now on */
	PON_OFFSET,	/* an offset of the PHC from the OLT's seconds */
};

/*
 * The time of day the OLT gave (ITU-T G.984.3 Amendment 2 clause 10.4.6),
 * which omcid tells once the kernel took it, as KEY=VALUE lines:
 *	superframe=N		the frame at which the PHC takes the time
 *	sec=S, nsec=NS		TstampN, on the PTP timescale (TAI)
 *	monotonic_ns=NS		CLOCK_MONOTONIC when the kernel took it
 */
struct pon_tod {
	uint32_t superframe;
	ns_t tstamp;
	ns_t at;
};

/* Read the file's @len bytes of @text. False when a line is missing or not a number. */
bool pon_tod_parse(const char *text, size_t len, struct pon_tod *t);

/* Forget the anchor: the next second is counted from. */
void pon_reset(struct pon *p);

/* A second that began at the PHC's time @t; with PON_OFFSET, its offset in @offset. */
enum pon_second pon_second(struct pon *p, ns_t t, ns_t *offset);

#endif
