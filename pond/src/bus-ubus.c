// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* bus.h over ubus: one connection, made when first needed and again after it was lost. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>

#include "bus.h"

#define TIMEOUT_MS 500
#define CALL_TIMEOUT_MS 2000
#define ACT_TIMEOUT_MS 120000

static struct ubus_context *g_ctx;
static struct blob_attr *g_reply;

static void got(struct ubus_request *req, int type, struct blob_attr *msg)
{
	(void)req;
	(void)type;
	free(g_reply);
	g_reply = msg ? blob_memdup(msg) : NULL;
}

/* The value of @a as text in @out. */
static void value(struct blob_attr *a, char *out)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_STRING:
		snprintf(out, BUS_VALUE, "%s", blobmsg_get_string(a));
		break;
	case BLOBMSG_TYPE_INT64:
		snprintf(out, BUS_VALUE, "%lld", (long long)blobmsg_get_u64(a));
		break;
	case BLOBMSG_TYPE_INT32:
		snprintf(out, BUS_VALUE, "%d", (int)blobmsg_get_u32(a));
		break;
	case BLOBMSG_TYPE_INT16:
		snprintf(out, BUS_VALUE, "%d", (int)blobmsg_get_u16(a));
		break;
	case BLOBMSG_TYPE_BOOL:
		snprintf(out, BUS_VALUE, "%s", blobmsg_get_bool(a) ? "true" : "false");
		break;
	case BLOBMSG_TYPE_ARRAY: {
		struct blob_attr *cur;
		size_t rem, len = 0;

		out[0] = '\0';
		blobmsg_for_each_attr(cur, a, rem) {
			if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
				continue;
			len += (size_t)snprintf(out + len, len < BUS_VALUE ? BUS_VALUE - len : 0,
						"%s%s", len ? " " : "", blobmsg_get_string(cur));
			if (len >= BUS_VALUE)
				break;
		}
		break;
	}
	default:
		out[0] = '\0';
	}
}

/* The value at @path in the attributes @data of @len bytes. */
static void find(struct blob_attr *data, size_t len, const char *path, char *out)
{
	size_t k = strcspn(path, ".");
	struct blob_attr *cur;
	size_t rem = len;

	__blob_for_each_attr(cur, data, rem) {
		if (strlen(blobmsg_name(cur)) != k || strncmp(blobmsg_name(cur), path, k))
			continue;
		if (!path[k])
			value(cur, out);
		else if (blobmsg_type(cur) == BLOBMSG_TYPE_TABLE)
			find(blobmsg_data(cur), blobmsg_data_len(cur), path + k + 1, out);
		return;
	}
}

/* The id of @object, connecting first: 0, or a negative errno. */
static int lookup(const char *object, uint32_t *id)
{
	int err;

	if (!g_ctx)
		g_ctx = ubus_connect(NULL);
	if (!g_ctx)
		return -ENOTCONN;
	err = ubus_lookup_id(g_ctx, object, id);
	if (err == UBUS_STATUS_CONNECTION_FAILED) {
		ubus_free(g_ctx);
		g_ctx = NULL;
		return -ENOTCONN;
	}
	return err ? -ENOENT : 0;
}

int bus_status(const char *object, const char *const *paths, size_t n,
	       char (*out)[BUS_VALUE])
{
	uint32_t id;
	int err;

	for (size_t i = 0; i < n; i++)
		out[i][0] = '\0';
	err = lookup(object, &id);
	if (err)
		return err;
	free(g_reply);
	g_reply = NULL;
	if (ubus_invoke(g_ctx, id, "status", NULL, got, NULL, TIMEOUT_MS) || !g_reply)
		return -EIO;
	for (size_t i = 0; i < n; i++)
		find(blob_data(g_reply), blob_len(g_reply), paths[i], out[i]);
	return 0;
}

int bus_call(const char *object, const char *method,
	     const struct bus_arg *args, size_t nargs, char *out, size_t cap)
{
	struct blob_buf b = { 0 };
	uint32_t id;
	size_t len;
	char *text;
	int err;

	err = lookup(object, &id);
	if (err)
		return err;
	blob_buf_init(&b, 0);
	for (size_t i = 0; i < nargs; i++) {
		void *array;

		if (!args[i].array) {
			blobmsg_add_u32(&b, args[i].name, args[i].values[0]);
			continue;
		}
		array = blobmsg_open_array(&b, args[i].name);
		for (size_t k = 0; k < args[i].n; k++)
			blobmsg_add_u32(&b, NULL, args[i].values[k]);
		blobmsg_close_array(&b, array);
	}
	free(g_reply);
	g_reply = NULL;
	err = ubus_invoke(g_ctx, id, method, b.head, got, NULL, CALL_TIMEOUT_MS);
	blob_buf_free(&b);
	if (err || !g_reply)
		return -EIO;
	text = blobmsg_format_json(g_reply, true);
	if (!text)
		return -EIO;
	len = strlen(text);
	if (len >= cap) {
		free(text);
		return -EFBIG;
	}
	memcpy(out, text, len + 1);
	free(text);
	return (int)len;
}

int bus_act(const char *method, const char *const *names,
	    const char *const *values, size_t n, const char *input, char *out,
	    size_t cap)
{
	enum { CODE, OUTPUT, __MAX };
	static const struct blobmsg_policy policy[] = {
		[CODE] = { .name = "code", .type = BLOBMSG_TYPE_INT32 },
		[OUTPUT] = { .name = "output", .type = BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *tb[__MAX];
	struct blob_buf b = { 0 };
	uint32_t id;
	int err;

	if (cap)
		out[0] = '\0';
	err = lookup("onu-act", &id);
	if (err)
		return err;
	blob_buf_init(&b, 0);
	for (size_t i = 0; i < n; i++)
		if (values[i])
			blobmsg_add_string(&b, names[i], values[i]);
	if (input)
		blobmsg_add_string(&b, "input", input);
	free(g_reply);
	g_reply = NULL;
	err = ubus_invoke(g_ctx, id, method, b.head, got, NULL, ACT_TIMEOUT_MS);
	blob_buf_free(&b);
	if (err == UBUS_STATUS_INVALID_ARGUMENT)
		return -EINVAL;
	if (err || !g_reply)
		return -EIO;
	blobmsg_parse(policy, __MAX, tb, blob_data(g_reply), blob_len(g_reply));
	if (!tb[CODE])
		return -EIO;
	if (tb[OUTPUT] && cap)
		snprintf(out, cap, "%s", blobmsg_get_string(tb[OUTPUT]));
	return (int)blobmsg_get_u32(tb[CODE]);
}
