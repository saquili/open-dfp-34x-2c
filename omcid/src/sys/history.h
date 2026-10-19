/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The conversation with the OLT: the last frames received and sent, each
 * with the time it passed and what became of it, numbered from the daemon's
 * start, for a reader that follows them (ADR 0011, the method "frames" on
 * ubus). The ONU-G's logical password is zeroed in a frame before the frame
 * is kept, so no reader ever gets it.
 */

#ifndef OMCI_SYS_HISTORY_H
#define OMCI_SYS_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "omci/frame.h"

struct omci_publish_ops;

/* The frames kept, and the most frames one answer to a reader holds. */
#define OMCI_HISTORY_LEN 1024u
#define OMCI_HISTORY_PAGE 200u

/**
 * struct omci_history_rec - A frame kept.
 * @sec: the wall clock when it passed, in seconds since 1970
 * @ms: and the milliseconds of that second
 * @verdict: what became of it, an &enum omci_ctl_tap_verdict
 * @type: its message type
 * @masked: the logical password was zeroed in it
 * @frame: the frame
 */
struct omci_history_rec {
	uint32_t sec;
	uint16_t ms;
	uint8_t verdict;
	uint8_t type;
	bool masked;
	uint8_t frame[OMCI_FRAME_LEN];
};

/**
 * struct omci_history - The frames kept.
 * @rec: frame number n at @rec[n % OMCI_HISTORY_LEN]
 * @head: the frames kept since the start, which is the next one's number
 */
struct omci_history {
	struct omci_history_rec rec[OMCI_HISTORY_LEN];
	uint32_t head;
};

/**
 * omci_history_add() - Keep a frame, in place of the oldest when full.
 * @h: the frames kept
 * @sec: the wall clock, in seconds since 1970
 * @ms: and the milliseconds of that second
 * @verdict: what became of it
 * @type: its message type
 * @frame: the frame; the copy kept has the logical password zeroed
 */
void omci_history_add(struct omci_history *h, uint32_t sec, uint16_t ms,
		      uint8_t verdict, uint8_t type,
		      const uint8_t frame[static OMCI_FRAME_LEN]);

/**
 * omci_publish_frames() - Write the frames kept from a number on.
 * @h: the frames kept
 * @since: the number of the first frame the reader wants; one older than
 *	the frames kept, or one past @h's head (the reader followed a daemon
 *	that has since started again), gives the oldest kept
 * @max: the most frames to write, at most %OMCI_HISTORY_PAGE; 0 for that
 * @ops: the writer
 * @ctx: passed to @ops
 *
 * What is written is, as JSON, {"head":N,"first":N,"frames":[{"seq":N,
 * "sec":N,"ms":N,"verdict":N,"type":N,"masked":B,"frame":"hex"},...]}:
 * the next frame's number, the oldest one kept, and the frames.
 */
void omci_publish_frames(const struct omci_history *h, uint32_t since,
			 uint32_t max, const struct omci_publish_ops *ops,
			 void *ctx);

#endif /* OMCI_SYS_HISTORY_H */
