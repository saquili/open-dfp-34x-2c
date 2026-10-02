// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of performance monitoring. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ME_ONU_DATA 2u

#include "hw/hw.h"
#include "me/pm.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "omci/frame.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

#define ME_PM_DS 321u
#define ME_PM_US 322u
#define UNI_INSTANCE 0x0101u

#define IF_IN_OCTETS 0u
#define IF_IN_UCAST 1u
#define IF_IN_MULTICAST 2u
#define IF_IN_BROADCAST 3u
#define IF_OUT_OCTETS 5u
#define IF_OUT_DISCARDS 6u
#define IF_OUT_UCAST 7u
#define IF_OUT_MULTICAST 8u
#define IF_OUT_BROADCAST 9u
#define ETHER_CRC_ALIGN 35u

static unsigned int uni_port(void)
{
	return omci_pm_port_of(ME_PM_US, UNI_INSTANCE);
}

static void hw_zero(void)
{
	for (unsigned int i = 0; i < 70u; i++)
		omci_hw_counter_force(uni_port(), i, 0);
}

static uint32_t attr32(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		       uint8_t no)
{
	uint8_t buf[8];
	size_t len = 0;

	if (omci_mib_attr_get(mib, cls, inst, no, buf, sizeof(buf), &len) !=
		    OMCI_OK ||
	    len < 4)
		return 0xDEADBEEFu;
	return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
	       ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static void ensure_row(struct omci_mib *mib, uint16_t cls)
{
	(void)!omci_mib_create(mib, cls, UNI_INSTANCE);
}

static void get_current(struct omci_engine *eng, uint16_t cls, uint16_t tid)
{
	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, tid, 28u, true, false, cls, UNI_INSTANCE);
	(void)!omci_t_rx(eng, req);
}

static void test_a_counter_reaches_its_attribute(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	struct omci_pm *pm = NULL;

	CHECK_EQ(omci_pm_init(&pm, fx.mib), OMCI_ERR_BUSY);

	ensure_row(fx.mib, ME_PM_US);
	hw_zero();

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 9000000000ull);
	get_current(fx.eng, ME_PM_US, 0x0800u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS,
			      9000000000ull + 123456u);
	omci_hw_counter_force(uni_port(), IF_IN_BROADCAST, 77u);
	get_current(fx.eng, ME_PM_US, 0x0801u);

	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 123456u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 6u), 77u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_downstream_and_upstream_are_not_swapped(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	ensure_row(fx.mib, ME_PM_DS);
	ensure_row(fx.mib, ME_PM_US);

	hw_zero();
	get_current(fx.eng, ME_PM_US, 0x0900u);
	get_current(fx.eng, ME_PM_DS, 0x0901u);

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 1000u);
	omci_hw_counter_force(uni_port(), IF_OUT_OCTETS, 2000u);
	get_current(fx.eng, ME_PM_US, 0x0902u);
	get_current(fx.eng, ME_PM_DS, 0x0903u);

	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 1000u);
	CHECK_EQ(attr32(fx.mib, ME_PM_DS, UNI_INSTANCE, 4u), 2000u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_a_summed_attribute(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	ensure_row(fx.mib, ME_PM_US);

	hw_zero();
	get_current(fx.eng, ME_PM_US, 0x0A00u);

	omci_hw_counter_force(uni_port(), IF_IN_UCAST, 100u);
	omci_hw_counter_force(uni_port(), IF_IN_MULTICAST, 10u);
	omci_hw_counter_force(uni_port(), IF_IN_BROADCAST, 20u);
	get_current(fx.eng, ME_PM_US, 0x0A01u);

	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 5u), 130u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 7u), 10u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 6u), 20u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_the_arithmetic_is_64_bit(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	ensure_row(fx.mib, ME_PM_US);
	hw_zero();

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 0xFFFFFFF0ull);
	get_current(fx.eng, ME_PM_US, 0x0B00u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 0x100000010ull);
	get_current(fx.eng, ME_PM_US, 0x0B01u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0x20u);

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 0x1000000D0ull);
	get_current(fx.eng, ME_PM_US, 0x0B02u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0xE0u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_get_reports_the_previous_interval(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	ensure_row(fx.mib, ME_PM_US);
	hw_zero();

	struct omci_pm *pm = NULL;

	omci_t_down(&fx);
	CHECK(omci_t_up(&fx));
	ensure_row(fx.mib, ME_PM_US);
	CHECK_EQ(omci_pm_init(&pm, fx.mib), OMCI_ERR_BUSY);

	uint8_t req[OMCI_FRAME_LEN];

	get_current(fx.eng, ME_PM_US, 0x0C00u);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 500u);
	get_current(fx.eng, ME_PM_US, 0x0C01u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 500u);

	omci_t_build(req, 0x0C02u, 9u, true, false, ME_PM_US, UNI_INSTANCE);
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_the_interval_rolls_at_fifteen_minutes(void)
{
	struct omci_mib *mib = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	struct omci_pm *pm = NULL;

	CHECK_EQ(omci_pm_init(&pm, mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ME_PM_US, UNI_INSTANCE), OMCI_OK);

	hw_zero();
	CHECK(!omci_pm_tick(pm, 1000u));
	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, true), OMCI_OK);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 800u);

	CHECK(!omci_pm_tick(pm, 1000u + OMCI_PM_INTERVAL_SEC - 1u));
	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, true), OMCI_OK);
	CHECK_EQ(attr32(mib, ME_PM_US, UNI_INSTANCE, 4u), 800u);

	CHECK(omci_pm_tick(pm, 1000u + OMCI_PM_INTERVAL_SEC));

	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, false), OMCI_OK);
	CHECK_EQ(attr32(mib, ME_PM_US, UNI_INSTANCE, 4u), 800u);
	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, true), OMCI_OK);
	CHECK_EQ(attr32(mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);

	uint8_t end = 0;
	size_t len = 0;

	CHECK_EQ(omci_mib_attr_get(mib, ME_PM_US, UNI_INSTANCE, 1u, &end, 1,
				   &len),
		 OMCI_OK);
	CHECK_EQ(end, 1u);

	hw_zero();
	omci_pm_fini(pm);
	omci_mib_fini(mib);
}

static void test_an_attribute_without_a_source_reads_zero(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	ensure_row(fx.mib, ME_PM_US);
	ensure_row(fx.mib, ME_PM_DS);
	hw_zero();
	get_current(fx.eng, ME_PM_US, 0x0D00u);
	get_current(fx.eng, ME_PM_DS, 0x0D01u);
	omci_hw_counter_force(uni_port(), ETHER_CRC_ALIGN, 42u);
	get_current(fx.eng, ME_PM_US, 0x0D02u);
	get_current(fx.eng, ME_PM_DS, 0x0D03u);

	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 8u), 42u);
	CHECK_EQ(attr32(fx.mib, ME_PM_DS, UNI_INSTANCE, 8u), 0u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_the_surface(void)
{
	CHECK(omci_pm_owns(ME_PM_US));
	CHECK(omci_pm_owns(ME_PM_DS));
	CHECK(!omci_pm_owns(24u));
	CHECK(!omci_pm_owns(263u));
	CHECK(!omci_pm_owns(171u));

	CHECK_EQ(omci_pm_port_of(ME_PM_US, 0x0101u), 0u);
	CHECK_EQ(omci_pm_port_of(ME_PM_DS, 0x0102u), 0u);
	CHECK(omci_pm_port_of(ME_PM_US, 0x0101u) != 1u);
	CHECK(omci_pm_port_of(ME_PM_US, 0x0101u) != 2u);

	omci_engine_tick(NULL, 0);
	omci_pm_forget(NULL, 0, 0);
	omci_pm_forget_all(NULL);
	omci_pm_forget(NULL, ME_PM_US, UNI_INSTANCE);

	CHECK_EQ(omci_pm_init(NULL, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_pm_refresh(NULL, ME_PM_US, 0, true), OMCI_ERR_INVAL);
	CHECK(!omci_pm_tick(NULL, 0));
	omci_pm_set_wall(NULL, 0);
	omci_pm_set_wall(NULL, 1786797296ull);
	omci_pm_resync(NULL, 0);
	omci_pm_resync(NULL, 1786797296ull);
	omci_pm_fini(NULL);

	struct omci_mib *mib = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	struct omci_pm *pm = NULL;

	CHECK_EQ(omci_pm_init(&pm, mib), OMCI_OK);
	CHECK_EQ(omci_pm_refresh(pm, 263u, 0, true), OMCI_ERR_ME_UNKNOWN);

	CHECK(omci_pm_refresh(pm, ME_PM_US, 0x0777u, true) != OMCI_OK);

	omci_pm_forget(pm, ME_PM_US, 0x0888u);
	omci_pm_forget_all(pm);

	CHECK_EQ(omci_mib_create(mib, ME_PM_US, UNI_INSTANCE), OMCI_OK);
	omci_hw_counter_fail_next(true);
	CHECK(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, true) != OMCI_OK);
	omci_hw_counter_fail_next(false);

	omci_hw_counter_fail_next(true);
	CHECK(!omci_pm_tick(pm, 5000u));
	CHECK(omci_pm_tick(pm, 5000u + OMCI_PM_INTERVAL_SEC));
	omci_hw_counter_fail_next(false);
	uint8_t probe[8];
	size_t plen = 0;

	CHECK(omci_mib_attr_get(mib, ME_PM_US, 0x0777u, 4u, probe,
				sizeof(probe), &plen) != OMCI_OK);
	omci_pm_fini(pm);
	omci_mib_fini(mib);
}

static void test_every_counter_is_accounted_for(void)
{
	static const struct {
		const char *name;
		unsigned int idx;
	} all[] = {
		{ "ifInOctets", 0 },
		{ "ifInUcastPkts", 1 },
		{ "ifInMulticastPkts", 2 },
		{ "ifInBroadcastPkts", 3 },
		{ "ifOutOctets", 5 },
		{ "ifOutDiscards", 6 },
		{ "ifOutUcastPkts", 7 },
		{ "ifOutMulticastPkts", 8 },
		{ "ifOutBroadcastPkts", 9 },
		{ "dot1dTpPortInDiscards", 11 },
		{ "dot3InPauseFrames", 13 },
		{ "dot3OutPauseFrames", 14 },
		{ "dot3StatsSingleCollisionFrames", 18 },
		{ "dot3StatsMultipleCollisionFrames", 19 },
		{ "dot3StatsDeferredTransmissions", 20 },
		{ "dot3StatsLateCollisions", 21 },
		{ "dot3StatsExcessiveCollisions", 22 },
		{ "dot3StatsSymbolErrors", 24 },
		{ "dot3ControlInUnknownOpcodes", 25 },
		{ "etherStatsDropEvents", 26 },
		{ "etherStatsFragments", 32 },
		{ "etherStatsJabbers", 33 },
		{ "etherStatsCollisions", 34 },
		{ "etherStatsCRCAlignErrors", 35 },
		{ "etherStatsTxUndersizePkts", 43 },
		{ "etherStatsTxOversizePkts", 44 },
		{ "etherStatsTxPkts64Octets", 45 },
		{ "etherStatsTxPkts65to127Octets", 46 },
		{ "etherStatsTxPkts128to255Octets", 47 },
		{ "etherStatsTxPkts256to511Octets", 48 },
		{ "etherStatsTxPkts512to1023Octets", 49 },
		{ "etherStatsTxPkts1024to1518Octets", 50 },
		{ "etherStatsTxPkts1519toMaxOctets", 51 },
		{ "etherStatsTxBroadcastPkts", 52 },
		{ "etherStatsTxMulticastPkts", 53 },
		{ "etherStatsRxUndersizePkts", 57 },
		{ "etherStatsRxOversizePkts", 59 },
		{ "etherStatsRxPkts64Octets", 60 },
		{ "etherStatsRxPkts65to127Octets", 61 },
		{ "etherStatsRxPkts128to255Octets", 62 },
		{ "etherStatsRxPkts256to511Octets", 63 },
		{ "etherStatsRxPkts512to1023Octets", 64 },
		{ "etherStatsRxPkts1024to1518Octets", 65 },
		{ "etherStatsRxPkts1519toMaxOctets", 66 },
		{ "inOamPduPkts", 67 },
		{ "outOamPduPkts", 68 },
	};
	const unsigned int n = (unsigned int)(ARRAY_SIZE(all));

	unsigned int mapped = 0;

	for (unsigned int i = 0; i < n; i++) {
		const char *why = NULL;
		const bool ok = omci_pm_counter_accounted(all[i].idx, &why);

		if (!ok)
			printf("  UNACCOUNTED: %s (index %u)\n", all[i].name,
			       all[i].idx);
		CHECK(ok);
		CHECK(why != NULL && why[0] != '\0');
		if (ok)
			mapped++;
	}
	printf("  %u/%u counters accounted for\n", mapped, n);

	CHECK(!omci_pm_counter_accounted(9999u, NULL));
}

static void test_a_failed_counter_read_keeps_the_last_values(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	ensure_row(fx.mib, ME_PM_US);
	hw_zero();
	get_current(fx.eng, ME_PM_US, 0x0E00u);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 4242u);

	char log[512];
	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x0E01u, 28u, true, false, ME_PM_US, UNI_INSTANCE);
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(log, { CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK); });
	CHECK(strstr(log, "could not be refreshed") == NULL);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 4242u);

	omci_hw_counter_fail_next(true);
	omci_t_build(req, 0x0E02u, 28u, true, false, ME_PM_US, UNI_INSTANCE);
	omci_t_tx_reset();
	OMCI_T_CAPTURE_LOG(log, { CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK); });
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK(strstr(log,
		     "pm: 322/257 could not be refreshed; serving the last known counters") !=
	      NULL);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 4242u);

	omci_hw_counter_fail_next(false);
	hw_zero();
	omci_t_down(&fx);
}

static void test_a_non_pm_class_is_left_alone(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x0F01u, 9u, true, false, 263u, 0x8001u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);

	omci_t_down(&fx);
}

static void test_a_get_for_a_missing_instance_costs_nothing(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	hw_zero();

	uint8_t req[OMCI_FRAME_LEN];

	for (unsigned int i = 0; i < 40u; i++) {
		omci_t_build(req, (uint16_t)(0x1000u + i), 28u, true, false,
			     ME_PM_US, (uint16_t)(0x0900u + i));
		(void)!omci_t_rx(fx.eng, req);
	}

	ensure_row(fx.mib, ME_PM_US);
	get_current(fx.eng, ME_PM_US, 0x1100u);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 4321u);
	get_current(fx.eng, ME_PM_US, 0x1101u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 4321u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_delete_forgets_the_interval(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	hw_zero();
	ensure_row(fx.mib, ME_PM_US);

	get_current(fx.eng, ME_PM_US, 0x1200u);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 1500u);
	get_current(fx.eng, ME_PM_US, 0x1201u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 1500u);

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x11F0u, 6u, true, false, ME_PM_US, 0x0999u);
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	get_current(fx.eng, ME_PM_US, 0x11F1u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 1500u);

	omci_t_build(req, 0x1202u, 6u, true, false, ME_PM_US, UNI_INSTANCE);
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);
	ensure_row(fx.mib, ME_PM_US);

	get_current(fx.eng, ME_PM_US, 0x1203u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_mib_reset_forgets_every_interval(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	hw_zero();
	ensure_row(fx.mib, ME_PM_US);

	get_current(fx.eng, ME_PM_US, 0x1300u);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 2500u);
	get_current(fx.eng, ME_PM_US, 0x1301u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 2500u);

	uint8_t req[OMCI_FRAME_LEN];

	omci_t_build(req, 0x1302u, 15u, true, false, ME_ONU_DATA, 0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, req), OMCI_OK);

	ensure_row(fx.mib, ME_PM_US);
	get_current(fx.eng, ME_PM_US, 0x1303u);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);

	hw_zero();
	omci_t_down(&fx);
}

static void test_forgetting_everything_starts_the_clock_over(void)
{
	struct omci_mib *mib = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	struct omci_pm *pm = NULL;

	CHECK_EQ(omci_pm_init(&pm, mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, ME_PM_US, UNI_INSTANCE), OMCI_OK);

	CHECK(!omci_pm_tick(pm, 1000u));
	CHECK(!omci_pm_tick(pm, 1000u + OMCI_PM_INTERVAL_SEC - 1u));
	omci_pm_forget_all(pm);

	CHECK(!omci_pm_tick(pm, 5000u));
	CHECK(!omci_pm_tick(pm, 5000u + OMCI_PM_INTERVAL_SEC - 1u));
	CHECK(omci_pm_tick(pm, 5000u + OMCI_PM_INTERVAL_SEC));

	omci_pm_fini(pm);
	omci_mib_fini(mib);
}

static void test_a_failed_seed_does_not_report_a_lifetime_counter(void)
{
	struct omci_t_fixture fx;

	CHECK(omci_t_up(&fx));
	hw_zero();
	ensure_row(fx.mib, ME_PM_US);

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 9000000000ull);

	char log[512];

	omci_log_level_set(OMCI_LOG_WARN);

	omci_hw_counter_fail_next(true);
	OMCI_T_CAPTURE_LOG(log, { get_current(fx.eng, ME_PM_US, 0x1400u); });
	omci_hw_counter_fail_next(false);
	CHECK_EQ(omci_t_log_count(log, "pm: 322/257 could not be refreshed"),
		 1u);

	OMCI_T_CAPTURE_LOG(log, { get_current(fx.eng, ME_PM_US, 0x1401u); });
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);
	CHECK_EQ(omci_t_log_count(log, "pm: 322/257 could not be refreshed"),
		 1u);

	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 9000000000ull + 640u);
	OMCI_T_CAPTURE_LOG(log, { get_current(fx.eng, ME_PM_US, 0x1402u); });
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK_EQ(attr32(fx.mib, ME_PM_US, UNI_INSTANCE, 4u), 640u);
	CHECK(strstr(log, "could not be refreshed") == NULL);

	hw_zero();
	omci_t_down(&fx);
}

#define W0 1786797296ull

static bool tick_at(struct omci_pm *pm, uint64_t mono, uint64_t wall)
{
	omci_pm_set_wall(pm, wall);
	return omci_pm_tick(pm, mono);
}

static void pm_up(struct omci_mib **mib, struct omci_pm **pm)
{
	*mib = NULL;
	*pm = NULL;
	CHECK_EQ(omci_mib_init(mib), OMCI_OK);
	CHECK_EQ(omci_pm_init(pm, *mib), OMCI_OK);
	CHECK_EQ(omci_mib_create(*mib, ME_PM_US, UNI_INSTANCE), OMCI_OK);

	hw_zero();
	CHECK(!omci_pm_tick(*pm, 1000u));
	CHECK_EQ(omci_pm_refresh(*pm, ME_PM_US, UNI_INSTANCE, true), OMCI_OK);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 800u);
}

static void pm_down(struct omci_mib *mib, struct omci_pm *pm)
{
	hw_zero();
	omci_pm_fini(pm);
	omci_mib_fini(mib);
}

static uint32_t previous_octets(struct omci_mib *mib, struct omci_pm *pm)
{
	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, false), OMCI_OK);
	return attr32(mib, ME_PM_US, UNI_INSTANCE, 4u);
}

static void test_a_synchronised_window_rolls_at_the_olt_boundary(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);

	omci_pm_resync(pm, W0);

	CHECK(!tick_at(pm, 1300u, W0 + 300u));
	CHECK_EQ(previous_octets(mib, pm), 0u);

	CHECK(!tick_at(pm, 1603u, W0 + 603u));
	CHECK_EQ(previous_octets(mib, pm), 0u);

	CHECK(tick_at(pm, 1604u, W0 + 604u));
	CHECK_EQ(previous_octets(mib, pm), 800u);
	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, true), OMCI_OK);
	CHECK_EQ(attr32(mib, ME_PM_US, UNI_INSTANCE, 4u), 0u);

	CHECK(!tick_at(pm, 1605u, W0 + 605u));
	CHECK(!tick_at(pm, 1606u, W0 + 606u));
	CHECK_EQ(previous_octets(mib, pm), 800u);

	pm_down(mib, pm);
}

static void test_the_first_synchronisation_does_not_close_the_window(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);

	CHECK(!tick_at(pm, 1010u, 0u));
	omci_pm_resync(pm, W0);
	CHECK(!omci_pm_tick(pm, 1011u));
	CHECK_EQ(previous_octets(mib, pm), 0u);

	pm_down(mib, pm);
}

static void test_a_clock_stepped_forward_renames_without_closing(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);

	omci_pm_resync(pm, W0);
	CHECK(!tick_at(pm, 1011u, W0 + 11u));

	uint8_t before = 0;
	size_t len = 0;

	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, true), OMCI_OK);
	CHECK_EQ(omci_mib_attr_get(mib, ME_PM_US, UNI_INSTANCE, 1u, &before, 1,
				   &len),
		 OMCI_OK);

	omci_pm_resync(pm, W0 + 11u + 3u * 3600u);
	CHECK(!omci_pm_tick(pm, 1012u));
	CHECK_EQ(previous_octets(mib, pm), 0u);

	uint8_t after = 0;

	CHECK_EQ(omci_pm_refresh(pm, ME_PM_US, UNI_INSTANCE, true), OMCI_OK);
	CHECK_EQ(omci_mib_attr_get(mib, ME_PM_US, UNI_INSTANCE, 1u, &after, 1,
				   &len),
		 OMCI_OK);
	CHECK_EQ(after, (uint8_t)(before + 12u));

	CHECK(tick_at(pm, 1700u, W0 + 11u + 3u * 3600u + 604u));
	CHECK_EQ(previous_octets(mib, pm), 800u);

	pm_down(mib, pm);
}

static void test_a_clock_stepped_backward_does_not_stall_rolling(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);

	omci_pm_resync(pm, W0);
	CHECK(tick_at(pm, 1604u, W0 + 604u));
	CHECK_EQ(previous_octets(mib, pm), 800u);

	omci_pm_resync(pm, W0 + 604u - 2u * 3600u);
	omci_hw_counter_force(uni_port(), IF_IN_OCTETS, 1500u);

	CHECK(tick_at(pm, 2500u, W0 + 604u - 2u * 3600u + 900u));
	CHECK_EQ(previous_octets(mib, pm), 700u);

	pm_down(mib, pm);
}

static void
test_pushing_a_clock_that_was_never_synced_adopts_rather_than_rolls(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);

	CHECK(!tick_at(pm, 1001u, W0));
	CHECK(!tick_at(pm, 1002u, W0 + 1u));
	CHECK_EQ(previous_octets(mib, pm), 0u);

	omci_pm_resync(pm, 0u);
	CHECK(!omci_pm_tick(pm, 1003u));
	CHECK(omci_pm_tick(pm, 1000u + OMCI_PM_INTERVAL_SEC));
	CHECK_EQ(previous_octets(mib, pm), 800u);

	pm_down(mib, pm);
}

static void test_withdrawing_the_clock_returns_to_counting_from_boot(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);

	omci_pm_resync(pm, W0);
	omci_pm_set_wall(pm, 0u);

	CHECK(!omci_pm_tick(pm, 1000u + OMCI_PM_INTERVAL_SEC - 1u));
	CHECK_EQ(previous_octets(mib, pm), 0u);
	CHECK(omci_pm_tick(pm, 1000u + OMCI_PM_INTERVAL_SEC));
	CHECK_EQ(previous_octets(mib, pm), 800u);

	CHECK(!omci_pm_tick(pm, 1000u + OMCI_PM_INTERVAL_SEC + 1u));

	pm_down(mib, pm);
}

int main(void)
{
	RUN(test_a_counter_reaches_its_attribute);
	RUN(test_downstream_and_upstream_are_not_swapped);
	RUN(test_a_summed_attribute);
	RUN(test_the_arithmetic_is_64_bit);
	RUN(test_get_reports_the_previous_interval);
	RUN(test_the_interval_rolls_at_fifteen_minutes);
	RUN(test_a_synchronised_window_rolls_at_the_olt_boundary);
	RUN(test_the_first_synchronisation_does_not_close_the_window);
	RUN(test_a_clock_stepped_forward_renames_without_closing);
	RUN(test_a_clock_stepped_backward_does_not_stall_rolling);
	RUN(test_withdrawing_the_clock_returns_to_counting_from_boot);
	RUN(test_pushing_a_clock_that_was_never_synced_adopts_rather_than_rolls);
	RUN(test_an_attribute_without_a_source_reads_zero);
	RUN(test_the_surface);
	RUN(test_every_counter_is_accounted_for);
	RUN(test_a_failed_counter_read_keeps_the_last_values);
	RUN(test_a_non_pm_class_is_left_alone);
	RUN(test_a_get_for_a_missing_instance_costs_nothing);
	RUN(test_delete_forgets_the_interval);
	RUN(test_mib_reset_forgets_every_interval);
	RUN(test_a_failed_seed_does_not_report_a_lifetime_counter);
	RUN(test_forgetting_everything_starts_the_clock_over);
	return OMCI_T_REPORT("test_pm");
}
