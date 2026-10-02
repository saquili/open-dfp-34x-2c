// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The client library of omcid/libomcid.h. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define OMCID_CONN_TIMEOUT_S 5

#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "omcid/libomcid.h"

struct omcid_conn {
	int fd;
	uint8_t payload[OMCI_CTL_PAYLOAD_MAX];
};

static struct omcid_conn g_conn = { .fd = -1 };

struct omcid_conn *omcid_connect(const char *sock)
{
	if (g_conn.fd >= 0) {
		errno = EBUSY;
		return NULL;
	}

	const char *path = sock != NULL ? sock : OMCI_CTL_SOCKET_PATH;

	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	memcpy(addr.sun_path, path, strlen(path) + 1u);

	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return NULL;

	if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
		const int saved = errno;
		(void)close(fd);
		errno = saved;
		return NULL;
	}

	const struct timeval tv = { .tv_sec = OMCID_CONN_TIMEOUT_S,
				    .tv_usec = 0 };
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	g_conn.fd = fd;
	return &g_conn;
}

void omcid_disconnect(struct omcid_conn *conn)
{
	if (conn == NULL || conn->fd < 0)
		return;
	(void)close(conn->fd);
	conn->fd = -1;
}

/* Read @len bytes; the daemon closing the socket is ECONNRESET. */
static bool read_exactly(int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		const ssize_t n = recv(fd, &buf[got], len - got, 0);

		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n == 0) {
			errno = ECONNRESET;
			return false;
		}
		if (errno == EINTR)
			continue;
		return false;
	}
	return true;
}

/*
 * Send a request and read the response; the payload lands in
 * @conn->payload and its length in @paylen.
 *
 * Return: 0, -1 with errno set, or the status the daemon answered with.
 */
static int transact(struct omcid_conn *conn, uint16_t op, uint16_t class_id,
		    uint16_t instance, uint16_t arg, size_t *paylen)
{
	if (conn == NULL || conn->fd < 0) {
		errno = EBADF;
		return -1;
	}

	uint8_t req[OMCI_CTL_REQ_LEN];

	memset(req, 0, sizeof(req));
	omci_be32_store(&req[OMCI_CTL_REQ_MAGIC], OMCI_CTL_MAGIC);
	omci_be16_store(&req[OMCI_CTL_REQ_OP], op);
	omci_be16_store(&req[OMCI_CTL_REQ_CLASS], class_id);
	omci_be16_store(&req[OMCI_CTL_REQ_INSTANCE], instance);
	omci_be16_store(&req[OMCI_CTL_REQ_ARG], arg);

	size_t put = 0;

	while (put < sizeof(req)) {
		const ssize_t n =
			send(conn->fd, &req[put], sizeof(req) - put, 0);
		if (n > 0) {
			put += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}

	uint8_t hdr[OMCI_CTL_RSP_LEN];

	if (!read_exactly(conn->fd, hdr, sizeof(hdr)))
		return -1;

	if (omci_be32(&hdr[OMCI_CTL_RSP_MAGIC]) != OMCI_CTL_MAGIC) {
		errno = EPROTO;
		return -1;
	}

	const uint16_t status = (uint16_t)omci_be16(&hdr[OMCI_CTL_RSP_STATUS]);
	const size_t len = (size_t)omci_be16(&hdr[OMCI_CTL_RSP_PAYLEN]);

	if (len > OMCI_CTL_PAYLOAD_MAX) {
		errno = EPROTO;
		return -1;
	}

	if (len > 0 && !read_exactly(conn->fd, conn->payload, len))
		return -1;

	*paylen = len;
	return status == OMCI_CTL_OK ? 0 : (int)status;
}

int omcid_me_get(struct omcid_conn *conn, uint16_t class_id, uint16_t instance,
		 uint8_t *out, size_t cap, size_t *n)
{
	size_t len = 0;
	const int rc =
		transact(conn, OMCI_CTL_OP_ME_GET, class_id, instance, 0, &len);
	if (rc != 0)
		return rc;

	if (len > cap)
		len = cap;
	if (out != NULL && len > 0)
		memcpy(out, conn->payload, len);
	if (n != NULL)
		*n = len;
	return 0;
}

int omcid_me_list(struct omcid_conn *conn, uint16_t class_id, uint16_t *out,
		  size_t cap, size_t *n)
{
	size_t len = 0;
	const int rc =
		transact(conn, OMCI_CTL_OP_ME_LIST, class_id, 0, 0, &len);
	if (rc != 0)
		return rc;

	size_t count = len / 2u;

	if (count > cap)
		count = cap;
	if (out != NULL)
		for (size_t i = 0; i < count; i++)
			out[i] = (uint16_t)omci_be16(&conn->payload[i * 2u]);
	if (n != NULL)
		*n = count;
	return 0;
}

int omcid_stats(struct omcid_conn *conn, struct omcid_conn_stats *out)
{
	size_t len = 0;
	const int rc = transact(conn, OMCI_CTL_OP_STATS, 0, 0, 0, &len);

	if (rc != 0)
		return rc;
	if (len < OMCI_CTL_STATS_LEN) {
		errno = EPROTO;
		return -1;
	}

	if (out != NULL) {
		out->handled = omci_be32(&conn->payload[0]);
		out->rejected = omci_be32(&conn->payload[4]);
		out->answered = omci_be32(&conn->payload[8]);
	}
	return 0;
}

int omcid_tap_subscribe(struct omcid_conn *conn)
{
	size_t len = 0;

	return transact(conn, OMCI_CTL_OP_TAP, 0, 0, 0, &len);
}

int omcid_tap_next(struct omcid_conn *conn, struct omcid_tap_record *out)
{
	if (conn == NULL || conn->fd < 0) {
		errno = EBADF;
		return -1;
	}

	uint8_t hdr[OMCI_CTL_RSP_LEN];

	if (!read_exactly(conn->fd, hdr, sizeof(hdr))) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}

	if (omci_be32(&hdr[OMCI_CTL_RSP_MAGIC]) != OMCI_CTL_MAGIC ||
	    omci_be16(&hdr[OMCI_CTL_RSP_PAYLEN]) != OMCI_CTL_TAP_REC_LEN) {
		errno = EPROTO;
		return -1;
	}

	uint8_t rec[OMCI_CTL_TAP_REC_LEN];

	if (!read_exactly(conn->fd, rec, sizeof(rec)))
		return -1;

	if (out != NULL) {
		out->verdict = rec[OMCI_CTL_TAP_VERDICT];
		out->detail = rec[OMCI_CTL_TAP_DETAIL];
		out->dropped = (uint16_t)omci_be16(&rec[OMCI_CTL_TAP_DROPPED]);
		memcpy(out->frame, &rec[OMCI_CTL_TAP_FRAME],
		       sizeof(out->frame));
	}
	return 1;
}

int omcid_vlan_fate_list(struct omcid_conn *conn,
			 struct omcid_vlan_fate_entry *out, size_t cap,
			 size_t *n, size_t *total, unsigned int *overflow)
{
	if (n != NULL)
		*n = 0;

	size_t got = 0, want = 0;

	/* Read page after page until one comes back empty or @out is full. */
	for (;;) {
		size_t len = 0;
		const int rc = transact(conn, OMCI_CTL_OP_VLAN_FATES, 0, 0,
					(uint16_t)got, &len);
		if (rc != 0)
			return rc;
		if (len < OMCI_CTL_VLAN_HDR_LEN) {
			errno = EPROTO;
			return -1;
		}

		want = omci_be16(&conn->payload[OMCI_CTL_VLAN_TOTAL]);
		if (overflow != NULL)
			*overflow = (unsigned int)omci_be16(
				&conn->payload[OMCI_CTL_VLAN_OVERFLOW]);
		if (total != NULL)
			*total = want;

		const size_t here =
			(len - OMCI_CTL_VLAN_HDR_LEN) / OMCI_CTL_VLAN_REC_LEN;
		for (size_t i = 0; i < here; i++) {
			if (out != NULL && got < cap) {
				const uint8_t *r =
					&conn->payload[OMCI_CTL_VLAN_HDR_LEN +
						       i * OMCI_CTL_VLAN_REC_LEN];
				out[got].instance = (uint16_t)omci_be16(&r[0]);
				out[got].row = r[2];
				out[got].fate = r[3];
				out[got].match_cvid =
					(uint16_t)omci_be16(&r[4]);
				out[got].net_vid = (uint16_t)omci_be16(&r[6]);
				out[got].rival_match =
					(uint16_t)omci_be16(&r[8]);
				out[got].rival_net =
					(uint16_t)omci_be16(&r[10]);
			}
			got++;
		}

		if (here == 0u)
			break;
		if (out != NULL && got >= cap)
			break;
	}

	if (n != NULL)
		*n = (out == NULL) ? 0u : (got < cap ? got : cap);
	return 0;
}
