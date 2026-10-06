// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onucfg: read and change the stick's configuration store (cfgstore.h).
 *
 *   onucfg [--store PATH] get KEY        print the value of KEY, or fail
 *   onucfg [--store PATH] list           print every KEY=VALUE line
 *   onucfg [--store PATH] set KEY VALUE  set KEY, keeping the store whole
 *   onucfg [--store PATH] unset KEY      remove KEY
 *
 * The store is a file or an MTD character device; --store or ONUCFG_STORE
 * names it, and --size gives the size of a store that does not exist yet. A
 * change writes the slot that is not current, then the store carries the new
 * one, so a power loss in the middle keeps the old configuration (see
 * cfgstore.h). On an MTD device the slot's erase blocks are erased first.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <mtd/mtd-user.h>

#include "cfgstore.h"

/* A store at most this big, so a bad --size cannot ask for a huge buffer. */
#define REGION_MAX (1u << 20)

static int fail(const char *what)
{
	fprintf(stderr, "onucfg: %s: %s\n", what, strerror(errno));
	return 1;
}

/*
 * Write @len bytes of @buf to the store at @off. On an MTD device the erase
 * blocks the write falls in are erased first; @off and @len are a whole slot,
 * which the caller has laid on erase-block boundaries. On a file the bytes are
 * written in place.
 */
static int write_region(int fd, bool mtd, size_t erase, size_t off,
			const uint8_t *buf, size_t len)
{
	if (mtd) {
		struct erase_info_user e;
		size_t start = off - off % erase;
		size_t end = off + len;

		end = end % erase ? end + erase - end % erase : end;
		e.start = (uint32_t)start;
		e.length = (uint32_t)(end - start);
		if (ioctl(fd, MEMERASE, &e) != 0)
			return -1;
	}
	if (pwrite(fd, buf, len, (off_t)off) != (ssize_t)len)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *store = getenv("ONUCFG_STORE");
	size_t size = 0;
	int opt;
	static const struct option opts[] = {
		{ "store", required_argument, NULL, 's' },
		{ "size", required_argument, NULL, 'z' },
		{ NULL, 0, NULL, 0 },
	};

	while ((opt = getopt_long(argc, argv, "s:z:", opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			store = optarg;
			break;
		case 'z':
			size = (size_t)strtoul(optarg, NULL, 0);
			break;
		default:
			return 2;
		}
	}
	if (!store) {
		fprintf(stderr, "onucfg: no store (--store or ONUCFG_STORE)\n");
		return 2;
	}

	int nargs = argc - optind;
	const char *verb = nargs > 0 ? argv[optind] : NULL;
	const char *key = nargs > 1 ? argv[optind + 1] : NULL;
	const char *value = nargs > 2 ? argv[optind + 2] : NULL;
	bool writing = verb && (!strcmp(verb, "set") || !strcmp(verb, "unset"));

	if (!verb ||
	    (!strcmp(verb, "get") && nargs != 2) ||
	    (!strcmp(verb, "set") && nargs != 3) ||
	    (!strcmp(verb, "unset") && nargs != 2) ||
	    (!strcmp(verb, "list") && nargs != 1) ||
	    (strcmp(verb, "get") && strcmp(verb, "set") && strcmp(verb, "unset") &&
	     strcmp(verb, "list"))) {
		fprintf(stderr,
			"usage: onucfg [--store PATH] [--size N] "
			"get KEY | list | set KEY VALUE | unset KEY\n");
		return 2;
	}

	int fd = open(store, writing ? O_RDWR : O_RDONLY);

	if (fd < 0 && writing && errno == ENOENT && size)
		fd = open(store, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return fail(store);

	/*
	 * An MTD device gives its own size and erase block; a file its size,
	 * or --size for one that does not exist yet, grown to that size.
	 */
	struct mtd_info_user mi;
	bool mtd = ioctl(fd, MEMGETINFO, &mi) == 0;
	size_t erase = mtd ? mi.erasesize : 0;
	size_t region_len;

	if (mtd) {
		region_len = mi.size;
	} else {
		struct stat st;

		if (fstat(fd, &st) != 0) {
			close(fd);
			return fail(store);
		}
		region_len = st.st_size > 0 ? (size_t)st.st_size : size;
		if (writing && (size_t)st.st_size < region_len &&
		    ftruncate(fd, (off_t)region_len) != 0) {
			close(fd);
			return fail(store);
		}
	}
	if (region_len == 0 || region_len > REGION_MAX ||
	    cfg_payload_cap(region_len) == 0) {
		fprintf(stderr, "onucfg: %s: no usable store size\n", store);
		close(fd);
		return 2;
	}

	uint8_t *region = calloc(1, region_len);
	size_t cap = cfg_payload_cap(region_len) + 1;
	char *payload = calloc(1, cap);

	if (!region || !payload) {
		free(region);
		free(payload);
		close(fd);
		return fail("out of memory");
	}

	ssize_t got = pread(fd, region, region_len, 0);

	if (got < 0) {
		free(region);
		free(payload);
		close(fd);
		return fail(store);
	}

	uint32_t seq = 0;
	int slot = -1;
	int err = cfg_load(region, region_len, payload, cap, NULL, &seq, &slot);

	if (err && err != -ENOENT) {
		fprintf(stderr, "onucfg: %s: %s\n", store, strerror(-err));
		goto out_fail;
	}

	int rc = 0;

	if (!strcmp(verb, "get")) {
		char val[CFG_VALUE_MAX + 1];

		err = cfg_get(payload, key, val, sizeof(val));
		if (err == 0)
			printf("%s\n", val);
		else
			rc = 1;
	} else if (!strcmp(verb, "list")) {
		fputs(payload, stdout);
	} else if (!strcmp(verb, "set")) {
		err = cfg_set(payload, cap, key, value);
		if (err) {
			fprintf(stderr, "onucfg: set %s: %s\n", key,
				strerror(-err));
			goto out_fail;
		}
	} else { /* unset */
		cfg_unset(payload, key);
	}

	if (writing) {
		size_t off, len;

		err = cfg_store(region, region_len, slot, seq, payload,
				strlen(payload), &off, &len);
		if (err) {
			fprintf(stderr, "onucfg: %s: %s\n", store,
				strerror(-err));
			goto out_fail;
		}
		if (write_region(fd, mtd, erase, off, region + off, len) != 0) {
			fail(store);
			goto out_fail;
		}
	}

	free(region);
	free(payload);
	close(fd);
	return rc;

out_fail:
	free(region);
	free(payload);
	close(fd);
	return 1;
}
