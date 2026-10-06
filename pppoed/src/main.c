// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The commands of pppoed: up, discover and sniff. */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "pppoed.h"

static int g_verbose;

void pppoed_log(int level, const char *fmt, ...)
{
	static const char *const tag[] = {
		[3] = "ERR", [4] = "WRN", [6] = "INF", [7] = "DBG"
	};
	va_list ap;

	if (level == 7 && !g_verbose)
		return;

	fprintf(stderr, "[%s] pppoed: ",
		(level >= 0 && level <= 7 && tag[level]) ? tag[level] : "---");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* Read "user secret" from the first line of a file. */
static int creds_from_file(const char *path, char *user, size_t ulen,
			   char *secret, size_t slen)
{
	char line[512];
	FILE *f = fopen(path, "re");
	int n;

	if (!f)
		return -errno;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);

	n = sscanf(line, "%63s %255s", user, secret);
	(void)ulen;
	(void)slen;
	return n == 2 ? 0 : -EINVAL;
}

/* fe80::@iid as text. */
static void print_link_local(char *out, size_t cap, const uint8_t *iid)
{
	struct in6_addr a;

	memset(&a, 0, sizeof(a));
	a.s6_addr[0] = 0xfe;
	a.s6_addr[1] = 0x80;
	memcpy(a.s6_addr + 8, iid, PPP_IID_LEN);
	if (!inet_ntop(AF_INET6, &a, out, (socklen_t)cap))
		snprintf(out, cap, "?");
}

static int print_usage(void)
{
	fprintf(stderr,
		"usage: pppoed up -i IFACE (-f FILE | -u USER -p SECRET) [-s SERVICE]\n"
		"                 [-D] [-P] [-4] [-k SECONDS] [-n TRIES] [-t MS] [-v]\n"
		"       pppoed discover -i IFACE [-s SERVICE] [-n TRIES] [-t MS] [-T] [-A] [-v]\n"
		"       pppoed sniff -i IFACE [-t MS] [-e ETHERTYPE]\n"
		"\n"
		"  up        open a session: discovery, LCP, authentication, IPCP and\n"
		"            IPv6CP; then put the addresses on the ppp interface and\n"
		"            keep the link until the peer ends it, or for -k seconds.\n"
		"            -4 leaves IPv6 out.\n"
		"            -f reads \"user secret\" from a file; -u and -p put the\n"
		"            secret on the command line, where every process sees it.\n"
		"            -D adds a default route through the link.\n"
		"            -P passes the IPv4 address on: it goes on no interface\n"
		"            here, for a host behind this one to take.\n"
		"  discover  run the discovery alone and print the session; -T ends\n"
		"            the session with a PADT, -A listens to every ethertype.\n"
		"  sniff     print the frames seen on an interface; nothing is sent.\n");
	return 2;
}

static int cmd_discover(int argc, char **argv)
{
	const char *ifname = NULL, *service = "";
	unsigned int tries = 3, timeout = 3000;
	bool terminate = false, all_proto = false;
	struct pppoe_session s;
	int fd, err, opt;

	while ((opt = getopt(argc, argv, "i:s:n:t:TAv")) != -1) {
		switch (opt) {
		case 'i':
			ifname = optarg;
			break;
		case 's':
			service = optarg;
			break;
		case 'n':
			tries = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 't':
			timeout = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'T':
			terminate = true;
			break;
		case 'A':
			all_proto = true;
			g_verbose = 1;
			break;
		case 'v':
			g_verbose = 1;
			break;
		default:
			return print_usage();
		}
	}
	if (!ifname || !tries || !timeout)
		return print_usage();

	fd = pppoe_disc_open(ifname, &s, all_proto);
	if (fd < 0) {
		PPPOED_ERR("cannot open a discovery socket on %s: %s", ifname,
			   strerror(-fd));
		return 1;
	}
	PPPOED_DEBUG("%s is ifindex %u, %02x:%02x:%02x:%02x:%02x:%02x",
		     s.ifname, s.ifindex, s.local[0], s.local[1], s.local[2],
		     s.local[3], s.local[4], s.local[5]);

	err = pppoe_discover(fd, &s, service, tries, timeout);
	if (err) {
		if (err == -ETIMEDOUT)
			PPPOED_ERR(
				"no PADO or PADS in %u attempt(s) of %u ms on %s: nothing on this segment answers PPPoE",
				tries, timeout, ifname);
		else
			PPPOED_ERR("discovery failed: %s", strerror(-err));
		close(fd);
		return 1;
	}

	printf("session %u peer %02x:%02x:%02x:%02x:%02x:%02x ac %s\n", s.sid,
	       s.peer[0], s.peer[1], s.peer[2], s.peer[3], s.peer[4], s.peer[5],
	       s.ac_name[0] ? s.ac_name : "-");

	if (terminate) {
		err = pppoe_terminate(fd, &s);
		if (err)
			PPPOED_WARN("PADT could not be sent: %s",
				    strerror(-err));
		else
			PPPOED_INFO("session %u terminated with a PADT", s.sid);
	}

	close(fd);
	return 0;
}

static void on_stop(int sig)
{
	(void)sig;
	pppoed_stop = 1;
}

/*
 * Open a session and keep it. The MD5 test runs first: a wrong digest
 * would look like a wrong secret to the peer. SIGTERM, SIGINT and SIGHUP end
 * the session as a peer expects, with an LCP Terminate-Request and a PADT, so
 * that the concentrator frees it at once instead of after its echo timeout;
 * a session that does not come up after its discovery gets a PADT too.
 */
static int cmd_up(int argc, char **argv)
{
	const char *ifname = NULL, *service = "", *credfile = NULL;
	char user[64] = "", secret[256] = "";
	unsigned int tries = 3, timeout = 3000, keep = 0;
	bool defaultroute = false, pass_on = false, want_ipv6 = true;
	struct sigaction sa = { .sa_handler = on_stop };
	struct pppoe_session s;
	struct ppp_link l;
	int fd, err, opt;

	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	while ((opt = getopt(argc, argv, "i:s:u:p:f:n:t:k:DP4v")) != -1) {
		switch (opt) {
		case 'i':
			ifname = optarg;
			break;
		case 's':
			service = optarg;
			break;
		case 'u':
			snprintf(user, sizeof(user), "%s", optarg);
			break;
		case 'p':
			snprintf(secret, sizeof(secret), "%s", optarg);
			break;
		case 'f':
			credfile = optarg;
			break;
		case 'n':
			tries = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 't':
			timeout = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'k':
			keep = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'D':
			defaultroute = true;
			break;
		case 'P':
			pass_on = true;
			break;
		case '4':
			want_ipv6 = false;
			break;
		case 'v':
			g_verbose = 1;
			break;
		default:
			return print_usage();
		}
	}
	if (!ifname || !tries || !timeout)
		return print_usage();

	if (credfile) {
		err = creds_from_file(credfile, user, sizeof(user), secret,
				      sizeof(secret));
		if (err) {
			PPPOED_ERR("%s: %s", credfile, strerror(-err));
			return 1;
		}
	}

	if (!md5_selftest()) {
		PPPOED_ERR(
			"the MD5 self-test failed; CHAP would silently be wrong");
		return 1;
	}

	fd = pppoe_disc_open(ifname, &s, false);
	if (fd < 0) {
		PPPOED_ERR("cannot open a discovery socket on %s: %s", ifname,
			   strerror(-fd));
		return 1;
	}

	err = pppoe_discover(fd, &s, service, tries, timeout);
	if (err) {
		PPPOED_ERR("discovery failed: %s", strerror(-err));
		close(fd);
		return 1;
	}

	err = ppp_open(&s, &l);
	if (err) {
		PPPOED_ERR("cannot take the session to the kernel: %s",
			   strerror(-err));
		pppoe_terminate(fd, &s);
		close(fd);
		return 1;
	}

	err = ppp_negotiate(&l, user, secret, want_ipv6);
	if (err) {
		PPPOED_ERR("negotiation failed: %s", strerror(-err));
		ppp_close(&l);
		pppoe_terminate(fd, &s);
		close(fd);
		return 1;
	}

	err = ppp_ifconfig(&l, defaultroute, pass_on);
	if (err) {
		PPPOED_ERR("cannot configure %s: %s", l.ifname, strerror(-err));
		ppp_terminate(&l);
		ppp_close(&l);
		pppoe_terminate(fd, &s);
		close(fd);
		return 1;
	}

	{
		struct in_addr a = { .s_addr = l.local_ip };
		struct in_addr b = { .s_addr = l.peer_ip };
		struct in_addr d1 = { .s_addr = l.dns1 };
		struct in_addr d2 = { .s_addr = l.dns2 };
		char sl[INET_ADDRSTRLEN], sp[INET_ADDRSTRLEN];
		char s1[INET_ADDRSTRLEN], s2[INET_ADDRSTRLEN];

		snprintf(sl, sizeof(sl), "%s", inet_ntoa(a));
		snprintf(sp, sizeof(sp), "%s", inet_ntoa(b));
		snprintf(s1, sizeof(s1), "%s", inet_ntoa(d1));
		snprintf(s2, sizeof(s2), "%s", inet_ntoa(d2));
		if (l.ipv4_up)
			printf("up %s local %s peer %s session %u dns %s %s\n",
			       l.ifname, sl, sp, s.sid, l.dns1 ? s1 : "-",
			       l.dns2 ? s2 : "-");
		if (l.ipv6_up) {
			char ll[INET6_ADDRSTRLEN], pl[INET6_ADDRSTRLEN];

			print_link_local(ll, sizeof(ll), l.iid_local);
			print_link_local(pl, sizeof(pl), l.iid_peer);
			printf("up6 %s local %s peer %s session %u\n", l.ifname,
			       ll, pl, s.sid);
		}
		fflush(stdout);
	}

	err = ppp_run(&l, keep);
	if (err == -EINTR)
		PPPOED_INFO("stopping: the session ends");
	else if (err)
		PPPOED_WARN("the link ended: %s", strerror(-err));

	ppp_terminate(&l);
	ppp_close(&l);
	pppoe_terminate(fd, &s);
	close(fd);
	return 0;
}

static int cmd_sniff(int argc, char **argv)
{
	const char *ifname = NULL;
	unsigned int ms = 5000, want = 0;
	struct pppoe_session s;
	int fd, opt;
	long deadline;

	while ((opt = getopt(argc, argv, "i:t:e:")) != -1) {
		switch (opt) {
		case 'i':
			ifname = optarg;
			break;
		case 't':
			ms = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'e':
			want = (unsigned int)strtoul(optarg, NULL, 16);
			break;
		default:
			return print_usage();
		}
	}
	if (!ifname)
		return print_usage();

	fd = pppoe_disc_open(ifname, &s, true);
	if (fd < 0) {
		PPPOED_ERR("cannot listen on %s: %s", ifname, strerror(-fd));
		return 1;
	}

	deadline = pppoed_now_ms() + (long)ms;
	for (;;) {
		uint8_t buf[PPPOE_FRAME_MAX];
		long left = deadline - pppoed_now_ms();
		ssize_t n;
		uint16_t et;

		if (left <= 0)
			break;
		n = pppoed_wait_frame(fd, buf, sizeof(buf), (int)left);
		if (n < 0)
			break;
		if (n < 14)
			continue;
		et = pppoe_get16(buf + 12);
		if (want && et != want)
			continue;
		printf("%02x:%02x:%02x:%02x:%02x:%02x > %02x:%02x:%02x:%02x:%02x:%02x  type %04x  %zd B%s\n",
		       buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], buf[0],
		       buf[1], buf[2], buf[3], buf[4], buf[5], et, n,
		       (et == 0x8863 || et == 0x8864) ?
			       (buf[15] == 0x00 ? "  PPPoE session" :
						  "  PPPoE discovery") :
			       "");
		fflush(stdout);
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return print_usage();

	if (strcmp(argv[1], "sniff") == 0)
		return cmd_sniff(argc - 1, argv + 1);

	if (strcmp(argv[1], "discover") == 0)
		return cmd_discover(argc - 1, argv + 1);
	if (strcmp(argv[1], "up") == 0)
		return cmd_up(argc - 1, argv + 1);

	return print_usage();
}
