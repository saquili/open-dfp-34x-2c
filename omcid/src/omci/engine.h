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
 * omci_engine_reprogram() - Program the data path again from the MIB.
 * @eng: the engine
 *
 * The GEM ports, the bridges and the VLAN rules are applied again as the MIB
 * holds them, as after a change of the backend's mode. What the backend
 * declines it logs; the MIB does not change.
 */
void omci_engine_reprogram(struct omci_engine *eng);

/**
 * omci_engine_changes() - A number that changes with every request of the
 *	OLT that may change what it set up.
 * @eng: the engine
 *
 * MIB reset, Create, Delete, Set and Synchronize time change it, whether
 * they succeed or not; a request refused before it is carried out does
 * not.
 *
 * Return: the number.
 */
__must_check uint32_t omci_engine_changes(const struct omci_engine *eng);

/* The most bytes omci_engine_save() writes. */
#define OMCI_ENGINE_SAVE_MAX (36u + OMCI_MIB_SAVE_MAX)

/**
 * omci_engine_save() - Write what the engine needs to go on after the
 *	daemon starts again.
 * @eng: the engine
 * @out: where the bytes go
 * @cap: room in @out; %OMCI_ENGINE_SAVE_MAX is always enough
 * @out_len: how many bytes were written
 *
 * The bytes hold the MIB, the time of day that Synchronize time set and
 * where the performance monitoring intervals stand. The time is that of
 * the monotonic clock, which a daemon started again on the same system
 * goes on with.
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or %OMCI_ERR_TRUNCATED when @cap is
 * too small.
 */
__must_check enum omci_status omci_engine_save(const struct omci_engine *eng,
					       uint8_t *out, size_t cap,
					       size_t *out_len);

/**
 * omci_engine_load() - Go on from what omci_engine_save() wrote.
 * @eng: the engine
 * @in: the bytes
 * @len: their number
 *
 * The MIB, the time of day and the intervals become what they were, every
 * read in progress ends, and the data path is programmed again from the
 * MIB. Nothing is sent to the OLT: to it, the daemon never stopped. The
 * counts of the interval in progress start from zero.
 *
 * Return: %OMCI_OK; %OMCI_ERR_INVAL; %OMCI_ERR_PROTO for bytes that are not
 * an engine's of this daemon; %OMCI_ERR_TRUNCATED for bytes that end too
 * soon; or the error of omci_mib_load(). After an error the MIB is empty.
 */
__must_check enum omci_status omci_engine_load(struct omci_engine *eng,
					       const uint8_t *in, size_t len);

/**
 * omci_engine_mcast_fd() - The descriptor the multicast agent waits on.
 * @eng: the engine
 *
 * Return: a descriptor that becomes readable when the subscriber side's
 * multicast changes, or -1 for none: the agent then looks once a second.
 */
__must_check int omci_engine_mcast_fd(const struct omci_engine *eng);

/**
 * omci_engine_mcast_service() - Let the multicast agent answer what the
 *	subscriber side did.
 * @eng: the engine, or NULL
 */
void omci_engine_mcast_service(struct omci_engine *eng);

struct omci_mcast;

/**
 * omci_engine_mcast() - The engine's multicast agent (mcast/agent.h).
 * @eng: the engine
 *
 * Return: the agent, or NULL for an engine without one.
 */
struct omci_mcast *omci_engine_mcast(struct omci_engine *eng);

/**
 * omci_engine_pon_fd() - The descriptor that tells of a change of the PON
 *	device.
 * @eng: the engine
 *
 * Return: a descriptor that becomes readable when the PON device announces a
 * change of its status, or -1 for none: the engine then reads the encryption
 * of the Port-IDs once a second.
 */
__must_check int omci_engine_pon_fd(const struct omci_engine *eng);

/**
 * omci_engine_pon_service() - Take what the PON device announced, and bring
 *	the encryption state of the GEM port network CTPs up to date.
 * @eng: the engine, or NULL
 */
void omci_engine_pon_service(struct omci_engine *eng);

/**
 * omci_engine_tick() - Let time pass.
 * @eng: the engine
 * @now_sec: monotonic time in seconds
 *
 * The performance monitoring intervals run on this clock. Once a second the
 * alarms are judged, and, when the PON device has no descriptor
 * (omci_engine_pon_fd()), the encryption of the Port-IDs is read.
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
