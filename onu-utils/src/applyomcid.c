// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyomcid: omcid in the shape of the settings: its mode, the VLANs the
 * sessions and the IPoE connections hold beside its bridge, the rules of the
 * services, the priority of IGMP, and the identity it reads.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "applyconf.h"
#include "applyomcid.h"
#include "applyutil.h"

/*
 * The priorities of the filters on sfp and pon are the registry's
 * (omcid/target/tcprio.h): omcid's VLAN rules take those from VLAN_PREF up,
 * and the management VLAN's, below, are onu-mgmt's alone.
 */
#define VLAN_PREF 1000

/*
 * Wait until omcid says that it programmed the data path for the mode $1:
 * 5 s at most (APPLIED_TENTHS). What omcid said of another mode, an earlier
 * signal's, is not it.
 */
static void omcid_applied(const char *mode)
{
	const char *want = strcmp(mode, "bridge") ? "router" : "bridge";
	char path[PATH_MAX], got[VAL];

	PATHF(path, "%s/run/omcid/applied", g_root);
	for (long i = 0; i < g_applied_tenths; i++) {
		if (!strcmp(read_line(path, got), want))
			return;
		sleep_tenths(1);
	}
	log_msg("omcid did not say that it is in %s mode: it may not have changed it", mode);
}

/*
 * The rules of the services, SVCn ("FIBRE/ACTION[/HOST]"), as omcid reads
 * them from /run/omcid/rules: a line "FIBRE ACTION [HOST]" each, readable by
 * its account. A new file only when they changed, as omcid programs its data
 * path again at each new one (its target/hw_kernel.c, rules_changed()).
 */
void omcid_rules(void)
{
	char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
	struct buf conf = { 0 }, rules = { 0 }, have = { 0 };
	struct words lines = { 0 };

	if (!is_dir(PATHF(dir, "%s/run/omcid", g_root)) || !cat(CONF, &conf))
		goto out;
	words_lines(&lines, buf_str(&conf));
	for (size_t i = 0; i < lines.n; i++) {
		const char *s = lines.v[i];
		struct words f = { 0 };
		size_t d;

		if (!starts(s, "SVC"))
			continue;
		d = strspn(s + 3, "0123456789");
		if (!d || s[3 + d] != '=' || !isdigit((unsigned char)s[4 + d]))
			continue;
		words_fields(&f, s, "=/");
		if (rules.len)
			buf_puts(&rules, "\n");
		buf_printf(&rules, "%s %s", field(&f, 1), field(&f, 2));
		if (*field(&f, 3))
			buf_printf(&rules, " %s", field(&f, 3));
		words_free(&f);
	}
	PATHF(path, "%s/rules", dir);
	if (exists(path) && cat(path, &have) && !strcmp(buf_str(&have), buf_str(&rules)))
		goto out;
	if (rules.len)
		buf_puts(&rules, "\n");
	PATHF(tmp, "%s/rules.new", dir);
	if (!write_text(tmp, buf_str(&rules), rules.len) || chmod(tmp, 0644) || rename(tmp, path))
		log_msg("omcid cannot read the rules of the services");
out:
	words_free(&lines);
	buf_free(&conf);
	buf_free(&rules);
	buf_free(&have);
}

/*
 * The priority the subscriber's IGMP goes up with where the OLT leaves it,
 * IGMP_PRIORITY, as omcid reads it from /run/omcid/igmp-priority at each
 * message (its target/hw_kernel.c): host, or auto, the default.
 */
void omcid_igmp(void)
{
	char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
	struct words lines = { 0 };
	struct buf conf = { 0 };
	const char *oi = "auto";

	if (!is_dir(PATHF(dir, "%s/run/omcid", g_root)))
		return;
	cat(CONF, &conf);
	words_lines(&lines, buf_str(&conf));
	for (size_t i = lines.n; i-- > 0;)
		if (starts(lines.v[i], "IGMP_PRIORITY=")) {
			if (!strcmp(lines.v[i] + 14, "host"))
				oi = "host";
			break;
		}
	PATHF(tmp, "%s/igmp-priority.new", dir);
	if (!write_line(tmp, oi) || chmod(tmp, 0644) ||
	    rename(tmp, PATHF(path, "%s/igmp-priority", dir)))
		log_msg("omcid cannot read the priority of IGMP");
	words_free(&lines);
	buf_free(&conf);
}

/*
 * The identity omcid reads, a copy of the unit's without the PLOAM password,
 * which root alone and the PON driver need: omcid is an account of its own.
 */
static void omcid_identity(void)
{
	char tmp[PATH_MAX], path[PATH_MAX];
	struct buf text = { 0 }, keep = { 0 };
	struct words lines = { 0 };

	if (!readable(IDENTITY))
		return;
	cat(IDENTITY, &text);
	words_lines(&lines, buf_str(&text));
	for (size_t i = 0; i < lines.n; i++)
		if (!starts(lines.v[i], "ploam_password=")) {
			buf_puts(&keep, lines.v[i]);
			buf_puts(&keep, "\n");
		}
	PATHF(tmp, "%s/run/omcid/identity.new", g_root);
	if (!write_text(tmp, buf_str(&keep), keep.len) || !keep.len ||
	    !give_group(tmp, "omcid", 0640) || rename(tmp, PATHF(path, "%s/run/omcid/identity", g_root)))
		log_msg("omcid cannot read the identity");
	words_free(&lines);
	buf_free(&text);
	buf_free(&keep);
}

bool omcid_runs(void)
{
	return !run(RUN_QUIET_ERR | RUN_QUIET_OUT, "pidof", "omcid", NULL);
}

/*
 * omcid in the mode $1. A running omcid is told by a signal, SIGUSR2 for
 * bridge mode and SIGUSR1 for "router", without the OLT's bridge: it keeps its
 * MIB, so what the OLT provisioned stays, and programs the data path again
 * from it, and says so in /run/omcid/applied, which is taken away before the
 * signal and waited for after it, for 5 s. One that does not run is started
 * as the instance "omcid" of procd's service "onu" (onu-run omcid), with -r
 * without the bridge; its log goes on in /run/omcid.log. The mode is kept in
 * /run/omcid.mode, so that an omcid that procd starts again after a crash
 * comes up in the mode the stick is in.
 */
void omcid_run(const char *mode)
{
	char path[PATH_MAX], where[PATH_MAX];

	write_line(PATHF(path, "%s/run/omcid.mode", g_root), mode);
	if (omcid_runs()) {
		rm_f(PATHF(path, "%s/run/omcid/applied", g_root));
		run(0, "killall", strcmp(mode, "bridge") ? "-USR1" : "-USR2", "omcid", NULL);
		omcid_applied(mode);
		return;
	}
	omcid_identity();
	/* procd runs it, and finds no program on a path of its own. */
	if (!command_v("onu-run", where, sizeof(where)))
		PATHF(where, "%s/onu-run", LIB);
	SVC_START("omcid", "--", where, "omcid");
}

/*
 * The VLANs the sessions hold, by their VID on the PON side, sorted, once
 * each; false when a session runs untagged on its GEM port.
 */
bool held_vlans(struct words *out)
{
	int s[MAX_SESSIONS], i[MAX_IPOE], cs = sessions(s), ci = ipoes(i);
	char v[VAL];

	for (int n = 0; n < cs; n++) {
		if (!*session_vlan(s[n], v))
			return false;
		words_add(out, v);
	}
	for (int n = 0; n < ci; n++)
		words_split(out, ipoe_vlan(i[n], v));
	words_sort(out, true, true);
	return true;
}

/*
 * What omcid is to do beside the sessions: "held VID...", the OLT's bridge
 * without the VLANs the sessions hold; "router", without the OLT's bridge,
 * beside a session that holds a whole GEM port.
 */
void held_want(struct buf *out)
{
	struct words held = { 0 };

	buf_reset(out);
	if (held_vlans(&held)) {
		buf_puts(out, "held");
		for (size_t n = 0; n < held.n; n++)
			buf_printf(out, " %s", held.v[n]);
	} else {
		buf_puts(out, "router");
	}
	words_free(&held);
}

/* What omcid is to do in the mode the settings make: "bridge", or held_want(). */
void omcid_want(struct buf *out)
{
	if (!strcmp(g_mode, "bridge")) {
		buf_reset(out);
		buf_puts(out, "bridge");
	} else {
		held_want(out);
	}
}

/*
 * What omcid built in bridge mode: the bridge and its VLAN rules, the filters
 * from priority VLAN_PREF up on the ingress of sfp and pon. The filters of the
 * management VLAN, which onu-mgmt put at its priorities of the same qdiscs,
 * stay: they keep that VLAN off the PON in every mode.
 */
static void bridge_down(void)
{
	static const char *const devs[] = { "sfp", "pon" };

	run(RUN_QUIET_ERR, "onu-nl", "link", "del", "omcibr0", NULL);
	for (size_t d = 0; d < ARRAY_SIZE(devs); d++) {
		struct words prefs = { 0 };
		struct buf out = { 0 };

		run_out(RUN_QUIET_ERR, &out, "onu-nl", "filter", "prefs", devs[d], NULL);
		words_split(&prefs, buf_str(&out));
		for (size_t i = 0; i < prefs.n; i++) {
			char *end;
			long pref = strtol(prefs.v[i], &end, 10);

			if (*end || pref >= VLAN_PREF)
				run(RUN_QUIET_ERR, "onu-nl", "filter", "del", devs[d], "pref",
				    prefs.v[i], NULL);
		}
		words_free(&prefs);
		buf_free(&out);
	}
}

/*
 * Bring omcid to $1, as omcid_want() gives it, from what it was told last.
 * omcid reads the held VLANs from /run/onu-held-vlans; for them its bridge is
 * taken down first, so that it builds it again without what it leaves out
 * now. Back to the whole bridge it adds what it left out.
 */
void omcid_sync(const char *want)
{
	char path[PATH_MAX], held[PATH_MAX];
	struct buf told = { 0 };

	PATHF(path, "%s/run/onu-omcid", g_root);
	PATHF(held, "%s/run/onu-held-vlans", g_root);
	cat(path, &told);
	if (!strcmp(buf_str(&told), want)) {
		buf_free(&told);
		return;
	}
	buf_free(&told);
	if (starts(want, "held ")) {
		struct buf vids = { 0 };

		buf_puts(&vids, want + 5);
		for (size_t i = 0; i < vids.len; i++)
			if (vids.p[i] == ' ')
				vids.p[i] = '\n';
		buf_puts(&vids, "\n");
		write_text(held, vids.p, vids.len);
		buf_free(&vids);
		bridge_down();
		omcid_run("bridge");
	} else if (!strcmp(want, "bridge")) {
		rm_f(held);
		omcid_run("bridge");
	} else {
		rm_f(held);
		omcid_run("router");
		bridge_down();
	}
	write_line(path, want);
}
