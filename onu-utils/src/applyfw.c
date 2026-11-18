// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyfw: onu-fw's files: the NAT and the protection of the local networks
 * with the forwards, the DMZs and the firewall's rules, those rules in the
 * switch's ACL, the guard of the stick's own programs, and the switch's checks
 * of malformed and hostile frames.
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
#include "applyfw.h"
#include "applyutil.h"

static void conns_in(struct buf *out);

/* The firewall's rules out of the switch's ACL, once they were put in. */
static void acl_clear(void)
{
	char conf[PATH_MAX], path[PATH_MAX];

	if (!exists(PATHF(conf, "%s/run/onu-fw-acl.conf", g_root)))
		return;
	if (run(0, "onu-fw", "acl-clear", NULL))
		log_msg("onu-fw did not take the firewall's rules out of the ACL");
	rm_f(conf);
	rm_f(PATHF(path, "%s/run/onu-fw.acl", g_root));
}

/* onu-fw's table away, once one was put in place, and the firewall's rules out of the ACL. */
void fw_clear(void)
{
	char conf[PATH_MAX];

	acl_clear();
	if (!exists(PATHF(conf, "%s/run/onu-fw.conf", g_root)))
		return;
	if (run(0, "onu-fw", "clear", NULL))
		log_msg("onu-fw did not take its table away");
	rm_f(conf);
}

/*
 * $1 into the file $2 of onu-fw's when it differs from what is there, written
 * as $3, $2 with ".new", and renamed: true when it is new.
 */
static bool fw_file(const struct buf *text, const char *path, const char *tmp)
{
	struct buf now = { 0 }, have = { 0 };
	bool same = false;

	write_text(tmp, buf_str(text), text->len);
	if (exists(path) && cat(path, &have)) {
		cat(tmp, &now);
		same = !strcmp(buf_str(&now), buf_str(&have));
	}
	buf_free(&now);
	buf_free(&have);
	if (same) {
		rm_f(tmp);
		return false;
	}
	rename(tmp, path);
	return true;
}

/* The rules of CONF by their place, 1 to MAX_RULES, and the devices they name. */
struct rules {
	char *at[MAX_RULES + 1];
	struct words named;
};

static void rules_read(const struct words *lines, struct rules *r)
{
	memset(r, 0, sizeof(*r));
	for (size_t i = 0; i < lines->n; i++) {
		const char *eq = keyed(lines->v[i], "RULE", true);
		struct words f = { 0 };
		long k;

		if (!eq || !eq[1])
			continue;
		k = num_of(lines->v[i] + 4);
		if (k >= 1 && k <= MAX_RULES) {
			free(r->at[k]);
			r->at[k] = strdup(eq + 1);
			if (!r->at[k])
				out_of_memory();
		}
		words_fields(&f, eq + 1, "_");
		for (size_t e = 2; e <= 3 && f.n == 8; e++)
			if (starts(f.v[e], "dev:")) {
				char named[VAL], m[VAL];

				snprintf(named, sizeof(named), "%.1s/%s", f.v[e] + 4,
					 lower(strlen(f.v[e]) > 6 ? f.v[e] + 6 : "", m));
				if (!words_has(&r->named, named))
					words_add(&r->named, named);
			}
		words_free(&f);
	}
}

static void rules_free(struct rules *r)
{
	for (int k = 0; k <= MAX_RULES; k++)
		free(r->at[k]);
	words_free(&r->named);
}

/*
 * The firewall's rules in the switch's ACL (ADR 0019), as onu-fw's file of
 * them: each network with its addresses and the connection it goes out by,
 * each connection that has an address now with its interface, the fixed
 * address of each device a rule names, the forwards and the DMZs in force, $1
 * (forwards_fw()), and the rules in their order. onu-fw compiles them to the
 * ACL's entries, and writes what became of each rule to /run/onu-fw.acl.
 * The entries from the internet name the connection's address, a lease's or
 * a session's, so they run again with lans_fw() whenever one comes or goes.
 * Without a rule in force the ACL holds none of them.
 */
static void acl_fw(const char *fwd)
{
	char conf[PATH_MAX], tmp[PATH_MAX], acl[PATH_MAX], net[VAL];
	struct words lines = { 0 }, conns = { 0 };
	struct buf b = { 0 }, cin = { 0 };
	struct rules r;
	int l[MAX_LANS], c;
	bool any = false;

	conf_lines(&lines);
	for (size_t i = 0; i < lines.n && !any; i++) {
		size_t len = strlen(lines.v[i]);

		any = keyed(lines.v[i], "RULE", false) && len >= 3 && !strcmp(lines.v[i] + len - 3,
									      "_on");
	}
	if (!any) {
		acl_clear();
		words_free(&lines);
		return;
	}
	c = lans(l);
	for (int i = 0; i < c; i++)
		buf_printf(&b, "lan %d %s %s\n", l[i], lan_net(l[i], net), lan_conn(l[i]));
	conns_in(&cin);
	words_lines(&conns, buf_str(&cin));
	for (size_t i = 0; i < conns.n; i++)
		buf_printf(&b, "wan %s\n", conns.v[i]);
	rules_read(&lines, &r);
	for (size_t i = 0; i < lines.n; i++) {
		const char *eq = keyed(lines.v[i], "FIXED", true);
		struct words f = { 0 };
		char named[VAL], m[VAL];

		if (!eq || !eq[1])
			continue;
		words_fields(&f, eq + 1, "/");
		snprintf(named, sizeof(named), "%s/%s", field(&f, 0), lower(field(&f, 1), m));
		if (words_has(&r.named, named))
			buf_printf(&b, "dev %s %s %s\n", field(&f, 0), m, field(&f, 2));
		words_free(&f);
	}
	for (int k = 1; k <= MAX_RULES; k++)
		if (r.at[k])
			buf_printf(&b, "rule %s\n", r.at[k]);
	if (*fwd)
		buf_printf(&b, "%s\n", fwd);
	PATHF(conf, "%s/run/onu-fw-acl.conf", g_root);
	PATHF(tmp, "%s/run/onu-fw-acl.conf.new", g_root);
	PATHF(acl, "%s/run/onu-fw.acl", g_root);
	if (fw_file(&b, conf, tmp) && run(0, "onu-fw", "acl", conf, acl, NULL)) {
		log_msg("onu-fw did not put the firewall's rules into the switch's ACL");
		rm_f(conf);
	}
	rules_free(&r);
	words_free(&lines);
	words_free(&conns);
	buf_free(&b);
	buf_free(&cin);
}

/*
 * The interface of each connection that has an IPv4 address, and the address,
 * "pppoe:N IFNAME ADDR" or "ipoe:K IFNAME ADDR", a line each: a session's
 * once onu-apply took its address (session_brought()), its interface pppN,
 * which the kernel names as it comes up; an IPoE connection's while it has
 * one, typed in or from a lease.
 */
static void conns_in(struct buf *out)
{
	struct words names = { 0 };
	char path[PATH_MAX], a[VAL], b[VAL], host[VAL];

	buf_reset(out);
	dir_names(SESSIONS, &names);
	for (size_t i = 0; i < names.n; i++) {
		if (!readable(PATHF(path, "%s/%s/ppp", SESSIONS, names.v[i])))
			continue;
		cat_val(path, a);
		if (!nonempty(PATHF(path, "%s/%s/up", SESSIONS, names.v[i])))
			continue;
		buf_printf(out, "pppoe:%s %s %s\n", names.v[i], a, cat_val(path, b));
	}
	words_free(&names);
	dir_names(IPOE_DIR, &names);
	for (size_t i = 0; i < names.n; i++) {
		if (!readable(PATHF(path, "%s/%s/dev", IPOE_DIR, names.v[i])))
			continue;
		cat_val(path, a);
		if (!nonempty(PATHF(path, "%s/%s/ipv4", IPOE_DIR, names.v[i])))
			continue;
		buf_printf(out, "ipoe:%s %s %s\n", names.v[i], a,
			   addr_only(cat_val(path, b), host));
	}
	words_free(&names);
	buf_chomp(out);
}

/* Whether $1 is a line PREFIXn=VALUE with a number and a value. */
static bool has_value(const char *s, const char *prefix)
{
	const char *eq = keyed(s, prefix, true);

	return eq && eq[1];
}

/*
 * The forwards in force, FORWARDk ("CONN/PROTO/PORTS/L/MAC/PORT/ON"), and
 * the DMZs, DMZk ("CONN/L/MAC"), as lines of onu-fw's table, into OUT: each
 * comes in by its connection's interface to the connection's address there,
 * while it has one, and goes to its device's fixed address, FIXEDk
 * ("L/MAC/IP"); a forward of both protocols is one of each.
 */
static void forwards_fw(struct buf *out)
{
	struct words lines = { 0 }, conns = { 0 }, fwd = { 0 }, dmz = { 0 };
	struct map at = { 0 }, ip = { 0 };
	struct buf cin = { 0 };
	char key[2 * VAL], m[VAL];

	buf_reset(out);
	conns_in(&cin);
	words_lines(&conns, buf_str(&cin));
	for (size_t i = 0; i < conns.n; i++) {
		struct words w = { 0 };

		words_split(&w, conns.v[i]);
		if (w.n == 3) {
			snprintf(key, sizeof(key), "%s %s", w.v[1], w.v[2]);
			map_set(&at, w.v[0], key);
		}
		words_free(&w);
	}
	conf_lines(&lines);
	for (size_t i = 0; i < lines.n; i++) {
		const char *s = lines.v[i];

		if (has_value(s, "FIXED")) {
			struct words f = { 0 };

			words_fields(&f, s, "=/");
			snprintf(key, sizeof(key), "%s/%s", field(&f, 1), lower(field(&f, 2), m));
			map_set(&ip, key, field(&f, 3));
			words_free(&f);
		} else if (has_value(s, "FORWARD")) {
			words_add(&fwd, s);
		} else if (has_value(s, "DMZ")) {
			words_add(&dmz, s);
		}
	}
	for (size_t i = 0; i < fwd.n; i++) {
		struct words f = { 0 };
		const char *dev, *where;

		words_fields(&f, fwd.v[i], "=/");
		snprintf(key, sizeof(key), "%s/%s", field(&f, 4), lower(field(&f, 5), m));
		dev = map_get(&ip, key);
		where = map_get(&at, field(&f, 1));
		if (!strcmp(field(&f, 7), "on") && where && dev && *dev) {
			if (strcmp(field(&f, 2), "udp"))
				buf_printf(out, "fwd %s tcp %s %s %s\n", where, field(&f, 3), dev,
					   field(&f, 6));
			if (strcmp(field(&f, 2), "tcp"))
				buf_printf(out, "fwd %s udp %s %s %s\n", where, field(&f, 3), dev,
					   field(&f, 6));
		}
		words_free(&f);
	}
	for (size_t i = 0; i < dmz.n; i++) {
		struct words f = { 0 };
		const char *dev, *where;

		words_fields(&f, dmz.v[i], "=/");
		snprintf(key, sizeof(key), "%s/%s", field(&f, 2), lower(field(&f, 3), m));
		dev = map_get(&ip, key);
		where = map_get(&at, field(&f, 1));
		if (where && dev && *dev)
			buf_printf(out, "dmz %s %s\n", where, dev);
		words_free(&f);
	}
	buf_chomp(out);
	words_free(&lines);
	words_free(&conns);
	words_free(&fwd);
	words_free(&dmz);
	map_free(&at);
	map_free(&ip);
	buf_free(&cin);
}

/*
 * The words of an end of a rule, $1, from (S) or to (D), into OUT; false for
 * one that cannot be of IPv4 here.
 */
static bool end_of(const char *e, bool from, const struct map *ifn, const struct map *ip, char *out)
{
	char l[2] = { 0 }, m[VAL], key[2 * VAL];
	const char *iface, *a;

	out[0] = '\0';
	if (!strcmp(e, "internet") || !strcmp(e, "lans"))
		return true;
	if (starts(e, "internet:")) {
		if (strchr(e + 9, ':'))
			return false;
		valf(out, " %saddr %s", from ? "s" : "d", e + 9);
		return true;
	}
	l[0] = strlen(e) > 4 ? e[4] : '\0';
	iface = map_get(ifn, l);
	if (!iface)
		return false;
	if (starts(e, "lan:")) {
		valf(out, " %s %s", from ? "iif" : "oif", iface);
		return true;
	}
	lower(strlen(e) > 6 ? e + 6 : "", m);
	if (from) {
		valf(out, " iif %s ether %s", iface, m);
		return true;
	}
	snprintf(key, sizeof(key), "%s/%s", l, m);
	a = map_get(ip, key);
	if (!a || !*a)
		return false;
	valf(out, " oif %s daddr %s", iface, a);
	return true;
}

/*
 * The firewall's defaults and its rules that allow, in force, as lines of
 * onu-fw's table. A rule's way is in from the internet, out to it, or
 * between the networks; what it comes from is an interface and a MAC address
 * there, or a part of the internet, and what it goes to an interface and the
 * fixed address of a device there, FIXEDk ("L/MAC/IP"), or a part of the
 * internet; "lans" and "internet" name no more than the way does. A rule of
 * both TCP and UDP is one of each. A rule of IPv6 alone, or of an address
 * of IPv6, is the ACL's alone: the stick routes no IPv6 to the networks'
 * devices yet.
 */
static void rules_fw(struct buf *b)
{
	const char *out = kv_get("FW_OUT"), *between = kv_get("FW_BETWEEN");
	struct words lines = { 0 };
	struct map ifn = { 0 }, ip = { 0 };
	struct rules r;
	int l[MAX_LANS], c = lans(l);

	buf_printf(b, "default out %s\n", *out ? out : "allow");
	buf_printf(b, "default between %s\n", *between ? between : "block");
	for (int i = 0; i < c; i++) {
		char n[16], v[VAL];

		snprintf(n, sizeof(n), "%d", l[i]);
		map_set(&ifn, n, lan_if(l[i], v));
	}
	conf_lines(&lines);
	for (size_t i = 0; i < lines.n; i++) {
		const char *eq = keyed(lines.v[i], "FIXED", true);
		struct words f = { 0 };
		char key[2 * VAL], m[VAL];

		if (!eq || !eq[1])
			continue;
		words_fields(&f, eq + 1, "/");
		snprintf(key, sizeof(key), "%s/%s", field(&f, 0), lower(field(&f, 1), m));
		map_set(&ip, key, field(&f, 2));
		words_free(&f);
	}
	rules_read(&lines, &r);
	for (int k = 1; k <= MAX_RULES; k++) {
		struct words f = { 0 };
		char from[VAL], to[VAL], line[3 * VAL];
		const char *way, *proto, *ports;

		if (!r.at[k])
			continue;
		words_fields(&f, r.at[k], "_");
		if (f.n != 8 || strcmp(f.v[1], "allow") || !strcmp(f.v[6], "ipv6") ||
		    strcmp(f.v[7], "on") || !end_of(f.v[2], true, &ifn, &ip, from) ||
		    !end_of(f.v[3], false, &ifn, &ip, to)) {
			words_free(&f);
			continue;
		}
		way = starts(f.v[2], "internet") ? "in" : starts(f.v[3],
								 "internet") ? "out" : "between";
		snprintf(line, sizeof(line), "allow %s%s%s", way, from, to);
		proto = f.v[4];
		ports = f.v[5];
		if (!strcmp(proto, "icmp"))
			buf_printf(b, "%s icmp\n", line);
		if (!strcmp(proto, "tcp") || !strcmp(proto, "both"))
			buf_printf(b, "%s tcp%s%s\n", line, *ports ? " " : "", ports);
		if (!strcmp(proto, "udp") || !strcmp(proto, "both"))
			buf_printf(b, "%s udp%s%s\n", line, *ports ? " " : "", ports);
		if (!strcmp(proto, "any"))
			buf_printf(b, "%s\n", line);
		words_free(&f);
	}
	rules_free(&r);
	words_free(&lines);
	map_free(&ifn);
	map_free(&ip);
}

/*
 * The NAT the local networks go out through, and the protection that keeps
 * the internet's new connections from them, as onu-fw's table: each
 * network's interface and addresses, the GEM interface of each connection
 * they go out by, whose flows the switch carries, and every way out from
 * the operator's side, the sessions' interfaces and each IPoE connection's,
 * by which no new connection comes in to a network but what a forward or a
 * DMZ takes (forwards_fw()) or a rule lets in, and the firewall's defaults
 * and rules (rules_fw()); and the firewall's rules in the switch's ACL
 * (acl_fw()). Without networks the table goes, and the rules with it. It
 * runs again when a session's address comes or goes, and at each sync,
 * which follows an IPoE connection's lease.
 */
void lans_fw(void)
{
	int l[MAX_LANS], c = lans(l), k[MAX_IPOE], ck;
	struct words conns = { 0 };
	struct buf b = { 0 }, fwd = { 0 };
	char conf[PATH_MAX], tmp[PATH_MAX], path[PATH_MAX], v[VAL], net[VAL];

	if (!c) {
		fw_clear();
		return;
	}
	forwards_fw(&fwd);
	for (int i = 0; i < c; i++)
		buf_printf(&b, "lan %s %s\n", lan_if(l[i], v), lan_net(l[i], net));
	lan_conns(&conns);
	for (size_t i = 0; i < conns.n; i++) {
		if (starts(conns.v[i], "pppoe:"))
			PATHF(path, "%s/%s/gem", SESSIONS, conns.v[i] + 6);
		else
			PATHF(path, "%s/%s/gem", IPOE_DIR,
			      starts(conns.v[i], "ipoe:") ? conns.v[i] + 5 : conns.v[i]);
		if (*cat_val(path, v))
			buf_printf(&b, "wan %s\n", v);
	}
	/* The sessions' interfaces, pppN, and the IPoE connections'. */
	if (have_sessions())
		buf_puts(&b, "out ppp*\n");
	ck = ipoes(k);
	for (int i = 0; i < ck; i++)
		if (readable(PATHF(path, "%s/%d/dev", IPOE_DIR, k[i])))
			buf_printf(&b, "out %s\n", cat_val(path, v));
	if (fwd.len)
		buf_printf(&b, "%s\n", fwd.p);
	rules_fw(&b);
	/* What onu-fw holds already is not put in place again. */
	PATHF(conf, "%s/run/onu-fw.conf", g_root);
	PATHF(tmp, "%s/run/onu-fw.conf.new", g_root);
	if (fw_file(&b, conf, tmp) && run(0, "onu-fw", "apply", conf, NULL)) {
		log_msg("onu-fw did not take the NAT of the local networks");
		rm_f(conf);
	}
	acl_fw(buf_str(&fwd));
	words_free(&conns);
	buf_free(&b);
	buf_free(&fwd);
}

/*
 * The guard of the stick's own programs, onu-fw's table "inet onu_guard", in
 * every shape of the stick, with networks or without: what comes in to the
 * stick by any interface but the management VLAN's is dropped, but what one
 * of its programs takes there, and nothing is forwarded to that VLAN nor from
 * it. Its file, /run/onu-guard.conf, names the management interfaces by the
 * beginning of their names, mgmt and the mgmtVID of a pair on trial, so that
 * a pair added beside the old one is reached at once; the address on the
 * host link itself, a bench's (DATA_ADDRESS of /run/onu-mgmt), which is a
 * management address too; the sessions' interfaces, pppN, whose dhcp6d asks
 * for a prefix; the interface of each IPoE connection that is up, with its
 * DHCP client when its address comes from DHCP; each link a session hands its
 * address down by, with udhcpd where one hands its IPv4 address and dhcp6d's
 * server where one hands its prefix; each local network's interface, with its
 * DHCP server when it has one; and the sides a ping is answered from. It runs
 * at boot, before the servers start (onu-mgmt start, by "onu-apply guard"),
 * and after each apply and sync; what onu-fw holds already is not put in
 * place again. One that fails leaves the one there was, and is tried again at
 * the next run.
 */
void guard_fw(void)
{
	struct words lines = { 0 }, names = { 0 }, links = { 0 };
	struct buf b = { 0 }, text = { 0 };
	char path[PATH_MAX], conf[PATH_MAX], tmp[PATH_MAX], v[VAL], host[VAL];
	int s[MAX_SESSIONS], cs = sessions(s), l[MAX_LANS], cl = lans(l);

	buf_puts(&b, "mgmt mgmt*\n");
	cat(PATHF(path, "%s/run/onu-mgmt", g_root), &text);
	words_lines(&lines, buf_str(&text));
	for (size_t i = 0; i < lines.n; i++)
		if (starts(lines.v[i], "DATA_ADDRESS=")) {
			if (lines.v[i][13])
				buf_printf(&b, "data sfp %s\n", addr_only(lines.v[i] + 13, host));
			break;
		}
	if (cs)
		buf_puts(&b, "session ppp*\n");
	dir_names(IPOE_DIR, &names);
	for (size_t i = 0; i < names.n; i++) {
		int k = (int)num_of(names.v[i]);
		bool dhcp;

		if (!readable(PATHF(path, "%s/%s/dev", IPOE_DIR, names.v[i])))
			continue;
		dhcp = !strcmp(ipoe_setting(k, "KIND"), "dhcp") && ipoe_carries(k, "ipv4");
		buf_printf(&b, "ipoe %s%s\n", cat_val(path, v), dhcp ? " dhcp" : "");
	}
	pass_links(&links);
	for (size_t i = 0; i < links.n; i++) {
		static const char *const fams[][2] = { { "ipv4", " dhcp" }, { "ipv6", " dhcp6" } };
		char take[32] = "";

		for (size_t f = 0; f < ARRAY_SIZE(fams); f++)
			for (int n = 0; n < cs; n++) {
				char dv[VAL], lk[VAL];

				if (strcmp(link_of(down_vlan(s[n], dv), lk),
					   links.v[i]) || !hands(s[n], fams[f][0]))
					continue;
				strcat(take, fams[f][1]);
				break;
			}
		buf_printf(&b, "link %s%s\n", links.v[i], take);
	}
	for (int i = 0; i < cl; i++)
		buf_printf(&b, "lan %s%s\n", lan_if(l[i], v),
			   strcmp(lan_setting(l[i], "DHCP"), "on") ? "" : " dhcp");
	if (!strcmp(kv_get("FW_PING_IN"), "allow"))
		buf_puts(&b, "ping in\n");
	if (!strcmp(kv_get("FW_PING_LANS"), "allow"))
		buf_puts(&b, "ping lans\n");
	PATHF(conf, "%s/run/onu-guard.conf", g_root);
	PATHF(tmp, "%s/run/onu-guard.conf.new", g_root);
	if (fw_file(&b, conf, tmp) && run(0, "onu-fw", "guard", conf, NULL)) {
		log_msg("onu-fw did not put the guard in place: the one there was stays, if any");
		rm_f(conf);
	}
	words_free(&lines);
	words_free(&names);
	words_free(&links);
	buf_free(&b);
	buf_free(&text);
}

/*
 * The switch's checks of malformed and hostile frames, as DOS_* say
 * (Documentation/networking/devlink/rtl9602c.rst, "DoS checks"): the frames
 * of the ports DOS_SIDE names are judged, each check on dropping what it
 * stops, and a flood check counting against its threshold. A flood check is
 * on with its threshold or off, since a threshold of 0 stops the first frame.
 * What is in force is kept in /run/onu-dos, so that a run that changes nothing
 * writes nothing, and a stick that never had a check on is left as the switch
 * starts, with none. The frames are judged only once the rest is in place, and
 * a parameter the switch does not take leaves them not judged at all.
 */
void dos_switch(void)
{
	char path[PATH_MAX], want[VAL], have[VAL], mask_s[32], ports_s[8];
	const char *side = kv_get("DOS_SIDE");
	struct words th = { 0 };
	unsigned long mask = 0;
	int ports;

	for (size_t i = 0; i < ARRAY_SIZE(DOS_ONOFF); i++)
		if (!strcmp(kv_getf("DOS_%s", DOS_ONOFF[i].name), "on"))
			mask |= 1ul << DOS_ONOFF[i].bit;
	for (size_t i = 0; i < ARRAY_SIZE(DOS_FLOODS); i++) {
		const char *v = kv_getf("DOS_%s", DOS_FLOODS[i].name);
		char t[VAL];

		if (!*v || !strcmp(v, "off"))
			continue;
		if (!only_of(v, "0123456789") || v[0] == '0' || strlen(v) > 3 || num_of(v) > 255) {
			log_msg("DOS_%s: not a threshold; the check stays off", DOS_FLOODS[i].name);
			continue;
		}
		mask |= 1ul << DOS_FLOODS[i].bit;
		snprintf(t, sizeof(t), "%s=%s", DOS_FLOODS[i].param, v);
		words_add(&th, t);
	}
	/* Port 0 is the host's, port 2 the operator's. */
	ports = !strcmp(side, "host") ? 1 : !strcmp(side, "both") ? 5 : 4;
	if (!mask)
		ports = 0;
	snprintf(want, sizeof(want), "%d %lu", ports, mask);
	for (size_t i = 0; i < th.n; i++) {
		size_t len = strlen(want);

		snprintf(want + len, sizeof(want) - len, " %s", th.v[i]);
	}
	PATHF(path, "%s/run/onu-dos", g_root);
	read_line(path, have);
	if (!strcmp(want, *have ? have : "0 0"))
		goto out;
	if (run(0, "onu-nl", "param", "dos_ports", "0", NULL)) {
		log_msg("the switch did not take dos_ports 0: its checks stay as they were");
		goto out;
	}
	for (size_t i = 0; i < th.n; i++) {
		char *eq = strchr(th.v[i], '=');

		*eq = '\0';
		if (run(0, "onu-nl", "param", th.v[i], eq + 1, NULL)) {
			log_msg("the switch did not take %s %s: its checks are off", th.v[i],
				eq + 1);
			rm_f(path);
			goto out;
		}
	}
	snprintf(mask_s, sizeof(mask_s), "%lu", mask);
	snprintf(ports_s, sizeof(ports_s), "%d", ports);
	if (run(0, "onu-nl", "param", "dos_actions", "0", NULL) ||
	    run(0, "onu-nl", "param", "dos_checks", mask_s, NULL) ||
	    run(0, "onu-nl", "param", "dos_ports", ports_s, NULL)) {
		log_msg("the switch did not take its checks: they are off");
		run(RUN_QUIET_ERR, "onu-nl", "param", "dos_ports", "0", NULL);
		rm_f(path);
		goto out;
	}
	write_line(path, want);
out:
	words_free(&th);
}
