// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the bridges programmed from MAC bridge ports, mappers and GEM ports. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/bridge.h"
#include "mib/store.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

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

static void gem(struct omci_mib *mib, uint16_t inst, uint16_t port_id)
{
	CHECK_EQ(omci_mib_create(mib, 268u, inst), OMCI_OK);
	set_u16(mib, 268u, inst, OMCI_BRIDGE_GEM_ATTR_PORT_ID, port_id);
}

static void iwtp(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		 uint16_t ctp)
{
	CHECK_EQ(omci_mib_create(mib, cls, inst), OMCI_OK);
	set_u16(mib, cls, inst, OMCI_BRIDGE_GEMIW_ATTR_CTP, ctp);
}

static void bridge_port(struct omci_mib *mib, uint16_t inst, uint16_t bridge,
			uint8_t num, uint8_t tp_type, uint16_t tp_ptr)
{
	CHECK_EQ(omci_mib_create(mib, 47u, inst), OMCI_OK);
	set_u16(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_BRIDGE, bridge);
	set_u8(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_NUM, num);
	set_u8(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_TPTYPE, tp_type);
	set_u16(mib, 47u, inst, OMCI_BRIDGE_PORT_ATTR_TPPTR, tp_ptr);
}

static void vlan_filter(struct omci_mib *mib, uint16_t inst,
			const uint16_t *vids, unsigned int n)
{
	uint8_t table[24] = { 0 };

	CHECK_EQ(omci_mib_create(mib, 84u, inst), OMCI_OK);
	for (unsigned int i = 0; i < n && i < 12; i++) {
		table[i * 2] = (uint8_t)(vids[i] >> 8);
		table[i * 2 + 1] = (uint8_t)vids[i];
	}
	CHECK_EQ(omci_mib_attr_set(mib, 84u, inst, OMCI_BRIDGE_VLAN_ATTR_TABLE,
				   table, sizeof(table)),
		 OMCI_OK);
	set_u8(mib, 84u, inst, OMCI_BRIDGE_VLAN_ATTR_COUNT, (uint8_t)n);
}

static const struct omci_hw_bridge_port *
port_of(const struct omci_hw_bridge_port *p, size_t n, uint16_t port_id)
{
	for (size_t i = 0; i < n; i++)
		if (p[i].port_id == port_id)
			return &p[i];
	return NULL;
}

static bool carries(const struct omci_hw_bridge_port *p, uint16_t vid)
{
	if (p == NULL)
		return false;
	for (uint8_t i = 0; i < p->num_vlan; i++)
		if (p->vlan[i] == vid)
			return true;
	return false;
}

int main(void)
{
	struct omci_mib *mib = NULL;
	const struct omci_hw_bridge_port *ap = NULL;
	const uint16_t v1410[] = { 1410u };
	const uint16_t v1430[] = { 1430u };
	const uint16_t v1420[] = { 1420u };
	const uint16_t vall[] = { 1410u, 1420u, 1430u };
	unsigned int refused = 0;
	size_t n;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);

	gem(mib, 0x0116u, 278u);
	gem(mib, 0x0117u, 279u);
	gem(mib, 0x0118u, 280u);
	gem(mib, 0x0119u, 281u);
	gem(mib, 0x07feu, 2046u);
	gem(mib, 0x07ffu, 2047u);
	iwtp(mib, 266u, 0x0116u, 0x0116u);
	iwtp(mib, 266u, 0x0117u, 0x0117u);
	iwtp(mib, 266u, 0x0118u, 0x0118u);
	iwtp(mib, 266u, 0x0119u, 0x0119u);
	iwtp(mib, 266u, 0x07feu, 0x07feu);
	iwtp(mib, 281u, 0x0001u, 0x07ffu);

	CHECK_EQ(omci_mib_create(mib, 130u, 0x1102u), OMCI_OK);
	set_u16(mib, 130u, 0x1102u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 0, 0x0116u);
	set_u16(mib, 130u, 0x1102u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 5, 0x0117u);
	set_u16(mib, 130u, 0x1102u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 6, 0x0117u);
	CHECK_EQ(omci_mib_create(mib, 130u, 0x1103u), OMCI_OK);
	set_u16(mib, 130u, 0x1103u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 4, 0x0118u);
	CHECK_EQ(omci_mib_create(mib, 130u, 0x1104u), OMCI_OK);
	set_u16(mib, 130u, 0x1104u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 4, 0x0119u);

	CHECK_EQ(omci_mib_create(mib, 45u, 0x0101u), OMCI_OK);
	bridge_port(mib, 0x0101u, 0x0101u, 1u, OMCI_BRIDGE_TP_PPTP_ETH,
		    0x0101u);
	bridge_port(mib, 0x1102u, 0x0101u, 2u, OMCI_BRIDGE_TP_MAPPER, 0x1102u);
	bridge_port(mib, 0x1103u, 0x0101u, 3u, OMCI_BRIDGE_TP_MAPPER, 0x1103u);
	bridge_port(mib, 0x1104u, 0x0101u, 4u, OMCI_BRIDGE_TP_MAPPER, 0x1104u);
	bridge_port(mib, 0x110bu, 0x0101u, 11u, OMCI_BRIDGE_TP_GEM_IW, 0x07feu);
	bridge_port(mib, 0x110au, 0x0101u, 10u, OMCI_BRIDGE_TP_MCAST_GEM,
		    0x0001u);

	vlan_filter(mib, 0x1102u, v1410, 1);
	vlan_filter(mib, 0x1103u, v1430, 1);
	vlan_filter(mib, 0x1104u, v1420, 1);
	vlan_filter(mib, 0x110bu, vall, 3);

	CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
	CHECK_EQ(refused, 0u);

	n = omci_hw_bridge_recorded(&ap);

	CHECK_EQ(n, 7u);
	printf("    the operator's six bridge ports resolve to seven interfaces\n");

	CHECK(port_of(ap, n, OMCI_HW_BRIDGE_UNI) != NULL);
	CHECK(port_of(ap, n, 278u) != NULL);
	CHECK(port_of(ap, n, 279u) != NULL);
	CHECK(port_of(ap, n, 280u) != NULL);
	CHECK(port_of(ap, n, 281u) != NULL);
	CHECK(port_of(ap, n, 2046u) != NULL);
	CHECK(port_of(ap, n, 2047u) != NULL);
	printf("    three hops of pointers: ME 47 -> ME 130 -> ME 266 -> ME 268 -> Port-ID\n");

	CHECK(carries(port_of(ap, n, 278u), 1410u));
	CHECK(carries(port_of(ap, n, 279u), 1410u));
	CHECK(carries(port_of(ap, n, 280u), 1430u));
	CHECK(carries(port_of(ap, n, 281u), 1420u));
	CHECK(carries(port_of(ap, n, 2046u), 1410u));
	CHECK(carries(port_of(ap, n, 2046u), 1420u));
	CHECK(carries(port_of(ap, n, 2046u), 1430u));
	CHECK_EQ(port_of(ap, n, 2046u)->num_vlan, 3u);
	printf("    ME 84's filter list reaches the port it is keyed to\n");

	CHECK_EQ(port_of(ap, n, OMCI_HW_BRIDGE_UNI)->num_vlan, 0u);

	{
		bridge_port(mib, 0x1105u, 0x0101u, 5u, OMCI_BRIDGE_TP_GEM_IW,
			    0x0fffu);
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_hw_bridge_recorded(&ap), 7u);
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x1105u), OMCI_OK);
	}

	{
		bridge_port(mib, 0x1106u, 0x0101u, 6u, 2u, 0x0001u);
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x1106u), OMCI_OK);
	}
	printf("    a pointer to nothing and a kind we do not bridge are each refused alone\n");

	{
		const struct omci_hw_bridge_port *before = NULL;
		const size_t before_n = omci_bridge_last(&before);

		char log[512];

		omci_hw_bridge_fail_next();
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_bridge_program(mib, &refused),
				 OMCI_ERR_HW);
		});
		CHECK_EQ(omci_bridge_last(&before), before_n);
		char says[96];
		(void)snprintf(
			says, sizeof(says),
			"me47: the device refused a bridge of %zu port(s): OMCI_ERR_HW",
			before_n);
		CHECK_EQ(omci_t_log_count(log, says), 1u);
		printf("    a refused apply leaves the record of what is in force alone\n");
	}

	{
		CHECK(!omci_bridge_instance_rejected(0x1102u));

		omci_hw_bridge_reject_next(279u);
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_DECLINED);

		CHECK(omci_bridge_instance_rejected(0x1102u));
		CHECK(!omci_bridge_instance_rejected(0x1103u));
		CHECK(!omci_bridge_instance_rejected(0x110bu));
		printf("    the mapper's refused second GEM port names ME 47 0x1102 and no other port\n");

		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		CHECK(!omci_bridge_instance_rejected(0x1102u));
		printf("    and a device that stops refusing stops being reported\n");

		omci_hw_bridge_reject_next(279u);
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_DECLINED);
		CHECK(omci_bridge_instance_rejected(0x1102u));
		CHECK(!omci_bridge_instance_rejected(0u));
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		CHECK(!omci_bridge_instance_rejected(0x1102u));
	}

	{
		bridge_port(mib, 0x1107u, 0x0101u, 7u, OMCI_BRIDGE_TP_GEM_IW,
			    0x0ffeu);
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK(!omci_bridge_instance_rejected(0x1107u));
		printf("    a port whose pointer has not arrived yet is counted, never reported\n");
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x1107u), OMCI_OK);
	}

	{
		gem(mib, 0x07fdu, 0u);
		iwtp(mib, 266u, 0x07fdu, 0x07fdu);
		bridge_port(mib, 0x1108u, 0x0101u, 8u, OMCI_BRIDGE_TP_GEM_IW,
			    0x07fdu);
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 0u);
		n = omci_hw_bridge_recorded(&ap);
		CHECK_EQ(n, 8u);
		CHECK(OMCI_HW_BRIDGE_UNI != 0u);
		CHECK(port_of(ap, n, 0u) != NULL);
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x1108u), OMCI_OK);
		printf("    Port-ID 0 resolves like any other\n");
	}

	{
		uint16_t twelve[12];

		for (unsigned int i = 0; i < 12u; i++)
			twelve[i] = (uint16_t)(101u + i);
		gem(mib, 0x07fcu, 3000u);
		iwtp(mib, 266u, 0x07fcu, 0x07fcu);
		bridge_port(mib, 0x1109u, 0x0101u, 9u, OMCI_BRIDGE_TP_GEM_IW,
			    0x07fcu);
		vlan_filter(mib, 0x1109u, twelve, 12);
		for (unsigned int count = 0; count <= 20u; count += 4u) {
			if (count == 12u)
				continue;
			set_u8(mib, 84u, 0x1109u, OMCI_BRIDGE_VLAN_ATTR_COUNT,
			       (uint8_t)count);
			CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
			n = omci_hw_bridge_recorded(&ap);
			const struct omci_hw_bridge_port *p =
				port_of(ap, n, 3000u);
			CHECK(p != NULL);
			if (p == NULL)
				continue;

			CHECK_EQ(p->num_vlan,
				 count == 0u || count > 12u ? 12u : count);
			for (unsigned int i = 0; i < p->num_vlan; i++)
				CHECK_EQ(p->vlan[i], 101u + i);
		}
		set_u8(mib, 84u, 0x1109u, OMCI_BRIDGE_VLAN_ATTR_COUNT, 12u);
		CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		n = omci_hw_bridge_recorded(&ap);
		CHECK_EQ(port_of(ap, n, 3000u)->num_vlan, 12u);
		CHECK(carries(port_of(ap, n, 3000u), 112u));
		printf("    all twelve entries of a filter table reach the port\n");
	}

	{
		char log[1024];

		CHECK_EQ(omci_mib_create(mib, 130u, 0x1110u), OMCI_OK);
		set_u16(mib, 130u, 0x1110u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 0,
			0x0116u);
		set_u16(mib, 130u, 0x1110u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 1,
			0xffffu);
		set_u16(mib, 130u, 0x1110u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 2,
			0u);
		bridge_port(mib, 0x110cu, 0x0101u, 12u, OMCI_BRIDGE_TP_MAPPER,
			    0x1110u);
		omci_log_level_set(OMCI_LOG_WARN);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		});
		CHECK(strstr(log, "mapper 0x1110") == NULL);
		CHECK_EQ(refused, 0u);

		set_u16(mib, 130u, 0x1110u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 3,
			0x0fffu);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		});
		omci_log_level_set(OMCI_LOG_ERR);
		CHECK_EQ(
			omci_t_log_count(
				log,
				"me130: mapper 0x1110 p-bit 3 points at ME 266 0x0fff, which names no GEM port"),
			1u);
		CHECK_EQ(omci_t_log_count(log, "mapper 0x1110"), 1u);
		printf("    a null pointer of a mapper is silent, a pointer to nothing is said once\n");
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x110cu), OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, 130u, 0x1110u), OMCI_OK);
	}

	{
		char log[1024];

		gem(mib, 0x0100u, 3333u);
		iwtp(mib, 266u, 0x0100u, 0x0100u);
		CHECK_EQ(omci_mib_create(mib, 130u, 0x1112u), OMCI_OK);
		set_u16(mib, 130u, 0x1112u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 7,
			0x0116u);
		set_u8(mib, 130u, 0x1112u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 8,
		       1u);
		bridge_port(mib, 0x110du, 0x0101u, 13u, OMCI_BRIDGE_TP_MAPPER,
			    0x1112u);
		omci_log_level_set(OMCI_LOG_WARN);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		});
		omci_log_level_set(OMCI_LOG_ERR);
		CHECK_EQ(refused, 0u);
		CHECK(strstr(log, "mapper 0x1112") == NULL);
		n = omci_hw_bridge_recorded(&ap);
		CHECK(port_of(ap, n, 3333u) == NULL);
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x110du), OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, 130u, 0x1112u), OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, 266u, 0x0100u), OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, 268u, 0x0100u), OMCI_OK);
	}

	{
		char log[1024];

		CHECK_EQ(omci_mib_create(mib, 130u, 0x1113u), OMCI_OK);
		set_u16(mib, 130u, 0x1113u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 0,
			0x0116u);
		set_u16(mib, 130u, 0x1113u, OMCI_BRIDGE_MAPPER_ATTR_FIRST + 1,
			0x0116u);
		bridge_port(mib, 0x110eu, 0x0101u, 14u, OMCI_BRIDGE_TP_MAPPER,
			    0x1113u);
		omci_log_level_set(OMCI_LOG_DEBUG);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		});
		omci_log_level_set(OMCI_LOG_ERR);
		CHECK(strstr(log, "mapper 0x1113") == NULL);
		n = omci_hw_bridge_recorded(&ap);

		char says[96];
		(void)snprintf(
			says, sizeof(says),
			"me47: %zu bridge port(s) programmed, %u unresolved, %u refused by the device",
			n, refused, 0u);
		CHECK_EQ(omci_t_log_count(log, says), 1u);
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x110eu), OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, 130u, 0x1113u), OMCI_OK);
	}

	{
		char log[1024];

		bridge_port(mib, 0x110fu, 0x0101u, 15u, OMCI_BRIDGE_TP_GEM_IW,
			    0x0ff0u);
		bridge_port(mib, 0x1114u, 0x0101u, 16u,
			    OMCI_BRIDGE_TP_MCAST_GEM, 0x0ff1u);
		omci_log_level_set(OMCI_LOG_WARN);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_bridge_program(mib, &refused), OMCI_OK);
		});
		omci_log_level_set(OMCI_LOG_ERR);
		CHECK_EQ(refused, 2u);
		CHECK_EQ(
			omci_t_log_count(
				log,
				"me47: port 0x110f points at ME 266 0x0ff0, which names no GEM port"),
			1u);
		CHECK_EQ(
			omci_t_log_count(
				log,
				"me47: port 0x1114 points at ME 281 0x0ff1, which names no GEM port"),
			1u);
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x110fu), OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, 47u, 0x1114u), OMCI_OK);
	}

	omci_mib_fini(mib);
	return OMCI_T_REPORT("test_bridge");
}
