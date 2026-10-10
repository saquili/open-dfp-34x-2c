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
 *                                            standard input and remove the
 *                                            key of every -KEY line, in one
 *                                            write
 *   onu-config check                         the settings of the data path on
 *                                            the standard input, judged
 *   onu-config settings                      the same, taken as they come
 *   onu-config current                       the settings in force, of the
 *                                            running system's files, with
 *                                            each account's user and whether
 *                                            its secret is set
 *   onu-config change                        the change on the standard
 *                                            input over the settings in
 *                                            force, judged and written to
 *                                            the running system's files
 *   onu-config password-check                whether the line of the
 *                                            standard input is root's
 *                                            password, in the exit status
 *   onu-config password-state                "set", or "default" while the
 *                                            default password is in force
 *   onu-config identity-current              the identity in force, without
 *                                            its passwords
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
 * current and change read the settings in force from the files of the
 * running system, /run/onu.conf and each session's account,
 * /run/onu-pppoeN, which root alone may read (ONU_ROOT, when set, is put in
 * front, for the tests). current prints them as settings does, and for each
 * account PPPOEn_USER and PPPOEn_SECRET_SET, 1 or 0, never the secret.
 * change takes KEY=VALUE lines over them, an account's among them, judges
 * the whole as check does, and writes /run/onu.conf and the accounts that
 * changed, each a new file renamed over the old; it says why when it
 * refuses, and then writes nothing. onu-apply try applies what it wrote.
 * The status daemon has them run through onu-act (ADR 0014).
 *
 * password-check and password-state read root's hash from /run/onu-auth,
 * which root alone may read, so that a program that checks a password need
 * not hold the hash (password.h). password-check exits 0 for the password
 * in force, 3 for the default one, "admin", while none is set, 4 for one
 * kept in the older form, which the caller then keeps again with
 * onu-config password, and 1 for any other.
 *
 * identity-current prints the identity the unit introduces itself with,
 * the name=value lines of /run/onu-identity, which root alone may read,
 * without the PLOAM and the logical passwords: ploam_set and
 * loid_password_set say whether each is set. source= is where the
 * identity came from, of /run/onu-identity-source.
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
#include "password.h"
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
 * Set the KEY=VALUE lines of @in in @payload, and remove the key of each
 * -KEY line. An empty line is skipped; any other line without a valid key,
 * or that does not fit, fails the whole import before anything is written.
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
		if (line[0] == '-') {
			if (!cfg_valid_key(line + 1)) {
				fprintf(stderr, "onu-config: import: not -KEY: %s\n", line);
				err = -EINVAL;
				break;
			}
			cfg_unset(payload, line + 1);
			continue;
		}
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

/* A path of the running system, with ONU_ROOT in front when set. */
static void root_path(const char *path, int n, char *out, size_t cap)
{
	const char *root = getenv("ONU_ROOT");

	if (n)
		snprintf(out, cap, "%s%s%d", root ? root : "", path, n);
	else
		snprintf(out, cap, "%s%s", root ? root : "", path);
}

#define CONF_PATH "/run/onu.conf"
#define ACCOUNT_PATH "/run/onu-pppoe"

/*
 * The settings in force: /run/onu.conf, then each session's account, "USER
 * SECRET"; a line that is not taken is left out, and said.
 */
static void settings_in_force(struct settings *s)
{
	static char text[SETTINGS_TEXT_MAX];
	char path[PATH_MAX], err[160], *line, *save = NULL;
	char acct[SETTINGS_USER_MAX + SETTINGS_SECRET_MAX + 8];
	char user[SETTINGS_USER_MAX + 1], secret[SETTINGS_SECRET_MAX + 1];
	char kv[SETTINGS_SECRET_MAX + 32];
	size_t len = 0;
	FILE *f;

	settings_defaults(s);
	root_path(CONF_PATH, 0, path, sizeof(path));
	f = fopen(path, "r");
	if (f) {
		len = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
	}
	text[len] = '\0';
	for (line = strtok_r(text, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save))
		if (settings_line(s, line, err, sizeof(err)))
			fprintf(stderr, "onu-config: %s; left out\n", err);
	for (int n = 1; n <= SETTINGS_SESSIONS; n++) {
		root_path(ACCOUNT_PATH, n, path, sizeof(path));
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(acct, sizeof(acct), f) &&
		    sscanf(acct, "%63s %255s", user, secret) == 2) {
			snprintf(kv, sizeof(kv), "PPPOE%d_USER=%s", n, user);
			(void)settings_line(s, kv, err, sizeof(err));
			snprintf(kv, sizeof(kv), "PPPOE%d_SECRET=%s", n, secret);
			(void)settings_line(s, kv, err, sizeof(err));
		}
		fclose(f);
	}
	explicit_bzero(text, sizeof(text));
	explicit_bzero(acct, sizeof(acct));
	explicit_bzero(secret, sizeof(secret));
	explicit_bzero(kv, sizeof(kv));
}

static int current_cmd(void)
{
	static char out[SETTINGS_TEXT_MAX];
	struct settings s;
	int rc = 1;

	settings_in_force(&s);
	settings_settle(&s);
	if (settings_format(&s, out, sizeof(out)) >= sizeof(out)) {
		fprintf(stderr, "onu-config: the settings are too long\n");
		goto out;
	}
	fputs(out, stdout);
	for (int n = 0; n < SETTINGS_SESSIONS; n++)
		if (*s.acct[n].user)
			printf("PPPOE%d_USER=%s\nPPPOE%d_SECRET_SET=%d\n", n + 1,
			       s.acct[n].user, n + 1, *s.acct[n].secret != '\0');
	rc = fflush(stdout) ? 1 : 0;
out:
	explicit_bzero(&s, sizeof(s));
	return rc;
}

/* Write @text to @path through a new file renamed over it, with @mode. */
static int write_file(const char *path, const char *text, mode_t mode)
{
	char tmp[PATH_MAX + 8];
	size_t len = strlen(text);
	int fd, err = 0;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return -errno;
	if (fchmod(fd, mode))
		err = -errno;
	else if (write(fd, text, len) != (ssize_t)len)
		err = -EIO;
	if (close(fd) && !err)
		err = -errno;
	if (!err && rename(tmp, path))
		err = -errno;
	if (err)
		unlink(tmp);
	return err;
}

static int change_cmd(void)
{
	static char text[SETTINGS_TEXT_MAX], out[SETTINGS_TEXT_MAX + 64];
	static struct settings s, before;
	char err[160], path[PATH_MAX], *line, *save = NULL;
	char acct[SETTINGS_USER_MAX + SETTINGS_SECRET_MAX + 4];
	size_t len = fread(text, 1, sizeof(text), stdin);
	int rc = 1, e;

	if (len == sizeof(text)) {
		fprintf(stderr, "onu-config: the settings are too long\n");
		goto out;
	}
	text[len] = '\0';
	settings_in_force(&s);
	before = s;
	for (line = strtok_r(text, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		if (settings_line(&s, line, err, sizeof(err))) {
			fprintf(stderr, "onu-config: %s\n", err);
			goto out;
		}
	}
	settings_settle(&s);
	if (!settings_coherent(&s, err, sizeof(err))) {
		fprintf(stderr, "onu-config: %s\n", err);
		goto out;
	}
	len = (size_t)snprintf(out, sizeof(out),
			       "# The settings of the data path, of onu-config change.\n");
	if (len + settings_format(&s, out + len, sizeof(out) - len) >= sizeof(out)) {
		fprintf(stderr, "onu-config: the settings are too long\n");
		goto out;
	}

	/* The accounts that changed, then the settings. */
	for (int n = 0; n < SETTINGS_SESSIONS; n++) {
		const struct settings_account *a = &s.acct[n], *b = &before.acct[n];

		if (!strcmp(a->user, b->user) && !strcmp(a->secret, b->secret))
			continue;
		root_path(ACCOUNT_PATH, n + 1, path, sizeof(path));
		if (*a->user) {
			snprintf(acct, sizeof(acct), "%s %s\n", a->user, a->secret);
			e = write_file(path, acct, 0600);
		} else {
			e = unlink(path) && errno != ENOENT ? -errno : 0;
		}
		if (e) {
			fprintf(stderr, "onu-config: %s: %s\n", path, strerror(-e));
			rc = 2;
			goto out;
		}
	}
	root_path(CONF_PATH, 0, path, sizeof(path));
	e = write_file(path, out, 0644);
	if (e) {
		fprintf(stderr, "onu-config: %s: %s\n", path, strerror(-e));
		rc = 2;
		goto out;
	}
	rc = 0;
out:
	explicit_bzero(&s, sizeof(s));
	explicit_bzero(&before, sizeof(before));
	explicit_bzero(text, sizeof(text));
	explicit_bzero(acct, sizeof(acct));
	return rc;
}

#define AUTH_PATH "/run/onu-auth"

/* The hash in force, the first word of /run/onu-auth, or "" for the default. */
static void hash_in_force(char *out, size_t cap)
{
	char path[PATH_MAX];
	FILE *f;

	out[0] = '\0';
	root_path(AUTH_PATH, 0, path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return;
	if (!fgets(out, (int)cap, f))
		out[0] = '\0';
	fclose(f);
	out[strcspn(out, "\r\n ")] = '\0';
}

static int password_state_cmd(void)
{
	char h[256];

	hash_in_force(h, sizeof(h));
	puts(*h ? "set" : "default");
	explicit_bzero(h, sizeof(h));
	return fflush(stdout) ? 2 : 0;
}

static int password_check_cmd(void)
{
	static const char deflt[] = "admin";
	char secret[256], h[256];
	unsigned char diff;
	int rc = 1;

	if (!fgets(secret, sizeof(secret), stdin))
		secret[0] = '\0';
	secret[strcspn(secret, "\r\n")] = '\0';
	hash_in_force(h, sizeof(h));
	if (!*h) {
		/* The default password, compared without a shortcut either way. */
		diff = strlen(secret) != sizeof(deflt) - 1;
		for (size_t i = 0; i < sizeof(deflt) - 1; i++)
			diff |= (unsigned char)(secret[i % sizeof(secret)] ^ deflt[i]);
		rc = diff ? 1 : 3;
	} else {
		switch (password_verify(secret, h)) {
		case PASSWORD_MATCHES:
			rc = 0;
			break;
		case PASSWORD_OLDER:
			rc = 4;
			break;
		default:
			rc = 1;
		}
	}
	explicit_bzero(secret, sizeof(secret));
	explicit_bzero(h, sizeof(h));
	return rc;
}

#define IDENTITY_PATH "/run/onu-identity"
#define IDENTITY_SOURCE_PATH "/run/onu-identity-source"

static int identity_current_cmd(void)
{
	static const char *const secret[] = { "ploam_password", "loid_password" };
	static const char *const said[] = { "ploam_set", "loid_password_set" };
	bool set[2] = { false, false };
	char path[PATH_MAX], line[512], source[32] = "";
	FILE *f;

	root_path(IDENTITY_PATH, 0, path, sizeof(path));
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		bool hidden = false;

		line[strcspn(line, "\r\n")] = '\0';
		for (int i = 0; i < 2; i++) {
			size_t n = strlen(secret[i]);

			if (!strncmp(line, secret[i], n) && line[n] == '=') {
				hidden = true;
				set[i] = line[n + 1] != '\0';
			}
		}
		if (!hidden && strchr(line, '='))
			puts(line);
	}
	if (f)
		fclose(f);
	explicit_bzero(line, sizeof(line));
	for (int i = 0; i < 2; i++)
		printf("%s=%d\n", said[i], set[i]);
	root_path(IDENTITY_SOURCE_PATH, 0, path, sizeof(path));
	f = fopen(path, "r");
	if (f) {
		if (fgets(source, sizeof(source), f))
			source[strcspn(source, "\r\n")] = '\0';
		fclose(f);
	}
	printf("source=%s\n", source);
	return fflush(stdout) ? 2 : 0;
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
			      !strcmp(argv[optind], "settings") ||
			      !strcmp(argv[optind], "current") ||
			      !strcmp(argv[optind], "change") ||
			      !strcmp(argv[optind], "password-check") ||
			      !strcmp(argv[optind], "password-state") ||
			      !strcmp(argv[optind], "identity-current"))) {
		if (argc - optind != 1) {
			fputs("usage: onu-config check | settings | current | change"
			      " | password-check | password-state | identity-current\n",
			      stderr);
			return 2;
		}
		if (!strcmp(argv[optind], "password-check"))
			return password_check_cmd();
		if (!strcmp(argv[optind], "password-state"))
			return password_state_cmd();
		if (!strcmp(argv[optind], "identity-current"))
			return identity_current_cmd();
		if (!strcmp(argv[optind], "current"))
			return current_cmd();
		if (!strcmp(argv[optind], "change"))
			return change_cmd();
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
