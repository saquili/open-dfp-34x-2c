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
 *   onu-config defaults                      the default of every key, as
 *                                            KEY=VALUE lines: the table of
 *                                            settings.h is where they are
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
 *   onu-config password-state                "set", "default" while the
 *                                            default password is in force,
 *                                            or "unknown" without a store
 *                                            that could be read
 *   onu-config identity-current              the identity in force, without
 *                                            its passwords
 *   onu-config slots DEVICE                  print the lines of the store of
 *                                            before, the two slots of the
 *                                            partition (cfgstore.h); exit 3
 *                                            when it holds none
 *   onu-config erase-slots DEVICE            erase the partition of that
 *                                            store, and refuse any other
 *   onu-config load|save|identity|password|auth HASH|system|pd N|tls|ssh
 *                                            the files the running system
 *                                            reads, to and from the store:
 *                                            onu-config.sh, in ONU_LIB
 *                                            (/usr/lib/onu), carries them out
 *
 * The store is a file of KEY=VALUE lines, at most 64 KiB:
 * /config/open-dfp-34x-2c.conf, in the JFFS2 file system of the flash
 * partition "config", which the shipped firmware keeps its own files in
 * (ADR 0016), unless --store or ONU_STORE names another. A file
 * that is missing is an empty store. A change writes the whole store to a
 * new file beside it, which it then renames over it, so that a power loss in
 * the middle keeps the old one; a change that leaves the store as it was
 * writes nothing. A store that is not a file of such lines is refused.
 * Changes are made one at a time, under a lock of the store's directory.
 * The partition is mounted read-only: a change finds the directory on a
 * file system mounted read-only, mounts it read-write for the write alone,
 * and read-only again after it.
 *
 * Builds before 2026-10-11 kept the store in the whole partition, two slots
 * of its own format, where the shipped firmware's file system had been.
 * slots and erase-slots let onu-config load take such a store into the file
 * before the partition is mounted, since JFFS2 would erase what it does not
 * know.
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
 * /run/onu-pppoeN, which only root and pppoed may read (onu-apply gives a
 * session's file to the group pppoed; ONU_ROOT, when set, is put in
 * front, for the tests). current prints them as settings does, and for each
 * account PPPOEn_USER and PPPOEn_SECRET_SET, 1 or 0, never the secret.
 * change takes KEY=VALUE lines over them, an account's among them, judges
 * the whole as check does, and writes the settings and the accounts that
 * changed into /run/onu-pending, which onu-apply try puts in force; it says
 * why when it refuses, and then writes nothing. It refuses as well a session whose VLAN,
 * or the VLAN it hands down in, is a management VLAN in use, of
 * /run/onu-mgmt or of a pair on trial, whose frames the switch keeps
 * between the host side and the stick. onu-apply try applies what it wrote.
 * The status daemon has them run through onu-act (ADR 0011).
 *
 * password-check and password-state read root's hash from /run/onu-auth,
 * which root alone may read, so that a program that checks a password need
 * not hold the hash (password.h). password-check exits 0 for the password
 * in force, 3 for the default one, "admin", while none is set, 4 for one
 * kept in the older form, which the caller then keeps again with
 * onu-config password, 5 for any while the password is not known, when
 * onu-config load found no store it could read and wrote "!" there, and 1
 * for any other.
 *
 * identity-current prints the identity the unit introduces itself with,
 * the name=value lines of /run/onu-identity, which root alone may read,
 * without the PLOAM and the logical passwords: ploam_set and
 * loid_password_set say whether each is set. source= is where the
 * identity came from, of /run/onu-identity-source.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>

#include <mtd/mtd-user.h>

#include "cfgstore.h"
#include "password.h"
#include "settings.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The store, unless --store or ONU_STORE names another, and its most. */
#define STORE_PATH "/config/open-dfp-34x-2c.conf"
#define STORE_MAX 65536u

/* A partition of the store of before at most this big. */
#define REGION_MAX (1u << 20)

static int fail(const char *what)
{
	fprintf(stderr, "onu-config: %s: %s\n", what, strerror(errno));
	return 1;
}

/*
 * The store @path into @payload, of @cap bytes, the NUL included, every
 * line ended by a newline: 0, 0 with nothing for a missing file, -EFBIG for
 * one of more than @cap - 1 bytes, -EILSEQ for one that is not KEY=VALUE
 * lines, or -errno.
 */
static int read_store(const char *path, char *payload, size_t cap)
{
	struct stat st;
	size_t len = 0;
	ssize_t n;
	int fd, err = 0;

	payload[0] = '\0';
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? 0 : -errno;
	if (fstat(fd, &st) != 0) {
		err = -errno;
	} else if (!S_ISREG(st.st_mode)) {
		err = -EILSEQ;
	} else if ((uintmax_t)st.st_size >= cap) {
		err = -EFBIG;
	} else {
		while ((n = read(fd, payload + len, cap - 1 - len)) > 0)
			len += (size_t)n;
		if (n < 0)
			err = -errno;
	}
	close(fd);
	if (err)
		return err;
	payload[len] = '\0';
	if (strlen(payload) != len)
		return -EILSEQ;
	if (len && payload[len - 1] != '\n') {
		if (len + 1 >= cap)
			return -EFBIG;
		payload[len++] = '\n';
		payload[len] = '\0';
	}
	for (char *line = payload; *line;) {
		char *nl = strchr(line, '\n'), *eq = strchr(line, '=');
		char key[CFG_KEY_MAX + 1];

		if (nl == line) {
			line++;
			continue;
		}
		if (!eq || eq > nl || (size_t)(eq - line) > CFG_KEY_MAX)
			return -EILSEQ;
		memcpy(key, line, (size_t)(eq - line));
		key[eq - line] = '\0';
		if (!cfg_valid_key(key))
			return -EILSEQ;
		line = nl + 1;
	}
	return 0;
}

/* The directory of the store @path, in @dir of @cap bytes. */
static void store_dir(const char *path, char *dir, size_t cap)
{
	char *slash;

	snprintf(dir, cap, "%s", path);
	slash = strrchr(dir, '/');
	if (slash)
		*(slash == dir ? slash + 1 : slash) = '\0';
	else
		snprintf(dir, cap, ".");
}

/*
 * Take the lock of the store's directory @dir: a socket of the abstract
 * namespace bound to a name made from @dir, which one process alone can hold
 * and the kernel lets go of when the process ends, as the stick's kernel has
 * no file locks (CONFIG_FILE_LOCKING). A change that finds it held waits, 50
 * ms at a time, for up to 10 s. Return: the socket to close to let go, or
 * -errno.
 */
#define LOCK_TRIES 200
#define LOCK_WAIT_US 50000

static int store_lock(const char *dir)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int n, err = -EAGAIN;

	if (fd < 0)
		return -errno;
	n = snprintf(sa.sun_path + 1, sizeof(sa.sun_path) - 1, "onu-config:%s",
		     dir);
	if (n < 0 || (size_t)n >= sizeof(sa.sun_path) - 1)
		n = (int)sizeof(sa.sun_path) - 2;
	for (int i = 0; i < LOCK_TRIES; i++) {
		if (bind(fd, (const struct sockaddr *)&sa,
			 (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
				     1 + (size_t)n)) == 0)
			return fd;
		err = -errno;
		if (err != -EADDRINUSE)
			break;
		usleep(LOCK_WAIT_US);
	}
	close(fd);
	return err;
}

/*
 * Write @payload as the store @path: to the file @path.new, read and write
 * for root alone, then renamed over @path, and the directory synced, so that
 * the store is the old one or the new one whole. 0 or -errno.
 */
static int write_store(const char *path, const char *payload)
{
	char tmp[PATH_MAX], dir[PATH_MAX];
	size_t len = strlen(payload), off = 0;
	int fd, d, err = 0;

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.new", path) >= sizeof(tmp))
		return -ENAMETOOLONG;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;
	while (off < len) {
		ssize_t n = write(fd, payload + off, len - off);

		if (n < 0) {
			err = -errno;
			break;
		}
		off += (size_t)n;
	}
	if (!err && fsync(fd) != 0)
		err = -errno;
	if (close(fd) != 0 && !err)
		err = -errno;
	if (!err && rename(tmp, path) != 0)
		err = -errno;
	if (err) {
		unlink(tmp);
		return err;
	}
	store_dir(path, dir, sizeof(dir));
	d = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (d >= 0) {
		fsync(d);
		close(d);
	}
	return 0;
}

/* The flags /config is mounted with besides read-only (onu-config.sh). */
#define STORE_MOUNT_FLAGS (MS_NOSUID | MS_NODEV | MS_NOEXEC)

/*
 * Write @payload as the store @path, as write_store() does. When the store's
 * directory is on a file system mounted read-only, the directory being its
 * mount point, mount it read-write for the write, and read-only again after
 * it; a file system that stays read-write is only reported. 0 or -errno.
 */
static int change_store(const char *path, const char *payload)
{
	char dir[PATH_MAX];
	struct statvfs vfs;
	bool remounted = false;
	int err;

	store_dir(path, dir, sizeof(dir));
	if (statvfs(dir, &vfs) == 0 && (vfs.f_flag & ST_RDONLY)) {
		if (mount(NULL, dir, NULL, MS_REMOUNT | STORE_MOUNT_FLAGS, NULL) != 0)
			return -errno;
		remounted = true;
	}
	err = write_store(path, payload);
	if (remounted &&
	    mount(NULL, dir, NULL, MS_REMOUNT | MS_RDONLY | STORE_MOUNT_FLAGS, NULL) != 0)
		fprintf(stderr, "onu-config: %s stays read-write: %s\n", dir,
			strerror(errno));
	return err;
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

/*
 * The longest set of settings that check and settings take: as much as the
 * store holds.
 */
#define SETTINGS_TEXT_MAX STORE_MAX

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
		"load", "save", "identity", "password", "auth", "system", "pd",
		"tls", "ssh", "token",
	};

	for (size_t i = 0; i < ARRAY_SIZE(verbs); i++)
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

/*
 * slots and erase-slots on @dev, an MTD device or, for the tests, a file of
 * its bytes: the store of before printed, or its partition erased, every
 * byte 0xff then, as the flash erases. 0, 3 when @dev holds no such store, 1
 * or 2 on an error.
 */
static int slots_cmd(const char *dev, bool erase)
{
	struct mtd_info_user mi;
	struct stat st;
	uint8_t *region = NULL;
	char *payload = NULL;
	size_t len = 0;
	int fd, rc = 1, err;
	bool mtd;

	fd = open(dev, (erase ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (fd < 0)
		return fail(dev);
	mtd = ioctl(fd, MEMGETINFO, &mi) == 0;
	if (mtd)
		len = mi.size;
	else if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
		len = (size_t)st.st_size;
	if (len == 0 || len > REGION_MAX || cfg_payload_cap(len) == 0) {
		fprintf(stderr, "onu-config: %s: not a partition of the store of before\n", dev);
		close(fd);
		return 2;
	}
	region = malloc(len);
	payload = malloc(cfg_payload_cap(len) + 1);
	if (!region || !payload) {
		fail("out of memory");
		goto out;
	}
	if (pread(fd, region, len, 0) != (ssize_t)len) {
		fail(dev);
		goto out;
	}
	err = cfg_load(region, len, payload, cfg_payload_cap(len) + 1, NULL, NULL, NULL);
	if (err == -ENOENT) {
		rc = 3;
		goto out;
	}
	if (err) {
		fprintf(stderr, "onu-config: %s: %s\n", dev, strerror(-err));
		goto out;
	}
	if (!erase) {
		fputs(payload, stdout);
		rc = fflush(stdout) ? 1 : 0;
		goto out;
	}
	if (mtd) {
		struct erase_info_user e = { .start = 0, .length = mi.size };

		if (ioctl(fd, MEMERASE, &e) != 0) {
			fail(dev);
			goto out;
		}
	} else {
		memset(region, 0xff, len);
		if (pwrite(fd, region, len, 0) != (ssize_t)len) {
			fail(dev);
			goto out;
		}
	}
	rc = 0;
out:
	free(region);
	free(payload);
	close(fd);
	return rc;
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
/* onu-mgmt's management pair in use, and a pair on trial (onu-mgmt). */
#define MGMT_PATH "/run/onu-mgmt"
#define MGMT_PENDING_PATH "/run/onu-trial.d/mgmt"

/*
 * The management pair of @path: its VLAN, the line VLAN=, or 0, into @vid,
 * and its address with the prefix length, the line ADDRESS=, or "", into
 * @addr.
 */
static void mgmt_pair_of(const char *path, unsigned int *vid, char *addr,
			 size_t cap)
{
	char p[PATH_MAX], line[96];
	FILE *f;

	*vid = 0;
	*addr = '\0';
	root_path(path, 0, p, sizeof(p));
	f = fopen(p, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "VLAN=", 5))
			*vid = (unsigned int)strtoul(line + 5, NULL, 10);
		else if (!strncmp(line, "ADDRESS=", 8))
			snprintf(addr, cap, "%s", line + 8);
	}
	if (f)
		fclose(f);
}

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

static int defaults_cmd(void)
{
	static char out[SETTINGS_TEXT_MAX];

	if (settings_format_defaults(out, sizeof(out)) >= sizeof(out)) {
		fprintf(stderr, "onu-config: the defaults are too long\n");
		return 1;
	}
	fputs(out, stdout);
	return fflush(stdout) ? 1 : 0;
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

/*
 * A change waits in PENDING_PATH until onu-apply try puts it in force,
 * under its lock, once it has kept the settings it goes back to: written
 * straight into /run/onu.conf, a change would be in force for an onu-apply
 * sync that came between, without a trial to undo it. The directory holds
 * onu.conf and each account that changed, onu-pppoeN, empty for one
 * removed; a new change replaces the whole, made aside and renamed.
 */
#define PENDING_PATH "/run/onu-pending"

/* The files of the pending change @dir, and @dir, gone. */
static void pending_remove(const char *dir)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/onu.conf", dir);
	unlink(path);
	for (int n = 1; n <= SETTINGS_SESSIONS; n++) {
		snprintf(path, sizeof(path), "%s/onu-pppoe%d", dir, n);
		unlink(path);
	}
	rmdir(dir);
}

static int change_cmd(void)
{
	static char text[SETTINGS_TEXT_MAX], out[SETTINGS_TEXT_MAX + 64];
	static struct settings s, before;
	char err[160], path[PATH_MAX], dir[PATH_MAX], tmp[PATH_MAX + 8] = "";
	char *line, *save = NULL;
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
	mgmt_pair_of(MGMT_PATH, &s.mgmt_vlan[0], s.mgmt_address[0],
		     sizeof(s.mgmt_address[0]));
	mgmt_pair_of(MGMT_PENDING_PATH, &s.mgmt_vlan[1], s.mgmt_address[1],
		     sizeof(s.mgmt_address[1]));
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

	/* The settings and the accounts that changed, aside, then pending. */
	root_path(PENDING_PATH, 0, dir, sizeof(dir));
	snprintf(tmp, sizeof(tmp), "%s.new", dir);
	pending_remove(tmp);
	if (mkdir(tmp, 0700)) {
		fprintf(stderr, "onu-config: %s: %s\n", tmp, strerror(errno));
		rc = 2;
		goto out;
	}
	for (int n = 0; n < SETTINGS_SESSIONS; n++) {
		const struct settings_account *a = &s.acct[n], *b = &before.acct[n];

		if (!strcmp(a->user, b->user) && !strcmp(a->secret, b->secret))
			continue;
		snprintf(path, sizeof(path), "%s/onu-pppoe%d", tmp, n + 1);
		acct[0] = '\0';
		if (*a->user)
			snprintf(acct, sizeof(acct), "%s %s\n", a->user, a->secret);
		e = write_file(path, acct, 0600);
		if (e) {
			fprintf(stderr, "onu-config: %s: %s\n", path, strerror(-e));
			rc = 2;
			goto out;
		}
	}
	snprintf(path, sizeof(path), "%s/onu.conf", tmp);
	e = write_file(path, out, 0644);
	if (!e) {
		pending_remove(dir);
		if (rename(tmp, dir))
			e = -errno;
	}
	if (e) {
		fprintf(stderr, "onu-config: %s: %s\n", dir, strerror(-e));
		rc = 2;
		goto out;
	}
	rc = 0;
out:
	if (rc && *tmp)
		pending_remove(tmp);
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
	puts(!strcmp(h, "!") ? "unknown" : *h ? "set" : "default");
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
	if (!strcmp(h, "!")) {
		/* No store could be read: no password is taken. */
		rc = 5;
	} else if (!*h) {
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
	int opt;
	static const struct option opts[] = {
		{ "store", required_argument, NULL, 's' },
		{ NULL, 0, NULL, 0 },
	};

	while ((opt = getopt_long(argc, argv, "s:", opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			store = optarg;
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
			      !strcmp(argv[optind], "defaults") ||
			      !strcmp(argv[optind], "change") ||
			      !strcmp(argv[optind], "password-check") ||
			      !strcmp(argv[optind], "password-state") ||
			      !strcmp(argv[optind], "identity-current"))) {
		if (argc - optind != 1) {
			fputs("usage: onu-config check | settings | current | defaults | change | password-check | password-state | identity-current\n",
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
		if (!strcmp(argv[optind], "defaults"))
			return defaults_cmd();
		if (!strcmp(argv[optind], "change"))
			return change_cmd();
		return settings_cmd(!strcmp(argv[optind], "check"));
	}
	if (argc - optind == 2 && (!strcmp(argv[optind], "slots") ||
				   !strcmp(argv[optind], "erase-slots")))
		return slots_cmd(argv[optind + 1], !strcmp(argv[optind], "erase-slots"));
	if (!store || !*store)
		store = STORE_PATH;

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
		fputs("usage: onu-config [--store PATH] get KEY | list |\n"
		      "                  set KEY VALUE | unset KEY | import\n"
		      "       onu-config check | settings\n"
		      "       onu-config slots DEVICE | erase-slots DEVICE\n"
		      "       onu-config load | save | identity | password |\n"
		      "                  auth HASH | system | pd N | tls | ssh\n"
		      "       onu-config token [new | off | install]\n", stderr);
		return 2;
	}

	size_t cap = STORE_MAX + 1;
	char *payload = calloc(1, cap);
	/* What the store holds now, to tell whether a change changes anything. */
	char *before = NULL;
	char dir[PATH_MAX];
	int err, lock = -1;

	if (!payload)
		return fail("out of memory");
	/*
	 * A change reads, changes and writes the store under a lock of its
	 * directory, so that two changes at once do not lose one of them.
	 */
	if (writing) {
		store_dir(store, dir, sizeof(dir));
		lock = store_lock(dir);
		if (lock < 0) {
			fprintf(stderr, "onu-config: %s: no lock: %s\n", dir,
				strerror(-lock));
			goto out_fail;
		}
	}
	err = read_store(store, payload, cap);
	if (err) {
		fprintf(stderr, "onu-config: %s: %s\n", store,
			err == -EFBIG ? "larger than 64 KiB" :
			err == -EILSEQ ? "not a store of KEY=VALUE lines" : strerror(-err));
		goto out_fail;
	}
	if (writing) {
		before = strdup(payload);
		if (!before) {
			fail("out of memory");
			goto out_fail;
		}
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

	if (writing && strcmp(before, payload)) {
		err = change_store(store, payload);
		if (err) {
			fprintf(stderr, "onu-config: %s: %s\n", store, strerror(-err));
			goto out_fail;
		}
	}

	if (lock >= 0)
		close(lock);
	free(before);
	free(payload);
	return rc;

out_fail:
	if (lock >= 0)
		close(lock);
	free(before);
	free(payload);
	return 1;
}
