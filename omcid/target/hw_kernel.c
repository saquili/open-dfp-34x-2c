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
#include <linux/filter.h>
#include <linux/gen_stats.h>
#include <linux/genetlink.h>
#include <linux/if_packet.h>
#include <arpa/inet.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_gact.h>
#include <linux/tc_act/tc_vlan.h>
#include <linux/netlink.h>
#include <linux/pon.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>

#include "hw/hw.h"
#include "hw_kernel.h"
#include "omcid/array_size.h"
#include "omcid/attrs.h"
#include "omcid/ctl_proto.h"
#include "sys/log.h"
#include "tcprio.h"

/*
 * The identity of the ONU, a copy of the unit's own data that onu-apply makes
 * for omcid before it starts, without the PLOAM password, which only the PON
 * driver needs: one "key=value" per line, with the keys vendor_id, version, equipment_id,
 * tm_option, omcc_version, vendor_product_code and serial (four letters
 * and eight hexadecimal digits), and optionally sw_version (or one per
 * image, sw_version0 and sw_version1) and pack_version, the versions the
 * software images and the circuit packs report, and loid and
 * loid_password, the logical ONU ID and password.
 */
#define IDENTITY_PATH OMCI_RUN_DIR "/identity"

/*
 * The flags of the filters omcid adds: in the switch alone. The kernel test
 * (make check-kernel) builds with 0, so that its dummy interfaces, which no
 * switch is behind, take them in software.
 */
#ifndef OMCID_TC_FLAGS
#define OMCID_TC_FLAGS TCA_CLS_FLAGS_SKIP_SW
#endif

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
 * queue per IEEE 802.1p priority on each T-CONT unless the driver says
 * otherwise, and OMCC version 0xA3: ITU-T G.988, baseline message set only.
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

/* Read a value of the identity file as a number: decimal, or 0x and hexadecimal. */
static bool identity_number(const char *key, unsigned long *out)
{
	char t[32];
	char *end;

	if (!identity_copy(key, t, sizeof(t)))
		return false;
	*out = strtoul(t, &end, 0);
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
 * The upstream queues of each T-CONT, from the driver, which the priority
 * queues the ONU makes for each T-CONT follow; the board default without it.
 */
static void caps_tcont_queues(struct omci_hw_caps *caps)
{
	struct pon_dev_attrs a;
	uint8_t buf[4096] __aligned(4);
	uint32_t n;

	if (pon_dev_get(&a, buf, sizeof(buf)) != 0 ||
	    !a.val[PON_A_DEV_NUM_TCONT_QUEUES])
		return;

	n = pon_attr_u32(&a, PON_A_DEV_NUM_TCONT_QUEUES, 0);
	if (!n || n > UINT8_MAX)
		return;
	if (n != caps->num_priq_per_tcont)
		OMCI_INFO(
			"caps: the driver reports %u queue(s) per T-CONT, the board default is %u",
			n, caps->num_priq_per_tcont);
	caps->num_priq_per_tcont = (uint8_t)n;
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

/*
 * The version of the software images. The identity file may name one
 * (sw_version), which both images then report, both valid: so a unit shows
 * an OLT what the firmware it came with showed; or one for each image
 * (sw_version0, sw_version1), which take precedence. Otherwise it is the
 * release of the running kernel, and only the committed image is valid.
 */
static void identity_software(struct omci_hw_caps *caps)
{
	char version[sizeof(caps->identity.sw_version[0])] = "";
	char own[2][sizeof(caps->identity.sw_version[0])] = { "", "" };
	bool each = identity_copy("sw_version0", own[0], sizeof(own[0])) |
		    identity_copy("sw_version1", own[1], sizeof(own[1]));

	caps->sw_valid_images = (uint8_t)(1u << caps->sw_committed_image);
	if (identity_copy("sw_version", version, sizeof(version)) || each) {
		caps->sw_valid_images = 3u;
	} else {
		FILE *f = fopen("/proc/sys/kernel/osrelease", "re");

		if (f) {
			if (fgets(version, sizeof(version), f))
				version[strcspn(version, "\n")] = '\0';
			fclose(f);
		}
	}

	for (unsigned int i = 0; i < 2; i++) {
		const char *v = own[i][0] ? own[i] : version;

		if (v[0])
			snprintf(caps->identity.sw_version[i],
				 sizeof(caps->identity.sw_version[i]), "%s", v);
	}
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
	caps_tcont_queues(out);
	caps_gem_ports(out);
	ip_host_mac(out);

	ident |= identity_copy("vendor_id", out->identity.vendor_id,
			       sizeof(out->identity.vendor_id));
	ident |= identity_copy("version", out->identity.version,
			       sizeof(out->identity.version));
	ident |= identity_copy("equipment_id", out->identity.equipment_id,
			       sizeof(out->identity.equipment_id));
	(void)identity_copy("pack_version", out->identity.pack_version,
			    sizeof(out->identity.pack_version));
	(void)identity_copy("loid", out->identity.loid,
			    sizeof(out->identity.loid));
	(void)identity_copy("loid_password", out->identity.loid_password,
			    sizeof(out->identity.loid_password));
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
	uint8_t pbits;
	uint8_t queue;
	uint8_t weight;
	uint32_t cir;
	uint32_t pir;
	uint32_t ds_rate;
	uint32_t ds_burst;
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

/* An unsigned attribute of four or eight bytes, as NLA_UINT sends it. */
static uint32_t nla_uint32(const uint8_t *v, uint16_t len)
{
	uint64_t v64 = 0;
	uint32_t v32 = 0;

	if (len == 8) {
		memcpy(&v64, v, sizeof(v64));
		return v64 > UINT32_MAX ? UINT32_MAX : (uint32_t)v64;
	}
	if (len == 4)
		memcpy(&v32, v, sizeof(v32));
	return v32;
}

/*
 * The attributes of a GEM link: its port identifier, direction and T-CONT,
 * the upstream attributes, which the kernel reports for a port that carries
 * traffic upstream, and the downstream limit, which it reports for a port that
 * carries traffic downstream.
 */
static void gem_info_data(const uint8_t *d, const uint8_t *end,
			  struct gem_present *g)
{
	g->pbits = 0xffu;
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
		if (dtype == IFLA_GEM_PBITS && dlen >= NLA_HDRLEN_ + 1)
			g->pbits = dv[0];
		if (dtype == IFLA_GEM_QUEUE && dlen >= NLA_HDRLEN_ + 1)
			g->queue = dv[0];
		if (dtype == IFLA_GEM_WEIGHT && dlen >= NLA_HDRLEN_ + 1)
			g->weight = dv[0];
		if (dtype == IFLA_GEM_CIR)
			g->cir = nla_uint32(dv, (uint16_t)(dlen - NLA_HDRLEN_));
		if (dtype == IFLA_GEM_PIR)
			g->pir = nla_uint32(dv, (uint16_t)(dlen - NLA_HDRLEN_));
		if (dtype == IFLA_GEM_DS_RATE)
			g->ds_rate =
				nla_uint32(dv, (uint16_t)(dlen - NLA_HDRLEN_));
		if (dtype == IFLA_GEM_DS_BURST)
			g->ds_burst =
				nla_uint32(dv, (uint16_t)(dlen - NLA_HDRLEN_));
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

/* Remove an interface: a GEM interface, or the bridge. */
static int link_delete(int ifindex)
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

/* The upstream attributes of port @g into @data at @dlen. Return: the new length. */
static size_t gem_upstream_put(uint8_t *data, size_t dlen,
			       const struct omci_hw_gem_port *g)
{
	dlen = nla_put(data, dlen, IFLA_GEM_PBITS, &g->pbits, sizeof(g->pbits));
	dlen = nla_put(data, dlen, IFLA_GEM_QUEUE, &g->queue, sizeof(g->queue));
	dlen = nla_put(data, dlen, IFLA_GEM_WEIGHT, &g->weight,
		       sizeof(g->weight));
	dlen = nla_put(data, dlen, IFLA_GEM_CIR, &g->cir, sizeof(g->cir));
	return nla_put(data, dlen, IFLA_GEM_PIR, &g->pir, sizeof(g->pir));
}

/* The downstream limit of port @g into @data at @dlen. Return: the new length. */
static size_t gem_downstream_put(uint8_t *data, size_t dlen,
				 const struct omci_hw_gem_port *g)
{
	dlen = nla_put(data, dlen, IFLA_GEM_DS_RATE, &g->ds_rate,
		       sizeof(g->ds_rate));
	return nla_put(data, dlen, IFLA_GEM_DS_BURST, &g->ds_burst,
		       sizeof(g->ds_burst));
}

/* Whether link @have already has port @g's upstream attributes. */
static bool gem_upstream_same(const struct gem_present *have,
			      const struct omci_hw_gem_port *g)
{
	if (g->direction == OMCI_HW_GEM_DIR_DOWNSTREAM)
		return true;
	return have->pbits == g->pbits && have->queue == g->queue &&
	       have->weight == g->weight && have->cir == g->cir &&
	       have->pir == g->pir;
}

/* Whether link @have already has port @g's downstream limit. */
static bool gem_downstream_same(const struct gem_present *have,
				const struct omci_hw_gem_port *g)
{
	if (g->direction == OMCI_HW_GEM_DIR_UPSTREAM)
		return true;
	return have->ds_rate == g->ds_rate && have->ds_burst == g->ds_burst;
}

/* The link information of kind "gem" with @data, into @info. Return: its length. */
static size_t gem_link_info_put(uint8_t *info, const uint8_t *data, size_t dlen)
{
	size_t ilen = nla_put(info, 0, IFLA_INFO_KIND, "gem", sizeof("gem"));

	*(uint16_t *)(info + ilen) = (uint16_t)(NLA_HDRLEN_ + dlen);
	*(uint16_t *)(info + ilen + 2) = IFLA_INFO_DATA | NLA_F_NESTED;
	memcpy(info + ilen + NLA_HDRLEN_, data, dlen);
	return ilen + NL_ALIGN(NLA_HDRLEN_ + dlen);
}

/*
 * Give GEM interface @ifindex port @g's upstream attributes and downstream
 * limit, through the link's changelink, which changes them without taking the
 * port down.
 */
static int gem_change(int ifindex, const struct omci_hw_gem_port *g)
{
	uint8_t req[256] __aligned(4);
	uint8_t data[96] __aligned(4);
	uint8_t info[160] __aligned(4);
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC,
				 .ifi_index = ifindex };
	size_t dlen = 0, ilen, off;

	if (g->direction != OMCI_HW_GEM_DIR_DOWNSTREAM)
		dlen = gem_upstream_put(data, dlen, g);
	if (g->direction != OMCI_HW_GEM_DIR_UPSTREAM)
		dlen = gem_downstream_put(data, dlen, g);
	ilen = gem_link_info_put(info, data, dlen);

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_NEWLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 4;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = NL_HDRLEN + sizeof(ifi);
	*(uint16_t *)(req + off) = (uint16_t)(NLA_HDRLEN_ + ilen);
	*(uint16_t *)(req + off + 2) = IFLA_LINKINFO | NLA_F_NESTED;
	memcpy(req + off + NLA_HDRLEN_, info, ilen);
	off += NL_ALIGN(NLA_HDRLEN_ + ilen);

	*(uint32_t *)&req[0] = (uint32_t)off;
	return rtnl_talk(req, off, NULL, 0, NULL);
}

/*
 * Create a GEM interface on @lower, up, with the port's direction and T-CONT,
 * the upstream attributes of a port that carries traffic upstream, and the
 * downstream limit of one that has it.
 */
static int gem_create(const struct omci_hw_gem_port *g, int lower)
{
	uint8_t req[512] __aligned(4);
	uint8_t data[96] __aligned(4);
	uint8_t info[160] __aligned(4);
	char name[IFNAMSIZ];
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };
	uint32_t tcont = g->tcont;
	size_t dlen = 0, ilen = 0, off;

	snprintf(name, sizeof(name), GEM_NAME_FMT, g->port_id);

	dlen = nla_put(data, dlen, IFLA_GEM_PORT_ID, &g->port_id,
		       sizeof(g->port_id));
	dlen = nla_put(data, dlen, IFLA_GEM_DIRECTION, &g->direction,
		       sizeof(g->direction));
	if (g->direction != OMCI_HW_GEM_DIR_DOWNSTREAM) {
		dlen = nla_put(data, dlen, IFLA_GEM_TCONT, &tcont,
			       sizeof(tcont));
		dlen = gem_upstream_put(data, dlen, g);
	}
	if (g->direction != OMCI_HW_GEM_DIR_UPSTREAM && g->ds_rate != 0)
		dlen = gem_downstream_put(data, dlen, g);
	if (g->multicast && g->direction == OMCI_HW_GEM_DIR_DOWNSTREAM)
		dlen = nla_put(data, dlen, IFLA_GEM_MULTICAST, NULL, 0);

	ilen = gem_link_info_put(info, data, dlen);

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

/* The program told of each change published (omci_hw_kernel_notify()). */
static const char *g_notify;

void omci_hw_kernel_notify(const char *program)
{
	struct sigaction sa = { .sa_handler = SIG_IGN,
				.sa_flags = SA_NOCLDWAIT };

	g_notify = program;
	if (program == NULL)
		return;
	/* The daemon does not wait for its children, and none stays a zombie. */
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGCHLD, &sa, NULL);
}

/*
 * Tell g_notify of a change: run it with the argument "omcid", without
 * waiting for it. It gets the daemon's standard streams and nothing else of
 * it, and the default dispositions of the signals the daemon ignores, which
 * a program would keep.
 */
static void notify_run(void)
{
	pid_t pid;

	if (g_notify == NULL)
		return;
	pid = fork();
	if (pid == 0) {
		long max = sysconf(_SC_OPEN_MAX);

		(void)signal(SIGCHLD, SIG_DFL);
		(void)signal(SIGPIPE, SIG_DFL);
		if (max <= 0 || max > 4096)
			max = 1024;
		for (int fd = 3; fd < max; fd++)
			(void)close(fd);
		execl(g_notify, g_notify, "omcid", (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		OMCI_WARN("%s was not told of a change: %s", g_notify,
			  strerror(errno));
}

/*
 * Make the GEM interfaces those of @ports: an interface that matches a port
 * stays, given the port's upstream attributes where they differ; the others
 * go, and the missing ones are made. An interface the kernel will not give
 * new upstream attributes keeps its old ones, and its port counts as
 * rejected.
 */
static enum omci_status kernel_gem_apply(const struct omci_hw_gem_port *ports,
					 size_t n,
					 struct omci_hw_reject *rejected)
{
	struct gem_present have[OMCI_HW_GEM_MAX_PORTS * 2];
	bool keep[ARRAY_SIZE(have)];
	bool changed = false;
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
			if (have[j].direction != ports[i].direction ||
			    have[j].tcont != ports[i].tcont ||
			    have[j].multicast != ports[i].multicast)
				continue;
			keep[j] = true;
			if (gem_upstream_same(&have[j], &ports[i]) &&
			    gem_downstream_same(&have[j], &ports[i]))
				continue;
			err = gem_change(have[j].ifindex, &ports[i]);
			if (err) {
				OMCI_WARN(
					"gem: the kernel refused Port-ID %u priorities %#04x, queue %u, weight %u, rates %u/%u, downstream limit %u bytes/s, burst %u: %s (%s)",
					ports[i].port_id, ports[i].pbits,
					ports[i].queue, ports[i].weight,
					ports[i].cir, ports[i].pir,
					ports[i].ds_rate, ports[i].ds_burst,
					strerror(-err), omci_hw_last_extack());
				omci_hw_reject_add(rejected, ports[i].port_id);
				continue;
			}
			OMCI_INFO(
				"gem: Port-ID %u now takes priorities %#04x, queue %u, weight %u, rates %u/%u bytes/s, downstream limit %u bytes/s, burst %u",
				ports[i].port_id, ports[i].pbits,
				ports[i].queue, ports[i].weight, ports[i].cir,
				ports[i].pir, ports[i].ds_rate,
				ports[i].ds_burst);
		}
	}

	for (size_t j = 0; j < nhave; j++) {
		if (keep[j])
			continue;
		err = link_delete(have[j].ifindex);
		if (err) {
			OMCI_WARN("gem: could not remove Port-ID %u: %s",
				  have[j].port_id, strerror(-err));
			if (changed)
				notify_run();
			return OMCI_ERR_HW;
		}
		changed = true;
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
		changed = true;
		OMCI_INFO(
			"gem: Port-ID %u up as " GEM_NAME_FMT
			" (direction %u, T-CONT %u, priorities %#04x, queue %u, weight %u, rates %u/%u bytes/s, downstream limit %u bytes/s)",
			ports[i].port_id, ports[i].port_id, ports[i].direction,
			ports[i].tcont, ports[i].pbits, ports[i].queue,
			ports[i].weight, ports[i].cir, ports[i].pir,
			ports[i].ds_rate);
	}

	if (changed)
		notify_run();
	return (rejected != NULL && rejected->n) ? OMCI_DECLINED : OMCI_OK;
}

/*
 * The T-CONTs' Alloc-IDs as the kernel last took them; unknown until the
 * first mapping, so that a daemon that starts again tells the kernel all.
 */
static uint16_t g_tcont_sent[OMCI_HW_TCONT_MAX];
static bool g_tcont_known;

/*
 * Map T-CONT @index of the PON device @dev to @alloc, or unmap it for
 * %OMCI_HW_ALLOC_NONE, with a tcont-set request. Return: 0, or a negative
 * errno.
 */
static int tcont_set(uint32_t dev, uint32_t index, uint16_t alloc)
{
	uint8_t attrs[48] __aligned(4);
	uint8_t rsp[512] __aligned(4);
	uint32_t a = alloc;
	uint16_t family = 0;
	size_t alen = 0, rlen = 0;
	int err;

	err = pon_family_id(&family);
	if (err)
		return err;
	alen = nla_put(attrs, alen, PON_A_TCONT_ID, &dev, sizeof(dev));
	alen = nla_put(attrs, alen, PON_A_TCONT_INDEX, &index, sizeof(index));
	if (alloc != OMCI_HW_ALLOC_NONE)
		alen = nla_put(attrs, alen, PON_A_TCONT_ALLOC_ID, &a,
			       sizeof(a));
	return nl_request(family, PON_CMD_TCONT_SET, attrs, alen, NLM_F_ACK,
			  rsp, sizeof(rsp), &rlen);
}

/*
 * The Alloc-ID the kernel holds for T-CONT @index of the PON device @dev,
 * or %OMCI_HW_ALLOC_NONE for an idle T-CONT, in @alloc, with a tcont-get
 * request. Return: 0, or a negative errno.
 */
static int tcont_get(uint32_t dev, uint32_t index, uint16_t *alloc)
{
	uint8_t attrs[32] __aligned(4);
	uint8_t rsp[512] __aligned(4);
	uint16_t family = 0;
	size_t alen = 0, rlen = 0;
	const uint8_t *p;
	const uint8_t *end;
	int err;

	err = pon_family_id(&family);
	if (err)
		return err;
	alen = nla_put(attrs, alen, PON_A_TCONT_ID, &dev, sizeof(dev));
	alen = nla_put(attrs, alen, PON_A_TCONT_INDEX, &index, sizeof(index));
	err = nl_request(family, PON_CMD_TCONT_GET, attrs, alen, 0, rsp,
			 sizeof(rsp), &rlen);
	if (err)
		return err;
	if (rlen < NL_HDRLEN + 4)
		return -EPROTO;

	*alloc = OMCI_HW_ALLOC_NONE;
	p = rsp + NL_HDRLEN + 4;
	end = rsp + rlen;
	while (p + NLA_HDRLEN_ <= end) {
		uint16_t len = *(const uint16_t *)p;
		uint16_t type = *(const uint16_t *)(p + 2) & 0x3fff;

		if (len < NLA_HDRLEN_ || p + len > end)
			break;
		if (type == PON_A_TCONT_ALLOC_ID && len >= NLA_HDRLEN_ + 4)
			*alloc = (uint16_t)nla_uint32(p + NLA_HDRLEN_, 4);
		p += NL_ALIGN(len);
	}
	return 0;
}

/*
 * Map the T-CONTs to their Alloc-IDs, telling the kernel only the mappings
 * that change. The T-CONTs that give up an Alloc-ID are unmapped first, so
 * that the kernel never sees one Alloc-ID on two T-CONTs. The first time,
 * the kernel is asked what each T-CONT holds, and a daemon started again
 * leaves the T-CONTs of its MIB as they are; a kernel that does not say has
 * every T-CONT unmapped. A T-CONT the kernel will not map is rejected. A
 * kernel without tcont-set binds the Alloc-IDs itself, which is taken as
 * done.
 */
static enum omci_status kernel_tcont_apply(const uint16_t *alloc, size_t n,
					   struct omci_hw_reject *rejected)
{
	struct pon_dev_attrs a;
	uint8_t buf[4096] __aligned(4);
	uint32_t dev;
	int err;

	if (rejected != NULL)
		memset(rejected, 0, sizeof(*rejected));
	if (alloc == NULL && n > 0)
		return OMCI_ERR_INVAL;
	if (n > OMCI_HW_TCONT_MAX)
		return OMCI_ERR_NOMEM;
	if (pon_dev_get(&a, buf, sizeof(buf)) != 0)
		return OMCI_ERR_HW;
	dev = pon_attr_u32(&a, PON_A_DEV_ID, 0);

	if (!g_tcont_known) {
		bool told = true;

		for (size_t i = 0; i < n && told; i++)
			told = !tcont_get(dev, (uint32_t)i, &g_tcont_sent[i]);
		g_tcont_known = told;
	}

	for (size_t i = 0; i < n; i++) {
		if (g_tcont_known && (g_tcont_sent[i] == OMCI_HW_ALLOC_NONE ||
				      g_tcont_sent[i] == alloc[i]))
			continue;
		err = tcont_set(dev, (uint32_t)i, OMCI_HW_ALLOC_NONE);
		if (err == -EOPNOTSUPP) {
			OMCI_INFO(
				"tcont: the kernel does not map T-CONTs; it binds the Alloc-IDs itself");
			return OMCI_OK;
		}
		if (err) {
			OMCI_WARN(
				"tcont: the kernel did not unmap T-CONT %zu: %s",
				i, strerror(-err));
			omci_hw_reject_add(rejected, (uint16_t)i);
			continue;
		}
		g_tcont_sent[i] = OMCI_HW_ALLOC_NONE;
	}
	g_tcont_known = true;

	for (size_t i = 0; i < n; i++) {
		if (alloc[i] == OMCI_HW_ALLOC_NONE ||
		    g_tcont_sent[i] == alloc[i])
			continue;
		err = tcont_set(dev, (uint32_t)i, alloc[i]);
		if (err) {
			OMCI_WARN(
				"tcont: the kernel refused T-CONT %zu Alloc-ID %u: %s",
				i, alloc[i], strerror(-err));
			omci_hw_reject_add(rejected, (uint16_t)i);
			continue;
		}
		OMCI_INFO("tcont: T-CONT %zu takes Alloc-ID %u", i, alloc[i]);
		g_tcont_sent[i] = alloc[i];
	}
	return (rejected != NULL && rejected->n) ? OMCI_DECLINED : OMCI_OK;
}

/* The bridge of the OMCI configuration, and whether it filters VLANs (-1: unknown). */
#define BRIDGE_IFNAME "omcibr0"

static int g_bridge_filtering = -1;

/*
 * The bridge ports and the translations of the VLAN rules last programmed,
 * so that a change of either completes the VLANs of the ports (see
 * bridge_vlans_complete()), and the VLANs given to ports besides the
 * OLT's, each said once.
 */
static struct omci_hw_bridge_port g_br_ports[OMCI_HW_BRIDGE_MAX_PORTS];
static size_t g_br_n;
static struct omci_hw_vlan_xlate g_br_xlate[OMCI_HW_VLAN_MAX_RULES];
static size_t g_br_xlate_n;

#define BR_VIDS_MAX                                            \
	(OMCI_HW_BRIDGE_MAX_PORTS * OMCI_HW_BRIDGE_MAX_VLANS + \
	 OMCI_HW_VLAN_MAX_RULES)

static struct {
	uint16_t port_id;
	uint16_t vid;
} g_br_extra[OMCI_HW_BRIDGE_MAX_PORTS * BR_VIDS_MAX];
static size_t g_br_extra_n;

/*
 * Whether the VLAN rules discard the single-tagged frames that no rule
 * takes, and the subscriber's VIDs of the rules in force that take tagged
 * frames.
 */
static bool g_br_discard;
static uint16_t g_br_sub[OMCI_HW_VLAN_MAX_RULES];
static size_t g_br_sub_n;

static void uni_discard_sync(const uint16_t *vids, size_t n);

/* Router mode, and whether the log has said what it leaves out. */
static bool g_router;
static bool g_router_told;

/*
 * The VLANs the stick's own sessions hold, by the VID they have on the PON
 * side: one VID a line in HELD_PATH, which onu-apply writes before it has the
 * data path programmed again (SIGUSR2). The OLT's VLAN rules into those
 * VLANs, and its bridge ports that carry nothing else, stay out of the
 * hardware, so that the VLANs' frames reach the sessions and no frame of the
 * subscriber's crosses into them; the rest of what the OLT provisions is
 * bridged as before. A bridge that filters no VLAN would carry the sessions'
 * VLANs too, and the host link's VLAN the sessions hand their addresses on
 * in: while the stick holds VLANs, such a bridge stays out of the hardware
 * altogether, as in router mode (g_held_unbridged).
 */
#define HELD_PATH "/run/onu-held-vlans"
#define HELD_MAX 8u

static uint16_t g_held[HELD_MAX];
static size_t g_held_n;
static bool g_held_unbridged;
static bool g_held_told;

/*
 * Whether the bridge's going out of the hardware or coming back for the held
 * VLANs has changed what of the MIB applies: the OLT creates the bridge
 * ports before their VLAN filters, and the VLAN rules and the multicast
 * settings that came while the bridge was out were not applied. The daemon
 * then programs the whole data path again (kernel_reprogram_due()).
 */
static bool g_reprogram_due;

static void bridge_remove(void);

void omci_hw_kernel_router(bool on)
{
	g_router = on;
	g_router_told = false;
	g_held_told = false;
}

static bool kernel_routing(void)
{
	return g_router;
}

/*
 * The mode the data path was last programmed for, once it was, replaced
 * whole: what a program that signalled the change (onu-apply) waits for,
 * having removed the file before it signalled. No hook is run for it.
 */
#define APPLIED_PATH OMCI_RUN_DIR "/applied"

static void kernel_mode_done(bool router)
{
	char tmp[64];
	FILE *f;
	bool ok = false;

	snprintf(tmp, sizeof(tmp), "%s.new", APPLIED_PATH);
	f = fopen(tmp, "we");
	if (f != NULL) {
		ok = fputs(router ? "router\n" : "bridge\n", f) >= 0;
		ok = fclose(f) == 0 && ok;
		ok = ok && rename(tmp, APPLIED_PATH) == 0;
	}
	if (!ok)
		OMCI_WARN("mode: not told in %s", APPLIED_PATH);
}

/*
 * The date and time the OLT gave, with the monotonic time they came at, for
 * the stick's time daemon (timed -o), which takes the time now as the one
 * given plus the time since. Replaced whole, as KEY=VALUE lines.
 */
#define CLOCK_PATH OMCI_RUN_DIR "/clock"

static void kernel_time_of_day(uint64_t utc)
{
	struct timespec mono;
	char tmp[64];
	FILE *f;
	bool ok = false;

	if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0)
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", CLOCK_PATH);
	f = fopen(tmp, "we");
	if (f != NULL) {
		ok = fprintf(f, "utc=%llu\nmonotonic_ns=%lld\n",
			     (unsigned long long)utc,
			     (long long)mono.tv_sec * 1000000000LL +
				     mono.tv_nsec) > 0;
		ok = fclose(f) == 0 && ok;
		ok = ok && rename(tmp, CLOCK_PATH) == 0;
	}
	if (!ok)
		OMCI_WARN("sync-time: not told in %s", CLOCK_PATH);
}

/*
 * The Time of day information the kernel took, for the stick's time daemon
 * (timed -t), which then knows that the PON's PTP clock holds the OLT's time
 * from frame @superframe on: KEY=VALUE lines, replaced whole, with the
 * monotonic time the kernel took it at.
 */
#define TOD_PATH OMCI_RUN_DIR "/tod"

static void kernel_tod_tell(uint32_t superframe, uint64_t sec, uint32_t nsec)
{
	struct timespec mono;
	char tmp[64];
	FILE *f;
	bool ok = false;

	if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0)
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", TOD_PATH);
	f = fopen(tmp, "we");
	if (f != NULL) {
		ok = fprintf(f,
			     "superframe=%u\nsec=%llu\nnsec=%u\n"
			     "monotonic_ns=%lld\n",
			     superframe, (unsigned long long)sec, nsec,
			     (long long)mono.tv_sec * 1000000000LL +
				     mono.tv_nsec) > 0;
		ok = fclose(f) == 0 && ok;
		ok = ok && rename(tmp, TOD_PATH) == 0;
	}
	if (!ok)
		OMCI_WARN("tod: not told in %s", TOD_PATH);
}

/*
 * The OLT's Time of day information: the kernel sets the PON's PTP clock at
 * superframe @superframe to the time at which that frame reaches the ONU,
 * from TstampN (@sec, @nsec) and the ONU's equalization delay, with a tod-set
 * request (ITU-T G.984.3 Amendment 2 clause 10.4.6). What it took is told in
 * TOD_PATH.
 */
static void kernel_tod_info(uint32_t superframe, uint64_t sec, uint32_t nsec)
{
	uint8_t attrs[64] __aligned(4);
	uint8_t rsp[512] __aligned(4);
	uint8_t buf[4096] __aligned(4);
	struct pon_dev_attrs a;
	uint16_t family = 0;
	size_t alen = 0, rlen = 0;
	uint32_t dev;
	int err;

	err = pon_family_id(&family);
	if (!err)
		err = pon_dev_get(&a, buf, sizeof(buf));
	if (!err) {
		dev = pon_attr_u32(&a, PON_A_DEV_ID, 0);
		alen = nla_put(attrs, alen, PON_A_TOD_ID, &dev, sizeof(dev));
		alen = nla_put(attrs, alen, PON_A_TOD_SUPERFRAME, &superframe,
			       sizeof(superframe));
		alen = nla_put(attrs, alen, PON_A_TOD_SEC, &sec, sizeof(sec));
		alen = nla_put(attrs, alen, PON_A_TOD_NSEC, &nsec,
			       sizeof(nsec));
		err = nl_request(family, PON_CMD_TOD_SET, attrs, alen,
				 NLM_F_ACK, rsp, sizeof(rsp), &rlen);
	}
	if (err)
		OMCI_WARN("tod: the clock is not set at superframe %u: %s",
			  superframe, strerror(-err));
	else {
		OMCI_INFO(
			"tod: the clock takes TstampN %llu.%09u at superframe %u",
			(unsigned long long)sec, nsec, superframe);
		kernel_tod_tell(superframe, sec, nsec);
	}
}

/* Whether an address is already among those read, for the same side. */
static bool fdb_has(const struct omci_hw_fdb *out, size_t n, const uint8_t *mac,
		    bool ani)
{
	for (size_t i = 0; i < n; i++)
		if (out[i].ani == ani && !memcmp(out[i].mac, mac, 6))
			return true;
	return false;
}

/*
 * The addresses the switch forwards by: the entries its driver gives as its
 * own (NTF_SELF) for the UNI and for the PON side, once for each side; the
 * bridge's own entries are left out. A static entry is reported with
 * NUD_NOARP or NUD_PERMANENT.
 */
static enum omci_status kernel_fdb(struct omci_hw_fdb *out, size_t cap,
				   size_t *n)
{
	static uint8_t rsp[65536] __aligned(4);
	uint8_t req[NL_HDRLEN + NL_ALIGN(sizeof(struct ndmsg))] __aligned(4);
	struct ndmsg ndm = { .ndm_family = AF_BRIDGE };
	const int uni = (int)if_nametoindex(UNI_IFNAME);
	const int pon = (int)if_nametoindex(PON_IFNAME);
	size_t got = 0;

	if (out == NULL || n == NULL)
		return OMCI_ERR_INVAL;
	*n = 0;

	memset(req, 0, sizeof(req));
	*(uint32_t *)&req[0] = sizeof(req);
	*(uint16_t *)&req[4] = RTM_GETNEIGH;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_DUMP;
	*(uint32_t *)&req[8] = 21;
	memcpy(req + NL_HDRLEN, &ndm, sizeof(ndm));
	if (rtnl_talk(req, sizeof(req), rsp, sizeof(rsp), &got))
		return OMCI_ERR_HW;

	for (size_t off = 0; off + NL_HDRLEN <= got;) {
		const uint32_t mlen = *(const uint32_t *)(rsp + off);
		const struct ndmsg *m = (const void *)(rsp + off + NL_HDRLEN);
		const uint8_t *a = rsp + off + NL_HDRLEN + NL_ALIGN(sizeof(*m));
		const uint8_t *end = rsp + off + mlen;

		if (mlen < NL_HDRLEN + sizeof(*m))
			break;
		off += NL_ALIGN(mlen);
		if (*(const uint16_t *)(rsp + off - NL_ALIGN(mlen) + 4) !=
			    RTM_NEWNEIGH ||
		    !(m->ndm_flags & NTF_SELF) ||
		    (m->ndm_ifindex != uni && m->ndm_ifindex != pon))
			continue;
		while (a + NLA_HDRLEN_ <= end) {
			const uint16_t alen = *(const uint16_t *)a;
			const uint16_t atype = *(const uint16_t *)(a + 2);
			const bool ani = m->ndm_ifindex == pon;

			if (alen < NLA_HDRLEN_ || a + alen > end)
				break;
			if (atype == NDA_LLADDR && alen == NLA_HDRLEN_ + 6 &&
			    *n < cap &&
			    !fdb_has(out, *n, a + NLA_HDRLEN_, ani)) {
				memcpy(out[*n].mac, a + NLA_HDRLEN_, 6);
				out[*n].ani = ani;
				out[*n].dynamic =
					!(m->ndm_state &
					  (NUD_NOARP | NUD_PERMANENT));
				(*n)++;
			}
			a += NL_ALIGN(alen);
		}
	}
	return OMCI_OK;
}

/* Whether the bridge stays out of the hardware: in router mode, or for held VLANs. */
static bool bridge_out(void)
{
	return g_router || g_held_unbridged;
}

/* Without the bridge, say once that the OLT's bridging is not applied. */
static bool router_skips(const char *what)
{
	if (g_router) {
		if (!g_router_told) {
			OMCI_INFO(
				"router mode: the OLT's %s and the rest of its bridging stay out of the hardware",
				what);
			g_router_told = true;
		}
		return true;
	}
	if (g_held_unbridged) {
		if (!g_held_told) {
			OMCI_INFO(
				"held: the OLT's bridge filters no VLAN and would carry the sessions' VLANs; its %s and the rest of its bridging stay out of the hardware",
				what);
			g_held_told = true;
		}
		return true;
	}
	return false;
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
	g_br_extra_n = 0;

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
/*
 * Replace the file @path with @text, unless it holds @text already as the
 * last write left it in @last (@cap bytes); @what names it in a warning. A
 * change is told (notify_run()).
 */
static void run_publish(const char *path, const char *text, char *last,
			size_t cap, bool *published, const char *what)
{
	char tmp[64];
	bool ok = false;
	FILE *f;

	if (*published && !strcmp(text, last))
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "we");
	if (f != NULL) {
		ok = fputs(text, f) >= 0;
		ok = fclose(f) == 0 && ok;
		ok = ok && rename(tmp, path) == 0;
	}
	if (!ok) {
		OMCI_WARN("%s could not be published in %s", what, path);
		return;
	}
	snprintf(last, cap, "%s", text);
	*published = true;
	notify_run();
}

/*
 * The VLANs the OLT lets through each GEM port, published for onu-apply: a
 * "VID PORT-ID" line for each VID a GEM port's bridge port carries, held by
 * a session or not, in GEM_VLANS_PATH. onu-apply runs a session whose GEM
 * port is "auto" on the GEM port that carries the session's VLAN on the
 * fibre, so that a session follows its service when the OLT moves it to
 * another port. The file is replaced whole, and only when it changes.
 */
#define GEM_VLANS_PATH OMCI_RUN_DIR "/gem-vlans"
/* Room for the lines of every VID of every port, and the end. */
#define GEM_VLANS_TEXT \
	(OMCI_HW_BRIDGE_MAX_PORTS * OMCI_HW_BRIDGE_MAX_VLANS * 12 + 1)

/*
 * The stick's management VLAN, onu-mgmt's line VLAN= of MGMT_PATH, or 0.
 * The boot has the switch drop its frames on the PON's ingress, so that the
 * network behind the PON never reaches the stick's management.
 */
#define MGMT_PATH "/run/onu-mgmt"

static unsigned int mgmt_vlan(void)
{
	char line[64];
	unsigned int vid = 0;
	FILE *f = fopen(MGMT_PATH, "re");

	while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
		if (!strncmp(line, "VLAN=", 5)) {
			vid = (unsigned int)strtoul(line + 5, NULL, 10);
			break;
		}
	}
	if (f != NULL)
		fclose(f);
	return vid;
}

static void gem_vlans_publish(const struct omci_hw_bridge_port *ports, size_t n)
{
	static char last[GEM_VLANS_TEXT];
	static unsigned int warned;
	static bool published;
	char text[sizeof(last)] = "";
	unsigned int mgmt = mgmt_vlan(), clash = 0;
	size_t len = 0;

	for (size_t i = 0; i < n; i++) {
		if (ports[i].port_id == OMCI_HW_BRIDGE_UNI)
			continue;
		for (uint8_t v = 0; v < ports[i].num_vlan; v++) {
			len += (size_t)snprintf(text + len, sizeof(text) - len,
						"%u %u\n", ports[i].vlan[v],
						ports[i].port_id);
			if (mgmt && ports[i].vlan[v] == mgmt)
				clash = mgmt;
		}
	}
	/* Said once for each VLAN, again when it comes back after it went. */
	if (clash && clash != warned)
		OMCI_WARN(
			"bridge: the OLT carries VLAN %u, the stick's management VLAN, whose frames the switch drops from the PON: give the stick another management VLAN",
			clash);
	warned = clash;
	run_publish(GEM_VLANS_PATH, text, last, sizeof(last), &published,
		    "bridge: the VLANs of the GEM ports");
}

static unsigned int bridge_port_ifindex(const struct omci_hw_bridge_port *p)
{
	char name[IFNAMSIZ];

	if (p->port_id == OMCI_HW_BRIDGE_UNI)
		return if_nametoindex(UNI_IFNAME);

	snprintf(name, sizeof(name), GEM_NAME_FMT, p->port_id);
	return if_nametoindex(name);
}

static bool vid_in(const uint16_t *vids, size_t n, uint16_t vid)
{
	for (size_t i = 0; i < n; i++)
		if (vids[i] == vid)
			return true;
	return false;
}

/* Add @vid to a set of VIDs; true when it was not in it and fitted. */
static bool vid_put(uint16_t *vids, size_t *n, size_t cap, uint16_t vid)
{
	if (vid_in(vids, *n, vid) || *n >= cap)
		return false;
	vids[(*n)++] = vid;
	return true;
}

/* Read the held VLANs again (HELD_PATH); the log says when they change. */
static void held_load(void)
{
	uint16_t vids[HELD_MAX];
	char list[HELD_MAX * 6] = "";
	unsigned int vid;
	size_t n = 0, len = 0;
	FILE *f = fopen(HELD_PATH, "re");

	if (f) {
		while (n < HELD_MAX && fscanf(f, "%u", &vid) == 1)
			if (vid >= 1 && vid <= 4094)
				(void)vid_put(vids, &n, HELD_MAX,
					      (uint16_t)vid);
		fclose(f);
	}
	if (n == g_held_n && !memcmp(vids, g_held, n * sizeof(*vids)))
		return;
	memcpy(g_held, vids, n * sizeof(*vids));
	g_held_n = n;
	g_held_told = false;
	if (!n) {
		g_held_unbridged = false;
		OMCI_INFO(
			"held: the stick's sessions hold no VLAN; the OLT's bridging applies whole");
		return;
	}
	for (size_t i = 0; i < n && len < sizeof(list); i++)
		len += (size_t)snprintf(list + len, sizeof(list) - len, "%s%u",
					i ? ", " : "", vids[i]);
	OMCI_INFO(
		"held: the stick's sessions hold VLAN %s of the PON side; the OLT's rules into them and its ports of nothing else stay out of the hardware",
		list);
}

static bool held(uint16_t vid)
{
	return vid_in(g_held, g_held_n, vid);
}

/*
 * The bridge ports without the held VLANs, in @out: a port that had VLANs
 * and has none left, which carries a held VLAN alone, is left out. Returns
 * how many are left.
 */
static size_t held_strip(const struct omci_hw_bridge_port *ports, size_t n,
			 struct omci_hw_bridge_port *out)
{
	size_t kept = 0;

	for (size_t i = 0; i < n; i++) {
		struct omci_hw_bridge_port q = ports[i];

		q.num_vlan = 0;
		for (uint8_t v = 0; v < ports[i].num_vlan; v++)
			if (!held(ports[i].vlan[v]))
				q.vlan[q.num_vlan++] = ports[i].vlan[v];
		if (ports[i].num_vlan && !q.num_vlan) {
			OMCI_INFO(
				"bridge: Port-ID %u carries held VLANs alone and stays out of the bridge",
				ports[i].port_id);
			continue;
		}
		out[kept++] = q;
	}
	return kept;
}

static bool br_extra_has(uint16_t port_id, uint16_t vid)
{
	for (size_t i = 0; i < g_br_extra_n; i++)
		if (g_br_extra[i].port_id == port_id &&
		    g_br_extra[i].vid == vid)
			return true;
	return false;
}

/*
 * Complete the VLANs of the bridge ports while the bridge filters VLANs.
 * A port the OLT gave no VLAN filter, the UNI as a rule, takes every frame,
 * so it carries every VLAN of the other ports. And a translation changes a
 * frame's VID as it enters, upstream at sfp and downstream at pon, while the
 * switch forwards the frame by the VID it arrived with (measured on an
 * operator's line, 2026-10-05): a port that carries one VID of a
 * translation carries the other as well, or the bridge stops its frames in
 * one direction or both. A rule that pushes a tag adds nothing here.
 *
 * While the rules discard the single-tagged frames no rule takes, the VIDs
 * the UNI carries that no rule takes from the subscriber, which it carries
 * for the way down only, are dropped as they come in from the subscriber
 * (uni_discard_sync()). So are the VLANs the stick's sessions hold: the
 * switch forwards a frame by the members of its VLAN, not by the port it came
 * in by, and the PON port is a member of the sessions' VLANs, so that the
 * subscriber's frames tagged with one went out on the fibre (measured on this
 * board, 100 of 100). The subscriber's VIDs of the rules left out need no
 * such rule: no bridge port carries them any more, and the host link's own
 * VLAN, which may be one of them, takes the frames the stick routes. Without
 * VLAN filtering there is no such set, and nothing is dropped.
 */
static void bridge_vlans_complete(void)
{
	uint16_t all[BR_VIDS_MAX];
	uint16_t drop[BR_VIDS_MAX];
	size_t n_all = 0, n_drop = 0;

	if (g_bridge_filtering != 1) {
		uni_discard_sync(NULL, 0);
		return;
	}

	for (size_t i = 0; i < g_br_n; i++)
		for (uint8_t v = 0; v < g_br_ports[i].num_vlan; v++)
			(void)vid_put(all, &n_all, ARRAY_SIZE(all),
				      g_br_ports[i].vlan[v]);

	for (size_t i = 0; i < g_br_n; i++) {
		const struct omci_hw_bridge_port *p = &g_br_ports[i];
		const unsigned int ifindex = bridge_port_ifindex(p);
		uint16_t want[BR_VIDS_MAX];
		size_t n_want = 0;

		if (!ifindex)
			continue;
		if (p->num_vlan)
			for (uint8_t v = 0; v < p->num_vlan; v++)
				(void)vid_put(want, &n_want, ARRAY_SIZE(want),
					      p->vlan[v]);
		else
			for (size_t v = 0; v < n_all; v++)
				(void)vid_put(want, &n_want, ARRAY_SIZE(want),
					      all[v]);

		/* Each pass can only add; one per rule reaches every chain. */
		for (size_t pass = 0; pass <= g_br_xlate_n; pass++) {
			bool grew = false;

			for (size_t r = 0; r < g_br_xlate_n; r++) {
				const uint16_t sub = g_br_xlate[r].match_cvid;
				const uint16_t net = g_br_xlate[r].assign_cvid;

				if (vid_in(want, n_want, sub))
					grew |= vid_put(want, &n_want,
							ARRAY_SIZE(want), net);
				else if (vid_in(want, n_want, net))
					grew |= vid_put(want, &n_want,
							ARRAY_SIZE(want), sub);
			}
			if (!grew)
				break;
		}

		if (p->port_id == OMCI_HW_BRIDGE_UNI && g_br_discard)
			for (size_t v = 0; v < n_want; v++)
				if (!vid_in(g_br_sub, g_br_sub_n, want[v]))
					(void)vid_put(drop, &n_drop,
						      ARRAY_SIZE(drop),
						      want[v]);
		if (p->port_id == OMCI_HW_BRIDGE_UNI)
			for (size_t v = 0; v < g_held_n; v++)
				(void)vid_put(drop, &n_drop, ARRAY_SIZE(drop),
					      g_held[v]);

		for (size_t v = 0; v < n_want; v++) {
			const uint16_t vid = want[v];
			int err;

			if (vid_in(p->vlan, p->num_vlan, vid) ||
			    br_extra_has(p->port_id, vid))
				continue;
			err = bridge_vlan((int)ifindex, vid, true);
			if (err) {
				OMCI_WARN(
					"bridge: VLAN %u on %s %u, which its frames need, refused: %s (%s)",
					vid,
					p->port_id == OMCI_HW_BRIDGE_UNI ?
						"the UNI, port" :
						"Port-ID",
					p->port_id, strerror(-err),
					omci_hw_last_extack());
				continue;
			}
			if (g_br_extra_n < ARRAY_SIZE(g_br_extra)) {
				g_br_extra[g_br_extra_n].port_id = p->port_id;
				g_br_extra[g_br_extra_n].vid = vid;
				g_br_extra_n++;
			}
			if (p->port_id == OMCI_HW_BRIDGE_UNI)
				OMCI_INFO(
					"bridge: the UNI carries VLAN %u too, as a port without a filter of its own",
					vid);
			else
				OMCI_INFO(
					"bridge: Port-ID %u carries VLAN %u too, the other VID of a translation",
					p->port_id, vid);
		}
	}

	uni_discard_sync(drop, n_drop);
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

	gem_vlans_publish(ports, n);
	if (!n) {
		g_br_n = 0;
		return OMCI_OK;
	}
	held_load();
	{
		static struct omci_hw_bridge_port kept[OMCI_HW_BRIDGE_MAX_PORTS];
		bool unbridged = false;

		if (g_held_n) {
			n = held_strip(ports, n, kept);
			ports = kept;
			unbridged = true;
			for (size_t i = 0; i < n; i++)
				if (ports[i].num_vlan)
					unbridged = false;
		}
		if (unbridged != g_held_unbridged) {
			g_held_unbridged = unbridged;
			g_held_told = false;
			g_reprogram_due = true;
			if (unbridged)
				bridge_remove();
		}
	}
	if (router_skips("bridge"))
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

	memcpy(g_br_ports, ports, n * sizeof(*ports));
	g_br_n = n;
	bridge_vlans_complete();

	return (rejected != NULL && rejected->n) ? OMCI_DECLINED : OMCI_OK;
}

/*
 * The alarms of the PON device. The driver reports loss of signal, of frame
 * and of the message channel; signal fail and degrade it does not report,
 * and the daemon judges them from the BIP errors (kernel_pon()).
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

/* An unsigned attribute of four or eight bytes, in full. */
static bool nla_uint64(const uint8_t *v, uint16_t len, uint64_t *out)
{
	uint32_t v32;

	if (len == 8) {
		memcpy(out, v, sizeof(*out));
		return true;
	}
	if (len != 4)
		return false;
	memcpy(&v32, v, sizeof(v32));
	*out = v32;
	return true;
}

/*
 * The state of the PON receiver: whether the ONU is in operation, the level
 * the transmitter is at, and the BIP errors of the downstream among the
 * counters, which the driver reports only while the PON is up.
 */
static enum omci_status kernel_pon(struct omci_hw_pon *out)
{
	struct pon_dev_attrs a;
	uint8_t buf[4096] __aligned(4);
	const uint8_t *p, *end;
	unsigned int i, fec_have = 0;

	if (out == NULL)
		return OMCI_ERR_INVAL;
	memset(out, 0, sizeof(*out));

	/* The four counts of the downstream FEC, all or none. */
	const struct {
		uint16_t type;
		uint64_t *out;
	} fec[] = {
		{ PON_A_DEV_STATS_FEC_CORRECTED_BYTES,
		  &out->fec_corrected_bytes },
		{ PON_A_DEV_STATS_FEC_CORRECTED_CODEWORDS,
		  &out->fec_corrected_codewords },
		{ PON_A_DEV_STATS_FEC_UNCORRECTABLE_CODEWORDS,
		  &out->fec_uncorrectable_codewords },
		{ PON_A_DEV_STATS_FEC_CODEWORDS, &out->fec_codewords },
	};

	if (pon_dev_get(&a, buf, sizeof(buf)))
		return OMCI_ERR_HW;

	out->operating = pon_attr_u32(&a, PON_A_DEV_ONU_STATE, 0) ==
			 PON_ONU_STATE_O5;
	if (a.val[PON_A_DEV_POWER_LEVEL]) {
		out->power_level =
			(uint8_t)pon_attr_u32(&a, PON_A_DEV_POWER_LEVEL, 0);
		out->power_level_valid = true;
	}

	p = a.val[PON_A_DEV_STATS];
	end = p ? p + a.len[PON_A_DEV_STATS] : NULL;
	while (p && p + NLA_HDRLEN_ <= end) {
		uint16_t alen = *(const uint16_t *)p;
		uint16_t atype = *(const uint16_t *)(p + 2) & 0x3fff;

		if (alen < NLA_HDRLEN_ || p + alen > end)
			break;
		if (atype == PON_A_DEV_STATS_BIP_ERRORS)
			out->bip_valid = nla_uint64(p + NLA_HDRLEN_,
						    alen - NLA_HDRLEN_,
						    &out->bip_errors);
		for (i = 0; i < ARRAY_SIZE(fec); i++)
			if (atype == fec[i].type &&
			    nla_uint64(p + NLA_HDRLEN_, alen - NLA_HDRLEN_,
				       fec[i].out))
				fec_have |= 1u << i;
		p += NL_ALIGN(alen);
	}
	out->fec_valid = fec_have == (1u << ARRAY_SIZE(fec)) - 1u;
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

/* Read every statistic of an interface, names and values. */
static int counter_cache_load(const char *ifname)
{
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

	g_ctr.n = n;
	g_ctr.valid = true;
	return 0;
}

/* The value of the statistic of the last interface read with a name. */
static bool counter_named(const char *name, uint64_t *out)
{
	unsigned int i;

	for (i = 0; i < g_ctr.n; i++) {
		const char *s = (const char *)g_ctr.strings->data +
				(size_t)i * ETH_GSTRING_LEN;

		if (strncmp(s, name, ETH_GSTRING_LEN))
			continue;
		*out = g_ctr.stats->data[i];
		return true;
	}
	return false;
}

/* Read every counter of a port at once: the statistics are read afresh. */
static enum omci_status
kernel_port_counters(unsigned int port, uint64_t out[OMCI_HW_PORT_COUNTERS])
{
	unsigned int i;

	if (out == NULL)
		return OMCI_ERR_INVAL;
	memset(out, 0, OMCI_HW_PORT_COUNTERS * sizeof(out[0]));

	if (counter_cache_load(port_ifname(port)))
		return OMCI_ERR_HW;

	for (i = 0; i < ARRAY_SIZE(COUNTERS); i++)
		if (COUNTERS[i].idx < OMCI_HW_PORT_COUNTERS &&
		    !counter_named(COUNTERS[i].name, &out[COUNTERS[i].idx]))
			return OMCI_ERR_HW;
	return OMCI_OK;
}

/* The bytes of the GEM header, which the count of octets sent includes. */
#define GEM_HEADER_BYTES 5u

/*
 * The counts of a GEM port, the ethtool statistics of its interface. The
 * octets sent count every GEM frame whole, its header included, and the
 * octets received the payload alone (Documentation/networking/pon.rst).
 */
static enum omci_status kernel_gem_counts(uint16_t port_id,
					  struct omci_hw_gem_counts *out)
{
	char name[IFNAMSIZ];
	uint64_t tx_octets = 0;

	if (out == NULL)
		return OMCI_ERR_INVAL;
	memset(out, 0, sizeof(*out));

	snprintf(name, sizeof(name), GEM_NAME_FMT, port_id);
	if (counter_cache_load(name))
		return OMCI_ERR_HW;
	if (!counter_named("pon_tx_octets", &tx_octets) ||
	    !counter_named("pon_tx_frames", &out->tx_frames) ||
	    !counter_named("pon_rx_frames", &out->rx_frames) ||
	    !counter_named("pon_rx_octets", &out->rx_payload))
		return OMCI_ERR_HW;

	if (tx_octets > out->tx_frames * GEM_HEADER_BYTES)
		out->tx_payload = tx_octets - out->tx_frames * GEM_HEADER_BYTES;
	return OMCI_OK;
}

/* The hwmon device of the optics. */
#define HWMON_DIR "/sys/class/hwmon"
#define HWMON_NAME "gn25l95"

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

/*
 * The alarm thresholds of the optics the daemon judges by, in hwmon units
 * (milliamperes, microwatts), turned into those of SFF-8472: the alarm
 * thresholds are the crit and lcrit attributes.
 */
static enum omci_status kernel_optics_limits(struct omci_hw_optics_limits *out)
{
	static const struct {
		enum omci_hw_limit which;
		const char *file;
		long long scale;
	} LIMITS[] = {
		{ OMCI_HW_LIMIT_BIAS_HIGH, "curr1_crit", 500 },
		{ OMCI_HW_LIMIT_TX_LOW, "power1_lcrit", 10 },
		{ OMCI_HW_LIMIT_TX_HIGH, "power1_crit", 10 },
		{ OMCI_HW_LIMIT_RX_LOW, "power2_lcrit", 10 },
		{ OMCI_HW_LIMIT_RX_HIGH, "power2_crit", 10 },
	};
	char dir[160];
	long long v;
	unsigned int i;

	if (out == NULL)
		return OMCI_ERR_INVAL;
	memset(out, 0, sizeof(*out));

	if (!hwmon_path(dir, sizeof(dir)))
		return OMCI_ERR_HW;

	for (i = 0; i < ARRAY_SIZE(LIMITS); i++) {
		if (!hwmon_value(dir, LIMITS[i].file, &v) || v < 0 ||
		    v * LIMITS[i].scale > UINT16_MAX)
			continue;
		out->value[LIMITS[i].which] = (uint16_t)(v * LIMITS[i].scale);
		out->valid |= (uint8_t)(1u << LIMITS[i].which);
	}
	return out->valid ? OMCI_OK : OMCI_ERR_HW;
}

/*
 * The priorities of the filters are the registry's (tcprio.h); the VLAN
 * rules have one each, from TC_PRIO_VLAN.
 */
#define TC_PRIO_LAST (TC_PRIO_VLAN + OMCI_HW_VLAN_MAX_RULES)
_Static_assert(OMCI_HW_VLAN_MAX_RULES <= TC_PRIO_VLAN_N,
	       "more VLAN rules than priorities in the registry");

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
 * A VLAN filter: the frames it takes, untagged ones or those whose outer tag,
 * of protocol @match_proto (802.1Q for 0), has VID @match_vid and, with
 * @match_has_pri, priority @match_pri; and what its vlan action does to them:
 * TCA_VLAN_ACT_MODIFY or PUSH writes a tag of protocol @push_proto (802.1Q
 * for 0) and VID @vid with priority @pri, or keeps the frame's priority for
 * OMCI_HW_VLAN_PRI_KEEP; POP takes the outer tag off.
 */
struct tc_vlan_filter {
	bool untagged;
	uint16_t match_proto;
	uint16_t match_vid;
	bool match_has_pri;
	uint8_t match_pri;
	int v_action;
	uint16_t push_proto;
	uint16_t vid;
	uint8_t pri;
};

/* Add an ingress filter, in the switch only (skip_sw). */
static int tc_filter_add(int ifindex, unsigned int prio,
			 const struct tc_vlan_filter *f)
{
	uint8_t req[1024] __aligned(4);
	const uint16_t match_proto = f->match_proto ? f->match_proto :
						      ETH_P_8021Q;
	struct tcmsg tcm = {
		.tcm_family = AF_UNSPEC,
		.tcm_ifindex = ifindex,
		.tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS),
		.tcm_info =
			tc_info(prio, f->untagged ? ETH_P_ALL : match_proto),
	};
	struct tc_vlan parms = {
		.action = TC_ACT_PIPE,
		.v_action = f->v_action,
	};
	uint32_t flags = OMCID_TC_FLAGS;
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
		uint16_t proto = htons(match_proto);

		off = nla_put(req, off, TCA_FLOWER_KEY_ETH_TYPE, &proto,
			      sizeof(proto));
		off = nla_put(req, off, TCA_FLOWER_KEY_VLAN_ID, &f->match_vid,
			      sizeof(f->match_vid));
		if (f->match_has_pri)
			off = nla_put(req, off, TCA_FLOWER_KEY_VLAN_PRIO,
				      &f->match_pri, sizeof(f->match_pri));
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
		if (f->push_proto) {
			uint16_t proto = htons(f->push_proto);

			off = nla_put(req, off, TCA_VLAN_PUSH_VLAN_PROTOCOL,
				      &proto, sizeof(proto));
		}
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

/* The most upstream filters a rule takes: one per priority. */
#define VLAN_UP_MAX 8u

/*
 * The filters of a rule: the upstream ones on sfp, and on pon the downstream
 * one that undoes them. A translation is undone by the translation back,
 * keeping the priority; a C-tag pushed onto an untagged frame, and an S-tag
 * pushed in front of a C-tag, by popping the tag. A rule that takes one
 * priority matches it upstream; an S-tag that takes the priority of the
 * C-tag behind it from frames of any priority takes one upstream filter per
 * priority, as a push names the priority it gives, and so does a rule that
 * takes each priority apart (@per_pri), so that the switch gives the frames
 * of each priority the GEM port of that priority. Returns the number of
 * upstream filters, 0 for a rule of no kind known here.
 */
static size_t vlan_filters(const struct omci_hw_vlan_xlate *r,
			   struct tc_vlan_filter up[static VLAN_UP_MAX],
			   struct tc_vlan_filter *down)
{
	switch (r->kind) {
	case OMCI_HW_VLAN_CVID_XLATE: {
		const bool apart = r->per_pri && !r->match_has_pri;
		const size_t n = apart ? VLAN_UP_MAX : 1u;

		for (size_t i = 0; i < n; i++)
			up[i] = (struct tc_vlan_filter){
				.match_vid = r->match_cvid,
				.match_has_pri = apart || r->match_has_pri,
				.match_pri = apart ? (uint8_t)i : r->match_pri,
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
		return n;
	}
	case OMCI_HW_VLAN_CVID_PUSH:
		up[0] = (struct tc_vlan_filter){
			.untagged = true,
			.v_action = TCA_VLAN_ACT_PUSH,
			.vid = r->assign_cvid,
			.pri = r->assign_pri,
		};
		*down = (struct tc_vlan_filter){
			.match_vid = r->assign_cvid,
			.v_action = TCA_VLAN_ACT_POP,
		};
		return 1;
	case OMCI_HW_VLAN_SVID_PUSH: {
		const bool copy = r->assign_pri == OMCI_HW_VLAN_PRI_COPY;
		const size_t n = copy && !r->match_has_pri ? VLAN_UP_MAX : 1u;

		for (size_t i = 0; i < n; i++) {
			const uint8_t pri = r->match_has_pri ? r->match_pri :
							       (uint8_t)i;

			up[i] = (struct tc_vlan_filter){
				.match_vid = r->match_cvid,
				.match_has_pri = copy || r->match_has_pri,
				.match_pri = pri,
				.v_action = TCA_VLAN_ACT_PUSH,
				.push_proto = ETH_P_8021AD,
				.vid = r->assign_svid,
				.pri = copy ? pri : r->assign_pri,
			};
		}
		*down = (struct tc_vlan_filter){
			.match_proto = ETH_P_8021AD,
			.match_vid = r->assign_svid,
			.v_action = TCA_VLAN_ACT_POP,
		};
		return n;
	}
	default:
		return 0;
	}
}

/*
 * The OLT's translations, published for onu-apply: a "SUB NET" line for each
 * C-VID the OLT translates from the subscriber's side, held or not, with the
 * VID it leads into, in XLATE_PATH. onu-apply runs a session given the
 * subscriber's VID of a translation in the VID it leads into, as the
 * subscriber's frames would leave. The file is replaced whole, and only when
 * it changes.
 */
#define XLATE_PATH OMCI_RUN_DIR "/xlate"

static void xlate_publish(const struct omci_hw_vlan_xlate *rules, size_t n)
{
	static char last[OMCI_HW_VLAN_MAX_RULES * 12 + 1];
	static bool published;
	char text[sizeof(last)] = "";
	uint16_t subs[OMCI_HW_VLAN_MAX_RULES];
	size_t len = 0, n_subs = 0;

	for (size_t i = 0; i < n; i++) {
		if (rules[i].kind != OMCI_HW_VLAN_CVID_XLATE ||
		    !vid_put(subs, &n_subs, ARRAY_SIZE(subs),
			     rules[i].match_cvid))
			continue;
		len += (size_t)snprintf(text + len, sizeof(text) - len,
					"%u %u\n", rules[i].match_cvid,
					rules[i].assign_cvid);
	}
	run_publish(XLATE_PATH, text, last, sizeof(last), &published,
		    "vlan: the translations");
}

/* Replace the VLAN rules, each with its filters (see vlan_filters()). */
static enum omci_status
kernel_vlan_apply(const struct omci_hw_vlan_xlate *rules, size_t n,
		  struct omci_hw_reject *rejected)
{
	unsigned int uni = if_nametoindex(UNI_IFNAME);
	unsigned int pon = if_nametoindex(PON_IFNAME);
	struct omci_hw_vlan_xlate xlate[OMCI_HW_VLAN_MAX_RULES];
	uint16_t sub[OMCI_HW_VLAN_MAX_RULES];
	size_t n_xlate = 0, n_sub = 0;
	bool discard = false;
	int err;

	if (rejected != NULL)
		memset(rejected, 0, sizeof(*rejected));

	if (rules == NULL && n > 0)
		return OMCI_ERR_INVAL;
	if (n > OMCI_HW_VLAN_MAX_RULES)
		return OMCI_ERR_NOMEM;
	xlate_publish(rules, n);
	held_load();
	if (router_skips("VLAN rules"))
		return OMCI_OK;
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

	for (unsigned int prio = TC_PRIO_VLAN; prio < TC_PRIO_LAST; prio++) {
		(void)tc_filter_del((int)uni, prio);
		(void)tc_filter_del((int)pon, prio);
	}

	for (size_t i = 0; i < n; i++) {
		const unsigned int prio = TC_PRIO_VLAN + (unsigned int)i;
		const struct omci_hw_vlan_xlate *r = &rules[i];
		struct tc_vlan_filter up[VLAN_UP_MAX], down;

		/* No filter of its own: what it drops follows from the others. */
		if (r->kind == OMCI_HW_VLAN_DISCARD_TAGGED) {
			discard = true;
			continue;
		}

		const uint16_t net = r->kind == OMCI_HW_VLAN_SVID_PUSH ?
					     r->assign_svid :
					     r->assign_cvid;

		if (held(net)) {
			OMCI_INFO(
				"vlan: rule %zu (C-VID %u) leads into held VLAN %u and stays out of the hardware",
				i, r->match_cvid, net);
			continue;
		}

		const size_t n_up = vlan_filters(r, up, &down);

		if (!n_up) {
			OMCI_WARN(
				"vlan: rule %zu (C-VID %u) is of no kind known here; not programmed",
				i, r->match_cvid);
			omci_hw_reject_add(rejected, r->match_cvid);
			continue;
		}

		err = 0;
		for (size_t k = 0; k < n_up && !err; k++)
			err = tc_filter_add((int)uni, prio, &up[k]);
		if (err) {
			OMCI_WARN("vlan: upstream rule %zu refused: %s (%s)", i,
				  strerror(-err), omci_hw_last_extack());
			(void)tc_filter_del((int)uni, prio);
			omci_hw_reject_add(rejected, r->match_cvid);
			continue;
		}
		err = r->up_only ? 0 : tc_filter_add((int)pon, prio, &down);
		if (err) {
			OMCI_WARN("vlan: downstream rule %zu refused: %s (%s)",
				  i, strerror(-err), omci_hw_last_extack());
			(void)tc_filter_del((int)uni, prio);
			omci_hw_reject_add(rejected, r->match_cvid);
			continue;
		}
		if (r->kind == OMCI_HW_VLAN_CVID_XLATE)
			xlate[n_xlate++] = *r;
		if (r->kind != OMCI_HW_VLAN_CVID_PUSH)
			sub[n_sub++] = r->match_cvid;

		char pri[16] = "";

		if (r->match_has_pri)
			snprintf(pri, sizeof(pri), " priority %u",
				 r->match_pri);
		if (r->kind == OMCI_HW_VLAN_CVID_PUSH)
			OMCI_INFO("vlan: untagged <-> C-VID %u in hardware",
				  r->assign_cvid);
		else if (r->kind == OMCI_HW_VLAN_SVID_PUSH)
			OMCI_INFO(
				"vlan: C-VID %u%s <-> S-VID %u in front of it, in hardware%s",
				r->match_cvid, pri, r->assign_svid,
				r->up_only ? ", upstream only" : "");
		else
			OMCI_INFO(
				"vlan: C-VID %u%s <-> %u translated in hardware%s%s",
				r->match_cvid, pri, r->assign_cvid,
				n_up > 1u ? ", each priority apart" : "",
				r->up_only ? ", upstream only" : "");
	}

	memcpy(g_br_xlate, xlate, n_xlate * sizeof(*xlate));
	g_br_xlate_n = n_xlate;
	memcpy(g_br_sub, sub, n_sub * sizeof(*sub));
	g_br_sub_n = n_sub;
	g_br_discard = discard;
	bridge_vlans_complete();

	return (rejected != NULL && rejected->n) ? OMCI_DECLINED : OMCI_OK;
}

/*
 * The bridge out of the hardware, for the held VLANs: the bridge itself, the
 * VLAN rules and the drops at the UNI. The traps of IGMP and MLD, the
 * multicast tags and the bridge port filters follow when the data path is
 * programmed again.
 */
static void bridge_remove(void)
{
	const int br = (int)if_nametoindex(BRIDGE_IFNAME);
	const int uni = (int)if_nametoindex(UNI_IFNAME);
	const int pon = (int)if_nametoindex(PON_IFNAME);

	for (unsigned int prio = TC_PRIO_VLAN; prio < TC_PRIO_LAST; prio++) {
		if (uni)
			(void)tc_filter_del(uni, prio);
		if (pon)
			(void)tc_filter_del(pon, prio);
	}
	uni_discard_sync(NULL, 0);
	if (br && link_delete(br))
		OMCI_WARN("bridge: %s could not be removed", BRIDGE_IFNAME);
	g_br_n = 0;
	g_br_xlate_n = 0;
	g_br_sub_n = 0;
	g_br_extra_n = 0;
	g_bridge_filtering = -1;
}

static bool kernel_reprogram_due(void)
{
	const bool due = g_reprogram_due;

	g_reprogram_due = false;
	return due;
}

/* The kernel backend. */
/*
 * Multicast. The bridge snoops IGMP and MLD and keeps the groups it forwards
 * to sfp in its MDB, which the agent reads (RTM_GETMDB), changes
 * (RTM_NEWMDB, RTM_DELMDB) and hears change (RTNLGRP_MDB). The messages
 * come up a packet socket through a filter that lets IGMP and the MLD behind
 * a hop-by-hop header through, untagged or under one or two tags: those that
 * sfp received are the subscriber's, those that a GEM interface received the
 * OLT's, and no other interface's are taken. One epoll descriptor stands for
 * both sockets. In router mode sfp is no bridge port: there is nothing to
 * read or change.
 */
static int g_mc_epoll = -1;
static int g_mc_events = -1;
static int g_mc_packets = -1;
static bool g_mc_tried;

/* The bridge's timers are in hundredths of a second (USER_HZ). */
#define CENTI_PER_SEC 100u

/*
 * The traps of the subscriber side's IGMP and MLD, while the agent sends
 * them upstream itself: IGMP, and the MLD query (130), version 1 report
 * (131) and done (132) and version 2 report (143), each a flower rule on sfp
 * with priorities from TRAP_PRIO, before the VLAN rules (tc_vlan_apply).
 */
#define TRAP_PRIO TC_PRIO_TRAP
static const struct {
	uint16_t proto;
	uint8_t ip_proto;
	int icmp_type;
} g_traps[] = {
	{ ETH_P_IP, IPPROTO_IGMP, -1 },
	{ ETH_P_IPV6, IPPROTO_ICMPV6, 130 },
	{ ETH_P_IPV6, IPPROTO_ICMPV6, 131 },
	{ ETH_P_IPV6, IPPROTO_ICMPV6, 132 },
	{ ETH_P_IPV6, IPPROTO_ICMPV6, 143 },
};
_Static_assert(ARRAY_SIZE(g_traps) <= TC_PRIO_TRAP_N,
	       "more traps than priorities in the registry");
/*
 * Whether the traps are on: -1 until the first setting, which also clears
 * what an earlier run left; and the last failure to set them, said once.
 */
static int g_mc_trap = -1;
static int g_mc_trap_err;
static int g_mc_tx = -1;

/*
 * A flower rule of one gact action, @action: the frames of protocol @proto,
 * ETH_P_ALL for every EtherType; with @ip_proto, of that IP protocol; with an
 * @icmp_type not negative, of that ICMPv6 type; with @dmac, to that MAC
 * address, compared where @dmask has ones (all of it without @dmask); with
 * @smac, from that address under @smask; with @dip, to that IPv4 address;
 * with @vid, of that VID, for @proto ETH_P_8021Q.
 * @handle 0 lets the kernel choose it.
 */
struct tc_gact_rule {
	uint16_t proto;
	uint8_t ip_proto;
	int icmp_type;
	const uint8_t *dmac;
	const uint8_t *dmask;
	const uint8_t *smac;
	const uint8_t *smask;
	const uint8_t *dip;
	uint16_t vid;
	int action;
	uint32_t handle;
};

/* Add a flower rule of one gact action on an ingress, in the switch only. */
static int tc_gact_add(int ifindex, unsigned int prio,
		       const struct tc_gact_rule *r)
{
	static const uint8_t all[ETH_ALEN] = { 0xff, 0xff, 0xff,
					       0xff, 0xff, 0xff };
	uint8_t req[512] __aligned(4);
	struct tcmsg tcm = {
		.tcm_family = AF_UNSPEC,
		.tcm_ifindex = ifindex,
		.tcm_handle = r->handle,
		.tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS),
		.tcm_info = tc_info(prio, r->proto),
	};
	struct tc_gact gact = { .action = r->action };
	const uint16_t eth = htons(r->proto);
	uint32_t flags = OMCID_TC_FLAGS;
	size_t off, opts, acts, act1, aopts;

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_NEWTFILTER;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
			       NLM_F_EXCL;
	*(uint32_t *)&req[8] = 24;
	memcpy(req + NL_HDRLEN, &tcm, sizeof(tcm));
	off = NL_HDRLEN + sizeof(tcm);
	off = nla_put(req, off, TCA_KIND, "flower", sizeof("flower"));

	opts = off;
	off += NLA_HDRLEN_;
	if (r->proto != ETH_P_ALL)
		off = nla_put(req, off, TCA_FLOWER_KEY_ETH_TYPE, &eth,
			      sizeof(eth));
	if (r->ip_proto)
		off = nla_put(req, off, TCA_FLOWER_KEY_IP_PROTO, &r->ip_proto,
			      sizeof(r->ip_proto));
	if (r->icmp_type >= 0) {
		const uint8_t type = (uint8_t)r->icmp_type, mask = 0xffu;

		off = nla_put(req, off, TCA_FLOWER_KEY_ICMPV6_TYPE, &type,
			      sizeof(type));
		off = nla_put(req, off, TCA_FLOWER_KEY_ICMPV6_TYPE_MASK, &mask,
			      sizeof(mask));
	}
	if (r->dmac) {
		off = nla_put(req, off, TCA_FLOWER_KEY_ETH_DST, r->dmac,
			      ETH_ALEN);
		off = nla_put(req, off, TCA_FLOWER_KEY_ETH_DST_MASK,
			      r->dmask ? r->dmask : all, ETH_ALEN);
	}
	if (r->smac) {
		off = nla_put(req, off, TCA_FLOWER_KEY_ETH_SRC, r->smac,
			      ETH_ALEN);
		off = nla_put(req, off, TCA_FLOWER_KEY_ETH_SRC_MASK, r->smask,
			      ETH_ALEN);
	}
	if (r->dip) {
		off = nla_put(req, off, TCA_FLOWER_KEY_IPV4_DST, r->dip, 4);
		off = nla_put(req, off, TCA_FLOWER_KEY_IPV4_DST_MASK, all, 4);
	}
	if (r->vid)
		off = nla_put(req, off, TCA_FLOWER_KEY_VLAN_ID, &r->vid,
			      sizeof(r->vid));
	off = nla_put(req, off, TCA_FLOWER_FLAGS, &flags, sizeof(flags));

	acts = off;
	off += NLA_HDRLEN_;
	act1 = off;
	off += NLA_HDRLEN_;
	off = nla_put(req, off, TCA_ACT_KIND, "gact", sizeof("gact"));
	aopts = off;
	off += NLA_HDRLEN_;
	off = nla_put(req, off, TCA_GACT_PARMS, &gact, sizeof(gact));
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

/* Put the traps on sfp, or take them off; on a failure none is left. */
static int mcast_traps(int uni, bool on)
{
	int err = 0;

	for (size_t i = 0; i < ARRAY_SIZE(g_traps); i++)
		(void)tc_filter_del(uni, TRAP_PRIO + (unsigned int)i);
	if (!on)
		return 0;
	err = tc_clsact_ensure(uni);
	for (size_t i = 0; i < ARRAY_SIZE(g_traps) && !err; i++) {
		const struct tc_gact_rule r = {
			.proto = g_traps[i].proto,
			.ip_proto = g_traps[i].ip_proto,
			.icmp_type = g_traps[i].icmp_type,
			.action = TC_ACT_TRAP,
		};

		err = tc_gact_add(uni, TRAP_PRIO + (unsigned int)i, &r);
	}
	if (err)
		(void)mcast_traps(uni, false);
	return err;
}

/*
 * The frames the subscriber sends in a VLAN the UNI carries for the way down
 * only, which the VLAN rules discard as frames no rule takes: a flower rule
 * each on sfp's ingress, with priorities from DISCARD_PRIO, ahead of the
 * traps of IGMP and MLD and of the bridge port filters, as the VLAN rules
 * take a frame first at the UNI. The rules change only when the VIDs do.
 */
#define DISCARD_PRIO TC_PRIO_DISCARD
#define DISCARD_MAX TC_PRIO_DISCARD_N

static uint16_t g_uni_drop[DISCARD_MAX];
static size_t g_uni_drop_n;
/*
 * Whether the rules an earlier run left were taken away: a daemon that ends
 * leaves its rules on sfp, and one that starts again knows none of them.
 */
static bool g_uni_drop_clear;

static void uni_discard_sync(const uint16_t *vids, size_t n)
{
	const int uni = (int)if_nametoindex(UNI_IFNAME);
	bool same = n == g_uni_drop_n && g_uni_drop_clear;
	int err;

	for (size_t i = 0; same && i < n; i++)
		same = vid_in(g_uni_drop, g_uni_drop_n, vids[i]);
	if (same || !uni)
		return;
	g_uni_drop_clear = true;

	for (unsigned int i = 0; i < DISCARD_MAX; i++)
		(void)tc_filter_del(uni, DISCARD_PRIO + i);
	g_uni_drop_n = 0;
	if (!n)
		return;
	if (n > DISCARD_MAX) {
		OMCI_WARN(
			"vlan: %zu VLANs to drop from the subscriber; the first %u are",
			n, DISCARD_MAX);
		n = DISCARD_MAX;
	}
	err = tc_clsact_ensure(uni);
	for (size_t i = 0; i < n && !err; i++) {
		const struct tc_gact_rule r = {
			.proto = ETH_P_8021Q,
			.icmp_type = -1,
			.vid = vids[i],
			.action = TC_ACT_SHOT,
		};

		err = tc_gact_add(uni, DISCARD_PRIO + (unsigned int)i, &r);
		if (err)
			break;
		g_uni_drop[g_uni_drop_n++] = vids[i];
		OMCI_INFO(
			"vlan: the subscriber's frames of VLAN %u are dropped: no rule takes them, and the rules discard what they do not take",
			vids[i]);
	}
	if (err)
		OMCI_WARN(
			"vlan: frames of VLANs no rule takes still pass from the subscriber: %s (%s)",
			strerror(-err), omci_hw_last_extack());
}

/*
 * The bridge port filters: a flower rule each, on sfp's ingress for the frames
 * that go upstream and on pon's for those that go downstream, with priorities
 * from FILTER_PRIO in the order given. They come after the traps of IGMP and
 * MLD, so that the agent still takes those, and before the counting rules of
 * the multicast groups, so that a filter drops a group's frames.
 */
#define FILTER_PRIO TC_PRIO_FILTER
_Static_assert(OMCI_HW_FILTER_MAX <= TC_PRIO_FILTER_N,
	       "more bridge port filters than priorities in the registry");

static bool mac_any(const uint8_t *mask)
{
	for (size_t i = 0; i < ETH_ALEN; i++)
		if (mask[i])
			return true;
	return false;
}

static enum omci_status kernel_filter_apply(const struct omci_hw_filter *rules,
					    size_t n)
{
	const int uni = (int)if_nametoindex(UNI_IFNAME);
	const int pon = (int)if_nametoindex(PON_IFNAME);
	unsigned int up = 0, down = 0;
	int err = 0;

	if (n > OMCI_HW_FILTER_MAX)
		return OMCI_ERR_NOMEM;
	if (!uni || !pon)
		return OMCI_ERR_HW;
	for (unsigned int i = 0; i < OMCI_HW_FILTER_MAX; i++) {
		(void)tc_filter_del(uni, FILTER_PRIO + i);
		(void)tc_filter_del(pon, FILTER_PRIO + i);
	}
	if (n == 0 || router_skips("bridge port filters"))
		return OMCI_OK;
	err = tc_clsact_ensure(uni);
	if (!err)
		err = tc_clsact_ensure(pon);
	for (size_t i = 0; i < n && !err; i++) {
		const struct omci_hw_filter *f = &rules[i];
		const struct tc_gact_rule r = {
			.proto = f->ethertype ? f->ethertype : ETH_P_ALL,
			.icmp_type = -1,
			.dmac = mac_any(f->dmask) ? f->dmac : NULL,
			.dmask = f->dmask,
			.smac = mac_any(f->smask) ? f->smac : NULL,
			.smask = f->smask,
			.action = f->pass ? TC_ACT_OK : TC_ACT_SHOT,
		};

		err = tc_gact_add(f->upstream ? uni : pon,
				  FILTER_PRIO + (f->upstream ? up++ : down++),
				  &r);
		if (err)
			OMCI_WARN("filter: rule %zu: %s (%s)", i,
				  strerror(-err), omci_hw_last_extack());
	}
	return err ? OMCI_ERR_HW : OMCI_OK;
}

/*
 * The downstream tags of the multicast VLANs: a flower rule each on pon's
 * ingress, with priorities from DS_PRIO, whose VLAN no VLAN rule shares.
 */
#define DS_PRIO TC_PRIO_DS
_Static_assert(OMCI_HW_MCAST_DS_MAX <= TC_PRIO_DS_N,
	       "more downstream tags than priorities in the registry");

static enum omci_status kernel_mcast_ds(const struct omci_hw_mcast_ds *rules,
					size_t n)
{
	const int pon = (int)if_nametoindex(PON_IFNAME);
	int err = 0;

	if (n > OMCI_HW_MCAST_DS_MAX)
		return OMCI_ERR_NOMEM;
	if (!pon)
		return OMCI_ERR_HW;
	for (unsigned int i = 0; i < OMCI_HW_MCAST_DS_MAX; i++)
		(void)tc_filter_del(pon, DS_PRIO + i);
	if (n == 0 || router_skips("multicast tags"))
		return OMCI_OK;
	err = tc_clsact_ensure(pon);
	for (size_t i = 0; i < n && !err; i++) {
		struct tc_vlan_filter f = {
			.match_vid = rules[i].vid,
			.vid = rules[i].tci & 0x0fffu,
			.pri = (uint8_t)(rules[i].tci >> 13),
		};

		switch (rules[i].action) {
		case OMCI_HW_MCAST_DS_STRIP:
			f.v_action = TCA_VLAN_ACT_POP;
			break;
		case OMCI_HW_MCAST_DS_ADD:
			f.v_action = TCA_VLAN_ACT_PUSH;
			f.push_proto = ETH_P_8021AD;
			break;
		case OMCI_HW_MCAST_DS_REPLACE_VID:
			f.pri = OMCI_HW_VLAN_PRI_KEEP;
			f.v_action = TCA_VLAN_ACT_MODIFY;
			break;
		default:
			f.v_action = TCA_VLAN_ACT_MODIFY;
			break;
		}
		err = tc_filter_add(pon, DS_PRIO + (unsigned int)i, &f);
		if (err)
			OMCI_WARN(
				"mcast: the downstream tag of VLAN %u: %s (%s)",
				(unsigned int)rules[i].vid, strerror(-err),
				omci_hw_last_extack());
	}
	return err ? OMCI_ERR_HW : OMCI_OK;
}

/*
 * The downstream frames of the groups the agent counts: a flower rule each
 * on pon's ingress, with priorities from WATCH_PRIO and a handle of its
 * own, that passes the frames and only counts them. An IPv4 group is taken
 * by its address; an IPv6 group by the MAC address it maps to (RFC 2464),
 * as the switch compares no IPv6 address.
 */
#define WATCH_PRIO TC_PRIO_WATCH
_Static_assert(OMCI_HW_MCAST_WATCH_MAX <= TC_PRIO_WATCH_N,
	       "more counted groups than priorities in the registry");
#define WATCH_HANDLE 1u
static struct {
	bool used;
	uint8_t group[OMCI_HW_MCAST_ADDR_LEN];
} g_mc_watch[OMCI_HW_MCAST_WATCH_MAX];
/* Whether what an earlier run left was cleared; the last failure, said once. */
static bool g_mc_watch_clear;
static int g_mc_watch_err;

/* Whether an address is IPv4, in the IPv4-mapped form. */
static bool mapped_v4(const uint8_t *a)
{
	static const uint8_t prefix[12] = { 0, 0, 0, 0, 0,    0,
					    0, 0, 0, 0, 0xff, 0xff };

	return memcmp(a, prefix, sizeof(prefix)) == 0;
}

/* The counting rule of a group; OMCI_HW_MCAST_WATCH_MAX when there is none. */
static size_t watch_slot(const uint8_t *group)
{
	size_t i = 0;

	while (i < OMCI_HW_MCAST_WATCH_MAX &&
	       (!g_mc_watch[i].used ||
		memcmp(g_mc_watch[i].group, group, OMCI_HW_MCAST_ADDR_LEN)))
		i++;
	return i;
}

/* The payload of the first attribute of @type in [p, end); NULL when there is none. */
static const uint8_t *nla_find(const uint8_t *p, const uint8_t *end,
			       uint16_t type, uint16_t *len)
{
	while (p + NLA_HDRLEN_ <= end) {
		const uint16_t alen = *(const uint16_t *)p;
		const uint16_t atype = *(const uint16_t *)(p + 2) & 0x3fff;

		if (alen < NLA_HDRLEN_ || p + alen > end)
			return NULL;
		if (atype == type) {
			*len = (uint16_t)(alen - NLA_HDRLEN_);
			return p + NLA_HDRLEN_;
		}
		p += NL_ALIGN(alen);
	}
	return NULL;
}

/*
 * The frames the counting rule of priority @prio has passed: the counts of
 * its action, which the kernel brings up to date from the switch as it
 * answers.
 */
static int tc_watch_frames(int ifindex, unsigned int prio, uint16_t proto,
			   uint64_t *frames)
{
	static uint8_t rsp[4096] __aligned(4);
	uint8_t req[NL_HDRLEN + sizeof(struct tcmsg) + 16] __aligned(4);
	struct tcmsg tcm = {
		.tcm_family = AF_UNSPEC,
		.tcm_ifindex = ifindex,
		.tcm_handle = WATCH_HANDLE,
		.tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS),
		.tcm_info = tc_info(prio, proto),
	};
	const uint8_t *a, *end, *pkt64;
	struct gnet_stats_basic basic;
	size_t got = 0, off;
	uint16_t len = 0, len64 = 0;
	int err;

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = RTM_GETTFILTER;
	*(uint16_t *)&req[6] = NLM_F_REQUEST;
	*(uint32_t *)&req[8] = 25;
	memcpy(req + NL_HDRLEN, &tcm, sizeof(tcm));
	off = nla_put(req, NL_HDRLEN + sizeof(tcm), TCA_KIND, "flower",
		      sizeof("flower"));
	*(uint32_t *)&req[0] = (uint32_t)off;
	err = rtnl_talk(req, off, rsp, sizeof(rsp), &got);
	if (err)
		return err;
	if (got < NL_HDRLEN + sizeof(tcm) ||
	    *(const uint16_t *)(rsp + 4) != RTM_NEWTFILTER)
		return -EPROTO;

	/* The options, their actions, the first action, its counts. */
	end = rsp + *(const uint32_t *)rsp;
	a = nla_find(rsp + NL_HDRLEN + sizeof(tcm), end, TCA_OPTIONS, &len);
	if (a)
		a = nla_find(a, a + len, TCA_FLOWER_ACT, &len);
	if (a)
		a = nla_find(a, a + len, 1, &len);
	if (a)
		a = nla_find(a, a + len, TCA_ACT_STATS, &len);
	if (!a)
		return -EPROTO;

	/* A count past 32 bits comes in an attribute of its own. */
	end = a + len;
	pkt64 = nla_find(a, end, TCA_STATS_PKT64, &len64);
	a = nla_find(a, end, TCA_STATS_BASIC, &len);
	if (!a || len < sizeof(basic))
		return -EPROTO;
	memcpy(&basic, a, sizeof(basic));
	*frames = basic.packets;
	if (pkt64 && len64 == sizeof(*frames))
		memcpy(frames, pkt64, sizeof(*frames));
	return 0;
}

static enum omci_status kernel_mcast_watch(const uint8_t *group, bool on)
{
	const int pon = (int)if_nametoindex(PON_IFNAME);
	size_t i = watch_slot(group);
	int err;

	if (!pon)
		return OMCI_ERR_HW;
	if (!g_mc_watch_clear) {
		for (unsigned int k = 0; k < OMCI_HW_MCAST_WATCH_MAX; k++)
			(void)tc_filter_del(pon, WATCH_PRIO + k);
		g_mc_watch_clear = true;
	}
	if (!on) {
		if (i == OMCI_HW_MCAST_WATCH_MAX)
			return OMCI_OK;
		err = tc_filter_del(pon, WATCH_PRIO + (unsigned int)i);
		if (err) {
			OMCI_DEBUG("mcast: a counting rule stays: %s",
				   strerror(-err));
			return OMCI_ERR_HW;
		}
		g_mc_watch[i].used = false;
		return OMCI_OK;
	}
	if (i < OMCI_HW_MCAST_WATCH_MAX)
		return OMCI_OK;
	i = 0;
	while (i < OMCI_HW_MCAST_WATCH_MAX && g_mc_watch[i].used)
		i++;
	if (i == OMCI_HW_MCAST_WATCH_MAX)
		return OMCI_ERR_NOMEM;

	{
		const bool v4 = mapped_v4(group);
		const uint8_t dmac[ETH_ALEN] = { 0x33,	    0x33,
						 group[12], group[13],
						 group[14], group[15] };
		const struct tc_gact_rule r = {
			.proto = v4 ? ETH_P_IP : ETH_P_IPV6,
			.icmp_type = -1,
			.dmac = v4 ? NULL : dmac,
			.dip = v4 ? &group[12] : NULL,
			.action = TC_ACT_OK,
			.handle = WATCH_HANDLE,
		};

		err = tc_clsact_ensure(pon);
		if (!err)
			err = tc_gact_add(pon, WATCH_PRIO + (unsigned int)i,
					  &r);
	}
	if (err) {
		if (err != g_mc_watch_err)
			OMCI_WARN(
				"mcast: the frames of a group cannot be counted: %s (%s)",
				strerror(-err), omci_hw_last_extack());
		g_mc_watch_err = err;
		return OMCI_ERR_HW;
	}
	g_mc_watch_err = 0;
	g_mc_watch[i].used = true;
	memcpy(g_mc_watch[i].group, group, OMCI_HW_MCAST_ADDR_LEN);
	return OMCI_OK;
}

static enum omci_status kernel_mcast_seen(const uint8_t *group,
					  uint64_t *frames)
{
	const int pon = (int)if_nametoindex(PON_IFNAME);
	const size_t i = watch_slot(group);
	int err;

	if (i == OMCI_HW_MCAST_WATCH_MAX)
		return OMCI_DECLINED;
	if (!pon)
		return OMCI_ERR_HW;
	err = tc_watch_frames(pon, WATCH_PRIO + (unsigned int)i,
			      mapped_v4(group) ? ETH_P_IP : ETH_P_IPV6, frames);
	if (err) {
		OMCI_DEBUG("mcast: the count of a group could not be read: %s",
			   strerror(-err));
		return OMCI_ERR_HW;
	}
	return OMCI_OK;
}

/* The Ethernet address of sfp, which the agent's own messages are from. */
static enum omci_status kernel_mcast_mac(uint8_t mac[static 6])
{
	struct ifreq ifr = { 0 };
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return OMCI_ERR_HW;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", UNI_IFNAME);
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		close(fd);
		return OMCI_ERR_HW;
	}
	close(fd);
	memcpy(mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
	return OMCI_OK;
}

/*
 * Send a message upstream through the interface of a GEM port, or to the
 * subscriber side through sfp.
 */
static enum omci_status kernel_mcast_send(uint16_t port_id,
					  const uint8_t *frame, size_t len)
{
	struct sockaddr_ll sll = {
		.sll_family = AF_PACKET,
		.sll_halen = ETH_ALEN,
	};
	char name[IFNAMSIZ];

	if (g_mc_tx < 0)
		g_mc_tx = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (port_id == OMCI_HW_MCAST_UNI)
		snprintf(name, sizeof(name), "%s", UNI_IFNAME);
	else
		snprintf(name, sizeof(name), GEM_NAME_FMT, port_id);
	sll.sll_ifindex = (int)if_nametoindex(name);
	if (g_mc_tx < 0 || !sll.sll_ifindex || len < ETH_HLEN)
		return OMCI_ERR_HW;
	memcpy(sll.sll_addr, frame, ETH_ALEN);
	memcpy(&sll.sll_protocol, &frame[2 * ETH_ALEN], 2);
	if (sendto(g_mc_tx, frame, len, 0, (struct sockaddr *)&sll,
		   sizeof(sll)) != (ssize_t)len) {
		OMCI_DEBUG("mcast: %s would not send: %s", name,
			   strerror(errno));
		return OMCI_ERR_HW;
	}
	return OMCI_OK;
}

static int mcast_packet_socket(void)
{
	static const struct sock_filter code[] = {
		/* X: the bytes of tags before the EtherType */
		BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 0),
		BPF_STMT(BPF_LD | BPF_H | BPF_IND, 12),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021Q, 1, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021AD, 0, 6),
		BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 4),
		BPF_STMT(BPF_LD | BPF_H | BPF_IND, 12),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021Q, 1, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021AD, 0, 2),
		BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 8),
		BPF_STMT(BPF_LD | BPF_H | BPF_IND, 12),
		/* IPv4 with the protocol IGMP */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_IP, 0, 2),
		BPF_STMT(BPF_LD | BPF_B | BPF_IND, 14 + 9),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_IGMP, 3, 4),
		/* IPv6 with a hop-by-hop header first */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_IPV6, 0, 3),
		BPF_STMT(BPF_LD | BPF_B | BPF_IND, 14 + 6),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_HOPOPTS, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, 0xffff),
		BPF_STMT(BPF_RET | BPF_K, 0),
	};
	const struct sock_fprog prog = {
		.len = ARRAY_SIZE(code),
		.filter = (struct sock_filter *)code,
	};
	/* Every interface: the frame says which it came from. */
	struct sockaddr_ll sll = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
	};
	int on = 1;
	int fd, err;

	/* No frame comes before the filter is on and the socket bound. */
	fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) <
		    0 ||
	    setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &on, sizeof(on)) < 0 ||
	    bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		err = -errno;
		close(fd);
		return err;
	}
	return fd;
}

static int mcast_event_socket(void)
{
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
		.nl_groups = 1u << (RTNLGRP_MDB - 1),
	};
	int fd, err;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		err = -errno;
		close(fd);
		return err;
	}
	return fd;
}

/* The descriptor of both sockets, made once; -1 when they cannot be made. */
static int kernel_mcast_fd(void)
{
	struct epoll_event ev = { .events = EPOLLIN };
	int err;

	if (g_mc_tried)
		return g_mc_epoll;
	g_mc_tried = true;

	g_mc_packets = mcast_packet_socket();
	g_mc_events = mcast_event_socket();
	g_mc_epoll = epoll_create1(EPOLL_CLOEXEC);
	if (g_mc_packets < 0 || g_mc_events < 0 || g_mc_epoll < 0) {
		err = g_mc_packets < 0 ? g_mc_packets :
		      g_mc_events < 0  ? g_mc_events :
					 -errno;
		OMCI_WARN(
			"mcast: no messages of %s or changes of the bridge's groups to wait on (%s); looking once a second",
			UNI_IFNAME, strerror(-err));
		if (g_mc_epoll >= 0)
			close(g_mc_epoll);
		g_mc_epoll = -1;
		return -1;
	}
	ev.data.fd = g_mc_packets;
	(void)epoll_ctl(g_mc_epoll, EPOLL_CTL_ADD, g_mc_packets, &ev);
	ev.data.fd = g_mc_events;
	(void)epoll_ctl(g_mc_epoll, EPOLL_CTL_ADD, g_mc_events, &ev);
	OMCI_INFO(
		"mcast: reading the IGMP and MLD messages of %s and the GEM interfaces, and the groups of %s",
		UNI_IFNAME, BRIDGE_IFNAME);
	return g_mc_epoll;
}

/*
 * Which side an interface is: the subscriber's (1), the OLT's (2) when it is
 * a GEM interface, or neither (0).
 */
static int mcast_side(int ifindex)
{
	char name[IFNAMSIZ];
	unsigned int port;
	int n = 0;

	if (!if_indextoname((unsigned int)ifindex, name))
		return 0;
	if (strcmp(name, UNI_IFNAME) == 0)
		return 1;
	return sscanf(name, GEM_NAME_FMT "%n", &port, &n) == 1 &&
			       name[n] == '\0' ?
		       2 :
		       0;
}

/*
 * The next message: one that sfp or a GEM interface received, not one the
 * stick sent out of it, with the tag the receive path took off.
 */
static enum omci_status kernel_mcast_frame(struct omci_hw_mcast_frame *out)
{
	uint8_t ctl[CMSG_SPACE(sizeof(struct tpacket_auxdata))] __aligned(8);

	if (g_mc_packets < 0)
		return OMCI_DECLINED;
	for (;;) {
		struct sockaddr_ll from = { 0 };
		struct iovec iov = { .iov_base = out->data,
				     .iov_len = sizeof(out->data) };
		struct msghdr msg = {
			.msg_name = &from,
			.msg_namelen = sizeof(from),
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = ctl,
			.msg_controllen = sizeof(ctl),
		};
		struct cmsghdr *c;
		const ssize_t n = recvmsg(g_mc_packets, &msg, 0);

		int side;

		if (n < 0)
			return OMCI_DECLINED;
		if (from.sll_pkttype == PACKET_OUTGOING || bridge_out())
			continue;
		side = mcast_side(from.sll_ifindex);
		if (side == 0)
			continue;
		out->len = (uint16_t)n;
		out->from_pon = side == 2;
		out->tagged = false;
		for (c = CMSG_FIRSTHDR(&msg); c != NULL;
		     c = CMSG_NXTHDR(&msg, c)) {
			struct tpacket_auxdata aux;

			if (c->cmsg_level != SOL_PACKET ||
			    c->cmsg_type != PACKET_AUXDATA)
				continue;
			memcpy(&aux, CMSG_DATA(c), sizeof(aux));
			if (aux.tp_status & TP_STATUS_VLAN_VALID) {
				out->tagged = true;
				out->tci = aux.tp_vlan_tci;
			}
		}
		return OMCI_OK;
	}
}

/* An address of the bridge's MDB, IPv4 or IPv6, as the agent keeps them. */
static void mdb_addr(uint8_t out[static OMCI_HW_MCAST_ADDR_LEN], const void *in,
		     bool v6)
{
	memset(out, 0, OMCI_HW_MCAST_ADDR_LEN);
	if (v6) {
		memcpy(out, in, OMCI_HW_MCAST_ADDR_LEN);
		return;
	}
	out[10] = 0xffu;
	out[11] = 0xffu;
	memcpy(&out[12], in, 4);
}

/*
 * One MDBA_MDB_ENTRY_INFO: a struct br_mdb_entry, then the entry's own
 * attributes, the source among them for a group from one source.
 */
static void mdb_entry(const uint8_t *p, uint16_t len, int uni,
		      struct omci_hw_mcast_group *out, size_t cap, size_t *n)
{
	struct br_mdb_entry e;
	const uint8_t *a, *end = p + len;
	struct omci_hw_mcast_group g = { .vid = 0 };
	bool v6;

	if (len < sizeof(e))
		return;
	memcpy(&e, p, sizeof(e));
	if ((int)e.ifindex != uni || (e.flags & MDB_FLAGS_BLOCKED) ||
	    (e.addr.proto != htons(ETH_P_IP) &&
	     e.addr.proto != htons(ETH_P_IPV6)))
		return;
	v6 = e.addr.proto == htons(ETH_P_IPV6);
	mdb_addr(g.group,
		 v6 ? (const void *)&e.addr.u.ip6 : (const void *)&e.addr.u.ip4,
		 v6);
	g.vid = e.vid;
	g.permanent = e.state == MDB_PERMANENT;

	for (a = p + NL_ALIGN(sizeof(e)); a + NLA_HDRLEN_ <= end;) {
		const uint16_t alen = *(const uint16_t *)a;
		const uint16_t atype = *(const uint16_t *)(a + 2) & 0x3fff;

		if (alen < NLA_HDRLEN_ || a + alen > end)
			break;
		if (atype == MDBA_MDB_EATTR_SOURCE &&
		    alen == NLA_HDRLEN_ + (v6 ? 16u : 4u))
			mdb_addr(g.source, a + NLA_HDRLEN_, v6);
		a += NL_ALIGN(alen);
	}
	if (*n < cap)
		out[(*n)++] = g;
}

/* Walk the nested attributes of @type in [p, end), calling @fn on each. */
static void nla_each(const uint8_t *p, const uint8_t *end, uint16_t type,
		     void (*fn)(const uint8_t *, uint16_t, void *), void *ctx)
{
	while (p + NLA_HDRLEN_ <= end) {
		const uint16_t alen = *(const uint16_t *)p;
		const uint16_t atype = *(const uint16_t *)(p + 2) & 0x3fff;

		if (alen < NLA_HDRLEN_ || p + alen > end)
			return;
		if (atype == type)
			fn(p + NLA_HDRLEN_, (uint16_t)(alen - NLA_HDRLEN_),
			   ctx);
		p += NL_ALIGN(alen);
	}
}

struct mdb_walk {
	int uni;
	struct omci_hw_mcast_group *out;
	size_t cap;
	size_t n;
};

static void mdb_on_info(const uint8_t *p, uint16_t len, void *ctx)
{
	struct mdb_walk *w = ctx;

	mdb_entry(p, len, w->uni, w->out, w->cap, &w->n);
}

static void mdb_on_entry(const uint8_t *p, uint16_t len, void *ctx)
{
	nla_each(p, p + len, MDBA_MDB_ENTRY_INFO, mdb_on_info, ctx);
}

static void mdb_on_mdb(const uint8_t *p, uint16_t len, void *ctx)
{
	nla_each(p, p + len, MDBA_MDB_ENTRY, mdb_on_entry, ctx);
}

static enum omci_status kernel_mcast_groups(struct omci_hw_mcast_group *out,
					    size_t cap, size_t *n)
{
	static uint8_t rsp[65536] __aligned(4);
	uint8_t req[NL_HDRLEN + NL_ALIGN(sizeof(struct br_port_msg))] __aligned(
		4);
	struct br_port_msg bpm = { .family = AF_BRIDGE };
	struct mdb_walk w = { .out = out, .cap = cap };
	uint8_t drain[4096] __aligned(4);
	size_t got = 0;
	int err;

	*n = 0;
	while (g_mc_events >= 0 &&
	       recv(g_mc_events, drain, sizeof(drain), MSG_DONTWAIT) > 0)
		;
	if (bridge_out())
		return OMCI_OK;
	bpm.ifindex = if_nametoindex(BRIDGE_IFNAME);
	w.uni = (int)if_nametoindex(UNI_IFNAME);
	if (!bpm.ifindex || !w.uni)
		return OMCI_OK;

	memset(req, 0, sizeof(req));
	*(uint32_t *)&req[0] = sizeof(req);
	*(uint16_t *)&req[4] = RTM_GETMDB;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_DUMP;
	*(uint32_t *)&req[8] = 20;
	memcpy(req + NL_HDRLEN, &bpm, sizeof(bpm));
	err = rtnl_talk(req, sizeof(req), rsp, sizeof(rsp), &got);
	if (err) {
		OMCI_WARN("mcast: the groups of %s could not be read: %s",
			  BRIDGE_IFNAME, strerror(-err));
		return OMCI_ERR_HW;
	}
	for (size_t off = 0; off + NL_HDRLEN <= got;) {
		const uint32_t mlen = *(const uint32_t *)(rsp + off);
		const uint16_t type = *(const uint16_t *)(rsp + off + 4);
		const uint8_t *body = rsp + off + NL_HDRLEN +
				      NL_ALIGN(sizeof(struct br_port_msg));

		/* A dump answers with RTM_GETMDB, a change with RTM_NEWMDB. */
		if ((type == RTM_GETMDB || type == RTM_NEWMDB) &&
		    body <= rsp + off + mlen)
			nla_each(body, rsp + off + mlen, MDBA_MDB, mdb_on_mdb,
				 &w);
		off += NL_ALIGN(mlen);
	}
	*n = w.n;
	return OMCI_OK;
}

/* Add a static group to sfp, or remove a group, static or snooped. */
static enum omci_status mdb_change(const struct omci_hw_mcast_group *g,
				   bool add)
{
	static const uint8_t none[OMCI_HW_MCAST_ADDR_LEN];
	uint8_t req[256] __aligned(4);
	struct br_port_msg bpm = { .family = AF_BRIDGE };
	struct br_mdb_entry e;
	const bool v6 = !(g->group[10] == 0xffu && g->group[11] == 0xffu &&
			  !memcmp(g->group, none, 10));
	size_t off;
	int err;

	if (bridge_out() || (g->alias && g_bridge_filtering != 1))
		return OMCI_OK;
	memset(&e, 0, sizeof(e));
	bpm.ifindex = if_nametoindex(BRIDGE_IFNAME);
	e.ifindex = if_nametoindex(UNI_IFNAME);
	if (!bpm.ifindex || !e.ifindex)
		return OMCI_ERR_HW;
	e.state = MDB_PERMANENT;
	e.vid = g_bridge_filtering == 1 ? g->vid : 0;
	e.addr.proto = htons(v6 ? ETH_P_IPV6 : ETH_P_IP);
	if (v6)
		memcpy(&e.addr.u.ip6, g->group, 16);
	else
		memcpy(&e.addr.u.ip4, &g->group[12], 4);

	memset(req, 0, sizeof(req));
	*(uint16_t *)&req[4] = add ? RTM_NEWMDB : RTM_DELMDB;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK |
			       (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
	*(uint32_t *)&req[8] = 21;
	memcpy(req + NL_HDRLEN, &bpm, sizeof(bpm));
	off = NL_HDRLEN + NL_ALIGN(sizeof(bpm));
	off = nla_put(req, off, MDBA_SET_ENTRY, &e, sizeof(e));
	if (memcmp(g->source, none, sizeof(none)) != 0) {
		const size_t nest = off;

		off = nla_put(req, off + NLA_HDRLEN_, MDBE_ATTR_SOURCE,
			      v6 ? g->source : &g->source[12], v6 ? 16 : 4);
		*(uint16_t *)(req + nest) = (uint16_t)(off - nest);
		*(uint16_t *)(req + nest + 2) = MDBA_SET_ENTRY_ATTRS |
						NLA_F_NESTED;
	}
	*(uint32_t *)&req[0] = (uint32_t)off;

	err = rtnl_talk(req, off, NULL, 0, NULL);
	/*
	 * A group gone already is declined: the bridge says -ENOENT, or, from
	 * __br_mdb_del() when its own leave took the snooped entry first,
	 * -EINVAL with no reason, where a request it cannot read gets one.
	 */
	if (!add && (err == -ENOENT || (err == -EINVAL && !g_rtnl_extack[0])))
		return OMCI_DECLINED;
	if (err) {
		OMCI_WARN("mcast: %s would not %s a group: %s (%s)",
			  BRIDGE_IFNAME, add ? "add" : "remove", strerror(-err),
			  omci_hw_last_extack());
		return OMCI_ERR_HW;
	}
	return OMCI_OK;
}

static enum omci_status
kernel_mcast_group_add(const struct omci_hw_mcast_group *g)
{
	return mdb_change(g, true);
}

static enum omci_status
kernel_mcast_group_del(const struct omci_hw_mcast_group *g)
{
	return mdb_change(g, false);
}

/*
 * The leave of sfp, and the bridge's timers from the profile's IGMP
 * settings, 0 for the default of RFC 3376: a robustness of 2, a query each
 * 125 s, 10 s to answer it, 1 s to answer a group's query. A group lives
 * robustness times the query interval and the answer time (RFC 3376 8.4).
 */
static enum omci_status kernel_mcast_port(const struct omci_hw_mcast_port *cfg)
{
	const uint32_t rob = cfg->robustness ? cfg->robustness : 2u;
	const uint64_t query =
		(uint64_t)(cfg->query_interval ? cfg->query_interval : 125u) *
		CENTI_PER_SEC;
	const uint64_t answer =
		(uint64_t)(cfg->query_response ? cfg->query_response : 100u) *
		(CENTI_PER_SEC / 10u);
	const uint64_t last =
		(uint64_t)(cfg->last_member ? cfg->last_member : 10u) *
		(CENTI_PER_SEC / 10u);
	const uint64_t member = rob * query + answer;
	const uint64_t querier = rob * query + answer / 2u;
	const uint8_t fast = cfg->fast_leave ? 1u : 0u;
	uint8_t req[384] __aligned(4);
	uint8_t data[160] __aligned(4);
	uint8_t info[192] __aligned(4);
	struct ifinfomsg ifi = { .ifi_family = AF_BRIDGE };
	size_t off, nest, dlen = 0, ilen = 0;
	int br, uni, err;

	uni = (int)if_nametoindex(UNI_IFNAME);
	/* Without the bridge sfp is no bridge port, and nothing is trapped. */
	if (uni && g_mc_trap != (int)(cfg->trap && !bridge_out())) {
		err = mcast_traps(uni, cfg->trap && !bridge_out());
		if (err) {
			if (err != g_mc_trap_err)
				OMCI_WARN(
					"mcast: the traps of IGMP and MLD on %s could not be set: %s (%s)",
					UNI_IFNAME, strerror(-err),
					omci_hw_last_extack());
			g_mc_trap_err = err;
			return OMCI_ERR_HW;
		}
		g_mc_trap_err = 0;
		g_mc_trap = (int)(cfg->trap && !bridge_out());
		OMCI_INFO("mcast: the IGMP and MLD of %s go %s", UNI_IFNAME,
			  g_mc_trap ?
				  "to the agent, which sends them upstream" :
				  "upstream through the switch");
	}
	if (bridge_out())
		return OMCI_OK;
	br = (int)if_nametoindex(BRIDGE_IFNAME);
	if (!br || !uni) {
		OMCI_DEBUG("mcast: no %s with %s yet", BRIDGE_IFNAME,
			   UNI_IFNAME);
		return OMCI_ERR_HW;
	}

	/* The leave of the port. */
	memset(req, 0, sizeof(req));
	ifi.ifi_index = uni;
	*(uint16_t *)&req[4] = RTM_SETLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 22;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = NL_HDRLEN + sizeof(ifi);
	nest = off;
	off = nla_put(req, off + NLA_HDRLEN_, IFLA_BRPORT_FAST_LEAVE, &fast,
		      sizeof(fast));
	*(uint16_t *)(req + nest) = (uint16_t)(off - nest);
	*(uint16_t *)(req + nest + 2) = IFLA_PROTINFO | NLA_F_NESTED;
	*(uint32_t *)&req[0] = (uint32_t)off;
	err = rtnl_talk(req, off, NULL, 0, NULL);
	if (err) {
		OMCI_WARN("mcast: the leave of %s could not be set: %s (%s)",
			  UNI_IFNAME, strerror(-err), omci_hw_last_extack());
		return OMCI_ERR_HW;
	}

	/* The bridge's timers. */
	dlen = nla_put(data, dlen, IFLA_BR_MCAST_LAST_MEMBER_CNT, &rob,
		       sizeof(rob));
	dlen = nla_put(data, dlen, IFLA_BR_MCAST_STARTUP_QUERY_CNT, &rob,
		       sizeof(rob));
	dlen = nla_put(data, dlen, IFLA_BR_MCAST_QUERY_INTVL, &query,
		       sizeof(query));
	dlen = nla_put(data, dlen, IFLA_BR_MCAST_QUERY_RESPONSE_INTVL, &answer,
		       sizeof(answer));
	dlen = nla_put(data, dlen, IFLA_BR_MCAST_LAST_MEMBER_INTVL, &last,
		       sizeof(last));
	dlen = nla_put(data, dlen, IFLA_BR_MCAST_MEMBERSHIP_INTVL, &member,
		       sizeof(member));
	dlen = nla_put(data, dlen, IFLA_BR_MCAST_QUERIER_INTVL, &querier,
		       sizeof(querier));
	ilen = nla_put(info, ilen, IFLA_INFO_KIND, "bridge", sizeof("bridge"));
	*(uint16_t *)(info + ilen) = (uint16_t)(NLA_HDRLEN_ + dlen);
	*(uint16_t *)(info + ilen + 2) = IFLA_INFO_DATA | NLA_F_NESTED;
	memcpy(info + ilen + NLA_HDRLEN_, data, dlen);
	ilen += NL_ALIGN(NLA_HDRLEN_ + dlen);

	memset(req, 0, sizeof(req));
	ifi.ifi_family = AF_UNSPEC;
	ifi.ifi_index = br;
	*(uint16_t *)&req[4] = RTM_NEWLINK;
	*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
	*(uint32_t *)&req[8] = 23;
	memcpy(req + NL_HDRLEN, &ifi, sizeof(ifi));
	off = NL_HDRLEN + sizeof(ifi);
	*(uint16_t *)(req + off) = (uint16_t)(NLA_HDRLEN_ + ilen);
	*(uint16_t *)(req + off + 2) = IFLA_LINKINFO | NLA_F_NESTED;
	memcpy(req + off + NLA_HDRLEN_, info, ilen);
	off += NL_ALIGN(NLA_HDRLEN_ + ilen);
	*(uint32_t *)&req[0] = (uint32_t)off;
	err = rtnl_talk(req, off, NULL, 0, NULL);
	if (err) {
		OMCI_WARN("mcast: the timers of %s could not be set: %s (%s)",
			  BRIDGE_IFNAME, strerror(-err), omci_hw_last_extack());
		return OMCI_ERR_HW;
	}
	return OMCI_OK;
}

/*
 * The rate of the UNI towards the subscriber is a tbf at the root of the
 * UNI, which the switch driver takes as the port's egress shaper, in steps
 * of 1000 bytes a second; the burst and the queue shape only what the CPU
 * sends there itself: a hundredth of a second at the rate, at least two
 * frames, and half a second more. The tbf has the backend's own handle, so
 * that taking the limit away removes only a tbf of the backend.
 */
#define UNI_TBF_HANDLE TC_H_MAKE(0x0b70U << 16, 0)
#define UNI_TBF_BURST_MIN 3200U
#define UNI_TBF_BURST_MAX (4U << 20)
#define UNI_TBF_LIMIT_MAX (16U << 20)

static enum omci_status kernel_uni_rate(uint64_t bytes_ps)
{
	uint8_t req[NL_HDRLEN + sizeof(struct tcmsg) + 128] __aligned(4);
	const int uni = (int)if_nametoindex(UNI_IFNAME);
	struct tcmsg tcm = {
		.tcm_family = AF_UNSPEC,
		.tcm_ifindex = uni,
		.tcm_parent = TC_H_ROOT,
		.tcm_handle = UNI_TBF_HANDLE,
	};
	struct tc_tbf_qopt q = {
		.rate = {
			.linklayer = TC_LINKLAYER_ETHERNET,
			.rate = bytes_ps > UINT32_MAX ? UINT32_MAX :
							(uint32_t)bytes_ps,
		},
	};
	uint64_t burst = bytes_ps / 100, limit;
	uint32_t burst32;
	size_t off, opts;
	int err;

	if (uni <= 0)
		return OMCI_ERR_HW;

	memset(req, 0, sizeof(req));
	memcpy(req + NL_HDRLEN, &tcm, sizeof(tcm));
	off = NL_HDRLEN + sizeof(tcm);

	if (bytes_ps == 0) {
		*(uint16_t *)&req[4] = RTM_DELQDISC;
		*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK;
		*(uint32_t *)&req[8] = 23;
		*(uint32_t *)&req[0] = (uint32_t)off;
		err = rtnl_talk(req, off, NULL, 0, NULL);
		/* No tbf of the backend at the root: nothing to take away. */
		if (err == -ENOENT || err == -EINVAL)
			err = 0;
	} else {
		if (burst < UNI_TBF_BURST_MIN)
			burst = UNI_TBF_BURST_MIN;
		if (burst > UNI_TBF_BURST_MAX)
			burst = UNI_TBF_BURST_MAX;
		burst32 = (uint32_t)burst;
		limit = burst + bytes_ps / 2;
		q.limit = limit > UNI_TBF_LIMIT_MAX ? UNI_TBF_LIMIT_MAX :
						      (uint32_t)limit;

		*(uint16_t *)&req[4] = RTM_NEWQDISC;
		*(uint16_t *)&req[6] = NLM_F_REQUEST | NLM_F_ACK |
				       NLM_F_CREATE | NLM_F_REPLACE;
		*(uint32_t *)&req[8] = 23;
		off = nla_put(req, off, TCA_KIND, "tbf", sizeof("tbf"));
		opts = off;
		off += NLA_HDRLEN_;
		off = nla_put(req, off, TCA_TBF_PARMS, &q, sizeof(q));
		off = nla_put(req, off, TCA_TBF_BURST, &burst32,
			      sizeof(burst32));
		if (bytes_ps > UINT32_MAX)
			off = nla_put(req, off, TCA_TBF_RATE64, &bytes_ps,
				      sizeof(bytes_ps));
		*(uint16_t *)(req + opts) = (uint16_t)(off - opts);
		*(uint16_t *)(req + opts + 2) = TCA_OPTIONS | NLA_F_NESTED;
		*(uint32_t *)&req[0] = (uint32_t)off;
		err = rtnl_talk(req, off, NULL, 0, NULL);
	}
	if (err) {
		OMCI_WARN(
			"uni: the kernel did not set the rate of %s to %llu bytes/s: %s",
			UNI_IFNAME, (unsigned long long)bytes_ps,
			strerror(-err));
		return OMCI_ERR_HW;
	}
	return OMCI_OK;
}

const struct omci_hw_ops omci_hw_kernel_ops = {
	.caps = kernel_caps,
	.source = kernel_source,
	.vlan_apply = kernel_vlan_apply,
	.gem_apply = kernel_gem_apply,
	.tcont_apply = kernel_tcont_apply,
	.bridge_apply = kernel_bridge_apply,
	.alarms = kernel_alarms,
	.pon = kernel_pon,
	.port_counters = kernel_port_counters,
	.gem_counts = kernel_gem_counts,
	.optics = kernel_optics,
	.optics_limits = kernel_optics_limits,
	.set_router = omci_hw_kernel_router,
	.routing = kernel_routing,
	.reprogram_due = kernel_reprogram_due,
	.fdb = kernel_fdb,
	.mcast_port = kernel_mcast_port,
	.mcast_groups = kernel_mcast_groups,
	.mcast_group_add = kernel_mcast_group_add,
	.mcast_group_del = kernel_mcast_group_del,
	.mcast_fd = kernel_mcast_fd,
	.mcast_frame = kernel_mcast_frame,
	.mcast_send = kernel_mcast_send,
	.mcast_mac = kernel_mcast_mac,
	.mcast_ds = kernel_mcast_ds,
	.filter_apply = kernel_filter_apply,
	.mcast_watch = kernel_mcast_watch,
	.mcast_seen = kernel_mcast_seen,
	.uni_rate = kernel_uni_rate,
	.mode_done = kernel_mode_done,
	.time_of_day = kernel_time_of_day,
	.tod_info = kernel_tod_info,
};
