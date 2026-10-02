/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The allocator: many sizes, growth and shrinking, zeroed memory, alignment, a large block. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

#define BLOCKS 400

static void fill(unsigned char *p, size_t n, unsigned char seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(seed + i * 7);
}

static int intact(const unsigned char *p, size_t n, unsigned char seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] != (unsigned char)(seed + i * 7))
			return 0;
	return 1;
}

int main(void)
{
	unsigned char *block[BLOCKS];
	size_t size[BLOCKS];
	unsigned int i;
	unsigned char *p, *q;
	void *aligned = NULL;
	int *z;

	for (i = 0; i < BLOCKS; i++) {
		size[i] = 1 + (i * 37) % 3000;
		block[i] = malloc(size[i]);
		CHECK(block[i] != NULL);
		if (block[i])
			fill(block[i], size[i], (unsigned char)i);
	}
	for (i = 0; i < BLOCKS; i += 2) {		/* free every other block ... */
		CHECK(block[i] && intact(block[i], size[i], (unsigned char)i));
		free(block[i]);
		block[i] = NULL;
	}
	for (i = 0; i < BLOCKS; i += 2) {		/* ... and take them again */
		block[i] = malloc(size[i] + 11);
		CHECK(block[i] != NULL);
		if (block[i])
			fill(block[i], size[i] + 11, (unsigned char)(i + 1));
	}
	for (i = 0; i < BLOCKS; i++) {
		size_t n = (i % 2) ? size[i] : size[i] + 11;
		unsigned char seed = (i % 2) ? (unsigned char)i : (unsigned char)(i + 1);

		CHECK(block[i] && intact(block[i], n, seed));
		free(block[i]);
	}

	p = malloc(100);
	CHECK(p != NULL);
	fill(p, 100, 5);
	q = realloc(p, 5000);				/* grow: the first 100 bytes stay */
	CHECK(q != NULL && intact(q, 100, 5));
	p = realloc(q, 40);				/* shrink: the first 40 stay */
	CHECK(p != NULL && intact(p, 40, 5));
	free(p);

	z = calloc(1000, sizeof(int));
	CHECK(z != NULL);
	for (i = 0; z && i < 1000; i++)
		CHECK(z[i] == 0);
	free(z);

	CHECK(posix_memalign(&aligned, 64, 1000) == 0);
	CHECK(((uintptr_t)aligned & 63) == 0);
	free(aligned);

	p = malloc(1024 * 1024);			/* large enough to be mapped */
	CHECK(p != NULL);
	if (p) {
		memset(p, 0xa5, 1024 * 1024);
		CHECK(p[0] == 0xa5 && p[1024 * 1024 - 1] == 0xa5);
		free(p);
	}
	{
		volatile size_t huge = (size_t)-1;

		CHECK(malloc(huge) == NULL);		/* an impossible size is refused */
	}
	free(NULL);
	DONE("malloc");
}
