/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Performance monitoring (G.988 I.4): the history data entities of the
 * Ethernet UNI (classes 24, 89 and 296), of the MAC bridge ports (52, 321
 * and 322), of the GEM ports (341) and of the downstream FEC (312), and the
 * Ethernet frame extended PM (334), filled from the counters of the
 * hardware in intervals of fifteen minutes, with the threshold crossing
 * alerts their threshold data (273 and 274) ask for.
 *
 * Every instance has two bins: the counts of the interval in progress,
 * which Get current data reads, and those of the interval that closed last,
 * which Get reads. A count stays at the most its attribute holds rather than
 * wrap. The intervals are the ONU's own: they end every fifteen minutes from
 * the start of the daemon, or from where Synchronize time puts the
 * boundary, and are numbered from 0 there.
 */

#ifndef OMCI_ME_PM_H
#define OMCI_ME_PM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mib/store.h"
#include "omcid/attrs.h"
#include "omcid/status.h"

/* The length of an interval. */
#define OMCI_PM_INTERVAL_SEC 900u

/* The alarm numbers of the threshold crossing alerts are below this. */
#define OMCI_PM_TCA_BITS 16u

/* The most instances counted at once. */
#define OMCI_PM_MAX_INSTANCES 64u

/**
 * omci_pm_owns() - Whether a class is one this module fills.
 * @class_id: the class
 *
 * Return: true when it is.
 */
__must_check bool omci_pm_owns(uint16_t class_id);

/**
 * omci_pm_class_at() - The classes this module fills, one by one.
 * @i: the place, from 0
 *
 * Return: the class, or 0 past the last.
 */
__must_check uint16_t omci_pm_class_at(size_t i);

struct omci_pm;

/**
 * omci_pm_init() - Set up performance monitoring; one per process.
 * @out: the state
 * @mib: the MIB it fills
 *
 * Return: %OMCI_OK, %OMCI_ERR_INVAL, or %OMCI_ERR_BUSY while one exists.
 */
__must_check enum omci_status omci_pm_init(struct omci_pm **out,
					   struct omci_mib *mib);
void omci_pm_fini(struct omci_pm *pm);

/**
 * omci_pm_tick() - Count, and close the interval when it is over.
 * @pm: the state
 * @now_sec: monotonic time in seconds
 *
 * Once a second it takes the counters of the hardware into every instance
 * the MIB holds, starting to count for an instance it has not seen and
 * forgetting one the MIB no longer has. When the interval is over, the
 * counts of each instance become its history and start again from zero,
 * and every threshold crossing alert clears; otherwise each count is held
 * against its threshold, and an alert is raised for one that reached it.
 * The first tick starts the first interval.
 *
 * Return: true when an interval closed.
 */
bool omci_pm_tick(struct omci_pm *pm, uint64_t now_sec);

/**
 * omci_pm_synchronize() - Start the intervals again (Synchronize time).
 * @pm: the state
 * @now_sec: monotonic time in seconds
 * @offset_sec: how far into its interval the OLT's time is, 0 to 899: the
 *	seconds since the last quarter hour of the time it gave, or 0
 *
 * Every count and every history goes back to zero, every threshold
 * crossing alert clears, the interval end time becomes 0, and the interval
 * in progress ends when the OLT's time reaches the next quarter hour (G.988
 * 9.1.1, A.3.33). An extended PM instance that counts continuously is left
 * as it is.
 */
void omci_pm_synchronize(struct omci_pm *pm, uint64_t now_sec,
			 unsigned int offset_sec);

/**
 * struct omci_pm_schedule - Where the intervals stand.
 * @started: whether an interval runs
 * @next_end: the monotonic second the interval in progress ends
 * @interval_end_time: the number of the last interval that closed
 */
struct omci_pm_schedule {
	bool started;
	uint64_t next_end;
	uint8_t interval_end_time;
};

/**
 * omci_pm_schedule_get() - Where the intervals stand.
 * @pm: the state
 * @out: the schedule
 */
void omci_pm_schedule_get(const struct omci_pm *pm,
			  struct omci_pm_schedule *out);

/**
 * omci_pm_schedule_set() - Have the intervals go on where they stood.
 * @pm: the state
 * @in: the schedule, as omci_pm_schedule_get() gave it
 *
 * The counts start from zero; the intervals end when they did before, as
 * Synchronize time placed them.
 */
void omci_pm_schedule_set(struct omci_pm *pm,
			  const struct omci_pm_schedule *in);

/**
 * omci_pm_changed() - Take what the OLT created or set.
 * @pm: the state
 * @class_id: the class
 * @instance: the instance
 *
 * An instance just created starts counting from zero. An extended PM
 * instance whose control block asks for its counts to be cleared has them
 * cleared, and its alerts, and the request bit reads 0 again.
 */
void omci_pm_changed(struct omci_pm *pm, uint16_t class_id, uint16_t instance);

/**
 * omci_pm_refresh() - Write the counts of an instance into the MIB.
 * @pm: the state
 * @class_id: the class
 * @instance: the instance
 * @current: true for the counts of the interval in progress (Get current
 *	data), false for those of the last closed one (Get)
 *
 * For Get current data the counters are read first.
 *
 * Return: %OMCI_OK; %OMCI_ERR_HW when a counter could not be read, the
 * counts written being the last known; the error of a class or instance
 * that is not there; %OMCI_ERR_NOMEM when no more instances can be counted.
 */
__must_check enum omci_status omci_pm_refresh(struct omci_pm *pm,
					      uint16_t class_id,
					      uint16_t instance, bool current);

/**
 * omci_pm_tca() - The instances of a class under a threshold crossing alert.
 * @pm: the state
 * @class_id: the class
 * @bit: the alert's alarm number
 * @out: the instances
 * @cap: room in @out
 *
 * Return: how many were written.
 */
size_t omci_pm_tca(const struct omci_pm *pm, uint16_t class_id, uint8_t bit,
		   uint16_t *out, size_t cap);

/**
 * omci_pm_counter_accounted() - Whether a counter of the switch is accounted for.
 * @idx: the counter
 * @why: what it feeds, or why it feeds nothing; may be NULL
 *
 * Return: true when the counter feeds an attribute or is known not to.
 */
__must_check bool omci_pm_counter_accounted(unsigned int idx, const char **why);

/**
 * omci_pm_forget() - Stop counting for an instance.
 * @pm: the state
 * @class_id: the class
 * @instance: the instance
 */
void omci_pm_forget(struct omci_pm *pm, uint16_t class_id, uint16_t instance);

/**
 * omci_pm_forget_all() - Stop counting for every instance, as a MIB reset
 *	does; the intervals go on.
 * @pm: the state
 */
void omci_pm_forget_all(struct omci_pm *pm);

#endif
