/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The time of day the OLT gives with Synchronize time, kept as an offset
 * from the monotonic clock.
 */

#ifndef OMCI_CLOCK_H
#define OMCI_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "omcid/attrs.h"

/**
 * struct omci_clock - The time of day, once the OLT has set it.
 * @wall_at_sync: seconds since 1970-01-01 UTC at the last synchronisation
 * @mono_at_sync: the monotonic time of the last synchronisation
 * @set: false until the first valid synchronisation
 */
struct omci_clock {
	uint64_t wall_at_sync;
	uint64_t mono_at_sync;
	bool set;
};

/**
 * omci_clock_sync() - Set the time of day.
 * @c: the clock
 * @year: the year, 1970 to 2200
 * @month: 1 to 12
 * @day: 1 to the length of the month
 * @hour: 0 to 23
 * @minute: 0 to 59
 * @second: 0 to 60, a leap second included
 * @mono_now: the monotonic time now, in seconds
 *
 * Return: false, with the clock untouched, for a date that does not exist.
 */
bool omci_clock_sync(struct omci_clock *c, uint16_t year, uint8_t month,
		     uint8_t day, uint8_t hour, uint8_t minute, uint8_t second,
		     uint64_t mono_now);

/**
 * omci_clock_is_set() - Whether the OLT has set the time of day.
 * @c: the clock
 *
 * Return: true after a valid synchronisation.
 */
__must_check bool omci_clock_is_set(const struct omci_clock *c);

/**
 * omci_clock_now() - The time of day.
 * @c: the clock
 * @mono_now: the monotonic time now, in seconds
 *
 * Return: seconds since 1970-01-01 UTC, or 0 while the clock is not set.
 */
__must_check uint64_t omci_clock_now(const struct omci_clock *c,
				     uint64_t mono_now);

/**
 * omci_clock_forget() - Unset the clock, as a MIB reset does.
 * @c: the clock, or NULL
 */
void omci_clock_forget(struct omci_clock *c);

/**
 * omci_clock_days_from_civil() - Count days since 1970-01-01.
 * @year: the year of the proleptic Gregorian calendar
 * @month: 1 to 12
 * @day: 1 to 31
 *
 * Return: the number of days, negative before 1970.
 */
__must_check int64_t omci_clock_days_from_civil(int64_t year,
						unsigned int month,
						unsigned int day);

/**
 * struct omci_civil - A date and a time of day, UTC.
 * @year: the year
 * @month: 1 to 12
 * @day: 1 to 31
 * @hour: 0 to 23
 * @minute: 0 to 59
 * @second: 0 to 59
 */
struct omci_civil {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
};

/**
 * omci_clock_civil() - Take seconds since 1970-01-01 UTC apart.
 * @secs: the seconds, up to the year 2200 that the clock keeps
 * @out: the date and the time of day
 */
void omci_clock_civil(uint64_t secs, struct omci_civil *out);

#endif
