// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The kernel backend (target/hw_kernel.c) against the kernel the host runs:
 * in a network namespace of its own, with dummy interfaces named sfp and pon
 * in place of the switch's ports, the filters it adds are read back with tc
 * of iproute2. It is built with OMCID_TC_FLAGS=0, so that the filters, which
 * on the board go to the switch alone, go into software. The encrypted
 * Port-IDs and the announcements of the PON family are read over the PON
 * class's stand-in MAC, pon_sim, when the kernel has it; the other parts that
 * need the PON family of the stick's driver are left to the board.
 *
 * It needs root, to make the namespace, and tc; without them it says that
 * it skipped (make check-kernel).
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hw/hw.h"
#include "hw_kernel.h"
#include "omcid/array_size.h"
#include "tcprio.h"

extern const struct omci_hw_ops omci_hw_kernel_ops;

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

/* What tc shows of the ingress of @dev, all on one line, for strstr(). */
static const char *shown(const char *dev)
{
	static char out[16384];
	char cmd[128];
	size_t len = 0;
	FILE *p;
	int c;

	snprintf(cmd, sizeof(cmd), "tc filter show dev %s ingress", dev);
	p = popen(cmd, "r");
	if (!p)
		return "";
	while ((c = fgetc(p)) != EOF && len + 1 < sizeof(out))
		out[len++] = c == '\n' || c == '\t' ? ' ' : (char)c;
	out[len] = '\0';
	pclose(p);
	return out;
}

/*
 * Whether a filter of priority @pref on @dev holds @text (any filter of it,
 * with @text NULL): tc shows each filter from "filter protocol" to the next.
 */
static bool at(const char *dev, unsigned int pref, const char *text)
{
	char head[48];
	const char *s = shown(dev);

	snprintf(head, sizeof(head), " pref %u ", pref);
	for (const char *f = strstr(s, "filter protocol"); f;) {
		const char *next = strstr(f + 1, "filter protocol");
		size_t len = next ? (size_t)(next - f) : strlen(f);
		const char *h = strstr(f, head);

		if (h && h < f + len) {
			const char *t = text ? strstr(f, text) : f;

			if (t && t < f + len)
				return true;
		}
		f = next;
	}
	return false;
}

/* How many filters of priority @pref on @dev hold @text. */
static unsigned int count_at(const char *dev, unsigned int pref,
			     const char *text)
{
	char head[48];
	const char *s = shown(dev);
	unsigned int n = 0;

	snprintf(head, sizeof(head), " pref %u ", pref);
	for (const char *f = strstr(s, "filter protocol"); f;) {
		const char *next = strstr(f + 1, "filter protocol");
		size_t len = next ? (size_t)(next - f) : strlen(f);
		const char *h = strstr(f, head);
		const char *t = strstr(f, text);

		if (h && h < f + len && t && t < f + len)
			n++;
		f = next;
	}
	return n;
}

static int sh(const char *cmd)
{
	return system(cmd);
}

static void vlan_rules(void)
{
	const struct omci_hw_vlan_xlate rules[] = {
		{ .kind = OMCI_HW_VLAN_CVID_XLATE,
		  .match_cvid = 410,
		  .assign_cvid = 1410,
		  .assign_pri = OMCI_HW_VLAN_PRI_KEEP },
		{ .kind = OMCI_HW_VLAN_CVID_PUSH,
		  .match_cvid = 0,
		  .assign_cvid = 420,
		  .assign_pri = 4 },
	};
	struct omci_hw_reject rejected;

	puts("the VLAN rules: a translation both ways, a push and its pop");
	CHECK(omci_hw_kernel_ops.vlan_apply(rules, 2, &rejected) == OMCI_OK);
	CHECK(rejected.n == 0);
	CHECK(at("sfp", TC_PRIO_VLAN, "vlan_id 410"));
	CHECK(at("sfp", TC_PRIO_VLAN, "modify id 1410"));
	CHECK(at("pon", TC_PRIO_VLAN, "vlan_id 1410"));
	CHECK(at("pon", TC_PRIO_VLAN, "modify id 410"));
	CHECK(at("sfp", TC_PRIO_VLAN + 1, "push id 420"));
	CHECK(at("pon", TC_PRIO_VLAN + 1, "vlan_id 420"));
	CHECK(at("pon", TC_PRIO_VLAN + 1, "pop"));

	puts("a smaller set takes the rules of before away");
	CHECK(omci_hw_kernel_ops.vlan_apply(rules, 1, &rejected) == OMCI_OK);
	CHECK(at("sfp", TC_PRIO_VLAN, "vlan_id 410"));
	CHECK(!at("sfp", TC_PRIO_VLAN + 1, NULL) &&
	      !at("pon", TC_PRIO_VLAN + 1, NULL));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", TC_PRIO_VLAN, NULL) && !at("pon", TC_PRIO_VLAN, NULL));

	const struct omci_hw_vlan_xlate apart = {
		.kind = OMCI_HW_VLAN_CVID_XLATE,
		.match_cvid = 410,
		.assign_cvid = 1410,
		.assign_pri = OMCI_HW_VLAN_PRI_KEEP,
		.per_pri = true,
	};

	puts("a translation that takes each priority apart: eight filters up, one down");
	CHECK(omci_hw_kernel_ops.vlan_apply(&apart, 1, &rejected) == OMCI_OK);
	CHECK(rejected.n == 0);
	for (unsigned int pcp = 0; pcp < 8; pcp++) {
		char text[16];

		snprintf(text, sizeof(text), "vlan_prio %u", pcp);
		CHECK(at("sfp", TC_PRIO_VLAN, text));
	}
	CHECK(at("sfp", TC_PRIO_VLAN, "modify id 1410"));
	CHECK(at("pon", TC_PRIO_VLAN, "modify id 410"));
	CHECK(!at("pon", TC_PRIO_VLAN, "vlan_prio"));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", TC_PRIO_VLAN, NULL) && !at("pon", TC_PRIO_VLAN, NULL));

	const struct omci_hw_vlan_xlate zero = {
		.kind = OMCI_HW_VLAN_CVID_XLATE,
		.match_cvid = 410,
		.assign_cvid = 1410,
		.assign_pri = 0,
	};

	puts("a translation that gives priority 0: a pop and a push up, a modify down");
	CHECK(omci_hw_kernel_ops.vlan_apply(&zero, 1, &rejected) == OMCI_OK);
	CHECK(rejected.n == 0);
	CHECK(at("sfp", TC_PRIO_VLAN, "pop"));
	CHECK(at("sfp", TC_PRIO_VLAN, "push id 1410"));
	CHECK(at("sfp", TC_PRIO_VLAN, "priority 0"));
	CHECK(!at("sfp", TC_PRIO_VLAN, "modify"));
	CHECK(at("pon", TC_PRIO_VLAN, "modify id 410"));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", TC_PRIO_VLAN, NULL) && !at("pon", TC_PRIO_VLAN, NULL));

	const struct omci_hw_vlan_xlate two[] = {
		{ .kind = OMCI_HW_VLAN_SCVID_PUSH,
		  .assign_cvid = 410,
		  .inner_has_pri = true,
		  .inner_pri = 5,
		  .assign_svid = 200,
		  .assign_pri = 3 },
		{ .kind = OMCI_HW_VLAN_SVID_PUSH,
		  .match_cvid = 100,
		  .match_has_pri = true,
		  .match_pri = 6,
		  .assign_svid = 300,
		  .assign_pri = 6,
		  .inner_has_pri = true,
		  .inner_pri = 0 },
		{ .kind = OMCI_HW_VLAN_SVID_PUSH,
		  .match_cvid = 101,
		  .assign_svid = 301,
		  .assign_pri = 1,
		  .inner_has_pri = true,
		  .inner_pri = 4 },
	};

	puts("two tags on untagged frames, both popped; S-tags before C-tags given a priority");
	CHECK(omci_hw_kernel_ops.vlan_apply(two, 3, &rejected) == OMCI_OK);
	CHECK(rejected.n == 0);
	CHECK(at("sfp", TC_PRIO_VLAN, "push id 410"));
	CHECK(at("sfp", TC_PRIO_VLAN, "priority 5"));
	CHECK(at("sfp", TC_PRIO_VLAN, "push id 200"));
	CHECK(at("sfp", TC_PRIO_VLAN, "802.1ad"));
	CHECK(at("pon", TC_PRIO_VLAN, "vlan_id 200"));
	CHECK(at("pon", TC_PRIO_VLAN, "vlan_ethtype"));
	CHECK(at("pon", TC_PRIO_VLAN, "pop"));
	CHECK(at("sfp", TC_PRIO_VLAN + 1, "pop"));
	CHECK(at("sfp", TC_PRIO_VLAN + 1, "push id 100"));
	CHECK(at("sfp", TC_PRIO_VLAN + 1, "push id 300"));
	CHECK(at("pon", TC_PRIO_VLAN + 1, "vlan_id 300"));
	CHECK(at("sfp", TC_PRIO_VLAN + 2, "modify id 101"));
	CHECK(at("sfp", TC_PRIO_VLAN + 2, "priority 4"));
	CHECK(at("sfp", TC_PRIO_VLAN + 2, "push id 301"));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", TC_PRIO_VLAN, NULL) && !at("pon", TC_PRIO_VLAN, NULL));

	const struct omci_hw_vlan_xlate etype[] = {
		{ .kind = OMCI_HW_VLAN_CVID_PUSH,
		  .assign_cvid = 35,
		  .assign_pri = 0,
		  .ethertype = OMCI_HW_ETHERTYPE_PPPOE },
		{ .kind = OMCI_HW_VLAN_CVID_PUSH,
		  .assign_cvid = 36,
		  .assign_pri = 0,
		  .ethertype = OMCI_HW_ETHERTYPE_IPV4 },
		{ .kind = OMCI_HW_VLAN_CVID_XLATE,
		  .match_cvid = 410,
		  .assign_cvid = 1410,
		  .assign_pri = OMCI_HW_VLAN_PRI_KEEP,
		  .ethertype = OMCI_HW_ETHERTYPE_IPV6 },
	};

	puts("rules on an EtherType: a filter for each, PPPoE's two, of untagged frames or behind the tag; undone downstream by the VID");
	CHECK(omci_hw_kernel_ops.vlan_apply(etype, 3, &rejected) == OMCI_OK);
	CHECK(rejected.n == 0);
	CHECK(count_at("sfp", TC_PRIO_VLAN, "push id 35") == 2u);
	CHECK(count_at("sfp", TC_PRIO_VLAN, "num_of_vlans 0") == 2u);
	CHECK(at("sfp", TC_PRIO_VLAN, "ppp_disc") ||
	      at("sfp", TC_PRIO_VLAN, "8863"));
	CHECK(count_at("pon", TC_PRIO_VLAN, "pop") == 1u);
	CHECK(at("pon", TC_PRIO_VLAN, "vlan_id 35"));
	CHECK(at("sfp", TC_PRIO_VLAN + 1, "eth_type ip"));
	CHECK(at("sfp", TC_PRIO_VLAN + 1, "push id 36"));
	CHECK(at("sfp", TC_PRIO_VLAN + 2, "vlan_id 410"));
	CHECK(at("sfp", TC_PRIO_VLAN + 2, "vlan_ethtype ipv6"));
	CHECK(at("pon", TC_PRIO_VLAN + 2, "modify id 410"));
	CHECK(!at("pon", TC_PRIO_VLAN + 2, "vlan_ethtype"));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", TC_PRIO_VLAN, NULL) && !at("pon", TC_PRIO_VLAN, NULL));
}

/*
 * The discards of VLAN rules: drops on sfp at the discards' priorities, one
 * a rule, of any priority, of one, or of each priority but those that
 * earlier rules take.
 */
static void vlan_discards(void)
{
	const struct omci_hw_vlan_xlate rules[] = {
		{ .kind = OMCI_HW_VLAN_DISCARD_VID, .match_cvid = 430 },
		{ .kind = OMCI_HW_VLAN_DISCARD_VID,
		  .match_cvid = 440,
		  .keep_pri = 1u << 5 },
		{ .kind = OMCI_HW_VLAN_DISCARD_VID,
		  .match_cvid = 450,
		  .match_has_pri = true,
		  .match_pri = 3 },
	};
	const unsigned int first = TC_PRIO_VLAN_DISCARD;
	struct omci_hw_reject rejected;

	puts("the discards of VLAN rules: drops on sfp, ahead of the other rules");
	CHECK(omci_hw_kernel_ops.vlan_apply(rules, 3, &rejected) == OMCI_OK);
	CHECK(rejected.n == 0);
	CHECK(at("sfp", first, "vlan_id 430"));
	CHECK(at("sfp", first, "drop"));
	CHECK(!at("sfp", first, "vlan_prio"));
	CHECK(at("sfp", first + 1, "vlan_id 440"));
	for (unsigned int pcp = 0; pcp < 8; pcp++) {
		char text[16];

		snprintf(text, sizeof(text), "vlan_prio %u", pcp);
		CHECK(at("sfp", first + 1, text) == (pcp != 5));
		CHECK(at("sfp", first + 2, text) == (pcp == 3));
	}
	CHECK(!at("pon", first, NULL) && !at("sfp", TC_PRIO_VLAN, NULL));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", first, NULL) && !at("sfp", first + 1, NULL) &&
	      !at("sfp", first + 2, NULL));
}

static void port_filters(void)
{
	const struct omci_hw_filter rules[] = {
		{ .upstream = true, .pass = false, .ethertype = 0x86dd },
		{ .upstream = false,
		  .pass = true,
		  .dmac = { 0x01, 0x00, 0x5e, 0, 0, 0 },
		  .dmask = { 0xff, 0xff, 0xff, 0x80, 0, 0 } },
	};

	puts("the bridge port filters: upstream on sfp, downstream on pon, in order");
	CHECK(omci_hw_kernel_ops.filter_apply(rules, 2) == OMCI_OK);
	CHECK(at("sfp", TC_PRIO_FILTER, "protocol ipv6"));
	CHECK(at("sfp", TC_PRIO_FILTER, "action drop"));
	CHECK(at("pon", TC_PRIO_FILTER, "dst_mac 01:00:5e:00:00:00/25"));
	CHECK(at("pon", TC_PRIO_FILTER, "action pass"));
	CHECK(omci_hw_kernel_ops.filter_apply(NULL, 0) == OMCI_OK);
	CHECK(!at("sfp", TC_PRIO_FILTER, NULL) &&
	      !at("pon", TC_PRIO_FILTER, NULL));
}

static void multicast_tags(void)
{
	const struct omci_hw_mcast_ds rules[] = {
		{ .vid = 840, .action = OMCI_HW_MCAST_DS_STRIP },
	};

	puts("the downstream tag of a multicast VLAN, taken off on pon");
	CHECK(omci_hw_kernel_ops.mcast_ds(rules, 1) == OMCI_OK);
	CHECK(at("pon", TC_PRIO_DS, "vlan_id 840"));
	CHECK(at("pon", TC_PRIO_DS, "pop"));
	CHECK(omci_hw_kernel_ops.mcast_ds(NULL, 0) == OMCI_OK);
	CHECK(!at("pon", TC_PRIO_DS, NULL));
}

/*
 * The drops of the subscriber's VLANs that an earlier run of the daemon left
 * on sfp, at the priorities of the drops, go with the first set of VLAN rules
 * of the next run, which knows none of them.
 */
static void leftover_drops(void)
{
	const unsigned int first = TC_PRIO_DISCARD;
	const unsigned int last = TC_PRIO_DISCARD + TC_PRIO_DISCARD_N - 1;
	char cmd[160];

	puts("the drops at the UNI that a daemon of before left go with the first rules");
	sh("tc qdisc add dev sfp clsact 2>/dev/null");
	snprintf(
		cmd, sizeof(cmd),
		"tc filter add dev sfp ingress pref %u protocol 802.1Q flower vlan_id 99 action drop",
		first);
	sh(cmd);
	snprintf(
		cmd, sizeof(cmd),
		"tc filter add dev sfp ingress pref %u protocol 802.1Q flower vlan_id 98 action drop",
		last);
	sh(cmd);
	CHECK(at("sfp", first, "vlan_id 99") && at("sfp", last, "vlan_id 98"));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, NULL) == OMCI_OK);
	CHECK(!at("sfp", first, NULL) && !at("sfp", last, NULL));
}

/* The first line of the file @path, or "" without one. */
static const char *first_line(const char *path)
{
	static char line[32];
	FILE *f = fopen(path, "r");

	line[0] = '\0';
	if (f && fgets(line, sizeof(line), f))
		line[strcspn(line, "\n")] = '\0';
	if (f)
		fclose(f);
	return line;
}

static void mode_applied(void)
{
	puts("the mode the data path was programmed for is told in /run/omcid/applied");
	mkdir("/run/omcid", 0755);
	unlink("/run/omcid/applied");
	omci_hw_kernel_ops.mode_done(true);
	CHECK(!strcmp(first_line("/run/omcid/applied"), "router"));
	omci_hw_kernel_ops.mode_done(false);
	CHECK(!strcmp(first_line("/run/omcid/applied"), "bridge"));
	CHECK(access("/run/omcid/applied.new", F_OK) != 0);
}

/* The file @path whole, up to a few KiB, or "" without it. */
static const char *whole(const char *path)
{
	static char text[2048];
	FILE *f = fopen(path, "r");
	size_t n = 0;

	if (f) {
		n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
	}
	text[n] = '\0';
	return text;
}

static long long monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void time_of_day(void)
{
	unsigned long long utc = 0;
	long long at = -1, before, after;

	puts("the OLT's date and time are told in /run/omcid/clock, with the monotonic time");
	mkdir("/run/omcid", 0755);
	unlink("/run/omcid/clock");
	before = monotonic_ns();
	omci_hw_kernel_ops.time_of_day(1786797045u);
	after = monotonic_ns();
	CHECK(sscanf(whole("/run/omcid/clock"), "utc=%llu\nmonotonic_ns=%lld\n",
		     &utc, &at) == 2);
	CHECK(utc == 1786797045u);
	CHECK(at >= before && at <= after);
	CHECK(access("/run/omcid/clock.new", F_OK) != 0);
	unlink("/run/omcid/clock");
}

/* How many times @needle is in @hay. */
static int times(const char *hay, const char *needle)
{
	int n = 0;

	for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle))
		n++;
	return n;
}

/*
 * Apply a bridge whose one port carries @vid and 410, with the daemon's log
 * (standard error) in the file @log, and return how often the warning about
 * the management VLAN is in it afterwards.
 */
static int clash_warnings(const char *log, uint16_t vid)
{
	struct omci_hw_bridge_port port = { .bridge = 1,
					    .port_id = 257,
					    .num_vlan = 2 };
	int saved, fd;

	port.vlan[0] = vid;
	port.vlan[1] = 410;
	fflush(stderr);
	fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
	saved = dup(STDERR_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);
	(void)omci_hw_kernel_ops.bridge_apply(&port, 1, NULL);
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	return times(whole(log), "the stick's management VLAN");
}

static void mgmt_vlan_clash(void)
{
	const char *log = "/tmp/hw-kernel-test.log";
	FILE *f;

	puts("a VLAN of the OLT that is the stick's management VLAN is said once, and again when it returns");
	mkdir("/run/omcid", 0755);
	f = fopen("/run/onu-mgmt", "w");
	CHECK(f != NULL);
	if (f) {
		fputs("VLAN=4094\nADDRESS=192.0.2.1/30\n", f);
		fclose(f);
	}
	unlink(log);
	CHECK(clash_warnings(log, 4093) == 0);
	CHECK(!strstr(whole("/run/omcid/gem-vlans"), "4094 "));
	CHECK(clash_warnings(log, 4094) == 1);
	CHECK(strstr(whole("/run/omcid/gem-vlans"), "4094 257\n") != NULL);
	CHECK(clash_warnings(log, 4094) == 1);
	CHECK(clash_warnings(log, 4093) == 1);
	CHECK(clash_warnings(log, 4094) == 2);
	unlink("/run/onu-mgmt");
	unlink(log);
	CHECK(omci_hw_kernel_ops.bridge_apply(NULL, 0, NULL) == OMCI_OK);
}

/* What tc shows of the root qdisc of @dev, on one line. */
static const char *root_qdisc(const char *dev)
{
	static char out[1024];
	char cmd[128];
	size_t len = 0;
	FILE *p;
	int c;

	snprintf(cmd, sizeof(cmd), "tc qdisc show dev %s root", dev);
	p = popen(cmd, "r");
	if (!p)
		return "";
	while ((c = fgetc(p)) != EOF && len + 1 < sizeof(out))
		out[len++] = c == '\n' ? ' ' : (char)c;
	out[len] = '\0';
	pclose(p);
	return out;
}

static void uni_rate(void)
{
	puts("the UNI's rate: a tbf at the root of sfp, replaced, then taken away");
	CHECK(omci_hw_kernel_ops.uni_rate(1250000) == OMCI_OK);
	CHECK(strstr(root_qdisc("sfp"), "tbf b70: root") != NULL);
	CHECK(strstr(root_qdisc("sfp"), "rate 10Mbit") != NULL);
	CHECK(omci_hw_kernel_ops.uni_rate(2500000) == OMCI_OK);
	CHECK(strstr(root_qdisc("sfp"), "rate 20Mbit") != NULL);
	CHECK(omci_hw_kernel_ops.uni_rate(0) == OMCI_OK);
	CHECK(strstr(root_qdisc("sfp"), "tbf") == NULL);
	CHECK(omci_hw_kernel_ops.uni_rate(0) == OMCI_OK);
}

/* A packet socket bound to @dev, to send on it or to read what it sends. */
static int packet_socket(const char *dev)
{
	struct sockaddr_ll sll = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex = (int)if_nametoindex(dev),
	};
	const int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);

	if (fd < 0 || sll.sll_ifindex == 0 ||
	    bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

/* An IGMPv2 general query as the OLT sends it, untagged. */
static size_t igmp_query(uint8_t *f)
{
	static const uint8_t head[] = {
		0x01, 0x00, 0x5e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x5e,
		0x00, 0x53, 0x99, 0x08, 0x00, 0x45, 0xc0, 0x00, 0x1c,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x0a,
		0x00, 0x00, 0x01, 0xe0, 0x00, 0x00, 0x01,
	};

	memset(f, 0, 64);
	memcpy(f, head, sizeof(head));
	f[sizeof(head)] = 0x11u;
	f[sizeof(head) + 1] = 100u;
	return 60;
}

/* Send @len bytes on @dev's peer, which arrive on @dev. */
static void arrive(const char *peer, const uint8_t *f, size_t len)
{
	const int fd = packet_socket(peer);

	CHECK(fd >= 0 && send(fd, f, len, 0) == (ssize_t)len);
	if (fd >= 0)
		close(fd);
}

/* Wait a moment for what was sent to be queued on the receive side. */
static void settle(void)
{
	usleep(100000);
}

/*
 * The messages of IGMP and MLD: from sfp the subscriber's, from a GEM
 * interface the OLT's, from none other; the Ethernet address of sfp for the
 * agent's own; and a message sent to the subscriber side or to a GEM port.
 */
static void multicast_frames(void)
{
	static const char *const links[] = { "sfp",   "sfpx",	  "gem257",
					     "gemx",  "gem257.5", "gemv",
					     "other", "otherx" };
	struct omci_hw_mcast_frame f;
	uint8_t frame[64], mac[6], back[128];
	unsigned int sfp_mac[6] = { 0 }, from_pon = 0, from_uni = 0;
	FILE *fp;
	int fd;

	puts("the IGMP and MLD messages: the side by the interface, the Ethernet address, a message sent");
	sh("ip link del sfp");
	sh("ip link add sfp type veth peer name sfpx");
	sh("ip link add gem257 type veth peer name gemx");
	sh("ip link add gem257.5 type veth peer name gemv");
	sh("ip link add other type veth peer name otherx");
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		char cmd[64];

		snprintf(cmd, sizeof(cmd), "ip link set %s up", links[i]);
		sh(cmd);
	}
	CHECK(omci_hw_kernel_ops.mcast_fd() >= 0);
	/* The new interfaces say their own MLD; none waits after it is read. */
	settle();
	while (omci_hw_kernel_ops.mcast_frame(&f) == OMCI_OK)
		;
	CHECK(omci_hw_kernel_ops.mcast_frame(&f) == OMCI_DECLINED);

	(void)igmp_query(frame);
	arrive("sfpx", frame, 60);
	arrive("gemx", frame, 60);
	arrive("gemv", frame, 60);
	arrive("otherx", frame, 60);
	settle();
	/*
	 * Of the four, one from sfp and one from the GEM interface are read: not
	 * those of a VLAN interface over a GEM interface and of any other. The
	 * kernel's own MLD of the new interfaces is no query, and is not 60 bytes.
	 */
	while (omci_hw_kernel_ops.mcast_frame(&f) == OMCI_OK) {
		if (f.len != 60)
			continue;
		CHECK(f.data[14 + 20] == 0x11u);
		if (f.from_pon)
			from_pon++;
		else
			from_uni++;
	}
	CHECK(from_uni == 1 && from_pon == 1);

	/* The Ethernet address of sfp (the namespace has no /sys of its own, so ip tells it). */
	fp = popen("ip -o link show sfp", "r");
	CHECK(fp != NULL);
	if (fp) {
		char line[512];
		const char *eth = NULL;

		while (fgets(line, sizeof(line), fp) && !eth)
			eth = strstr(line, "link/ether ");
		CHECK(eth != NULL);
		CHECK(eth && sscanf(eth + 11, "%x:%x:%x:%x:%x:%x", &sfp_mac[0],
				    &sfp_mac[1], &sfp_mac[2], &sfp_mac[3],
				    &sfp_mac[4], &sfp_mac[5]) == 6);
		pclose(fp);
	}
	CHECK(omci_hw_kernel_ops.mcast_mac(mac) == OMCI_OK);
	for (unsigned int i = 0; i < 6; i++)
		CHECK(mac[i] == sfp_mac[i]);

	/* A message to the subscriber side goes out of sfp, one to a GEM port out of its interface. */
	fd = packet_socket("sfpx");
	CHECK(fd >= 0);
	CHECK(omci_hw_kernel_ops.mcast_send(OMCI_HW_MCAST_UNI, frame, 60) ==
	      OMCI_OK);
	settle();
	CHECK(fd >= 0 && recv(fd, back, sizeof(back), MSG_DONTWAIT) == 60);
	CHECK(memcmp(back, frame, 60) == 0);
	if (fd >= 0)
		close(fd);
	fd = packet_socket("gemx");
	CHECK(fd >= 0);
	CHECK(omci_hw_kernel_ops.mcast_send(257, frame, 60) == OMCI_OK);
	settle();
	CHECK(fd >= 0 && recv(fd, back, sizeof(back), MSG_DONTWAIT) == 60);
	if (fd >= 0)
		close(fd);
	/* No such GEM interface, and a frame too short for an Ethernet header. */
	CHECK(omci_hw_kernel_ops.mcast_send(3, frame, 60) == OMCI_ERR_HW);
	CHECK(omci_hw_kernel_ops.mcast_send(OMCI_HW_MCAST_UNI, frame, 8) ==
	      OMCI_ERR_HW);
}

/* Whether the kernel has the tbf qdisc. */
static bool tbf_qdisc(void)
{
	const char *add =
		"tc qdisc add dev tp root tbf rate 1mbit burst 4k limit 8k 2>&-";
	bool ok;

	if (sh("ip link add tp type dummy 2>/dev/null"))
		return false;
	ok = sh(add) == 0;
	sh("ip link del tp");
	return ok;
}

/* Whether the kernel has the vlan action, which some kernels leave out. */
static bool vlan_action(void)
{
	if (sh("tc actions add action vlan pop index 4242 >/dev/null 2>&1"))
		return false;
	return sh("tc actions del action vlan index 4242 >/dev/null 2>&1") == 0;
}

/*
 * A group removed when it is gone already, as the bridge's own leave takes the
 * snooped entry before the agent removes it: declined, which the agent passes
 * over, rather than refused with a warning.
 */
static void mcast_group_gone(void)
{
	struct omci_hw_mcast_group g = { .permanent = true };
	const uint8_t addr[4] = { 233, 252, 0, 1 };

	puts("a group removed when it is gone already is declined, not refused");
	/* A bridge of its own, in place of what the earlier cases left. */
	sh("ip link del omcibr0 2>/dev/null");
	if (sh("ip link add omcibr0 type bridge") != 0 ||
	    sh("ip link set omcibr0 up") != 0 ||
	    sh("ip link set sfp master omcibr0") != 0) {
		puts("(no bridge: skipped)");
		return;
	}
	g.group[10] = 0xff;
	g.group[11] = 0xff;
	memcpy(&g.group[12], addr, sizeof(addr));
	CHECK(omci_hw_kernel_ops.mcast_group_add(&g) == OMCI_OK);
	CHECK(omci_hw_kernel_ops.mcast_group_del(&g) == OMCI_OK);
	CHECK(omci_hw_kernel_ops.mcast_group_del(&g) == OMCI_DECLINED);
	sh("ip link set sfp nomaster; ip link del omcibr0");
}

#define PON_SIM "/sys/kernel/debug/pon_sim/"

/* Whether Port-ID @port is marked in @e. */
static bool marked(const struct omci_hw_encrypted *e, unsigned int port)
{
	return (e->map[port / 32u] >> (port % 32u) & 1u) != 0u;
}

/*
 * The Port-IDs the OLT encrypts, from the family "pon", and the
 * announcements of their change on its group "monitor", over the stand-in
 * MAC pon_sim (CONFIG_PON_SIM), whose debugfs file "encrypt" marks a
 * Port-ID as Encrypted_Port-ID does. In the initial network namespace,
 * where the kernel announces.
 */
static void pon_encryption(void)
{
	struct omci_hw_encrypted e;
	struct pollfd pfd = { .events = POLLIN };

	puts("the Port-IDs the OLT encrypts, read and followed on the PON family");
	if (access(PON_SIM "encrypt", W_OK) != 0) {
		puts("(no stand-in PON MAC: skipped)");
		return;
	}
	CHECK(sh("echo 5 > " PON_SIM "state") == 0);
	pfd.fd = omci_hw_kernel_ops.pon_fd();
	CHECK(pfd.fd >= 0);
	CHECK(omci_hw_kernel_ops.pon_fd() == pfd.fd);
	omci_hw_kernel_ops.pon_drain();
	CHECK(omci_hw_kernel_ops.encrypted(&e) == OMCI_OK);
	CHECK(!marked(&e, 300) && !marked(&e, 4095));

	/* A mark is announced, and read with the OMCC's. */
	CHECK(sh("echo 300 1 > " PON_SIM "encrypt") == 0);
	CHECK(sh("echo 4095 1 > " PON_SIM "encrypt") == 0);
	CHECK(poll(&pfd, 1, 2000) == 1);
	omci_hw_kernel_ops.pon_drain();
	CHECK(poll(&pfd, 1, 0) == 0);
	CHECK(omci_hw_kernel_ops.encrypted(&e) == OMCI_OK);
	CHECK(marked(&e, 300) && marked(&e, 4095) && !marked(&e, 301));

	/* An unmark, and the marks gone with the ONU-ID. */
	CHECK(sh("echo 300 0 > " PON_SIM "encrypt") == 0);
	CHECK(poll(&pfd, 1, 2000) == 1);
	omci_hw_kernel_ops.pon_drain();
	CHECK(omci_hw_kernel_ops.encrypted(&e) == OMCI_OK);
	CHECK(!marked(&e, 300) && marked(&e, 4095));
	CHECK(sh("echo 3 > " PON_SIM "state") == 0);
	CHECK(omci_hw_kernel_ops.encrypted(&e) == OMCI_OK);
	CHECK(!marked(&e, 4095));
	CHECK(sh("echo 5 > " PON_SIM "state") == 0);
	omci_hw_kernel_ops.pon_drain();
}

int main(void)
{
	if (geteuid() != 0) {
		puts("test_hw_kernel: SKIPPED: it needs root, network namespaces and tc");
		return 0;
	}
	/* Before the namespace of its own, as the PON family announces outside it. */
	pon_encryption();

	if (unshare(CLONE_NEWNET) != 0 ||
	    sh("command -v tc >/dev/null 2>&1") != 0) {
		printf("test_hw_kernel: %d checks, %d failed; the rest SKIPPED: it needs network namespaces and tc\n",
		       checks, failures);
		return failures ? 1 : 0;
	}
	if (sh("ip link add sfp type dummy && ip link set sfp up") != 0 ||
	    sh("ip link add pon type dummy && ip link set pon up") != 0) {
		puts("test_hw_kernel: no dummy interfaces");
		return 1;
	}

	/* First, as the drops go with the run's first set of VLAN rules. */
	leftover_drops();
	if (vlan_action()) {
		vlan_rules();
		multicast_tags();
	} else {
		puts("(this kernel has no vlan action: the VLAN rules and the multicast tags are skipped)");
	}
	vlan_discards();
	port_filters();
	mode_applied();
	time_of_day();
	mgmt_vlan_clash();
	if (tbf_qdisc())
		uni_rate();
	else
		puts("(this kernel has no tbf qdisc: the UNI's rate is skipped)");
	multicast_frames();
	mcast_group_gone();

	printf("test_hw_kernel: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
