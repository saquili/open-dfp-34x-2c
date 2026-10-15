// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * mcsniff: a passive look at the multicast of a stick, for a bench or a unit
 * that cannot run tcpdump.
 *
 *   mcsniff SECONDS IFACE...
 *
 * Listens on each interface for SECONDS seconds and prints one line for each
 * IGMP message (query, reports, leave) and each MLD message, with the time,
 * the interface, the direction, the VLAN tag and priority, the addresses and
 * the group, and once a second one line for each stream of IPv4 multicast
 * data (interface, VLAN, group, frames, bytes). Nothing else is printed, and
 * nothing is sent.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>

#define MAX_IF 8
#define MAX_STREAM 32

struct stream {
	int ifi;
	int vlan;
	uint32_t group;
	unsigned long frames, bytes;
};

static const char *names[MAX_IF];
static struct stream streams[MAX_STREAM];
static int nstream;

static void stamp(char *out, size_t cap)
{
	struct timespec ts;
	struct tm tm;

	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	snprintf(out, cap, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min,
		 tm.tm_sec, ts.tv_nsec / 1000000);
}

static void flush_streams(void)
{
	char t[64];

	stamp(t, sizeof(t));
	for (int i = 0; i < nstream; i++) {
		struct in_addr a = { .s_addr = streams[i].group };

		printf("%s %-8s stream vlan=%d group=%s frames=%lu bytes=%lu\n", t,
		       names[streams[i].ifi], streams[i].vlan, inet_ntoa(a),
		       streams[i].frames, streams[i].bytes);
	}
	nstream = 0;
	fflush(stdout);
}

static void count_stream(int ifi, int vlan, uint32_t group, size_t len)
{
	for (int i = 0; i < nstream; i++) {
		if (streams[i].ifi == ifi && streams[i].vlan == vlan &&
		    streams[i].group == group) {
			streams[i].frames++;
			streams[i].bytes += len;
			return;
		}
	}
	if (nstream < MAX_STREAM) {
		streams[nstream] = (struct stream){ ifi, vlan, group, 1, len };
		nstream++;
	}
}

static void packet(int ifi, const uint8_t *p, size_t len, int pkttype,
		   int vlan, int pcp)
{
	const char *dir = pkttype == PACKET_OUTGOING ? "out" : "in ";
	uint16_t type;
	size_t off = 12;
	char t[64], s[INET6_ADDRSTRLEN], d[INET6_ADDRSTRLEN], mac[40];

	if (len < 14)
		return;
	while (off + 2 <= len) {
		type = (uint16_t)(p[off] << 8 | p[off + 1]);
		if (type == 0x8100 || type == 0x88a8) {
			if (off + 4 > len)
				return;
			vlan = ((p[off + 2] & 0x0f) << 8) | p[off + 3];
			pcp = p[off + 2] >> 5;
			off += 4;
			continue;
		}
		off += 2;
		break;
	}
	stamp(t, sizeof(t));
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x>%02x:%02x:%02x:%02x:%02x:%02x",
		 p[6], p[7], p[8], p[9], p[10], p[11], p[0], p[1], p[2], p[3], p[4], p[5]);

	if (type == ETH_P_IP && len >= off + 20) {
		const uint8_t *ip = p + off;
		size_t hl = (size_t)(ip[0] & 0x0f) * 4;
		struct in_addr src, dst;

		memcpy(&src, ip + 12, 4);
		memcpy(&dst, ip + 16, 4);
		if (ip[9] == 2 && len >= off + hl + 8) {
			const uint8_t *g = ip + hl;
			struct in_addr grp;

			memcpy(&grp, g + 4, 4);
			char gs[INET_ADDRSTRLEN];

			inet_ntop(AF_INET, &src, s, sizeof(s));
			inet_ntop(AF_INET, &dst, d, sizeof(d));
			inet_ntop(AF_INET, &grp, gs, sizeof(gs));
			printf("%s %-8s %s vlan=%d pcp=%d mac %s %s > %s IGMP type=0x%02x group=%s\n",
			       t, names[ifi], dir, vlan, pcp, mac, s, d, g[0], gs);
			fflush(stdout);
		} else if ((ntohl(dst.s_addr) >> 28) == 0xe &&
			   (ntohl(dst.s_addr) >> 8) != 0xe00000) {
			count_stream(ifi, vlan, dst.s_addr, len);
		}
	} else if (type == ETH_P_IPV6 && len >= off + 48) {
		const uint8_t *ip = p + off;
		uint8_t next = ip[6];
		size_t o = 40;

		if (next == 0 && len >= off + 48) {	/* hop-by-hop (MLD) */
			next = ip[40];
			o = 40 + ((size_t)ip[41] + 1) * 8;
		}
		if (next == 58 && len > off + o && ip[24] == 0xff &&
		    (ip[o] == 130 || ip[o] == 131 || ip[o] == 132 || ip[o] == 143)) {
			inet_ntop(AF_INET6, ip + 8, s, sizeof(s));
			inet_ntop(AF_INET6, ip + 24, d, sizeof(d));
			printf("%s %-8s %s vlan=%d pcp=%d mac %s %s > %s MLD type=%u\n", t,
			       names[ifi], dir, vlan, pcp, mac, s, d, ip[o]);
			fflush(stdout);
		}
	}
}

int main(int argc, char **argv)
{
	struct pollfd pfd[MAX_IF];
	int n = argc - 2, secs;
	time_t end, last;

	if (argc < 3 || n > MAX_IF) {
		fprintf(stderr, "usage: mcsniff SECONDS IFACE...\n");
		return 2;
	}
	secs = atoi(argv[1]);
	for (int i = 0; i < n; i++) {
		struct sockaddr_ll sll = {
			.sll_family = AF_PACKET,
			.sll_protocol = htons(ETH_P_ALL),
			.sll_ifindex = (int)if_nametoindex(argv[i + 2]),
		};
		int one = 1;

		names[i] = argv[i + 2];
		pfd[i].fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
		pfd[i].events = POLLIN;
		if (pfd[i].fd < 0 || !sll.sll_ifindex ||
		    bind(pfd[i].fd, (struct sockaddr *)&sll, sizeof(sll)) ||
		    setsockopt(pfd[i].fd, SOL_PACKET, PACKET_AUXDATA, &one, sizeof(one))) {
			fprintf(stderr, "mcsniff: %s: %s\n", argv[i + 2], strerror(errno));
			return 1;
		}
	}
	end = time(NULL) + secs;
	last = time(NULL);
	printf("mcsniff: %d s on", secs);
	for (int i = 0; i < n; i++)
		printf(" %s", names[i]);
	printf("\n");
	fflush(stdout);

	while (time(NULL) < end) {
		if (poll(pfd, (nfds_t)n, 200) > 0) {
			for (int i = 0; i < n; i++) {
				uint8_t buf[2048], ctl[128];
				struct sockaddr_ll from;
				struct iovec iov = { buf, sizeof(buf) };
				struct msghdr mh = {
					.msg_name = &from, .msg_namelen = sizeof(from),
					.msg_iov = &iov, .msg_iovlen = 1,
					.msg_control = ctl, .msg_controllen = sizeof(ctl),
				};
				ssize_t r;
				int vlan = 0, pcp = 0;

				if (!(pfd[i].revents & POLLIN))
					continue;
				r = recvmsg(pfd[i].fd, &mh, MSG_DONTWAIT);
				if (r <= 0)
					continue;
				for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c;
				     c = CMSG_NXTHDR(&mh, c)) {
					struct tpacket_auxdata *a;

					if (c->cmsg_level != SOL_PACKET ||
					    c->cmsg_type != PACKET_AUXDATA)
						continue;
					a = (struct tpacket_auxdata *)CMSG_DATA(c);
					if (a->tp_status & TP_STATUS_VLAN_VALID) {
						vlan = a->tp_vlan_tci & 0x0fff;
						pcp = a->tp_vlan_tci >> 13;
					}
				}
				packet(i, buf, (size_t)r, from.sll_pkttype, vlan, pcp);
			}
		}
		if (time(NULL) != last) {
			last = time(NULL);
			flush_streams();
		}
	}
	flush_streams();
	printf("mcsniff: done\n");
	return 0;
}
