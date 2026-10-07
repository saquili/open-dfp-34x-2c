// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the providers, on the files of tests/fixtures and on netlink dumps made here. */

#include "prov.h"
#include "providers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
static int checks;

static void check(bool cond, const char *what)
{
	checks++;
	if (cond) {
		printf("  ok    %s\n", what);
	} else {
		printf("  FAIL  %s\n", what);
		fails++;
	}
}

static const char *render(int (*fn)(struct prov_buf *), char *buf, size_t cap,
			  int *err)
{
	struct prov_buf b = { buf, cap, 0, false };

	*err = fn(&b);
	if (*err)
		return "";
	return b.overflow ? "<overflow>" : buf;
}

static void test_values(void)
{
	char buf[8192];
	const char *s;
	int err;

	puts("providers, against the captured stick");

	s = render(prov_uptime, buf, sizeof(buf), &err);

	check(!err && strstr(s, "\"up_s\":832.34") != NULL,
	      "uptime carries up_s to the hundredth");
	check(!err && strstr(s, "\"idle_s\":819.16") != NULL,
	      "uptime carries idle_s");

	s = render(prov_memory, buf, sizeof(buf), &err);
	check(!err && strstr(s, "\"total_kb\":") != NULL, "memory has a total");
	check(!err && strstr(s, "\"total_kb\":-1") == NULL,
	      "memory total was found, not defaulted");
	check(!err && strstr(s, "\"available_kb\":-1") == NULL,
	      "MemAvailable was found -- the field a person should read");

	s = render(prov_kernel, buf, sizeof(buf), &err);
	check(!err && strstr(s, "Linux version 6.18") != NULL,
	      "kernel version is ours");
	check(!err && strchr(s, '\n') == NULL,
	      "kernel version is one line, newline stripped");

	s = render(prov_model, buf, sizeof(buf), &err);
	check(!err && strstr(s, "DFP-34X-2C3") != NULL,
	      "model comes from the device tree");

	s = render(prov_image, buf, sizeof(buf), &err);
	check(!err && strstr(s, "\"name\":\"") != NULL, "image has a name");
	check(!err && strstr(s, "\"variant\":\"\"") == NULL,
	      "image says whether it runs from RAM or flash");
}

static void test_ports(void)
{
	char buf[16384];
	const char *s;
	int err;

	puts("interfaces, against the captured /sys/class/net");

	s = render(prov_ports, buf, sizeof(buf), &err);
	check(!err, "the interface list reads");
	check(strstr(s, "\"name\":\"sfp\"") != NULL, "the SFP port is listed");
	check(strstr(s, "\"admin_up\":true") != NULL,
	      "flags are read as hexadecimal, so an up port reads as up");
	check(strstr(s, "\"admin_up\":false") == NULL ||
		      strstr(s, "\"name\":\"br0\"") != NULL,
	      "nothing reads down that the capture says is up");
	check(strstr(s, "\"speed_mbit\":2500") != NULL,
	      "the SFP lane's speed comes through (2.5G to the router)");
	check(strstr(s, "\"name\":\"gem257\"") != NULL,
	      "a GEM interface is listed like any other port");
	check(strstr(s, "\"cpu_path\"") != NULL &&
		      strstr(s, "\"packets\":") == NULL,
	      "the software counts are named cpu_path, not 'packets'");
	check(strstr(s, "\"rx_missed\"") != NULL,
	      "the conduit's missed-frame counter is carried");
	check(strstr(s, "\"name\":\"lo\"") == NULL, "loopback is left out");
}

static void test_banks(void)
{
	char buf[32768];
	const char *s;
	int err;

	puts("the flash, against the captured /proc/mtd and both env copies");

	s = render(prov_banks, buf, sizeof(buf), &err);
	check(!err, "the partition table reads");

	check(strstr(s, "\"name\":\"boot\",\"mtd\":0,\"size_b\":262144") !=
		      NULL,
	      "partition sizes are read as hexadecimal, so boot is 256 KB not 40000 bytes");
	check(strstr(s, "\"name\":\"env2\"") != NULL,
	      "env2 is its own partition, matched by name and not confused with env");
	check(strstr(s, "\"name\":\"r1\",\"mtd\":7") != NULL,
	      "the whole table is read, down to the last partition");

	check(strstr(s, "\"erasesize_reported_b\":4096") != NULL,
	      "the erasesize the driver reports is carried under its own name");
	check(strstr(s, "\"erase_block_b\":4096") != NULL,
	      "and, being smaller than the chip, is the erase block");
	check(strstr(s, "\"size_src\":\"/proc/mtd\"") != NULL,
	      "the value says where the sizes came from");

	check(strstr(s, "\"writable\":true") == NULL,
	      "nothing is reported writable");
	check(strstr(s, "\"writable\":null") != NULL &&
		      strstr(s, "\"unreadable\":") != NULL,
	      "with no MTD device to ask, writability is null with a named reason");

	check(strstr(s, "\"layout\":\"redundant\"") != NULL,
	      "the environment is the redundant kind, decided by the CRC not by a guess");
	check(strstr(s, "\"crc_ok\":false") == NULL &&
		      strstr(s, "\"crc_ok\":true") != NULL,
	      "both copies check out -- the CRC covers data from offset 5, not 4");
	check(strstr(s, "\"in_use\":\"env\"") != NULL,
	      "the copy with the higher flag byte wins");

	check(strstr(s, "\"differ\":[\"sw_version0\"]") != NULL,
	      "the one variable the two copies disagree about is named");
	check(strstr(s, "\"differ_n\":1") != NULL, "and counted");
	check(strstr(s, "STALE-COPY") == NULL,
	      "the stale copy's value for that variable does not reach the answer");

	check(strstr(s, "\"active_bank\":0") != NULL,
	      "sw_active decodes to bank 0");
	check(strstr(s, "\"commit_bank\":0") != NULL,
	      "sw_commit decodes to bank 0");
	check(strstr(s, "\"trial_bank\":null") != NULL,
	      "sw_tryactive 2 is the no-trial sentinel, so no bank is being tried");
	check(strstr(s, "\"trial_bank\":2") == NULL,
	      "and it is never reported as bank 2, which does not exist");
	check(strstr(s, "\"next_boot_bank\":0") != NULL,
	      "with no trial armed the next boot follows the commit");
	check(strstr(s, "\"sw_tryactive\":\"2\"") != NULL,
	      "the raw variable is kept beside the decoded bank");

	check(strstr(s, "\"crc_recorded\":\"5bfcffa5\"") != NULL,
	      "each bank's recorded CRC is carried as what it is, a record");
	check(strstr(s, "\"kernel_part\":\"k1\",\"rootfs_part\":\"r1\"") !=
		      NULL,
	      "bank 1 is k1 and r1, resolved out of the partition table");

	check(strstr(s,
		     "\"bank\":0,\"kernel_part\":\"k0\",\"rootfs_part\":\"r0\","
		     "\"valid_recorded\":\"1\",\"crc_recorded\":\"5bfcffa5\","
		     "\"version_recorded\":\"1.0\"") != NULL,
	      "bank 0's recorded state comes from the copy in use, field for field");

	check(strstr(s, "\"rootfs\":{\"error\":") != NULL,
	      "with no kernel command line, the mounted bank is an error and not bank 0");
}

static void test_bank_rules(void)
{
	puts("the boot script's own rules");

	check(prov_uboot_bank("0") == 0, "a commit of \"0\" boots bank 0");
	check(prov_uboot_bank("1") == 1, "a commit of \"1\" boots bank 1");
	check(prov_uboot_bank("00") == 1,
	      "itest.s compares strings, so \"00\" boots bank 1 where atoi would say 0");
	check(prov_uboot_bank(NULL) == 1,
	      "an unset variable expands to \"\", which the else branch sends to bank 1");

	check(prov_root_mtd("console=ttyS0,115200 mtdparts=x:1328K@512K(linux),"
			    "2512K@1840K(rootfs) root=31:5") == 5,
	      "root=31:5 is mtdblock 5");
	check(prov_root_mtd("console=ttyS0,115200 root=31:7") == 7,
	      "root=31:7 is mtdblock 7");
	check(prov_root_mtd("root=/dev/mtdblock7") == 7,
	      "the /dev/mtdblockN spelling reads the same");
	check(prov_root_mtd("root=/dev/ram0") == -1,
	      "a ramdisk root names no mtd partition");
	check(prov_root_mtd("console=ttyS0 nfsroot=31:5 ip=dhcp") == -1,
	      "nfsroot= does not answer for root=");
	check(prov_root_mtd("root=8:2") == -1,
	      "a major that is not mtdblock's 31 is not an mtd index");

	check(prov_rootfs_bank("r0") == 0 && prov_rootfs_bank("r1") == 1,
	      "r0 and r1 are the two banks' root filesystems");
	check(prov_rootfs_bank("rootfs") == -1,
	      "the \"rootfs\" alias follows whichever bank booted, so it names none");
}

#define T_RTM_NEWLINK 16
#define T_RTM_NEWNEIGH 28
#define T_RTM_NEWTFILTER 44

#define T_NLA_F_NESTED 0x8000

#define T_IFLA_IFNAME 3
#define T_IFLA_MASTER 10
#define T_IFLA_LINKINFO 18
#define T_IFLA_AF_SPEC 26
#define T_IFLA_INFO_KIND 1
#define T_IFLA_INFO_DATA 2
#define T_IFLA_GEM_PORT_ID 1
#define T_IFLA_GEM_DIRECTION 2
#define T_IFLA_GEM_TCONT 3
#define T_IFLA_GEM_MULTICAST 4
#define T_IFLA_BRIDGE_VLAN_INFO 2

#define T_RTM_NEWADDR 20
#define T_RTM_NEWROUTE 24
#define T_IFLA_ADDRESS 1
#define T_IFLA_MTU 4
#define T_IFLA_LINK 5
#define T_IFLA_OPERSTATE 16
#define T_IFLA_VLAN_ID 1
#define T_IFA_ADDRESS 1
#define T_IFA_LOCAL 2
#define T_RTA_DST 1
#define T_RTA_OIF 4
#define T_RTA_GATEWAY 5
#define T_RTA_TABLE 15
#define T_AF_INET 2
#define T_AF_INET6 10
#define T_NDA_DST 1

#define T_NDA_LLADDR 2
#define T_NDA_VLAN 5
#define T_NDA_MASTER 9
#define T_NUD_REACHABLE 0x02
#define T_NUD_STALE 0x04
#define T_NUD_NOARP 0x40
#define T_NUD_PERMANENT 0x80
#define T_NTF_SELF 0x02
#define T_NTF_OFFLOADED 0x20

#define T_TCA_KIND 1
#define T_TCA_OPTIONS 2
#define T_TCA_FLOWER_ACT 3
#define T_TCA_FLOWER_KEY_ETH_DST 4
#define T_TCA_FLOWER_KEY_ETH_DST_MASK 5
#define T_TCA_FLOWER_KEY_ETH_TYPE 8
#define T_TCA_FLOWER_KEY_IP_PROTO 9
#define T_TCA_FLOWER_KEY_IPV4_SRC 10
#define T_TCA_FLOWER_KEY_IPV4_SRC_MASK 11
#define T_TCA_FLOWER_FLAGS 22
#define T_TCA_FLOWER_KEY_VLAN_ID 23
#define T_TCA_FLOWER_KEY_VLAN_ETH_TYPE 25
#define T_TCA_FLOWER_KEY_ENC_KEY_ID 26
#define T_TCA_ACT_KIND 1
#define T_TCA_ACT_OPTIONS 2
#define T_TCA_VLAN_PARMS 2
#define T_TCA_VLAN_PUSH_VLAN_ID 3
#define T_TCA_VLAN_PUSH_VLAN_PROTOCOL 4
#define T_TCA_VLAN_ACT_MODIFY 3

#define T_CLSACT_INGRESS 0xFFFFFFF2U

#define T_PON_A_DEV_ID 1
#define T_PON_A_DEV_IFINDEX 2
#define T_PON_A_DEV_TYPE 3
#define T_PON_A_DEV_ONU_STATE 4
#define T_PON_A_DEV_ONU_ID 5
#define T_PON_A_DEV_ALLOC_ID 6
#define T_PON_A_DEV_OMCC_PORT_ID 7
#define T_PON_A_DEV_ALARMS 8
#define T_PON_A_DEV_STATS 9
#define T_PON_A_DEV_SERIAL_NUMBER 10
#define T_PON_A_DEV_STATS_SN_REQUESTS 1
#define T_PON_A_DEV_STATS_PLOAM_RX 3
#define T_PON_A_DEV_STATS_BWMAP_RX 6
#define T_PON_A_DEV_STATS_BIP_ERRORS 10
#define T_PON_A_DEV_STATS_FEC_CODEWORDS 14
#define T_PON_CMD_DEV_GET 1
#define T_PON_FAMILY_ID 25

struct nlb {
	unsigned char buf[8192];
	size_t len;
	size_t msg;
	size_t nest[4];
	unsigned int depth;
};

static void nlb_raw(struct nlb *b, const void *p, size_t n)
{
	memcpy(b->buf + b->len, p, n);
	b->len += n;
}

static void nlb_pad(struct nlb *b)
{
	while (b->len & 3)
		b->buf[b->len++] = 0;
}

static void nlb_msg(struct nlb *b, uint16_t type)
{
	uint32_t zero = 0, seq = 1, pid = 0;
	uint16_t flags = 2;

	b->msg = b->len;
	nlb_raw(b, &zero, 4);
	nlb_raw(b, &type, 2);
	nlb_raw(b, &flags, 2);
	nlb_raw(b, &seq, 4);
	nlb_raw(b, &pid, 4);
}

static void nlb_msg_end(struct nlb *b)
{
	uint32_t mlen = (uint32_t)(b->len - b->msg);

	memcpy(b->buf + b->msg, &mlen, 4);
	nlb_pad(b);
}

static void nlb_attr(struct nlb *b, uint16_t type, const void *val, size_t n)
{
	uint16_t alen = (uint16_t)(4 + n);

	nlb_raw(b, &alen, 2);
	nlb_raw(b, &type, 2);
	if (n)
		nlb_raw(b, val, n);
	nlb_pad(b);
}

static void nlb_u8(struct nlb *b, uint16_t type, uint8_t v)
{
	nlb_attr(b, type, &v, sizeof(v));
}

static void nlb_u16(struct nlb *b, uint16_t type, uint16_t v)
{
	nlb_attr(b, type, &v, sizeof(v));
}

static void nlb_u32(struct nlb *b, uint16_t type, uint32_t v)
{
	nlb_attr(b, type, &v, sizeof(v));
}

static void nlb_u64(struct nlb *b, uint16_t type, uint64_t v)
{
	nlb_attr(b, type, &v, sizeof(v));
}

static void nlb_be16(struct nlb *b, uint16_t type, uint16_t v)
{
	uint16_t n = htons(v);

	nlb_attr(b, type, &n, sizeof(n));
}

static void nlb_str(struct nlb *b, uint16_t type, const char *s)
{
	nlb_attr(b, type, s, strlen(s) + 1);
}

static void nlb_flag(struct nlb *b, uint16_t type)
{
	nlb_attr(b, type, NULL, 0);
}

static void nlb_nest(struct nlb *b, uint16_t type)
{
	uint16_t zero = 0;
	uint16_t t = (uint16_t)(type | T_NLA_F_NESTED);

	b->nest[b->depth++] = b->len;
	nlb_raw(b, &zero, 2);
	nlb_raw(b, &t, 2);
}

static void nlb_nest_end(struct nlb *b)
{
	size_t at = b->nest[--b->depth];
	uint16_t alen = (uint16_t)(b->len - at);

	memcpy(b->buf + at, &alen, 2);
}

static void nlb_ifinfo(struct nlb *b, uint8_t family, int32_t index)
{
	uint16_t iftype = 1;
	uint32_t flags = 0x1003, change = 0;

	b->buf[b->len++] = family;
	b->buf[b->len++] = 0;
	nlb_raw(b, &iftype, 2);
	nlb_raw(b, &index, 4);
	nlb_raw(b, &flags, 4);
	nlb_raw(b, &change, 4);
}

static void nlb_genl(struct nlb *b, uint8_t cmd)
{
	uint16_t reserved = 0;

	b->buf[b->len++] = cmd;
	b->buf[b->len++] = 1;
	nlb_raw(b, &reserved, 2);
}

static void nlb_ndmsg(struct nlb *b, int32_t index, uint16_t state,
		      uint8_t flags)
{
	uint16_t pad = 0;

	b->buf[b->len++] = 7;
	b->buf[b->len++] = 0;
	nlb_raw(b, &pad, 2);
	nlb_raw(b, &index, 4);
	nlb_raw(b, &state, 2);
	b->buf[b->len++] = flags;
	b->buf[b->len++] = 0;
}

static void nlb_tcmsg(struct nlb *b, int32_t index, uint32_t handle,
		      uint32_t parent, uint32_t info)
{
	uint16_t pad = 0;

	b->buf[b->len++] = 0;
	b->buf[b->len++] = 0;
	nlb_raw(b, &pad, 2);
	nlb_raw(b, &index, 4);
	nlb_raw(b, &handle, 4);
	nlb_raw(b, &parent, 4);
	nlb_raw(b, &info, 4);
}

static const char *nlb_render(int (*fn)(const char *, size_t,
					struct prov_buf *),
			      const struct nlb *in, char *buf, size_t cap)
{
	struct prov_buf b = { buf, cap, 0, false };

	if (fn((const char *)in->buf, in->len, &b))
		return "";
	return b.overflow ? "<overflow>" : buf;
}

static bool well_formed(const char *s)
{
	int depth = 0;
	bool in_str = false;
	size_t i;

	for (i = 0; s[i]; i++) {
		if (in_str) {
			if (s[i] == '\\' && s[i + 1])
				i++;
			else if (s[i] == '"')
				in_str = false;
			continue;
		}
		switch (s[i]) {
		case '"':
			in_str = true;
			break;
		case '{':
		case '[':
			depth++;
			break;
		case '}':
		case ']':
			if (--depth < 0)
				return false;
			break;
		case ',':

			if (s[i + 1] == ',' || s[i + 1] == '}' ||
			    s[i + 1] == ']')
				return false;
			if (i && (s[i - 1] == '{' || s[i - 1] == '[' ||
				  s[i - 1] == ','))
				return false;
			break;
		case ':':
			if (s[i + 1] == ',' || s[i + 1] == '}' ||
			    s[i + 1] == ']')
				return false;
			break;
		default:
			break;
		}
	}
	return depth == 0 && !in_str;
}

static int occurrences(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	int found = 0;

	while ((hay = strstr(hay, needle))) {
		found++;
		hay += n;
	}
	return found;
}

static void nlb_links(struct nlb *b)
{
	memset(b, 0, sizeof(*b));

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, 2);
	nlb_str(b, T_IFLA_IFNAME, "cpu");
	nlb_msg_end(b);

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, 3);
	nlb_str(b, T_IFLA_IFNAME, "sfp");
	nlb_u32(b, T_IFLA_MASTER, 5);
	nlb_msg_end(b);

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, 4);
	nlb_str(b, T_IFLA_IFNAME, "pon");
	nlb_msg_end(b);

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, 5);
	nlb_str(b, T_IFLA_IFNAME, "br0");
	nlb_nest(b, T_IFLA_LINKINFO);
	nlb_str(b, T_IFLA_INFO_KIND, "bridge");
	nlb_nest_end(b);
	nlb_msg_end(b);

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, 6);
	nlb_str(b, T_IFLA_IFNAME, "gem257");
	nlb_u32(b, T_IFLA_MASTER, 5);
	nlb_nest(b, T_IFLA_LINKINFO);
	nlb_str(b, T_IFLA_INFO_KIND, "gem");
	nlb_nest(b, T_IFLA_INFO_DATA);
	nlb_u16(b, T_IFLA_GEM_PORT_ID, 257);
	nlb_u8(b, T_IFLA_GEM_DIRECTION, 3);
	nlb_u32(b, T_IFLA_GEM_TCONT, 1);
	nlb_nest_end(b);
	nlb_nest_end(b);
	nlb_msg_end(b);

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, 7);
	nlb_str(b, T_IFLA_IFNAME, "gem4095");
	nlb_nest(b, T_IFLA_LINKINFO);
	nlb_str(b, T_IFLA_INFO_KIND, "gem");
	nlb_nest(b, T_IFLA_INFO_DATA);
	nlb_u16(b, T_IFLA_GEM_PORT_ID, 4095);
	nlb_u8(b, T_IFLA_GEM_DIRECTION, 2);
	nlb_flag(b, T_IFLA_GEM_MULTICAST);
	nlb_nest_end(b);
	nlb_nest_end(b);
	nlb_msg_end(b);

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, 8);
	nlb_str(b, T_IFLA_IFNAME, "gem9");
	nlb_nest(b, T_IFLA_LINKINFO);
	nlb_str(b, T_IFLA_INFO_KIND, "gem");
	nlb_nest(b, T_IFLA_INFO_DATA);
	nlb_u16(b, T_IFLA_GEM_PORT_ID, 1123);
	nlb_u8(b, T_IFLA_GEM_DIRECTION, 1);
	nlb_u32(b, T_IFLA_GEM_TCONT, 2);
	nlb_nest_end(b);
	nlb_nest_end(b);
	nlb_msg_end(b);
}

static void test_netlink_gem(void)
{
	struct nlb links;
	char buf[16384];
	const char *s;

	puts("GEM ports, out of a link dump iproute2 on this image cannot decode");

	nlb_links(&links);
	s = nlb_render(prov_nl_gem_ports, &links, buf, sizeof(buf));

	check(occurrences(s, "\"ifindex\":") == 3,
	      "only the three links of kind \"gem\" are listed");
	check(strstr(s, "\"name\":\"sfp\"") == NULL &&
		      strstr(s, "\"name\":\"br0\"") == NULL,
	      "a port of another kind is not a GEM port just because it is in the dump");

	check(strstr(s,
		     "\"name\":\"gem257\",\"ifindex\":6,\"master\":\"br0\","
		     "\"port_id\":257,\"direction\":\"both\",\"direction_bits\":3,"
		     "\"tcont\":1,\"multicast\":false") != NULL,
	      "a bidirectional GEM port carries its Port-ID, T-CONT and bridge");

	check(strstr(s,
		     "\"name\":\"gem4095\",\"ifindex\":7,\"master\":null,"
		     "\"port_id\":4095,\"direction\":\"downstream\","
		     "\"direction_bits\":2,\"tcont\":null,\"multicast\":true") !=
		      NULL,
	      "a downstream multicast port reports no T-CONT rather than T-CONT 0");
	check(strstr(s, "\"tcont\":0") == NULL,
	      "and no GEM port is given T-CONT 0");

	check(strstr(s, "\"name\":\"gem9\",\"ifindex\":8,\"master\":null,"
			"\"port_id\":1123") != NULL,
	      "the Port-ID comes from the attribute, not from the digits in the name");
	check(strstr(s, "\"port_id\":9") == NULL,
	      "so a port named gem9 carrying Port-ID 1123 is never reported as 9");

	check(strstr(s, "\"direction\":\"upstream\",\"direction_bits\":1") !=
		      NULL,
	      "an upstream-only port names its one direction bit");

	check(well_formed(s),
	      "the GEM list is balanced, with no empty element");
}

static void test_netlink_fdb(void)
{
	struct nlb links, fdb;
	struct prov_buf b;
	char buf[16384];
	const char *s;
	static const unsigned char mc[6] = {
		0x01, 0x00, 0x5e, 0x00, 0x00, 0x01
	};
	static const unsigned char router[6] = { 0x00, 0x00, 0x5e,
						 0x00, 0x53, 0x02 };
	static const unsigned char ours[6] = { 0x00, 0x00, 0x5e,
					       0x00, 0x53, 0x01 };
	static const unsigned char other[6] = { 0x00, 0x00, 0x5e,
						0x00, 0x53, 0x04 };
	static const unsigned char stale[6] = { 0x00, 0x00, 0x5e,
						0x00, 0x53, 0x05 };

	puts("the forwarding database, as the reference capture describes it");

	nlb_links(&links);
	memset(&fdb, 0, sizeof(fdb));

	nlb_msg(&fdb, T_RTM_NEWNEIGH);
	nlb_ndmsg(&fdb, 2, T_NUD_PERMANENT, T_NTF_SELF);
	nlb_attr(&fdb, T_NDA_LLADDR, mc, sizeof(mc));
	nlb_msg_end(&fdb);

	nlb_msg(&fdb, T_RTM_NEWNEIGH);
	nlb_ndmsg(&fdb, 3, T_NUD_REACHABLE, 0);
	nlb_attr(&fdb, T_NDA_LLADDR, router, sizeof(router));
	nlb_u32(&fdb, T_NDA_MASTER, 5);
	nlb_msg_end(&fdb);

	nlb_msg(&fdb, T_RTM_NEWNEIGH);
	nlb_ndmsg(&fdb, 3, T_NUD_REACHABLE, T_NTF_SELF);
	nlb_attr(&fdb, T_NDA_LLADDR, router, sizeof(router));
	nlb_u16(&fdb, T_NDA_VLAN, 410);
	nlb_msg_end(&fdb);

	nlb_msg(&fdb, T_RTM_NEWNEIGH);
	nlb_ndmsg(&fdb, 6, T_NUD_PERMANENT, 0);
	nlb_attr(&fdb, T_NDA_LLADDR, ours, sizeof(ours));
	nlb_u32(&fdb, T_NDA_MASTER, 5);
	nlb_u16(&fdb, T_NDA_VLAN, 1);
	nlb_msg_end(&fdb);

	nlb_msg(&fdb, T_RTM_NEWNEIGH);
	nlb_ndmsg(&fdb, 99, T_NUD_NOARP,
		  (uint8_t)(T_NTF_SELF | T_NTF_OFFLOADED));
	nlb_attr(&fdb, T_NDA_LLADDR, other, sizeof(other));
	nlb_msg_end(&fdb);

	nlb_msg(&fdb, T_RTM_NEWNEIGH);
	nlb_ndmsg(&fdb, 3, T_NUD_STALE, 0);
	nlb_attr(&fdb, T_NDA_LLADDR, stale, sizeof(stale));
	nlb_u32(&fdb, T_NDA_MASTER, 5);
	nlb_msg_end(&fdb);

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	check(prov_nl_fdb((const char *)links.buf, links.len,
			  (const char *)fdb.buf, fdb.len, &b) == 0 &&
		      !b.overflow,
	      "the database reads");
	s = buf;

	check(occurrences(s, "\"addr\":") == 6,
	      "every entry in the dump is reported");

	check(strstr(s, "\"dev\":\"cpu\"") != NULL &&
		      strstr(s, "\"dev\":\"gem257\"") != NULL,
	      "an interface index becomes the name the link dump gave it");
	check(strstr(s, "\"dev\":null,\"ifindex\":99") != NULL,
	      "an index the dump did not carry is null, and the index is kept beside it");

	check(strstr(s,
		     "\"addr\":\"00:00:5e:00:53:02\",\"vlan\":null,"
		     "\"master\":\"br0\",\"state\":\"reachable\",\"learned\":true,"
		     "\"self\":false") != NULL,
	      "an address the bridge learned says so, and names the bridge");
	check(strstr(s,
		     "\"addr\":\"00:00:5e:00:53:01\",\"vlan\":1,\"master\":\"br0\","
		     "\"state\":\"permanent\",\"learned\":false") != NULL,
	      "the port's own address is permanent and was not learned");

	check(strstr(s, "\"state\":\"static\",\"learned\":false") != NULL,
	      "NUD_NOARP is the configured entry, and it was not learned either");
	check(occurrences(s, "\"learned\":true") == 3,
	      "the two reachable entries and the stale one count as learned");

	check(strstr(s,
		     "\"addr\":\"00:00:5e:00:53:05\",\"vlan\":null,"
		     "\"master\":\"br0\",\"state\":\"stale\",\"learned\":true") !=
		      NULL,
	      "an aged-out entry keeps the kernel's own word and still counts as learned");

	check(strstr(s, "\"vlan\":410,\"master\":null,\"state\":\"reachable\","
			"\"learned\":true,\"self\":true") != NULL,
	      "the switch's own table comes back under self, with its VLAN");
	check(strstr(s, "\"vlan\":0") == NULL,
	      "an entry with no NDA_VLAN reports null, never VLAN 0, which is a real VLAN");

	check(strstr(s, "\"offloaded\":true") != NULL &&
		      occurrences(s, "\"offloaded\":true") == 1,
	      "NTF_OFFLOADED is carried, and only for the entry that has it");

	check(well_formed(s),
	      "the database is balanced, with no empty element");
}

static void test_netlink_vlans(void)
{
	struct nlb br;
	char buf[16384];
	const char *s;
	unsigned int i;

	static const uint16_t pvid_untagged = 0x2 | 0x4;
	static const char *const ports[] = { "sfp", "gem257", "gem4095",
					     "br0" };

	puts("per-port VLAN membership, as the reference capture describes it");

	memset(&br, 0, sizeof(br));

	for (i = 0; i < 4; i++) {
		uint16_t vi[2] = { pvid_untagged, 1 };

		nlb_msg(&br, T_RTM_NEWLINK);
		nlb_ifinfo(&br, 7, (int32_t)(3 + i));
		nlb_str(&br, T_IFLA_IFNAME, ports[i]);
		nlb_nest(&br, T_IFLA_AF_SPEC);
		nlb_attr(&br, T_IFLA_BRIDGE_VLAN_INFO, vi, sizeof(vi));
		nlb_nest_end(&br);
		nlb_msg_end(&br);
	}

	{
		uint16_t tagged_pvid[2] = { 0x2, 410 };
		uint16_t untagged_only[2] = { 0x4, 7 };

		nlb_msg(&br, T_RTM_NEWLINK);
		nlb_ifinfo(&br, 7, 3);
		nlb_str(&br, T_IFLA_IFNAME, "sfp");
		nlb_nest(&br, T_IFLA_AF_SPEC);
		nlb_attr(&br, T_IFLA_BRIDGE_VLAN_INFO, tagged_pvid,
			 sizeof(tagged_pvid));
		nlb_attr(&br, T_IFLA_BRIDGE_VLAN_INFO, untagged_only,
			 sizeof(untagged_only));
		nlb_nest_end(&br);
		nlb_msg_end(&br);
	}

	{
		uint16_t v1[2] = { pvid_untagged, 1 };
		uint16_t v410[2] = { 0, 410 };

		nlb_msg(&br, T_RTM_NEWLINK);
		nlb_ifinfo(&br, 7, 20);
		nlb_str(&br, T_IFLA_IFNAME, "bench0");
		nlb_nest(&br, T_IFLA_AF_SPEC);
		nlb_attr(&br, T_IFLA_BRIDGE_VLAN_INFO, v1, sizeof(v1));
		nlb_attr(&br, T_IFLA_BRIDGE_VLAN_INFO, v410, sizeof(v410));
		nlb_nest_end(&br);
		nlb_msg_end(&br);
	}

	{
		uint16_t begin[2] = { 0x8, 100 };
		uint16_t end[2] = { 0x10, 200 };

		nlb_msg(&br, T_RTM_NEWLINK);
		nlb_ifinfo(&br, 7, 21);
		nlb_str(&br, T_IFLA_IFNAME, "bench1");
		nlb_nest(&br, T_IFLA_AF_SPEC);
		nlb_attr(&br, T_IFLA_BRIDGE_VLAN_INFO, begin, sizeof(begin));
		nlb_attr(&br, T_IFLA_BRIDGE_VLAN_INFO, end, sizeof(end));
		nlb_nest_end(&br);
		nlb_msg_end(&br);
	}

	s = nlb_render(prov_nl_vlans, &br, buf, sizeof(buf));

	check(strstr(s,
		     "\"vlan\":410,\"pvid\":true,\"egress_untagged\":false") !=
		      NULL,
	      "a PVID that is carried tagged keeps the two flags apart");
	check(strstr(s, "\"vlan\":7,\"pvid\":false,\"egress_untagged\":true") !=
		      NULL,
	      "and so does an untagged VLAN that is not the port's default");
	check(strstr(s, "{\"dev\":\"sfp\",\"vlan\":1,\"pvid\":true,"
			"\"egress_untagged\":true,\"range_begin\":false,"
			"\"range_end\":false}") != NULL,
	      "the capture's \"1 PVID Egress Untagged\" comes back field for field");
	check(occurrences(s, "\"dev\":\"br0\"") == 1,
	      "the bridge device itself is a row like any port");

	check(strstr(s, "\"dev\":\"bench0\",\"vlan\":1,") != NULL &&
		      strstr(s, "\"dev\":\"bench0\",\"vlan\":410,") != NULL,
	      "a port in two VLANs reports both, not just the last attribute");
	check(occurrences(s, "\"dev\":\"bench0\"") == 2,
	      "and reports them exactly once each");

	check(strstr(s,
		     "\"dev\":\"bench1\",\"vlan\":100,\"pvid\":false,"
		     "\"egress_untagged\":false,\"range_begin\":true") != NULL,
	      "a compressed range is flagged as a range and not as a lone VLAN");
	check(strstr(s, "\"vlan\":200,\"pvid\":false,\"egress_untagged\":false,"
			"\"range_begin\":false,\"range_end\":true") != NULL,
	      "and so is its far end");

	check(occurrences(s, "\"vlan\":") == 10,
	      "every VLAN entry in the dump is reported");

	check(well_formed(s),
	      "the VLAN list is balanced, with no empty element");
}

static void test_netlink_filters(void)
{
	struct nlb tf;
	char buf[16384];
	const char *s;

	static const int32_t vlan_parms[6] = { 1, 0, 3,
					       1, 1, T_TCA_VLAN_ACT_MODIFY };

	puts("tc filters on the ingress hook");

	memset(&tf, 0, sizeof(tf));
	s = nlb_render(prov_nl_filters, &tf, buf, sizeof(buf));
	check(!strcmp(s, "[]"),
	      "a board with no rules installed serves an empty list, not an error");

	memset(&tf, 0, sizeof(tf));
	nlb_msg(&tf, T_RTM_NEWTFILTER);

	nlb_tcmsg(&tf, 3, 1, T_CLSACT_INGRESS, (49152U << 16) | htons(0x8100));
	nlb_str(&tf, T_TCA_KIND, "flower");
	nlb_nest(&tf, T_TCA_OPTIONS);
	nlb_u16(&tf, T_TCA_FLOWER_KEY_VLAN_ID, 410);
	nlb_be16(&tf, T_TCA_FLOWER_KEY_ETH_TYPE, 0x8100);
	nlb_u32(&tf, T_TCA_FLOWER_FLAGS, 0x2 | 0x4);
	nlb_u32(&tf, T_TCA_FLOWER_KEY_ENC_KEY_ID, 7);
	nlb_nest(&tf, T_TCA_FLOWER_ACT);
	nlb_nest(&tf, 1);
	nlb_str(&tf, T_TCA_ACT_KIND, "vlan");
	nlb_nest(&tf, T_TCA_ACT_OPTIONS);
	nlb_attr(&tf, T_TCA_VLAN_PARMS, vlan_parms, sizeof(vlan_parms));
	nlb_u16(&tf, T_TCA_VLAN_PUSH_VLAN_ID, 1410);
	nlb_be16(&tf, T_TCA_VLAN_PUSH_VLAN_PROTOCOL, 0x8100);
	nlb_nest_end(&tf);
	nlb_nest_end(&tf);
	nlb_nest_end(&tf);
	nlb_nest_end(&tf);
	nlb_msg_end(&tf);

	nlb_msg(&tf, T_RTM_NEWTFILTER);
	nlb_tcmsg(&tf, 4, 2, T_CLSACT_INGRESS, (100U << 16) | htons(0x88a8));
	nlb_str(&tf, T_TCA_KIND, "flower");
	nlb_nest(&tf, T_TCA_OPTIONS);
	nlb_u16(&tf, T_TCA_FLOWER_KEY_VLAN_ID, 11);
	nlb_be16(&tf, T_TCA_FLOWER_KEY_VLAN_ETH_TYPE, 0x8100);
	nlb_nest_end(&tf);
	nlb_msg_end(&tf);

	{
		static const unsigned char mc_addr[6] = { 0x01, 0, 0, 0, 0, 0 };
		static const unsigned char mc_mask[6] = { 0x01, 0, 0, 0, 0, 0 };
		static const unsigned char ip_src[4] = { 192, 0, 2, 2 };
		static const unsigned char ip_mask[4] = { 255, 255, 255, 0 };

		nlb_msg(&tf, T_RTM_NEWTFILTER);
		nlb_tcmsg(&tf, 3, 4, T_CLSACT_INGRESS,
			  (300U << 16) | htons(0x0800));
		nlb_str(&tf, T_TCA_KIND, "flower");
		nlb_nest(&tf, T_TCA_OPTIONS);
		nlb_attr(&tf, T_TCA_FLOWER_KEY_ETH_DST, mc_addr,
			 sizeof(mc_addr));
		nlb_attr(&tf, T_TCA_FLOWER_KEY_ETH_DST_MASK, mc_mask,
			 sizeof(mc_mask));
		nlb_attr(&tf, T_TCA_FLOWER_KEY_IPV4_SRC, ip_src,
			 sizeof(ip_src));
		nlb_attr(&tf, T_TCA_FLOWER_KEY_IPV4_SRC_MASK, ip_mask,
			 sizeof(ip_mask));
		nlb_u8(&tf, T_TCA_FLOWER_KEY_IP_PROTO, 17);
		nlb_u32(&tf, T_TCA_FLOWER_FLAGS, 0x2 | 0x8);
		nlb_nest_end(&tf);
		nlb_msg_end(&tf);
	}

	nlb_msg(&tf, T_RTM_NEWTFILTER);
	nlb_tcmsg(&tf, 3, 3, T_CLSACT_INGRESS, (200U << 16) | htons(0x0003));
	nlb_str(&tf, T_TCA_KIND, "matchall");
	nlb_msg_end(&tf);

	s = nlb_render(prov_nl_filters, &tf, buf, sizeof(buf));

	check(occurrences(s, "\"handle\":") == 4,
	      "every filter in the dump is reported");
	check(strstr(s,
		     "\"handle\":1,\"priority\":49152,\"ethertype\":33024") !=
		      NULL,
	      "the handle, the preference and the ethertype come out of tcm_info");
	check(strstr(s, "\"ethertype\":129") == NULL,
	      "the ethertype in tcm_info is network order -- 0x8100, not 0x0081");

	check(strstr(s, "\"vlan_id\":410,") != NULL,
	      "the flower VLAN id is host order, despite what its header comment says");
	check(strstr(s, "39425") == NULL,
	      "and is never the byte-swapped 39425 the header's \"be16\" would give");

	check(strstr(s, "\"offload\":{\"skip_sw\":true,\"skip_hw\":false,"
			"\"in_hw\":true,\"not_in_hw\":false}") != NULL,
	      "the rule says whether the kernel got it into hardware");

	check(strstr(s,
		     "\"kind\":\"vlan\",\"vlan_action\":\"modify\",\"index\":1,"
		     "\"vlan_id\":1410,\"vlan_tpid\":33024,\"vlan_prio\":null") !=
		      NULL,
	      "the action decodes to `vlan modify id 1410`, which is the operator's rule");
	check(strstr(s, "33285") == NULL,
	      "the action's VLAN id is host order too, not the byte-swapped 33285");

	check(strstr(s, "\"match_unparsed\":[26]") != NULL,
	      "a match this decoder cannot name is reported as a number, not dropped");
	check(strstr(s, "\"match_unparsed\":[]") != NULL,
	      "and a rule with nothing unknown in it says so");

	check(strstr(s,
		     "\"vlan_id\":11,\"vlan_prio\":null,\"vlan_ethtype\":33024") !=
		      NULL,
	      "the inner tag's TPID is network order and lands under its own name");
	check(occurrences(s, "\"vlan_ethtype\":null") == 2,
	      "a rule with one tag or none leaves it null, not a copy of the ethertype");

	check(strstr(s, "\"eth_dst\":\"01:00:00:00:00:00\","
			"\"eth_dst_mask\":\"01:00:00:00:00:00\"") != NULL,
	      "a masked destination match carries its mask, so it reads as all multicast");
	check(strstr(s, "\"ipv4_src\":\"192.0.2.2\","
			"\"ipv4_src_mask\":\"255.255.255.0\"") != NULL,
	      "an address match and its prefix both come through");
	check(strstr(s, "\"ip_proto\":17,") != NULL, "so does the L4 protocol");

	check(strstr(s, "\"skip_sw\":true,\"skip_hw\":false,\"in_hw\":false,"
			"\"not_in_hw\":true") != NULL,
	      "a rule the driver did not take says NOT in hardware, not merely nothing");

	check(strstr(s, "\"handle\":3,\"priority\":200,\"ethertype\":3,") !=
		      NULL,
	      "a filter of an unknown kind keeps its identity");
	check(strstr(s, "\"kind\":\"matchall\",\"options_decoded\":false") !=
		      NULL,
	      "and says plainly that its options were not decoded");

	check(well_formed(s),
	      "the filter list is balanced, with no empty element");
}

static void nlb_link(struct nlb *b, int32_t index, const char *name,
		     int32_t lower, const char *kind, int vlan, uint8_t oper)
{
	static const unsigned char mac[6] = { 0x00, 0x00, 0x5e,
					      0x00, 0x53, 0x01 };

	nlb_msg(b, T_RTM_NEWLINK);
	nlb_ifinfo(b, 0, index);
	nlb_str(b, T_IFLA_IFNAME, name);
	nlb_attr(b, T_IFLA_ADDRESS, mac, sizeof(mac));
	nlb_u32(b, T_IFLA_MTU, 1500);
	nlb_u32(b, T_IFLA_LINK, (uint32_t)lower);
	nlb_u8(b, T_IFLA_OPERSTATE, oper);
	if (kind) {
		nlb_nest(b, T_IFLA_LINKINFO);
		nlb_str(b, T_IFLA_INFO_KIND, kind);
		if (vlan >= 0) {
			nlb_nest(b, T_IFLA_INFO_DATA);
			nlb_u16(b, T_IFLA_VLAN_ID, (uint16_t)vlan);
			nlb_nest_end(b);
		}
		nlb_nest_end(b);
	}
	nlb_msg_end(b);
}

static void nlb_addr(struct nlb *b, uint8_t family, uint8_t prefix,
		     uint8_t scope, int32_t index, uint16_t attr,
		     const void *ip, size_t n)
{
	uint8_t head[4] = { family, prefix, 0, scope };

	nlb_msg(b, T_RTM_NEWADDR);
	nlb_raw(b, head, sizeof(head));
	nlb_raw(b, &index, 4);
	nlb_attr(b, attr, ip, n);
	nlb_msg_end(b);
}

static void nlb_route(struct nlb *b, uint8_t family, uint8_t dst_len,
		      uint8_t table, uint8_t proto, uint8_t type,
		      const void *dst, size_t n, const void *gw, int32_t oif)
{
	uint8_t head[8] = { family, dst_len, 0, 0, table, proto, 0, type };
	uint32_t flags = 0;

	nlb_msg(b, T_RTM_NEWROUTE);
	nlb_raw(b, head, sizeof(head));
	nlb_raw(b, &flags, 4);
	nlb_u32(b, T_RTA_TABLE, table);
	if (dst)
		nlb_attr(b, T_RTA_DST, dst, n);
	if (gw)
		nlb_attr(b, T_RTA_GATEWAY, gw, n);
	nlb_u32(b, T_RTA_OIF, (uint32_t)oif);
	nlb_msg_end(b);
}

static void nlb_neigh(struct nlb *b, uint8_t family, int32_t index,
		      uint16_t state, const void *ip, size_t n,
		      const unsigned char *mac)
{
	uint16_t pad = 0;

	nlb_msg(b, T_RTM_NEWNEIGH);
	b->buf[b->len++] = family;
	b->buf[b->len++] = 0;
	nlb_raw(b, &pad, 2);
	nlb_raw(b, &index, 4);
	nlb_raw(b, &state, 2);
	b->buf[b->len++] = 0;
	b->buf[b->len++] = 0;
	nlb_attr(b, T_NDA_DST, ip, n);
	if (mac)
		nlb_attr(b, T_NDA_LLADDR, mac, 6);
	nlb_msg_end(b);
}

static void test_netlink_network(void)
{
	static const unsigned char host[6] = { 0x00, 0x00, 0x5e,
					       0x00, 0x53, 0x02 };
	static const uint8_t mgmt_ip[4] = { 192, 0, 2, 254 };
	static const uint8_t lan_ip[4] = { 169, 254, 0, 1 };
	static const uint8_t net[4] = { 192, 0, 2, 252 };
	static const uint8_t host_ip[4] = { 192, 0, 2, 253 };
	static const uint8_t public_ip[4] = { 203, 0, 113, 24 };
	static const uint8_t gw[4] = { 198, 51, 100, 1 };
	/* Marked link-local by its scope, as the kernel says, whatever its prefix. */
	static const uint8_t ll6[16] = { 0x20, 0x01, 0x0d, 0xb8,
					 [8] = 0x02, [15] = 0x01 };
	static const uint8_t doc6[16] = { 0x20, 0x01, 0x0d, 0xb8, [15] = 0x01 };
	struct nlb links, addrs, routes, neigh;
	struct prov_buf b;
	char buf[16384];
	const char *s;

	puts("the network: interfaces with VLANs and addresses, routes, neighbours");

	memset(&links, 0, sizeof(links));
	nlb_link(&links, 2, "cpu", 2, NULL, -1, 0);
	nlb_link(&links, 3, "sfp", 2, "dsa", -1, 6);
	nlb_link(&links, 4, "pon", 2, "dsa", -1, 6);
	nlb_link(&links, 5, "mgmt", 3, "vlan", 4094, 6);
	nlb_link(&links, 6, "lan", 3, "vlan", 36, 6);
	nlb_link(&links, 9, "gem257", 4, "gem", -1, 0);
	nlb_link(&links, 10, "wan", 9, "vlan", 410, 2);

	memset(&addrs, 0, sizeof(addrs));
	nlb_addr(&addrs, T_AF_INET, 30, 0, 5, T_IFA_LOCAL, mgmt_ip, 4);
	nlb_addr(&addrs, T_AF_INET, 32, 0, 6, T_IFA_LOCAL, lan_ip, 4);
	nlb_addr(&addrs, T_AF_INET6, 64, 253, 5, T_IFA_ADDRESS, ll6, 16);

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	check(prov_nl_links((const char *)links.buf, links.len,
			    (const char *)addrs.buf, addrs.len, &b) == 0 &&
		      !b.overflow && well_formed(buf),
	      "the interfaces read, balanced");
	s = buf;
	check(occurrences(s, "\"ifindex\":") == 7, "every interface is listed");
	check(strstr(s, "\"name\":\"cpu\",\"ifindex\":2,\"kind\":null,"
			"\"lower\":null") != NULL,
	      "the conduit runs over nothing: a link to itself is no lower device");
	check(strstr(s, "\"name\":\"sfp\",\"ifindex\":3,\"kind\":\"dsa\","
			"\"lower\":\"cpu\",\"vlan\":null") != NULL,
	      "a switch port names its conduit and has no VLAN");
	check(strstr(s, "\"name\":\"lan\",\"ifindex\":6,\"kind\":\"vlan\","
			"\"lower\":\"sfp\",\"vlan\":36") != NULL,
	      "a VLAN names its port and its VID");
	check(strstr(s, "\"name\":\"wan\",\"ifindex\":10,\"kind\":\"vlan\","
			"\"lower\":\"gem257\",\"vlan\":410,\"master\":null,"
			"\"operstate\":\"down\"") != NULL,
	      "a VLAN over a GEM port, and a link that is down, read as such");
	check(strstr(s, "\"mtu\":1500,\"mac\":\"00:00:5e:00:53:01\"") != NULL,
	      "the MTU and the MAC address are reported");
	check(strstr(s, "\"addresses\":[{\"family\":\"ipv4\",\"address\":"
			"\"192.0.2.254/30\",\"link_local\":false},{\"family\":"
			"\"ipv6\",\"address\":\"2001:db8::200:0:0:1/64\","
			"\"link_local\":true}]") != NULL,
	      "an interface carries its IPv4 and IPv6 addresses, link-local marked");
	check(strstr(s, "\"169.254.0.1/32\"") != NULL &&
		      occurrences(s, "\"addresses\":[]") == 5,
	      "and the others carry theirs or none");

	memset(&routes, 0, sizeof(routes));
	nlb_route(&routes, T_AF_INET, 0, 254, 3, 1, NULL, 4, gw, 10);
	nlb_route(&routes, T_AF_INET, 30, 254, 2, 1, net, 4, NULL, 5);
	nlb_route(&routes, T_AF_INET, 32, 254, 3, 1, public_ip, 4, NULL, 6);
	nlb_route(&routes, T_AF_INET, 32, 255, 2, 2, mgmt_ip, 4, NULL, 5);
	nlb_route(&routes, T_AF_INET6, 64, 254, 2, 1, doc6, 16, NULL, 5);
	nlb_route(&routes, T_AF_INET6, 8, 254, 2, 5, doc6, 16, NULL, 5);

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	check(prov_nl_routes((const char *)links.buf, links.len,
			     (const char *)routes.buf, routes.len, &b) == 0 &&
		      !b.overflow && well_formed(buf),
	      "the routes read, balanced");
	s = buf;
	check(occurrences(s, "\"destination\":") == 4,
	      "the local table and a multicast route are left out");
	check(strstr(s, "{\"family\":\"ipv4\",\"destination\":\"default\","
			"\"gateway\":\"198.51.100.1\",\"dev\":\"wan\","
			"\"protocol\":\"static\"") != NULL,
	      "a route with no destination is the default, through its gateway");
	check(strstr(s, "\"destination\":\"192.0.2.252/30\",\"gateway\":null,"
			"\"dev\":\"mgmt\",\"protocol\":\"kernel\"") != NULL,
	      "a connected network has no gateway and comes from the kernel");
	check(strstr(s, "\"destination\":\"203.0.113.24/32\",\"gateway\":null,"
			"\"dev\":\"lan\"") != NULL,
	      "the host route to the handed-over address names its VLAN");
	check(strstr(s, "\"family\":\"ipv6\",\"destination\":\"2001:db8::1/64\"") !=
		      NULL,
	      "an IPv6 route reads in IPv6 notation");

	memset(&neigh, 0, sizeof(neigh));
	nlb_neigh(&neigh, T_AF_INET, 5, T_NUD_REACHABLE, host_ip, 4, host);
	nlb_neigh(&neigh, T_AF_INET, 6, T_NUD_STALE, public_ip, 4, host);
	nlb_neigh(&neigh, T_AF_INET6, 5, T_NUD_NOARP, ll6, 16, NULL);
	nlb_neigh(&neigh, 7, 3, T_NUD_PERMANENT, host, 6, host);

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	check(prov_nl_neighbours((const char *)links.buf, links.len,
				 (const char *)neigh.buf, neigh.len, &b) == 0 &&
		      !b.overflow && well_formed(buf),
	      "the neighbours read, balanced");
	s = buf;
	check(occurrences(s, "\"address\":") == 2,
	      "a multicast mapping and a bridge entry are not neighbours");
	check(strstr(s, "{\"family\":\"ipv4\",\"address\":\"192.0.2.253\","
			"\"mac\":\"00:00:5e:00:53:02\",\"dev\":\"mgmt\","
			"\"state\":\"reachable\"}") != NULL,
	      "a neighbour names its address, MAC, interface and state");
	check(strstr(s, "\"address\":\"203.0.113.24\"") != NULL &&
		      strstr(s, "\"state\":\"stale\"") != NULL,
	      "the host behind the handed-over address is a neighbour on its VLAN");
}

static void test_netlink_malformed(void)
{
	struct nlb b;
	char buf[4096];
	const char *s;
	uint32_t huge = 65536;

	puts("malformed dumps");

	memset(&b, 0, sizeof(b));
	nlb_msg(&b, T_RTM_NEWLINK);
	nlb_ifinfo(&b, 0, 3);
	nlb_str(&b, T_IFLA_IFNAME, "sfp");
	nlb_msg_end(&b);
	memcpy(b.buf, &huge, 4);
	s = nlb_render(prov_nl_vlans, &b, buf, sizeof(buf));
	check(!strcmp(s, "[]"),
	      "a record longer than its buffer ends the walk");

	memset(&b, 0, sizeof(b));
	nlb_msg(&b, T_RTM_NEWTFILTER);
	b.buf[b.len++] = 0;
	nlb_msg_end(&b);
	s = nlb_render(prov_nl_filters, &b, buf, sizeof(buf));
	check(!strcmp(s, "[]"),
	      "a truncated fixed header is skipped, not read past");

	memset(&b, 0, sizeof(b));
	nlb_msg(&b, T_RTM_NEWLINK);
	nlb_ifinfo(&b, 7, 3);
	nlb_str(&b, T_IFLA_IFNAME, "sfp");
	nlb_nest(&b, T_IFLA_AF_SPEC);
	{
		uint16_t bad[2] = { 4000, 0 };

		nlb_raw(&b, bad, 4);
	}
	nlb_nest_end(&b);
	nlb_msg_end(&b);
	s = nlb_render(prov_nl_vlans, &b, buf, sizeof(buf));
	check(!strcmp(s, "[]"),
	      "an attribute longer than its nest ends that walk");

	{
		struct nlb links;
		struct prov_buf pb = { buf, sizeof(buf), 0, false };
		static const unsigned char shortmac[4] = { 1, 2, 3, 4 };

		nlb_links(&links);
		memset(&b, 0, sizeof(b));
		nlb_msg(&b, T_RTM_NEWNEIGH);
		nlb_ndmsg(&b, 3, T_NUD_REACHABLE, 0);
		nlb_attr(&b, T_NDA_LLADDR, shortmac, sizeof(shortmac));
		nlb_msg_end(&b);

		check(prov_nl_fdb((const char *)links.buf, links.len,
				  (const char *)b.buf, b.len, &pb) == 0,
		      "a short hardware address does not stop the walk");
		check(strstr(buf, "\"addr\":null") != NULL,
		      "and is reported as no address rather than as six bytes it does not have");
	}
}

static void test_netlink_onu(void)
{
	struct nlb links, dev;
	struct prov_buf b;
	char buf[16384];
	const char *s;

	static const unsigned char sn[8] = {
		'A', 'B', 'C', 'D', 0x12, 0x34, 0x56, 0x78,
	};

	puts("the ONU below the interface, as the pon family reports it");

	nlb_links(&links);
	memset(&dev, 0, sizeof(dev));

	nlb_msg(&dev, T_PON_FAMILY_ID);
	nlb_genl(&dev, T_PON_CMD_DEV_GET);
	nlb_u32(&dev, T_PON_A_DEV_ID, 0);
	nlb_u32(&dev, T_PON_A_DEV_IFINDEX, 4);
	nlb_u32(&dev, T_PON_A_DEV_TYPE, 1);
	nlb_u32(&dev, T_PON_A_DEV_ONU_STATE, 5);
	nlb_u32(&dev, T_PON_A_DEV_ONU_ID, 1);
	nlb_u32(&dev, T_PON_A_DEV_ALLOC_ID, 1);
	nlb_u32(&dev, T_PON_A_DEV_ALLOC_ID, 1024);
	nlb_u32(&dev, T_PON_A_DEV_OMCC_PORT_ID, 4095);
	nlb_u32(&dev, T_PON_A_DEV_ALARMS, 0);

	nlb_nest(&dev, T_PON_A_DEV_STATS);
	nlb_u32(&dev, T_PON_A_DEV_STATS_SN_REQUESTS, 12);
	nlb_u32(&dev, T_PON_A_DEV_STATS_PLOAM_RX, 624);
	nlb_u64(&dev, T_PON_A_DEV_STATS_BWMAP_RX, 4300000000ULL);
	nlb_u32(&dev, T_PON_A_DEV_STATS_BIP_ERRORS, 0);
	nlb_u64(&dev, T_PON_A_DEV_STATS_FEC_CODEWORDS, 1224000ULL);
	nlb_nest_end(&dev);
	nlb_attr(&dev, T_PON_A_DEV_SERIAL_NUMBER, sn, sizeof(sn));
	nlb_msg_end(&dev);

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	check(prov_nl_onu((const char *)links.buf, links.len,
			  (const char *)dev.buf, dev.len, &b) == 0,
	      "a dev-get reply parses");
	s = buf;

	check(strstr(s, "\"state\":\"o5\"") != NULL,
	      "the activation state is named, not numbered");
	check(strstr(s, "\"interface\":\"pon\"") != NULL,
	      "the ifindex is resolved against the link dump");
	check(strstr(s, "\"type\":\"gpon\"") != NULL,
	      "and the PON type is named");
	check(strstr(s, "\"onu_id\":1") != NULL, "the ONU-ID the OLT assigned");
	check(strstr(s, "\"alloc_ids\":[1,1024]") != NULL,
	      "BOTH allocations are reported, not just the last of the repeated attribute");
	check(strstr(s, "\"omcc_port_id\":4095") != NULL, "the OMCC Port-ID");
	check(strstr(s, "\"alarms\":[]") != NULL,
	      "no alarm is an empty list, which is not the same as no answer");
	check(strstr(s, "\"serial\":\"ABCD12345678\"") != NULL,
	      "the serial number is four ASCII bytes and four in hexadecimal");
	check(strstr(s, "\"sn_requests\":12") != NULL,
	      "a counter the kernel wrote in four bytes");
	check(strstr(s, "\"bwmap_rx\":4300000000") != NULL,
	      "and one it wrote in eight, which four would have read as nonsense");
	check(strstr(s, "\"bip_errors\":0") != NULL,
	      "a counter of zero is a measurement and is reported as one");
	check(strstr(s, "\"ploam_overflows\":null") != NULL,
	      "while a counter the MAC does not keep is null, not zero");
	check(strstr(s, "\"fec_codewords\":1224000") != NULL,
	      "the FEC counts follow the BIP errors");
	check(strstr(s, "\"fec_corrected_bytes\":null") != NULL,
	      "each under its own name");
	check(well_formed(s), "the object is balanced");

	memset(&dev, 0, sizeof(dev));
	nlb_msg(&dev, T_PON_FAMILY_ID);
	nlb_genl(&dev, T_PON_CMD_DEV_GET);
	nlb_u32(&dev, T_PON_A_DEV_ID, 0);
	nlb_u32(&dev, T_PON_A_DEV_IFINDEX, 4);
	nlb_u32(&dev, T_PON_A_DEV_TYPE, 1);
	nlb_u32(&dev, T_PON_A_DEV_ONU_STATE, 3);
	nlb_u32(&dev, T_PON_A_DEV_ALARMS, 11);
	nlb_attr(&dev, T_PON_A_DEV_SERIAL_NUMBER, sn, sizeof(sn));
	nlb_msg_end(&dev);

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	prov_nl_onu((const char *)links.buf, links.len, (const char *)dev.buf,
		    dev.len, &b);

	check(strstr(s, "\"state\":\"o3\"") != NULL, "O3 is O3");
	check(strstr(s, "\"onu_id\":null") != NULL,
	      "an unassigned ONU-ID is null and never 0");
	check(strstr(s, "\"omcc_port_id\":null") != NULL,
	      "and an unconfigured OMCC is null and never Port-ID 0");
	check(strstr(s, "\"alloc_ids\":[]") != NULL, "with no allocations");
	check(strstr(s, "\"alarms\":[\"los\",\"lof\",\"pee\"]") != NULL,
	      "the alarm bits are named individually");

	memset(&dev, 0, sizeof(dev));
	nlb_msg(&dev, T_PON_FAMILY_ID);
	nlb_genl(&dev, T_PON_CMD_DEV_GET);
	nlb_u32(&dev, T_PON_A_DEV_ID, 0);
	nlb_msg_end(&dev);

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	prov_nl_onu((const char *)links.buf, links.len, (const char *)dev.buf,
		    dev.len, &b);

	check(strstr(s, "\"state\":null") != NULL &&
		      strstr(s, "\"type\":null") != NULL,
	      "a MAC that reports no status says so in every field");
	check(strstr(s, "\"alarms\":null") != NULL,
	      "and no alarm word is null, which an empty list would have claimed to know");
	check(strstr(s, "\"counters\":null") != NULL,
	      "and no counters at all, rather than ten zeros");
	check(strstr(s, "\"serial\":null") != NULL,
	      "including the serial number");

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	check(prov_nl_onu((const char *)links.buf, links.len, "", 0, &b) == 0 &&
		      strcmp(buf, "null") == 0,
	      "a kernel with no ONU answers null rather than failing");

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	prov_nl_onu((const char *)links.buf, links.len, (const char *)dev.buf,
		    8, &b);
	check(strcmp(buf, "null") == 0, "and half a message is not an ONU");
}

static void test_optics(void)
{
	struct prov_buf b;
	char buf[1024];

	puts("the optics, found by name among the hwmon devices");

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	check(prov_optics(&b) == 0, "the row reads");

	check(strstr(buf, "\"temperature_c\":44.050") != NULL,
	      "the module's temperature, in degrees rather than millidegrees");
	check(strstr(buf, "\"temperature_c\":39") == NULL,
	      "and not the other hwmon device's, which is what hwmon0 would have given");
	check(strstr(buf, "\"vcc_v\":3.217") != NULL, "its supply");
	check(strstr(buf, "\"bias_ma\":7") != NULL, "the laser bias");
	check(strstr(buf, "\"tx_power_uw\":1686") != NULL,
	      "the light it sends");
	check(strstr(buf, "\"rx_power_uw\":10") != NULL,
	      "and the light it receives, which is the one that answers 'is there a fibre'");
	check(well_formed(buf), "the object is balanced");
}

static void test_writers(void)
{
	char small[8];
	char buf[256];
	struct prov_buf b;

	puts("json writers");

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	pv_str(&b, "a\"b\\c\nd");
	check(!strcmp(buf, "\"a\\\"b\\\\c\\nd\""),
	      "quotes, backslashes and newlines are escaped");

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	pv_str(&b, "esc\033[2J");
	check(strstr(buf, "\\u001b") != NULL,
	      "a control byte from the kernel log cannot reach a terminal as an escape");

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	pv_fixed(&b, -1234, 2);
	check(!strcmp(buf, "-12.34"),
	      "a negative fixed-point keeps its fraction");

	b = (struct prov_buf){ buf, sizeof(buf), 0, false };
	pv_fixed(&b, 5, 3);
	check(!strcmp(buf, "0.005"),
	      "a small fixed-point keeps its leading zeros");

	memset(small, 'X', sizeof(small));
	b = (struct prov_buf){ small, sizeof(small), 0, false };
	pv_str(&b, "this string is far too long for the buffer");
	check(b.overflow, "a value that does not fit sets overflow");
	check(memchr(small, '\0', sizeof(small)) != NULL,
	      "and still terminates the string, so the caller's strlen stays in bounds");
	check(strlen(small) < sizeof(small),
	      "and does not run past the buffer");
}

/* A file of the test tree: write it with @mode, or read it whole. */
static void put(const char *path, const char *text, mode_t mode)
{
	char rooted[512];
	const char *at = prov_path(path, rooted, sizeof(rooted));
	FILE *f = fopen(at, "w");

	if (f) {
		fputs(text, f);
		fclose(f);
		chmod(at, mode);
	}
}

static const char *slurp(const char *path, char *buf, size_t cap)
{
	return prov_slurp(path, buf, cap, NULL) ? NULL : buf;
}

static void pause_10ms(void)
{
	const struct timespec ts = { .tv_nsec = 10 * 1000 * 1000 };

	nanosleep(&ts, NULL);
}

/* The settings once onu-mode's stand-in has ended, waiting up to 5 s. */
static const char *settled(char *buf, size_t cap)
{
	const char *s = "";
	int err;

	for (int i = 0; i < 500; i++) {
		s = render(prov_settings, buf, cap, &err);
		if (strstr(s, "\"applying\":false"))
			break;
		pause_10ms();
	}
	return s;
}

/*
 * onu-mode's stand-in notes that it ran, then waits for run/release, so that
 * the tests see a change that is still being applied.
 */
static const char apply_stub[] =
	"#!/bin/sh\n"
	"echo \"$1\" >>\"$POND_ROOT/run/applied\"\n"
	"i=0\n"
	"while [ ! -e \"$POND_ROOT/run/release\" ] && [ $i -lt 500 ]; do\n"
	"\tsleep 0.01\n"
	"\ti=$((i + 1))\n"
	"done\n";

static void test_settings(void)
{
	static const char *const bad[] = {
		"FOO=1",
		"MODE=switch",
		"MODE=router;reboot",
		"WAN_VLAN=0",
		"WAN_VLAN=4095",
		"WAN_GEM=4096",
		"WAN_GEM=012",
		"LAN_ADDRESS=192.0.2.1",
		"LAN_ADDRESS=192.0.2.1/33",
		"LAN_ADDRESS=192.0.2.256/24",
		"WAN_GATEWAY=192.0.2.01",
		"LAN_DHCP=yes",
		"MODE=passthru",
		"WAN_MAC=01:00:5e:00:00:01",
		"WAN_MAC=00:00:00:00:00:00",
		"WAN_MAC=00:00:5e:00:53",
		"WAN_MAC=00-00-5e-00-53-01",
		"LAN_VLAN=4095",
		"PASS_GATEWAY=169.254.0",
		"PPPOE_USER=two words",
		"PPPOE_SECRET=",
		"a line without an equals sign",
	};
	char buf[4096], file[512], err[256], what[128], stub[512];
	const char *s;
	struct stat st;
	int rc;

	puts("the settings of the mode, and changes to them");

	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	setenv("POND_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);

	s = settled(buf, sizeof(buf));
	check(strstr(s, "\"MODE\":\"\"") &&
		      strstr(s, "\"WAN_PROTO\":\"dhcp\""),
	      "without a file, the settings are onu-mode's defaults");
	check(strstr(s, "\"sessions\":[]") && strstr(s, "\"mode\":null") &&
		      strstr(s, "\"wan\":null"),
	      "no session, and nothing brought up yet");

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		rc = prov_settings_set(bad[i], err, sizeof(err));
		snprintf(what, sizeof(what), "refused: %s", bad[i]);
		check(rc == -EINVAL && *err, what);
	}
	rc = prov_settings_set("MODE=router\nWAN_PROTO=pppoe\n", err, sizeof(err));
	check(rc == -EINVAL && strstr(err, "PPPOE1_SECRET"),
	      "PPPoE without a user and a secret is refused");
	rc = prov_settings_set("PPPOE2_USER=bench\n", err, sizeof(err));
	check(rc == -EINVAL && strstr(err, "PPPOE2_SECRET"),
	      "an account without a secret is refused");
	rc = prov_settings_set("SESSION8_VLAN=410\n", err, sizeof(err));
	check(rc == -EINVAL, "a session past the seventh is refused");
	rc = prov_settings_set("SESSION1_FAMILY=ipx\n", err, sizeof(err));
	check(rc == -EINVAL, "a family that is not one is refused");
	rc = prov_settings_set("LAN_DHCP_START=192.0.2.100\nLAN_DHCP_END=192.0.2.199\n",
			       err, sizeof(err));
	check(rc == -EINVAL, "a DHCP range outside the LAN's network is refused");
	check(!slurp("/run/onu.conf", file, sizeof(file)) &&
		      !slurp("/run/applied", file, sizeof(file)),
	      "a refused change writes nothing and applies nothing");

	rc = prov_settings_set("MODE=router\r\nWAN_PROTO=pppoe\r\nWAN_VLAN=410\r\n"
			       "LAN_ADDRESS=192.0.2.1/24\r\n"
			       "LAN_DHCP_START=192.0.2.100\r\n"
			       "LAN_DHCP_END=192.0.2.199\r\n"
			       "PPPOE_USER=bench\r\nPPPOE_SECRET=not-a-real-one\r\n",
			       err, sizeof(err));
	check(rc == 0, "a whole change, with the line ends of a form, is taken");
	s = slurp("/run/onu.conf", file, sizeof(file));
	check(s && strstr(s, "\nMODE=router\n") && strstr(s, "\nWAN_VLAN=410\n") &&
		      strstr(s, "\nLAN_DHCP=on\n"),
	      "onu.conf has the change and keeps the rest");
	s = slurp("/run/onu-pppoe1", file, sizeof(file));
	check(s && !strcmp(s, "bench not-a-real-one\n"),
	      "the first session's file has the user and the secret, as pppoed reads them");
	check(!stat(prov_path("/run/onu-pppoe1", file, sizeof(file)), &st) &&
		      (st.st_mode & 0777) == 0600,
	      "and only its owner can read it");

	for (int i = 0; i < 500 && !slurp("/run/applied", file, sizeof(file)); i++)
		pause_10ms();
	s = slurp("/run/applied", file, sizeof(file));
	check(s && !strcmp(s, "apply\n"), "the change starts onu-mode apply");

	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"applying\":true") != NULL,
	      "the settings say a change is being applied");
	check(strstr(s, "\"sessions\":[{\"id\":1,\"user\":\"bench\",\"secret_set\":true,") != NULL,
	      "they give the session's user and that a secret is set");
	check(!strstr(s, "not-a-real-one"), "but never the secret");
	rc = prov_settings_set("WAN_VLAN=36", err, sizeof(err));
	check(rc == -EBUSY, "a change while one is being applied is refused");

	put("/run/release", "", 0644);
	s = settled(buf, sizeof(buf));
	check(strstr(s, "\"applying\":false") &&
		      strstr(s, "\"last_apply_status\":0"),
	      "once onu-mode has ended, the settings say how it ended");

	rc = prov_settings_set("WAN_PROTO=static\nWAN_ADDRESS=198.51.100.2/24\n"
			       "WAN_GATEWAY=198.51.100.1\n",
			       err, sizeof(err));
	check(rc == 0, "a change of a few settings is taken");
	s = slurp("/run/onu.conf", file, sizeof(file));
	check(s && strstr(s, "\nMODE=router\n") && strstr(s, "\nWAN_VLAN=410\n") &&
		      strstr(s, "\nWAN_PROTO=static\n"),
	      "and merged with the settings before it");
	s = slurp("/run/onu-pppoe1", file, sizeof(file));
	check(s && !strcmp(s, "bench not-a-real-one\n"),
	      "a change without PPPoE leaves the PPPoE file as it was");
	settled(buf, sizeof(buf));

	rc = prov_settings_set("WAN_GATEWAY=203.0.113.1", err, sizeof(err));
	check(rc == -EINVAL, "a gateway outside the WAN's network is refused");
	rc = prov_settings_set("WAN_GATEWAY=198.51.100.2", err, sizeof(err));
	check(rc == -EINVAL, "a gateway that is the WAN's own address is refused");
	rc = prov_settings_set("WAN_GATEWAY=", err, sizeof(err));
	check(rc == -EINVAL, "a static WAN without a gateway is refused");
	rc = prov_settings_set("LAN_DHCP_START=192.0.2.199\nLAN_DHCP_END=192.0.2.100\n",
			       err, sizeof(err));
	check(rc == -EINVAL, "a DHCP range that ends before it starts is refused");

	rc = prov_settings_set("MODE=passthrough\nWAN_PROTO=pppoe\nPASS_GATEWAY=127.0.0.1\n",
			       err, sizeof(err));
	check(rc == -EINVAL && strstr(err, "PASS_GATEWAY"),
	      "a passthrough gateway no station can have is refused");
	rc = prov_settings_set("MODE=passthrough\nWAN_PROTO=pppoe\nLAN_VLAN=410\n"
			       "WAN_MAC=00:00:5e:00:53:01\n",
			       err, sizeof(err));
	check(rc == 0, "passthrough over PPPoE, its link to the router in a VLAN, is taken");
	s = slurp("/run/onu.conf", file, sizeof(file));
	check(s && strstr(s, "\nMODE=passthrough\n") && strstr(s, "\nLAN_VLAN=410\n") &&
		      strstr(s, "\nPASS_GATEWAY=169.254.0.1\n") &&
		      strstr(s, "\nWAN_MAC=00:00:5e:00:53:01\n"),
	      "onu.conf has the passthrough settings, the gateway its default");
	settled(buf, sizeof(buf));

	put("/run/onu-mode", "router\n", 0644);
	put("/run/onu-wan", "gem1 wan ppp0\n", 0644);
	put("/run/onu-dns", "198.51.100.53\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"mode\":\"router\"") && strstr(s, "\"out\":\"ppp0\"") &&
		      strstr(s, "\"dns\":\"198.51.100.53\"") && strstr(s, "\"pass\":null"),
	      "the settings carry what onu-mode brought up");
	put("/run/onu-pass", "192.0.2.2\n", 0644);
	put("/run/onu-lan", "lan\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"pass\":{\"address\":\"192.0.2.2\",\"lan\":\"lan\"}") != NULL,
	      "and the address a session handed to the router");
	mkdir(prov_path("/run/session", file, sizeof(file)), 0755);
	mkdir(prov_path("/run/session/1", file, sizeof(file)), 0755);
	put("/run/session/1/dev", "wan410\n", 0644);
	put("/run/session/1/gem", "gem1\n", 0644);
	put("/run/session/1/ppp", "ppp0\n", 0644);
	put("/run/session/1/ipv4", "192.0.2.2\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"hands_on\":true,\"dev\":\"wan410\",\"gem_dev\":\"gem1\","
			"\"ppp\":\"ppp0\",\"ipv4\":\"192.0.2.2\",\"ipv6\":null,"
			"\"prefix\":null,\"prefix_host\":null") != NULL,
	      "a session carries what onu-mode brought up for it");
	put("/run/session/1/prefix", "2001:db8:1200::/56\n", 0644);
	put("/run/session/1/host", "fe80::2\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"prefix\":\"2001:db8:1200::/56\",\"prefix_host\":\"fe80::2\"") != NULL,
	      "and the IPv6 prefix dhcp6d took for it, and the host it went to");
	check(strstr(s, "\"store\":null") != NULL,
	      "without a word from onu-store, the store's state is unknown");
	put("/run/onu-store", "saved\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"store\":\"saved\"") != NULL,
	      "and with one, what became of the settings in the store");

	setenv("POND_APPLY", "/nonexistent/onu-mode", 1);
	rc = prov_settings_set("LAN_DHCP=off", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && strstr(s, "\"last_apply_status\":127"),
	      "an onu-mode that cannot run is reported, not hidden");
	unsetenv("POND_APPLY");
}

/* onu-store's stand-in: the identity it is given goes to a file. */
static const char store_stub[] =
	"#!/bin/sh\n[ \"$1\" = identity ] && cat > \"$POND_ROOT/stored-identity\"\n";

static bool file_has(const char *path, const char *text)
{
	char buf[2048];

	return !prov_slurp(path, buf, sizeof(buf), NULL) && strstr(buf, text) != NULL;
}

static void test_panel_values(void)
{
	char buf[4096], file[512], err[256], stub[512];
	const char *s;
	struct stat st;
	int rc, e;

	puts("the panel's identity, account and link");
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/run/onu-identity",
	    "serial=XXXXXXXXXXXX\nvendor_id=XXXX\nversion=V1.0\n"
	    "ploam_password=0123456789\nmodel=EXAMPLE\n", 0600);
	put("/run/onu-identity-source", "store\n", 0644);
	s = render(prov_identity, buf, sizeof(buf), &e);
	check(!e && strstr(s, "\"serial\":\"XXXXXXXXXXXX\"") && strstr(s, "\"ploam_set\":true") &&
		      strstr(s, "\"source\":\"store\""),
	      "the identity is served, and whether a PLOAM password is set");
	check(!strstr(s, "0123456789"), "but never the PLOAM password itself");

	put("/store-stub", store_stub, 0755);
	setenv("POND_STORE", prov_path("/store-stub", stub, sizeof(stub)), 1);
	setenv("POND_REIDENTIFY", "true", 1);
	check(prov_identity_set("SERIAL=XXXX", err, sizeof(err)) == -EINVAL,
	      "a serial number of the wrong form is refused");
	check(prov_identity_set("MODEL=a=b", err, sizeof(err)) == -EINVAL,
	      "a value with '=' is refused");
	check(prov_identity_set("PLOAM=not-a-ploam-password", err, sizeof(err)) == -EINVAL,
	      "a PLOAM password of the wrong form is refused");
	check(prov_identity_set("COLOUR=red", err, sizeof(err)) == -EINVAL,
	      "a field the identity lacks is refused");
	rc = prov_identity_set("SERIAL=000000000000\nPLOAM=\nVERSION=V2.0\n", err, sizeof(err));
	check(rc == 0 && file_has("/stored-identity", "SERIAL=000000000000\nVERSION=V2.0\n") &&
		      !file_has("/stored-identity", "PLOAM"),
	      "a good identity goes to onu-store, an empty PLOAM keeping the one there is");
	for (int i = 0; i < 100; i++) {
		prov_identity_reap();
		pause_10ms();
	}

	/* The sessions start from a stick without settings or accounts. */
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/run/onu-pppoe%d", n);
		unlink(prov_path(path, file, sizeof(file)));
	}
	rc = prov_session_set("USER=user@example.net\nSECRET=example-secret\nVLAN=410\n"
			      "FAMILY=ipv4\nMAC=00:00:5e:00:53:01\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "MODE=\n") &&
		      file_has("/run/onu.conf", "SESSION1_VLAN=410") &&
		      file_has("/run/onu.conf", "SESSION1_FAMILY=ipv4") &&
		      file_has("/run/onu-pppoe1", "user@example.net example-secret"),
	      "an account makes the stick hold a session, in the operator's VLAN");
	rc = prov_session_set("USER=user@example.net/ipv6\nSECRET=example-secret\n"
			      "VLAN=410\nFAMILY=ipv6\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "SESSION2_FAMILY=ipv6") &&
		      file_has("/run/onu-pppoe2", "user@example.net/ipv6 example-secret"),
	      "a second account takes the first free session");
	check(strstr(s, "\"id\":1,") && strstr(s, "\"id\":2,") &&
		      strstr(s, "\"family\":\"ipv6\",\"hands_on\":false"),
	      "both are served, and the IPv6 one hands no address on");
	check(prov_session_set("ID=2\nMAC=00:00:5e:00:53:02\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "one MAC address"),
	      "two sessions in one VLAN that would send from two MAC addresses are refused");
	check(prov_session_set("ID=1\nVLAN=5000\n", err, sizeof(err)) == -EINVAL,
	      "a VLAN out of range is refused");
	check(prov_session_set("ID=3\nVLAN=36\n", err, sizeof(err)) == -EINVAL,
	      "a change to a session without an account is refused");
	check(prov_session_set("VLAN=36\n", err, sizeof(err)) == -EINVAL,
	      "a new session without a user is refused");
	rc = prov_session_set("ID=2\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "SESSION2_") &&
		      stat(prov_path("/run/onu-pppoe2", file, sizeof(file)), &st) != 0 &&
		      file_has("/run/onu-pppoe1", "user@example.net"),
	      "removing a session forgets its account and its settings, and keeps the other");
	rc = prov_session_set("ID=1\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && strstr(s, "\"sessions\":[]") &&
		      stat(prov_path("/run/onu-pppoe1", file, sizeof(file)), &st) != 0,
	      "without accounts there are no sessions: the router dials through the stick");

	rc = prov_link_set("VLAN=36\nGATEWAY=169.254.0.1\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "LAN_VLAN=36") &&
		      file_has("/run/onu.conf", "PASS_GATEWAY=169.254.0.1"),
	      "the link to the router takes its VLAN and the gateway it hands out");
	check(prov_link_set("VLAN=0", err, sizeof(err)) == -EINVAL, "a VLAN of 0 is refused");
	(void)s;
	unsetenv("POND_STORE");
	unsetenv("POND_REIDENTIFY");
}

/* onu-update's stand-in: the status of a stick that runs bank 1 on trial. */
static const char update_stub[] =
	"#!/bin/sh\n"
	"[ \"$1\" = status ] || exit 2\n"
	"printf 'RUNNING=1\\nCOMMITTED=0\\nNEXT=0\\nTRIAL=1\\n'\n"
	"printf 'BANK0_STATE=valid\\nBANK0_VERSION=0123456789ab\\nBANK0_TIME=1759000000\\n'\n"
	"printf 'BANK0_KERNEL_SIZE=1900544\\nBANK0_ROOTFS_SIZE=2031616\\n'\n"
	"printf 'BANK1_STATE=valid\\nBANK1_VERSION=7c5fba548abc\\nBANK1_TIME=1759800000\\n'\n"
	"printf 'BANK1_KERNEL_SIZE=1900544\\nBANK1_ROOTFS_SIZE=2031616\\n'\n";

static void test_firmware(void)
{
	char buf[2048], stub[512];
	const char *s;
	int err;

	puts("the banks and the updates");

	put("/update-stub", update_stub, 0755);
	setenv("POND_ONU_UPDATE", prov_path("/update-stub", stub, sizeof(stub)), 1);
	s = render(prov_firmware, buf, sizeof(buf), &err);
	check(!err && strstr(s, "\"running\":1,\"committed\":0,\"next\":0,\"trial\":1,"
				"\"failed\":null"),
	      "the running bank, the committed one, the next start and the trial");
	check(strstr(s, "{\"bank\":1,\"state\":\"valid\",\"version\":\"7c5fba548abc\","
			"\"time\":1759800000,\"kernel_size_b\":1900544,"
			"\"rootfs_size_b\":2031616}") != NULL,
	      "each bank, with its version, time and sizes");
	check(strstr(s, "\"update\":{\"state\":\"idle\",\"received_b\":0") != NULL,
	      "no update under way");

	setenv("POND_ONU_UPDATE", "/nonexistent/onu-update", 1);
	s = render(prov_firmware, buf, sizeof(buf), &err);
	check(err == -EIO, "an onu-update that cannot run is an error, not empty banks");
	unsetenv("POND_ONU_UPDATE");
}

static void test_table(void)
{
	const struct prov *p;

	puts("the table");

	p = prov_find("uptime");
	check(p && prov_present(p), "a served item is present");

	p = prov_find("optics");
	check(p && prov_present(p),
	      "the optics are served, now that the driver publishes them as hwmon");
	p = prov_find("omci");
	check(p != NULL,
	      "an unserved item is still in the table, so the gap stays visible");
	check(p && !prov_present(p),
	      "but it is absent, so the schema leaves it out");

	p = prov_find("bridge");
	check(p && prov_present(p) && (p->flags & PROV_SLOW),
	      "the bridge is served, and marked slow because it walks two whole tables");
	p = prov_find("gem_ports");
	check(p && prov_present(p) && !(p->flags & PROV_SLOW),
	      "the GEM ports are served, and bounded enough to poll");
	p = prov_find("tc_rules");
	check(p && prov_present(p), "the tc rules are served");

	p = prov_find("gem_counters");
	check(p && prov_present(p),
	      "per-GEM-port counters are served now that the MAC's own have a uAPI");
	check(p && !(p->flags & PROV_SLOW),
	      "and pollable: two ioctls per gem interface, none at all without one");

	p = prov_find("settings");
	check(p && prov_present(p) && p->read && p->write,
	      "the settings of the mode are served, and can be changed");

	check(prov_find("no_such_item") == NULL, "an unknown id finds nothing");
}

static void test_failure_paths(void)
{
	char buf[64];
	size_t len;

	puts("the failure paths themselves");

	check(prov_slurp("/nonexistent/for/the/test", buf, sizeof(buf), &len) !=
		      0,
	      "a missing file is an error, not an empty value");
	check(prov_slurp("/proc/meminfo", buf, 8, &len) != 0,
	      "a file larger than the buffer is an error, not its first eight bytes");
	check(prov_field("MemTotal: 24004 kB\n", "MemFree", -7) == -7,
	      "a key that is not there returns the caller's absent value");
	check(prov_field("CmaMemFree: 99 kB\nMemFree: 12 kB\n", "MemFree",
			 -1) == 12,
	      "a key is matched at a line start, not inside another key's name");
}

int main(void)
{
	if (!getenv("POND_ROOT")) {
		fprintf(stderr,
			"POND_ROOT is unset: point it at tests/fixtures/stick\n");
		return 2;
	}

	test_values();
	test_ports();
	test_banks();
	test_bank_rules();
	test_netlink_gem();
	test_netlink_fdb();
	test_netlink_vlans();
	test_netlink_filters();
	test_netlink_network();
	test_netlink_malformed();
	test_netlink_onu();
	test_optics();
	test_writers();
	test_settings();
	test_firmware();
	test_panel_values();
	test_table();
	test_failure_paths();

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
