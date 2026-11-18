// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyppp: the stick's own PPPoE sessions: each as an interface of netifd,
 * what it brought handed down to the router behind the stick, its delegated
 * prefix, and the sessions following their settings and their GEM ports.
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
#include "applyomcid.h"
#include "applyppp.h"
#include "applyutil.h"
#include "sha256.h"

static void pass_lease(int n, const char *addr, const char *dev, const char *peer);
static void pass_lease_down(int n);

static char *session_gem(int n, char *out)
{
	char v[VAL];

	return conn_gem(setting(n, "GEM"), session_vlan(n, v), out);
}

/* What session $1 has: the fact $2 with the value $3, or none without one. */
static void note(int n, const char *fact, const char *value)
{
	char dir[PATH_MAX], path[PATH_MAX];

	if (!is_dir(PATHF(dir, "%s/%d", SESSIONS, n)))
		return;
	PATHF(path, "%s/%s", dir, fact);
	if (value && *value)
		write_line(path, value);
	else
		rm_f(path);
}

/* The fact $2 of session $1, as its file has it. */
static char *fact_of(int n, const char *fact, char *out)
{
	char path[PATH_MAX];

	return cat_val(PATHF(path, "%s/%d/%s", SESSIONS, n, fact), out);
}

/*
 * The delegated IPv6 prefix of session $1, whose link $2 came up on its
 * interface $3, handed on to the host by dhcp6d: the link to the host gets
 * IPv6, and the stick forwards it. A session other than main_pd() has dhcp6d
 * route what comes from its prefix by the session's own table. dhcp6d keeps
 * the host's binding in /run/dhcp6d/pdN, which pd_keep() keeps in the store.
 * It is procd's service dhcp6dN, and runs as the account dhcp6d with the three
 * powers it uses: ports 546 and 547, the raw socket of its Router
 * Advertisements, and the route of the prefix.
 */
static void pd_up(int n, const char *ppp, const char *dev)
{
	char lan[VAL], name[32], log[PATH_MAX], dir[PATH_MAX], pd[PATH_MAX], ses[16], table[16];
	const char *args[40];
	size_t a = 0;

	if (!*fact_of(n, "lan", lan))
		return;
	snprintf(name, sizeof(name), "dhcp6d%d", n);
	svc_stop(name);
	write_line(FORWARD6, "1");
	PATHF(log, "%s/run/dhcp6d.log", g_root);
	PATHF(dir, "%s/%d", SESSIONS, n);
	PATHF(pd, "%s/run/dhcp6d/pd%d", g_root, n);
	snprintf(ses, sizeof(ses), "ses%d", n);
	static const char *const caps = "net_bind_service,net_admin,net_raw";
	const char *const fixed[] = { "-o", log, "--", DROP, "-c", caps, "dhcp6d", "dhcp6d",
				      "-w", ppp, "-l", lan, "-m", dev, "-o", dir, "-b", pd,
				      "-x", CALL, "-n", ses, NULL };

	for (const char *const *s = fixed; *s; s++)
		args[a++] = *s;
	if (n != main_pd()) {
		snprintf(table, sizeof(table), "%d", table_of(n));
		args[a++] = "-t";
		args[a++] = table;
	}
	args[a] = NULL;
	svc_start(name, args);
	log_msg("session %d: its IPv6 prefix goes to the router on %s", n, lan);
}

/*
 * Session $1 as an interface of netifd, "sesN", of the protocol "pppoed"
 * (pppoed/README.rst): netifd runs pppoed on the device $2, with the account of
 * /run/onu-pppoeN, for the family $3, the address handed on ($4, 1 or 0) and
 * the default route through it ($5), keeps it and starts it again when it
 * ends, and puts what the session brings in place; pppoed writes it to
 * /run/session/N.
 */
static void session_net(int n, const char *dev, const char *fam, int pass, int route)
{
	struct buf b = { 0 };
	char part[16];

	buf_printf(&b, "config interface 'ses%d'\n\toption device '%s'\n", n, dev);
	buf_printf(&b, "\toption proto 'pppoed'\n\toption session '%d'\n", n);
	buf_printf(&b, "\toption family '%s'\n\toption pass '%d'\n", fam, pass);
	buf_printf(&b, "\toption defaultroute '%d'\n\n", route);
	snprintf(part, sizeof(part), "ses%d", n);
	if (run_in(0, &b, "onu-net", "set", part, NULL))
		log_msg("session %d: netifd was not given it", n);
	buf_free(&b);
}

/*
 * What session $1 brought, which pppoed wrote in /run/session/N, handed on as
 * its role, in /run/session/N/role, says: in the role "pass" or "routed" the
 * address goes to the host, by the stick's default route or by the session's
 * own table; "pass" gives the stick's name servers. With "pd" the delegated
 * IPv6 prefix goes to the host by dhcp6d. netifd runs it for each event of the
 * session's interface, once a time; what is handed on already stays as it is.
 */
static void session_brought(int n)
{
	char d[PATH_MAX], path[PATH_MAX], role[VAL], role6[VAL], dev[VAL], addr[VAL], v[VAL];
	char svc[PATH_MAX];
	int l[MAX_LANS], cl;

	PATHF(d, "%s/%d", SESSIONS, n);
	role[0] = '\0';
	role6[0] = '\0';
	if (readable(PATHF(path, "%s/role", d)))
		read_words(path, role, role6);
	if (!*fact_of(n, "ppp", dev))
		return;
	guard(dev);
	fact_of(n, "ipv4", addr);
	if (*addr && strcmp(fact_of(n, "pass", v), addr) && strcmp(fact_of(n, "up", v), addr)) {
		char dns[VAL], peer[VAL], conn[VAL];

		write_line(PATHF(path, "%s/up", d), addr);
		fact_of(n, "dns", dns);
		if (!strcmp(role, "pass")) {
			PATHF(path, "%s/run/onu-dns", g_root);
			if (*dns)
				write_line(path, dns);
			else
				rm_f(path);
		}
		if (!strcmp(role, "pass") || !strcmp(role, "routed"))
			pass_lease(n, addr, dev, fact_of(n, "peer", peer));
		/*
		 * Its networks' devices get its name servers, and what is
		 * forwarded to them comes in by its interface, to its address.
		 */
		snprintf(conn, sizeof(conn), "pppoe:%d", n);
		cl = lans(l);
		for (int i = 0; i < cl; i++)
			if (!strcmp(lan_conn(l[i]), conn))
				lan_dhcp(l[i]);
		lans_fw();
		log_msg("session %d: IPv4 up on %s", n, dev);
	}
	PATHF(svc, "%s/dhcp6d%d", SVC_DIR, n);
	if (nonempty(PATHF(path, "%s/ipv6", d)) && !strcmp(role6, "pd") && !exists(svc)) {
		log_msg("session %d: IPv6 up on %s", n, dev);
		pd_up(n, dev, fact_of(n, "dev", v));
	}
}

/*
 * What session $1 handed on, taken back when it ends. What the session brought
 * is pppoed's to take away: by the time netifd tells of the end, the next
 * pppoed may have written its own.
 */
static void session_lost(int n)
{
	char path[PATH_MAX], role[VAL], role6[VAL], name[32];

	role[0] = '\0';
	role6[0] = '\0';
	if (readable(PATHF(path, "%s/%d/role", SESSIONS, n)))
		read_words(path, role, role6);
	if (!strcmp(role6, "pd")) {
		snprintf(name, sizeof(name), "dhcp6d%d", n);
		svc_stop(name);
	}
	if (!strcmp(role, "pass") || !strcmp(role, "routed"))
		pass_lease_down(n);
	rm_f(PATHF(path, "%s/%d/up", SESSIONS, n));
	/*
	 * Its forwards go with its interface, whose name the next session to
	 * come up may take.
	 */
	lans_fw();
}

/*
 * An event of an interface of netifd, which netifd runs onu-apply for (netifd
 * -h), with ACTION, INTERFACE and DEVICE: a session's interface, sesN, that
 * comes up or changes hands on what it brought; one that goes down takes it
 * back. The others are no business of onu-apply.
 */
void iface_event(void)
{
	const char *iface = getenv("INTERFACE"), *action = getenv("ACTION");
	char dir[PATH_MAX];
	int n;

	if (!iface || strlen(iface) != 4 || !starts(iface,
						    "ses") || iface[3] < '1' || iface[3] > '7')
		return;
	n = iface[3] - '0';
	if (!is_dir(PATHF(dir, "%s/%d", SESSIONS, n)) || !action)
		return;
	if (!strcmp(action, "ifup") || !strcmp(action, "ifupdate"))
		session_brought(n);
	else if (!strcmp(action, "ifdown"))
		session_lost(n);
}

/*
 * What session $1 hands down as netifd's part "pass-N": the route to its
 * address $2 on its link $3, the gateway $4 answered for on the link without
 * being taken (proxy ARP) when it is not the stick's own address there, and,
 * for a session other than main_pass(), its table: a rule sends what comes
 * from $2 to it, and its default route goes by the session's interface.
 * netifd puts them in place, and takes them away with the part or with the
 * interface they belong to.
 */
static void pass_net(int n, const char *addr, const char *lan, const char *gw)
{
	struct buf b = { 0 };
	char part[16];

	buf_printf(&b, "config route\n\toption interface '%s'\n", lan);
	buf_printf(&b, "\toption target '%s/32'\n\n", addr);
	if (strcmp(gw, LINK_ADDRESS)) {
		buf_printf(&b, "config neighbor\n\toption interface '%s'\n", lan);
		buf_printf(&b, "\toption ipaddr '%s'\n\toption proxy '1'\n\n", gw);
	}
	if (n != main_pass()) {
		buf_printf(&b, "config rule\n\toption src '%s/32'\n", addr);
		buf_printf(&b, "\toption lookup '%d'\n\n", table_of(n));
		buf_printf(&b, "config route\n\toption interface 'ses%d'\n", n);
		buf_printf(&b, "\toption target '0.0.0.0/0'\n\toption table '%d'\n\n", table_of(n));
	}
	snprintf(part, sizeof(part), "pass-%d", n);
	if (run_in(0, &b, "onu-net", "set", part, NULL))
		log_msg("session %d: netifd was not given its address", n);
	buf_free(&b);
}

/*
 * The address $2 of session $1, whose link is $3, handed down: a host route
 * to it on the session's link to the host, and a DHCP server there that
 * leases it alone, as a /32 with the stick as the gateway, the session's name
 * servers and its MTU. A lease runs for a minute: the router asks again
 * before it ends, so it finds a new address of a new session that soon. The
 * gateway is the concentrator's address $4, as a router that made the
 * session itself would have it, and the stick answers ARP for it on the link
 * without taking it: a packet to it goes on into the session, so that the
 * concentrator answers it. Without the peer's address it is the stick's own
 * address on the link, LINK_ADDRESS, which the DHCP server answers from
 * either way. A session other than main_pass() routes what comes from its
 * address by a table of its own, through its link.
 */
static void pass_lease(int n, const char *addr, const char *dev, const char *peer)
{
	char lan[VAL], name[VAL + 8], path[PATH_MAX], leases[PATH_MAX], conf[PATH_MAX];
	char v[VAL], mtu[VAL], dns[VAL];
	struct buf b = { 0 };
	const char *gw = LINK_ADDRESS;

	if (!*fact_of(n, "lan", lan))
		return;
	snprintf(name, sizeof(name), "udhcpd-%s", lan);
	svc_stop(name);
	PATHF(leases, "%s/run/udhcpd-%s.leases", g_root, lan);
	PATHF(conf, "%s/run/udhcpd-%s.conf", g_root, lan);
	if (strcmp(fact_of(n, "pass", v), addr))
		rm_f(leases);
	if (*peer && only_of(peer, "0123456789.") && strcmp(peer, "0.0.0.0"))
		gw = peer;
	pass_net(n, addr, lan, gw);
	cat_val(PATHF(path, "%s/%s/mtu", NET, dev), mtu);
	fact_of(n, "dns", dns);
	buf_printf(&b, "interface %s\nstart %s\nend %s\nmax_leases 1\n", lan, addr, addr);
	buf_printf(&b, "lease_file %s\npidfile %s/udhcpd-%s.pid\n", leases, RUN, lan);
	buf_printf(&b, "opt subnet 255.255.255.255\nopt router %s\n", gw);
	buf_printf(&b, "opt staticroutes %s/32 0.0.0.0, 0.0.0.0/0 %s\n", gw, gw);
	if (*dns)
		buf_printf(&b, "opt dns %s\n", dns);
	if (*mtu)
		buf_printf(&b, "opt mtu %s\n", mtu);
	buf_puts(&b, "opt lease 60\n");
	write_text(conf, b.p, b.len);
	buf_free(&b);
	if (!exists(leases))
		write_text(leases, "", 0);
	note(n, "pass", addr);
	note(n, "gateway", gw);
	/* The stick's own way out: what the status daemon shows as the address handed on. */
	if (n == main_pass()) {
		write_line(PATHF(path, "%s/run/onu-pass", g_root), addr);
		write_line(PATHF(path, "%s/run/onu-lan", g_root), lan);
		PATHF(path, "%s/run/onu-pass-proxy", g_root);
		if (strcmp(gw, LINK_ADDRESS))
			write_line(path, gw);
		else
			rm_f(path);
	}
	SVC_START(name, "--", "udhcpd", "-f", conf);
	log_msg("session %d: its address goes to the router on %s, with the gateway %s", n, lan,
		gw);
}

/* The address session $1 handed down taken back, when the session ends. */
static void pass_lease_down(int n)
{
	char addr[VAL], lan[VAL], name[VAL + 8], part[16], path[PATH_MAX], v[VAL];

	fact_of(n, "pass", addr);
	if (*fact_of(n, "lan", lan)) {
		snprintf(name, sizeof(name), "udhcpd-%s", lan);
		svc_stop(name);
	}
	snprintf(part, sizeof(part), "pass-%d", n);
	run(0, "onu-net", "del", part, NULL);
	if (*addr && !strcmp(addr, cat_val(PATHF(path, "%s/run/onu-pass", g_root), v))) {
		rm_f(path);
		rm_f(PATHF(path, "%s/run/onu-pass-proxy", g_root));
		rm_f(PATHF(path, "%s/run/onu-lan", g_root));
	}
	note(n, "pass", NULL);
	note(n, "gateway", NULL);
}

/*
 * Room for PPPoE's 8 bytes of headers around a 1500-byte packet (RFC 4638) on
 * the PON port, the GEM interface $1 and the WAN interface $2, as far as each
 * takes it: pppoed asks the concentrator for 1500 bytes, and keeps to 1492
 * when the interface has no room or the concentrator does not agree. A WAN
 * interface that an IPoE connection shares stays at Ethernet's 1500, which
 * the connection's packets keep to on the PON.
 */
static void pppoe_mtu(const char *gem, const char *wan)
{
	const char *devs[3];

	devs[0] = "pon";
	devs[1] = gem;
	devs[2] = wan;
	for (size_t i = 0; i < ARRAY_SIZE(devs); i++) {
		if (!strcmp(devs[i], wan) && ipoe_on_dev(wan))
			return;
		if (run(RUN_QUIET_ERR, "onu-nl", "link", "set", devs[i], "mtu", "1508", NULL))
			return;
	}
}

/*
 * The interface of session $1 on the GEM interface $2: the GEM interface
 * itself, or wanVID for a VLAN.
 */
static char *session_dev(int n, const char *gem, char *out)
{
	char vlan[VAL];

	if (*session_vlan(n, vlan))
		valf(out, "wan%s", vlan);
	else
		copy_val(out, gem);
	return out;
}

/* The digest of the account of session $1, which keeps the secret out. */
static char *account_digest(int n, char *out)
{
	uint8_t digest[SHA256_LEN];
	char path[PATH_MAX];
	struct sha256 h;
	struct buf b = { 0 };

	out[0] = '\0';
	if (!readable(PATHF(path, "%s/run/onu-pppoe%d", g_root, n)))
		return out;
	{
		char chunk[512];
		ssize_t got;
		int fd = open(path, O_RDONLY | O_CLOEXEC);

		while (fd >= 0 && (got = read(fd, chunk, sizeof(chunk))) > 0)
			buf_add(&b, chunk, (size_t)got);
		if (fd >= 0)
			close(fd);
	}
	sha256_init(&h);
	sha256_update(&h, buf_str(&b), b.len);
	sha256_final(&h, digest);
	buf_free(&b);
	for (size_t i = 0; i < SHA256_LEN; i++)
		snprintf(out + 2 * i, VAL - 2 * i, "%02x", digest[i]);
	return out;
}

/*
 * What session $1 on the GEM interface $2 is made of, one line: its
 * interface, its family and MAC address, its account (a digest), and what it
 * hands down by which link, the default routes among it. sessions_sync()
 * takes a session down and up again when this changes, and leaves the others
 * up: a change of one session leaves the other sessions as they are (ADR
 * 0011). It is in /run/session/N/makings, for root only.
 */
static void session_makings(int n, const char *gem, struct buf *out)
{
	char dev[VAL], lan[VAL], account[VAL], ipoe[16] = "";
	const char *role = "-", *role6 = "-";
	int k;

	session_dev(n, gem, dev);
	if (hands(n, "ipv4"))
		role = n == main_pass() ? "pass" : "routed";
	else if (strcmp(family(n), "ipv6") && is_main_route("pppoe", n))
		role = "route";
	if (hands(n, "ipv6"))
		role6 = n == main_pd() ? "pd-main" : "pd";
	k = ipoe_on_dev(dev);
	if (k)
		snprintf(ipoe, sizeof(ipoe), "%d", k);
	buf_reset(out);
	buf_printf(out, "%s %s %s mac=%s %s %s ", gem, dev, family(n), setting(n, "MAC"), role,
		   role6);
	buf_printf(out, "lan=%s %s ipoe=%s", down_vlan(n, lan), account_digest(n, account), ipoe);
}

/*
 * pppoed, an account of its own, reads the sessions' accounts: the group
 * pppoed's and no one else's but root's. An account put in place anew, by a
 * change taken or a trial that goes back, is root's alone until this runs, so
 * it runs after each such put. $1 is one session; without it (0), all of them.
 */
void accounts_to_pppoed(int only)
{
	for (int n = only ? only : 1; n <= (only ? only : MAX_SESSIONS); n++) {
		char path[PATH_MAX];

		if (!exists(PATHF(path, "%s/run/onu-pppoe%d", g_root, n)))
			continue;
		if (!give_group(path, "pppoed", 0640))
			log_msg("onu-pppoe%d: pppoed cannot read it", n);
	}
}

/*
 * Session $1 up on the GEM interface $2: its interface, made for the first
 * session on it, or again when it is gone (the kernel takes a VLAN interface
 * away with the GEM interface under it, which an OLT's new provisioning
 * replaces, before the other sessions on it have moved), room for PPPoE's
 * headers, and its client.
 */
static void session_up(int n, const char *gem)
{
	char dir[PATH_MAX], path[PATH_MAX], dev[VAL], vlan[VAL], mac[VAL], lan[VAL], v[VAL];
	const char *fam = family(n), *role = "none", *role6 = "none";
	struct buf makings = { 0 }, hands_text = { 0 };
	int pass = 0, route = 0;
	mode_t mask;

	session_dev(n, gem, dev);
	PATHF(dir, "%s/%d", SESSIONS, n);
	if (!dev_shared(dir, dev) || !exists(PATHF(path, "%s/%s", NET, dev))) {
		run(0, "onu-nl", "link", "set", gem, "up", NULL);
		if (strcmp(dev, gem))
			run(0, "onu-nl", "link", "add", "vlan", gem, dev, session_vlan(n, vlan),
			    NULL);
		copy_val(mac, setting(n, "MAC"));
		if (!*mac)
			pon_mac(mac);
		if (*mac && run(0, "onu-nl", "link", "set", dev, "address", mac, NULL))
			log_msg("session %d: the interface did not take its MAC address", n);
		if (strcmp(dev, gem))
			run(0, "onu-nl", "link", "set", dev, "up", NULL);
		pppoe_mtu(gem, dev);
	}
	mkdir_p(dir);
	/*
	 * The programs of the session write what they bring into its directory,
	 * as the accounts of the group onu (the boot script's, post-build.sh).
	 */
	if (!give_group(dir, "onu", 02770))
		log_msg("session %d: its directory is not the group onu's", n);
	accounts_to_pppoed(n);
	write_line(PATHF(path, "%s/gem", dir), gem);
	write_line(PATHF(path, "%s/dev", dir), dev);
	write_line(PATHF(path, "%s/family", dir), fam);
	if (hands(n, "ipv4")) {
		pass = 1;
		if (n == main_pass()) {
			route = 1;
			role = "pass";
		} else {
			role = "routed";
		}
	} else if (strcmp(fam, "ipv6") && is_main_route("pppoe", n)) {
		route = 1;
	}
	if (hands(n, "ipv6"))
		role6 = "pd";
	if (strcmp(role, "none") || strcmp(role6, "none"))
		write_line(PATHF(path, "%s/lan", dir), link_of(down_vlan(n, v), lan));
	/*
	 * What it hands down, for the status daemon, which shows what the stick
	 * does rather than work it out again.
	 */
	if (strcmp(role, "none"))
		buf_puts(&hands_text, "ipv4");
	if (strcmp(role6, "none"))
		buf_puts(&hands_text, hands_text.len ? " ipv6" : "ipv6");
	if (hands_text.len)
		buf_puts(&hands_text, "\n");
	write_text(PATHF(path, "%s/hands", dir), buf_str(&hands_text), hands_text.len);
	snprintf(v, sizeof(v), "%s %s", role, role6);
	write_line(PATHF(path, "%s/role", dir), v);
	session_makings(n, gem, &makings);
	mask = umask(077);
	write_line(PATHF(path, "%s/makings", dir), buf_str(&makings));
	umask(mask);
	session_net(n, dev, fam, pass, route);
	log_msg("session %d on %s (%s), GEM port %s", n, dev, fam,
		starts(gem, "gem") ? gem + 3 : gem);
	buf_free(&makings);
	buf_free(&hands_text);
}

/* Whether a running session other than $1 is on the GEM interface $2. */
static bool gem_shared(int n, const char *gem)
{
	struct words names = { 0 };
	bool shared = false;
	char own[PATH_MAX];

	PATHF(own, "%s/%d/gem", SESSIONS, n);
	dir_names(SESSIONS, &names);
	for (size_t i = 0; i < names.n && !shared; i++) {
		char path[PATH_MAX], v[VAL];

		PATHF(path, "%s/%s/gem", SESSIONS, names.v[i]);
		if (readable(path) && strcmp(path, own))
			shared = !strcmp(cat_val(path, v), gem);
	}
	words_free(&names);
	return shared;
}

/*
 * Session $1 down: its client, which ends the session with the concentrator,
 * and its interface when no other session is on it. Its names are its own:
 * sessions_sync() holds the GEM port it goes up on next.
 */
static void session_down(int n)
{
	char dir[PATH_MAX], path[PATH_MAX], dev[VAL], gem[VAL] = "", name[32];
	struct words left = { 0 };

	PATHF(dir, "%s/%d", SESSIONS, n);
	if (!readable(PATHF(path, "%s/dev", dir)))
		return;
	read_line(path, dev);
	read_line(PATHF(path, "%s/gem", dir), gem);
	snprintf(name, sizeof(name), "ses%d", n);
	if (run(0, "onu-net", "del", name, NULL))
		log_msg("session %d: netifd still has it", n);
	snprintf(name, sizeof(name), "dhcp6d%d", n);
	svc_stop(name);
	pass_lease_down(n);
	if (!dev_shared(dir, dev)) {
		if (strcmp(dev, gem))
			run(RUN_QUIET_ERR, "onu-nl", "link", "del", dev, NULL);
		/*
		 * What pppoe_mtu() raised goes back to Ethernet's MTU, the PON port
		 * last, once no session is on the GEM port.
		 */
		if (!gem_shared(n, gem))
			run(RUN_QUIET_ERR, "onu-nl", "link", "set", gem, "mtu", "1500", NULL);
	}
	rm_rf(dir);
	dir_names(SESSIONS, &left);
	if (!left.n)
		run(RUN_QUIET_ERR, "onu-nl", "link", "set", "pon", "mtu", "1500", NULL);
	words_free(&left);
	log_msg("session %d down", n);
}

void sessions_down(void)
{
	struct words names = { 0 };
	char path[PATH_MAX];

	dir_names(SESSIONS, &names);
	for (size_t i = 0; i < names.n; i++)
		if (is_dir(PATHF(path, "%s/%s", SESSIONS, names.v[i])))
			session_down((int)num_of(names.v[i]));
	words_free(&names);
	feeds_down();
	rm_f(PATHF(path, "%s/run/onu-dns", g_root));
}

/*
 * The host each session delegates its prefix to, which dhcp6d keeps in
 * /run/dhcp6d/pdN, goes into the store when it changes, and so does its going:
 * after a restart of the stick, dhcp6d routes the prefix to that host at once,
 * since a router that holds its delegation asks for none (measured:
 * NetworkManager kept its /56 for a day and asked for the name servers only).
 * What went into the store is in /run/onu-pdN.kept, which is root's alone.
 */
static void pd_keep(void)
{
	if (!have("onu-config"))
		return;
	for (int n = 1; n <= MAX_SESSIONS; n++) {
		char pd[PATH_MAX], kept[PATH_MAX], a[VAL], b[VAL], num[16];

		PATHF(pd, "%s/run/dhcp6d/pd%d", g_root, n);
		PATHF(kept, "%s/run/onu-pd%d.kept", g_root, n);
		if (!exists(pd) && !exists(kept))
			continue;
		if (!strcmp(cat_val(pd, a), cat_val(kept, b)))
			continue;
		snprintf(num, sizeof(num), "%d", n);
		if (run(0, "onu-config", "pd", num, NULL))
			continue;
		if (exists(pd))
			copy_file(pd, kept);
		else
			rm_f(kept);
	}
}

/*
 * Passthrough: each session follows its GEM port, a session whose makings
 * changed, or whose interface is gone, goes down and up again, and a session
 * without its account any more goes; the others stay up. The kernel takes a
 * VLAN interface away with the GEM interface it is on, which omcid makes anew
 * when the OLT provisions anew: the names are then the same, and only the
 * interface's absence says that the session is not there. Sessions that start
 * together authenticate apart, as pppoed under netifd waits 3 s for each
 * session before it (-w): an operator's concentrator refused one of two
 * sessions that authenticated at once, without saying why, and took each
 * alone (measured).
 */
void sessions_sync(void)
{
	int want[MAX_SESSIONS], cw = sessions(want);
	struct buf held = { 0 };

	/*
	 * The OLT's translations may have come since: omcid leaves the
	 * sessions' VLANs out as they now are, before the sessions move.
	 */
	held_want(&held);
	omcid_sync(buf_str(&held));
	buf_free(&held);
	for (int n = 1; n <= MAX_SESSIONS; n++) {
		char path[PATH_MAX], cur[VAL] = "", dev[VAL] = "", gem[VAL], d[VAL];
		struct buf mk = { 0 }, have_mk = { 0 };

		if (readable(PATHF(path, "%s/%d/gem", SESSIONS, n)))
			read_line(path, cur);
		if (readable(PATHF(path, "%s/%d/dev", SESSIONS, n)))
			read_line(path, dev);
		if (!in_list(want, cw, n)) {
			if (*cur)
				session_down(n);
			continue;
		}
		session_gem(n, gem);
		if (*gem) {
			bool changed = strcmp(gem, cur) || strcmp(session_dev(n, gem, d), dev) ||
				       (*cur && !exists(PATHF(path, "%s/%s", NET, dev)));

			if (!changed) {
				session_makings(n, gem, &mk);
				cat(PATHF(path, "%s/%d/makings", SESSIONS, n), &have_mk);
				changed = strcmp(buf_str(&mk), buf_str(&have_mk));
			}
			if (changed) {
				if (*cur)
					session_down(n);
				session_up(n, gem);
			}
		} else if (*cur) {
			log_msg("session %d: its GEM port is gone", n);
			session_down(n);
		}
		buf_free(&mk);
		buf_free(&have_mk);
	}
	pd_keep();
}
