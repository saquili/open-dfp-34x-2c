// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the rules of the services (me/svcrule.h): read from a text, and
 * what they make of the VLAN rules that reach the hardware. Two instances of
 * class 171 stand for the Ethernet UNI (association 2) and the VEIP (10);
 * others reach their output through a bridge port (association 0), and an
 * IP host's are of association 3.
 */

#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "hw/hw.h"
#include "me/svcrule.h"
#include "me/vlan.h"
#include "mib/store.h"

static void pack(uint8_t row[16], uint32_t w0, uint32_t w1, uint32_t w2,
		 uint32_t w3)
{
	const uint32_t w[4] = { w0, w1, w2, w3 };

	for (int i = 0; i < 4; i++) {
		row[i * 4 + 0] = (uint8_t)(w[i] >> 24);
		row[i * 4 + 1] = (uint8_t)(w[i] >> 16);
		row[i * 4 + 2] = (uint8_t)(w[i] >> 8);
		row[i * 4 + 3] = (uint8_t)w[i];
	}
}

/* A translation of host VID @host to fibre VID @fibre, of any priority, kept. */
static void xlate(struct omci_mib *mib, uint16_t instance, unsigned int host,
		  unsigned int fibre)
{
	uint8_t row[16];

	pack(row, 0xf8000000u, (8u << 28) | (host << 15), 0x400f0000u,
	     (8u << 16) | (fibre << 3));
	CHECK_EQ(omci_mib_table_put(mib, 171u, instance, 6u, row, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
}

/* Untagged frames tagged with fibre VID @fibre at priority 5. */
static void push(struct omci_mib *mib, uint16_t instance, unsigned int fibre)
{
	uint8_t row[16];

	pack(row, 0xf8000000u, 0xf8000000u, 0x000f0000u,
	     (5u << 16) | (fibre << 3));
	CHECK_EQ(omci_mib_table_put(mib, 171u, instance, 6u, row, 16, 8,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
}

/* An instance of class 171 of association @type, pointing at @ptr. */
static void me171(struct omci_mib *mib, uint16_t instance, uint8_t type,
		  uint16_t ptr)
{
	const uint8_t p[2] = { (uint8_t)(ptr >> 8), (uint8_t)ptr };

	CHECK_EQ(omci_mib_create(mib, 171u, instance), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 171u, instance, 1u, &type, 1u),
		 OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 171u, instance, 7u, p, 2u), OMCI_OK);
}

/* A bridge port of termination point type @tp. */
static void port(struct omci_mib *mib, uint16_t instance, uint8_t tp)
{
	CHECK_EQ(omci_mib_create(mib, 47u, instance), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, 47u, instance, 3u, &tp, 1u), OMCI_OK);
}

/* The host VID of the programmed rule that gives fibre VID @fibre; 0 for none. */
static unsigned int host_of(unsigned int fibre)
{
	const struct omci_hw_vlan_xlate *r = NULL;
	const size_t n = omci_hw_vlan_recorded(&r);

	for (size_t i = 0; i < n; i++)
		if (r[i].kind == OMCI_HW_VLAN_CVID_XLATE &&
		    r[i].assign_cvid == fibre)
			return r[i].match_cvid;
	return 0u;
}

/* How many records of the last programming have fate @f. */
static size_t fates(enum omci_vlan_fate f)
{
	const struct omci_vlan_fate_rec *rec = NULL;
	const size_t n = omci_vlan_fates(&rec, NULL);
	size_t k = 0;

	for (size_t i = 0; i < n; i++)
		k += rec[i].fate == (uint8_t)f;
	return k;
}

static void test_parse(void)
{
	struct omci_svc_rule r[OMCI_SVC_MAX];

	CHECK_EQ(omci_svc_parse("500 move 411\n200 off\n100 only_uni\n"
				"300 only_veip",
				r, OMCI_SVC_MAX),
		 4u);
	CHECK_EQ(r[0].fibre, 500u);
	CHECK_EQ(r[0].action, OMCI_SVC_MOVE);
	CHECK_EQ(r[0].host, 411u);
	CHECK(!r[0].made);
	CHECK_EQ(r[1].action, OMCI_SVC_OFF);
	CHECK_EQ(r[2].action, OMCI_SVC_ONLY_UNI);
	CHECK_EQ(r[3].fibre, 300u);
	CHECK_EQ(r[3].action, OMCI_SVC_ONLY_VEIP);

	/* Lines that are no rule are left out; the rules around them are kept. */
	CHECK_EQ(omci_svc_parse("x off\n" /* no VID */
				"0 off\n" /* VID 0 */
				"4095 off\n" /* past 4094 */
				"99999 off\n" /* far past it */
				"200off\n" /* no space */
				"200 offs\n" /* no such action */
				"200 drop\n" /* nor this */
				"200 move\n" /* a move without its VID */
				"200 move x\n" /* nor a VID */
				"200 off x\n" /* more after it */
				"201 move 202\n" /* a rule */
				"201 off\n" /* a second for its VID */
				"\n"
				"4094 off\n", /* the last VID */
				r, OMCI_SVC_MAX),
		 2u);
	CHECK_EQ(r[0].fibre, 201u);
	CHECK_EQ(r[0].host, 202u);
	CHECK_EQ(r[1].fibre, 4094u);

	/* No more rules than places, and none from an empty text. */
	CHECK_EQ(omci_svc_parse("1 off\n2 off\n3 off\n", r, 2u), 2u);
	CHECK_EQ(omci_svc_parse("", r, OMCI_SVC_MAX), 0u);

	CHECK(omci_svc_find(r, 2u, 2u) == &r[1]);
	CHECK(omci_svc_find(r, 2u, 3u) == NULL);
	CHECK(strcmp(omci_svc_action_name(OMCI_SVC_ONLY_VEIP), "only_veip") ==
	      0);
	CHECK(strcmp(omci_svc_action_name(9u), "?") == 0);
	CHECK(strcmp(omci_vlan_fate_name(OMCI_VLAN_FATE_LOCAL), "local") == 0);
}

static void test_program(void)
{
	struct omci_mib *mib = NULL;
	const struct omci_svc_rule *person = NULL, *made = NULL;
	size_t made_n = 0;
	struct omci_svc_rule rules[4];

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	me171(mib, 0x101u, 2u, 0x101u);
	me171(mib, 0x601u, 10u, 0x601u);
	xlate(mib, 0x101u, 410u, 100u);
	xlate(mib, 0x101u, 420u, 200u);
	xlate(mib, 0x601u, 410u, 500u);

	/*
	 * No person's rule: the VEIP's 410 clashes with the UNI's, and goes to
	 * the host in its own fibre VID, which no rule takes.
	 */
	omci_hw_service_rules_set(rules, 0u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(100u), 410u);
	CHECK_EQ(host_of(200u), 420u);
	CHECK_EQ(host_of(500u), 500u);
	CHECK_EQ(omci_vlan_services(&person, &made, &made_n), 0u);
	CHECK_EQ(made_n, 1u);
	CHECK_EQ(made[0].fibre, 500u);
	CHECK_EQ(made[0].host, 500u);
	CHECK(made[0].made);

	/* A person's move, and the VEIP's service moved by hand too. */
	rules[0] = (struct omci_svc_rule){ .fibre = 200u,
					   .host = 421u,
					   .action = OMCI_SVC_MOVE };
	rules[1] = (struct omci_svc_rule){ .fibre = 500u,
					   .host = 430u,
					   .action = OMCI_SVC_MOVE };
	omci_hw_service_rules_set(rules, 2u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(200u), 421u);
	CHECK_EQ(host_of(500u), 430u);
	CHECK_EQ(omci_vlan_services(&person, &made, &made_n), 2u);
	CHECK_EQ(person[0].host, 421u);
	CHECK_EQ(made_n, 0u);

	/* Off: no rule gives fibre VID 200, and its row is the stick's. */
	rules[0] =
		(struct omci_svc_rule){ .fibre = 200u, .action = OMCI_SVC_OFF };
	omci_hw_service_rules_set(rules, 1u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(200u), 0u);
	CHECK_EQ(fates(OMCI_VLAN_FATE_LOCAL), 1u);

	/* One output alone: of the VEIP's 500 the UNI keeps none, of 100 the VEIP none. */
	rules[0] = (struct omci_svc_rule){ .fibre = 500u,
					   .action = OMCI_SVC_ONLY_UNI };
	rules[1] = (struct omci_svc_rule){ .fibre = 100u,
					   .action = OMCI_SVC_ONLY_VEIP };
	omci_hw_service_rules_set(rules, 2u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(500u), 0u);
	CHECK_EQ(host_of(100u), 0u);
	CHECK_EQ(host_of(200u), 420u);
	CHECK_EQ(fates(OMCI_VLAN_FATE_LOCAL), 2u);

	/* The VEIP keeps its 500 under only_veip, and the UNI its 100 under only_uni. */
	rules[0].action = OMCI_SVC_ONLY_VEIP;
	rules[1].action = OMCI_SVC_ONLY_UNI;
	omci_hw_service_rules_set(rules, 2u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(100u), 410u);
	CHECK_EQ(host_of(500u), 500u);

	omci_hw_service_rules_set(rules, 0u);
	omci_mib_fini(mib);
}

static void test_free_vid(void)
{
	struct omci_mib *mib = NULL;
	const struct omci_svc_rule *person = NULL, *made = NULL;
	size_t made_n = 0;
	struct omci_svc_rule rules[1];

	/*
	 * The fibre VIDs of the clashing services are hosts of other rules:
	 * the first moves to 2, the second to 3, which the first took.
	 */
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	me171(mib, 0x101u, 2u, 0x101u);
	me171(mib, 0x601u, 10u, 0x601u);
	xlate(mib, 0x101u, 410u, 100u);
	xlate(mib, 0x101u, 411u, 101u);
	xlate(mib, 0x101u, 500u, 102u);
	xlate(mib, 0x101u, 501u, 103u);
	xlate(mib, 0x601u, 410u, 500u);
	xlate(mib, 0x601u, 411u, 501u);
	omci_hw_service_rules_set(rules, 0u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(500u), 2u);
	CHECK_EQ(host_of(501u), 3u);
	(void)omci_vlan_services(&person, &made, &made_n);
	CHECK_EQ(made_n, 2u);
	omci_mib_fini(mib);
}

static void test_bridge_ports(void)
{
	struct omci_mib *mib = NULL;
	const struct omci_svc_rule *person = NULL, *made = NULL;
	size_t made_n = 0;
	struct omci_svc_rule rules[1];

	/*
	 * Rules of bridge ports (association 0): of a UNI's port and of the
	 * VEIP's they clash and the later moves; of a mapper's port, or of a
	 * port that is not there, they are left to the programming as they were.
	 */
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	port(mib, 0x201u, 1u);
	port(mib, 0x202u, 11u);
	port(mib, 0x203u, 3u);
	me171(mib, 0x701u, 0u, 0x201u);
	me171(mib, 0x702u, 0u, 0x202u);
	me171(mib, 0x703u, 0u, 0x203u);
	me171(mib, 0x704u, 0u, 0x2ffu);
	xlate(mib, 0x701u, 410u, 100u);
	xlate(mib, 0x702u, 410u, 500u);
	xlate(mib, 0x703u, 420u, 200u);
	xlate(mib, 0x704u, 420u, 600u);
	omci_hw_service_rules_set(rules, 0u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(100u), 410u);
	CHECK_EQ(host_of(500u), 500u);
	CHECK_EQ(host_of(200u), 420u);
	CHECK_EQ(host_of(600u), 0u);
	(void)omci_vlan_services(&person, &made, &made_n);
	CHECK_EQ(made_n, 1u);
	CHECK_EQ(fates(OMCI_VLAN_FATE_SHADOWED), 1u);
	omci_mib_fini(mib);
}

static void test_ip_host(void)
{
	struct omci_mib *mib = NULL;
	const struct omci_svc_rule *person = NULL, *made = NULL;
	size_t made_n = 0;
	struct omci_svc_rule rules[1];
	uint16_t fibre = 0, host = 0;

	/*
	 * The UNI's internet tags untagged frames, and so does an IP host's
	 * voice, which reaches a device behind the stick: the voice goes to the
	 * host in a VLAN of its own, its fibre VID, and the move is the stick's.
	 */
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	me171(mib, 0x101u, 2u, 0x101u);
	me171(mib, 0x901u, 3u, 0x0000u);
	push(mib, 0x101u, 35u);
	push(mib, 0x901u, 600u);
	omci_hw_service_rules_set(rules, 0u);
	CHECK(!omci_vlan_phone(&fibre, &host));
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(600u), 600u);
	CHECK(omci_vlan_phone(&fibre, &host));
	CHECK(fibre == 600u && host == 600u);
	(void)omci_vlan_services(&person, &made, &made_n);
	CHECK(made_n == 1u && made[0].fibre == 600u && made[0].host == 600u &&
	      made[0].made);

	/* Its fibre VID the host VID of another rule: the first free one. */
	xlate(mib, 0x101u, 600u, 101u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(600u), 2u);
	CHECK(omci_vlan_phone(&fibre, &host) && host == 2u);

	/* A person's move, which is no move of the stick's. */
	rules[0] = (struct omci_svc_rule){ .fibre = 600u,
					   .host = 700u,
					   .action = OMCI_SVC_MOVE };
	omci_hw_service_rules_set(rules, 1u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(600u), 700u);
	(void)omci_vlan_services(&person, &made, &made_n);
	CHECK_EQ(made_n, 0u);

	/* The UNI's alone: no voice. */
	rules[0] = (struct omci_svc_rule){ .fibre = 600u,
					   .action = OMCI_SVC_ONLY_UNI };
	omci_hw_service_rules_set(rules, 1u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(600u), 0u);
	CHECK(!omci_vlan_phone(&fibre, &host));

	/* An IP host's bridge port (association 0, type 4) is the same. */
	omci_hw_service_rules_set(rules, 0u);
	CHECK_EQ(omci_mib_delete(mib, 171u, 0x901u), OMCI_OK);
	port(mib, 0x204u, 4u);
	me171(mib, 0x902u, 0u, 0x204u);
	push(mib, 0x902u, 610u);
	CHECK_EQ(omci_vlan_program(mib, 171u, 6u, NULL), OMCI_OK);
	CHECK_EQ(host_of(610u), 610u);
	omci_mib_fini(mib);
}

int main(void)
{
	test_parse();
	test_program();
	test_free_vid();
	test_bridge_ports();
	test_ip_host();
	return OMCI_T_REPORT("test_svcrule");
}
