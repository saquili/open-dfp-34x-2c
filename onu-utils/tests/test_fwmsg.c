// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the stick's own table of nf_tables, as a batch (fwmsg.h). */

#include <stdio.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>

#include "fwmsg.h"

static int failures;
static int checks;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static char g_buf[2 * 32768];
static char g_err[128];

/* The requests of the batch of @len bytes, by type, one a letter, in @out. */
static void kinds(size_t len, char *out, size_t cap, uint32_t *ack_last)
{
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)g_buf;
	int left = (int)len;
	size_t n = 0;

	*ack_last = 0;
	for (; mnl_nlmsg_ok(nlh, left) && n + 1 < cap; nlh = mnl_nlmsg_next(nlh, &left)) {
		char k = '?';

		switch (nlh->nlmsg_type) {
		case NFNL_MSG_BATCH_BEGIN:
			k = '<';
			break;
		case NFNL_MSG_BATCH_END:
			k = '>';
			break;
		case NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_DESTROYTABLE:
			k = 'D';
			break;
		case NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWTABLE:
			k = 'T';
			break;
		case NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWFLOWTABLE:
			k = 'F';
			break;
		case NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWCHAIN:
			k = 'C';
			break;
		case NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWRULE:
			k = 'R';
			break;
		}
		if (nlh->nlmsg_flags & NLM_F_ACK)
			*ack_last = nlh->nlmsg_seq;
		out[n++] = k;
	}
	out[n] = '\0';
}

/* Whether @what, an expression's name, is in the batch of @len bytes. */
static bool has_bytes(size_t len, const char *what)
{
	return memmem(g_buf, len, what, strlen(what) + 1) != NULL;
}

static void test_lines(void)
{
	static const char *const bad[] = {
		"lan net1",
		"lan net1 192.168.1.1/24",
		"lan net1 192.168.1.0/7",
		"lan net1 192.168.1.0/33",
		"lan net1 192.168.1.0/x",
		"lan net1 300.168.1.0/24",
		"lan bad/name 192.168.1.0/24",
		"lan averyverylongname 192.168.1.0/24",
		"wan",
		"wan gem 257",
		"offload maybe",
		"route 0.0.0.0/0",
	};
	struct fw_spec s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		fw_spec_init(&s);
		CHECK(fw_spec_line(&s, bad[i], g_err, sizeof(g_err)) == -1 && *g_err);
	}
	fw_spec_init(&s);
	CHECK(s.offload);
	CHECK(fw_spec_line(&s, "", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "# the networks", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "lan sfp 10.0.0.0/8", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "wan gem257", g_err, sizeof(g_err)) == 0);
	/* Two sessions on one GEM interface. */
	CHECK(fw_spec_line(&s, "wan gem257", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "offload off", g_err, sizeof(g_err)) == 0);
	CHECK(s.nlans == 2 && s.nwans == 1 && !s.offload);
	CHECK(!strcmp(s.lan[0].ifname, "net1") && s.lan[0].net == 0xc0a80100u &&
	      s.lan[0].len == 24);
	CHECK(s.lan[1].net == 0x0a000000u && s.lan[1].len == 8);

	/* No more networks than the stick routes. */
	fw_spec_init(&s);
	for (int i = 0; i < FW_LANS_MAX; i++)
		CHECK(fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)) == -1);
}

static void test_batch(void)
{
	char k[64];
	struct fw_spec s;
	uint32_t last, acked;
	size_t len;

	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "lan net2 10.1.0.0/16", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "wan gem257", g_err, sizeof(g_err)));

	/*
	 * The table there is goes, then the table, the flowtable, the two
	 * chains, and the rules: two flows of each network, the answers, the
	 * protection and the NAT of each.
	 */
	len = fw_batch(&s, g_buf, sizeof(g_buf), 100, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCRRRRRRRRR>"));
	CHECK(last == acked && last == 100 + 14);
	CHECK(has_bytes(len, "flow_offload") && has_bytes(len, "masq") &&
	      has_bytes(len, "gem257") && has_bytes(len, "sfp") && has_bytes(len, "net2"));

	/* Without the flowtable, no flow goes to it. */
	s.offload = false;
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTCCRRRRR>") && last == acked);
	CHECK(!has_bytes(len, "flow_offload"));

	/* A table without networks keeps nothing out and translates nothing. */
	fw_spec_init(&s);
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCR>"));

	/* A batch that does not fit is none. */
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)));
	CHECK(fw_batch(&s, g_buf, 512, 1, &last) == 0);

	len = fw_batch_clear(g_buf, sizeof(g_buf), 7, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<D>") && last == 8 && acked == 8);
}

int main(void)
{
	test_lines();
	test_batch();
	printf("test_fwmsg: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
