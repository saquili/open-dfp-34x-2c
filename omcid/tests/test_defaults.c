// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the instances the ONU makes itself, and their values. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/table.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"
#include "model_tsv.h"
#include "omcid/array_size.h"

static size_t read_attr(struct omci_mib *mib, uint16_t cls, uint16_t inst,
			unsigned int attr, uint8_t *out, size_t cap)
{
	size_t n = 0;

	CHECK_EQ(omci_mib_attr_get(mib, cls, inst, (uint8_t)attr, out, cap, &n),
		 OMCI_OK);
	return n;
}

static unsigned int read_u(struct omci_mib *mib, uint16_t cls, uint16_t inst,
			   unsigned int attr)
{
	uint8_t b[8] = { 0 };
	const size_t n = read_attr(mib, cls, inst, attr, b, sizeof(b));
	unsigned int v = 0;

	for (size_t i = 0; i < n && i < 4u; i++)
		v = (v << 8) | b[i];
	return v;
}

static bool text_is(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		    unsigned int attr, const char *want, uint8_t pad)
{
	uint8_t b[64];
	const size_t n = read_attr(mib, cls, inst, attr, b, sizeof(b));
	const size_t len = strlen(want);

	if (len > n || memcmp(b, want, len) != 0)
		return false;
	for (size_t i = len; i < n; i++)
		if (b[i] != pad)
			return false;
	return true;
}

static unsigned int instances_of(struct omci_mib *mib, uint16_t cls)
{
	unsigned int n = 0;
	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(mib, &cur))
		n += cur.class_id == cls ? 1u : 0u;
	return n;
}

static void populate(struct omci_mib **mib, struct omci_hw_caps *caps)
{
	*mib = NULL;
	CHECK_EQ(omci_hw_caps(caps), OMCI_OK);
	CHECK_EQ(omci_mib_init(mib), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(*mib, caps), OMCI_OK);
}

static void test_the_instances_are_the_ones_the_onu_creates(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib;
	unsigned int unis = 0;

	populate(&mib, &caps);
	for (unsigned int i = 0; i < caps.port_count; i++)
		unis += caps.ports[i].pptp_instance != 0u ? 1u : 0u;

	for (unsigned int c = 0; c < tsv_class_count; c++) {
		const struct tsv_class *t = &tsv_classes[c];
		const unsigned int n = instances_of(mib, t->id);

		/*
		 * The ONU makes the tables of a bridge port with the port, and
		 * the classes of phone sockets with them, of which the
		 * synthetic ONU has none.
		 */
		if (t->by_olt || t->id == 49u || t->id == 50u || t->id == 79u ||
		    t->id == 53u || t->id == 138u || t->id == 141u)
			CHECK_EQ(n, 0u);
		else
			CHECK(n > 0u);
	}

	/* A cardholder and a circuit pack for each port, and for the VEIP. */
	CHECK_EQ(instances_of(mib, 2u), 1u);
	CHECK_EQ(instances_of(mib, 5u), (unsigned int)caps.port_count + 1u);
	CHECK_EQ(instances_of(mib, 6u), (unsigned int)caps.port_count + 1u);
	CHECK_EQ(instances_of(mib, 7u), 2u);
	CHECK_EQ(instances_of(mib, 11u), unis);
	CHECK_EQ(instances_of(mib, 131u), 1u);
	CHECK_EQ(instances_of(mib, 134u), (unsigned int)caps.ip_host_count);
	CHECK_EQ(instances_of(mib, 256u), 1u);
	CHECK_EQ(instances_of(mib, 257u), 1u);
	CHECK_EQ(instances_of(mib, 262u), (unsigned int)caps.num_tcont);
	CHECK_EQ(instances_of(mib, 263u), 1u);
	CHECK_EQ(instances_of(mib, 264u), unis);
	CHECK_EQ(instances_of(mib, 277u),
		 (unsigned int)(caps.num_tcont * caps.num_priq_per_tcont +
				unis * caps.num_priq_per_port));
	CHECK_EQ(instances_of(mib, 278u), (unsigned int)caps.num_tcont);
	CHECK_EQ(instances_of(mib, 287u), 1u);
	printf("  %zu instances in the default MIB\n", omci_mib_count(mib));

	omci_mib_fini(mib);
}

/*
 * The subscriber ports told to the OLT, as an ONT stood in for had them:
 * four Ethernet UNIs of one slot and the VEIP; the UNIs alone; the VEIP
 * alone, without the UNIs' slot and queues.
 */
static void test_the_ports_told_are_those_chosen(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;
	const struct omci_hw_port pon = { .pptp_instance = 0,
					  .cardholder = 0x0180,
					  .plugin_type = 0xf8 };

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	for (unsigned int i = 0; i < 4u; i++)
		caps.ports[i] =
			(struct omci_hw_port){ .pptp_instance =
						       (uint16_t)(0x0101u + i),
					       .cardholder = 0x0101,
					       .plugin_type = 0x2f };
	caps.ports[4] = pon;
	caps.port_count = 5;
	caps.uni_port_count = 4;
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK_EQ(instances_of(mib, 11u), 4u);
	CHECK_EQ(instances_of(mib, 264u), 4u);
	CHECK(omci_mib_exists(mib, 11u, 0x0104u));
	/* One slot of the four UNIs, the PON's and the VEIP's. */
	CHECK_EQ(instances_of(mib, 5u), 3u);
	CHECK_EQ(read_u(mib, 6u, 0x0101u, 2u), 4u);
	CHECK_EQ(instances_of(mib, 329u), 1u);
	CHECK_EQ(instances_of(mib, 277u),
		 (unsigned int)(caps.num_tcont * caps.num_priq_per_tcont +
				4u * caps.num_priq_per_port));

	caps.uni_report = OMCI_HW_UNI_PPTP;
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK_EQ(instances_of(mib, 11u), 4u);
	CHECK_EQ(instances_of(mib, 329u), 0u);
	CHECK_EQ(instances_of(mib, 5u), 2u);

	caps.uni_report = OMCI_HW_UNI_VEIP;
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK_EQ(instances_of(mib, 11u), 0u);
	CHECK_EQ(instances_of(mib, 264u), 0u);
	CHECK_EQ(instances_of(mib, 329u), 1u);
	CHECK(!omci_mib_exists(mib, 5u, 0x0101u));
	CHECK_EQ(instances_of(mib, 277u),
		 (unsigned int)(caps.num_tcont * caps.num_priq_per_tcont));
	omci_mib_fini(mib);
}

/*
 * Two phone sockets of their own slot: a POTS UNI, a UNI-G and a VoIP line
 * status each, of one number, a circuit pack of type POTS with two ports
 * and no queues, and VoIP config data that offers SIP set up through OMCI;
 * told with the VEIP alone too, and no VoIP config data without them.
 */
static void test_the_phone_sockets_told_are_those_chosen(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;
	uint8_t methods[4];

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	for (unsigned int i = 0; i < 2u; i++)
		caps.ports[caps.port_count++] = (struct omci_hw_port){
			.pptp_instance = (uint16_t)(0x0201u + i),
			.cardholder = 0x0102,
			.plugin_type = OMCI_HW_PLUGIN_POTS
		};
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK_EQ(instances_of(mib, 53u), 2u);
	CHECK(omci_mib_exists(mib, 53u, 0x0202u));
	CHECK(omci_mib_exists(mib, 141u, 0x0202u));
	CHECK(omci_mib_exists(mib, 264u, 0x0201u));
	CHECK_EQ(instances_of(mib, 11u), 1u);
	CHECK_EQ(read_u(mib, 5u, 0x0102u, 1u), 32u);
	CHECK_EQ(read_u(mib, 6u, 0x0102u, 1u), 32u);
	CHECK_EQ(read_u(mib, 6u, 0x0102u, 2u), 2u);
	CHECK_EQ(read_u(mib, 6u, 0x0102u, 12u), 0u);
	CHECK_EQ(instances_of(mib, 277u),
		 (unsigned int)(caps.num_tcont * caps.num_priq_per_tcont +
				caps.num_priq_per_port));
	CHECK_EQ(read_u(mib, 138u, 0u, 1u), 1u);
	CHECK_EQ(read_u(mib, 138u, 0u, 2u), 0u);
	CHECK_EQ(read_attr(mib, 138u, 0u, 3u, methods, sizeof(methods)), 4u);
	CHECK_EQ(methods[3], 1u);
	CHECK_EQ(read_u(mib, 138u, 0u, 5u), 0xffffu);

	caps.uni_report = OMCI_HW_UNI_VEIP;
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK_EQ(instances_of(mib, 11u), 0u);
	CHECK_EQ(instances_of(mib, 53u), 2u);
	CHECK_EQ(instances_of(mib, 264u), 2u);

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK_EQ(instances_of(mib, 138u), 0u);
	omci_mib_fini(mib);
}

static void test_the_identifiers_follow_the_rules_of_the_standard(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib;

	populate(&mib, &caps);

	for (unsigned int i = 0; i < 5; i++) {
		static const uint16_t single[] = { 2, 131, 256, 257, 287 };

		CHECK(omci_mib_exists(mib, single[i], 0u));
	}

	for (unsigned int i = 0; i < caps.port_count; i++) {
		const uint16_t id = caps.ports[i].cardholder;

		CHECK_EQ(id >> 8, 1u);
		CHECK(id & 0xffu);
		CHECK(omci_mib_exists(mib, 5u, id));
		CHECK(omci_mib_exists(mib, 6u, id));
		if (caps.ports[i].pptp_instance != 0u) {
			CHECK(omci_mib_exists(mib, 11u,
					      caps.ports[i].pptp_instance));
			CHECK(omci_mib_exists(mib, 264u,
					      caps.ports[i].pptp_instance));
		}
	}

	CHECK(omci_mib_exists(mib, 7u, 0x0000u));
	CHECK(omci_mib_exists(mib, 7u, 0x0001u));

	for (unsigned int n = 0; n < caps.num_tcont; n++) {
		CHECK(omci_mib_exists(mib, 262u, (uint16_t)(0x8000u | n)));
		CHECK(omci_mib_exists(mib, 278u, (uint16_t)(0x8000u | n)));
	}
	CHECK(!omci_mib_exists(mib, 262u,
			       (uint16_t)(0x8000u | caps.num_tcont)));
	CHECK(omci_mib_exists(mib, 263u, caps.ani_instance));

	unsigned int up = 0, down = 0;
	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(mib, &cur))
		if (cur.class_id == 277u) {
			if ((cur.instance & 0x8000u) != 0u)
				up++;
			else
				down++;
		}
	CHECK_EQ(up, (unsigned int)caps.num_tcont * caps.num_priq_per_tcont);
	CHECK(down > 0u);

	omci_mib_fini(mib);
}

static void test_the_values_the_sheet_states(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib;
	uint8_t b[64];

	populate(&mib, &caps);
	const uint16_t uni = caps.ports[0].cardholder;
	const uint16_t pon = caps.ports[1].cardholder;

	CHECK_EQ(read_u(mib, 2u, 0u, 1u), 0u);

	CHECK_EQ(read_u(mib, 5u, uni, 2u), 0u);
	CHECK_EQ(read_u(mib, 5u, uni, 3u), 0u);
	CHECK(text_is(mib, 5u, uni, 4u, "", ' '));
	CHECK_EQ(read_u(mib, 5u, uni, 1u), caps.ports[0].plugin_type);
	CHECK_EQ(read_u(mib, 6u, uni, 1u), read_u(mib, 5u, uni, 1u));

	CHECK_EQ(read_attr(mib, 6u, uni, 3u, b, sizeof(b)), 8u);
	CHECK_MEM(b, caps.identity.serial, 8);
	CHECK(text_is(mib, 6u, uni, 4u, caps.identity.version, 0));
	CHECK(text_is(mib, 6u, uni, 5u, caps.identity.vendor_id, 0));
	CHECK(text_is(mib, 6u, uni, 9u, "", ' '));

	CHECK_EQ(read_u(mib, 11u, caps.ports[0].pptp_instance, 8u), 1518u);
	CHECK_EQ(read_u(mib, 11u, caps.ports[0].pptp_instance, 11u), 2u);
	CHECK_EQ(read_u(mib, 11u, caps.ports[0].pptp_instance, 2u),
		 read_u(mib, 11u, caps.ports[0].pptp_instance, 1u) ?
			 read_u(mib, 11u, caps.ports[0].pptp_instance, 1u) :
			 caps.ports[0].plugin_type);

	CHECK(text_is(mib, 131u, 0u, 1u, "", ' '));
	CHECK(text_is(mib, 131u, 0u, 2u, "", ' '));
	CHECK(text_is(mib, 131u, 0u, 3u, "0", 0));

	CHECK(text_is(mib, 256u, 0u, 1u, caps.identity.vendor_id, 0));
	CHECK_EQ(read_attr(mib, 256u, 0u, 3u, b, sizeof(b)), 8u);
	CHECK_MEM(b, caps.identity.serial, 8);
	CHECK_MEM(b, caps.identity.vendor_id, 4);
	CHECK_EQ(read_u(mib, 256u, 0u, 4u), caps.identity.tm_option);
	CHECK(text_is(mib, 256u, 0u, 10u, caps.identity.loid, 0));
	CHECK(caps.identity.loid[0] != '\0');
	CHECK(text_is(mib, 256u, 0u, 11u, "", 0));
	CHECK_EQ(read_u(mib, 256u, 0u, 5u), 0u);
	CHECK_EQ(read_u(mib, 256u, 0u, 1u) == 0u, false);

	CHECK_EQ(read_u(mib, 257u, 0u, 2u), caps.identity.omcc_version);
	CHECK_EQ(read_u(mib, 257u, 0u, 5u), 1u);
	CHECK_EQ(read_u(mib, 257u, 0u, 8u), 1u);
	CHECK_EQ(read_u(mib, 257u, 0u, 14u), 1u);
	CHECK_EQ(read_u(mib, 257u, 0u, 7u), caps.num_tcont);
	CHECK_EQ(read_u(mib, 257u, 0u, 6u),
		 (unsigned int)caps.num_tcont * caps.num_priq_per_tcont);

	for (unsigned int n = 0; n < caps.num_tcont; n++) {
		const uint16_t id = (uint16_t)(0x8000u | n);

		CHECK_EQ(read_u(mib, 262u, id, 1u), 0x00ffu);
		CHECK_EQ(read_u(mib, 262u, id, 2u), 1u);
		CHECK(read_u(mib, 262u, id, 3u) <= 2u);
		CHECK_EQ(read_u(mib, 278u, id, 1u), id);
		CHECK_EQ(read_u(mib, 278u, id, 2u), 0u);
		CHECK_EQ(read_u(mib, 278u, id, 4u), 0u);
	}

	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 2u), caps.num_tcont);
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 3u), 48u);
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 5u), 0u);
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 6u), 5u);
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 7u), 9u);
	CHECK(read_u(mib, 263u, caps.ani_instance, 7u) >
	      read_u(mib, 263u, caps.ani_instance, 6u));
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 11u), 0xffu);
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 12u), 0xffu);
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 15u), 0x81u);
	CHECK_EQ(read_u(mib, 263u, caps.ani_instance, 16u), 0x81u);

	CHECK_EQ(read_u(mib, 6u, pon, 11u), caps.num_tcont);
	CHECK_EQ(read_u(mib, 6u, pon, 12u),
		 (unsigned int)caps.num_tcont * caps.num_priq_per_tcont);
	CHECK_EQ(read_u(mib, 6u, pon, 13u), caps.num_tcont);
	CHECK_EQ(read_u(mib, 6u, uni, 11u), 0u);
	CHECK_EQ(read_u(mib, 6u, uni, 12u), caps.num_priq_per_port);

	CHECK_EQ(read_u(mib, 277u, 0x8000u, 8u), 1u);
	CHECK_EQ(read_u(mib, 277u, 0x8000u, 10u), 0u);
	CHECK_EQ(read_attr(mib, 277u, 0x8000u, 13u, b, sizeof(b)), 8u);
	for (unsigned int i = 0; i < 4u; i++)
		CHECK_EQ((unsigned int)((b[2u * i] << 8) | b[2u * i + 1u]),
			 read_u(mib, 277u, 0x8000u, 2u));
	CHECK_EQ(read_u(mib, 277u, 0x8000u, 14u), 0xffffu);
	CHECK_EQ(read_u(mib, 277u, 0x8000u, 15u), 9u);

	CHECK_EQ(read_u(mib, 7u, 0u, 2u) + read_u(mib, 7u, 1u, 2u), 1u);
	CHECK_EQ(read_u(mib, 7u, 0u, 3u) + read_u(mib, 7u, 1u, 3u), 1u);
	CHECK_EQ(read_u(mib, 7u, caps.sw_committed_image, 2u), 1u);
	CHECK(text_is(mib, 7u, 0u, 1u, caps.identity.sw_version[0], 0));
	CHECK(text_is(mib, 7u, 1u, 1u, caps.identity.sw_version[1], 0));

	CHECK_EQ(read_attr(mib, 134u, 0u, 2u, b, sizeof(b)), 6u);
	CHECK_MEM(b, caps.identity.mac, 6);
	for (unsigned int i = 0; i < 5u; i++)
		CHECK_EQ(read_u(mib, 134u, 0u, 4u + i),
			 read_u(mib, 134u, 0u, 9u + i));

	omci_mib_fini(mib);
}

static void test_the_identity_the_seam_gives_is_the_identity_the_mib_holds(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;
	uint8_t b[64];

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	snprintf(caps.identity.vendor_id, sizeof(caps.identity.vendor_id), "%s",
		 "SEAM");
	snprintf(caps.identity.version, sizeof(caps.identity.version), "%s",
		 "seam-version");
	snprintf(caps.identity.equipment_id, sizeof(caps.identity.equipment_id),
		 "%s", "seam-equipment");
	snprintf(caps.identity.sw_version[0],
		 sizeof(caps.identity.sw_version[0]), "%s", "seam-sw-0");
	snprintf(caps.identity.sw_version[1],
		 sizeof(caps.identity.sw_version[1]), "%s", "seam-sw-1");
	for (unsigned int i = 0; i < 8u; i++)
		caps.identity.serial[i] = (uint8_t)(0x90u + i);
	caps.identity.tm_option = 1u;
	caps.identity.omcc_version = 0xb3u;
	caps.identity.vendor_product_code = 4242u;
	snprintf(caps.identity.pack_version, sizeof(caps.identity.pack_version),
		 "%s", "seam-pack");
	caps.sw_committed_image = 1u;
	caps.sw_valid_images = 0u;
	caps.num_tcont = 3u;
	caps.num_priq_per_tcont = 5u;
	caps.num_priq_per_port = 3u;
	caps.ip_host_count = 2u;
	caps.ipv6_host_count = 2u;
	caps.uni_port_count = 4u;
	caps.num_gem_ports = 99u;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);

	CHECK(text_is(mib, 256u, 0u, 1u, "SEAM", 0));
	CHECK(text_is(mib, 256u, 0u, 2u, "seam-version", 0));
	CHECK_EQ(read_attr(mib, 256u, 0u, 3u, b, sizeof(b)), 8u);
	CHECK_MEM(b, caps.identity.serial, 8);
	CHECK_EQ(read_u(mib, 256u, 0u, 4u), 1u);
	CHECK(text_is(mib, 257u, 0u, 1u, "seam-equipment", 0));
	CHECK_EQ(read_u(mib, 257u, 0u, 2u), 0xb3u);
	CHECK_EQ(read_u(mib, 257u, 0u, 3u), 4242u);
	CHECK_EQ(read_u(mib, 257u, 0u, 9u), 99u);
	CHECK_EQ(read_u(mib, 257u, 0u, 6u), 15u);
	CHECK_EQ(read_u(mib, 257u, 0u, 7u), 3u);
	CHECK(text_is(mib, 7u, 0u, 1u, "seam-sw-0", 0));
	CHECK(text_is(mib, 7u, 1u, 1u, "seam-sw-1", 0));
	CHECK_EQ(read_u(mib, 7u, 1u, 2u), 1u);
	CHECK_EQ(read_u(mib, 7u, 0u, 2u), 0u);
	CHECK_EQ(read_u(mib, 7u, 1u, 4u), 1u);
	CHECK_EQ(read_u(mib, 7u, 0u, 4u), 0u);
	CHECK(text_is(mib, 6u, caps.ports[0].cardholder, 4u, "seam-pack", 0));
	CHECK(text_is(mib, 6u, caps.ports[1].cardholder, 4u, "seam-pack", 0));
	CHECK_EQ(instances_of(mib, 262u), 3u);
	CHECK_EQ(instances_of(mib, 278u), 3u);
	CHECK_EQ(instances_of(mib, 134u), 2u);
	CHECK_EQ(instances_of(mib, 277u), 3u * 5u + 3u);

	/*
	 * The IPv6 hosts follow the IP hosts in numbering, locked, with the MAC
	 * address and a link-layer DUID (RFC 8415 11.2) of it.
	 */
	CHECK_EQ(instances_of(mib, 347u), 2u);
	for (unsigned int id = 2u; id <= 3u; id++) {
		uint8_t duid[25] = { 0x00, 0x03, 0x00, 0x01 };

		CHECK(omci_mib_exists(mib, 347u, (uint16_t)id));
		CHECK_EQ(read_u(mib, 347u, (uint16_t)id, 1u), 0u);
		CHECK_EQ(read_attr(mib, 347u, (uint16_t)id, 2u, b, sizeof(b)),
			 6u);
		CHECK_MEM(b, caps.identity.mac, 6);
		memcpy(&duid[4], caps.identity.mac, 6);
		CHECK_EQ(read_attr(mib, 347u, (uint16_t)id, 12u, b, sizeof(b)),
			 25u);
		CHECK_MEM(b, duid, sizeof(duid));
		CHECK_EQ(read_attr(mib, 347u, (uint16_t)id, 5u, b, sizeof(b)),
			 16u);
		for (unsigned int i = 0; i < 16u; i++)
			CHECK_EQ(b[i], 0u);
	}
	CHECK(!omci_mib_exists(mib, 347u, 4u));
	CHECK_EQ(read_u(mib, 6u, caps.ports[0].cardholder, 2u), 4u);

	caps.port_count = (uint8_t)(ARRAY_SIZE(caps.ports) + 1u);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_ERR_INVAL);
	CHECK_EQ(omci_me_defaults_apply(NULL, &caps), OMCI_ERR_INVAL);
	CHECK_EQ(omci_me_defaults_apply(mib, NULL), OMCI_ERR_INVAL);

	omci_mib_fini(mib);
}

static void test_an_image_the_backend_calls_valid_is_valid_but_not_running(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(caps.sw_valid_images, 1u << caps.sw_committed_image);
	caps.sw_valid_images = 3u;
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);

	for (unsigned int n = 0; n < 2u; n++) {
		const unsigned int running = n == caps.sw_committed_image;

		CHECK_EQ(read_u(mib, 7u, n, 2u), running);
		CHECK_EQ(read_u(mib, 7u, n, 3u), running);
		CHECK_EQ(read_u(mib, 7u, n, 4u), 1u);
	}
	omci_mib_fini(mib);
}

static void test_the_population_is_repeatable(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib;
	uint8_t first[64], second[64];

	populate(&mib, &caps);
	const size_t count = omci_mib_count(mib);
	const size_t n1 = read_attr(mib, 256u, 0u, 3u, first, sizeof(first));

	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK_EQ(omci_mib_count(mib), count);
	const size_t n2 = read_attr(mib, 256u, 0u, 3u, second, sizeof(second));

	CHECK_EQ(n1, n2);
	CHECK_MEM(first, second, n1);

	CHECK_EQ(omci_mib_create(mib, 45u, 0x0abcu), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	CHECK(!omci_mib_exists(mib, 45u, 0x0abcu));
	CHECK_EQ(omci_mib_count(mib), count);

	omci_mib_fini(mib);
}

struct seen {
	unsigned int n;
	struct {
		uint16_t cls, inst;
		uint8_t attr;
	} what[512];
};

static void on_change(void *ctx, uint16_t cls, uint16_t inst, uint8_t attr,
		      const void *value, size_t len)
{
	struct seen *s = ctx;

	(void)value;
	(void)len;
	if (s->n < ARRAY_SIZE(s->what)) {
		s->what[s->n].cls = cls;
		s->what[s->n].inst = inst;
		s->what[s->n].attr = attr;
	}
	s->n++;
}

static const struct omci_mib_notify_ops sink = { .attr_changed = on_change };

static void test_an_instance_is_announced_once_and_whole(void)
{
	static struct seen s;
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	omci_mib_notify_set(mib, &sink, &s);

	memset(&s, 0, sizeof(s));
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);

	unsigned int want = 0;
	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(mib, &cur)) {
		const struct tsv_class *t = tsv_class(cur.class_id);

		for (unsigned int a = 1; t != NULL && a <= t->attr_count; a++)
			want += t->attrs[a - 1].avc ? 1u : 0u;
	}
	printf("  %u announcements for %zu instances\n", s.n,
	       omci_mib_count(mib));
	CHECK_EQ(s.n, want);
	CHECK(want > 0u);

	for (unsigned int i = 1; i < s.n && i < 512u; i++) {
		const bool same = s.what[i].cls == s.what[i - 1].cls &&
				  s.what[i].inst == s.what[i - 1].inst;

		if (same)
			CHECK(s.what[i].attr > s.what[i - 1].attr);
	}
	for (unsigned int i = 0; i < s.n && i < 512u; i++) {
		const struct tsv_class *t = tsv_class(s.what[i].cls);

		CHECK(t != NULL && s.what[i].attr >= 1u &&
		      s.what[i].attr <= t->attr_count &&
		      t->attrs[s.what[i].attr - 1u].avc);
	}

	memset(&s, 0, sizeof(s));
	omci_mib_notify_mute(mib, true);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	omci_mib_notify_mute(mib, false);
	CHECK_EQ(s.n, 0u);

	omci_mib_notify_set(mib, NULL, NULL);
	omci_mib_fini(mib);
}

static void test_a_mib_reset_rebuilds_the_same_mib(void)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);
	const size_t count = omci_mib_count(fx.mib);

	CHECK_EQ(omci_mib_create(fx.mib, 84u, 0x0abcu), OMCI_OK);
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, 0x0100u, 15u, true, false, 2u, 0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK_EQ(omci_mib_count(fx.mib), count);
	CHECK(!omci_mib_exists(fx.mib, 84u, 0x0abcu));

	omci_t_down(&fx);
}

int main(int argc, char **argv)
{
	if (!tsv_open(argc, argv)) {
		printf("test_defaults: no table to read\n");
		return 1;
	}

	RUN(test_the_instances_are_the_ones_the_onu_creates);
	RUN(test_the_ports_told_are_those_chosen);
	RUN(test_the_phone_sockets_told_are_those_chosen);
	RUN(test_the_identifiers_follow_the_rules_of_the_standard);
	RUN(test_the_values_the_sheet_states);
	RUN(test_the_identity_the_seam_gives_is_the_identity_the_mib_holds);
	RUN(test_an_image_the_backend_calls_valid_is_valid_but_not_running);
	RUN(test_the_population_is_repeatable);
	RUN(test_an_instance_is_announced_once_and_whole);
	RUN(test_a_mib_reset_rebuilds_the_same_mib);

	return OMCI_T_REPORT("test_defaults");
}
