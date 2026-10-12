// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The kernel backend (target/hw_kernel.c) against the kernel the host runs:
 * in a network namespace of its own, with dummy interfaces named sfp and pon
 * in place of the switch's ports, the filters it adds are read back with tc
 * of iproute2. It is built with OMCID_TC_FLAGS=0, so that the filters, which
 * on the board go to the switch alone, go into software. The parts that
 * need the PON family of the stick's driver are left to the board.
 *
 * It needs root, to make the namespace, and tc; without them it says that
 * it skipped (make check-kernel).
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hw/hw.h"
#include "hw_kernel.h"
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
	CHECK(!at("sfp", TC_PRIO_VLAN + 1, NULL) && !at("pon", TC_PRIO_VLAN + 1, NULL));
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
	CHECK(!at("sfp", TC_PRIO_FILTER, NULL) && !at("pon", TC_PRIO_FILTER, NULL));
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
	snprintf(cmd, sizeof(cmd),
		 "tc filter add dev sfp ingress pref %u protocol 802.1Q flower vlan_id 99 action drop",
		 first);
	sh(cmd);
	snprintf(cmd, sizeof(cmd),
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
	struct omci_hw_bridge_port port = { .bridge = 1, .port_id = 257, .num_vlan = 2 };
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

/* Whether the kernel has the tbf qdisc. */
static bool tbf_qdisc(void)
{
	if (sh("ip link add tbfprobe type dummy 2>/dev/null"))
		return false;
	const bool ok = sh("tc qdisc add dev tbfprobe root tbf rate 1mbit burst 4k limit 8k 2>/dev/null") == 0;

	sh("ip link del tbfprobe");
	return ok;
}

/* Whether the kernel has the vlan action, which some kernels leave out. */
static bool vlan_action(void)
{
	if (sh("tc actions add action vlan pop index 4242 >/dev/null 2>&1"))
		return false;
	return sh("tc actions del action vlan index 4242 >/dev/null 2>&1") == 0;
}

int main(void)
{
	if (geteuid() != 0 || unshare(CLONE_NEWNET) != 0 ||
	    sh("command -v tc >/dev/null 2>&1") != 0) {
		puts("test_hw_kernel: SKIPPED: it needs root, network namespaces and tc");
		return 0;
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
	port_filters();
	mode_applied();
	mgmt_vlan_clash();
	if (tbf_qdisc())
		uni_rate();
	else
		puts("(this kernel has no tbf qdisc: the UNI's rate is skipped)");

	printf("test_hw_kernel: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
