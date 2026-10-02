/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* qsort and bsearch on integers, strings and structures. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

#define COUNT 2000

struct rec {
	int key;
	int tag;
};

static int by_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return (x > y) - (x < y);
}

static int by_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int by_key(const void *a, const void *b)
{
	const struct rec *x = a, *y = b;

	return (x->key > y->key) - (x->key < y->key);
}

int main(void)
{
	static int v[COUNT];
	static struct rec r[COUNT];
	const char *names[] = { "mike", "alpha", "zulu", "bravo", "yankee", "charlie", "x-ray" };
	const char *sorted[] = { "alpha", "bravo", "charlie", "mike", "x-ray", "yankee", "zulu" };
	uint32_t seed = 12345;
	uint32_t sum = 0, weighted = 0;
	int i, key;
	int *hit;

	for (i = 0; i < COUNT; i++) {			/* a fixed linear congruential sequence */
		seed = seed * 1103515245u + 12345u;
		v[i] = (int)((seed >> 8) % 100000) - 50000;
		r[i].key = v[i] % 50;
		r[i].tag = i;
		sum += (uint32_t)v[i];
	}
	qsort(v, COUNT, sizeof(v[0]), by_int);
	for (i = 1; i < COUNT; i++)
		CHECK(v[i - 1] <= v[i]);
	for (i = 0; i < COUNT; i++) {
		sum -= (uint32_t)v[i];			/* the same numbers came out */
		weighted += (uint32_t)v[i] * (uint32_t)(i + 1);
	}
	CHECK(sum == 0);
	printf("weighted sum %lu\n", (unsigned long)weighted);

	key = v[COUNT / 2];
	hit = bsearch(&key, v, COUNT, sizeof(v[0]), by_int);
	CHECK(hit != NULL && *hit == key);
	key = 100001;
	CHECK(bsearch(&key, v, COUNT, sizeof(v[0]), by_int) == NULL);

	qsort(names, 7, sizeof(names[0]), by_str);
	for (i = 0; i < 7; i++)
		CHECK(strcmp(names[i], sorted[i]) == 0);

	qsort(r, COUNT, sizeof(r[0]), by_key);
	for (i = 1; i < COUNT; i++)
		CHECK(r[i - 1].key <= r[i].key);
	DONE("qsort");
}
