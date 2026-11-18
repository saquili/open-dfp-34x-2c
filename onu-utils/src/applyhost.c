// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyhost: the host side of passthrough: the links a session hands its
 * address down by, and the local networks the stick routes, with their DHCP
 * servers.
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
#include "applyhost.h"
#include "applyipoe.h"
#include "applyppp.h"
#include "applyutil.h"

/*
 * The local networks as netifd's part "nets": each one's interface, a VLAN on
 * the host link or the host link itself, with the stick's address in it and
 * a strict reverse-path check, and a rule that sends what comes from the
 * network to its connection's table; in the table of each connection that
 * networks go out by, the routes to all of the networks, and a session's
 * default route (an IPoE connection's is its own part's, ipoe_net()).
 * Without networks the part goes.
 */
static void lans_net(void)
{
	int l[MAX_LANS], c = lans(l);
	struct words conns = { 0 };
	struct buf b = { 0 };
	char ifn[VAL], net[VAL], table[VAL];

	if (!c) {
		run(0, "onu-net", "del", "nets", NULL);
		return;
	}
	for (int i = 0; i < c; i++) {
		lan_if(l[i], ifn);
		buf_puts(&b, "config device\n");
		if (strcmp(ifn, "sfp"))
			buf_printf(&b,
				   "\toption type '8021q'\n\toption ifname 'sfp'\n\toption vid '%s'\n",
				   lan_setting(l[i], "VLAN"));
		buf_printf(&b, "\toption name '%s'\n\toption rpfilter 'strict'\n", ifn);
		buf_puts(&b, "\toption ipv6 '0'\n\n");
		buf_printf(&b,
			   "config interface 'net%d'\n\toption device '%s'\n\toption proto 'static'\n",
			   l[i], ifn);
		buf_printf(&b, "\toption ipaddr '%s'\n\n", lan_setting(l[i], "ADDRESS"));
		buf_printf(&b, "config rule\n\toption src '%s'\n\toption lookup '%s'\n\n",
			   lan_net(l[i], net),
			   table_of_conn(lan_conn(l[i]), table));
	}
	lan_conns(&conns);
	for (size_t k = 0; k < conns.n; k++) {
		table_of_conn(conns.v[k], table);
		if (starts(conns.v[k], "pppoe:")) {
			buf_printf(&b, "config route\n\toption interface 'ses%s'\n",
				   conns.v[k] + 6);
			buf_printf(&b, "\toption target '0.0.0.0/0'\n\toption table '%s'\n\n",
				   table);
		}
		for (int i = 0; i < c; i++) {
			buf_printf(&b, "config route\n\toption interface 'net%d'\n", l[i]);
			buf_printf(&b, "\toption target '%s'\n\toption table '%s'\n\n",
				   lan_net(l[i], net),
				   table);
		}
	}
	if (run_in(0, &b, "onu-net", "set", "nets", NULL))
		log_msg("netifd was not given the local networks");
	words_free(&conns);
	buf_free(&b);
}

/*
 * Network $1's DHCP server, when its DHCP is on: the addresses of its pool,
 * all of the network's but its first and last when none is given (the server
 * never leases its own), the stick as the router, the IPv4 name servers of
 * its connection once it has them, the lease's time, and the fixed address
 * of each of its devices. It writes its leases every ten seconds, so that the
 * status daemon shows a device soon.
 */
void lan_dhcp(int n)
{
	char name[32], conf[PATH_MAX], leases[PATH_MAX], path[PATH_MAX];
	char host[VAL], from[VAL], to[VAL], v[VAL], prefix[32];
	struct words lines = { 0 }, dns = { 0 };
	struct buf b = { 0 }, text = { 0 };
	const char *addr, *conn;
	uint32_t mask, net;
	long long count;

	snprintf(name, sizeof(name), "udhcpd-net%d", n);
	PATHF(conf, "%s/run/udhcpd-net%d.conf", g_root, n);
	PATHF(leases, "%s/run/udhcpd-net%d.leases", g_root, n);
	svc_stop(name);
	if (strcmp(lan_setting(n, "DHCP"), "on") || !*lan_setting(n, "CONN")) {
		rm_f(conf);
		return;
	}
	addr = lan_setting(n, "ADDRESS");
	mask = len_mask(addr_len(addr));
	net = ip_num(addr_only(addr, host)) & mask;
	copy_val(from, lan_setting(n, "POOL_FROM"));
	copy_val(to, lan_setting(n, "POOL_TO"));
	if (!*from)
		num_ip(net + 1, from);
	if (!*to)
		num_ip(net + (~mask & 0xffffffffu) - 1, to);
	count = (long long)ip_num(to) - (long long)ip_num(from) + 1;
	if (count > 512)
		count = 512;
	conn = lan_conn(n);
	if (starts(conn, "pppoe:")) {
		cat(PATHF(path, "%s/%ld/dns", SESSIONS, num_of(conn + 6)), &text);
		words_split(&dns, buf_str(&text));
	} else {
		/* udhcpd gives IPv4 name servers alone. */
		struct words all = { 0 };

		cat(PATHF(path, "%s/%s/dns", IPOE_DIR, after_colon(conn)), &text);
		words_split(&all, buf_str(&text));
		for (size_t i = 0; i < all.n; i++)
			if (!strchr(all.v[i], ':'))
				words_add(&dns, all.v[i]);
		words_free(&all);
	}
	buf_printf(&b, "interface %s\nstart %s\nend %s\nmax_leases %lld\n", lan_if(n, v), from, to,
		   count);
	buf_printf(&b, "lease_file %s\npidfile %s/udhcpd-net%d.pid\nauto_time 10\n", leases, RUN,
		   n);
	buf_printf(&b, "opt subnet %s\nopt router %s\n", num_ip(mask, v), host);
	if (dns.n) {
		buf_puts(&b, "opt dns ");
		words_join(&dns, " ", &b);
		buf_puts(&b, "\n");
	}
	buf_printf(&b, "opt lease %s\n", lan_setting(n, "LEASE"));
	snprintf(prefix, sizeof(prefix), "=%d/", n);
	conf_lines(&lines);
	for (size_t i = 0; i < lines.n; i++) {
		const char *eq = keyed(lines.v[i], "FIXED", false);
		struct words f = { 0 };
		const char *s;

		if (!eq || !starts(eq, prefix))
			continue;
		/* IFS='=/' read -r _ _ MAC IP: the fourth field takes the rest. */
		s = lines.v[i];
		for (int k = 0; k < 3; k++) {
			size_t len = strcspn(s, "=/");

			words_addn(&f, s, len);
			s += len;
			if (*s)
				s++;
		}
		buf_printf(&b, "static_lease %s %s\n", field(&f, 2), s);
		words_free(&f);
	}
	write_text(conf, b.p, b.len);
	if (!exists(leases))
		write_text(leases, "", 0);
	SVC_START(name, "--", "udhcpd", "-f", conf);
	words_free(&lines);
	words_free(&dns);
	buf_free(&b);
	buf_free(&text);
}

/*
 * The local networks up: netifd's part, each network's DHCP server, and the
 * NAT they go out through; a network that went takes its server with it.
 */
void lans_up(void)
{
	lans_net();
	for (int n = 1; n <= MAX_LANS; n++)
		lan_dhcp(n);
	lans_fw();
}

static void lans_down(void)
{
	for (int n = 1; n <= MAX_LANS; n++) {
		char name[32], path[PATH_MAX];

		snprintf(name, sizeof(name), "udhcpd-net%d", n);
		svc_stop(name);
		rm_f(PATHF(path, "%s/run/udhcpd-net%d.conf", g_root, n));
	}
	run(0, "onu-net", "del", "nets", NULL);
	fw_clear();
}

/* Whether the switch's L3 engine decrements the TTL or hop limit of what it routes. */
static void ttl_decrement(const char *on)
{
	if (run(0, "onu-nl", "param", "l3_ttl_decrement", on, NULL))
		log_msg("the switch did not take l3_ttl_decrement %s", on);
}

/*
 * The links $1 as netifd's part "links": each a VLAN on the host link, or the
 * host link itself, with the stick's address, a strict reverse-path check,
 * and IPv6 where a session hands its prefix down by it (the host link keeps
 * its own). Without links the part goes.
 */
static void links_net(const struct words *links)
{
	int s[MAX_SESSIONS], cs = sessions(s);
	struct buf b = { 0 };

	if (!links->n) {
		run(0, "onu-net", "del", "links", NULL);
		return;
	}
	for (size_t i = 0; i < links->n; i++) {
		const char *lan = links->v[i];
		int v6 = !strcmp(lan, "sfp");

		for (int n = 0; n < cs; n++) {
			char dv[VAL], lk[VAL];

			if (!strcmp(link_of(down_vlan(s[n], dv), lk), lan) && hands(s[n], "ipv6"))
				v6 = 1;
		}
		buf_puts(&b, "config device\n");
		if (strcmp(lan, "sfp"))
			buf_printf(&b,
				   "\toption type '8021q'\n\toption ifname 'sfp'\n\toption vid '%s'\n",
				   starts(lan, "lan") ? lan + 3 : lan);
		buf_printf(&b, "\toption name '%s'\n\toption rpfilter 'strict'\n", lan);
		buf_printf(&b, "\toption ipv6 '%d'\n\n", v6);
		buf_printf(&b, "config interface '%s'\n\toption device '%s'\n", lan, lan);
		buf_puts(&b, "\toption proto 'static'\n");
		buf_printf(&b, "\toption ipaddr '%s/32'\n\n", LINK_ADDRESS);
	}
	if (run_in(0, &b, "onu-net", "set", "links", NULL))
		log_msg("netifd was not given the links to the host");
	buf_free(&b);
}

/*
 * Passthrough mode: the links to the host, one for each VLAN the sessions
 * hand their addresses down in, a VLAN on the host link or the host link
 * itself, with the stick's address on each; the sessions' addresses come
 * later. The stick stands in for the operator's ONT between the router and
 * the concentrator, which is no hop: the switch routes the sessions' packets
 * with their TTL unchanged, as it does the gateway the router is given.
 * A link forwards only what comes from the address it hands on (BCP 38): a
 * strict reverse-path check drops a packet whose source the stick would not
 * route back by the link. Without it, packets of any source that reached the
 * stick's MAC on the link went out into the session (measured: about half of
 * 1000, the rest cut by the limit on the frames to the CPU). The switch routes
 * only the handed address's own packets in hardware.
 */
void pass_up(void)
{
	struct words links = { 0 };
	int l[MAX_LANS];

	write_line(FORWARD, "1");
	/* A router of local networks is a hop: what it routes counts it. */
	ttl_decrement(lans(l) ? "true" : "false");
	pass_links(&links);
	links_net(&links);
	words_free(&links);
}

void pass_down(void)
{
	struct words none = { 0 };
	char path[PATH_MAX];

	lans_down();
	ipoes_down();
	sessions_down();
	links_net(&none);
	rm_f(PATHF(path, "%s/run/onu-lan", g_root));
	rm_f(PATHF(path, "%s/run/onu-pass", g_root));
	rm_f(PATHF(path, "%s/run/onu-pass-proxy", g_root));
	ttl_decrement("true");
	write_line(FORWARD, "0");
	write_line(FORWARD6, "0");
}
