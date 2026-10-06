// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The configuration store of cfgstore.h: the format and nothing else. */

#include <errno.h>
#include <string.h>

#include <zlib.h>

#include "cfgstore.h"

/* The header fields, as byte offsets from the start of a slot. */
#define H_MAGIC 0u
#define H_VERSION 4u
#define H_FLAGS 6u
#define H_SEQ 8u
#define H_LENGTH 12u
#define H_CRC 16u
#define H_RESERVED 20u

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t payload_crc(const uint8_t *data, size_t len)
{
	return (uint32_t)crc32(crc32(0L, Z_NULL, 0), data, (unsigned int)len);
}

/* The length of one slot of a region of @region_len bytes. */
static size_t slot_len(size_t region_len)
{
	return region_len / 2u;
}

size_t cfg_payload_cap(size_t region_len)
{
	size_t slot = slot_len(region_len);

	if (slot <= CFG_HEADER)
		return 0;
	return slot - CFG_HEADER;
}

/*
 * Read the header of the slot at @slot, and, when it is valid, its sequence
 * number in @seq and its payload length in @len. A slot is valid when the
 * magic and version are the store's, the length fits the slot, and the
 * payload matches the stored checksum.
 */
static bool slot_valid(const uint8_t *region, size_t region_len, int slot,
		       uint32_t *seq, size_t *len)
{
	size_t cap = cfg_payload_cap(region_len);
	const uint8_t *s = region + (size_t)slot * slot_len(region_len);
	size_t plen;

	if (get32(s + H_MAGIC) != CFG_MAGIC || get16(s + H_VERSION) != CFG_VERSION)
		return false;
	plen = get32(s + H_LENGTH);
	if (plen > cap)
		return false;
	if (get32(s + H_CRC) != payload_crc(s + CFG_HEADER, plen))
		return false;
	*seq = get32(s + H_SEQ);
	*len = plen;
	return true;
}

int cfg_load(const uint8_t *region, size_t region_len, char *payload,
	     size_t cap, size_t *len_out, uint32_t *seq_out, int *slot_out)
{
	uint32_t seq[2] = { 0, 0 };
	size_t len[2] = { 0, 0 };
	bool ok[2];
	int best;

	if (cfg_payload_cap(region_len) == 0)
		return -ENOENT;

	ok[0] = slot_valid(region, region_len, 0, &seq[0], &len[0]);
	ok[1] = slot_valid(region, region_len, 1, &seq[1], &len[1]);

	if (ok[0] && ok[1])
		best = seq[1] > seq[0] ? 1 : 0;
	else if (ok[0])
		best = 0;
	else if (ok[1])
		best = 1;
	else
		return -ENOENT;

	if (len[best] + 1 > cap)
		return -EFBIG;

	memcpy(payload, region + (size_t)best * slot_len(region_len) + CFG_HEADER,
	       len[best]);
	payload[len[best]] = '\0';
	if (len_out)
		*len_out = len[best];
	if (seq_out)
		*seq_out = seq[best];
	if (slot_out)
		*slot_out = best;
	return 0;
}

int cfg_store(uint8_t *region, size_t region_len, int cur_slot,
	      uint32_t cur_seq, const char *payload, size_t len,
	      size_t *off_out, size_t *len_out)
{
	int slot = cur_slot == 0 ? 1 : 0;
	size_t off = (size_t)slot * slot_len(region_len);
	uint8_t *s = region + off;

	if (len > cfg_payload_cap(region_len))
		return -EFBIG;

	memset(s, 0, CFG_HEADER);
	put32(s + H_MAGIC, CFG_MAGIC);
	put16(s + H_VERSION, CFG_VERSION);
	put16(s + H_FLAGS, 0);
	put32(s + H_SEQ, cur_seq + 1u);
	put32(s + H_LENGTH, (uint32_t)len);
	put32(s + H_CRC, payload_crc((const uint8_t *)payload, len));
	put32(s + H_RESERVED, 0);
	memcpy(s + CFG_HEADER, payload, len);

	if (off_out)
		*off_out = off;
	if (len_out)
		*len_out = CFG_HEADER + len;
	return 0;
}

bool cfg_valid_key(const char *key)
{
	size_t i;

	if (key[0] < 'A' || key[0] > 'Z')
		return false;
	for (i = 0; key[i]; i++) {
		char c = key[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '_'))
			return false;
	}
	return i <= CFG_KEY_MAX;
}

/*
 * The line of @key in @payload: its start in @start and the start of the next
 * line (or the end) in @next. Returns whether the key was found. A line is
 * KEY=VALUE up to a newline; the key is what comes before the first '='.
 */
static bool find_line(const char *payload, const char *key, size_t *start,
		      size_t *next)
{
	size_t keylen = strlen(key);
	const char *p = payload;

	while (*p) {
		const char *nl = strchr(p, '\n');
		const char *eq = strchr(p, '=');
		size_t line = nl ? (size_t)(nl - p) : strlen(p);
		size_t klen = eq && (!nl || eq < nl) ? (size_t)(eq - p) : line;

		if (klen == keylen && memcmp(p, key, keylen) == 0) {
			*start = (size_t)(p - payload);
			*next = *start + line + (nl ? 1u : 0u);
			return true;
		}
		p += line + (nl ? 1u : 0u);
	}
	return false;
}

int cfg_get(const char *payload, const char *key, char *out, size_t cap)
{
	size_t start, next;
	const char *line, *eq;
	size_t vlen;

	if (!find_line(payload, key, &start, &next))
		return -ENOENT;
	line = payload + start;
	eq = strchr(line, '=');
	if (!eq || (size_t)(eq - payload) >= next)
		return -ENOENT;
	vlen = next - (size_t)(eq + 1 - payload);
	if (vlen && (eq[1 + vlen - 1] == '\n'))
		vlen--;
	if (vlen + 1 > cap)
		return -EFBIG;
	memcpy(out, eq + 1, vlen);
	out[vlen] = '\0';
	return 0;
}

int cfg_set(char *payload, size_t cap, const char *key, const char *value)
{
	size_t start, next, cur = strlen(payload);
	size_t keylen = strlen(key), vallen = strlen(value);
	size_t linelen = keylen + 1 + vallen + 1; /* KEY=VALUE\n */
	bool found;

	if (!cfg_valid_key(key) || vallen > CFG_VALUE_MAX || strchr(value, '\n'))
		return -EINVAL;

	found = find_line(payload, key, &start, &next);
	if (!found) {
		start = cur;
		next = cur;
	}

	if (cur - (next - start) + linelen + 1 > cap)
		return -EFBIG;

	/* Make room for the new line where the old one was (or at the end). */
	memmove(payload + start + linelen, payload + next, cur - next + 1);
	memcpy(payload + start, key, keylen);
	payload[start + keylen] = '=';
	memcpy(payload + start + keylen + 1, value, vallen);
	payload[start + keylen + 1 + vallen] = '\n';
	return 0;
}

int cfg_unset(char *payload, const char *key)
{
	size_t start, next, cur = strlen(payload);

	if (!find_line(payload, key, &start, &next))
		return 0;
	memmove(payload + start, payload + next, cur - next + 1);
	return 0;
}
