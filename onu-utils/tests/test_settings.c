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
 * sense; the settings are in @s and their lines in g_out. */
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

int main(void)
{
	test_defaults();
	test_values();
	test_accounts();
	test_gone();
	test_sessions();
	test_mgmt();
	test_before();
	test_format();
	printf("test_settings: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
