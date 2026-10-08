// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The control socket of ctl.h. Requests are read without blocking and
 * answered at once; a client that follows the frames reads them from a
 * ring, and one that falls behind loses the oldest and is told how many.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "me/table.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "sys/ctl.h"
#include "sys/log.h"
#include "sys/wire.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000
#endif

/* A client: the request being read, and where it is in the ring of frames. */
struct ctl_client {
	int fd;
	uint8_t req[OMCI_CTL_REQ_LEN];
	uint8_t got;

	bool tapping;
	unsigned long long tail;
	uint16_t dropped;
};

struct omci_ctl {
	int listen_fd;
	struct ctl_client client[OMCI_CTL_MAX_CLIENTS];

	const struct omci_mib *mib;
	const struct omci_ctl_counters *counters;

	uint8_t rsp[OMCI_CTL_RSP_LEN + OMCI_CTL_PAYLOAD_MAX];

	uint8_t ring[OMCI_CTL_TAP_RING][OMCI_CTL_TAP_REC_LEN];
	unsigned long long head;
};

static struct omci_ctl g_ctl = { .listen_fd = -1 };

enum omci_status omci_ctl_open(struct omci_ctl **out, const char *path,
			       const struct omci_mib *mib,
			       const struct omci_ctl_counters *counters)
{
	if (out == NULL || path == NULL)
		return OMCI_ERR_INVAL;
	*out = NULL;

	struct sockaddr_un addr;

	omci_wire_fill(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		OMCI_ERR("ctl: socket path is too long: %s", path);
		return OMCI_ERR_INVAL;
	}
	memcpy(addr.sun_path, path, strlen(path) + 1u);

	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS; i++) {
		g_ctl.client[i].fd = -1;
		g_ctl.client[i].tapping = false;
		g_ctl.client[i].dropped = 0;
	}
	g_ctl.head = 0;

	g_ctl.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_ctl.listen_fd < 0) {
		OMCI_ERR("ctl: cannot create the control socket (errno %d)",
			 errno);
		return OMCI_ERR_IO;
	}

	(void)unlink(path);

	const mode_t caller_umask = umask(0177);
	const int bound = bind(g_ctl.listen_fd, (const struct sockaddr *)&addr,
			       sizeof(addr));
	(void)umask(caller_umask);

	if (bound != 0 ||
	    listen(g_ctl.listen_fd, (int)OMCI_CTL_MAX_CLIENTS) != 0) {
		OMCI_ERR("ctl: cannot listen on %s (errno %d)", path, errno);
		(void)close(g_ctl.listen_fd);
		g_ctl.listen_fd = -1;
		return OMCI_ERR_IO;
	}

	g_ctl.mib = mib;
	g_ctl.counters = counters;
	*out = &g_ctl;

	OMCI_INFO("ctl: listening on %s", path);
	return OMCI_OK;
}

size_t omci_ctl_pollfds(struct omci_ctl *ctl, struct pollfd *out, size_t cap)
{
	if (ctl == NULL || out == NULL || ctl->listen_fd < 0 || cap == 0)
		return 0;

	size_t n = 0;

	out[n].fd = ctl->listen_fd;
	out[n].events = POLLIN;
	out[n].revents = 0;
	n++;

	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS && n < cap; i++) {
		const struct ctl_client *c = &ctl->client[i];

		if (c->fd < 0)
			continue;
		out[n].fd = c->fd;
		out[n].events = POLLIN;

		if (c->tapping && c->tail < ctl->head)
			out[n].events |= POLLOUT;

		out[n].revents = 0;
		n++;
	}
	return n;
}

/* Fill the response header. Return: the length of the whole response. */
static size_t rsp_frame(struct omci_ctl *ctl, uint16_t status, size_t paylen)
{
	omci_be32_store(&ctl->rsp[OMCI_CTL_RSP_MAGIC], OMCI_CTL_MAGIC);
	omci_be16_store(&ctl->rsp[OMCI_CTL_RSP_STATUS], status);
	omci_be16_store(&ctl->rsp[OMCI_CTL_RSP_PAYLEN], (unsigned int)paylen);
	return OMCI_CTL_RSP_LEN + paylen;
}

/* The values of every attribute of an instance, end to end, tables left out. */
static size_t op_me_get(struct omci_ctl *ctl, uint16_t class_id,
			uint16_t instance, uint16_t *status)
{
	if (!omci_mib_exists(ctl->mib, class_id, instance)) {
		*status = OMCI_CTL_ERR_NOT_FOUND;
		return 0;
	}

	uint8_t *pay = &ctl->rsp[OMCI_CTL_RSP_LEN];
	size_t off = 0;

	for (uint8_t attr = 1; attr <= 16u; attr++) {
		size_t n = 0;
		const enum omci_status rc = omci_mib_attr_get(
			ctl->mib, class_id, instance, attr, &pay[off],
			OMCI_CTL_PAYLOAD_MAX - off, &n);

		if (rc == OMCI_OK)
			off += n;
	}

	*status = OMCI_CTL_OK;
	return off;
}

/* Every attribute of an instance, a record each; a table with its rows. */
static size_t op_me_dump(struct omci_ctl *ctl, uint16_t class_id,
			 uint16_t instance, uint16_t *status)
{
	const struct omci_me_class *cls = omci_me_class_by_id(class_id);
	uint8_t *pay = &ctl->rsp[OMCI_CTL_RSP_LEN];
	size_t off = 0;

	*status = OMCI_CTL_ERR_NOT_FOUND;
	if (cls == NULL || !omci_mib_exists(ctl->mib, class_id, instance))
		return 0;

	for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);
		const bool table = omci_me_attr_is_table(a);
		const size_t rows =
			table ? omci_mib_table_count(ctl->mib, class_id,
						     instance, attr) :
				1u;
		const size_t len = rows * a->size;

		if (off + OMCI_CTL_DUMP_HDR_LEN + len > OMCI_CTL_PAYLOAD_MAX) {
			*status = OMCI_CTL_ERR_TOO_BIG;
			return 0;
		}
		pay[off] = (uint8_t)(table ? attr | OMCI_CTL_DUMP_TABLE : attr);
		omci_be16_store(&pay[off + 1u], (unsigned int)len);
		off += OMCI_CTL_DUMP_HDR_LEN;

		memset(&pay[off], 0, len);
		for (size_t r = 0; r < rows; r++, off += a->size) {
			const enum omci_status rc =
				table ? omci_mib_table_get(ctl->mib, class_id,
							   instance, attr, r,
							   &pay[off], a->size) :
					omci_mib_attr_get(ctl->mib, class_id,
							  instance, attr,
							  &pay[off], a->size,
							  NULL);

			/* A value the store does not hold stays zeros. */
			(void)rc;
		}
	}

	*status = OMCI_CTL_OK;
	return off;
}

/* The instances of a class. */
static size_t op_me_list(struct omci_ctl *ctl, uint16_t class_id,
			 uint16_t *status)
{
	uint8_t *pay = &ctl->rsp[OMCI_CTL_RSP_LEN];
	size_t off = 0;

	struct omci_mib_cursor c;

	for (enum omci_status rc = omci_mib_walk_first(ctl->mib, &c);
	     rc == OMCI_OK; rc = omci_mib_walk_next(ctl->mib, &c)) {
		if (c.class_id != class_id)
			continue;
		if (off + 2u > OMCI_CTL_PAYLOAD_MAX) {
			*status = OMCI_CTL_ERR_TOO_BIG;
			return 0;
		}
		omci_be16_store(&pay[off], c.instance);
		off += 2u;
	}

	*status = OMCI_CTL_OK;
	return off;
}

/*
 * Send a client the frames it has not had. Frames that left the ring
 * before it read them are counted in the next record it gets.
 *
 * Return: false when the client must be dropped.
 */
static bool tap_flush(struct omci_ctl *ctl, struct ctl_client *c)
{
	while (c->tail < ctl->head) {
		if (ctl->head - c->tail > OMCI_CTL_TAP_RING) {
			const unsigned long long lost =
				ctl->head - c->tail - OMCI_CTL_TAP_RING;
			c->dropped = (uint16_t)(lost > 0xFFFFu - c->dropped ?
							0xFFFFu :
							c->dropped + lost);
			c->tail = ctl->head - OMCI_CTL_TAP_RING;
		}

		uint8_t *rec = ctl->ring[c->tail % OMCI_CTL_TAP_RING];

		omci_be16_store(&rec[OMCI_CTL_TAP_DROPPED], c->dropped);

		const size_t len =
			rsp_frame(ctl, OMCI_CTL_OK, OMCI_CTL_TAP_REC_LEN);
		omci_wire_copy(&ctl->rsp[OMCI_CTL_RSP_LEN], rec,
			       OMCI_CTL_TAP_REC_LEN);

		const ssize_t put = send(c->fd, ctl->rsp, len, MSG_NOSIGNAL);

		if (put < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EINTR)
				return true;
			return false;
		}
		if ((size_t)put != len)
			return false;

		c->dropped = 0;
		c->tail++;
	}
	return true;
}

static void tap_flush_all(struct omci_ctl *ctl)
{
	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS; i++) {
		struct ctl_client *c = &ctl->client[i];

		if (c->fd < 0 || !c->tapping)
			continue;
		if (!tap_flush(ctl, c)) {
			(void)close(c->fd);
			c->fd = -1;
			c->got = 0;
			c->tapping = false;
		}
	}
}

void omci_ctl_tap_publish(struct omci_ctl *ctl, uint8_t verdict, uint8_t detail,
			  const uint8_t frame[static OMCI_FRAME_LEN])
{
	if (ctl == NULL)
		return;

	bool any = false;

	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS && !any; i++)
		any = ctl->client[i].fd >= 0 && ctl->client[i].tapping;
	if (!any)
		return;

	uint8_t *rec = ctl->ring[ctl->head % OMCI_CTL_TAP_RING];

	rec[OMCI_CTL_TAP_VERDICT] = verdict;
	rec[OMCI_CTL_TAP_DETAIL] = detail;
	omci_be16_store(&rec[OMCI_CTL_TAP_DROPPED], 0u);
	omci_wire_copy(&rec[OMCI_CTL_TAP_FRAME], frame, OMCI_FRAME_LEN);
	ctl->head++;

	tap_flush_all(ctl);
}

/* Start following the frames, from the next one. */
static size_t op_tap(struct omci_ctl *ctl, struct ctl_client *c,
		     uint16_t *status)
{
	c->tapping = true;

	c->tail = ctl->head;
	c->dropped = 0;
	*status = OMCI_CTL_OK;
	return 0;
}

static size_t op_stats(struct omci_ctl *ctl, uint16_t *status)
{
	uint8_t *pay = &ctl->rsp[OMCI_CTL_RSP_LEN];
	const struct omci_ctl_counters zero = { 0, 0, 0 };
	const struct omci_ctl_counters *c =
		ctl->counters != NULL ? ctl->counters : &zero;

	omci_be32_store(&pay[0], (unsigned int)c->handled);
	omci_be32_store(&pay[4], (unsigned int)c->rejected);
	omci_be32_store(&pay[8], (unsigned int)c->answered);

	*status = OMCI_CTL_OK;
	return OMCI_CTL_STATS_LEN;
}

/* One page of the fates of the VLAN rules, from record @first. */
static size_t op_vlan_fates(struct omci_ctl *ctl, uint16_t first,
			    uint16_t *status)
{
	uint8_t *pay = &ctl->rsp[OMCI_CTL_RSP_LEN];

	const struct omci_vlan_fate_rec *f = NULL;
	unsigned int over = 0;
	const size_t total = omci_vlan_fates(&f, &over);

	static_assert(OMCI_VLAN_FATE_MAX <= 0xFFFFu,
		      "the record total must fit the response header");
	static_assert(OMCI_MIB_TABLE_ENTRIES <= 0xFFFFu,
		      "the overflow count must fit the header");

	static_assert(OMCI_CTL_VLAN_HDR_LEN + OMCI_CTL_VLAN_PAGE_MAX *
						      OMCI_CTL_VLAN_REC_LEN <=
			      OMCI_CTL_PAYLOAD_MAX,
		      "one page must fit the response payload");

	omci_be16_store(&pay[OMCI_CTL_VLAN_TOTAL], (unsigned int)total);
	omci_be16_store(&pay[OMCI_CTL_VLAN_OVERFLOW], over);

	size_t off = OMCI_CTL_VLAN_HDR_LEN;

	for (size_t i = first;
	     i < total && (i - first) < OMCI_CTL_VLAN_PAGE_MAX; i++) {
		omci_be16_store(&pay[off + 0u], f[i].instance);
		pay[off + 2u] = f[i].row;
		pay[off + 3u] = f[i].fate;
		omci_be16_store(&pay[off + 4u], f[i].match_cvid);
		omci_be16_store(&pay[off + 6u], f[i].net_vid);
		omci_be16_store(&pay[off + 8u], f[i].rival_match);
		omci_be16_store(&pay[off + 10u], f[i].rival_net);
		off += OMCI_CTL_VLAN_REC_LEN;
	}

	*status = OMCI_CTL_OK;
	return off;
}

/* Answer a complete request. Return: false when the client must be dropped. */
static bool answer(struct omci_ctl *ctl, struct ctl_client *c)
{
	uint16_t status = OMCI_CTL_OK;
	size_t paylen = 0;

	if (omci_be32(&c->req[OMCI_CTL_REQ_MAGIC]) != OMCI_CTL_MAGIC) {
		status = OMCI_CTL_ERR_PROTO;
	} else {
		const uint16_t op =
			(uint16_t)omci_be16(&c->req[OMCI_CTL_REQ_OP]);
		const uint16_t cls =
			(uint16_t)omci_be16(&c->req[OMCI_CTL_REQ_CLASS]);
		const uint16_t inst =
			(uint16_t)omci_be16(&c->req[OMCI_CTL_REQ_INSTANCE]);

		switch (op) {
		case OMCI_CTL_OP_ME_GET:
			paylen = op_me_get(ctl, cls, inst, &status);
			break;
		case OMCI_CTL_OP_ME_LIST:
			paylen = op_me_list(ctl, cls, &status);
			break;
		case OMCI_CTL_OP_ME_DUMP:
			paylen = op_me_dump(ctl, cls, inst, &status);
			break;
		case OMCI_CTL_OP_STATS:
			paylen = op_stats(ctl, &status);
			break;
		case OMCI_CTL_OP_TAP:
			paylen = op_tap(ctl, c, &status);
			break;
		case OMCI_CTL_OP_VLAN_FATES:
			paylen = op_vlan_fates(
				ctl,
				(uint16_t)omci_be16(&c->req[OMCI_CTL_REQ_ARG]),
				&status);
			break;
		default:
			status = OMCI_CTL_ERR_OP;
			break;
		}
	}

	const size_t len = rsp_frame(ctl, status, paylen);

	const ssize_t put = send(c->fd, ctl->rsp, len, MSG_NOSIGNAL);

	if (put < 0 || (size_t)put != len) {
		OMCI_DEBUG(
			"ctl: client write did not complete (errno %d) — dropping it",
			errno);
		return false;
	}
	return true;
}

/* Read what a client sent; a request may come in pieces. */
static bool serve_client(struct omci_ctl *ctl, struct ctl_client *c)
{
	const ssize_t n =
		recv(c->fd, &c->req[c->got], OMCI_CTL_REQ_LEN - c->got, 0);
	if (n == 0)
		return false;
	if (n < 0)
		return errno == EINTR || errno == EAGAIN ||
		       errno == EWOULDBLOCK;

	c->got += (uint8_t)n;
	if (c->got < OMCI_CTL_REQ_LEN)
		return true;

	c->got = 0;
	return answer(ctl, c);
}

static void drop_client(struct omci_ctl *ctl, int fd)
{
	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS; i++) {
		if (ctl->client[i].fd != fd)
			continue;
		(void)close(ctl->client[i].fd);
		ctl->client[i].fd = -1;
		ctl->client[i].got = 0;
		ctl->client[i].tapping = false;
		return;
	}
}

static void accept_client(struct omci_ctl *ctl)
{
	const int fd = accept(ctl->listen_fd, NULL, NULL);

	if (fd < 0)
		return;

	const int fl = fcntl(fd, F_GETFL, 0);

	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
		OMCI_DEBUG(
			"ctl: cannot make a client non-blocking (errno %d) — refusing it",
			errno);
		(void)close(fd);
		return;
	}

	const int sndbuf = 16 * 1024;
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS; i++) {
		if (ctl->client[i].fd < 0) {
			ctl->client[i].fd = fd;
			ctl->client[i].got = 0;
			ctl->client[i].tapping = false;
			ctl->client[i].dropped = 0;
			return;
		}
	}

	OMCI_DEBUG("ctl: refusing a client — all %u slots are in use",
		   OMCI_CTL_MAX_CLIENTS);
	(void)close(fd);
}

static struct ctl_client *client_by_fd(struct omci_ctl *ctl, int fd)
{
	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS; i++)
		if (ctl->client[i].fd == fd)
			return &ctl->client[i];
	return NULL;
}

void omci_ctl_service(struct omci_ctl *ctl, const struct pollfd *fds, size_t n)
{
	if (ctl == NULL || fds == NULL)
		return;

	for (size_t i = 0; i < n; i++) {
		if (fds[i].revents == 0)
			continue;

		if (fds[i].fd == ctl->listen_fd) {
			accept_client(ctl);
			continue;
		}

		struct ctl_client *c = client_by_fd(ctl, fds[i].fd);

		if (c == NULL)
			continue;

		if ((fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 ||
		    !serve_client(ctl, c))
			drop_client(ctl, fds[i].fd);
	}

	tap_flush_all(ctl);
}

void omci_ctl_close(struct omci_ctl *ctl)
{
	if (ctl == NULL)
		return;

	for (unsigned int i = 0; i < OMCI_CTL_MAX_CLIENTS; i++) {
		if (ctl->client[i].fd >= 0)
			(void)close(ctl->client[i].fd);
		ctl->client[i].fd = -1;
		ctl->client[i].got = 0;
		ctl->client[i].tapping = false;
	}
	if (ctl->listen_fd >= 0)
		(void)close(ctl->listen_fd);
	ctl->listen_fd = -1;
}
