/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The OMCI engine: it answers the frames the OLT sends, from the management
 * information base, and keeps the hardware in step with what the OLT set up.
 */

#ifndef OMCI_ENGINE_H
#define OMCI_ENGINE_H

#include <stdint.h>

#include "mib/store.h"
#include "omci/frame.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/**
 * struct omci_tx_ops - How the engine sends a frame.
 * @send: called with every frame to send, answers and notifications alike
 */
struct omci_tx_ops {
	void (*send)(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN]);
};

/**
 * struct omci_tap_ops - An observer of the frames the engine handles.
 * @frame: called for every frame received or sent, with a verdict from
 *	&enum omci_ctl_tap_verdict and, as the detail, the message type
 */
struct omci_tap_ops {
	void (*frame)(void *ctx, uint8_t verdict, uint8_t detail,
		      const uint8_t wire[static OMCI_FRAME_LEN]);
};

struct omci_engine;

/**
 * omci_engine_init() - Start the engine.
 * @out: the engine
 * @mib: the management information base it serves
 * @tx: how it sends frames
 * @tx_ctx: passed to @tx
 *
 * A process runs one engine at a time.
 *
 * Return: %OMCI_OK; %OMCI_ERR_INVAL; %OMCI_ERR_BUSY while another engine
 * runs; or the failure of the alarm or performance monitoring set-up.
 */
__must_check enum omci_status omci_engine_init(struct omci_engine **out,
					       struct omci_mib *mib,
					       const struct omci_tx_ops *tx,
					       void *tx_ctx);

/**
 * omci_engine_fini() - Stop the engine.
 * @eng: the engine, or NULL
 */
void omci_engine_fini(struct omci_engine *eng);

/**
 * omci_engine_tap_set() - Attach an observer of the frames, or detach it.
 * @eng: the engine
 * @ops: the observer, or NULL
 * @ctx: passed to @ops
 */
void omci_engine_tap_set(struct omci_engine *eng,
			 const struct omci_tap_ops *ops, void *ctx);

/**
 * omci_engine_tick() - Let time pass.
 * @eng: the engine
 * @now_sec: monotonic time in seconds
 *
 * The performance monitoring intervals run on this clock.
 */
void omci_engine_tick(struct omci_engine *eng, uint64_t now_sec);

/**
 * omci_engine_rx() - Handle one frame from the OLT.
 * @eng: the engine
 * @wire: the frame
 *
 * Return: %OMCI_OK when the frame was answered, or deliberately left
 * unanswered; %OMCI_ERR_PROTO or %OMCI_ERR_MIC for a frame dropped as
 * malformed; %OMCI_ERR_INVAL without @eng.
 */
__must_check enum omci_status
omci_engine_rx(struct omci_engine *eng,
	       const uint8_t wire[static OMCI_FRAME_LEN]);

#endif
