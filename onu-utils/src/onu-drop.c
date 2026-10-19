// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-drop: start a program as an account of its own, with only the powers
 * it needs (ADR 0011, rule 2).
 *
 *   onu-drop [-c CAP[,CAP...]] USER PROGRAM [ARGUMENT...]
 *
 * Started as root, it takes the account USER and its groups, keeps of root's
 * powers those named by -c and no others, and runs PROGRAM in its own place.
 * The powers are kept for PROGRAM and for whatever it runs (ambient), and
 * nothing it runs can have more: the rest is out of the bounding set, and
 * the program can gain nothing from a file (no_new_privs). Without -c it
 * runs with none. The names are those of capabilities(7) without "cap_":
 * net_admin, net_raw, net_bind_service and sys_time. The account is no
 * root's (user ID 0).
 *
 * Exit status: 2 for a wrong command line, 1 when it cannot become the
 * account or run the program (nothing of PROGRAM has run then).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/capability.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const struct {
	const char *name;
	int cap;
} caps_known[] = {
	{ "net_bind_service", CAP_NET_BIND_SERVICE },
	{ "net_admin", CAP_NET_ADMIN },
	{ "net_raw", CAP_NET_RAW },
	{ "sys_time", CAP_SYS_TIME },
};

static int help(void)
{
	fprintf(stderr, "usage: onu-drop [-c CAP[,CAP...]] USER PROGRAM [ARGUMENT...]\n");
	return 2;
}

/* The mask of the powers @list names, "net_raw,sys_time", into @mask: 0, or -1. */
static int caps_parse(const char *list, uint64_t *mask)
{
	*mask = 0;
	for (;;) {
		size_t n = strcspn(list, ","), i;

		for (i = 0; i < ARRAY_SIZE(caps_known); i++)
			if (strlen(caps_known[i].name) == n && !strncmp(list, caps_known[i].name, n))
				break;
		if (i == ARRAY_SIZE(caps_known))
			return -1;
		*mask |= 1ull << caps_known[i].cap;
		if (!list[n])
			return 0;
		list += n + 1;
	}
}

/* The powers outside @mask leave the bounding set: no program of ours gets them back. */
static int bounding_drop(uint64_t mask)
{
	for (int cap = 0; prctl(PR_CAPBSET_READ, cap) >= 0; cap++)
		if (!(mask & (1ull << cap)) && prctl(PR_CAPBSET_DROP, cap))
			return -1;
	return 0;
}

/* Exactly @mask, permitted, effective and inheritable. */
static int caps_set(uint64_t mask)
{
	struct __user_cap_header_struct hdr = { .version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3] = { };

	for (int i = 0; i < _LINUX_CAPABILITY_U32S_3; i++) {
		uint32_t part = (uint32_t)(mask >> (32 * i));

		data[i].permitted = part;
		data[i].effective = part;
		data[i].inheritable = part;
	}
	return (int)syscall(SYS_capset, &hdr, data);
}

/* Exactly @mask stays with what the program runs. */
static int ambient_set(uint64_t mask)
{
	if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0))
		return -1;
	for (int cap = 0; cap < 64; cap++)
		if ((mask & (1ull << cap)) &&
		    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0))
			return -1;
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t mask = 0;
	struct passwd *pw;
	int first = 1;

	if (argc > 2 && !strcmp(argv[1], "-c")) {
		if (caps_parse(argv[2], &mask))
			return help();
		first = 3;
	}
	if (argc - first < 2)
		return help();
	pw = getpwnam(argv[first]);
	if (!pw || !pw->pw_uid) {
		fprintf(stderr, "onu-drop: %s is no account to run as\n", argv[first]);
		return 1;
	}
	if (geteuid()) {
		fprintf(stderr, "onu-drop: it is started as root\n");
		return 1;
	}
	if (bounding_drop(mask) || prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) ||
	    initgroups(pw->pw_name, pw->pw_gid) || setgid(pw->pw_gid) ||
	    setuid(pw->pw_uid) || caps_set(mask) || ambient_set(mask) ||
	    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) ||
	    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		fprintf(stderr, "onu-drop: cannot run as %s: %s\n", pw->pw_name,
			strerror(errno));
		return 1;
	}
	execvp(argv[first + 1], argv + first + 1);
	fprintf(stderr, "onu-drop: %s: %s\n", argv[first + 1], strerror(errno));
	return 1;
}
