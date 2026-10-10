// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* pppoed's messages to netifd (nproto.h), sent over ubus. */

#include <errno.h>
#include <stdio.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>

#include "nproto.h"
#include "pppoed.h"

#define TIMEOUT_MS 3000

int nproto_send(const char *iface, const char *json)
{
	struct ubus_context *ctx = ubus_connect(NULL);
	struct blob_buf b = { 0 };
	char object[96];
	uint32_t id;
	int ret;

	if (!ctx)
		return -ENOTCONN;
	snprintf(object, sizeof(object), "network.interface.%s", iface);
	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_string(&b, json)) {
		ret = -EINVAL;
	} else {
		ret = ubus_lookup_id(ctx, object, &id);
		if (!ret)
			ret = ubus_invoke(ctx, id, "notify_proto", b.head, NULL, NULL, TIMEOUT_MS);
		if (ret) {
			PPPOED_WARN("netifd's %s did not take the message: %s", object,
				    ubus_strerror(ret));
			ret = -EIO;
		}
	}
	blob_buf_free(&b);
	ubus_free(ctx);
	return ret;
}
