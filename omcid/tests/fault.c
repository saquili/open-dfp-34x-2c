// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Fault injection for the tests: make the next system call or allocation fail. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "fault.h"

enum act { ACT_NONE, ACT_FAIL, ACT_RETURN, ACT_SHORT, ACT_SIGNAL };

#define PLANS_PER_CALL 4u

struct plan {
	enum act act;
	unsigned int nth;
	bool sticky;
	int err;
	long value;
	unsigned int base;
	unsigned int fired;
};

struct hook {
	void (*fn)(void *data);
	void *arg;
};

static struct plan g_plan[FAULT_COUNT][PLANS_PER_CALL];
static unsigned int g_seen[FAULT_COUNT];
static struct hook g_hook[FAULT_COUNT];
static bool g_in_hook;

#define SOCKOPTS_KEPT 8u
static char g_path[sizeof((struct sockaddr_un *)0)->sun_path + 1u];
static struct fault_sockopt g_sockopt[SOCKOPTS_KEPT];
static unsigned int g_sockopt_n;

void fault_reset(void)
{
	memset(g_plan, 0, sizeof(g_plan));
	memset(g_seen, 0, sizeof(g_seen));
	memset(g_hook, 0, sizeof(g_hook));
	memset(g_path, 0, sizeof(g_path));
	memset(g_sockopt, 0, sizeof(g_sockopt));
	g_sockopt_n = 0;
	g_in_hook = false;
}

static void arm(enum fault_call call, enum act act, unsigned int nth,
		bool sticky, int err, long value)
{
	for (unsigned int i = 0; i < PLANS_PER_CALL; i++) {
		struct plan *p = &g_plan[call][i];

		if (p->act != ACT_NONE)
			continue;
		p->act = act;
		p->nth = nth;
		p->sticky = sticky;
		p->err = err;
		p->value = value;
		p->base = g_seen[call];
		p->fired = 0;
		return;
	}
	abort();
}

void fault_fail(enum fault_call call, unsigned int nth, int err)
{
	arm(call, ACT_FAIL, nth, false, err, 0);
}

void fault_fail_from(enum fault_call call, unsigned int nth, int err)
{
	arm(call, ACT_FAIL, nth, true, err, 0);
}

void fault_return(enum fault_call call, unsigned int nth, long value, int err)
{
	arm(call, ACT_RETURN, nth, false, err, value);
}

void fault_short(enum fault_call call, unsigned int nth, size_t limit)
{
	arm(call, ACT_SHORT, nth, false, 0, (long)limit);
}

void fault_signal(enum fault_call call, unsigned int nth, int sig)
{
	arm(call, ACT_SIGNAL, nth, false, EINTR, sig);
}

void fault_hook(enum fault_call call, void (*fn)(void *), void *arg)
{
	g_hook[call].fn = fn;
	g_hook[call].arg = arg;
}

unsigned int fault_fired(enum fault_call call)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < PLANS_PER_CALL; i++)
		n += g_plan[call][i].fired;
	return n;
}

unsigned int fault_seen(enum fault_call call)
{
	return g_seen[call];
}

const char *fault_connect_path(void)
{
	return g_path;
}

unsigned int fault_sockopts(struct fault_sockopt *out, unsigned int cap)
{
	for (unsigned int i = 0;
	     i < g_sockopt_n && i < SOCKOPTS_KEPT && i < cap; i++)
		out[i] = g_sockopt[i];
	return g_sockopt_n;
}

static void note_path(const struct sockaddr *addr)
{
	if (g_in_hook || addr == NULL || addr->sa_family != AF_UNIX)
		return;
	const struct sockaddr_un *un = (const struct sockaddr_un *)addr;

	memcpy(g_path, un->sun_path, sizeof(un->sun_path));
	g_path[sizeof(un->sun_path)] = '\0';
}

static struct plan *due(enum fault_call call)
{
	if (g_in_hook)
		return NULL;

	if (g_hook[call].fn != NULL) {
		g_in_hook = true;
		g_hook[call].fn(g_hook[call].arg);
		g_in_hook = false;
	}

	g_seen[call]++;
	for (unsigned int i = 0; i < PLANS_PER_CALL; i++) {
		struct plan *p = &g_plan[call][i];

		if (p->act == ACT_NONE)
			continue;
		const unsigned int k = g_seen[call] - p->base;

		if (k < p->nth || (k > p->nth && !p->sticky))
			continue;
		p->fired++;
		return p;
	}
	return NULL;
}

static long altered(struct plan *p)
{
	if (p->act == ACT_SIGNAL)
		(void)raise((int)p->value);
	if (p->act == ACT_RETURN && p->value >= 0)
		return p->value;
	errno = p->err;
	return -1;
}

int __wrap_socket(int domain, int type, int protocol)
{
	struct plan *p = due(FAULT_SOCKET);

	if (p != NULL)
		return (int)altered(p);
	return __real_socket(domain, type, protocol);
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	note_path(addr);
	struct plan *p = due(FAULT_BIND);

	if (p != NULL)
		return (int)altered(p);
	return __real_bind(fd, addr, len);
}

int __wrap_listen(int fd, int backlog)
{
	struct plan *p = due(FAULT_LISTEN);

	if (p != NULL)
		return (int)altered(p);
	return __real_listen(fd, backlog);
}

int __wrap_accept(int fd, struct sockaddr *restrict addr,
		  socklen_t *restrict len)
{
	struct plan *p = due(FAULT_ACCEPT);

	if (p != NULL)
		return (int)altered(p);
	return __real_accept(fd, addr, len);
}

int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	note_path(addr);
	struct plan *p = due(FAULT_CONNECT);

	if (p != NULL)
		return (int)altered(p);
	return __real_connect(fd, addr, len);
}

int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	struct plan *p = due(FAULT_POLL);

	if (p != NULL)
		return (int)altered(p);
	return __real_poll(fds, nfds, timeout);
}

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
	struct plan *p = due(FAULT_READ);

	if (p != NULL) {
		if (p->act != ACT_SHORT)
			return (ssize_t)altered(p);
		if ((size_t)p->value < n)
			n = (size_t)p->value;
	}
	return __real_read(fd, buf, n);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
	struct plan *p = due(FAULT_WRITE);

	if (p != NULL) {
		if (p->act != ACT_SHORT)
			return (ssize_t)altered(p);
		if ((size_t)p->value < n)
			n = (size_t)p->value;
	}
	return __real_write(fd, buf, n);
}

ssize_t __wrap_send(int fd, const void *buf, size_t n, int flags)
{
	struct plan *p = due(FAULT_SEND);

	if (p != NULL) {
		if (p->act != ACT_SHORT)
			return (ssize_t)altered(p);
		if ((size_t)p->value < n)
			n = (size_t)p->value;
	}
	return __real_send(fd, buf, n, flags);
}

ssize_t __wrap_recv(int fd, void *buf, size_t n, int flags)
{
	struct plan *p = due(FAULT_RECV);

	if (p != NULL) {
		if (p->act != ACT_SHORT)
			return (ssize_t)altered(p);
		if ((size_t)p->value < n)
			n = (size_t)p->value;
	}
	return __real_recv(fd, buf, n, flags);
}

int __wrap_fcntl(int fd, int cmd, ...)
{
	struct plan *p = due(FAULT_FCNTL);

	if (p != NULL)
		return (int)altered(p);
	if (cmd != F_SETFL)
		return __real_fcntl(fd, cmd);

	va_list ap;

	va_start(ap, cmd);
	const int arg = va_arg(ap, int);

	va_end(ap);
	return __real_fcntl(fd, cmd, arg);
}

int __wrap_clock_gettime(clockid_t clk, struct timespec *ts)
{
	struct plan *p = due(FAULT_CLOCK_GETTIME);

	if (p != NULL)
		return (int)altered(p);
	return __real_clock_gettime(clk, ts);
}

int __wrap_setsockopt(int fd, int level, int optname, const void *optval,
		      socklen_t len)
{
	if (!g_in_hook) {
		if (g_sockopt_n < SOCKOPTS_KEPT) {
			struct fault_sockopt *o = &g_sockopt[g_sockopt_n];

			o->level = level;
			o->optname = optname;
			o->len = len;
			o->a = 0;
			o->b = 0;
			if (len == sizeof(int)) {
				o->a = *(const int *)optval;
			} else if (len == sizeof(struct timeval)) {
				o->a = (long)((const struct timeval *)optval)
					       ->tv_sec;
				o->b = (long)((const struct timeval *)optval)
					       ->tv_usec;
			}
		}
		g_sockopt_n++;
	}
	struct plan *p = due(FAULT_SETSOCKOPT);

	if (p != NULL)
		return (int)altered(p);
	return __real_setsockopt(fd, level, optname, optval, len);
}
