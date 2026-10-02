/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Performance monitoring: the Ethernet frame history data of the UNI,
 * downstream and upstream (classes 321 and 322), in intervals of fifteen
 * minutes, filled from the counters of the switch.
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

/**
 * omci_pm_owns() - Whether a class is one this module fills.
 * @class_id: the class
 *
 * Return: true when it is.
 */
__must_check bool omci_pm_owns(uint16_t class_id);

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
 * omci_pm_tick() - Close the interval when it is over.
 * @pm: the state
 * @now_sec: monotonic time in seconds
 *
 * Once the OLT has set the time of day, intervals end on the quarter hours
 * of that clock; before, they run from the first tick.
 *
 * Return: true when an interval closed.
 */
bool omci_pm_tick(struct omci_pm *pm, uint64_t now_sec);

/**
 * omci_pm_set_wall() - Follow the time of day.
 * @pm: the state
 * @wall_sec: seconds since 1970, or 0 when the time of day is unknown
 */
void omci_pm_set_wall(struct omci_pm *pm, uint64_t wall_sec);

/**
 * omci_pm_resync() - Start the intervals again from a new time of day.
 * @pm: the state
 * @wall_sec: seconds since 1970
 *
 * Called when the OLT synchronises the time: the current interval starts
 * on the quarter hour that holds @wall_sec.
 */
void omci_pm_resync(struct omci_pm *pm, uint64_t wall_sec);

/**
 * omci_pm_refresh() - Write the counts of an instance into the MIB.
 * @pm: the state
 * @class_id: the class
 * @instance: the instance
 * @current: true for the counts of the interval in progress (Get current
 *	data), false for those of the last closed one (Get)
 *
 * Return: %OMCI_OK; %OMCI_DECLINED when an instance read for the first time
 * has no counts yet; %OMCI_ERR_HW when a counter could not be read; the
 * error of a class or instance that is not there; %OMCI_ERR_NOMEM.
 */
__must_check enum omci_status omci_pm_refresh(struct omci_pm *pm,
					      uint16_t class_id,
					      uint16_t instance, bool current);

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
 * omci_pm_forget_all() - Stop counting for every instance, as a MIB reset does.
 * @pm: the state
 */
void omci_pm_forget_all(struct omci_pm *pm);

/**
 * omci_pm_port_of() - The switch port an instance counts.
 * @class_id: the class
 * @instance: the instance
 *
 * Return: the port; every instance counts the UNI.
 */
__must_check unsigned int omci_pm_port_of(uint16_t class_id, uint16_t instance);

#endif
