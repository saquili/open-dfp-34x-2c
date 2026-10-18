// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* What onu-act does (act.h): the table of its methods. */

#include <errno.h>
#include <string.h>

#include "act.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define LIB "/usr/lib/onu/"
#define LOWER "abcdefghijklmnopqrstuvwxyz"
#define UPPER "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define DIGITS "0123456789"

/*
 * What the status daemon asks of the stick, as it ran it itself before
 * (pond/README.rst): the lane of the SFP port, the management pair, the
 * changes on trial, the name, the identity, the updates, the restart, and
 * the settings of the data path with the accounts, which the status daemon
 * sees without their secrets (onu-config current) and changes on trial
 * (onu-config change, then onu-apply try), and what became of them (onu-apply
 * state: the mode, the sessions' facts, the changes on trial), the changes
 * that omcid, dhcp6d and the DHCP client's script tell of (onu-call, each its
 * own method: "omcid", "dhcp6d" and "dhcpc" follow what changed, as onu-apply
 * does), root's password, which it
 * checks and replaces without holding its hash, and an update file, which
 * streams from it into the bank that does not run (onu-update write -) or
 * from the running bank to it (onu-update read) through a pipe it passes.
 * A new password, an update written and a start of the other bank need
 * root's password in force with the call.
 */
const struct act_method act_methods[] = {
	{ .name = "lane_status", .program = LIB "onu-lane", .fixed = { "status" } },
	{ .name = "lane_try", .program = LIB "onu-lane", .fixed = { "try" },
	  .params = { { "mode", LOWER DIGITS "-", 16 } } },
	{ .name = "lane_keep", .program = LIB "onu-lane", .fixed = { "keep" } },
	{ .name = "lane_revert", .program = LIB "onu-lane", .fixed = { "revert" } },
	{ .name = "mgmt_status", .program = LIB "onu-mgmt", .fixed = { "status" } },
	{ .name = "mgmt_add", .program = LIB "onu-mgmt", .fixed = { "add" },
	  .params = { { "vlan", DIGITS, 4 }, { "address", DIGITS "./", 18 } } },
	{ .name = "mgmt_commit", .program = LIB "onu-mgmt", .fixed = { "commit" } },
	{ .name = "mgmt_cancel", .program = LIB "onu-mgmt", .fixed = { "cancel" } },
	{ .name = "changes_confirm", .program = LIB "onu-change", .fixed = { "confirm" },
	  .params = { { "address", DIGITS "abcdefABCDEF.:", 45, true } } },
	{ .name = "changes_revert", .program = LIB "onu-change", .fixed = { "revert" } },
	{ .name = "system_store", .program = "/usr/sbin/onu-config", .fixed = { "system" },
	  .input = true },
	{ .name = "hostname", .program = "/bin/hostname",
	  .params = { { "name", LOWER UPPER DIGITS "-", 63 } } },
	{ .name = "identity_store", .program = "/usr/sbin/onu-config", .fixed = { "identity" },
	  .input = true },
	{ .name = "identity_current", .program = "/usr/sbin/onu-config",
	  .fixed = { "identity-current" } },
	{ .name = "reidentify", .program = "/usr/sbin/onu-apply", .fixed = { "reidentify" },
	  .background = true },
	{ .name = "update_status", .program = "/usr/sbin/onu-update", .fixed = { "status" } },
	{ .name = "update_try", .program = "/usr/sbin/onu-update", .fixed = { "try" },
	  .params = { { "bank", "01", 1, true } }, .needs_password = true },
	{ .name = "restart", .program = "/sbin/reboot", .background = true },
	{ .name = "settings_current", .program = "/usr/sbin/onu-config", .fixed = { "current" } },
	{ .name = "settings_change", .program = "/usr/sbin/onu-config", .fixed = { "change" },
	  .input = true },
	{ .name = "settings_state", .program = "/usr/sbin/onu-apply", .fixed = { "state" } },
	{ .name = "settings_try", .program = "/usr/sbin/onu-apply", .fixed = { "try" },
	  .timeout_s = 120 },
	{ .name = "omcid", .program = "/usr/sbin/onu-apply", .fixed = { "omcid" },
	  .background = true },
	{ .name = "dhcp6d", .program = "/usr/sbin/onu-apply", .fixed = { "dhcp6d" },
	  .background = true },
	{ .name = "dhcpc", .program = "/usr/sbin/onu-apply", .fixed = { "dhcpc" },
	  .background = true },
	{ .name = "password_check", .program = "/usr/sbin/onu-config",
	  .fixed = { "password-check" }, .input = true },
	{ .name = "password_state", .program = "/usr/sbin/onu-config",
	  .fixed = { "password-state" } },
	{ .name = "password_set", .program = "/usr/sbin/onu-config", .fixed = { "password" },
	  .input = true, .needs_password = true },
	{ .name = "update_write", .program = "/usr/sbin/onu-update", .fixed = { "write", "-" },
	  .fd_in = true, .needs_password = true, .timeout_s = 600 },
	{ .name = "update_read", .program = "/usr/sbin/onu-update", .fixed = { "read" },
	  .fd_out = true, .timeout_s = 600 },
};

const size_t act_method_count = ARRAY_SIZE(act_methods);

const struct act_method *act_find(const char *name)
{
	for (size_t i = 0; i < act_method_count; i++)
		if (!strcmp(act_methods[i].name, name))
			return &act_methods[i];
	return NULL;
}

/* Whether @v may stand in the place of @p: never an option, and of its characters. */
static bool value_ok(const struct act_param *p, const char *v)
{
	size_t n = strlen(v);

	return n <= p->max && v[0] != '-' && strspn(v, p->chars) == n;
}

int act_argv(const struct act_method *m, const char *const *values,
	     const char **argv, const char **bad)
{
	size_t n = 0;

	argv[n++] = m->program;
	for (size_t i = 0; m->fixed[i]; i++)
		argv[n++] = m->fixed[i];
	for (size_t i = 0; m->params[i].name; i++) {
		const struct act_param *p = &m->params[i];
		const char *v = values ? values[i] : NULL;

		if (!v || !*v) {
			if (p->optional)
				continue;
			*bad = p->name;
			return -EINVAL;
		}
		if (!value_ok(p, v)) {
			*bad = p->name;
			return -EINVAL;
		}
		argv[n++] = v;
	}
	argv[n] = NULL;
	return 0;
}
