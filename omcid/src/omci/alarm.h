/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Alarms: the state of the alarms of the hardware, per managed entity, as
 * the OLT reads it with Get all alarms and hears it in alarm notifications
 * (G.988 A.1.4).
 */

#ifndef OMCI_ALARM_H
#define OMCI_ALARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omci/frame.h"
#include "omci/optical.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/* An alarm bitmap: 224 bits, alarm 0 in the most significant bit of byte 0. */
#define OMCI_ALARM_BITMAP_LEN 28u

/* The most entities that can carry an alarm at the same time. */
#define OMCI_ALARM_MAX_ENTITIES 32u

/* The most entities raised by omci_alarm_raised() at the same time. */
#define OMCI_ALARM_MAX_RAISED 64u

/* The ANI-G, which carries the alarms of the PON side, and its one instance. */
#define OMCI_ALARM_ANI_G 263u
#define OMCI_ALARM_ANI_G_INSTANCE 0x8001u

/**
 * struct omci_alarm_entry - The alarms of one entity.
 * @class_id: the class
 * @instance: the instance
 * @bitmap: one bit per alarm, set while the alarm stands
 */
struct omci_alarm_entry {
	uint16_t class_id;
	uint16_t instance;
	uint8_t bitmap[OMCI_ALARM_BITMAP_LEN];
};

/**
 * struct omci_alarm_thresholds - The thresholds of the ANI-G (G.988 9.2.1).
 * @sf: signal fail, the k of a rate of 10^-k
 * @sd: signal degrade, likewise
 * @optical: the thresholds of the optical powers
 */
struct omci_alarm_thresholds {
	uint8_t sf;
	uint8_t sd;
	struct omci_optical_thresholds optical;
};

/* What omci_alarm_levels() has: the received and the transmitted level. */
#define OMCI_ALARM_LEVEL_RX 0x1u
#define OMCI_ALARM_LEVEL_TX 0x2u

struct omci_alarm;

/**
 * omci_alarm_init() - Set up the alarm state; one per process.
 * @out: the state
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or %OMCI_ERR_BUSY while one exists.
 */
__must_check enum omci_status omci_alarm_init(struct omci_alarm **out);
/**
 * omci_alarm_fini() - Release the alarm state.
 * @al: the state, or NULL
 */
void omci_alarm_fini(struct omci_alarm *al);

/**
 * omci_alarm_judge() - Judge the alarms the hardware does not raise itself.
 * @al: the state
 * @now_sec: the time, in seconds of a clock that never goes back
 * @t: the thresholds of the ANI-G
 *
 * Takes the count of BIP errors of the receiver and declares or clears
 * signal fail and signal degrade (omci_ber_judge()): while the ONU is not
 * in operation, or the count is not to be had, there is no rate to judge,
 * the history is dropped and both clear. Then takes the readings of the
 * transceiver and judges the optical alarms (omci_optical_judge()) against
 * the limits the thresholds and the transceiver's own give, reading those
 * of the transceiver until it has them once. The next omci_alarm_poll()
 * reports what was declared.
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or the failure of the read of the
 * receiver.
 */
__must_check enum omci_status
omci_alarm_judge(struct omci_alarm *al, uint64_t now_sec,
		 const struct omci_alarm_thresholds *t);

/**
 * omci_alarm_levels() - The optical levels the last judgement read.
 * @al: the state
 * @rx: the received level, in units of 0.002 dB referred to 1 mW
 * @tx: the transmitted level, likewise
 *
 * Return: OMCI_ALARM_LEVEL_RX and OMCI_ALARM_LEVEL_TX for the levels that
 * were read; the others are left as they were.
 */
__must_check uint8_t omci_alarm_levels(const struct omci_alarm *al, int16_t *rx,
				       int16_t *tx);

/**
 * omci_alarm_poll() - Read the alarms of the hardware.
 * @al: the state
 * @changed: set when any bitmap changed since the last poll; may be NULL
 *
 * The alarms omci_alarm_judge() declared are reported with them.
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or the failure of the hardware read,
 * which leaves the state as it was.
 */
__must_check enum omci_status omci_alarm_poll(struct omci_alarm *al,
					      bool *changed);

/**
 * omci_alarm_raised() - Set the entities of a class that stand under an
 *	alarm the daemon judges outside the hardware's.
 * @al: the state
 * @class_id: the class
 * @bit: the alarm's number
 * @instances: the instances under it; it clears for every other
 * @n: how many
 *
 * The next omci_alarm_poll() reports them with the others; at most
 * %OMCI_ALARM_MAX_RAISED are raised at once, and the entities beyond
 * %OMCI_ALARM_MAX_ENTITIES are not reported.
 */
void omci_alarm_raised(struct omci_alarm *al, uint16_t class_id, uint8_t bit,
		       const uint16_t *instances, size_t n);

/**
 * omci_alarm_suppress() - Hold back the notifications of an entity, or not.
 * @al: the state
 * @class_id: the class
 * @instance: the instance
 * @suppressed: true while the entity is under alarm-reporting control
 *	(G.988 A.1.4.3): its alarms are kept, and reported by Get all alarms
 *	unless the OLT asks for the others only, but not notified
 */
void omci_alarm_suppress(struct omci_alarm *al, uint16_t class_id,
			 uint16_t instance, bool suppressed);

/**
 * omci_alarm_standing() - Whether an alarm of an entity stands.
 * @al: the state
 * @class_id: the class
 * @instance: the instance
 *
 * Return: true when any alarm of the entity stands.
 */
__must_check bool omci_alarm_standing(const struct omci_alarm *al,
				      uint16_t class_id, uint16_t instance);

/**
 * omci_alarm_snapshot() - Freeze the standing alarms for Get all alarms.
 * @al: the state
 * @skip_suppressed: leave out the entities under alarm-reporting control
 *
 * What is frozen is what the OLT knows from then on: an entity left out is
 * clear to it, and a later change is notified against that.
 *
 * Return: the number of entities with an alarm, which is the number of
 * Get all alarms next commands the OLT may send.
 */
uint16_t omci_alarm_snapshot(struct omci_alarm *al, bool skip_suppressed);

/**
 * omci_alarm_snapshot_drop() - Discard the frozen alarms.
 * @al: the state, or NULL
 */
void omci_alarm_snapshot_drop(struct omci_alarm *al);

/**
 * omci_alarm_snapshot_get() - One entity of the frozen alarms.
 * @al: the state
 * @seq: its place, from 0
 * @out: the entity and its bitmap
 *
 * Return: false when @seq is past the end.
 */
__must_check bool omci_alarm_snapshot_get(const struct omci_alarm *al,
					  uint16_t seq,
					  struct omci_alarm_entry *out);

/**
 * omci_alarm_build_notification() - Build an alarm notification.
 * @al: the state; its sequence number advances
 * @entry: the entity and its bitmap
 * @out: the frame (G.988 A.3.19)
 */
void omci_alarm_build_notification(struct omci_alarm *al,
				   const struct omci_alarm_entry *entry,
				   uint8_t out[static OMCI_FRAME_LEN]);

/**
 * omci_alarm_take_changes() - The entities whose alarms the OLT has not heard.
 * @al: the state
 * @out: the entities
 * @cap: room in @out
 *
 * Each entity not under alarm-reporting control whose alarms differ from
 * what the OLT was last told, by a notification or a Get all alarms, is
 * written once, and counted as told.
 *
 * Return: how many were written.
 */
size_t omci_alarm_take_changes(struct omci_alarm *al,
			       struct omci_alarm_entry *out, size_t cap);

/**
 * omci_alarm_reset_sequence() - Start the alarm sequence number again at 1.
 * @al: the state, or NULL
 */
void omci_alarm_reset_sequence(struct omci_alarm *al);

/**
 * omci_alarm_sequence() - The sequence number of the next notification.
 * @al: the state
 *
 * Return: 1 to 255, or 0 without @al.
 */
__must_check uint8_t omci_alarm_sequence(const struct omci_alarm *al);

#endif
