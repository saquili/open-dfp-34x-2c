// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the settings of the data path (settings.h). */

#include <stdio.h>
#include <string.h>

#include "fwmsg.h"
#include "settings.h"

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

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static char g_err[256];
static char g_out[65536];

/* Take the lines of @text over the defaults, as onu-config check does: 0, or
 * -1 at the first line that is not taken, or -2 when the whole makes no
 * sense; the settings are in @s and their lines in g_out.
 */
static unsigned int g_mgmt[2];
static const char *g_mgmt_address[2] = { "", "" };

static int judge(struct settings *s, const char *text)
{
	static char copy[65536];
	char *line, *save = NULL;

	snprintf(copy, sizeof(copy), "%s", text);
	settings_defaults(s);
	g_err[0] = '\0';
	for (line = strtok_r(copy, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save))
		if (settings_line(s, line, g_err, sizeof(g_err)))
			return -1;
	settings_settle(s);
	s->mgmt_vlan[0] = g_mgmt[0];
	s->mgmt_vlan[1] = g_mgmt[1];
	for (int k = 0; k < 2; k++)
		snprintf(s->mgmt_address[k], sizeof(s->mgmt_address[k]), "%s",
			 g_mgmt_address[k]);
	if (!settings_coherent(s, g_err, sizeof(g_err)))
		return -2;
	g_out[0] = '\0';
	settings_format(s, g_out, sizeof(g_out));
	return 0;
}

static bool has(const char *line)
{
	char want[128];

	snprintf(want, sizeof(want), "%s\n", line);
	return !strncmp(g_out, want, strlen(want)) ||
	       (strstr(g_out, want) && strstr(g_out, want)[-1] == '\n');
}

/* The accounts of the first two sessions. */
#define ONE "PPPOE1_USER=a\nPPPOE1_SECRET=x\n"
#define TWO "PPPOE2_USER=b\nPPPOE2_SECRET=x\n"

static void test_defaults(void)
{
	struct settings s;

	/* Without an account there is nothing to write. */
	CHECK(judge(&s, "") == 0 && !*g_out);
	/* Remarks and empty lines are not settings. */
	CHECK(judge(&s, "# The settings of the data path, from pond.\n\n" ONE) == 0);
	CHECK(!strncmp(g_out, "SESSION1_GEM=auto\nSESSION1_VLAN=\n", 33));
}

static void test_values(void)
{
	static const char *const bad[] = {
		"FOO=1",
		"WAN_VLAN=0",
		"WAN_VLAN=4095",
		"WAN_GEM=4096",
		"WAN_GEM=012",
		"WAN_MAC=01:00:5e:00:00:01",
		"WAN_MAC=00:00:00:00:00:00",
		"WAN_MAC=00:00:5e:00:53",
		"WAN_MAC=00-00-5e-00-53-01",
		"SESSION1_PASS=yes",
		"SESSION1_HOST_VLAN=4095",
		"SESSION1_HOST_VLAN=none",
		"SESSION8_VLAN=410",
		"SESSION1_FAMILY=ipx",
		"PPPOE_USER=two words",
		"PPPOE_SECRET=",
		"PPPOE8_USER=bench",
		"PASS_GATEWAY=169.254.0.1",
		"LAN_VLAN=4095",
		"a line without an equals sign",
	};
	struct settings s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		int rc = judge(&s, bad[i]);

		CHECK(rc == -1 && *g_err);
		if (rc != -1)
			printf("    taken: %s\n", bad[i]);
	}
	CHECK(judge(&s, "PASS_GATEWAY=169.254.0.1") == -1 &&
	      !strcmp(g_err, "PASS_GATEWAY: no such setting"));
	CHECK(judge(&s, "WAN_VLAN=0") == -1 && !strcmp(g_err, "WAN_VLAN: not a value of it"));
	/* What is wrong names the key, never the value: a secret stays one. */
	CHECK(judge(&s, "PPPOE3_SECRET=xyzzy plugh") == -1 &&
	      strstr(g_err, "PPPOE3_SECRET") && !strstr(g_err, "xyzzy") &&
	      !strstr(g_err, "plugh"));
	CHECK(judge(&s, ONE "SESSION1_VLAN=410\r\nSESSION1_GEM=1024\r\n") == 0 &&
	      has("SESSION1_VLAN=410") && has("SESSION1_GEM=1024"));
	/* A later line over an earlier one. */
	CHECK(judge(&s, ONE "SESSION1_VLAN=410\nSESSION1_VLAN=36\n") == 0 &&
	      has("SESSION1_VLAN=36"));
}

/* The bench's router of before is gone: its keys are taken and left out. */
static void test_gone(void)
{
	struct settings s;

	CHECK(judge(&s, ONE "MODE=router\nWAN_PROTO=pppoe\nWAN_ADDRESS=x\nWAN_GATEWAY=\n"
			"LAN_ADDRESS=192.0.2.1/24\nLAN_DHCP=on\nLAN_DHCP_START=a\n"
			"LAN_DHCP_END=b\n") == 0);
	CHECK(!strstr(g_out, "MODE") && !strstr(g_out, "WAN_") && !strstr(g_out, "LAN_"));
	CHECK(has("SESSION1_GEM=auto"));
}

static void test_accounts(void)
{
	struct settings s;

	CHECK(judge(&s, "PPPOE2_USER=bench\n") == -2 && strstr(g_err, "PPPOE2_SECRET"));
	CHECK(judge(&s, "PPPOE2_SECRET=not-a-real-one\n") == -2);
	CHECK(judge(&s, "PPPOE2_USER=bench\nPPPOE2_SECRET=not-a-real-one\n") == 0);
	/* The session's settings are written, its account never. */
	CHECK(has("SESSION2_GEM=auto") && has("SESSION2_FAMILY=both") &&
	      has("SESSION2_PASS=on") && has("SESSION2_HOST_VLAN="));
	CHECK(!strstr(g_out, "bench") && !strstr(g_out, "not-a-real-one") &&
	      !strstr(g_out, "PPPOE"));
	CHECK(!strstr(g_out, "SESSION1_") && !strstr(g_out, "SESSION3_"));
	/* An empty user removes the account, with its secret. */
	CHECK(judge(&s, "PPPOE2_USER=bench\nPPPOE2_SECRET=not-a-real-one\nPPPOE2_USER=\n") == 0 &&
	      !strstr(g_out, "SESSION2_"));
	/* The single account of before is the first session's. */
	CHECK(judge(&s, "PPPOE_USER=bench\nPPPOE_SECRET=not-a-real-one\n") == 0 &&
	      has("SESSION1_GEM=auto"));
}


static void test_sessions(void)
{
	struct settings s;

	/* One VLAN, one interface, one MAC address. */
	CHECK(judge(&s, ONE TWO "SESSION1_VLAN=410\nSESSION2_VLAN=410\n"
			"SESSION1_MAC=00:00:5e:00:53:01\nSESSION2_MAC=00:00:5e:00:53:02\n"
			"SESSION2_PASS=off\n") == -2 &&
	      strstr(g_err, "one MAC address"));
	CHECK(judge(&s, ONE TWO "SESSION1_VLAN=410\nSESSION2_VLAN=36\n"
			"SESSION1_MAC=00:00:5e:00:53:01\nSESSION2_MAC=00:00:5e:00:53:02\n") == 0);
	/* In one VLAN to the host one session hands IPv4 down, and one IPv6. */
	CHECK(judge(&s, ONE TWO "SESSION1_VLAN=410\nSESSION2_VLAN=36\n"
			"SESSION1_HOST_VLAN=410\nSESSION2_HOST_VLAN=410\n") == -2 &&
	      strstr(g_err, "an IPv4 address"));
	CHECK(judge(&s, ONE TWO "SESSION1_VLAN=410\nSESSION2_VLAN=36\n"
			"SESSION1_HOST_VLAN=410\nSESSION2_HOST_VLAN=410\n"
			"SESSION1_FAMILY=ipv4\nSESSION2_FAMILY=ipv6\n") == 0);
	CHECK(judge(&s, ONE TWO "SESSION1_VLAN=410\nSESSION2_VLAN=36\n"
			"SESSION1_HOST_VLAN=untagged\nSESSION2_HOST_VLAN=untagged\n"
			"SESSION1_FAMILY=ipv6\nSESSION2_FAMILY=both\n") == -2 &&
	      strstr(g_err, "an IPv6 prefix"));
	/* A session that keeps its address hands nothing down. */
	CHECK(judge(&s, ONE TWO "SESSION1_VLAN=410\nSESSION2_VLAN=36\n"
			"SESSION1_HOST_VLAN=410\nSESSION2_HOST_VLAN=410\nSESSION2_PASS=off\n") == 0);
}

static void test_mgmt(void)
{
	struct settings s;

	/* Nothing of the data path in a management VLAN, on either side. */
	g_mgmt[0] = 4094;
	g_mgmt[1] = 100;
	CHECK(judge(&s, ONE "SESSION1_VLAN=410\n") == 0);
	CHECK(judge(&s, ONE "SESSION1_VLAN=4094\n") == -2 &&
	      strstr(g_err, "session 1: VLAN 4094 is the management VLAN"));
	CHECK(judge(&s, ONE "SESSION1_VLAN=410\nSESSION1_HOST_VLAN=100\n") == -2 &&
	      strstr(g_err, "VLAN 100 is the management VLAN"));
	CHECK(judge(&s, ONE "SESSION1_VLAN=100\nSESSION1_HOST_VLAN=untagged\n") == -2);
	CHECK(judge(&s, ONE "SESSION1_VLAN=410\nSESSION1_HOST_VLAN=untagged\n") == 0);
	/* A session without an account is no session. */
	CHECK(judge(&s, "SESSION3_VLAN=4094\n") == 0);
	g_mgmt[0] = g_mgmt[1] = 0;
	CHECK(judge(&s, ONE "SESSION1_VLAN=4094\n") == 0);
}

static void test_before(void)
{
	struct settings s;

	/* The single session of before took the WAN's settings. */
	CHECK(judge(&s, ONE "MODE=passthrough\nWAN_VLAN=410\nWAN_MAC=00:00:5e:00:53:01\n"
			"WAN_GEM=1024\n") == 0);
	CHECK(has("SESSION1_VLAN=410") && has("SESSION1_MAC=00:00:5e:00:53:01") &&
	      has("SESSION1_GEM=1024"));
	/* The keys of before are never written. */
	CHECK(!strstr(g_out, "WAN_") && !strstr(g_out, "MODE"));
	/* Its own win. */
	CHECK(judge(&s, ONE "WAN_VLAN=410\nSESSION1_VLAN=36\n") == 0 && has("SESSION1_VLAN=36"));
	/* The one link of before is the link of each session naming none. */
	CHECK(judge(&s, ONE TWO "LAN_VLAN=36\nSESSION2_HOST_VLAN=37\nSESSION2_VLAN=40\n") == 0 &&
	      has("SESSION1_HOST_VLAN=36") && has("SESSION2_HOST_VLAN=37") &&
	      !strstr(g_out, "LAN_VLAN"));
	CHECK(judge(&s, ONE "LAN_VLAN=\n") == 0 && has("SESSION1_HOST_VLAN=untagged"));
	/* What is settled stays so when it is read again. */
	CHECK(judge(&s, ONE "MODE=passthrough\nWAN_VLAN=410\nLAN_VLAN=36\n") == 0);
	{
		char again[sizeof(g_out) + 64];

		snprintf(again, sizeof(again), "%s%s", ONE, g_out);
		CHECK(judge(&s, again) == 0 && has("SESSION1_VLAN=410") &&
		      has("SESSION1_HOST_VLAN=36") && !strstr(g_out, "WAN_"));
	}
}

static void test_format(void)
{
	struct settings s;
	char small[16];
	size_t need;

	CHECK(judge(&s, ONE) == 0);
	need = settings_format(&s, small, sizeof(small));
	CHECK(need == strlen(g_out) && need >= sizeof(small));
	CHECK(strlen(small) == sizeof(small) - 1 && !strncmp(small, g_out, sizeof(small) - 1));
}

static void test_defaults_format(void)
{
	char out[4096], small[16], line[128], err[160];
	struct settings s;
	size_t need = settings_format_defaults(out, sizeof(out));
	char *save = NULL, *l, copy[4096];
	int lines = 0, bad = 0;

	CHECK(need == strlen(out) && need < sizeof(out));
	CHECK(strstr(out, "WAN_GEM=auto\n") && strstr(out, "WAN_VLAN=\n"));
	CHECK(strstr(out, "SESSION1_GEM=auto\n") && strstr(out, "SESSION3_FAMILY=both\n"));
	CHECK(strstr(out, "SESSION7_PASS=on\n") && strstr(out, "SESSION2_VLAN=\n"));
	CHECK(strstr(out, "SESSION7_HOST_VLAN=\n"));
	/* Every line is a setting the table takes, and the table has them all. */
	settings_defaults(&s);
	snprintf(copy, sizeof(copy), "%s", out);
	for (l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		snprintf(line, sizeof(line), "%s", l);
		lines++;
		if (settings_line(&s, line, err, sizeof(err)))
			bad++;
	}
	CHECK(bad == 0);
	CHECK(lines == SET_WAN_MAC + 1 + SETTINGS_SESSIONS * SETTINGS_NSKEYS + SETTINGS_NFKEYS + 1);
	CHECK(strstr(out, "FW_OUT=allow\n") && strstr(out, "FW_BETWEEN=block\n"));
	/* The stick answers a ping from neither side unless told to. */
	CHECK(strstr(out, "FW_PING_IN=block\n") && strstr(out, "FW_PING_LANS=block\n"));
	/* omcid gives IGMP a priority the operator's rules take unless told not to. */
	CHECK(strstr(out, "IGMP_PRIORITY=auto\n") != NULL);
	/* The values given are the ones the table starts from. */
	CHECK(strstr(out, "WAN_MAC=\n") != NULL);
	need = settings_format_defaults(small, sizeof(small));
	CHECK(need == strlen(out) && strlen(small) == sizeof(small) - 1);
}

/* Session 1 keeps its address, and network 1 goes out by it. */
#define KEEPS ONE "SESSION1_PASS=off\n"
#define NET1 "LAN1_CONN=pppoe:1\nLAN1_VLAN=100\nLAN1_ADDRESS=192.168.1.1/24\n"

static void test_lans(void)
{
	static const char *const bad[] = {
		"LAN9_CONN=pppoe:1",
		"LAN0_CONN=pppoe:1",
		"LAN1_CONN=pppoe:8",
		"LAN1_CONN=ipoe:8",
		"LAN1_CONN=ipoe:0",
		"LAN1_CONN=ipoe:",
		"LAN1_FOO=1",
		"LAN1_ADDRESS=192.168.1.0/24",
		"LAN1_ADDRESS=192.168.1.255/24",
		"LAN1_ADDRESS=192.168.1.1/31",
		"LAN1_ADDRESS=192.168.1.1/32",
		"LAN1_ADDRESS=192.168.1.1/7",
		"LAN1_ADDRESS=192.168.1.1",
		"LAN1_ADDRESS=192.168.01.1/24",
		"LAN1_ADDRESS=256.168.1.1/24",
		"LAN1_VLAN=4095",
		"LAN1_DHCP=yes",
		"LAN1_POOL_FROM=192.168.1",
		"LAN1_LEASE=59",
		"LAN1_LEASE=604801",
		"LAN1_LEASE=0600",
		"LAN1_IPV6_SUBNET=g",
		"LAN1_IPV6_SUBNET=10000",
		"LAN1_NAME=a\tb",
		"LAN1_NAME=123456789012345678901234567890123",
	};
	struct settings s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		int rc = judge(&s, bad[i]);

		CHECK(rc == -1 && *g_err);
		if (rc != -1)
			printf("    taken: %s\n", bad[i]);
	}

	/* A network is written whole, with the defaults filled in. */
	CHECK(judge(&s, KEEPS NET1 "LAN1_NAME=Biuro \xc5\xbcó\xc5\x82te\n") == 0);
	CHECK(has("LAN1_CONN=pppoe:1") && has("LAN1_VLAN=100") &&
	      has("LAN1_ADDRESS=192.168.1.1/24") && has("LAN1_DHCP=on") &&
	      has("LAN1_LEASE=86400") && has("LAN1_IPV6=off") &&
	      has("LAN1_NAME=Biuro \xc5\xbcó\xc5\x82te"));
	/* 32 characters of a name, each of four bytes. */
	{
		char text[320];
		size_t at;

		at = (size_t)snprintf(text, sizeof(text), "%s", KEEPS NET1 "LAN1_NAME=");
		for (int i = 0; i < 32; i++, at += 4)
			memcpy(text + at, "\xf0\x9f\x8f\xa0", 4);
		memcpy(text + at, "\n", 2);
		CHECK(judge(&s, text) == 0);
	}
	/* Without a connection there is no network to write. */
	CHECK(judge(&s, KEEPS "LAN2_VLAN=200\n") == 0 && !strstr(g_out, "LAN2_"));
	/* Untagged on the host link. */
	CHECK(judge(&s, KEEPS "LAN1_CONN=pppoe:1\nLAN1_ADDRESS=10.0.0.1/8\n") == 0 &&
	      has("LAN1_VLAN="));

	/* What a network needs, and what keeps two apart. */
	CHECK(judge(&s, KEEPS "LAN1_CONN=pppoe:1\n") == -2 &&
	      strstr(g_err, "LAN1_ADDRESS"));
	CHECK(judge(&s, ONE NET1) == -2 && strstr(g_err, "hands its address down"));
	CHECK(judge(&s, "SESSION1_PASS=off\n" NET1) == -2 &&
	      strstr(g_err, "no account"));
	CHECK(judge(&s, KEEPS TWO "SESSION2_HOST_VLAN=100\n" NET1) == -2 &&
	      strstr(g_err, "session 2 hands its address down in 100"));
	CHECK(judge(&s, KEEPS TWO NET1 "LAN2_CONN=pppoe:1\nLAN2_ADDRESS=10.0.0.1/24\n"
			"SESSION2_HOST_VLAN=untagged\n") == -2 &&
	      strstr(g_err, "untagged link"));
	CHECK(judge(&s, KEEPS NET1 "LAN2_CONN=pppoe:1\nLAN2_VLAN=100\n"
			"LAN2_ADDRESS=10.0.0.1/24\n") == -2 &&
	      strstr(g_err, "networks 1 and 2 are in one VLAN"));
	CHECK(judge(&s, KEEPS NET1 "LAN2_CONN=pppoe:1\nLAN2_VLAN=200\n"
			"LAN2_ADDRESS=192.168.0.1/16\n") == -2 &&
	      strstr(g_err, "networks 1 and 2 share addresses"));
	g_mgmt[0] = 100;
	CHECK(judge(&s, KEEPS NET1) == -2 && strstr(g_err, "management VLAN"));
	g_mgmt[0] = 0;
	/* Nor may a network share addresses with a management pair's, the one in
	 * use or the one on trial, whose VLAN is another.
	 */
	g_mgmt_address[0] = "192.168.1.1/24";
	CHECK(judge(&s, KEEPS NET1) == -2 &&
	      strstr(g_err, "network 1 shares addresses with the management address 192.168.1.1/24"));
	g_mgmt_address[0] = "198.51.100.2/30";
	g_mgmt_address[1] = "192.168.0.1/16";
	CHECK(judge(&s, KEEPS NET1) == -2 && strstr(g_err, "192.168.0.1/16"));
	g_mgmt_address[1] = "";
	CHECK(judge(&s, KEEPS NET1) == 0);
	g_mgmt_address[0] = "";

	/* The pool: a range of the network's addresses without the stick's. */
	CHECK(judge(&s, KEEPS NET1 "LAN1_POOL_FROM=192.168.1.100\n"
			"LAN1_POOL_TO=192.168.1.199\n") == 0 &&
	      has("LAN1_POOL_FROM=192.168.1.100"));
	CHECK(judge(&s, KEEPS NET1 "LAN1_POOL_FROM=192.168.1.100\n") == -2 &&
	      strstr(g_err, "pool"));
	CHECK(judge(&s, KEEPS NET1 "LAN1_POOL_FROM=192.168.1.100\n"
			"LAN1_POOL_TO=192.168.1.50\n") == -2);
	CHECK(judge(&s, KEEPS NET1 "LAN1_POOL_FROM=192.168.1.100\n"
			"LAN1_POOL_TO=192.168.2.50\n") == -2);
	CHECK(judge(&s, KEEPS NET1 "LAN1_POOL_FROM=192.168.1.0\n"
			"LAN1_POOL_TO=192.168.1.50\n") == -2);
	CHECK(judge(&s, KEEPS NET1 "LAN1_POOL_FROM=192.168.1.100\n"
			"LAN1_POOL_TO=192.168.1.255\n") == -2);
	CHECK(judge(&s, KEEPS NET1 "LAN1_POOL_FROM=192.168.1.1\n"
			"LAN1_POOL_TO=192.168.1.50\n") == -2);
}

#define DEV "00:00:5e:00:53:a1"

static void test_fixed(void)
{
	static const char *const bad[] = {
		"FIXED0=1/" DEV "/192.168.1.10",
		"FIXED513=1/" DEV "/192.168.1.10",
		"FIXED01=1/" DEV "/192.168.1.10",
		"FIXED1_FOO=x",
		"FIXED1=9/" DEV "/192.168.1.10",
		"FIXED1=1/" DEV,
		"FIXED1=1/01:00:5e:00:00:01/192.168.1.10",
		"FIXED1=1-" DEV "-192.168.1.10",
		"FIXED1=1/" DEV "/192.168.1.300",
		"FIXED1_NAME=a\nb",
	};
	struct settings s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		int rc = judge(&s, bad[i]);

		CHECK(rc == -1 && *g_err);
		if (rc != -1)
			printf("    taken: %s\n", bad[i]);
	}
	CHECK(judge(&s, KEEPS NET1 "FIXED512=1/" DEV "/192.168.1.10\n"
			"FIXED512_NAME=Drukarka\n") == 0 &&
	      has("FIXED512=1/" DEV "/192.168.1.10") && has("FIXED512_NAME=Drukarka"));
	/* An empty one is none, its name gone with it. */
	CHECK(judge(&s, KEEPS NET1 "FIXED3=1/" DEV "/192.168.1.10\nFIXED3_NAME=x\n"
			"FIXED3=\n") == 0 && !strstr(g_out, "FIXED3"));
	CHECK(judge(&s, KEEPS "FIXED1=1/" DEV "/192.168.1.10\n") == -2 &&
	      strstr(g_err, "network 1 is not set up"));
	CHECK(judge(&s, KEEPS NET1 "FIXED1=1/" DEV "/192.168.2.10\n") == -2 &&
	      strstr(g_err, "not an address of network 1"));
	CHECK(judge(&s, KEEPS NET1 "FIXED1=1/" DEV "/192.168.1.1\n") == -2);
	CHECK(judge(&s, KEEPS NET1 "FIXED1=1/" DEV "/192.168.1.255\n") == -2);
	CHECK(judge(&s, KEEPS NET1 "FIXED1=1/" DEV "/192.168.1.10\n"
			"FIXED2=1/00:00:5E:00:53:A1/192.168.1.11\n") == -2 &&
	      strstr(g_err, "of one device"));
	CHECK(judge(&s, KEEPS NET1 "FIXED1=1/" DEV "/192.168.1.10\n"
			"FIXED2=1/00:00:5e:00:53:a2/192.168.1.10\n") == -2 &&
	      strstr(g_err, "give one address"));
}

/* A static connection in VLAN 420, and one by DHCP in 421. */
#define STATIC1 "IPOE1_KIND=static\nIPOE1_VLAN=420\nIPOE1_ADDRESS=192.0.2.10/24\n" \
		"IPOE1_GATEWAY=192.0.2.1\n"
#define DHCP2 "IPOE2_KIND=dhcp\nIPOE2_VLAN=421\n"
/* A static connection with an address alone and a gateway outside it. */
#define STATIC32 "IPOE1_KIND=static\nIPOE1_VLAN=420\nIPOE1_ADDRESS=203.0.113.7/32\n" \
		 "IPOE1_GATEWAY=198.51.100.1\n"

static void test_ipoe(void)
{
	static const char *const bad[] = {
		"IPOE0_KIND=static",
		"IPOE8_KIND=static",
		"IPOE1_KIND=pppoe",
		"IPOE1_FOO=1",
		"IPOE1_VLAN=4095",
		"IPOE1_GEM=auto1",
		"IPOE1_FAMILY=ipx",
		"IPOE1_MAC=01:00:5e:00:00:01",
		"IPOE1_ADDRESS=192.0.2.0/24",
		"IPOE1_ADDRESS=192.0.2.3/30",
		"IPOE1_ADDRESS=192.0.2.10/33",
		"IPOE1_ADDRESS=10.0.0.1/7",
		"IPOE1_ADDRESS=192.0.2.10",
		"IPOE1_GATEWAY=192.0.2",
		"IPOE1_DNS=198.51.100.53,198.51.100.54",
		"IPOE1_DNS=198.51.100.53  198.51.100.54",
		"IPOE1_DNS= 198.51.100.53",
		"IPOE1_DNS=198.51.100.53 ",
		"IPOE1_DNS=example.net",
		"IPOE1_ADDRESS6=2001:db8::2",
		"IPOE1_ADDRESS6=2001:db8::2/129",
		"IPOE1_ADDRESS6=2001:db8::g/64",
		"IPOE1_GATEWAY6=2001:db8::1/64",
		"IPOE1_GATEWAY6=fe80::1%wan420",
		"IPOE1_PREFIX6=2001:db8:5100::1/56",
		"IPOE1_PREFIX6=2001:db8::/65",
		"IPOE1_PREFIX6=2001:db8::/8",
	};
	struct settings s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		int rc = judge(&s, bad[i]);

		CHECK(rc == -1 && *g_err);
		if (rc != -1)
			printf("    taken: %s\n", bad[i]);
	}

	/* A connection is written whole, with the defaults filled in. */
	CHECK(judge(&s, STATIC1 "IPOE1_DNS=198.51.100.53 2001:db8::53\n") == 0);
	CHECK(has("IPOE1_KIND=static") && has("IPOE1_GEM=auto") && has("IPOE1_VLAN=420") &&
	      has("IPOE1_MAC=") && has("IPOE1_FAMILY=ipv4") &&
	      has("IPOE1_ADDRESS=192.0.2.10/24") && has("IPOE1_GATEWAY=192.0.2.1") &&
	      has("IPOE1_DNS=198.51.100.53 2001:db8::53") && has("IPOE1_ADDRESS6=") &&
	      has("IPOE1_GATEWAY6=") && has("IPOE1_PREFIX6="));
	CHECK(judge(&s, DHCP2) == 0 && has("IPOE2_KIND=dhcp") && !strstr(g_out, "IPOE1_"));
	/* An empty kind is no connection, and nothing of it is written. */
	CHECK(judge(&s, STATIC1 "IPOE1_KIND=\n") == 0 && !strstr(g_out, "IPOE1_"));
	/* Both families, IPv6 typed in too, a link-local gateway among them. */
	CHECK(judge(&s, STATIC1 "IPOE1_FAMILY=both\nIPOE1_ADDRESS6=2001:db8:5::2/64\n"
			"IPOE1_GATEWAY6=fe80::1\nIPOE1_PREFIX6=2001:db8:5100::/56\n") == 0 &&
	      has("IPOE1_PREFIX6=2001:db8:5100::/56") && has("IPOE1_GATEWAY6=fe80::1"));

	/* What a connection needs. */
	CHECK(judge(&s, "IPOE1_KIND=dhcp\n") == -2 && strstr(g_err, "IPOE1_VLAN"));
	CHECK(judge(&s, "IPOE1_KIND=static\nIPOE1_VLAN=420\n") == -2 &&
	      strstr(g_err, "IPOE1_ADDRESS and IPOE1_GATEWAY"));
	CHECK(judge(&s, STATIC1 "IPOE1_GATEWAY=192.0.3.1\n") == -2 &&
	      strstr(g_err, "the gateway is no other address of its network"));
	CHECK(judge(&s, STATIC1 "IPOE1_GATEWAY=192.0.2.10\n") == -2);
	CHECK(judge(&s, STATIC1 "IPOE1_GATEWAY=192.0.2.255\n") == -2);
	/* Up to a /30 the gateway is in the network, from /8 on. */
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=192.0.2.9/30\nIPOE1_GATEWAY=192.0.2.13\n") == -2 &&
	      strstr(g_err, "the gateway is no other address of its network"));
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=10.1.2.3/8\nIPOE1_GATEWAY=11.0.0.1\n") == -2 &&
	      strstr(g_err, "the gateway is no other address of its network"));
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=10.1.2.3/8\nIPOE1_GATEWAY=10.0.0.1\n") == 0);

	/* An address alone, its gateway outside it, on the link. */
	CHECK(judge(&s, STATIC32) == 0 && has("IPOE1_ADDRESS=203.0.113.7/32") &&
	      has("IPOE1_GATEWAY=198.51.100.1"));
	/* Both addresses of a /31 are hosts', the gateway the other or outside it. */
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=192.0.2.10/31\nIPOE1_GATEWAY=192.0.2.11\n") == 0 &&
	      has("IPOE1_ADDRESS=192.0.2.10/31"));
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=192.0.2.11/31\nIPOE1_GATEWAY=192.0.2.10\n") == 0);
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=192.0.2.10/31\nIPOE1_GATEWAY=198.51.100.1\n") == 0);
	/* Not its own address, nor one no host has. */
	CHECK(judge(&s, STATIC32 "IPOE1_GATEWAY=203.0.113.7\n") == -2 &&
	      strstr(g_err, "the gateway is no other host's address"));
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=192.0.2.10/31\nIPOE1_GATEWAY=192.0.2.10\n") == -2 &&
	      strstr(g_err, "the gateway is no other host's address"));
	CHECK(judge(&s, STATIC32 "IPOE1_GATEWAY=255.255.255.255\n") == -2);
	CHECK(judge(&s, STATIC32 "IPOE1_GATEWAY=224.0.0.1\n") == -2);
	CHECK(judge(&s, STATIC32 "IPOE1_GATEWAY=0.0.0.0\n") == -2);
	CHECK(judge(&s, STATIC32 "IPOE1_GATEWAY=127.0.0.1\n") == -2);
	CHECK(judge(&s, STATIC1 "IPOE1_FAMILY=both\n") == -2 &&
	      strstr(g_err, "IPOE1_ADDRESS6 and IPOE1_GATEWAY6"));
	/* IPv6 alone needs no IPv4. */
	CHECK(judge(&s, "IPOE1_KIND=static\nIPOE1_VLAN=420\nIPOE1_FAMILY=ipv6\n"
			"IPOE1_ADDRESS6=2001:db8:5::2/64\nIPOE1_GATEWAY6=2001:db8:5::1\n") == 0);
	/* DHCP needs nothing typed in. */
	CHECK(judge(&s, DHCP2 "IPOE2_FAMILY=both\n") == 0);

	/* One in a VLAN, none in the management VLAN. */
	CHECK(judge(&s, STATIC1 DHCP2 "IPOE2_VLAN=420\n") == -2 &&
	      strstr(g_err, "connections 1 and 2 are in one VLAN"));
	g_mgmt[0] = 421;
	CHECK(judge(&s, DHCP2) == -2 && strstr(g_err, "VLAN 421 is the management VLAN"));
	g_mgmt[0] = 0;
	/* Beside a session in its VLAN, the session's interface and MAC address. */
	CHECK(judge(&s, ONE DHCP2 "SESSION1_VLAN=421\nIPOE2_VLAN=421\n") == 0);
	CHECK(judge(&s, ONE DHCP2 "SESSION1_VLAN=421\nSESSION1_MAC=00:00:5e:00:53:01\n"
			"IPOE2_MAC=00:00:5e:00:53:02\n") == -2 &&
	      strstr(g_err, "connection 2 and session 1 are in one VLAN"));
	CHECK(judge(&s, ONE DHCP2 "SESSION1_VLAN=421\nSESSION1_MAC=00:00:5e:00:53:01\n"
			"IPOE2_MAC=00:00:5E:00:53:01\n") == 0);

	/* Seven connections at most, of every kind together. */
	{
		char text[1024];
		size_t at = 0;

		for (int n = 1; n <= 4; n++)
			at += (size_t)snprintf(text + at, sizeof(text) - at,
					       "PPPOE%d_USER=u%d\nPPPOE%d_SECRET=x\nSESSION%d_PASS=off\n",
					       n, n, n, n);
		for (int n = 1; n <= 3; n++)
			at += (size_t)snprintf(text + at, sizeof(text) - at,
					       "IPOE%d_KIND=dhcp\nIPOE%d_VLAN=%d\n", n, n, 100 + n);
		CHECK(judge(&s, text) == 0);
		snprintf(text + at, sizeof(text) - at, "IPOE7_KIND=dhcp\nIPOE7_VLAN=200\n");
		CHECK(judge(&s, text) == -2 && strstr(g_err, "8 connections"));
	}

	/* A network out by a connection: one set up, with IPv4. */
	CHECK(judge(&s, STATIC1 "LAN1_CONN=ipoe:1\nLAN1_ADDRESS=192.168.1.1/24\n") == 0 &&
	      has("LAN1_CONN=ipoe:1"));
	CHECK(judge(&s, DHCP2 "LAN1_CONN=ipoe:2\nLAN1_ADDRESS=192.168.1.1/24\n"
			"LAN2_CONN=ipoe:2\nLAN2_VLAN=20\nLAN2_ADDRESS=192.168.2.1/24\n") == 0);
	CHECK(judge(&s, STATIC1 "LAN1_CONN=ipoe:2\nLAN1_ADDRESS=192.168.1.1/24\n") == -2 &&
	      strstr(g_err, "network 1: connection 2 is not set up"));
	CHECK(judge(&s, DHCP2 "IPOE2_FAMILY=ipv6\nLAN1_CONN=ipoe:2\n"
			"LAN1_ADDRESS=192.168.1.1/24\n") == -2 &&
	      strstr(g_err, "connection 2 carries no IPv4"));
	CHECK(judge(&s, STATIC1 "LAN1_CONN=ipoe:1\nLAN1_ADDRESS=192.0.2.129/25\n") == -2 &&
	      strstr(g_err, "network 1 and connection 1 share addresses"));
	/* An address alone: a network that holds it, or the gateway outside it. */
	CHECK(judge(&s, STATIC32 "LAN1_CONN=ipoe:1\nLAN1_ADDRESS=192.168.1.1/24\n") == 0);
	CHECK(judge(&s, STATIC32 "LAN1_CONN=ipoe:1\nLAN1_ADDRESS=203.0.113.1/24\n") == -2 &&
	      strstr(g_err, "network 1 and connection 1 share addresses"));
	CHECK(judge(&s, STATIC32 "LAN1_CONN=ipoe:1\nLAN1_ADDRESS=198.51.100.254/24\n") == -2 &&
	      strstr(g_err, "network 1 holds the gateway of connection 1"));
	CHECK(judge(&s, STATIC1 "IPOE1_ADDRESS=192.0.2.10/31\nIPOE1_GATEWAY=192.0.2.11\n"
			"LAN1_CONN=ipoe:1\nLAN1_ADDRESS=192.0.2.1/24\n") == -2 &&
	      strstr(g_err, "network 1 and connection 1 share addresses"));
	/* Sessions and connections are networks' ways out side by side. */
	CHECK(judge(&s, KEEPS NET1 DHCP2 "LAN2_CONN=ipoe:2\nLAN2_ADDRESS=10.0.0.1/24\n") == 0);
}

/* Network 1's device DEV, with its fixed address, and network 2 by a connection. */
#define PINNED KEEPS NET1 "FIXED1=1/" DEV "/192.168.1.10\n"
#define NET2 DHCP2 "LAN2_CONN=ipoe:2\nLAN2_VLAN=200\nLAN2_ADDRESS=10.0.0.1/24\n" \
	     "FIXED2=2/00:00:5e:00:53:a2/10.0.0.20\n"

static void test_forwards(void)
{
	static const char *const bad[] = {
		"FORWARD0=pppoe:1/tcp/443/1/" DEV "/443/on",
		"FORWARD97=pppoe:1/tcp/443/1/" DEV "/443/on",
		"FORWARD01=pppoe:1/tcp/443/1/" DEV "/443/on",
		"FORWARD1_FOO=x",
		"FORWARD1=pppoe:8/tcp/443/1/" DEV "/443/on",
		"FORWARD1=lan:1/tcp/443/1/" DEV "/443/on",
		"FORWARD1=pppoe:1/icmp/443/1/" DEV "/443/on",
		"FORWARD1=pppoe:1/tcp/0/1/" DEV "/443/on",
		"FORWARD1=pppoe:1/tcp/65536/1/" DEV "/443/on",
		"FORWARD1=pppoe:1/tcp/0443/1/" DEV "/443/on",
		"FORWARD1=pppoe:1/tcp/9000-8000/1/" DEV "/9000/on",
		"FORWARD1=pppoe:1/tcp/8000-/1/" DEV "/8000/on",
		"FORWARD1=pppoe:1/tcp/443/9/" DEV "/443/on",
		"FORWARD1=pppoe:1/tcp/443/1/01:00:5e:00:00:01/443/on",
		"FORWARD1=pppoe:1/tcp/443/1/" DEV "/0/on",
		"FORWARD1=pppoe:1/tcp/65000-65010/1/" DEV "/65530/on",
		"FORWARD1=pppoe:1/tcp/443/1/" DEV "/443/yes",
		"FORWARD1=pppoe:1/tcp/443/1/" DEV "/443",
		"FORWARD1=pppoe:1/tcp/443/1/" DEV "/443/on/x",
		"FORWARD1_NAME=a\tb",
		"DMZ0=pppoe:1/1/" DEV,
		"DMZ8=pppoe:1/1/" DEV,
		"DMZ1_NAME=x",
		"DMZ1=pppoe:1/1",
		"DMZ1=pppoe:1/0/" DEV,
		"DMZ1=ipoe:8/1/" DEV,
		"DMZ1=pppoe:1/1/" DEV "/x",
		"SVC0=500/off",
		"SVC33=500/off",
		"SVC1_NAME=x",
		"SVC1=0/off",
		"SVC1=4095/off",
		"SVC1=500/drop",
		"SVC1=500/move",
		"SVC1=500/move/0",
		"SVC1=500/off/411",
		"SVC1=500/only_uni/x",
		"SVC1=0500/off",
	};
	struct settings s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		int rc = judge(&s, bad[i]);

		CHECK(rc == -1 && *g_err);
		if (rc != -1)
			printf("    taken: %s\n", bad[i]);
	}

	/* A forward and a DMZ are written as they are, with a name for people. */
	CHECK(judge(&s, PINNED "FORWARD96=pppoe:1/both/8000-8010/1/" DEV "/8000/off\n"
			"FORWARD96_NAME=Kamera w gara\xc5\xbcu\n"
			"FORWARD2=pppoe:1/udp/3074/1/00:00:5E:00:53:A1/3075/on\n"
			"DMZ7=pppoe:1/1/" DEV "\n") == 0);
	CHECK(has("FORWARD96=pppoe:1/both/8000-8010/1/" DEV "/8000/off") &&
	      has("FORWARD96_NAME=Kamera w gara\xc5\xbcu") &&
	      has("FORWARD2=pppoe:1/udp/3074/1/00:00:5E:00:53:A1/3075/on") &&
	      !strstr(g_out, "FORWARD2_NAME") && has("DMZ7=pppoe:1/1/" DEV));
	/* An empty one is none, its name gone with it. */
	CHECK(judge(&s, PINNED "FORWARD3=pppoe:1/tcp/443/1/" DEV "/443/on\nFORWARD3_NAME=x\n"
			"FORWARD3=\nDMZ1=pppoe:1/1/" DEV "\nDMZ1=\n") == 0 &&
	      !strstr(g_out, "FORWARD3") && !strstr(g_out, "DMZ1"));

	/* The rules of the services are written as they are; an empty one is none. */
	CHECK(judge(&s, PINNED "SVC32=500/move/411\nSVC1=200/off\nSVC2=100/only_uni\n"
			"SVC3=300/only_veip\nSVC4=600/off\nSVC4=\n") == 0);
	CHECK(has("SVC32=500/move/411") && has("SVC1=200/off") && has("SVC2=100/only_uni") &&
	      has("SVC3=300/only_veip") && !strstr(g_out, "SVC4"));
	/* Two of one service, or two moves to one host VLAN, are refused. */
	CHECK(judge(&s, PINNED "SVC1=500/off\nSVC2=500/only_uni\n") == -2 &&
	      strstr(g_err, "one service"));
	CHECK(judge(&s, PINNED "SVC1=500/move/411\nSVC2=501/move/411\n") == -2 &&
	      strstr(g_err, "one host VLAN"));
	CHECK(judge(&s, PINNED "SVC1=500/move/411\nSVC2=501/off\nSVC3=502/move/412\n") == 0);

	/* A port may go to another, and a range to the same ports or to others. */
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/8080/1/" DEV "/80/on\n") == 0);
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/8000-8010/1/" DEV "/8000/on\n") == 0);
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/8000-8010/1/" DEV "/9000/on\n") == 0 &&
	      has("FORWARD1=pppoe:1/tcp/8000-8010/1/" DEV "/9000/on"));
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/65530-65535/1/" DEV "/65530/on\n") == 0);
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/both/1000-1010/1/" DEV "/65525/on\n") == 0);

	/*
	 * The ranges moved to other ports move 1024 ports at most, of the
	 * forwards in force, a range of both protocols twice; a range kept
	 * and a port alone move none.
	 */
	CHECK(SETTINGS_MOVED_MAX == FW_MOVED_MAX);
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/both/10000-10511/1/" DEV "/20000/on\n"
			"FORWARD2=pppoe:1/tcp/30000-39999/1/" DEV "/30000/on\n"
			"FORWARD3=pppoe:1/udp/443/1/" DEV "/8443/on\n") == 0);
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/both/10000-10511/1/" DEV "/20000/on\n"
			"FORWARD4=pppoe:1/udp/2000-2001/1/" DEV "/3000/on\n") == -2 &&
	      strstr(g_err,
		     "forward 4: with it, ranges moved to other ports move 1026 ports, more than 1024 (TCP and UDP count apart)"));
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/10000-11024/1/" DEV "/20000/on\n") == -2 &&
	      strstr(g_err, "forward 1: with it, ranges moved to other ports move 1025 ports"));
	/* One kept but not in force moves none. */
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/both/10000-10511/1/" DEV "/20000/on\n"
			"FORWARD4=pppoe:1/udp/2000-2001/1/" DEV "/3000/off\n") == 0);

	/* Its device: in a network out by its connection, at a fixed address. */
	CHECK(judge(&s, KEEPS NET1 "FORWARD1=pppoe:1/tcp/443/1/" DEV "/443/on\n") == -2 &&
	      strstr(g_err, "forward 1: the device " DEV " has no fixed address in network 1"));
	CHECK(judge(&s, PINNED "FORWARD5=pppoe:1/tcp/443/2/" DEV "/443/on\n") == -2 &&
	      strstr(g_err, "forward 5: network 2 is not set up"));
	CHECK(judge(&s, PINNED NET2 "FORWARD1=pppoe:1/tcp/443/2/00:00:5e:00:53:a2/443/on\n") == -2 &&
	      strstr(g_err, "forward 1: network 2 does not go out by pppoe:1"));
	CHECK(judge(&s, PINNED NET2 "FORWARD1=ipoe:2/tcp/443/2/00:00:5e:00:53:a2/443/on\n") == 0);
	/* One kept but not in force is judged as well. */
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/443/1/00:00:5e:00:53:a9/443/off\n") == -2 &&
	      strstr(g_err, "no fixed address"));

	/* No two forwards of a connection take a port of one protocol. */
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/8000-8010/1/" DEV "/8000/on\n"
			"FORWARD2=pppoe:1/tcp/8010/1/" DEV "/80/off\n") == -2 &&
	      strstr(g_err, "forwards 1 and 2 take the same ports of pppoe:1"));
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/both/53/1/" DEV "/53/on\n"
			"FORWARD2=pppoe:1/udp/53/1/" DEV "/53/on\n") == -2);
	CHECK(judge(&s, PINNED "FORWARD1=pppoe:1/tcp/53/1/" DEV "/53/on\n"
			"FORWARD2=pppoe:1/udp/53/1/" DEV "/53/on\n"
			"FORWARD3=pppoe:1/tcp/54-60/1/" DEV "/54/on\n") == 0);
	CHECK(judge(&s, PINNED NET2 "FORWARD1=pppoe:1/tcp/443/1/" DEV "/443/on\n"
			"FORWARD2=ipoe:2/tcp/443/2/00:00:5e:00:53:a2/443/on\n") == 0);

	/* A DMZ: a device as a forward's, and one for a connection. */
	CHECK(judge(&s, PINNED NET2 "DMZ1=pppoe:1/1/" DEV "\nDMZ2=ipoe:2/2/00:00:5e:00:53:a2\n") == 0);
	CHECK(judge(&s, PINNED "DMZ1=pppoe:1/1/00:00:5e:00:53:a9\n") == -2 &&
	      strstr(g_err, "DMZ 1: the device 00:00:5e:00:53:a9 has no fixed address"));
	CHECK(judge(&s, PINNED NET2 "DMZ3=pppoe:1/2/00:00:5e:00:53:a2\n") == -2 &&
	      strstr(g_err, "DMZ 3: network 2 does not go out by pppoe:1"));
	CHECK(judge(&s, PINNED "FIXED2=1/00:00:5e:00:53:a2/192.168.1.20\n"
			"DMZ1=pppoe:1/1/" DEV "\nDMZ4=pppoe:1/1/00:00:5e:00:53:a2\n") == -2 &&
	      strstr(g_err, "DMZs 1 and 4 are of one connection, pppoe:1"));

	/* The most forwards there may be, each with a name, and a DMZ. */
	{
		static char text[16384];
		size_t at = (size_t)snprintf(text, sizeof(text), "%s", PINNED "DMZ1=pppoe:1/1/" DEV "\n");

		for (int i = 1; i <= SETTINGS_FORWARDS; i++)
			at += (size_t)snprintf(text + at, sizeof(text) - at,
					       "FORWARD%d=pppoe:1/both/%d-%d/1/" DEV "/%d/on\n"
					       "FORWARD%d_NAME=Forward %d\n",
					       i, 10000 + 10 * i, 10009 + 10 * i, 10000 + 10 * i,
					       i, i);
		CHECK(at < sizeof(text) && judge(&s, text) == 0 &&
		      has("FORWARD96=pppoe:1/both/10960-10969/1/" DEV "/10960/on") &&
		      has("FORWARD96_NAME=Forward 96"));
	}
}


static void test_rules(void)
{
	static const char *const bad[] = {
		"RULE0=1_allow_lans_internet_tcp_443_any_on",
		"RULE97=1_allow_lans_internet_tcp_443_any_on",
		"RULE01=1_allow_lans_internet_tcp_443_any_on",
		"RULE1_FOO=x",
		"RULE1=0_allow_lans_internet_tcp_443_any_on",
		"RULE1=97_allow_lans_internet_tcp_443_any_on",
		"RULE1=01_allow_lans_internet_tcp_443_any_on",
		"RULE1=1_pass_lans_internet_tcp_443_any_on",
		"RULE1=1_allow_lan_internet_tcp_443_any_on",
		"RULE1=1_allow_lan:0_internet_tcp_443_any_on",
		"RULE1=1_allow_lan:9_internet_tcp_443_any_on",
		"RULE1=1_allow_lan:10_internet_tcp_443_any_on",
		"RULE1=1_allow_dev:1_internet_tcp_443_any_on",
		"RULE1=1_allow_dev:1:01:00:5e:00:00:01_internet_tcp_443_any_on",
		"RULE1=1_allow_dev:1-" DEV "_internet_tcp_443_any_on",
		"RULE1=1_allow_lans_internet:_tcp_443_any_on",
		"RULE1=1_allow_lans_internet:10.0.0.0/33_tcp_443_any_on",
		"RULE1=1_allow_lans_internet:10.0.0/8_tcp_443_any_on",
		"RULE1=1_allow_lans_internet:2001:db8::/129_tcp_443_any_on",
		"RULE1=1_allow_lans_internet:example.net_tcp_443_any_on",
		"RULE1=1_allow_lans_internet_sctp_443_any_on",
		"RULE1=1_allow_lans_internet_tcp_0_any_on",
		"RULE1=1_allow_lans_internet_tcp_65536_any_on",
		"RULE1=1_allow_lans_internet_tcp_0443_any_on",
		"RULE1=1_allow_lans_internet_tcp_9000-8000_any_on",
		"RULE1=1_allow_lans_internet_tcp_8000-_any_on",
		"RULE1=1_allow_lans_internet_any_443_any_on",
		"RULE1=1_allow_lans_internet_icmp_443_any_on",
		"RULE1=1_allow_lans_internet_tcp_443_ipx_on",
		"RULE1=1_allow_lans_internet_tcp_443_any_yes",
		"RULE1=1_allow_lans_internet_tcp_443_any",
		"RULE1=1_allow_lans_internet_tcp_443_any_on_x",
		"RULE1=1/allow/lans/internet/tcp/443/any/on",
		"RULE1_NAME=a\tb",
		/* 41 characters. */
		"RULE1_NAME=12345678901234567890123456789012345678901",
		"FW_OUT=deny",
		"FW_OUT=",
		"FW_BETWEEN=on",
		"FW_IN=block",
		"FW_PING_IN=on",
		"FW_PING_LANS=",
		"FW_PING=allow",
	};
	struct settings s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		int rc = judge(&s, bad[i]);

		CHECK(rc == -1 && *g_err);
		if (rc != -1)
			printf("    taken: %s\n", bad[i]);
	}

	/* The panel's rules, written as they are, in their order, with names for people. */
	CHECK(judge(&s, PINNED NET2
		    "RULE1=1_allow_lan:2_dev:1:" DEV "_tcp_9100_any_on\n"
		    "RULE1_NAME=Drukarka dla Biura\n"
		    "RULE2=2_block_lans_internet_tcp_25_any_on\n"
		    "RULE3=3_allow_internet_dev:1:" DEV "_tcp_443_ipv6_on\n"
		    "RULE3_NAME=NAS po IPv6 \xc5\xbc\xc3\xb3\xc5\x82w\n"
		    "RULE4=4_block_dev:1:" DEV "_internet_any__any_off\n"
		    "RULE96=7_allow_internet:203.0.113.0/24_lans_both_8000-8100_ipv4_on\n"
		    "RULE95=9_allow_lans_internet:2001:db8::/32_icmp__ipv6_on\n"
		    "RULE94=10_allow_lans_internet:198.51.100.7_udp__any_on\n"
		    "RULE93=11_block_internet:10.1.2.3/8_lan:2_any__ipv4_on\n"
		    /* 40 characters, of two bytes each. */
		    "RULE93_NAME=\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc"
		    "\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc"
		    "\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc"
		    "\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc"
		    "\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\xc5\xbc\n"
		    "FW_OUT=block\nFW_BETWEEN=allow\n") == 0);
	CHECK(has("RULE1=1_allow_lan:2_dev:1:" DEV "_tcp_9100_any_on") &&
	      has("RULE1_NAME=Drukarka dla Biura") && has("RULE4=4_block_dev:1:" DEV "_internet_any__any_off") &&
	      has("RULE96=7_allow_internet:203.0.113.0/24_lans_both_8000-8100_ipv4_on") &&
	      !strstr(g_out, "RULE2_NAME") && has("FW_OUT=block") && has("FW_BETWEEN=allow"));
	/* The rules in their order, after the forwards. */
	CHECK(strstr(g_out, "RULE1=") < strstr(g_out, "RULE2=") &&
	      strstr(g_out, "RULE95=") < strstr(g_out, "RULE96="));
	/* A default is not written, and an empty rule is none, its name gone with it. */
	CHECK(judge(&s, PINNED NET2 "FW_OUT=allow\nFW_BETWEEN=block\n"
		    "RULE5=5_block_lans_internet_tcp_25_any_on\nRULE5_NAME=x\nRULE5=\n") == 0 &&
	      !strstr(g_out, "FW_") && !strstr(g_out, "RULE"));
	/* Without any network, a default is kept all the same. */
	CHECK(judge(&s, "FW_OUT=block\n") == 0 && !strcmp(g_out, "FW_OUT=block\n"));
	/* A ping answered from either side, in bridge mode as well, and not again by default. */
	CHECK(judge(&s, "FW_PING_IN=allow\nFW_PING_LANS=allow\n") == 0 &&
	      !strcmp(g_out, "FW_PING_IN=allow\nFW_PING_LANS=allow\n"));
	CHECK(judge(&s, "FW_PING_IN=block\nFW_PING_LANS=block\n") == 0 && !*g_out);
	/* The priority of IGMP: the host's is written, the default is not, another is none. */
	CHECK(judge(&s, "IGMP_PRIORITY=host\n") == 0 && !strcmp(g_out, "IGMP_PRIORITY=host\n"));
	CHECK(judge(&s, "IGMP_PRIORITY=auto\n") == 0 && !*g_out);
	CHECK(judge(&s, "IGMP_PRIORITY=olt\n") == -1 && strstr(g_err, "IGMP_PRIORITY"));

	/* From somewhere to somewhere else. */
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_internet_internet:10.0.0.0/8_any__any_on\n") == -2 &&
	      strstr(g_err, "rule 1: from the internet to the internet"));
	CHECK(judge(&s, PINNED NET2 "RULE2=1_block_lan:1_dev:1:" DEV "_any__any_on\n") == -2 &&
	      strstr(g_err, "rule 2: within one network, 1"));
	CHECK(judge(&s, PINNED NET2 "RULE2=1_block_dev:2:00:00:5e:00:53:a2_dev:2:00:00:5e:00:53:a2_any__any_on\n") == -2 &&
	      strstr(g_err, "within one network, 2"));
	CHECK(judge(&s, PINNED NET2 "RULE1=1_block_lans_lan:1_any__any_on\n"
		    "RULE2=2_block_lan:1_lans_any__any_on\nRULE3=3_block_lans_lans_any__any_on\n") == 0);
	/* The family of its address. */
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_internet:192.0.2.0/24_lan:1_any__ipv6_on\n") == -2 &&
	      strstr(g_err, "rule 1: an address of IPv4 in a rule of ipv6"));
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_lan:1_internet:2001:db8::1_any__ipv4_on\n") == -2 &&
	      strstr(g_err, "rule 1: an address of IPv6 in a rule of ipv4"));
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_lan:1_internet:2001:db8::1_any__any_on\n") == 0);
	/* A network it names is set up; a device it goes to has a fixed address. */
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_lan:3_internet_any__any_on\n") == -2 &&
	      strstr(g_err, "rule 1: network 3 is not set up"));
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_lan:1_dev:4:00:00:5e:00:53:a4_any__any_on\n") == -2 &&
	      strstr(g_err, "rule 1: network 4 is not set up"));
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_lan:2_dev:1:00:00:5e:00:53:a9_any__any_off\n") == -2 &&
	      strstr(g_err, "rule 1: the device 00:00:5e:00:53:a9 has no fixed address in network 1"));
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_dev:1:00:00:5e:00:53:a9_lan:2_any__any_on\n") == 0);
	CHECK(judge(&s, PINNED NET2 "RULE1=1_allow_lan:2_dev:1:00:00:5E:00:53:A1_any__any_on\n") == 0);
	/* An ID is one rule's. */
	CHECK(judge(&s, PINNED NET2 "RULE1=5_allow_lans_internet_any__any_on\n"
		    "RULE7=5_block_lans_internet_any__any_on\n") == -2 &&
	      strstr(g_err, "rules 1 and 7 have one ID, 5"));

	/*
	 * The most rules there may be, each of the longest and with a name of
	 * 40 characters of four bytes, beside 512 fixed addresses, eight
	 * networks and seven sessions: within the 64 KiB of the store.
	 */
	{
		static char text[65536];
		size_t at = 0;

		for (int n = 1; n <= SETTINGS_SESSIONS; n++)
			at += (size_t)snprintf(text + at, sizeof(text) - at,
					       "PPPOE%d_USER=user%d\nPPPOE%d_SECRET=x\n"
					       "SESSION%d_PASS=off\nSESSION%d_VLAN=%d\n",
					       n, n, n, n, n, 30 + n);
		for (int l = 1; l <= SETTINGS_LANS; l++)
			at += (size_t)snprintf(text + at, sizeof(text) - at,
					       "LAN%d_CONN=pppoe:%d\nLAN%d_VLAN=%d\n"
					       "LAN%d_ADDRESS=10.%d.0.1/16\n",
					       l, (l - 1) % 7 + 1, l, 100 + l, l, l);
		for (int i = 0; i < SETTINGS_FIXED; i++)
			at += (size_t)snprintf(text + at, sizeof(text) - at,
					       "FIXED%d=%d/02:00:00:00:%02x:%02x/10.%d.%d.%d\n",
					       i + 1, i / 64 + 1, i / 64 + 1, i % 64, i / 64 + 1,
					       100 + i % 64, 100 + i % 64);
		for (int i = 1; i <= SETTINGS_RULES; i++) {
			at += (size_t)snprintf(text + at, sizeof(text) - at,
					       "RULE%d=%d_block_dev:%d:02:00:00:00:%02x:%02x_internet:0000:0000:0000:0000:0000:ffff:255.255.255.255/128_both_65000-65535_any_off\nRULE%d_NAME=",
					       i, i, (i - 1) % 8 + 1, (i - 1) % 8 + 1, i % 64, i);
			for (int c = 0; c < 40; c++)
				at += (size_t)snprintf(text + at, sizeof(text) - at, "\xf0\x9f\x98\x80");
			at += (size_t)snprintf(text + at, sizeof(text) - at, "\n");
		}
		CHECK(at < sizeof(text) && judge(&s, text) == 0 && strstr(g_out, "RULE96=96_block") &&
		      strlen(g_out) < 65536);
		printf("  96 rules beside 512 fixed addresses: %zu bytes\n", strlen(g_out));
	}
}

int main(void)
{
	test_defaults();
	test_defaults_format();
	test_values();
	test_accounts();
	test_gone();
	test_sessions();
	test_mgmt();
	test_before();
	test_format();
	test_lans();
	test_fixed();
	test_ipoe();
	test_forwards();
	test_rules();
	printf("test_settings: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
