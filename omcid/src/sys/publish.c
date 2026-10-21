// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* What the daemon publishes of what it holds (publish.h). */

#include <stdio.h>

#include "me/svcrule.h"
#include "me/table.h"
#include "me/vlan.h"
#include "mib/store.h"
#include "sys/ctl.h"
#include "sys/publish.h"

/* The ONU-G, and its attribute that holds the logical password. */
#define ONU_G 256u
#define LOGICAL_PASSWORD 11u

/* The most bytes of a value written: of an attribute, or of a table's row. */
#define VALUE_MAX 256u

/* An attribute's value in hexadecimal: room for the rows of every table. */
static char g_hex[2u * OMCI_MIB_TABLE_ENTRIES * OMCI_MIB_TABLE_ENTRY_MAX + 1u];

/*
 * The lengths of the JSON text around the instances: "{"mes":[", and
 * after them the list's end, "truncated", "logical_password_set" and the
 * keys of the counts.
 */
#define HEAD_LEN 8u
#define TAIL_KEYS_LEN (14u + 24u + 21u + 12u + 12u + 2u)

static size_t digits(uint32_t v)
{
	size_t n = 1;

	while (v >= 10u) {
		v /= 10u;
		n++;
	}
	return n;
}

static bool left_out(uint16_t class_id, uint8_t attr)
{
	return class_id == ONU_G && attr == LOGICAL_PASSWORD;
}

static size_t value_size(const struct omci_me_attr *a)
{
	return a->size < VALUE_MAX ? a->size : VALUE_MAX;
}

/* The bytes of an attribute: a row's, times the rows of a table. */
static size_t attr_bytes(const struct omci_mib *mib, uint16_t class_id,
			 uint16_t instance, uint8_t attr,
			 const struct omci_me_attr *a)
{
	if (!omci_me_attr_is_table(a))
		return value_size(a);
	return omci_mib_table_count(mib, class_id, instance, attr) *
	       value_size(a);
}

/* The length of an instance as JSON text. */
static size_t instance_len(const struct omci_mib *mib, uint16_t class_id,
			   uint16_t instance)
{
	const struct omci_me_class *cls = omci_me_class_by_id(class_id);
	size_t len =
		9u + digits(class_id) + 12u + digits(instance) + 10u + 12u + 2u;
	size_t attrs = 0, tables = 0;

	for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);

		if (omci_me_attr_is_table(a))
			len += digits(attr) + (tables++ ? 1u : 0u);
		if (left_out(class_id, attr))
			continue;
		len += digits(attr) + 5u +
		       2u * attr_bytes(mib, class_id, instance, attr, a) +
		       (attrs++ ? 1u : 0u);
	}
	return len;
}

/* An attribute's value in hexadecimal, in g_hex; what the store lacks is zeros. */
static void hex(const struct omci_mib *mib, uint16_t class_id,
		uint16_t instance, uint8_t attr, const struct omci_me_attr *a)
{
	static const char digit[] = "0123456789abcdef";
	const bool table = omci_me_attr_is_table(a);
	const size_t size = value_size(a);
	const size_t rows =
		table ? omci_mib_table_count(mib, class_id, instance, attr) :
			1u;
	uint8_t v[VALUE_MAX];
	char *p = g_hex;

	for (size_t r = 0; r < rows; r++) {
		for (size_t i = 0; i < size; i++)
			v[i] = 0;
		if (table)
			(void)!omci_mib_table_get(mib, class_id, instance, attr,
						  r, v, sizeof(v));
		else
			(void)!omci_mib_attr_get(mib, class_id, instance, attr,
						 v, sizeof(v), NULL);
		for (size_t i = 0; i < size; i++) {
			*p++ = digit[v[i] >> 4];
			*p++ = digit[v[i] & 15u];
		}
	}
	*p = '\0';
}

static void write_instance(const struct omci_mib *mib, uint16_t class_id,
			   uint16_t instance,
			   const struct omci_publish_ops *ops, void *ctx)
{
	const struct omci_me_class *cls = omci_me_class_by_id(class_id);
	char key[4];

	ops->open(ctx, NULL, false);
	ops->num(ctx, "class", class_id);
	ops->num(ctx, "instance", instance);
	ops->open(ctx, "attrs", false);
	for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
		if (left_out(class_id, attr))
			continue;
		hex(mib, class_id, instance, attr, omci_me_attr_get(cls, attr));
		(void)snprintf(key, sizeof(key), "%u", attr);
		ops->str(ctx, key, g_hex);
	}
	ops->close(ctx, false);
	ops->open(ctx, "tables", true);
	for (uint8_t attr = 1; attr <= cls->attr_count; attr++)
		if (omci_me_attr_is_table(omci_me_attr_get(cls, attr)))
			ops->num(ctx, NULL, attr);
	ops->close(ctx, true);
	ops->close(ctx, false);
}

/* Whether an ONU-G has a logical password. */
static bool password_set(const struct omci_mib *mib)
{
	struct omci_mib_cursor c;
	bool set = false;

	for (enum omci_status rc = omci_mib_walk_first(mib, &c); rc == OMCI_OK;
	     rc = omci_mib_walk_next(mib, &c)) {
		uint8_t v[VALUE_MAX] = { 0 };
		size_t n = 0;

		if (c.class_id != ONU_G ||
		    omci_mib_attr_get(mib, ONU_G, c.instance, LOGICAL_PASSWORD,
				      v, sizeof(v), &n) != OMCI_OK)
			continue;
		for (size_t i = 0; i < n; i++)
			set |= v[i] != 0u;
	}
	return set;
}

static size_t tail_len(const struct omci_ctl_counters *counters, bool truncated,
		       bool secret)
{
	return TAIL_KEYS_LEN + (truncated ? 4u : 5u) + (secret ? 4u : 5u) +
	       digits((uint32_t)counters->handled) +
	       digits((uint32_t)counters->rejected) +
	       digits((uint32_t)counters->answered);
}

static void write_frames(const struct omci_ctl_counters *counters,
			 const struct omci_publish_ops *ops, void *ctx)
{
	ops->open(ctx, "frames", false);
	ops->num(ctx, "handled", (uint32_t)counters->handled);
	ops->num(ctx, "rejected", (uint32_t)counters->rejected);
	ops->num(ctx, "answered", (uint32_t)counters->answered);
	ops->close(ctx, false);
}

size_t omci_publish_mib(const struct omci_mib *mib,
			const struct omci_ctl_counters *counters,
			const uint16_t *classes, size_t n, size_t max,
			const struct omci_publish_ops *ops, void *ctx)
{
	const bool secret = password_set(mib);
	const size_t rounds = classes != NULL ? n : 1u;
	/* The end is kept room for as it would be at its longest. */
	const size_t tail = tail_len(counters, false, false);
	size_t len = HEAD_LEN, written = 0;
	bool truncated = false;

	ops->open(ctx, "mes", true);
	for (size_t k = 0; k < rounds && !truncated; k++) {
		struct omci_mib_cursor c;

		for (enum omci_status rc = omci_mib_walk_first(mib, &c);
		     rc == OMCI_OK && !truncated;
		     rc = omci_mib_walk_next(mib, &c)) {
			if (classes != NULL && c.class_id != classes[k])
				continue;

			const size_t one =
				instance_len(mib, c.class_id, c.instance) +
				(written ? 1u : 0u);

			if (max != 0 && len + one + tail > max) {
				truncated = true;
				continue;
			}
			write_instance(mib, c.class_id, c.instance, ops, ctx);
			len += one;
			written++;
		}
	}
	ops->close(ctx, true);
	ops->flag(ctx, "truncated", truncated);
	ops->flag(ctx, "logical_password_set", secret);
	write_frames(counters, ops, ctx);
	return len + tail_len(counters, truncated, secret);
}

void omci_publish_status(const struct omci_mib *mib,
			 const struct omci_ctl_counters *counters,
			 const struct omci_publish_ops *ops, void *ctx)
{
	ops->num(ctx, "instances", (uint32_t)omci_mib_count(mib));
	write_frames(counters, ops, ctx);
}

/* One rule of the services, as an element of "rules". */
static void write_rule(const struct omci_svc_rule *r,
		       const struct omci_publish_ops *ops, void *ctx)
{
	ops->open(ctx, NULL, false);
	ops->num(ctx, "fibre", r->fibre);
	ops->str(ctx, "action", omci_svc_action_name(r->action));
	if (r->action == OMCI_SVC_MOVE)
		ops->num(ctx, "host", r->host);
	ops->flag(ctx, "auto", r->made);
	ops->close(ctx, false);
}

void omci_publish_services(const struct omci_publish_ops *ops, void *ctx)
{
	const struct omci_svc_rule *person = NULL, *made = NULL;
	size_t made_n = 0;
	const size_t n = omci_vlan_services(&person, &made, &made_n);

	ops->open(ctx, "rules", true);
	for (size_t i = 0; i < n; i++)
		write_rule(&person[i], ops, ctx);
	for (size_t i = 0; i < made_n; i++)
		write_rule(&made[i], ops, ctx);
	ops->close(ctx, true);
}

/* Class 148, Authentication security method, and its fields of 25 bytes. */
enum {
	ME_AUTH = 148,
	AUTH_USER1 = 2,
	AUTH_PASSWORD = 3,
	AUTH_REALM = 4,
	AUTH_USER2 = 5,
	AUTH_FIELD = 25,
};

/*
 * Field @attr of instance @inst of class 148 as text after the @len bytes
 * of @out: its printable ASCII up to the first other byte. Returns the new
 * length.
 */
static size_t auth_text(const struct omci_mib *mib, uint16_t inst, uint8_t attr,
			char *out, size_t len)
{
	uint8_t v[AUTH_FIELD] = { 0 };

	(void)!omci_mib_attr_get(mib, ME_AUTH, inst, attr, v, sizeof(v), NULL);
	for (size_t i = 0; i < sizeof(v) && v[i] >= 0x20u && v[i] < 0x7fu; i++)
		out[len++] = (char)v[i];
	out[len] = '\0';
	return len;
}

void omci_publish_login(const struct omci_mib *mib,
			const struct omci_publish_ops *ops, void *ctx)
{
	struct omci_mib_cursor cur;
	char user[2 * AUTH_FIELD + 1], text[AUTH_FIELD + 1];

	for (enum omci_status w = omci_mib_walk_first(mib, &cur); w == OMCI_OK;
	     w = omci_mib_walk_next(mib, &cur)) {
		if (cur.class_id != ME_AUTH)
			continue;
		size_t n = auth_text(mib, cur.instance, AUTH_USER1, user, 0);

		n = auth_text(mib, cur.instance, AUTH_USER2, user, n);
		if (n == 0u)
			continue;
		ops->str(ctx, "user", user);
		(void)auth_text(mib, cur.instance, AUTH_PASSWORD, text, 0);
		ops->str(ctx, "secret", text);
		(void)auth_text(mib, cur.instance, AUTH_REALM, text, 0);
		ops->str(ctx, "realm", text);
		return;
	}
}
