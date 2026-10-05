/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The attributes that take only some of the values their size can hold,
 * and those whose value the ONU changes on its own.
 */

#ifndef OMCI_ME_VALUES_H
#define OMCI_ME_VALUES_H

#include <stdbool.h>
#include <stdint.h>

#include "omcid/attrs.h"

/**
 * omci_me_value_allowed() - Whether an attribute takes a value.
 * @class_id: the class
 * @attr_no: the attribute
 * @value: the value, as many bytes as the attribute has
 *
 * Return: false for a value the attribute does not take; true for any
 * value of an attribute that takes every value its size holds.
 */
__must_check bool omci_me_value_allowed(uint16_t class_id, uint8_t attr_no,
					const uint8_t *value);

/**
 * omci_me_value_kept() - Whether an attribute keeps the value the OLT sets.
 * @class_id: the class
 * @attr_no: the attribute
 *
 * Return: false for an attribute the ONU changes on its own, which a Get
 * after a Set may find changed: alarm-reporting control, which ends by
 * itself (G.988 A.1.4.3), and the control block of the Ethernet frame
 * extended PM, whose clear bit is an action that reads 0 again (G.988
 * 9.3.32).
 */
__must_check bool omci_me_value_kept(uint16_t class_id, uint8_t attr_no);

#endif
