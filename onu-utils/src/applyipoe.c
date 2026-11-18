// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyipoe: the connections that take their address as a router does
 * (IPoE), typed in or from the operator's DHCP server, as interfaces of
 * netifd with their routing tables.
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
#include "applyhost.h"
#include "applyipoe.h"
#include "applyutil.h"

/* The GEM interface of IPoE connection $1, as a session's (conn_gem()). */
static char *ipoe_gem(int k, char *out)
{
	char v[VAL];

	return conn_gem(ipoe_setting(k, "GEM"), ipoe_vlan(k, v), out);
}

/*
 * What IPoE connection $1 on the GEM interface $2 is made of, one line: its
 * interface, kind, family and MAC address, what is typed in for a static
 * one, and whether it is the stick's own way out. ipoes_sync() takes it down
 * and up again when this changes. It is in /run/ipoe/K/makings.
 */
static void ipoe_makings(int k, const char *gem, struct buf *out)
{
	char dev[VAL], dns[VAL];

	copy_val(dns, ipoe_setting(k, "DNS"));
	for (char *s = dns; *s; s++)
		if (*s == ' ')
			*s = ',';
	buf_reset(out);
	buf_printf(out, "%s %s %s %s mac=%s route=%s ", gem, ipoe_dev(k, dev),
		   ipoe_setting(k, "KIND"), ipoe_setting(k, "FAMILY"), ipoe_setting(k, "MAC"),
		   is_main_route("ipoe", k) ? "1" : "");
	buf_printf(out, "%s %s %s %s %s dns=%s", ipoe_setting(k, "ADDRESS"),
		   ipoe_setting(k, "GATEWAY"), ipoe_setting(k, "ADDRESS6"),
		   ipoe_setting(k, "GATEWAY6"), ipoe_setting(k, "PREFIX6"), dns);
}

/*
 * IPoE connection $1's default route by the gateway $2, as netifd's
 * sections, in the table $4, or in the main table without it. A gateway
 * outside the address's network, $3 (an operator that gives an address
 * alone, a /32, or a /31 whose other address the gateway is not), is on the
 * link all the same: a route to it alone comes first, as the kernel takes a
 * gateway only where a route on the link leads to it, and the default route
 * is netifd's "onlink" too, which the kernel takes without one: netifd adds
 * a part's routes in an order of its own when it reads the parts again.
 */
static void ipoe_gw_routes(struct buf *b, int k, const char *gw, bool onlink, const char *table)
{
	if (onlink) {
		buf_printf(b, "config route\n\toption interface 'ipoe%d'\n", k);
		buf_printf(b, "\toption target '%s/32'\n", gw);
		if (table)
			buf_printf(b, "\toption table '%s'\n", table);
		buf_puts(b, "\n");
	}
	buf_printf(b, "config route\n\toption interface 'ipoe%d'\n", k);
	buf_printf(b, "\toption target '0.0.0.0/0'\n\toption gateway '%s'\n", gw);
	if (onlink)
		buf_puts(b, "\toption onlink '1'\n");
	if (table)
		buf_printf(b, "\toption table '%s'\n", table);
	buf_puts(b, "\n");
}

/*
 * IPoE connection $1 as netifd's part "ipoe-K": its interface, ipoeK, on its
 * device, with the address it has, of /run/ipoe/K, or none yet (protocol
 * "none", which brings the interface up for the DHCP client); its table
 * with its default route by the gateway and the route to its network, which
 * an address alone, a /32, has none of; a rule that sends what comes from
 * its address there; and the stick's own default route when main_route()
 * names it. A gateway outside the address's network is reached on the link
 * (ipoe_gw_routes()). The text given last is kept in /run/ipoe/K/net, and
 * the same is not given again. What changed also gives its networks'
 * devices the name servers now.
 */
static void ipoe_net(int k)
{
	char d[PATH_MAX], path[PATH_MAX], net[PATH_MAX], tmp[PATH_MAX];
	char dev[VAL], addr[VAL], gw[VAL], addr6[VAL], table[16], dns[VAL], conn[16];
	struct buf b = { 0 }, have = { 0 };
	const char *gw6 = ipoe_setting(k, "GATEWAY6");
	int l[MAX_LANS], cl;

	PATHF(d, "%s/%d", IPOE_DIR, k);
	cat_val(PATHF(path, "%s/dev", d), dev);
	cat_val(PATHF(path, "%s/ipv4", d), addr);
	cat_val(PATHF(path, "%s/router", d), gw);
	cat_val(PATHF(path, "%s/ipv6", d), addr6);
	snprintf(table, sizeof(table), "%d", 110 + k);
	buf_printf(&b, "config interface 'ipoe%d'\n\toption device '%s'\n", k, dev);
	if (!*addr && !*addr6) {
		buf_puts(&b, "\toption proto 'none'\n\n");
	} else {
		buf_puts(&b, "\toption proto 'static'\n");
		if (*addr)
			buf_printf(&b, "\toption ipaddr '%s'\n", addr);
		if (*addr6)
			buf_printf(&b, "\toption ip6addr '%s'\n", addr6);
		buf_puts(&b, "\n");
	}
	if (*addr && *gw) {
		long len = addr_len(addr);
		uint32_t mask = len_mask(len), net_n;
		char host[VAL], v[VAL];
		bool onlink;

		addr_only(addr, host);
		net_n = ip_num(host) & mask;
		onlink = (ip_num(gw) & mask) != net_n;
		/* An address alone, a /32, has no network to route to. */
		if (len < 32) {
			buf_printf(&b, "config route\n\toption interface 'ipoe%d'\n", k);
			buf_printf(&b, "\toption target '%s/%ld'\n\toption table '%s'\n\n",
				   num_ip(net_n, v),
				   len, table);
		}
		ipoe_gw_routes(&b, k, gw, onlink, table);
		buf_printf(&b, "config rule\n\toption src '%s/32'\n\toption lookup '%s'\n\n", host,
			   table);
		if (is_main_route("ipoe", k))
			ipoe_gw_routes(&b, k, gw, onlink, NULL);
	}
	if (*addr6 && *gw6 && k == ipoe_main6()) {
		buf_printf(&b, "config route6\n\toption interface 'ipoe%d'\n", k);
		buf_printf(&b, "\toption target '::/0'\n\toption gateway '%s'\n\n", gw6);
	}
	/* The name servers are no part of netifd's, but of its networks' servers. */
	buf_printf(&b, "# dns %s\n", cat_val(PATHF(path, "%s/dns", d), dns));
	PATHF(net, "%s/net", d);
	PATHF(tmp, "%s/net.new", d);
	write_text(tmp, b.p, b.len);
	if (exists(net) && cat(net, &have)) {
		struct buf now = { 0 };
		bool same;

		cat(tmp, &now);
		same = !strcmp(buf_str(&now), buf_str(&have));
		buf_free(&now);
		if (same) {
			rm_f(tmp);
			goto out;
		}
	}
	rename(tmp, net);
	snprintf(conn, sizeof(conn), "ipoe-%d", k);
	if (run_in(0, &b, "onu-net", "set", conn, NULL))
		log_msg("connection %d: netifd was not given it", k);
	snprintf(conn, sizeof(conn), "ipoe:%d", k);
	cl = lans(l);
	for (int i = 0; i < cl; i++)
		if (!strcmp(lan_conn(l[i]), conn))
			lan_dhcp(l[i]);
out:
	buf_free(&b);
	buf_free(&have);
}

/*
 * What a static IPoE connection $1 has, as typed in, in /run/ipoe/K: its
 * addresses, of the families it carries, its gateway and name servers.
 */
static void ipoe_static(int k)
{
	static const char *const facts[] = { "ipv4", "router", "dns", "ipv6", "prefix", "until" };
	char d[PATH_MAX], path[PATH_MAX];

	PATHF(d, "%s/%d", IPOE_DIR, k);
	for (size_t i = 0; i < ARRAY_SIZE(facts); i++)
		rm_f(PATHF(path, "%s/%s", d, facts[i]));
	if (ipoe_carries(k, "ipv4")) {
		write_line(PATHF(path, "%s/ipv4", d), ipoe_setting(k, "ADDRESS"));
		write_line(PATHF(path, "%s/router", d), ipoe_setting(k, "GATEWAY"));
	}
	if (ipoe_carries(k, "ipv6")) {
		write_line(PATHF(path, "%s/ipv6", d), ipoe_setting(k, "ADDRESS6"));
		if (*ipoe_setting(k, "PREFIX6"))
			write_line(PATHF(path, "%s/prefix", d), ipoe_setting(k, "PREFIX6"));
	}
	if (*ipoe_setting(k, "DNS"))
		write_line(PATHF(path, "%s/dns", d), ipoe_setting(k, "DNS"));
}

/*
 * IPoE connection $1 up on the GEM interface $2: its interface, made unless a
 * session runs on it already, at Ethernet's MTU (a session beside it keeps
 * to 1492), its address typed in, or its DHCP client, which runs as the
 * account dhcpc with the powers of its raw socket and its port, and its
 * part of netifd. The operator's side is kept from the host side's networks
 * as a session's is (guard()).
 */
static void ipoe_up(int k, const char *gem)
{
	char d[PATH_MAX], path[PATH_MAX], dev[VAL], vlan[VAL], mac[VAL], name[32], script[PATH_MAX];
	struct buf makings = { 0 };
	mode_t mask;

	ipoe_dev(k, dev);
	PATHF(d, "%s/%d", IPOE_DIR, k);
	if (!dev_shared(d, dev) || !exists(PATHF(path, "%s/%s", NET, dev))) {
		run(0, "onu-nl", "link", "set", gem, "up", NULL);
		run(0, "onu-nl", "link", "add", "vlan", gem, dev, ipoe_vlan(k, vlan), NULL);
		copy_val(mac, ipoe_setting(k, "MAC"));
		if (!*mac)
			pon_mac(mac);
		if (*mac && run(0, "onu-nl", "link", "set", dev, "address", mac, NULL))
			log_msg("connection %d: the interface did not take its MAC address", k);
		run(0, "onu-nl", "link", "set", dev, "up", NULL);
	}
	run(RUN_QUIET_ERR, "onu-nl", "link", "set", dev, "mtu", "1500", NULL);
	mkdir_p(d);
	/*
	 * The DHCP client's script writes what the lease brings here, as the
	 * account dhcpc of the group onu.
	 */
	if (!give_group(d, "onu", 02770))
		log_msg("connection %d: its directory is not the group onu's", k);
	write_line(PATHF(path, "%s/gem", d), gem);
	write_line(PATHF(path, "%s/dev", d), dev);
	write_line(PATHF(path, "%s/kind", d), ipoe_setting(k, "KIND"));
	ipoe_makings(k, gem, &makings);
	mask = umask(077);
	write_line(PATHF(path, "%s/makings", d), buf_str(&makings));
	umask(mask);
	buf_free(&makings);
	if (!strcmp(ipoe_setting(k, "KIND"), "static")) {
		ipoe_static(k);
	} else if (ipoe_carries(k, "ipv4")) {
		snprintf(name, sizeof(name), "udhcpc-ipoe%d", k);
		PATHF(script, "%s/onu-dhcpc", LIB);
		SVC_START(name, "--", DROP, "-c", "net_raw,net_bind_service", "dhcpc", "udhcpc",
			  "-f", "-R",
			  "-i", dev, "-s", script);
	}
	ipoe_net(k);
	guard(dev);
	log_msg("connection %d on %s (%s), GEM port %s", k, dev, ipoe_setting(k, "KIND"),
		starts(gem, "gem") ? gem + 3 : gem);
}

/*
 * IPoE connection $1 down: its DHCP client, which gives its address back,
 * its part of netifd, and its interface when no session is on it.
 */
static void ipoe_down(int k)
{
	char d[PATH_MAX], path[PATH_MAX], dev[VAL], name[32];

	PATHF(d, "%s/%d", IPOE_DIR, k);
	if (!readable(PATHF(path, "%s/dev", d)))
		return;
	read_line(path, dev);
	snprintf(name, sizeof(name), "udhcpc-ipoe%d", k);
	svc_stop(name);
	snprintf(name, sizeof(name), "ipoe-%d", k);
	if (run(0, "onu-net", "del", name, NULL))
		log_msg("connection %d: netifd still has it", k);
	if (!dev_shared(d, dev))
		run(RUN_QUIET_ERR, "onu-nl", "link", "del", dev, NULL);
	rm_rf(d);
	log_msg("connection %d down", k);
}

void ipoes_down(void)
{
	struct words names = { 0 };

	dir_names(IPOE_DIR, &names);
	for (size_t i = 0; i < names.n; i++) {
		char path[PATH_MAX];

		if (is_dir(PATHF(path, "%s/%s", IPOE_DIR, names.v[i])))
			ipoe_down((int)num_of(names.v[i]));
	}
	words_free(&names);
}

/*
 * The IPoE connections follow their settings and their GEM ports as the
 * sessions do (sessions_sync()), and the address a lease brought: one whose
 * makings changed, or whose interface is gone, goes down and up again; one
 * no longer set up goes; the others are given what their DHCP client wrote.
 */
void ipoes_sync(void)
{
	int want[MAX_IPOE], cw = ipoes(want);

	for (int k = 1; k <= MAX_IPOE; k++) {
		char path[PATH_MAX], cur[VAL] = "", dev[VAL] = "", gem[VAL], d[VAL];
		struct buf mk = { 0 }, have = { 0 };

		if (readable(PATHF(path, "%s/%d/gem", IPOE_DIR, k)))
			read_line(path, cur);
		if (readable(PATHF(path, "%s/%d/dev", IPOE_DIR, k)))
			read_line(path, dev);
		if (!in_list(want, cw, k)) {
			if (*cur)
				ipoe_down(k);
			continue;
		}
		ipoe_gem(k, gem);
		if (*gem) {
			bool changed = strcmp(gem, cur) || strcmp(ipoe_dev(k, d), dev) ||
				       (*cur && !exists(PATHF(path, "%s/%s", NET, dev)));

			if (!changed) {
				ipoe_makings(k, gem, &mk);
				cat(PATHF(path, "%s/%d/makings", IPOE_DIR, k), &have);
				changed = strcmp(buf_str(&mk), buf_str(&have));
			}
			if (changed) {
				if (*cur)
					ipoe_down(k);
				ipoe_up(k, gem);
			} else if (*cur) {
				ipoe_net(k);
			}
		} else if (*cur) {
			log_msg("connection %d: its GEM port is gone", k);
			ipoe_down(k);
		}
		buf_free(&mk);
		buf_free(&have);
	}
}
