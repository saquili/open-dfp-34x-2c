// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the OLT set up, as omcid holds it: the instances of the managed
 * entity classes that describe the services and the unit, every attribute
 * of each, the rows of their tables, and omcid's own counts of the frames.
 * It is read over omcid's control socket (omcid/include/omcid/ctl_proto.h):
 * a request of 12 bytes, an answer of an 8-byte header and up to 512 bytes,
 * every field big-endian. pond asks for the list of each class's instances
 * (ME_LIST), then for each instance's records (ME_DUMP): an attribute's
 * number, the table bit, the length of its value, the value.
 *
 * A value is written as hexadecimal; the panel knows what the attributes
 * mean. The logical password of the ONU-G is never written, only whether
 * one is set.
 */

#define _GNU_SOURCE

#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define OMCID_SOCKET "/run/omcid.sock"

#define CTL_MAGIC 0x4F4D4349u
#define CTL_REQ_LEN 12u
#define CTL_RSP_LEN 8u
#define CTL_PAYLOAD_MAX 512u
#define CTL_OK 0u
#define CTL_OP_ME_LIST 2u
#define CTL_OP_STATS 3u
#define CTL_OP_ME_DUMP 6u
#define CTL_DUMP_HDR_LEN 3u
#define CTL_DUMP_TABLE 0x80u
#define CTL_TIMEOUT_MS 2000

/* ONU-G, and its attribute that holds the logical password. */
#define CLASS_ONU_G 256u
#define ATTR_LOGICAL_PASSWORD 11u

/* Room kept at the end of the value for the counts and the closing. */
#define TAIL_ROOM 512u

/*
 * The classes read, in the order a reader takes in a unit: what it is, its
 * software, its ports, the bridges, the VLAN rules, the channels and their
 * bandwidth, multicast, and the host interfaces.
 */
static const uint16_t g_classes[] = {
	256, /* ONU-G */
	257, /* ONU2-G */
	7,   /* software image */
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

static bool sendall(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		n -= (size_t)w;
	}
	return true;
}

static bool recvall(int fd, uint8_t *p, size_t n)
{
	while (n > 0) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t r;

		if (poll(&pfd, 1, CTL_TIMEOUT_MS) <= 0)
			return false;
		r = recv(fd, p, n, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

static void be16_put(uint8_t *p, unsigned int v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static unsigned int be16(const uint8_t *p)
{
	return (unsigned int)p[0] << 8 | p[1];
}

static unsigned long be32(const uint8_t *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	       (unsigned long)p[2] << 8 | p[3];
}

/*
 * One request and its answer: the status, or -1 when the socket failed;
 * the payload in @pay, its length in @len.
 */
static int ask(int fd, unsigned int op, unsigned int cls, unsigned int inst,
	       uint8_t *pay, size_t *len)
{
	uint8_t req[CTL_REQ_LEN] = { 0 }, hdr[CTL_RSP_LEN];
	size_t n;

	req[0] = (uint8_t)(CTL_MAGIC >> 24);
	req[1] = (uint8_t)(CTL_MAGIC >> 16);
	req[2] = (uint8_t)(CTL_MAGIC >> 8);
	req[3] = (uint8_t)CTL_MAGIC;
	be16_put(&req[4], op);
	be16_put(&req[6], cls);
	be16_put(&req[8], inst);
	if (!sendall(fd, req, sizeof(req)) || !recvall(fd, hdr, sizeof(hdr)))
		return -1;
	if (be32(hdr) != CTL_MAGIC)
		return -1;
	n = be16(&hdr[6]);
	if (n > CTL_PAYLOAD_MAX || (n && !recvall(fd, pay, n)))
		return -1;
	*len = n;
	return (int)be16(&hdr[4]);
}

/* omcid's socket: POND_OMCID_SOCKET names another, for the tests. */
static int dial(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *sock = getenv("POND_OMCID_SOCKET");
	char buf[sizeof(sa.sun_path)];
	const char *path = prov_path(sock && *sock ? sock : OMCID_SOCKET, buf, sizeof(buf));
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	memcpy(sa.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int err = -errno;

		close(fd);
		return err;
	}
	return fd;
}

static void pv_hex(struct prov_buf *out, const uint8_t *p, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	char s[2 * CTL_PAYLOAD_MAX + 1];
	size_t i;

	for (i = 0; i < n; i++) {
		s[2 * i] = digits[p[i] >> 4];
		s[2 * i + 1] = digits[p[i] & 15];
	}
	s[2 * n] = '\0';
	pv_str(out, s);
}

/*
 * The records of one instance: "attrs", {"1": "hex", ...}, and "tables", the
 * numbers of its table attributes. A record cut short ends the list.
 */
static void pv_records(struct prov_buf *out, unsigned int cls,
		       const uint8_t *pay, size_t len, bool *secret)
{
	size_t at = 0;
	bool first = true;
	char key[8];

	pv_key(out, "attrs");
	pv_open(out, '{');
	while (at + CTL_DUMP_HDR_LEN <= len) {
		unsigned int attr = pay[at] & ~CTL_DUMP_TABLE & 0xffu;
		size_t n = be16(&pay[at + 1]);

		if (at + CTL_DUMP_HDR_LEN + n > len)
			break;
		if (!first)
			pv_comma(out);
		first = false;
		snprintf(key, sizeof(key), "%u", attr);
		pv_key(out, key);
		if (cls == CLASS_ONU_G && attr == ATTR_LOGICAL_PASSWORD) {
			size_t i;

			for (i = 0; i < n; i++)
				*secret |= pay[at + CTL_DUMP_HDR_LEN + i] != 0;
			pv_null(out);
		} else {
			pv_hex(out, &pay[at + CTL_DUMP_HDR_LEN], n);
		}
		at += CTL_DUMP_HDR_LEN + n;
	}
	pv_close(out, '}');

	pv_comma(out);
	pv_key(out, "tables");
	pv_open(out, '[');
	first = true;
	for (size_t t = 0; t + CTL_DUMP_HDR_LEN <= len;
	     t += CTL_DUMP_HDR_LEN + be16(&pay[t + 1])) {
		if (!(pay[t] & CTL_DUMP_TABLE))
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_num(out, pay[t] & ~CTL_DUMP_TABLE & 0xffu);
	}
	pv_close(out, ']');
}

int prov_omci(struct prov_buf *out)
{
	uint8_t pay[CTL_PAYLOAD_MAX], list[CTL_PAYLOAD_MAX];
	bool secret = false, truncated = false, firstme = true;
	size_t len, nlist, i, k;
	int fd = dial(), st;

	if (fd < 0)
		return fd;

	pv_open(out, '{');
	pv_key(out, "mes");
	pv_open(out, '[');
	for (k = 0; k < ARRAY_SIZE(g_classes) && !truncated; k++) {
		st = ask(fd, CTL_OP_ME_LIST, g_classes[k], 0, list, &nlist);
		if (st < 0)
			goto broken;
		if (st != CTL_OK)
			continue;
		for (i = 0; i + 2 <= nlist; i += 2) {
			unsigned int inst = be16(&list[i]);

			if (out->cap - out->len < TAIL_ROOM + 2 * CTL_PAYLOAD_MAX + 256) {
				truncated = true;
				break;
			}
			st = ask(fd, CTL_OP_ME_DUMP, g_classes[k], inst, pay, &len);
			if (st < 0)
				goto broken;
			if (!firstme)
				pv_comma(out);
			firstme = false;
			pv_open(out, '{');
			pv_key(out, "class");
			pv_num(out, g_classes[k]);
			pv_comma(out);
			pv_key(out, "instance");
			pv_num(out, inst);
			pv_comma(out);
			if (st == CTL_OK) {
				pv_records(out, g_classes[k], pay, len, &secret);
			} else {
				/* Too big to dump, or gone since the list. */
				pv_key(out, "error");
				pv_num(out, st);
			}
			pv_close(out, '}');
		}
	}
	pv_close(out, ']');

	pv_comma(out);
	pv_key(out, "truncated");
	pv_bool(out, truncated);
	pv_comma(out);
	pv_key(out, "logical_password_set");
	pv_bool(out, secret);
	pv_comma(out);
	pv_key(out, "frames");
	if (ask(fd, CTL_OP_STATS, 0, 0, pay, &len) == CTL_OK && len >= 12) {
		pv_open(out, '{');
		pv_key(out, "handled");
		pv_num(out, (long long)be32(&pay[0]));
		pv_comma(out);
		pv_key(out, "rejected");
		pv_num(out, (long long)be32(&pay[4]));
		pv_comma(out);
		pv_key(out, "answered");
		pv_num(out, (long long)be32(&pay[8]));
		pv_close(out, '}');
	} else {
		pv_null(out);
	}
	pv_close(out, '}');
	close(fd);
	return 0;

broken:
	close(fd);
	return -EPROTO;
}
