// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The kernel backend: the hardware of the board through the kernel's own
 * interfaces. The PON device answers on the generic netlink family "pon"
 * (serial number, T-CONTs, GEM ports, alarms). GEM ports are links of kind
 * "gem" stacked on the interface pon, and they join the bridge omcibr0 with
 * the subscriber interface sfp; the switch carries that bridge. VLAN rules
 * are tc flower filters with a vlan action (modify, push or pop), offloaded
 * to the switch. The
 * counters come from ethtool and the optical readings from hwmon.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/ethtool.h>
#include <linux/genetlink.h>
#include <arpa/inet.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_vlan.h>
#include <linux/netlink.h>
#include <linux/pon.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "hw/hw.h"
#include "hw_kernel.h"
#include "omcid/array_size.h"
#include "omcid/attrs.h"
#include "sys/log.h"

/*
 * The identity of the ONU, written at boot from the unit's own data: one
 * "key=value" per line, with the keys vendor_id, version, equipment_id,
 * tm_option, omcc_version, vendor_product_code and serial (four letters
 * and eight hexadecimal digits).
 */
#define IDENTITY_PATH "/run/onu-identity"

/*
 * The switch ports and the interfaces of the board: the subscriber side,
 * the PON side, and the management VLAN of the subscriber side.
 */
#define UNI_PORT 0u
#define PON_PORT 2u
#define UNI_IFNAME "sfp"
#define PON_IFNAME "pon"
#define MGMT_IFNAME "mgmt"

/*
 * The board, in the terms of G.988: an Ethernet UNI in slot 1 (plug-in
 * type 47, 10/100/1000 Ethernet), the PON side in slot 0x80 (type 248,
 * GPON 2488/1244), eight T-CONTs unless the driver says otherwise, one
 * queue per IEEE 802.1p priority, and OMCC version 0xA3: ITU-T G.988,
 * baseline message set only.
 */
#define UNI_INSTANCE 0x0101u
#define PON_CARDHOLDER 0x0180u
#define ANI_INSTANCE 0x8001u
#define PLUGIN_ETHERNET 47u
#define PLUGIN_GPON 248u
#define BOARD_TCONTS 8u
#define BOARD_QUEUES 8u
#define OMCC_BASELINE 0xa3u

/* Where the description came from, for the log. */
static char g_source[160] = "not read yet";
static bool g_have_serial;
static bool g_have_identity;

/* The value of @key in the identity file, up to the end of its line; NULL without one. */
static const char *identity_get(const char *key)
{
	static char buf[4096];
	static bool loaded;
	static size_t len;
	size_t klen = strlen(key);
	const char *p;

	if (!loaded) {
		FILE *f = fopen(IDENTITY_PATH, "re");

		loaded = true;
		if (f) {
			len = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
			buf[len] = '\0';
			g_have_identity = len > 0;
		}
	}
	if (!len)
		return NULL;

	for (p = buf; p && *p; p = strchr(p, '\n')) {
		if (p[0] == '\n')
			p++;
		if (strncmp(p, key, klen) || p[klen] != '=')
			continue;
		return p + klen + 1;
	}
	return NULL;
}

/* Copy a value of the identity file as a string. */
static bool identity_copy(const char *key, char *out, size_t cap)
{
	const char *v = identity_get(key);
	size_t n;

	if (!v)
		return false;
	n = strcspn(v, "\n");
	if (n >= cap)
		n = cap - 1;
	memcpy(out, v, n);
	out[n] = '\0';
	return n > 0;
}

/* Read a value of the identity file as a decimal number. */
static bool identity_number(const char *key, unsigned long *out)
{
	char t[32];
	char *end;

	if (!identity_copy(key, t, sizeof(t)))
		return false;
	*out = strtoul(t, &end, 10);
	return end != t;
}

/* The sizes of a netlink header and an attribute header, and their alignment. */
#define NL_HDRLEN 16u
#define NLA_HDRLEN_ 4u
#define NL_ALIGN(n) (((n) + 3u) & ~3u)

/*
 * Send one generic netlink request and collect the answer: one message, or
 * every message of a dump when @flags asks for one.
 *
 * Return: 0, or a negative errno.
 */
static int nl_request(int type, uint8_t cmd, const void *attrs, size_t alen,
		      uint16_t flags, void *out, size_t cap, size_t *outlen)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	uint8_t req[256] __aligned(4);
	uint8_t *rsp = out;
	size_t got = 0;
	ssize_t n;
	int fd, err = 0;
	uint32_t seq = 1;

	if (NL_HDRLEN + 4 + alen > sizeof(req))
		return -EINVAL;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		err = -errno;
		goto out;
	}

	memset(req, 0, sizeof(req));
	*(uint32_t *)&req[0] = (uint32_t)(NL_HDRLEN + 4 + alen);
	*(uint16_t *)&req[4] = (uint16_t)type;
	*(uint16_t *)&req[6] = (uint16_t)(NLM_F_REQUEST | flags);
	*(uint32_t *)&req[8] = seq;
	req[NL_HDRLEN] = cmd;
	req[NL_HDRLEN + 1] = 1;
	if (alen)
		memcpy(req + NL_HDRLEN + 4, attrs, alen);

	if (send(fd, req, NL_HDRLEN + 4 + alen, 0) < 0) {
		err = -errno;
		goto out;
	}

	for (;;) {
		uint8_t buf[8192] __aligned(4);
		const uint8_t *p = buf;
		size_t left;

		n = recv(fd, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			goto out;
		}
		left = (size_t)n;

		while (left >= NL_HDRLEN) {
			uint32_t mlen = *(const uint32_t *)p;
			uint16_t mtype = *(const uint16_t *)(p + 4);

			if (mlen < NL_HDRLEN || mlen > left) {
				err = -EPROTO;
				goto out;
			}
			if (mtype == NLMSG_ERROR) {
				int kerr = 0;

				if (mlen >= NL_HDRLEN + sizeof(kerr))
					memcpy(&kerr, p + NL_HDRLEN,
					       sizeof(kerr));
				err = kerr ? kerr : 0;
				goto out;
			}
			if (mtype == NLMSG_DONE)
				goto out;

			if (got + mlen > cap) {
				err = -EFBIG;
				goto out;
			}
			memcpy(rsp + got, p, mlen);
			got += mlen;

			if (!(flags & NLM_F_DUMP))
				goto out;

			left -= NL_ALIGN(mlen);
			p += NL_ALIGN(mlen);
		}
	}
out:
	close(fd);
	*outlen = got;
	return err;
}

/* The number the kernel gave the family "pon". */
static int pon_family_id(uint16_t *id)
{
	uint8_t attrs[32] __aligned(4);
	uint8_t rsp[4096] __aligned(4);
	size_t rlen = 0;
	size_t nlen = strlen(PON_FAMILY_NAME) + 1;
	const uint8_t *p;
	const uint8_t *end;
	int err;

	*(uint16_t *)&attrs[0] = (uint16_t)(NLA_HDRLEN_ + nlen);
	*(uint16_t *)&attrs[2] = CTRL_ATTR_FAMILY_NAME;
	memcpy(attrs + NLA_HDRLEN_, PON_FAMILY_NAME, nlen);

	err = nl_request(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, attrs,
			 NL_ALIGN(NLA_HDRLEN_ + nlen), 0, rsp, sizeof(rsp),
			 &rlen);
	if (err)
		return err;
	if (rlen < NL_HDRLEN + 4)
		return -EPROTO;

	p = rsp + NL_HDRLEN + 4;
	end = rsp + rlen;
	while (p + NLA_HDRLEN_ <= end) {
		uint16_t alen = *(const uint16_t *)p;
		uint16_t atype = *(const uint16_t *)(p + 2) & 0x3fff;

		if (alen < NLA_HDRLEN_)
			break;
		if (atype == CTRL_ATTR_FAMILY_ID &&
		    alen >= NLA_HDRLEN_ + sizeof(*id)) {
			memcpy(id, p + NLA_HDRLEN_, sizeof(*id));
			return 0;
		}
		p += NL_ALIGN(alen);
	}
	return -ENOENT;
}

/* The attributes of the PON device, pointing into the buffer of the answer. */
struct pon_dev_attrs {
	const uint8_t *val[PON_A_DEV_MAX + 1];
	uint16_t len[PON_A_DEV_MAX + 1];
	bool present;
};

/* Read the attributes of the PON device. Return: 0, or a negative errno. */
static int pon_dev_get(struct pon_dev_attrs *out, uint8_t *buf, size_t cap)
{
	uint16_t family = 0;
	size_t rlen = 0;
	const uint8_t *p;
	const uint8_t *end;
	int err;

	memset(out, 0, sizeof(*out));

	err = pon_family_id(&family);
	if (err)
		return err;

	err = nl_request(family, PON_CMD_DEV_GET, NULL, 0, NLM_F_DUMP, buf, cap,
			 &rlen);
	if (err)
		return err;
	if (rlen < NL_HDRLEN + 4)
		return -ENODEV;

	p = buf + NL_HDRLEN + 4;
	end = buf + rlen;
	while (p + NLA_HDRLEN_ <= end) {
		uint16_t alen = *(const uint16_t *)p;
		uint16_t atype = *(const uint16_t *)(p + 2) & 0x3fff;

		if (alen < NLA_HDRLEN_ || p + alen > end)
			break;
		if (atype <= PON_A_DEV_MAX) {
			out->val[atype] = p + NLA_HDRLEN_;
			out->len[atype] = (uint16_t)(alen - NLA_HDRLEN_);
		}
		p += NL_ALIGN(alen);
	}
	out->present = true;
	return 0;
}

/* A 32-bit attribute, or @absent. */
static uint32_t pon_attr_u32(const struct pon_dev_attrs *a, unsigned int type,
			     uint32_t absent)
{
	uint32_t v;

	if (type > PON_A_DEV_MAX || !a->val[type] || a->len[type] < sizeof(v))
		return absent;
	memcpy(&v, a->val[type], sizeof(v));
	return v;
}

/* A serial number from its text: four letters, then eight hexadecimal digits. */
static bool serial_from_text(const char *s, uint8_t out[static 8])
{
	unsigned int i;

	if (!s || strlen(s) < 12)
		return false;
	memcpy(out, s, 4);
	for (i = 0; i < 4; i++) {
		char t[3] = { s[4 + 2 * i], s[5 + 2 * i], '\0' };
		char *endp;
		unsigned long v = strtoul(t, &endp, 16);

		if (endp != t + 2)
			return false;
		out[4 + i] = (uint8_t)v;
	}
	return true;
}

/* The number of T-CONTs, from the driver. Return: false when it did not say. */
static bool caps_tconts(struct omci_hw_caps *caps)
{
	struct pon_dev_attrs a;
	uint8_t buf[4096] __aligned(4);
	uint32_t n;

	if (pon_dev_get(&a, buf, sizeof(buf)) != 0 ||
	    !a.val[PON_A_DEV_NUM_TCONTS])
		return false;

	n = pon_attr_u32(&a, PON_A_DEV_NUM_TCONTS, 0);
	if (!n || n > UINT8_MAX)
		return false;

	if (n != caps->num_tcont)
		OMCI_INFO(
			"caps: the driver reports %u T-CONT(s), the board default is %u",
			n, caps->num_tcont);
	caps->num_tcont = (uint8_t)n;
	return true;
}

/*
 * The serial number, from the driver, which the OLT knows the ONU by; the
 * identity file is the fallback.
 */
static void identity_serial(struct omci_hw_caps *caps)
{
	struct pon_dev_attrs a;
	uint8_t buf[4096] __aligned(4);
	uint8_t from_file[8];
	bool have_file;

	have_file = serial_from_text(identity_get("serial"), from_file);

	if (pon_dev_get(&a, buf, sizeof(buf)) == 0 &&
	    a.val[PON_A_DEV_SERIAL_NUMBER] &&
	    a.len[PON_A_DEV_SERIAL_NUMBER] == sizeof(caps->identity.serial)) {
		memcpy(caps->identity.serial, a.val[PON_A_DEV_SERIAL_NUMBER],
		       sizeof(caps->identity.serial));
		g_have_serial = true;

		if (have_file &&
		    memcmp(from_file, caps->identity.serial, sizeof(from_file)))
			OMCI_WARN(
				"serial: the driver and the flash disagree; using the driver's");
		return;
	}

	if (have_file) {
		memcpy(caps->identity.serial, from_file, sizeof(from_file));
		g_have_serial = true;
		OMCI_WARN("serial: the driver did not answer; using the flash");
	}
}

/* The version of the software images: the release of the running kernel. */
static void identity_software(struct omci_hw_caps *caps)
{
	char version[sizeof(caps->identity.sw_version[0])] = "";
	FILE *f = fopen("/proc/sys/kernel/osrelease", "re");

	if (f) {
		if (fgets(version, sizeof(version), f))
			version[strcspn(version, "\n")] = '\0';
		fclose(f);
	}
	if (!version[0])
		return;

	snprintf(caps->identity.sw_version[0],
		 sizeof(caps->identity.sw_version[0]), "%s", version);
	snprintf(caps->identity.sw_version[1],
		 sizeof(caps->identity.sw_version[1]), "%s", version);
}

/* The fixed part of the description of the board. */
static void board_caps(struct omci_hw_caps *out)
{
	memset(out, 0, sizeof(*out));

	out->ports[0] = (struct omci_hw_port){ .pptp_instance = UNI_INSTANCE,
					       .cardholder = UNI_INSTANCE,
					       .plugin_type = PLUGIN_ETHERNET };
	out->ports[1] = (struct omci_hw_port){ .pptp_instance = 0,
					       .cardholder = PON_CARDHOLDER,
					       .plugin_type = PLUGIN_GPON };
	out->port_count = 2;
	out->uni_port_count = 1;
	out->ani_instance = ANI_INSTANCE;
	out->ip_host_count = 1;
	out->sw_committed_image = 0;
	out->num_gem_ports = OMCI_HW_GEM_MAX_PORTS;
	out->num_tcont = BOARD_TCONTS;
	out->num_priq_per_tcont = BOARD_QUEUES;
	out->num_priq_per_port = BOARD_QUEUES;
	out->identity.omcc_version = OMCC_BASELINE;
}

/* The GEM ports the driver can carry, up to what the daemon handles. */
static void caps_gem_ports(struct omci_hw_caps *caps)
{
	struct pon_dev_attrs a;
	uint8_t buf[4096] __aligned(4);
	uint32_t n;

	if (pon_dev_get(&a, buf, sizeof(buf)) != 0)
		return;
	n = pon_attr_u32(&a, PON_A_DEV_NUM_GEM_PORTS, 0);
	if (n && n < caps->num_gem_ports)
		caps->num_gem_ports = (uint16_t)n;
}

/* The MAC address of the IP host: the management interface's. */
static void ip_host_mac(struct omci_hw_caps *caps)
{
	unsigned int m[6];
	FILE *f = fopen("/sys/class/net/" MGMT_IFNAME "/address", "re");

	if (!f)
		return;
	if (fscanf(f, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4],
		   &m[5]) == 6)
		for (unsigned int i = 0; i < 6; i++)
			caps->identity.mac[i] = (uint8_t)m[i];
	fclose(f);
}

static enum omci_status kernel_caps(struct omci_hw_caps *out)
{
	unsigned long n;
	bool ident = false;
	bool tconts;

	if (out == NULL)
		return OMCI_ERR_INVAL;

	board_caps(out);
	tconts = caps_tconts(out);
	caps_gem_ports(out);
	ip_host_mac(out);

	ident |= identity_copy("vendor_id", out->identity.vendor_id,
			       sizeof(out->identity.vendor_id));
	ident |= identity_copy("version", out->identity.version,
			       sizeof(out->identity.version));
	ident |= identity_copy("equipment_id", out->identity.equipment_id,
			       sizeof(out->identity.equipment_id));
	if (identity_number("tm_option", &n))
		out->identity.tm_option = (uint8_t)n;
	if (identity_number("omcc_version", &n))
		out->identity.omcc_version = (uint8_t)n;
	if (identity_number("vendor_product_code", &n))
		out->identity.vendor_product_code = (uint16_t)n;

	identity_serial(out);
	identity_software(out);

	snprintf(g_source, sizeof(g_source),
		 "kernel: %u T-CONT(s) %s; identity %s; serial %s",
		 out->num_tcont,
		 tconts ? "from the driver" :
			  "by default (the driver did not say)",
		 ident ? "from " IDENTITY_PATH : "not found",
		 g_have_serial ? "from the PON driver" : "not found");
	return OMCI_OK;
}

static const char *kernel_source(void)
{
	return g_source;
}

/* A GEM port is the interface gemN, N its port identifier. */
#define GEM_NAME_FMT "gem%u"

/* A GEM interface that exists. */
struct gem_present {
	uint16_t port_id;
	uint8_t direction;
	uint8_t tcont;
	int ifindex;
	bool multicast;
};

/* The reason the kernel gave for the last refusal. */
static char g_rtnl_extack[128];

static const char *omci_hw_last_extack(void)
{
	return g_rtnl_extack[0] ? g_rtnl_extack : "no reason given";
}

/* Keep the message of the extended acknowledgement of an error answer. */
static void rtnl_extack(const uint8_t *p, uint32_t mlen, uint16_t flags)
{
	const uint8_t *end = p + mlen;
	const uint8_t *tlv;
	uint32_t orig;

	g_rtnl_extack[0] = '\0';
	if (mlen < NL_HDRLEN + sizeof(int) + NL_HDRLEN)
		return;

	memcpy(&orig, p + NL_HDRLEN + sizeof(int), sizeof(orig));
	tlv = p + NL_HDRLEN + sizeof(int);
	tlv += (flags & 0x100) ? NL_HDRLEN : NL_ALIGN(orig);

	while (tlv + NLA_HDRLEN_ <= end) {
		uint16_t alen = *(const uint16_t *)tlv;
		uint16_t atype = *(const uint16_t *)(tlv + 2) & 0x3fff;

		if (alen < NLA_HDRLEN_ || tlv + alen > end)
			break;
		if (atype == NLMSGERR_ATTR_MSG) {
			size_t n = alen - NLA_HDRLEN_;

			if (n >= sizeof(g_rtnl_extack))
				n = sizeof(g_rtnl_extack) - 1;
			memcpy(g_rtnl_extack, tlv + NLA_HDRLEN_, n);
			g_rtnl_extack[n] = '\0';
			return;
		}
		tlv += NL_ALIGN(alen);
	}
}

/*
 * Send one route netlink request and collect the answer, or every message
 * of a dump.
 *
 * Return: 0, or a negative errno.
 */
static int rtnl_talk(const void *req, size_t len, void *out, size_t cap,
		     size_t *outlen)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	uint8_t *rsp = out;
	size_t got = 0;
	int fd, err = 0;
	bool dump = (((const uint8_t *)req)[6] & (NLM_F_DUMP & 0xff)) != 0 ||
		    (*(const uint16_t *)((const uint8_t *)req + 6) &
		     NLM_F_DUMP) != 0;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	{
		int on = 1;

		(void)setsockopt(fd, SOL_NETLINK, NETLINK_EXT_ACK, &on,
				 sizeof(on));
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		err = -errno;
		goto out;
	}
	if (send(fd, req, len, 0) < 0) {
		err = -errno;
		goto out;
	}

	for (;;) {
		uint8_t buf[16384] __aligned(4);
		const uint8_t *p = buf;
		size_t left;
		ssize_t n = recv(fd, buf, sizeof(buf), 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			goto out;
		}
		left = (size_t)n;

		while (left >= NL_HDRLEN) {
			uint32_t mlen = *(const uint32_t *)p;
			uint16_t mtype = *(const uint16_t *)(p + 4);

			if (mlen < NL_HDRLEN || mlen > left) {
				err = -EPROTO;
				goto out;
			}
			if (mtype == NLMSG_ERROR) {
				int kerr = 0;

				if (mlen >= NL_HDRLEN + sizeof(kerr))
					memcpy(&kerr, p + NL_HDRLEN,
					       sizeof(kerr));
				err = kerr;
				if (err)
					rtnl_extack(p, mlen,
						    *(const uint16_t *)(p + 6));
				goto out;
			}
			if (mtype == NLMSG_DONE)
				goto out;

			if (out && got + mlen <= cap) {
				memcpy(rsp + got, p, mlen);
				got += mlen;
			} else if (out) {
				err = -EFBIG;
				goto out;
			}

			if (!dump)
				goto out;

			left -= NL_ALIGN(mlen);
			p += NL_ALIGN(mlen);
		}
	}
out:
	close(fd);
	if (outlen)
		*outlen = got;
	return err;
}

/* Append an attribute. Return: the offset after it. */
static size_t nla_put(uint8_t *buf, size_t off, uint16_t type, const void *val,
		      size_t len)
{
	*(uint16_t *)(buf + off) = (uint16_t)(NLA_HDRLEN_ + len);
	*(uint16_t *)(buf + off + 2) = type;
	if (len)
		memcpy(buf + off + NLA_HDRLEN_, val, len);
	return off + NL_ALIGN(NLA_HDRLEN_ + len);
}

/* The attributes of a GEM link: its port identifier, direction and T-CONT. */
static void gem_info_data(const uint8_t *d, const uint8_t *end,
			  struct gem_present *g)
{
	while (d + NLA_HDRLEN_ <= end) {
		uint16_t dlen = *(const uint16_t *)d;
		uint16_t dtype = *(const uint16_t *)(d + 2) & 0x3fff;
		const uint8_t *dv = d + NLA_HDRLEN_;

		if (dlen < NLA_HDRLEN_ || d + dlen > end)
			break;
		if (dtype == IFLA_GEM_PORT_ID && dlen >= NLA_HDRLEN_ + 2)
			memcpy(&g->port_id, dv, 2);
		if (dtype == IFLA_GEM_DIRECTION && dlen >= NLA_HDRLEN_ + 1)
			g->direction = dv[0];
		if (dtype == IFLA_GEM_TCONT && dlen >= NLA_HDRLEN_ + 4) {
			uint32_t tcont;

			memcpy(&tcont, dv, 4);
			g->tcont = (uint8_t)tcont;
		}
		if (dtype == IFLA_GEM_MULTICAST)
			g->multicast = true;
		d += NL_ALIGN(dlen);
	}
}

/* Whether the link information of a link names the kind "gem"; its data go to @g. */
static bool gem_link_info(const uint8_t *q, const uint8_t *end,
			  struct gem_present *g)
{
	bool is_gem = false;

	while (q + NLA_HDRLEN_ <= end) {
		uint16_t qlen = *(const uint16_t *)q;
		uint16_t qtype = *(const uint16_t *)(q + 2) & 0x3fff;

		if (qlen < NLA_HDRLEN_ || q + qlen > end)
			break;
		if (qtype == IFLA_INFO_KIND)
			is_gem = !strncmp((const char *)q + NLA_HDRLEN_, "gem",
					  4);
		if (qtype == IFLA_INFO_DATA)
			gem_info_data(q + NLA_HDRLEN_, q + qlen, g);
		q += NL_ALIGN(qlen);
	}
	return is_gem;
}

/* The GEM interfaces stacked on the interface @lower. Return: their number. */
static size_t gem_present(struct gem_present *out, size_t cap, int lower)
{
	uint8_t req[NL_HDRLEN + sizeof(struct ifinfomsg) + 16] __aligned(4);
	static uint8_t rsp[64 * 1024] __aligned(4);
	size_t rlen = 0, off = 0, n = 0;
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };

	memset(req, 0, sizeof(req));
	*(uint32_t *)&req[0] = NL_HDRLEN + sizeof(ifi);
	*(uint16_t *)&req[4] = RTM_GETLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_DUMP;
	*(uint32_t *)&req[8] = 1;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));

	if (rtnl_talk(req, NL_HDRLEN + sizeof(ifi), rsp, sizeof(rsp), &rlen))
		return 0;

	while (off + NL_HDRLEN + sizeof(struct ifinfomsg) <= rlen) {
		uint32_t mlen = *(const uint32_t *)(rsp + off);
		const uint8_t *p =
			rsp + off + NL_HDRLEN + sizeof(struct ifinfomsg);
		const uint8_t *end = rsp + off + mlen;
		struct ifinfomsg got;
		struct gem_present g = { .ifindex = 0 };
		bool is_gem = false, mine = false;

		if (mlen < NL_HDRLEN || off + mlen > rlen)
			break;
		memcpy(&got, rsp + off + NL_HDRLEN, sizeof(got));
		g.ifindex = got.ifi_index;

		while (p + NLA_HDRLEN_ <= end) {
			uint16_t alen = *(const uint16_t *)p;
			uint16_t atype = *(const uint16_t *)(p + 2) & 0x3fff;
			const uint8_t *v = p + NLA_HDRLEN_;

			if (alen < NLA_HDRLEN_ || p + alen > end)
				break;

			if (atype == IFLA_LINK && alen >= NLA_HDRLEN_ + 4) {
				int link;

				memcpy(&link, v, sizeof(link));
				mine = link == lower;
			} else if (atype == IFLA_LINKINFO) {
				is_gem = gem_link_info(v, p + alen, &g);
			}
			p += NL_ALIGN(alen);
		}

		if (is_gem && mine && n < cap)
			out[n++] = g;
		off += NL_ALIGN(mlen);
	}
	return n;
}

/* Remove a GEM interface. */
static int gem_delete(int ifindex)
{
	uint8_t req[NL_HDRLEN + sizeof(struct ifinfomsg)] __aligned(4);
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC,
				 .ifi_index = ifindex };

	memset(req, 0, sizeof(req));
	*(uint32_t *)&req[0] = sizeof(req);
	*(uint16_t *)&req[4] = RTM_DELLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 2;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));

	return rtnl_talk(req, sizeof(req), NULL, 0, NULL);
}

/* Create a GEM interface on @lower, up, with the port's direction and T-CONT. */
static int gem_create(const struct omci_hw_gem_port *g, int lower)
{
	uint8_t req[512] __aligned(4);
	uint8_t data[64] __aligned(4);
	uint8_t info[128] __aligned(4);
	char name[IFNAMSIZ];
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };
	uint32_t tcont = g->tcont;
	size_t dlen = 0, ilen = 0, off;

	snprintf(name, sizeof(name), GEM_NAME_FMT, g->port_id);

	dlen = nla_put(data, dlen, IFLA_GEM_PORT_ID, &g->port_id,
		       sizeof(g->port_id));
	dlen = nla_put(data, dlen, IFLA_GEM_DIRECTION, &g->direction,
		       sizeof(g->direction));
	if (g->direction != OMCI_HW_GEM_DIR_DOWNSTREAM)
		dlen = nla_put(data, dlen, IFLA_GEM_TCONT, &tcont,
			       sizeof(tcont));
	if (g->multicast && g->direction == OMCI_HW_GEM_DIR_DOWNSTREAM)
		dlen = nla_put(data, dlen, IFLA_GEM_MULTICAST, NULL, 0);

	ilen = nla_put(info, ilen, IFLA_INFO_KIND, "gem", sizeof("gem"));
	*(uint16_t *)(info + ilen) = (uint16_t)(NLA_HDRLEN_ + dlen);
	*(uint16_t *)(info + ilen + 2) = IFLA_INFO_DATA | NLA_F_NESTED;
	memcpy(info + ilen + NLA_HDRLEN_, data, dlen);
	ilen += NL_ALIGN(NLA_HDRLEN_ + dlen);

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_NEWLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
			       NLM_F_EXCL;
	*(uint32_t *)&req[8] = 3;

	ifi.ifi_flags = IFF_UP;
	ifi.ifi_change = IFF_UP;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = NL_HDRLEN + sizeof(ifi);

	off = nla_put(req, off, IFLA_LINK, &lower, sizeof(lower));
	off = nla_put(req, off, IFLA_IFNAME, name, strlen(name) + 1);
	*(uint16_t *)(req + off) = (uint16_t)(NLA_HDRLEN_ + ilen);
	*(uint16_t *)(req + off + 2) = IFLA_LINKINFO | NLA_F_NESTED;
	memcpy(req + off + NLA_HDRLEN_, info, ilen);
	off += NL_ALIGN(NLA_HDRLEN_ + ilen);

	*(uint32_t *)&req[0] = (uint32_t)off;
	return rtnl_talk(req, off, NULL, 0, NULL);
}

/*
 * Make the GEM interfaces those of @ports: an interface that matches a port
 * stays, the others go, and the missing ones are made.
 */
static enum omci_status kernel_gem_apply(const struct omci_hw_gem_port *ports,
					 size_t n,
					 struct omci_hw_reject *rejected)
{
	struct gem_present have[OMCI_HW_GEM_MAX_PORTS * 2];
	bool keep[ARRAY_SIZE(have)];
	size_t nhave;
	unsigned int lower;
	int err;

	if (rejected != NULL)
		memset(rejected, 0, sizeof(*rejected));

	if (ports == NULL && n > 0)
		return OMCI_ERR_INVAL;
	if (n > OMCI_HW_GEM_MAX_PORTS)
		return OMCI_ERR_NOMEM;

	lower = if_nametoindex(PON_IFNAME);
	if (!lower) {
		OMCI_WARN("gem: there is no %s interface to stack GEM ports on",
			  PON_IFNAME);
		return OMCI_ERR_HW;
	}

	nhave = gem_present(have, ARRAY_SIZE(have), (int)lower);
	memset(keep, 0, sizeof(keep));

	for (size_t i = 0; i < n; i++) {
		for (size_t j = 0; j < nhave; j++) {
			if (have[j].port_id != ports[i].port_id)
				continue;
			if (have[j].direction == ports[i].direction &&
			    have[j].tcont == ports[i].tcont &&
			    have[j].multicast == ports[i].multicast)
				keep[j] = true;
		}
	}

	for (size_t j = 0; j < nhave; j++) {
		if (keep[j])
			continue;
		err = gem_delete(have[j].ifindex);
		if (err) {
			OMCI_WARN("gem: could not remove Port-ID %u: %s",
				  have[j].port_id, strerror(-err));
			return OMCI_ERR_HW;
		}
	}

	for (size_t i = 0; i < n; i++) {
		bool present = false;

		for (size_t j = 0; j < nhave; j++)
			if (keep[j] && have[j].port_id == ports[i].port_id)
				present = true;
		if (present)
			continue;

		err = gem_create(&ports[i], (int)lower);
		if (err) {
			OMCI_WARN(
				"gem: the kernel refused Port-ID %u (direction %u, T-CONT %u): %s (%s)",
				ports[i].port_id, ports[i].direction,
				ports[i].tcont, strerror(-err),
				omci_hw_last_extack());
			omci_hw_reject_add(rejected, ports[i].port_id);
			continue;
		}
		OMCI_INFO("gem: Port-ID %u up as " GEM_NAME_FMT
			  " (direction %u, T-CONT %u)",
			  ports[i].port_id, ports[i].port_id,
			  ports[i].direction, ports[i].tcont);
	}

	return (rejected != NULL && rejected->n) ? OMCI_DECLINED : OMCI_OK;
}

/* The bridge of the OMCI configuration, and whether it filters VLANs (-1: unknown). */
#define BRIDGE_IFNAME "omcibr0"

static int g_bridge_filtering = -1;

/* Router mode, and whether the log has said what it leaves out. */
static bool g_router;
static bool g_router_told;

void omci_hw_kernel_router(bool on)
{
	g_router = on;
	g_router_told = false;
}

/* In router mode, say once that the OLT's bridging is not applied. */
static bool router_skips(const char *what)
{
	if (!g_router)
		return false;
	if (!g_router_told) {
		OMCI_INFO(
			"router mode: the OLT's %s and the rest of its bridging stay out of the hardware",
			what);
		g_router_told = true;
	}
	return true;
}

/* Put an interface, up, into the bridge @master. */
static int rtnl_link_master(int ifindex, int master)
{
	uint8_t req[NL_HDRLEN + sizeof(struct ifinfomsg) + 32] __aligned(4);
	struct ifinfomsg ifi = {
		.ifi_family = AF_UNSPEC,
		.ifi_index = ifindex,
		.ifi_flags = IFF_UP,
		.ifi_change = IFF_UP,
	};
	size_t off;

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_SETLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 10;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = nla_put(req, NL_HDRLEN + sizeof(ifi), IFLA_MASTER, &master,
		      sizeof(master));
	*(uint32_t *)&req[0] = (uint32_t)off;

	return rtnl_talk(req, off, NULL, 0, NULL);
}

/* The bridge, made when it does not exist, VLAN filtering off. */
static int bridge_ensure(int *ifindex)
{
	uint8_t req[512] __aligned(4);
	uint8_t info[128] __aligned(4);
	uint8_t data[32] __aligned(4);
	struct ifinfomsg ifi = {
		.ifi_family = AF_UNSPEC,
		.ifi_flags = IFF_UP,
		.ifi_change = IFF_UP,
	};
	uint16_t default_pvid = 0;
	size_t dlen = 0, ilen = 0, off;
	int err;

	*ifindex = (int)if_nametoindex(BRIDGE_IFNAME);
	if (*ifindex)
		return 0;

	dlen = nla_put(data, dlen, IFLA_BR_VLAN_DEFAULT_PVID, &default_pvid,
		       sizeof(default_pvid));
	ilen = nla_put(info, ilen, IFLA_INFO_KIND, "bridge", sizeof("bridge"));
	*(uint16_t *)(info + ilen) = (uint16_t)(NLA_HDRLEN_ + dlen);
	*(uint16_t *)(info + ilen + 2) = IFLA_INFO_DATA | NLA_F_NESTED;
	memcpy(info + ilen + NLA_HDRLEN_, data, dlen);
	ilen += NL_ALIGN(NLA_HDRLEN_ + dlen);

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_NEWLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
			       NLM_F_EXCL;
	*(uint32_t *)&req[8] = 11;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = NL_HDRLEN + sizeof(ifi);
	off = nla_put(req, off, IFLA_IFNAME, BRIDGE_IFNAME,
		      sizeof(BRIDGE_IFNAME));
	*(uint16_t *)(req + off) = (uint16_t)(NLA_HDRLEN_ + ilen);
	*(uint16_t *)(req + off + 2) = IFLA_LINKINFO | NLA_F_NESTED;
	memcpy(req + off + NLA_HDRLEN_, info, ilen);
	off += NL_ALIGN(NLA_HDRLEN_ + ilen);
	*(uint32_t *)&req[0] = (uint32_t)off;

	err = rtnl_talk(req, off, NULL, 0, NULL);
	if (err)
		return err;

	g_bridge_filtering = 0;

	*ifindex = (int)if_nametoindex(BRIDGE_IFNAME);
	return *ifindex ? 0 : -ENODEV;
}

/* Turn the bridge's VLAN filtering on or off. */
static int bridge_set_filtering(int ifindex, bool on)
{
	uint8_t req[256] __aligned(4);
	uint8_t info[64] __aligned(4);
	uint8_t data[32] __aligned(4);
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC,
				 .ifi_index = ifindex };
	uint8_t filtering = on ? 1 : 0;
	size_t dlen = 0, ilen = 0, off;

	dlen = nla_put(data, dlen, IFLA_BR_VLAN_FILTERING, &filtering,
		       sizeof(filtering));
	ilen = nla_put(info, ilen, IFLA_INFO_KIND, "bridge", sizeof("bridge"));
	*(uint16_t *)(info + ilen) = (uint16_t)(NLA_HDRLEN_ + dlen);
	*(uint16_t *)(info + ilen + 2) = IFLA_INFO_DATA | NLA_F_NESTED;
	memcpy(info + ilen + NLA_HDRLEN_, data, dlen);
	ilen += NL_ALIGN(NLA_HDRLEN_ + dlen);

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_NEWLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 13;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = NL_HDRLEN + sizeof(ifi);
	*(uint16_t *)(req + off) = (uint16_t)(NLA_HDRLEN_ + ilen);
	*(uint16_t *)(req + off + 2) = IFLA_LINKINFO | NLA_F_NESTED;
	memcpy(req + off + NLA_HDRLEN_, info, ilen);
	off += NL_ALIGN(NLA_HDRLEN_ + ilen);
	*(uint32_t *)&req[0] = (uint32_t)off;

	return rtnl_talk(req, off, NULL, 0, NULL);
}

/* Add a VID to a bridge port, or remove it. */
static int bridge_vlan(int ifindex, uint16_t vid, bool add)
{
	uint8_t req[NL_HDRLEN + sizeof(struct ifinfomsg) + 64] __aligned(4);
	struct ifinfomsg ifi = { .ifi_family = AF_BRIDGE,
				 .ifi_index = ifindex };
	struct bridge_vlan_info vinfo = { .flags = 0, .vid = vid };
	size_t off, spec;

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = add ? RTM_SETLINK : RTM_DELLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 12;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = NL_HDRLEN + sizeof(ifi);

	spec = off;
	off += NLA_HDRLEN_;
	off = nla_put(req, off, IFLA_BRIDGE_VLAN_INFO, &vinfo, sizeof(vinfo));
	*(uint16_t *)(req + spec) = (uint16_t)(off - spec);
	*(uint16_t *)(req + spec + 2) = IFLA_AF_SPEC | NLA_F_NESTED;

	*(uint32_t *)&req[0] = (uint32_t)off;
	return rtnl_talk(req, off, NULL, 0, NULL);
}

/* The interface of a bridge port: sfp for the UNI, gemN for a GEM port. */
static unsigned int bridge_port_ifindex(const struct omci_hw_bridge_port *p)
{
	char name[IFNAMSIZ];

	if (p->port_id == OMCI_HW_BRIDGE_UNI)
		return if_nametoindex(UNI_IFNAME);

	snprintf(name, sizeof(name), GEM_NAME_FMT, p->port_id);
	return if_nametoindex(name);
}

/*
 * Put the ports into the bridge with their VIDs. VLAN filtering is on as
 * soon as one port has a VID list; with none, every frame is forwarded. A
 * GEM port whose interface does not exist yet is left for the next time.
 */
static enum omci_status
kernel_bridge_apply(const struct omci_hw_bridge_port *ports, size_t n,
		    struct omci_hw_reject *rejected)
{
	int br = 0;
	int err;

	if (rejected != NULL)
		memset(rejected, 0, sizeof(*rejected));

	if (ports == NULL && n > 0)
		return OMCI_ERR_INVAL;
	if (n > OMCI_HW_BRIDGE_MAX_PORTS)
		return OMCI_ERR_NOMEM;

	if (!n || router_skips("bridge"))
		return OMCI_OK;

	err = bridge_ensure(&br);
	if (err) {
		OMCI_WARN("bridge: %s could not be created: %s", BRIDGE_IFNAME,
			  strerror(-err));
		return OMCI_ERR_HW;
	}

	{
		bool filters = false;

		for (size_t i = 0; i < n; i++)
			if (ports[i].num_vlan)
				filters = true;

		if (g_bridge_filtering != (int)filters) {
			err = bridge_set_filtering(br, filters);
			if (err)
				OMCI_WARN(
					"bridge: VLAN filtering could not be turned %s: %s",
					filters ? "on" : "off", strerror(-err));
			else {
				g_bridge_filtering = (int)filters;
				OMCI_INFO(
					"bridge: VLAN filtering %s -- %s",
					filters ? "on" : "off",
					filters ?
						"the OLT provisioned VLAN filters" :
						"the OLT provisioned none, so every frame is forwarded");
			}
		}
	}

	for (size_t i = 0; i < n; i++) {
		unsigned int ifindex = bridge_port_ifindex(&ports[i]);

		if (!ifindex) {
			OMCI_DEBUG("bridge: Port-ID %u has no interface yet",
				   ports[i].port_id);
			continue;
		}

		err = rtnl_link_master((int)ifindex, br);
		if (err && err != -EBUSY) {
			OMCI_WARN("bridge: the kernel refused Port-ID %u: %s",
				  ports[i].port_id, strerror(-err));
			omci_hw_reject_add(rejected, ports[i].port_id);
			continue;
		}

		for (uint8_t v = 0; v < ports[i].num_vlan; v++) {
			err = bridge_vlan((int)ifindex, ports[i].vlan[v], true);
			if (err) {
				OMCI_WARN(
					"bridge: VLAN %u on Port-ID %u: %s (%s)",
					ports[i].vlan[v], ports[i].port_id,
					strerror(-err), omci_hw_last_extack());

				omci_hw_reject_add(rejected, ports[i].port_id);
				continue;
			}
			OMCI_INFO("bridge: Port-ID %u carries VLAN %u",
				  ports[i].port_id, ports[i].vlan[v]);
		}
	}

	return (rejected != NULL && rejected->n) ? OMCI_DECLINED : OMCI_OK;
}

/*
 * The alarms of the PON device. The driver reports loss of signal, of frame
 * and of the message channel; signal fail and degrade it does not report.
 */
static enum omci_status kernel_alarms(uint32_t *mask)
{
	struct pon_dev_attrs a;
	uint8_t buf[4096] __aligned(4);
	uint32_t alarms;
	int err;

	if (mask == NULL)
		return OMCI_ERR_INVAL;
	*mask = 0;

	err = pon_dev_get(&a, buf, sizeof(buf));
	if (err)
		return OMCI_ERR_HW;
	if (!a.val[PON_A_DEV_ALARMS])
		return OMCI_ERR_HW;

	alarms = pon_attr_u32(&a, PON_A_DEV_ALARMS, 0);
	if (alarms & PON_ALARM_LOS)
		*mask |= 1u << OMCI_HW_ALARM_LOS;
	if (alarms & PON_ALARM_LOF)
		*mask |= 1u << OMCI_HW_ALARM_LOF;
	if (alarms & PON_ALARM_LOM)
		*mask |= 1u << OMCI_HW_ALARM_LOM;

	return OMCI_OK;
}

/* The counters of struct omci_hw_ops, by the names ethtool gives them. */
static const struct {
	unsigned int idx;
	const char *name;
} COUNTERS[] = {
	{ 0, "rx_octets" },
	{ 1, "rx_unicast" },
	{ 2, "rx_multicast" },
	{ 3, "rx_broadcast" },
	{ 5, "tx_octets" },
	{ 6, "tx_discards" },
	{ 7, "tx_unicast" },
	{ 8, "tx_multicast" },
	{ 9, "tx_broadcast" },
	{ 11, "rx_forward_discards" },
	{ 13, "rx_pause" },
	{ 14, "tx_pause" },
	{ 18, "tx_single_collisions" },
	{ 19, "tx_multiple_collisions" },
	{ 20, "tx_deferred" },
	{ 21, "tx_late_collisions" },
	{ 22, "tx_excessive_collisions" },
	{ 24, "rx_symbol_errors" },
	{ 25, "rx_unknown_opcodes" },
	{ 26, "rx_drops" },
	{ 32, "rx_fragments" },
	{ 33, "rx_jabbers" },
	{ 34, "tx_collisions" },
	{ 35, "rx_crc_errors" },
	{ 43, "tx_undersize" },
	{ 44, "tx_oversize" },
	{ 45, "tx_64_octets" },
	{ 46, "tx_65_127_octets" },
	{ 47, "tx_128_255_octets" },
	{ 48, "tx_256_511_octets" },
	{ 49, "tx_512_1023_octets" },
	{ 50, "tx_1024_1518_octets" },
	{ 51, "tx_1519_max_octets" },
	{ 52, "tx_ether_broadcast" },
	{ 53, "tx_ether_multicast" },
	{ 57, "rx_undersize" },
	{ 59, "rx_oversize" },
	{ 60, "rx_64_octets" },
	{ 61, "rx_65_127_octets" },
	{ 62, "rx_128_255_octets" },
	{ 63, "rx_256_511_octets" },
	{ 64, "rx_512_1023_octets" },
	{ 65, "rx_1024_1518_octets" },
	{ 66, "rx_1519_max_octets" },

};

/* The statistics of the last interface read: names and values. */
struct counter_cache {
	unsigned int port;
	bool valid;
	unsigned int n;
	struct ethtool_gstrings *strings;
	struct ethtool_stats *stats;
};

static struct counter_cache g_ctr;

/* The interface of a switch port. */
static const char *port_ifname(unsigned int port)
{
	switch (port) {
	case UNI_PORT:
		return UNI_IFNAME;
	case PON_PORT:
		return PON_IFNAME;
	default:
		return NULL;
	}
}

/* Run one ethtool command on an interface. */
static int ethtool_call(const char *ifname, void *cmd)
{
	struct ifreq ifr;
	int fd, err = 0;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	ifr.ifr_data = cmd;
	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0)
		err = -errno;
	close(fd);
	return err;
}

static void counter_cache_drop(void)
{
	free(g_ctr.strings);
	free(g_ctr.stats);
	g_ctr.strings = NULL;
	g_ctr.stats = NULL;
	g_ctr.valid = false;
}

/* Read every statistic of a port's interface, names and values. */
static int counter_cache_load(unsigned int port)
{
	const char *ifname = port_ifname(port);
	struct ethtool_sset_info info_hdr;
	struct {
		struct ethtool_sset_info hdr;
		uint32_t count;
	} info;
	unsigned int n;
	int err;

	if (!ifname)
		return -EINVAL;

	counter_cache_drop();

	memset(&info, 0, sizeof(info));
	info.hdr.cmd = ETHTOOL_GSSET_INFO;
	info.hdr.sset_mask = 1ull << ETH_SS_STATS;
	err = ethtool_call(ifname, &info);
	if (err)
		return err;
	(void)info_hdr;
	n = info.count;
	if (!n)
		return -ENODATA;

	g_ctr.strings =
		calloc(1, sizeof(*g_ctr.strings) + (size_t)n * ETH_GSTRING_LEN);
	g_ctr.stats =
		calloc(1, sizeof(*g_ctr.stats) + (size_t)n * sizeof(uint64_t));
	if (!g_ctr.strings || !g_ctr.stats) {
		counter_cache_drop();
		return -ENOMEM;
	}

	g_ctr.strings->cmd = ETHTOOL_GSTRINGS;
	g_ctr.strings->string_set = ETH_SS_STATS;
	g_ctr.strings->len = n;
	err = ethtool_call(ifname, g_ctr.strings);
	if (err) {
		counter_cache_drop();
		return err;
	}

	g_ctr.stats->cmd = ETHTOOL_GSTATS;
	g_ctr.stats->n_stats = n;
	err = ethtool_call(ifname, g_ctr.stats);
	if (err) {
		counter_cache_drop();
		return err;
	}

	g_ctr.port = port;
	g_ctr.n = n;
	g_ctr.valid = true;
	return 0;
}

/* Read one counter: every call reads the statistics afresh. */
static enum omci_status kernel_counter(unsigned int port, unsigned int idx,
				       uint64_t *out)
{
	const char *name = NULL;
	unsigned int i;

	if (out == NULL)
		return OMCI_ERR_INVAL;
	*out = 0;

	for (i = 0; i < ARRAY_SIZE(COUNTERS); i++)
		if (COUNTERS[i].idx == idx)
			name = COUNTERS[i].name;
	if (!name)
		return OMCI_ERR_INVAL;

	if (counter_cache_load(port))
		return OMCI_ERR_HW;

	for (i = 0; i < g_ctr.n; i++) {
		const char *s = (const char *)g_ctr.strings->data +
				(size_t)i * ETH_GSTRING_LEN;

		if (strncmp(s, name, ETH_GSTRING_LEN))
			continue;
		*out = g_ctr.stats->data[i];
		return OMCI_OK;
	}
	return OMCI_ERR_HW;
}

/* The hwmon device of the optics. */
#define HWMON_DIR "/sys/class/hwmon"
#define HWMON_NAME "rtl9602c_optics"

/* Find the optics among the hwmon devices, once. */
static bool hwmon_path(char *out, size_t cap)
{
	static char found[128];
	static bool looked;
	unsigned int i;

	if (looked) {
		if (!found[0])
			return false;
		snprintf(out, cap, "%s", found);
		return true;
	}
	looked = true;

	for (i = 0; i < 16; i++) {
		char path[160];
		char name[64] = "";
		FILE *f;

		snprintf(path, sizeof(path), "%s/hwmon%u/name", HWMON_DIR, i);
		f = fopen(path, "re");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = '\0';
		fclose(f);
		if (strcmp(name, HWMON_NAME))
			continue;
		snprintf(found, sizeof(found), "%s/hwmon%u", HWMON_DIR, i);
		snprintf(out, cap, "%s", found);
		return true;
	}
	return false;
}

/* Read one value of a hwmon file. */
static bool hwmon_value(const char *dir, const char *file, long long *out)
{
	char path[224];
	char t[64];
	FILE *f;
	bool ok = false;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "re");
	if (!f)
		return false;
	if (fgets(t, sizeof(t), f)) {
		char *end;

		*out = strtoll(t, &end, 10);
		ok = end != t;
	}
	fclose(f);
	return ok;
}

/*
 * The readings of the optics, in hwmon units (millidegrees, millivolts,
 * milliamperes, microwatts), turned into those of SFF-8472.
 */
static enum omci_status kernel_optics(struct omci_hw_optics *out)
{
	char dir[160];
	long long v;
	unsigned int got = 0;

	if (out == NULL)
		return OMCI_ERR_INVAL;
	memset(out, 0, sizeof(*out));

	if (!hwmon_path(dir, sizeof(dir)))
		return OMCI_ERR_HW;

	if (hwmon_value(dir, "temp1_input", &v)) {
		out->temperature =
			(uint16_t)(int16_t)((v * 256 + (v < 0 ? -500 : 500)) /
					    1000);
		out->valid |= 1u << OMCI_HW_OPTIC_TEMPERATURE;
		got++;
	}

	if (hwmon_value(dir, "in0_input", &v)) {
		out->voltage = (uint16_t)(v * 10);
		out->valid |= 1u << OMCI_HW_OPTIC_VOLTAGE;
		got++;
	}

	if (hwmon_value(dir, "curr1_input", &v)) {
		out->bias = (uint16_t)(v * 500);
		out->valid |= 1u << OMCI_HW_OPTIC_BIAS;
		got++;
	}

	if (hwmon_value(dir, "power1_input", &v)) {
		out->tx_power = (uint16_t)(v * 10);
		out->valid |= 1u << OMCI_HW_OPTIC_TX_POWER;
		got++;
	}
	if (hwmon_value(dir, "power2_input", &v)) {
		out->rx_power = (uint16_t)(v * 10);
		out->valid |= 1u << OMCI_HW_OPTIC_RX_POWER;
		got++;
	}

	return got ? OMCI_OK : OMCI_ERR_HW;
}

/* The priorities of the VLAN filters, one per rule. */
#define TC_PRIO_BASE 1000u
#define TC_PRIO_LAST (TC_PRIO_BASE + OMCI_HW_VLAN_MAX_RULES)

/* The info word of a filter: its priority and protocol. */
static uint32_t tc_info(unsigned int prio, uint16_t proto)
{
	return (uint32_t)prio << 16 | htons(proto);
}

/* Give an interface the clsact qdisc, which filters hang from. */
static int tc_clsact_ensure(int ifindex)
{
	uint8_t req[NL_HDRLEN + sizeof(struct tcmsg) + 32] __aligned(4);
	struct tcmsg tcm = {
		.tcm_family = AF_UNSPEC,
		.tcm_ifindex = ifindex,
		.tcm_parent = TC_H_CLSACT,
		.tcm_handle = TC_H_MAKE(TC_H_CLSACT, 0),
	};
	size_t off;
	int err;

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_NEWQDISC;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
			       NLM_F_EXCL;
	*(uint32_t *)&req[8] = 20;
	memcpy(req + NL_HDRLEN, &tcm, sizeof(tcm));
	off = nla_put(req, NL_HDRLEN + sizeof(tcm), TCA_KIND, "clsact",
		      sizeof("clsact"));
	*(uint32_t *)&req[0] = (uint32_t)off;

	err = rtnl_talk(req, off, NULL, 0, NULL);
	return err == -EEXIST ? 0 : err;
}

/* Remove the ingress filter of a priority, whatever its protocol. */
static int tc_filter_del(int ifindex, unsigned int prio)
{
	uint8_t req[NL_HDRLEN + sizeof(struct tcmsg)] __aligned(4);
	struct tcmsg tcm = {
		.tcm_family = AF_UNSPEC,
		.tcm_ifindex = ifindex,
		.tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS),
		.tcm_info = tc_info(prio, 0),
	};
	int err;

	memset(req, 0, sizeof(req));
	*(uint32_t *)&req[0] = sizeof(req);
	*(uint16_t *)&req[4] = RTM_DELTFILTER;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 21;
	memcpy(req + NL_HDRLEN, &tcm, sizeof(tcm));

	err = rtnl_talk(req, sizeof(req), NULL, 0, NULL);
	return err == -ENOENT ? 0 : err;
}

/*
 * A VLAN filter: the frames it takes, untagged ones or those with an outer
 * C-tag of @match_vid, and what its vlan action does to them: TCA_VLAN_ACT_
 * MODIFY or PUSH writes a C-tag of @vid with priority @pri, or keeps the
 * frame's priority for OMCI_HW_VLAN_PRI_KEEP; POP takes the C-tag off.
 */
struct tc_vlan_filter {
	bool untagged;
	uint16_t match_vid;
	int v_action;
	uint16_t vid;
	uint8_t pri;
};

/* Add an ingress filter, in the switch only (skip_sw). */
static int tc_filter_add(int ifindex, unsigned int prio,
			 const struct tc_vlan_filter *f)
{
	uint8_t req[1024] __aligned(4);
	struct tcmsg tcm = {
		.tcm_family = AF_UNSPEC,
		.tcm_ifindex = ifindex,
		.tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS),
		.tcm_info =
			tc_info(prio, f->untagged ? ETH_P_ALL : ETH_P_8021Q),
	};
	struct tc_vlan parms = {
		.action = TC_ACT_PIPE,
		.v_action = f->v_action,
	};
	uint32_t flags = TCA_CLS_FLAGS_SKIP_SW;
	size_t off, opts, acts, act1, aopts;

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_NEWTFILTER;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
			       NLM_F_EXCL;
	*(uint32_t *)&req[8] = 22;
	memcpy(req + NL_HDRLEN, &tcm, sizeof(tcm));
	off = NL_HDRLEN + sizeof(tcm);

	off = nla_put(req, off, TCA_KIND, "flower", sizeof("flower"));

	opts = off;
	off += NLA_HDRLEN_;

	if (f->untagged) {
		uint8_t none = 0;

		off = nla_put(req, off, TCA_FLOWER_KEY_NUM_OF_VLANS, &none,
			      sizeof(none));
	} else {
		uint16_t proto = htons(ETH_P_8021Q);

		off = nla_put(req, off, TCA_FLOWER_KEY_ETH_TYPE, &proto,
			      sizeof(proto));
		off = nla_put(req, off, TCA_FLOWER_KEY_VLAN_ID, &f->match_vid,
			      sizeof(f->match_vid));
	}
	off = nla_put(req, off, TCA_FLOWER_FLAGS, &flags, sizeof(flags));

	acts = off;
	off += NLA_HDRLEN_;
	act1 = off;
	off += NLA_HDRLEN_;
	off = nla_put(req, off, TCA_ACT_KIND, "vlan", sizeof("vlan"));
	aopts = off;
	off += NLA_HDRLEN_;
	off = nla_put(req, off, TCA_VLAN_PARMS, &parms, sizeof(parms));
	if (f->v_action != TCA_VLAN_ACT_POP) {
		off = nla_put(req, off, TCA_VLAN_PUSH_VLAN_ID, &f->vid,
			      sizeof(f->vid));
		if (f->pri != OMCI_HW_VLAN_PRI_KEEP)
			off = nla_put(req, off, TCA_VLAN_PUSH_VLAN_PRIORITY,
				      &f->pri, sizeof(f->pri));
	}
	*(uint16_t *)(req + aopts) = (uint16_t)(off - aopts);
	*(uint16_t *)(req + aopts + 2) = TCA_ACT_OPTIONS | NLA_F_NESTED;
	*(uint16_t *)(req + act1) = (uint16_t)(off - act1);
	*(uint16_t *)(req + act1 + 2) = 1 | NLA_F_NESTED;
	*(uint16_t *)(req + acts) = (uint16_t)(off - acts);
	*(uint16_t *)(req + acts + 2) = TCA_FLOWER_ACT | NLA_F_NESTED;

	*(uint16_t *)(req + opts) = (uint16_t)(off - opts);
	*(uint16_t *)(req + opts + 2) = TCA_OPTIONS | NLA_F_NESTED;

	*(uint32_t *)&req[0] = (uint32_t)off;
	return rtnl_talk(req, off, NULL, 0, NULL);
}

/*
 * The two filters of a rule: the upstream one on sfp, and on pon the
 * downstream one that undoes it. A translation is undone by the translation
 * back, keeping the priority; a C-tag pushed onto an untagged frame by
 * popping it. An S-VID push is refused: the switch adds an S-tag but cannot
 * take one off a downstream frame, which it does not see as tagged.
 */
static bool vlan_filters(const struct omci_hw_vlan_xlate *r,
			 struct tc_vlan_filter *up, struct tc_vlan_filter *down)
{
	switch (r->kind) {
	case OMCI_HW_VLAN_CVID_XLATE:
		*up = (struct tc_vlan_filter){
			.match_vid = r->match_cvid,
			.v_action = TCA_VLAN_ACT_MODIFY,
			.vid = r->assign_cvid,
			.pri = r->assign_pri,
		};
		*down = (struct tc_vlan_filter){
			.match_vid = r->assign_cvid,
			.v_action = TCA_VLAN_ACT_MODIFY,
			.vid = r->match_cvid,
			.pri = OMCI_HW_VLAN_PRI_KEEP,
		};
		return true;
	case OMCI_HW_VLAN_CVID_PUSH:
		*up = (struct tc_vlan_filter){
			.untagged = true,
			.v_action = TCA_VLAN_ACT_PUSH,
			.vid = r->assign_cvid,
			.pri = r->assign_pri,
		};
		*down = (struct tc_vlan_filter){
			.match_vid = r->assign_cvid,
			.v_action = TCA_VLAN_ACT_POP,
		};
		return true;
	default:
		return false;
	}
}

/* Replace the VLAN rules, two filters each (see vlan_filters()). */
static enum omci_status
kernel_vlan_apply(const struct omci_hw_vlan_xlate *rules, size_t n,
		  struct omci_hw_reject *rejected)
{
	unsigned int uni = if_nametoindex(UNI_IFNAME);
	unsigned int pon = if_nametoindex(PON_IFNAME);
	int err;

	if (rejected != NULL)
		memset(rejected, 0, sizeof(*rejected));

	if (rules == NULL && n > 0)
		return OMCI_ERR_INVAL;
	if (router_skips("VLAN rules"))
		return OMCI_OK;
	if (n > OMCI_HW_VLAN_MAX_RULES)
		return OMCI_ERR_NOMEM;
	if (!uni || !pon) {
		OMCI_WARN(
			"vlan: %s or %s is missing; no VLAN rule is programmed",
			UNI_IFNAME, PON_IFNAME);
		return OMCI_ERR_HW;
	}

	if (tc_clsact_ensure((int)uni) || tc_clsact_ensure((int)pon)) {
		OMCI_WARN("vlan: no ingress hook on %s or %s", UNI_IFNAME,
			  PON_IFNAME);
		return OMCI_ERR_HW;
	}

	for (unsigned int prio = TC_PRIO_BASE; prio < TC_PRIO_LAST; prio++) {
		(void)tc_filter_del((int)uni, prio);
		(void)tc_filter_del((int)pon, prio);
	}

	for (size_t i = 0; i < n; i++) {
		const unsigned int prio = TC_PRIO_BASE + (unsigned int)i;
		const struct omci_hw_vlan_xlate *r = &rules[i];
		struct tc_vlan_filter up, down;

		if (!vlan_filters(r, &up, &down)) {
			OMCI_WARN(
				"vlan: rule %zu (C-VID %u) adds an S-tag, which the switch cannot take off downstream frames; not programmed",
				i, r->match_cvid);
			omci_hw_reject_add(rejected, r->match_cvid);
			continue;
		}

		err = tc_filter_add((int)uni, prio, &up);
		if (err) {
			OMCI_WARN("vlan: upstream rule %zu refused: %s (%s)", i,
				  strerror(-err), omci_hw_last_extack());
			omci_hw_reject_add(rejected, r->match_cvid);
			continue;
		}
		err = tc_filter_add((int)pon, prio, &down);
		if (err) {
			OMCI_WARN("vlan: downstream rule %zu refused: %s (%s)",
				  i, strerror(-err), omci_hw_last_extack());
			(void)tc_filter_del((int)uni, prio);
			omci_hw_reject_add(rejected, r->match_cvid);
			continue;
		}
		if (r->kind == OMCI_HW_VLAN_CVID_PUSH)
			OMCI_INFO("vlan: untagged <-> C-VID %u in hardware",
				  r->assign_cvid);
		else
			OMCI_INFO(
				"vlan: C-VID %u <-> %u translated in hardware",
				r->match_cvid, r->assign_cvid);
	}

	return (rejected != NULL && rejected->n) ? OMCI_DECLINED : OMCI_OK;
}

/* The kernel backend. */
const struct omci_hw_ops omci_hw_kernel_ops = {
	.caps = kernel_caps,
	.source = kernel_source,
	.vlan_apply = kernel_vlan_apply,
	.gem_apply = kernel_gem_apply,
	.bridge_apply = kernel_bridge_apply,
	.alarms = kernel_alarms,
	.counter = kernel_counter,
	.optics = kernel_optics,
	.set_router = omci_hw_kernel_router,
};
