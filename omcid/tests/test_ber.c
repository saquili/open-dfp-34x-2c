// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the bit error rate: the history of BIP errors and the verdicts. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "omci/ber.h"

#include "harness.h"

static struct omci_ber g_b;

/* Feed @n seconds from @sec on, each adding @per errors to @total. */
static uint64_t feed(uint64_t sec, unsigned int n, uint64_t total, uint64_t per)
{
	for (unsigned int i = 0; i < n; i++) {
		total += per;
		omci_ber_sample(&g_b, sec + i, total);
	}
	return total;
}

static void test_errors_need_two_samples(void)
{
	uint64_t e = 99u;

	omci_ber_reset(&g_b);
	CHECK(!omci_ber_errors(&g_b, 1u, &e));

	omci_ber_sample(&g_b, 100u, 5u);
	CHECK(!omci_ber_errors(&g_b, 1u, &e));

	omci_ber_sample(&g_b, 101u, 12u);
	CHECK(omci_ber_errors(&g_b, 1u, &e));
	CHECK_EQ(e, 7u);
	CHECK(!omci_ber_errors(&g_b, 2u, &e));
	CHECK(!omci_ber_errors(&g_b, 0u, &e));
}

static void test_a_later_sample_in_the_same_second_replaces(void)
{
	uint64_t e = 0;

	omci_ber_reset(&g_b);
	omci_ber_sample(&g_b, 10u, 0u);
	omci_ber_sample(&g_b, 11u, 3u);
	omci_ber_sample(&g_b, 11u, 9u);
	CHECK(omci_ber_errors(&g_b, 1u, &e));
	CHECK_EQ(e, 9u);
}

static void test_skipped_seconds_had_no_error(void)
{
	uint64_t e = 0;

	omci_ber_reset(&g_b);
	omci_ber_sample(&g_b, 10u, 100u);
	omci_ber_sample(&g_b, 14u, 140u);

	CHECK(omci_ber_errors(&g_b, 1u, &e));
	CHECK_EQ(e, 40u);
	CHECK(omci_ber_errors(&g_b, 4u, &e));
	CHECK_EQ(e, 40u);
	CHECK(!omci_ber_errors(&g_b, 5u, &e));
}

static void test_the_history_starts_again(void)
{
	uint64_t e = 0;

	/* A total that falls: the receiver came up again. */
	omci_ber_reset(&g_b);
	omci_ber_sample(&g_b, 10u, 100u);
	omci_ber_sample(&g_b, 11u, 120u);
	omci_ber_sample(&g_b, 12u, 4u);
	CHECK(!omci_ber_errors(&g_b, 1u, &e));
	omci_ber_sample(&g_b, 13u, 6u);
	CHECK(omci_ber_errors(&g_b, 1u, &e));
	CHECK_EQ(e, 2u);

	/* A clock that went back. */
	omci_ber_sample(&g_b, 5u, 6u);
	CHECK(!omci_ber_errors(&g_b, 1u, &e));

	/* A gap longer than the history. */
	omci_ber_sample(&g_b, 6u, 7u);
	omci_ber_sample(&g_b, 6u + OMCI_BER_HISTORY + 1u, 8u);
	CHECK(!omci_ber_errors(&g_b, 1u, &e));

	/* A gap as long as the history is still filled in. */
	omci_ber_reset(&g_b);
	omci_ber_sample(&g_b, 0u, 0u);
	omci_ber_sample(&g_b, OMCI_BER_HISTORY, 50u);
	CHECK(omci_ber_errors(&g_b, OMCI_BER_HISTORY, &e));
	CHECK_EQ(e, 50u);
}

static void test_the_ring_wraps(void)
{
	uint64_t e = 0;

	omci_ber_reset(&g_b);
	(void)feed(0u, 3u * OMCI_BER_HISTORY, 0u, 2u);
	CHECK(omci_ber_errors(&g_b, OMCI_BER_HISTORY, &e));
	CHECK_EQ(e, 2u * OMCI_BER_HISTORY);
	CHECK(omci_ber_errors(&g_b, 7u, &e));
	CHECK_EQ(e, 14u);
	CHECK(!omci_ber_errors(&g_b, OMCI_BER_HISTORY + 1u, &e));
}

static void test_signal_fail_at_its_default(void)
{
	/* 10^-5 over a second of 2488320000 bits is 24883.2 errors. */
	omci_ber_reset(&g_b);
	uint64_t t = feed(0u, 2u, 0u, 24883u);

	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(!g_b.sf);

	t = feed(2u, 1u, t, 24884u);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(g_b.sf);

	/* It holds while the rate is a tenth of the threshold or more ... */
	t = feed(3u, 1u, t, 2489u);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(g_b.sf);

	/* ... and clears below that. */
	t = feed(4u, 1u, t, 2488u);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(!g_b.sf);
	(void)t;
}

static void test_signal_degrade_at_its_default(void)
{
	/* 10^-9 is judged over 10 s: 24.9 errors. */
	omci_ber_reset(&g_b);
	uint64_t t = feed(0u, 11u, 0u, 0u);

	t += 24u;
	omci_ber_sample(&g_b, 11u, t);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(!g_b.sd);

	t += 1u;
	omci_ber_sample(&g_b, 12u, t);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(g_b.sd);
	CHECK(!g_b.sf);

	/* Clearing judges 10^-10 over 100 s: not before that much history. */
	t = feed(13u, 80u, t, 0u);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(g_b.sd);

	/*
	 * The window of the second 110 still holds both seconds of the burst;
	 * that of the second 111 only the error of the second 12.
	 */
	t = feed(93u, 18u, t, 0u);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(g_b.sd);
	t = feed(111u, 1u, t, 0u);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(!g_b.sd);
	(void)t;
}

static void test_the_most_sensitive_thresholds(void)
{
	/*
	 * 25 errors in one second, after 100 quiet ones: signal fail at 10^-8,
	 * judged over a second, and signal degrade at 10^-10, over 100 s.
	 */
	omci_ber_reset(&g_b);
	uint64_t t = feed(0u, 101u, 0u, 0u);

	t = feed(101u, 1u, t, 25u);
	omci_ber_judge(&g_b, 8u, 10u);
	CHECK(g_b.sf);
	CHECK(g_b.sd);

	/* Signal fail clears once 10 s hold fewer than 25 errors. */
	t = feed(102u, 9u, t, 0u);
	omci_ber_judge(&g_b, 8u, 10u);
	CHECK(g_b.sf);
	t = feed(111u, 1u, t, 0u);
	omci_ber_judge(&g_b, 8u, 10u);
	CHECK(!g_b.sf);

	/* Signal degrade once 1000 s do, which takes that much history. */
	CHECK(g_b.sd);
	t = feed(112u, OMCI_BER_HISTORY, t, 0u);
	omci_ber_judge(&g_b, 8u, 10u);
	CHECK(!g_b.sd);
	(void)t;
}

static void test_the_least_sensitive_threshold(void)
{
	/* Signal fail at 10^-3: 2488320 errors in a second. */
	omci_ber_reset(&g_b);
	uint64_t t = feed(0u, 2u, 0u, 2488319u);

	omci_ber_judge(&g_b, 3u, 4u);
	CHECK(!g_b.sf);
	CHECK(g_b.sd);
	t = feed(2u, 1u, t, 2488320u);
	omci_ber_judge(&g_b, 3u, 4u);
	CHECK(g_b.sf);
	(void)t;
}

static void test_thresholds_out_of_range_take_the_defaults(void)
{
	static const uint8_t bad_sf[] = { 0u, 2u, 9u, 255u };
	static const uint8_t bad_sd[] = { 0u, 3u, 11u, 255u };

	for (unsigned int i = 0; i < sizeof(bad_sf); i++) {
		/* 24884 errors a second: 10^-5, the default signal fail. */
		omci_ber_reset(&g_b);
		(void)feed(0u, 11u, 0u, 24884u);
		omci_ber_judge(&g_b, bad_sf[i], bad_sd[i]);
		CHECK(g_b.sf);
		CHECK(g_b.sd);

		/* 2 errors a second: 8e-10, below 10^-9, the default degrade. */
		omci_ber_reset(&g_b);
		(void)feed(0u, 11u, 0u, 2u);
		omci_ber_judge(&g_b, bad_sf[i], bad_sd[i]);
		CHECK(!g_b.sf);
		CHECK(!g_b.sd);
	}
}

static void test_a_reset_clears_the_verdicts(void)
{
	omci_ber_reset(&g_b);
	(void)feed(0u, 11u, 0u, 30000u);
	omci_ber_judge(&g_b, 5u, 9u);
	CHECK(g_b.sf);
	CHECK(g_b.sd);

	omci_ber_reset(&g_b);
	CHECK(!g_b.sf);
	CHECK(!g_b.sd);
	CHECK_EQ(g_b.count, 0u);
}

int main(void)
{
	RUN(test_errors_need_two_samples);
	RUN(test_a_later_sample_in_the_same_second_replaces);
	RUN(test_skipped_seconds_had_no_error);
	RUN(test_the_history_starts_again);
	RUN(test_the_ring_wraps);
	RUN(test_signal_fail_at_its_default);
	RUN(test_signal_degrade_at_its_default);
	RUN(test_the_most_sensitive_thresholds);
	RUN(test_the_least_sensitive_threshold);
	RUN(test_thresholds_out_of_range_take_the_defaults);
	RUN(test_a_reset_clears_the_verdicts);
	return OMCI_T_REPORT("test_ber");
}
