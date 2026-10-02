/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Fault injection for the tests: make the next system call or allocation fail. */

#ifndef OMCI_TEST_FAULT_H
#define OMCI_TEST_FAULT_H

#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

enum fault_call {
	FAULT_SOCKET,
	FAULT_BIND,
	FAULT_LISTEN,
	FAULT_ACCEPT,
	FAULT_CONNECT,
	FAULT_POLL,
	FAULT_READ,
	FAULT_WRITE,
	FAULT_SEND,
	FAULT_RECV,
	FAULT_FCNTL,
	FAULT_CLOCK_GETTIME,
	FAULT_SETSOCKOPT,
	FAULT_COUNT
};

void fault_reset(void);

void fault_fail(enum fault_call call, unsigned int nth, int err);

void fault_fail_from(enum fault_call call, unsigned int nth, int err);

void fault_return(enum fault_call call, unsigned int nth, long value, int err);

void fault_short(enum fault_call call, unsigned int nth, size_t limit);

void fault_signal(enum fault_call call, unsigned int nth, int sig);

void fault_hook(enum fault_call call, void (*fn)(void *), void *arg);

unsigned int fault_fired(enum fault_call call);

unsigned int fault_seen(enum fault_call call);

const char *fault_connect_path(void);

struct fault_sockopt {
	int level;
	int optname;
	long a;
	long b;
	socklen_t len;
};

unsigned int fault_sockopts(struct fault_sockopt *out, unsigned int cap);

int __real_socket(int domain, int type, int protocol);
int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);
int __real_listen(int fd, int backlog);
int __real_accept(int fd, struct sockaddr *restrict addr,
		  socklen_t *restrict len);
int __real_connect(int fd, const struct sockaddr *addr, socklen_t len);
int __real_poll(struct pollfd *fds, nfds_t nfds, int timeout);
ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __real_send(int fd, const void *buf, size_t n, int flags);
ssize_t __real_recv(int fd, void *buf, size_t n, int flags);
int __real_fcntl(int fd, int cmd, ...);
int __real_clock_gettime(clockid_t clk, struct timespec *ts);
int __real_setsockopt(int fd, int level, int optname, const void *optval,
		      socklen_t len);

int __wrap_socket(int domain, int type, int protocol);
int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len);
int __wrap_listen(int fd, int backlog);
int __wrap_accept(int fd, struct sockaddr *restrict addr,
		  socklen_t *restrict len);
int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len);
int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout);
ssize_t __wrap_read(int fd, void *buf, size_t n);
ssize_t __wrap_write(int fd, const void *buf, size_t n);
ssize_t __wrap_send(int fd, const void *buf, size_t n, int flags);
ssize_t __wrap_recv(int fd, void *buf, size_t n, int flags);
int __wrap_fcntl(int fd, int cmd, ...);
int __wrap_clock_gettime(clockid_t clk, struct timespec *ts);
int __wrap_setsockopt(int fd, int level, int optname, const void *optval,
		      socklen_t len);

#endif
