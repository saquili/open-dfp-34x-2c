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

static char g_buf[2 * FW_BATCH_MAX];
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
		case NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWSET:
			k = 'S';
			break;
		case NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWSETELEM:
			k = 'E';
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

/*
 * Whether the batch of @len bytes maps the port @key to @to: an element of
 * a map, the key's value, its padding, and the value's two headers (of its
 * nest and of the data) before it.
 */
static bool element(size_t len, uint16_t key, uint16_t to)
{
	const unsigned char k[2] = { key >> 8, key & 0xff }, t[2] = { to >> 8, to & 0xff };
	const char *p = g_buf;

	while ((p = memmem(p, len - (size_t)(p - g_buf), k, sizeof(k)))) {
		if ((size_t)(p - g_buf) + 14 <= len && !memcmp(p + 12, t, sizeof(t)))
			return true;
		p++;
	}
	return false;
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
		"out",
		"out *",
		"out ppp**",
		"out wan421 x",
		"out averyverylongname",
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
	/* The sessions' interfaces by the beginning of their names, and one whole. */
	CHECK(fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "out wan421", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "out wan421", g_err, sizeof(g_err)) == 0);
	CHECK(s.nouts == 2 && !strcmp(s.out[0].ifname, "ppp") && s.out[0].prefix &&
	      !strcmp(s.out[1].ifname, "wan421") && !s.out[1].prefix);
	CHECK(s.nlans == 2 && s.nwans == 1 && !s.offload);
	CHECK(!strcmp(s.lan[0].ifname, "net1") && s.lan[0].net == 0xc0a80100u &&
	      s.lan[0].len == 24);
	CHECK(s.lan[1].net == 0x0a000000u && s.lan[1].len == 8);

	/* No more networks than the stick routes. */
	fw_spec_init(&s);
	for (int i = 0; i < FW_LANS_MAX; i++)
		CHECK(fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)) == 0);
	CHECK(fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)) == -1);
	/* Nor more ways out than its connections. */
	fw_spec_init(&s);
	for (int i = 0; i < FW_OUTS_MAX; i++) {
		char line[32];

		snprintf(line, sizeof(line), "out wan%d", 100 + i);
		CHECK(fw_spec_line(&s, line, g_err, sizeof(g_err)) == 0);
	}
	CHECK(fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)) == -1);
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
	CHECK(!fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)));

	/*
	 * The table there is goes, then the table, the flowtable, the two
	 * chains, and the rules: the cut of the segment size, two flows of
	 * each network each way, the answers, the protection and the NAT of
	 * each.
	 */
	len = fw_batch(&s, g_buf, sizeof(g_buf), 100, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCRRRRRRRRRRRRRR>"));
	CHECK(last == acked && last == 100 + 19);
	CHECK(has_bytes(len, "flow_offload") && has_bytes(len, "masq") &&
	      has_bytes(len, "gem257") && has_bytes(len, "sfp") && has_bytes(len, "net2"));
	CHECK(has_bytes(len, "rt") && has_bytes(len, "byteorder") && has_bytes(len, "exthdr"));

	/* Without the flowtable, no flow goes to it. */
	s.offload = false;
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTCCRRRRRR>") && last == acked);
	CHECK(!has_bytes(len, "flow_offload"));

	/*
	 * A second way out, an IPoE connection's interface: its own cut of the
	 * segment size, and the protection and the NAT of each network by it.
	 */
	s.offload = true;
	CHECK(!fw_spec_line(&s, "out wan421", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "wan gem300", g_err, sizeof(g_err)));
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCRRRRRRRRRRRRRRRRRRR>") && last == acked);
	CHECK(has_bytes(len, "wan421") && has_bytes(len, "gem300"));

	/* A table without networks keeps nothing out and translates nothing. */
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)));
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCRR>"));
	/* Nor one without a way out. */
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)));
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCRRRRR>"));
	CHECK(!has_bytes(len, "masq"));

	/* The most there may be fits a batch: every network by every way out. */
	fw_spec_init(&s);
	for (int i = 0; i < FW_LANS_MAX; i++) {
		char line[48];

		snprintf(line, sizeof(line), "lan net%d 192.168.%d.0/24", i + 1, i + 1);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
		snprintf(line, sizeof(line), "wan gem%d", 256 + i);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	CHECK(!fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)));
	for (int i = 1; i < FW_OUTS_MAX; i++) {
		char line[48];

		snprintf(line, sizeof(line), "out wan%d", 3000 + i);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	CHECK(len > 0 && last == 1 + 5 + 7 + 4 * 8 + 1 + 2 * 8 * 7);

	/* A batch that does not fit is none. */
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)));
	CHECK(fw_batch(&s, g_buf, 512, 1, &last) == 0);

	len = fw_batch_clear(g_buf, sizeof(g_buf), 7, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<D>") && last == 8 && acked == 8);
}

static void test_forward_lines(void)
{
	static const char *const bad[] = {
		"fwd ppp0 203.0.113.24 tcp 443 192.168.1.20",
		"fwd ppp0 203.0.113.24 tcp 443 192.168.1.20 443 x",
		"fwd ppp0 203.0.113.24 icmp 443 192.168.1.20 443",
		"fwd ppp0 203.0.113.24 both 443 192.168.1.20 443",
		"fwd ppp0 203.0.113 tcp 443 192.168.1.20 443",
		"fwd ppp0 203.0.113.24 tcp 443 192.168.1.300 443",
		"fwd bad/name 203.0.113.24 tcp 443 192.168.1.20 443",
		"fwd ppp0 203.0.113.24 tcp 0 192.168.1.20 443",
		"fwd ppp0 203.0.113.24 tcp 0443 192.168.1.20 443",
		"fwd ppp0 203.0.113.24 tcp 65536 192.168.1.20 443",
		"fwd ppp0 203.0.113.24 tcp 443 192.168.1.20 65536",
		"fwd ppp0 203.0.113.24 tcp 443x 192.168.1.20 443",
		"fwd ppp0 203.0.113.24 tcp 9000-8000 192.168.1.20 9000",
		"fwd ppp0 203.0.113.24 tcp 8000- 192.168.1.20 8000",
		/* A range moved past the last port. */
		"fwd ppp0 203.0.113.24 tcp 8000-8010 192.168.1.20 65530",
		"dmz ppp0 203.0.113.24",
		"dmz ppp0 203.0.113.24 192.168.1.30 x",
		"dmz ppp0 203.0.113.24 192.168.1.300",
		"dmz averyverylongname 203.0.113.24 192.168.1.30",
	};
	struct fw_spec s;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		fw_spec_init(&s);
		CHECK(fw_spec_line(&s, bad[i], g_err, sizeof(g_err)) == -1 && *g_err);
		CHECK(s.nfwds == 0 && s.ndmzs == 0);
	}
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 8080 192.168.1.20 80", g_err,
			    sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "fwd wan420 192.0.2.10 udp 8000-8010 192.168.2.5 8000", g_err,
			    sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "dmz wan420 192.0.2.10 192.168.2.7", g_err, sizeof(g_err)));
	CHECK(s.nfwds == 2 && s.ndmzs == 1);
	CHECK(!strcmp(s.fwd[0].ifname, "ppp0") && s.fwd[0].addr == 0xcb007118u &&
	      s.fwd[0].proto == 6 && s.fwd[0].from == 8080 && s.fwd[0].last == 8080 &&
	      s.fwd[0].dev == 0xc0a80114u && s.fwd[0].port == 80);
	CHECK(s.fwd[1].proto == 17 && s.fwd[1].from == 8000 && s.fwd[1].last == 8010 &&
	      s.fwd[1].port == 8000);
	CHECK(!strcmp(s.dmz[0].ifname, "wan420") && s.dmz[0].addr == 0xc000020au &&
	      s.dmz[0].dev == 0xc0a80207u);
	CHECK(!fw_fwd_moved(&s.fwd[0]) && !fw_fwd_moved(&s.fwd[1]) && s.nmoved == 0);
	/* A network line takes no word more than it has. */
	CHECK(fw_spec_line(&s, "lan net1 192.168.1.0/24 x", g_err, sizeof(g_err)) == -1);

	/* A range moved to other ports, up to the last port there is. */
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 8000-8010 192.168.1.20 9000", g_err,
			    sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 udp 8000-8010 192.168.1.20 65525", g_err,
			    sizeof(g_err)));
	CHECK(s.nfwds == 4 && fw_fwd_moved(&s.fwd[2]) && s.fwd[2].from == 8000 &&
	      s.fwd[2].last == 8010 && s.fwd[2].port == 9000 && s.fwd[3].port == 65525);
	CHECK(s.nmoved == 2 * 11);

	/* No more ports moved than a table holds, of every forward and protocol together. */
	fw_spec_init(&s);
	for (int i = 0; i < FW_MOVED_MAX / 64; i++) {
		char line[96];

		snprintf(line, sizeof(line), "fwd ppp0 203.0.113.24 %s %d-%d 192.168.1.20 %d",
			 i % 2 ? "udp" : "tcp", 1000 + 64 * i, 1063 + 64 * i, 30000 + 64 * i);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	CHECK(s.nmoved == FW_MOVED_MAX);
	CHECK(fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 2000-2001 192.168.1.20 3000", g_err,
			   sizeof(g_err)) == -1 &&
	      strstr(g_err, "more than 1024 ports moved"));
	/* A port, or a range kept, is moved by no map. */
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 2000 192.168.1.20 3000", g_err,
			    sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 2001-2100 192.168.1.20 2001", g_err,
			    sizeof(g_err)));
	CHECK(s.nmoved == FW_MOVED_MAX && s.nfwds == FW_MOVED_MAX / 64 + 2);

	/* No more forwards than the panel's, of each protocol; nor DMZs than connections. */
	fw_spec_init(&s);
	for (int i = 0; i < FW_FWDS_MAX; i++) {
		char line[80];

		snprintf(line, sizeof(line), "fwd ppp0 203.0.113.24 %s %d 192.168.1.20 %d",
			 i % 2 ? "udp" : "tcp", 1000 + i / 2, 1000 + i / 2);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	CHECK(fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 2000 192.168.1.20 2000", g_err,
			   sizeof(g_err)) == -1);
	for (int i = 0; i < FW_DMZS_MAX; i++)
		CHECK(!fw_spec_line(&s, "dmz ppp0 203.0.113.24 192.168.1.30", g_err, sizeof(g_err)));
	CHECK(fw_spec_line(&s, "dmz ppp0 203.0.113.24 192.168.1.30", g_err, sizeof(g_err)) == -1);
}

static void test_forward_batch(void)
{
	static char k[512];
	struct fw_spec s;
	uint32_t last, acked;
	size_t len;

	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "wan gem257", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 8080 192.168.1.20 80", g_err,
			    sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 udp 6000-6010 192.168.1.20 6000",
			    g_err, sizeof(g_err)));

	/*
	 * The prerouting chain after the other two, the forwarded connections
	 * let through after the answers, and the forwards' rules last.
	 */
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCCRRRRRRRRRRR>") && last == acked);
	CHECK(has_bytes(len, "prerouting") && has_bytes(len, "range") &&
	      has_bytes(len, "immediate"));
	CHECK(!has_bytes(len, "lookup") && !has_bytes(len, "__map%d"));

	/*
	 * A range moved to other ports: its map, then its elements, then the
	 * rule that looks it up, where the forward is; each port to its own.
	 */
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 8000-8002 192.168.1.20 9000", g_err,
			    sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 udp 7000 192.168.1.20 7000", g_err,
			    sizeof(g_err)));
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCCRRRRRRRRRRRSERR>") && last == acked);
	CHECK(has_bytes(len, "lookup") && has_bytes(len, "__map%d"));
	CHECK(element(len, 8000, 9000) && element(len, 8001, 9001) && element(len, 8002, 9002));
	CHECK(!element(len, 8003, 9003));

	/* A DMZ, after the forwards: DHCP's answers kept, and the rest, ping too, to the device. */
	CHECK(!fw_spec_line(&s, "dmz ppp0 203.0.113.24 192.168.1.30", g_err, sizeof(g_err)));
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCCRRRRRRRRRRRSERRRR>") && last == acked);

	/* A map of more elements than a request holds takes two of them. */
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "fwd ppp0 203.0.113.24 tcp 10000-11023 192.168.1.20 20000", g_err,
			    sizeof(g_err)));
	s.offload = false;
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTCCCRRRSEER>") && last == acked);
	CHECK(element(len, 10000, 20000) && element(len, 10511, 20511) &&
	      element(len, 10512, 20512) && element(len, 11023, 21023));

	/* A DMZ alone makes the chain, too. */
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "out wan420", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "dmz wan420 192.0.2.10 192.168.1.30", g_err, sizeof(g_err)));
	s.offload = false;
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTCCCRRRRRRR>") && last == acked);

	/* Without either, the table is as it was: no chain, nothing let through. */
	fw_spec_init(&s);
	CHECK(!fw_spec_line(&s, "lan net1 192.168.1.0/24", g_err, sizeof(g_err)));
	CHECK(!fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)));
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	kinds(len, k, sizeof(k), &acked);
	CHECK(len > 0 && !strcmp(k, "<DTFCCRRRRRRRR>") && !has_bytes(len, "prerouting"));

	/* The most there may be fits a batch: every forward, each protocol apart, and DMZ. */
	fw_spec_init(&s);
	for (int i = 0; i < FW_LANS_MAX; i++) {
		char line[48];

		snprintf(line, sizeof(line), "lan net%d 192.168.%d.0/24", i + 1, i + 1);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
		snprintf(line, sizeof(line), "wan gem%d", 256 + i);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	CHECK(!fw_spec_line(&s, "out ppp*", g_err, sizeof(g_err)));
	for (int i = 1; i < FW_OUTS_MAX; i++) {
		char line[48];

		snprintf(line, sizeof(line), "out wan%d", 3000 + i);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	for (int i = 0; i < FW_FWDS_MAX; i++) {
		char line[96];

		snprintf(line, sizeof(line),
			 "fwd averylongname1 203.0.113.24 %s 60000-60100 192.168.255.254 60000",
			 i % 2 ? "udp" : "tcp");
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	for (int i = 0; i < FW_DMZS_MAX; i++)
		CHECK(!fw_spec_line(&s, "dmz averylongname1 203.0.113.24 192.168.1.30", g_err,
				    sizeof(g_err)));
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	CHECK(len > 0 && len <= FW_BATCH_MAX &&
	      last == 1 + 6 + 7 + 4 * 8 + 2 + 2 * 8 * 7 + FW_FWDS_MAX + 2 * FW_DMZS_MAX);
	printf("  the longest batch of ranges kept: %zu bytes\n", len);

	/*
	 * And with every forward a range moved to other ports, which takes a
	 * map and its elements each, the ports moved as many as there may be.
	 */
	s.nfwds = 0;
	s.nmoved = 0;
	for (int i = 0; i < FW_FWDS_MAX; i++) {
		int n = FW_MOVED_MAX / FW_FWDS_MAX + (i < FW_MOVED_MAX % FW_FWDS_MAX);
		char line[96];

		snprintf(line, sizeof(line),
			 "fwd averylongname1 203.0.113.24 %s 60000-%d 192.168.255.254 50000",
			 i % 2 ? "udp" : "tcp", 60000 + n - 1);
		CHECK(!fw_spec_line(&s, line, g_err, sizeof(g_err)));
	}
	CHECK(s.nmoved == FW_MOVED_MAX);
	len = fw_batch(&s, g_buf, sizeof(g_buf), 1, &last);
	CHECK(len > 0 && len <= FW_BATCH_MAX &&
	      last == 1 + 6 + 7 + 4 * 8 + 2 + 2 * 8 * 7 + 3 * FW_FWDS_MAX + 2 * FW_DMZS_MAX);
	printf("  the longest batch: %zu bytes\n", len);
}

int main(void)
{
	test_lines();
	test_batch();
	test_forward_lines();
	test_forward_batch();
	printf("test_fwmsg: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
