/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Floating point on a core with no floating-point unit: arithmetic, comparison
 * and conversion of float and double (from the compiler's soft-float routines),
 * and the maths library. The constants were computed outside the program.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "check.h"

static int near(double a, double b)
{
	double d = a - b;

	if (d < 0)
		d = -d;
	return d <= 1e-9 * (b < 0 ? -b : b) + 1e-12;
}

int main(void)
{
	volatile double a = 1.5, b = -0.1, big = 1e300;
	volatile float f = 3.25f, g = 0.75f;
	volatile int i = -7;
	volatile unsigned int u = 4000000000u;
	volatile long long ll = -5000000000LL;
	volatile double inf = 1e308;

	CHECK(a + b == 1.4);
	CHECK(near(a * b, -0.15) && near(a / 3.0, 0.5) && near(a - b, 1.6));
	CHECK(f + g == 4.0f && f * g == 2.4375f);
	CHECK(near((double)(f / g), 13.0 / 3.0) || (double)(f / g) - 13.0 / 3.0 < 1e-6);
	CHECK(a > b && b < 0 && a >= 1.5 && a != b && !(a < b));
	CHECK((int)(a * 3) == 4 && (int)(-a * 3) == -4 && (int)i == -7);
	CHECK((double)u == 4000000000.0 && (double)ll == -5000000000.0);
	CHECK((long long)(big / 1e290) == 10000000000LL);
	CHECK((float)a == 1.5f && (double)f == 3.25);
	CHECK(inf * 10.0 > inf && -(inf * 10.0) < -inf);	/* overflow to infinity */
	CHECK(!(0.0 / (double)(a - a) == 0.0 / (double)(a - a)));	/* NaN is not equal to itself */

	CHECK(near(sqrt(2.0), 1.4142135623730951));
	CHECK(near(sin(1.0), 0.8414709848078965) && near(cos(1.0), 0.5403023058681398));
	CHECK(near(exp(1.0), 2.718281828459045) && near(log(10.0), 2.302585092994046));
	CHECK(near(pow(2.0, 0.5), 1.4142135623730951) && near(pow(10.0, 3.0), 1000.0));
	CHECK(floor(-2.5) == -3.0 && ceil(-2.5) == -2.0 && fabs(-4.25) == 4.25);
	CHECK(near(fmod(10.5, 3.0), 1.5) && near(atan2(1.0, 1.0), 0.7853981633974483));
	printf("%.6f %.6f\n", sqrt(2.0), exp(1.0));
	DONE("float");
}
