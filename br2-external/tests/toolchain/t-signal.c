/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Signals: delivery to a handler, blocking and pending, and a jump out of a
 * handler.
 */
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "check.h"

static volatile sig_atomic_t usr1, usr2, jumped;
static sigjmp_buf env;

static void on_usr(int sig)
{
	if (sig == SIGUSR1)
		usr1++;
	else
		usr2++;
}

static void on_alrm(int sig)
{
	(void)sig;
	jumped++;
	siglongjmp(env, 1);
}

int main(void)
{
	struct sigaction sa;
	sigset_t set, pending;
	int i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr;
	sigemptyset(&sa.sa_mask);
	CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
	CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);

	CHECK(raise(SIGUSR1) == 0 && usr1 == 1);
	CHECK(kill(getpid(), SIGUSR2) == 0 && usr2 == 1);

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
	CHECK(raise(SIGUSR1) == 0 && usr1 == 1);	/* held back while blocked */
	CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) == 1);
	CHECK(sigprocmask(SIG_UNBLOCK, &set, NULL) == 0);
	CHECK(usr1 == 2);				/* delivered on unblocking */

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alrm;
	sigemptyset(&sa.sa_mask);
	CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
	for (i = 0; i < 3; i++) {			/* jump out of a handler, three times */
		if (sigsetjmp(env, 1) == 0) {
			raise(SIGALRM);
			CHECK(0);			/* not reached */
		}
	}
	CHECK(jumped == 3);
	CHECK(sigprocmask(SIG_BLOCK, NULL, &pending) == 0 && !sigismember(&pending, SIGALRM));
	DONE("signal");
}
