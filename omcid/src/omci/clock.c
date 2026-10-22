// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The time of day from Synchronize time (G.988 A.3.33). */

#include <stdbool.h>
#include <stddef.h>

#include "omci/clock.h"

static const uint8_t g_month_days[12] = { 31, 28, 31, 30, 31, 30,
					  31, 31, 30, 31, 30, 31 };

static bool is_leap(uint16_t y)
{
	return (y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u;
}

/*
 * Days since 1970-01-01 in whole-number arithmetic: the year is counted from
 * March, so the leap day falls at its end, and in eras of 400 years.
 */
int64_t omci_clock_days_from_civil(int64_t year, unsigned int month,
				   unsigned int day)
{
	year -= month <= 2u ? 1 : 0;

	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const uint64_t yoe = (uint64_t)(year - era * 400);
	const uint64_t doy =
		(153u * (month + (month > 2u ? -3u : 9u)) + 2u) / 5u + day - 1u;
	const uint64_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

	return era * 146097 + (int64_t)doe - 719468;
}

/* The inverse of omci_clock_days_from_civil(), in the same eras of 400 years. */
void omci_clock_civil(uint64_t secs, struct omci_civil *out)
{
	const int64_t z = (int64_t)(secs / 86400u) + 719468;
	const int64_t era = z / 146097;
	const uint64_t doe = (uint64_t)(z - era * 146097);
	const uint64_t yoe =
		(doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	const uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
	const uint64_t mp = (5u * doy + 2u) / 153u;
	const uint64_t month = mp < 10u ? mp + 3u : mp - 9u;
	const uint32_t rest = (uint32_t)(secs % 86400u);

	out->year =
		(uint16_t)((int64_t)yoe + era * 400 + (month <= 2u ? 1 : 0));
	out->month = (uint8_t)month;
	out->day = (uint8_t)(doy - (153u * mp + 2u) / 5u + 1u);
	out->hour = (uint8_t)(rest / 3600u);
	out->minute = (uint8_t)(rest % 3600u / 60u);
	out->second = (uint8_t)(rest % 60u);
}

/* True for a date and time that exist, within the range the clock keeps. */
static bool stamp_is_real(uint16_t year, uint8_t month, uint8_t day,
			  uint8_t hour, uint8_t minute, uint8_t second)
{
	if (year < 1970u || year > 2200u)
		return false;
	if (month < 1u || month > 12u)
		return false;

	unsigned int limit = g_month_days[month - 1u];

	if (month == 2u && is_leap(year))
		limit = 29u;
	if (day < 1u || day > limit)
		return false;

	if (hour > 23u || minute > 59u)
		return false;

	return second <= 60u;
}

bool omci_clock_sync(struct omci_clock *c, uint16_t year, uint8_t month,
		     uint8_t day, uint8_t hour, uint8_t minute, uint8_t second,
		     uint64_t mono_now)
{
	if (c == NULL || !stamp_is_real(year, month, day, hour, minute, second))
		return false;

	const int64_t days =
		omci_clock_days_from_civil((int64_t)year, month, day);

	c->wall_at_sync = (uint64_t)days * 86400u + (uint64_t)hour * 3600u +
			  (uint64_t)minute * 60u + second;
	c->mono_at_sync = mono_now;
	c->set = true;
	return true;
}

bool omci_clock_is_set(const struct omci_clock *c)
{
	return c != NULL && c->set;
}

uint64_t omci_clock_now(const struct omci_clock *c, uint64_t mono_now)
{
	if (c == NULL || !c->set)
		return 0;

	if (mono_now < c->mono_at_sync)
		return c->wall_at_sync;

	return c->wall_at_sync + (mono_now - c->mono_at_sync);
}

void omci_clock_forget(struct omci_clock *c)
{
	if (c == NULL)
		return;
	c->wall_at_sync = 0;
	c->mono_at_sync = 0;
	c->set = false;
}
