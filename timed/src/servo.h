/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The servo: what to do with the clock for each offset a source gives. A
 * clock far out is stepped; a near one is steered by its frequency alone, by
 * a proportional-integral loop, as ptp4l and phc2sys of linuxptp steer
 * theirs. The kernel's own loop is not used: with the gains it takes it
 * could not follow the few parts per million the stick's crystal is off.
 *
 * Two samples some time apart after the start (4 s for PTP, whose samples
 * are filtered; 64 s for NTP, whose are noisier) give the rate at which the
 * clock drifts, which is taken whole, and the clock is stepped by what it is
 * out then, when that is more than 20 us, so that the integral does not have
 * to take it and give it back; from then on the loop corrects what is left.
 * A rate more than 300 ppm from the one in force is no crystal's: the
 * estimate starts again. After a step or a change of source the rate is
 * kept, as neither changes the crystal, and so is a rate the kernel held at
 * the start, which a timed before this one set: the first sample is taken at
 * once, stepping the clock by what it is out when that is more than 20 us. The gains follow the time between two samples: a
 * second for PTP, 2 to 64 s for NTP. Offsets are the local clock less the
 * reference, in nanoseconds; the frequency is in parts per billion, positive
 * to slow the clock down.
 */

#ifndef TIMED_SERVO_H
#define TIMED_SERVO_H

#include "timed.h"

/* The most the kernel takes: 500 ppm. */
#define SERVO_MAX_PPB 500000.0

enum servo_action {
	SERVO_HOLD,	/* keep the frequency: a sample before the rate is known, or one at the same time */
	SERVO_STEP,	/* step the clock by the offset, then keep the frequency */
	SERVO_JUMP,	/* the rate was just found: step by the offset and set the frequency */
	SERVO_FREQ,	/* set the frequency */
};

struct servo {
	ns_t step_limit;	/* an offset beyond it is stepped */
	unsigned int count;	/* samples since the start or the last step, up to 2 */
	bool rate_known;	/* the drift was estimated, or taken over */
	ns_t baseline;		/* the shortest time the rate is estimated over */
	ns_t first_offset;
	ns_t first_at;
	ns_t last_at;
	double drift;		/* the frequency that holds the clock's rate, ppb */
	double freq;		/* the frequency last given, ppb */
};

/*
 * The median of the last three offsets of a source that gives one a second
 * (PTP): one sample a software stamp put far out, as a sender that stalled
 * for a moment does, is not taken. Fewer than three give the last one.
 */
struct median3 {
	ns_t v[3];
	unsigned int n;
};

void median3_reset(struct median3 *m);
ns_t median3_add(struct median3 *m, ns_t v);

/*
 * A servo whose clock runs with @freq_ppb now (what the kernel holds at the
 * start); with @known, that rate is right and is not estimated again.
 */
void servo_init(struct servo *s, ns_t step_limit, double freq_ppb, bool known);

/* The time over which the next first estimate of the rate is made. */
#define SERVO_BASELINE_PTP (4 * NS_PER_SEC)
#define SERVO_BASELINE_NTP (64 * NS_PER_SEC)

/* The clock was stepped, or the source changed: lock again, keeping the drift. */
void servo_reset(struct servo *s);

/*
 * A sample: the offset @offset at monotonic time @at. Return what to do; with
 * SERVO_FREQ, *@freq is the frequency to set.
 */
enum servo_action servo_sample(struct servo *s, ns_t offset, ns_t at, double *freq);

#endif
