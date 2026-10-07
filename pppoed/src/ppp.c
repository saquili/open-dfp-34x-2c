// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The kernel's side of a PPPoE session: the PPPoE socket and the PPP unit. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <linux/if_pppox.h>
#include <linux/ppp_defs.h>
#include <linux/ppp-ioctl.h>

#include "pppoed.h"

int ppp_open(const struct pppoe_session *s, struct ppp_link *l)
{
	struct sockaddr_pppox sp = { 0 };
	int chindex, err;

	if (!s || !l || !s->sid)
		return -EINVAL;

	memset(l, 0, sizeof(*l));
	l->sk = l->chfd = l->fd = -1;
	l->unit = -1;
	l->mru = PPPOE_MTU;
	l->max_payload = s->max_payload > PPPOE_MTU ? s->max_payload : PPPOE_MTU;

	l->sk = socket(AF_PPPOX, SOCK_STREAM, PX_PROTO_OE);
	if (l->sk < 0) {
		err = -errno;
		PPPOED_ERR("socket(AF_PPPOX): %s -- is CONFIG_PPPOE built in?",
			   strerror(-err));
		return err;
	}

	sp.sa_family = AF_PPPOX;
	sp.sa_protocol = PX_PROTO_OE;
	sp.sa_addr.pppoe.sid = htons(s->sid);
	memcpy(sp.sa_addr.pppoe.remote, s->peer, ETH_ALEN);
	snprintf(sp.sa_addr.pppoe.dev, sizeof(sp.sa_addr.pppoe.dev), "%s",
		 s->ifname);

	if (connect(l->sk, (struct sockaddr *)&sp, sizeof(sp)) < 0) {
		err = -errno;
		PPPOED_ERR("connect(session %u, %s): %s", s->sid, s->ifname,
			   strerror(-err));
		goto fail;
	}

	if (ioctl(l->sk, PPPIOCGCHAN, &chindex) < 0) {
		err = -errno;
		PPPOED_ERR("PPPIOCGCHAN: %s", strerror(-err));
		goto fail;
	}

	l->chfd = open("/dev/ppp", O_RDWR);
	if (l->chfd < 0) {
		err = -errno;
		PPPOED_ERR("/dev/ppp: %s -- is CONFIG_PPP built in?",
			   strerror(-err));
		goto fail;
	}
	if (ioctl(l->chfd, PPPIOCATTCHAN, &chindex) < 0) {
		err = -errno;
		PPPOED_ERR("PPPIOCATTCHAN channel %d: %s", chindex,
			   strerror(-err));
		goto fail;
	}

	l->fd = open("/dev/ppp", O_RDWR);
	if (l->fd < 0) {
		err = -errno;
		PPPOED_ERR("second open of /dev/ppp: %s", strerror(-err));
		goto fail;
	}
	l->unit = -1;
	if (ioctl(l->fd, PPPIOCNEWUNIT, &l->unit) < 0) {
		err = -errno;
		PPPOED_ERR(
			"PPPIOCNEWUNIT: %s -- every ppp unit in use, or no CAP_NET_ADMIN",
			strerror(-err));
		goto fail;
	}
	if (ioctl(l->chfd, PPPIOCCONNECT, &l->unit) < 0) {
		err = -errno;
		PPPOED_ERR("PPPIOCCONNECT unit %d: %s", l->unit,
			   strerror(-err));
		goto fail;
	}

	snprintf(l->ifname, sizeof(l->ifname), "ppp%d", l->unit);

	{
		int mru = l->max_payload;

		if (ioctl(l->fd, PPPIOCSMRU, &mru) < 0)
			PPPOED_WARN("could not set the unit's MRU: %s",
				    strerror(errno));
	}

	PPPOED_INFO("session %u is channel %d and %s", s->sid, chindex,
		    l->ifname);
	return 0;

fail:
	ppp_close(l);
	return err;
}

void ppp_close(struct ppp_link *l)
{
	if (!l)
		return;
	if (l->fd >= 0)
		close(l->fd);
	if (l->chfd >= 0)
		close(l->chfd);
	if (l->sk >= 0)
		close(l->sk);
	l->fd = l->chfd = l->sk = -1;
}

int ppp_send(struct ppp_link *l, uint16_t proto, const void *body, size_t len)
{
	uint8_t buf[PPP_CTRL_MAX + 2];
	ssize_t n;

	if (len > PPP_CTRL_MAX)
		return -EMSGSIZE;

	pppoe_put16(buf, proto);
	if (len)
		memcpy(buf + 2, body, len);

	n = write(l->fd, buf, len + 2);
	if (n < 0)
		return -errno;
	return 0;
}

int ppp_recv(struct ppp_link *l, uint16_t *proto, void *buf, size_t bufsz,
	     int timeout_ms)
{
	struct pollfd pfd[2] = {
		{ .fd = l->chfd, .events = POLLIN },
		{ .fd = l->fd, .events = POLLIN },
	};
	uint8_t frame[PPP_CTRL_MAX + 2];
	ssize_t n;
	int r, which;

	r = poll(pfd, 2, timeout_ms);
	if (r < 0)
		return errno == EINTR ? -EINTR : -errno;
	if (r == 0)
		return -ETIMEDOUT;

	which = (pfd[0].revents & POLLIN) ? 0 : 1;
	n = read(pfd[which].fd, frame, sizeof(frame));
	if (n < 0)
		return -errno;

	if (n == 0)
		return -ENOTCONN;
	if (n < 2)
		return -EPROTO;

	*proto = pppoe_get16(frame);
	n -= 2;
	if ((size_t)n > bufsz)
		return -EMSGSIZE;
	if (n)
		memcpy(buf, frame + 2, (size_t)n);
	return (int)n;
}

/* The kernel's struct in6_ifreq, which the C library does not declare. */
struct ppp_in6_ifreq {
	struct in6_addr addr;
	uint32_t prefixlen;
	int ifindex;
};

/*
 * Let the interface carry IPv6: a system that keeps IPv6 off on interfaces
 * made after its start (net.ipv6.conf.default.disable_ipv6) keeps it off on
 * the ppp unit too, and then no address can go on it. IPv6CP has opened, so
 * this link is to carry IPv6. Return: 0, or a negative errno.
 */
static int ipv6_allow(const char *ifname)
{
	char path[64];
	int fd, err = 0;

	snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/disable_ipv6",
		 ifname);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (write(fd, "0", 1) != 1)
		err = -errno;
	close(fd);
	return err;
}

/* Put fe80::@iid/64, the link-local address, on the interface. */
static int ifconfig_ipv6(const struct ppp_link *l)
{
	struct ppp_in6_ifreq req;
	int fd, err;

	err = ipv6_allow(l->ifname);
	if (err)
		PPPOED_WARN("IPv6 may stay off on %s: %s", l->ifname,
			    strerror(-err));
	err = 0;

	memset(&req, 0, sizeof(req));
	req.ifindex = (int)if_nametoindex(l->ifname);
	if (!req.ifindex)
		return -errno;
	req.addr.s6_addr[0] = 0xfe;
	req.addr.s6_addr[1] = 0x80;
	memcpy(req.addr.s6_addr + 8, l->iid_local, PPP_IID_LEN);
	req.prefixlen = 64;

	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, SIOCSIFADDR, &req) < 0)
		err = -errno;
	close(fd);
	return err;
}

static int ifconfig_ipv4(int fd, struct ifreq *ifr, const struct ppp_link *l)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;

	if (!l->local_ip)
		return -EADDRNOTAVAIL;

	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = l->local_ip;
	if (ioctl(fd, SIOCSIFADDR, ifr) < 0)
		return -errno;

	if (l->peer_ip) {
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = l->peer_ip;
		if (ioctl(fd, SIOCSIFDSTADDR, ifr) < 0)
			return -errno;
	}

	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(0xffffffff);
	if (ioctl(fd, SIOCSIFNETMASK, ifr) < 0)
		return -errno;
	return 0;
}

uint16_t ppp_mtu(const struct ppp_link *l)
{
	if (l->take_payload && l->mru == PPPOE_MTU &&
	    l->max_payload > PPPOE_MTU)
		return l->max_payload;
	return l->mru < l->max_payload ? l->mru : l->max_payload;
}

int ppp_ifconfig(struct ppp_link *l, bool defaultroute, bool pass_on)
{
	struct ifreq ifr = { 0 };
	int fd, err = 0;

	if (!l->ipv4_up && !l->ipv6_up)
		return -EADDRNOTAVAIL;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", l->ifname);
	if (l->ipv4_up && !pass_on) {
		err = ifconfig_ipv4(fd, &ifr, l);
		if (err)
			goto out;
	}

	ifr.ifr_mtu = ppp_mtu(l);
	if (ifr.ifr_mtu > l->mru)
		PPPOED_INFO("the peer asks for an MRU of %u but agreed to PPP packets of %u bytes: %s sends up to %u",
			    l->mru, l->max_payload, l->ifname, ifr.ifr_mtu);
	if (ioctl(fd, SIOCSIFMTU, &ifr) < 0)
		PPPOED_WARN("could not set %s MTU to %d: %s", l->ifname,
			    ifr.ifr_mtu, strerror(errno));

	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
		err = -errno;
		goto out;
	}
	ifr.ifr_flags |= IFF_UP | IFF_POINTOPOINT;
	if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
		err = -errno;
		goto out;
	}

	if (l->ipv6_up) {
		int err6 = ifconfig_ipv6(l);

		if (err6) {
			PPPOED_WARN("no IPv6 link-local address on %s: %s",
				    l->ifname, strerror(-err6));
			l->ipv6_up = false;
			if (!l->ipv4_up)
				err = err6;
		}
	}

	if (defaultroute && l->ipv4_up) {
		struct rtentry rt = { 0 };
		struct sockaddr_in *d = (struct sockaddr_in *)&rt.rt_dst;
		struct sockaddr_in *m = (struct sockaddr_in *)&rt.rt_genmask;

		d->sin_family = AF_INET;
		m->sin_family = AF_INET;
		rt.rt_flags = RTF_UP;
		rt.rt_dev = l->ifname;
		if (ioctl(fd, SIOCADDRT, &rt) < 0)
			PPPOED_WARN("default route via %s refused: %s",
				    l->ifname, strerror(errno));
		else
			PPPOED_INFO("default route via %s", l->ifname);
	}

out:
	close(fd);
	return err;
}

volatile sig_atomic_t pppoed_stop;

int ppp_run(struct ppp_link *l, unsigned int seconds)
{
	uint8_t body[PPP_CTRL_MAX];
	unsigned long elapsed = 0;

	for (;;) {
		uint16_t proto;
		int n;

		if (pppoed_stop)
			return -EINTR;
		if (seconds && elapsed >= seconds * 1000UL)
			return 0;

		n = ppp_recv(l, &proto, body, sizeof(body), 1000);
		if (n == -ETIMEDOUT) {
			elapsed += 1000;
			continue;
		}
		if (n == -EINTR)
			return -EINTR;
		if (n < 0)
			return n;
		if (n < 4)
			continue;

		if (proto == PPP_PROTO_LCP) {
			uint8_t code = body[0], id = body[1];
			uint16_t len = pppoe_get16(body + 2);

			if ((size_t)len > (size_t)n)
				len = (uint16_t)n;

			switch (code) {
			case CP_ECHO_REQ: {
				uint8_t rep[PPP_CTRL_MAX];

				rep[0] = CP_ECHO_REP;
				rep[1] = id;
				pppoe_put16(rep + 2, 8);
				rep[4] = (uint8_t)(l->magic >> 24);
				rep[5] = (uint8_t)(l->magic >> 16);
				rep[6] = (uint8_t)(l->magic >> 8);
				rep[7] = (uint8_t)l->magic;
				ppp_send(l, PPP_PROTO_LCP, rep, 8);
				PPPOED_DEBUG("echo request %u answered", id);
				break;
			}
			case CP_TERM_REQ: {
				uint8_t ack[4] = { CP_TERM_ACK, id, 0, 4 };

				ppp_send(l, PPP_PROTO_LCP, ack, 4);
				PPPOED_INFO("the peer terminated the link");
				return 0;
			}
			case CP_PROTO_REJ:
				PPPOED_WARN("the peer rejected protocol %04x",
					    len >= 6 ? pppoe_get16(body + 4) :
						       0);
				break;
			default:
				PPPOED_DEBUG("LCP code %u in the network phase",
					     code);
				break;
			}
		} else if (proto == PPP_PROTO_IPCP ||
			   (proto == PPP_PROTO_IPV6CP && l->ipv6_up)) {
			PPPOED_DEBUG(
				"%s in the network phase, code %u, ignored",
				proto == PPP_PROTO_IPCP ? "IPCP" : "IPv6CP",
				body[0]);
		} else {
			uint8_t rej[PPP_CTRL_MAX];
			const size_t len = (size_t)n < sizeof(rej) - 6 ?
						   (size_t)n :
						   sizeof(rej) - 6;

			rej[0] = CP_PROTO_REJ;
			rej[1] = body[1];
			pppoe_put16(rej + 2, (uint16_t)(len + 6));
			pppoe_put16(rej + 4, proto);
			memcpy(rej + 6, body, len);
			ppp_send(l, PPP_PROTO_LCP, rej, len + 6);
			PPPOED_DEBUG("protocol %04x rejected", proto);
		}
	}
}

void ppp_terminate(struct ppp_link *l)
{
	uint8_t req[4] = { CP_TERM_REQ, 0xff, 0, 4 };
	uint8_t body[PPP_CTRL_MAX];
	uint16_t proto;

	if (ppp_send(l, PPP_PROTO_LCP, req, 4) < 0)
		return;

	if (ppp_recv(l, &proto, body, sizeof(body), 1000) > 0 &&
	    proto == PPP_PROTO_LCP && body[0] == CP_TERM_ACK)
		PPPOED_INFO("link terminated, the peer acknowledged");
	else
		PPPOED_INFO("terminate sent");
}
