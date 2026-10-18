// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-fw: the stick's own table of nf_tables, "inet onu" (fwmsg.h), which
 * onu-apply puts in place for the local networks.
 *
 *   onu-fw apply FILE   put the table that FILE describes in place of the one
 *                       there is
 *   onu-fw clear        take the table away
 *
 * The kernel takes a batch whole or not at all, so a table that fails leaves
 * the one there was. When the kernel refuses the flowtable, as one whose
 * devices cannot carry flows does, the table is put in place without it, and
 * the CPU carries the flows. Exit status 0, 1 when the kernel refused the
 * table, 2 for a file or a command that is none.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink.h>

#include "fwmsg.h"

/* A batch and room for the last request of it (mnl_nlmsg_batch_start()). */
static char g_buf[2 * FW_BATCH_MAX];

static void usage(void)
{
	fputs("usage: onu-fw apply FILE | clear\n", stderr);
}

/*
 * Send the batch of @len bytes, whose last acknowledged request is @last,
 * and read the acknowledgements: 0, or a negative errno, the first the
 * kernel gave.
 */
static int run(size_t len, uint32_t last)
{
	struct timeval tv = { .tv_sec = 2 };
	struct mnl_socket *nl = mnl_socket_open(NETLINK_NETFILTER);
	char ack[MNL_SOCKET_BUFFER_SIZE];
	int sndbuf = (int)len, rc = -ETIMEDOUT;

	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		rc = -errno;
		goto out;
	}
	setsockopt(mnl_socket_get_fd(nl), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	/*
	 * The kernel takes a message no longer than the socket's send buffer,
	 * 208 KiB by default (net.core.wmem_default): a batch with every
	 * forward comes near it, at 176 KiB.
	 */
	setsockopt(mnl_socket_get_fd(nl), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	if (mnl_socket_sendto(nl, g_buf, len) < 0) {
		rc = -errno;
		goto out;
	}
	for (;;) {
		ssize_t n = mnl_socket_recvfrom(nl, ack, sizeof(ack));
		struct nlmsghdr *nlh = (struct nlmsghdr *)ack;
		int left = (int)n;

		if (n < 0) {
			rc = errno == EAGAIN ? -ETIMEDOUT : -errno;
			goto out;
		}
		for (; mnl_nlmsg_ok(nlh, left); nlh = mnl_nlmsg_next(nlh, &left)) {
			const struct nlmsgerr *e = mnl_nlmsg_get_payload(nlh);

			if (nlh->nlmsg_type != NLMSG_ERROR)
				continue;
			if (e->error) {
				rc = e->error;
				goto out;
			}
			if (nlh->nlmsg_seq == last) {
				rc = 0;
				goto out;
			}
		}
	}
out:
	if (nl)
		mnl_socket_close(nl);
	return rc;
}

static int apply(const struct fw_spec *s)
{
	uint32_t seq = (uint32_t)time(NULL), last;
	size_t len = fw_batch(s, g_buf, sizeof(g_buf), seq, &last);

	if (!len) {
		fputs("onu-fw: the table does not fit a batch\n", stderr);
		return -E2BIG;
	}
	return run(len, last);
}

static int apply_file(const char *path)
{
	char line[256], err[128];
	struct fw_spec s;
	FILE *f = fopen(path, "r");
	int rc;

	if (!f) {
		fprintf(stderr, "onu-fw: %s: %s\n", path, strerror(errno));
		return 2;
	}
	fw_spec_init(&s);
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (fw_spec_line(&s, line, err, sizeof(err))) {
			fprintf(stderr, "onu-fw: %s: %s\n", path, err);
			fclose(f);
			return 2;
		}
	}
	fclose(f);

	rc = apply(&s);
	if (rc && s.offload) {
		fprintf(stderr,
			"onu-fw: the flowtable was refused (%s): the CPU carries the flows\n",
			strerror(-rc));
		s.offload = false;
		rc = apply(&s);
	}
	if (rc) {
		fprintf(stderr, "onu-fw: the kernel refused the table: %s\n", strerror(-rc));
		return 1;
	}
	return 0;
}

static int clear(void)
{
	uint32_t last;
	size_t len = fw_batch_clear(g_buf, sizeof(g_buf), (uint32_t)time(NULL), &last);
	int rc = run(len, last);

	if (rc) {
		fprintf(stderr, "onu-fw: the table was not taken away: %s\n", strerror(-rc));
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "apply"))
		return apply_file(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "clear"))
		return clear();
	usage();
	return 2;
}
