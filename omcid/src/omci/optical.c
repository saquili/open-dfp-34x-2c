// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The optical alarms and levels of the ANI-G. */

#include <stdbool.h>
#include <string.h>

#include "omci/optical.h"
#include "omci/test.h"

/* 30 dB, from a level referred to 1 uW to one referred to 1 mW. */
#define UW_TO_MW 15000

/* 3 dB, the step between two power levels of the transmitter. */
#define POWER_LEVEL_STEP 1500

/* 0.5 dB: the step of the thresholds, and the margin an alarm clears by. */
#define HALF_DB 250

/* The highest power level, the one the transmitter starts at. */
#define POWER_LEVEL_HIGH 2u

int16_t omci_optical_level(uint16_t tenths_uw)
{
	int16_t dbu = 0;

	if (!omci_test_power_dbu(tenths_uw, &dbu))
		return OMCI_OPTICAL_LEVEL_NONE;
	return (int16_t)(dbu - UW_TO_MW);
}

static bool applies(const struct omci_optical_limits *lim,
		    enum omci_hw_limit which)
{
	return (lim->set & (1u << (unsigned int)which)) != 0u;
}

static void put(struct omci_optical_limits *out, enum omci_hw_limit which,
		int32_t level)
{
	out->level[which] = level;
	out->set |= (uint8_t)(1u << (unsigned int)which);
}

/* A threshold the OLT sets, as a level: 0.5 dB a step. */
static void olt(struct omci_optical_limits *out, enum omci_hw_limit which,
		int32_t steps)
{
	put(out, which, steps * HALF_DB);
}

void omci_optical_limits(const struct omci_optical_thresholds *t,
			 const struct omci_hw_optics_limits *own,
			 const struct omci_hw_pon *pon,
			 struct omci_optical_limits *out)
{
	memset(out, 0, sizeof(*out));

	int32_t shift = 0;

	if (pon != NULL && pon->power_level_valid &&
	    pon->power_level <= POWER_LEVEL_HIGH)
		shift = (int32_t)(POWER_LEVEL_HIGH - pon->power_level) *
			POWER_LEVEL_STEP;

	for (unsigned int i = 0; own != NULL && i < OMCI_HW_LIMIT_COUNT; i++) {
		const enum omci_hw_limit which = (enum omci_hw_limit)i;

		if ((own->valid & (1u << i)) == 0u)
			continue;
		if (which == OMCI_HW_LIMIT_BIAS_HIGH) {
			out->bias_high = own->value[i];
			out->set |= (uint8_t)(1u << i);
			continue;
		}

		int32_t level = omci_optical_level(own->value[i]);

		if (which == OMCI_HW_LIMIT_TX_LOW ||
		    which == OMCI_HW_LIMIT_TX_HIGH)
			level -= shift;
		put(out, which, level);
	}

	if (t->rx_low != OMCI_OPTICAL_RX_POLICY)
		olt(out, OMCI_HW_LIMIT_RX_LOW, -(int32_t)t->rx_low);
	if (t->rx_high != OMCI_OPTICAL_RX_POLICY)
		olt(out, OMCI_HW_LIMIT_RX_HIGH, -(int32_t)t->rx_high);
	if (t->tx_low != OMCI_OPTICAL_TX_POLICY)
		olt(out, OMCI_HW_LIMIT_TX_LOW, (int8_t)t->tx_low);
	if (t->tx_high != OMCI_OPTICAL_TX_POLICY)
		olt(out, OMCI_HW_LIMIT_TX_HIGH, (int8_t)t->tx_high);
}

/* Below a low limit: declared under it, cleared 0.5 dB above it. */
static bool below(const struct omci_optical_limits *lim,
		  enum omci_hw_limit which, bool stands, int32_t level)
{
	if (!applies(lim, which))
		return false;
	return level < lim->level[which] + (stands ? HALF_DB : 0);
}

/* Above a high limit: declared over it, cleared 0.5 dB under it. */
static bool above(const struct omci_optical_limits *lim,
		  enum omci_hw_limit which, bool stands, int32_t level)
{
	if (!applies(lim, which))
		return false;
	return level > lim->level[which] - (stands ? HALF_DB : 0);
}

static bool got(const struct omci_hw_optics *o, enum omci_hw_optic which)
{
	return (o->valid & (1u << (unsigned int)which)) != 0u;
}

void omci_optical_judge(struct omci_optical *v, const struct omci_hw_optics *o,
			const struct omci_optical_limits *lim, bool operating)
{
	if (got(o, OMCI_HW_OPTIC_RX_POWER)) {
		const int32_t rx = omci_optical_level(o->rx_power);

		v->rx_low = below(lim, OMCI_HW_LIMIT_RX_LOW, v->rx_low, rx);
		v->rx_high = above(lim, OMCI_HW_LIMIT_RX_HIGH, v->rx_high, rx);
	}

	if (!operating) {
		v->tx_low = false;
		v->tx_high = false;
		v->bias_high = false;
		return;
	}

	if (got(o, OMCI_HW_OPTIC_TX_POWER)) {
		const int32_t tx = omci_optical_level(o->tx_power);

		v->tx_low = below(lim, OMCI_HW_LIMIT_TX_LOW, v->tx_low, tx);
		v->tx_high = above(lim, OMCI_HW_LIMIT_TX_HIGH, v->tx_high, tx);
	}

	if (got(o, OMCI_HW_OPTIC_BIAS)) {
		const uint32_t limit = lim->bias_high;
		const uint32_t margin = v->bias_high ? limit / 16u : 0u;

		v->bias_high = applies(lim, OMCI_HW_LIMIT_BIAS_HIGH) &&
			       o->bias > limit - margin;
	}
}
