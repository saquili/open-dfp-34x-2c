// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The PPPoE discovery (RFC 2516, section 5). */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <linux/if_packet.h>

#include "pppoed.h"

/* The Host-Uniq tag of this client, so that it takes only its own answers. */
static uint8_t g_uniq[4];

static void uniq_init(unsigned int ifindex)
{
	unsigned long v = (unsigned long)getpid() << 8 | (ifindex & 0xff);

	g_uniq[0] = (uint8_t)(v >> 24);
	g_uniq[1] = (uint8_t)(v >> 16);
	g_uniq[2] = (uint8_t)(v >> 8);
	g_uniq[3] = (uint8_t)v;
}

int pppoe_disc_open(const char *ifname, struct pppoe_session *s,
		    bool all_protocols)
{
	uint16_t proto = all_protocols ? ETH_P_ALL : ETH_P_PPP_DISC;
	struct sockaddr_ll sll = { 0 };
	struct ifreq ifr = { 0 };
	int fd;

	if (!ifname || !s || strlen(ifname) >= IFNAMSIZ)
		return -EINVAL;

	memset(s, 0, sizeof(*s));
	snprintf(s->ifname, sizeof(s->ifname), "%s", ifname);

	fd = socket(AF_PACKET, SOCK_RAW, htons(proto));
	if (fd < 0)
		return -errno;

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
		goto fail;
	s->ifindex = (unsigned int)ifr.ifr_ifindex;

	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
		goto fail;
	memcpy(s->local, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(proto);
	sll.sll_ifindex = (int)s->ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0)
		goto fail;

	uniq_init(s->ifindex);
	return fd;

fail: {
	int err = -errno;

	close(fd);
	return err;
}
}

/* Start a discovery packet; frame_finish() writes its length. */
static size_t frame_start(uint8_t *buf, const uint8_t *dst, const uint8_t *src,
			  uint8_t code, uint16_t sid)
{
	memcpy(buf, dst, ETH_ALEN);
	memcpy(buf + ETH_ALEN, src, ETH_ALEN);
	pppoe_put16(buf + 2 * ETH_ALEN, ETH_P_PPP_DISC);

	buf[ETH_HLEN + 0] = PPPOE_VER_TYPE;
	buf[ETH_HLEN + 1] = code;
	pppoe_put16(buf + ETH_HLEN + 2, sid);
	pppoe_put16(buf + ETH_HLEN + 4, 0);

	return ETH_HLEN + PPPOE_HDR_LEN;
}

static size_t tag_put(uint8_t *buf, size_t off, uint16_t type, const void *val,
		      size_t len)
{
	pppoe_put16(buf + off, type);
	pppoe_put16(buf + off + 2, (uint16_t)len);
	if (len)
		memcpy(buf + off + 4, val, len);
	return off + 4 + len;
}

static void frame_finish(uint8_t *buf, size_t off)
{
	pppoe_put16(buf + ETH_HLEN + 4,
		    (uint16_t)(off - ETH_HLEN - PPPOE_HDR_LEN));
}

static int frame_send(int fd, unsigned int ifindex, const uint8_t *buf,
		      size_t len)
{
	struct sockaddr_ll sll = { 0 };

	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_PPP_DISC);
	sll.sll_ifindex = (int)ifindex;
	sll.sll_halen = ETH_ALEN;
	memcpy(sll.sll_addr, buf, ETH_ALEN);

	if (sendto(fd, buf, len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0)
		return -errno;
	return 0;
}

/* The tags of a discovery packet that the client uses. */
struct disc_tags {
	bool have_uniq;
	bool uniq_ours;
	const uint8_t *cookie;
	size_t cookie_len;
	const uint8_t *relay;
	size_t relay_len;
	char ac_name[64];
	char error[128];
	bool have_error;
};

static bool tags_parse(const uint8_t *pl, size_t len, struct disc_tags *t)
{
	size_t off = 0;

	memset(t, 0, sizeof(*t));

	while (off + 4 <= len) {
		uint16_t type = pppoe_get16(pl + off);
		uint16_t tlen = pppoe_get16(pl + off + 2);
		const uint8_t *val = pl + off + 4;

		if (type == PPPOE_TAG_END_OF_LIST)
			break;
		if (off + 4 + tlen > len)
			return false;

		switch (type) {
		case PPPOE_TAG_HOST_UNIQ:
			t->have_uniq = true;
			t->uniq_ours = tlen == sizeof(g_uniq) &&
				       memcmp(val, g_uniq, sizeof(g_uniq)) == 0;
			break;
		case PPPOE_TAG_AC_COOKIE:
			if (tlen <= PPPOE_COOKIE_MAX) {
				t->cookie = val;
				t->cookie_len = tlen;
			}
			break;
		case PPPOE_TAG_RELAY_SESSION:
			t->relay = val;
			t->relay_len = tlen;
			break;
		case PPPOE_TAG_AC_NAME:
			if (tlen < sizeof(t->ac_name)) {
				memcpy(t->ac_name, val, tlen);
				t->ac_name[tlen] = '\0';
			}
			break;

		case PPPOE_TAG_SERVICE_ERR:
		case PPPOE_TAG_AC_SYSTEM_ERR:
		case PPPOE_TAG_GENERIC_ERR:
			t->have_error = true;
			{
				size_t n = tlen < sizeof(t->error) - 1 ?
						   tlen :
						   sizeof(t->error) - 1;

				memcpy(t->error, val, n);
				t->error[n] = '\0';
			}
			break;
		default:
			break;
		}
		off += 4 + tlen;
	}
	return true;
}

long pppoed_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#define now_ms() pppoed_now_ms()

int pppoed_wait_frame(int fd, void *buf, size_t bufsz, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	ssize_t n;
	int r;

	r = poll(&pfd, 1, timeout_ms);
	if (r < 0)
		return errno == EINTR ? -EINTR : -errno;
	if (r == 0)
		return -ETIMEDOUT;
	n = recv(fd, buf, bufsz, 0);
	return n < 0 ? -errno : (int)n;
}

/*
 * Wait for a discovery packet of @code addressed to us, with our Host-Uniq
 * if it carries one.
 *
 * Return: 0, -ETIMEDOUT, or a negative errno.
 */
static int wait_for(int fd, uint8_t code, const struct pppoe_session *s,
		    long deadline, uint8_t *buf, size_t bufsz,
		    struct disc_tags *tags, uint8_t peer[ETH_ALEN],
		    uint16_t *sid)
{
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long left = deadline - now_ms();
		ssize_t n;
		int r;

		if (left <= 0)
			return -ETIMEDOUT;

		r = poll(&pfd, 1, (int)left);
		if (r < 0)
			return errno == EINTR ? -EINTR : -errno;
		if (r == 0)
			return -ETIMEDOUT;

		n = recv(fd, buf, bufsz, 0);
		if (n < 0)
			return -errno;

		PPPOED_DEBUG(
			"rx %zd B dst %02x:%02x:%02x:%02x:%02x:%02x src %02x:%02x:%02x:%02x:%02x:%02x type %04x ver/type %02x code %02x",
			n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
			buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
			(size_t)n >= 14 ? pppoe_get16(buf + 12) : 0,
			(size_t)n > ETH_HLEN ? buf[ETH_HLEN] : 0,
			(size_t)n > ETH_HLEN + 1 ? buf[ETH_HLEN + 1] : 0);

		if ((size_t)n < ETH_HLEN + PPPOE_HDR_LEN) {
			PPPOED_DEBUG("  dropped: shorter than a PPPoE header");
			continue;
		}

		if (memcmp(buf + ETH_ALEN, s->local, ETH_ALEN) == 0) {
			PPPOED_DEBUG("  ours, outgoing");
			continue;
		}
		if (memcmp(buf, s->local, ETH_ALEN) != 0) {
			PPPOED_DEBUG("  dropped: not addressed to this MAC");
			continue;
		}
		if (buf[ETH_HLEN] != PPPOE_VER_TYPE) {
			PPPOED_DEBUG("  dropped: VER/TYPE is not 0x11");
			continue;
		}
		if (buf[ETH_HLEN + 1] != code) {
			PPPOED_DEBUG("  dropped: code %02x, waiting for %02x",
				     buf[ETH_HLEN + 1], code);
			continue;
		}

		{
			uint16_t plen = pppoe_get16(buf + ETH_HLEN + 4);

			if ((size_t)n <
			    (size_t)(ETH_HLEN + PPPOE_HDR_LEN) + plen)
				continue;
			if (!tags_parse(buf + ETH_HLEN + PPPOE_HDR_LEN, plen,
					tags))
				continue;
		}
		if (tags->have_uniq && !tags->uniq_ours) {
			PPPOED_DEBUG("  dropped: another host's Host-Uniq");
			continue;
		}

		memcpy(peer, buf + ETH_ALEN, ETH_ALEN);
		*sid = pppoe_get16(buf + ETH_HLEN + 2);
		return 0;
	}
}

int pppoe_discover(int fd, struct pppoe_session *s, const char *service,
		   unsigned int attempts, unsigned int timeout_ms)
{
	static const uint8_t bcast[ETH_ALEN] = { 0xff, 0xff, 0xff,
						 0xff, 0xff, 0xff };
	uint8_t tx[PPPOE_FRAME_MAX];
	uint8_t rx[PPPOE_FRAME_MAX];
	size_t svc_len = service ? strlen(service) : 0;
	unsigned int try;

	if (!s || svc_len >= sizeof(s->service))
		return -EINVAL;
	snprintf(s->service, sizeof(s->service), "%s", service ? service : "");

	for (try = 1; try <= attempts; try++) {
		struct disc_tags tags;
		uint8_t peer[ETH_ALEN];
		uint16_t sid = 0;
		size_t off;
		long deadline;
		int err;

		off = frame_start(tx, bcast, s->local, PPPOE_CODE_PADI, 0);
		off = tag_put(tx, off, PPPOE_TAG_SERVICE_NAME, service,
			      svc_len);
		off = tag_put(tx, off, PPPOE_TAG_HOST_UNIQ, g_uniq,
			      sizeof(g_uniq));
		frame_finish(tx, off);

		err = frame_send(fd, s->ifindex, tx, off);
		if (err)
			return err;
		PPPOED_DEBUG("PADI sent on %s, attempt %u of %u", s->ifname,
			     try, attempts);

		deadline = now_ms() + (long)timeout_ms;
		err = wait_for(fd, PPPOE_CODE_PADO, s, deadline, rx, sizeof(rx),
			       &tags, peer, &sid);
		if (err == -ETIMEDOUT)
			continue;
		if (err)
			return err;
		if (tags.have_error) {
			PPPOED_ERR("the concentrator refused the PADI: %s",
				   tags.error);
			return -ECONNREFUSED;
		}

		memcpy(s->peer, peer, ETH_ALEN);
		snprintf(s->ac_name, sizeof(s->ac_name), "%s", tags.ac_name);
		PPPOED_INFO("PADO from %02x:%02x:%02x:%02x:%02x:%02x%s%s",
			    peer[0], peer[1], peer[2], peer[3], peer[4],
			    peer[5],
			    tags.ac_name[0] ? ", access concentrator " : "",
			    tags.ac_name);

		off = frame_start(tx, s->peer, s->local, PPPOE_CODE_PADR, 0);
		off = tag_put(tx, off, PPPOE_TAG_SERVICE_NAME, service,
			      svc_len);
		off = tag_put(tx, off, PPPOE_TAG_HOST_UNIQ, g_uniq,
			      sizeof(g_uniq));
		if (tags.cookie)
			off = tag_put(tx, off, PPPOE_TAG_AC_COOKIE, tags.cookie,
				      tags.cookie_len);
		if (tags.relay)
			off = tag_put(tx, off, PPPOE_TAG_RELAY_SESSION,
				      tags.relay, tags.relay_len);
		frame_finish(tx, off);

		err = frame_send(fd, s->ifindex, tx, off);
		if (err)
			return err;
		PPPOED_DEBUG("PADR sent%s",
			     tags.cookie ?
				     " with the concentrator's cookie echoed" :
				     "");

		deadline = now_ms() + (long)timeout_ms;
		err = wait_for(fd, PPPOE_CODE_PADS, s, deadline, rx, sizeof(rx),
			       &tags, peer, &sid);
		if (err == -ETIMEDOUT)
			continue;
		if (err)
			return err;
		if (tags.have_error) {
			PPPOED_ERR("the concentrator refused the PADR: %s",
				   tags.error);
			return -ECONNREFUSED;
		}

		if (sid == 0) {
			PPPOED_ERR(
				"the PADS carries session id 0, which is not a session");
			return -EPROTO;
		}

		s->sid = sid;
		PPPOED_INFO(
			"session %u up on %s to %02x:%02x:%02x:%02x:%02x:%02x",
			sid, s->ifname, s->peer[0], s->peer[1], s->peer[2],
			s->peer[3], s->peer[4], s->peer[5]);
		return 0;
	}

	return -ETIMEDOUT;
}

int pppoe_terminate(int fd, const struct pppoe_session *s)
{
	uint8_t tx[PPPOE_FRAME_MAX];
	size_t off;

	if (!s || !s->sid)
		return -EINVAL;

	off = frame_start(tx, s->peer, s->local, PPPOE_CODE_PADT, s->sid);
	off = tag_put(tx, off, PPPOE_TAG_HOST_UNIQ, g_uniq, sizeof(g_uniq));
	frame_finish(tx, off);

	return frame_send(fd, s->ifindex, tx, off);
}
