// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The upload units of upload.h. Attributes are taken in order and a unit
 * closes when the next value would not fit. A MIB upload carries neither
 * table attributes nor the measurements of a PM entity.
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <strings.h>

#include "me/upload.h"
#include "omci/attrmask.h"
#include "omcid/array_size.h"

/*
 * The classes whose instances a MIB upload never reports, each by the clause
 * of G.988 that defines it.
 */
static const uint16_t g_never_uploaded[] = {
	83u, /* 9.13.3, physical path termination point LCT UNI */
	149u, /* 9.9.19, SIP config portal */
	154u, /* 9.9.20, MGC config portal */
	158u, /* 9.1.12, ONU remote debug */
	287u, /* 9.12.8, OMCI */
	288u, /* 9.12.9, managed entity */
	289u, /* 9.12.10, attribute */
	303u, /* 9.3.23, Dot1ag MEP status */
	304u, /* 9.3.24, Dot1ag MEP CCM database */
	308u, /* 9.12.12, general purpose buffer */
	317u, /* 9.14.5, RE config portal */
};

/* The attribute of a PM entity that holds what the OLT set. */
#define PM_CONTROL_BLOCK 2u

bool omci_me_upload_includes(const struct omci_me_class *cls)
{
	if (cls == NULL)
		return false;
	for (size_t i = 0; i < ARRAY_SIZE(g_never_uploaded); i++)
		if (cls->class_id == g_never_uploaded[i])
			return false;
	return true;
}

/*
 * A PM entity begins with the end time of its interval. Of its attributes a
 * MIB upload reports only the control block, which in classical PM points at
 * the threshold data; the rest are measurements (G.988 I.4).
 */
static bool is_pm(const struct omci_me_class *cls)
{
	const struct omci_me_attr *a = omci_me_attr_get(cls, 1);

	return a != NULL && a->name != NULL &&
	       strcasecmp(a->name, "Interval end time") == 0;
}

static bool reported(const struct omci_me_class *cls, uint8_t n,
		     const struct omci_me_attr *a)
{
	if (a == NULL || omci_me_attr_is_table(a))
		return false;
	return !is_pm(cls) || n == PM_CONTROL_BLOCK;
}

#define ANY_UNIT UINT_MAX

/*
 * Cut the attributes of @cls into units, keeping unit @want in @out.
 *
 * Return: the number of units.
 */
static unsigned int pack(const struct omci_me_class *cls, unsigned int want,
			 struct omci_me_upload_unit *out)
{
	struct omci_me_upload_unit unit = { 0 };
	unsigned int closed = 0;

	for (uint8_t n = 1; n <= cls->attr_count; n++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, n);

		if (!reported(cls, n, a))
			continue;

		if (unit.bytes + a->size <= OMCI_ME_UPLOAD_BYTES &&
		    unit.count < OMCI_ME_UPLOAD_MAX_ATTRS) {
			if (closed == want) {
				unit.attrs[unit.count] = n;
				unit.mask |= omci_attr_bit(n);
			}
			unit.count++;
			unit.bytes = (uint16_t)(unit.bytes + a->size);
			continue;
		}

		if (closed == want && out != NULL)
			*out = unit;
		closed++;

		unit = (struct omci_me_upload_unit){ 0 };
		if (closed == want) {
			unit.attrs[0] = n;
			unit.mask = omci_attr_bit(n);
		}
		unit.count = 1;
		unit.bytes = a->size;
	}

	if (unit.count != 0) {
		if (closed == want && out != NULL)
			*out = unit;
		closed++;
	}

	if (closed == 0) {
		if (want == 0 && out != NULL)
			*out = (struct omci_me_upload_unit){ 0 };
		return 1;
	}
	return closed;
}

unsigned int omci_me_upload_segments(const struct omci_me_class *cls)
{
	if (cls == NULL)
		return 1;
	return pack(cls, ANY_UNIT, NULL);
}

bool omci_me_upload_unit_at(const struct omci_me_class *cls, unsigned int index,
			    struct omci_me_upload_unit *out)
{
	if (cls == NULL || out == NULL)
		return false;

	*out = (struct omci_me_upload_unit){ 0 };
	return pack(cls, index, out) > index;
}
