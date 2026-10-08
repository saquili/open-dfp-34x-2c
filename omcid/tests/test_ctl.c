// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the control socket. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/table.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "omcid/array_size.h"
#include "omcid/attrs.h"
#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "omcid/libomcid.h"
#include "sys/ctl.h"
#include "sys/log.h"

#include "fault.h"
#include "harness.h"
#include "log_capture.h"

static char g_dir[64];
static char g_sock[128];
static char g_log[8192];

static struct omci_mib *g_mib;
static struct omci_ctl *g_ctl;
static const struct omci_ctl_counters g_counters = { .handled = 7,
						     .rejected = 2,
						     .answered = 9 };

/*
 * A translation of the frames of one VID and any priority. @tag goes into
 * the filter's TPID field, which the hardware does not match: rows that
 * differ only in it are different rows of one rule.
 */
static bool put_rule(uint16_t inst, uint32_t tag, uint32_t match,
		     uint32_t assign)
{
	uint8_t r[16];
	const uint32_t w[4] = {
		0xf8000000u,
		(8u << 28) | (match << 15) | ((tag & 7u) << 12),
		0x400f0000u,
		(8u << 16) | (assign << 3) | 2u,
	};
	for (int k = 0; k < 4; k++) {
		r[k * 4 + 0] = (uint8_t)(w[k] >> 24);
		r[k * 4 + 1] = (uint8_t)(w[k] >> 16);
		r[k * 4 + 2] = (uint8_t)(w[k] >> 8);
		r[k * 4 + 3] = (uint8_t)w[k];
	}
	return omci_mib_table_put(g_mib, 171u, inst, 6u, r, 16, 8,
				  OMCI_MIB_TABLE_LAST) == OMCI_OK;
}

enum fixture { BARE, DEFAULTS, ME171 };

static bool mib_up(enum fixture what)
{
	struct omci_hw_caps caps;

	g_mib = NULL;
	if (omci_hw_caps(&caps) != OMCI_OK || omci_mib_init(&g_mib) != OMCI_OK)
		return false;
	if (what == BARE)
		return true;
	if (omci_me_defaults_apply(g_mib, &caps) != OMCI_OK)
		return false;
	if (what == DEFAULTS)
		return true;

	static const struct {
		unsigned int pri, match, assign;
	} rows[] = {
		{ 6u, 410u, 1410u }, { 5u, 410u, 1410u }, { 0u, 410u, 1410u },
		{ 4u, 499u, 1410u }, { 3u, 410u, 1900u },
	};
	if (omci_mib_create(g_mib, 171u, 0x101u) != OMCI_OK ||
	    omci_mib_create(g_mib, 171u, 0x102u) != OMCI_OK)
		return false;
	for (size_t i = 0; i < ARRAY_SIZE(rows); i++)
		if (!put_rule(0x101u, rows[i].pri, rows[i].match,
			      rows[i].assign))
			return false;

	for (unsigned int k = 0; k < 139u; k++)
		if (!put_rule(0x102u, 0u, 700u + k, 1700u + k))
			return false;
	return omci_vlan_program(g_mib, 171u, 6u, NULL) == OMCI_OK;
}

static void mib_down(void)
{
	omci_mib_fini(g_mib);
	g_mib = NULL;
}

static bool server_up(const struct omci_ctl_counters *counters)
{
	(void)unlink(g_sock);
	g_ctl = NULL;
	return omci_ctl_open(&g_ctl, g_sock, g_mib, counters) == OMCI_OK;
}

static void server_down(void)
{
	omci_ctl_close(g_ctl);
	g_ctl = NULL;
	(void)unlink(g_sock);
}

static void settle(struct omci_ctl *ctl)
{
	for (unsigned int turn = 0; turn < 64u; turn++) {
		struct pollfd fds[OMCI_CTL_MAX_FDS];
		const size_t n = omci_ctl_pollfds(ctl, fds, OMCI_CTL_MAX_FDS);

		if (n == 0u || poll(fds, (nfds_t)n, 0) <= 0)
			return;
		omci_ctl_service(ctl, fds, n);
	}
}

static void serve_hook(void *ctl)
{
	settle(ctl);
}

static size_t held(void)
{
	struct pollfd fds[OMCI_CTL_MAX_FDS];

	return omci_ctl_pollfds(g_ctl, fds, OMCI_CTL_MAX_FDS);
}

static noinline void poison_stack(void)
{
	volatile unsigned char junk[16384];

	for (size_t i = 0; i < sizeof(junk); i++)
		junk[i] = 0xa5;
}

static int lowest_free(void)
{
	const int fd = dup(0);

	if (fd >= 0)
		(void)close(fd);
	return fd;
}

static int dial(void)
{
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };

	memcpy(addr.sun_path, g_sock, strlen(g_sock) + 1u);
	if (fd < 0 ||
	    connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
		if (fd >= 0)
			(void)close(fd);
		return -1;
	}
	return fd;
}

static void put_req_magic(int fd, uint32_t magic, uint16_t op, uint16_t cls,
			  uint16_t inst, uint16_t arg)
{
	uint8_t r[OMCI_CTL_REQ_LEN] = { 0 };

	omci_be32_store(&r[OMCI_CTL_REQ_MAGIC], magic);
	omci_be16_store(&r[OMCI_CTL_REQ_OP], op);
	omci_be16_store(&r[OMCI_CTL_REQ_CLASS], cls);
	omci_be16_store(&r[OMCI_CTL_REQ_INSTANCE], inst);
	omci_be16_store(&r[OMCI_CTL_REQ_ARG], arg);
	CHECK_EQ(send(fd, r, sizeof(r), MSG_NOSIGNAL), (ssize_t)sizeof(r));
}

static void put_req(int fd, uint16_t op, uint16_t cls, uint16_t inst,
		    uint16_t arg)
{
	put_req_magic(fd, OMCI_CTL_MAGIC, op, cls, inst, arg);
}

enum look { LOOK_NOTHING, LOOK_RESPONSE, LOOK_CLOSED };

static enum look get_rsp(int fd, uint16_t *status, uint8_t *pay, size_t cap,
			 size_t *paylen)
{
	uint8_t h[OMCI_CTL_RSP_LEN];
	const ssize_t n = recv(fd, h, sizeof(h), MSG_DONTWAIT);

	if (n == 0 || (n < 0 && errno == ECONNRESET))
		return LOOK_CLOSED;
	if (n < 0)
		return LOOK_NOTHING;
	CHECK_EQ(n, (ssize_t)sizeof(h));
	CHECK_EQ(omci_be32(&h[OMCI_CTL_RSP_MAGIC]), OMCI_CTL_MAGIC);
	*status = (uint16_t)omci_be16(&h[OMCI_CTL_RSP_STATUS]);
	const size_t len = omci_be16(&h[OMCI_CTL_RSP_PAYLEN]);

	CHECK(len <= cap);
	if (len > 0u && len <= cap)
		CHECK_EQ(recv(fd, pay, len, MSG_DONTWAIT), (ssize_t)len);
	*paylen = len;
	return LOOK_RESPONSE;
}

static unsigned int ask(int fd, uint16_t op, uint16_t cls, uint16_t inst,
			uint16_t arg, uint8_t *pay, size_t cap, size_t *paylen)
{
	uint16_t st = 0xffffu;
	size_t n = 0;

	put_req(fd, op, cls, inst, arg);
	settle(g_ctl);
	if (get_rsp(fd, &st, pay, cap, &n) != LOOK_RESPONSE)
		return 0xffffu;
	if (paylen != NULL)
		*paylen = n;
	return st;
}

static void test_open_and_close(void)
{
	CHECK(mib_up(BARE));
	struct omci_ctl *ctl = (struct omci_ctl *)1;

	CHECK_EQ(omci_ctl_open(NULL, g_sock, g_mib, &g_counters),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_ctl_open(&ctl, NULL, g_mib, &g_counters), OMCI_ERR_INVAL);

	char longpath[200];

	memset(longpath, 'x', sizeof(longpath) - 1u);
	longpath[sizeof(longpath) - 1u] = '\0';
	fault_reset();
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_ctl_open(&ctl, longpath, g_mib, &g_counters),
			 OMCI_ERR_INVAL);
	});
	CHECK(ctl == NULL);
	CHECK(strstr(g_log, "ctl: socket path is too long: xxxx") != NULL);
	CHECK_EQ(fault_seen(FAULT_SOCKET), 0u);

	const int free_before = lowest_free();

	fault_reset();
	fault_fail(FAULT_SOCKET, 1, EMFILE);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_ctl_open(&ctl, g_sock, g_mib, &g_counters),
			 OMCI_ERR_IO);
	});
	CHECK_EQ(fault_fired(FAULT_SOCKET), 1u);
	CHECK(ctl == NULL);
	CHECK(strstr(g_log,
		     "ctl: cannot create the control socket (errno 24)") !=
	      NULL);
	CHECK_EQ(lowest_free(), free_before);

	fault_reset();
	poison_stack();
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_ctl_open(&ctl,
				       "/nonexistent-directory/omcid.sock",
				       g_mib, &g_counters),
			 OMCI_ERR_IO);
	});
	CHECK(ctl == NULL);
	CHECK(strstr(g_log,
		     "ctl: cannot listen on /nonexistent-directory/omcid.sock (errno 2)") !=
	      NULL);
	CHECK_EQ(strcmp(fault_connect_path(),
			"/nonexistent-directory/omcid.sock"),
		 0);
	{
		const char *named = fault_connect_path();
		unsigned int not_zero = 0;

		for (size_t i = strlen(named) + 1u;
		     i < sizeof((struct sockaddr_un *)0)->sun_path; i++)
			not_zero += named[i] != '\0' ? 1u : 0u;
		CHECK_EQ(not_zero, 0u);
	}
	CHECK_EQ(lowest_free(), free_before);

	fault_reset();
	fault_fail(FAULT_LISTEN, 1, EOPNOTSUPP);
	(void)unlink(g_sock);
	OMCI_T_CAPTURE_LOG(g_log, {
		CHECK_EQ(omci_ctl_open(&ctl, g_sock, g_mib, &g_counters),
			 OMCI_ERR_IO);
	});
	CHECK_EQ(fault_fired(FAULT_LISTEN), 1u);
	CHECK(strstr(g_log, "ctl: cannot listen on ") != NULL &&
	      strstr(g_log, "(errno 95)") != NULL);
	CHECK_EQ(lowest_free(), free_before);

	fault_reset();
	CHECK(server_up(&g_counters));
	CHECK_EQ(held(), 1u);

	struct pollfd fds[OMCI_CTL_MAX_FDS];
	const int c1 = dial();
	const int c2 = dial();

	settle(g_ctl);
	CHECK_EQ(held(), 3u);
	CHECK_EQ(omci_ctl_pollfds(g_ctl, fds, 2), 2u);
	CHECK_EQ(omci_ctl_pollfds(g_ctl, fds, 0), 0u);
	CHECK_EQ(omci_ctl_pollfds(g_ctl, NULL, 4), 0u);
	CHECK_EQ(omci_ctl_pollfds(NULL, fds, 4), 0u);
	CHECK_EQ(omci_ctl_pollfds(g_ctl, fds, OMCI_CTL_MAX_FDS), 3u);
	CHECK_EQ(fds[0].events, POLLIN);

	uint16_t st;
	size_t n;
	uint8_t pay[16];

	omci_ctl_close(g_ctl);
	CHECK_EQ(get_rsp(c1, &st, pay, sizeof(pay), &n), LOOK_CLOSED);
	CHECK_EQ(get_rsp(c2, &st, pay, sizeof(pay), &n), LOOK_CLOSED);
	CHECK_EQ(omci_ctl_pollfds(g_ctl, fds, OMCI_CTL_MAX_FDS), 0u);
	(void)close(c1);
	(void)close(c2);
	omci_ctl_close(g_ctl);
	omci_ctl_close(NULL);
	g_ctl = NULL;
	(void)unlink(g_sock);
	CHECK_EQ(lowest_free(), free_before);

	omci_ctl_service(NULL, fds, 1);
	uint8_t frame[OMCI_FRAME_LEN] = { 0 };

	omci_ctl_tap_publish(NULL, OMCI_CTL_TAP_TX, 0, frame);
	mib_down();
}

static void test_replies(void)
{
	CHECK(mib_up(DEFAULTS));
	CHECK(server_up(&g_counters));
	const int c = dial();

	CHECK(c >= 0);
	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	size_t n = 99;

	CHECK_EQ(ask(c, OMCI_CTL_OP_STATS, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK_EQ(n, OMCI_CTL_STATS_LEN);
	CHECK_EQ(omci_be32(&pay[0]), 7u);
	CHECK_EQ(omci_be32(&pay[4]), 2u);
	CHECK_EQ(omci_be32(&pay[8]), 9u);

	put_req_magic(c, 0xdeadbeefu, OMCI_CTL_OP_STATS, 0, 0, 0);
	settle(g_ctl);
	uint16_t st = 0;

	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(st, OMCI_CTL_ERR_PROTO);
	CHECK_EQ(n, 0u);

	CHECK_EQ(ask(c, 99, 0, 0, 0, pay, sizeof(pay), &n), OMCI_CTL_ERR_OP);
	CHECK_EQ(n, 0u);
	CHECK_EQ(ask(c, OMCI_CTL_OP_STATS, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);

	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_LIST, 9999, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK_EQ(n, 0u);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_LIST, 5, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK_EQ(n, 6u);
	CHECK_EQ(omci_be16(&pay[0]), 0x101u);
	CHECK_EQ(omci_be16(&pay[2]), 0x180u);
	CHECK_EQ(omci_be16(&pay[4]), 0x106u);

	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_GET, 256, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK(n > 0u);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_GET, 256, 7, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_ERR_NOT_FOUND);
	CHECK_EQ(n, 0u);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_GET, 9999, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_ERR_NOT_FOUND);

	/* A dump has a record per attribute, the values ME_GET gives. */
	uint8_t got[OMCI_CTL_PAYLOAD_MAX];
	size_t m = 0;

	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_GET, 256, 0, 0, got, sizeof(got), &m),
		 OMCI_CTL_OK);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_DUMP, 256, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK_EQ(pay[0], 1u);
	CHECK_EQ(omci_be16(&pay[1]), 4u);
	CHECK_MEM(&pay[3], got, 4);
	CHECK_EQ(pay[7], 2u);
	CHECK_EQ(omci_be16(&pay[8]), 14u);
	CHECK_MEM(&pay[10], &got[4], 14);
	CHECK_EQ(n, m + OMCI_CTL_DUMP_HDR_LEN *
				    omci_me_class_by_id(256)->attr_count);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_DUMP, 256, 7, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_ERR_NOT_FOUND);
	CHECK_EQ(n, 0u);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_DUMP, 9999, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_ERR_NOT_FOUND);

	/* A table's record holds its rows, which ME_GET leaves out. */
	uint8_t row[16] = { 0xf8, 0,	0,    0,    0x80, 0x23, 0x80, 0,
			    0,	  0x0f, 0xa0, 0x00, 0x00, 0x0f, 0x00, 0x00 };

	CHECK_EQ(omci_mib_create(g_mib, 171u, 1u), OMCI_OK);
	CHECK_EQ(omci_mib_table_put(g_mib, 171u, 1u, 6u, row, sizeof(row), 8u,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	row[5] = 0x24;
	CHECK_EQ(omci_mib_table_put(g_mib, 171u, 1u, 6u, row, sizeof(row), 8u,
				    OMCI_MIB_TABLE_LAST),
		 OMCI_OK);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_DUMP, 171, 1, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);

	size_t at = 0;

	while (at < n && (pay[at] & 0x7fu) != 6u)
		at += OMCI_CTL_DUMP_HDR_LEN + omci_be16(&pay[at + 1u]);
	CHECK(at < n);
	CHECK_EQ(pay[at], 6u | OMCI_CTL_DUMP_TABLE);
	CHECK_EQ(omci_be16(&pay[at + 1u]), 32u);
	CHECK_EQ(pay[at + 3u + 5u], 0x23u);
	CHECK_EQ(pay[at + 3u + 16u + 5u], 0x24u);

	/* Rows beyond the payload: the answer says so, and holds nothing. */
	for (uint8_t k = 0; k < 40u; k++) {
		row[5] = k;
		row[6] = 0x7f;
		(void)omci_mib_table_put(g_mib, 171u, 1u, 6u, row, sizeof(row),
					 8u, OMCI_MIB_TABLE_LAST);
	}
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_DUMP, 171, 1, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_ERR_TOO_BIG);
	CHECK_EQ(n, 0u);
	CHECK_EQ(omci_mib_delete(g_mib, 171u, 1u), OMCI_OK);

	for (uint16_t i = 1; i <= 256u; i++)
		CHECK_EQ(omci_mib_create(g_mib, 321u, i), OMCI_OK);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_LIST, 321, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK_EQ(n, OMCI_CTL_PAYLOAD_MAX);
	CHECK_EQ(omci_be16(&pay[0]), 1u);
	CHECK_EQ(omci_be16(&pay[510]), 256u);
	CHECK_EQ(omci_mib_create(g_mib, 321u, 257u), OMCI_OK);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_LIST, 321, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_ERR_TOO_BIG);
	CHECK_EQ(n, 0u);
	CHECK_EQ(ask(c, OMCI_CTL_OP_ME_LIST, 5, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);

	size_t widest = 0;

	for (size_t k = 0; k < omci_me_class_count; k++) {
		size_t total = 0;

		for (uint8_t a = 1; a <= omci_me_classes[k].attr_count; a++) {
			const struct omci_me_attr *at =
				omci_me_attr_get(&omci_me_classes[k], a);
			if (at != NULL && !omci_me_attr_is_table(at))
				total += at->size;
		}
		if (total > widest)
			widest = total;
	}
	CHECK(widest > 0u && widest <= OMCI_CTL_PAYLOAD_MAX);

	(void)close(c);
	server_down();
	CHECK(server_up(NULL));
	const int d = dial();

	CHECK_EQ(ask(d, OMCI_CTL_OP_STATS, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK_EQ(omci_be32(&pay[0]) | omci_be32(&pay[4]) | omci_be32(&pay[8]),
		 0u);
	(void)close(d);
	server_down();
	mib_down();
}

static void test_requests_in_pieces(void)
{
	CHECK(mib_up(BARE));
	CHECK(server_up(&g_counters));
	const int c = dial();
	uint8_t req[OMCI_CTL_REQ_LEN] = { 0 };
	uint8_t pay[64];
	uint16_t st = 0;
	size_t n = 0;

	omci_be32_store(&req[OMCI_CTL_REQ_MAGIC], OMCI_CTL_MAGIC);
	omci_be16_store(&req[OMCI_CTL_REQ_OP], OMCI_CTL_OP_STATS);

	CHECK_EQ(send(c, &req[0], 6, 0), 6);
	settle(g_ctl);
	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_NOTHING);
	CHECK_EQ(send(c, &req[6], 6, 0), 6);
	settle(g_ctl);
	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(st, OMCI_CTL_OK);

	for (unsigned int i = 0; i < sizeof(req); i++) {
		CHECK_EQ(send(c, &req[i], 1, 0), 1);
		settle(g_ctl);
		CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n),
			 i + 1u < sizeof(req) ? LOOK_NOTHING : LOOK_RESPONSE);
	}

	uint8_t two[2u * OMCI_CTL_REQ_LEN];

	memcpy(two, req, sizeof(req));
	memcpy(two + sizeof(req), req, sizeof(req));
	CHECK_EQ(send(c, two, sizeof(two), 0), (ssize_t)sizeof(two));
	settle(g_ctl);
	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_NOTHING);
	(void)close(c);
	server_down();
	mib_down();
}

static void test_clients(void)
{
	CHECK(mib_up(BARE));
	CHECK(server_up(&g_counters));
	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	uint16_t st = 0;
	size_t n = 0;

	omci_log_level_set(OMCI_LOG_DEBUG);

	int c[5];

	for (unsigned int i = 0; i < 4; i++) {
		c[i] = dial();
		CHECK(c[i] >= 0);
	}
	settle(g_ctl);
	CHECK_EQ(held(), 1u + OMCI_CTL_MAX_CLIENTS);
	c[4] = dial();
	CHECK(c[4] >= 0);
	OMCI_T_CAPTURE_LOG(g_log, { settle(g_ctl); });
	CHECK(strstr(g_log,
		     "ctl: refusing a client — all 4 slots are in use") !=
	      NULL);
	CHECK_EQ(get_rsp(c[4], &st, pay, sizeof(pay), &n), LOOK_CLOSED);
	CHECK_EQ(held(), 1u + OMCI_CTL_MAX_CLIENTS);
	for (unsigned int i = 0; i < 4; i++)
		CHECK_EQ(ask(c[i], OMCI_CTL_OP_STATS, 0, 0, 0, pay, sizeof(pay),
			     &n),
			 OMCI_CTL_OK);
	(void)close(c[4]);

	(void)close(c[0]);
	settle(g_ctl);
	CHECK_EQ(held(), 1u + 3u);
	CHECK_EQ(shutdown(c[1], SHUT_WR), 0);
	settle(g_ctl);
	CHECK_EQ(held(), 1u + 2u);
	CHECK_EQ(get_rsp(c[1], &st, pay, sizeof(pay), &n), LOOK_CLOSED);
	(void)close(c[1]);

	c[0] = dial();
	settle(g_ctl);
	CHECK_EQ(held(), 1u + 3u);
	CHECK_EQ(ask(c[0], OMCI_CTL_OP_STATS, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);

	put_req(c[2], OMCI_CTL_OP_STATS, 0, 0, 0);
	fault_fail(FAULT_RECV, 1, EINTR);
	settle(g_ctl);
	CHECK_EQ(fault_fired(FAULT_RECV), 1u);
	CHECK_EQ(get_rsp(c[2], &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(st, OMCI_CTL_OK);
	CHECK_EQ(held(), 1u + 3u);
	fault_reset();
	put_req(c[2], OMCI_CTL_OP_STATS, 0, 0, 0);
	fault_fail(FAULT_RECV, 1, EAGAIN);
	settle(g_ctl);
	CHECK_EQ(fault_fired(FAULT_RECV), 1u);
	CHECK_EQ(get_rsp(c[2], &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(held(), 1u + 3u);
	fault_reset();

	put_req(c[2], OMCI_CTL_OP_STATS, 0, 0, 0);
	fault_fail(FAULT_RECV, 1, ECONNRESET);
	settle(g_ctl);
	CHECK_EQ(fault_fired(FAULT_RECV), 1u);
	CHECK_EQ(held(), 1u + 2u);
	CHECK_EQ(get_rsp(c[2], &st, pay, sizeof(pay), &n), LOOK_CLOSED);
	(void)close(c[2]);
	fault_reset();

	put_req(c[3], OMCI_CTL_OP_STATS, 0, 0, 0);
	fault_fail(FAULT_SEND, 1, EPIPE);
	OMCI_T_CAPTURE_LOG(g_log, { settle(g_ctl); });
	CHECK(strstr(g_log,
		     "ctl: client write did not complete (errno 32) — dropping it") !=
	      NULL);
	CHECK_EQ(held(), 1u + 1u);
	CHECK_EQ(get_rsp(c[3], &st, pay, sizeof(pay), &n), LOOK_CLOSED);
	(void)close(c[3]);
	fault_reset();

	put_req(c[0], OMCI_CTL_OP_STATS, 0, 0, 0);
	fault_short(FAULT_SEND, 1, 5);
	OMCI_T_CAPTURE_LOG(g_log, { settle(g_ctl); });
	CHECK(strstr(g_log, "client write did not complete") != NULL);
	CHECK_EQ(held(), 1u);
	(void)close(c[0]);
	fault_reset();

	const int d = dial();

	fault_fail(FAULT_ACCEPT, 1, ECONNABORTED);
	put_req(d, OMCI_CTL_OP_STATS, 0, 0, 0);
	settle(g_ctl);
	CHECK_EQ(fault_fired(FAULT_ACCEPT), 1u);
	CHECK_EQ(get_rsp(d, &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(held(), 2u);
	(void)close(d);
	settle(g_ctl);
	fault_reset();

	for (unsigned int which = 1; which <= 2u; which++) {
		fault_reset();
		fault_fail(FAULT_FCNTL, which, EBADF);
		const int e = dial();

		OMCI_T_CAPTURE_LOG(g_log, { settle(g_ctl); });
		CHECK_EQ(fault_fired(FAULT_FCNTL), 1u);
		CHECK(strstr(g_log,
			     "ctl: cannot make a client non-blocking (errno 9) — refusing it") !=
		      NULL);
		CHECK_EQ(get_rsp(e, &st, pay, sizeof(pay), &n), LOOK_CLOSED);
		CHECK_EQ(held(), 1u);
		(void)close(e);
	}

	fault_reset();
	fault_return(FAULT_FCNTL, 1, 0, 0);
	const int z = dial();

	settle(g_ctl);
	CHECK_EQ(fault_fired(FAULT_FCNTL), 1u);
	CHECK_EQ(held(), 2u);
	CHECK_EQ(ask(z, OMCI_CTL_OP_STATS, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	(void)close(z);
	settle(g_ctl);

	fault_reset();
	const int f = dial();

	settle(g_ctl);
	struct fault_sockopt opts[4];

	CHECK_EQ(fault_sockopts(opts, 4), 1u);
	CHECK_EQ(opts[0].level, SOL_SOCKET);
	CHECK_EQ(opts[0].optname, SO_SNDBUF);
	CHECK_EQ(opts[0].a, 16 * 1024);
	CHECK_EQ(opts[0].len, sizeof(int));
	(void)close(f);
	settle(g_ctl);
	fault_reset();
	omci_log_level_set(OMCI_LOG_ERR);
	server_down();
	mib_down();
}

static void publish(const uint8_t tag)
{
	uint8_t frame[OMCI_FRAME_LEN];

	memset(frame, tag, sizeof(frame));
	omci_ctl_tap_publish(g_ctl, OMCI_CTL_TAP_RX_DISPATCH, tag, frame);
}

static void test_tap_failures(void)
{
	CHECK(mib_up(BARE));
	CHECK(server_up(&g_counters));
	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	uint16_t st = 0;
	size_t n = 0;

	const int a = dial();

	CHECK_EQ(ask(a, OMCI_CTL_OP_TAP, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	CHECK_EQ(n, 0u);
	publish(1);
	CHECK_EQ(get_rsp(a, &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(st, OMCI_CTL_OK);
	CHECK_EQ(n, OMCI_CTL_TAP_REC_LEN);
	CHECK_EQ(pay[OMCI_CTL_TAP_VERDICT], OMCI_CTL_TAP_RX_DISPATCH);
	CHECK_EQ(pay[OMCI_CTL_TAP_DETAIL], 1u);
	CHECK_EQ(pay[OMCI_CTL_TAP_FRAME], 1u);

	fault_fail(FAULT_SEND, 1, ECONNRESET);
	publish(2);
	CHECK_EQ(fault_fired(FAULT_SEND), 1u);
	CHECK_EQ(held(), 1u);
	CHECK_EQ(get_rsp(a, &st, pay, sizeof(pay), &n), LOOK_CLOSED);
	(void)close(a);

	fault_reset();
	const int b = dial();

	CHECK_EQ(ask(b, OMCI_CTL_OP_TAP, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	fault_short(FAULT_SEND, 1, 10);
	publish(3);
	CHECK_EQ(fault_fired(FAULT_SEND), 1u);
	CHECK_EQ(held(), 1u);
	(void)close(b);
	fault_reset();

	const int c = dial();

	CHECK_EQ(ask(c, OMCI_CTL_OP_TAP, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	fault_fail(FAULT_SEND, 1, EAGAIN);
	publish(4);
	CHECK_EQ(fault_fired(FAULT_SEND), 1u);
	CHECK_EQ(held(), 2u);
	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_NOTHING);
	settle(g_ctl);
	CHECK_EQ(get_rsp(c, &st, pay, sizeof(pay), &n), LOOK_RESPONSE);
	CHECK_EQ(pay[OMCI_CTL_TAP_DETAIL], 4u);
	(void)close(c);
	fault_reset();
	settle(g_ctl);

	const int d = dial();

	CHECK_EQ(ask(d, OMCI_CTL_OP_TAP, 0, 0, 0, pay, sizeof(pay), &n),
		 OMCI_CTL_OK);
	struct pollfd fds[OMCI_CTL_MAX_FDS];
	const size_t have = omci_ctl_pollfds(g_ctl, fds, OMCI_CTL_MAX_FDS);

	CHECK_EQ(have, 2u);
	(void)close(d);
	publish(5);
	CHECK_EQ(held(), 1u);
	CHECK(poll(fds, (nfds_t)have, 0) > 0);
	CHECK_EQ(fds[1].revents, POLLNVAL);
	omci_ctl_service(g_ctl, fds, have);
	CHECK_EQ(held(), 1u);
	server_down();
	mib_down();
}

static size_t row_as_stored(uint16_t class_id, uint16_t instance, uint8_t *out,
			    size_t cap)
{
	const struct omci_me_class *cls = omci_me_class_by_id(class_id);
	size_t off = 0;

	for (uint8_t attr = 1; cls != NULL && attr <= cls->attr_count; attr++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);
		size_t n = 0;

		if (a == NULL || omci_me_attr_is_table(a) ||
		    off + a->size > cap)
			continue;
		CHECK_EQ(omci_mib_attr_get(g_mib, class_id, instance, attr,
					   &out[off], a->size, &n),
			 OMCI_OK);
		CHECK_EQ(n, (size_t)a->size);
		off += n;
	}
	return off;
}

static void test_client_library(void)
{
	CHECK(mib_up(ME171));
	CHECK(server_up(&g_counters));
	struct omcid_conn *conn = omcid_connect(g_sock);

	CHECK(conn != NULL);
	if (conn == NULL) {
		server_down();
		mib_down();
		return;
	}
	fault_hook(FAULT_RECV, serve_hook, g_ctl);

	struct omcid_conn_stats stats = { 0, 0, 0 };

	CHECK_EQ(omcid_stats(conn, &stats), 0);
	CHECK_EQ(stats.handled, 7u);
	CHECK_EQ(stats.rejected, 2u);
	CHECK_EQ(stats.answered, 9u);

	uint16_t inst[32];
	size_t n = 0;

	CHECK_EQ(omcid_me_list(conn, 5, inst, 32, &n), 0);
	CHECK_EQ(n, 3u);

	uint8_t row[OMCI_CTL_PAYLOAD_MAX];
	size_t got = 0;

	CHECK_EQ(omcid_me_get(conn, 5, inst[0], row, sizeof(row), &got), 0);
	CHECK(got > 0u);

	{
		static const struct {
			uint16_t cls;
			uint16_t inst;
		} rows[] = { { 5, 0 }, { 5, 0 }, { 256, 0 }, { 263, 0x8001 } };
		uint8_t want[OMCI_CTL_PAYLOAD_MAX];

		for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
			const uint16_t instance = i < 2u ? inst[i] :
							   rows[i].inst;
			const size_t want_n = row_as_stored(
				rows[i].cls, instance, want, sizeof(want));
			size_t have_n = 0;

			memset(row, 0xee, sizeof(row));
			CHECK_EQ(omcid_me_get(conn, rows[i].cls, instance, row,
					      sizeof(row), &have_n),
				 0);
			CHECK(want_n > 0u);
			CHECK_EQ(have_n, want_n);
			CHECK_MEM(row, want, want_n);
		}
	}

	size_t none = 99;

	CHECK_EQ(omcid_me_list(conn, 9999, inst, 32, &none), 0);
	CHECK_EQ(none, 0u);

	CHECK_EQ(omcid_me_get(conn, 5, 0xBEEF, row, sizeof(row), &got),
		 OMCI_CTL_ERR_NOT_FOUND);
	CHECK_EQ(omcid_stats(conn, &stats), 0);
	CHECK_EQ(stats.handled, 7u);

	{
		struct omcid_vlan_fate_entry rec[128];
		size_t nf = 0, total = 0;
		unsigned int over = 99u;

		CHECK_EQ(omcid_vlan_fate_list(conn, rec, ARRAY_SIZE(rec), &nf,
					      &total, &over),
			 0);

		CHECK_EQ(total, 128u);
		CHECK_EQ(nf, 128u);
		CHECK_EQ(over, 16u);
		CHECK(total > 3u * OMCI_CTL_VLAN_PAGE_MAX);

		/*
		 * Instance 0x101 in list order, sorted on the filter: the rows
		 * of VID 410 by their TPID field, 0, 3, 5, 6, then VID 499.
		 */
		static const uint8_t listed[5] = { 2u, 4u, 1u, 0u, 3u };

		CHECK_EQ(rec[0].fate, OMCI_CTL_VLAN_FATE_PROGRAMMED);
		CHECK_EQ(rec[0].match_cvid, 410u);
		CHECK_EQ(rec[0].net_vid, 1410u);

		CHECK_EQ(rec[1].fate, OMCI_CTL_VLAN_FATE_SHADOWED);
		CHECK_EQ(rec[1].match_cvid, 410u);
		CHECK_EQ(rec[1].net_vid, 1900u);
		CHECK_EQ(rec[1].rival_match, 410u);
		CHECK_EQ(rec[1].rival_net, 1410u);

		CHECK_EQ(rec[2].fate, OMCI_CTL_VLAN_FATE_COLLAPSED);
		CHECK_EQ(rec[3].fate, OMCI_CTL_VLAN_FATE_COLLAPSED);

		CHECK_EQ(rec[4].fate, OMCI_CTL_VLAN_FATE_PROGRAMMED);
		CHECK_EQ(rec[4].match_cvid, 499u);
		CHECK_EQ(rec[4].net_vid, 1410u);

		unsigned int seen_0101 = 0, seen_0102 = 0;

		for (size_t i = 0; i < nf; i++) {
			if (rec[i].instance == 0x101u) {
				CHECK_EQ(rec[i].row, listed[seen_0101]);
				seen_0101++;
			} else {
				CHECK_EQ(rec[i].instance, 0x102u);
				CHECK_EQ(rec[i].row, (uint8_t)seen_0102);
				seen_0102++;
			}
		}
		CHECK_EQ(seen_0101, 5u);
		CHECK_EQ(seen_0102, 123u);
		CHECK_EQ(rec[nf - 1u].fate, OMCI_CTL_VLAN_FATE_OVER_BUDGET);

		size_t only_total = 0, only_n = 99u;

		CHECK_EQ(omcid_vlan_fate_list(conn, NULL, 200u, &only_n,
					      &only_total, NULL),
			 0);
		CHECK_EQ(only_n, 0u);
		CHECK_EQ(only_total, 128u);

		struct omcid_vlan_fate_entry few[2];
		size_t few_n = 0, said = 0;

		CHECK_EQ(omcid_vlan_fate_list(conn, few, 2u, &few_n, &said,
					      NULL),
			 0);
		CHECK_EQ(few_n, 2u);
		CHECK_EQ(said, 128u);
	}

	CHECK_EQ(omcid_tap_subscribe(conn), 0);
	fault_hook(FAULT_RECV, NULL, NULL);
	for (uint8_t t = 1; t <= 3u; t++)
		publish(t);
	for (uint8_t t = 1; t <= 3u; t++) {
		struct omcid_tap_record r;

		memset(&r, 0xee, sizeof(r));
		CHECK_EQ(omcid_tap_next(conn, &r), 1);
		CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_DISPATCH);
		CHECK_EQ(r.detail, t);
		CHECK_EQ(r.dropped, 0u);
		CHECK_EQ(r.frame[0], t);
		CHECK_EQ(r.frame[OMCI_FRAME_LEN - 1u], t);
	}
	publish(9);
	CHECK_EQ(omcid_tap_next(conn, NULL), 1);

	fault_fail(FAULT_RECV, 1, EAGAIN);
	struct omcid_tap_record r;

	CHECK_EQ(omcid_tap_next(conn, &r), 0);

	omcid_disconnect(conn);
	settle(g_ctl);
	CHECK_EQ(held(), 1u);
	fault_reset();

	const int c = dial();
	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	size_t pn = 0;

	CHECK_EQ(ask(c, OMCI_CTL_OP_VLAN_FATES, 0, 0, 0, pay, sizeof(pay), &pn),
		 OMCI_CTL_OK);
	CHECK_EQ(pn, OMCI_CTL_VLAN_HDR_LEN +
			     OMCI_CTL_VLAN_PAGE_MAX * OMCI_CTL_VLAN_REC_LEN);
	CHECK_EQ(omci_be16(&pay[OMCI_CTL_VLAN_TOTAL]), 128u);
	CHECK_EQ(omci_be16(&pay[OMCI_CTL_VLAN_OVERFLOW]), 16u);
	CHECK_EQ(ask(c, OMCI_CTL_OP_VLAN_FATES, 0, 0, 500, pay, sizeof(pay),
		     &pn),
		 OMCI_CTL_OK);
	CHECK_EQ(pn, OMCI_CTL_VLAN_HDR_LEN);
	CHECK_EQ(omci_be16(&pay[OMCI_CTL_VLAN_TOTAL]), 128u);
	(void)close(c);
	server_down();
	mib_down();
}

static void test_a_client_may_be_descriptor_zero(void)
{
	if (fcntl(0, F_GETFD) < 0)
		(void)open("/dev/null", O_RDONLY);
	const int keep = dup(0);

	CHECK(keep > 2);

	CHECK(mib_up(BARE));
	CHECK(server_up(&g_counters));
	const int client = socket(AF_UNIX, SOCK_STREAM, 0);

	CHECK(client > 2);

	(void)close(0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };

	memcpy(addr.sun_path, g_sock, strlen(g_sock) + 1u);
	CHECK_EQ(connect(client, (const struct sockaddr *)&addr, sizeof(addr)),
		 0);
	settle(g_ctl);

	CHECK_EQ(held(), 2u);
	uint16_t status = 0xffffu;
	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	size_t paylen = 0;

	put_req(client, OMCI_CTL_OP_STATS, 0, 0, 0);
	settle(g_ctl);
	CHECK_EQ(get_rsp(client, &status, pay, sizeof(pay), &paylen),
		 LOOK_RESPONSE);
	CHECK_EQ(status, OMCI_CTL_OK);

	CHECK_EQ(ask(client, OMCI_CTL_OP_TAP, 0, 0, 0, pay, sizeof(pay),
		     &paylen),
		 OMCI_CTL_OK);
	publish(7);
	settle(g_ctl);
	CHECK_EQ(get_rsp(client, &status, pay, sizeof(pay), &paylen),
		 LOOK_RESPONSE);
	CHECK_EQ(paylen, (size_t)OMCI_CTL_TAP_REC_LEN);
	CHECK_EQ(pay[OMCI_CTL_TAP_DETAIL], 7u);

	server_down();
	CHECK_EQ(fcntl(0, F_GETFD), -1);

	(void)close(client);
	mib_down();
	CHECK_EQ(dup2(keep, 0), 0);
	(void)close(keep);
}

static void test_the_listener_may_be_descriptor_zero(void)
{
	if (fcntl(0, F_GETFD) < 0)
		(void)open("/dev/null", O_RDONLY);
	const int keep = dup(0);

	CHECK(keep > 2);

	CHECK(mib_up(BARE));
	(void)close(0);
	CHECK(server_up(&g_counters));

	struct pollfd fds[OMCI_CTL_MAX_FDS];

	CHECK_EQ(omci_ctl_pollfds(g_ctl, fds, OMCI_CTL_MAX_FDS), 1u);
	CHECK_EQ(fds[0].fd, 0);
	CHECK_EQ(held(), 1u);

	const int client = dial();

	CHECK(client > 2);
	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	size_t paylen = 0;

	CHECK_EQ(ask(client, OMCI_CTL_OP_STATS, 0, 0, 0, pay, sizeof(pay),
		     &paylen),
		 OMCI_CTL_OK);
	CHECK_EQ(held(), 2u);

	(void)close(client);
	settle(g_ctl);
	server_down();
	CHECK_EQ(fcntl(0, F_GETFD), -1);
	mib_down();
	CHECK_EQ(dup2(keep, 0), 0);
	(void)close(keep);
}

int main(void)
{
	snprintf(g_dir, sizeof(g_dir), "/tmp/omcid-test-ctl-%ld",
		 (long)getpid());
	if (mkdir(g_dir, 0700) != 0) {
		printf("test_ctl: cannot make %s\n", g_dir);
		return 1;
	}
	snprintf(g_sock, sizeof(g_sock), "%s/ctl.sock", g_dir);
	omci_log_level_set(OMCI_LOG_ERR);

	RUN(test_client_library);
	RUN(test_open_and_close);
	RUN(test_replies);
	RUN(test_requests_in_pieces);
	RUN(test_clients);
	RUN(test_tap_failures);
	RUN(test_a_client_may_be_descriptor_zero);
	RUN(test_the_listener_may_be_descriptor_zero);

	(void)unlink(g_sock);
	(void)rmdir(g_dir);
	return OMCI_T_REPORT("test_ctl");
}
