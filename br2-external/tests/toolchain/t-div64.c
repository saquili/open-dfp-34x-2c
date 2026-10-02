/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * 64-bit division and remainder, which the core has no instruction for and which
 * come from the compiler's runtime library, and the 32-bit division and
 * remainder that use the divide unit. The vectors were computed with exact
 * integer arithmetic outside the program (C99 rounding towards zero).
 */
#include <stdint.h>
#include <stdio.h>
#include "check.h"

static const struct { unsigned long long a, b, q, r; } udiv[] = {
	{ 18446744073709551615ULL, 3ULL, 6148914691236517205ULL, 0ULL },
	{ 18446744073709551615ULL, 4294967295ULL, 4294967297ULL, 0ULL },
	{ 1311768467463790320ULL, 4660ULL, 281495379284075ULL, 820ULL },
	{ 9223372036854775808ULL, 7ULL, 1317624576693539401ULL, 1ULL },
	{ 1000000000000000000ULL, 1000000007ULL, 999999993ULL, 49ULL },
	{ 4294967295ULL, 65536ULL, 65535ULL, 65535ULL },
	{ 4294967296ULL, 4294967296ULL, 1ULL, 0ULL },
	{ 18446744073709551615ULL, 18446744073709551615ULL, 1ULL, 0ULL },
	{ 12345ULL, 18446744073709551600ULL, 0ULL, 12345ULL },
	{ 16045690984503111693ULL, 68719476737ULL, 233495534ULL, 67596719135ULL },
	{ 7ULL, 7ULL, 1ULL, 0ULL },
	{ 0ULL, 5ULL, 0ULL, 0ULL },
};

static const struct { long long a, b, q, r; } sdiv[] = {
	{ (-9223372036854775807LL - 1), 3LL, -3074457345618258602LL, -2LL },
	{ (-9223372036854775807LL - 1), -3LL, 3074457345618258602LL, -2LL },
	{ 9223372036854775807LL, -7LL, -1317624576693539401LL, 0LL },
	{ -1000000000000000000LL, 999999937LL, -1000000063LL, -3969LL },
	{ 123456789012345678LL, -12345LL, -10000549940246LL, 8808LL },
	{ -5LL, 2LL, -2LL, -1LL },
	{ 5LL, -2LL, -2LL, 1LL },
	{ -5LL, -2LL, 2LL, -1LL },
	{ (-9223372036854775807LL - 1), 1LL, (-9223372036854775807LL - 1), 0LL },
	{ 1099511640121LL, -1048576LL, -1048576LL, 12345LL },
	{ -8589934592LL, 2147483648LL, -4LL, 0LL },
	{ 0LL, -9LL, 0LL, 0LL },
};

int main(void)
{
	unsigned int i;
	uint64_t x = 0x9E3779B97F4A7C15ULL;
	volatile int vz = 2;
	volatile int32_t m = -2147483647 - 1;
	volatile uint32_t a32 = 4000000000u, b32 = 7;

	for (i = 0; i < sizeof(udiv) / sizeof(udiv[0]); i++) {
		volatile unsigned long long a = udiv[i].a, b = udiv[i].b;

		CHECK(a / b == udiv[i].q);
		CHECK(a % b == udiv[i].r);
	}
	for (i = 0; i < sizeof(sdiv) / sizeof(sdiv[0]); i++) {
		volatile long long a = sdiv[i].a, b = sdiv[i].b;

		CHECK(a / b == sdiv[i].q);
		CHECK(a % b == sdiv[i].r);
	}
	{
		volatile unsigned long long ua = udiv[0].a, ub = udiv[0].b;
		volatile long long sa = sdiv[2].a, sb = sdiv[2].b;

		printf("%llu\n", ua / ub);
		printf("%lld\n", sa / sb);
	}

	/* A quotient and a remainder always add up: 20000 pairs of a sequence. */
	for (i = 0; i < 20000; i++) {
		volatile uint64_t b;
		uint64_t a;

		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		a = x;
		b = (x >> (i % 61)) | 1;
		CHECK((a / b) * b + a % b == a && a % b < b);
	}

	CHECK(a32 / b32 == 571428571u && a32 % b32 == 3u);
	CHECK(100 / vz == 50 && -100 / vz == -50 && -101 / vz == -50 && -101 % vz == -1);
	CHECK(m / 2 == -1073741824 && m % 3 == -2);
	DONE("div64");
}
