/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The boot loader's environment, as the loader of this board keeps it: two
 * copies of 8 KiB in the partitions env and env2. A copy is a CRC32 of its
 * data (big-endian, as the board's CPU is), a flag byte, and the data: the
 * variables as NUL-terminated KEY=VALUE strings, sorted by key, ended by an
 * empty string and padded with zeros.
 *
 * The flag says which copy is current: 1 for the copy written last, 0 for
 * the one it replaced. A save writes the whole new environment into the copy
 * that is not current, flag 1, and then clears the flag of the copy that was,
 * so that a power loss at any point leaves at least one whole copy that the
 * loader takes. Of two copies whose data check out the loader takes the one
 * flagged 1 against one flagged 0, and otherwise the second (env2).
 *
 * This module is the format alone; the caller carries the copies to and from
 * the flash (or, in a test, a file).
 */

#ifndef ONU_BOOTENV_H
#define ONU_BOOTENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOTENV_SIZE 0x2000u
#define BOOTENV_HEADER 5u
#define BOOTENV_DATA (BOOTENV_SIZE - BOOTENV_HEADER)
#define BOOTENV_ACTIVE 1u
#define BOOTENV_OBSOLETE 0u

/* The byte offset of the flag in a copy. */
#define BOOTENV_FLAG_OFFSET 4u

/* Whether the CRC of @copy, BOOTENV_SIZE bytes, matches its data. */
bool bootenv_valid(const uint8_t *copy);

/**
 * bootenv_current() - The copy the boot loader takes.
 * @copy0: the copy in env
 * @copy1: the copy in env2
 *
 * Return: 0 or 1, or -ENOENT when neither checks out (the loader then starts
 * with its built-in environment, which this system leaves alone).
 */
int bootenv_current(const uint8_t *copy0, const uint8_t *copy1);

/**
 * bootenv_get() - The value of @key in @copy.
 *
 * Return: the value, NUL-terminated inside the copy, or NULL when the key is
 * not set or the data is not ended inside the copy.
 */
const char *bootenv_get(const uint8_t *copy, const char *key);

/**
 * bootenv_set() - Set @key to @value in the data of @copy, or remove it when
 * @value is NULL, keeping the variables sorted. The header is left as it is:
 * bootenv_seal() writes it once the data is complete.
 *
 * Return: 0, -EINVAL for an empty key, a key with '=' or a value with a
 * newline, -EBADMSG when the data is not ended inside the copy, -ENOSPC when
 * the variables do not fit, -ENOMEM.
 */
int bootenv_set(uint8_t *copy, const char *key, const char *value);

/* Write the CRC of the data of @copy and @flag into its header. */
void bootenv_seal(uint8_t *copy, uint8_t flag);

/**
 * bootenv_next() - Walk the variables of @copy: the one after offset *@pos in
 * the data, 0 for the first.
 *
 * Return: the variable, "KEY=VALUE", or NULL after the last; *@pos moves on.
 */
const char *bootenv_next(const uint8_t *copy, size_t *pos);

#endif
