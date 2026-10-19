// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * ponid: print the identity of the unit from its factory configuration, as
 * "key=value" lines, or the value of one key. The boot script writes the
 * lines to /run/onu-identity, which omcid reads, and hands the serial
 * number to the PON driver. The output holds the PLOAM password, so it
 * must be kept private.
 *
 * Three more keys are printed only when they are named, so that they stay
 * out of the lines omcid reads: the password of the shipped firmware's
 * administrator, which the first start takes for root's (onu-config load),
 * and the address and the MAC address the shipped firmware gave the host
 * link, which the boot script gives it too.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "factory.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The two files of the factory configuration that carry the identity. */
#define FILE_UNIT "lastgood_hs.xml"
#define FILE_PON "lastgood.xml"

/* How a value is printed: as the file has it, or checked, in a usual form. */
enum shape {
	AS_IS,
	TEXT,	/* the escapes of XML undone */
	MAC,	/* six pairs of lowercase digits apart by colons */
	PREFIX, /* address/length, with the netmask of the name @mask */
};

/*
 * Each key ponid prints, and where the factory configuration holds it; one
 * that is @asked is printed only when it is named.
 */
struct field {
	const char *key;
	const char *file;
	const char *name;
	const char *mask;
	enum shape shape;
	bool asked;
};

static const struct field FIELDS[] = {
	{ .key = "serial", .file = FILE_UNIT, .name = "GPON_SN" },
	{ .key = "vendor_id", .file = FILE_UNIT, .name = "PON_VENDOR_ID" },
	{ .key = "equipment_serial", .file = FILE_UNIT, .name = "HW_SERIAL_NO" },
	{ .key = "version", .file = FILE_UNIT, .name = "HW_HWVER" },
	{ .key = "equipment_id", .file = FILE_UNIT, .name = "HW_CWMP_PRODUCTCLASS" },
	{ .key = "model", .file = FILE_UNIT, .name = "GPON_ONU_MODEL" },
	{ .key = "ploam_format", .file = FILE_PON, .name = "GPON_PLOAM_FORMAT" },
	{ .key = "ploam_password", .file = FILE_PON, .name = "GPON_PLOAM_PASSWD" },
	{ .key = "tm_option", .file = FILE_PON, .name = "OMCI_TM_OPT" },
	{ .key = "omcc_version", .file = FILE_PON, .name = "OMCC_VER" },
	{ .key = "vendor_product_code", .file = FILE_PON, .name = "OMCI_VENDOR_PRODUCT_CODE" },
	{ .key = "admin_password", .file = FILE_PON, .name = "SUSER_PASSWORD",
	  .shape = TEXT, .asked = true },
	{ .key = "lan_address", .file = FILE_PON, .name = "LAN_IP_ADDR",
	  .mask = "LAN_SUBNET", .shape = PREFIX, .asked = true },
	{ .key = "mac", .file = FILE_UNIT, .name = "ELAN_MAC_ADDR",
	  .shape = MAC, .asked = true },
};

#define IMAGE_MAX (1024u * 1024u)
#define FILE_MAX (256u * 1024u)

/* The read-only device of the partition "config", from /proc/mtd. */
static bool config_partition(char *out, size_t cap)
{
	char line[256];
	FILE *f = fopen("/proc/mtd", "re");
	bool found = false;

	if (f == NULL)
		return false;
	while (!found && fgets(line, sizeof(line), f) != NULL) {
		unsigned int idx;
		char name[64];

		if (sscanf(line, "mtd%u: %*x %*x \"%63[^\"]\"", &idx, name) ==
			    2 &&
		    strcmp(name, "config") == 0) {
			snprintf(out, cap, "/dev/mtd%uro", idx);
			found = true;
		}
	}
	fclose(f);
	return found;
}

/* Read up to @cap bytes of @path. Return: the size, or -1 with errno set. */
static long read_image(const char *path, uint8_t *out, size_t cap)
{
	size_t len = 0;
	const int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	while (len < cap) {
		const ssize_t n = read(fd, out + len, cap - len);

		if (n < 0) {
			const int saved = errno;

			close(fd);
			errno = saved;
			return -1;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	return (long)len;
}

/* @value of @f in its form, with a second name of @doc for a prefix. */
static int shape(const struct field *f, const char *doc, char *value,
		 size_t cap)
{
	char mask[32];
	char raw[128];

	switch (f->shape) {
	case AS_IS:
		return 0;
	case TEXT:
		return factory_text(value);
	case MAC:
		snprintf(raw, sizeof(raw), "%s", value);
		return factory_mac(raw, value, cap);
	case PREFIX:
		if (factory_value(doc, f->mask, mask, sizeof(mask)) != 0)
			return -ENOENT;
		snprintf(raw, sizeof(raw), "%s", value);
		return factory_prefix(raw, mask, value, cap);
	}
	return -EINVAL;
}

int main(int argc, char **argv)
{
	static uint8_t img[IMAGE_MAX];
	static uint8_t file[FILE_MAX];
	char path[64];
	const char *src = NULL;
	const char *want = NULL;
	bool printed = false;
	int opt;

	while ((opt = getopt(argc, argv, "f:h")) != -1) {
		switch (opt) {
		case 'f':
			src = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-f IMAGE] [KEY]\n",
				argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (optind < argc)
		want = argv[optind];

	if (src == NULL && config_partition(path, sizeof(path)))
		src = path;
	if (src == NULL) {
		fprintf(stderr,
			"ponid: no partition \"config\" in /proc/mtd\n");
		return 1;
	}

	const long imglen = read_image(src, img, sizeof(img));

	if (imglen < 0) {
		fprintf(stderr, "ponid: %s: %s\n", src, strerror(errno));
		return 1;
	}

	for (size_t i = 0; i < ARRAY_SIZE(FIELDS); i++) {
		char value[128];
		size_t filelen = 0;
		int err;

		if (want != NULL ? strcmp(want, FIELDS[i].key) != 0 :
				   FIELDS[i].asked)
			continue;

		err = factory_file(img, (size_t)imglen, FIELDS[i].file, file,
				   sizeof(file) - 1, &filelen);
		if (err) {
			fprintf(stderr, "ponid: %s: %s\n", FIELDS[i].file,
				strerror(-err));
			continue;
		}
		file[filelen] = '\0';
		if (factory_value((const char *)file, FIELDS[i].name, value,
				  sizeof(value)) != 0)
			continue;
		if (FIELDS[i].asked && value[0] == '\0')
			continue;
		if (shape(&FIELDS[i], (const char *)file, value,
			  sizeof(value)) != 0) {
			/* Never the value: it may be a password. */
			fprintf(stderr, "ponid: %s: not of its form\n",
				FIELDS[i].key);
			continue;
		}

		if (want != NULL)
			printf("%s\n", value);
		else
			printf("%s=%s\n", FIELDS[i].key, value);
		printed = true;
	}

	if (!printed) {
		fprintf(stderr, "ponid: nothing found%s%s\n",
			want != NULL ? " for " : "", want != NULL ? want : "");
		return 1;
	}
	return 0;
}
