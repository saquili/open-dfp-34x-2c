/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * dhcp6d: the IPv6 of a PPPoE session handed on. On the session's link it is
 * a DHCPv6 client (RFC 8415) that asks for a delegated prefix (IA_PD) and the
 * name servers (RFC 3646); on the link to the host behind the stick it is a
 * DHCPv6 server that delegates that prefix to the host, and a router that
 * advertises itself (RFC 4861) with the link's MTU and the name servers
 * (RFC 8106). The stick routes the prefix to the host, and its default route
 * goes out by the session.
 *
 * The protocol is in plain functions over bytes and a clock in milliseconds,
 * which the tests drive; main.c holds the sockets, the routes and the files.
 */

#ifndef DHCP6D_H
#define DHCP6D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The ports and the address of every server and relay on a link. */
#define DHCP6_CLIENT_PORT 546
#define DHCP6_SERVER_PORT 547
#define DHCP6_ALL_SERVERS "ff02::1:2"

/* Message types (RFC 8415, section 7.3). */
#define DHCP6_SOLICIT 1
#define DHCP6_ADVERTISE 2
#define DHCP6_REQUEST 3
#define DHCP6_CONFIRM 4
#define DHCP6_RENEW 5
#define DHCP6_REBIND 6
#define DHCP6_REPLY 7
#define DHCP6_RELEASE 8
#define DHCP6_DECLINE 9
#define DHCP6_INFORMATION_REQUEST 11

/* Options (RFC 8415, section 21; RFC 3646). */
#define DHCP6_OPT_CLIENTID 1
#define DHCP6_OPT_SERVERID 2
#define DHCP6_OPT_IA_NA 3
#define DHCP6_OPT_ORO 6
#define DHCP6_OPT_PREFERENCE 7
#define DHCP6_OPT_ELAPSED_TIME 8
#define DHCP6_OPT_STATUS_CODE 13
#define DHCP6_OPT_RAPID_COMMIT 14
#define DHCP6_OPT_DNS_SERVERS 23
#define DHCP6_OPT_IA_PD 25
#define DHCP6_OPT_IAPREFIX 26

/* Status codes (RFC 8415, section 21.13). */
#define DHCP6_SUCCESS 0
#define DHCP6_UNSPEC_FAIL 1
#define DHCP6_NO_BINDING 3
#define DHCP6_NO_PREFIX_AVAIL 6

/* Transmission parameters, in milliseconds (RFC 8415, section 7.6). */
#define DHCP6_SOL_TIMEOUT 1000
#define DHCP6_SOL_MAX_RT 3600000
#define DHCP6_REQ_TIMEOUT 1000
#define DHCP6_REQ_MAX_RT 30000
#define DHCP6_REQ_MAX_RC 10
#define DHCP6_REN_TIMEOUT 10000
#define DHCP6_REN_MAX_RT 600000
#define DHCP6_REB_TIMEOUT 10000
#define DHCP6_REB_MAX_RT 600000

/* The infinite lifetime. */
#define DHCP6_INFINITY 0xffffffffu

#define DHCP6_MSG_MAX 1232 /* the IPv6 minimum MTU less the headers */
#define DHCP6_DUID_MAX 130
#define DHCP6_DNS_MAX 3

static inline uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/*
 * msg.c: a message built option by option, and the options of a received
 * one found by code. A builder that runs out of room says so in @overflow,
 * and its message is not sent.
 */
struct msg {
	uint8_t *buf;
	size_t cap;
	size_t len;
	bool overflow;
	/* where the open nested option starts, or 0 */
	size_t nest[2];
	unsigned int depth;
};

void msg_start(struct msg *m, uint8_t *buf, size_t cap, uint8_t type,
	       uint32_t xid);
void msg_opt(struct msg *m, uint16_t code, const void *val, size_t len);
void msg_opt16(struct msg *m, uint16_t code, uint16_t v);
void msg_open(struct msg *m, uint16_t code);
void msg_raw(struct msg *m, const void *val, size_t len);
void msg_close(struct msg *m);

/* The option @code at the top level of @opts, or NULL; @len its length. */
const uint8_t *opt_find(const uint8_t *opts, size_t olen, uint16_t code,
			size_t *len);
/* The status code within @opts: SUCCESS when it carries none. */
uint16_t opt_status(const uint8_t *opts, size_t olen);

/* The DUID-LL of a MAC address (RFC 8415, section 11.4): 10 bytes. */
size_t duid_ll(uint8_t *out, const uint8_t mac[6]);

/*
 * A delegated prefix, with the times it is preferred and valid until, and
 * when its server wants to hear from the client (in ms of the clock).
 */
struct prefix {
	uint8_t addr[16];
	uint8_t len;
	int64_t preferred_until;
	int64_t valid_until;
};

/* client.c */
enum client_state {
	CLIENT_SOLICIT,
	CLIENT_REQUEST,
	CLIENT_BOUND,
	CLIENT_RENEW,
	CLIENT_REBIND,
};

/* What a step of the client brought about. */
#define CLIENT_EV_BOUND 1u /* a prefix came, or another one */
#define CLIENT_EV_LOST 2u /* the prefix is gone */
#define CLIENT_EV_DNS 4u /* the name servers changed */

struct client {
	enum client_state state;
	uint8_t duid[DHCP6_DUID_MAX];
	size_t duid_len;
	uint32_t iaid;
	uint32_t xid;
	/* the first transmission of this exchange, and the next one */
	int64_t started;
	int64_t next;
	/* the time the retransmission waits, and how many went */
	int64_t rt;
	unsigned int count;
	/* in Solicit: the best Advertise so far, kept until the first RT */
	uint8_t best[DHCP6_MSG_MAX];
	size_t best_len;
	int best_pref;
	/* the server of the binding */
	uint8_t server[DHCP6_DUID_MAX];
	size_t server_len;
	/* the binding: its prefix, and when to renew and rebind */
	bool bound;
	struct prefix prefix;
	int64_t t1_at;
	int64_t t2_at;
	uint8_t dns[DHCP6_DNS_MAX][16];
	unsigned int ndns;
	uint32_t rand;
};

void client_init(struct client *c, const uint8_t *duid, size_t duid_len,
		 uint32_t iaid, uint32_t seed, int64_t now);

/*
 * client_step() - Run the client at @now: take the message @in, if any, and
 * fire what is due.
 * @out: the message to send to all servers and relays, if it returns more
 * than 0 bytes
 * @events: CLIENT_EV_* bits
 *
 * Return: the length of the message in @out, or 0.
 */
size_t client_step(struct client *c, int64_t now, const uint8_t *in,
		   size_t inlen, uint8_t *out, size_t cap, unsigned int *events);

/* When the client next wants to run. */
int64_t client_deadline(const struct client *c);

/* A Release of the binding, to send as the client stops; 0 without one. */
size_t client_release(struct client *c, int64_t now, uint8_t *out, size_t cap);

/* server.c */
#define SERVER_EV_ROUTE 1u /* the host's binding came or changed: route to it */
#define SERVER_EV_UNROUTE 2u /* the binding ended: no route */

struct server {
	uint8_t duid[DHCP6_DUID_MAX];
	size_t duid_len;
	/* what there is to delegate, from the client's binding */
	bool have;
	struct prefix prefix;
	/* a prefix of before, offered with no lifetime so that the host drops it */
	bool have_old;
	struct prefix old;
	uint8_t dns[DHCP6_DNS_MAX][16];
	unsigned int ndns;
	/* the one host's binding */
	bool bound;
	uint8_t host[DHCP6_DUID_MAX];
	size_t host_len;
	uint32_t host_iaid;
	uint8_t host_addr[16];
	int64_t bound_until;
};

void server_init(struct server *s, const uint8_t *duid, size_t duid_len);

/* What there is to delegate: @p, or nothing with NULL. */
void server_offer(struct server *s, const struct prefix *p, int64_t now);

/*
 * server_answer() - Answer a host's message @in, sent from @src.
 * @events: SERVER_EV_* bits; on ROUTE, the route goes to @s->host_addr
 *
 * Return: the length of the answer in @out, to send back to @src, or 0.
 */
size_t server_answer(struct server *s, const uint8_t *in, size_t inlen,
		     const uint8_t src[16], int64_t now, uint8_t *out,
		     size_t cap, unsigned int *events);

/* Whether the binding ran out by @now; its route then goes. */
bool server_expire(struct server *s, int64_t now);

/* ra.c */
#define ICMP6_ROUTER_SOLICIT 133
#define ICMP6_ROUTER_ADVERT 134

struct ra {
	/* seconds the host may use the stick as its default router; 0: none */
	uint16_t lifetime;
	uint32_t mtu;
	uint8_t mac[6];
	uint8_t dns[DHCP6_DNS_MAX][16];
	unsigned int ndns;
	uint32_t dns_lifetime;
};

/* The Router Advertisement of @r, its checksum left to the kernel. */
size_t ra_build(const struct ra *r, uint8_t *out, size_t cap);

/* main.c, and the tests' stand-in */
void dhcp6d_log(int level, const char *fmt, ...)
	__attribute((format(printf, 2, 3)));

#define DHCP6D_ERR(...) dhcp6d_log(3, __VA_ARGS__)
#define DHCP6D_WARN(...) dhcp6d_log(4, __VA_ARGS__)
#define DHCP6D_INFO(...) dhcp6d_log(6, __VA_ARGS__)
#define DHCP6D_DEBUG(...) dhcp6d_log(7, __VA_ARGS__)

#endif
