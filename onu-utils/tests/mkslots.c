// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * mkslots FILE SIZE: a partition of the store of before step 9 for the
 * tests, SIZE bytes of erased flash with the KEY=VALUE lines of the standard
 * input in its first slot (cfgstore.h).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfgstore.h"

int main(int argc, char **argv)
{
	size_t size, len, off, wrote;
	uint8_t *region;
	char *payload;
	FILE *f;

	if (argc != 3)
		return 2;
	size = strtoul(argv[2], NULL, 0);
	region = malloc(size);
	payload = calloc(1, size + 1);
	if (!region || !payload)
		return 1;
	memset(region, 0xff, size);
	len = fread(payload, 1, size, stdin);
	if (cfg_store(region, size, -1, 0, payload, len, &off, &wrote))
		return 1;
	f = fopen(argv[1], "wb");
	if (!f || fwrite(region, 1, size, f) != size || fclose(f))
		return 1;
	return 0;
}
