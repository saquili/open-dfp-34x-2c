// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the host backend. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"

#include "harness.h"
#include "omcid/array_size.h"

static void
test_a_forced_reading_before_any_read_starts_from_the_recorded_stick(void)
{
	omci_hw_optics_force(OMCI_HW_OPTIC_TX_POWER, true, 777u);

	struct omci_hw_optics o;

	CHECK_EQ(omci_hw_optics(&o), OMCI_OK);
	CHECK_EQ(o.tx_power, 777u);
	CHECK_EQ(o.temperature, 12405u);
	CHECK_EQ(o.voltage, 32327u);
	CHECK_EQ(o.bias, 6950u);
	CHECK_EQ(o.rx_power, 34u);
	CHECK_EQ(o.valid, 0x1fu);

	omci_hw_optics_force(OMCI_HW_OPTIC_RX_POWER, false, 0u);
	CHECK_EQ(omci_hw_optics(&o), OMCI_OK);
	CHECK_EQ(o.valid, 0x0fu);

	for (int i = 0; i < OMCI_HW_OPTIC_COUNT; i++)
		omci_hw_optics_force((enum omci_hw_optic)i, false, 0u);
	CHECK_EQ(omci_hw_optics(&o), OMCI_ERR_HW);
	CHECK_EQ(o.valid, 0u);

	omci_hw_optics_force((enum omci_hw_optic)99, true, 5u);
	CHECK_EQ(omci_hw_optics(&o), OMCI_ERR_HW);
	CHECK_EQ(omci_hw_optics(NULL), OMCI_ERR_INVAL);

	omci_hw_optics_reset();
	CHECK_EQ(omci_hw_optics(&o), OMCI_OK);
	CHECK_EQ(o.tx_power, 16412u);
	CHECK_EQ(o.valid, 0x1fu);
}

static void
test_a_forced_threshold_before_any_read_starts_from_the_defaults(void)
{
	omci_hw_optics_limit_force(OMCI_HW_LIMIT_RX_LOW, true, 20u);

	struct omci_hw_optics_limits l;

	CHECK_EQ(omci_hw_optics_limits(&l), OMCI_OK);
	CHECK_EQ(l.value[OMCI_HW_LIMIT_RX_LOW], 20u);
	CHECK_EQ(l.value[OMCI_HW_LIMIT_BIAS_HIGH], 50000u);
	CHECK_EQ(l.valid, 0x1fu);

	for (int i = 0; i < OMCI_HW_LIMIT_COUNT; i++)
		omci_hw_optics_limit_force((enum omci_hw_limit)i, false, 0u);
	CHECK_EQ(omci_hw_optics_limits(&l), OMCI_ERR_HW);
	CHECK_EQ(l.valid, 0u);

	omci_hw_optics_limit_force((enum omci_hw_limit)99, true, 5u);
	CHECK_EQ(omci_hw_optics_limits(&l), OMCI_ERR_HW);
	CHECK_EQ(omci_hw_optics_limits(NULL), OMCI_ERR_INVAL);

	omci_hw_optics_limits_reset();
	CHECK_EQ(omci_hw_optics_limits(&l), OMCI_OK);
	CHECK_EQ(l.value[OMCI_HW_LIMIT_RX_LOW], 10u);
}

static void test_the_receiver_level_is_forced(void)
{
	struct omci_hw_pon p;

	omci_hw_pon_level_force(true, 1u);
	CHECK_EQ(omci_hw_pon(&p), OMCI_OK);
	CHECK(p.power_level_valid);
	CHECK_EQ(p.power_level, 1u);
	omci_hw_pon_level_force(false, 0u);
}

static void test_the_capabilities_need_somewhere_to_go(void)
{
	struct omci_hw_caps caps;

	CHECK_EQ(omci_hw_caps(NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(caps.port_count, 2u);
	CHECK_EQ(caps.num_tcont, 4u);
	CHECK(strcmp(omci_hw_source(),
		     "synthetic ONU (host build, no device present)") == 0);
}

static void test_the_host_device_says_all_of_what_it_is(void)
{
	struct omci_hw_caps caps;

	memset(&caps, 0xa5, sizeof(caps));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);

	CHECK_EQ(caps.port_count, 2u);
	CHECK_EQ(caps.ports[0].pptp_instance, 0x0101u);
	CHECK_EQ(caps.ports[0].cardholder, 0x0101u);
	CHECK_EQ(caps.ports[0].plugin_type, 0x2fu);
	CHECK_EQ(caps.ports[1].pptp_instance, 0u);
	CHECK_EQ(caps.ports[1].cardholder, 0x0180u);
	CHECK_EQ(caps.ports[1].plugin_type, 0xf8u);
	for (unsigned int i = 2; i < ARRAY_SIZE(caps.ports); i++) {
		CHECK_EQ(caps.ports[i].pptp_instance, 0u);
		CHECK_EQ(caps.ports[i].cardholder, 0u);
		CHECK_EQ(caps.ports[i].plugin_type, 0u);
	}
	CHECK_EQ(caps.ani_instance, 0x8001u);
	CHECK_EQ(caps.ip_host_count, 1u);
	CHECK_EQ(caps.sw_committed_image, 0u);
	CHECK_EQ(caps.num_gem_ports, 32u);

	static const char maker[sizeof(caps.identity.vendor_id)] = "ZZZZ";
	static const char version[sizeof(caps.identity.version)] =
		"0.1-synthetic";
	static const char equipment[sizeof(caps.identity.equipment_id)] =
		"SYNTHETIC-ONU";
	static const char sw0[sizeof(caps.identity.sw_version[0])] =
		"0.1.0-synth";
	static const char sw1[sizeof(caps.identity.sw_version[1])] =
		"0.0.0-empty";
	CHECK_MEM(caps.identity.vendor_id, maker, sizeof(maker));
	CHECK_MEM(caps.identity.version, version, sizeof(version));
	CHECK_MEM(caps.identity.equipment_id, equipment, sizeof(equipment));
	CHECK_MEM(caps.identity.sw_version[0], sw0, sizeof(sw0));
	CHECK_MEM(caps.identity.sw_version[1], sw1, sizeof(sw1));
	CHECK_MEM(caps.identity.serial,
		  ((const uint8_t[8]){ 'Z', 'Z', 'Z', 'Z', 0, 0, 0, 1 }), 8);
	CHECK_EQ(caps.identity.tm_option, 0u);
	CHECK_EQ(caps.identity.omcc_version, 0xa0u);
	CHECK_EQ(caps.identity.vendor_product_code, 1u);
	CHECK_MEM(caps.identity.mac,
		  ((const uint8_t[6]){ 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 }),
		  6);

	CHECK_EQ(caps.num_tcont, 4u);
	CHECK_EQ(caps.num_priq_per_tcont, 2u);
	CHECK_EQ(caps.num_priq_per_port, 2u);
	CHECK_EQ(caps.uni_port_count, 1u);
}

static void test_a_set_that_is_refused_leaves_the_last_good_set_recorded(void)
{
	struct omci_hw_vlan_xlate v[OMCI_HW_VLAN_MAX_RULES + 1u];
	struct omci_hw_gem_port g[OMCI_HW_GEM_MAX_PORTS + 1u];
	struct omci_hw_bridge_port b[OMCI_HW_BRIDGE_MAX_PORTS + 1u];

	memset(v, 0, sizeof(v));
	memset(g, 0, sizeof(g));
	memset(b, 0, sizeof(b));

	v[0] = (struct omci_hw_vlan_xlate){ .kind = OMCI_HW_VLAN_CVID_XLATE,
					    .match_cvid = 410,
					    .assign_cvid = 1410 };
	g[0] = (struct omci_hw_gem_port){ .port_id = 278,
					  .direction = OMCI_HW_GEM_DIR_BOTH,
					  .tcont = 1 };
	b[0] = (struct omci_hw_bridge_port){
		.bridge = 1, .port_id = 278, .num_vlan = 1, .vlan = { 1410 }
	};
	struct omci_hw_reject rej = { .n = 9 };

	CHECK_EQ(omci_hw_vlan_apply(v, 1, &rej), OMCI_OK);
	CHECK_EQ(rej.n, 0u);
	CHECK_EQ(omci_hw_gem_apply(g, 1, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_bridge_apply(b, 1, NULL), OMCI_OK);

	rej.n = 9;
	CHECK_EQ(omci_hw_vlan_apply(NULL, 1, &rej), OMCI_ERR_INVAL);
	CHECK_EQ(rej.n, 0u);
	CHECK_EQ(omci_hw_vlan_apply(v, OMCI_HW_VLAN_MAX_RULES + 1u, &rej),
		 OMCI_ERR_NOMEM);
	CHECK_EQ(omci_hw_gem_apply(NULL, 1, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_hw_gem_apply(g, OMCI_HW_GEM_MAX_PORTS + 1u, NULL),
		 OMCI_ERR_NOMEM);
	{
		uint16_t t[OMCI_HW_TCONT_MAX + 1u] = { 0 };

		CHECK_EQ(omci_hw_tcont_apply(NULL, 1, NULL), OMCI_ERR_INVAL);
		CHECK_EQ(omci_hw_tcont_apply(t, OMCI_HW_TCONT_MAX + 1u, NULL),
			 OMCI_ERR_NOMEM);
	}
	CHECK_EQ(omci_hw_bridge_apply(NULL, 1, NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_hw_bridge_apply(b, OMCI_HW_BRIDGE_MAX_PORTS + 1u, NULL),
		 OMCI_ERR_NOMEM);

	const struct omci_hw_vlan_xlate *rv = NULL;
	const struct omci_hw_gem_port *rg = NULL;
	const struct omci_hw_bridge_port *rb = NULL;

	CHECK_EQ(omci_hw_vlan_recorded(&rv), 1u);
	CHECK_EQ(rv[0].match_cvid, 410u);
	CHECK_EQ(omci_hw_gem_recorded(&rg), 1u);
	CHECK_EQ(rg[0].port_id, 278u);
	CHECK_EQ(omci_hw_bridge_recorded(&rb), 1u);
	CHECK_EQ(rb[0].port_id, 278u);

	CHECK_EQ(omci_hw_vlan_apply(NULL, 0, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_vlan_recorded(NULL), 0u);
	CHECK_EQ(omci_hw_gem_apply(NULL, 0, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_gem_recorded(NULL), 0u);
	CHECK_EQ(omci_hw_bridge_apply(NULL, 0, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_bridge_recorded(NULL), 0u);
}

static void test_a_refusal_that_is_asked_for_is_given_once(void)
{
	struct omci_hw_vlan_xlate v[2];
	struct omci_hw_gem_port g[2];
	struct omci_hw_bridge_port b[2];
	struct omci_hw_reject rej = { 0 };

	memset(v, 0, sizeof(v));
	memset(g, 0, sizeof(g));
	memset(b, 0, sizeof(b));
	v[0] = (struct omci_hw_vlan_xlate){ .kind = OMCI_HW_VLAN_CVID_XLATE,
					    .match_cvid = 410,
					    .assign_cvid = 1410 };
	v[1] = (struct omci_hw_vlan_xlate){ .kind = OMCI_HW_VLAN_CVID_XLATE,
					    .match_cvid = 36,
					    .assign_cvid = 1502 };
	g[0] = (struct omci_hw_gem_port){ .port_id = 278,
					  .direction = OMCI_HW_GEM_DIR_BOTH,
					  .tcont = 1 };
	g[1] = (struct omci_hw_gem_port){ .port_id = 279,
					  .direction = OMCI_HW_GEM_DIR_BOTH,
					  .tcont = 1 };
	b[0] = (struct omci_hw_bridge_port){
		.bridge = 1, .port_id = 278, .num_vlan = 1, .vlan = { 1410 }
	};
	b[1] = (struct omci_hw_bridge_port){
		.bridge = 1, .port_id = 279, .num_vlan = 1, .vlan = { 1410 }
	};

	omci_hw_vlan_reject_next(36);
	CHECK_EQ(omci_hw_vlan_apply(v, 2, &rej), OMCI_DECLINED);
	CHECK_EQ(rej.n, 1u);
	CHECK(omci_hw_reject_has(&rej, 36));
	CHECK_EQ(omci_hw_vlan_apply(v, 2, &rej), OMCI_OK);
	CHECK_EQ(rej.n, 0u);

	omci_hw_gem_reject_next(279);
	CHECK_EQ(omci_hw_gem_apply(g, 2, &rej), OMCI_DECLINED);
	CHECK(omci_hw_reject_has(&rej, 279));
	CHECK_EQ(omci_hw_gem_apply(g, 2, &rej), OMCI_OK);
	CHECK_EQ(rej.n, 0u);

	omci_hw_bridge_reject_next(279);
	CHECK_EQ(omci_hw_bridge_apply(b, 2, &rej), OMCI_DECLINED);
	CHECK(omci_hw_reject_has(&rej, 279));
	CHECK_EQ(omci_hw_bridge_apply(b, 2, &rej), OMCI_OK);
	CHECK_EQ(rej.n, 0u);

	omci_hw_vlan_reject_next(99);
	CHECK_EQ(omci_hw_vlan_apply(v, 2, &rej), OMCI_OK);
	CHECK_EQ(rej.n, 0u);
	CHECK_EQ(omci_hw_vlan_apply(v, 2, &rej), OMCI_OK);
	omci_hw_gem_reject_next(99);
	CHECK_EQ(omci_hw_gem_apply(g, 2, &rej), OMCI_OK);
	CHECK_EQ(rej.n, 0u);
	omci_hw_bridge_reject_next(99);
	CHECK_EQ(omci_hw_bridge_apply(b, 2, &rej), OMCI_OK);
	CHECK_EQ(rej.n, 0u);
}

static void test_a_list_of_refusals_keeps_each_id_once_and_stops_when_full(void)
{
	struct omci_hw_reject r = { 0 };

	omci_hw_reject_add(NULL, 5);
	CHECK(!omci_hw_reject_has(NULL, 5));
	CHECK(!omci_hw_reject_has(&r, 5));

	omci_hw_reject_add(&r, 5);
	omci_hw_reject_add(&r, 7);
	omci_hw_reject_add(&r, 5);
	CHECK_EQ(r.n, 2u);
	CHECK(omci_hw_reject_has(&r, 5));
	CHECK(omci_hw_reject_has(&r, 7));
	CHECK(!omci_hw_reject_has(&r, 6));

	for (uint16_t id = 100; id < 100u + 2u * OMCI_HW_REJECT_MAX; id++)
		omci_hw_reject_add(&r, id);
	CHECK_EQ(r.n, OMCI_HW_REJECT_MAX);
	CHECK(omci_hw_reject_has(&r, 5));
}

static void test_the_multicast_bridge_keeps_its_groups(void)
{
	struct omci_hw_mcast_group g = { .vid = 7 }, out[2];
	struct omci_hw_mcast_frame f;
	struct omci_hw_mcast_port port = { .fast_leave = true };
	uint8_t frame[600];
	size_t n = 0;

	omci_hw_mcast_reset();
	CHECK_EQ(omci_hw_mcast_port(NULL), OMCI_ERR_INVAL);
	CHECK_EQ(omci_hw_mcast_port(&port), OMCI_OK);
	port.fast_leave = false;
	CHECK_EQ(omci_hw_mcast_port_recorded(&port), 1u);
	CHECK(port.fast_leave);

	/* Snooped groups come and go; a static one stays. */
	g.group[15] = 1u;
	omci_hw_mcast_snoop(&g, true);
	omci_hw_mcast_snoop(&g, true);
	g.group[15] = 2u;
	g.permanent = true;
	CHECK_EQ(omci_hw_mcast_group_add(&g), OMCI_OK);
	CHECK_EQ(omci_hw_mcast_group_add(&g), OMCI_OK);
	omci_hw_mcast_snoop(&g, false);
	CHECK_EQ(omci_hw_mcast_groups(out, 1, &n), OMCI_OK);
	CHECK_EQ(n, 1u);
	CHECK_EQ(omci_hw_mcast_groups(out, 2, &n), OMCI_OK);
	CHECK_EQ(n, 2u);
	CHECK(!out[0].permanent && out[1].permanent);
	g.group[15] = 1u;
	omci_hw_mcast_snoop(&g, false);
	CHECK_EQ(omci_hw_mcast_groups(out, 2, &n), OMCI_OK);
	CHECK_EQ(n, 1u);
	CHECK_EQ(omci_hw_mcast_group_del(&g), OMCI_DECLINED);

	/* Full: no more groups, snooped or added. */
	for (unsigned int i = 0; i < OMCI_HW_MCAST_MAX_GROUPS; i++) {
		g.group[14] = (uint8_t)i;
		omci_hw_mcast_snoop(&g, true);
	}
	g.group[14] = 0xeeu;
	CHECK_EQ(omci_hw_mcast_group_add(&g), OMCI_ERR_NOMEM);

	/* Failures, each once. */
	omci_hw_mcast_fail_next(true, true, true);
	CHECK_EQ(omci_hw_mcast_groups(out, 2, &n), OMCI_ERR_HW);
	CHECK_EQ(omci_hw_mcast_groups(out, 2, &n), OMCI_OK);
	CHECK_EQ(omci_hw_mcast_group_add(&g), OMCI_ERR_HW);
	CHECK_EQ(omci_hw_mcast_group_del(&g), OMCI_ERR_HW);
	CHECK_EQ(omci_hw_mcast_group_del(&g), OMCI_DECLINED);

	/* Messages: eight wait, in order; a long one is cut. */
	memset(frame, 0x5a, sizeof(frame));
	for (unsigned int i = 0; i < 9u; i++) {
		frame[0] = (uint8_t)i;
		omci_hw_mcast_receive(frame, i == 0u ? sizeof(frame) : 64u,
				      i == 1u, 0x0123u);
	}
	for (unsigned int i = 0; i < 8u; i++) {
		CHECK_EQ(omci_hw_mcast_frame(&f), OMCI_OK);
		CHECK_EQ(f.data[0], i);
		CHECK_EQ(f.len, i == 0u ? OMCI_HW_MCAST_FRAME_MAX : 64u);
		CHECK_EQ(f.tagged, i == 1u);
	}
	CHECK_EQ(omci_hw_mcast_frame(&f), OMCI_DECLINED);

	/* A message upstream longer than a frame is refused. */
	CHECK_EQ(omci_hw_mcast_send(256u, frame, sizeof(frame)),
		 OMCI_ERR_INVAL);
	omci_hw_mcast_reset();
	CHECK_EQ(omci_hw_mcast_groups(out, 2, &n), OMCI_OK);
	CHECK_EQ(n, 0u);
}

int main(void)
{
	test_a_forced_reading_before_any_read_starts_from_the_recorded_stick();
	RUN(test_a_forced_threshold_before_any_read_starts_from_the_defaults);
	RUN(test_the_receiver_level_is_forced);
	RUN(test_the_capabilities_need_somewhere_to_go);
	RUN(test_the_host_device_says_all_of_what_it_is);
	RUN(test_a_set_that_is_refused_leaves_the_last_good_set_recorded);
	RUN(test_a_refusal_that_is_asked_for_is_given_once);
	RUN(test_a_list_of_refusals_keeps_each_id_once_and_stops_when_full);
	RUN(test_the_multicast_bridge_keeps_its_groups);
	return OMCI_T_REPORT("test_hw_host");
}
