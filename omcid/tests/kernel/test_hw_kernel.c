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

#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hw/hw.h"
#include "hw_kernel.h"

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
	CHECK(at("sfp", 1000, "vlan_id 410"));
	CHECK(at("sfp", 1000, "modify id 1410"));
	CHECK(at("pon", 1000, "vlan_id 1410"));
	CHECK(at("pon", 1000, "modify id 410"));
	CHECK(at("sfp", 1001, "push id 420"));
	CHECK(at("pon", 1001, "vlan_id 420"));
	CHECK(at("pon", 1001, "pop"));

	puts("a smaller set takes the rules of before away");
	CHECK(omci_hw_kernel_ops.vlan_apply(rules, 1, &rejected) == OMCI_OK);
	CHECK(at("sfp", 1000, "vlan_id 410"));
	CHECK(!at("sfp", 1001, NULL) && !at("pon", 1001, NULL));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", 1000, NULL) && !at("pon", 1000, NULL));

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
		CHECK(at("sfp", 1000, text));
	}
	CHECK(at("sfp", 1000, "modify id 1410"));
	CHECK(at("pon", 1000, "modify id 410"));
	CHECK(!at("pon", 1000, "vlan_prio"));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, &rejected) == OMCI_OK);
	CHECK(!at("sfp", 1000, NULL) && !at("pon", 1000, NULL));
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
	CHECK(at("sfp", 240, "protocol ipv6"));
	CHECK(at("sfp", 240, "action drop"));
	CHECK(at("pon", 240, "dst_mac 01:00:5e:00:00:00/25"));
	CHECK(at("pon", 240, "action pass"));
	CHECK(omci_hw_kernel_ops.filter_apply(NULL, 0) == OMCI_OK);
	CHECK(!at("sfp", 240, NULL) && !at("pon", 240, NULL));
}

static void multicast_tags(void)
{
	const struct omci_hw_mcast_ds rules[] = {
		{ .vid = 840, .action = OMCI_HW_MCAST_DS_STRIP },
	};

	puts("the downstream tag of a multicast VLAN, taken off on pon");
	CHECK(omci_hw_kernel_ops.mcast_ds(rules, 1) == OMCI_OK);
	CHECK(at("pon", 200, "vlan_id 840"));
	CHECK(at("pon", 200, "pop"));
	CHECK(omci_hw_kernel_ops.mcast_ds(NULL, 0) == OMCI_OK);
	CHECK(!at("pon", 200, NULL));
}

/*
 * The drops of the subscriber's VLANs that an earlier run of the daemon left
 * on sfp, at priorities 50 to 65, go with the first set of VLAN rules of the
 * next run, which knows none of them.
 */
static void leftover_drops(void)
{
	puts("the drops at the UNI that a daemon of before left go with the first rules");
	sh("tc qdisc add dev sfp clsact 2>/dev/null");
	sh("tc filter add dev sfp ingress pref 50 protocol 802.1Q flower vlan_id 99 action drop");
	sh("tc filter add dev sfp ingress pref 65 protocol 802.1Q flower vlan_id 98 action drop");
	CHECK(at("sfp", 50, "vlan_id 99") && at("sfp", 65, "vlan_id 98"));
	CHECK(omci_hw_kernel_ops.vlan_apply(NULL, 0, NULL) == OMCI_OK);
	CHECK(!at("sfp", 50, NULL) && !at("sfp", 65, NULL));
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
	if (tbf_qdisc())
		uni_rate();
	else
		puts("(this kernel has no tbf qdisc: the UNI's rate is skipped)");

	printf("test_hw_kernel: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
