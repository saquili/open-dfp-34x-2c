// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-svc: the stick's programs as procd's services, which procd starts
 * again when they end.
 *
 *   onu-svc start NAME [-r THRESHOLD,TIMEOUT,RETRY] [-l | -o FILE] -- PROGRAM [ARG...]
 *                       the service NAME runs PROGRAM: procd starts it, and
 *                       again TIMEOUT seconds after it ends; a run shorter
 *                       than THRESHOLD seconds is a crash, and after RETRY
 *                       crashes in a row procd gives up (0: never). The
 *                       default is 60,2,0. -l puts the program's output in
 *                       the system log, -o appends it to FILE. A service
 *                       that runs already with another program or other
 *                       values is started again with these; with the same,
 *                       nothing happens.
 *   onu-svc stop NAME   procd ends the program and forgets the service
 *   onu-svc list [NAME] a line for each service: its name, "running" and
 *                       the program's process, or "stopped" and its exit
 *                       status, and how often procd started it again
 *
 * The services live in procd, as long as procd runs; there is no file of
 * them. procd is asked over ubus, on ubus's own socket.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>

#include "svcmsg.h"

#define TIMEOUT_MS 3000

static int usage(void)
{
	fputs("usage: onu-svc start NAME [-r THRESHOLD,TIMEOUT,RETRY] [-l | -o FILE] -- PROGRAM [ARG...]\n"
	      "       onu-svc stop NAME\n"
	      "       onu-svc list [NAME]\n", stderr);
	return 2;
}

enum { INST_RUNNING, INST_PID, INST_EXIT, INST_RESPAWNS, __INST_MAX };

static const struct blobmsg_policy inst_policy[__INST_MAX] = {
	[INST_RUNNING] = { "running", BLOBMSG_TYPE_BOOL },
	[INST_PID] = { "pid", BLOBMSG_TYPE_INT32 },
	[INST_EXIT] = { "exit_code", BLOBMSG_TYPE_INT32 },
	[INST_RESPAWNS] = { "respawns", BLOBMSG_TYPE_INT32 },
};

static const struct blobmsg_policy instances_policy = { "instances", BLOBMSG_TYPE_TABLE };

/* A line for each instance of each service of procd's answer to "list". */
static void print_list(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *svc, *inst, *instances, *tb[__INST_MAX];
	size_t rem, rem2;

	blobmsg_for_each_attr(svc, msg, rem) {
		blobmsg_parse(&instances_policy, 1, &instances, blobmsg_data(svc),
			      blobmsg_data_len(svc));
		if (!instances) {
			printf("%s stopped\n", blobmsg_name(svc));
			continue;
		}
		blobmsg_for_each_attr(inst, instances, rem2) {
			blobmsg_parse(inst_policy, __INST_MAX, tb, blobmsg_data(inst),
				      blobmsg_data_len(inst));
			printf("%s", blobmsg_name(svc));
			if (strcmp(blobmsg_name(inst), blobmsg_name(svc)))
				printf("/%s", blobmsg_name(inst));
			if (tb[INST_RUNNING] && blobmsg_get_bool(tb[INST_RUNNING]))
				printf(" running %u", tb[INST_PID] ? blobmsg_get_u32(tb[INST_PID]) : 0);
			else
				printf(" stopped %u", tb[INST_EXIT] ? blobmsg_get_u32(tb[INST_EXIT]) : 0);
			if (tb[INST_RESPAWNS])
				printf(" respawns %u", blobmsg_get_u32(tb[INST_RESPAWNS]));
			printf("\n");
		}
	}
}

/* Call @method of procd's "service" with the JSON text @json. */
static int call(const char *method, const char *json, ubus_data_handler_t cb)
{
	struct ubus_context *ctx;
	struct blob_buf b = { 0 };
	uint32_t id;
	int ret;

	ctx = ubus_connect(NULL);
	if (!ctx) {
		fprintf(stderr, "onu-svc: no ubus\n");
		return 1;
	}
	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_string(&b, json)) {
		fprintf(stderr, "onu-svc: not a message: %s\n", json);
		ret = UBUS_STATUS_INVALID_ARGUMENT;
	} else {
		ret = ubus_lookup_id(ctx, "service", &id);
		if (!ret)
			ret = ubus_invoke(ctx, id, method, b.head, cb, NULL, TIMEOUT_MS);
		if (ret)
			fprintf(stderr, "onu-svc: procd's %s: %s\n", method, ubus_strerror(ret));
	}
	blob_buf_free(&b);
	ubus_free(ctx);
	return ret ? 1 : 0;
}

static int start(int argc, char **argv)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *name = argv[1];
	const char *out = NULL;
	bool log = false;
	char json[4096];
	int ch;

	if (argc < 2 || !svc_name_ok(name))
		return usage();
	optind = 2;
	while ((ch = getopt(argc, argv, "+r:lo:")) != -1) {
		switch (ch) {
		case 'r':
			if (svc_parse_respawn(optarg, &r))
				return usage();
			break;
		case 'l':
			log = true;
			break;
		case 'o':
			out = optarg;
			break;
		default:
			return usage();
		}
	}
	/* The program follows "--", which getopt has taken. */
	if (optind >= argc || strcmp(argv[optind - 1], "--") || (log && out))
		return usage();
	if (svc_set_json(json, sizeof(json), name, &r, log, out,
			 (const char *const *)(argv + optind)) < 0) {
		fprintf(stderr, "onu-svc: the command of %s is too long\n", name);
		return 1;
	}
	return call("set", json, NULL);
}

int main(int argc, char **argv)
{
	char json[256];

	if (argc < 2)
		return usage();
	if (!strcmp(argv[1], "start"))
		return start(argc - 1, argv + 1);
	if (!strcmp(argv[1], "stop")) {
		if (argc != 3 || !svc_name_ok(argv[2]))
			return usage();
		svc_name_json(json, sizeof(json), argv[2]);
		return call("delete", json, NULL);
	}
	if (!strcmp(argv[1], "list")) {
		if (argc > 3 || (argc == 3 && !svc_name_ok(argv[2])))
			return usage();
		if (argc == 3)
			svc_name_json(json, sizeof(json), argv[2]);
		else
			strcpy(json, "{}");
		return call("list", json, print_list);
	}
	return usage();
}
