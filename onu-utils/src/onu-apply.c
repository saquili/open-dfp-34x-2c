// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-apply: put the stick's data path in the shape its settings give: the
 * OLT's bridge, and the stick's own PPPoE sessions with their addresses handed
 * on.
 *
 *   onu-apply start    at boot: take the settings of /run/onu.conf and apply
 *   onu-apply reload   apply the settings of /run/onu.conf again, after a change,
 *                      and keep them in the configuration store (onu-config)
 *   onu-apply try      apply them on trial: unconfirmed in 90 s (TRIAL_LIMIT),
 *                      the stick goes back to the settings it had
 *   onu-apply confirm  keep the change on trial, in the store as well
 *   onu-apply revert   end the trial now, back to the settings it had
 *   onu-apply stop     take the sessions down and stop omcid
 *   onu-apply identity give the PON driver the serial number and PLOAM
 *                      password of /run/onu-identity, and the OLT's disable
 *                      the store keeps, and bring the PON up
 *   onu-apply reidentify  take the PON down, give it the identity of
 *                      /run/onu-identity, and register with the OLT again
 *   onu-apply status   the shape in force, the sessions, and the address
 *                      handed on
 *   onu-apply iface    what netifd runs for an event of an interface (netifd
 *                      -h), with ACTION, INTERFACE and DEVICE: a session's
 *                      interface that comes up hands on what it brought
 *   onu-apply guard    the filters that keep the sessions' packets off the
 *                      host side again, after the management pair changed,
 *                      and the guard of the stick's own programs (guard_fw()),
 *                      which onu-mgmt start puts in place at boot, before
 *                      the servers start
 *   onu-apply state    the state last brought up, as lines name=value for the
 *                      status daemon (state() below)
 *   onu-apply sync     follow what changed outside the settings: the GEM
 *                      ports and VLANs the OLT provisions, the host's IPv6
 *                      binding, an IPoE connection's DHCP lease; omcid (-x),
 *                      dhcp6d (-x) and the DHCP client's script run it as
 *                      "onu-apply omcid", "onu-apply dhcp6d" and
 *                      "onu-apply dhcpc"
 *
 * One onu-apply at a time changes the stick; the others wait for it.
 *
 * It is a function of the settings and of the facts of the moment (ADR 0011):
 * what the stick is to be goes to netifd as text (onu-net), to procd as the
 * list of the programs of the service "onu" (onu-svc), to the kernel's
 * filters and the switch's parameters (onu-nl), and to the firewall (onu-fw).
 * It takes nothing down that it did not make, and run twice on the same
 * input it changes nothing.
 *
 * The settings are KEY=value lines in /run/onu.conf, which onu-config writes
 * from the store at boot and at a change; /etc/onu.conf says what each key
 * means, and the default of a key is the table of settings.c, which this
 * program shares with onu-config (onu-config defaults prints it). The account
 * of PPPoE session N, 1 to 7, is in /run/onu-pppoeN, as "user secret",
 * readable by root and pppoed only; /etc/onu-pppoe, if a bench's files carry
 * one, seeds the first. At boot, onu-config has put the saved ones in these
 * files before onu-apply starts.
 *
 * There is no mode to choose. Without a PPPoE account, omcid connects the host
 * link to the PON as the OLT provisions it (bridge). With one or more, the
 * stick holds the sessions itself (passthrough), and each session runs on a
 * GEM port that the OLT provisioned, or on a VLAN on it, with the settings
 * SESSIONn_GEM, SESSIONn_VLAN, SESSIONn_MAC and SESSIONn_FAMILY (ipv4, ipv6
 * or both), and hands its address down to the host or keeps it on the stick,
 * SESSIONn_PASS (on or off), in the VLAN of the host link SESSIONn_HOST_VLAN
 * names (nothing for the session's own VLAN, untagged for none). Beside
 * sessions in VLANs omcid still bridges the rest of what the OLT provisions,
 * such as television and telephone, and leaves out the sessions' VLANs, which
 * it reads from /run/onu-held-vlans. A session's VLAN may be the subscriber's
 * VID of a translation the OLT provisions, the VID a router behind the stick
 * would use: the session then runs in the VID it leads into, as omcid
 * publishes it (/run/omcid/xlate), and a session whose GEM port is auto runs
 * on the GEM port that the OLT lets that VID through (/run/omcid/gem-vlans):
 * the session follows its service when the OLT moves it to another VID or
 * another GEM port. Beside a session untagged on its GEM port, which holds the
 * whole port, omcid leaves the OLT's bridge out (omcid -r).
 *
 * Sessions in one VLAN share its interface, wanVID, and the MAC of the first.
 * In each VLAN towards the host, the first session told to hand down that
 * carries IPv4 hands its address and its name servers down, and the first
 * that carries IPv6 its delegated prefix, through dhcp6d, once its IPv6 is
 * up; the others keep theirs on the stick. What a session has is in the
 * directory /run/session/N: gem, dev and family, hands, the families it hands
 * down ("ipv4 ipv6", one of them or nothing), role, lan where it hands down,
 * and once it is up ppp, ipv4, peer, ipv6 and dns, which pppoed writes, and
 * pass and gateway once its address went down. Each session is an interface
 * of netifd, sesN, of the protocol "pppoed": netifd keeps pppoed and starts it
 * again when it ends, puts what the session brought in place, and runs
 * "onu-apply iface" when the interface comes up or goes down (ADR 0011).
 * In passthrough, the address handed down goes on no interface of the
 * stick's, and the one host on its link, the router behind the stick, takes
 * it by DHCP, as a /32 with the concentrator's address as its gateway. Each
 * VLAN the sessions hand down in is a link of its own, lanVID on the host
 * link, or the host link itself for none. netifd holds the links with their
 * addresses and checks, the part "links" of its configuration, and what each
 * session hands down, its route, proxy ARP entry and table, the part
 * "pass-N" (onu-net).
 *
 * A local network, N from 1 to 8, routes the devices of the host side out by
 * a session that keeps its address on the stick, LANn_CONN=pppoe:S: a VLAN of
 * the host link, netN, or the host link itself, with the stick's address in
 * the network, LANn_ADDRESS; a rule sends what comes from the network to the
 * session's table, which holds the session's default route and the routes to
 * all of the networks, so that the networks reach each other without the
 * session; and a DHCP server, udhcpd-netN, leases its addresses, LANn_POOL_FROM
 * to LANn_POOL_TO, for LANn_LEASE seconds, with the stick as the router, the
 * session's name servers, and the fixed address of each device FIXEDk gives
 * ("N/MAC/IP"), its leases in /run/udhcpd-netN.leases. netifd holds the
 * networks, their rules and routes, the part "nets"; the NAT through which the
 * networks go out, and the protection that keeps the internet's new
 * connections from them, is onu-fw's table of nf_tables (lans_fw()). When no
 * session hands an address down, the first network's connection is the stick's
 * own default route too (main_route()).
 *
 * A connection that takes its address as a router does, IPoE, K from 1 to 7,
 * runs in a VLAN of the PON, IPOEk_VLAN, on its GEM port as a session's, on
 * the interface wanVID, which it shares with the sessions in that VLAN, at
 * Ethernet's MTU. Its address is typed in, IPOEk_KIND=static, with its
 * gateway and name servers, or comes from the operator's DHCP server,
 * IPOEk_KIND=dhcp, by udhcpc, the procd instance udhcpc-ipoeK, which runs as
 * the account dhcpc and whose script, onu-dhcpc, writes what the lease
 * brings into /run/ipoe/K and has onu-apply follow it ("onu-apply dhcpc", by
 * onu-call). netifd holds the interface, ipoeK, with the address, the part
 * "ipoe-K": a table of the connection's own holds its default route, a rule
 * sends what comes from its address there, and the connection is the
 * stick's own default route when main_route() names it. A gateway outside
 * the address's network, as an operator gives with an address alone, a /32,
 * is reached on the link, by a route to it alone. A local network goes out
 * by it as by a session, LANn_CONN=ipoe:K. What it has is in /run/ipoe/K:
 * gem, dev, kind, and once it has an address ipv4, router, dns, until (the
 * end of a lease, in seconds of the uptime), ipv6 and prefix. No DHCPv6
 * client runs for a connection by DHCP.
 *
 * A forward, FORWARDk ("CONN/PROTO/PORTS/L/MAC/PORT/ON"), sends what comes in
 * new by the connection CONN to its IPv4 address, of TCP, UDP or both, to a
 * port or a range of ports, to the device MAC of network L at its fixed
 * address: a port to the port PORT, a range to as many ports from PORT, the
 * same or others, which onu-fw maps one by one; one with ON off is kept and
 * not in force. A connection's DMZ, DMZk ("CONN/L/MAC"), takes what no
 * forward takes, a ping too, but DHCP's answers, which stay with the stick,
 * as do the answers to its own traffic. They are lines of onu-fw's table
 * (forwards_fw()), by the connection's interface and its address there while
 * it has one: a session's once its IPv4 is up, an IPoE connection's while it
 * has an address.
 *
 * The firewall's rules, RULEk ("ID_ACTION_FROM_TO_PROTO_PORTS_FAMILY_ON"),
 * are the switch's ACL's (ADR 0019). Where no rule decides, the table drops
 * a new connection from the internet into a network, one from a network to
 * the internet when FW_OUT is block, and one between two networks when
 * FW_BETWEEN is block, as lines "default out|between block|allow" of
 * onu-fw's file; a rule that allows, in force, is a line "allow WAY ..."
 * there as well, which lets a new connection of IPv4 through where its
 * way's default would drop it (rules_fw()). A rule that blocks is the ACL's
 * alone, and so, until the stick routes IPv6 to the networks' devices, is
 * one of IPv6 alone. onu-fw puts the rules in force into the ACL from
 * /run/onu-fw-acl.conf, the networks, the connections' addresses, the
 * devices the rules name, the forwards, the DMZs and the rules (acl_fw()),
 * and writes what became of each rule to /run/onu-fw.acl, "ID in|part|out
 * ENTRIES [WHY]" a line; the rules leave the ACL with the last rule in
 * force, and with the networks.
 *
 * The stick talks to the host over its management VLAN, and its servers
 * answer there alone, in every shape, a router's too: onu-fw's guard, the
 * table "inet onu_guard", drops what comes in to the stick by any other
 * interface, but what a program of the stick takes there, and forwards
 * nothing to the management VLAN nor from it (guard_fw()). A ping is
 * answered from the operator's side when FW_PING_IN is allow, from the local
 * networks and the links to the router when FW_PING_LANS is; on the
 * management VLAN always.
 *
 * The program is in parts, each with its header: applyconf, the files and
 * the settings; applyomcid, omcid; applyconn, what the connections share;
 * applyppp, the PPPoE sessions; applyipoe, the IPoE connections; applyhost,
 * the links and the local networks of the host side; applyfw, onu-fw's
 * files; and applyutil, what the shell gave its script for nothing.
 *
 * Each message goes to the system log, as "onu-apply", and to standard
 * error. ONU_ROOT names another root for the files it reads and writes, which
 * the tests use; the programs it runs are found by PATH, with /usr/lib/onu
 * after it.
 *
 * Exit status: 0, but 1 when identity or reidentify finds no serial number or
 * cannot keep the PON disabled, when try cannot put the trial in place, and
 * when revert finds no trial; 2 for a wrong command line.
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
#include "applyconn.h"
#include "applyfw.h"
#include "applyhost.h"
#include "applyipoe.h"
#include "applyomcid.h"
#include "applyppp.h"
#include "applyutil.h"
#include "settings.h"

/*
 * What changed outside the settings, followed: with accounts or IPoE
 * connections those, without them omcid alone. The IPoE connections come
 * first: one that leaves a session's VLAN lets the session that goes down
 * and up again for it take its interface's room for PPPoE again. Run twice
 * on the same facts, it changes nothing.
 */
static void sync_all(void)
{
	struct buf held = { 0 };

	if (!in_passthrough())
		return;
	held_want(&held);
	omcid_sync(buf_str(&held));
	buf_free(&held);
	ipoes_sync();
	sessions_sync();
	feeds_all();
	/* The connections' GEM ports, which the OLT may have moved, carry the networks' flows. */
	lans_fw();
}

/*
 * Take $1 for this process: a symbolic link to its PID, which symlink makes,
 * or fails to make, in one step. One whose process is gone is taken over.
 * Fails while a live process holds it.
 */
static bool take(const char *path)
{
	char pid[32], holder[64];
	ssize_t n;

	snprintf(pid, sizeof(pid), "%d", (int)getpid());
	if (!symlink(pid, path))
		return true;
	n = readlink(path, holder, sizeof(holder) - 1);
	if (n > 0) {
		char *end;
		long h;

		holder[n] = '\0';
		h = strtol(holder, &end, 10);
		if (!*end && h > 0 && !kill((pid_t)h, 0))
			return false;
	}
	rm_f(path);
	return !symlink(pid, path);
}

static bool g_locked;

static void unlock(void)
{
	if (g_locked)
		unlink(LOCK);
}

static void on_signal(int sig)
{
	(void)sig;
	if (g_locked)
		unlink(LOCK);
	_exit(1);
}

/*
 * One onu-apply at a time: the panel's reload, netifd's events, omcid's and
 * dhcp6d's changes each run one. The stick's kernel has no flock
 * (FILE_LOCKING); the lock is LOCK, held until this process ends. Without
 * /run there is nothing to lock, nor to change.
 */
static void lock(void)
{
	struct sigaction sa = { .sa_handler = on_signal };

	while (!take(LOCK)) {
		if (!is_dir(RUN))
			exit(1);
		sleep_tenths(1);
	}
	g_locked = true;
	atexit(unlock);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

/* The values of the lines of TEXT that begin with KEY=, one a line. */
static void key_values(const char *text, const char *key, bool second_only, struct buf *out)
{
	struct words lines = { 0 };
	size_t len = strlen(key);

	buf_reset(out);
	words_lines(&lines, text);
	for (size_t i = 0; i < lines.n; i++) {
		const char *v;

		if (strncmp(lines.v[i], key, len) || lines.v[i][len] != '=')
			continue;
		v = lines.v[i] + len + 1;
		if (out->len)
			buf_puts(out, "\n");
		/* cut -d= -f2 takes the field up to the next '='; -f2- the rest. */
		buf_add(out, v, second_only ? strcspn(v, "=") : strlen(v));
	}
	words_free(&lines);
}

/*
 * The identity to the PON driver, which takes it while the PON is down, then
 * the PON up. Twenty hex digits of PLOAM password are its ten octets.
 *
 * An ONU the OLT disabled stays disabled across a restart, until the OLT
 * enables it (ITU-T G.984.3 clause 10.2.4 and clause 11.1.2): pon keep keeps
 * the disable in the configuration store, PON_DISABLED, and the driver takes
 * it here, before the PON comes up, which then comes up in O7, with the laser
 * off. Only the disable is given, never its end: an enable is the OLT's, or
 * pon enable's. A driver that does not take it leaves the PON down, dark.
 */
static int identity(void)
{
	struct buf text = { 0 }, serial = { 0 }, ploam = { 0 }, disabled = { 0 };
	int rc = 0;

	cat(IDENTITY, &text);
	key_values(buf_str(&text), "serial", true, &serial);
	key_values(buf_str(&text), "ploam_password", false, &ploam);
	if (!serial.len) {
		log_msg("no serial number: the PON stays down");
		rc = 1;
		goto out;
	}
	if (run(0, "onu-nl", "param", "gpon_serial", serial.p, NULL))
		log_msg("the PON driver did not take the serial number");
	if (ploam.len) {
		struct buf value = { 0 };

		if (only_of(ploam.p, "0123456789ABCDEFabcdef") && ploam.len == 20)
			buf_puts(&value, "0x");
		buf_puts(&value, ploam.p);
		if (run(0, "onu-nl", "param", "gpon_password", value.p, NULL))
			log_msg("the PON driver did not take the PLOAM password");
		buf_free(&value);
	}
	run_out(RUN_QUIET_ERR, &disabled, "onu-config", "get", "PON_DISABLED", NULL);
	if (!strcmp(buf_str(&disabled), "1")) {
		if (run(0, "onu-nl", "param", "gpon_disabled", "true", NULL)) {
			log_msg("the OLT disabled this ONU, and the PON driver cannot keep it disabled: the PON stays down (pon enable forgets the disable)");
			rc = 1;
			goto out;
		}
		log_msg("the OLT disabled this ONU: the PON comes up in O7, laser off, until the OLT enables it (pon enable forgets the disable)");
	}
	if (run(0, "onu-nl", "link", "set", "pon", "up", NULL))
		log_msg("the PON does not come up");
out:
	buf_free(&text);
	buf_free(&serial);
	buf_free(&ploam);
	buf_free(&disabled);
	return rc;
}

/*
 * A new identity: the OLT sees the stick leave and a new one arrive, and
 * omcid starts again; it goes on from the MIB it kept in /run/omcid/mib until
 * the OLT builds a new one for it.
 */
static int reidentify(void)
{
	char path[PATH_MAX], want[VAL] = "", rest[VAL];
	int i;

	/*
	 * The sessions end first, while the PON still carries their PADT: a
	 * concentrator keeps a session it is not told of until its LCP gives up
	 * (seen: a session of before stayed beside the new one). They come back
	 * once the OLT provisions the new identity's GEM ports (sync).
	 */
	if (in_passthrough()) {
		load();
		ipoes_down();
		sessions_down();
	}
	run(0, "onu-nl", "link", "set", "pon", "down", NULL);
	if (identity())
		return 1;
	if (readable(PATHF(path, "%s/run/onu-omcid", g_root)))
		read_words(path, want, rest);
	svc_stop("omcid");
	/*
	 * procd ends it in its own time: until it has, omcid_run would find the
	 * daemon on its way out and signal it, rather than start one.
	 */
	for (i = 0; omcid_runs() && i < 50; i++)
		sleep_tenths(1);
	if (omcid_runs()) {
		log_msg("omcid did not end in 5 s: killed");
		run(0, "killall", "-KILL", "omcid", NULL);
		sleep_tenths(1);
	}
	omcid_run(strcmp(want, "router") ? "bridge" : "router");
	log_msg("registering again");
	return 0;
}

/*
 * The settings in force, /run/onu.conf and the sessions' accounts, copied
 * to the directory $1 whole, readable by root alone; nothing is left of
 * what $1 held.
 */
static bool settings_copy(const char *dir)
{
	char tmp[PATH_MAX], path[PATH_MAX], to[PATH_MAX];
	mode_t mask;
	bool ok;

	PATHF(tmp, "%s.new", dir);
	rm_rf(tmp);
	mask = umask(077);
	ok = !mkdir(tmp, 0777) && copy_file(CONF, PATHF(to, "%s/onu.conf", tmp));
	for (int n = 1; ok && n <= MAX_SESSIONS; n++)
		if (exists(PATHF(path, "%s/run/onu-pppoe%d", g_root, n)))
			ok = copy_file(path, PATHF(to, "%s/onu-pppoe%d", tmp, n));
	umask(mask);
	if (!ok) {
		rm_rf(tmp);
		log_msg("the settings were not copied to %s", dir);
		return false;
	}
	rm_rf(dir);
	return !rename(tmp, dir);
}

static void apply(void)
{
	char path[PATH_MAX], old[VAL] = "";
	struct buf want = { 0 };

	if (readable(PATHF(path, "%s/run/onu-mode", g_root)))
		read_line(path, old);
	load_mode();
	/*
	 * From passthrough to passthrough nothing comes down here: the sessions
	 * whose makings changed go down and up again in sessions_sync(), and
	 * the others stay up.
	 */
	if (!strcmp(old, "passthrough") && strcmp(g_mode, "passthrough")) {
		/* Down with the settings it came up with. */
		if (readable(CONF_MODE))
			CONF = CONF_MODE;
		load();
		pass_down();
		CONF = CONF_MAIN;
		load_mode();
	}
	omcid_rules();
	omcid_igmp();
	omcid_want(&want);
	omcid_sync(buf_str(&want));
	buf_free(&want);
	write_line(path, g_mode);
	if (!strcmp(g_mode, "passthrough")) {
		pass_up();
		lans_up();
	}
	copy_file(CONF, CONF_MODE);
	settings_copy(APPLIED);
	log_msg("%s mode", g_mode);
	sync_all();
	guard_fw();
	dos_switch();
}

/* Seconds of /proc/uptime. */
static long uptime_s(void)
{
	char path[PATH_MAX], up[VAL], rest[VAL];

	read_words(PATHF(path, "%s/proc/uptime", g_root), up, rest);
	return num_of(up);
}

/*
 * The change onu-config change left in PENDING put in force: each account it
 * changed, an empty file for one it removed, then the settings. Until then
 * an onu-apply sync applies the settings in force alone.
 */
static void pending_take(void)
{
	char from[PATH_MAX], to[PATH_MAX];

	if (!is_dir(PENDING))
		return;
	for (int n = 1; n <= MAX_SESSIONS; n++) {
		if (!exists(PATHF(from, "%s/onu-pppoe%d", PENDING, n)))
			continue;
		PATHF(to, "%s/run/onu-pppoe%d", g_root, n);
		if (nonempty(from)) {
			rename(from, to);
		} else {
			rm_f(from);
			rm_f(to);
		}
	}
	if (exists(PATHF(from, "%s/onu.conf", PENDING)))
		rename(from, CONF_MAIN);
	accounts_to_pppoed(0);
	rm_rf(PENDING);
}

/*
 * A change on trial (ADR 0011): the settings of /run/onu.conf and the accounts
 * applied but not saved, and the ones applied before kept aside. Unless it
 * is confirmed in TRIAL_LIMIT seconds, the stick goes back to those, so that
 * a change that cut off whoever made it comes undone. A change during a
 * trial starts it again, and goes back to the settings of before the first.
 * The keeper of the deadlines (onu-change watch) takes it back, with the
 * token, which keeps it from ending a later trial.
 */
static int try_change(void)
{
	char dir[PATH_MAX], tmp[PATH_MAX];
	struct buf b = { 0 };
	bool ok;

	if (!exists(TRIAL)) {
		rm_rf(KEPT);
		if (is_dir(APPLIED) && !copy_tree(APPLIED, KEPT))
			log_msg("the settings in force are not kept: an unconfirmed change stays");
	}
	pending_take();
	apply();
	snprintf(dir, sizeof(dir), "%s", TRIAL);
	*strrchr(dir, '/') = '\0';
	buf_printf(&b, "UNTIL=%ld\nTOKEN=%d\n", uptime_s() + g_trial_limit, (int)getpid());
	PATHF(tmp, "%s.new", TRIAL);
	ok = mkdir_p(dir) && write_text(tmp, b.p, b.len) && !rename(tmp, TRIAL);
	buf_free(&b);
	if (!ok)
		return 1;
	log_msg("the change is on trial for %ld s", g_trial_limit);
	return 0;
}

/* The change on trial kept, in the configuration store as well. */
static void confirm(void)
{
	if (!exists(TRIAL))
		return;
	if (have("onu-config") && run(0, "onu-config", "save", NULL))
		log_msg("the change is kept until the next start only");
	rm_f(TRIAL);
	rm_rf(KEPT);
	log_msg("the change is confirmed");
}

/* The token of the trial, the TOKEN= lines of TRIAL. */
static void trial_value(const char *key, struct buf *out)
{
	struct buf text = { 0 };

	cat(TRIAL, &text);
	key_values(buf_str(&text), key, false, out);
	buf_free(&text);
}

/* The trial named by $1, its token, ended: back to the settings kept. */
static void expire(const char *token)
{
	char path[PATH_MAX], tmp[PATH_MAX], from[PATH_MAX], to[PATH_MAX];
	struct buf t = { 0 };
	bool mine;

	if (!readable(TRIAL))
		return;
	trial_value("TOKEN", &t);
	mine = !strcmp(buf_str(&t), token);
	buf_free(&t);
	if (!mine)
		return;
	rm_f(TRIAL);
	PATHF(tmp, "%s.new", CONF_MAIN);
	if (readable(PATHF(path, "%s/onu.conf", KEPT)) && copy_file(path, tmp) && !chmod(tmp,
											 0644)) {
		for (int n = 1; n <= MAX_SESSIONS; n++)
			rm_f(PATHF(to, "%s/run/onu-pppoe%d", g_root, n));
		for (int n = 1; n <= MAX_SESSIONS; n++)
			if (exists(PATHF(from, "%s/onu-pppoe%d", KEPT, n)))
				rename(from, PATHF(to, "%s/run/onu-pppoe%d", g_root, n));
		accounts_to_pppoed(0);
		rename(tmp, CONF_MAIN);
	}
	rm_rf(KEPT);
	rm_f(tmp);
	apply();
	log_msg("the change was not confirmed: back to the settings of before");
}

/*
 * The state last brought up, as the lines "name=value" for the status daemon,
 * which asks for them through onu-act and reads no file of /run itself: the
 * mode, the name servers, the address handed to the router and where, what
 * became of the settings in the store, each session's facts (session.N.dev,
 * gem, hands, ipv6, lan, gateway), each IPoE connection's (ipoe.K.dev, gem,
 * ipv4, router, dns, ipv6, prefix, and lease, the seconds left of a DHCP
 * lease), and when each change on trial goes back (trial.NAME, and
 * trial.mgmt.address). A fact that is not there is left out. This is the one
 * place that names these files for a reader, and the lines are the contract:
 * the files themselves may change under it.
 */
static void state_line(const char *name, const char *path)
{
	char v[VAL];

	if (readable(path) && *read_line(path, v))
		printf("%s=%s\n", name, v);
}

static void state(void)
{
	static const char *const sfacts[] = { "dev", "gem", "hands", "ipv6", "lan", "gateway" };
	static const char *const ifacts[] = { "dev", "gem", "ipv4", "router", "dns", "ipv6",
					      "prefix" };
	struct words names = { 0 }, lines = { 0 };
	char path[PATH_MAX], name[64];
	struct buf out = { 0 };
	long up;

	state_line("mode", PATHF(path, "%s/run/onu-mode", g_root));
	state_line("dns", PATHF(path, "%s/run/onu-dns", g_root));
	if (readable(PATHF(path, "%s/run/onu-pass", g_root))) {
		state_line("pass.address", path);
		state_line("pass.lan", PATHF(path, "%s/run/onu-lan", g_root));
		state_line("pass.peer_gateway", PATHF(path, "%s/run/onu-pass-proxy", g_root));
	}
	state_line("store", PATHF(path, "%s/run/onu-store", g_root));
	for (int n = 1; n <= MAX_SESSIONS; n++)
		for (size_t f = 0; f < ARRAY_SIZE(sfacts); f++) {
			snprintf(name, sizeof(name), "session.%d.%s", n, sfacts[f]);
			state_line(name, PATHF(path, "%s/%d/%s", SESSIONS, n, sfacts[f]));
		}
	up = uptime_s();
	dir_names(IPOE_DIR, &names);
	for (size_t i = 0; i < names.n; i++) {
		char until[VAL] = "", *end;
		long u;

		if (!readable(PATHF(path, "%s/%s/dev", IPOE_DIR, names.v[i])))
			continue;
		for (size_t f = 0; f < ARRAY_SIZE(ifacts); f++) {
			snprintf(name, sizeof(name), "ipoe.%s.%s", names.v[i], ifacts[f]);
			state_line(name, PATHF(path, "%s/%s/%s", IPOE_DIR, names.v[i], ifacts[f]));
		}
		if (readable(PATHF(path, "%s/%s/until", IPOE_DIR, names.v[i])))
			read_line(path, until);
		u = strtol(until, &end, 10);
		if (*until && !*end && u > up)
			printf("ipoe.%s.lease=%ld\n", names.v[i], u - up);
	}
	fflush(stdout);
	/* onu-change status: "NAME UNTIL", and for mgmt "NAME UNTIL ADDRESS". */
	run_out(RUN_QUIET_ERR, &out, "onu-change", "status", NULL);
	words_lines(&lines, buf_str(&out));
	for (size_t i = 0; i < lines.n; i++) {
		char word[2][VAL];
		const char *s = lines.v[i], *e;

		/* read -r NAME UNTIL ADDRESS: the address takes the rest of the line. */
		for (int w = 0; w < 2; w++) {
			size_t n;

			s += strspn(s, " \t");
			n = strcspn(s, " \t");
			snprintf(word[w], VAL, "%.*s", (int)n, s);
			s += n;
		}
		s += strspn(s, " \t");
		e = s + strlen(s);
		while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		if (!*word[1])
			continue;
		printf("trial.%s=%s\n", word[0], word[1]);
		if (e > s)
			printf("trial.%s.address=%.*s\n", word[0], (int)(e - s), s);
	}
	words_free(&names);
	words_free(&lines);
	buf_free(&out);
}

static void status(void)
{
	struct words names = { 0 };
	char path[PATH_MAX], mode[VAL] = "", a[VAL], b[VAL];
	struct buf t = { 0 };

	if (readable(PATHF(path, "%s/run/onu-mode", g_root)))
		read_line(path, mode);
	printf("mode %s\n", *mode ? mode : "none");
	dir_names(SESSIONS, &names);
	for (size_t i = 0; i < names.n; i++) {
		if (!readable(PATHF(path, "%s/%s/dev", SESSIONS, names.v[i])))
			continue;
		/* Each file read in a step of its own: path is the one buffer. */
		cat_val(path, a);
		cat_val(PATHF(path, "%s/%s/family", SESSIONS, names.v[i]), b);
		printf("session %s on %s (%s)", names.v[i], a, b);
		if (readable(PATHF(path, "%s/%s/ppp", SESSIONS, names.v[i])))
			printf(", up on %s", cat_val(path, a));
		if (readable(PATHF(path, "%s/%s/ipv4", SESSIONS, names.v[i])))
			printf(", %s", cat_val(path, a));
		printf("\n");
	}
	if (nonempty(PATHF(path, "%s/run/onu-dns", g_root)))
		printf("dns %s\n", cat_val(path, a));
	if (readable(TRIAL)) {
		trial_value("UNTIL", &t);
		printf("trial until %s\n", buf_str(&t));
	}
	for (size_t i = 0; i < names.n; i++) {
		if (!readable(PATHF(path, "%s/%s/pass", SESSIONS, names.v[i])))
			continue;
		cat_val(path, a);
		printf("pass %s on %s from session %s\n", a,
		       cat_val(PATHF(path, "%s/%s/lan", SESSIONS, names.v[i]), b), names.v[i]);
	}
	words_free(&names);
	buf_free(&t);
}

static bool is_one_of(const char *s, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(s, *list))
			return true;
	return false;
}

int main(int argc, char **argv)
{
	static const char *const locked[] = { "start", "reload", "try", "confirm", "revert",
					      "expire", "stop", "identity", "reidentify", "iface",
					      "guard", NULL };
	static const char *const syncs[] = { "sync", "omcid", "dhcp6d", "dhcpc", NULL };
	const char *cmd = argc > 1 ? argv[1] : "";
	char path[PATH_MAX], v[VAL];

	paths();
	openlog("onu-apply", 0, LOG_USER);
	signal(SIGPIPE, SIG_IGN);
	if (is_one_of(cmd, locked))
		lock();
	if (!strcmp(cmd, "start")) {
		int s[MAX_SESSIONS];

		if (!exists(CONF_MAIN) && !copy_file(PATHF(path, "%s/etc/onu.conf", g_root),
						     CONF_MAIN))
			write_text(CONF_MAIN, "", 0);
		if (!sessions(s) && readable(PATHF(path, "%s/etc/onu-pppoe", g_root))) {
			mode_t mask = umask(077);

			copy_file(path, PATHF(v, "%s/run/onu-pppoe1", g_root));
			umask(mask);
		}
		apply();
	} else if (!strcmp(cmd, "reload")) {
		if (have("onu-config") && run(0, "onu-config", "save", NULL))
			log_msg("the settings are applied but not saved");
		/* What is saved is kept: no trial goes back on it. */
		rm_f(TRIAL);
		rm_rf(KEPT);
		apply();
	} else if (!strcmp(cmd, "try")) {
		return try_change();
	} else if (!strcmp(cmd, "confirm")) {
		confirm();
	} else if (!strcmp(cmd, "revert")) {
		struct buf t = { 0 };

		if (!readable(TRIAL))
			return 1;
		trial_value("TOKEN", &t);
		expire(buf_str(&t));
		buf_free(&t);
	} else if (!strcmp(cmd, "expire")) {
		expire(argc > 2 ? argv[2] : "");
	} else if (!strcmp(cmd, "stop")) {
		load();
		if (in_passthrough())
			pass_down();
		svc_stop("omcid");
		static const char *const gone[] = { "onu-mode", "onu-omcid", "onu-held-vlans",
						     "omcid.mode", "omcid/applied", NULL };

		for (const char *const *f = gone; *f; f++)
			rm_f(PATHF(path, "%s/run/%s", g_root, *f));
	} else if (!strcmp(cmd, "identity")) {
		return identity();
	} else if (!strcmp(cmd, "reidentify")) {
		return reidentify();
	} else if (!strcmp(cmd, "status")) {
		status();
	} else if (!strcmp(cmd, "state")) {
		state();
	} else if (is_one_of(cmd, syncs)) {
		/*
		 * A change told while another waits for the lock is that one's too:
		 * at most one waits, and it reads everything once it holds the lock.
		 */
		if (!take(AGAIN))
			return 0;
		lock();
		rm_f(AGAIN);
		load();
		sync_all();
		guard_fw();
	} else if (!strcmp(cmd, "iface")) {
		load();
		iface_event();
	} else if (!strcmp(cmd, "guard")) {
		struct words names = { 0 };

		dir_names(SESSIONS, &names);
		for (size_t i = 0; i < names.n; i++)
			if (readable(PATHF(path, "%s/%s/ppp", SESSIONS, names.v[i]))) {
				cat_val(path, v);
				if (exists(PATHF(path, "%s/%s/up", SESSIONS, names.v[i])))
					guard(v);
			}
		words_free(&names);
		dir_names(IPOE_DIR, &names);
		for (size_t i = 0; i < names.n; i++)
			if (readable(PATHF(path, "%s/%s/dev", IPOE_DIR, names.v[i])))
				guard(cat_val(path, v));
		words_free(&names);
		load();
		guard_fw();
	} else {
		fputs("usage: onu-apply start|reload|try|confirm|revert|stop|identity|reidentify|status|state|sync|guard|dhcpc\n",
		      stderr);
		return 2;
	}
	return 0;
}
