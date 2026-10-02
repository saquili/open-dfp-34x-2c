// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * ponid: print the identity of the unit from its factory configuration, as
 * "key=value" lines, or the value of one key. The boot script writes the
 * lines to /run/onu-identity, which omcid reads, and hands the serial
 * number to the PON driver. The output holds the PLOAM password, so it
 * must be kept private.
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

/* Each key ponid prints, and where the factory configuration holds it. */
static const struct {
	const char *key;
	const char *file;
	const char *name;
} FIELDS[] = {
	{ "serial", FILE_UNIT, "GPON_SN" },
	{ "vendor_id", FILE_UNIT, "PON_VENDOR_ID" },
	{ "equipment_serial", FILE_UNIT, "HW_SERIAL_NO" },
	{ "version", FILE_UNIT, "HW_HWVER" },
	{ "equipment_id", FILE_UNIT, "HW_CWMP_PRODUCTCLASS" },
	{ "model", FILE_UNIT, "GPON_ONU_MODEL" },
	{ "ploam_format", FILE_PON, "GPON_PLOAM_FORMAT" },
	{ "ploam_password", FILE_PON, "GPON_PLOAM_PASSWD" },
	{ "tm_option", FILE_PON, "OMCI_TM_OPT" },
	{ "omcc_version", FILE_PON, "OMCC_VER" },
	{ "vendor_product_code", FILE_PON, "OMCI_VENDOR_PRODUCT_CODE" },
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

		if (want != NULL && strcmp(want, FIELDS[i].key) != 0)
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
