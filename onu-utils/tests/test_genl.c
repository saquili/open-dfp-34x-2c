// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the generic netlink requests, against the kernel the tests run on:
 * its controller family, "nlctrl", is in every kernel. An answer read past its
 * end never returns, so an alarm turns that into a failure.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/genetlink.h>

#include "genl.h"

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static void on_alarm(int sig)
{
	static const char msg[] =
		"  FAIL the answer was read past its end and never returned\n";

	(void)sig;
	if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0)
		_exit(2);
	_exit(1);
}

int main(void)
{
	struct mnl_socket *nl = mnl_socket_open(NETLINK_GENERIC);
	uint16_t id = 0;
	uint32_t group = 0;

	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		printf("test_genl: no generic netlink socket: %s\n",
		       strerror(errno));
		return 1;
	}
	signal(SIGALRM, on_alarm);
	alarm(5);

	CHECK(genl_resolve(nl, "nlctrl", "notify", &id, &group) == 0);
	CHECK(id == GENL_ID_CTRL);
	CHECK(group != 0);

	group = 7;
	CHECK(genl_resolve(nl, "nlctrl", "no-such-group", &id, &group) == 0);
	CHECK(group == 0);

	CHECK(genl_resolve(nl, "no-such-family", NULL, &id, NULL) == -ENOENT);

	alarm(0);
	mnl_socket_close(nl);
	printf("test_genl: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
