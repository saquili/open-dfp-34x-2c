// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The kernel answers a parameter with its type and a list of values, one for
 * each configuration mode it has; a boolean's value is a flag, there when
 * true. A number to set is decimal, or hexadecimal after "0x", and nothing
 * else: no sign, no space, no octal.
 */

#include "dlparam.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/devlink.h>
#include <linux/genetlink.h>

#include "genl.h"

static void put_handle(struct nlmsghdr *nlh, const struct dl_dev *d)
{
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_BUS_NAME, d->bus);
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_DEV_NAME, d->dev);
}

struct nlmsghdr *dl_dev_dump_msg(char *buf, uint16_t family, uint32_t seq)
{
	return genl_request(buf, family, DEVLINK_CMD_GET, NLM_F_DUMP, seq);
}

struct nlmsghdr *dl_param_get_msg(char *buf, uint16_t family, uint32_t seq,
				  const struct dl_dev *d, const char *name)
{
	struct nlmsghdr *nlh;

	nlh = genl_request(buf, family, DEVLINK_CMD_PARAM_GET, 0, seq);
	put_handle(nlh, d);
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_PARAM_NAME, name);
	return nlh;
}

/* A number of @text, to at most @max. */
static int number(const char *text, uint64_t max, uint64_t *v)
{
	const char *digits = text, *allowed = "0123456789";
	int base = 10;

	if (!strncmp(text, "0x", 2) || !strncmp(text, "0X", 2)) {
		digits = text + 2;
		allowed = "0123456789abcdefABCDEF";
		base = 16;
	}
	if (!*digits || digits[strspn(digits, allowed)])
		return -EINVAL;
	errno = 0;
	*v = strtoull(digits, NULL, base);
	if (errno == ERANGE || *v > max)
		return -ERANGE;
	return 0;
}

int dl_param_set_msg(char *buf, uint16_t family, uint32_t seq,
		     const struct dl_dev *d, const char *name, uint8_t type,
		     const char *text, struct nlmsghdr **out)
{
	static const uint64_t max[] = {
		[DEVLINK_VAR_ATTR_TYPE_U8] = UINT8_MAX,
		[DEVLINK_VAR_ATTR_TYPE_U16] = UINT16_MAX,
		[DEVLINK_VAR_ATTR_TYPE_U32] = UINT32_MAX,
		[DEVLINK_VAR_ATTR_TYPE_U64] = UINT64_MAX,
	};
	struct nlmsghdr *nlh;
	uint64_t v = 0;
	int err = 0;

	switch (type) {
	case DEVLINK_VAR_ATTR_TYPE_U8:
	case DEVLINK_VAR_ATTR_TYPE_U16:
	case DEVLINK_VAR_ATTR_TYPE_U32:
	case DEVLINK_VAR_ATTR_TYPE_U64:
		err = number(text, max[type], &v);
		break;
	case DEVLINK_VAR_ATTR_TYPE_STRING:
	case DEVLINK_VAR_ATTR_TYPE_NUL_STRING:
		if (strlen(text) >= DL_VALUE_MAX)
			err = -ERANGE;
		break;
	case DEVLINK_VAR_ATTR_TYPE_FLAG:
		if (!strcmp(text, "true"))
			v = 1;
		else if (strcmp(text, "false"))
			err = -EINVAL;
		break;
	default:
		err = -EOPNOTSUPP;
	}
	if (err)
		return err;

	nlh = genl_request(buf, family, DEVLINK_CMD_PARAM_SET, 0, seq);
	put_handle(nlh, d);
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_PARAM_NAME, name);
	mnl_attr_put_u8(nlh, DEVLINK_ATTR_PARAM_TYPE, type);
	mnl_attr_put_u8(nlh, DEVLINK_ATTR_PARAM_VALUE_CMODE,
			DEVLINK_PARAM_CMODE_RUNTIME);
	switch (type) {
	case DEVLINK_VAR_ATTR_TYPE_U8:
		mnl_attr_put_u8(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA, (uint8_t)v);
		break;
	case DEVLINK_VAR_ATTR_TYPE_U16:
		mnl_attr_put_u16(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA, (uint16_t)v);
		break;
	case DEVLINK_VAR_ATTR_TYPE_U32:
		mnl_attr_put_u32(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA, (uint32_t)v);
		break;
	case DEVLINK_VAR_ATTR_TYPE_U64:
		mnl_attr_put_u64(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA, v);
		break;
	case DEVLINK_VAR_ATTR_TYPE_FLAG:
		if (v)
			mnl_attr_put(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA, 0, NULL);
		break;
	default:
		mnl_attr_put_strz(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA, text);
	}
	*out = nlh;
	return 0;
}

/* Copy a string attribute that ends with its NUL and fits @cap. */
static int string(const struct nlattr *a, char *to, size_t cap)
{
	const char *s = mnl_attr_get_payload(a);
	size_t n = mnl_attr_get_payload_len(a);
	size_t len = strnlen(s, n);

	if (len == n || len >= cap)
		return -EINVAL;
	memcpy(to, s, len + 1);
	return 0;
}

int dl_dev_parse(const struct nlmsghdr *nlh, struct dl_dev *d)
{
	const struct nlattr *a;
	bool bus = false, dev = false;

	mnl_attr_for_each(a, nlh, sizeof(struct genlmsghdr)) {
		if (mnl_attr_get_type(a) == DEVLINK_ATTR_BUS_NAME)
			bus = !string(a, d->bus, sizeof(d->bus));
		else if (mnl_attr_get_type(a) == DEVLINK_ATTR_DEV_NAME)
			dev = !string(a, d->dev, sizeof(d->dev));
	}
	return bus && dev ? 0 : -EINVAL;
}

/* The value of a list entry whose mode is runtime, or NULL. */
static const struct nlattr *runtime_value(const struct nlattr *list,
					  bool *found)
{
	const struct nlattr *value, *a;

	mnl_attr_for_each_nested(value, list) {
		const struct nlattr *data = NULL;
		bool runtime = false;

		if (mnl_attr_get_type(value) != DEVLINK_ATTR_PARAM_VALUE)
			continue;
		mnl_attr_for_each_nested(a, value) {
			if (mnl_attr_get_type(a) == DEVLINK_ATTR_PARAM_VALUE_CMODE &&
			    mnl_attr_get_payload_len(a) == 1)
				runtime = mnl_attr_get_u8(a) ==
					  DEVLINK_PARAM_CMODE_RUNTIME;
			else if (mnl_attr_get_type(a) == DEVLINK_ATTR_PARAM_VALUE_DATA)
				data = a;
		}
		if (runtime) {
			*found = true;
			return data;
		}
	}
	return NULL;
}

/* @data as text, in @p->type; NULL @data is a false flag. */
static int value_text(const struct nlattr *data, struct dl_param *p)
{
	static const uint16_t size[] = {
		[DEVLINK_VAR_ATTR_TYPE_U8] = 1,
		[DEVLINK_VAR_ATTR_TYPE_U16] = 2,
		[DEVLINK_VAR_ATTR_TYPE_U32] = 4,
		[DEVLINK_VAR_ATTR_TYPE_U64] = 8,
	};
	unsigned long long v;

	switch (p->type) {
	case DEVLINK_VAR_ATTR_TYPE_FLAG:
		snprintf(p->value, sizeof(p->value), "%s", data ? "true" : "false");
		return 0;
	case DEVLINK_VAR_ATTR_TYPE_STRING:
	case DEVLINK_VAR_ATTR_TYPE_NUL_STRING:
		return data ? string(data, p->value, sizeof(p->value)) : -EINVAL;
	case DEVLINK_VAR_ATTR_TYPE_U8:
	case DEVLINK_VAR_ATTR_TYPE_U16:
	case DEVLINK_VAR_ATTR_TYPE_U32:
	case DEVLINK_VAR_ATTR_TYPE_U64:
		break;
	default:
		return -EINVAL;
	}
	if (!data || mnl_attr_get_payload_len(data) != size[p->type])
		return -EINVAL;
	switch (p->type) {
	case DEVLINK_VAR_ATTR_TYPE_U8:
		v = mnl_attr_get_u8(data);
		break;
	case DEVLINK_VAR_ATTR_TYPE_U16:
		v = mnl_attr_get_u16(data);
		break;
	case DEVLINK_VAR_ATTR_TYPE_U32:
		v = mnl_attr_get_u32(data);
		break;
	default:
		v = mnl_attr_get_u64(data);
	}
	snprintf(p->value, sizeof(p->value), "%llu", v);
	return 0;
}

int dl_param_parse(const struct nlmsghdr *nlh, struct dl_param *p)
{
	const struct nlattr *a, *param = NULL, *list = NULL, *data = NULL;
	bool typed = false;

	memset(p, 0, sizeof(*p));
	mnl_attr_for_each(a, nlh, sizeof(struct genlmsghdr))
		if (mnl_attr_get_type(a) == DEVLINK_ATTR_PARAM)
			param = a;
	if (!param)
		return -EINVAL;
	mnl_attr_for_each_nested(a, param) {
		if (mnl_attr_get_type(a) == DEVLINK_ATTR_PARAM_TYPE &&
		    mnl_attr_get_payload_len(a) == 1) {
			p->type = mnl_attr_get_u8(a);
			typed = true;
		} else if (mnl_attr_get_type(a) == DEVLINK_ATTR_PARAM_VALUES_LIST) {
			list = a;
		}
	}
	if (!typed)
		return -EINVAL;
	if (list)
		data = runtime_value(list, &p->have);
	if (!p->have)
		return 0;
	return value_text(data, p);
}
