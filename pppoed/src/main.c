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
#include <sys/prctl.h>

#include "nproto.h"
#include "pppoed.h"

/* Where pppoed is, for netifd to run it (cmd_proto()). */
#define PPPOED_PATH "/usr/sbin/pppoed"
/*
 * The session runs as the account pppoed, started by onu-drop (onu-utils)
 * with the two powers it uses: the raw socket of the discovery, and the
 * PPP unit with its interface and addresses.
 */
#define PPPOED_DROP "/usr/lib/onu/onu-drop"

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
		"                 [-D] [-P] [-M] [-4 | -6] [-k SECONDS] [-n TRIES] [-t MS] [-v]\n"
		"                 [-N INTERFACE] [-o DIR] [-w SECONDS]\n"
		"       pppoed discover -i IFACE [-s SERVICE] [-n TRIES] [-t MS] [-T] [-A] [-v]\n"
		"       pppoed sniff -i IFACE [-t MS] [-e ETHERTYPE]\n"
		"\n"
		"  up        open a session: discovery, LCP, authentication, IPCP and\n"
		"            IPv6CP; then put the addresses on the ppp interface and\n"
		"            keep the link until the peer ends it, or for -k seconds.\n"
		"            -4 leaves IPv6 out, -6 leaves IPv4 out.\n"
		"            -f reads \"user secret\" from a file; -u and -p put the\n"
		"            secret on the command line, where every process sees it.\n"
		"            -D adds a default route through the link.\n"
		"            -P passes the IPv4 address on: it goes on no interface\n"
		"            here, for a host behind this one to take.\n"
		"            -M sends packets as large as the discovery agreed to\n"
		"            (RFC 4638) when the peer's LCP asks for 1492.\n"
		"            -N reports the session to netifd's INTERFACE, which puts\n"
		"            the address and the default route in place; -o writes it\n"
		"            to the files of DIR (ppp, ipv4, ipv6, dns, peer) while it\n"
		"            lasts; -w waits SECONDS before the discovery.\n"
		"  discover  run the discovery alone and print the session; -T ends\n"
		"            the session with a PADT, -A listens to every ethertype.\n"
		"  sniff     print the frames seen on an interface; nothing is sent.\n"
		"\n"
		"As /lib/netifd/proto/pppoed.sh it is netifd's protocol \"pppoed\".\n");
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
/* The fact @name of the session in @dir, @value, or none for NULL. */
static void fact(const char *dir, const char *name, const char *value)
{
	char path[256];
	FILE *f;

	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!value) {
		unlink(path);
		return;
	}
	f = fopen(path, "we");
	if (!f) {
		PPPOED_WARN("%s: %s", path, strerror(errno));
		return;
	}
	fprintf(f, "%s\n", value);
	fclose(f);
}

/*
 * No fact of the session in @dir: before it starts, as one that was killed
 * left its own, and after it ends.
 */
static void facts_clear(const char *dir)
{
	for (const char *const *f = (const char *const[]){ "ppp", "ipv4", "ipv6", "dns", "peer", NULL };
	     *f; f++)
		fact(dir, *f, NULL);
}

/*
 * A session that does not come up under netifd ends after a while, so that
 * netifd, which starts it again at once, does not do so in a loop.
 */
static int give_up(const char *netifd)
{
	if (netifd)
		for (int i = 0; i < 5 && !pppoed_stop; i++)
			sleep(1);
	return 1;
}

/*
 * Tell netifd that the link is up. netifd may be reconnecting to a ubusd that
 * started again and has not published its interfaces yet, so the message is
 * sent again for a few seconds before it is given up.
 */
#define TELL_TRIES 10

static int tell_netifd(const char *netifd, const char *json)
{
	for (int i = 0; i < TELL_TRIES && !pppoed_stop; i++) {
		if (!nproto_send(netifd, json))
			return 0;
		sleep(1);
	}
	return -EIO;
}

static int cmd_up(int argc, char **argv)
{
	const char *ifname = NULL, *service = "", *credfile = NULL;
	const char *netifd = NULL, *facts = NULL;
	char user[64] = "", secret[256] = "";
	unsigned int tries = 3, timeout = 3000, keep = 0, wait = 0;
	bool defaultroute = false, pass_on = false, take_payload = false;
	bool want_ipv4 = true, want_ipv6 = true;
	struct sigaction sa = { .sa_handler = on_stop };
	struct sigaction ign = { .sa_handler = SIG_IGN };
	pid_t parent = getppid();
	struct pppoe_session s;
	struct ppp_link l;
	int fd, err, opt;

	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	/*
	 * A line to a log whose reader is gone fails instead of ending the
	 * process before the session ends as a peer expects.
	 */
	sigemptyset(&ign.sa_mask);
	sigaction(SIGPIPE, &ign, NULL);

	while ((opt = getopt(argc, argv, "i:s:u:p:f:n:t:k:DPM46vN:o:w:")) != -1) {
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
		case 'M':
			take_payload = true;
			break;
		case '4':
			want_ipv6 = false;
			break;
		case '6':
			want_ipv4 = false;
			break;
		case 'v':
			g_verbose = 1;
			break;
		case 'N':
			netifd = optarg;
			break;
		case 'o':
			facts = optarg;
			break;
		case 'w':
			wait = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		default:
			return print_usage();
		}
	}
	if (!ifname || !tries || !timeout || (!want_ipv4 && !want_ipv6))
		return print_usage();
	/*
	 * Under netifd the session ends with netifd, as on its SIGTERM: a
	 * netifd that procd starts again opens a session of its own, and one
	 * left behind would stay open beside it.
	 */
	if (netifd) {
		if (prctl(PR_SET_PDEATHSIG, SIGTERM))
			PPPOED_WARN("the session may outlive netifd: %s", strerror(errno));
		if (getppid() != parent)
			pppoed_stop = 1;
	}
	facts_clear(facts);

	/* Sessions that ended together start again apart. */
	while (wait-- && !pppoed_stop)
		sleep(1);
	if (pppoed_stop)
		return 0;

	if (credfile) {
		err = creds_from_file(credfile, user, sizeof(user), secret,
				      sizeof(secret));
		if (err) {
			PPPOED_ERR("%s: %s", credfile, strerror(-err));
			return give_up(netifd);
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
		return give_up(netifd);
	}

	err = pppoe_discover(fd, &s, service, tries, timeout);
	if (err) {
		PPPOED_ERR("discovery failed: %s", strerror(-err));
		close(fd);
		return give_up(netifd);
	}

	err = ppp_open(&s, &l);
	if (err) {
		PPPOED_ERR("cannot take the session to the kernel: %s",
			   strerror(-err));
		pppoe_terminate(fd, &s);
		close(fd);
		return give_up(netifd);
	}
	l.take_payload = take_payload;

	err = ppp_negotiate(&l, user, secret, want_ipv4, want_ipv6);
	if (err) {
		PPPOED_ERR("negotiation failed: %s", strerror(-err));
		ppp_close(&l);
		pppoe_terminate(fd, &s);
		close(fd);
		return give_up(netifd);
	}

	/*
	 * Under netifd the address and the default route are netifd's to put,
	 * which it takes away from an interface it holds otherwise: here the
	 * link only comes up, with its MTU and its IPv6 link-local address.
	 */
	if (netifd)
		err = ppp_ifconfig(&l, false, true);
	else
		err = ppp_ifconfig(&l, defaultroute, pass_on);
	if (err) {
		PPPOED_ERR("cannot configure %s: %s", l.ifname, strerror(-err));
		ppp_terminate(&l);
		ppp_close(&l);
		pppoe_terminate(fd, &s);
		close(fd);
		return give_up(netifd);
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
			fact(facts, "ipv6", ll);
		}
		fflush(stdout);

		if (l.ipv4_up) {
			char dns[2 * INET_ADDRSTRLEN];

			snprintf(dns, sizeof(dns), "%s%s%s", l.dns1 ? s1 : "",
				 l.dns1 && l.dns2 ? " " : "", l.dns2 ? s2 : "");
			fact(facts, "ipv4", sl);
			fact(facts, "peer", sp);
			fact(facts, "dns", *dns ? dns : NULL);
		}
		fact(facts, "ppp", l.ifname);

		if (netifd) {
			struct nproto_link nl = {
				.ifname = l.ifname,
				.local = l.ipv4_up ? sl : NULL,
				.peer = l.ipv4_up && l.peer_ip ? sp : NULL,
				.dns = { l.dns1 ? s1 : NULL, l.dns2 ? s2 : NULL },
				.session = s.sid,
				.pass = pass_on,
				.defaultroute = defaultroute,
			};
			char json[1024];

			if (nproto_up_json(json, sizeof(json), &nl) < 0 ||
			    tell_netifd(netifd, json)) {
				/*
				 * A link that netifd does not know is up stays
				 * pending for ever: end it, and netifd opens another.
				 */
				PPPOED_ERR("netifd was not told that %s is up: the session ends",
					   l.ifname);
				ppp_terminate(&l);
				ppp_close(&l);
				pppoe_terminate(fd, &s);
				close(fd);
				facts_clear(facts);
				return give_up(netifd);
			}
		}
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
	facts_clear(facts);
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

/*
 * netifd's protocol handler (nproto.h): "'' dump", or "pppoed setup|teardown
 * IFACE CONFIG [DEVICE]". At setup netifd runs pppoed for the session that
 * CONFIG names, on DEVICE, as the account pppoed, with the session's account
 * in /run/onu-pppoeN, and its facts in /run/session/N; at teardown it ends
 * it.
 */
static int cmd_proto(int argc, char **argv)
{
	char json[1024], family[16] = "both", account[64], dir[64], wait[12];
	const char *iface = argc > 3 ? argv[3] : NULL;
	const char *config = argc > 4 ? argv[4] : NULL;
	const char *device = argc > 5 ? argv[5] : NULL;
	const char *cmd[32];
	int session, c = 0;

	if (!strcmp(argv[2], "dump")) {
		if (nproto_dump_json(json, sizeof(json)) < 0)
			return 1;
		puts(json);
		return 0;
	}
	if (!iface)
		return print_usage();
	if (!strcmp(argv[2], "teardown"))
		return nproto_kill_json(json, sizeof(json)) < 0 || nproto_send(iface, json);

	session = nproto_session(config);
	if (session < 0 || !device) {
		PPPOED_ERR("%s: no session 1 to 7, or no device", iface);
		return 1;
	}
	nproto_string(config, "family", family, sizeof(family));
	snprintf(account, sizeof(account), "/run/onu-pppoe%d", session);
	snprintf(dir, sizeof(dir), "/run/session/%d", session);
	snprintf(wait, sizeof(wait), "%d", 3 * (session - 1));
	cmd[c++] = PPPOED_DROP;
	cmd[c++] = "-c";
	cmd[c++] = "net_raw,net_admin";
	cmd[c++] = "pppoed";
	cmd[c++] = PPPOED_PATH;
	cmd[c++] = "up";
	cmd[c++] = "-i";
	cmd[c++] = device;
	cmd[c++] = "-f";
	cmd[c++] = account;
	cmd[c++] = "-M";
	cmd[c++] = "-N";
	cmd[c++] = iface;
	cmd[c++] = "-o";
	cmd[c++] = dir;
	cmd[c++] = "-w";
	cmd[c++] = wait;
	if (!strcmp(family, "ipv4"))
		cmd[c++] = "-4";
	else if (!strcmp(family, "ipv6"))
		cmd[c++] = "-6";
	if (nproto_bool(config, "pass") == 1)
		cmd[c++] = "-P";
	if (nproto_bool(config, "defaultroute") == 1)
		cmd[c++] = "-D";
	cmd[c] = NULL;
	return nproto_run_json(json, sizeof(json), cmd) < 0 || nproto_send(iface, json);
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return print_usage();

	if (argc >= 3 && (!strcmp(argv[2], "dump") || !strcmp(argv[2], "setup") ||
			  !strcmp(argv[2], "teardown")))
		return cmd_proto(argc, argv);

	if (strcmp(argv[1], "sniff") == 0)
		return cmd_sniff(argc - 1, argv + 1);

	if (strcmp(argv[1], "discover") == 0)
		return cmd_discover(argc - 1, argv + 1);
	if (strcmp(argv[1], "up") == 0)
		return cmd_up(argc - 1, argv + 1);

	return print_usage();
}
