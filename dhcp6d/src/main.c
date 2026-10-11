// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * dhcp6d's sockets, routes and files.
 *
 *   dhcp6d -w WAN -l LAN [-m IFACE] [-o DIR] [-b FILE] [-t TABLE] [-v]
 *       the prefix of WAN's servers delegated to the host on LAN; the DUID is
 *       IFACE's MAC address (WAN, a PPP link, has none); DIR gets the files
 *       "prefix" and "host"; FILE keeps the host's binding for the next
 *       start; with TABLE the default route goes in that routing table, by
 *       which what comes from the prefix is routed
 *   dhcp6d -c IFACE [-m IFACE] [-v]
 *       the client alone: the prefix a server on IFACE delegates, printed;
 *       it makes no route, as a host that takes its router from the
 *       advertisements would not
 *   dhcp6d -s PREFIX/LEN -l LAN [-d DNS] [-v]
 *       the server alone, delegating a fixed prefix, as an operator would
 *
 * The routes it makes are of protocol "dhcp": the default route through the
 * server that delegated the prefix, the prefix as unreachable, so that what
 * is not the host's goes nowhere rather than back out, and the prefix
 * through the host it is delegated to. With -t the default route is in a
 * table of its own, with a rule that routes by it what comes from the prefix:
 * beside another session that hands a prefix on, each host's traffic leaves
 * by the session its prefix came from. SIGTERM, SIGINT and SIGHUP take them
 * away, tell the host the stick is no longer its router, and release the
 * prefix.
 *
 * The host's binding outlives dhcp6d in the file of -b: a router that holds
 * its delegation asks for none when the stick comes back (measured:
 * NetworkManager, which kept handing out its /56 for a day and only asked
 * for the name servers), so the binding of before is taken back at the start,
 * and the prefix routed to that host as soon as it is delegated again. The
 * file is "DUID IAID ADDRESS PREFIX/LEN", the DUID in hex; it goes when the
 * host releases its binding or the binding runs out, not when dhcp6d stops.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fib_rules.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bus.h"
#include "dhcp6d.h"
#include "state.h"

#define ROUTE_METRIC 1024
#define UNREACHABLE_METRIC 4096
/* How long the host may keep the stick as its router, in seconds. */
#define RA_LIFETIME 1800
#define RA_INTERVAL_MIN 200000
#define RA_INTERVAL_MAX 600000
/* The first advertisements after a change, in ms apart (RFC 4861, 6.2.4). */
#define RA_BURST_GAP 4000
#define RA_BURST 3
#define RA_RS_GAP 3000

static volatile sig_atomic_t g_stop;
static int g_verbose;

void dhcp6d_log(int level, const char *fmt, ...)
{
	va_list ap;

	if (level > 6 && !g_verbose)
		return;
	fputs(level <= 3 ? "[ERR] " : level == 4 ? "[WRN] " :
		level == 6 ? "[INF] " : "[DBG] ", stderr);
	fputs("dhcp6d: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t seed(void)
{
	uint32_t s = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (read(fd, &s, sizeof(s)) != (ssize_t)sizeof(s))
			s = 0;
		close(fd);
	}
	return s ? s : (uint32_t)now_ms() | 1;
}

static int if_mac(const char *ifname, uint8_t mac[6])
{
	struct ifreq ifr;
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0), err = 0;

	if (fd < 0)
		return -errno;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFHWADDR, &ifr))
		err = -errno;
	else
		memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	close(fd);
	return err;
}

static uint32_t if_mtu(const char *ifname)
{
	struct ifreq ifr;
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	uint32_t mtu = 1280;

	if (fd < 0)
		return mtu;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (!ioctl(fd, SIOCGIFMTU, &ifr) && ifr.ifr_mtu >= 1280)
		mtu = (uint32_t)ifr.ifr_mtu;
	close(fd);
	return mtu;
}

/* Whether @ifname has its link-local address yet: sockets need it. */
static bool if_has_ll(const char *ifname)
{
	struct ifaddrs *list, *a;
	bool found = false;

	if (getifaddrs(&list))
		return false;
	for (a = list; a && !found; a = a->ifa_next) {
		const struct sockaddr_in6 *s6;

		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET6 ||
		    strcmp(a->ifa_name, ifname))
			continue;
		s6 = (const struct sockaddr_in6 *)(const void *)a->ifa_addr;
		found = IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr);
	}
	freeifaddrs(list);
	return found;
}

static const char *ntop(const uint8_t a[16], char *buf, size_t cap)
{
	return inet_ntop(AF_INET6, a, buf, (socklen_t)cap) ? buf : "?";
}

/* Send the request @req of @len bytes to the kernel; its answer, 0 or -errno. */
static int nl_request(const void *req, size_t len)
{
	struct sockaddr_nl to = { .nl_family = AF_NETLINK };
	uint8_t reply[512];
	int fd, err = 0;
	ssize_t n;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	if (sendto(fd, req, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
		err = -errno;
		goto out;
	}
	n = recv(fd, reply, sizeof(reply), 0);
	if (n < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
		err = n < 0 ? -errno : -EPROTO;
		goto out;
	}
	{
		const struct nlmsghdr *rh = (const struct nlmsghdr *)(void *)reply;

		if (rh->nlmsg_type == NLMSG_ERROR)
			err = ((const struct nlmsgerr *)NLMSG_DATA(rh))->error;
	}
out:
	close(fd);
	return err;
}

/* An attribute of @t with @l bytes of @v, at @off of the request @req. */
#define ATTR(t, v, l)                                                       \
	do {                                                                \
		struct rtattr *ra = (struct rtattr *)((uint8_t *)&req + off); \
		ra->rta_type = (t);                                         \
		ra->rta_len = (unsigned short)RTA_LENGTH(l);                \
		memcpy(RTA_DATA(ra), (v), (l));                             \
		off += RTA_ALIGN(ra->rta_len);                              \
	} while (0)

/* A route of @type to @dst/@plen, through @gw (or none) on @oif, in @table. */
static int route(bool add, uint8_t type, const uint8_t dst[16], uint8_t plen,
		 const uint8_t *gw, unsigned int oif, uint32_t metric, uint32_t table)
{
	struct {
		struct nlmsghdr nh;
		struct rtmsg rt;
		uint8_t attrs[128];
	} req;
	size_t off;
	int err;

	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
			     (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
	req.nh.nlmsg_seq = 1;
	req.rt.rtm_family = AF_INET6;
	req.rt.rtm_dst_len = plen;
	req.rt.rtm_table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC;
	req.rt.rtm_protocol = RTPROT_DHCP;
	req.rt.rtm_scope = RT_SCOPE_UNIVERSE;
	req.rt.rtm_type = type;
	off = NLMSG_ALIGN(sizeof(req.nh) + sizeof(req.rt));
	ATTR(RTA_DST, dst, 16);
	if (gw)
		ATTR(RTA_GATEWAY, gw, 16);
	if (oif)
		ATTR(RTA_OIF, &oif, sizeof(oif));
	ATTR(RTA_PRIORITY, &metric, sizeof(metric));
	ATTR(RTA_TABLE, &table, sizeof(table));
	req.nh.nlmsg_len = (uint32_t)off;

	err = nl_request(&req, off);
	/* Taking away a route that is not there is no failure. */
	if (!add && err == -ESRCH)
		err = 0;
	return err;
}

/* The rule that routes what comes from @src by @table. */
static int rule(bool add, const struct prefix *src, uint32_t table)
{
	struct {
		struct nlmsghdr nh;
		struct fib_rule_hdr fr;
		uint8_t attrs[64];
	} req;
	size_t off;
	int err;

	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_type = add ? RTM_NEWRULE : RTM_DELRULE;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_EXCL : 0);
	req.nh.nlmsg_seq = 1;
	req.fr.family = AF_INET6;
	req.fr.src_len = src->len;
	req.fr.action = FR_ACT_TO_TBL;
	req.fr.table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC;
	off = NLMSG_ALIGN(sizeof(req.nh) + sizeof(req.fr));
	ATTR(FRA_SRC, src->addr, 16);
	ATTR(FRA_TABLE, &table, sizeof(table));
	req.nh.nlmsg_len = (uint32_t)off;

	err = nl_request(&req, off);
	/* A rule that is there already, or gone already, is no failure. */
	if ((add && err == -EEXIST) || (!add && err == -ENOENT))
		err = 0;
	return err;
}
#undef ATTR

static int udp_socket(const char *ifname, uint16_t port, bool join_servers)
{
	struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	unsigned int idx = if_nametoindex(ifname);
	int one = 1;

	if (fd < 0)
		return -errno;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname)) ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx)) ||
	    bind(fd, (struct sockaddr *)&a, sizeof(a))) {
		int e = -errno;

		close(fd);
		return e;
	}
	if (join_servers) {
		struct ipv6_mreq mr = { .ipv6mr_interface = idx };

		inet_pton(AF_INET6, DHCP6_ALL_SERVERS, &mr.ipv6mr_multiaddr);
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mr, sizeof(mr))) {
			int e = -errno;

			close(fd);
			return e;
		}
	}
	return fd;
}

/* The socket for Router Solicitations in, and Advertisements out. */
static int nd_socket(const char *ifname)
{
	int fd = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_ICMPV6);
	unsigned int idx = if_nametoindex(ifname);
	struct icmp6_filter f;
	struct ipv6_mreq mr = { .ipv6mr_interface = idx };
	int hops = 255;

	if (fd < 0)
		return -errno;
	ICMP6_FILTER_SETBLOCKALL(&f);
	ICMP6_FILTER_SETPASS(ICMP6_ROUTER_SOLICIT, &f);
	inet_pton(AF_INET6, "ff02::2", &mr.ipv6mr_multiaddr);
	if (setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &f, sizeof(f)) ||
	    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname)) ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx)) ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops)) ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mr, sizeof(mr))) {
		int e = -errno;

		close(fd);
		return e;
	}
	return fd;
}

static int send_to(int fd, const uint8_t *buf, size_t len, const char *addr,
		   const uint8_t *addr16, uint16_t port, unsigned int idx)
{
	struct sockaddr_in6 to = { .sin6_family = AF_INET6, .sin6_port = htons(port),
				   .sin6_scope_id = idx };

	if (addr16)
		memcpy(&to.sin6_addr, addr16, 16);
	else
		inet_pton(AF_INET6, addr, &to.sin6_addr);
	if (sendto(fd, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) >= 0)
		return 0;
	/* A link-local address still being checked (DAD) cannot send yet. */
	if (errno != EADDRNOTAVAIL)
		DHCP6D_WARN("send: %s", strerror(errno));
	return -errno;
}

/* A host's binding as -b keeps it: DUID, IAID, address and prefix. */
#define BINDING_TEXT (2 * DHCP6_DUID_MAX + 2 * INET6_ADDRSTRLEN + 32)

/* The daemon's state across the loop. */
struct daemon {
	const char *wan, *lan, *dir, *binding;
	/* the name of its object on ubus, dhcp6d.NAME, or none */
	const char *name;
	/* the program told of a change of the binding, and the binding told */
	const char *notify;
	char binding_text[BINDING_TEXT];
	bool binding_known;
	unsigned int wan_idx, lan_idx;
	int csock, ssock, rsock;
	struct client client;
	struct server server;
	bool client_on, server_on, fixed, print;
	/* the server that delegated, which the default route goes through */
	uint8_t router[16];
	bool default_route;
	/* the table of the default route, and the rule into it of the prefix */
	uint32_t table;
	bool ruled;
	struct prefix rule_prefix;
	/* the prefix routed nowhere, and the one routed to the host */
	bool unreachable;
	struct prefix unreachable_prefix;
	bool routed;
	struct prefix routed_prefix;
	uint8_t routed_via[16];
	struct ra ra;
	int64_t ra_next;
	unsigned int ra_burst;
	int64_t ra_last;
};

/* The line @text in the file @path, written whole; no file without one. */
static void write_path(const char *path, const char *text)
{
	char tmp[264];
	FILE *f;

	if (!text) {
		unlink(path);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%s\n", text);
	if (fclose(f) || rename(tmp, path))
		unlink(tmp);
}

static void write_file(const struct daemon *d, const char *name, const char *text)
{
	char path[256];

	if (!d->dir)
		return;
	snprintf(path, sizeof(path), "%s/%s", d->dir, name);
	write_path(path, text);
}

/*
 * Tell the program of -x that the host's binding changed: run it with the
 * argument "dhcp6d", without waiting for it. It gets the daemon's standard
 * streams and nothing else of it, and the default dispositions of the
 * signals the daemon ignores, which a program would keep.
 */
static void notify_run(const struct daemon *d)
{
	pid_t pid;

	if (!d->notify)
		return;
	pid = fork();
	if (pid == 0) {
		long max = sysconf(_SC_OPEN_MAX);

		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		if (max <= 0 || max > 4096)
			max = 1024;
		for (int fd = 3; fd < max; fd++)
			close(fd);
		execl(d->notify, d->notify, "dhcp6d", (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		DHCP6D_WARN("%s was not told of the binding: %s", d->notify, strerror(errno));
}

/*
 * What the daemon has now, published on ubus when it changed (bus.h): the
 * prefix delegated to it, or the fixed one it delegates, and the host it
 * routes the prefix to.
 */
static void publish(const struct daemon *d, const struct prefix *fixed)
{
	char prefix[INET6_ADDRSTRLEN + 4], host[INET6_ADDRSTRLEN], a[INET6_ADDRSTRLEN];
	const struct prefix *p = fixed;
	struct dhcp6d_state st = {
		.name = d->name, .wan = d->wan, .lan = d->lan,
		.bound = d->server.bound, .routed = d->routed,
	};

	if (!d->name)
		return;
	if (d->client_on && d->client.bound)
		p = &d->client.prefix;
	if (p) {
		snprintf(prefix, sizeof(prefix), "%s/%u", ntop(p->addr, a, sizeof(a)), p->len);
		st.prefix = prefix;
	}
	if (d->routed)
		st.host = ntop(d->routed_via, host, sizeof(host));
	bus_publish(&st);
}

/* The host's binding as it is now, in the file of -b, and told when it changed. */
static void save_binding(struct daemon *d)
{
	char text[BINDING_TEXT] = "", a[INET6_ADDRSTRLEN], p[INET6_ADDRSTRLEN];
	const struct server *s = &d->server;
	size_t off = 0;

	if (!d->binding)
		return;
	if (s->bound) {
		for (size_t i = 0; i < s->host_len; i++)
			off += (size_t)snprintf(text + off, sizeof(text) - off, "%02x",
						s->host[i]);
		snprintf(text + off, sizeof(text) - off, " %u %s %s/%u", s->host_iaid,
			 ntop(s->host_addr, a, sizeof(a)), ntop(s->prefix.addr, p, sizeof(p)),
			 s->prefix.len);
	}
	if (d->binding_known && !strcmp(text, d->binding_text))
		return;
	write_path(d->binding, s->bound ? text : NULL);
	snprintf(d->binding_text, sizeof(d->binding_text), "%s", text);
	d->binding_known = true;
	notify_run(d);
}

/* Bring the routes to what the client and the server hold now. */
static void sync_routes(struct daemon *d)
{
	char a[INET6_ADDRSTRLEN], b[INET6_ADDRSTRLEN], text[128];
	/* Only a stick that delegates on routes; the client alone prints. */
	bool want_un = d->client_on && d->server_on && d->client.bound;
	bool want_host = d->server.bound && d->server.have;

	if (d->unreachable && (!want_un || memcmp(&d->unreachable_prefix,
						  &d->client.prefix, 17))) {
		route(false, RTN_UNREACHABLE, d->unreachable_prefix.addr,
		      d->unreachable_prefix.len, NULL, 0, UNREACHABLE_METRIC, RT_TABLE_MAIN);
		d->unreachable = false;
	}
	if (want_un && !d->unreachable) {
		d->unreachable_prefix = d->client.prefix;
		if (!route(true, RTN_UNREACHABLE, d->unreachable_prefix.addr,
			   d->unreachable_prefix.len, NULL, 0, UNREACHABLE_METRIC, RT_TABLE_MAIN))
			d->unreachable = true;
	}

	/* With a table of its own, what comes from the prefix goes by it. */
	if (d->ruled && (!want_un || !d->table ||
			 memcmp(&d->rule_prefix, &d->client.prefix, 17))) {
		rule(false, &d->rule_prefix, d->table);
		d->ruled = false;
	}
	if (want_un && d->table && !d->ruled) {
		int err = rule(true, &d->client.prefix, d->table);

		if (err) {
			DHCP6D_WARN("no rule into table %u: %s", d->table, strerror(-err));
		} else {
			d->ruled = true;
			d->rule_prefix = d->client.prefix;
		}
	}

	if (d->routed && (!want_host || memcmp(&d->routed_prefix, &d->server.prefix, 17) ||
			  memcmp(d->routed_via, d->server.host_addr, 16))) {
		route(false, RTN_UNICAST, d->routed_prefix.addr, d->routed_prefix.len,
		      d->routed_via, d->lan_idx, ROUTE_METRIC, RT_TABLE_MAIN);
		d->routed = false;
		write_file(d, "host", NULL);
	}
	if (want_host && !d->routed) {
		int err = route(true, RTN_UNICAST, d->server.prefix.addr,
				d->server.prefix.len, d->server.host_addr,
				d->lan_idx, ROUTE_METRIC, RT_TABLE_MAIN);

		if (err) {
			DHCP6D_WARN("no route to the host: %s", strerror(-err));
			return;
		}
		d->routed = true;
		d->routed_prefix = d->server.prefix;
		memcpy(d->routed_via, d->server.host_addr, 16);
		snprintf(text, sizeof(text), "%s", ntop(d->routed_via, a, sizeof(a)));
		write_file(d, "host", text);
		DHCP6D_INFO("%s/%u delegated to %s on %s",
			    ntop(d->routed_prefix.addr, b, sizeof(b)),
			    d->routed_prefix.len, a, d->lan);
	}
}

static void default_route(struct daemon *d, bool on)
{
	static const uint8_t any[16];
	const uint32_t table = d->table ? d->table : RT_TABLE_MAIN;
	int err;

	if (d->default_route) {
		route(false, RTN_UNICAST, any, 0, d->router, d->wan_idx, ROUTE_METRIC, table);
		d->default_route = false;
	}
	if (!on)
		return;
	err = route(true, RTN_UNICAST, any, 0, d->router, d->wan_idx, ROUTE_METRIC, table);
	if (err)
		DHCP6D_WARN("no default route: %s", strerror(-err));
	else
		d->default_route = true;
}

static void ra_send(struct daemon *d, int64_t now)
{
	uint8_t buf[256];
	size_t n;

	if (d->rsock < 0)
		return;
	d->ra.lifetime = d->server.have ? RA_LIFETIME : 0;
	d->ra.mtu = if_mtu(d->wan ? d->wan : d->lan);
	memcpy(d->ra.dns, d->server.dns, sizeof(d->ra.dns));
	d->ra.ndns = d->server.ndns;
	d->ra.dns_lifetime = 3 * RA_INTERVAL_MAX / 1000;
	n = ra_build(&d->ra, buf, sizeof(buf));
	if (n && send_to(d->rsock, buf, n, "ff02::1", NULL, 0, d->lan_idx) == -EADDRNOTAVAIL) {
		/* The link's address is not usable yet: again in a second. */
		d->ra_next = now + 1000;
		return;
	}
	DHCP6D_DEBUG("router advertisement, lifetime %u s", d->ra.lifetime);
	d->ra_last = now;
	if (d->ra_burst) {
		d->ra_burst--;
		d->ra_next = now + RA_BURST_GAP;
	} else {
		d->ra_next = now + RA_INTERVAL_MIN +
			     (int64_t)(seed() % (RA_INTERVAL_MAX - RA_INTERVAL_MIN));
	}
}

/* What changed in the client's binding goes to the server and the routes. */
static void client_events(struct daemon *d, unsigned int ev, int64_t now)
{
	char a[INET6_ADDRSTRLEN], text[64];

	if (ev & (CLIENT_EV_BOUND | CLIENT_EV_LOST)) {
		const struct prefix *p = d->client.bound ? &d->client.prefix : NULL;

		if (p) {
			snprintf(text, sizeof(text), "%s/%u",
				 ntop(p->addr, a, sizeof(a)), p->len);
			DHCP6D_INFO("prefix %s from %s", text, d->wan);
			write_file(d, "prefix", text);
			if (d->print) {
				printf("prefix %s\n", text);
				fflush(stdout);
			}
		} else {
			write_file(d, "prefix", NULL);
		}
		if (d->server_on)
			server_offer(&d->server, p, now);
		if (d->server_on)
			default_route(d, p != NULL);
		d->ra_burst = RA_BURST;
		d->ra_next = now;
	}
	if (ev & CLIENT_EV_DNS) {
		memcpy(d->server.dns, d->client.dns, sizeof(d->server.dns));
		d->server.ndns = d->client.ndns;
		d->ra_next = now;
	}
	sync_routes(d);
}

static void on_client(struct daemon *d, int64_t now, const uint8_t *in, size_t len,
		      const uint8_t *src)
{
	uint8_t out[DHCP6_MSG_MAX];
	unsigned int ev;
	size_t n;
	bool was = d->client.bound;

	n = client_step(&d->client, now, in, len, out, sizeof(out), &ev);
	if (n)
		send_to(d->csock, out, n, DHCP6_ALL_SERVERS, NULL, DHCP6_SERVER_PORT,
			d->wan_idx);
	/* A binding's server is the router: the default route goes through it. */
	if (src && d->client.bound && (!was || (ev & CLIENT_EV_BOUND)))
		memcpy(d->router, src, 16);
	if (ev)
		client_events(d, ev, now);
	/* A renewal of the same prefix carries new lifetimes on to the host. */
	if (d->client.bound && d->server_on && d->server.have) {
		d->server.prefix = d->client.prefix;
		if (d->server.bound)
			d->server.bound_until = d->client.prefix.valid_until;
	}
}

static void on_server(struct daemon *d, int64_t now)
{
	uint8_t in[DHCP6_MSG_MAX], out[DHCP6_MSG_MAX];
	struct sockaddr_in6 from;
	socklen_t flen = sizeof(from);
	unsigned int ev;
	ssize_t n;
	size_t m;

	n = recvfrom(d->ssock, in, sizeof(in), 0, (struct sockaddr *)&from, &flen);
	if (n <= 0)
		return;
	m = server_answer(&d->server, in, (size_t)n, from.sin6_addr.s6_addr, now,
			  out, sizeof(out), &ev);
	if (m)
		send_to(d->ssock, out, m, NULL, from.sin6_addr.s6_addr,
			ntohs(from.sin6_port), d->lan_idx);
	if (ev)
		save_binding(d);
	/* Even with nothing new: a route that failed before is tried again. */
	sync_routes(d);
}

static void on_rs(struct daemon *d, int64_t now)
{
	uint8_t buf[256];

	if (recv(d->rsock, buf, sizeof(buf), 0) <= 0)
		return;
	/* A solicitation is answered, but not more than once in 3 s. */
	if (now - d->ra_last >= RA_RS_GAP)
		d->ra_next = now;
}

static int print_usage(void)
{
	fprintf(stderr,
		"usage: dhcp6d -w WAN -l LAN [-m IFACE] [-o DIR] [-b FILE] [-t TABLE]\n"
		"              [-x PROGRAM] [-n NAME] [-v]\n"
		"       dhcp6d -c IFACE [-m IFACE] [-v]\n"
		"       dhcp6d -s PREFIX/LEN -l LAN [-d DNS] [-v]\n"
		"\n"
		"  -w -l  the prefix WAN's servers delegate, delegated on to the host\n"
		"         on LAN, with the stick as the host's router; -m names the\n"
		"         interface whose MAC address is the DUID, -o the directory of\n"
		"         the files \"prefix\" and \"host\"; -b the file that keeps the\n"
		"         host's binding for the next start; -t puts the default route\n"
		"         in TABLE, by which what comes from the prefix is routed; -x\n"
		"         runs PROGRAM with the argument dhcp6d when the binding changes;\n"
		"         -n publishes the state on ubus as the object dhcp6d.NAME.\n"
		"  -c     the client alone: print the prefix delegated on IFACE.\n"
		"  -s     the server alone: delegate PREFIX/LEN on LAN, with DNS.\n");
	return 2;
}

static int fixed_prefix(const char *arg, struct prefix *p)
{
	char addr[INET6_ADDRSTRLEN];
	const char *slash = strchr(arg, '/');
	long len;

	if (!slash || (size_t)(slash - arg) >= sizeof(addr))
		return -EINVAL;
	memcpy(addr, arg, (size_t)(slash - arg));
	addr[slash - arg] = '\0';
	len = strtol(slash + 1, NULL, 10);
	memset(p, 0, sizeof(*p));
	if (inet_pton(AF_INET6, addr, p->addr) != 1 || len < 1 || len > 128)
		return -EINVAL;
	p->len = (uint8_t)len;
	return 0;
}

/* The prefix of the first ADDRESS/LEN word of line @field of file @path. */
static bool prefix_in(const char *path, int field, struct prefix *p)
{
	char word[INET6_ADDRSTRLEN + 6];
	bool ok = false;
	FILE *f = fopen(path, "r");

	if (!f)
		return false;
	for (int i = 0; i <= field && fscanf(f, "%51s", word) == 1; i++)
		ok = i == field && !fixed_prefix(word, p);
	fclose(f);
	return ok;
}

/*
 * What a dhcp6d of before that did not end in order, killed, left: for the
 * prefix it last wrote to the file "prefix", which an orderly end removes,
 * and the one of the host's binding, the prefix unreachable, the prefix
 * through the host and the rule into the table; and the default route of
 * the table. A delegation that comes again makes them again; one of
 * another prefix would leave them. The kernel takes away only routes of
 * protocol dhcp, ours.
 */
static void clear_leftovers(const struct daemon *d)
{
	static const uint8_t any[16];
	struct prefix p[2];
	char path[256];
	int n = 0;

	if (d->dir) {
		snprintf(path, sizeof(path), "%s/prefix", d->dir);
		n += prefix_in(path, 0, &p[n]);
	}
	if (d->binding)
		n += prefix_in(d->binding, 3, &p[n]);
	for (int i = 0; i < n; i++) {
		route(false, RTN_UNREACHABLE, p[i].addr, p[i].len, NULL, 0,
		      UNREACHABLE_METRIC, RT_TABLE_MAIN);
		route(false, RTN_UNICAST, p[i].addr, p[i].len, NULL, 0,
		      ROUTE_METRIC, RT_TABLE_MAIN);
		if (d->table)
			rule(false, &p[i], d->table);
	}
	if (d->table)
		route(false, RTN_UNICAST, any, 0, NULL, 0, ROUTE_METRIC, d->table);
}

static int nibble(char c)
{
	return c >= '0' && c <= '9' ? c - '0' :
	       c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The host's binding of before, from the file of -b, back in the server. */
static void load_binding(struct daemon *d)
{
	char hex[2 * DHCP6_DUID_MAX + 2], iaid[12], addr[INET6_ADDRSTRLEN + 1],
		pfx[INET6_ADDRSTRLEN + 6], *end;
	uint8_t duid[DHCP6_DUID_MAX], ll[16];
	struct prefix was;
	unsigned long id;
	size_t len;
	FILE *f;
	int n;

	if (!d->binding)
		return;
	f = fopen(d->binding, "r");
	if (!f)
		return;
	n = fscanf(f, "%261s %11s %46s %51s", hex, iaid, addr, pfx);
	fclose(f);
	if (n != 4) {
		DHCP6D_WARN("%s: not a binding", d->binding);
		return;
	}
	len = strlen(hex) / 2;
	errno = 0;
	id = strtoul(iaid, &end, 10);
	if (strlen(hex) % 2 || !len || len > DHCP6_DUID_MAX || errno || *end || id > UINT32_MAX ||
	    inet_pton(AF_INET6, addr, ll) != 1 || fixed_prefix(pfx, &was)) {
		DHCP6D_WARN("%s: not a binding", d->binding);
		return;
	}
	for (size_t i = 0; i < len; i++) {
		int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);

		if (hi < 0 || lo < 0) {
			DHCP6D_WARN("%s: not a binding", d->binding);
			return;
		}
		duid[i] = (uint8_t)(hi << 4 | lo);
	}
	server_restore(&d->server, duid, len, (uint32_t)id, ll, &was);
	DHCP6D_INFO("the host of before, %s, gets %s again once it comes", addr, pfx);
}

int main(int argc, char **argv)
{
	struct daemon d = { .csock = -1, .ssock = -1, .rsock = -1 };
	const char *duid_if = NULL, *client_if = NULL, *fixed = NULL, *dns = NULL;
	struct sigaction sa = { .sa_handler = on_stop };
	uint8_t mac[6], duid[16];
	struct prefix fp;
	int opt;

	while ((opt = getopt(argc, argv, "w:l:m:o:b:c:s:d:t:x:n:v")) != -1) {
		switch (opt) {
		case 'w':
			d.wan = optarg;
			break;
		case 'l':
			d.lan = optarg;
			break;
		case 'm':
			duid_if = optarg;
			break;
		case 'o':
			d.dir = optarg;
			break;
		case 'b':
			d.binding = optarg;
			break;
		case 'c':
			client_if = optarg;
			break;
		case 's':
			fixed = optarg;
			break;
		case 'd':
			dns = optarg;
			break;
		case 'x':
			d.notify = optarg;
			break;
		case 'n':
			d.name = optarg;
			break;
		case 't': {
			char *end;
			unsigned long t = strtoul(optarg, &end, 10);

			/* Not the kernel's own tables: main, local, default. */
			if (*end || !t || t > UINT32_MAX - 1 || t == RT_TABLE_MAIN ||
			    t == RT_TABLE_LOCAL || t == RT_TABLE_DEFAULT)
				return print_usage();
			d.table = (uint32_t)t;
			break;
		}
		case 'v':
			g_verbose = 1;
			break;
		default:
			return print_usage();
		}
	}
	if (client_if) {
		if (d.wan || d.lan || fixed || d.table || d.binding || d.notify)
			return print_usage();
		d.wan = client_if;
		d.print = true;
	} else if (fixed) {
		if (d.wan || !d.lan || d.table || d.binding || d.notify ||
		    fixed_prefix(fixed, &fp))
			return print_usage();
		d.fixed = true;
	} else if (!d.wan || !d.lan) {
		return print_usage();
	}
	d.client_on = d.wan != NULL;
	d.server_on = d.lan != NULL;

	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (d.notify) {
		/* The daemon does not wait for its children, and none stays a zombie. */
		struct sigaction ign = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };

		sigemptyset(&ign.sa_mask);
		sigaction(SIGCHLD, &ign, NULL);
	}

	/* The links need their link-local addresses before their sockets. */
	for (int i = 0; !g_stop; i++) {
		if ((!d.wan || if_has_ll(d.wan)) && (!d.lan || if_has_ll(d.lan)))
			break;
		if (i == 30)
			DHCP6D_WARN("waiting for the link-local addresses");
		sleep(1);
	}

	if (d.client_on) {
		static const uint8_t none[6];

		d.wan_idx = if_nametoindex(d.wan);
		/* A PPP link has no MAC address: the DUID takes another's. */
		if ((if_mac(duid_if ? duid_if : d.wan, mac) || !memcmp(mac, none, 6)) &&
		    (!d.lan || if_mac(d.lan, mac) || !memcmp(mac, none, 6))) {
			DHCP6D_ERR("no MAC address for the DUID: name one with -m");
			return 1;
		}
		client_init(&d.client, duid, duid_ll(duid, mac), 1, seed(), now_ms());
		d.csock = udp_socket(d.wan, DHCP6_CLIENT_PORT, false);
		if (d.csock < 0) {
			DHCP6D_ERR("%s: %s", d.wan, strerror(-d.csock));
			return 1;
		}
	}
	if (d.server_on) {
		d.lan_idx = if_nametoindex(d.lan);
		if (if_mac(d.lan, d.ra.mac)) {
			DHCP6D_ERR("%s: no MAC address", d.lan);
			return 1;
		}
		server_init(&d.server, duid, duid_ll(duid, d.ra.mac));
		clear_leftovers(&d);
		load_binding(&d);
		d.ssock = udp_socket(d.lan, DHCP6_SERVER_PORT, true);
		if (d.ssock < 0) {
			DHCP6D_ERR("%s: %s", d.lan, strerror(-d.ssock));
			return 1;
		}
		if (!d.fixed) {
			d.rsock = nd_socket(d.lan);
			if (d.rsock < 0) {
				DHCP6D_ERR("%s: %s", d.lan, strerror(-d.rsock));
				return 1;
			}
		}
	}
	if (d.fixed && dns && inet_pton(AF_INET6, dns, d.server.dns[0]) == 1)
		d.server.ndns = 1;

	int64_t bus_retry = 0;

	while (!g_stop) {
		struct pollfd pf[4];
		int64_t now = now_ms(), deadline = INT64_MAX / 4;
		int nfd = 0, wait;

		/* ubusd may come later than dhcp6d, or start again. */
		if (d.name && bus_fd() < 0 && now >= bus_retry) {
			if (bus_open(d.name))
				bus_retry = now + 10000;
		}
		if (d.name && bus_fd() < 0 && bus_retry < deadline)
			deadline = bus_retry;

		/* The fixed prefix stays half an hour preferred, an hour valid. */
		if (d.fixed) {
			fp.preferred_until = now + 1800000;
			fp.valid_until = now + 3600000;
			server_offer(&d.server, &fp, now);
		}
		if (d.client_on) {
			on_client(&d, now, NULL, 0, NULL);
			deadline = client_deadline(&d.client);
		}
		if (d.rsock >= 0 && d.ra_next && d.ra_next <= now)
			ra_send(&d, now);
		if (d.rsock >= 0 && d.ra_next && d.ra_next < deadline)
			deadline = d.ra_next;
		if (d.server_on && server_expire(&d.server, now)) {
			save_binding(&d);
			sync_routes(&d);
		}
		if (d.server_on && d.server.bound && d.server.bound_until < deadline)
			deadline = d.server.bound_until;

		if (d.csock >= 0)
			pf[nfd++] = (struct pollfd){ .fd = d.csock, .events = POLLIN };
		if (d.ssock >= 0)
			pf[nfd++] = (struct pollfd){ .fd = d.ssock, .events = POLLIN };
		if (d.rsock >= 0)
			pf[nfd++] = (struct pollfd){ .fd = d.rsock, .events = POLLIN };
		if (bus_fd() >= 0)
			pf[nfd++] = (struct pollfd){ .fd = bus_fd(), .events = POLLIN };
		publish(&d, d.fixed ? &fp : NULL);
		wait = deadline - now > 60000 ? 60000 :
		       deadline - now < 0    ? 0 :
					       (int)(deadline - now);
		if (poll(pf, (nfds_t)nfd, wait) < 0 && errno != EINTR)
			break;
		now = now_ms();
		for (int i = 0; i < nfd; i++) {
			if (!(pf[i].revents & POLLIN))
				continue;
			if (pf[i].fd == d.csock) {
				uint8_t in[DHCP6_MSG_MAX];
				struct sockaddr_in6 from;
				socklen_t flen = sizeof(from);
				ssize_t n = recvfrom(d.csock, in, sizeof(in), 0,
						     (struct sockaddr *)&from, &flen);

				if (n > 0)
					on_client(&d, now, in, (size_t)n,
						  from.sin6_addr.s6_addr);
			} else if (pf[i].fd == d.ssock) {
				on_server(&d, now);
			} else if (pf[i].fd == d.rsock) {
				on_rs(&d, now);
			} else {
				bus_handle();
			}
		}
	}

	/* Stopping: the host learns the stick is no longer its router. */
	DHCP6D_INFO("stopping");
	if (d.server_on) {
		server_offer(&d.server, NULL, now_ms());
		d.server.bound = false;
		d.ra_burst = 0;
		ra_send(&d, now_ms());
		sync_routes(&d);
	}
	if (d.client_on) {
		uint8_t out[DHCP6_MSG_MAX];
		size_t n = client_release(&d.client, now_ms(), out, sizeof(out));

		if (n)
			send_to(d.csock, out, n, DHCP6_ALL_SERVERS, NULL,
				DHCP6_SERVER_PORT, d.wan_idx);
		sync_routes(&d);
		if (d.server_on)
			default_route(&d, false);
	}
	write_file(&d, "prefix", NULL);
	write_file(&d, "host", NULL);
	bus_close();
	return 0;
}
