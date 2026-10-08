// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the client library against a stand-in for the daemon. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "omcid/attrs.h"
#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "omcid/libomcid.h"
#include "omci/frame.h"

#include "fault.h"
#include "harness.h"

static char g_dir[64];
static char g_sock[128];

struct fake {
	int lfd;
	int cfd;
	unsigned int requests;
	uint8_t req[OMCI_CTL_REQ_LEN];
	uint8_t reply[2048];
	size_t reply_len;
	bool close_after;
	uint8_t push[256];
	size_t push_len;

	size_t (*respond)(struct fake *f, const uint8_t req[OMCI_CTL_REQ_LEN],
			  uint8_t *out);
};

static struct fake g_fake;

static void fake_close_peer(struct fake *f)
{
	if (f->cfd >= 0)
		(void)close(f->cfd);
	f->cfd = -1;
}

static void fake_serve(void *arg)
{
	struct fake *f = arg;

	if (f->cfd < 0)
		f->cfd = accept(f->lfd, NULL, NULL);
	if (f->cfd < 0)
		return;

	bool acted = false;

	if (f->push_len > 0u) {
		(void)send(f->cfd, f->push, f->push_len, MSG_NOSIGNAL);
		f->push_len = 0;
		acted = true;
	}
	uint8_t req[OMCI_CTL_REQ_LEN];

	if (recv(f->cfd, req, sizeof(req), MSG_DONTWAIT) ==
	    (ssize_t)sizeof(req)) {
		memcpy(f->req, req, sizeof(req));
		f->requests++;

		size_t len = f->reply_len;

		if (f->respond != NULL)
			len = f->respond(f, req, f->reply);
		if (len > 0u)
			(void)send(f->cfd, f->reply, len, MSG_NOSIGNAL);
		acted = true;
	}
	if (acted && f->close_after)
		fake_close_peer(f);
}

static bool fake_up(void)
{
	memset(&g_fake, 0, sizeof(g_fake));
	g_fake.cfd = -1;
	(void)unlink(g_sock);
	g_fake.lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };

	memcpy(addr.sun_path, g_sock, strlen(g_sock) + 1u);
	if (g_fake.lfd < 0 ||
	    bind(g_fake.lfd, (const struct sockaddr *)&addr, sizeof(addr)) !=
		    0 ||
	    listen(g_fake.lfd, 4) != 0)
		return false;

	(void)fcntl(g_fake.lfd, F_SETFL,
		    fcntl(g_fake.lfd, F_GETFL, 0) | O_NONBLOCK);
	fault_reset();
	fault_hook(FAULT_RECV, fake_serve, &g_fake);
	return true;
}

static void fake_down(void)
{
	fault_reset();
	fake_close_peer(&g_fake);
	if (g_fake.lfd >= 0)
		(void)close(g_fake.lfd);
	g_fake.lfd = -1;
	(void)unlink(g_sock);
}

static void set_reply_raw(uint32_t magic, uint16_t status, size_t len,
			  const uint8_t *pay, size_t n)
{
	omci_be32_store(&g_fake.reply[OMCI_CTL_RSP_MAGIC], magic);
	omci_be16_store(&g_fake.reply[OMCI_CTL_RSP_STATUS], status);
	omci_be16_store(&g_fake.reply[OMCI_CTL_RSP_PAYLEN], (unsigned int)len);
	if (n > 0u)
		memcpy(&g_fake.reply[OMCI_CTL_RSP_LEN], pay, n);
	g_fake.reply_len = OMCI_CTL_RSP_LEN + n;
}

static void set_reply(uint16_t status, const uint8_t *pay, size_t n)
{
	set_reply_raw(OMCI_CTL_MAGIC, status, n, pay, n);
}

static struct omcid_conn *connect_fake(void)
{
	struct omcid_conn *c = omcid_connect(g_sock);

	CHECK(c != NULL);
	return c;
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

static void test_connect(void)
{
	CHECK(fake_up());

	fault_fail(FAULT_CONNECT, 1, ECONNREFUSED);
	errno = 0;
	CHECK(omcid_connect(NULL) == NULL);
	CHECK_EQ(errno, ECONNREFUSED);
	CHECK_EQ(strcmp(fault_connect_path(), OMCI_CTL_SOCKET_PATH), 0);

	char longpath[200];

	memset(longpath, 'x', sizeof(longpath) - 1u);
	longpath[sizeof(longpath) - 1u] = '\0';
	fault_reset();
	errno = 0;
	CHECK(omcid_connect(longpath) == NULL);
	CHECK_EQ(errno, ENAMETOOLONG);
	CHECK_EQ(fault_seen(FAULT_SOCKET), 0u);
	fault_hook(FAULT_RECV, fake_serve, &g_fake);

	fault_fail(FAULT_SOCKET, 1, EMFILE);
	errno = 0;
	CHECK(omcid_connect(g_sock) == NULL);
	CHECK_EQ(errno, EMFILE);
	CHECK_EQ(fault_fired(FAULT_SOCKET), 1u);

	const int free_before = lowest_free();

	errno = 0;
	CHECK(omcid_connect("/nonexistent-directory/omcid.sock") == NULL);
	CHECK_EQ(errno, ENOENT);
	CHECK_EQ(lowest_free(), free_before);

	fault_fail(FAULT_CONNECT, 1, ECONNREFUSED);
	errno = 0;
	poison_stack();
	CHECK(omcid_connect(g_sock) == NULL);
	CHECK_EQ(errno, ECONNREFUSED);
	CHECK_EQ(lowest_free(), free_before);
	{
		const char *named = fault_connect_path();
		unsigned int not_zero = 0;

		CHECK_EQ(strcmp(named, g_sock), 0);
		for (size_t i = strlen(g_sock) + 1u;
		     i < sizeof((struct sockaddr_un *)0)->sun_path; i++)
			not_zero += named[i] != '\0' ? 1u : 0u;
		CHECK_EQ(not_zero, 0u);
	}
	fault_reset();
	fault_hook(FAULT_RECV, fake_serve, &g_fake);

	struct omcid_conn *conn = omcid_connect(g_sock);

	CHECK(conn != NULL);
	struct fault_sockopt opts[4];

	CHECK_EQ(fault_sockopts(opts, 4), 2u);
	CHECK_EQ(opts[0].level, SOL_SOCKET);
	CHECK_EQ(opts[0].optname, SO_RCVTIMEO);
	CHECK_EQ(opts[0].len, sizeof(struct timeval));
	CHECK_EQ(opts[0].a, 5);
	CHECK_EQ(opts[0].b, 0);
	CHECK_EQ(opts[1].level, SOL_SOCKET);
	CHECK_EQ(opts[1].optname, SO_SNDTIMEO);
	CHECK_EQ(opts[1].a, 5);
	CHECK_EQ(opts[1].b, 0);

	const unsigned int sockets = fault_seen(FAULT_SOCKET);

	errno = 0;
	CHECK(omcid_connect(g_sock) == NULL);
	CHECK_EQ(errno, EBUSY);
	CHECK_EQ(fault_seen(FAULT_SOCKET), sockets);

	set_reply(OMCI_CTL_OK, (const uint8_t[12]){ 0 }, 12);
	struct omcid_conn_stats st;

	CHECK_EQ(omcid_stats(conn, &st), 0);
	omcid_disconnect(conn);
	uint8_t b;

	CHECK_EQ(recv(g_fake.cfd, &b, 1, MSG_DONTWAIT), 0);
	omcid_disconnect(conn);
	omcid_disconnect(NULL);

	errno = 0;
	CHECK_EQ(omcid_stats(conn, &st), -1);
	CHECK_EQ(errno, EBADF);
	errno = 0;
	CHECK_EQ(omcid_stats(NULL, &st), -1);
	CHECK_EQ(errno, EBADF);
	struct omcid_tap_record rec;

	errno = 0;
	CHECK_EQ(omcid_tap_next(conn, &rec), -1);
	CHECK_EQ(errno, EBADF);
	errno = 0;
	CHECK_EQ(omcid_tap_next(NULL, &rec), -1);
	CHECK_EQ(errno, EBADF);

	fake_close_peer(&g_fake);
	conn = connect_fake();
	CHECK_EQ(omcid_stats(conn, &st), 0);
	omcid_disconnect(conn);
	fake_down();
}

static void test_requests(void)
{
	CHECK(fake_up());
	struct omcid_conn *conn = connect_fake();
	uint8_t out[64];
	size_t n = 99;
	uint16_t ids[8];

	set_reply(OMCI_CTL_OK, (const uint8_t[3]){ 1, 2, 3 }, 3);
	CHECK_EQ(omcid_me_get(conn, 0x1234, 0x5678, out, sizeof(out), &n), 0);
	CHECK_EQ(g_fake.requests, 1u);
	CHECK_EQ(omci_be32(&g_fake.req[OMCI_CTL_REQ_MAGIC]), OMCI_CTL_MAGIC);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_OP]), OMCI_CTL_OP_ME_GET);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_CLASS]), 0x1234u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_INSTANCE]), 0x5678u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_ARG]), 0u);
	CHECK_EQ(n, 3u);
	CHECK_MEM(out, ((const uint8_t[3]){ 1, 2, 3 }), 3);

	set_reply(OMCI_CTL_OK,
		  (const uint8_t[10]){ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 },
		  10);
	memset(out, 0xee, sizeof(out));
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, 4, &n), 0);
	CHECK_EQ(n, 4u);
	CHECK_MEM(out, ((const uint8_t[5]){ 10, 11, 12, 13, 0xee }), 5);
	CHECK_EQ(omcid_me_get(conn, 1, 2, NULL, 4, &n), 0);
	CHECK_EQ(n, 4u);
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, sizeof(out), NULL), 0);
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, 0, &n), 0);
	CHECK_EQ(n, 0u);

	set_reply(OMCI_CTL_OK, (const uint8_t[5]){ 1, 0, 2, 0xab, 0xcd }, 5);
	CHECK_EQ(omcid_me_dump(conn, 0x0100, 0x0002, out, sizeof(out), &n), 0);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_OP]), OMCI_CTL_OP_ME_DUMP);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_CLASS]), 0x0100u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_INSTANCE]), 0x0002u);
	CHECK_EQ(n, 5u);
	CHECK_MEM(out, ((const uint8_t[5]){ 1, 0, 2, 0xab, 0xcd }), 5);
	memset(out, 0xee, sizeof(out));
	CHECK_EQ(omcid_me_dump(conn, 1, 2, out, 2, &n), 0);
	CHECK_EQ(n, 2u);
	CHECK_MEM(out, ((const uint8_t[3]){ 1, 0, 0xee }), 3);
	CHECK_EQ(omcid_me_dump(conn, 1, 2, NULL, 4, NULL), 0);
	set_reply(OMCI_CTL_ERR_TOO_BIG, NULL, 0);
	CHECK_EQ(omcid_me_dump(conn, 1, 2, out, sizeof(out), &n),
		 OMCI_CTL_ERR_TOO_BIG);

	set_reply(OMCI_CTL_OK,
		  (const uint8_t[6]){ 0x01, 0x01, 0x80, 0x00, 0xab, 0xcd }, 6);
	CHECK_EQ(omcid_me_list(conn, 5, ids, 8, &n), 0);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_OP]), OMCI_CTL_OP_ME_LIST);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_CLASS]), 5u);
	CHECK_EQ(n, 3u);
	CHECK_EQ(ids[0], 0x0101u);
	CHECK_EQ(ids[1], 0x8000u);
	CHECK_EQ(ids[2], 0xabcdu);
	ids[0] = ids[1] = ids[2] = 0;
	CHECK_EQ(omcid_me_list(conn, 5, ids, 2, &n), 0);
	CHECK_EQ(n, 2u);
	CHECK_EQ(ids[1], 0x8000u);
	CHECK_EQ(ids[2], 0u);
	CHECK_EQ(omcid_me_list(conn, 5, NULL, 8, &n), 0);
	CHECK_EQ(n, 3u);
	CHECK_EQ(omcid_me_list(conn, 5, ids, 8, NULL), 0);

	uint8_t cnt[OMCI_CTL_STATS_LEN];

	omci_be32_store(&cnt[0], 11);
	omci_be32_store(&cnt[4], 22);
	omci_be32_store(&cnt[8], 33);
	set_reply(OMCI_CTL_OK, cnt, sizeof(cnt));
	struct omcid_conn_stats st = { 0, 0, 0 };

	CHECK_EQ(omcid_stats(conn, &st), 0);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_OP]), OMCI_CTL_OP_STATS);
	CHECK_EQ(st.handled, 11u);
	CHECK_EQ(st.rejected, 22u);
	CHECK_EQ(st.answered, 33u);
	CHECK_EQ(omcid_stats(conn, NULL), 0);
	set_reply(OMCI_CTL_OK, cnt, 8);
	errno = 0;
	CHECK_EQ(omcid_stats(conn, &st), -1);
	CHECK_EQ(errno, EPROTO);

	set_reply(OMCI_CTL_ERR_TOO_BIG, NULL, 0);
	n = 7;
	CHECK_EQ(omcid_me_list(conn, 5, ids, 8, &n), (int)OMCI_CTL_ERR_TOO_BIG);
	CHECK_EQ(n, 7u);
	set_reply(OMCI_CTL_ERR_PROTO, NULL, 0);
	st.handled = 123;
	CHECK_EQ(omcid_stats(conn, &st), (int)OMCI_CTL_ERR_PROTO);
	CHECK_EQ(st.handled, 123u);

	set_reply(OMCI_CTL_ERR_NOT_FOUND, (const uint8_t[4]){ 9, 9, 9, 9 }, 4);
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, sizeof(out), &n),
		 (int)OMCI_CTL_ERR_NOT_FOUND);
	set_reply(OMCI_CTL_OK, cnt, sizeof(cnt));
	CHECK_EQ(omcid_stats(conn, &st), 0);
	CHECK_EQ(st.answered, 33u);

	set_reply(OMCI_CTL_OK, (const uint8_t[1]){ 0x5a }, 1);
	memset(out, 0, sizeof(out));
	n = 99;
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, sizeof(out), &n), 0);
	CHECK_EQ(n, 1u);
	CHECK_EQ(out[0], 0x5au);
	set_reply(OMCI_CTL_ERR_NOT_FOUND, (const uint8_t[1]){ 0x77 }, 1);
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, sizeof(out), &n),
		 (int)OMCI_CTL_ERR_NOT_FOUND);
	set_reply(OMCI_CTL_OK, cnt, sizeof(cnt));
	CHECK_EQ(omcid_stats(conn, &st), 0);
	CHECK_EQ(st.handled, 11u);
	CHECK_EQ(st.rejected, 22u);
	CHECK_EQ(st.answered, 33u);

	omcid_disconnect(conn);
	fake_down();
}

static void test_the_wire_misbehaves(void)
{
	uint8_t out[64];
	size_t n = 0;
	struct omcid_conn_stats st;
	struct omcid_conn *conn;

	CHECK(fake_up());
	conn = connect_fake();
	set_reply_raw(0x12345678u, OMCI_CTL_OK, 0, NULL, 0);
	errno = 0;
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, sizeof(out), &n), -1);
	CHECK_EQ(errno, EPROTO);
	omcid_disconnect(conn);
	fake_down();

	CHECK(fake_up());
	conn = connect_fake();
	set_reply_raw(OMCI_CTL_MAGIC, OMCI_CTL_OK, OMCI_CTL_PAYLOAD_MAX + 1u,
		      NULL, 0);
	errno = 0;
	CHECK_EQ(omcid_me_get(conn, 1, 2, out, sizeof(out), &n), -1);
	CHECK_EQ(errno, EPROTO);
	omcid_disconnect(conn);
	fake_down();

	for (unsigned int where = 0; where < 3u; where++) {
		CHECK(fake_up());
		conn = connect_fake();
		if (where == 0u)
			g_fake.reply_len = 0;
		else if (where == 1u) {
			set_reply(OMCI_CTL_OK, NULL, 0);
			g_fake.reply_len = 5;
		} else {
			set_reply_raw(OMCI_CTL_MAGIC, OMCI_CTL_OK, 12,
				      (const uint8_t[5]){ 1, 2, 3, 4, 5 }, 5);
		}
		g_fake.close_after = true;
		errno = 0;
		CHECK_EQ(omcid_stats(conn, &st), -1);
		CHECK_EQ(errno, ECONNRESET);
		omcid_disconnect(conn);
		fake_down();
	}

	CHECK(fake_up());
	conn = connect_fake();
	g_fake.reply_len = 0;
	fault_fail(FAULT_RECV, 1, EAGAIN);
	errno = 0;
	CHECK_EQ(omcid_stats(conn, &st), -1);
	CHECK_EQ(errno, EAGAIN);
	omcid_disconnect(conn);
	fake_down();

	CHECK(fake_up());
	conn = connect_fake();
	uint8_t cnt[OMCI_CTL_STATS_LEN] = { 0 };

	omci_be32_store(&cnt[0], 5);
	set_reply(OMCI_CTL_OK, cnt, sizeof(cnt));
	fault_fail(FAULT_SEND, 1, EINTR);
	CHECK_EQ(omcid_stats(conn, &st), 0);
	CHECK_EQ(fault_fired(FAULT_SEND), 1u);
	CHECK_EQ(st.handled, 5u);
	fault_fail(FAULT_RECV, 1, EINTR);
	CHECK_EQ(omcid_stats(conn, &st), 0);
	CHECK_EQ(fault_fired(FAULT_RECV), 1u);
	CHECK_EQ(st.handled, 5u);

	const unsigned int before = g_fake.requests;

	fault_short(FAULT_SEND, 1, 5);
	CHECK_EQ(omcid_me_get(conn, 0x0102, 0x0304, out, sizeof(out), &n), 0);
	CHECK_EQ(g_fake.requests, before + 1u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_CLASS]), 0x0102u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_INSTANCE]), 0x0304u);

	const unsigned int fired_before = fault_fired(FAULT_SEND);

	fault_return(FAULT_SEND, 1, 0, 0);
	errno = EINTR;
	CHECK_EQ(omcid_stats(conn, &st), -1);
	CHECK_EQ(fault_fired(FAULT_SEND), fired_before + 1u);
	set_reply(OMCI_CTL_OK, cnt, sizeof(cnt));
	CHECK_EQ(omcid_stats(conn, &st), 0);
	CHECK_EQ(st.handled, 5u);

	fault_fail(FAULT_SEND, 1, EPIPE);
	errno = 0;
	CHECK_EQ(omcid_stats(conn, &st), -1);
	CHECK_EQ(errno, EPIPE);
	omcid_disconnect(conn);
	fake_down();
}

static void push_record(uint16_t dropped, size_t len, uint32_t magic)
{
	uint8_t *p = g_fake.push;

	omci_be32_store(&p[OMCI_CTL_RSP_MAGIC], magic);
	omci_be16_store(&p[OMCI_CTL_RSP_STATUS], OMCI_CTL_OK);
	omci_be16_store(&p[OMCI_CTL_RSP_PAYLEN], (unsigned int)len);
	uint8_t *rec = &p[OMCI_CTL_RSP_LEN];

	memset(rec, 0, OMCI_CTL_TAP_REC_LEN);
	rec[OMCI_CTL_TAP_VERDICT] = 6;
	rec[OMCI_CTL_TAP_DETAIL] = 77;
	omci_be16_store(&rec[OMCI_CTL_TAP_DROPPED], dropped);
	for (unsigned int i = 0; i < OMCI_FRAME_LEN; i++)
		rec[OMCI_CTL_TAP_FRAME + i] = (uint8_t)(0x40u + i);
	g_fake.push_len = OMCI_CTL_RSP_LEN + OMCI_CTL_TAP_REC_LEN;
}

static void test_tap(void)
{
	struct omcid_tap_record rec;
	struct omcid_conn *conn;

	CHECK(fake_up());
	conn = connect_fake();
	set_reply(OMCI_CTL_OK, NULL, 0);
	CHECK_EQ(omcid_tap_subscribe(conn), 0);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_OP]), OMCI_CTL_OP_TAP);
	set_reply(OMCI_CTL_ERR_OP, NULL, 0);
	CHECK_EQ(omcid_tap_subscribe(conn), (int)OMCI_CTL_ERR_OP);

	push_record(258, OMCI_CTL_TAP_REC_LEN, OMCI_CTL_MAGIC);
	memset(&rec, 0xee, sizeof(rec));
	CHECK_EQ(omcid_tap_next(conn, &rec), 1);
	CHECK_EQ(rec.verdict, 6u);
	CHECK_EQ(rec.detail, 77u);
	CHECK_EQ(rec.dropped, 258u);
	for (unsigned int i = 0; i < OMCI_FRAME_LEN; i++)
		CHECK_EQ(rec.frame[i], 0x40u + i);
	push_record(0, OMCI_CTL_TAP_REC_LEN, OMCI_CTL_MAGIC);
	CHECK_EQ(omcid_tap_next(conn, NULL), 1);

	fault_fail(FAULT_RECV, 1, EAGAIN);
	CHECK_EQ(omcid_tap_next(conn, &rec), 0);
	fault_fail(FAULT_RECV, 1, ECONNRESET);
	errno = 0;
	CHECK_EQ(omcid_tap_next(conn, &rec), -1);
	CHECK_EQ(errno, ECONNRESET);
	omcid_disconnect(conn);
	fake_down();

	CHECK(fake_up());
	conn = connect_fake();
	push_record(0, OMCI_CTL_TAP_REC_LEN, 0x12345678u);
	errno = 0;
	CHECK_EQ(omcid_tap_next(conn, &rec), -1);
	CHECK_EQ(errno, EPROTO);
	omcid_disconnect(conn);
	fake_down();

	CHECK(fake_up());
	conn = connect_fake();
	push_record(0, OMCI_CTL_TAP_REC_LEN - 1u, OMCI_CTL_MAGIC);
	errno = 0;
	CHECK_EQ(omcid_tap_next(conn, &rec), -1);
	CHECK_EQ(errno, EPROTO);
	omcid_disconnect(conn);
	fake_down();

	CHECK(fake_up());
	conn = connect_fake();
	push_record(0, OMCI_CTL_TAP_REC_LEN, OMCI_CTL_MAGIC);
	g_fake.push_len = OMCI_CTL_RSP_LEN + 10u;
	g_fake.close_after = true;
	errno = 0;
	CHECK_EQ(omcid_tap_next(conn, &rec), -1);
	CHECK_EQ(errno, ECONNRESET);
	omcid_disconnect(conn);
	fake_down();
}

#define FATES 100u

static size_t fate_pages(struct fake *f, const uint8_t req[OMCI_CTL_REQ_LEN],
			 uint8_t *out)
{
	(void)f;
	const size_t first = omci_be16(&req[OMCI_CTL_REQ_ARG]);
	size_t off = OMCI_CTL_RSP_LEN + OMCI_CTL_VLAN_HDR_LEN;

	omci_be32_store(&out[OMCI_CTL_RSP_MAGIC], OMCI_CTL_MAGIC);
	omci_be16_store(&out[OMCI_CTL_RSP_LEN + OMCI_CTL_VLAN_TOTAL], FATES);
	omci_be16_store(&out[OMCI_CTL_RSP_LEN + OMCI_CTL_VLAN_OVERFLOW], 7);
	for (size_t i = first;
	     i < FATES && (i - first) < OMCI_CTL_VLAN_PAGE_MAX; i++) {
		omci_be16_store(&out[off + 0], (unsigned int)(0x100u + i));
		out[off + 2] = (uint8_t)i;
		out[off + 3] = (uint8_t)(i % 7u);
		omci_be16_store(&out[off + 4], (unsigned int)(1000u + i));
		omci_be16_store(&out[off + 6], (unsigned int)(2000u + i));
		omci_be16_store(&out[off + 8], (unsigned int)(3000u + i));
		omci_be16_store(&out[off + 10], (unsigned int)(4000u + i));
		off += OMCI_CTL_VLAN_REC_LEN;
	}
	omci_be16_store(&out[OMCI_CTL_RSP_STATUS], OMCI_CTL_OK);
	omci_be16_store(&out[OMCI_CTL_RSP_PAYLEN],
			(unsigned int)(off - OMCI_CTL_RSP_LEN));
	return off;
}

static void test_fate_list(void)
{
	static struct omcid_vlan_fate_entry rec[FATES + 10u];
	size_t n = 99, total = 99;
	unsigned int over = 99;

	CHECK(fake_up());
	struct omcid_conn *conn = connect_fake();

	g_fake.respond = fate_pages;

	CHECK_EQ(omcid_vlan_fate_list(conn, rec, FATES + 10u, &n, &total,
				      &over),
		 0);
	CHECK_EQ(n, FATES);
	CHECK_EQ(total, FATES);
	CHECK_EQ(over, 7u);
	CHECK_EQ(g_fake.requests, 4u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_OP]),
		 OMCI_CTL_OP_VLAN_FATES);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_CLASS]), 0u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_INSTANCE]), 0u);
	CHECK_EQ(omci_be16(&g_fake.req[OMCI_CTL_REQ_ARG]), 100u);
	for (size_t i = 0; i < FATES; i++) {
		CHECK_EQ(rec[i].instance, 0x100u + i);
		CHECK_EQ(rec[i].row, (uint8_t)i);
		CHECK_EQ(rec[i].fate, (uint8_t)(i % 7u));
		CHECK_EQ(rec[i].match_cvid, 1000u + i);
		CHECK_EQ(rec[i].net_vid, 2000u + i);
		CHECK_EQ(rec[i].rival_match, 3000u + i);
		CHECK_EQ(rec[i].rival_net, 4000u + i);
	}

	g_fake.requests = 0;
	CHECK_EQ(omcid_vlan_fate_list(conn, rec, 50, &n, &total, &over), 0);
	CHECK_EQ(n, 50u);
	CHECK_EQ(total, FATES);
	CHECK_EQ(g_fake.requests, 2u);
	CHECK_EQ(rec[49].instance, 0x100u + 49u);

	CHECK_EQ(omcid_vlan_fate_list(conn, NULL, 10, &n, &total, &over), 0);
	CHECK_EQ(n, 0u);
	CHECK_EQ(total, FATES);
	CHECK_EQ(omcid_vlan_fate_list(conn, rec, 10, NULL, NULL, NULL), 0);

	g_fake.respond = NULL;
	set_reply(OMCI_CTL_OK, (const uint8_t[2]){ 0, 5 }, 2);
	errno = 0;
	CHECK_EQ(omcid_vlan_fate_list(conn, rec, 10, &n, &total, &over), -1);
	CHECK_EQ(errno, EPROTO);
	CHECK_EQ(n, 0u);

	set_reply(OMCI_CTL_ERR_OP, NULL, 0);
	CHECK_EQ(omcid_vlan_fate_list(conn, rec, 10, &n, &total, &over),
		 (int)OMCI_CTL_ERR_OP);

	uint8_t hdr[OMCI_CTL_VLAN_HDR_LEN] = { 0 };

	set_reply(OMCI_CTL_OK, hdr, sizeof(hdr));
	CHECK_EQ(omcid_vlan_fate_list(conn, rec, 10, &n, &total, &over), 0);
	CHECK_EQ(n, 0u);
	CHECK_EQ(total, 0u);
	CHECK_EQ(over, 0u);
	omcid_disconnect(conn);
	fake_down();
}

int main(void)
{
	snprintf(g_dir, sizeof(g_dir), "/tmp/omcid-test-client-%ld",
		 (long)getpid());
	if (mkdir(g_dir, 0700) != 0) {
		printf("test_client: cannot make %s\n", g_dir);
		return 1;
	}
	snprintf(g_sock, sizeof(g_sock), "%s/fake.sock", g_dir);

	RUN(test_connect);
	RUN(test_requests);
	RUN(test_the_wire_misbehaves);
	RUN(test_tap);
	RUN(test_fate_list);

	(void)unlink(g_sock);
	(void)rmdir(g_dir);
	return OMCI_T_REPORT("test_client");
}
