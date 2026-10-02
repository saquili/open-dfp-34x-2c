/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The baseline OMCI message: a frame of 48 bytes with a fixed layout
 * (ITU-T G.988, clauses 11.1 and 11.2). The daemon does not use the
 * extended message format.
 */

#ifndef OMCI_FRAME_H
#define OMCI_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omcid/attrs.h"
#include "omcid/status.h"

#define OMCI_FRAME_LEN 48u

/* The message contents, bytes 9 to 40 of a frame (G.988 11.2.6). */
#define OMCI_CONTENTS_LEN 32u

/* The device identifier of the baseline format (G.988 11.2.3). */
#define OMCI_DEVICE_ID_BASELINE 0x0au

/*
 * The first four bytes of the trailer: two zero bytes, then the length of
 * the header and contents, 40 (G.988 11.2.7).
 */
#define OMCI_TRAILER 0x00000028u

/**
 * struct omci_frame - A baseline message, field by field.
 * @tci: transaction correlation identifier, without its priority bit
 * @priority: the priority bit of the identifier: the high-priority channel
 * @msg_type: the message type (bits 1 to 5 of the type byte)
 * @ar: acknowledge request: the sender expects an answer
 * @ak: acknowledgement: this message is an answer
 * @device_id: the device identifier
 * @class_id: the managed entity class
 * @instance: the managed entity instance
 * @contents: the message contents
 * @mic: the message integrity check as received
 */
struct omci_frame {
	uint16_t tci;
	bool priority;
	uint8_t msg_type;
	bool ar;
	bool ak;
	uint8_t device_id;
	uint16_t class_id;
	uint16_t instance;
	uint8_t contents[OMCI_CONTENTS_LEN];
	uint32_t mic;
};

/**
 * omci_frame_parse() - Take a received frame apart.
 * @wire: the frame as it arrived
 * @out: the fields
 *
 * The integrity check is not verified here; omci_frame_mic_ok() does that.
 *
 * Return: %OMCI_OK; %OMCI_ERR_INVAL without @out; %OMCI_ERR_PROTO when the
 * device identifier or the trailer is not the baseline format's.
 */
__must_check enum omci_status
omci_frame_parse(const uint8_t wire[static OMCI_FRAME_LEN],
		 struct omci_frame *out);

/**
 * omci_frame_build() - Lay out a frame for sending.
 * @in: the fields; @in->mic is not used
 * @wire: the frame, with its trailer and integrity check
 *
 * Return: %OMCI_OK, or %OMCI_ERR_INVAL without @in.
 */
__must_check enum omci_status
omci_frame_build(const struct omci_frame *in,
		 uint8_t wire[static OMCI_FRAME_LEN]);

/**
 * omci_frame_mic_ok() - Check the integrity check of a received frame.
 * @wire: the frame
 *
 * Return: true when its last four bytes are the check of the first 44.
 */
__must_check bool omci_frame_mic_ok(const uint8_t wire[static OMCI_FRAME_LEN]);

/**
 * omci_frame_mic() - Compute a message integrity check.
 * @data: the bytes to cover
 * @len: their number
 *
 * The check of the baseline format is the CRC-32 of ITU-T I.363.5
 * (G.988 11.2.7): generator 0x04C11DB7, register preset to all ones, bits
 * taken most significant first, result complemented.
 *
 * Return: the check, or 0 without @data.
 */
__must_check uint32_t omci_frame_mic(const uint8_t *data, size_t len);

#endif
