/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Functions registered with atexit run, last registered first, when the program
 * returns from main or calls exit. The program finishes by returning from main,
 * so a program that stops here after "ok atexit" has hung in its exit handling.
 */
#include <stdio.h>
#include <stdlib.h>

static void first(void)
{
	puts("handler 1");
}

static void second(void)
{
	puts("handler 2");
}

static void third(void)
{
	puts("handler 3");
	fflush(stdout);
}

int main(void)
{
	if (atexit(first) || atexit(second) || atexit(third)) {
		puts("FAILED atexit: cannot register");
		return 1;
	}
	puts("ok atexit");
	return 0;
}
