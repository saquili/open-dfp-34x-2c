// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Parsing and building baseline OMCI frames, and their integrity check. */

#include <assert.h>
#include <stdbool.h>

#include "omcid/byteorder.h"
#include "omci/frame.h"
#include "sys/wire.h"

/* Byte offsets of the fields of a frame (G.988 11.2). */
enum {
	OFF_TCI = 0,
	OFF_MT = 2,
	OFF_DEVICE_ID = 3,
	OFF_CLASS = 4,
	OFF_INSTANCE = 6,
	OFF_CONTENTS = 8,
	OFF_TRAILER = 40,
	OFF_MIC = 44,
};

static_assert(OFF_CONTENTS + OMCI_CONTENTS_LEN == OFF_TRAILER,
	      "contents run 8..39");
static_assert(OFF_MIC + 4 == OMCI_FRAME_LEN, "the MIC is the last four bytes");

/*
 * The message type byte: bit 8 is reserved, bit 7 asks for an answer,
 * bit 6 marks an answer, bits 1 to 5 hold the type (G.988 11.2.2).
 */
#define MT_RESERVED_BIT 0x80u
#define MT_AR_BIT 0x40u
#define MT_AK_BIT 0x20u
#define MT_TYPE_MASK 0x1fu

/* The top bit of the transaction identifier selects the channel (G.988 11.2.1). */
#define TCI_PRIORITY_BIT 0x8000u
#define TCI_ID_MASK 0x7fffu

/* The CRC-32 of omci_frame_mic(), four bits at a time. */
static const uint32_t crc32_nibble[16] = {
	0x00000000u, 0x04c11db7u, 0x09823b6eu, 0x0d4326d9u,
	0x130476dcu, 0x17c56b6bu, 0x1a864db2u, 0x1e475005u,
	0x2608edb8u, 0x22c9f00fu, 0x2f8ad6d6u, 0x2b4bcb61u,
	0x350c9b64u, 0x31cd86d3u, 0x3c8ea00au, 0x384fbdbdu,
};

static uint32_t crc32_begin(void)
{
	return 0xffffffffu;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint32_t)p[i] << 24;
		crc = (crc << 4) ^ crc32_nibble[(crc >> 28) & 0x0fu];
		crc = (crc << 4) ^ crc32_nibble[(crc >> 28) & 0x0fu];
	}
	return crc;
}

static uint32_t crc32_final(uint32_t crc)
{
	return ~crc;
}

uint32_t omci_frame_mic(const uint8_t *data, size_t len)
{
	if (data == NULL)
		return 0;

	return crc32_final(crc32_update(crc32_begin(), data, len));
}

enum omci_status omci_frame_parse(const uint8_t wire[static OMCI_FRAME_LEN],
				  struct omci_frame *out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;

	const unsigned int tci = omci_be16(&wire[OFF_TCI]);
	const uint8_t mt = wire[OFF_MT];

	if (wire[OFF_DEVICE_ID] != OMCI_DEVICE_ID_BASELINE)
		return OMCI_ERR_PROTO;
	if (omci_be32(&wire[OFF_TRAILER]) != OMCI_TRAILER)
		return OMCI_ERR_PROTO;

	out->tci = (uint16_t)(tci & TCI_ID_MASK);
	out->priority = (tci & TCI_PRIORITY_BIT) != 0u;
	out->msg_type = (uint8_t)(mt & MT_TYPE_MASK);
	out->ar = (mt & MT_AR_BIT) != 0u;
	out->ak = (mt & MT_AK_BIT) != 0u;
	out->device_id = wire[OFF_DEVICE_ID];
	out->class_id = (uint16_t)omci_be16(&wire[OFF_CLASS]);
	out->instance = (uint16_t)omci_be16(&wire[OFF_INSTANCE]);
	omci_wire_copy(out->contents, &wire[OFF_CONTENTS], OMCI_CONTENTS_LEN);
	out->mic = omci_be32(&wire[OFF_MIC]);

	return OMCI_OK;
}

enum omci_status omci_frame_build(const struct omci_frame *in,
				  uint8_t wire[static OMCI_FRAME_LEN])
{
	if (in == NULL)
		return OMCI_ERR_INVAL;

	unsigned int tci = in->tci & TCI_ID_MASK;

	if (in->priority)
		tci |= TCI_PRIORITY_BIT;

	uint8_t mt = (uint8_t)(in->msg_type & MT_TYPE_MASK);

	if (in->ar)
		mt |= MT_AR_BIT;
	if (in->ak)
		mt |= MT_AK_BIT;

	omci_be16_store(&wire[OFF_TCI], tci);
	wire[OFF_MT] = mt;
	wire[OFF_DEVICE_ID] = in->device_id;
	omci_be16_store(&wire[OFF_CLASS], in->class_id);
	omci_be16_store(&wire[OFF_INSTANCE], in->instance);
	omci_wire_copy(&wire[OFF_CONTENTS], in->contents, OMCI_CONTENTS_LEN);
	omci_be32_store(&wire[OFF_TRAILER], OMCI_TRAILER);
	omci_be32_store(&wire[OFF_MIC], omci_frame_mic(wire, OFF_MIC));

	return OMCI_OK;
}

bool omci_frame_mic_ok(const uint8_t wire[static OMCI_FRAME_LEN])
{
	return omci_be32(&wire[OFF_MIC]) == omci_frame_mic(wire, OFF_MIC);
}
