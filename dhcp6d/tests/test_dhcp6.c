// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the protocol: the messages, the client against the server in the
 * same process on a clock of the test's, with and without Rapid Commit, the
 * renewal, a new prefix from the operator, a prefix that runs out, the
 * retransmissions, a binding taken back after a restart, and the Router
 * Advertisement.
 */

#include <stdio.h>
#include <string.h>

#include "dhcp6d.h"

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

static const uint8_t HOST_MAC[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x02 };
static const uint8_t STICK_MAC[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 };
/* 2001:db8:1200::/56, a documentation prefix. */
static const uint8_t PREFIX[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x12 };
static const uint8_t PREFIX2[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x34 };
static const uint8_t HOST_LL[16] = { 0xfe, 0x80, [8] = 0x02, [15] = 0x02 };
static const uint8_t DNS[16] = { 0x20, 0x01, 0x0d, 0xb8, [15] = 0x53 };

static void offer(struct server *s, const uint8_t addr[16], int64_t now)
{
	struct prefix p = { .len = 56,
			    .preferred_until = now + 3600 * 1000,
			    .valid_until = now + 7200 * 1000 };

	memcpy(p.addr, addr, 16);
	server_offer(s, &p, now);
}

static void test_messages(void)
{
	uint8_t buf[64], duid[16];
	struct msg m;
	size_t len;
	const uint8_t *v;

	puts("messages");
	msg_start(&m, buf, sizeof(buf), DHCP6_SOLICIT, 0x123456);
	msg_opt16(&m, DHCP6_OPT_ELAPSED_TIME, 7);
	msg_open(&m, DHCP6_OPT_IA_PD);
	msg_raw(&m, "\0\0\0\1", 4);
	msg_opt16(&m, DHCP6_OPT_STATUS_CODE, DHCP6_NO_PREFIX_AVAIL);
	msg_close(&m);
	CHECK(!m.overflow && buf[0] == DHCP6_SOLICIT && buf[1] == 0x12 &&
	      buf[3] == 0x56);
	v = opt_find(buf + 4, m.len - 4, DHCP6_OPT_IA_PD, &len);
	CHECK(v && len == 4 + 6);
	CHECK(v && opt_status(v + 4, len - 4) == DHCP6_NO_PREFIX_AVAIL);
	CHECK(opt_status(buf + 4, m.len - 4) == DHCP6_SUCCESS);
	CHECK(!opt_find(buf + 4, m.len - 4, DHCP6_OPT_DNS_SERVERS, &len));

	msg_start(&m, buf, 8, DHCP6_REPLY, 1);
	msg_opt(&m, DHCP6_OPT_CLIENTID, "0123456789", 10);
	CHECK(m.overflow);

	CHECK(duid_ll(duid, HOST_MAC) == 10 && duid[1] == 3 && duid[3] == 1 &&
	      !memcmp(duid + 4, HOST_MAC, 6));
}

/* Remove the Rapid Commit option from a Solicit, as an older client sends it. */
static size_t without_rapid_commit(uint8_t *msg, size_t len)
{
	for (size_t off = 4; off + 4 <= len;) {
		uint16_t code = get16(msg + off), l = get16(msg + off + 2);

		if (code == DHCP6_OPT_RAPID_COMMIT) {
			memmove(msg + off, msg + off + 4 + l, len - off - 4 - l);
			return len - 4 - l;
		}
		off += 4 + (size_t)l;
	}
	return len;
}

static void test_rapid_commit(void)
{
	uint8_t duid[16], sduid[16], a[DHCP6_MSG_MAX], b[DHCP6_MSG_MAX];
	struct client c;
	struct server s;
	unsigned int ev, sev;
	size_t n, r;
	int64_t now = 1000;

	puts("a Solicit with Rapid Commit binds at once");
	client_init(&c, duid, duid_ll(duid, HOST_MAC), 1, 7, now);
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	memcpy(s.dns[0], DNS, 16);
	s.ndns = 1;

	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	CHECK(n > 4 && a[0] == DHCP6_SOLICIT && !ev);
	/* Before the stick has a prefix, it advertises none. */
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	CHECK(r && b[0] == DHCP6_ADVERTISE && !sev);
	client_step(&c, now, b, r, a, sizeof(a), &ev);
	CHECK(c.best_len == 0 && !c.bound);

	offer(&s, PREFIX, now);
	n = client_step(&c, now + 1200, NULL, 0, a, sizeof(a), &ev);
	CHECK(n && a[0] == DHCP6_SOLICIT);
	r = server_answer(&s, a, n, HOST_LL, now + 1200, b, sizeof(b), &sev);
	CHECK(r && b[0] == DHCP6_REPLY && (sev & SERVER_EV_ROUTE) && s.bound);
	CHECK(!memcmp(s.host_addr, HOST_LL, 16));
	n = client_step(&c, now + 1201, b, r, a, sizeof(a), &ev);
	CHECK((ev & CLIENT_EV_BOUND) && (ev & CLIENT_EV_DNS) && !n);
	CHECK(c.bound && c.prefix.len == 56 && !memcmp(c.prefix.addr, PREFIX, 16));
	CHECK(c.ndns == 1 && !memcmp(c.dns[0], DNS, 16));
	/* T1 from the server, at most ten minutes, so a new prefix travels fast. */
	CHECK(c.t1_at == now + 1201 + 600 * 1000);
	CHECK(c.t2_at == now + 1201 + 960 * 1000);
	CHECK(client_deadline(&c) == c.t1_at);
}

static void test_advertise_request(void)
{
	uint8_t duid[16], sduid[16], a[DHCP6_MSG_MAX], b[DHCP6_MSG_MAX];
	struct client c;
	struct server s;
	unsigned int ev, sev;
	size_t n, r;
	int64_t now = 5000, first;

	puts("without Rapid Commit: Advertise, then Request at the end of the first wait");
	client_init(&c, duid, duid_ll(duid, HOST_MAC), 1, 9, now);
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	offer(&s, PREFIX, now);

	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	n = without_rapid_commit(a, n);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	CHECK(r && b[0] == DHCP6_ADVERTISE && !sev && !s.bound);
	client_step(&c, now + 10, b, r, a, sizeof(a), &ev);
	CHECK(c.best_len == r && c.state == CLIENT_SOLICIT);
	first = c.next;
	CHECK(first >= now + 1000 && first <= now + 1100);

	n = client_step(&c, first, NULL, 0, a, sizeof(a), &ev);
	CHECK(n && a[0] == DHCP6_REQUEST && c.state == CLIENT_REQUEST);
	CHECK(opt_find(a + 4, n - 4, DHCP6_OPT_SERVERID, &r));
	r = server_answer(&s, a, n, HOST_LL, first, b, sizeof(b), &sev);
	CHECK(r && b[0] == DHCP6_REPLY && (sev & SERVER_EV_ROUTE));
	client_step(&c, first + 1, b, r, a, sizeof(a), &ev);
	CHECK((ev & CLIENT_EV_BOUND) && c.state == CLIENT_BOUND);
}

static void test_renew_and_new_prefix(void)
{
	uint8_t duid[16], sduid[16], a[DHCP6_MSG_MAX], b[DHCP6_MSG_MAX];
	struct client c;
	struct server s;
	unsigned int ev, sev;
	size_t n, r, len;
	int64_t now = 0;

	puts("renewal at T1, and a new prefix from the operator");
	client_init(&c, duid, duid_ll(duid, HOST_MAC), 1, 11, now);
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	offer(&s, PREFIX, now);
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	client_step(&c, now, b, r, a, sizeof(a), &ev);
	CHECK(c.bound);

	now = c.t1_at;
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	CHECK(n && a[0] == DHCP6_RENEW && c.state == CLIENT_RENEW);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	CHECK(r && b[0] == DHCP6_REPLY && !sev);
	client_step(&c, now, b, r, a, sizeof(a), &ev);
	CHECK(c.state == CLIENT_BOUND && !(ev & CLIENT_EV_BOUND));

	/* The operator moves the line to another prefix. */
	offer(&s, PREFIX2, now);
	now = c.t1_at;
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	{
		const uint8_t *ia = opt_find(b + 4, r - 4, DHCP6_OPT_IA_PD, &len);
		int prefixes = 0, zero = 0;

		for (size_t off = 12; ia && off + 4 <= len;) {
			uint16_t code = get16(ia + off), l = get16(ia + off + 2);

			if (code == DHCP6_OPT_IAPREFIX) {
				prefixes++;
				zero += !get32(ia + off + 8);
			}
			off += 4 + (size_t)l;
		}
		CHECK(prefixes == 2 && zero == 1);
	}
	client_step(&c, now, b, r, a, sizeof(a), &ev);
	CHECK((ev & CLIENT_EV_BOUND) && !memcmp(c.prefix.addr, PREFIX2, 16));
	/* Told once, the prefix of before is not offered again. */
	CHECK(!s.have_old);
}

static void test_lost(void)
{
	uint8_t duid[16], sduid[16], a[DHCP6_MSG_MAX], b[DHCP6_MSG_MAX];
	struct client c;
	struct server s;
	unsigned int ev, sev;
	size_t n, r;
	int64_t now = 0, t;

	puts("a prefix that no server renews runs out, and the client starts over");
	client_init(&c, duid, duid_ll(duid, HOST_MAC), 1, 13, now);
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	offer(&s, PREFIX, now);
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	client_step(&c, now, b, r, a, sizeof(a), &ev);

	for (t = c.t1_at; t < c.t2_at; t = client_deadline(&c))
		client_step(&c, t, NULL, 0, a, sizeof(a), &ev);
	n = client_step(&c, c.t2_at, NULL, 0, a, sizeof(a), &ev);
	CHECK(c.state == CLIENT_REBIND && (!n || a[0] == DHCP6_REBIND));
	CHECK(!opt_find(a + 4, n - 4, DHCP6_OPT_SERVERID, &r));
	for (t = client_deadline(&c); t < c.prefix.valid_until; t = client_deadline(&c))
		client_step(&c, t, NULL, 0, a, sizeof(a), &ev);
	client_step(&c, c.prefix.valid_until, NULL, 0, a, sizeof(a), &ev);
	CHECK((ev & CLIENT_EV_LOST) && !c.bound && c.state == CLIENT_SOLICIT);
}

/* The IA_PD of the answer @b of @r bytes: how many prefixes, how many at 0. */
static void count_prefixes(const uint8_t *b, size_t r, int *prefixes, int *zero)
{
	size_t len;
	const uint8_t *ia = opt_find(b + 4, r - 4, DHCP6_OPT_IA_PD, &len);

	*prefixes = *zero = 0;
	for (size_t off = 12; ia && off + 4 <= len;) {
		uint16_t code = get16(ia + off), l = get16(ia + off + 2);

		if (code == DHCP6_OPT_IAPREFIX) {
			(*prefixes)++;
			*zero += !get32(ia + off + 8);
		}
		off += 4 + (size_t)l;
	}
}

static void test_restore(void)
{
	uint8_t duid[16], sduid[16], a[DHCP6_MSG_MAX], b[DHCP6_MSG_MAX];
	struct prefix was = { .len = 56 };
	struct client c;
	struct server s;
	unsigned int ev, sev;
	size_t n, r;
	int64_t now = 0;
	int prefixes, zero;

	puts("a binding taken back after a restart: the host's Renew is known");
	client_init(&c, duid, duid_ll(duid, HOST_MAC), 1, 17, now);
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	offer(&s, PREFIX, now);
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	client_step(&c, now, b, r, a, sizeof(a), &ev);
	CHECK(c.bound);

	/* The stick starts again: a server of nothing, but the binding. */
	memcpy(was.addr, PREFIX, 16);
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	server_restore(&s, c.duid, c.duid_len, c.iaid, HOST_LL, &was);
	CHECK(s.bound && !s.have);
	/* It waits for the prefix, however long that takes. */
	CHECK(!server_expire(&s, now + 86400000));
	offer(&s, PREFIX, now);
	CHECK(!s.have_old);

	now = c.t1_at;
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	CHECK(n && a[0] == DHCP6_RENEW);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	CHECK(r && b[0] == DHCP6_REPLY && !sev);
	CHECK(opt_status(b + 4, r - 4) == DHCP6_SUCCESS);
	count_prefixes(b, r, &prefixes, &zero);
	CHECK(prefixes == 1 && zero == 0);
	client_step(&c, now, b, r, a, sizeof(a), &ev);
	CHECK(c.state == CLIENT_BOUND && !(ev & (CLIENT_EV_BOUND | CLIENT_EV_LOST)));

	/* Back with another prefix: the host's next Renew hears of both. */
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	server_restore(&s, c.duid, c.duid_len, c.iaid, HOST_LL, &was);
	offer(&s, PREFIX2, now);
	CHECK(s.have_old);
	now = c.t1_at;
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	count_prefixes(b, r, &prefixes, &zero);
	CHECK(prefixes == 2 && zero == 1);
	client_step(&c, now, b, r, a, sizeof(a), &ev);
	CHECK((ev & CLIENT_EV_BOUND) && !memcmp(c.prefix.addr, PREFIX2, 16));

	/* A DUID too long for a binding is not taken back. */
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	server_restore(&s, b, DHCP6_DUID_MAX + 1, 1, HOST_LL, NULL);
	CHECK(!s.bound);
}

static void test_retransmission(void)
{
	uint8_t duid[16], a[DHCP6_MSG_MAX];
	struct client c;
	unsigned int ev;
	int64_t now = 0, prev = 0, gap_prev = 0;
	int sent = 0;

	puts("a Solicit unanswered is sent again ever later, up to SOL_MAX_RT");
	client_init(&c, duid, duid_ll(duid, HOST_MAC), 1, 17, now);
	for (int i = 0; i < 20; i++) {
		now = client_deadline(&c);
		if (client_step(&c, now, NULL, 0, a, sizeof(a), &ev)) {
			int64_t gap = now - prev;

			if (sent >= 2 && gap < 1000 * 300)
				CHECK(gap >= gap_prev * 18 / 10 && gap <= gap_prev * 22 / 10);
			gap_prev = gap;
			prev = now;
			sent++;
		}
	}
	CHECK(sent == 20);
	CHECK(c.rt <= DHCP6_SOL_MAX_RT * 11 / 10);
}

static void test_server_rules(void)
{
	uint8_t duid[16], sduid[16], other[16], a[DHCP6_MSG_MAX], b[DHCP6_MSG_MAX];
	struct client c;
	struct server s;
	unsigned int ev, sev;
	size_t n, r, len;
	int64_t now = 0;

	puts("the server: another server's message, a Renew it does not know, Release, Information-Request");
	client_init(&c, duid, duid_ll(duid, HOST_MAC), 1, 19, now);
	server_init(&s, sduid, duid_ll(sduid, STICK_MAC));
	offer(&s, PREFIX, now);
	n = client_step(&c, now, NULL, 0, a, sizeof(a), &ev);
	r = server_answer(&s, a, n, HOST_LL, now, b, sizeof(b), &sev);
	client_step(&c, now, b, r, a, sizeof(a), &ev);

	/* The same Renew, to a server that has never seen the host. */
	{
		struct server fresh;

		server_init(&fresh, sduid, duid_ll(sduid, STICK_MAC));
		offer(&fresh, PREFIX, now);
		n = client_step(&c, c.t1_at, NULL, 0, a, sizeof(a), &ev);
		r = server_answer(&fresh, a, n, HOST_LL, c.t1_at, b, sizeof(b), &sev);
		const uint8_t *ia = opt_find(b + 4, r - 4, DHCP6_OPT_IA_PD, &len);

		CHECK(ia && opt_status(ia + 12, len - 12) == DHCP6_NO_BINDING);
		CHECK(!fresh.bound);
	}

	/* A message that names another server is not answered. */
	duid_ll(other, HOST_MAC);
	{
		struct msg m;

		msg_start(&m, a, sizeof(a), DHCP6_REQUEST, 1);
		msg_opt(&m, DHCP6_OPT_CLIENTID, duid, 10);
		msg_opt(&m, DHCP6_OPT_SERVERID, other, 10);
		msg_opt(&m, DHCP6_OPT_IA_PD, "\0\0\0\1\0\0\0\0\0\0\0\0", 12);
		CHECK(!server_answer(&s, a, m.len, HOST_LL, now, b, sizeof(b), &sev));

		msg_start(&m, a, sizeof(a), DHCP6_INFORMATION_REQUEST, 2);
		msg_opt(&m, DHCP6_OPT_CLIENTID, duid, 10);
		memcpy(s.dns[0], DNS, 16);
		s.ndns = 1;
		r = server_answer(&s, a, m.len, HOST_LL, now, b, sizeof(b), &sev);
		CHECK(r && b[0] == DHCP6_REPLY &&
		      opt_find(b + 4, r - 4, DHCP6_OPT_DNS_SERVERS, &len) && len == 16);
	}

	n = client_release(&c, now + 10, a, sizeof(a));
	CHECK(n && a[0] == DHCP6_RELEASE && !c.bound);
	r = server_answer(&s, a, n, HOST_LL, now + 10, b, sizeof(b), &sev);
	CHECK(r && (sev & SERVER_EV_UNROUTE) && !s.bound);
}

static void test_ra(void)
{
	struct ra r = { .lifetime = 1800, .mtu = 1492, .ndns = 1, .dns_lifetime = 1800 };
	uint8_t buf[128];
	size_t n;

	puts("the Router Advertisement");
	memcpy(r.mac, STICK_MAC, 6);
	memcpy(r.dns[0], DNS, 16);
	n = ra_build(&r, buf, sizeof(buf));
	CHECK(n == 16 + 8 + 8 + 24);
	CHECK(buf[0] == ICMP6_ROUTER_ADVERT && get16(buf + 6) == 1800 && buf[5] == 0x40);
	CHECK(buf[16] == 1 && !memcmp(buf + 18, STICK_MAC, 6));
	CHECK(buf[24] == 5 && get32(buf + 28) == 1492);
	CHECK(buf[32] == 25 && buf[33] == 3 && !memcmp(buf + 40, DNS, 16));
	r.ndns = 0;
	r.lifetime = 0;
	CHECK(ra_build(&r, buf, sizeof(buf)) == 32 && get16(buf + 6) == 0);
	CHECK(!ra_build(&r, buf, 16));
}

int main(void)
{
	test_messages();
	test_rapid_commit();
	test_advertise_request();
	test_renew_and_new_prefix();
	test_lost();
	test_restore();
	test_retransmission();
	test_server_rules();
	test_ra();
	printf("test_dhcp6: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
