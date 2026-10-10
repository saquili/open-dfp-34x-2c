// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-config: the stick's settings, their store in the flash and their rules.
 *
 *   onu-config [--store PATH] get KEY        print the value of KEY, or fail
 *   onu-config [--store PATH] list           print every KEY=VALUE line
 *   onu-config [--store PATH] set KEY VALUE  set KEY, keeping the store whole
 *   onu-config [--store PATH] unset KEY      remove KEY
 *   onu-config [--store PATH] import         set every KEY=VALUE line of the
 *                                            standard input, in one write
 *   onu-config check                         the settings of the data path on
 *                                            the standard input, judged
 *   onu-config settings                      the same, taken as they come
 *   onu-config load|save|migrate|identity|password|auth HASH|system|pd N
 *                                            the files the running system
 *                                            reads, to and from the store:
 *                                            onu-config.sh, in ONU_LIB
 *                                            (/usr/lib/onu), carries them out
 *
 * The store is a file or an MTD character device; --store or ONU_STORE names
 * it, and --size gives the size of a store that does not exist yet. A change
 * writes the slot that is not current, then the store carries the new one, so
 * a power loss in the middle keeps the old configuration (see cfgstore.h). On
 * an MTD device the slot's erase blocks are erased first. A change that leaves
 * the configuration as it was writes nothing.
 *
 * check and settings read KEY=VALUE lines of the data path's settings and
 * PPPoE accounts, a later line over an earlier one (settings.h), and print
 * the settings with every default filled in and the keys of before taken
 * over, never an account. check refuses a set with a line that is not a
 * setting or a value, or settings that make no sense together, and says
 * why; settings leaves such a line out, says so on the standard error, and
 * judges nothing. They need no store.
 *
 * onu-config was onucfg and onu-store before (ADR 0015).
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
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
#include "settings.h"

/* A store at most this big, so a bad --size cannot ask for a huge buffer. */
#define REGION_MAX (1u << 20)

static int fail(const char *what)
{
	fprintf(stderr, "onu-config: %s: %s\n", what, strerror(errno));
	return 1;
}

/*
 * Write @len bytes of @buf to the store at @off. On an MTD device the erase
 * blocks the write falls in are erased first; @off is the start of a slot,
 * which the slot layout puts on an erase-block boundary. On a file the bytes
 * are written in place.
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

/*
 * Set the KEY=VALUE lines of @in in @payload. An empty line is skipped; any
 * other line without a valid key, or that does not fit, fails the whole
 * import before anything is written.
 */
static int import_lines(FILE *in, char *payload, size_t cap)
{
	char *line = NULL;
	size_t linecap = 0;
	ssize_t n;
	int err = 0;

	while ((n = getline(&line, &linecap, in)) > 0) {
		char *eq;

		if (line[n - 1] == '\n')
			line[--n] = '\0';
		if (n == 0)
			continue;
		eq = strchr(line, '=');
		if (!eq) {
			fprintf(stderr, "onu-config: import: not KEY=VALUE: %s\n",
				line);
			err = -EINVAL;
			break;
		}
		*eq = '\0';
		err = cfg_set(payload, cap, line, eq + 1);
		if (err) {
			fprintf(stderr, "onu-config: import %s: %s\n", line,
				strerror(-err));
			break;
		}
	}
	free(line);
	return err;
}

/* The longest set of settings that check and settings take. */
#define SETTINGS_TEXT_MAX 16384

/* check (@strict) or settings, on the standard input and output. */
static int settings_cmd(bool strict)
{
	static char text[SETTINGS_TEXT_MAX], out[SETTINGS_TEXT_MAX];
	struct settings s;
	char err[160], *line, *save = NULL;
	size_t len = fread(text, 1, sizeof(text), stdin);
	int rc = 1;

	if (len == sizeof(text)) {
		fprintf(stderr, "onu-config: the settings are too long\n");
		goto out;
	}
	text[len] = '\0';
	settings_defaults(&s);
	for (line = strtok_r(text, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		if (!settings_line(&s, line, err, sizeof(err)))
			continue;
		if (strict) {
			fprintf(stderr, "onu-config: %s\n", err);
			goto out;
		}
		fprintf(stderr, "onu-config: %s; left out\n", err);
	}
	settings_settle(&s);
	if (strict && !settings_coherent(&s, err, sizeof(err))) {
		fprintf(stderr, "onu-config: %s\n", err);
		goto out;
	}
	if (settings_format(&s, out, sizeof(out)) >= sizeof(out)) {
		fprintf(stderr, "onu-config: the settings are too long\n");
		goto out;
	}
	fputs(out, stdout);
	rc = fflush(stdout) ? 1 : 0;
out:
	explicit_bzero(&s, sizeof(s));
	explicit_bzero(text, sizeof(text));
	return rc;
}

/* The verbs of the files the running system reads, onu-config.sh's. */
static bool files_verb(const char *verb)
{
	static const char *const verbs[] = {
		"load", "save", "migrate", "identity", "password", "auth", "system", "pd",
	};

	for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++)
		if (!strcmp(verb, verbs[i]))
			return true;
	return false;
}

/*
 * Run onu-config.sh of ONU_LIB, /usr/lib/onu unless set, with the verb and
 * the arguments of @args, on the store @store when one was named.
 */
static int files(char **args, const char *store)
{
	const char *lib = getenv("ONU_LIB");
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/onu-config.sh", lib && *lib ? lib : "/usr/lib/onu");
	if (store && setenv("ONU_STORE", store, 1))
		return fail("ONU_STORE");
	/* args[-1] is onu-config's own, or an option's: the script's name there. */
	args[-1] = path;
	execv(path, args - 1);
	fail(path);
	return 127;
}

int main(int argc, char **argv)
{
	const char *store = getenv("ONU_STORE");
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
	if (optind < argc && files_verb(argv[optind]))
		return files(argv + optind, store);
	if (optind < argc && (!strcmp(argv[optind], "check") ||
			      !strcmp(argv[optind], "settings"))) {
		if (argc - optind != 1) {
			fputs("usage: onu-config check | settings\n", stderr);
			return 2;
		}
		return settings_cmd(!strcmp(argv[optind], "check"));
	}
	if (!store) {
		fprintf(stderr, "onu-config: no store (--store or ONU_STORE)\n");
		return 2;
	}

	int nargs = argc - optind;
	const char *verb = nargs > 0 ? argv[optind] : NULL;
	const char *key = nargs > 1 ? argv[optind + 1] : NULL;
	const char *value = nargs > 2 ? argv[optind + 2] : NULL;
	bool writing = verb && (!strcmp(verb, "set") || !strcmp(verb, "unset") ||
				!strcmp(verb, "import"));

	if (!verb ||
	    (!strcmp(verb, "get") && nargs != 2) ||
	    (!strcmp(verb, "set") && nargs != 3) ||
	    (!strcmp(verb, "unset") && nargs != 2) ||
	    (!strcmp(verb, "list") && nargs != 1) ||
	    (!strcmp(verb, "import") && nargs != 1) ||
	    (strcmp(verb, "get") && strcmp(verb, "set") && strcmp(verb, "unset") &&
	     strcmp(verb, "list") && strcmp(verb, "import"))) {
		fputs("usage: onu-config [--store PATH] [--size N] get KEY | list |\n"
		      "                  set KEY VALUE | unset KEY | import\n"
		      "       onu-config check | settings\n"
		      "       onu-config load | save | migrate | identity | password |\n"
		      "                  auth HASH | system | pd N\n", stderr);
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
		fprintf(stderr, "onu-config: %s: no usable store size\n", store);
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
	/* What the store holds now, to tell whether a change changes anything. */
	char *before = NULL;
	int err = cfg_load(region, region_len, payload, cap, NULL, &seq, &slot);

	if (err && err != -ENOENT) {
		fprintf(stderr, "onu-config: %s: %s\n", store, strerror(-err));
		goto out_fail;
	}

	if (err == 0 && writing)
		before = strdup(payload);

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
			fprintf(stderr, "onu-config: set %s: %s\n", key,
				strerror(-err));
			goto out_fail;
		}
	} else if (!strcmp(verb, "import")) {
		err = import_lines(stdin, payload, cap);
		if (err)
			goto out_fail;
	} else { /* unset */
		cfg_unset(payload, key);
	}

	if (writing && !(before && !strcmp(before, payload))) {
		size_t off, len;

		err = cfg_store(region, region_len, slot, seq, payload,
				strlen(payload), &off, &len);
		if (err) {
			fprintf(stderr, "onu-config: %s: %s\n", store,
				strerror(-err));
			goto out_fail;
		}
		if (write_region(fd, mtd, erase, off, region + off, len) != 0) {
			fail(store);
			goto out_fail;
		}
	}

	free(before);
	free(region);
	free(payload);
	close(fd);
	return rc;

out_fail:
	free(before);
	free(region);
	free(payload);
	close(fd);
	return 1;
}
