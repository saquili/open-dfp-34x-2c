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
#include "omcid/attrs.h"
#include "omcid/status.h"

/* An alarm bitmap: 224 bits, alarm 0 in the most significant bit of byte 0. */
#define OMCI_ALARM_BITMAP_LEN 28u

/* The most entities that can carry an alarm at the same time. */
#define OMCI_ALARM_MAX_ENTITIES 8u

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
 * omci_alarm_poll() - Read the alarms of the hardware.
 * @al: the state
 * @changed: set when any bitmap changed since the last poll; may be NULL
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or the failure of the hardware read,
 * which leaves the state as it was.
 */
__must_check enum omci_status omci_alarm_poll(struct omci_alarm *al,
					      bool *changed);

/**
 * omci_alarm_snapshot() - Freeze the standing alarms for Get all alarms.
 * @al: the state
 *
 * Return: the number of entities with an alarm, which is the number of
 * Get all alarms next commands the OLT may send.
 */
uint16_t omci_alarm_snapshot(struct omci_alarm *al);

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
 * omci_alarm_changed() - The entities whose alarms changed at the last poll.
 * @al: the state
 * @out: the entities
 * @cap: room in @out
 *
 * Return: how many were written.
 */
size_t omci_alarm_changed(const struct omci_alarm *al,
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
