// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of onu-act's table of methods (act.h). */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "act.h"

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

/* The arguments of @method with @values, end to end with blanks, or "" on an error. */
static const char *run(const char *method, const char *const *values,
		       const char **bad)
{
	static char line[512];
	const struct act_method *m = act_find(method);
	const char *argv[ACT_ARGV_MAX + 1];
	size_t len = 0;

	line[0] = '\0';
	*bad = NULL;
	if (!m || act_argv(m, values, argv, bad))
		return line;
	for (size_t i = 0; argv[i]; i++)
		len += (size_t)snprintf(line + len, sizeof(line) - len, "%s%s",
					i ? " " : "", argv[i]);
	return line;
}

static void test_the_table(void)
{
	puts("the table");
	CHECK(act_method_count >= 20);
	for (size_t i = 0; i < act_method_count; i++) {
		const struct act_method *m = &act_methods[i];
		size_t fixed = 0, params = 0;

		CHECK(act_find(m->name) == m);
		CHECK(m->program[0] == '/');
		while (m->fixed[fixed])
			fixed++;
		while (m->params[params].name)
			params++;
		CHECK(fixed <= ACT_FIXED_MAX && params <= ACT_PARAMS_MAX);
		for (size_t k = 0; k < params; k++) {
			CHECK(m->params[k].max > 0 && m->params[k].max <= ACT_VALUE_MAX);
			/* An optional value is the last one. */
			CHECK(!m->params[k].optional || k == params - 1);
			CHECK(!strchr(m->params[k].chars, '/') ||
			      !strcmp(m->name, "mgmt_add"));
		}
		CHECK(!(m->input && m->background));
		CHECK(!(m->fd_in && (m->fd_out || m->input || m->background)));
		CHECK(!(m->fd_out && m->background));
	}
	CHECK(act_find("sh") == NULL && act_find("") == NULL);
}

static void test_values_in_their_places(void)
{
	const char *const lane[] = { "2500basex" };
	const char *const pair[] = { "100", "192.0.2.10/30" };
	const char *const here[] = { "fe80::1" };
	const char *const none[] = { NULL };
	const char *const bank[] = { "1" };
	const char *const on[] = { "on" }, *const off[] = { "off" };
	const char *bad;

	puts("values in their places");
	CHECK(!strcmp(run("lane_try", lane, &bad), "/usr/lib/onu/onu-lane try 2500basex"));
	CHECK(!strcmp(run("lane_status", NULL, &bad), "/usr/lib/onu/onu-lane status"));
	CHECK(!strcmp(run("mgmt_add", pair, &bad),
		      "/usr/lib/onu/onu-mgmt add 100 192.0.2.10/30"));
	CHECK(!strcmp(run("changes_confirm", here, &bad),
		      "/usr/lib/onu/onu-change confirm fe80::1"));
	CHECK(!strcmp(run("changes_confirm", none, &bad), "/usr/lib/onu/onu-change confirm"));
	CHECK(!strcmp(run("update_try", bank, &bad), "/usr/sbin/onu-update try 1"));
	CHECK(!strcmp(run("update_try", NULL, &bad), "/usr/sbin/onu-update try"));
	CHECK(!strcmp(run("update_return", bank, &bad), "/usr/sbin/onu-update return 1"));
	CHECK(!strcmp(run("update_return", NULL, &bad), "/usr/sbin/onu-update return"));
	CHECK(!strcmp(run("update_backup", NULL, &bad), "/usr/sbin/onu-update backup"));
	CHECK(act_find("update_backup")->fd_out && !act_find("update_backup")->fd_in);
	CHECK(!strcmp(run("restart", NULL, &bad), "/sbin/reboot"));
	CHECK(act_find("system_store")->input && act_find("identity_store")->input);
	CHECK(act_find("reidentify")->background && act_find("restart")->background);
	CHECK(!strcmp(run("settings_current", NULL, &bad), "/usr/sbin/onu-config current"));
	CHECK(act_find("settings_change")->input && !act_find("settings_current")->input);
	CHECK(!strcmp(run("settings_state", NULL, &bad), "/usr/sbin/onu-apply state"));
	/*
	 * What omcid, dhcp6d and the DHCP client's script tell of a change: each
	 * its own, at once, no more.
	 */
	CHECK(!strcmp(run("omcid", NULL, &bad), "/usr/sbin/onu-apply omcid"));
	CHECK(!strcmp(run("dhcp6d", NULL, &bad), "/usr/sbin/onu-apply dhcp6d"));
	CHECK(!strcmp(run("dhcpc", NULL, &bad), "/usr/sbin/onu-apply dhcpc"));
	CHECK(act_find("omcid")->background && act_find("dhcp6d")->background &&
	      act_find("dhcpc")->background);
	CHECK(!act_find("dhcpc")->input && !act_find("dhcpc")->params[0].name);
	CHECK(!act_find("omcid")->input && !act_find("omcid")->needs_password &&
	      !act_find("omcid")->params[0].name);
	CHECK(!act_find("settings_state")->input && !act_find("settings_state")->needs_password);
	CHECK(!act_find("settings_try")->background && act_find("settings_try")->timeout_s >= 60);
	CHECK(!strcmp(run("update_write", NULL, &bad), "/usr/sbin/onu-update write -"));
	CHECK(act_find("update_write")->fd_in && act_find("update_read")->fd_out);
	/* The keys of the ssh server and of the panel. */
	CHECK(!strcmp(run("ssh_state", NULL, &bad), "/usr/sbin/onu-config ssh show"));
	CHECK(!strcmp(run("ssh_host_key_new", NULL, &bad), "/usr/sbin/onu-config ssh new"));
	CHECK(!strcmp(run("ssh_key_add", NULL, &bad), "/usr/sbin/onu-config ssh-keys add"));
	CHECK(!strcmp(run("ssh_key_remove", NULL, &bad), "/usr/sbin/onu-config ssh-keys remove"));
	CHECK(act_find("ssh_key_add")->input && act_find("ssh_key_remove")->input);
	CHECK(!strcmp(run("ssh_keys_only", on, &bad), "/usr/sbin/onu-config ssh-keys only on"));
	CHECK(!strcmp(run("ssh_keys_only", off, &bad), "/usr/sbin/onu-config ssh-keys only off"));
	CHECK(!strcmp(run("tls_key_new", NULL, &bad), "/usr/sbin/onu-config tls new"));
	CHECK(!strcmp(run("tls_own_store", NULL, &bad), "/usr/sbin/onu-config tls own"));
	CHECK(act_find("tls_own_store")->input);
	CHECK(!strcmp(run("tls_own_remove", NULL, &bad), "/usr/sbin/onu-config tls self-signed"));
	/* The token of scripts: a new one, printed, or none. */
	CHECK(!strcmp(run("token_new", NULL, &bad), "/usr/sbin/onu-config token new"));
	CHECK(!strcmp(run("token_off", NULL, &bad), "/usr/sbin/onu-config token off"));
	CHECK(!act_find("token_new")->input && !act_find("token_new")->background);
	/*
	 * What would give a caller root, or change the keys of the stick's
	 * servers or the token of scripts, needs root's password, and no more.
	 */
	for (size_t i = 0; i < act_method_count; i++) {
		const char *n = act_methods[i].name;

		CHECK(act_methods[i].needs_password == (!strcmp(n, "password_set") ||
						  !strcmp(n, "update_write") ||
						  !strcmp(n, "update_try") ||
						  !strcmp(n, "update_return") ||
						  !strcmp(n, "update_backup") ||
						  !strcmp(n, "ssh_host_key_new") ||
						  !strcmp(n, "ssh_key_add") ||
						  !strcmp(n, "ssh_key_remove") ||
						  !strcmp(n, "ssh_keys_only") ||
						  !strcmp(n, "tls_key_new") ||
						  !strcmp(n, "tls_own_store") ||
						  !strcmp(n, "tls_own_remove") ||
						  !strcmp(n, "token_new") ||
						  !strcmp(n, "token_off")));
	}
}

static void test_what_is_refused(void)
{
	const char *const option[] = { "-rf" };
	const char *const path[] = { "../x" };
	const char *const blank[] = { "2500 basex" };
	const char *const empty[] = { "" };
	const char *const lng[] = { "abcdefghijklmnopq" };
	const char *const pair[] = { "100", "192.0.2.10/30;reboot" };
	const char *const name[] = { "stick 7" };
	const char *const bank[] = { "2" };
	const char *bad;

	puts("what is refused");
	CHECK(!*run("lane_try", option, &bad) && bad && !strcmp(bad, "mode"));
	CHECK(!*run("lane_try", path, &bad) && bad);
	CHECK(!*run("lane_try", blank, &bad) && bad);
	CHECK(!*run("lane_try", empty, &bad) && bad);
	CHECK(!*run("lane_try", NULL, &bad) && bad);
	CHECK(!*run("lane_try", lng, &bad) && bad);
	CHECK(!*run("mgmt_add", pair, &bad) && bad && !strcmp(bad, "address"));
	CHECK(!*run("hostname", name, &bad) && bad);
	CHECK(!*run("update_try", bank, &bad) && bad);
	CHECK(!*run("ssh_keys_only", option, &bad) && bad && !strcmp(bad, "state"));
	CHECK(!*run("ssh_keys_only", NULL, &bad) && bad);
	CHECK(!*run("ssh_keys_only", lng, &bad) && bad);
	CHECK(!*run("nothing", NULL, &bad) && !bad);
}

int main(void)
{
	test_the_table();
	test_values_in_their_places();
	test_what_is_refused();
	printf("test_act: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
