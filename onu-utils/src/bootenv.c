// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The boot loader's environment of bootenv.h: the format and nothing else. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "bootenv.h"

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t data_crc(const uint8_t *copy)
{
	return (uint32_t)crc32(crc32(0L, Z_NULL, 0), copy + BOOTENV_HEADER,
			       BOOTENV_DATA);
}

bool bootenv_valid(const uint8_t *copy)
{
	return get32(copy) == data_crc(copy);
}

int bootenv_current(const uint8_t *copy0, const uint8_t *copy1)
{
	bool ok0 = bootenv_valid(copy0), ok1 = bootenv_valid(copy1);
	uint8_t f0 = copy0[BOOTENV_FLAG_OFFSET], f1 = copy1[BOOTENV_FLAG_OFFSET];

	if (!ok0 && !ok1)
		return -ENOENT;
	if (ok0 != ok1)
		return ok0 ? 0 : 1;
	if (f0 == BOOTENV_ACTIVE && f1 == BOOTENV_OBSOLETE)
		return 0;
	return 1;
}

const char *bootenv_next(const uint8_t *copy, size_t *pos)
{
	const char *data = (const char *)copy + BOOTENV_HEADER;
	size_t len;

	if (*pos >= BOOTENV_DATA || !data[*pos])
		return NULL;
	len = strnlen(data + *pos, BOOTENV_DATA - *pos);
	if (*pos + len >= BOOTENV_DATA)
		return NULL;
	*pos += len + 1;
	return data + *pos - len - 1;
}

/* The length of the key of the variable @var, up to its '='. */
static size_t key_len(const char *var)
{
	const char *eq = strchr(var, '=');

	return eq ? (size_t)(eq - var) : strlen(var);
}

const char *bootenv_get(const uint8_t *copy, const char *key)
{
	size_t pos = 0, klen = strlen(key);
	const char *var;

	while ((var = bootenv_next(copy, &pos)))
		if (key_len(var) == klen && !memcmp(var, key, klen) &&
		    var[klen] == '=')
			return var + klen + 1;
	return NULL;
}

/* Whether the data of @copy is ended, by an empty string, inside the copy. */
static bool ended(const uint8_t *copy)
{
	const char *data = (const char *)copy + BOOTENV_HEADER;
	size_t pos = 0, len;

	while (pos < BOOTENV_DATA && data[pos]) {
		len = strnlen(data + pos, BOOTENV_DATA - pos);
		if (pos + len >= BOOTENV_DATA)
			return false;
		pos += len + 1;
	}
	return pos < BOOTENV_DATA;
}

/* Compare two variables by their keys, as the loader sorts them. */
static int by_key(const void *a, const void *b)
{
	const char *x = *(const char *const *)a, *y = *(const char *const *)b;
	size_t lx = key_len(x), ly = key_len(y);
	int c = memcmp(x, y, lx < ly ? lx : ly);

	if (c)
		return c;
	return lx < ly ? -1 : lx > ly;
}

int bootenv_set(uint8_t *copy, const char *key, const char *value)
{
	size_t klen = strlen(key), n = 0, cap = 64, pos = 0, out = 0, i;
	const char **vars;
	char *added = NULL, *data;
	const char *var;
	int err = 0;

	if (!klen || strchr(key, '=') || strchr(key, '\n') ||
	    (value && strchr(value, '\n')))
		return -EINVAL;
	if (!ended(copy))
		return -EBADMSG;

	vars = malloc(cap * sizeof(*vars));
	if (!vars)
		return -ENOMEM;
	while ((var = bootenv_next(copy, &pos))) {
		if (key_len(var) == klen && !memcmp(var, key, klen))
			continue;
		if (n + 1 >= cap) {
			const char **more = realloc(vars, 2 * cap * sizeof(*vars));

			if (!more) {
				err = -ENOMEM;
				goto out;
			}
			vars = more;
			cap *= 2;
		}
		vars[n++] = var;
	}
	if (value) {
		size_t vlen = strlen(value);

		added = malloc(klen + vlen + 2);
		if (!added) {
			err = -ENOMEM;
			goto out;
		}
		memcpy(added, key, klen);
		added[klen] = '=';
		memcpy(added + klen + 1, value, vlen + 1);
		vars[n++] = added;
	}
	qsort(vars, n, sizeof(*vars), by_key);

	data = malloc(BOOTENV_DATA);
	if (!data) {
		err = -ENOMEM;
		goto out;
	}
	memset(data, 0, BOOTENV_DATA);
	for (i = 0; i < n; i++) {
		size_t len = strlen(vars[i]) + 1;

		/* Room for this one and the empty string that ends them. */
		if (out + len + 1 > BOOTENV_DATA) {
			err = -ENOSPC;
			break;
		}
		memcpy(data + out, vars[i], len);
		out += len;
	}
	if (!err)
		memcpy(copy + BOOTENV_HEADER, data, BOOTENV_DATA);
	free(data);
out:
	free(added);
	free(vars);
	return err;
}

void bootenv_seal(uint8_t *copy, uint8_t flag)
{
	copy[BOOTENV_FLAG_OFFSET] = flag;
	put32(copy, data_crc(copy));
}
