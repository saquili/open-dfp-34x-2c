/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The optical alarms of the ANI-G (G.988 9.2.1), judged from the readings
 * of the transceiver against the thresholds the OLT sets, or the ONU's own
 * where the OLT leaves them to it, and the optical levels the ANI-G reports.
 */

#ifndef OMCI_OPTICAL_H
#define OMCI_OPTICAL_H

#include <stdbool.h>
#include <stdint.h>

#include "hw/hw.h"
#include "omcid/attrs.h"

/* The threshold values that leave a limit to the ONU's own policy. */
#define OMCI_OPTICAL_RX_POLICY 0xFFu
#define OMCI_OPTICAL_TX_POLICY 0x81u

/* The level an optical level attribute gives no light: its lowest. */
#define OMCI_OPTICAL_LEVEL_NONE INT16_MIN

/**
 * struct omci_optical_thresholds - The optical thresholds of the ANI-G.
 * @rx_low: the lower optical threshold: 0 to 254 for 0 to -127 dBm in
 *	steps of 0.5 dB, or OMCI_OPTICAL_RX_POLICY
 * @rx_high: the upper optical threshold, likewise
 * @tx_low: the lower transmit power threshold: dBm in steps of 0.5 dB, two's
 *	complement, or OMCI_OPTICAL_TX_POLICY
 * @tx_high: the upper transmit power threshold, likewise
 */
struct omci_optical_thresholds {
	uint8_t rx_low;
	uint8_t rx_high;
	uint8_t tx_low;
	uint8_t tx_high;
};

/**
 * struct omci_optical_limits - What the readings are judged against.
 * @level: the limits of the powers, by &enum omci_hw_limit, as levels in
 *	units of 0.002 dB referred to 1 mW; the entry of the bias is unused
 * @bias_high: the high limit of the laser bias current, in 2 uA
 * @set: bit N set when limit N of &enum omci_hw_limit applies
 */
struct omci_optical_limits {
	int32_t level[OMCI_HW_LIMIT_COUNT];
	uint16_t bias_high;
	uint8_t set;
};

/**
 * struct omci_optical - The optical alarms that stand.
 * @rx_low: low received optical power
 * @rx_high: high received optical power
 * @tx_low: low transmit optical power
 * @tx_high: high transmit optical power
 * @bias_high: laser bias current above its limit
 */
struct omci_optical {
	bool rx_low;
	bool rx_high;
	bool tx_low;
	bool tx_high;
	bool bias_high;
};

/**
 * omci_optical_level() - An optical power as the ANI-G gives its levels.
 * @tenths_uw: the power, in units of 0.1 uW
 *
 * Return: the level, in units of 0.002 dB referred to 1 mW, or
 * OMCI_OPTICAL_LEVEL_NONE for no power.
 */
__must_check int16_t omci_optical_level(uint16_t tenths_uw);

/**
 * omci_optical_limits() - Work out the limits of the optical alarms.
 * @t: the thresholds of the ANI-G
 * @own: the transceiver's own alarm thresholds, or NULL without them
 * @pon: the state of the receiver, or NULL when it is not known
 * @out: the limits
 *
 * A threshold the OLT has set is the limit. One it leaves to the ONU is the
 * transceiver's alarm threshold for that reading; those of the transmitted
 * power follow the power level the OLT has set, 3 dB lower for each level
 * below the highest. The bias has only the transceiver's high threshold.
 * A limit that has neither does not apply.
 */
void omci_optical_limits(const struct omci_optical_thresholds *t,
			 const struct omci_hw_optics_limits *own,
			 const struct omci_hw_pon *pon,
			 struct omci_optical_limits *out);

/**
 * omci_optical_judge() - Declare or clear the optical alarms.
 * @v: the alarms, as the last judgement left them
 * @o: the readings of the transceiver
 * @lim: the limits
 * @operating: the ONU is in operation and transmits
 *
 * An alarm is declared when its reading passes its limit, and cleared once
 * the reading is back inside it by 0.5 dB, or by a sixteenth for the bias,
 * so that a reading that sits on the limit does not make it come and go.
 * The transmitter's alarms are judged only in operation, and clear out of
 * it. A reading that is not there leaves its alarms as they were.
 */
void omci_optical_judge(struct omci_optical *v, const struct omci_hw_optics *o,
			const struct omci_optical_limits *lim, bool operating);

#endif
