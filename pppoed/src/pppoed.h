/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * pppoed: a PPPoE client (RFC 2516) with the PPP of RFC 1661 on top: LCP,
 * PAP or CHAP-MD5 (RFC 1334, RFC 1994), IPCP (RFC 1332) and IPv6CP (RFC
 * 5072). The session
 * itself runs in the kernel's PPPoE and PPP drivers; this program runs the
 * discovery and the negotiation, and keeps the link.
 */

#ifndef PPPOED_H
#define PPPOED_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <net/if.h>
#include <linux/if_ether.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The PPPoE header: version and type 1, the code of a discovery packet. */
#define PPPOE_VER_TYPE 0x11
#define PPPOE_HDR_LEN 6
#define PPPOE_CODE_PADI 0x09
#define PPPOE_CODE_PADO 0x07
#define PPPOE_CODE_PADR 0x19
#define PPPOE_CODE_PADS 0x65
#define PPPOE_CODE_PADT 0xa7
#define PPPOE_CODE_SESSION 0x00

/* The tags of a discovery packet (RFC 2516, appendix A). */
#define PPPOE_TAG_END_OF_LIST 0x0000
#define PPPOE_TAG_SERVICE_NAME 0x0101
#define PPPOE_TAG_AC_NAME 0x0102
#define PPPOE_TAG_HOST_UNIQ 0x0103
#define PPPOE_TAG_AC_COOKIE 0x0104
#define PPPOE_TAG_VENDOR_SPEC 0x0105
#define PPPOE_TAG_RELAY_SESSION 0x0110
#define PPPOE_TAG_SERVICE_ERR 0x0201
#define PPPOE_TAG_AC_SYSTEM_ERR 0x0202
#define PPPOE_TAG_GENERIC_ERR 0x0203

/* Sizes: an Ethernet payload less the PPPoE header, the MTU of the link. */
#define PPPOE_MAX_PAYLOAD 1494
#define PPPOE_MTU 1492
#define PPPOE_FRAME_MAX (ETH_HLEN + PPPOE_HDR_LEN + PPPOE_MAX_PAYLOAD)

#define PPPOE_COOKIE_MAX 64

static inline uint16_t pppoe_get16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline void pppoe_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xff);
}

/**
 * struct pppoe_session - A PPPoE session.
 * @sid: its session identifier; 0 until the PADS
 * @peer: the MAC address of the access concentrator
 * @local: the MAC address of the interface
 * @ac_name: the name of the access concentrator, from its PADO
 * @service: the service name asked for
 * @ifindex: the index of the interface
 * @ifname: its name
 */
struct pppoe_session {
	uint16_t sid;
	uint8_t peer[ETH_ALEN];
	uint8_t local[ETH_ALEN];
	char ac_name[64];
	char service[64];
	unsigned int ifindex;
	char ifname[IFNAMSIZ];
};

/**
 * pppoe_disc_open() - Open a socket for PPPoE discovery on an interface.
 * @ifname: the interface
 * @s: the session; filled with the interface's index and address
 * @all_protocols: listen to every ethertype rather than to discovery only
 *
 * Return: the socket, or a negative errno.
 */
int pppoe_disc_open(const char *ifname, struct pppoe_session *s,
		    bool all_protocols);

/**
 * pppoe_discover() - Run the discovery: PADI, PADO, PADR, PADS.
 * @fd: the discovery socket
 * @s: the session; on success, its identifier and peer
 * @service: the service name to ask for; "" for any
 * @attempts: how often to start again when an answer does not come
 * @timeout_ms: how long to wait for each answer
 *
 * Return: 0, -ETIMEDOUT, -ECONNREFUSED when the concentrator sends an
 * error tag, or another negative errno.
 */
int pppoe_discover(int fd, struct pppoe_session *s, const char *service,
		   unsigned int attempts, unsigned int timeout_ms);

/* The monotonic clock in milliseconds, and a wait for one frame. */
long pppoed_now_ms(void);
int pppoed_wait_frame(int fd, void *buf, size_t bufsz, int timeout_ms);

/**
 * pppoe_terminate() - End a session with a PADT.
 * @fd: the discovery socket
 * @s: the session
 *
 * Return: 0, or a negative errno.
 */
int pppoe_terminate(int fd, const struct pppoe_session *s);

/* PPP protocol numbers. */
#define PPP_PROTO_IP 0x0021
#define PPP_PROTO_IPV6 0x0057
#define PPP_PROTO_IPCP 0x8021
#define PPP_PROTO_IPV6CP 0x8057
#define PPP_PROTO_LCP 0xc021
#define PPP_PROTO_PAP 0xc023
#define PPP_PROTO_CHAP 0xc223

/* The codes of the control protocols (RFC 1661, section 5). */
#define CP_CONF_REQ 1
#define CP_CONF_ACK 2
#define CP_CONF_NAK 3
#define CP_CONF_REJ 4
#define CP_TERM_REQ 5
#define CP_TERM_ACK 6
#define CP_CODE_REJ 7
#define CP_PROTO_REJ 8
#define CP_ECHO_REQ 9
#define CP_ECHO_REP 10
#define CP_DISC_REQ 11

/* LCP options, and IPCP options with the name servers of RFC 1877. */
#define LCP_OPT_MRU 1
#define LCP_OPT_AUTH 3
#define LCP_OPT_MAGIC 5
#define LCP_OPT_PFC 7
#define LCP_OPT_ACFC 8

#define IPCP_OPT_ADDRESSES 1
#define IPCP_OPT_COMPRESS 2
#define IPCP_OPT_ADDRESS 3
#define IPCP_OPT_DNS1 129
#define IPCP_OPT_DNS2 131

/* The IPv6CP option of RFC 5072: the interface identifier, of 8 bytes. */
#define IPV6CP_OPT_IFACE_ID 1
#define PPP_IID_LEN 8

/* The CHAP algorithm, and the codes of PAP and of CHAP. */
#define CHAP_MD5 5

#define PAP_AUTH_REQ 1
#define PAP_AUTH_ACK 2
#define PAP_AUTH_NAK 3

#define CHAP_CHALLENGE 1
#define CHAP_RESPONSE 2
#define CHAP_SUCCESS 3
#define CHAP_FAILURE 4

/* The largest control packet. */
#define PPP_CTRL_MAX 1500

/**
 * struct ppp_link - The PPP link of a session.
 * @sk: the PPPoE socket that carries the session
 * @chfd: the PPP channel
 * @fd: the PPP unit, the interface pppN
 * @unit: N
 * @ifname: "pppN"
 * @magic: our magic number
 * @peer_magic: the peer's
 * @mru: the peer's MRU
 * @auth: the authentication protocol the peer asks for, or 0
 * @chap_alg: the CHAP algorithm
 * @local_ip: the address the peer gave us, network order
 * @peer_ip: the peer's address
 * @dns1: the first name server, or 0
 * @dns2: the second
 * @iid_local: our IPv6 interface identifier
 * @iid_peer: the peer's
 * @ipv4_up: IPCP is open
 * @ipv6_up: IPv6CP is open
 * @peer_name: the peer's name
 */
struct ppp_link {
	int sk;
	int chfd;
	int fd;
	int unit;
	char ifname[IFNAMSIZ];
	uint32_t magic;
	uint32_t peer_magic;
	uint16_t mru;
	uint16_t auth;
	uint8_t chap_alg;
	uint32_t local_ip, peer_ip, dns1, dns2;
	uint8_t iid_local[PPP_IID_LEN], iid_peer[PPP_IID_LEN];
	bool ipv4_up, ipv6_up;
	char peer_name[64];
};

/* Open the kernel's side of a session: the PPPoE socket, its channel and a unit. */
int ppp_open(const struct pppoe_session *s, struct ppp_link *l);
/* Close what ppp_open() opened. */
void ppp_close(struct ppp_link *l);

/* Send and receive control packets; ppp_recv() returns the body's length. */
int ppp_send(struct ppp_link *l, uint16_t proto, const void *body, size_t len);
int ppp_recv(struct ppp_link *l, uint16_t *proto, void *buf, size_t bufsz,
	     int timeout_ms);

/*
 * LCP, then the authentication the peer asks for, then IPCP and, with
 * @want_ipv6, IPv6CP.
 */
int ppp_negotiate(struct ppp_link *l, const char *user, const char *secret,
		  bool want_ipv6);

/*
 * Put the negotiated addresses on the interface: the IPv4 address and maybe
 * a default route, and the IPv6 link-local address.
 */
int ppp_ifconfig(struct ppp_link *l, bool defaultroute);

/* Keep the link: answer echo requests until the peer ends it, or @seconds pass. */
int ppp_run(struct ppp_link *l, unsigned int seconds);

/* Ask the peer to end the link. */
void ppp_terminate(struct ppp_link *l);

/* MD5 (RFC 1321), for CHAP: of one buffer, of three in a row, and its own test. */
void md5(const void *data, size_t len, uint8_t out[16]);
void md5_three(const void *a, size_t alen, const void *b, size_t blen,
	       const void *c, size_t clen, uint8_t out[16]);
bool md5_selftest(void);

/* A log line on the standard error, at a syslog level; debug lines need -v. */
void pppoed_log(int level, const char *fmt, ...)
	__attribute((format(printf, 2, 3)));

#define PPPOED_ERR(...) pppoed_log(3, __VA_ARGS__)
#define PPPOED_WARN(...) pppoed_log(4, __VA_ARGS__)
#define PPPOED_INFO(...) pppoed_log(6, __VA_ARGS__)
#define PPPOED_DEBUG(...) pppoed_log(7, __VA_ARGS__)

#endif
