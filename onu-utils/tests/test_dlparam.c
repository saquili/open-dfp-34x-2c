// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the devlink parameter requests and of reading the kernel's
 * answers, which are built here as the kernel builds them.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/devlink.h>
#include <linux/genetlink.h>

#include "dlparam.h"

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

#define FAMILY 0x1d

static char buf[8192];
static const struct dl_dev SWITCH = { "platform", "1b000000.switch" };

static uint8_t cmd_of(const struct nlmsghdr *nlh)
{
	return ((const struct genlmsghdr *)mnl_nlmsg_get_payload(nlh))->cmd;
}

static const struct nlattr *attr(const struct nlmsghdr *nlh, uint16_t type)
{
	const struct nlattr *a;

	mnl_attr_for_each(a, nlh, sizeof(struct genlmsghdr))
		if (mnl_attr_get_type(a) == type)
			return a;
	return NULL;
}

static bool is_str(const struct nlattr *a, const char *s)
{
	return a && mnl_attr_validate(a, MNL_TYPE_NUL_STRING) == 0 &&
	       !strcmp(mnl_attr_get_str(a), s);
}

static void test_requests(void)
{
	struct nlmsghdr *nlh = dl_dev_dump_msg(buf, FAMILY, 1);

	CHECK(nlh->nlmsg_type == FAMILY);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_DUMP));
	CHECK(cmd_of(nlh) == DEVLINK_CMD_GET);

	nlh = dl_param_get_msg(buf, FAMILY, 2, &SWITCH, "sfp_lane");
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	CHECK(cmd_of(nlh) == DEVLINK_CMD_PARAM_GET);
	CHECK(is_str(attr(nlh, DEVLINK_ATTR_BUS_NAME), "platform"));
	CHECK(is_str(attr(nlh, DEVLINK_ATTR_DEV_NAME), "1b000000.switch"));
	CHECK(is_str(attr(nlh, DEVLINK_ATTR_PARAM_NAME), "sfp_lane"));
}

/* Set @text as @type: the error, and the request in @out. */
static int set(uint8_t type, const char *text, struct nlmsghdr **out)
{
	*out = NULL;
	return dl_param_set_msg(buf, FAMILY, 3, &SWITCH, "p", type, text, out);
}

static const struct nlattr *data_of(const struct nlmsghdr *nlh)
{
	return attr(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA);
}

static void test_set_numbers(void)
{
	struct nlmsghdr *nlh;

	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U32, "128", &nlh) == 0);
	CHECK(nlh && cmd_of(nlh) == DEVLINK_CMD_PARAM_SET);
	CHECK(nlh && nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK));
	CHECK(nlh && is_str(attr(nlh, DEVLINK_ATTR_BUS_NAME), "platform"));
	CHECK(nlh && is_str(attr(nlh, DEVLINK_ATTR_DEV_NAME), "1b000000.switch"));
	CHECK(nlh && is_str(attr(nlh, DEVLINK_ATTR_PARAM_NAME), "p"));
	CHECK(nlh && mnl_attr_get_u8(attr(nlh, DEVLINK_ATTR_PARAM_TYPE)) ==
			     DEVLINK_VAR_ATTR_TYPE_U32);
	CHECK(nlh && mnl_attr_get_u8(attr(nlh, DEVLINK_ATTR_PARAM_VALUE_CMODE)) ==
			     DEVLINK_PARAM_CMODE_RUNTIME);
	CHECK(nlh && mnl_attr_get_payload_len(data_of(nlh)) == 4 &&
	      mnl_attr_get_u32(data_of(nlh)) == 128);

	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U32, "0x1F", &nlh) == 0);
	CHECK(nlh && mnl_attr_get_u32(data_of(nlh)) == 31);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U32, "010", &nlh) == 0);
	CHECK(nlh && mnl_attr_get_u32(data_of(nlh)) == 10);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U32, "4294967295", &nlh) == 0);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U32, "4294967296", &nlh) == -ERANGE);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U8, "255", &nlh) == 0);
	CHECK(nlh && mnl_attr_get_payload_len(data_of(nlh)) == 1 &&
	      mnl_attr_get_u8(data_of(nlh)) == 255);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U8, "256", &nlh) == -ERANGE);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U16, "65535", &nlh) == 0);
	CHECK(nlh && mnl_attr_get_payload_len(data_of(nlh)) == 2);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U64, "18446744073709551615", &nlh) == 0);
	CHECK(nlh && mnl_attr_get_u64(data_of(nlh)) == UINT64_MAX);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_U64, "18446744073709551616", &nlh) == -ERANGE);

	for (const char *const *bad = (const char *const[]){
		     "", "-1", "+1", " 1", "1 ", "0x", "0x0x1", "1e3", "12a",
		     "0xg", NULL };
	     *bad; bad++) {
		CHECK(set(DEVLINK_VAR_ATTR_TYPE_U32, *bad, &nlh) == -EINVAL);
		CHECK(nlh == NULL);
	}
}

static void test_set_others(void)
{
	char text[DL_VALUE_MAX + 1];
	struct nlmsghdr *nlh;

	CHECK(set(DEVLINK_VAR_ATTR_TYPE_STRING, "2500basex", &nlh) == 0);
	CHECK(nlh && is_str(data_of(nlh), "2500basex"));
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_STRING, "", &nlh) == 0);
	CHECK(nlh && is_str(data_of(nlh), ""));
	memset(text, 'a', DL_VALUE_MAX);
	text[DL_VALUE_MAX] = '\0';
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_STRING, text, &nlh) == -ERANGE);
	text[DL_VALUE_MAX - 1] = '\0';
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_STRING, text, &nlh) == 0);

	CHECK(set(DEVLINK_VAR_ATTR_TYPE_FLAG, "true", &nlh) == 0);
	CHECK(nlh && data_of(nlh) && mnl_attr_get_payload_len(data_of(nlh)) == 0);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_FLAG, "false", &nlh) == 0);
	CHECK(nlh && !data_of(nlh));
	CHECK(nlh && attr(nlh, DEVLINK_ATTR_PARAM_TYPE));
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_FLAG, "1", &nlh) == -EINVAL);
	CHECK(set(DEVLINK_VAR_ATTR_TYPE_FLAG, "True", &nlh) == -EINVAL);

	CHECK(set(DEVLINK_VAR_ATTR_TYPE_BINARY, "00", &nlh) == -EOPNOTSUPP);
	CHECK(set(0, "1", &nlh) == -EOPNOTSUPP);
}

static struct nlmsghdr *answer(uint8_t cmd)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct genlmsghdr *g = mnl_nlmsg_put_extra_header(nlh, sizeof(*g));

	nlh->nlmsg_type = FAMILY;
	g->cmd = cmd;
	g->version = 1;
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_BUS_NAME, "platform");
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_DEV_NAME, "1b000000.switch");
	return nlh;
}

static void test_dev(void)
{
	struct nlmsghdr *nlh = answer(DEVLINK_CMD_NEW);
	struct dl_dev d;

	CHECK(dl_dev_parse(nlh, &d) == 0);
	CHECK(!strcmp(d.bus, "platform") && !strcmp(d.dev, "1b000000.switch"));

	nlh = mnl_nlmsg_put_header(buf);
	mnl_nlmsg_put_extra_header(nlh, sizeof(struct genlmsghdr));
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_BUS_NAME, "platform");
	CHECK(dl_dev_parse(nlh, &d) == -EINVAL);
	/* A name without its NUL. */
	mnl_attr_put(nlh, DEVLINK_ATTR_DEV_NAME, 3, "abc");
	CHECK(dl_dev_parse(nlh, &d) == -EINVAL);
}

/* A value of a parameter: its mode, and its data, NULL for none. */
struct value {
	uint8_t cmode;
	const void *data;
	uint16_t len;
};

/* A parameter, of type @type unless not @typed, with the values @v. */
static struct nlmsghdr *param_answer(bool typed, uint8_t type,
				     const struct value *v, size_t n)
{
	struct nlmsghdr *nlh = answer(DEVLINK_CMD_PARAM_NEW);
	struct nlattr *param, *list, *value;

	param = mnl_attr_nest_start(nlh, DEVLINK_ATTR_PARAM);
	mnl_attr_put_strz(nlh, DEVLINK_ATTR_PARAM_NAME, "p");
	if (typed)
		mnl_attr_put_u8(nlh, DEVLINK_ATTR_PARAM_TYPE, type);
	list = mnl_attr_nest_start(nlh, DEVLINK_ATTR_PARAM_VALUES_LIST);
	for (size_t i = 0; i < n; i++) {
		value = mnl_attr_nest_start(nlh, DEVLINK_ATTR_PARAM_VALUE);
		mnl_attr_put_u8(nlh, DEVLINK_ATTR_PARAM_VALUE_CMODE, v[i].cmode);
		if (v[i].data)
			mnl_attr_put(nlh, DEVLINK_ATTR_PARAM_VALUE_DATA,
				     v[i].len, v[i].data);
		mnl_attr_nest_end(nlh, value);
	}
	mnl_attr_nest_end(nlh, list);
	mnl_attr_nest_end(nlh, param);
	return nlh;
}

static void test_param_values(void)
{
	static const uint32_t five = 5, pps = 128;
	static const uint8_t u8 = 200;
	static const uint16_t u16 = 1500;
	static const uint64_t u64 = 1ull << 40;
	const struct value both[] = {
		{ DEVLINK_PARAM_CMODE_DRIVERINIT, &five, 4 },
		{ DEVLINK_PARAM_CMODE_RUNTIME, &pps, 4 },
	};
	struct dl_param p;

	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U32, both, 2), &p) == 0);
	CHECK(p.type == DEVLINK_VAR_ATTR_TYPE_U32 && p.have && !strcmp(p.value, "128"));

	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U8,
					  &(struct value){ 0, &u8, 1 }, 1), &p) == 0);
	CHECK(p.have && !strcmp(p.value, "200"));
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U16,
					  &(struct value){ 0, &u16, 2 }, 1), &p) == 0);
	CHECK(p.have && !strcmp(p.value, "1500"));
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U64,
					  &(struct value){ 0, &u64, 8 }, 1), &p) == 0);
	CHECK(p.have && !strcmp(p.value, "1099511627776"));

	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_STRING,
					  &(struct value){ 0, "auto", 5 }, 1), &p) == 0);
	CHECK(p.have && !strcmp(p.value, "auto"));

	/* A boolean is true with its flag, false without. */
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_FLAG,
					  &(struct value){ 0, "", 0 }, 1), &p) == 0);
	CHECK(p.have && !strcmp(p.value, "true"));
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_FLAG,
					  &(struct value){ 0, NULL, 0 }, 1), &p) == 0);
	CHECK(p.have && !strcmp(p.value, "false"));
}

static void test_param_answers_that_are_not_read(void)
{
	static const uint32_t pps = 128;
	struct dl_param p;
	struct nlmsghdr *nlh;

	/* No value in the runtime mode: read, and none had. */
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U32,
					  &(struct value){ 3, &pps, 4 }, 1), &p) == 0);
	CHECK(!p.have);
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U32, NULL, 0), &p) == 0);
	CHECK(!p.have);

	CHECK(dl_param_parse(param_answer(false, 0, &(struct value){ 0, &pps, 4 }, 1), &p) == -EINVAL);
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U32,
					  &(struct value){ 0, &pps, 2 }, 1), &p) == -EINVAL);
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_U32,
					  &(struct value){ 0, NULL, 0 }, 1), &p) == -EINVAL);
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_STRING,
					  &(struct value){ 0, "auto", 4 }, 1), &p) == -EINVAL);
	CHECK(dl_param_parse(param_answer(true, DEVLINK_VAR_ATTR_TYPE_BINARY,
					  &(struct value){ 0, "ab", 2 }, 1), &p) == -EINVAL);

	nlh = answer(DEVLINK_CMD_PARAM_NEW);
	CHECK(dl_param_parse(nlh, &p) == -EINVAL);
}

int main(void)
{
	test_requests();
	test_set_numbers();
	test_set_others();
	test_dev();
	test_param_values();
	test_param_answers_that_are_not_read();
	printf("test_dlparam: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
