/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The stick's own configuration store: the settings and the identity of the
 * unit kept across power loss, where the factory configuration of the
 * firmware the board shipped with is only read.
 *
 * The store is a region of flash split into two equal slots. A slot holds a
 * header and a payload of KEY=VALUE lines. A write goes to the slot that is
 * not current and raises a sequence number, so the current slot is untouched
 * until the new one is whole: a write cut by a power loss leaves the older
 * slot current, its payload intact. The reader takes the slot with the
 * highest sequence number whose header and payload check out.
 *
 * Every integer in the header is big-endian, as the stick's CPU is, so a
 * store written on one is read the same on the other. This module is the
 * format alone: it works on a region in memory, and the caller carries the
 * bytes to and from the flash (or, in a test, a file).
 */

#ifndef ONU_CFGSTORE_H
#define ONU_CFGSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The magic number "ONU1" and the header, in bytes, at the start of a slot. */
#define CFG_MAGIC 0x4f4e5531u
#define CFG_VERSION 1u
#define CFG_HEADER 24u

/* The most a key may be, and the most a value may be, without the newline. */
#define CFG_KEY_MAX 63u
#define CFG_VALUE_MAX 1024u

/**
 * cfg_payload_cap() - The room a slot of a region of @region_len bytes has for
 * its payload, or 0 when the region is too small for two slots with a header
 * each.
 */
size_t cfg_payload_cap(size_t region_len);

/**
 * cfg_load() - Read the current payload from a store region.
 * @region: the whole region, both slots
 * @region_len: its size
 * @payload: where the payload goes, with a terminating NUL
 * @cap: room in @payload, the NUL included
 * @len_out: the payload's length without the NUL, may be NULL
 * @seq_out: the current slot's sequence number, may be NULL
 * @slot_out: the current slot, 0 or 1, may be NULL
 *
 * The current slot is the valid one with the higher sequence number. A slot
 * is valid when its magic, version and length fit and its payload matches the
 * header's checksum.
 *
 * Return: 0, -ENOENT when neither slot is valid (an empty store), -EFBIG when
 * the payload does not fit @cap.
 */
int cfg_load(const uint8_t *region, size_t region_len, char *payload,
	     size_t cap, size_t *len_out, uint32_t *seq_out, int *slot_out);

/**
 * cfg_store() - Write a payload into the region's other slot.
 * @region: the whole region, changed in place
 * @region_len: its size
 * @cur_slot: the current slot from cfg_load(), or -1 for an empty store
 * @cur_seq: the current sequence number, 0 for an empty store
 * @payload: the payload to write
 * @len: its length
 * @off_out: the offset in @region of the slot that changed
 * @len_out: the number of bytes of that slot that were written
 *
 * The payload goes to the slot that is not @cur_slot (slot 0 for an empty
 * store) with the sequence number raised by one. Only that slot's bytes
 * change, and @off_out and @len_out bound them, so the caller erases and
 * writes that slot alone and leaves the current one whole until this one is.
 *
 * Return: 0, -EFBIG when the payload does not fit a slot.
 */
int cfg_store(uint8_t *region, size_t region_len, int cur_slot,
	      uint32_t cur_seq, const char *payload, size_t len,
	      size_t *off_out, size_t *len_out);

/**
 * cfg_valid_key() - Whether @key is a key the store accepts: a letter, then
 * letters, digits and underscores, at most %CFG_KEY_MAX of them.
 */
bool cfg_valid_key(const char *key);

/**
 * cfg_get() - Find the value of @key in a payload.
 * @payload: the payload, a NUL-terminated C string of KEY=VALUE lines
 * @key: the key
 * @out: the value, with a terminating NUL
 * @cap: room in @out, the NUL included
 *
 * Return: 0, -ENOENT without the key, -EFBIG when the value does not fit @out.
 */
int cfg_get(const char *payload, const char *key, char *out, size_t cap);

/**
 * cfg_set() - Set @key to @value in a payload, replacing its line or adding
 * one at the end.
 * @payload: the payload, changed in place, kept NUL-terminated
 * @cap: the room in @payload, the NUL included
 * @key: the key, which cfg_valid_key() must accept
 * @value: the value, without a newline, at most %CFG_VALUE_MAX bytes
 *
 * Return: 0, -EINVAL for a bad key or a value with a newline or too long,
 * -EFBIG when the line does not fit @cap.
 */
int cfg_set(char *payload, size_t cap, const char *key, const char *value);

/**
 * cfg_unset() - Remove the line of @key from a payload.
 *
 * Return: 0 whether or not the key was there.
 */
int cfg_unset(char *payload, const char *key);

#endif
