// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyconf: the files onu-apply reads and writes, the settings, and what
 * follows from the settings and the accounts alone: which sessions hand what
 * down and by which link, the stick's default routes, the routing tables, the
 * IPoE connections and the local networks set up; and procd's service "onu",
 * a file for each of its programs.
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
#include "applyutil.h"
#include "settings.h"

/* The files it reads and writes, under ONU_ROOT. */
char RUN[PATH_MAX], CONF_MAIN[PATH_MAX], IDENTITY[PATH_MAX], SESSIONS[PATH_MAX];
char IPOE_DIR[PATH_MAX], NET[PATH_MAX], FORWARD[PATH_MAX], FORWARD6[PATH_MAX];
char LIB[PATH_MAX], DROP[PATH_MAX], CALL[PATH_MAX], SVC_DIR[PATH_MAX];
char LOCK[PATH_MAX], AGAIN[PATH_MAX];
char APPLIED[PATH_MAX], KEPT[PATH_MAX], PENDING[PATH_MAX], TRIAL[PATH_MAX];
const char *CONF = CONF_MAIN;
char CONF_MODE[PATH_MAX];
long g_trial_limit = 90;
long g_applied_tenths = 50;
char g_mode[16];
static char g_default_family[16];

const struct dos_check DOS_ONOFF[DOS_NONOFF] = {
	{ "LAND", 1, NULL }, { "BLAT", 2, NULL }, { "SYN_FIN", 3, NULL }, { "XMAS", 4, NULL },
	{ "NULL_SCAN", 5, NULL }, { "SYN_LOW_PORT", 6, NULL }, { "TCP_SHORT", 7, NULL },
	{ "TCP_FRAG", 8, NULL }, { "ICMP_FRAG", 9, NULL }, { "PING_OF_DEATH", 10, NULL },
	{ "UDP_BOMB", 11, NULL }, { "SYN_DATA", 12, NULL },
};

const struct dos_check DOS_FLOODS[DOS_NFLOODS] = {
	{ "SYN_FLOOD", 13, "dos_synflood_thresh" },
	{ "FIN_FLOOD", 14, "dos_finflood_thresh" },
	{ "ICMP_FLOOD", 15, "dos_icmpflood_thresh" },
};

static const char *const SESSION_KEYS[] = { "GEM", "VLAN", "MAC", "FAMILY", "PASS", "HOST_VLAN" };
static const char *const IPOE_KEYS[] = { "KIND", "GEM", "VLAN", "MAC", "FAMILY", "ADDRESS",
					 "GATEWAY", "DNS", "ADDRESS6", "GATEWAY6", "PREFIX6" };
static const char *const LAN_KEYS[] = { "CONN", "VLAN", "ADDRESS", "DHCP", "POOL_FROM",
					"POOL_TO", "LEASE", "IPV6", "IPV6_SUBNET" };
static const char *const FW_KEYS[] = { "OUT", "BETWEEN", "PING_IN", "PING_LANS" };

/* The settings. */

struct kv {
	char *key, *val;
};

static struct kv *g_kv;
static size_t g_nkv, g_ckv;

static void kv_clear(void)
{
	for (size_t i = 0; i < g_nkv; i++) {
		free(g_kv[i].key);
		free(g_kv[i].val);
	}
	g_nkv = 0;
}

static void kv_set(const char *key, const char *val)
{
	char *copy = strdup(val);

	if (!copy)
		out_of_memory();
	for (size_t i = 0; i < g_nkv; i++)
		if (!strcmp(g_kv[i].key, key)) {
			free(g_kv[i].val);
			g_kv[i].val = copy;
			return;
		}
	if (g_nkv == g_ckv) {
		g_ckv = g_ckv ? g_ckv * 2 : 256;
		g_kv = realloc(g_kv, g_ckv * sizeof(*g_kv));
		if (!g_kv)
			out_of_memory();
	}
	g_kv[g_nkv].key = strdup(key);
	if (!g_kv[g_nkv].key)
		out_of_memory();
	g_kv[g_nkv++].val = copy;
}

/* The value of the setting $1, nothing for one not set. */
const char *kv_get(const char *key)
{
	for (size_t i = 0; i < g_nkv; i++)
		if (!strcmp(g_kv[i].key, key))
			return g_kv[i].val;
	return "";
}

__attribute((format(printf, 1, 2))) const char *kv_getf(const char *fmt, ...)
{
	char key[64];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(key, sizeof(key), fmt, ap);
	va_end(ap);
	return kv_get(key);
}

static __attribute((format(printf, 2, 3))) void kv_setf(const char *val, const char *fmt, ...)
{
	char key[64];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(key, sizeof(key), fmt, ap);
	va_end(ap);
	kv_set(key, val);
}

/* Setting $2 of session $1, of IPoE connection $1, of network $1. */
const char *setting(int n, const char *k)
{
	return kv_getf("SESSION%d_%s", n, k);
}

const char *ipoe_setting(int k, const char *key)
{
	return kv_getf("IPOE%d_%s", k, key);
}

const char *lan_setting(int n, const char *k)
{
	return kv_getf("LAN%d_%s", n, k);
}

static bool key_in(const char *key, const char *prefix, char lo, char hi,
		   const char *const *suffixes,
		   size_t nsuffixes)
{
	size_t len = strlen(prefix);

	if (strncmp(key, prefix, len) || key[len] < lo || key[len] > hi || key[len + 1] != '_')
		return false;
	for (size_t i = 0; i < nsuffixes; i++)
		if (!strcmp(key + len + 2, suffixes[i]))
			return true;
	return false;
}

#define VALUE_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789./:_-"

/*
 * The settings of /run/onu.conf, or of the file the shape came up with: a key
 * that is not one of those read here and a value with characters no setting
 * has are left out. A network's name is for people; its fixed addresses are
 * read where they are needed (lan_dhcp()), and so are the forwards, the DMZs
 * and the firewall's rules (forwards_fw(), rules_fw()). Name servers are one
 * blank apart.
 */
static void load_file(bool *legacy_lan)
{
	static const char *const dos_side[] = { "SIDE" };
	struct words lines = { 0 };
	struct buf text = { 0 };

	cat(CONF, &text);
	words_lines(&lines, buf_str(&text));
	for (size_t i = 0; i < lines.n; i++) {
		char *key = lines.v[i], *value = strchr(key, '=');
		bool known = false;

		if (value)
			*value++ = '\0';
		else
			value = key + strlen(key);
		if (!strcmp(key, "WAN_GEM") || !strcmp(key, "WAN_VLAN") || !strcmp(key,
										   "WAN_MAC")) {
			known = true;
		} else if (!strcmp(key, "LAN_VLAN")) {
			*legacy_lan = true;
			known = true;
		} else if (key_in(key, "SESSION", '1', '7', SESSION_KEYS,
				  ARRAY_SIZE(SESSION_KEYS)) ||
			   key_in(key, "LAN", '1', '8', LAN_KEYS, ARRAY_SIZE(LAN_KEYS))) {
			known = true;
		} else if (key_in(key, "IPOE", '1', '7', IPOE_KEYS, ARRAY_SIZE(IPOE_KEYS))) {
			if (!strcmp(key + 6, "DNS")) {
				if (only_of(value, "0123456789ABCDEFabcdef.: ")) {
					kv_set(key, value);
					continue;
				}
			}
			known = true;
		} else if (starts(key, "FW_")) {
			for (size_t k = 0; k < ARRAY_SIZE(FW_KEYS); k++)
				known |= !strcmp(key + 3, FW_KEYS[k]);
		} else if (starts(key, "DOS_")) {
			for (size_t k = 0; k < ARRAY_SIZE(DOS_ONOFF); k++)
				known |= !strcmp(key + 4, DOS_ONOFF[k].name);
			for (size_t k = 0; k < ARRAY_SIZE(DOS_FLOODS); k++)
				known |= !strcmp(key + 4, DOS_FLOODS[k].name);
			known |= !strcmp(key + 4, dos_side[0]);
		}
		if (!known)
			continue;
		if (!only_of(value, VALUE_CHARS)) {
			log_msg("%s: not a value; the default stays", key);
			continue;
		}
		kv_set(key, value);
	}
	words_free(&lines);
	buf_free(&text);
}

/*
 * The settings: the defaults, which are the table of settings.c and nowhere
 * else (onu-config defaults), then those of CONF. WAN_GEM, WAN_VLAN, WAN_MAC
 * and LAN_VLAN are keys of before, which the first session and the sessions'
 * links take over; the bench's router of before, MODE and its WAN_ and LAN_
 * settings, is gone, and its keys are left out.
 */
void load(void)
{
	static char defaults[16384];
	bool legacy_lan = false;
	struct words lines = { 0 };
	char wan[VAL];

	snprintf(g_mode, sizeof(g_mode), "%s", "bridge");
	kv_clear();
	kv_set("LAN_VLAN", "");
	kv_set("WAN_GEM", "");
	kv_set("WAN_VLAN", "");
	kv_set("WAN_MAC", "");
	if (settings_format_defaults(defaults, sizeof(defaults)) >= sizeof(defaults)) {
		log_msg("the defaults of the settings are not known: they do not fit");
		defaults[0] = '\0';
	}
	words_lines(&lines, defaults);
	for (size_t i = 0; i < lines.n; i++) {
		char *value = strchr(lines.v[i], '=');

		if (!value || value == lines.v[i])
			continue;
		*value++ = '\0';
		kv_set(lines.v[i], value);
	}
	words_free(&lines);
	snprintf(g_default_family, sizeof(g_default_family), "%s", setting(1, "FAMILY"));
	if (readable(CONF))
		load_file(&legacy_lan);
	/* The single session of before: its settings were the WAN's. */
	static const char *const wan_keys[][2] = {
		{ "SESSION1_VLAN", "WAN_VLAN" },
		{ "SESSION1_MAC", "WAN_MAC" },
	};

	if (!strcmp(setting(1, "GEM"), "auto")) {
		copy_val(wan, kv_get("WAN_GEM"));
		kv_set("SESSION1_GEM", wan);
	}
	for (size_t i = 0; i < ARRAY_SIZE(wan_keys); i++)
		if (!*kv_get(wan_keys[i][0])) {
			copy_val(wan, kv_get(wan_keys[i][1]));
			kv_set(wan_keys[i][0], wan);
		}
	/*
	 * The one link of before, LAN_VLAN or the host link itself, is the link
	 * of each session that names none of its own.
	 */
	if (legacy_lan) {
		copy_val(wan, *kv_get("LAN_VLAN") ? kv_get("LAN_VLAN") : "untagged");
		for (int n = 1; n <= MAX_SESSIONS; n++)
			if (!*setting(n, "HOST_VLAN"))
				kv_setf(wan, "SESSION%d_HOST_VLAN", n);
	}
}

/*
 * The VLAN $1 leads into on the PON side, where the OLT translates that VLAN
 * of the subscriber's side, as omcid publishes the translations in
 * /run/omcid/xlate ("SUB NET" lines); $1 itself without one. A session can so
 * be given the VLAN a router behind the stick would use.
 */
static char *xlate_vlan(const char *v, char *out)
{
	struct words lines = { 0 };
	char path[PATH_MAX];
	struct buf b = { 0 };

	copy_val(out, v);
	if (!*v)
		return out;
	cat(PATHF(path, "%s/run/omcid/xlate", g_root), &b);
	words_lines(&lines, buf_str(&b));
	for (size_t i = 0; i < lines.n; i++) {
		struct words f = { 0 };
		bool match;

		words_split(&f, lines.v[i]);
		match = f.n && !strcmp(f.v[0], v);
		if (match && *field(&f, 1))
			copy_val(out, f.v[1]);
		words_free(&f);
		if (match)
			break;
	}
	words_free(&lines);
	buf_free(&b);
	return out;
}

/* The VLAN session $1 runs in on the PON side. */
char *session_vlan(int n, char *out)
{
	return xlate_vlan(setting(n, "VLAN"), out);
}

/* The sessions that have an account, by number. */
int sessions(int *out)
{
	char path[PATH_MAX];
	int c = 0;

	for (int n = 1; n <= MAX_SESSIONS; n++)
		if (nonempty(PATHF(path, "%s/run/onu-pppoe%d", g_root, n)))
			out[c++] = n;
	return c;
}

bool have_sessions(void)
{
	int s[MAX_SESSIONS];

	return sessions(s) > 0;
}

/* The family of session $1: ipv4, ipv6 or both. */
const char *family(int n)
{
	const char *f = setting(n, "FAMILY");

	if (!strcmp(f, "ipv4") || !strcmp(f, "ipv6") || !strcmp(f, "both"))
		return f;
	return g_default_family;
}

/*
 * The VLAN of the host link session $1 hands its address down in: the one
 * its HOST_VLAN names, else its own, the VID the operator names its service
 * with; nothing for none (untagged).
 */
char *down_vlan(int n, char *out)
{
	const char *hv = setting(n, "HOST_VLAN");

	if (!strcmp(hv, "untagged"))
		out[0] = '\0';
	else
		copy_val(out, *hv ? hv : setting(n, "VLAN"));
	return out;
}

/* The link to the host for the VLAN $1: lanVID, or the host link itself. */
char *link_of(const char *vlan, char *out)
{
	if (*vlan)
		valf(out, "lan%s", vlan);
	else
		copy_val(out, "sfp");
	return out;
}

/*
 * Whether session $1 hands its $2, ipv4 or ipv6, down: it is told to, it
 * carries that family, and it is the first in its VLAN towards the host that
 * is both. In one VLAN one session at most hands its IPv4 address down, and
 * one its IPv6 prefix.
 */
bool hands(int n, const char *fam)
{
	char hv[VAL], other[VAL];
	int s[MAX_SESSIONS], c = sessions(s);

	down_vlan(n, hv);
	for (int i = 0; i < c; i++) {
		const char *f;

		if (!strcmp(setting(s[i], "PASS"), "off"))
			continue;
		if (strcmp(down_vlan(s[i], other), hv))
			continue;
		f = family(s[i]);
		if (!strcmp(f, "both") || !strcmp(f, fam))
			return s[i] == n;
	}
	return false;
}

/*
 * The session the stick's own default route goes by, of those that hand an
 * IPv4 address down: the first; 0 for none. Each other has a routing table of
 * its own, by which what comes from its address goes by its link.
 */
int main_pass(void)
{
	int s[MAX_SESSIONS], c = sessions(s);

	for (int i = 0; i < c; i++)
		if (hands(s[i], "ipv4"))
			return s[i];
	return 0;
}

/*
 * The session the stick's own IPv6 default route goes by, of those that hand
 * a prefix down: the first. Each other has its table, as main_pass().
 */
int main_pd(void)
{
	int s[MAX_SESSIONS], c = sessions(s);

	for (int i = 0; i < c; i++)
		if (hands(s[i], "ipv6"))
			return s[i];
	return 0;
}

/*
 * The connection the stick's own IPv4 default route goes by, pppoe:N or
 * ipoe:K: main_pass()'s session, and when no session hands an address down,
 * the connection the first local network goes out by. The stick reaches out
 * by it, and the switch's engine, which routes by the main table alone,
 * finds a way out there for the networks' translated frames, each of which
 * leaves by the connection its flow was translated for.
 */
static char *main_route(char *out)
{
	int l[MAX_LANS], mp = main_pass();

	out[0] = '\0';
	if (mp)
		valf(out, "pppoe:%d", mp);
	else if (lans(l))
		copy_val(out, lan_conn(l[0]));
	return out;
}

bool is_main_route(const char *kind, int n)
{
	char want[VAL], got[VAL];

	snprintf(want, sizeof(want), "%s:%d", kind, n);
	return !strcmp(main_route(got), want);
}

/* The routing table of session $1, for what it hands down. */
int table_of(int n)
{
	return 100 + n;
}

/*
 * The routing table of the connection $1, pppoe:N or ipoe:K: a session's,
 * 101 to 107, or an IPoE connection's, 111 to 117; nothing for another.
 */
char *table_of_conn(const char *conn, char *out)
{
	out[0] = '\0';
	if (starts(conn, "pppoe:"))
		valf(out, "%ld", 100 + num_of(conn + 6));
	else if (starts(conn, "ipoe:"))
		valf(out, "%ld", 110 + num_of(conn + 5));
	return out;
}

/* The links to the host the sessions hand down by, sorted, once each. */
void pass_links(struct words *out)
{
	int s[MAX_SESSIONS], c = sessions(s);
	char v[VAL], l[VAL];

	for (int i = 0; i < c; i++)
		if (strcmp(setting(s[i], "PASS"), "off"))
			words_add(out, link_of(down_vlan(s[i], v), l));
	words_sort(out, false, true);
}

static void svc_sync(void)
{
	if (run(0, "onu-svc", "sync", "onu", SVC_DIR, NULL))
		log_msg("procd was not given the programs of %s", SVC_DIR);
}

/*
 * The program ARGS as the instance $1, its line written under a name of a dot
 * and renamed into place, so that a sync never reads half of it.
 */
void svc_start(const char *name, const char *const *args)
{
	char tmp[PATH_MAX], dst[PATH_MAX];
	struct buf line = { 0 };

	for (size_t i = 0; args[i]; i++) {
		if (i)
			buf_puts(&line, " ");
		buf_puts(&line, args[i]);
	}
	buf_puts(&line, "\n");
	PATHF(tmp, "%s/.%s", SVC_DIR, name);
	PATHF(dst, "%s/%s", SVC_DIR, name);
	if (!mkdir_p(SVC_DIR) || !write_text(tmp, line.p, line.len) || rename(tmp, dst)) {
		log_msg("%s is no program of procd: not started", name);
		buf_free(&line);
		return;
	}
	buf_free(&line);
	svc_sync();
}

/* The instance $1 ended and forgotten; none is no error. */
void svc_stop(const char *name)
{
	char path[PATH_MAX];

	rm_f(PATHF(path, "%s/%s", SVC_DIR, name));
	if (is_dir(SVC_DIR))
		svc_sync();
}

/* The IPoE connections set up, by number: those with a kind. */
int ipoes(int *out)
{
	int c = 0;

	for (int k = 1; k <= MAX_IPOE; k++)
		if (*ipoe_setting(k, "KIND"))
			out[c++] = k;
	return c;
}

/* The VLAN IPoE connection $1 runs in on the PON side, translated as a session's is. */
char *ipoe_vlan(int k, char *out)
{
	return xlate_vlan(ipoe_setting(k, "VLAN"), out);
}

/* The interface of IPoE connection $1: wanVID. */
char *ipoe_dev(int k, char *out)
{
	char v[VAL];

	valf(out, "wan%s", ipoe_vlan(k, v));
	return out;
}

/* The IPoE connection set up on the interface $1, if any; 0 for none. */
int ipoe_on_dev(const char *dev)
{
	int i[MAX_IPOE], c = ipoes(i);
	char d[VAL];

	for (int n = 0; n < c; n++)
		if (!strcmp(ipoe_dev(i[n], d), dev))
			return i[n];
	return 0;
}

/* Whether IPoE connection $1 carries the family $2, ipv4 or ipv6. */
bool ipoe_carries(int k, const char *fam)
{
	const char *f = ipoe_setting(k, "FAMILY");

	if (!strcmp(f, "both") || !strcmp(f, fam))
		return true;
	return !*f && !strcmp(fam, "ipv4");
}

/*
 * The first IPoE connection with a static IPv6 address, which has the
 * stick's own IPv6 default route when no session hands a prefix down; 0 for
 * none.
 */
int ipoe_main6(void)
{
	int i[MAX_IPOE], c;

	if (main_pd())
		return 0;
	c = ipoes(i);
	for (int n = 0; n < c; n++)
		if (!strcmp(ipoe_setting(i[n], "KIND"), "static") && ipoe_carries(i[n], "ipv6") &&
		    *ipoe_setting(i[n], "ADDRESS6"))
			return i[n];
	return 0;
}

bool in_passthrough(void)
{
	char path[PATH_MAX];
	struct buf b = { 0 };
	bool yes;

	cat(PATHF(path, "%s/run/onu-mode", g_root), &b);
	yes = !strcmp(buf_str(&b), "passthrough");
	buf_free(&b);
	return yes;
}

/* The local networks set up, by number: those with a connection. */
int lans(int *out)
{
	int c = 0;

	for (int n = 1; n <= MAX_LANS; n++)
		if (*lan_setting(n, "CONN"))
			out[c++] = n;
	return c;
}

/* Network $1's interface: netN, a VLAN on the host link, or the host link itself. */
char *lan_if(int n, char *out)
{
	if (*lan_setting(n, "VLAN"))
		valf(out, "net%d", n);
	else
		copy_val(out, "sfp");
	return out;
}

/* The connection network $1 goes out by: pppoe:N or ipoe:K. */
const char *lan_conn(int n)
{
	return lan_setting(n, "CONN");
}

/* Network $1 as address/length: its first address and its prefix length. */
char *lan_net(int n, char *out)
{
	const char *la = lan_setting(n, "ADDRESS");
	const char *slash = strchr(la, '/');
	char host[VAL], v[VAL];
	long len = addr_len(la);

	addr_only(la, host);
	valf(out, "%s/%s", num_ip(ip_num(host) & len_mask(len), v), slash ? slash + 1 : la);
	return out;
}

/* The connections the networks go out by, sorted, once each. */
void lan_conns(struct words *out)
{
	int l[MAX_LANS], c = lans(l);

	for (int i = 0; i < c; i++)
		words_add(out, lan_conn(l[i]));
	words_sort(out, false, true);
}

/* The lines of CONF. */
void conf_lines(struct words *lines)
{
	struct buf b = { 0 };

	cat(CONF, &b);
	words_lines(lines, buf_str(&b));
	buf_free(&b);
}

/*
 * The settings of CONF, and the shape they make, which no setting chooses:
 * an account or an IPoE connection makes the stick hold its connections,
 * and none leaves the OLT's bridge.
 */
void load_mode(void)
{
	int i[MAX_IPOE];

	load();
	snprintf(g_mode, sizeof(g_mode), "%s",
		 have_sessions() || ipoes(i) ? "passthrough" : "bridge");
}

void paths(void)
{
	const char *root = getenv("ONU_ROOT"), *path = getenv("PATH"), *v;
	struct buf p = { 0 };

	snprintf(g_root, sizeof(g_root), "%s", root ? root : "");
	PATHF(RUN, "%s/run", g_root);
	PATHF(CONF_MAIN, "%s/run/onu.conf", g_root);
	PATHF(CONF_MODE, "%s/run/onu-mode.conf", g_root);
	PATHF(IDENTITY, "%s/run/onu-identity", g_root);
	PATHF(SESSIONS, "%s/run/session", g_root);
	PATHF(IPOE_DIR, "%s/run/ipoe", g_root);
	PATHF(NET, "%s/sys/class/net", g_root);
	PATHF(FORWARD, "%s/proc/sys/net/ipv4/ip_forward", g_root);
	PATHF(FORWARD6, "%s/proc/sys/net/ipv6/conf/all/forwarding", g_root);
	PATHF(LIB, "%s/usr/lib/onu", g_root);
	PATHF(DROP, "%s/onu-drop", LIB);
	PATHF(CALL, "%s/onu-call", LIB);
	PATHF(SVC_DIR, "%s/run/onu-svc.d", g_root);
	PATHF(LOCK, "%s/run/onu-apply.lock", g_root);
	PATHF(AGAIN, "%s/run/onu-apply.again", g_root);
	PATHF(APPLIED, "%s/run/onu-applied", g_root);
	PATHF(KEPT, "%s/run/onu-kept", g_root);
	PATHF(PENDING, "%s/run/onu-pending", g_root);
	PATHF(TRIAL, "%s/run/onu-trial.d/settings", g_root);
	buf_printf(&p, "%s:%s", path ? path : "/usr/sbin:/usr/bin:/sbin:/bin", LIB);
	setenv("PATH", p.p, 1);
	buf_free(&p);
	v = getenv("TRIAL_LIMIT");
	if (v && *v)
		g_trial_limit = num_of(v);
	v = getenv("APPLIED_TENTHS");
	if (v && *v)
		g_applied_tenths = num_of(v);
}
