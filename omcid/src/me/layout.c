// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The row layout of layout.h. */

#include "me/layout.h"

size_t omci_me_attr_offset(const struct omci_me_class *cls, uint8_t attr_no)
{
	const struct omci_me_attr *target = omci_me_attr_get(cls, attr_no);

	if (target == NULL || omci_me_attr_is_table(target))
		return OMCI_ME_NO_OFFSET;

	size_t off = 0;

	for (uint8_t n = 1; n < attr_no; n++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, n);

		if (!omci_me_attr_is_table(a))
			off += a->size;
	}
	return off;
}

size_t omci_me_row_size(const struct omci_me_class *cls)
{
	if (cls == NULL)
		return 0;

	size_t size = 0;

	for (uint8_t n = 1; n <= cls->attr_count; n++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, n);

		if (!omci_me_attr_is_table(a))
			size += a->size;
	}
	return size;
}
