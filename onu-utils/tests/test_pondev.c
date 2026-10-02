// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of reading and printing the messages of the "pon" family. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>

#include <linux/if_link.h>
#include <linux/rtnetlink.h>

#include "gemlink.h"
#include "pondev.h"

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

static struct nlmsghdr *message(char *buf, uint8_t cmd)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct genlmsghdr *g = mnl_nlmsg_put_extra_header(nlh, sizeof(*g));

	g->cmd = cmd;
	g->version = 1;
	return nlh;
}

/* A device in O5 with every attribute, as the kernel sends it. */
static struct nlmsghdr *full_dev(char *buf)
{
	static const uint8_t serial[PON_SERIAL_LEN] = {
		'A', 'B', 'C', 'D', 0x12, 0x34, 0x56, 0x78
	};
	struct nlmsghdr *nlh = message(buf, PON_CMD_DEV_GET);
	struct nlattr *stats;

	mnl_attr_put_u32(nlh, PON_A_DEV_ID, 0);
	mnl_attr_put_u32(nlh, PON_A_DEV_IFINDEX, 4);
	mnl_attr_put_u32(nlh, PON_A_DEV_TYPE, PON_TYPE_GPON);
	mnl_attr_put_u32(nlh, PON_A_DEV_ONU_STATE, PON_ONU_STATE_O5);
	mnl_attr_put_u32(nlh, PON_A_DEV_ONU_ID, 3);
	mnl_attr_put_u32(nlh, PON_A_DEV_ALLOC_ID, 3);
	mnl_attr_put_u32(nlh, PON_A_DEV_OMCC_PORT_ID, 3);
	mnl_attr_put_u32(nlh, PON_A_DEV_ALARMS, 0);
	stats = mnl_attr_nest_start(nlh, PON_A_DEV_STATS);
	mnl_attr_put_u32(nlh, PON_A_DEV_STATS_PLOAM_RX, 120);
	mnl_attr_put_u64(nlh, PON_A_DEV_STATS_BWMAP_RX, 5000000000ull);
	mnl_attr_put_u32(nlh, PON_A_DEV_STATS_BIP_ERRORS, 0);
	mnl_attr_nest_end(nlh, stats);
	mnl_attr_put(nlh, PON_A_DEV_SERIAL_NUMBER, sizeof(serial), serial);
	mnl_attr_put_u32(nlh, PON_A_DEV_NUM_GEM_PORTS, 2);
	mnl_attr_put_u32(nlh, PON_A_DEV_NUM_TCONTS, 1);
	return nlh;
}

static char *printed(const struct pon_dev *d, bool serial)
{
	char *text = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&text, &len);

	pon_dev_print(f, d, "pon", serial);
	fclose(f);
	return text;
}

static void test_a_full_device_is_read(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct pon_dev d;

	CHECK(pon_dev_parse(full_dev(buf), &d) == 0);
	CHECK(d.state == PON_ONU_STATE_O5);
	CHECK(d.onu_id == 3 && d.alloc_id == 3 && d.omcc == 3);
	CHECK(d.ifindex == 4 && d.gem_ports == 2 && d.tconts == 1);
	CHECK(d.stats[PON_A_DEV_STATS_PLOAM_RX] == 120);
	CHECK(d.stats[PON_A_DEV_STATS_BWMAP_RX] == 5000000000ull);
	CHECK(d.stats_have & (1u << PON_A_DEV_STATS_BIP_ERRORS));
	CHECK(!(d.stats_have & (1u << PON_A_DEV_STATS_GEM_FRAMES)));
	CHECK(d.have & (1u << PON_A_DEV_SERIAL_NUMBER));
}

static void test_the_serial_number_is_printed_only_when_asked(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct pon_dev d;
	char *hidden, *shown;

	CHECK(pon_dev_parse(full_dev(buf), &d) == 0);
	hidden = printed(&d, false);
	shown = printed(&d, true);
	CHECK(strstr(hidden, "pon  GPON  O5 operation  ONU-ID 3  Alloc-ID 3"));
	CHECK(strstr(hidden, "alarms none  GEM ports up to 2  T-CONTs up to 1"));
	CHECK(strstr(hidden, "serial number set (-s shows it)"));
	CHECK(!strstr(hidden, "ABCD"));
	CHECK(strstr(shown, "serial number ABCD12345678"));
	CHECK(strstr(hidden, "bandwidth maps"));
	CHECK(strstr(hidden, "5000000000"));
	CHECK(!strstr(hidden, "GEM frames"));
	free(hidden);
	free(shown);
}

static void test_alarms_and_states_are_named(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = message(buf, PON_CMD_DEV_CHANGE_NTF);
	struct pon_dev d;
	char *text;

	mnl_attr_put_u32(nlh, PON_A_DEV_ONU_STATE, PON_ONU_STATE_O1);
	mnl_attr_put_u32(nlh, PON_A_DEV_ALARMS, PON_ALARM_LOS | PON_ALARM_LOF);
	CHECK(pon_dev_parse(nlh, &d) == 0);
	text = printed(&d, false);
	CHECK(strstr(text, "O1 initial"));
	CHECK(strstr(text, "alarms LOS LOF"));
	CHECK(strstr(text, "serial number not set"));
	CHECK(!strstr(text, "ONU-ID"));
	free(text);

	CHECK(!strcmp(pon_state_name(PON_ONU_STATE_O7), "O7 emergency stop"));
	CHECK(!strcmp(pon_state_name(0), "unknown"));
	CHECK(!strcmp(pon_state_name(99), "unknown"));
}

static void test_a_malformed_attribute_is_refused(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = message(buf, PON_CMD_DEV_GET);
	const uint8_t short_serial[4] = { 'A', 'B', 'C', 'D' };
	struct pon_dev d;

	mnl_attr_put_u16(nlh, PON_A_DEV_ONU_STATE, 5);
	CHECK(pon_dev_parse(nlh, &d) != 0);

	nlh = message(buf, PON_CMD_DEV_GET);
	mnl_attr_put(nlh, PON_A_DEV_SERIAL_NUMBER, sizeof(short_serial),
		     short_serial);
	CHECK(pon_dev_parse(nlh, &d) != 0);
}

static void test_a_tcont_is_read(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = message(buf, PON_CMD_TCONT_GET);
	struct pon_tcont t;

	mnl_attr_put_u32(nlh, PON_A_TCONT_ID, 0);
	mnl_attr_put_u32(nlh, PON_A_TCONT_INDEX, 1);
	mnl_attr_put_u32(nlh, PON_A_TCONT_ALLOC_ID, 1024);
	CHECK(pon_tcont_parse(nlh, &t) == 0);
	CHECK(t.dev == 0 && t.index == 1 && t.alloc_id == 1024);
}

static void test_an_idle_tcont_prints_as_idle(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct pon_tcont t;
	char *text = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&text, &len);

	nlh = message(buf, PON_CMD_TCONT_GET);
	mnl_attr_put_u32(nlh, PON_A_TCONT_ID, 0);
	mnl_attr_put_u32(nlh, PON_A_TCONT_INDEX, 0);
	mnl_attr_put_u32(nlh, PON_A_TCONT_ALLOC_ID, 0);
	CHECK(pon_tcont_parse(nlh, &t) == 0);
	pon_tcont_print(f, &t);

	nlh = message(buf, PON_CMD_TCONT_GET);
	mnl_attr_put_u32(nlh, PON_A_TCONT_ID, 0);
	mnl_attr_put_u32(nlh, PON_A_TCONT_INDEX, 2);
	CHECK(pon_tcont_parse(nlh, &t) == 0);
	pon_tcont_print(f, &t);
	fclose(f);

	CHECK(!strcmp(text, "    T-CONT 0  Alloc-ID 0\n    T-CONT 2  idle\n"));
	free(text);
}

static struct nlmsghdr *link_message(char *buf, const char *name,
				     const char *kind)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct ifinfomsg *ifm = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifm));
	struct nlattr *info;

	nlh->nlmsg_type = RTM_NEWLINK;
	ifm->ifi_family = AF_UNSPEC;
	mnl_attr_put_strz(nlh, IFLA_IFNAME, name);
	info = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
	mnl_attr_put_strz(nlh, IFLA_INFO_KIND, kind);
	mnl_attr_nest_end(nlh, info);
	return nlh;
}

static char *gem_printed(const struct gem_link *g)
{
	char *text = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&text, &len);

	gem_link_print(f, g);
	fclose(f);
	return text;
}

static void test_gem_links_are_read(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlattr *info, *data;
	struct nlmsghdr *nlh;
	struct gem_link g;
	char *text;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_NEWLINK;
	mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg));
	mnl_attr_put_strz(nlh, IFLA_IFNAME, "gem4095");
	info = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
	mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "gem");
	data = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
	mnl_attr_put_u16(nlh, IFLA_GEM_PORT_ID, 4095);
	mnl_attr_put_u8(nlh, IFLA_GEM_DIRECTION, IFLA_GEM_DIR_DOWNSTREAM);
	mnl_attr_put(nlh, IFLA_GEM_MULTICAST, 0, NULL);
	mnl_attr_nest_end(nlh, data);
	mnl_attr_nest_end(nlh, info);
	CHECK(gem_link_parse(nlh, &g) == 1);
	text = gem_printed(&g);
	CHECK(!strcmp(text, "gem4095 port 4095 down multicast\n"));
	free(text);

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_NEWLINK;
	mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg));
	mnl_attr_put_strz(nlh, IFLA_IFNAME, "gem256");
	info = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
	mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "gem");
	data = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
	mnl_attr_put_u16(nlh, IFLA_GEM_PORT_ID, 256);
	mnl_attr_put_u32(nlh, IFLA_GEM_TCONT, 0);
	mnl_attr_nest_end(nlh, data);
	mnl_attr_nest_end(nlh, info);
	CHECK(gem_link_parse(nlh, &g) == 1);
	text = gem_printed(&g);
	CHECK(!strcmp(text, "gem256 port 256 both tcont 0\n"));
	free(text);

	nlh = link_message(buf, "sfp", "dsa");
	CHECK(gem_link_parse(nlh, &g) == 0);

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_NEWLINK;
	mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg));
	mnl_attr_put_strz(nlh, IFLA_IFNAME, "gem1");
	info = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
	mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "gem");
	data = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
	mnl_attr_put_u32(nlh, IFLA_GEM_PORT_ID, 1);
	mnl_attr_nest_end(nlh, data);
	mnl_attr_nest_end(nlh, info);
	CHECK(gem_link_parse(nlh, &g) == -EINVAL);
}

int main(void)
{
	test_a_full_device_is_read();
	test_the_serial_number_is_printed_only_when_asked();
	test_alarms_and_states_are_named();
	test_a_malformed_attribute_is_refused();
	test_a_tcont_is_read();
	test_an_idle_tcont_prints_as_idle();
	test_gem_links_are_read();

	printf("test_pondev: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
