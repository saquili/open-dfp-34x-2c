// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that only the owner can reach the control socket. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "mib/store.h"
#include "omcid/array_size.h"
#include "sys/ctl.h"

#include "harness.h"

static char g_path[64];

static mode_t mode_of(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
		return (mode_t)0xFFFFu;
	return st.st_mode & 07777u;
}

static mode_t open_under(mode_t caller_umask, bool *umask_kept)
{
	struct omci_mib *mib = NULL;
	struct omci_ctl *ctl = NULL;
	static struct omci_ctl_counters counters;

	(void)umask(caller_umask);
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_ctl_open(&ctl, g_path, mib, &counters), OMCI_OK);

	const mode_t after = umask(caller_umask);
	*umask_kept = after == caller_umask;
	const mode_t mode = mode_of(g_path);

	omci_ctl_close(ctl);
	omci_mib_fini(mib);
	(void)unlink(g_path);
	return mode;
}

static void test_the_socket_has_mode_0600_under_any_umask(void)
{
	static const mode_t umasks[] = { 0000, 0002, 0022, 0077, 0177, 0277 };

	for (size_t i = 0; i < ARRAY_SIZE(umasks); i++) {
		bool kept = false;
		const mode_t mode = open_under(umasks[i], &kept);

		if (mode != 0600u)
			printf("    umask %04o: mode %04o\n",
			       (unsigned int)umasks[i], (unsigned int)mode);
		CHECK_EQ(mode, 0600u);
		CHECK(kept);
	}
}

static void
test_a_stale_file_at_the_path_is_replaced_by_a_socket_with_the_same_mode(void)
{
	const int fd = open(g_path, O_CREAT | O_WRONLY, 0666);

	CHECK(fd >= 0);
	if (fd >= 0)
		(void)close(fd);

	bool kept = false;

	CHECK_EQ(open_under(0000, &kept), 0600u);
	CHECK(kept);
}

static void test_the_socket_still_takes_connections(void)
{
	struct omci_mib *mib = NULL;
	struct omci_ctl *ctl = NULL;
	static struct omci_ctl_counters counters;

	(void)umask(0022);
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_ctl_open(&ctl, g_path, mib, &counters), OMCI_OK);

	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	CHECK(fd >= 0);
	if (fd >= 0) {
		struct sockaddr_un addr;

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		memcpy(addr.sun_path, g_path, strlen(g_path) + 1u);
		CHECK_EQ(connect(fd, (const struct sockaddr *)&addr,
				 sizeof(addr)),
			 0);
		(void)close(fd);
	}

	omci_ctl_close(ctl);
	omci_mib_fini(mib);
	(void)unlink(g_path);
}

int main(void)
{
	(void)snprintf(g_path, sizeof(g_path), "/tmp/omcid-ctlmode-%ld.sock",
		       (long)getpid());
	(void)unlink(g_path);

	RUN(test_the_socket_has_mode_0600_under_any_umask);
	RUN(test_a_stale_file_at_the_path_is_replaced_by_a_socket_with_the_same_mode);
	RUN(test_the_socket_still_takes_connections);
	return OMCI_T_REPORT("test_ctl_mode");
}
