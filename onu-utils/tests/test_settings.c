// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the settings of the data path (settings.h). */

#include <stdio.h>
#include <string.h>

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
static char g_out[8192];

/* Take the lines of @text over the defaults, as onu-config check does: 0, or
 * -1 at the first line that is not taken, or -2 when the whole makes no
 * sense; the settings are in @s and their lines in g_out.
 */
static unsigned int g_mgmt[2];

static int judge(struct settings *s, const char *text)
{
	char copy[4096], *line, *save = NULL;

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
	CHECK(lines == SET_WAN_MAC + 1 + SETTINGS_SESSIONS * SETTINGS_NSKEYS);
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
	printf("test_settings: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
