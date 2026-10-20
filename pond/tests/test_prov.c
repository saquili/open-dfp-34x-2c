// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the providers, on the files of tests/fixtures and on netlink dumps made here. */

#include "prov.h"
#include "providers.h"
#include "value.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/socket.h>
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
	struct prov_buf b = { .buf = buf, .cap = cap };

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

	check(!err && strstr(s, "\"cached_kb\":4976") != NULL,
	      "memory says how much the page cache holds");

	s = render(prov_cpu, buf, sizeof(buf), &err);
	check(!err && strstr(s, "\"system\":26732") != NULL &&
		      strstr(s, "\"idle\":374601") != NULL,
	      "cpu carries the ticks of the first line of /proc/stat");
	check(!err && strstr(s, "\"softirq\":285") != NULL &&
		      strstr(s, "\"steal\":0") != NULL,
	      "cpu reads every state of the line, in its order");
	check(!err && strstr(s, "\"load\":[0.28,0.18,0.13]") != NULL,
	      "cpu carries the three load averages");
	check(!err && strstr(s, "\"running\":1,\"threads\":49") != NULL,
	      "cpu carries the runnable and existing threads");
	check(!err && strstr(s, "\"model\":\"Lexra LX5281 V0.2\"") != NULL,
	      "cpu names the processor's model");

	s = render(prov_hostname, buf, sizeof(buf), &err);
	check(!err && !strcmp(s, "\"dfp-34x-2c3\""),
	      "hostname is the kernel's, without its newline");

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

	check(strstr(s, "\"name\":\"boot\",\"mtd\":0,\"size_b\":262144,\"type\":\"norflash\","
			"\"type_code\":3,\"writable\":false}") != NULL,
	      "the driver's type and flags come from sysfs: boot takes no writes");
	check(strstr(s, "\"name\":\"env\",\"mtd\":1,\"size_b\":8192,\"type\":\"norflash\","
			"\"type_code\":3,\"writable\":true}") != NULL,
	      "and env does");
	check(strstr(s, "\"name\":\"r1\",\"mtd\":7,\"size_b\":2572288,\"type\":null,"
			"\"type_code\":null,\"writable\":null,\"unreadable\":") != NULL,
	      "a partition sysfs says nothing of is null, with a named reason");

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
#define T_PON_A_DEV_EQD 15
#define T_PON_A_DEV_ONU_STATE_TIME 16
#define T_PON_A_DEV_STATS_SN_REQUESTS 1
#define T_PON_A_DEV_STATS_PLOAM_RX 3
#define T_PON_A_DEV_STATS_BWMAP_RX 6
#define T_PON_A_DEV_STATS_BIP_ERRORS 10
#define T_PON_A_DEV_STATS_FEC_CODEWORDS 14
#define T_PON_A_DEV_STATS_PASSWORD_REQUESTS 15
#define T_PON_A_DEV_STATS_PASSWORD_REPLIES 16
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
	struct prov_buf b = { .buf = buf, .cap = cap };

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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
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
		struct prov_buf pb = { .buf = buf, .cap = sizeof(buf) };
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
	nlb_u32(&dev, T_PON_A_DEV_STATS_PASSWORD_REQUESTS, 2);
	nlb_u32(&dev, T_PON_A_DEV_STATS_PASSWORD_REPLIES, 2);
	nlb_nest_end(&dev);
	nlb_attr(&dev, T_PON_A_DEV_SERIAL_NUMBER, sn, sizeof(sn));
	nlb_u32(&dev, T_PON_A_DEV_EQD, 97328);
	nlb_u64(&dev, T_PON_A_DEV_ONU_STATE_TIME, 5000000000ULL);
	nlb_msg_end(&dev);

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
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
	check(strstr(s, "\"password_requests\":2,\"password_replies\":2") != NULL,
	      "the OLT's requests for the password, and the answers");
	check(strstr(s, "\"eqd\":97328") != NULL,
	      "the equalization delay of Ranging_Time");
	check(strstr(s, "\"state_ms\":5000000000") != NULL,
	      "the time in the state, past what four bytes hold");
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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
	prov_nl_onu((const char *)links.buf, links.len, (const char *)dev.buf,
		    dev.len, &b);

	check(strstr(s, "\"state\":\"o3\"") != NULL, "O3 is O3");
	check(strstr(s, "\"eqd\":null") != NULL &&
		      strstr(s, "\"state_ms\":null") != NULL,
	      "no equalization delay before O5, and no time the MAC did not give");
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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
	check(prov_nl_onu((const char *)links.buf, links.len, "", 0, &b) == 0 &&
		      strcmp(buf, "null") == 0,
	      "a kernel with no ONU answers null rather than failing");

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
	prov_nl_onu((const char *)links.buf, links.len, (const char *)dev.buf,
		    8, &b);
	check(strcmp(buf, "null") == 0, "and half a message is not an ONU");
}

static void test_optics(void)
{
	struct prov_buf b;
	char buf[1024];

	puts("the optics, found by name among the hwmon devices");

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
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

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
	pv_str(&b, "a\"b\\c\nd");
	check(!strcmp(buf, "\"a\\\"b\\\\c\\nd\""),
	      "quotes, backslashes and newlines are escaped");

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
	pv_str(&b, "esc\033[2J");
	check(strstr(buf, "\\u001b") != NULL,
	      "a control byte from the kernel log cannot reach a terminal as an escape");

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
	pv_fixed(&b, -1234, 2);
	check(!strcmp(buf, "-12.34"),
	      "a negative fixed-point keeps its fraction");

	b = (struct prov_buf){ .buf = buf, .cap = sizeof(buf) };
	pv_fixed(&b, 5, 3);
	check(!strcmp(buf, "0.005"),
	      "a small fixed-point keeps its leading zeros");

	memset(small, 'X', sizeof(small));
	b = (struct prov_buf){ .buf = small, .cap = sizeof(small) };
	pv_str(&b, "this string is far too long for the buffer");
	check(b.overflow, "a value that does not fit sets overflow");
	check(memchr(small, '\0', sizeof(small)) != NULL,
	      "and still terminates the string, so the caller's strlen stays in bounds");
	check(strlen(small) < sizeof(small),
	      "and does not run past the buffer");
}

/* A buffer that grows on the heap, as a reply's does (value.c); @ctx counts. */
static bool heap_grow(struct prov_buf *b, size_t cap)
{
	char *p = realloc(b->buf, cap);

	if (!p)
		return false;
	b->buf = p;
	b->cap = cap;
	(*(int *)b->ctx)++;
	return true;
}

static void test_growth(void)
{
	char fixed[64];
	int grows = 0;
	struct prov_buf b = { .max = 4096, .grow = heap_grow, .ctx = &grows };

	puts("a buffer that grows");

	for (int i = 0; i < 1000; i++)
		pv_str(&b, "ab");
	check(!b.overflow && b.len == 4000 && strlen(b.buf) == 4000,
	      "a text of 4000 bytes is written whole into a buffer that started with none");
	check(grows == 5 && b.cap == 4096,
	      "the buffer grew five times, twice its size each time, not at each write");
	for (int i = 0; i < 10; i++)
		pv_str(&b, "a little more than there is room for");
	check(b.overflow && b.cap == 4096 && strlen(b.buf) < b.cap,
	      "a text that would pass its most overflows, and the buffer stays within it");
	free(b.buf);

	grows = 0;
	b = (struct prov_buf){ .max = 100000, .grow = heap_grow, .ctx = &grows };
	pv_raw(&b, "{");
	pv_reserve(&b, 65536);
	check(!b.overflow && b.cap - b.len > 65536 && grows == 2,
	      "room is made for what another program writes into the buffer");
	pv_reserve(&b, 200000);
	check(!b.overflow && b.cap == 100000,
	      "as far as the buffer grows, and no further");
	free(b.buf);

	b = (struct prov_buf){ .buf = fixed, .cap = sizeof(fixed) };
	pv_reserve(&b, 65536);
	check(b.cap == sizeof(fixed) && !b.overflow,
	      "a buffer of a fixed size keeps its size");
}

/* A provider of a string of @g_big_len bytes of JSON, quotes included. */
static size_t g_big_len;

static int big_read(struct prov_buf *out)
{
	static const char x[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
	size_t left = g_big_len - 2;

	pv_raw(out, "\"");
	while (left && !out->overflow) {
		size_t n = left < sizeof(x) - 1 ? left : sizeof(x) - 1;

		pv_raw(out, x + sizeof(x) - 1 - n);
		left -= n;
	}
	pv_raw(out, "\"");
	return 0;
}

static int broken_read(struct prov_buf *out)
{
	pv_raw(out, "{\"half\":");
	return -EIO;
}

/* A connection of no socket: its send buffer is all a reply touches. */
static struct mg_connection *conn_new(void)
{
	struct mg_connection *c = calloc(1, sizeof(*c));

	if (c)
		c->send.align = MG_IO_SIZE;
	return c;
}

static void conn_free(struct mg_connection *c)
{
	mg_iobuf_free(&c->send);
	free(c);
}

/*
 * The reply that @c holds: its status, and its body, which must be as long
 * as its Content-Length says; -1 for one that is not. The caller frees @body.
 */
static int reply_of(struct mg_connection *c, char **body)
{
	char *t = malloc(c->send.len + 1), *end, *cl;
	int status = -1;

	*body = NULL;
	if (!t || !c->send.len)
		goto out;
	memcpy(t, c->send.buf, c->send.len);
	t[c->send.len] = '\0';
	end = strstr(t, "\r\n\r\n");
	cl = strstr(t, "\r\nContent-Length: ");
	if (strncmp(t, "HTTP/1.1 ", 9) || !end || !cl || cl > end ||
	    strtoul(cl + 18, NULL, 10) != c->send.len - (size_t)(end + 4 - t))
		goto out;
	status = atoi(t + 9);
	*body = strdup(end + 4);
out:
	free(t);
	return status;
}

static void test_value_replies(void)
{
	static const struct prov big = { "big", "Test", "a long value", big_read, NULL, 0 };
	static const struct prov broken = { "broken", "Test", "half a value", broken_read,
					    NULL, 0 };
	struct mg_connection *c = conn_new();
	char *body = NULL;
	int status;

	puts("the values as replies");
	if (!c)
		return;

	g_big_len = 300 * 1024;
	value_get(c, &big, NULL, 0);
	status = reply_of(c, &body);
	check(status == 200 && body && strlen(body) == g_big_len + 21 &&
		      !strncmp(body, "{\"id\":\"big\",\"value\":\"xxx", 24) &&
		      !strcmp(body + strlen(body) - 3, "x\"}"),
	      "a value larger than a buffer of before is served whole, its length as it is");
	check(c->send.size < 2 * c->send.len,
	      "and its send buffer is no more than twice what it holds");
	free(body);
	c->send.len = 0;
	value_sent(c);
	check(!c->send.buf && !c->send.size,
	      "once the reply has gone, its send buffer goes back");

	g_big_len = VALUE_MAX;
	value_get(c, &big, NULL, 0);
	status = reply_of(c, &body);
	check(status == 200 && body && strlen(body) == VALUE_MAX + 21,
	      "a value of the most a value may be is served whole");
	free(body);
	c->send.len = 0;
	value_sent(c);

	g_big_len = VALUE_MAX + 1;
	value_get(c, &big, NULL, 0);
	status = reply_of(c, &body);
	check(status == 500 && body && strstr(body, "{\"error\":\"big\",") &&
		      strstr(body, "\"detail\":\"the value is larger than 1310720 bytes\"}"),
	      "one byte more is an error that says so, not a value cut short");
	free(body);
	c->send.len = 0;
	value_sent(c);

	value_get(c, &broken, NULL, 0);
	status = reply_of(c, &body);
	check(status == 500 && body && !strstr(body, "half") && strstr(body, "\"error\":\"broken\""),
	      "a value that cannot be read is an error, and none of what it wrote goes");
	free(body);
	c->send.len = 0;

	mg_http_reply(c, 200, "", "%s", "{}");
	body = strndup((char *)c->send.buf, c->send.len);
	value_get(c, &big, NULL, 0);
	check(body && c->send.len > strlen(body) &&
		      !memcmp(c->send.buf, body, strlen(body)) &&
		      !memcmp(c->send.buf + strlen(body), "HTTP/1.1 500", 12),
	      "a reply goes after one that waits to be sent, which it leaves as it was");
	free(body);
	c->send.len = 0;

	value_getm(c, mg_str("uptime,omci,no_such_item"));
	status = reply_of(c, &body);
	check(status == 200 && body && well_formed(body) &&
		      strstr(body, "{\"uptime\":{") && strstr(body, "\"up_s\":832.34") &&
		      strstr(body, "\"omci\":{\"error\":\"slow: ask for it alone\"}") &&
		      strstr(body, "\"no_such_item\":null}"),
	      "several values are served together, a slow one and an unknown one in their place");
	free(body);
	c->send.len = 0;

	value_schema(c);
	status = reply_of(c, &body);
	check(status == 200 && body && well_formed(body) &&
		      strstr(body, "{\"id\":\"settings\",\"cat\":\"Mode\""),
	      "the list of values is served");
	free(body);
	conn_free(c);
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

/* The settings once onu-apply's stand-in has ended, waiting up to 5 s. */
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
 * onu-apply's stand-in notes that it ran, then waits for run/release, so that
 * the tests see a change that is still being applied.
 */
/*
 * onu-apply's stand-in: "state" is the real onu-apply's, which reads the
 * files of the scratch root; "try" puts the change onu-config change left in
 * /run/onu-pending in force first, as onu-apply does, then says it ran and
 * waits for the test's release.
 */
static const char apply_stub[] =
	"#!/bin/sh\n"
	"[ \"$1\" = state ] && exec \"$POND_REAL_ONU_APPLY\" state\n"
	"P=$POND_ROOT/run/onu-pending\n"
	"if [ \"$1\" = try ] && [ -d \"$P\" ]; then\n"
	"\tfor f in \"$P\"/onu-pppoe*; do\n"
	"\t\t[ -e \"$f\" ] || continue\n"
	"\t\tif [ -s \"$f\" ]; then mv \"$f\" \"$POND_ROOT/run/\";\n"
	"\t\telse rm -f \"$f\" \"$POND_ROOT/run/${f##*/}\"; fi\n"
	"\tdone\n"
	"\tmv \"$P/onu.conf\" \"$POND_ROOT/run/onu.conf\"\n"
	"\trm -rf \"$P\"\n"
	"fi\n"
	"echo \"$1\" >>\"$POND_ROOT/run/applied\"\n"
	"i=0\n"
	"while [ ! -e \"$POND_ROOT/run/release\" ] && [ $i -lt 500 ]; do\n"
	"\tsleep 0.01\n"
	"\ti=$((i + 1))\n"
	"done\n";

/* Until onu-apply's stand-in has put the pending change in force. */
static void wait_taken(void)
{
	char path[512];
	struct stat st;

	prov_path("/run/onu-pending", path, sizeof(path));
	for (int i = 0; i < 500 && !stat(path, &st); i++)
		pause_10ms();
}

static void test_settings(void)
{
	static const char *const bad[] = {
		"FOO=1",
		"WAN_VLAN=0",
		"WAN_VLAN=4095",
		"WAN_GEM=4096",
		"WAN_GEM=012",
		"WAN_MAC=01:00:5e:00:00:01",
		"WAN_MAC=00:00:00:00:00:00",
		"WAN_MAC=00:00:5e:00:53",
		"WAN_MAC=00-00-5e-00-53-01",
		"SESSION1_PASS=yes",
		"SESSION1_HOST_VLAN=4095",
		"SESSION1_HOST_VLAN=none",
		"PPPOE_USER=two words",
		"PPPOE_SECRET=",
		"a line without an equals sign",
	};
	char buf[4096], file[512], err[256], what[128], stub[512];
	const char *s;
	struct stat st;
	int rc;

	puts("the settings of the data path, and changes to them");

	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);

	s = settled(buf, sizeof(buf));
	check(strstr(s, "\"sessions\":[]") && strstr(s, "\"mode\":null"),
	      "without a file, no session, and nothing brought up yet");

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		rc = prov_settings_set(bad[i], err, sizeof(err));
		snprintf(what, sizeof(what), "refused: %s", bad[i]);
		check(rc == -EINVAL && *err, what);
	}
	rc = prov_settings_set("PPPOE2_USER=bench\n", err, sizeof(err));
	check(rc == -EINVAL && strstr(err, "PPPOE2_SECRET"),
	      "an account without a secret is refused");
	rc = prov_settings_set("SESSION8_VLAN=410\n", err, sizeof(err));
	check(rc == -EINVAL, "a session past the seventh is refused");
	rc = prov_settings_set("SESSION1_FAMILY=ipx\n", err, sizeof(err));
	check(rc == -EINVAL, "a family that is not one is refused");
	check(!slurp("/run/onu.conf", file, sizeof(file)) &&
		      !slurp("/run/applied", file, sizeof(file)),
	      "a refused change writes nothing and applies nothing");

	rc = prov_settings_set("PPPOE_USER=bench\r\nPPPOE_SECRET=not-a-real-one\r\n"
			       "SESSION1_VLAN=410\r\nSESSION1_FAMILY=ipv4\r\n",
			       err, sizeof(err));
	check(rc == 0, "a whole change, with the line ends of a form, is taken");
	wait_taken();
	s = slurp("/run/onu.conf", file, sizeof(file));
	check(s && strstr(s, "\nSESSION1_VLAN=410\n") && strstr(s, "\nSESSION1_FAMILY=ipv4\n") &&
		      strstr(s, "\nSESSION1_PASS=on\n"),
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
	check(s && !strcmp(s, "try\n"), "the change starts onu-apply try: it is on trial");

	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"applying\":true") != NULL,
	      "the settings say a change is being applied");
	check(strstr(s, "\"sessions\":[{\"id\":1,\"user\":\"bench\",\"secret_set\":true,") != NULL,
	      "they give the session's user and that a secret is set");
	check(!strstr(s, "not-a-real-one"), "but never the secret");
	rc = prov_settings_set("SESSION1_VLAN=36", err, sizeof(err));
	check(rc == -EBUSY, "a change while one is being applied is refused");

	put("/run/release", "", 0644);
	s = settled(buf, sizeof(buf));
	check(strstr(s, "\"applying\":false") &&
		      strstr(s, "\"last_apply_status\":0"),
	      "once onu-apply has ended, the settings say how it ended");

	rc = prov_settings_set("SESSION1_HOST_VLAN=36\n", err, sizeof(err));
	check(rc == 0, "a change of a few settings is taken");
	wait_taken();
	s = slurp("/run/onu.conf", file, sizeof(file));
	check(s && strstr(s, "\nSESSION1_VLAN=410\n") && strstr(s, "\nSESSION1_HOST_VLAN=36\n"),
	      "and merged with the settings before it");
	s = slurp("/run/onu-pppoe1", file, sizeof(file));
	check(s && !strcmp(s, "bench not-a-real-one\n"),
	      "a change without PPPoE leaves the PPPoE file as it was");
	settled(buf, sizeof(buf));

	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"trials\":[]") != NULL, "no change on trial without /run/onu-trial.d");
	mkdir(prov_path("/run/onu-trial.d", file, sizeof(file)), 0755);
	put("/run/onu-trial.d/settings", "UNTIL=1264\nTOKEN=99\n", 0644);
	put("/run/onu-trial.d/mgmt",
	    "VLAN=100\nADDRESS=192.0.2.10/30\nIFACE=mgmt100\nUNTIL=1800\nTOKEN=98\n", 0644);
	put("/run/onu-trial.d/settings.new", "UNTIL=1\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"trials\":[{\"name\":\"settings\",\"until_s\":1264},"
			"{\"name\":\"mgmt\",\"until_s\":1800,\"address\":\"192.0.2.10/30\"}]") != NULL,
	      "the settings tell each change on trial, when it goes back, and the pair's address");
	rc = prov_settings_set("CONFIRM=1\n", err, sizeof(err));
	check(rc == -EINVAL, "a confirmation is no setting: /api/set/changes takes it");
	unlink(prov_path("/run/onu-trial.d/settings", file, sizeof(file)));
	unlink(prov_path("/run/onu-trial.d/settings.new", file, sizeof(file)));
	unlink(prov_path("/run/onu-trial.d/mgmt", file, sizeof(file)));

	rc = prov_settings_set("SESSION1_MAC=00:00:5e:00:53:01\nPPPOE2_USER=b\nPPPOE2_SECRET=x\n"
			       "SESSION2_VLAN=410\nSESSION2_MAC=00:00:5e:00:53:02\n",
			       err, sizeof(err));
	check(rc == -EINVAL && strstr(err, "one MAC address"),
	      "two sessions in one VLAN that send from two MAC addresses are refused");
	rc = prov_settings_set("PASS_GATEWAY=169.254.0.1\n", err, sizeof(err));
	check(rc == -EINVAL && strstr(err, "PASS_GATEWAY"),
	      "the stick's address on the link to the router is no setting any more");
	rc = prov_settings_set("MODE=router\nWAN_PROTO=pppoe\nWAN_MAC=00:00:5e:00:53:01\n",
			       err, sizeof(err));
	check(rc == 0, "the keys of the bench's router of before are taken");
	wait_taken();
	s = slurp("/run/onu.conf", file, sizeof(file));
	check(s && strstr(s, "\nSESSION1_MAC=00:00:5e:00:53:01\n") && !strstr(s, "MODE") &&
		      !strstr(s, "WAN_") && !strstr(s, "LAN_VLAN") && !strstr(s, "PASS_GATEWAY"),
	      "onu.conf has the WAN's MAC address of before as the first session's, "
	      "and none of the router's keys");
	settled(buf, sizeof(buf));

	put("/run/onu-mode", "passthrough\n", 0644);
	put("/run/onu-dns", "198.51.100.53\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"mode\":\"passthrough\"") &&
		      strstr(s, "\"dns\":\"198.51.100.53\"") && strstr(s, "\"pass\":null"),
	      "the settings carry what onu-apply brought up");
	put("/run/onu-pass", "192.0.2.2\n", 0644);
	put("/run/onu-lan", "lan\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"pass\":{\"address\":\"192.0.2.2\",\"lan\":\"lan\","
			"\"peer_gateway\":null}") != NULL,
	      "and the address a session handed to the router");
	put("/run/onu-pass-proxy", "192.0.2.1\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"peer_gateway\":\"192.0.2.1\"}") != NULL,
	      "with the concentrator's address as its gateway");
	mkdir(prov_path("/run/session", file, sizeof(file)), 0755);
	mkdir(prov_path("/run/session/1", file, sizeof(file)), 0755);
	put("/run/session/1/dev", "wan410\n", 0644);
	put("/run/session/1/gem", "gem1\n", 0644);
	/* What the session brought is netifd's (bus-file.c reads it here). */
	mkdir(prov_path("/run/ubus", file, sizeof(file)), 0755);
	put("/run/ubus/network.interface.ses1",
	    "up=true\nl3_device=ppp0\ndata.address=192.0.2.2\ndata.peer=192.0.2.1\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"hands_on\":false,\"dev\":\"wan410\"") != NULL,
	      "a session hands nothing down until onu-apply says it does");
	put("/run/session/1/hands", "ipv4 ipv6\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"hands_on\":true,\"dev\":\"wan410\",\"gem_dev\":\"gem1\","
			"\"ppp\":\"ppp0\",\"ipv4\":\"192.0.2.2\",\"ipv6\":null,"
			"\"prefix\":null,\"prefix_host\":null,\"lan\":null,"
			"\"gateway\":null,\"dns\":null}") != NULL,
	      "a session carries what onu-apply brought up for it");
	put("/run/session/1/lan", "lan410\n", 0644);
	put("/run/session/1/gateway", "192.0.2.1\n", 0644);
	put("/run/ubus/network.interface.ses1",
	    "l3_device=ppp0\ndata.address=192.0.2.2\n"
	    "dns-server=198.51.100.53 198.51.100.54\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"lan\":\"lan410\",\"gateway\":\"192.0.2.1\","
			"\"dns\":\"198.51.100.53 198.51.100.54\"}") != NULL,
	      "and where it hands its address down, with the gateway and name servers");
	put("/run/ubus/dhcp6d.ses1",
	    "name=ses1\nprefix=2001:db8:1200::/56\nhost=fe80::2\nbound=true\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"prefix\":\"2001:db8:1200::/56\",\"prefix_host\":\"fe80::2\"") != NULL,
	      "and the IPv6 prefix dhcp6d took for it, and the host it went to");
	check(strstr(s, "\"store\":null") != NULL,
	      "without a word from onu-config, the store's state is unknown");
	put("/run/onu-store", "saved\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"store\":\"saved\"") != NULL,
	      "and with one, what became of the settings in the store");

	setenv("POND_ONU_APPLY", "/nonexistent/onu-apply", 1);
	rc = prov_settings_set("SESSION1_PASS=off", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && strstr(s, "\"last_apply_status\":127"),
	      "an onu-apply that cannot run is reported, not hidden");
	unsetenv("POND_ONU_APPLY");
}

/* onu-config's stand-in: the identity it is given goes to a file. */
static const char store_stub[] =
	"#!/bin/sh\n[ \"$1\" = identity ] && cat > \"$POND_ROOT/stored-identity\"\n";

static bool file_has(const char *path, const char *text)
{
	char buf[2048];

	return !prov_slurp(path, buf, sizeof(buf), NULL) && strstr(buf, text) != NULL;
}

static void test_panel_values(void)
{
	char buf[4096], file[512], err[256], stub[512], onu_config[512];
	const char *s;
	struct stat st;
	int rc, e;

	puts("the panel's identity, account and link");
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/run/onu-identity",
	    "serial=XXXXXXXXXXXX\nvendor_id=XXXX\nversion=V1.0\n"
	    "ploam_password=0123456789\nmodel=EXAMPLE\nloid=example-line\n"
	    "loid_password=lpw00000\nsw_version0=V1.0.0\n", 0600);
	put("/run/onu-identity-source", "store\n", 0644);
	s = render(prov_identity, buf, sizeof(buf), &e);
	check(!e && strstr(s, "\"serial\":\"XXXXXXXXXXXX\"") && strstr(s, "\"ploam_set\":true") &&
		      strstr(s, "\"source\":\"store\""),
	      "the identity is served, and whether a PLOAM password is set");
	check(!strstr(s, "0123456789"), "but never the PLOAM password itself");
	check(strstr(s, "\"loid\":\"example-line\"") &&
		      strstr(s, "\"loid_password_set\":true") && !strstr(s, "lpw00000"),
	      "the logical ONU ID is served, its password only as set or not");
	check(strstr(s, "\"sw_version0\":\"V1.0.0\"") && strstr(s, "\"sw_version1\":\"\""),
	      "each software image's version is a field of its own");

	/* onu-config is the stand-in for the identity alone. */
	snprintf(onu_config, sizeof(onu_config), "%s", getenv("POND_ONU_CONFIG") ?: "");
	put("/store-stub", store_stub, 0755);
	setenv("POND_ONU_CONFIG", prov_path("/store-stub", stub, sizeof(stub)), 1);
	/* onu-apply's stand-in takes the pending change, and waits for nothing. */
	put("/apply-stub", apply_stub, 0755);
	put("/run/release", "", 0644);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);
	check(prov_identity_set("SERIAL=XXXX", err, sizeof(err)) == -EINVAL,
	      "a serial number of the wrong form is refused");
	check(prov_identity_set("MODEL=a=b", err, sizeof(err)) == -EINVAL,
	      "a value with '=' is refused");
	check(prov_identity_set("PLOAM=not-a-ploam-password", err, sizeof(err)) == -EINVAL,
	      "a PLOAM password of the wrong form is refused");
	check(prov_identity_set("COLOUR=red", err, sizeof(err)) == -EINVAL,
	      "a field the identity lacks is refused");
	check(prov_identity_set("TM_OPTION=3", err, sizeof(err)) == -EINVAL &&
		      prov_identity_set("VENDOR_PRODUCT_CODE=65536", err, sizeof(err)) == -EINVAL &&
		      prov_identity_set("OMCC_VERSION=-1", err, sizeof(err)) == -EINVAL,
	      "a number beyond its attribute is refused");
	check(prov_identity_set("LOID_PASSWORD=thirteen-long", err, sizeof(err)) == -EINVAL,
	      "a logical password longer than twelve characters is refused");
	rc = prov_identity_set("SERIAL=000000000000\nPLOAM=\nVERSION=V2.0\n", err, sizeof(err));
	check(rc == 0 && file_has("/stored-identity", "SERIAL=000000000000\nVERSION=V2.0\n") &&
		      !file_has("/stored-identity", "PLOAM"),
	      "a good identity goes to onu-config, an empty PLOAM keeping the one there is");
	rc = prov_identity_set("LOID=example7\nLOID_PASSWORD=\nOMCC_VERSION=0xA3\nTM_OPTION=2\n",
			       err, sizeof(err));
	check(rc == 0 &&
		      file_has("/stored-identity", "LOID=example7\nOMCC_VERSION=0xA3\nTM_OPTION=2\n"),
	      "a logical ONU ID and numbers in hexadecimal go to onu-config, an empty "
	      "logical password keeping the one there is");
	setenv("POND_ONU_CONFIG", onu_config, 1);

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
	check(rc == 0 && !file_has("/run/onu.conf", "MODE=") &&
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
		      strstr(s, "\"family\":\"ipv6\",\"pass\":\"on\",\"host_vlan\":\"\","
				"\"hands_on\":false"),
	      "both are served, both hand down in their own VLAN, and the IPv6 one no IPv4");
	rc = prov_session_set("USER=third@example.net\nSECRET=example-secret\nVLAN=410\n"
			      "FAMILY=ipv6\n",
			      err, sizeof(err));
	check(rc == -EINVAL && strstr(err, "sessions 2 and 3 both hand an IPv6 prefix down"),
	      "a second IPv6 prefix handed down in one VLAN to the host is refused");
	rc = prov_session_set("USER=third@example.net\nSECRET=example-secret\nVLAN=410\n"
			      "FAMILY=ipv6\nPASS=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "SESSION3_PASS=off"),
	      "a session that keeps its prefix on the stick is taken");
	rc = prov_session_set("ID=3\nPASS=on\nHOST_VLAN=37\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "SESSION3_PASS=on\nSESSION3_HOST_VLAN=37") &&
		      strstr(s, "\"pass\":\"on\",\"host_vlan\":\"37\""),
	      "and so is one that hands it down in a VLAN of its own");
	check(prov_session_set("ID=3\nHOST_VLAN=4095\n", err, sizeof(err)) == -EINVAL &&
		      prov_session_set("ID=3\nPASS=maybe\n", err, sizeof(err)) == -EINVAL,
	      "a VLAN to the host out of range, and PASS other than on or off, are refused");
	rc = prov_session_set("ID=3\nHOST_VLAN=untagged\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "SESSION3_HOST_VLAN=untagged"),
	      "the host link itself, without a VLAN, is taken");
	rc = prov_session_set("ID=3\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
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

	/* The one link to the router of before becomes the sessions' own. */
	put("/run/onu.conf", "MODE=\nLAN_VLAN=36\nPASS_GATEWAY=169.254.0.1\nPASS_PEER=on\n"
			     "SESSION1_VLAN=410\nSESSION1_FAMILY=ipv4\n",
	    0644);
	put("/run/onu-pppoe1", "user@example.net example-secret\n", 0600);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"family\":\"ipv4\",\"pass\":\"on\",\"host_vlan\":\"36\"") != NULL,
	      "the link of before, LAN_VLAN, is the VLAN a session hands down in");
	rc = prov_session_set("ID=1\nMAC=00:00:5e:00:53:01\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "SESSION1_HOST_VLAN=36") &&
		      !file_has("/run/onu.conf", "LAN_VLAN") &&
		      !file_has("/run/onu.conf", "PASS_GATEWAY") &&
		      !file_has("/run/onu.conf", "PASS_PEER"),
	      "and a change writes it as the session's own, without the link's settings");
	put("/run/onu.conf", "MODE=\nLAN_VLAN=\nSESSION1_VLAN=410\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"host_vlan\":\"untagged\"") != NULL,
	      "an empty LAN_VLAN of before is the host link itself");
	rc = prov_session_set("ID=1\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	(void)s;
	unsetenv("POND_ONU_APPLY");
}

/*
 * A lease as udhcpd writes it: seconds left, address, MAC, name, padding. The
 * MAC of device @last is 00:00:5e:00:53:aN, N being @last, of the documentation
 * range of RFC 7042.
 */
static void lease_at(unsigned char *b, uint32_t left, const char *ip4, unsigned int last,
		     const char *name)
{
	memset(b, 0, 36);
	b[0] = left >> 24;
	b[1] = left >> 16;
	b[2] = left >> 8;
	b[3] = left;
	memcpy(b + 4, ip4, 4);
	memcpy(b + 8, "\x00\x00\x5e\x00\x53", 5);
	b[13] = 0xa0 | (last & 0x0f);
	snprintf((char *)b + 14, 20, "%s", name);
}

static void test_lans(void)
{
	static char buf[16384], lbuf[4096];
	unsigned char leases[8 + 2 * 36];
	char file[512], err[256], stub[512];
	uint64_t written = (uint64_t)time(NULL) - 10;
	const char *s;
	FILE *f;
	int rc;

	puts("the local networks, their fixed addresses and devices");
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	put("/run/release", "", 0644);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/run/onu-pppoe%d", n);
		unlink(prov_path(path, file, sizeof(file)));
	}
	rc = prov_session_set("USER=user@example.net\nSECRET=example-secret\nVLAN=410\nPASS=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && strstr(s, "\"lans\":[]") && strstr(s, "\"fixed\":[]"),
	      "a stick without networks serves them as empty lists: the panel's pages are there");
	check(!strstr(s, "\"leases\""), "and no leases among the settings, which the panel polls");
	s = render(prov_leases, lbuf, sizeof(lbuf), &rc);
	check(rc == 0 && !strcmp(s, "[]"), "the leases are a value of their own, an empty list");

	check(prov_lan_set("VLAN=100\n", err, sizeof(err)) == -EINVAL && strstr(err, "needs CONN"),
	      "a new network needs a connection");
	rc = prov_lan_set("CONN=pppoe:1\nNAME=Dom\nVLAN=100\nADDRESS=192.168.1.1/24\n"
			  "POOL_FROM=192.168.1.100\nPOOL_TO=192.168.1.199\nLEASE=3600\n",
			  err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "LAN1_CONN=pppoe:1") &&
		      strstr(s, "\"lans\":[{\"id\":1,\"conn\":\"pppoe:1\",\"name\":\"Dom\","
				"\"vlan\":\"100\",\"address\":\"192.168.1.1/24\",\"dhcp\":\"on\","
				"\"pool_from\":\"192.168.1.100\",\"pool_to\":\"192.168.1.199\","
				"\"lease_s\":3600,\"ipv6\":\"off\",\"ipv6_subnet\":\"\"}]"),
	      "a network is taken, and served as the panel reads it");
	check(!strstr(s, "\"LAN1_CONN\""), "and not again among the other settings");
	check(prov_lan_set("CONN=pppoe:1\nVLAN=100\nADDRESS=10.0.0.1/24\n", err, sizeof(err)) ==
			      -EINVAL &&
		      strstr(err, "networks 1 and 2 are in one VLAN"),
	      "a second network in its VLAN is refused, as onu-config judges it");

	rc = prov_fixed_set("LAN=1\nMAC=00:00:5E:00:53:A1\nIP=192.168.1.10\nNAME=Drukarka\n",
			    err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a1/192.168.1.10") &&
		      strstr(s, "\"fixed\":[{\"lan\":1,\"mac\":\"00:00:5e:00:53:a1\","
				"\"ip\":\"192.168.1.10\",\"name\":\"Drukarka\"}]"),
	      "a fixed address is taken, its MAC in small letters");
	rc = prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a1\nIP=192.168.1.11\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a1/192.168.1.11") &&
		      !file_has("/run/onu.conf", "FIXED2"),
	      "a device's new address replaces the one it had");
	check(prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a2\nIP=192.168.1.11\n", err, sizeof(err)) ==
			      -EINVAL &&
		      strstr(err, "give one address"),
	      "another device's address is refused");
	check(prov_fixed_set("MAC=00:00:5e:00:53:a2\nIP=192.168.1.12\n", err, sizeof(err)) == -EINVAL,
	      "a fixed address without a network is refused");

	/* Two leases, written ten seconds ago: one with an hour left, one with five seconds. */
	for (int i = 0; i < 8; i++)
		leases[i] = (unsigned char)(written >> (56 - 8 * i));
	lease_at(leases + 8, 3600, "\xc0\xa8\x01\x64", 0x03, "laptop");
	lease_at(leases + 8 + 36, 5, "\xc0\xa8\x01\x65", 0x04, "");
	f = fopen(prov_path("/run/udhcpd-net1.leases", file, sizeof(file)), "w");
	if (f) {
		fwrite(leases, 1, sizeof(leases), f);
		fclose(f);
	}
	s = render(prov_leases, lbuf, sizeof(lbuf), &rc);
	check(rc == 0 && s[0] == '[' &&
		      strstr(s, "{\"lan\":1,\"mac\":\"00:00:5e:00:53:a3\",\"ip\":\"192.168.1.100\","
				"\"name\":\"laptop\",\"left_s\":35") &&
		      !strstr(s, "192.168.1.101"),
	      "a device with a lease is served with the time it has left, and an expired one not");
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(rc == 0 && !strstr(s, "laptop") && !strstr(s, "\"leases\""),
	      "and not among the settings");

	rc = prov_lan_set("ID=1\nCONN=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "LAN1_") && !file_has("/run/onu.conf", "FIXED") &&
		      strstr(s, "\"lans\":[]") && strstr(s, "\"fixed\":[]"),
	      "a network removed takes its fixed addresses with it");
	s = render(prov_leases, lbuf, sizeof(lbuf), &rc);
	check(rc == 0 && !strcmp(s, "[]"),
	      "and its devices are no longer served, though udhcpd's file is still there");
	unlink(prov_path("/run/udhcpd-net1.leases", file, sizeof(file)));
	rc = prov_session_set("ID=1\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	(void)s;
	unsetenv("POND_ONU_APPLY");
}

static void test_ipoe(void)
{
	static char buf[16384];
	char file[512], err[256], stub[512];
	const char *s;
	int rc;

	puts("the connections that take their address as a router does");
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	put("/run/release", "", 0644);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/run/onu-pppoe%d", n);
		unlink(prov_path(path, file, sizeof(file)));
	}
	s = settled(buf, sizeof(buf));
	check(strstr(s, "\"ipoe\":[]") != NULL,
	      "a stick without connections serves them as an empty list: the panel's page is there");

	check(prov_ipoe_set("VLAN=420\n", err, sizeof(err)) == -EINVAL && strstr(err, "needs KIND"),
	      "a new connection needs a kind");
	rc = prov_ipoe_set("KIND=static\nVLAN=420\nGEM=auto\nMAC=\nFAMILY=ipv4\n"
			   "ADDRESS=192.0.2.10/24\nGATEWAY=192.0.2.1\n"
			   "DNS=198.51.100.53, 198.51.100.54\nADDRESS6=\nGATEWAY6=\nPREFIX6=\n",
			   err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "IPOE1_KIND=static") &&
		      file_has("/run/onu.conf", "IPOE1_DNS=198.51.100.53 198.51.100.54") &&
		      strstr(s, "\"ipoe\":[{\"id\":1,\"kind\":\"static\",\"vlan\":\"420\","
				"\"gem\":\"auto\",\"mac\":\"\",\"family\":\"ipv4\","
				"\"address\":\"192.0.2.10/24\",\"gateway\":\"192.0.2.1\","
				"\"dns\":\"198.51.100.53 198.51.100.54\",\"address6\":\"\","
				"\"gateway6\":\"\",\"prefix6\":\"\",\"dev\":null,\"gem_dev\":null,"
				"\"up\":false,\"ipv4\":null,\"router\":null,\"dns_now\":null,"
				"\"ipv6\":null,\"prefix\":null,\"lease\":null}]"),
	      "a static connection is taken, its name servers one blank apart, and served as the panel reads it");
	check(!strstr(s, "\"IPOE1_KIND\""), "and not again among the other settings");

	/* What onu-apply brought up for it, as it says in its state. */
	mkdir(prov_path("/run/ipoe", file, sizeof(file)), 0755);
	mkdir(prov_path("/run/ipoe/1", file, sizeof(file)), 0755);
	put("/run/ipoe/1/dev", "wan420\n", 0644);
	put("/run/ipoe/1/gem", "gem256\n", 0644);
	put("/run/ipoe/1/ipv4", "192.0.2.10/24\n", 0644);
	put("/run/ipoe/1/router", "192.0.2.1\n", 0644);
	put("/run/ipoe/1/dns", "198.51.100.53 198.51.100.54\n", 0644);
	put("/run/ipoe/1/until", "999999999\n", 0644);
	s = render(prov_settings, buf, sizeof(buf), &rc);
	check(strstr(s, "\"dev\":\"wan420\",\"gem_dev\":\"gem256\",\"up\":true,"
			"\"ipv4\":\"192.0.2.10/24\",\"router\":\"192.0.2.1\","
			"\"dns_now\":\"198.51.100.53 198.51.100.54\",\"ipv6\":null,"
			"\"prefix\":null,\"lease\":") &&
		      !strstr(s, "\"lease\":null"),
	      "with what onu-apply brought up for it: the address in effect, and the lease's time");
	for (const char *const *f = (const char *const[]){ "dev", "gem", "ipv4", "router", "dns",
							     "until", NULL };
	     *f; f++) {
		char path[48];

		snprintf(path, sizeof(path), "/run/ipoe/1/%s", *f);
		unlink(prov_path(path, file, sizeof(file)));
	}
	check(prov_ipoe_set("KIND=dhcp\nVLAN=420\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "connections 1 and 2 are in one VLAN"),
	      "a second connection in its VLAN is refused, as onu-config judges it");
	check(prov_ipoe_set("ID=1\nGATEWAY=198.51.100.1\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "gateway"),
	      "a gateway out of its network is refused");

	rc = prov_ipoe_set("ID=1\nKIND=dhcp\nVLAN=420\nFAMILY=both\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "IPOE1_KIND=dhcp") &&
		      !file_has("/run/onu.conf", "192.0.2.10") &&
		      strstr(s, "\"kind\":\"dhcp\",\"vlan\":\"420\",\"gem\":\"auto\",\"mac\":\"\","
				"\"family\":\"both\",\"address\":\"\""),
	      "a connection by DHCP keeps none of the addresses typed in for it");

	rc = prov_lan_set("CONN=ipoe:1\nVLAN=20\nADDRESS=192.168.2.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "LAN1_CONN=ipoe:1") &&
		      strstr(s, "\"conn\":\"ipoe:1\""),
	      "a network goes out by a connection");
	rc = prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a1\nIP=192.168.2.10\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0, "a device of it has a fixed address");

	/* Six connections more: the seventh is the last one. */
	rc = prov_session_set("USER=user@example.net\nSECRET=example-secret\nVLAN=410\nPASS=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0, "a session beside the connection");
	for (int n = 2; n <= 6; n++) {
		char line[64];

		snprintf(line, sizeof(line), "KIND=dhcp\nVLAN=%d\n", 900 + n);
		rc |= prov_ipoe_set(line, err, sizeof(err));
		s = settled(buf, sizeof(buf));
	}
	check(rc == 0 && file_has("/run/onu.conf", "IPOE6_KIND=dhcp"),
	      "connections up to the seventh are taken");
	check(prov_ipoe_set("KIND=dhcp\nVLAN=999\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "all 7 connections are taken"),
	      "an eighth connection is refused");
	check(prov_session_set("USER=other@example.net\nSECRET=example-secret\nVLAN=36\nPASS=off\n",
			       err, sizeof(err)) == -EINVAL &&
		      strstr(err, "all 7 connections are taken"),
	      "and so is a session that would be the eighth");

	rc = prov_ipoe_set("ID=1\nKIND=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "IPOE1_") && !file_has("/run/onu.conf", "LAN1_") &&
		      !file_has("/run/onu.conf", "FIXED") && strstr(s, "\"lans\":[]"),
	      "a connection removed takes the networks that go out by it with it");

	rc = prov_lan_set("CONN=pppoe:1\nADDRESS=192.168.3.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "LAN1_CONN=pppoe:1"), "a network out by the session");
	rc = prov_session_set("ID=1\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "LAN1_") && strstr(s, "\"lans\":[]"),
	      "and so does a session removed");
	for (int n = 2; n <= 6; n++) {
		char line[32];

		snprintf(line, sizeof(line), "ID=%d\nKIND=\n", n);
		prov_ipoe_set(line, err, sizeof(err));
		s = settled(buf, sizeof(buf));
	}
	s = settled(buf, sizeof(buf));
	(void)s;
	unsetenv("POND_ONU_APPLY");
}

static void test_session_pass(void)
{
	static char buf[16384];
	char file[512], err[256], stub[512];
	const char *s;
	int rc;

	puts("a session that comes to hand its address down");
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	put("/run/release", "", 0644);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/run/onu-pppoe%d", n);
		unlink(prov_path(path, file, sizeof(file)));
	}
	rc = prov_session_set("USER=user@example.net\nSECRET=example-secret\nVLAN=410\nPASS=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_ipoe_set("KIND=dhcp\nVLAN=420\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_lan_set("CONN=pppoe:1\nVLAN=100\nADDRESS=192.168.1.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_lan_set("CONN=ipoe:1\nVLAN=20\nADDRESS=192.168.2.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a1\nIP=192.168.1.10\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_fixed_set("LAN=2\nMAC=00:00:5e:00:53:a2\nIP=192.168.2.10\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "LAN1_CONN=pppoe:1") &&
		      file_has("/run/onu.conf", "LAN2_CONN=ipoe:1") &&
		      file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a1/192.168.1.10") &&
		      file_has("/run/onu.conf", "FIXED2=2/00:00:5e:00:53:a2/192.168.2.10"),
	      "a network out by a session that keeps its address, one out by a connection, a fixed address in each");

	/* onu-config's own rule stays: the settings alone are refused. */
	check(prov_settings_set("SESSION1_PASS=on\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "network 1: session 1 hands its address down"),
	      "onu-config still refuses a network out by a session that hands its address down");
	check(file_has("/run/onu.conf", "SESSION1_PASS=off") &&
		      file_has("/run/onu.conf", "LAN1_CONN=pppoe:1"),
	      "and nothing of it is written");

	rc = prov_session_set("ID=1\nPASS=on\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "SESSION1_PASS=on") &&
		      file_has("/run/onu-pppoe1", "user@example.net") &&
		      !file_has("/run/onu.conf", "LAN1_") &&
		      !file_has("/run/onu.conf", "00:00:5e:00:53:a1") &&
		      !strstr(s, "\"conn\":\"pppoe:1\"") && !strstr(s, "00:00:5e:00:53:a1"),
	      "PASS=on takes the session's networks and their fixed addresses away in the same write");
	check(file_has("/run/onu.conf", "LAN2_CONN=ipoe:1") &&
		      file_has("/run/onu.conf", "FIXED2=2/00:00:5e:00:53:a2/192.168.2.10") &&
		      strstr(s, "\"conn\":\"ipoe:1\"") && strstr(s, "00:00:5e:00:53:a2"),
	      "and leaves the other connection's network and its fixed address");

	/* A write that keeps the session handing down drops nothing more. */
	rc = prov_session_set("ID=1\nVLAN=36\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "SESSION1_VLAN=36") &&
		      file_has("/run/onu.conf", "LAN2_CONN=ipoe:1"),
	      "a change of a session that hands its address down leaves the other networks");
	/* Kept on the stick again, the session takes a network once more. */
	rc = prov_session_set("ID=1\nPASS=off\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_lan_set("CONN=pppoe:1\nVLAN=100\nADDRESS=192.168.1.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "LAN1_CONN=pppoe:1"),
	      "a session that keeps its address has a network again");
	rc = prov_session_set("ID=1\nVLAN=410\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "LAN1_CONN=pppoe:1"),
	      "and a change that leaves PASS off keeps it");

	prov_session_set("ID=1\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	prov_ipoe_set("ID=1\nKIND=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	(void)s;
	unsetenv("POND_ONU_APPLY");
}

static void test_forwards(void)
{
	static char buf[16384];
	unsigned char leases[8 + 2 * 36];
	char file[512], err[256], stub[512];
	uint64_t written = (uint64_t)time(NULL) - 10;
	const char *s;
	FILE *f;
	int rc;

	puts("the ports forwarded and the DMZs");
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	put("/run/release", "", 0644);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/run/onu-pppoe%d", n);
		unlink(prov_path(path, file, sizeof(file)));
	}
	rc = prov_session_set("USER=user@example.net\nSECRET=example-secret\nVLAN=410\nPASS=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_lan_set("CONN=pppoe:1\nVLAN=100\nADDRESS=192.168.1.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && strstr(s, "\"forwards\":[]") && strstr(s, "\"dmz\":[]"),
	      "a stick without forwards serves them as empty lists: the panel's page is there");

	/* A device with an hour of its lease left, and one whose lease ran out. */
	for (int i = 0; i < 8; i++)
		leases[i] = (unsigned char)(written >> (56 - 8 * i));
	lease_at(leases + 8, 3600, "\xc0\xa8\x01\x64", 0x03, "nas");
	lease_at(leases + 8 + 36, 5, "\xc0\xa8\x01\x65", 0x04, "");
	f = fopen(prov_path("/run/udhcpd-net1.leases", file, sizeof(file)), "w");
	if (f) {
		fwrite(leases, 1, sizeof(leases), f);
		fclose(f);
	}

	rc = prov_forward_set("NAME=Serwer WWW\nCONN=pppoe:1\nPROTO=tcp\nEXT_FROM=443\nEXT_TO=443\n"
			      "LAN=1\nMAC=00:00:5E:00:53:A3\nINT_PORT=8443\nON=on\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 &&
		      file_has("/run/onu.conf",
			       "FORWARD1=pppoe:1/tcp/443/1/00:00:5e:00:53:a3/8443/on\n"
			       "FORWARD1_NAME=Serwer WWW\n") &&
		      strstr(s, "\"forwards\":[{\"id\":1,\"name\":\"Serwer WWW\",\"conn\":\"pppoe:1\","
				"\"proto\":\"tcp\",\"ext_from\":443,\"ext_to\":443,\"lan\":1,"
				"\"mac\":\"00:00:5e:00:53:a3\",\"int_port\":8443,\"on\":true}]"),
	      "a forward is taken, its MAC in small letters, and served as the panel reads it");
	check(file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a3/192.168.1.100") &&
		      strstr(s, "\"fixed\":[{\"lan\":1,\"mac\":\"00:00:5e:00:53:a3\","
				"\"ip\":\"192.168.1.100\""),
	      "its device keeps the address of its lease, fixed in the same write");
	check(!strstr(s, "\"FORWARD1\"") && !strstr(s, "\"FORWARD1_NAME\""),
	      "and the forward is not again among the other settings");

	rc = prov_forward_set("CONN=pppoe:1\nPROTO=both\nEXT_FROM=6000\nEXT_TO=6010\nLAN=1\n"
			      "MAC=00:00:5e:00:53:a3\nON=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 &&
		      file_has("/run/onu.conf",
			       "FORWARD2=pppoe:1/both/6000-6010/1/00:00:5e:00:53:a3/6000/off") &&
		      !file_has("/run/onu.conf", "FIXED2") &&
		      strstr(s, "\"id\":2,\"name\":\"\",\"conn\":\"pppoe:1\",\"proto\":\"both\","
				"\"ext_from\":6000,\"ext_to\":6010,\"lan\":1,"
				"\"mac\":\"00:00:5e:00:53:a3\",\"int_port\":6000,\"on\":false}"),
	      "a range of both protocols kept off takes the next number, to the same ports, the address fixed once");
	check(prov_forward_set("CONN=pppoe:1\nPROTO=udp\nEXT_FROM=6005\nLAN=1\n"
			       "MAC=00:00:5e:00:53:a3\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "forwards 2 and 3 take the same ports of pppoe:1"),
	      "a port another forward of the connection takes is refused, as onu-config judges it");
	rc = prov_forward_set("CONN=pppoe:1\nEXT_FROM=7000\nEXT_TO=7010\nINT_PORT=8000\nLAN=1\n"
			      "MAC=00:00:5e:00:53:a3\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 &&
		      file_has("/run/onu.conf",
			       "FORWARD3=pppoe:1/tcp/7000-7010/1/00:00:5e:00:53:a3/8000/on") &&
		      strstr(s, "\"ext_from\":7000,\"ext_to\":7010,\"lan\":1,"
				"\"mac\":\"00:00:5e:00:53:a3\",\"int_port\":8000,\"on\":true}"),
	      "a range moved to other ports is taken");
	rc = prov_forward_set("ID=3\nCONN=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "FORWARD3"), "and taken away");
	check(prov_forward_set("CONN=pppoe:1\nEXT_FROM=7000\nEXT_TO=8024\nINT_PORT=20000\nLAN=1\n"
			       "MAC=00:00:5e:00:53:a3\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "ranges moved to other ports move 1025 ports, more than 1024"),
	      "but one that moves more ports than a table holds is refused, as onu-config judges it");
	check(prov_forward_set("CONN=pppoe:1\nEXT_FROM=22\nLAN=1\nMAC=00:00:5e:00:53:a4\n", err,
			       sizeof(err)) == -EINVAL &&
		      strstr(err, "00:00:5e:00:53:a4 has no address in network 1 to keep"),
	      "a device whose lease ran out has no address to keep");
	check(prov_forward_set("CONN=pppoe:1\nEXT_FROM=22\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "a device is"),
	      "a forward without a device is refused");
	check(prov_forward_set("ID=97\nCONN=\n", err, sizeof(err)) == -EINVAL && strstr(err, "ID"),
	      "and one past the 96th");

	rc = prov_forward_set("ID=1\nNAME=WWW\nCONN=pppoe:1\nPROTO=tcp\nEXT_FROM=443\nEXT_TO=443\n"
			      "LAN=1\nMAC=00:00:5e:00:53:a3\nINT_PORT=443\nON=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 &&
		      file_has("/run/onu.conf",
			       "FORWARD1=pppoe:1/tcp/443/1/00:00:5e:00:53:a3/443/off\n"
			       "FORWARD1_NAME=WWW\n") &&
		      !file_has("/run/onu.conf", "FORWARD3"),
	      "a forward is changed by its ID");
	/* onu-config's own rule stays: the settings alone are refused. */
	check(prov_settings_set("FIXED1=\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "forward 1: the device 00:00:5e:00:53:a3 has no fixed address") &&
		      file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a3/192.168.1.100"),
	      "onu-config still refuses a forward whose device has no fixed address");
	/* A new address for the device keeps its forwards. */
	rc = prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a3\nIP=192.168.1.101\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a3/192.168.1.101") &&
		      file_has("/run/onu.conf", "FORWARD1=") && file_has("/run/onu.conf", "FORWARD2="),
	      "a fixed address changed keeps the forwards to its device");

	rc = prov_dmz_set("CONN=pppoe:1\nLAN=1\nMAC=00:00:5e:00:53:a3\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "DMZ1=pppoe:1/1/00:00:5e:00:53:a3") &&
		      strstr(s, "\"dmz\":[{\"conn\":\"pppoe:1\",\"lan\":1,"
				"\"mac\":\"00:00:5e:00:53:a3\"}]") &&
		      !strstr(s, "\"DMZ1\""),
	      "a connection's DMZ is taken, and served as the panel reads it");
	rc = prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a5\nIP=192.168.1.50\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_dmz_set("CONN=pppoe:1\nLAN=1\nMAC=00:00:5E:00:53:A5\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "DMZ1=pppoe:1/1/00:00:5e:00:53:a5") &&
		      !file_has("/run/onu.conf", "DMZ2") &&
		      !file_has("/run/onu.conf", "FIXED3"),
	      "another device replaces it, at the fixed address it has");
	check(prov_dmz_set("LAN=1\nMAC=00:00:5e:00:53:a5\n", err, sizeof(err)) == -EINVAL,
	      "a DMZ without a connection is refused");
	rc = prov_dmz_set("CONN=pppoe:1\nMAC=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "DMZ") && strstr(s, "\"dmz\":[]"),
	      "MAC= takes the DMZ away");

	rc = prov_forward_set("ID=2\nCONN=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "FORWARD2") &&
		      file_has("/run/onu.conf", "FORWARD1="),
	      "CONN= takes a forward away");

	/*
	 * A fixed address taken away takes the forwards to its device and the
	 * DMZ that is its device with it, and leaves another device's.
	 */
	rc = prov_dmz_set("CONN=pppoe:1\nLAN=1\nMAC=00:00:5e:00:53:a5\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_forward_set("NAME=Gra\nCONN=pppoe:1\nPROTO=udp\nEXT_FROM=5000\nLAN=1\n"
			       "MAC=00:00:5e:00:53:a5\n",
			       err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "FORWARD2=pppoe:1/udp/5000/1/00:00:5e:00:53:a5") &&
		      file_has("/run/onu.conf", "DMZ1=pppoe:1/1/00:00:5e:00:53:a5"),
	      "a device with a fixed address, a forward and the DMZ");
	rc = prov_fixed_set("LAN=1\nMAC=00:00:5E:00:53:A5\nIP=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "00:00:5e:00:53:a5") &&
		      !file_has("/run/onu.conf", "FORWARD2") && !file_has("/run/onu.conf", "DMZ") &&
		      !strstr(s, "00:00:5e:00:53:a5") && strstr(s, "\"dmz\":[]"),
	      "its fixed address taken away takes its forward and the DMZ in the same write");
	check(file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a3/192.168.1.101") &&
		      file_has("/run/onu.conf", "FORWARD1=pppoe:1/tcp/443/1/00:00:5e:00:53:a3"),
	      "and leaves another device's fixed address and forward");

	rc = prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a5\nIP=192.168.1.50\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_dmz_set("CONN=pppoe:1\nLAN=1\nMAC=00:00:5e:00:53:a5\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_lan_set("ID=1\nCONN=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "FORWARD") && !file_has("/run/onu.conf", "DMZ") &&
		      !file_has("/run/onu.conf", "FIXED") && strstr(s, "\"forwards\":[]") &&
		      strstr(s, "\"dmz\":[]"),
	      "a network removed takes its forwards and its DMZ with it");

	unlink(prov_path("/run/udhcpd-net1.leases", file, sizeof(file)));
	prov_session_set("ID=1\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	(void)s;
	unsetenv("POND_ONU_APPLY");
}

/* Whether the file @path holds @text, a file as large as the settings. */
static bool big_file_has(const char *path, const char *text)
{
	static char buf[65536];

	return !prov_slurp(path, buf, sizeof(buf), NULL) && strstr(buf, text) != NULL;
}

/* The rule of ID @id among the rules that @s serves, or NULL. */
static const char *rule_at(const char *s, int id)
{
	const char *rules = strstr(s, "\"rules\":["), *end = rules ? strchr(rules, ']') : NULL;
	char want[16];
	const char *at;

	snprintf(want, sizeof(want), "{\"id\":%d,", id);
	at = rules ? strstr(rules, want) : NULL;
	return at && at < end ? at : NULL;
}

static void test_rules(void)
{
	static char buf[65536], many[32768];
	unsigned char leases[8 + 36];
	char file[512], err[256], stub[512];
	uint64_t written = (uint64_t)time(NULL) - 10;
	const char *s, *a, *b;
	size_t at = 0;
	FILE *f;
	int rc;

	puts("the firewall's rules and its defaults");
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	put("/run/release", "", 0644);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/run/onu-pppoe%d", n);
		unlink(prov_path(path, file, sizeof(file)));
	}
	rc = prov_session_set("USER=user@example.net\nSECRET=example-secret\nVLAN=410\nPASS=off\n",
			      err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_lan_set("CONN=pppoe:1\nVLAN=100\nADDRESS=192.168.1.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_lan_set("CONN=pppoe:1\nVLAN=200\nADDRESS=192.168.2.1/24\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && strstr(s, "\"rules\":[]") &&
		      strstr(s, "\"fw\":{\"out\":\"allow\",\"between\":\"block\","
				"\"ping_in\":\"block\",\"ping_lans\":\"block\"}"),
	      "a stick without rules serves none, and the defaults: out allowed, between blocked, no ping answered");

	/* The printer of network 1, with an hour of its lease left. */
	for (int i = 0; i < 8; i++)
		leases[i] = (unsigned char)(written >> (56 - 8 * i));
	lease_at(leases + 8, 3600, "\xc0\xa8\x01\x64", 0x03, "printer");
	f = fopen(prov_path("/run/udhcpd-net1.leases", file, sizeof(file)), "w");
	if (f) {
		fwrite(leases, 1, sizeof(leases), f);
		fclose(f);
	}

	rc = prov_rule_set("NAME=Drukarka dla Biura\nACTION=allow\nFROM=lan:2\n"
			   "TO=dev:1:00:00:5E:00:53:A3\nPROTO=tcp\nPORTS=9100\nFAMILY=any\nON=on\n",
			   err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 &&
		      file_has("/run/onu.conf",
			       "RULE1=1_allow_lan:2_dev:1:00:00:5e:00:53:a3_tcp_9100_any_on\n"
			       "RULE1_NAME=Drukarka dla Biura\n") &&
		      strstr(s, "\"rules\":[{\"id\":1,\"name\":\"Drukarka dla Biura\",\"action\":\"allow\","
				"\"from\":\"lan:2\",\"to\":\"dev:1:00:00:5e:00:53:a3\",\"proto\":\"tcp\","
				"\"ports\":\"9100\",\"family\":\"any\",\"on\":true}]"),
	      "a rule is taken, its device's MAC in small letters, and served as the panel reads it");
	check(file_has("/run/onu.conf", "FIXED1=1/00:00:5e:00:53:a3/192.168.1.100") &&
		      !strstr(s, "\"RULE1\"") && !strstr(s, "\"FW_"),
	      "the device it goes to keeps the address of its lease, and the rule is not among the other settings");

	rc = prov_rule_set("ACTION=block\nFROM=lans\nTO=internet\nPROTO=tcp\nPORTS=25\n", err,
			   sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_rule_set("NAME=Biuro z zewn\xc4\x85trz\nACTION=allow\nFROM=internet:203.0.113.0/24\n"
			    "TO=lan:1\nPROTO=any\nPORTS=\nFAMILY=ipv4\nON=off\n",
			    err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "RULE2=2_block_lans_internet_tcp_25_any_on\n") &&
		      file_has("/run/onu.conf",
			       "RULE3=3_allow_internet:203.0.113.0/24_lan:1_any__ipv4_off\n"
			       "RULE3_NAME=Biuro z zewn\xc4\x85trz\n") &&
		      strstr(s, "\"id\":3,\"name\":\"Biuro z zewn\xc4\x85trz\",\"action\":\"allow\","
				"\"from\":\"internet:203.0.113.0/24\",\"to\":\"lan:1\",\"proto\":\"any\","
				"\"ports\":\"\",\"family\":\"ipv4\",\"on\":false}"),
	      "rules are added at the end, each with the next ID, the defaults filled in");

	rc = prov_rule_set("ID=3\nMOVE=up\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	a = rule_at(s, 3);
	b = rule_at(s, 2);
	check(rc == 0 && file_has("/run/onu.conf", "RULE2=3_allow_internet") &&
		      file_has("/run/onu.conf", "RULE3=2_block_lans") &&
		      file_has("/run/onu.conf", "RULE2_NAME=Biuro") && a && b && a < b,
	      "MOVE=up moves a rule one place, with its name and its ID");
	rc = prov_rule_set("ID=1\nMOVE=up\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_rule_set("ID=2\nMOVE=down\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "RULE1=1_allow") &&
		      file_has("/run/onu.conf", "RULE3=2_block"),
	      "and the first goes no higher, the last no lower");

	rc = prov_rule_set("ID=2\nNAME=Bez SMTP\nACTION=block\nFROM=dev:2:00:00:5e:00:53:a7\n"
			   "TO=internet\nPROTO=both\nPORTS=25-26\nFAMILY=any\nON=on\n",
			   err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 &&
		      file_has("/run/onu.conf",
			       "RULE3=2_block_dev:2:00:00:5e:00:53:a7_internet_both_25-26_any_on\n"
			       "RULE3_NAME=Bez SMTP\n") &&
		      !file_has("/run/onu.conf", "FIXED2"),
	      "a rule is changed by its ID, in its place; a device it comes from needs no fixed address");

	check(prov_rule_set("ACTION=allow\nFROM=internet\nTO=internet:10.0.0.0/8\n", err,
			    sizeof(err)) == -EINVAL &&
		      strstr(err, "rule 4: from the internet to the internet"),
	      "a rule from the internet to the internet is refused, as onu-config judges it");
	check(prov_rule_set("ACTION=allow\nFROM=lan:1\nTO=dev:1:00:00:5e:00:53:a3\n", err,
			    sizeof(err)) == -EINVAL &&
		      strstr(err, "within one network"),
	      "and one within one network");
	check(prov_rule_set("ACTION=allow\nFROM=lans\nTO=internet:192.0.2.0/24\nFAMILY=ipv6\n", err,
			    sizeof(err)) == -EINVAL &&
		      strstr(err, "an address of IPv4 in a rule of ipv6"),
	      "and one whose address is not of its family");
	check(prov_rule_set("ACTION=allow\nFROM=lans_x\nTO=internet\n", err, sizeof(err)) ==
			      -EINVAL &&
		      strstr(err, "FROM"),
	      "an end that would not stay one field is refused by pond");
	check(prov_rule_set("ACTION=pass\nFROM=lans\nTO=internet\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "ACTION"),
	      "and so is an action that is none");
	check(prov_rule_set("ID=97\nACTION=\n", err, sizeof(err)) == -EINVAL && strstr(err, "ID"),
	      "and an ID past the 96th");
	check(prov_rule_set("ACTION=allow\nFROM=lan:2\nTO=dev:1:00:00:5e:00:53:a9\n", err,
			    sizeof(err)) == -EINVAL &&
		      strstr(err, "00:00:5e:00:53:a9 has no address in network 1 to keep"),
	      "a device a rule goes to without an address to keep is refused");

	rc = prov_rule_set("ID=3\nACTION=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "RULE2=2_block_dev:2") &&
		      !file_has("/run/onu.conf", "RULE3") && !rule_at(s, 3),
	      "ACTION= takes a rule away, and the rules after it move up");
	rc = prov_rule_set("ACTION=allow\nFROM=lans\nTO=internet:2001:db8::/32\nPROTO=icmp\n", err,
			   sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "RULE3=3_allow_lans_internet:2001:db8::/32_icmp__any_on"),
	      "a new rule takes the ID after the largest");

	/* The defaults. */
	rc = prov_fw_set("OUT=block\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "FW_OUT=block") &&
		      strstr(s, "\"fw\":{\"out\":\"block\",\"between\":\"block\","
				"\"ping_in\":\"block\",\"ping_lans\":\"block\"}"),
	      "OUT=block blocks the networks' new connections to the internet where no rule decides");
	rc = prov_fw_set("BETWEEN=allow\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && strstr(s, "\"fw\":{\"out\":\"block\",\"between\":\"allow\","),
	      "BETWEEN=allow lets them reach each other, and OUT stays");
	check(prov_fw_set("OUT=maybe\n", err, sizeof(err)) == -EINVAL && strstr(err, "OUT"),
	      "a default that is none is refused");
	/* The pings, which the stick answers on the management VLAN whatever they say. */
	rc = prov_fw_set("PING_IN=allow\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "FW_PING_IN=allow") &&
		      strstr(s, "\"between\":\"allow\",\"ping_in\":\"allow\",\"ping_lans\":\"block\"}"),
	      "PING_IN=allow answers a ping from the internet, and the others stay");
	rc = prov_fw_set("PING_LANS=allow\nPING_IN=block\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && file_has("/run/onu.conf", "FW_PING_LANS=allow") &&
		      !file_has("/run/onu.conf", "FW_PING_IN") &&
		      strstr(s, "\"ping_in\":\"block\",\"ping_lans\":\"allow\"}"),
	      "PING_LANS=allow answers one from the local networks, and PING_IN=block none from the internet");
	check(prov_fw_set("PING_LANS=on\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "PING_LANS: allow or block"),
	      "a ping setting that is neither allow nor block is refused");
	rc = prov_fw_set("OUT=allow\nBETWEEN=block\nPING_IN=block\nPING_LANS=block\n", err,
			 sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "FW_") &&
		      strstr(s, "\"fw\":{\"out\":\"allow\",\"between\":\"block\","
				"\"ping_in\":\"block\",\"ping_lans\":\"block\"}"),
	      "the defaults again are not written");

	/* A fixed address taken away takes the rules that go to its device. */
	rc = prov_fixed_set("LAN=1\nMAC=00:00:5e:00:53:a3\nIP=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "dev:1:00:00:5e:00:53:a3") &&
		      file_has("/run/onu.conf", "RULE2=2_block_dev:2") && !rule_at(s, 1),
	      "a fixed address taken away takes the rules that go to its device");
	/* A network removed takes the rules that name it or a device of it. */
	rc = prov_lan_set("ID=2\nCONN=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && !file_has("/run/onu.conf", "dev:2") &&
		      file_has("/run/onu.conf", "_allow_lans_internet:2001:db8::/32") &&
		      strstr(s, "\"rules\":[{\"id\":3,"),
	      "a network removed takes the rules that name it or its devices");

	/* 96 rules, and no 97th. */
	for (int i = 1; i <= 96; i++)
		at += (size_t)snprintf(many + at, sizeof(many) - at,
				       "RULE%d=%d_block_lan:1_internet_tcp_%d_any_on\n", i, i, 1000 + i);
	rc = prov_settings_set(many, err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && rule_at(s, 96) && big_file_has("/run/onu.conf", "RULE96="),
	      "96 rules are taken");
	check(prov_rule_set("ACTION=allow\nFROM=lans\nTO=internet\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "all 96 rules are taken"),
	      "and a 97th is refused, as the panel's stand-in refuses it");
	rc = prov_rule_set("ID=1\nACTION=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	rc |= prov_rule_set("ACTION=allow\nFROM=lans\nTO=internet\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	check(rc == 0 && big_file_has("/run/onu.conf", "RULE96=1_allow_lans_internet_any__any_on") &&
		      big_file_has("/run/onu.conf", "RULE1=2_block"),
	      "one taken away makes room at the end, and the new rule takes the first free ID");

	unlink(prov_path("/run/udhcpd-net1.leases", file, sizeof(file)));
	prov_session_set("ID=1\nUSER=\n", err, sizeof(err));
	s = settled(buf, sizeof(buf));
	(void)s;
	unsetenv("POND_ONU_APPLY");
}

/* onu-update's stand-in: the status of a stick that runs bank 1 on trial. */
static const char update_stub[] =
	"#!/bin/sh\n"
	"[ \"$1\" = status ] || exit 2\n"
	"printf 'RUNNING=1\\nCOMMITTED=0\\nNEXT=0\\nTRIAL=1\\n'\n"
	"printf 'BANK0_STATE=valid\\nBANK0_VERSION=0123456789ab\\nBANK0_TIME=1759000000\\n'\n"
	"printf 'BANK0_KERNEL_SIZE=1900544\\nBANK0_ROOTFS_SIZE=2031616\\n'\n"
	"printf 'BANK1_STATE=valid\\nBANK1_VERSION=7c5fba548abc\\nBANK1_TIME=1759800000\\n'\n"
	"printf 'BANK1_KERNEL_SIZE=1900544\\nBANK1_ROOTFS_SIZE=2031616\\n'\n"
	"printf 'BANK1_KERNEL_USED=1712345\\nBANK1_ROOTFS_USED=1899954\\n'\n";

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
			"\"rootfs_size_b\":2031616,\"kernel_used_b\":1712345,"
			"\"rootfs_used_b\":1899954}") != NULL,
	      "each bank, with its version, time, sizes and what its images use");
	check(strstr(s, "\"rootfs_size_b\":2031616,\"kernel_used_b\":null,"
			"\"rootfs_used_b\":null}") != NULL,
	      "a bank whose use onu-update does not give has none");
	check(strstr(s, "\"update\":{\"state\":\"idle\",\"received_b\":0") != NULL,
	      "no update under way");

	setenv("POND_ONU_UPDATE", "/nonexistent/onu-update", 1);
	s = render(prov_firmware, buf, sizeof(buf), &err);
	check(err == -EIO, "an onu-update that cannot run is an error, not empty banks");
	unsetenv("POND_ONU_UPDATE");
}

/* Stand-ins for onu-lane, onu-mgmt, onu-config and hostname. */
static const char lane_stub[] =
	"#!/bin/sh\n"
	"echo \"$*\" >>\"$POND_ROOT/lane.log\"\n"
	"case $1 in\n"
	"status) printf 'CHOICE=auto\\nNOW=2500basex\\n'\n"
	"  [ -e \"$POND_ROOT/lane.trial\" ] &&"
	" printf 'TRIAL_MODE=1000basex\\nTRIAL_PREVIOUS=auto\\nTRIAL_UNTIL=1060\\n' ;;\n"
	"try) [ \"$2\" = bad ] && exit 2; [ -e \"$POND_ROOT/lane.trial\" ] &&"
	" { echo 'onu-lane: a trial runs already'; exit 1; } ;;\n"
	"esac\n"
	"exit 0\n";

static const char mgmt_stub[] =
	"#!/bin/sh\n"
	"printf 'VLAN=4094\\nADDRESS=198.51.100.2/30\\nIFACE=mgmt\\nDATA_ADDRESS=\\n'\n"
	"[ -e \"$POND_ROOT/mgmt.pending\" ] &&"
	" printf 'PENDING_VLAN=100\\nPENDING_ADDRESS=192.0.2.6/30\\nPENDING_UNTIL=1100\\n'\n"
	"exit 0\n";

static const char system_store_stub[] =
	"#!/bin/sh\n[ \"$1\" = system ] && cat >\"$POND_ROOT/stored-system\"\n";

static const char hostname_stub[] =
	"#!/bin/sh\necho \"$1\" >\"$POND_ROOT/hostname-set\"\n";

static void test_product(void);

static void test_system(void)
{
	char buf[2048], stub[512], err[256], path[512], onu_config[512];
	const char *s;
	int e;

	puts("the stick's name, its lane and its management pair");

	put("/lane-stub", lane_stub, 0755);
	setenv("POND_ONU_LANE", prov_path("/lane-stub", stub, sizeof(stub)), 1);
	s = render(prov_sfp_lane, buf, sizeof(buf), &e);
	check(!e && !strcmp(s, "{\"choice\":\"auto\",\"active\":\"2500basex\",\"trial\":null}"),
	      "the lane's setting and the one it runs in, no trial");
	put("/lane.trial", "", 0644);
	s = render(prov_sfp_lane, buf, sizeof(buf), &e);
	check(!e && strstr(s, "\"trial\":{\"mode\":\"1000basex\",\"previous\":\"auto\","
			      "\"until_s\":1060}") != NULL,
	      "a setting on trial, what it replaced and when it ends");
	check(prov_sfp_lane_set("MODE=1000basex-an\n", err, sizeof(err)) == -EBUSY &&
		      strstr(err, "a trial runs already") != NULL,
	      "a second trial is refused, with onu-lane's reason");
	unlink(prov_path("/lane.trial", path, sizeof(path)));
	check(prov_sfp_lane_set("MODE=bad\n", err, sizeof(err)) == -EINVAL,
	      "a setting that is none is refused");
	check(prov_sfp_lane_set("MODE=1000basex-an\n", err, sizeof(err)) == 0 &&
		      prov_sfp_lane_set("KEEP=1\n", err, sizeof(err)) == 0 &&
		      prov_sfp_lane_set("REVERT=1\n", err, sizeof(err)) == 0 &&
		      file_has("/lane.log", "try 1000basex-an\nkeep\nrevert\n"),
	      "a trial, a keep and a revert go to onu-lane");
	check(prov_sfp_lane_set("SPEED=10\n", err, sizeof(err)) == -EINVAL,
	      "a change that is none of them is refused");
	{
		struct prov_wait w = { .err = err, .errcap = sizeof(err) };
		char err2[256];

		e = prov_sfp_lane_write("KEEP=1\n", err, sizeof(err), prov_waited, &w);
		check(e == -EINPROGRESS && !w.ended &&
			      prov_sfp_lane_write("REVERT=1\n", err2, sizeof(err2),
						  prov_waited, &w) == -EBUSY,
		      "a change is not waited for, and a second one waits its turn");
		check(prov_wait(e, &w) == 0 && w.ended,
		      "the first is told its end when it has ended");
	}
	unsetenv("POND_ONU_LANE");

	put("/mgmt-stub", mgmt_stub, 0755);
	setenv("POND_ONU_MGMT", prov_path("/mgmt-stub", stub, sizeof(stub)), 1);
	s = render(prov_mgmt, buf, sizeof(buf), &e);
	check(!e && !strcmp(s, "{\"vlan\":4094,\"address\":\"198.51.100.2/30\","
			       "\"data_address\":null,\"pending\":null}"),
	      "the management pair in use");
	put("/mgmt.pending", "", 0644);
	s = render(prov_mgmt, buf, sizeof(buf), &e);
	check(!e && strstr(s, "\"pending\":{\"vlan\":100,\"address\":\"192.0.2.6/30\","
			      "\"until_s\":1100}") != NULL,
	      "a new pair that waits, and when it goes unless confirmed");
	unsetenv("POND_ONU_MGMT");

	put("/system-store", system_store_stub, 0755);
	put("/hostname-stub", hostname_stub, 0755);
	snprintf(onu_config, sizeof(onu_config), "%s", getenv("POND_ONU_CONFIG") ?: "");
	setenv("POND_ONU_CONFIG", prov_path("/system-store", stub, sizeof(stub)), 1);
	setenv("POND_HOSTNAME", prov_path("/hostname-stub", path, sizeof(path)), 1);
	check(prov_hostname_set("HOSTNAME=stick-7\n", err, sizeof(err)) == 0 &&
		      file_has("/stored-system", "HOSTNAME=stick-7\n") &&
		      file_has("/hostname-set", "stick-7\n"),
	      "a name goes to the store and to the kernel");
	check(prov_hostname_set("HOSTNAME=-stick\n", err, sizeof(err)) == -EINVAL &&
		      prov_hostname_set("HOSTNAME=a b\n", err, sizeof(err)) == -EINVAL &&
		      prov_hostname_set("NAME=x\n", err, sizeof(err)) == -EINVAL,
	      "a name that is not one of RFC 1123 is refused");
	setenv("POND_ONU_CONFIG", onu_config, 1);
	unsetenv("POND_HOSTNAME");
	test_product();
}

/* What the panel shows for the product name @line of /run/onu-system, and its slug. */
static bool product_is(const char *line, const char *name, const char *slug)
{
	char buf[256], text[256], got[PRODUCT_NAME_MAX + 1];
	const char *s;
	int e;

	snprintf(text, sizeof(text), "HOSTNAME=stick\n%s\nNTP_SERVER=192.0.2.1\n", line);
	put("/run/onu-system", text, 0644);
	s = render(prov_product, buf, sizeof(buf), &e);
	snprintf(text, sizeof(text), "\"%s\"", name);
	product_slug(got, sizeof(got));
	return !e && !strcmp(s, text) && !strcmp(got, slug);
}

static void test_product(void)
{
	static const char *const good[] = {
		"BuchOS", "x", "7", "a-1.b", "Fibre Box 2.0", "Abcdefghij Klmnopqr9",
	};
	static const char *const bad[] = {
		"", " x", "x ", "-x", "x.", ".x", "<b>x</b>", "a&b", "a\"b", "a'b", "a/b",
		"a\\b", "a_b", "a;b", "a=b", "a\tb", "Abcdefghij Klmnopqrs9",
		"\305\232wiat\305\202ow\303\263d",
	};
	char buf[256], stub[512], err[256], path[512], onu_config[512];
	bool all = true;
	const char *s;
	int e;

	puts("the name the panel shows, and the files named after it");
	for (size_t i = 0; i < ARRAY_SIZE(good); i++)
		all &= product_name_ok(good[i]);
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++)
		all &= !product_name_ok(bad[i]);
	check(all, "1 to 20 letters, digits, blanks, hyphens and dots of ASCII, "
		   "a letter or a digit first and last");

	mkdir(prov_path("/run", path, sizeof(path)), 0755);
	unlink(prov_path("/run/onu-system", path, sizeof(path)));
	s = render(prov_product, buf, sizeof(buf), &e);
	product_slug(stub, sizeof(stub));
	check(!e && !strcmp(s, "\"BuchOS\"") && !strcmp(stub, "buchos"),
	      "without the stick's own settings, BuchOS, and files that begin buchos");
	check(product_is("SFP_LANE=auto", "BuchOS", "buchos"), "without the setting too");
	check(product_is("PRODUCT_NAME=Fibre Box 2.0", "Fibre Box 2.0", "fibre-box-2-0"),
	      "the stick's name, and its slug in lower case, with a hyphen for each gap");
	check(product_is("PRODUCT_NAME=A - . B", "A - . B", "a-b") &&
		      product_is("PRODUCT_NAME=Abcdefghij Klmnopqr9", "Abcdefghij Klmnopqr9",
				 "abcdefghij-klmnopqr9"),
	      "a run of blanks, hyphens and dots is one hyphen; 20 characters are taken");
	check(product_is("PRODUCT_NAME=<b>x</b>", "BuchOS", "buchos") &&
		      product_is("PRODUCT_NAME=Abcdefghij Klmnopqrs9", "BuchOS", "buchos") &&
		      product_is("PRODUCT_NAME=\305\232wiat\305\202ow\303\263d", "BuchOS",
				 "buchos") &&
		      product_is("PRODUCT_NAME=", "BuchOS", "buchos"),
	      "one that is no name, written there by hand, is BuchOS");
	unlink(prov_path("/run/onu-system", path, sizeof(path)));

	put("/system-store", system_store_stub, 0755);
	snprintf(onu_config, sizeof(onu_config), "%s", getenv("POND_ONU_CONFIG") ?: "");
	setenv("POND_ONU_CONFIG", prov_path("/system-store", stub, sizeof(stub)), 1);
	check(prov_product_set("PRODUCT_NAME=Fibre Box 2.0\n", err, sizeof(err)) == 0 &&
		      file_has("/stored-system", "PRODUCT_NAME=Fibre Box 2.0\n"),
	      "a name goes to the store, as the stick's own setting");
	check(prov_product_set("PRODUCT_NAME=\n", err, sizeof(err)) == 0 &&
		      file_has("/stored-system", "PRODUCT_NAME=\n"),
	      "an empty one too, which takes it away for BuchOS");
	put("/stored-system", "", 0644);
	check(prov_product_set("PRODUCT_NAME=<script>\n", err, sizeof(err)) == -EINVAL &&
		      strstr(err, "PRODUCT_NAME") != NULL &&
		      prov_product_set("PRODUCT_NAME= x\n", err, sizeof(err)) == -EINVAL &&
		      prov_product_set("PRODUCT_NAME=Abcdefghij Klmnopqrs9\n", err,
				       sizeof(err)) == -EINVAL &&
		      prov_product_set("NAME=x\n", err, sizeof(err)) == -EINVAL &&
		      file_has("/stored-system", "") && !file_has("/stored-system", "PRODUCT"),
	      "one that is no name is refused, and nothing is stored");
	setenv("POND_ONU_CONFIG", onu_config, 1);
}

static void test_clock(void)
{
	char buf[2048], path[512];
	const char *s;
	int e;

	puts("the clock, and what timed says of it");
	mkdir(prov_path("/run", path, sizeof(path)), 0755);
	mkdir(prov_path("/run/timed", path, sizeof(path)), 0755);
	unlink(prov_path("/run/timed/state", path, sizeof(path)));
	s = render(prov_clock, buf, sizeof(buf), &e);
	check(!e && !strncmp(s, "{\"now_s\":", 9) &&
		      strstr(s, ",\"source\":null,\"state\":null,\"offset_ns\":null,"
				"\"updated_s\":null,\"steps\":null,\"freq_ppb\":null,\"ntp\":null,"
				"\"ptp\":null,\"olt\":null}") != NULL,
	      "without timed's state the clock has its time and no source");

	put("/run/timed/state",
	    "source=ptp\nstate=locked\noffset_ns=-1200\nupdated=1791892800\nsteps=1\nfreq_ppb=-5120\n"
	    "ntp_server=192.0.2.1\nntp_good=1\nntp_offset_ns=-30005728\n"
	    "ntp_delay_ns=180625\nntp_stratum=2\n"
	    "ptp_interface=mgmt\nptp_good=1\nptp_master=001b19fffe000001\n"
	    "ptp_via=l2\nptp_delay_ns=48229\nptp_offset_ns=-1200\n"
	    "ptp_stamps=hardware\nptp_phc=/dev/ptp0\nptp_phc_set=1\nptp_phc_freq_ppb=-6512\n"
	    "ptp_sys_phc_offset_ns=-830\nptp_sys_phc_window_ns=2880\nptp_tx_stamps=12\nptp_tx_missed=0\n"
	    "olt_file=/run/omcid/clock\nolt_good=0\n",
	    0644);
	s = render(prov_clock, buf, sizeof(buf), &e);
	check(!e && strstr(s, ",\"source\":\"ptp\",\"state\":\"locked\",\"offset_ns\":-1200,"
			      "\"updated_s\":1791892800,\"steps\":1,\"freq_ppb\":-5120,") != NULL,
	      "the source in use, the state, the offset, the frequency");
	check(strstr(s, "\"ntp\":{\"server\":\"192.0.2.1\",\"good\":true,"
			"\"offset_ns\":-30005728,\"delay_ns\":180625,\"stratum\":2}") != NULL,
	      "what NTP gave");
	check(strstr(s, "\"ptp\":{\"interface\":\"mgmt\",\"good\":true,"
			"\"master\":\"001b19fffe000001\",\"via\":\"l2\","
			"\"delay_ns\":48229,\"offset_ns\":-1200,\"stamps\":\"hardware\","
			"\"phc_freq_ppb\":-6512,\"sys_phc_offset_ns\":-830,"
			"\"tx_stamps\":12,\"tx_missed\":0}") != NULL,
	      "what PTP gave, with the switch's hardware stamps");
	check(strstr(s, "\"olt\":{\"good\":false,\"utc_s\":null,\"offset_ns\":null}}") != NULL,
	      "the OLT asked, and nothing from it yet");

	put("/run/timed/state", "source=none\nstate=unsynchronized\nsteps=x\nntp_server=192.0.2.1\nntp_good=2\n",
	    0644);
	s = render(prov_clock, buf, sizeof(buf), &e);
	check(!e && strstr(s, "\"steps\":null,\"freq_ppb\":null,\"ntp\":{\"server\":\"192.0.2.1\",\"good\":null,"
			      "\"offset_ns\":null,\"delay_ns\":null,\"stratum\":null},"
			      "\"ptp\":null,\"olt\":null}") != NULL,
	      "a source not asked is null, and a value that is none is null");
	unlink(prov_path("/run/timed/state", path, sizeof(path)));
}

/* omcid's answer to "mib", as its ubus object gives it. */
#define OMCID_MIB                                                              \
	"{\"mes\":[{\"class\":256,\"instance\":0,\"attrs\":{\"1\":\"5a5a5a5a\"},"  \
	"\"tables\":[]}],\"truncated\":false,\"logical_password_set\":true,"       \
	"\"frames\":{\"handled\":5,\"rejected\":1,\"answered\":4}}"

/*
 * The largest settings the stick can have, made as the panel makes them,
 * through the providers, and judged by onu-config: seven sessions, eight
 * networks, 512 fixed addresses, each name of 32 characters, the most that
 * need escaping that the store holds, and 512 devices with a lease in each
 * network, each name of 20 bytes that need \u00XX, a value of their own.
 * They are served whole: alone, as a reply, and the settings among other
 * values as the panel polls them.
 */
static void test_largest_settings(void)
{
	static char buf[2 << 20], change[65536];
	char name[57];
	static unsigned char leases[8 + 36 * 512];
	char file[512], err[256], stub[512], user[64], line[512];
	uint64_t written = (uint64_t)time(NULL);
	struct mg_connection *c = conn_new();
	struct prov_buf b;
	size_t len = 0, whole, leased;
	char *body = NULL;
	int grows = 0, rc = 0, status;
	FILE *f;

	puts("the largest settings, served whole");
	if (!c)
		return;
	/*
	 * A name of the most that the store holds for each: 24 quotes and 8
	 * characters of four bytes, 56 bytes, 80 as JSON.
	 */
	memset(name, '"', 24);
	for (int i = 0; i < 8; i++)
		memcpy(name + 24 + 4 * i, "\xf0\x9f\x98\x80", 4);
	name[56] = '\0';
	mkdir(prov_path("/run", file, sizeof(file)), 0755);
	put("/apply-stub", apply_stub, 0755);
	put("/run/release", "", 0644);
	setenv("POND_ONU_APPLY", prov_path("/apply-stub", stub, sizeof(stub)), 1);
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		snprintf(line, sizeof(line), "/run/onu-pppoe%d", n);
		unlink(prov_path(line, file, sizeof(file)));
	}

	/* Users of 63 characters, each one that JSON escapes. */
	for (int n = 1; n <= 7; n++) {
		memset(user, n & 1 ? '"' : '\\', 63);
		user[63] = '\0';
		snprintf(line, sizeof(line), "USER=%s\nSECRET=example-secret\nVLAN=410\nPASS=off\n",
			 user);
		rc |= prov_session_set(line, err, sizeof(err));
		settled(buf, sizeof(buf));
	}
	for (int n = 1; n <= 8; n++) {
		int net = 100 + 4 * n;

		snprintf(line, sizeof(line),
			 "CONN=pppoe:%d\nNAME=%s\nVLAN=%d\nADDRESS=192.168.%d.1/22\n"
			 "POOL_FROM=192.168.%d.0\nPOOL_TO=192.168.%d.255\nLEASE=604800\n",
			 (n - 1) % 7 + 1, name, 100 + n, net, net + 1, net + 2);
		rc |= prov_lan_set(line, err, sizeof(err));
		settled(buf, sizeof(buf));
	}
	/* The fixed addresses, 64 in each network, in one change: one by one is slow. */
	for (int i = 0; i < 512; i++) {
		int lan = i / 64 + 1, j = i % 64;

		len += (size_t)snprintf(change + len, sizeof(change) - len,
					"FIXED%d=%d/02:00:00:00:%02x:%02x/192.168.%d.%d\n"
					"FIXED%d_NAME=%s\n",
					i + 1, lan, lan, j, 103 + 4 * lan, 100 + j, i + 1, name);
	}
	rc |= prov_settings_set(change, err, sizeof(err));
	settled(buf, sizeof(buf));
	check(rc == 0 && len < sizeof(change),
	      "seven sessions, eight networks and 512 fixed addresses are taken");

	for (int i = 0; i < 8; i++)
		leases[i] = (unsigned char)(written >> (56 - 8 * i));
	for (int n = 1; n <= 8; n++) {
		for (int i = 0; i < 512; i++) {
			unsigned char *l = leases + 8 + 36 * i;

			memset(l, 0, 36);
			memset(l, 0xff, 4);
			l[4] = 192;
			l[5] = 168;
			l[6] = (unsigned char)(100 + 4 * n + i / 156);
			l[7] = (unsigned char)(100 + i % 156);
			memcpy(l + 8, "\xaa\xbb\xcc\xdd", 4);
			l[12] = (unsigned char)n;
			l[13] = (unsigned char)i;
			memset(l + 14, 0x01, 20);
		}
		snprintf(line, sizeof(line), "/run/udhcpd-net%d.leases", n);
		f = fopen(prov_path(line, file, sizeof(file)), "w");
		if (f) {
			fwrite(leases, 1, sizeof(leases), f);
			fclose(f);
		}
	}

	b = (struct prov_buf){ .max = VALUE_MAX + 1, .grow = heap_grow, .ctx = &grows };
	rc = prov_settings(&b);
	whole = b.len;
	printf("  the largest settings come to %zu bytes of JSON\n", whole);
	check(rc == 0 && !b.overflow && well_formed(b.buf) &&
		      occurrences(b.buf, "\"mac\":\"02:00:00:00:") == 512 &&
		      occurrences(b.buf, "\"name\":\"\\\"\\\"") == 512 + 8 &&
		      occurrences(b.buf, "\"user\":\"") == 7,
	      "the settings are whole: every fixed address, each name escaped");
	check(!strstr(b.buf, "\"leases\"") && !strstr(b.buf, "aa:bb:cc:dd:") && whole < 96 * 1024,
	      "without the devices, which are a value of their own: what the panel polls stays small");
	free(b.buf);

	b = (struct prov_buf){ .max = VALUE_MAX + 1, .grow = heap_grow, .ctx = &grows };
	rc = prov_leases(&b);
	leased = b.len;
	printf("  the most leases come to %zu bytes of JSON\n", leased);
	check(rc == 0 && !b.overflow && well_formed(b.buf) && b.buf[0] == '[' &&
		      occurrences(b.buf, "\"mac\":\"aa:bb:cc:dd:") == 8 * 512 &&
		      occurrences(b.buf, "\"left_s\":42949672") == 8 * 512 &&
		      occurrences(b.buf, "\\u0001") == 8 * 512 * 20,
	      "the leases are whole: every device of every network, each name escaped");
	check(leased > 96 * 1024 && leased <= VALUE_MAX,
	      "more than a reply had room for before, within the most a value may be");
	free(b.buf);

	value_get(c, prov_find("settings"), NULL, 0);
	status = reply_of(c, &body);
	check(status == 200 && body && well_formed(body) &&
		      !strncmp(body, "{\"id\":\"settings\",\"value\":{\"settings\":", 37) &&
		      strlen(body) >= whole + 26 &&
		      occurrences(body, "\"mac\":\"02:00:00:00:") == 512,
	      "and as a reply, its length as the headers say");
	free(body);
	c->send.len = 0;
	value_sent(c);

	value_get(c, prov_find("leases"), NULL, 0);
	status = reply_of(c, &body);
	check(status == 200 && body && well_formed(body) &&
		      !strncmp(body, "{\"id\":\"leases\",\"value\":[{", 25) &&
		      strlen(body) >= leased + 24 &&
		      occurrences(body, "\"mac\":\"aa:bb:cc:dd:") == 8 * 512,
	      "the leases as a reply of their own");
	free(body);
	c->send.len = 0;
	value_sent(c);

	value_getm(c, mg_str("uptime,settings,memory"));
	status = reply_of(c, &body);
	check(status == 200 && body && well_formed(body) && strstr(body, "\"uptime\":{") &&
		      strstr(body, "\"memory\":{") &&
		      occurrences(body, "\"mac\":\"02:00:00:00:") == 512 &&
		      !strstr(body, "aa:bb:cc:dd:"),
	      "and the settings among the values the panel polls together, without the devices");
	free(body);
	conn_free(c);

	for (int n = 1; n <= 8; n++) {
		snprintf(line, sizeof(line), "/run/udhcpd-net%d.leases", n);
		unlink(prov_path(line, file, sizeof(file)));
	}
	unlink(prov_path("/run/onu.conf", file, sizeof(file)));
	for (int n = 1; n <= 7; n++) {
		snprintf(line, sizeof(line), "/run/onu-pppoe%d", n);
		unlink(prov_path(line, file, sizeof(file)));
	}
	unsetenv("POND_ONU_APPLY");
}

static void test_omci(void)
{
	char buf[16384], path[200];
	const char *s;
	FILE *f;
	int err;

	puts("the OLT's provisioning, from omcid on ubus");

	snprintf(path, sizeof(path), "%s/run", getenv("POND_ROOT"));
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/run/ubus", getenv("POND_ROOT"));
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/run/ubus/omcid.mib", getenv("POND_ROOT"));
	f = fopen(path, "w");
	check(f != NULL, "omcid's answer is in place");
	if (!f)
		return;
	fputs(OMCID_MIB "\n", f);
	fclose(f);

	s = render(prov_omci, buf, sizeof(buf), &err);
	check(!err && !strcmp(s, OMCID_MIB), "omcid's answer is the value as it is");
	s = render(prov_omci, buf, 200, &err);
	check(err < 0, "an answer that does not fit is an error, not a value cut short");

	unlink(path);
	s = render(prov_omci, buf, sizeof(buf), &err);
	check(err < 0, "without omcid the value is an error, not an empty unit");
}

/* omcid's answer to "frames", as its ubus object gives it. */
#define OMCID_FRAMES                                                           \
	"{\"head\":2,\"first\":0,\"frames\":[{\"seq\":1,\"sec\":1792270000,"     \
	"\"ms\":5,\"verdict\":5,\"type\":9,\"masked\":false,\"frame\":\"12\"}]}"

static int frames_from_one(struct prov_buf *out)
{
	return prov_find_from("omci_frames")->read(out, 1);
}

static void test_omci_frames(void)
{
	char buf[16384], path[200];
	const char *s;
	FILE *f;
	int err;

	puts("the conversation with the OLT, from omcid on ubus");

	snprintf(path, sizeof(path), "%s/run/ubus/omcid.frames", getenv("POND_ROOT"));
	f = fopen(path, "w");
	check(f != NULL, "omcid's answer is in place");
	if (!f)
		return;
	fputs(OMCID_FRAMES "\n", f);
	fclose(f);

	s = render(prov_omci_frames, buf, sizeof(buf), &err);
	check(!err && !strcmp(s, OMCID_FRAMES), "omcid's answer is the value as it is");
	s = render(frames_from_one, buf, sizeof(buf), &err);
	check(!err && !strcmp(s, OMCID_FRAMES), "a reader that names a point gets the answer");
	check(!prov_find_from("omci"), "a value that is no log has no point to read from");
	s = render(prov_omci_frames, buf, 300, &err);
	check(err < 0, "a buffer without room for a frame is an error");

	unlink(path);
	s = render(prov_omci_frames, buf, sizeof(buf), &err);
	check(err < 0, "without omcid the value is an error, not an empty conversation");
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
	check(p && prov_present(p) && (p->flags & PROV_SLOW),
	      "the OLT's provisioning is served, and slow: the whole MIB of omcid");

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
	test_growth();
	test_value_replies();
	test_settings();
	test_firmware();
	test_panel_values();
	test_lans();
	test_ipoe();
	test_session_pass();
	test_largest_settings();
	test_forwards();
	test_rules();
	test_omci();
	test_omci_frames();
	test_system();
	test_clock();
	test_table();
	test_failure_paths();

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
