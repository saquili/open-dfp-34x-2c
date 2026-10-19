// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the firewall's rules as the switch's ACL (fwacl.h): rule sets
 * compiled to the tc commands that would add them, against the commands
 * written out here, and the requests that add an entry (aclmsg.h).
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/if_ether.h>
#include <linux/pkt_cls.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_gact.h>

#include "aclmsg.h"
#include "fwacl.h"

static int failures;
static int checks;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static struct acl_spec g_spec;
static struct acl_set g_set;
static char g_out[32768];

/* Take @lines, NULL at the end, and compile them with @room. */
static int compile(const char *const *lines, int room)
{
	char err[128];

	acl_spec_init(&g_spec);
	for (; *lines; lines++)
		if (acl_spec_line(&g_spec, *lines, err, sizeof(err))) {
			printf("  %s\n", err);
			return -1;
		}
	acl_compile(&g_spec, room, &g_set);
	return 0;
}

/* The commands of the set and the status of each rule, a line each. */
static const char *listing(void)
{
	size_t len = 0;

	g_out[0] = '\0';
	for (int i = 0; i < g_set.n; i++) {
		len += (size_t)acl_entry_format(&g_set.e[i], true, g_out + len, sizeof(g_out) - len);
		len += (size_t)snprintf(g_out + len, sizeof(g_out) - len, "\n");
	}
	for (int i = 0; i < g_set.nst; i++) {
		len += (size_t)acl_status_format(&g_set.st[i], g_out + len, sizeof(g_out) - len);
		len += (size_t)snprintf(g_out + len, sizeof(g_out) - len, "\n");
	}
	return g_out;
}

/* Whether the listing of @lines is @want, the lines of which end at NULL. */
static int golden(const char *const *lines, int room, const char *const *want)
{
	char expect[sizeof(g_out)];
	size_t len = 0;

	expect[0] = '\0';
	for (; *want; want++)
		len += (size_t)snprintf(expect + len, sizeof(expect) - len, "%s\n", *want);
	if (compile(lines, room))
		return 0;
	if (strcmp(listing(), expect)) {
		printf("  got:\n%s  want:\n%s", g_out, expect);
		return 0;
	}
	return 1;
}

#define SFP "tc filter add dev sfp ingress pref "
#define PON "tc filter add dev pon ingress pref "
#define IP " protocol ip flower skip_sw"
#define PPP " protocol ppp_ses flower skip_sw ppp_proto ip"

/*
 * The panel's stand-in's four rules (pond/web/src/mocks/handlers.ts), with
 * its three networks, its fixed addresses, its forwards in force and its
 * DMZ: Biuro may print in Dom, which takes a pass and a pass of the
 * printer's answers; no network sends mail out, one drop each, behind the
 * passes of the local destinations; the NAS by IPv6 is not the ACL's yet;
 * the camera's rule is off.
 */
static void test_mock(void)
{
	static const char *const in[] = {
		"lan 1 192.168.1.0/24 pppoe:4",
		"lan 2 192.168.2.0/24 ipoe:2",
		"lan 3 192.168.3.0/24 pppoe:4",
		"wan pppoe:4 ppp0 198.51.100.4",
		"wan ipoe:2 wan420 203.0.113.2",
		"dev 1 00:00:5e:00:53:16 192.168.1.113",
		"dev 1 00:00:5e:00:53:21 192.168.1.10",
		"dev 1 00:00:5e:00:53:19 192.168.1.119",
		"fwd wan420 203.0.113.2 tcp 443 192.168.2.107 443",
		"fwd ppp0 198.51.100.4 udp 3074 192.168.1.114 3074",
		"dmz wan420 203.0.113.2 192.168.2.101",
		"rule 1_allow_lan:2_dev:1:00:00:5e:00:53:16_tcp_9100_any_on",
		"rule 2_block_lans_internet_tcp_25_any_on",
		"rule 3_allow_internet_dev:1:00:00:5e:00:53:21_tcp_443_ipv6_on",
		"rule 4_block_dev:1:00:00:5e:00:53:19_internet_any__any_off",
		NULL,
	};
	static const char *const want[] = {
		SFP "400" IP " src_ip 192.168.2.0/24 dst_ip 192.168.1.113/32 ip_proto tcp dst_port 9100 action pass",
		SFP "401" IP " src_ip 192.168.1.113/32 dst_ip 192.168.2.0/24 ip_proto tcp src_port 9100 action pass",
		SFP "402" IP " dst_ip 192.168.1.0/24 action pass",
		SFP "403" IP " dst_ip 192.168.2.0/24 action pass",
		SFP "404" IP " dst_ip 192.168.3.0/24 action pass",
		SFP "405" IP " dst_ip 255.255.255.255/32 action pass",
		SFP "406" IP " dst_ip 224.0.0.0/4 ip_proto udp action pass",
		SFP "407" IP " src_ip 192.168.1.0/24 ip_proto tcp dst_port 25 action drop",
		SFP "408" IP " src_ip 192.168.2.0/24 ip_proto tcp dst_port 25 action drop",
		SFP "409" IP " src_ip 192.168.3.0/24 ip_proto tcp dst_port 25 action drop",
		"1 in 2",
		"2 in 3",
		"3 out 0 ipv6",
		"4 out 0 off",
		NULL,
	};

	CHECK(golden(in, ACL_ROOM, want));
	CHECK(g_set.n == 10 && g_set.passes == 5);
}

/* Three networks, a session and an IPoE connection, forwards and a DMZ. */
static const char *const g_nets[] = {
	"lan 1 192.168.1.0/24 pppoe:1",
	"lan 2 10.0.0.0/24 ipoe:1",
	"lan 3 172.16.0.0/16 pppoe:1",
	"wan pppoe:1 ppp0 198.51.100.4",
	"wan ipoe:1 wan420 203.0.113.2",
	"dev 1 00:00:5e:00:53:a1 192.168.1.10",
	"dev 2 00:00:5e:00:53:a2 10.0.0.20",
	"fwd ppp0 198.51.100.4 tcp 8080 192.168.1.10 80",
	"fwd ppp0 198.51.100.4 tcp 7000-7010 192.168.1.10 9000",
	"fwd wan420 203.0.113.2 tcp 1500 10.0.0.30 22",
	"dmz wan420 203.0.113.2 10.0.0.20",
};

#define NETS 11

static void with_nets(const char **lines, const char *const *rules)
{
	int n = 0;

	for (int i = 0; i < NETS; i++)
		lines[n++] = g_nets[i];
	while (*rules)
		lines[n++] = *rules++;
	lines[n] = NULL;
}

/*
 * A range of ports in masked prefixes; both protocols, and their answers by
 * the session the network goes out by; ICMP to a part of the internet; every
 * network to every other, a pair each; a network allowed to another, with
 * the answers; a part of the internet to a device through a forward moved
 * to another port, with the device's answers; a range through a range moved,
 * the rule's ports cut to what the forward takes; a range through the DMZ,
 * less the port a forward takes there; every protocol through the DMZ, an
 * echo request of it, its part of every port left out; a network no forward
 * reaches; a device to a part of the internet that holds a network, by its
 * MAC address; and a rule of IPv6.
 */
static void test_rules(void)
{
	static const char *const rules[] = {
		"rule 1_block_lans_internet_tcp_6000-6010_any_on",
		"rule 2_allow_lan:1_internet_both_53_ipv4_on",
		"rule 3_block_lan:2_internet:203.0.113.0/24_icmp__any_on",
		"rule 4_block_lans_lans_any__any_on",
		"rule 5_allow_lans_lan:2_tcp_22_any_on",
		"rule 6_allow_internet:198.51.100.0/24_dev:1:00:00:5e:00:53:a1_tcp_80_any_on",
		"rule 7_block_internet_lan:1_tcp_9005-9100_any_on",
		"rule 8_block_internet_dev:2:00:00:5e:00:53:a2_tcp_1000-2000_any_on",
		"rule 9_allow_internet_dev:2:00:00:5e:00:53:a2_any__any_on",
		"rule 10_block_internet_lan:3_any__any_on",
		"rule 11_allow_dev:1:00:00:5e:00:53:a1_internet:10.0.0.0/8_udp__any_on",
		"rule 12_block_lans_internet:2001:db8::/32_any__ipv6_on",
		NULL,
	};
	static const char *const want[] = {
		/* Between the networks: every network to every other, then 5 with its answers. */
		SFP "400" IP " src_ip 192.168.1.0/24 dst_ip 10.0.0.0/24 action drop",
		SFP "401" IP " src_ip 192.168.1.0/24 dst_ip 172.16.0.0/16 action drop",
		SFP "402" IP " src_ip 10.0.0.0/24 dst_ip 192.168.1.0/24 action drop",
		SFP "403" IP " src_ip 10.0.0.0/24 dst_ip 172.16.0.0/16 action drop",
		SFP "404" IP " src_ip 172.16.0.0/16 dst_ip 192.168.1.0/24 action drop",
		SFP "405" IP " src_ip 172.16.0.0/16 dst_ip 10.0.0.0/24 action drop",
		SFP "406" IP " src_ip 192.168.1.0/24 dst_ip 10.0.0.0/24 ip_proto tcp dst_port 22 action pass",
		SFP "407" IP " src_ip 172.16.0.0/16 dst_ip 10.0.0.0/24 ip_proto tcp dst_port 22 action pass",
		SFP "408" IP " src_ip 10.0.0.0/24 dst_ip 192.168.1.0/24 ip_proto tcp src_port 22 action pass",
		SFP "409" IP " src_ip 10.0.0.0/24 dst_ip 172.16.0.0/16 ip_proto tcp src_port 22 action pass",
		/* The local destinations. */
		SFP "410" IP " dst_ip 192.168.1.0/24 action pass",
		SFP "411" IP " dst_ip 10.0.0.0/24 action pass",
		SFP "412" IP " dst_ip 172.16.0.0/16 action pass",
		SFP "413" IP " dst_ip 255.255.255.255/32 action pass",
		SFP "414" IP " dst_ip 224.0.0.0/4 ip_proto udp action pass",
		/* To the internet, and the answers of the devices reached from it. */
		SFP "415" IP " src_ip 192.168.1.0/24 ip_proto tcp dst_port 6000/0xfff8 action drop",
		SFP "416" IP " src_ip 192.168.1.0/24 ip_proto tcp dst_port 6008/0xfffe action drop",
		SFP "417" IP " src_ip 192.168.1.0/24 ip_proto tcp dst_port 6010 action drop",
		SFP "418" IP " src_ip 10.0.0.0/24 ip_proto tcp dst_port 6000/0xfff8 action drop",
		SFP "419" IP " src_ip 10.0.0.0/24 ip_proto tcp dst_port 6008/0xfffe action drop",
		SFP "420" IP " src_ip 10.0.0.0/24 ip_proto tcp dst_port 6010 action drop",
		SFP "421" IP " src_ip 172.16.0.0/16 ip_proto tcp dst_port 6000/0xfff8 action drop",
		SFP "422" IP " src_ip 172.16.0.0/16 ip_proto tcp dst_port 6008/0xfffe action drop",
		SFP "423" IP " src_ip 172.16.0.0/16 ip_proto tcp dst_port 6010 action drop",
		SFP "424" IP " src_ip 192.168.1.0/24 ip_proto tcp dst_port 53 action pass",
		SFP "425" IP " src_ip 192.168.1.0/24 ip_proto udp dst_port 53 action pass",
		SFP "426" IP " src_ip 10.0.0.0/24 dst_ip 203.0.113.0/24 ip_proto icmp action drop",
		SFP "427" IP " src_ip 192.168.1.10/32 dst_ip 198.51.100.0/24 ip_proto tcp src_port 80 action pass",
		SFP "428" IP " src_ip 10.0.0.20/32 ip_proto icmp type 0 action pass",
		SFP "429" IP " src_mac 00:00:5e:00:53:a1 dst_ip 10.0.0.0/8 ip_proto udp action pass",
		/* From the internet: the answers of 2, inside the session. */
		PON "400" PPP " dst_ip 198.51.100.4/32 ip_proto tcp src_port 53 action pass",
		PON "401" PPP " dst_ip 198.51.100.4/32 ip_proto udp src_port 53 action pass",
		/* 6: the forward of 8080 to 80. */
		PON "402" PPP " src_ip 198.51.100.0/24 dst_ip 198.51.100.4/32 ip_proto tcp dst_port 8080 action pass",
		/* 7: 9005-9010 of the forward of 7000-7010 to 9000, moved back. */
		PON "403" PPP " dst_ip 198.51.100.4/32 ip_proto tcp dst_port 7005 action drop",
		PON "404" PPP " dst_ip 198.51.100.4/32 ip_proto tcp dst_port 7006/0xfffe action drop",
		PON "405" PPP " dst_ip 198.51.100.4/32 ip_proto tcp dst_port 7008/0xfffe action drop",
		PON "406" PPP " dst_ip 198.51.100.4/32 ip_proto tcp dst_port 7010 action drop",
		/* 8: 1000-1499 and 1501-2000, by the IPoE connection's DMZ. */
		PON "407" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1000/0xfff8 action drop",
		PON "408" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1008/0xfff0 action drop",
		PON "409" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1024/0xff00 action drop",
		PON "410" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1280/0xff80 action drop",
		PON "411" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1408/0xffc0 action drop",
		PON "412" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1472/0xfff0 action drop",
		PON "413" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1488/0xfff8 action drop",
		PON "414" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1496/0xfffc action drop",
		PON "415" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1501 action drop",
		PON "416" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1502/0xfffe action drop",
		PON "417" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1504/0xffe0 action drop",
		PON "418" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1536/0xff00 action drop",
		PON "419" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1792/0xff80 action drop",
		PON "420" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1920/0xffc0 action drop",
		PON "421" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 1984/0xfff0 action drop",
		PON "422" IP " dst_ip 203.0.113.2/32 ip_proto tcp dst_port 2000 action drop",
		/* 9: an echo request through the DMZ. */
		PON "423" IP " dst_ip 203.0.113.2/32 ip_proto icmp type 8 action pass",
		"1 in 9",
		"2 in 4",
		"3 in 1",
		"4 in 6",
		"5 in 4",
		"6 in 2",
		"7 in 4",
		"8 in 16",
		"9 part 2 dmz",
		"10 out 0 nothing",
		"11 in 1",
		"12 out 0 ipv6",
		NULL,
	};
	const char *in[NETS + 16];

	with_nets(in, rules);
	CHECK(golden(in, ACL_ROOM, want));
	CHECK(g_set.n == 54);
}

/*
 * The rules in their order up to the last that fits whole: the second
 * protocol of every port does not, and the small rule after it may count
 * on it, so it is left out too. An IPoE connection's answers are IPv4.
 */
static void test_room(void)
{
	static const char *const rules[] = {
		"rule 1_block_lans_internet_tcp_1-65535_any_on",
		"rule 2_allow_lan:2_internet_tcp_443_any_on",
		"rule 3_block_lans_internet_udp_1-65535_any_on",
		"rule 4_block_lan:1_lan:2_any__any_on",
		NULL,
	};
	const char *in[NETS + 8];

	with_nets(in, rules);
	CHECK(compile(in, ACL_ROOM) == 0);
	/* 16 prefixes for each of 3 networks, the 5 passes, 2 for rule 2. */
	CHECK(g_set.n == 48 + 5 + 2);
	CHECK(g_set.st[0].state == ACL_IN && g_set.st[0].entries == 48);
	CHECK(g_set.st[1].state == ACL_IN);
	CHECK(g_set.st[2].state == ACL_OUT && !strcmp(g_set.st[2].why, "room"));
	CHECK(g_set.st[3].state == ACL_OUT && !strcmp(g_set.st[3].why, "room"));
	CHECK(strstr(listing(), PON "400" IP " dst_ip 203.0.113.2/32 ip_proto tcp src_port 443 action pass\n"));
	CHECK(strstr(listing(), "\n3 out 0 room\n4 out 0 room\n"));
	/* With less room the first rule fits alone, and then none. */
	CHECK(compile(in, 53) == 0 && g_set.n == 53 && g_set.st[1].state == ACL_OUT);
	CHECK(compile(in, 52) == 0 && g_set.n == 0 && g_set.st[0].state == ACL_OUT);
	/* Every priority is the range's, one each on its port. */
	CHECK(compile(in, ACL_ROOM) == 0);
	for (int i = 0; i < g_set.n; i++)
		CHECK(g_set.e[i].pref >= ACL_PREF && g_set.e[i].pref < ACL_PREF + ACL_PREFS);
}

/* No rule, or none in force: nothing, not even the passes. */
static void test_empty(void)
{
	static const char *const rules[] = {
		"rule 1_block_lans_internet_tcp_25_any_off",
		NULL,
	};
	const char *in[NETS + 4];

	with_nets(in, rules);
	CHECK(compile(in, ACL_ROOM) == 0 && g_set.n == 0 && g_set.nst == 1);
	CHECK(!strcmp(listing(), "1 out 0 off\n"));
}

/* A rule to a device the ACL cannot name by an address: one with none fixed. */
static void test_unfixed(void)
{
	static const char *const rules[] = {
		/* The device's answers go to its network: it has no fixed address. */
		"rule 1_allow_dev:1:00:00:5e:00:53:a9_lan:2_udp_5000_any_on",
		"rule 2_block_lan:2_dev:1:00:00:5e:00:53:a9_any__any_on",
		NULL,
	};
	static const char *const want[] = {
		SFP "400" IP " src_mac 00:00:5e:00:53:a9 dst_ip 10.0.0.0/24 ip_proto udp dst_port 5000 action pass",
		SFP "401" IP " src_ip 10.0.0.0/24 dst_ip 192.168.1.0/24 ip_proto udp src_port 5000 action pass",
		"1 in 2",
		"2 out 0 nothing",
		NULL,
	};
	const char *in[NETS + 4];

	with_nets(in, rules);
	CHECK(golden(in, ACL_ROOM, want));
}

static void test_lines(void)
{
	static const char *const bad[] = {
		"lan 0 10.0.0.0/24",
		"lan 9 10.0.0.0/24",
		"lan 1 10.0.0.0/33",
		"wan pppoe:1 ppp0 198.51.100",
		"dev 1 aa:bb:cc:dd:ee 10.0.0.1",
		"fwd ppp0 198.51.100.4 sctp 80 10.0.0.1 80",
		"fwd ppp0 198.51.100.4 tcp 65535-65535 10.0.0.1 0",
		"fwd ppp0 198.51.100.4 tcp 100-200 10.0.0.1 65500",
		"rule 1_allow_lans_internet_tcp_443_any",
		"rule 97_allow_lans_internet_tcp_443_any_on",
		"rule 1_allow_lans_internet_icmp_443_any_on",
		"rule 1_allow_lan:9_internet_tcp_443_any_on",
		"rule 1_allow_lans_internet:example_tcp_443_any_on",
		"rule 1_pass_lans_internet_tcp_443_any_on",
		"rule 1_allow_lans_internet_tcp_9000-8000_any_on",
		"nothing",
	};
	char err[128];

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		acl_spec_init(&g_spec);
		CHECK(acl_spec_line(&g_spec, bad[i], err, sizeof(err)) == -1);
	}
	acl_spec_init(&g_spec);
	CHECK(acl_spec_line(&g_spec, "", err, sizeof(err)) == 0);
	CHECK(acl_spec_line(&g_spec, "# a note", err, sizeof(err)) == 0);
	/* A network may name bits past its length, which are cleared. */
	CHECK(acl_spec_line(&g_spec, "rule 1_block_lans_internet:10.1.2.3/8_any__any_on", err,
			    sizeof(err)) == 0);
	CHECK(g_spec.rule[0].to.net == 0x0a000000u && g_spec.rule[0].to.mask == 0xff000000u);
	/* A forward of a connection there is no line of stays out of the rules. */
	CHECK(acl_spec_line(&g_spec, "fwd ppp9 192.0.2.1 tcp 80 10.0.0.1 80", err,
			    sizeof(err)) == 0 && g_spec.fwd_wan[0] == -1);
}

/* The attributes of a request, by type, at the top of the filter's options. */
struct seen {
	const struct nlattr *a[TCA_FLOWER_MAX + 1];
};

static int opt_cb(const struct nlattr *a, void *data)
{
	struct seen *s = data;
	unsigned int type = mnl_attr_get_type(a);

	if (type <= TCA_FLOWER_MAX)
		s->a[type] = a;
	return MNL_CB_OK;
}

static int top_cb(const struct nlattr *a, void *data)
{
	if (mnl_attr_get_type(a) == TCA_OPTIONS)
		mnl_attr_parse_nested(a, opt_cb, data);
	return MNL_CB_OK;
}

/* The gact action of the filter's list: its verdict, or -1. */
static int gact_of(const struct nlattr *acts)
{
	const struct nlattr *act, *x, *y;

	mnl_attr_for_each_nested(act, acts) {
		mnl_attr_for_each_nested(x, act) {
			if (mnl_attr_get_type(x) != TCA_ACT_OPTIONS)
				continue;
			mnl_attr_for_each_nested(y, x)
				if (mnl_attr_get_type(y) == TCA_GACT_PARMS)
					return ((const struct tc_gact *)mnl_attr_get_payload(y))->action;
		}
	}
	return -1;
}

static void test_msg(void)
{
	static char buf[8192];
	struct acl_entry ppp = { .port = ACL_PON, .pref = 402, .pppoe = true, .sip = 0xc6336400u,
				 .smask = 0xffffff00u, .dip = 0xc6336404u, .dmask = 0xffffffffu,
				 .proto = 6, .dport = 7006, .dpmask = 0xfffe, .icmp_type = -1 };
	struct acl_entry mac = { .port = ACL_SFP, .pref = 429, .pass = true, .smac = true,
				 .mac = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01 }, .proto = 1,
				 .icmp_type = 8 };
	struct nlmsghdr *nlh;
	struct tcmsg *tcm;
	struct seen s;

	nlh = acl_entry_msg(buf, 7, &ppp, true, 42);
	tcm = mnl_nlmsg_get_payload(nlh);
	CHECK(nlh->nlmsg_type == RTM_NEWTFILTER && nlh->nlmsg_seq == 42);
	CHECK(nlh->nlmsg_flags == (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL));
	CHECK(tcm->tcm_ifindex == 7 && tcm->tcm_parent == TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS));
	CHECK(tcm->tcm_info == TC_H_MAKE(402u << 16, htons(ETH_P_PPP_SES)));
	memset(&s, 0, sizeof(s));
	mnl_attr_parse(nlh, sizeof(*tcm), top_cb, &s);
	CHECK(s.a[TCA_FLOWER_KEY_ETH_TYPE] &&
	      mnl_attr_get_u16(s.a[TCA_FLOWER_KEY_ETH_TYPE]) == htons(ETH_P_PPP_SES));
	CHECK(s.a[TCA_FLOWER_KEY_PPP_PROTO] &&
	      mnl_attr_get_u16(s.a[TCA_FLOWER_KEY_PPP_PROTO]) == htons(0x0021));
	CHECK(s.a[TCA_FLOWER_KEY_IP_PROTO] && mnl_attr_get_u8(s.a[TCA_FLOWER_KEY_IP_PROTO]) == 6);
	CHECK(s.a[TCA_FLOWER_KEY_IPV4_SRC] &&
	      mnl_attr_get_u32(s.a[TCA_FLOWER_KEY_IPV4_SRC]) == htonl(0xc6336400u) &&
	      mnl_attr_get_u32(s.a[TCA_FLOWER_KEY_IPV4_SRC_MASK]) == htonl(0xffffff00u));
	CHECK(s.a[TCA_FLOWER_KEY_IPV4_DST] &&
	      mnl_attr_get_u32(s.a[TCA_FLOWER_KEY_IPV4_DST_MASK]) == 0xffffffffu);
	CHECK(s.a[TCA_FLOWER_KEY_TCP_DST] &&
	      mnl_attr_get_u16(s.a[TCA_FLOWER_KEY_TCP_DST]) == htons(7006) &&
	      mnl_attr_get_u16(s.a[TCA_FLOWER_KEY_TCP_DST_MASK]) == htons(0xfffe));
	CHECK(!s.a[TCA_FLOWER_KEY_TCP_SRC] && !s.a[TCA_FLOWER_KEY_UDP_DST] &&
	      !s.a[TCA_FLOWER_KEY_ETH_SRC]);
	CHECK(s.a[TCA_FLOWER_FLAGS] &&
	      mnl_attr_get_u32(s.a[TCA_FLOWER_FLAGS]) == TCA_CLS_FLAGS_SKIP_SW);
	CHECK(s.a[TCA_FLOWER_ACT] && gact_of(s.a[TCA_FLOWER_ACT]) == TC_ACT_SHOT);

	nlh = acl_entry_msg(buf, 3, &mac, false, 43);
	tcm = mnl_nlmsg_get_payload(nlh);
	CHECK(tcm->tcm_info == TC_H_MAKE(429u << 16, htons(ETH_P_IP)));
	memset(&s, 0, sizeof(s));
	mnl_attr_parse(nlh, sizeof(*tcm), top_cb, &s);
	CHECK(s.a[TCA_FLOWER_KEY_ETH_SRC] &&
	      !memcmp(mnl_attr_get_payload(s.a[TCA_FLOWER_KEY_ETH_SRC]), mac.mac, 6));
	CHECK(s.a[TCA_FLOWER_KEY_ICMPV4_TYPE] &&
	      mnl_attr_get_u8(s.a[TCA_FLOWER_KEY_ICMPV4_TYPE]) == 8);
	CHECK(!s.a[TCA_FLOWER_KEY_PPP_PROTO] && !s.a[TCA_FLOWER_KEY_IPV4_SRC] &&
	      !s.a[TCA_FLOWER_KEY_IPV4_DST]);
	CHECK(mnl_attr_get_u32(s.a[TCA_FLOWER_FLAGS]) == 0);
	CHECK(gact_of(s.a[TCA_FLOWER_ACT]) == TC_ACT_OK);
}

int main(void)
{
	test_mock();
	test_rules();
	test_room();
	test_empty();
	test_unfixed();
	test_lines();
	test_msg();
	printf("test_fwacl: %d of %d\n", checks - failures, checks);
	return failures != 0;
}
