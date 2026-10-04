/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The bit error rate of the downstream, judged from the count of its BIP
 * errors against the signal fail and signal degrade thresholds of the ANI-G
 * (G.988 9.2.1).
 */

#ifndef OMCI_BER_H
#define OMCI_BER_H

#include <stdbool.h>
#include <stdint.h>

#include "omcid/attrs.h"

/*
 * The seconds of history kept: the window in which a signal degrade at the
 * most sensitive threshold, 10^-10, is cleared, which judges 10^-11.
 */
#define OMCI_BER_HISTORY 1000u

/* The thresholds the ANI-G allows, as the exponent k of a rate of 10^-k. */
#define OMCI_BER_SF_MIN 3u
#define OMCI_BER_SF_MAX 8u
#define OMCI_BER_SD_MIN 4u
#define OMCI_BER_SD_MAX 10u

/* The thresholds the ANI-G starts with. */
#define OMCI_BER_SF_DEFAULT 5u
#define OMCI_BER_SD_DEFAULT 9u

/**
 * struct omci_ber - The BIP error totals of the last seconds, and the verdicts.
 * @total: the total at the end of each second, a ring
 * @head: the slot of the latest second
 * @count: the seconds held, up to OMCI_BER_HISTORY + 1
 * @sec: the latest second
 * @sf: signal fail stands
 * @sd: signal degrade stands
 */
struct omci_ber {
	uint64_t total[OMCI_BER_HISTORY + 1u];
	unsigned int head;
	unsigned int count;
	uint64_t sec;
	bool sf;
	bool sd;
};

/**
 * omci_ber_reset() - Forget the history and clear both verdicts.
 * @b: the state
 */
void omci_ber_reset(struct omci_ber *b);

/**
 * omci_ber_sample() - Take the total of BIP errors at a second.
 * @b: the state
 * @sec: the second, of a clock that never goes back
 * @total: the errors counted since the receiver came up
 *
 * A second skipped is taken to have had no error, and a later sample in the
 * same second replaces the earlier. A total that falls, which a receiver
 * that came up again does, or a gap longer than the history, starts the
 * history again from this sample; the verdicts are kept.
 */
void omci_ber_sample(struct omci_ber *b, uint64_t sec, uint64_t total);

/**
 * omci_ber_errors() - The errors counted in the last seconds.
 * @b: the state
 * @secs: the seconds, 1 to OMCI_BER_HISTORY
 * @out: the errors
 *
 * Return: false when the history is shorter than @secs.
 */
__must_check bool omci_ber_errors(const struct omci_ber *b, unsigned int secs,
				  uint64_t *out);

/**
 * omci_ber_judge() - Declare or clear signal fail and signal degrade.
 * @b: the state
 * @sf: the signal fail threshold, the k of 10^-k; one the ANI-G does not
 *	allow is taken as OMCI_BER_SF_DEFAULT
 * @sd: the signal degrade threshold, likewise with OMCI_BER_SD_DEFAULT
 *
 * A verdict is declared when the rate reaches its threshold, and cleared
 * once it has fallen below a tenth of it, the improvement by an order of
 * magnitude G.988 9.2.1 notes as industry practice. Each rate is measured
 * over a window long enough for about 25 errors at the rate judged: one
 * second down to 10^-8, ten times longer for each decade below.
 */
void omci_ber_judge(struct omci_ber *b, uint8_t sf, uint8_t sd);

#endif
