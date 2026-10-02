/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Numbered sets of request frames for testing the engine. Each axis is a
 * list of cases, built on demand and labelled, so that a test or a sweep
 * can drive the engine through every case of the axis.
 */

#ifndef OMCI_TEST_CASES_H
#define OMCI_TEST_CASES_H

#include <stdbool.h>
#include <stdint.h>

#include "omci/frame.h"
#include "omcid/attrs.h"

/**
 * enum omci_case_axis - The sets of cases.
 * @OMCI_CASE_ENVELOPE: a valid Get with one field of the header or trailer
 *	changed: the type byte, the device identifier, the trailer, the
 *	integrity check, the transaction identifier or the class
 * @OMCI_CASE_MUTATE: every byte of three valid frames set to every value,
 *	with the integrity check recomputed
 * @OMCI_CASE_MSGTYPE: every message type with every combination of the
 *	request and answer bits, on a class the device has and one it lacks
 * @OMCI_CASE_MTCLASS: every message type on one instance of each class, on
 *	both channels
 * @OMCI_CASE_REACH: requests made from a MIB so that each one reaches the
 *	part of the engine it is meant for
 * @OMCI_CASE_POPULATED: requests made from every instance of a MIB
 * @OMCI_CASE_AXIS_COUNT: the number of axes
 */
enum omci_case_axis {

	OMCI_CASE_ENVELOPE = 0,

	OMCI_CASE_MUTATE,

	OMCI_CASE_MSGTYPE,

	OMCI_CASE_MTCLASS,

	OMCI_CASE_REACH,

	OMCI_CASE_POPULATED,

	OMCI_CASE_AXIS_COUNT,
};

struct omci_mib;

/**
 * omci_case_prepare() - Make the cases of an axis that depends on a MIB.
 * @axis: %OMCI_CASE_REACH or %OMCI_CASE_POPULATED; other axes need nothing
 * @mib: the MIB the cases are made from
 */
void omci_case_prepare(enum omci_case_axis axis, const struct omci_mib *mib);

/**
 * omci_case_axis_populated() - Whether an axis is made from a MIB.
 * @axis: the axis
 *
 * Return: true for an axis that omci_case_prepare() must fill.
 */
__must_check bool omci_case_axis_populated(enum omci_case_axis axis);

/**
 * omci_case_capacity() - The most cases an axis made from a MIB can hold.
 * @axis: the axis
 *
 * Return: the bound, or 0 for an axis not made from a MIB.
 */
__must_check uint32_t omci_case_capacity(enum omci_case_axis axis);

/**
 * omci_case_count() - The number of cases of an axis.
 * @axis: the axis
 *
 * Return: the number; 0 for an axis made from a MIB before it is prepared.
 */
__must_check uint32_t omci_case_count(enum omci_case_axis axis);

/**
 * omci_case_build() - Build one case.
 * @axis: the axis
 * @index: the case, from 0
 * @wire: the frame
 *
 * Return: false, with a valid Get in @wire, when @index is past the end.
 */
bool omci_case_build(enum omci_case_axis axis, uint32_t index,
		     uint8_t wire[static OMCI_FRAME_LEN]);

/**
 * omci_case_label() - Name one case, for a report.
 * @axis: the axis
 * @index: the case
 * @out: the label; empty when @index is past the end
 * @cap: room in @out
 */
void omci_case_label(enum omci_case_axis axis, uint32_t index, char *out,
		     size_t cap);

/**
 * omci_case_axis_name() - The name of an axis.
 * @axis: the axis
 *
 * Return: the name, or NULL for an axis that does not exist.
 */
__must_check const char *omci_case_axis_name(enum omci_case_axis axis);

/**
 * omci_case_axis_by_name() - Find an axis by name.
 * @name: the name
 *
 * Return: the axis, or %OMCI_CASE_AXIS_COUNT when there is none.
 */
__must_check enum omci_case_axis omci_case_axis_by_name(const char *name);

#endif
