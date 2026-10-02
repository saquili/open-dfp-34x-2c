// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of following the frames through the control socket. */

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "mib/store.h"
#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "sys/ctl.h"

#include "harness.h"

#define MT_CREATE 4u
#define MT_GET 9u
#define MT_MIB_RESET 15u

#define MAX_TX 128
static uint8_t g_tx[MAX_TX][OMCI_FRAME_LEN];
static unsigned int g_tx_n;

static void on_send(void *ctx, const uint8_t f[static OMCI_FRAME_LEN])
{
	(void)ctx;
	if (g_tx_n < MAX_TX)
		memcpy(g_tx[g_tx_n], f, OMCI_FRAME_LEN);
	g_tx_n++;
}
static const struct omci_tx_ops tx_ops = { .send = on_send };

static void on_tap(void *ctx, uint8_t verdict, uint8_t detail,
		   const uint8_t frame[static OMCI_FRAME_LEN])
{
	omci_ctl_tap_publish(ctx, verdict, detail, frame);
}
static const struct omci_tap_ops tap_ops = { .frame = on_tap };

static void build(uint8_t out[OMCI_FRAME_LEN], uint16_t tci, uint8_t mt,
		  bool ar, bool ak, uint16_t class_id, uint16_t inst)
{
	struct omci_frame f = {
		.tci = tci,
		.msg_type = mt,
		.ar = ar,
		.ak = ak,
		.device_id = 0x0a,
		.class_id = class_id,
		.instance = inst,
	};
	(void)!omci_frame_build(&f, out);
}

static char g_sock[64];

static void flush_ring(struct omci_ctl *ctl)
{
	struct pollfd fds[OMCI_CTL_MAX_FDS];
	const size_t n = omci_ctl_pollfds(ctl, fds, OMCI_CTL_MAX_FDS);

	if (poll(fds, (nfds_t)n, 50) > 0)
		omci_ctl_service(ctl, fds, n);
}

static void pump(struct omci_ctl *ctl)
{
	struct pollfd fds[OMCI_CTL_MAX_FDS];
	const size_t n = omci_ctl_pollfds(ctl, fds, OMCI_CTL_MAX_FDS);

	if (poll(fds, (nfds_t)n, 200) > 0)
		omci_ctl_service(ctl, fds, n);
}

static int client_connect(struct omci_ctl *ctl)
{
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;

	const int rcv = 1024;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, g_sock, strlen(g_sock) + 1u);
	if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
		(void)close(fd);
		return -1;
	}
	pump(ctl);
	return fd;
}

static void client_request(struct omci_ctl *ctl, int fd, uint16_t op)
{
	uint8_t req[OMCI_CTL_REQ_LEN];

	memset(req, 0, sizeof(req));
	omci_be32_store(&req[OMCI_CTL_REQ_MAGIC], OMCI_CTL_MAGIC);
	omci_be16_store(&req[OMCI_CTL_REQ_OP], op);
	CHECK_EQ(send(fd, req, sizeof(req), 0), (ssize_t)sizeof(req));
	pump(ctl);
}

static bool slurp(int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		const ssize_t n = recv(fd, &buf[got], len - got, 0);

		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return false;
	}
	return true;
}

static size_t read_reply(int fd, uint16_t *status, uint8_t *pay, size_t cap)
{
	uint8_t hdr[OMCI_CTL_RSP_LEN];

	if (!slurp(fd, hdr, sizeof(hdr)))
		return (size_t)-1;
	if (omci_be32(&hdr[OMCI_CTL_RSP_MAGIC]) != OMCI_CTL_MAGIC)
		return (size_t)-1;
	*status = (uint16_t)omci_be16(&hdr[OMCI_CTL_RSP_STATUS]);
	const size_t len = (size_t)omci_be16(&hdr[OMCI_CTL_RSP_PAYLEN]);

	if (len > cap || (len > 0 && !slurp(fd, pay, len)))
		return (size_t)-1;
	return len;
}

struct rec {
	uint8_t verdict;
	uint8_t detail;
	uint16_t dropped;
	uint8_t frame[OMCI_FRAME_LEN];
};

static bool read_record(int fd, struct rec *out)
{
	struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };

	if (poll(&p, 1, 2000) <= 0 || (p.revents & POLLIN) == 0)
		return false;

	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	uint16_t status = 0;

	if (read_reply(fd, &status, pay, sizeof(pay)) != OMCI_CTL_TAP_REC_LEN ||
	    status != OMCI_CTL_OK)
		return false;
	out->verdict = pay[OMCI_CTL_TAP_VERDICT];
	out->detail = pay[OMCI_CTL_TAP_DETAIL];
	out->dropped = (uint16_t)omci_be16(&pay[OMCI_CTL_TAP_DROPPED]);
	memcpy(out->frame, &pay[OMCI_CTL_TAP_FRAME], OMCI_FRAME_LEN);
	return true;
}

static bool readable(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };

	return poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0;
}

int main(void)
{
	(void)snprintf(g_sock, sizeof(g_sock), "/tmp/omcid-tap-test-%d.sock",
		       (int)getpid());

	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;
	struct omci_ctl *ctl = NULL;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &tx_ops, NULL), OMCI_OK);
	CHECK_EQ(omci_ctl_open(&ctl, g_sock, mib, NULL), OMCI_OK);
	omci_engine_tap_set(eng, &tap_ops, ctl);

	uint8_t f[OMCI_FRAME_LEN];
	uint8_t pay[OMCI_CTL_PAYLOAD_MAX];
	uint16_t status = 0;
	struct rec r;

	build(f, 1, MT_GET, true, false, 257, 0);
	g_tx_n = 0;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK_EQ(g_tx_n, 1u);

	const int tapfd = client_connect(ctl);

	CHECK(tapfd >= 0);
	client_request(ctl, tapfd, OMCI_CTL_OP_TAP);
	CHECK_EQ(read_reply(tapfd, &status, pay, sizeof(pay)), 0u);
	CHECK_EQ(status, OMCI_CTL_OK);

	CHECK(!readable(tapfd));

	build(f, 0x0042, MT_GET, true, false, 257, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);

	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_DISPATCH);
	CHECK_EQ(r.detail, MT_GET);
	CHECK_EQ(r.dropped, 0u);
	CHECK_EQ(memcmp(r.frame, f, OMCI_FRAME_LEN), 0);

	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);
	CHECK_EQ(r.detail, MT_GET);
	CHECK_EQ((r.frame[2] & 0x20u) != 0u, true);
	CHECK_EQ(omci_be16(&r.frame[0]) & 0x7FFFu, 0x0042u);
	CHECK(!readable(tapfd));

	build(f, 2, MT_GET, true, false, 257, 0);
	f[20] ^= 0xFFu;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_ERR_MIC);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_MIC);
	CHECK(!readable(tapfd));

	build(f, 3, MT_GET, false, false, 257, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_ERR_PROTO);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_ARAK);
	CHECK_EQ(r.detail, MT_GET);

	build(f, 4, MT_GET, true, false, 257, 0);
	f[3] = 0x0bu;
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_ERR_PROTO);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_PARSE);
	CHECK_EQ(r.frame[3], 0x0bu);

	build(f, 5, MT_CREATE, true, false, 256, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_ACTION);
	CHECK_EQ(r.detail, MT_CREATE);
	CHECK_EQ(memcmp(r.frame, f, OMCI_FRAME_LEN), 0);

	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);
	CHECK_EQ(r.frame[8], 2u);
	CHECK(!readable(tapfd));

	build(f, 6, MT_GET, true, false, 3000, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_UNKNOWN_ME);
	CHECK_EQ(r.detail, MT_GET);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);
	CHECK_EQ(r.frame[8], 4u);
	CHECK(!readable(tapfd));

	build(f, 7, 29u, true, false, 257, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_UNHANDLED);
	CHECK_EQ(r.detail, 29u);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);
	CHECK_EQ(r.frame[8], 2u);
	CHECK(!readable(tapfd));

	build(f, 0x0055, MT_GET, true, false, 257, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_DISPATCH);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);

	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_REPLAY);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);

	build(f, 0x0000, MT_GET, true, false, 257, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_DISPATCH);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);

	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_DISPATCH);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);
	CHECK_EQ(r.frame[0], 0u);
	CHECK_EQ(r.frame[1], 0u);

	for (unsigned int k = 0; k < 16u; k++) {
		build(f, 0x0000, MT_GET, true, false, 257, (uint16_t)k);
		CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
		CHECK(read_record(tapfd, &r));
		CHECK(read_record(tapfd, &r));
	}

	build(f, 0x0055, MT_GET, true, false, 257, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_RX_REPLAY);
	CHECK(read_record(tapfd, &r));
	CHECK_EQ(r.verdict, OMCI_CTL_TAP_TX);

	for (unsigned int i = 0; i < OMCI_CTL_TAP_RING * 16u; i++) {
		build(f, (uint16_t)(0x100u + i), MT_GET, true, false, 257, 0);
		CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	}

	unsigned long total_dropped = 0;
	unsigned int seen = 0;

	for (unsigned int round = 0; round < 4096u; round++) {
		bool progress = false;

		while (readable(tapfd) && read_record(tapfd, &r)) {
			total_dropped += r.dropped;
			seen++;
			progress = true;
		}
		flush_ring(ctl);
		if (!progress && !readable(tapfd))
			break;
	}
	CHECK(seen > 0);
	CHECK(total_dropped > 0);

	CHECK_EQ(seen + total_dropped,
		 (unsigned long)(OMCI_CTL_TAP_RING * 16u) * 2ul);

	static const uint8_t seq_mt[] = { MT_MIB_RESET, MT_GET, MT_GET };
	static const uint16_t seq_cls[] = { 0, 257, 263 };

	uint8_t with_tap[MAX_TX][OMCI_FRAME_LEN];
	unsigned int with_tap_n;

	struct omci_mib *m2 = NULL;

	omci_engine_fini(eng);
	omci_mib_fini(mib);
	CHECK_EQ(omci_mib_init(&m2), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, m2, &tx_ops, NULL), OMCI_OK);
	omci_engine_tap_set(eng, &tap_ops, ctl);
	g_tx_n = 0;
	for (unsigned int i = 0; i < sizeof(seq_mt); i++) {
		build(f, (uint16_t)(0x200u + i), seq_mt[i], true, false,
		      seq_cls[i], 0);
		CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	}
	with_tap_n = g_tx_n < MAX_TX ? g_tx_n : MAX_TX;
	memcpy(with_tap, g_tx, (size_t)with_tap_n * OMCI_FRAME_LEN);

	struct omci_mib *m3 = NULL;

	omci_engine_fini(eng);
	omci_mib_fini(m2);
	CHECK_EQ(omci_mib_init(&m3), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, m3, &tx_ops, NULL), OMCI_OK);
	g_tx_n = 0;
	for (unsigned int i = 0; i < sizeof(seq_mt); i++) {
		build(f, (uint16_t)(0x200u + i), seq_mt[i], true, false,
		      seq_cls[i], 0);
		CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);
	}
	CHECK_EQ(g_tx_n, with_tap_n);
	CHECK(with_tap_n > 1);
	CHECK_EQ(memcmp(with_tap, g_tx, (size_t)with_tap_n * OMCI_FRAME_LEN),
		 0);

	(void)close(tapfd);
	pump(ctl);
	build(f, 0x0300, MT_GET, true, false, 257, 0);
	CHECK_EQ(omci_engine_rx(eng, f), OMCI_OK);

	omci_ctl_close(ctl);
	omci_engine_fini(eng);
	(void)unlink(g_sock);

	return OMCI_T_REPORT("test_tap");
}
