// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the negotiation after authentication: IPCP and IPv6CP, against a
 * peer in a child process at the other end of a socket pair. The peer opens
 * LCP without authentication, then plays one of the scenarios below.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "pppoed.h"

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

enum scenario {
	DUAL, /* both open; the peer gives the IPv4 addresses */
	NAK_OUR_IID, /* the peer naks our identifier with its own offer */
	ZERO_PEER_IID, /* the peer asks with a zero identifier first */
	NO_IPV6, /* the peer protocol-rejects IPv6CP */
	IPV4_ONLY, /* the client does not want IPv6 */
	IPV6_ONLY, /* the peer protocol-rejects IPCP */
	STOPPED, /* both open, then the client is told to stop */
};

static const uint8_t PEER_IID[PPP_IID_LEN] = { 2, 0, 0, 0, 0, 0, 0, 1 };
static const uint8_t OFFER_IID[PPP_IID_LEN] = { 0, 0x5e, 0, 0x53, 0, 0, 0, 7 };
static const uint8_t ZERO_IID[PPP_IID_LEN];

/* One frame from the peer: the protocol, then the control packet. */
static void peer_send(int fd, uint16_t proto, uint8_t code, uint8_t id,
		      const uint8_t *opts, size_t len)
{
	uint8_t f[PPP_CTRL_MAX];

	pppoe_put16(f, proto);
	f[2] = code;
	f[3] = id;
	pppoe_put16(f + 4, (uint16_t)(len + 4));
	if (len)
		memcpy(f + 6, opts, len);
	if (write(fd, f, len + 6) < 0)
		_exit(10);
}

static void peer_iid_req(int fd, uint8_t id, const uint8_t *iid)
{
	uint8_t o[2 + PPP_IID_LEN] = { IPV6CP_OPT_IFACE_ID, 2 + PPP_IID_LEN };

	memcpy(o + 2, iid, PPP_IID_LEN);
	peer_send(fd, PPP_PROTO_IPV6CP, CP_CONF_REQ, id, o, sizeof(o));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

/*
 * The peer. It exits 0 when what the client sent was right for the
 * scenario, and with the number of the first thing that was not.
 */
static void peer(int fd, enum scenario sc)
{
	static const uint8_t lcp_req[] = { LCP_OPT_MAGIC, 6,	0x11,
					   0x22,	  0x33, 0x44 };
	static const uint8_t ipcp_req[] = { IPCP_OPT_ADDRESS, 6, 192, 0, 2, 1 };
	bool lcp_open = false, ncp_started = false, saw_term = false;
	bool saw_v6_reject = false, naked_iid = false, sent_zero = false;

	peer_send(fd, PPP_PROTO_LCP, CP_CONF_REQ, 1, lcp_req, sizeof(lcp_req));
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		uint8_t f[PPP_CTRL_MAX];
		ssize_t n;

		if (poll(&p, 1, 10000) <= 0)
			_exit(11);
		n = read(fd, f, sizeof(f));
		if (n == 0)
			break;
		if (n < 6)
			_exit(12);

		const uint16_t proto = pppoe_get16(f);
		const uint8_t code = f[2], id = f[3];
		const uint8_t *opts = f + 6;
		const size_t olen = (size_t)pppoe_get16(f + 4) - 4;

		if (proto == PPP_PROTO_LCP) {
			if (code == CP_CONF_REQ)
				peer_send(fd, proto, CP_CONF_ACK, id, opts,
					  olen);
			else if (code == CP_TERM_REQ) {
				peer_send(fd, proto, CP_TERM_ACK, id, NULL, 0);
				saw_term = true;
			} else if (code == CP_CONF_ACK)
				lcp_open = true;
			else if (code == CP_PROTO_REJ &&
				 pppoe_get16(opts) == PPP_PROTO_IPV6CP)
				saw_v6_reject = true;
		} else if (proto == PPP_PROTO_IPCP && code == CP_CONF_REQ) {
			if (sc == IPV6_ONLY) {
				uint8_t rej[PPP_CTRL_MAX];

				pppoe_put16(rej, proto);
				memcpy(rej + 2, f + 2, (size_t)n - 2);
				peer_send(fd, PPP_PROTO_LCP, CP_PROTO_REJ, 9,
					  rej, (size_t)n);
			} else if (olen >= 6 && get32(opts + 2) == 0) {
				static const uint8_t nak[] = {
					IPCP_OPT_ADDRESS, 6, 192, 0,  2,   2,
					IPCP_OPT_DNS1,	  6, 198, 51, 100, 53,
					IPCP_OPT_DNS2,	  6, 198, 51, 100, 54
				};

				peer_send(fd, proto, CP_CONF_NAK, id, nak,
					  sizeof(nak));
			} else {
				peer_send(fd, proto, CP_CONF_ACK, id, opts,
					  olen);
			}
		} else if (proto == PPP_PROTO_IPV6CP && code == CP_CONF_REQ) {
			if (sc == IPV4_ONLY)
				_exit(13);
			if (sc == NO_IPV6) {
				uint8_t rej[PPP_CTRL_MAX];

				pppoe_put16(rej, proto);
				memcpy(rej + 2, f + 2, (size_t)n - 2);
				peer_send(fd, PPP_PROTO_LCP, CP_PROTO_REJ, 9,
					  rej, (size_t)n);
			} else if (olen != 2 + PPP_IID_LEN ||
				   opts[0] != IPV6CP_OPT_IFACE_ID ||
				   !memcmp(opts + 2, ZERO_IID, PPP_IID_LEN)) {
				_exit(14);
			} else if (sc == NAK_OUR_IID && !naked_iid) {
				uint8_t nak[2 + PPP_IID_LEN] = {
					IPV6CP_OPT_IFACE_ID, 2 + PPP_IID_LEN
				};

				memcpy(nak + 2, OFFER_IID, PPP_IID_LEN);
				peer_send(fd, proto, CP_CONF_NAK, id, nak,
					  sizeof(nak));
				naked_iid = true;
			} else {
				if (sc == NAK_OUR_IID &&
				    memcmp(opts + 2, OFFER_IID, PPP_IID_LEN))
					_exit(15);
				peer_send(fd, proto, CP_CONF_ACK, id, opts,
					  olen);
			}
		} else if (proto == PPP_PROTO_IPV6CP && code == CP_CONF_NAK) {
			/* The client refused our zero identifier: take its offer. */
			if (sc != ZERO_PEER_IID || olen != 2 + PPP_IID_LEN ||
			    !memcmp(opts + 2, ZERO_IID, PPP_IID_LEN))
				_exit(16);
			peer_iid_req(fd, 22, opts + 2);
		}

		if (lcp_open && !ncp_started) {
			ncp_started = true;
			peer_send(fd, PPP_PROTO_IPCP, CP_CONF_REQ, 20, ipcp_req,
				  sizeof(ipcp_req));
			if (sc == ZERO_PEER_IID) {
				peer_iid_req(fd, 21, ZERO_IID);
				sent_zero = true;
			} else {
				peer_iid_req(fd, 21, PEER_IID);
			}
		}
	}
	if (sc == IPV4_ONLY && !saw_v6_reject)
		_exit(17);
	if (sc == ZERO_PEER_IID && !sent_zero)
		_exit(18);
	if (sc == STOPPED && !saw_term)
		_exit(19);
	_exit(0);
}

static int run(enum scenario sc, struct ppp_link *l)
{
	int sv[2], status = -1, err;
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0)
		return -errno;
	pid = fork();
	if (pid == 0) {
		close(sv[0]);
		peer(sv[1], sc);
	}
	close(sv[1]);

	memset(l, 0, sizeof(*l));
	l->sk = -1;
	l->chfd = -1;
	l->fd = sv[0];
	l->magic = 0x55667788;
	err = ppp_negotiate(l, "", "", sc != IPV4_ONLY);
	if (sc == STOPPED && !err) {
		pppoed_stop = 1;
		err = ppp_run(l, 5) == -EINTR ? 0 : -EPROTO;
		ppp_terminate(l);
		pppoed_stop = 0;
	}
	close(sv[0]);
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		printf("  the peer of scenario %d ended with %d\n", (int)sc,
		       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return -EPROTO;
	}
	return err;
}

int main(void)
{
	struct ppp_link l;

	signal(SIGPIPE, SIG_IGN);

	CHECK(run(DUAL, &l) == 0);
	CHECK(l.ipv4_up && l.ipv6_up);
	CHECK(l.local_ip == htonl(0xc0000202));
	CHECK(l.peer_ip == htonl(0xc0000201));
	CHECK(l.dns1 == htonl(0xc6336435) && l.dns2 == htonl(0xc6336436));
	CHECK(!memcmp(l.iid_peer, PEER_IID, PPP_IID_LEN));
	CHECK(memcmp(l.iid_local, ZERO_IID, PPP_IID_LEN));
	CHECK(memcmp(l.iid_local, PEER_IID, PPP_IID_LEN));

	CHECK(run(NAK_OUR_IID, &l) == 0);
	CHECK(l.ipv6_up);
	CHECK(!memcmp(l.iid_local, OFFER_IID, PPP_IID_LEN));

	CHECK(run(ZERO_PEER_IID, &l) == 0);
	CHECK(l.ipv6_up);
	CHECK(memcmp(l.iid_peer, ZERO_IID, PPP_IID_LEN));
	CHECK(memcmp(l.iid_peer, l.iid_local, PPP_IID_LEN));

	CHECK(run(NO_IPV6, &l) == 0);
	CHECK(l.ipv4_up && !l.ipv6_up);

	CHECK(run(IPV4_ONLY, &l) == 0);
	CHECK(l.ipv4_up && !l.ipv6_up);

	CHECK(run(IPV6_ONLY, &l) == 0);
	CHECK(!l.ipv4_up && l.ipv6_up);

	/* Told to stop, the client leaves the link at once and terminates it. */
	CHECK(run(STOPPED, &l) == 0);

	printf("test_ncp: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
