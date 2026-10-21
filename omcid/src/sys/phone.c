// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The phone the OLT set up on the stick's phone sockets (publish.h,
 * omci_publish_phone()), as the managed entities of G.988 9.9 hold it: a
 * POTS UNI per socket, its SIP user data, the SIP agent and its servers,
 * the media profile and the RTP profile. Strings are the large strings,
 * network addresses and authentication methods they point at.
 */

#include <stdio.h>
#include <string.h>

#include "me/bridge.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "omcid/array_size.h"
#include "sys/publish.h"

enum {
	ME_POTS = 53,
	ME_TCP_UDP = 136,
	ME_NET_ADDRESS = 137,
	ME_VOIP_CONFIG = 138,
	ME_VOICE_CTP = 139,
	ME_MEDIA = 142,
	ME_RTP = 143,
	ME_AUTH = 148,
	ME_SIP_AGENT = 150,
	ME_SIP_USER = 153,
	ME_LARGE_STRING = 157,
	ME_VLAN_FILTER = 84,

	/* VoIP config data: the configuration method used. */
	CONFIG_METHOD = 4,
	METHOD_OMCI = 1,
	METHOD_TR069 = 3,

	/* VoIP voice CTP: its SIP user data, POTS UNI and media profile. */
	CTP_USER = 1,
	CTP_POTS = 2,
	CTP_MEDIA = 3,

	/* VoIP media profile: the first codec, every third; OOB DTMF; RTP profile. */
	MEDIA_CODEC = 3,
	MEDIA_OOB_DTMF = 15,
	MEDIA_RTP = 16,
	RTP_DTMF_EVENTS = 6,

	/* SIP agent config data. */
	AGENT_PROXY = 1,
	AGENT_OUTBOUND = 2,
	AGENT_TCP_UDP = 5,
	AGENT_HOST_PART = 8,
	AGENT_REGISTRAR = 10,

	/* SIP user data. */
	USER_AGENT = 1,
	USER_AOR = 2,
	USER_DISPLAY = 3,
	USER_AUTH = 4,
	USER_POTS = 10,

	/* Authentication security method, network address, TCP/UDP config data. */
	AUTH_USER1 = 2,
	AUTH_PASSWORD = 3,
	AUTH_USER2 = 5,
	ADDRESS_POINTER = 2,
	TCP_UDP_PORT = 1,
	TCP_UDP_PROTOCOL = 2,

	/* A large string: its parts and their size; the size of other strings. */
	LARGE_PARTS = 1,
	LARGE_PARTS_MAX = 15,
	FIELD = 25,
	TEXT_MAX = LARGE_PARTS_MAX * FIELD + 1,
};

/* A big-endian attribute of @size bytes, 0 when it cannot be read. */
static unsigned int number(const struct omci_mib *mib, uint16_t class_id,
			   uint16_t instance, uint8_t attr, size_t size)
{
	uint8_t b[4] = { 0 };
	unsigned int v = 0;

	(void)!omci_mib_attr_get(mib, class_id, instance, attr, b, size, NULL);
	for (size_t i = 0; i < size; i++)
		v = v << 8 | b[i];
	return v;
}

/*
 * A string attribute of @size bytes after the @len bytes of @out: its
 * printable ASCII up to the first other byte. Returns the new length.
 */
static size_t field(const struct omci_mib *mib, uint16_t class_id,
		    uint16_t instance, uint8_t attr, size_t size, char *out,
		    size_t len)
{
	uint8_t v[FIELD] = { 0 };

	(void)!omci_mib_attr_get(mib, class_id, instance, attr, v, size, NULL);
	for (size_t i = 0; i < size && v[i] >= 0x20u && v[i] < 0x7fu; i++)
		out[len++] = (char)v[i];
	out[len] = '\0';
	return len;
}

/* The text of large string @instance (G.988 9.12.5), its parts together. */
static void large_text(const struct omci_mib *mib, uint16_t instance,
		       char out[static TEXT_MAX])
{
	unsigned int parts =
		number(mib, ME_LARGE_STRING, instance, LARGE_PARTS, 1);
	size_t len = 0;

	out[0] = '\0';
	if (parts > LARGE_PARTS_MAX)
		parts = LARGE_PARTS_MAX;
	for (unsigned int p = 0; p < parts && len == p * FIELD; p++)
		len = field(mib, ME_LARGE_STRING, instance,
			    (uint8_t)(LARGE_PARTS + 1 + p), FIELD, out, len);
}

/* A string, or none when empty. */
static void text_or_none(const struct omci_publish_ops *ops, void *ctx,
			 const char *name, const char *text)
{
	if (*text)
		ops->str(ctx, name, text);
	else
		ops->none(ctx, name);
}

/* The large string that attribute @attr of an instance points at, or none. */
static void pointed(const struct omci_mib *mib, uint16_t class_id,
		    uint16_t instance, uint8_t attr,
		    const struct omci_publish_ops *ops, void *ctx,
		    const char *name)
{
	char text[TEXT_MAX];

	large_text(mib, (uint16_t)number(mib, class_id, instance, attr, 2),
		   text);
	text_or_none(ops, ctx, name, text);
}

/*
 * The first instance of @class_id whose attribute @attr, of @size bytes, is
 * @value, into @out; false without one.
 */
static bool holding(const struct omci_mib *mib, uint16_t class_id, uint8_t attr,
		    size_t size, uint16_t value, uint16_t *out)
{
	struct omci_mib_cursor cur;

	for (enum omci_status w = omci_mib_walk_first(mib, &cur); w == OMCI_OK;
	     w = omci_mib_walk_next(mib, &cur))
		if (cur.class_id == class_id &&
		    number(mib, class_id, cur.instance, attr, size) == value) {
			*out = cur.instance;
			return true;
		}
	return false;
}

/* The first instance of @class_id whose pointer @attr is @value. */
static bool pointing(const struct omci_mib *mib, uint16_t class_id,
		     uint8_t attr, uint16_t value, uint16_t *out)
{
	return holding(mib, class_id, attr, 2, value, out);
}

/*
 * The SIP user data of a socket: the one that points at its POTS UNI, or
 * the one its VoIP voice CTP points at. False without one.
 */
static bool sip_user(const struct omci_mib *mib, uint16_t pots, uint16_t *out)
{
	uint16_t ctp;

	if (pointing(mib, ME_SIP_USER, USER_POTS, pots, out))
		return true;
	if (!pointing(mib, ME_VOICE_CTP, CTP_POTS, pots, &ctp))
		return false;
	*out = (uint16_t)number(mib, ME_VOICE_CTP, ctp, CTP_USER, 2);
	return omci_mib_exists(mib, ME_SIP_USER, *out);
}

/* The username of an authentication security method: usernames 1 and 2 together. */
static void auth_user(const struct omci_mib *mib, uint16_t auth,
		      char out[static 2 * FIELD + 1])
{
	const size_t n = field(mib, ME_AUTH, auth, AUTH_USER1, FIELD, out, 0);

	(void)field(mib, ME_AUTH, auth, AUTH_USER2, FIELD, out, n);
}

/* A socket: its number, and the account of its SIP user data, if any. */
static void put_line(const struct omci_mib *mib, uint16_t pots,
		     const struct omci_publish_ops *ops, void *ctx)
{
	char aor[TEXT_MAX] = "", display[FIELD + 1] = "",
	     user[2 * FIELD + 1] = "", password[FIELD + 1] = "";
	uint16_t sip, auth;

	if (sip_user(mib, pots, &sip)) {
		large_text(mib,
			   (uint16_t)number(mib, ME_SIP_USER, sip, USER_AOR, 2),
			   aor);
		(void)field(mib, ME_SIP_USER, sip, USER_DISPLAY, FIELD, display,
			    0);
		auth = (uint16_t)number(mib, ME_SIP_USER, sip, USER_AUTH, 2);
		auth_user(mib, auth, user);
		(void)field(mib, ME_AUTH, auth, AUTH_PASSWORD, FIELD, password,
			    0);
	}
	ops->open(ctx, NULL, false);
	ops->num(ctx, "port", pots & 0xffu);
	text_or_none(ops, ctx, "number", aor);
	text_or_none(ops, ctx, "display", display);
	text_or_none(ops, ctx, "user", user);
	ops->flag(ctx, "password_set", password[0] != '\0');
	ops->close(ctx, false);
}

/*
 * The operator's SIP servers, of the SIP agent of the first socket's SIP
 * user data: its proxy, outbound proxy, registrar
 * (a network address), domain (the host part of its URIs), and its TCP/UDP
 * config data's port and protocol.
 */
static void put_server(const struct omci_mib *mib, uint16_t agent,
		       const struct omci_publish_ops *ops, void *ctx)
{
	const uint16_t tcp =
		(uint16_t)number(mib, ME_SIP_AGENT, agent, AGENT_TCP_UDP, 2);
	const uint16_t net =
		(uint16_t)number(mib, ME_SIP_AGENT, agent, AGENT_REGISTRAR, 2);
	const unsigned int protocol =
		number(mib, ME_TCP_UDP, tcp, TCP_UDP_PROTOCOL, 1);

	ops->open(ctx, "server", false);
	pointed(mib, ME_SIP_AGENT, agent, AGENT_PROXY, ops, ctx, "proxy");
	pointed(mib, ME_SIP_AGENT, agent, AGENT_OUTBOUND, ops, ctx,
		"outbound_proxy");
	pointed(mib, ME_NET_ADDRESS, net, ADDRESS_POINTER, ops, ctx,
		"registrar");
	pointed(mib, ME_SIP_AGENT, agent, AGENT_HOST_PART, ops, ctx, "domain");
	if (omci_mib_exists(mib, ME_TCP_UDP, tcp))
		ops->num(ctx, "port",
			 number(mib, ME_TCP_UDP, tcp, TCP_UDP_PORT, 2));
	else
		ops->none(ctx, "port");
	text_or_none(ops, ctx, "transport",
		     protocol == 6u  ? "tcp" :
		     protocol == 17u ? "udp" :
				       "");
	ops->close(ctx, false);
}

/* The name of a codec, a payload type of RFC 3551. */
static void codec_name(unsigned int pt, char out[static 24])
{
	static const struct {
		unsigned int pt;
		const char *name;
	} known[] = {
		{ 0, "G.711 \xc2\xb5-law" },
		{ 3, "GSM" },
		{ 4, "G.723" },
		{ 8, "G.711 A-law" },
		{ 9, "G.722" },
		{ 15, "G.728" },
		{ 18, "G.729" },
	};

	snprintf(out, 24, "payload type %u", pt);
	for (size_t i = 0; i < ARRAY_SIZE(known); i++)
		if (known[i].pt == pt)
			snprintf(out, 24, "%s", known[i].name);
}

/*
 * The codecs of a media profile in its order, each once, and how DTMF goes:
 * in the voice (inband), or out of it, as RTP events (RFC 2833, now RFC
 * 4733) when the RTP profile says so, or else by SIP.
 */
static void put_media(const struct omci_mib *mib, uint16_t media, bool have,
		      const struct omci_publish_ops *ops, void *ctx)
{
	unsigned int seen[4];
	size_t n = 0;

	ops->open(ctx, "codecs", true);
	for (unsigned int k = 0; k < 4u && have; k++) {
		const unsigned int pt = number(mib, ME_MEDIA, media,
					       (uint8_t)(MEDIA_CODEC + 3u * k),
					       1);
		bool dup = false;
		char name[24];

		for (size_t i = 0; i < n; i++)
			dup = dup || seen[i] == pt;
		if (dup)
			continue;
		seen[n++] = pt;
		codec_name(pt, name);
		ops->str(ctx, NULL, name);
	}
	ops->close(ctx, true);
	if (!have) {
		ops->none(ctx, "dtmf");
		return;
	}
	if (number(mib, ME_MEDIA, media, MEDIA_OOB_DTMF, 1) == 0u)
		ops->str(ctx, "dtmf", "inband");
	else if (number(mib, ME_RTP,
			(uint16_t)number(mib, ME_MEDIA, media, MEDIA_RTP, 2),
			RTP_DTMF_EVENTS, 1) != 0u)
		ops->str(ctx, "dtmf", "rfc2833");
	else
		ops->str(ctx, "dtmf", "info");
}

/*
 * The VLAN of the voice: of the IP host's VLAN rules (omci_vlan_phone()),
 * or of the VLAN filter of an IP host's bridge port, which keeps the tag
 * both ways; none without either.
 */
static void put_vlan(const struct omci_mib *mib,
		     const struct omci_publish_ops *ops, void *ctx)
{
	uint16_t fibre = 0, host = 0, port = 0;
	uint8_t vids[24] = { 0 };

	if (!omci_vlan_phone(&fibre, &host) &&
	    holding(mib, OMCI_BRIDGE_PORT_CLASS_ID,
		    OMCI_BRIDGE_PORT_ATTR_TPTYPE, 1, OMCI_BRIDGE_TP_IP_HOST,
		    &port) &&
	    omci_mib_attr_get(mib, ME_VLAN_FILTER, port,
			      OMCI_BRIDGE_VLAN_ATTR_TABLE, vids, sizeof(vids),
			      NULL) == OMCI_OK) {
		fibre = (uint16_t)((vids[0] << 8 | vids[1]) & 0x0fffu);
		host = fibre;
	}
	if (fibre == 0u) {
		ops->none(ctx, "vlan");
		return;
	}
	ops->open(ctx, "vlan", false);
	ops->num(ctx, "fibre", fibre);
	if (host != 0u)
		ops->num(ctx, "host", host);
	else
		ops->none(ctx, "host");
	ops->close(ctx, false);
}

/* Whether the MIB holds an instance of @class_id. */
static bool any_of(const struct omci_mib *mib, uint16_t class_id)
{
	struct omci_mib_cursor cur;

	for (enum omci_status w = omci_mib_walk_first(mib, &cur); w == OMCI_OK;
	     w = omci_mib_walk_next(mib, &cur))
		if (cur.class_id == class_id)
			return true;
	return false;
}

void omci_publish_phone(const struct omci_mib *mib,
			const struct omci_publish_ops *ops, void *ctx)
{
	const unsigned int method =
		number(mib, ME_VOIP_CONFIG, 0, CONFIG_METHOD, 1);
	struct omci_mib_cursor cur;
	uint16_t first = 0, sip = 0, ctp = 0, agent, media;
	bool lines = false;

	ops->str(ctx, "method",
		 method == METHOD_TR069				   ? "tr069" :
		 method == METHOD_OMCI || any_of(mib, ME_SIP_USER) ? "omci" :
								     "none");
	ops->open(ctx, "lines", true);
	for (enum omci_status w = omci_mib_walk_first(mib, &cur); w == OMCI_OK;
	     w = omci_mib_walk_next(mib, &cur)) {
		if (cur.class_id != ME_POTS)
			continue;
		first = lines ? first : cur.instance;
		lines = true;
		put_line(mib, cur.instance, ops, ctx);
	}
	ops->close(ctx, true);

	lines = lines && sip_user(mib, first, &sip);
	agent = (uint16_t)number(mib, ME_SIP_USER, sip, USER_AGENT, 2);
	if (lines && omci_mib_exists(mib, ME_SIP_AGENT, agent))
		put_server(mib, agent, ops, ctx);
	else
		ops->none(ctx, "server");

	lines = lines && pointing(mib, ME_VOICE_CTP, CTP_POTS, first, &ctp);
	media = (uint16_t)number(mib, ME_VOICE_CTP, ctp, CTP_MEDIA, 2);
	put_media(mib, media, lines && omci_mib_exists(mib, ME_MEDIA, media),
		  ops, ctx);
	put_vlan(mib, ops, ctx);
}

void omci_publish_phone_secrets(const struct omci_mib *mib,
				const struct omci_publish_ops *ops, void *ctx)
{
	struct omci_mib_cursor cur;

	ops->open(ctx, "lines", true);
	for (enum omci_status w = omci_mib_walk_first(mib, &cur); w == OMCI_OK;
	     w = omci_mib_walk_next(mib, &cur)) {
		char secret[FIELD + 1] = "";
		uint16_t sip;

		if (cur.class_id != ME_POTS ||
		    !sip_user(mib, cur.instance, &sip))
			continue;
		(void)field(mib, ME_AUTH,
			    (uint16_t)number(mib, ME_SIP_USER, sip, USER_AUTH,
					     2),
			    AUTH_PASSWORD, FIELD, secret, 0);
		ops->open(ctx, NULL, false);
		ops->num(ctx, "port", cur.instance & 0xffu);
		ops->str(ctx, "secret", secret);
		ops->close(ctx, false);
	}
	ops->close(ctx, true);
}

bool omci_publish_sip_account(const struct omci_mib *mib, uint16_t auth)
{
	uint16_t sip;

	return pointing(mib, ME_SIP_USER, USER_AUTH, auth, &sip);
}
