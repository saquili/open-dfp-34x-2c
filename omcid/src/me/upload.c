// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The upload units of upload.h. Attributes are taken in order and a unit
 * closes when the next value would not fit; table attributes are skipped.
 */

#include <limits.h>
#include <stdbool.h>

#include "me/upload.h"
#include "omci/attrmask.h"

#define ME_OMCI 287u

bool omci_me_upload_includes(const struct omci_me_class *cls)
{
	return cls != NULL && cls->class_id != ME_OMCI;
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

		if (a == NULL || omci_me_attr_is_table(a))
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
