// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* dhcp6d on ubus (bus.h). */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "bus.h"
#include "dhcp6d.h"
#include "state.h"

static struct ubus_context *g_ctx;
static char g_name[64];
static struct blob_attr *g_state;

static int status(struct ubus_context *ctx, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_buf b = { 0 };

	(void)obj;
	(void)method;
	(void)msg;
	if (g_state) {
		ubus_send_reply(ctx, req, g_state);
		return 0;
	}
	blob_buf_init(&b, 0);
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return 0;
}

static const struct ubus_method g_methods[] = {
	UBUS_METHOD_NOARG("status", status),
};

static struct ubus_object_type g_type = UBUS_OBJECT_TYPE("dhcp6d", g_methods);

static struct ubus_object g_object = {
	.type = &g_type,
	.methods = g_methods,
	.n_methods = ARRAY_SIZE(g_methods),
};

static void lost(struct ubus_context *ctx)
{
	(void)ctx;
	DHCP6D_WARN("ubusd went away: the state is published again when it is back");
	bus_close();
}

int bus_open(const char *name)
{
	int err;

	if (g_ctx)
		return 0;
	snprintf(g_name, sizeof(g_name), "dhcp6d.%s", name);
	g_object.name = g_name;
	g_ctx = ubus_connect(NULL);
	if (!g_ctx)
		return -ENOTCONN;
	g_ctx->connection_lost = lost;
	err = ubus_add_object(g_ctx, &g_object);
	if (err) {
		DHCP6D_WARN("%s is not on ubus: %s", g_name, ubus_strerror(err));
		ubus_free(g_ctx);
		g_ctx = NULL;
		return -EIO;
	}
	return 0;
}

int bus_fd(void)
{
	return g_ctx ? g_ctx->sock.fd : -1;
}

void bus_handle(void)
{
	if (g_ctx)
		ubus_handle_event(g_ctx);
}

/* A string of the state, left out when there is none. */
static void add_string(struct blob_buf *b, const char *name, const char *v)
{
	if (v)
		blobmsg_add_string(b, name, v);
}

void bus_publish(const struct dhcp6d_state *s)
{
	struct blob_buf b = { 0 };
	bool same;

	blob_buf_init(&b, 0);
	add_string(&b, "name", s->name);
	add_string(&b, "wan", s->wan);
	add_string(&b, "lan", s->lan);
	add_string(&b, "prefix", s->prefix);
	add_string(&b, "host", s->host);
	blobmsg_add_u8(&b, "bound", s->bound);
	blobmsg_add_u8(&b, "routed", s->routed);
	same = g_state && blob_attr_equal(g_state, b.head);
	if (!same) {
		free(g_state);
		g_state = blob_memdup(b.head);
	}
	if (g_ctx && !same)
		ubus_send_event(g_ctx, "dhcp6d", b.head);
	blob_buf_free(&b);
}

void bus_close(void)
{
	if (!g_ctx)
		return;
	ubus_free(g_ctx);
	g_ctx = NULL;
	g_object.id = 0;
}
