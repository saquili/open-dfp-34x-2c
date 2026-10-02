/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * How a MIB upload reports an instance: its attributes, in order, cut into
 * units that each fit one MIB upload next response (G.988 A.3.16).
 */

#ifndef OMCI_ME_UPLOAD_H
#define OMCI_ME_UPLOAD_H

#include <stdalign.h>
#include <stdbool.h>

#include "me/table.h"
#include "omcid/attrs.h"

/* Room for values in a MIB upload next response, after class, instance and mask. */
#define OMCI_ME_UPLOAD_BYTES 26u

/* The most attributes a unit can carry. */
#define OMCI_ME_UPLOAD_MAX_ATTRS 16u

/**
 * struct omci_me_upload_unit - The attributes one response reports.
 * @mask: their attribute mask
 * @attrs: their numbers
 * @count: how many there are
 * @bytes: the size of their values together
 */
struct omci_me_upload_unit {
	alignas(4) uint16_t mask;
	uint8_t attrs[OMCI_ME_UPLOAD_MAX_ATTRS];
	uint8_t count;
	uint16_t bytes;
};

/**
 * omci_me_upload_includes() - Whether a MIB upload reports a class.
 * @cls: the class
 *
 * Return: false for the OMCI entity (class 287): it holds only table
 * attributes, which a MIB upload never carries (G.988 I.1.3.2).
 */
__must_check bool omci_me_upload_includes(const struct omci_me_class *cls);

/**
 * omci_me_upload_segments() - The units an instance of a class takes.
 * @cls: the class
 *
 * Return: at least 1: an instance with nothing to report still takes one
 * unit, with an empty mask.
 */
__must_check unsigned int
omci_me_upload_segments(const struct omci_me_class *cls);

/**
 * omci_me_upload_unit_at() - One unit of an instance of a class.
 * @cls: the class
 * @index: the unit, from 0
 * @out: the unit
 *
 * Return: false when @index is past the last unit.
 */
__must_check bool omci_me_upload_unit_at(const struct omci_me_class *cls,
					 unsigned int index,
					 struct omci_me_upload_unit *out);

#endif
