// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-call: ask onu-act to do METHOD (src/act.h), which a program that runs
 * as an account of its own may not do itself.
 *
 *   onu-call METHOD
 *
 * It prints what the method's program wrote and exits with its status; for
 * a method run in the background, as soon as onu-act has started it, with 0.
 * The programs that tell onu-apply of a change they publish (omcid and
 * dhcp6d, with -x) run it with their name as the method: the method of that
 * name runs "onu-apply" for it, as root, and ubusd's access list lets each
 * program call its own and no other. Exit status 1 is a failure of the call
 * itself, 2 a wrong command line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#define TIMEOUT_MS 35000

enum { CODE, OUTPUT, __MAX };

static int g_code = 1;

static void got(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const struct blobmsg_policy policy[__MAX] = {
		[CODE] = { .name = "code", .type = BLOBMSG_TYPE_INT32 },
		[OUTPUT] = { .name = "output", .type = BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *tb[__MAX];

	(void)req;
	(void)type;
	blobmsg_parse(policy, __MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[OUTPUT])
		fputs(blobmsg_get_string(tb[OUTPUT]), stdout);
	if (tb[CODE])
		g_code = (int)blobmsg_get_u32(tb[CODE]) & 0xff;
}

int main(int argc, char **argv)
{
	struct blob_buf b = { 0 };
	struct ubus_context *ctx;
	uint32_t id;
	int ret;

	if (argc != 2 || !*argv[1]) {
		fprintf(stderr, "usage: onu-call METHOD\n");
		return 2;
	}
	ctx = ubus_connect(NULL);
	if (!ctx) {
		fprintf(stderr, "onu-call: no ubus\n");
		return 1;
	}
	blob_buf_init(&b, 0);
	ret = ubus_lookup_id(ctx, "onu-act", &id);
	if (!ret)
		ret = ubus_invoke(ctx, id, argv[1], b.head, got, NULL, TIMEOUT_MS);
	if (ret)
		fprintf(stderr, "onu-call: %s: %s\n", argv[1], ubus_strerror(ret));
	blob_buf_free(&b);
	ubus_free(ctx);
	return ret ? 1 : g_code;
}
