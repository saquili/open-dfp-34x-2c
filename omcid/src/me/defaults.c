// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The instances the ONU makes itself: the ONU and its equipment, the ANI,
 * the T-CONTs with their schedulers and queues, the software images, the IP
 * hosts and the OMCI entity, each with the values it has at start.
 */

#include <assert.h>
#include <stdbool.h>

#include "me/defaults.h"
#include "me/table.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "sys/log.h"
#include "sys/wire.h"

enum {
	ME_ONU_DATA = 2,
	ME_CARDHOLDER = 5,
	ME_CIRCUIT_PACK = 6,
	ME_SOFTWARE_IMAGE = 7,
	ME_PPTP_ETH_UNI = 11,
	ME_PPTP_POTS_UNI = 53,
	ME_PPTP_LCT_UNI = 83,
	ME_OLT_G = 131,
	ME_IP_HOST = 134,
	ME_VOIP_CONFIG = 138,
	ME_VOIP_LINE_STATUS = 141,
	ME_ONU_G = 256,
	ME_ONU2_G = 257,
	ME_TCONT = 262,
	ME_ANI_G = 263,
	ME_UNI_G = 264,
	ME_PRIORITY_QUEUE = 277,
	ME_TRAFFIC_SCHED = 278,
	ME_OMCI = 287,
	ME_VEIP = 329,
	ME_IPV6_HOST = 347,
};

/* The slot of the PON side: T-CONTs, schedulers and upstream queues are 0x80nn. */
#define PON_SLOT 0x80u

/*
 * The VEIP, the hand-off to the router of the stick (G.988 9.5.5): port 1 of
 * slot 6, whose integrated cardholder holds a plug-in unit of type 48, VEIP.
 */
#define VEIP_SLOT 6u
#define VEIP_PLUGIN_TYPE 48u
#define VEIP_INSTANCE ((uint16_t)(VEIP_SLOT << 8 | 1u))
#define VEIP_CARDHOLDER ((uint16_t)(0x0100u | VEIP_SLOT))

/*
 * No Alloc-ID yet: 0x00FF, the first of the two values that G.988 9.2.2 gives
 * the T-CONT of a G-PON ONU before the OLT maps it. 0xFFFF, the other, is the
 * only one of the later PONs, and an OLT of G-PON need not take it as free.
 * And the size given to every queue.
 */
#define ALLOC_ID_UNASSIGNED 0x00ffu
#define QUEUE_SIZE 0x0400u

/* An instance being built; the first failure stops the rest. */
struct build {
	struct omci_mib *mib;
	uint16_t class_id;
	uint16_t instance;
	int32_t rc;
};

static_assert(sizeof(struct build) == sizeof(void *) + 8,
	      "the status keeps the width its fixed type gave it");

/*
 * Start an instance. Its attribute changes are held back until finish(),
 * which reports each announced attribute once, with its final value.
 */
static void begin(struct build *b, uint16_t class_id, uint16_t instance)
{
	if (b->rc != OMCI_OK)
		return;

	b->class_id = class_id;
	b->instance = instance;

	omci_mib_notify_suspend(b->mib, true);
	b->rc = omci_mib_create(b->mib, class_id, instance);
}

static void finish(struct build *b)
{
	omci_mib_notify_suspend(b->mib, false);
	if (b->rc == OMCI_OK)
		b->rc = omci_mib_announce_all(b->mib, b->class_id, b->instance);
	if (b->rc != OMCI_OK)
		OMCI_ERR("defaults: %u/%#06x: %s", b->class_id, b->instance,
			 omci_status_str(b->rc));
}

static void put(struct build *b, uint8_t attr, const void *value, size_t len)
{
	if (b->rc == OMCI_OK)
		b->rc = omci_mib_attr_set(b->mib, b->class_id, b->instance,
					  attr, value, len);
}

static void put_u8(struct build *b, uint8_t attr, uint8_t value)
{
	put(b, attr, &value, sizeof(value));
}

static void put_u16(struct build *b, uint8_t attr, uint16_t value)
{
	uint8_t wire[2];

	omci_be16_store(wire, value);
	put(b, attr, wire, sizeof(wire));
}

/* A string attribute, padded to its size with @pad. */
static void put_text(struct build *b, uint8_t attr, const char *text,
		     uint8_t pad)
{
	uint8_t wire[OMCI_MIB_TABLE_ENTRY_MAX];
	const struct omci_me_attr *a =
		omci_me_attr_get(omci_me_class_by_id(b->class_id), attr);

	if (b->rc != OMCI_OK)
		return;

	omci_wire_fill(wire, pad, a->size);
	for (size_t i = 0; i < a->size && text[i] != '\0'; i++)
		wire[i] = (uint8_t)text[i];
	put(b, attr, wire, a->size);
}

static void put_blank(struct build *b, uint8_t attr)
{
	put_text(b, attr, "", ' ');
}

static void make_onu_data(struct build *b)
{
	begin(b, ME_ONU_DATA, 0);

	finish(b);
}

/*
 * ONU-G: vendor ID, version, serial number, traffic management option, and
 * the logical ONU ID and password (attributes 10 and 11), empty unless the
 * unit has them.
 */
static void make_onu_g(struct build *b, const struct omci_hw_caps *caps)
{
	begin(b, ME_ONU_G, 0);
	put_text(b, 1, caps->identity.vendor_id, 0);
	put_text(b, 2, caps->identity.version, 0);
	put(b, 3, caps->identity.serial, sizeof(caps->identity.serial));
	put_u8(b, 4, caps->identity.tm_option);
	put_text(b, 10, caps->identity.loid, 0);
	put_text(b, 11, caps->identity.loid_password, 0);

	finish(b);
}

/*
 * ONU2-G: equipment ID, OMCC version, vendor product code; security
 * capability and mode 1 (AES); the totals of priority queues, traffic
 * schedulers and GEM port identifiers; a deprecated attribute held at 1;
 * and a priority queue scale factor of 1 (attribute 14).
 */
static void make_onu2_g(struct build *b, const struct omci_hw_caps *caps)
{
	const unsigned int queues =
		(unsigned int)caps->num_tcont * caps->num_priq_per_tcont;

	begin(b, ME_ONU2_G, 0);
	put_text(b, 1, caps->identity.equipment_id, 0);
	put_u8(b, 2, caps->identity.omcc_version);
	put_u16(b, 3, caps->identity.vendor_product_code);
	put_u8(b, 4, 1);
	put_u8(b, 5, 1);
	put_u16(b, 6, (uint16_t)queues);
	put_u8(b, 7, caps->num_tcont);
	put_u8(b, 8, 1);
	put_u16(b, 9, caps->num_gem_ports);

	put_u16(b, 14, 1);
	finish(b);
}

/* OLT-G: its vendor ID and equipment ID blank until the OLT sets them. */
static void make_olt_g(struct build *b)
{
	begin(b, ME_OLT_G, 0);
	put_blank(b, 1);
	put_blank(b, 2);
	put_text(b, 3, "0", 0);

	finish(b);
}

static void make_omci(struct build *b)
{
	begin(b, ME_OMCI, 0);

	finish(b);
}

/* A cardholder: the plug-in unit type, the equipment identifiers blank. */
static void make_cardholder(struct build *b, const struct omci_hw_port *p)
{
	begin(b, ME_CARDHOLDER, p->cardholder);
	put_u8(b, 1, p->plugin_type);

	put_blank(b, 4);
	put_blank(b, 5);

	finish(b);
}

/*
 * A circuit pack: type, number of ports, serial number, version, vendor ID
 * and a blank equipment ID; on the PON side, the numbers of T-CONT buffers,
 * priority queues and traffic schedulers.
 */
/* The version a circuit pack reports: its own when the backend names one. */
static const char *pack_version(const struct omci_hw_caps *caps)
{
	return caps->identity.pack_version[0] ? caps->identity.pack_version :
						caps->identity.version;
}

/* The phone sockets of a slot. */
static uint8_t ports_of(const struct omci_hw_caps *caps, uint16_t cardholder)
{
	uint8_t n = 0;

	for (unsigned int i = 0; i < caps->port_count; i++)
		n += caps->ports[i].cardholder == cardholder ? 1u : 0u;
	return n;
}

static void make_circuit_pack(struct build *b, const struct omci_hw_caps *caps,
			      const struct omci_hw_port *p)
{
	const bool pon = p->pptp_instance == 0;
	const bool pots = p->plugin_type == OMCI_HW_PLUGIN_POTS;

	begin(b, ME_CIRCUIT_PACK, p->cardholder);
	put_u8(b, 1, p->plugin_type);
	put_u8(b, 2,
	       pon  ? 1u :
	       pots ? ports_of(caps, p->cardholder) :
		      caps->uni_port_count);
	put(b, 3, caps->identity.serial, sizeof(caps->identity.serial));
	put_text(b, 4, pack_version(caps), 0);
	put_text(b, 5, caps->identity.vendor_id, 0);

	put_blank(b, 9);
	put_u8(b, 11, pon ? caps->num_tcont : 0u);
	put_u8(b, 12,
	       pon  ? (uint8_t)(caps->num_tcont * caps->num_priq_per_tcont) :
	       pots ? 0u :
		      caps->num_priq_per_port);
	put_u8(b, 13, pon ? caps->num_tcont : 0u);
	finish(b);
}

/*
 * The Ethernet UNI: its sensed type, a largest frame of 1518 bytes, and
 * "bridged or IP" set to 2, which leaves the choice to its circuit pack.
 */
static void make_pptp(struct build *b, const struct omci_hw_port *p)
{
	begin(b, ME_PPTP_ETH_UNI, p->pptp_instance);

	put_u8(b, 2, p->plugin_type);

	put_u16(b, 8, 1518);

	put_u8(b, 11, 2);

	finish(b);
}

static void make_uni_g(struct build *b, const struct omci_hw_port *p)
{
	begin(b, ME_UNI_G, p->pptp_instance);

	finish(b);
}

/*
 * A phone socket: its POTS UNI, unlocked, of 600 ohm, full-time on-hook
 * transmission, no gain, enabled and on hook, with every value G.988 9.9.1
 * gives at its instantiation; and its VoIP line status, of no session yet
 * (9.9.11), which nothing on the stick changes, as no SIP agent runs here.
 */
static void make_pots(struct build *b, const struct omci_hw_port *p)
{
	begin(b, ME_PPTP_POTS_UNI, p->pptp_instance);
	finish(b);
	make_uni_g(b, p);
	begin(b, ME_VOIP_LINE_STATUS, p->pptp_instance);
	finish(b);
}

/*
 * The equipment of port @i: its cardholder and circuit pack, which the
 * ports of one slot share, the first of them making them; a subscriber port
 * also gets its UNI. A device that tells the OLT of its VEIP alone tells it
 * of no Ethernet UNI, nor of their slot; its phone sockets it tells all the
 * same.
 */
static void make_slot(struct build *b, const struct omci_hw_caps *caps,
		      unsigned int i)
{
	const struct omci_hw_port *p = &caps->ports[i];
	const bool pots = p->plugin_type == OMCI_HW_PLUGIN_POTS;
	bool made = false;

	if (p->pptp_instance != 0 && !pots &&
	    caps->uni_report == OMCI_HW_UNI_VEIP)
		return;
	for (unsigned int k = 0; k < i; k++)
		made = made || caps->ports[k].cardholder == p->cardholder;
	if (!made) {
		make_cardholder(b, p);
		make_circuit_pack(b, caps, p);
	}
	if (pots) {
		make_pots(b, p);
	} else if (p->pptp_instance != 0) {
		make_pptp(b, p);
		make_uni_g(b, p);
	}
}

/*
 * VoIP config data (G.988 9.9.18), of a device with phone sockets: SIP the
 * one signalling protocol it has, and OMCI the one way to set it up, neither
 * chosen yet; no configuration address; inactive, and no profile retrieved.
 */
static void make_voip_config(struct build *b)
{
	static const uint8_t by_omci[4] = { 0, 0, 0, 1 };

	begin(b, ME_VOIP_CONFIG, 0);
	put_u8(b, 1, 1u);
	put(b, 3, by_omci, sizeof(by_omci));
	put_u16(b, 5, 0xffffu);
	finish(b);
}

/* Whether the device has a phone socket. */
static bool has_pots(const struct omci_hw_caps *caps)
{
	bool pots = false;

	for (unsigned int i = 0; i < caps->port_count; i++)
		pots = pots ||
		       caps->ports[i].plugin_type == OMCI_HW_PLUGIN_POTS;
	return pots;
}

/*
 * ANI-G: status reporting supported, the number of T-CONTs, a GEM block of
 * 48 bytes, signal fail at 10^-5 and degrade at 10^-9, and the optical and
 * transmit power thresholds left to the ONU's own defaults (0xff, 0x81).
 */
static void make_ani_g(struct build *b, const struct omci_hw_caps *caps)
{
	begin(b, ME_ANI_G, caps->ani_instance);
	put_u8(b, 1, 1);
	put_u16(b, 2, caps->num_tcont);
	put_u16(b, 3, 48);

	put_u8(b, 6, 5);
	put_u8(b, 7, 9);

	put_u8(b, 11, 0xff);
	put_u8(b, 12, 0xff);

	put_u8(b, 15, 0x81);
	put_u8(b, 16, 0x81);
	finish(b);
}

static uint16_t tcont_id(unsigned int n)
{
	return (uint16_t)((PON_SLOT << 8) | n);
}

/* A T-CONT: no Alloc-ID yet, a deprecated attribute at 1, strict priority. */
static void make_tcont(struct build *b, unsigned int n)
{
	begin(b, ME_TCONT, tcont_id(n));
	put_u16(b, 1, ALLOC_ID_UNASSIGNED);
	put_u8(b, 2, 1);
	put_u8(b, 3, 1);
	finish(b);
}

/* The traffic scheduler of T-CONT @n, with strict priority. */
static void make_scheduler(struct build *b, unsigned int n)
{
	begin(b, ME_TRAFFIC_SCHED, (uint16_t)((PON_SLOT << 8) | n));
	put_u16(b, 1, tcont_id(n));

	put_u8(b, 3, 1);

	finish(b);
}

/*
 * A priority queue: its sizes, the port and priority it serves, its
 * scheduler, a weight of 1, and drop thresholds at the full size.
 */
static void make_queue(struct build *b, uint16_t id, uint8_t slot,
		       uint8_t number, uint16_t priority, uint16_t scheduler)
{
	uint8_t related[4];
	uint8_t drop[8];

	related[0] = slot;
	related[1] = number;
	omci_be16_store(&related[2], priority);
	for (unsigned int i = 0; i < 4; i++)
		omci_be16_store(&drop[2 * i], QUEUE_SIZE);

	begin(b, ME_PRIORITY_QUEUE, id);

	put_u16(b, 2, QUEUE_SIZE);
	put_u16(b, 3, QUEUE_SIZE);

	put(b, 6, related, sizeof(related));
	put_u16(b, 7, scheduler);
	put_u8(b, 8, 1);

	put(b, 13, drop, sizeof(drop));
	put_u16(b, 14, 0xffff);
	put_u8(b, 15, 9);

	finish(b);
}

/* The upstream queues: one per priority of every T-CONT, from 0x8000. */
static void make_upstream_queues(struct build *b,
				 const struct omci_hw_caps *caps)
{
	unsigned int number = 0;

	for (unsigned int t = 0; t < caps->num_tcont; t++)
		for (unsigned int q = 0; q < caps->num_priq_per_tcont;
		     q++, number++)
			make_queue(b, (uint16_t)(0x8000u | number), PON_SLOT,
				   (uint8_t)t, (uint16_t)q,
				   (uint16_t)((PON_SLOT << 8) | t));
}

/* The downstream queues: one per priority of every subscriber port, from 0. */
static void make_downstream_queues(struct build *b,
				   const struct omci_hw_caps *caps)
{
	unsigned int number = 0;

	for (unsigned int i = 0; i < caps->port_count; i++) {
		const struct omci_hw_port *p = &caps->ports[i];

		if (p->pptp_instance == 0 ||
		    p->plugin_type == OMCI_HW_PLUGIN_POTS ||
		    caps->uni_report == OMCI_HW_UNI_VEIP)
			continue;
		for (unsigned int q = 0; q < caps->num_priq_per_port;
		     q++, number++)
			make_queue(b, (uint16_t)number,
				   (uint8_t)(p->cardholder & 0xffu),
				   (uint8_t)(p->pptp_instance & 0xffu),
				   (uint16_t)q, 0);
	}
}

/*
 * Software image @n: its version; the committed image is reported as
 * committed, active and valid, the other one as valid when the backend says
 * it holds valid software, and as neither committed nor active.
 */
static void make_software_image(struct build *b,
				const struct omci_hw_caps *caps, unsigned int n)
{
	const bool running = n == caps->sw_committed_image;

	begin(b, ME_SOFTWARE_IMAGE, (uint16_t)n);
	put_text(b, 1, caps->identity.sw_version[n], 0);
	put_u8(b, 2, running ? 1u : 0u);
	put_u8(b, 3, running ? 1u : 0u);
	put_u8(b, 4, running || (caps->sw_valid_images >> n & 1u) ? 1u : 0u);

	finish(b);
}

/* An IP host and its MAC address. */
static void make_ip_host(struct build *b, const struct omci_hw_caps *caps,
			 unsigned int n)
{
	begin(b, ME_IP_HOST, (uint16_t)n);

	put(b, 2, caps->identity.mac, sizeof(caps->identity.mac));

	finish(b);
}

/*
 * An IPv6 host, locked as G.988 9.4.5 has it until the OLT unlocks it, with
 * its MAC address and a DHCPv6 unique identifier of the link-layer kind
 * (RFC 8415 11.2): type 3, hardware type 1 for Ethernet, and the address.
 * The ids follow those of the IP hosts, which G.988 wants kept apart.
 */
static void make_ipv6_host(struct build *b, const struct omci_hw_caps *caps,
			   unsigned int n)
{
	uint8_t duid[25] = { 0x00, 0x03, 0x00, 0x01 };

	begin(b, ME_IPV6_HOST, (uint16_t)(caps->ip_host_count + n));

	put(b, 2, caps->identity.mac, sizeof(caps->identity.mac));
	omci_wire_copy(&duid[4], caps->identity.mac,
		       sizeof(caps->identity.mac));
	put(b, 12, duid, sizeof(duid));

	finish(b);
}

/*
 * The VEIP's slot: its cardholder and circuit pack, of one port, and the
 * VEIP, locked as G.988 suggests of a default (A.1.6), disabled until the
 * engine finds the router behind it, with a blank interdomain name and no
 * IANA assigned port.
 */
static void make_veip(struct build *b, const struct omci_hw_caps *caps)
{
	begin(b, ME_CARDHOLDER, VEIP_CARDHOLDER);
	put_u8(b, 1, VEIP_PLUGIN_TYPE);
	put_blank(b, 4);
	put_blank(b, 5);
	finish(b);

	begin(b, ME_CIRCUIT_PACK, VEIP_CARDHOLDER);
	put_u8(b, 1, VEIP_PLUGIN_TYPE);
	put_u8(b, 2, 1u);
	put(b, 3, caps->identity.serial, sizeof(caps->identity.serial));
	put_text(b, 4, pack_version(caps), 0);
	put_text(b, 5, caps->identity.vendor_id, 0);
	put_blank(b, 9);
	finish(b);

	begin(b, ME_VEIP, VEIP_INSTANCE);
	put_u8(b, 1, 1u);
	put_u8(b, 2, 1u);
	put_u16(b, 5, 0xffffu);
	finish(b);
}

/*
 * The LCT UNI, which stands for the debug access to the stick through its
 * subscriber port and IP stack (G.988 9.13.3): one, unlocked.
 */
static void make_lct_uni(struct build *b)
{
	begin(b, ME_PPTP_LCT_UNI, 0);
	put_u8(b, 1, 0u);
	finish(b);
}

enum omci_status omci_me_defaults_apply(struct omci_mib *mib,
					const struct omci_hw_caps *caps)
{
	if (mib == NULL || caps == NULL ||
	    caps->port_count > ARRAY_SIZE(caps->ports))
		return OMCI_ERR_INVAL;

	(void)omci_mib_reset(mib);

	struct build b = { .mib = mib, .rc = OMCI_OK };

	make_onu_data(&b);
	make_onu_g(&b, caps);
	make_onu2_g(&b, caps);
	make_olt_g(&b);
	for (unsigned int i = 0; i < caps->port_count; i++)
		make_slot(&b, caps, i);
	if (has_pots(caps))
		make_voip_config(&b);
	make_ani_g(&b, caps);
	for (unsigned int n = 0; n < caps->num_tcont; n++)
		make_tcont(&b, n);
	for (unsigned int n = 0; n < caps->num_tcont; n++)
		make_scheduler(&b, n);
	make_upstream_queues(&b, caps);
	make_downstream_queues(&b, caps);
	for (unsigned int n = 0; n < 2; n++)
		make_software_image(&b, caps, n);
	for (unsigned int n = 0; n < caps->ip_host_count; n++)
		make_ip_host(&b, caps, n);
	for (unsigned int n = 0; n < caps->ipv6_host_count; n++)
		make_ipv6_host(&b, caps, n);
	if (caps->uni_report != OMCI_HW_UNI_PPTP)
		make_veip(&b, caps);
	make_lct_uni(&b);
	make_omci(&b);

	omci_mib_notify_suspend(mib, false);
	return b.rc;
}
