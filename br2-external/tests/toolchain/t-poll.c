/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* poll and select on a pipe: empty, readable, and with the writer gone. */
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>
#include "check.h"

int main(void)
{
	int fd[2];
	struct pollfd p[2];
	struct timeval tv;
	fd_set rd;
	char c;

	CHECK(pipe(fd) == 0);

	p[0].fd = fd[0];
	p[0].events = POLLIN;
	p[1].fd = fd[1];
	p[1].events = POLLOUT;
	CHECK(poll(p, 2, 20) == 1);			/* only the write end is ready */
	CHECK(p[0].revents == 0 && (p[1].revents & POLLOUT));

	CHECK(write(fd[1], "x", 1) == 1);
	p[0].revents = 0;
	CHECK(poll(p, 1, 1000) == 1 && (p[0].revents & POLLIN));

	FD_ZERO(&rd);
	FD_SET(fd[0], &rd);
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	CHECK(select(fd[0] + 1, &rd, NULL, NULL, &tv) == 1 && FD_ISSET(fd[0], &rd));

	CHECK(read(fd[0], &c, 1) == 1 && c == 'x');
	FD_ZERO(&rd);
	FD_SET(fd[0], &rd);
	tv.tv_sec = 0;
	tv.tv_usec = 20000;
	CHECK(select(fd[0] + 1, &rd, NULL, NULL, &tv) == 0);

	close(fd[1]);					/* writer gone: a read ends with 0 */
	p[0].revents = 0;
	CHECK(poll(p, 1, 1000) == 1 && (p[0].revents & POLLHUP));
	CHECK(read(fd[0], &c, 1) == 0);
	close(fd[0]);

	CHECK(poll(NULL, 0, 10) == 0);			/* a plain timeout */
	DONE("poll");
}
