// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * omcid on ubus (ADR 0011): the object "omcid", whose method "status" gives
 * the number of instances and the counts of the frames, "rules" the rules
 * of the services in force (me/svcrule.h), "login" the PPPoE login the OLT
 * sent (class 148), "phone" the phone the OLT set up on the phone sockets
 * (omci_publish_phone()), "phone_secrets" their SIP passwords, which pond
 * shows with root's password, "iptv" what the multicast agent holds of
 * television
 * (omci_mcast_publish()), "tv_test" a test of a group from the stick
 * (omci_mcast_test_start()), "mib" the MIB
 * (src/sys/publish.h), of the classes of its argument "classes", an array of
 * numbers, or of every class, in at most "max" bytes of JSON text, when
 * given, and "frames" the conversation with the OLT (src/sys/history.h),
 * from the frame numbered "since", at most "max" frames. The answers are written straight into blobmsg, with no text in
 * between. A change the OLT makes is told to the program of -x, which is how
 * the stick follows it; ubus carries no event of it. A daemon that finds no ubusd, or loses it, tries again
 * every ten seconds.
 */

#include <stdbool.h>
#include <stdint.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "bus_ubus.h"
#include "daemon.h"
#include "mcast/agent.h"
#include "omcid/array_size.h"
#include "sys/history.h"
#include "sys/log.h"
#include "sys/publish.h"

#define RETRY_SEC 10

/* The most classes a "mib" names, and the deepest nesting of an answer. */
#define CLASSES_MAX 64u
#define DEPTH_MAX 8u

static struct ubus_context *g_ctx;
static const struct omci_mib *g_mib;
static const struct omci_ctl_counters *g_counters;
static const struct omci_history *g_history;
static struct omci_mcast *g_mcast;
static uint64_t g_next_try;
static bool g_told;
static struct blob_buf g_buf;

/* A writer into a blob_buf: the cookies of the tables and arrays open. */
struct writer {
	struct blob_buf *b;
	void *open[DEPTH_MAX];
	unsigned int depth;
};

static void w_open(void *ctx, const char *name, bool array)
{
	struct writer *w = ctx;
	void *cookie = array ? blobmsg_open_array(w->b, name) :
			       blobmsg_open_table(w->b, name);

	if (w->depth < DEPTH_MAX)
		w->open[w->depth++] = cookie;
}

static void w_close(void *ctx, bool array)
{
	struct writer *w = ctx;

	if (w->depth == 0)
		return;
	if (array)
		blobmsg_close_array(w->b, w->open[--w->depth]);
	else
		blobmsg_close_table(w->b, w->open[--w->depth]);
}

static void w_str(void *ctx, const char *name, const char *value)
{
	blobmsg_add_string(((struct writer *)ctx)->b, name, value);
}

static void w_num(void *ctx, const char *name, uint32_t value)
{
	blobmsg_add_u32(((struct writer *)ctx)->b, name, value);
}

static void w_flag(void *ctx, const char *name, bool value)
{
	blobmsg_add_u8(((struct writer *)ctx)->b, name, value);
}

/* No value: an attribute of no type, which blobmsg_format_json() writes as null. */
static void w_none(void *ctx, const char *name)
{
	blobmsg_add_field(((struct writer *)ctx)->b, BLOBMSG_TYPE_UNSPEC, name,
			  NULL, 0);
}

static const struct omci_publish_ops g_ops = {
	.open = w_open,
	.close = w_close,
	.str = w_str,
	.num = w_num,
	.flag = w_flag,
	.none = w_none,
};

static int status(struct ubus_context *ctx, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method,
		  struct blob_attr *msg)
{
	struct writer w = { .b = &g_buf };

	(void)obj;
	(void)method;
	(void)msg;
	blob_buf_init(&g_buf, 0);
	omci_publish_status(g_mib, g_counters, &g_ops, &w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

/* The rules of the services in force (sys/publish.h, omci_publish_services()). */
static int rules(struct ubus_context *ctx, struct ubus_object *obj,
		 struct ubus_request_data *req, const char *method,
		 struct blob_attr *msg)
{
	struct writer w = { .b = &g_buf };

	(void)obj;
	(void)method;
	(void)msg;
	blob_buf_init(&g_buf, 0);
	omci_publish_services(&g_ops, &w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

/* The PPPoE login the OLT sent (omci_publish_login()), which pond shows with root's password. */
static int login(struct ubus_context *ctx, struct ubus_object *obj,
		 struct ubus_request_data *req, const char *method,
		 struct blob_attr *msg)
{
	struct writer w = { .b = &g_buf };

	(void)obj;
	(void)method;
	(void)msg;
	blob_buf_init(&g_buf, 0);
	omci_publish_login(g_mib, &g_ops, &w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

/* The phone the OLT set up on the phone sockets (omci_publish_phone()). */
static int phone(struct ubus_context *ctx, struct ubus_object *obj,
		 struct ubus_request_data *req, const char *method,
		 struct blob_attr *msg)
{
	struct writer w = { .b = &g_buf };

	(void)obj;
	(void)method;
	(void)msg;
	blob_buf_init(&g_buf, 0);
	omci_publish_phone(g_mib, &g_ops, &w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

/* The SIP passwords of the phone's lines (omci_publish_phone_secrets()). */
static int phone_secrets(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	struct writer w = { .b = &g_buf };

	(void)obj;
	(void)method;
	(void)msg;
	blob_buf_init(&g_buf, 0);
	omci_publish_phone_secrets(g_mib, &g_ops, &w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

/* What the multicast agent holds of television (omci_mcast_publish()). */
static int iptv(struct ubus_context *ctx, struct ubus_object *obj,
		struct ubus_request_data *req, const char *method,
		struct blob_attr *msg)
{
	struct writer w = { .b = &g_buf };

	(void)obj;
	(void)method;
	(void)msg;
	blob_buf_init(&g_buf, 0);
	omci_mcast_publish(g_mcast, &g_ops, &w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

enum { TEST_GROUP, TEST_VLAN, TEST_SECONDS, TEST_STOP, __TEST_MAX };

static const struct blobmsg_policy g_test_policy[] = {
	[TEST_GROUP] = { .name = "group", .type = BLOBMSG_TYPE_INT32 },
	[TEST_VLAN] = { .name = "vlan", .type = BLOBMSG_TYPE_INT32 },
	[TEST_SECONDS] = { .name = "seconds", .type = BLOBMSG_TYPE_INT32 },
	[TEST_STOP] = { .name = "stop", .type = BLOBMSG_TYPE_INT32 },
};

/* The word of a test that did not start, for pond (omci_mcast_test_start()). */
static const char *test_error(enum omci_status rc)
{
	switch (rc) {
	case OMCI_ERR_INVAL:
		return "invalid";
	case OMCI_ERR_BUSY:
		return "busy";
	case OMCI_DECLINED:
		return "refused";
	case OMCI_ERR_IO:
		return "unable";
	case OMCI_ERR_NOMEM:
		return "no_counter";
	default:
		return "not_sent";
	}
}

/*
 * A test of a group: with "group", an IPv4 address as a number, "vlan" (0
 * untagged) and "seconds" it starts, answering {"ok":true}, or
 * {"error":E,"reason":R}, R the policy's refusal; with "stop" not 0 it ends;
 * and every call but a start answers {"running":B,"bytes":N}, or
 * {"error":"none"} before any test. The arguments are numbers, as pond's
 * calls carry them (its bus.h).
 */
static int tv_test(struct ubus_context *ctx, struct ubus_object *obj,
		   struct ubus_request_data *req, const char *method,
		   struct blob_attr *msg)
{
	struct blob_attr *tb[__TEST_MAX];
	uint64_t bytes = 0;
	bool running = false;
	enum omci_status rc;

	(void)obj;
	(void)method;
	blobmsg_parse(g_test_policy, __TEST_MAX, tb, blob_data(msg),
		      blob_len(msg));
	blob_buf_init(&g_buf, 0);
	if (tb[TEST_GROUP] != NULL) {
		const uint32_t a = blobmsg_get_u32(tb[TEST_GROUP]);
		const uint8_t v4[4] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16),
					(uint8_t)(a >> 8), (uint8_t)a };
		uint8_t group[OMCI_MCAST_ADDR_LEN] = { 0 };
		const char *why = "";

		omci_mcast_addr_set_v4(group, v4);
		rc = omci_mcast_test_start(
			g_mcast, group,
			tb[TEST_VLAN] != NULL ?
				(uint16_t)blobmsg_get_u32(tb[TEST_VLAN]) :
				0u,
			tb[TEST_SECONDS] != NULL ?
				blobmsg_get_u32(tb[TEST_SECONDS]) :
				0u,
			&why);
		if (rc == OMCI_OK) {
			blobmsg_add_u8(&g_buf, "ok", true);
		} else {
			blobmsg_add_string(&g_buf, "error", test_error(rc));
			blobmsg_add_string(&g_buf, "reason", why);
		}
		ubus_send_reply(ctx, req, g_buf.head);
		return 0;
	}
	if (tb[TEST_STOP] != NULL && blobmsg_get_u32(tb[TEST_STOP]) != 0u)
		omci_mcast_test_stop(g_mcast);
	rc = omci_mcast_test_read(g_mcast, &bytes, &running);
	if (rc == OMCI_DECLINED) {
		blobmsg_add_string(&g_buf, "error", "none");
	} else {
		blobmsg_add_u8(&g_buf, "running", running);
		blobmsg_add_u64(&g_buf, "bytes", bytes);
		if (rc != OMCI_OK)
			blobmsg_add_string(&g_buf, "error", "read");
	}
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

enum { MIB_CLASSES, MIB_MAX, __MIB_MAX };

static const struct blobmsg_policy g_mib_policy[] = {
	[MIB_CLASSES] = { .name = "classes", .type = BLOBMSG_TYPE_ARRAY },
	[MIB_MAX] = { .name = "max", .type = BLOBMSG_TYPE_INT32 },
};

static int mib(struct ubus_context *ctx, struct ubus_object *obj,
	       struct ubus_request_data *req, const char *method,
	       struct blob_attr *msg)
{
	struct blob_attr *tb[__MIB_MAX], *cur;
	struct writer w = { .b = &g_buf };
	uint16_t classes[CLASSES_MAX];
	size_t n = 0, rem;

	(void)obj;
	(void)method;
	blobmsg_parse(g_mib_policy, __MIB_MAX, tb, blob_data(msg),
		      blob_len(msg));
	if (tb[MIB_CLASSES] != NULL) {
		blobmsg_for_each_attr(cur, tb[MIB_CLASSES], rem) {
			if (blobmsg_type(cur) != BLOBMSG_TYPE_INT32 ||
			    n == CLASSES_MAX)
				return UBUS_STATUS_INVALID_ARGUMENT;
			classes[n++] = (uint16_t)blobmsg_get_u32(cur);
		}
	}
	blob_buf_init(&g_buf, 0);
	(void)omci_publish_mib(
		g_mib, g_counters, tb[MIB_CLASSES] != NULL ? classes : NULL, n,
		tb[MIB_MAX] != NULL ? blobmsg_get_u32(tb[MIB_MAX]) : 0u, &g_ops,
		&w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

enum { FRAMES_SINCE, FRAMES_MAX, __FRAMES_MAX };

static const struct blobmsg_policy g_frames_policy[] = {
	[FRAMES_SINCE] = { .name = "since", .type = BLOBMSG_TYPE_INT32 },
	[FRAMES_MAX] = { .name = "max", .type = BLOBMSG_TYPE_INT32 },
};

static int frames(struct ubus_context *ctx, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method,
		  struct blob_attr *msg)
{
	struct blob_attr *tb[__FRAMES_MAX];
	struct writer w = { .b = &g_buf };

	(void)obj;
	(void)method;
	blobmsg_parse(g_frames_policy, __FRAMES_MAX, tb, blob_data(msg),
		      blob_len(msg));
	blob_buf_init(&g_buf, 0);
	omci_publish_frames(
		g_history,
		tb[FRAMES_SINCE] != NULL ? blobmsg_get_u32(tb[FRAMES_SINCE]) :
					   0u,
		tb[FRAMES_MAX] != NULL ? blobmsg_get_u32(tb[FRAMES_MAX]) : 0u,
		&g_ops, &w);
	ubus_send_reply(ctx, req, g_buf.head);
	return 0;
}

static const struct ubus_method g_methods[] = {
	UBUS_METHOD_NOARG("status", status),
	UBUS_METHOD_NOARG("rules", rules),
	UBUS_METHOD_NOARG("login", login),
	UBUS_METHOD_NOARG("phone", phone),
	UBUS_METHOD_NOARG("phone_secrets", phone_secrets),
	UBUS_METHOD_NOARG("iptv", iptv),
	UBUS_METHOD("tv_test", tv_test, g_test_policy),
	UBUS_METHOD("mib", mib, g_mib_policy),
	UBUS_METHOD("frames", frames, g_frames_policy),
};

static struct ubus_object_type g_type = UBUS_OBJECT_TYPE("omcid", g_methods);

static struct ubus_object g_object = {
	.name = "omcid",
	.type = &g_type,
	.methods = g_methods,
	.n_methods = ARRAY_SIZE(g_methods),
};

static void bus_close(void)
{
	if (g_ctx == NULL)
		return;
	ubus_free(g_ctx);
	g_ctx = NULL;
	g_object.id = 0;
	/*
	 * The id of the object's type is the one ubusd gave it. Another ubusd
	 * knows no such id and makes the object without a type, which "ubus
	 * list" does not show (measured), so the type is sent again with the
	 * object.
	 */
	g_type.id = 0;
}

static void lost(struct ubus_context *ctx)
{
	(void)ctx;
	OMCI_WARN(
		"ubus: ubusd went away; omcid is published again when it is back");
	bus_close();
	g_next_try = omcid_monotonic_seconds() + RETRY_SEC;
}

/* Connect and add the object, at most every RETRY_SEC seconds. */
static void connect_now(void)
{
	const uint64_t now = omcid_monotonic_seconds();
	int err;

	if (g_ctx != NULL || now < g_next_try)
		return;
	g_next_try = now + RETRY_SEC;
	g_ctx = ubus_connect(NULL);
	if (g_ctx == NULL) {
		if (!g_told)
			OMCI_INFO("ubus: no ubusd yet; trying every %d s",
				  RETRY_SEC);
		g_told = true;
		return;
	}
	g_ctx->connection_lost = lost;
	err = ubus_add_object(g_ctx, &g_object);
	if (err) {
		OMCI_WARN("ubus: omcid is not on ubus: %s", ubus_strerror(err));
		bus_close();
		return;
	}
	g_told = false;
	OMCI_INFO("ubus: omcid is on ubus");
}

static void bus_open(const struct omci_mib *m,
		     const struct omci_ctl_counters *counters,
		     const struct omci_history *history,
		     struct omci_mcast *mcast)
{
	g_mib = m;
	g_counters = counters;
	g_history = history;
	g_mcast = mcast;
	g_next_try = 0;
	connect_now();
}

static int bus_fd(void)
{
	connect_now();
	return g_ctx != NULL ? g_ctx->sock.fd : -1;
}

static void bus_handle(void)
{
	if (g_ctx != NULL)
		ubus_handle_event(g_ctx);
}

const struct omcid_bus omcid_bus_ubus = {
	.open = bus_open,
	.fd = bus_fd,
	.handle = bus_handle,
	.close = bus_close,
};
