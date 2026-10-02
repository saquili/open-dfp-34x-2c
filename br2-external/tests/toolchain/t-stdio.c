/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Formatted output and input: integers of every size, strings, soft-float. */
#include <stdio.h>
#include <string.h>
#include "check.h"

int main(void)
{
	char buf[64];
	int n;
	int a, b;
	char word[16];
	double d;
	unsigned int hex;

	printf("%d %i %u %x %X %o %c %s\n", -42, 42, 4000000000u, 255, 255, 8, 'z', "text");
	printf("[%5d] [%-5d] [%05d] [%+d] [%%]\n", 42, 42, 42, 42);
	printf("%ld %lld %llu %hd %hhd\n", -2000000000L, -9000000000000000000LL,
	       18000000000000000000ULL, (short)-3, (signed char)-4);
	printf("%.3f %.2e %g %g\n", 3.14159265, 12345.678, 0.0001, 1234567.0);
	printf("%10.4f|%-10.1f|\n", 2.5, -0.26);

	n = snprintf(buf, sizeof(buf), "%s-%d", "abc", 123);
	CHECK(n == 7 && strcmp(buf, "abc-123") == 0);
	{
		const char *volatile text = "truncated";

		n = snprintf(buf, 5, "%s", text);
		CHECK(n == 9 && strcmp(buf, "trun") == 0);
	}

	n = sscanf("12 0x1f hello 3.5", "%d %x %15s %lf", &a, &hex, word, &d);
	CHECK(n == 4 && a == 12 && hex == 31 && strcmp(word, "hello") == 0 && d == 3.5);
	n = sscanf("77/-8", "%d/%d", &a, &b);
	CHECK(n == 2 && a == 77 && b == -8);

	puts("puts line");
	fputs("fputs line\n", stdout);
	putchar('p');
	putchar('\n');
	fprintf(stdout, "%s\n", "fprintf line");
	fflush(stdout);
	DONE("stdio");
}
