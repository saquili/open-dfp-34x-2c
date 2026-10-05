// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of edge cases of the data path: bridges, GEM ports and VLAN filters. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/bridge.h"
#include "me/gem.h"
#include "me/pm.h"
#include "me/table.h"
#include "me/upload.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

static char g_log[1u << 14];

static void set_u16(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		    uint8_t attr, uint16_t v)
{
	const uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, b, sizeof(b)),
		 OMCI_OK);
}

static void set_u8(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		   uint8_t attr, uint8_t v)
{
	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, &v, sizeof(v)),
		 OMCI_OK);
}

static void chain(struct omci_mib *mib, uint16_t inst, uint16_t port_id)
{
	CHECK_EQ(omci_mib_create(mib, 268u, inst), OMCI_OK);
	set_u16(mib, 268u, inst, OMCI_BRIDGE_GEM_ATTR_PORT_ID, port_id);
	CHECK_EQ(omci_mib_create(mib, 266u, inst), OMCI_OK);
	set_u16(mib, 266u, inst, OMCI_BRIDGE_GEMIW_ATTR_CTP, inst);
}

static void bridge_port(struct omci_mib *mib, uint16_t inst, uint8_t tp_type,
			uint16_t tp_ptr)
{
	CHECK_EQ(omci_mib_create(mib, 47u, inst), OMCI_OK);
	set_u16(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_BRIDGE, 1u);
	set_u8(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_TPTYPE, tp_type);
	set_u16(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_TPPTR, tp_ptr);
}

static void vlan_filter(struct omci_mib *mib, uint16_t inst,
			const uint16_t *vids, unsigned int n, uint8_t count)
{
	uint8_t table[24] = { 0 };

	CHECK_EQ(omci_mib_create(mib, 84u, inst), OMCI_OK);
	for (unsigned int i = 0; i < n && i < 12u; i++) {
		table[i * 2] = (uint8_t)(vids[i] >> 8);
		table[i * 2 + 1] = (uint8_t)vids[i];
	}
	CHECK_EQ(omci_mib_attr_set(mib, 84u, inst, OMCI_BRIDGE_VLAN_ATTR_TABLE,
				   table, sizeof(table)),
		 OMCI_OK);
	set_u8(mib, 84u, inst, OMCI_BRIDGE_VLAN_ATTR_COUNT, count);
}

static uint16_t g_refuse_a, g_refuse_b;

static enum omci_status spy_bridge(const struct omci_hw_bridge_port *p,
				   size_t n, struct omci_hw_reject *rej)
{
	(void)p;
	(void)n;
	*rej = (struct omci_hw_reject){ 0 };
	if (g_refuse_a != 0u)
		omci_hw_reject_add(rej, g_refuse_a);
	if (g_refuse_b != 0u)
		omci_hw_reject_add(rej, g_refuse_b);
	return rej->n != 0u ? OMCI_DECLINED : OMCI_OK;
}

static struct omci_hw_ops g_spy;

static void
test_a_mapper_whose_two_ports_the_device_refuses_is_one_refused_instance(
	struct omci_mib *mib)
{
	chain(mib, 0x0301u, 300u);
	chain(mib, 0x0302u, 301u);
	CHECK_EQ(omci_mib_create(mib, 130u, 0x0401u), OMCI_OK);
	set_u16(mib, 130u, 0x0401u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 0, 0x0301u);
	set_u16(mib, 130u, 0x0401u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 1, 0x0302u);
	bridge_port(mib, 0x0501u, OMCI_BRIDGE_TP_MAPPER, 0x0401u);
	bridge_port(mib, 0x0502u, OMCI_BRIDGE_TP_PPTP_ETH, 1u);

	g_spy = *omci_hw();
	g_spy.bridge_apply = spy_bridge;
	omci_hw_ops_set(&g_spy);
	g_refuse_a = 300u;
	g_refuse_b = 301u;
	unsigned int refused = 99;

	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_DECLINED);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	omci_hw_ops_set(NULL);
	CHECK_EQ(refused, 0u);
	CHECK(omci_bridge_instance_rejected(0x0501u));
	CHECK(!omci_bridge_instance_rejected(0x0502u));
	CHECK(strstr(g_log, "me130: mapper 0x0401") == NULL);
}

static void
test_a_p_bit_that_names_no_gem_port_is_skipped_and_said(struct omci_mib *mib)
{
	chain(mib, 0x0301u, 300u);
	CHECK_EQ(omci_mib_create(mib, 266u, 0x0310u), OMCI_OK);
	set_u16(mib, 266u, 0x0310u, OMCI_BRIDGE_GEMIW_ATTR_CTP, 0x7777u);
	CHECK_EQ(omci_mib_create(mib, 130u, 0x0401u), OMCI_OK);
	set_u16(mib, 130u, 0x0401u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 0, 0x0301u);
	set_u16(mib, 130u, 0x0401u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 2, 0x0310u);
	bridge_port(mib, 0x0501u, OMCI_BRIDGE_TP_MAPPER, 0x0401u);

	unsigned int refused = 99;

	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK(strstr(g_log,
		     "me130: mapper 0x0401 p-bit 2 points at ME 266 0x0310, which names no GEM port") !=
	      NULL);
	const struct omci_hw_bridge_port *p = NULL;

	CHECK_EQ(omci_bridge_last(&p), 1u);
	CHECK(p != NULL && p[0].port_id == 300u);
	CHECK_EQ(refused, 0u);
}

static void test_the_table_holds_more_than_the_count_and_the_count_wins(
	struct omci_mib *mib)
{
	const uint16_t vids[3] = { 0x0064u, 0x20c8u, 0x012cu };

	vlan_filter(mib, 0x0601u, vids, 3, 2);
	bridge_port(mib, 0x0601u, OMCI_BRIDGE_TP_PPTP_ETH, 1u);

	unsigned int refused = 0;

	CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
	const struct omci_hw_bridge_port *p = NULL;

	CHECK_EQ(omci_bridge_last(&p), 1u);
	CHECK(p != NULL);
	if (p != NULL) {
		CHECK_EQ(p[0].port_id, OMCI_HW_BRIDGE_UNI);
		CHECK_EQ(p[0].num_vlan, 2u);
		CHECK_EQ(p[0].vlan[0], 100u);
		CHECK_EQ(p[0].vlan[1], 0x0c8u);
	}
}

static void test_more_user_ports_than_the_device_carries(struct omci_mib *mib)
{
	for (uint16_t i = 0; i < OMCI_HW_BRIDGE_MAX_PORTS + 1u; i++)
		bridge_port(mib, (uint16_t)(0x0700u + i),
			    OMCI_BRIDGE_TP_PPTP_ETH, 1u);

	unsigned int refused = 0;

	CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
	const struct omci_hw_bridge_port *p = NULL;

	CHECK_EQ(omci_bridge_last(&p), (size_t)OMCI_HW_BRIDGE_MAX_PORTS);
	CHECK_EQ(refused, 1u);
}

static void test_two_bridge_ports_on_one_gem_port_are_one_port_of_the_device(
	struct omci_mib *mib)
{
	chain(mib, 0x0301u, 300u);
	bridge_port(mib, 0x0501u, OMCI_BRIDGE_TP_GEM_IW, 0x0301u);
	bridge_port(mib, 0x0502u, OMCI_BRIDGE_TP_GEM_IW, 0x0301u);

	unsigned int refused = 0;

	CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
	const struct omci_hw_bridge_port *p = NULL;

	CHECK_EQ(omci_bridge_last(&p), 1u);
	CHECK_EQ(refused, 0u);
}

static void
test_more_gem_bridge_ports_than_the_device_carries(struct omci_mib *mib)
{
	for (uint16_t i = 0; i < OMCI_HW_BRIDGE_MAX_PORTS + 1u; i++) {
		chain(mib, (uint16_t)(0x0800u + i), (uint16_t)(300u + i));
		bridge_port(mib, (uint16_t)(0x0900u + i), OMCI_BRIDGE_TP_GEM_IW,
			    (uint16_t)(0x0800u + i));
	}
	unsigned int refused = 0;

	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK(strstr(g_log,
		     "me47: more than 16 bridge ports — Port-ID 316 is NOT bridged") !=
	      NULL);
	const struct omci_hw_bridge_port *p = NULL;

	CHECK_EQ(omci_bridge_last(&p), (size_t)OMCI_HW_BRIDGE_MAX_PORTS);
	CHECK_EQ(refused, 1u);
}

static void gem(struct omci_mib *mib, uint16_t inst, uint16_t port_id,
		uint16_t tcont, uint8_t dir)
{
	CHECK_EQ(omci_mib_create(mib, 268u, inst), OMCI_OK);
	set_u16(mib, 268u, inst, OMCI_GEM_ATTR_PORT_ID, port_id);
	set_u16(mib, 268u, inst, OMCI_GEM_ATTR_TCONT, tcont);
	set_u8(mib, 268u, inst, OMCI_GEM_ATTR_DIRECTION, dir);
}

static void test_a_downstream_port_with_a_tcont_pointer_ignores_the_pointer(
	struct omci_mib *mib)
{
	gem(mib, 0x0201u, 400u, 0x8000u, OMCI_HW_GEM_DIR_DOWNSTREAM);
	unsigned int refused = 99;

	omci_log_level_set(OMCI_LOG_DEBUG);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK(strstr(g_log,
		     "me268: instance 0x0201 is downstream only and still points at T-CONT 0x8000; ignoring the pointer") !=
	      NULL);
	const struct omci_hw_gem_port *p = NULL;

	CHECK_EQ(omci_gem_last(&p), 1u);
	CHECK(p != NULL && p[0].port_id == 400u &&
	      p[0].direction == OMCI_HW_GEM_DIR_DOWNSTREAM && p[0].tcont == 0u);
	CHECK_EQ(refused, 0u);
}

static void test_more_gem_ports_than_one_reconcile_carries(struct omci_mib *mib)
{
	for (uint16_t i = 0; i < OMCI_HW_GEM_MAX_PORTS + 1u; i++)
		gem(mib, (uint16_t)(0x0300u + i), (uint16_t)(500u + i), 0x0000u,
		    OMCI_HW_GEM_DIR_DOWNSTREAM);

	unsigned int refused = 0;

	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK(strstr(g_log,
		     "me268: more than 32 GEM ports — instance 0x0320 is NOT programmed") !=
	      NULL);
	const struct omci_hw_gem_port *p = NULL;

	CHECK_EQ(omci_gem_last(&p), (size_t)OMCI_HW_GEM_MAX_PORTS);
	CHECK_EQ(refused, 1u);
	CHECK(omci_gem_instance_rejected(0x0320u));
	CHECK(!omci_gem_instance_rejected(0x0300u));
}

static void test_every_fate_has_a_name(struct omci_mib *mib)
{
	(void)mib;
	CHECK_EQ(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_PROGRAMMED),
			"programmed"),
		 0);
	CHECK_EQ(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_COLLAPSED),
			"collapsed"),
		 0);
	CHECK_EQ(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_SHADOWED),
			"shadowed"),
		 0);
	CHECK_EQ(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_OVER_BUDGET),
			"over-budget"),
		 0);
	CHECK_EQ(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_UNSUPPORTED),
			"unsupported"),
		 0);
	CHECK_EQ(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_INERT), "inert"), 0);
	CHECK_EQ(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_REFUSED), "refused"),
		 0);
	CHECK_EQ(strcmp(omci_vlan_fate_name((enum omci_vlan_fate)99), "?"), 0);
}

static void rule_row(uint8_t out[16], unsigned int pri, unsigned int match,
		     unsigned int assign)
{
	const uint32_t w[4] = {
		0xf8000000u,
		(pri << 28) | (match << 15) | (5u << 12),
		0x400f0000u,
		(pri << 16) | (assign << 3) | 2u,
	};
	for (int k = 0; k < 4; k++) {
		out[k * 4 + 0] = (uint8_t)(w[k] >> 24);
		out[k * 4 + 1] = (uint8_t)(w[k] >> 16);
		out[k * 4 + 2] = (uint8_t)(w[k] >> 8);
		out[k * 4 + 3] = (uint8_t)w[k];
	}
}

static void
test_a_row_is_refused_only_when_it_decodes_to_the_rule_the_device_refused(
	struct omci_mib *mib)
{
	uint8_t row[16], other[16];

	rule_row(row, 6, 410, 1410);
	rule_row(other, 6, 99, 1900);
	CHECK_EQ(omci_mib_create(mib, 171u, 0x0101u), OMCI_OK);
	CHECK_EQ(omci_mib_table_put(mib, 171u, 0x0101u, 6u, row, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);

	omci_hw_vlan_reject_next(410);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_DECLINED);

	CHECK(omci_vlan_row_refused(row, sizeof(row)));
	CHECK(!omci_vlan_row_refused(other, sizeof(other)));
	CHECK(!omci_vlan_row_refused(row, 8));
	CHECK(!omci_vlan_row_refused(NULL, 16));
}

static void
test_a_counter_index_nobody_knows_has_no_reason(struct omci_mib *mib)
{
	(void)mib;
	const char *why = "stale";

	CHECK(!omci_pm_counter_accounted(0xfffeu, &why));
	CHECK(why == NULL);
	CHECK(!omci_pm_counter_accounted(0xfffeu, NULL));
}

static void test_the_tracking_slots_run_out_honestly(struct omci_mib *mib)
{
	struct omci_pm *pm = NULL;

	const uint16_t over = OMCI_PM_MAX_INSTANCES;

	CHECK_EQ(omci_pm_init(&pm, mib), OMCI_OK);
	for (uint16_t i = 0; i <= over; i++)
		CHECK_EQ(omci_mib_create(mib, 321u, i), OMCI_OK);

	for (uint16_t i = 0; i < over; i++)
		CHECK_EQ(omci_pm_refresh(pm, 321u, i, true), OMCI_OK);
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_pm_refresh(pm, 321u, over, true), OMCI_ERR_NOMEM);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK(strstr(g_log, "pm: no room to count 321/64") != NULL);

	omci_pm_forget(pm, 321u, 3u);
	CHECK_EQ(omci_pm_refresh(pm, 321u, over, true), OMCI_OK);
	omci_pm_fini(pm);
}

static void test_the_model_keeps_what_the_modules_rely_on(struct omci_mib *mib)
{
	(void)mib;
	for (size_t k = 0; k < omci_me_class_count; k++) {
		const struct omci_me_class *cls = &omci_me_classes[k];

		CHECK(cls->attr_count >= 1u &&
		      cls->attr_count <= OMCI_ATTR_MAX);
		for (uint8_t n = 1; n <= cls->attr_count; n++) {
			const struct omci_me_attr *a = omci_me_attr_get(cls, n);

			CHECK(a != NULL);
			if (a == NULL)
				continue;
			CHECK(omci_attr_bit(n) != 0u);
			CHECK(a->size >= 1u);
			if (!omci_me_attr_is_table(a)) {
				CHECK(a->size <= OMCI_ME_UPLOAD_BYTES);
				CHECK(a->size <= OMCI_MIB_TABLE_ENTRY_MAX);
			}
		}
	}
}

int main(void)
{
	omci_log_level_set(OMCI_LOG_ERR);
	RUN_MIB(test_a_mapper_whose_two_ports_the_device_refuses_is_one_refused_instance);
	RUN_MIB(test_a_p_bit_that_names_no_gem_port_is_skipped_and_said);
	RUN_MIB(test_the_table_holds_more_than_the_count_and_the_count_wins);
	RUN_MIB(test_more_user_ports_than_the_device_carries);
	RUN_MIB(test_two_bridge_ports_on_one_gem_port_are_one_port_of_the_device);
	RUN_MIB(test_more_gem_bridge_ports_than_the_device_carries);
	RUN_MIB(test_a_downstream_port_with_a_tcont_pointer_ignores_the_pointer);
	RUN_MIB(test_more_gem_ports_than_one_reconcile_carries);
	RUN_MIB(test_every_fate_has_a_name);
	RUN_MIB(test_a_row_is_refused_only_when_it_decodes_to_the_rule_the_device_refused);
	RUN_MIB(test_a_counter_index_nobody_knows_has_no_reason);
	RUN_MIB(test_the_tracking_slots_run_out_honestly);
	RUN_MIB(test_the_model_keeps_what_the_modules_rely_on);
	return OMCI_T_REPORT("test_me_edges");
}
