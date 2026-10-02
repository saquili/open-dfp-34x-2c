/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Where an attribute's value lies in the row of an instance: the values
 * follow each other in attribute order. A table attribute takes no room in
 * the row; its rows are kept apart.
 */

#ifndef OMCI_ME_LAYOUT_H
#define OMCI_ME_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "me/table.h"
#include "omcid/attrs.h"

/* The offset of an attribute that has no place in the row. */
#define OMCI_ME_NO_OFFSET SIZE_MAX

/**
 * omci_me_attr_offset() - The offset of an attribute in the row.
 * @cls: the class
 * @attr_no: the attribute
 *
 * Return: the offset, or %OMCI_ME_NO_OFFSET for a table attribute or one
 * the class does not have.
 */
__must_check size_t omci_me_attr_offset(const struct omci_me_class *cls,
					uint8_t attr_no);

/**
 * omci_me_row_size() - The size of a row of the class.
 * @cls: the class
 *
 * Return: the bytes of all attributes that are not tables.
 */
__must_check size_t omci_me_row_size(const struct omci_me_class *cls);

#endif
