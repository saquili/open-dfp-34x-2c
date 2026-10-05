// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of performance monitoring. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

#define ME_PPTP_ETH_UNI 11u
#define ME_ETH_PM 24u
#define ME_BRIDGE_PORT 47u
#define ME_BRIDGE_PORT_PM 52u
#define ME_ETH_PM_2 89u
#define ME_ANI_G 263u
#define ME_GEM_IW_TP 266u
#define ME_GEM_CTP 268u
#define ME_THRESHOLD_1 273u
#define ME_THRESHOLD_2 274u
#define ME_MCAST_GEM_IW_TP 281u
#define ME_ETH_PM_3 296u
#define ME_FEC_PM 312u
#define ME_PM_DS 321u
#define ME_PM_US 322u
#define ME_VEIP 329u
#define ME_EXT_PM 334u
#define ME_GEM_PM 341u

#define MT_CREATE 4u
#define MT_SET 8u
#define MT_GET 9u
#define MT_ALARM 16u
#define MT_GET_CURRENT 28u

/* The switch ports: the UNI and the PON side. */
#define UNI 0u
#define PON 2u

#define UNI_INSTANCE 0x0101u

/* Counters of the switch, by their number. */
#define RX_OCTETS 0u
#define RX_UCAST 1u
#define RX_MCAST 2u
#define RX_BCAST 3u
#define TX_OCTETS 5u
#define TX_DISCARDS 6u
#define TX_UCAST 7u
#define RX_FWD_DISCARDS 11u
#define TX_SINGLE_COLLISIONS 18u
#define TX_MULTIPLE_COLLISIONS 19u
#define TX_DEFERRED 20u
#define TX_LATE_COLLISIONS 21u
#define TX_EXCESSIVE_COLLISIONS 22u
#define RX_DROPS 26u
#define RX_FRAGMENTS 32u
#define RX_JABBERS 33u
#define RX_CRC 35u
#define RX_UNDERSIZE 57u
#define RX_OVERSIZE 59u
#define RX_64 60u

/* The control block of the extended PM: fields bits and disable bits. */
#define EXT_CONTINUOUS 0x0001u
#define EXT_DOWNSTREAM 0x0002u
#define EXT_MATCH_VID 0x8000u
#define EXT_CLEAR 0x8000u
#define EXT_OFF 0x4000u

static void hw_zero(void)
{
	for (unsigned int i = 0; i < OMCI_HW_PORT_COUNTERS; i++) {
		omci_hw_counter_force(UNI, i, 0);
		omci_hw_counter_force(PON, i, 0);
	}
	omci_hw_counter_fail_next(false);
	omci_hw_gem_counts_reset();
	omci_hw_pon_fec_force(false, 0, 0, 0, 0);
	omci_hw_pon_fail_next(false);
}

static void pm_up(struct omci_mib **mib, struct omci_pm **pm)
{
	*mib = NULL;
	*pm = NULL;
	hw_zero();
	CHECK_EQ(omci_mib_init(mib), OMCI_OK);
	CHECK_EQ(omci_pm_init(pm, *mib), OMCI_OK);
}

static void pm_down(struct omci_mib *mib, struct omci_pm *pm)
{
	omci_pm_fini(pm);
	omci_mib_fini(mib);
	hw_zero();
}

static void make(struct omci_mib *mib, uint16_t cls, uint16_t inst)
{
	CHECK_EQ(omci_mib_create(mib, cls, inst), OMCI_OK);
}

static void put(struct omci_mib *mib, uint16_t cls, uint16_t inst, uint8_t no,
		uint64_t v, size_t size)
{
	uint8_t b[8];

	for (size_t k = 0; k < size; k++)
		b[k] = (uint8_t)(v >> (8u * (size - 1u - k)));
	CHECK_EQ(omci_mib_attr_set_by_olt(mib, cls, inst, no, b, size),
		 OMCI_OK);
}

static uint64_t get(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		    uint8_t no)
{
	uint8_t b[16];
	size_t len = 0;
	uint64_t v = 0;

	if (omci_mib_attr_get(mib, cls, inst, no, b, sizeof(b), &len) !=
	    OMCI_OK)
		return UINT64_MAX;
	for (size_t k = 0; k < len && k < 8u; k++)
		v = (v << 8) | b[k];
	return v;
}

/* A count of the interval in progress, as Get current data reads it. */
static uint64_t now_count(struct omci_pm *pm, struct omci_mib *mib,
			  uint16_t cls, uint16_t inst, uint8_t no)
{
	CHECK_EQ(omci_pm_refresh(pm, cls, inst, true), OMCI_OK);
	return get(mib, cls, inst, no);
}

/* A count of the last closed interval, as Get reads it. */
static uint64_t last_count(struct omci_pm *pm, struct omci_mib *mib,
			   uint16_t cls, uint16_t inst, uint8_t no)
{
	CHECK_EQ(omci_pm_refresh(pm, cls, inst, false), OMCI_OK);
	return get(mib, cls, inst, no);
}

static void bridge_port(struct omci_mib *mib, uint16_t inst, uint8_t tp_type)
{
	make(mib, ME_BRIDGE_PORT, inst);
	put(mib, ME_BRIDGE_PORT, inst, 3u, tp_type, 1u);
}

static void ext_control(uint8_t b[16], uint16_t threshold, uint16_t parent,
			uint16_t parent_inst, uint16_t acc_off,
			uint16_t tca_off, uint16_t fields)
{
	memset(b, 0, 16u);
	omci_be16_store(&b[0], threshold);
	omci_be16_store(&b[2], parent);
	omci_be16_store(&b[4], parent_inst);
	omci_be16_store(&b[6], acc_off);
	omci_be16_store(&b[8], tca_off);
	omci_be16_store(&b[10], fields);
}

/* Make, or change, an extended PM instance. */
static void ext_pm(struct omci_mib *mib, uint16_t inst, uint16_t threshold,
		   uint16_t parent, uint16_t parent_inst, uint16_t acc_off,
		   uint16_t tca_off, uint16_t fields)
{
	uint8_t b[16];

	ext_control(b, threshold, parent, parent_inst, acc_off, tca_off,
		    fields);
	if (!omci_mib_exists(mib, ME_EXT_PM, inst))
		make(mib, ME_EXT_PM, inst);
	CHECK_EQ(omci_mib_attr_set_by_olt(mib, ME_EXT_PM, inst, 2u, b,
					  sizeof(b)),
		 OMCI_OK);
}

static size_t alerted(struct omci_pm *pm, uint16_t cls, uint8_t bit)
{
	uint16_t out[8];

	return omci_pm_tca(pm, cls, bit, out, ARRAY_SIZE(out));
}

static void test_the_surface(void)
{
	struct omci_pm *pm, *other = NULL;
	struct omci_mib *mib;
	uint16_t out[4];
	size_t n = 0;

	CHECK(omci_pm_owns(ME_ETH_PM));
	CHECK(omci_pm_owns(ME_GEM_PM));
	CHECK(!omci_pm_owns(ME_THRESHOLD_1));
	CHECK(!omci_pm_owns(ME_ANI_G));
	while (omci_pm_class_at(n) != 0u)
		n++;
	CHECK_EQ(n, 9u);

	CHECK(!omci_pm_tick(NULL, 0));
	omci_pm_synchronize(NULL, 0, 0);
	omci_pm_changed(NULL, ME_ETH_PM, UNI_INSTANCE);
	CHECK_EQ(omci_pm_refresh(NULL, ME_ETH_PM, UNI_INSTANCE, true),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_pm_tca(NULL, ME_ETH_PM, 0u, out, ARRAY_SIZE(out)), 0u);
	omci_pm_forget(NULL, ME_ETH_PM, UNI_INSTANCE);
	omci_pm_forget_all(NULL);
	omci_pm_fini(NULL);
	CHECK_EQ(omci_pm_init(NULL, NULL), OMCI_ERR_INVAL);

	pm_up(&mib, &pm);
	CHECK_EQ(omci_pm_init(&other, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_pm_init(&other, mib), OMCI_ERR_BUSY);
	CHECK_EQ(omci_pm_refresh(pm, ME_ANI_G, 0x8001u, true),
		 OMCI_ERR_ME_UNKNOWN);
	CHECK_EQ(omci_pm_refresh(pm, ME_ETH_PM, UNI_INSTANCE, true),
		 OMCI_ERR_INST_UNKNOWN);
	CHECK_EQ(omci_pm_tca(pm, ME_ETH_PM, 0u, NULL, 4u), 0u);
	CHECK_EQ(omci_pm_tca(pm, ME_ETH_PM, OMCI_PM_TCA_BITS, out,
			     ARRAY_SIZE(out)),
		 0u);
	omci_pm_changed(pm, ME_ANI_G, 0x8001u);
	omci_pm_changed(pm, ME_ETH_PM, UNI_INSTANCE);
	omci_pm_forget(pm, ME_ETH_PM, UNI_INSTANCE);
	pm_down(mib, pm);
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
	const char *why = "stale";

	for (unsigned int i = 0; i < ARRAY_SIZE(all); i++) {
		const bool ok = omci_pm_counter_accounted(all[i].idx, &why);

		if (!ok)
			printf("  UNACCOUNTED: %s (index %u)\n", all[i].name,
			       all[i].idx);
		CHECK(ok);
		CHECK(why != NULL && why[0] != '\0');
	}

	CHECK(omci_pm_counter_accounted(0u, NULL));
	CHECK(!omci_pm_counter_accounted(9999u, NULL));
	CHECK(!omci_pm_counter_accounted(4u, &why));
	CHECK(why == NULL);
}

static void test_the_ethernet_pm_counts_the_uni(void)
{
	static const struct {
		unsigned int idx;
		uint64_t v;
	} set[] = {
		{ RX_CRC, 5u },
		{ TX_EXCESSIVE_COLLISIONS, 1u },
		{ TX_LATE_COLLISIONS, 2u },
		{ RX_OVERSIZE, 3u },
		{ RX_DROPS, 4u },
		{ TX_DISCARDS, 6u },
		{ TX_SINGLE_COLLISIONS, 7u },
		{ TX_MULTIPLE_COLLISIONS, 8u },
		{ TX_DEFERRED, 9u },
	};
	static const uint64_t want[17] = {
		[3] = 5u, [4] = 1u, [5] = 2u,  [6] = 3u,  [7] = 4u,
		[8] = 6u, [9] = 7u, [10] = 8u, [12] = 9u,
	};
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_ETH_PM, UNI_INSTANCE);
	CHECK(!omci_pm_tick(pm, 1000u));

	for (size_t i = 0; i < ARRAY_SIZE(set); i++)
		omci_hw_counter_force(UNI, set[i].idx, set[i].v);
	omci_hw_counter_force(PON, RX_CRC, 1000u);
	CHECK(!omci_pm_tick(pm, 1001u));

	CHECK_EQ(omci_pm_refresh(pm, ME_ETH_PM, UNI_INSTANCE, true), OMCI_OK);
	for (uint8_t no = 3; no <= 16u; no++)
		CHECK_EQ(get(mib, ME_ETH_PM, UNI_INSTANCE, no), want[no]);

	pm_down(mib, pm);
}

static void test_the_ethernet_pm_2_and_3_count_what_the_uni_receives(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_ETH_PM_2, UNI_INSTANCE);
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_counter_force(UNI, RX_DROPS, 1u);
	omci_hw_counter_force(UNI, RX_FWD_DISCARDS, 2u);
	omci_hw_counter_force(UNI, RX_OCTETS, 1000u);
	omci_hw_counter_force(UNI, RX_UCAST, 10u);
	omci_hw_counter_force(UNI, RX_MCAST, 2u);
	omci_hw_counter_force(UNI, RX_BCAST, 3u);
	omci_hw_counter_force(UNI, RX_CRC, 1u);
	omci_hw_counter_force(UNI, RX_FRAGMENTS, 4u);
	omci_hw_counter_force(UNI, RX_JABBERS, 5u);
	omci_hw_counter_force(UNI, RX_UNDERSIZE, 6u);
	omci_hw_counter_force(UNI, RX_64, 7u);
	omci_hw_counter_force(UNI, TX_OCTETS, 99999u);
	CHECK(!omci_pm_tick(pm, 1001u));

	CHECK_EQ(omci_pm_refresh(pm, ME_ETH_PM_3, UNI_INSTANCE, true), OMCI_OK);
	/* Frames dropped on their way out count where they came in. */
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 3u), 3u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 1000u);
	/* The packets with the bad ones. */
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 5u), 25u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 6u), 3u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 7u), 2u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 8u), 6u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 9u), 4u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 10u), 5u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 11u), 7u);

	/* Nothing filters PPPoE yet, so nothing is counted. */
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_2, UNI_INSTANCE, 3u), 0u);

	pm_down(mib, pm);
}

static void test_get_reads_the_last_closed_interval(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_counter_force(UNI, RX_OCTETS, 800u);
	CHECK(!omci_pm_tick(pm, 1899u));
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 800u);
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 0u);

	CHECK(omci_pm_tick(pm, 1900u));
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 800u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 1u);
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);

	omci_hw_counter_force(UNI, RX_OCTETS, 1000u);
	CHECK(!omci_pm_tick(pm, 1901u));
	CHECK(omci_pm_tick(pm, 2800u));
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 200u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 2u);

	/* The same second counts once. */
	CHECK(!omci_pm_tick(pm, 2800u));

	pm_down(mib, pm);
}

static void test_a_count_stops_at_its_most(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_counter_force(UNI, RX_OCTETS, 0x100000005ull);
	CHECK(!omci_pm_tick(pm, 1001u));
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u),
		 0xFFFFFFFFu);
	omci_hw_counter_force(UNI, RX_OCTETS, 0x200000000ull);
	CHECK(!omci_pm_tick(pm, 1002u));
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u),
		 0xFFFFFFFFu);

	pm_down(mib, pm);
}

static void gem_counts(uint16_t port_id, uint64_t tx_frames, uint64_t rx_frames,
		       uint64_t tx_payload, uint64_t rx_payload)
{
	const struct omci_hw_gem_counts c = {
		.tx_frames = tx_frames,
		.rx_frames = rx_frames,
		.tx_payload = tx_payload,
		.rx_payload = rx_payload,
	};

	omci_hw_gem_counts_force(port_id, &c);
}

static void test_the_gem_port_pm_counts_its_gem_port(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_GEM_CTP, 0x8001u);
	put(mib, ME_GEM_CTP, 0x8001u, 1u, 1100u, 2u);
	make(mib, ME_GEM_PM, 0x8001u);
	gem_counts(1100u, 10u, 20u, 1000u, 2000u);
	CHECK(!omci_pm_tick(pm, 1000u));

	gem_counts(1100u, 15u, 30u, 1500u, 3000u);
	CHECK(!omci_pm_tick(pm, 1001u));
	CHECK_EQ(omci_pm_refresh(pm, ME_GEM_PM, 0x8001u, true), OMCI_OK);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 3u), 5u);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 4u), 10u);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 5u), 1000u);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 6u), 500u);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 7u), 0u);

	/* The GEM port was made again: its counts start from zero. */
	gem_counts(1100u, 3u, 4u, 300u, 400u);
	CHECK(!omci_pm_tick(pm, 1002u));
	CHECK_EQ(now_count(pm, mib, ME_GEM_PM, 0x8001u, 3u), 8u);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 4u), 14u);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 5u), 1400u);
	CHECK_EQ(get(mib, ME_GEM_PM, 0x8001u, 6u), 800u);

	/* The payload counts are eight bytes wide. */
	gem_counts(1100u, 3u, 4u, 300u, 400u + 0x100000000ull);
	CHECK(!omci_pm_tick(pm, 1003u));
	CHECK(now_count(pm, mib, ME_GEM_PM, 0x8001u, 5u) ==
	      1400u + 0x100000000ull);

	/* A GEM port that cannot be read, and an entity without one. */
	make(mib, ME_GEM_CTP, 0x8002u);
	put(mib, ME_GEM_CTP, 0x8002u, 1u, 1200u, 2u);
	make(mib, ME_GEM_PM, 0x8002u);
	make(mib, ME_GEM_PM, 0x8003u);
	CHECK_EQ(omci_pm_refresh(pm, ME_GEM_PM, 0x8002u, true), OMCI_ERR_HW);
	CHECK_EQ(now_count(pm, mib, ME_GEM_PM, 0x8003u, 3u), 0u);

	/* The host backend counts for eight GEM ports; a ninth is dropped. */
	for (uint16_t i = 0; i < 9u; i++)
		gem_counts((uint16_t)(2000u + i), 1u, 1u, 1u, 1u);

	struct omci_hw_gem_counts c;

	CHECK_EQ(omci_hw_gem_counts(2008u, &c), OMCI_ERR_HW);
	CHECK_EQ(omci_hw_gem_counts(1100u, &c), OMCI_OK);

	pm_down(mib, pm);
}

static void test_the_bridge_port_entities_count_their_side(void)
{
	static const uint16_t inst[] = { 0x0101u, 0x0201u, 0x0301u, 0x0401u };
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	bridge_port(mib, 0x0101u, 1u);
	bridge_port(mib, 0x0201u, 5u);
	bridge_port(mib, 0x0301u, 11u);
	for (size_t i = 0; i < ARRAY_SIZE(inst); i++) {
		make(mib, ME_PM_DS, inst[i]);
		make(mib, ME_PM_US, inst[i]);
		make(mib, ME_BRIDGE_PORT_PM, inst[i]);
	}
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_counter_force(UNI, RX_OCTETS, 100u);
	omci_hw_counter_force(UNI, TX_OCTETS, 200u);
	omci_hw_counter_force(UNI, RX_UCAST, 1u);
	omci_hw_counter_force(UNI, TX_UCAST, 2u);
	omci_hw_counter_force(UNI, RX_CRC, 7u);
	omci_hw_counter_force(UNI, RX_OVERSIZE, 9u);
	omci_hw_counter_force(PON, RX_OCTETS, 300u);
	omci_hw_counter_force(PON, TX_OCTETS, 400u);
	omci_hw_counter_force(PON, RX_UCAST, 3u);
	omci_hw_counter_force(PON, TX_UCAST, 4u);
	CHECK(!omci_pm_tick(pm, 1001u));

	/* Upstream at the UNI is what it receives; on the PON side, sends. */
	CHECK_EQ(now_count(pm, mib, ME_PM_US, 0x0101u, 4u), 100u);
	CHECK_EQ(get(mib, ME_PM_US, 0x0101u, 8u), 7u);
	CHECK_EQ(now_count(pm, mib, ME_PM_DS, 0x0101u, 4u), 200u);
	CHECK_EQ(get(mib, ME_PM_DS, 0x0101u, 8u), 0u);
	CHECK_EQ(now_count(pm, mib, ME_PM_US, 0x0201u, 4u), 400u);
	CHECK_EQ(now_count(pm, mib, ME_PM_DS, 0x0201u, 4u), 300u);
	CHECK_EQ(now_count(pm, mib, ME_PM_US, 0x0301u, 4u), 0u);
	CHECK_EQ(now_count(pm, mib, ME_PM_DS, 0x0401u, 4u), 0u);

	CHECK_EQ(now_count(pm, mib, ME_BRIDGE_PORT_PM, 0x0101u, 3u), 2u);
	CHECK_EQ(get(mib, ME_BRIDGE_PORT_PM, 0x0101u, 4u), 0u);
	CHECK_EQ(get(mib, ME_BRIDGE_PORT_PM, 0x0101u, 5u), 9u);
	CHECK_EQ(get(mib, ME_BRIDGE_PORT_PM, 0x0101u, 6u), 8u);
	CHECK_EQ(get(mib, ME_BRIDGE_PORT_PM, 0x0101u, 7u), 7u);
	CHECK_EQ(now_count(pm, mib, ME_BRIDGE_PORT_PM, 0x0201u, 3u), 4u);
	CHECK_EQ(get(mib, ME_BRIDGE_PORT_PM, 0x0201u, 6u), 3u);
	CHECK_EQ(now_count(pm, mib, ME_BRIDGE_PORT_PM, 0x0301u, 3u), 0u);
	CHECK_EQ(now_count(pm, mib, ME_BRIDGE_PORT_PM, 0x0401u, 3u), 0u);

	pm_down(mib, pm);
}

static void test_a_new_source_starts_from_its_readings(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	bridge_port(mib, 0x0101u, 1u);
	make(mib, ME_PM_US, 0x0101u);
	CHECK(!omci_pm_tick(pm, 1000u));
	omci_hw_counter_force(UNI, RX_OCTETS, 100u);
	CHECK(!omci_pm_tick(pm, 1001u));
	CHECK_EQ(now_count(pm, mib, ME_PM_US, 0x0101u, 4u), 100u);

	/* The port moves to the PON side, whose counters ran long before. */
	put(mib, ME_BRIDGE_PORT, 0x0101u, 3u, 5u, 1u);
	omci_hw_counter_force(PON, TX_OCTETS, 5000u);
	CHECK(!omci_pm_tick(pm, 1002u));
	CHECK_EQ(now_count(pm, mib, ME_PM_US, 0x0101u, 4u), 100u);
	omci_hw_counter_force(PON, TX_OCTETS, 5050u);
	CHECK(!omci_pm_tick(pm, 1003u));
	CHECK_EQ(now_count(pm, mib, ME_PM_US, 0x0101u, 4u), 150u);

	pm_down(mib, pm);
}

static void test_the_extended_pm_counts_what_its_control_block_names(void)
{
	static const struct {
		uint16_t inst;
		uint64_t octets;
	} want[] = {
		{ 1u, 100u }, { 2u, 200u }, { 3u, 400u },
		{ 4u, 300u }, { 5u, 300u }, { 6u, 0u },
		{ 7u, 0u },   { 8u, 0u },   { 9u, 0u },
	};
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	bridge_port(mib, 0x0201u, 5u);
	ext_pm(mib, 1u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u, 0u);
	ext_pm(mib, 2u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u,
	       EXT_DOWNSTREAM);
	ext_pm(mib, 3u, 0u, ME_BRIDGE_PORT, 0x0201u, 0u, 0u, 0u);
	ext_pm(mib, 4u, 0u, ME_GEM_IW_TP, 1u, 0u, 0u, EXT_DOWNSTREAM);
	ext_pm(mib, 5u, 0u, ME_MCAST_GEM_IW_TP, 1u, 0u, 0u, EXT_DOWNSTREAM);
	ext_pm(mib, 6u, 0u, ME_VEIP, 1u, 0u, 0u, 0u);
	ext_pm(mib, 7u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u,
	       EXT_MATCH_VID);
	/* PM2, the octets, is not collected; the others are. */
	ext_pm(mib, 8u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0x0002u, 0u, 0u);
	ext_pm(mib, 9u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, EXT_OFF, 0u, 0u);
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_counter_force(UNI, RX_OCTETS, 100u);
	omci_hw_counter_force(UNI, TX_OCTETS, 200u);
	omci_hw_counter_force(UNI, RX_UCAST, 1u);
	omci_hw_counter_force(PON, RX_OCTETS, 300u);
	omci_hw_counter_force(PON, TX_OCTETS, 400u);
	CHECK(!omci_pm_tick(pm, 1001u));

	for (size_t i = 0; i < ARRAY_SIZE(want); i++)
		CHECK_EQ(now_count(pm, mib, ME_EXT_PM, want[i].inst, 4u),
			 want[i].octets);
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 8u, 5u), 1u);
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 9u, 5u), 0u);

	pm_down(mib, pm);
}

static void test_an_extended_pm_can_count_continuously(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	ext_pm(mib, 1u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u,
	       EXT_CONTINUOUS);
	CHECK(!omci_pm_tick(pm, 1000u));

	/* It wraps rather than stop. */
	omci_hw_counter_force(UNI, RX_OCTETS, 0x100000010ull);
	CHECK(!omci_pm_tick(pm, 1001u));
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 1u, 4u), 0x10u);

	/* No interval: Get reads the counts so far, and the end time is 0. */
	CHECK(omci_pm_tick(pm, 1900u));
	CHECK_EQ(last_count(pm, mib, ME_EXT_PM, 1u, 4u), 0x10u);
	CHECK_EQ(get(mib, ME_EXT_PM, 1u, 1u), 0u);
	omci_pm_synchronize(pm, 1901u, 0u);
	CHECK_EQ(last_count(pm, mib, ME_EXT_PM, 1u, 4u), 0x10u);

	omci_hw_counter_force(UNI, RX_OCTETS, 0x100000020ull);
	CHECK(!omci_pm_tick(pm, 1902u));
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 1u, 4u), 0x20u);

	pm_down(mib, pm);
}

static void test_an_extended_pm_is_cleared_on_request(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;
	uint8_t b[16];
	size_t len = 0;

	pm_up(&mib, &pm);
	ext_pm(mib, 1u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u, 0u);
	CHECK(!omci_pm_tick(pm, 1000u));
	omci_hw_counter_force(UNI, RX_OCTETS, 100u);
	CHECK(omci_pm_tick(pm, 1900u));
	omci_hw_counter_force(UNI, RX_OCTETS, 150u);
	CHECK(!omci_pm_tick(pm, 1901u));
	CHECK_EQ(last_count(pm, mib, ME_EXT_PM, 1u, 4u), 100u);
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 1u, 4u), 50u);

	ext_pm(mib, 1u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, EXT_CLEAR, 0u, 0u);
	omci_pm_changed(pm, ME_EXT_PM, 1u);
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 1u, 4u), 0u);
	CHECK_EQ(last_count(pm, mib, ME_EXT_PM, 1u, 4u), 0u);

	/* Clearing is an action: the bit reads 0 again. */
	CHECK_EQ(omci_mib_attr_get(mib, ME_EXT_PM, 1u, 2u, b, sizeof(b), &len),
		 OMCI_OK);
	CHECK_EQ(len, 16u);
	CHECK_EQ(b[6], 0u);
	CHECK_EQ(b[7], 0u);

	/* Without the bit, a change clears nothing. */
	omci_hw_counter_force(UNI, RX_OCTETS, 170u);
	CHECK(!omci_pm_tick(pm, 1902u));
	omci_pm_changed(pm, ME_EXT_PM, 1u);
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 1u, 4u), 20u);

	pm_down(mib, pm);
}

static void test_the_fec_pm_counts_the_downstream_fec(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_FEC_PM, 0x8001u);
	omci_hw_pon_fec_force(true, 0u, 0u, 0u, 0u);
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_pon_fec_force(true, 10u, 2u, 1u, 1530u);
	CHECK(!omci_pm_tick(pm, 1001u));
	CHECK_EQ(omci_pm_refresh(pm, ME_FEC_PM, 0x8001u, true), OMCI_OK);
	CHECK_EQ(get(mib, ME_FEC_PM, 0x8001u, 3u), 10u);
	CHECK_EQ(get(mib, ME_FEC_PM, 0x8001u, 4u), 2u);
	CHECK_EQ(get(mib, ME_FEC_PM, 0x8001u, 5u), 1u);
	CHECK_EQ(get(mib, ME_FEC_PM, 0x8001u, 6u), 1530u);
	CHECK_EQ(get(mib, ME_FEC_PM, 0x8001u, 7u), 1u);

	/* A second counts once, however often it is read. */
	omci_hw_pon_fec_force(true, 10u, 2u, 2u, 1530u);
	CHECK_EQ(now_count(pm, mib, ME_FEC_PM, 0x8001u, 7u), 1u);
	CHECK(!omci_pm_tick(pm, 1002u));
	CHECK_EQ(now_count(pm, mib, ME_FEC_PM, 0x8001u, 7u), 1u);
	omci_hw_pon_fec_force(true, 10u, 2u, 5u, 1530u);
	CHECK(!omci_pm_tick(pm, 1003u));
	CHECK_EQ(now_count(pm, mib, ME_FEC_PM, 0x8001u, 7u), 2u);
	CHECK_EQ(get(mib, ME_FEC_PM, 0x8001u, 5u), 5u);

	/* A receiver that does not count FEC adds nothing. */
	omci_hw_pon_fec_force(false, 99u, 99u, 99u, 99u);
	CHECK(!omci_pm_tick(pm, 1004u));
	CHECK_EQ(now_count(pm, mib, ME_FEC_PM, 0x8001u, 3u), 10u);

	omci_hw_pon_fail_next(true);
	CHECK_EQ(omci_pm_refresh(pm, ME_FEC_PM, 0x8001u, true), OMCI_ERR_HW);
	omci_hw_pon_fail_next(false);

	pm_down(mib, pm);
}

static void test_a_count_that_reaches_its_threshold_raises_an_alert(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;
	uint16_t inst[4];

	pm_up(&mib, &pm);
	make(mib, ME_THRESHOLD_1, 1u);
	put(mib, ME_THRESHOLD_1, 1u, 1u, 10u, 4u);
	put(mib, ME_THRESHOLD_1, 1u, 2u, 0u, 4u);
	put(mib, ME_THRESHOLD_1, 1u, 3u, 0xFFFFu, 4u);
	put(mib, ME_THRESHOLD_1, 1u, 4u, 0xFFFFFFFFu, 4u);
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	put(mib, ME_ETH_PM_3, UNI_INSTANCE, 2u, 1u, 2u);
	/* Pointing at threshold data that is not there. */
	make(mib, ME_ETH_PM_3, 0x0102u);
	put(mib, ME_ETH_PM_3, 0x0102u, 2u, 7u, 2u);
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_counter_force(UNI, RX_DROPS, 9u);
	omci_hw_counter_force(UNI, RX_UNDERSIZE, 100u);
	omci_hw_counter_force(UNI, RX_FRAGMENTS, 100000u);
	omci_hw_counter_force(UNI, RX_JABBERS, 100000u);
	CHECK(!omci_pm_tick(pm, 1001u));
	for (uint8_t bit = 0; bit < 4u; bit++)
		CHECK_EQ(alerted(pm, ME_ETH_PM_3, bit), 0u);

	omci_hw_counter_force(UNI, RX_DROPS, 10u);
	CHECK(!omci_pm_tick(pm, 1002u));
	CHECK_EQ(omci_pm_tca(pm, ME_ETH_PM_3, 0u, inst, ARRAY_SIZE(inst)), 1u);
	CHECK_EQ(inst[0], UNI_INSTANCE);
	CHECK_EQ(omci_pm_tca(pm, ME_ETH_PM_3, 0u, inst, 0u), 0u);

	/* A threshold lowered under its count raises the alert at once. */
	put(mib, ME_THRESHOLD_1, 1u, 2u, 50u, 4u);
	CHECK(!omci_pm_tick(pm, 1003u));
	CHECK_EQ(alerted(pm, ME_ETH_PM_3, 1u), 1u);
	/* One that no longer holds clears it. */
	put(mib, ME_THRESHOLD_1, 1u, 2u, 0u, 4u);
	CHECK(!omci_pm_tick(pm, 1004u));
	CHECK_EQ(alerted(pm, ME_ETH_PM_3, 1u), 0u);
	CHECK_EQ(alerted(pm, ME_ETH_PM_3, 0u), 1u);

	/* The end of the interval clears every alert; the next counts anew. */
	CHECK(omci_pm_tick(pm, 1900u));
	CHECK_EQ(alerted(pm, ME_ETH_PM_3, 0u), 0u);
	omci_hw_counter_force(UNI, RX_DROPS, 20u);
	CHECK(!omci_pm_tick(pm, 1901u));
	CHECK_EQ(alerted(pm, ME_ETH_PM_3, 0u), 1u);

	/* Thresholds 8 to 14 are those of the threshold data 2. */
	make(mib, ME_THRESHOLD_2, 1u);
	put(mib, ME_THRESHOLD_2, 1u, 1u, 3u, 4u);
	make(mib, ME_ETH_PM, UNI_INSTANCE);
	put(mib, ME_ETH_PM, UNI_INSTANCE, 2u, 1u, 2u);
	CHECK(!omci_pm_tick(pm, 1902u));
	omci_hw_counter_force(UNI, TX_MULTIPLE_COLLISIONS, 3u);
	CHECK(!omci_pm_tick(pm, 1903u));
	CHECK_EQ(alerted(pm, ME_ETH_PM, 7u), 1u);

	/* The alerts of a bridge port skip numbers: MTU exceeded is 2. */
	bridge_port(mib, 0x0101u, 1u);
	make(mib, ME_BRIDGE_PORT_PM, 0x0101u);
	put(mib, ME_BRIDGE_PORT_PM, 0x0101u, 2u, 1u, 2u);
	put(mib, ME_THRESHOLD_1, 1u, 2u, 5u, 4u);
	CHECK(!omci_pm_tick(pm, 1904u));
	omci_hw_counter_force(UNI, RX_OVERSIZE, 5u);
	CHECK(!omci_pm_tick(pm, 1905u));
	CHECK_EQ(alerted(pm, ME_BRIDGE_PORT_PM, 2u), 1u);

	pm_down(mib, pm);
}

static void test_the_extended_pm_can_disable_its_alerts(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_THRESHOLD_1, 1u);
	put(mib, ME_THRESHOLD_1, 1u, 1u, 10u, 4u);
	ext_pm(mib, 2u, 1u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u, 0u);
	CHECK(!omci_pm_tick(pm, 1000u));

	omci_hw_counter_force(UNI, RX_DROPS, 10u);
	CHECK(!omci_pm_tick(pm, 1001u));
	CHECK_EQ(alerted(pm, ME_EXT_PM, 1u), 1u);

	/* Threshold 1 disabled: its alert clears. */
	ext_pm(mib, 2u, 1u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0x0001u, 0u);
	CHECK(!omci_pm_tick(pm, 1002u));
	CHECK_EQ(alerted(pm, ME_EXT_PM, 1u), 0u);
	ext_pm(mib, 2u, 1u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, EXT_OFF, 0u);
	CHECK(!omci_pm_tick(pm, 1003u));
	CHECK_EQ(alerted(pm, ME_EXT_PM, 1u), 0u);
	ext_pm(mib, 2u, 1u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u, 0u);
	CHECK(!omci_pm_tick(pm, 1004u));
	CHECK_EQ(alerted(pm, ME_EXT_PM, 1u), 1u);

	pm_down(mib, pm);
}

static void test_synchronize_starts_the_counts_again(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	ext_pm(mib, 1u, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u,
	       EXT_CONTINUOUS);
	CHECK(!omci_pm_tick(pm, 1000u));
	omci_hw_counter_force(UNI, RX_OCTETS, 100u);
	CHECK(omci_pm_tick(pm, 1900u));
	omci_hw_counter_force(UNI, RX_OCTETS, 150u);
	CHECK(!omci_pm_tick(pm, 1901u));
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 100u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 1u);

	/* 300 s into the OLT's quarter hour (an offset counts modulo 900). */
	omci_pm_synchronize(pm, 2000u, 300u + OMCI_PM_INTERVAL_SEC);
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 0u);
	/* What counts continuously is left alone. */
	CHECK_EQ(now_count(pm, mib, ME_EXT_PM, 1u, 4u), 150u);

	CHECK(!omci_pm_tick(pm, 2599u));
	CHECK(omci_pm_tick(pm, 2600u));
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 1u);

	pm_down(mib, pm);
}

static void test_a_daemon_that_stood_still_closes_the_missed_intervals(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(!omci_pm_tick(pm, 1000u));
	CHECK(omci_pm_tick(pm, 1000u + 3u * OMCI_PM_INTERVAL_SEC + 5u));
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 3u);
	CHECK(omci_pm_tick(pm, 1000u + 4u * OMCI_PM_INTERVAL_SEC));
	CHECK_EQ(last_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 1u), 4u);

	pm_down(mib, pm);
}

static void test_the_instances_counted_follow_the_mib(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	CHECK(!omci_pm_tick(pm, 1000u));

	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	omci_hw_counter_force(UNI, RX_OCTETS, 100u);
	CHECK(!omci_pm_tick(pm, 1001u));
	omci_hw_counter_force(UNI, RX_OCTETS, 130u);
	CHECK(!omci_pm_tick(pm, 1002u));
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 30u);

	/* Deleted and made again, it starts from zero. */
	CHECK_EQ(omci_mib_delete(mib, ME_ETH_PM_3, UNI_INSTANCE), OMCI_OK);
	CHECK(!omci_pm_tick(pm, 1003u));
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(!omci_pm_tick(pm, 1004u));
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);

	omci_hw_counter_force(UNI, RX_OCTETS, 150u);
	omci_pm_forget(pm, ME_ETH_PM_3, UNI_INSTANCE);
	omci_pm_forget(pm, ME_ETH_PM_3, 0x7777u);
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);
	omci_hw_counter_force(UNI, RX_OCTETS, 170u);
	CHECK(!omci_pm_tick(pm, 1005u));
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 20u);
	omci_pm_forget_all(pm);
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);

	pm_down(mib, pm);
}

static void test_no_room_for_another_instance(void)
{
	const uint16_t last = (uint16_t)(0x1000u + OMCI_PM_MAX_INSTANCES);
	struct omci_mib *mib;
	struct omci_pm *pm;
	char log[512];

	pm_up(&mib, &pm);
	for (uint16_t i = 0; i <= OMCI_PM_MAX_INSTANCES; i++)
		make(mib, ME_ETH_PM_3, (uint16_t)(0x1000u + i));

	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(log, { CHECK(!omci_pm_tick(pm, 1000u)); });
	CHECK_EQ(omci_t_log_count(log, "pm: no room to count 296/4160"), 1u);
	OMCI_T_CAPTURE_LOG(log, { CHECK(!omci_pm_tick(pm, 1001u)); });
	CHECK(strstr(log, "no room") == NULL);
	CHECK_EQ(omci_pm_refresh(pm, ME_ETH_PM_3, last, true), OMCI_ERR_NOMEM);

	omci_pm_forget_all(pm);
	OMCI_T_CAPTURE_LOG(log, { CHECK(!omci_pm_tick(pm, 1002u)); });
	CHECK_EQ(omci_t_log_count(log, "no room"), 1u);

	pm_down(mib, pm);
}

static void test_a_failed_read_keeps_the_last_counts(void)
{
	struct omci_mib *mib;
	struct omci_pm *pm;

	pm_up(&mib, &pm);
	make(mib, ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(!omci_pm_tick(pm, 1000u));
	omci_hw_counter_force(UNI, RX_OCTETS, 100u);
	CHECK(!omci_pm_tick(pm, 1001u));

	omci_hw_counter_fail_next(true);
	CHECK_EQ(omci_pm_refresh(pm, ME_ETH_PM_3, UNI_INSTANCE, true),
		 OMCI_ERR_HW);
	CHECK_EQ(get(mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 100u);
	CHECK(!omci_pm_tick(pm, 1002u));
	omci_hw_counter_fail_next(false);

	omci_hw_counter_force(UNI, RX_OCTETS, 150u);
	CHECK(!omci_pm_tick(pm, 1003u));
	CHECK_EQ(now_count(pm, mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 150u);

	pm_down(mib, pm);
}

/* Send a request to the engine; contents after the header, if any. */
static void request(struct omci_engine *eng, uint16_t tci, uint8_t mt,
		    uint16_t cls, uint16_t inst, const uint8_t *contents,
		    size_t len)
{
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tci, mt, true, false, cls, inst);
	if (contents != NULL)
		memcpy(&wire[8], contents, len);
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
}

static void test_get_and_get_current_data_through_the_engine(void)
{
	const uint8_t octets[2] = { 0x10u, 0x00u };
	struct omci_t_fixture fx;
	char log[512];

	hw_zero();
	CHECK(omci_t_up(&fx));
	make(fx.mib, ME_ETH_PM_3, UNI_INSTANCE);
	omci_engine_tick(fx.eng, 1000u);
	omci_hw_counter_force(UNI, RX_OCTETS, 500u);

	request(fx.eng, 0x0E00u, MT_GET_CURRENT, ME_ETH_PM_3, UNI_INSTANCE,
		octets, sizeof(octets));
	CHECK_EQ(get(fx.mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 500u);
	request(fx.eng, 0x0E01u, MT_GET, ME_ETH_PM_3, UNI_INSTANCE, octets,
		sizeof(octets));
	CHECK_EQ(get(fx.mib, ME_ETH_PM_3, UNI_INSTANCE, 4u), 0u);

	omci_hw_counter_fail_next(true);
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(log, {
		request(fx.eng, 0x0E02u, MT_GET_CURRENT, ME_ETH_PM_3,
			UNI_INSTANCE, octets, sizeof(octets));
	});
	CHECK(strstr(log, "could not be refreshed") != NULL);
	omci_hw_counter_fail_next(false);

	omci_t_down(&fx);
	hw_zero();
}

static void test_create_and_set_through_the_engine(void)
{
	const uint8_t octets[2] = { 0x10u, 0x00u };
	struct omci_t_fixture fx;
	uint8_t c[18];

	hw_zero();
	CHECK(omci_t_up(&fx));
	omci_engine_tick(fx.eng, 1000u);
	omci_hw_counter_force(UNI, RX_OCTETS, 50u);

	/* Counting starts when the OLT creates the entity. */
	ext_control(c, 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, 0u, 0u, 0u);
	request(fx.eng, 0x0F00u, MT_CREATE, ME_EXT_PM, 1u, c, 16u);
	omci_hw_counter_force(UNI, RX_OCTETS, 80u);
	request(fx.eng, 0x0F01u, MT_GET_CURRENT, ME_EXT_PM, 1u, octets,
		sizeof(octets));
	CHECK_EQ(get(fx.mib, ME_EXT_PM, 1u, 4u), 30u);

	/* A Set of the control block that asks for a clear. */
	c[0] = 0x40u;
	c[1] = 0x00u;
	ext_control(&c[2], 0u, ME_PPTP_ETH_UNI, UNI_INSTANCE, EXT_CLEAR, 0u,
		    0u);
	request(fx.eng, 0x0F02u, MT_SET, ME_EXT_PM, 1u, c, sizeof(c));
	request(fx.eng, 0x0F03u, MT_GET_CURRENT, ME_EXT_PM, 1u, octets,
		sizeof(octets));
	CHECK_EQ(get(fx.mib, ME_EXT_PM, 1u, 4u), 0u);

	omci_t_down(&fx);
	hw_zero();
}

/* The alarm notification of an entity among the frames sent, or NULL. */
static const uint8_t *notification(uint16_t cls, uint16_t inst)
{
	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++) {
		const uint8_t *f = omci_t_tx.frame[i];

		if ((f[2] & 0x1Fu) == MT_ALARM && omci_be16(&f[4]) == cls &&
		    omci_be16(&f[6]) == inst)
			return f;
	}
	return NULL;
}

static void test_an_alert_is_notified_and_cleared_at_the_interval_end(void)
{
	struct omci_t_fixture fx;
	const uint8_t *f;

	hw_zero();
	CHECK(omci_t_up(&fx));
	make(fx.mib, ME_THRESHOLD_1, 1u);
	put(fx.mib, ME_THRESHOLD_1, 1u, 1u, 10u, 4u);
	make(fx.mib, ME_ETH_PM_3, UNI_INSTANCE);
	put(fx.mib, ME_ETH_PM_3, UNI_INSTANCE, 2u, 1u, 2u);
	omci_engine_tick(fx.eng, 1000u);

	omci_hw_counter_force(UNI, RX_DROPS, 10u);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 1001u);
	f = notification(ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(f != NULL);
	if (f != NULL)
		CHECK_EQ(f[8], 0x80u);

	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 1900u);
	f = notification(ME_ETH_PM_3, UNI_INSTANCE);
	CHECK(f != NULL);
	if (f != NULL)
		CHECK_EQ(f[8], 0x00u);

	omci_t_down(&fx);
	hw_zero();
}

int main(void)
{
	RUN(test_the_surface);
	RUN(test_every_counter_is_accounted_for);
	RUN(test_the_ethernet_pm_counts_the_uni);
	RUN(test_the_ethernet_pm_2_and_3_count_what_the_uni_receives);
	RUN(test_get_reads_the_last_closed_interval);
	RUN(test_a_count_stops_at_its_most);
	RUN(test_the_gem_port_pm_counts_its_gem_port);
	RUN(test_the_bridge_port_entities_count_their_side);
	RUN(test_a_new_source_starts_from_its_readings);
	RUN(test_the_extended_pm_counts_what_its_control_block_names);
	RUN(test_an_extended_pm_can_count_continuously);
	RUN(test_an_extended_pm_is_cleared_on_request);
	RUN(test_the_fec_pm_counts_the_downstream_fec);
	RUN(test_a_count_that_reaches_its_threshold_raises_an_alert);
	RUN(test_the_extended_pm_can_disable_its_alerts);
	RUN(test_synchronize_starts_the_counts_again);
	RUN(test_a_daemon_that_stood_still_closes_the_missed_intervals);
	RUN(test_the_instances_counted_follow_the_mib);
	RUN(test_no_room_for_another_instance);
	RUN(test_a_failed_read_keeps_the_last_counts);
	RUN(test_get_and_get_current_data_through_the_engine);
	RUN(test_create_and_set_through_the_engine);
	RUN(test_an_alert_is_notified_and_cleared_at_the_interval_end);
	return OMCI_T_REPORT("test_pm");
}
