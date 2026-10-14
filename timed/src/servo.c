// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The proportional-integral servo. For an interval of t seconds between two
 * samples the gains are near those of linuxptp for software time stamps
 * (0.1 t^-0.3 and 0.001 t^0.4), in a form that needs no maths library:
 *
 *	kp = min(0.1, 0.7 / t)
 *	ki = min(0.001 (1 + t / 8), 0.3 / t)
 *	freq = kp * offset + drift + ki * offset,  drift += ki * offset
 *
 * so that one sample never corrects more than 70 % of the offset over its
 * interval, and the integral takes the rate the first estimate missed. The
 * first estimate of the rate takes two samples at least 4 s apart for PTP:
 * with software stamps off by some 10 us, a second apart would leave it some
 * 10 ppm out; and 64 s apart for NTP, whose offsets are some 100 us apart
 * from one answer to the next.
 */

#include "servo.h"

#define KP_MAX 0.1
#define KP_NORM_MAX 0.7
#define KI_SCALE 0.001
#define KI_NORM_MAX 0.3

/* An estimate this far from the rate in force is no crystal's. */
#define SANE_PPB 300000.0

/* Out by more than this when the rate is found, the clock is stepped (linuxptp's first_step_threshold). */
#define FIRST_STEP (20 * NS_PER_US)

/* The interval the gains are taken for, in seconds: no shorter than PTP's fastest, no longer than NTP's slowest. */
#define INTERVAL_MIN 0.0625
#define INTERVAL_MAX 1024.0

static double min2(double a, double b)
{
	return a < b ? a : b;
}

static double clamp_ppb(double v)
{
	return v > SERVO_MAX_PPB ? SERVO_MAX_PPB : v < -SERVO_MAX_PPB ? -SERVO_MAX_PPB : v;
}

void median3_reset(struct median3 *m)
{
	m->n = 0;
}

ns_t median3_add(struct median3 *m, ns_t v)
{
	ns_t a, b, c;

	m->v[m->n % 3] = v;
	m->n++;
	if (m->n < 3)
		return v;
	a = m->v[0];
	b = m->v[1];
	c = m->v[2];
	if ((a <= b && b <= c) || (c <= b && b <= a))
		return b;
	if ((b <= a && a <= c) || (c <= a && a <= b))
		return a;
	return c;
}

void servo_init(struct servo *s, ns_t step_limit, double freq_ppb, bool known)
{
	s->step_limit = step_limit;
	s->count = 0;
	s->rate_known = known;
	s->baseline = SERVO_BASELINE_PTP;
	s->first_offset = 0;
	s->first_at = 0;
	s->last_at = 0;
	s->drift = clamp_ppb(freq_ppb);
	s->freq = s->drift;
}

void servo_reset(struct servo *s)
{
	s->count = 0;
}

enum servo_action servo_sample(struct servo *s, ns_t offset, ns_t at, double *freq)
{
	double t, kp, ki, term, f;

	if (offset > s->step_limit || offset < -s->step_limit) {
		s->count = 0;
		return SERVO_STEP;
	}
	switch (s->count) {
	case 0:
		if (s->rate_known) {
			/* The rate is known: lock again on the phase alone. */
			s->count = 2;
			s->last_at = at;
			s->freq = s->drift;
			*freq = s->freq;
			return offset > FIRST_STEP || offset < -FIRST_STEP ? SERVO_JUMP : SERVO_FREQ;
		}
		s->first_offset = offset;
		s->first_at = at;
		s->count = 1;
		return SERVO_HOLD;
	case 1:
		if (at - s->first_at < s->baseline)
			return SERVO_HOLD;
		/* The clock gained (offset - first) in the time between, at the frequency given. */
		f = (double)(offset - s->first_offset) * 1e9 / (double)(at - s->first_at);
		if (f > SANE_PPB || f < -SANE_PPB) {
			s->first_offset = offset;
			s->first_at = at;
			return SERVO_HOLD;
		}
		s->drift = clamp_ppb(s->freq + f);
		s->count = 2;
		s->rate_known = true;
		s->last_at = at;
		s->freq = s->drift;
		*freq = s->freq;
		return offset > FIRST_STEP || offset < -FIRST_STEP ? SERVO_JUMP : SERVO_FREQ;
	default:
		if (at <= s->last_at)
			return SERVO_HOLD;
		t = (double)(at - s->last_at) / 1e9;
		t = t < INTERVAL_MIN ? INTERVAL_MIN : t > INTERVAL_MAX ? INTERVAL_MAX : t;
		kp = min2(KP_MAX, KP_NORM_MAX / t);
		ki = min2(KI_SCALE * (1.0 + t / 8.0), KI_NORM_MAX / t);
		term = ki * (double)offset;
		f = kp * (double)offset + s->drift + term;
		/* At the limit the integral stops: it would only wind up. */
		if (f > -SERVO_MAX_PPB && f < SERVO_MAX_PPB)
			s->drift += term;
		s->last_at = at;
		s->freq = clamp_ppb(f);
		*freq = s->freq;
		return SERVO_FREQ;
	}
}
