/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Calendar conversion of fixed instants, and the clocks. */
#include <time.h>
#include <sys/time.h>
#include "check.h"

int main(void)
{
	time_t t = 1700000000;				/* 2023-11-14 22:13:20 UTC, a Tuesday */
	struct tm tm;
	char buf[64];
	struct timespec a, b;
	struct timeval tv;
	long ms;

	CHECK(gmtime_r(&t, &tm) == &tm);
	CHECK(tm.tm_year == 123 && tm.tm_mon == 10 && tm.tm_mday == 14);
	CHECK(tm.tm_hour == 22 && tm.tm_min == 13 && tm.tm_sec == 20);
	CHECK(tm.tm_wday == 2 && tm.tm_yday == 317);
	CHECK(strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a %b %j", &tm) == 31);
	puts(buf);
	CHECK(timegm(&tm) == t);

	t = 2147483647;					/* the last second of 32-bit time */
	CHECK(gmtime_r(&t, &tm) == &tm);
	CHECK(strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) == 19);
	puts(buf);
	t = 0;
	CHECK(gmtime_r(&t, &tm) == &tm && tm.tm_year == 70 && tm.tm_wday == 4);

	/* The wall clock may be at any date (a board has no battery-backed clock), so
	 * only the clocks' agreement is checked. */
	CHECK(gettimeofday(&tv, NULL) == 0);
	t = time(NULL);
	CHECK(t - tv.tv_sec >= -1 && t - tv.tv_sec <= 1);

	CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
	b.tv_sec = 0;
	b.tv_nsec = 30 * 1000 * 1000;
	CHECK(nanosleep(&b, NULL) == 0);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	ms = (long)(b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
	CHECK(ms >= 29 && ms < 2000);
	DONE("time");
}
