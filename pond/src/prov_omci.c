// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the OLT set up, as omcid holds it: the instances of the managed
 * entity classes that describe the services and the unit, every attribute
 * of each in hexadecimal and the rows of their tables, and omcid's own
 * counts of the frames. omcid gives them on ubus (ADR 0011), as the answer
 * of its method "mib" to the classes below, and never the logical password
 * of the ONU-G, only whether one is set. The panel knows what the
 * attributes mean.
 */

#include "bus.h"
#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <stdint.h>

/*
 * The classes read, in the order a reader takes in a unit: what it is, its
 * software, its ports, the bridges, the VLAN rules, the channels and their
 * bandwidth, multicast, and the host interfaces.
 */
static const uint32_t g_classes[] = {
	256, /* ONU-G */
	257, /* ONU2-G */
	7,   /* software image */
	6,   /* circuit pack */
	131, /* OLT-G */
	263, /* ANI-G */
	11,  /* PPTP Ethernet UNI */
	329, /* virtual Ethernet interface point */
	134, /* IP host config data */
	45,  /* MAC bridge service profile */
	47,  /* MAC bridge port config data */
	130, /* IEEE 802.1p mapper service profile */
	84,  /* VLAN tagging filter data */
	171, /* extended VLAN tagging operation config data */
	78,  /* VLAN tagging operation config data */
	262, /* T-CONT */
	268, /* GEM port network CTP */
	266, /* GEM interworking termination point */
	281, /* multicast GEM interworking termination point */
	280, /* traffic descriptor */
	278, /* traffic scheduler */
	277, /* priority queue */
	272, /* GAL Ethernet profile */
	309, /* multicast operations profile */
	310, /* multicast subscriber config info */
	298, /* Dot1 rate limiter */
};

/* The room kept besides what omcid is asked to fit its answer in. */
#define ROOM 256u

/*
 * The most omcid is given for the managed entities, however much room the
 * buffer has: the room a value had when values had a buffer of a fixed size.
 */
#define MIB_TEXT (64u * 1024u)

int prov_omci(struct prov_buf *out)
{
	uint32_t max;
	struct bus_arg args[] = {
		{ .name = "max", .values = &max, .n = 1 },
		{ .name = "classes", .values = g_classes,
		  .n = ARRAY_SIZE(g_classes), .array = true },
	};
	int n;

	pv_reserve(out, MIB_TEXT);
	if (pv_room(out) < ROOM)
		return -ENOSPC;
	/* omcid leaves out what does not fit, and says so. */
	max = (uint32_t)(pv_room(out) - ROOM);
	if (max > MIB_TEXT - ROOM)
		max = MIB_TEXT - ROOM;
	n = bus_call("omcid", "mib", args, ARRAY_SIZE(args), out->buf + out->len,
		     pv_room(out));
	if (n < 0)
		return n;
	out->len += (size_t)n;
	return 0;
}

/*
 * The conversation with the OLT, as omcid keeps it (its method "frames"):
 * the frames from the number @since on, and the next one's number, so that
 * a reader that comes back with it gets only what is new. A frame is at
 * most this long as JSON text, and omcid gives at most 200 at a time.
 */
#define FRAME_TEXT 200u
#define FRAMES_MAX 200u

int prov_omci_frames_from(struct prov_buf *out, unsigned long since)
{
	uint32_t from = (uint32_t)since, max;
	struct bus_arg args[] = {
		{ .name = "since", .values = &from, .n = 1 },
		{ .name = "max", .values = &max, .n = 1 },
	};
	int n;

	pv_reserve(out, ROOM + FRAMES_MAX * FRAME_TEXT);
	if (pv_room(out) < ROOM + FRAME_TEXT)
		return -ENOSPC;
	max = (uint32_t)((pv_room(out) - ROOM) / FRAME_TEXT);
	if (max > FRAMES_MAX)
		max = FRAMES_MAX;
	n = bus_call("omcid", "frames", args, ARRAY_SIZE(args),
		     out->buf + out->len, pv_room(out));
	if (n < 0)
		return n;
	out->len += (size_t)n;
	return 0;
}

int prov_omci_frames(struct prov_buf *out)
{
	return prov_omci_frames_from(out, 0);
}
