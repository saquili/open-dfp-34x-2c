/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The tests the OLT can ask the ONU to run, and their results. */

#ifndef OMCI_TEST_H
#define OMCI_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include "omci/frame.h"
#include "omcid/attrs.h"

/**
 * omci_test_run() - Run a test and build its result.
 * @class_id: the class the Test named
 * @instance: the instance
 * @select_test: the test asked for
 * @tci: the transaction identifier of the Test, which the result repeats
 * @priority: its channel
 * @out: the test result frame (G.988 A.3.39)
 *
 * Return: false when there is no result to send.
 */
__must_check bool omci_test_run(uint16_t class_id, uint16_t instance,
				uint8_t select_test, uint16_t tci,
				bool priority,
				uint8_t out[static OMCI_FRAME_LEN]);

/**
 * omci_test_power_dbu() - Convert an optical power for a test result.
 * @tenths_uw: the power, in units of 0.1 uW
 * @out: the power in dBuW, in units of 0.002 dB
 *
 * Return: false for a power of zero, which has no logarithm.
 */
__must_check bool omci_test_power_dbu(uint16_t tenths_uw, int16_t *out);

#endif
