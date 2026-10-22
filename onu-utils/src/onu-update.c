// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-update: the two banks of the stick's flash and the updates that go
 * into them.
 *
 *   onu-update status          the banks, as KEY=VALUE lines: each one's
 *                              partitions' sizes and what its images use,
 *                              and its SHA-256 and signature once read; and
 *                              the keys whose signatures the stick trusts
 *   onu-update check FILE      check an update file (update.h), writing
 *                              nothing
 *   onu-update write FILE [BANK]
 *                              write an update file into the bank the stick
 *                              did not start from, and read it back; BANK,
 *                              0 or 1, names it after a start from memory
 *   onu-update read [BANK]     an update file of a bank, the running one by
 *                              default, on the standard output
 *   onu-update digest [BANK]   read a bank, or each that holds a system,
 *                              for its SHA-256 and its signature, at the
 *                              lowest priority
 *   onu-update try [BANK]      have the boot loader start BANK, the other one
 *                              by default, once, at the next start
 *   onu-update commit          have the boot loader start the running bank
 *                              from now on
 *   onu-update return [BANK]   have the boot loader start the shipped
 *                              firmware of BANK, the other one by default,
 *                              from now on
 *   onu-update backup          the whole flash, every partition in its turn,
 *                              on the standard output
 *   onu-update env [KEY | KEY=VALUE]...
 *                              print the boot loader's environment, some of
 *                              its variables, or change them in one save
 *
 * FILE "-" is the standard input, so an update can stream in without being
 * held whole: the stick has some 25 MB of memory.
 *
 * A release carries a signature behind its root filesystem (update.h),
 * which check, write, read and digest check with the keys of
 * /etc/onu-update.keys, one a line: "ed25519", the raw public key in base64
 * and the key's name. They say SIGNATURE=valid, with SIGNER=, the key's
 * name; none, for a file that carries no signature, such as one built from
 * the source by anyone; unknown, for one made with a key the stick does not
 * trust; or bad, for one that does not match the file, which check refuses
 * and write does not write. They say SHA256=, that of the whole file, which
 * a release's SHA256SUMS lists, too. Reading a bank takes some 3 s, so what
 * write and read found, and digest, which the boot script runs once, is kept
 * in /run/onu-update for the status, with a mark of what the bank held; a
 * change of the bank, or a start, forgets it.
 *
 * Bank 0 is the partitions k0 and r0, bank 1 k1 and r1. The bank the stick
 * started from is the one whose root filesystem the kernel mounted: the boot
 * loader passes root=31:5 for bank 0 and root=31:7 for bank 1, the numbers of
 * r0 and r1. After a start from memory, whose root filesystem is not
 * SquashFS, no bank runs, whatever root= says. The running bank is never
 * written.
 *
 * The boot loader starts the bank in sw_commit of its environment
 * (bootenv.h), or, when sw_tryactive holds 0 or 1, that bank once, with the
 * watchdog running: it sets sw_tryactive back to 2 before it starts it, so
 * that a system that does not come up is restarted by the watchdog into the
 * committed bank. The system of the tried bank makes it the committed one
 * (commit) once it is up.
 *
 * A bank being written is marked sw_valid<N>=0 first and sw_valid<N>=1,
 * sw_version<N>=VERSION when it has been read back whole, so a write cut
 * short leaves a bank that try refuses. try also sets onu_trial=<N>, which
 * the boot loader leaves alone and commit removes: a start from another bank
 * while it is set, after the loader took the trial, tells that the tried
 * system did not come up (FAILED in the status). ONU_ROOT, when set, is put in front
 * of every path (/proc/mtd, /proc/cmdline, /proc/device-tree, /etc/os-release
 * and the devices), for the tests: there the partitions are files.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <mtd/mtd-user.h>
#include <zlib.h>

#include "base64.h"
#include "bootenv.h"
#include "sha256.h"
#include "update.h"

#define CHUNK 0x10000u
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The keys whose signatures this system trusts, and what it read of each bank. */
#define KEYS "/etc/onu-update.keys"
#define KEYS_MAX 8u
#define KEY_NAME 64u
#define DIGESTS "/run/onu-update"

static const char *root = "";

static int say(const char *fmt, ...) __attribute((format(printf, 1, 2)));

static int say(const char *fmt, ...)
{
	va_list ap;

	fputs("onu-update: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	return -1;
}

static void at_root(char *out, size_t cap, const char *path)
{
	snprintf(out, cap, "%s%s", root, path);
}

/* The first line of the file at @path (under the root) in @out, or -1. */
static int read_text(const char *path, char *out, size_t cap)
{
	char full[512];
	FILE *f;
	size_t n;

	at_root(full, sizeof(full), path);
	f = fopen(full, "r");
	if (!f)
		return -1;
	n = fread(out, 1, cap - 1, f);
	fclose(f);
	out[n] = '\0';
	return 0;
}

/* A partition of the flash, or of a file standing in for one. */
struct part {
	int fd;
	bool mtd;
	uint32_t size;
	uint32_t erase;
	uint32_t erased;	/* erased from 0 up to here */
	char name[16];
};

/*
 * Find the partition @name in /proc/mtd: its number, size and erase size.
 * Return: its number, or -1.
 */
static int mtd_find(const char *name, uint32_t *size, uint32_t *erase)
{
	char path[512], line[256], want[40];
	FILE *f;
	int num = -1;

	at_root(path, sizeof(path), "/proc/mtd");
	f = fopen(path, "r");
	if (!f)
		return -1;
	snprintf(want, sizeof(want), "\"%s\"", name);
	while (fgets(line, sizeof(line), f)) {
		unsigned int n, s, e;
		char label[40];

		if (sscanf(line, "mtd%u: %x %x %39s", &n, &s, &e, label) == 4 &&
		    !strcmp(label, want)) {
			num = (int)n;
			*size = s;
			*erase = e;
			break;
		}
	}
	fclose(f);
	return num;
}

static int part_open(struct part *p, const char *name, bool write)
{
	struct mtd_info_user mi;
	char dev[64], path[512];
	int num;

	memset(p, 0, sizeof(*p));
	p->fd = -1;
	snprintf(p->name, sizeof(p->name), "%s", name);
	num = mtd_find(name, &p->size, &p->erase);
	if (num < 0)
		return say("no partition %s in /proc/mtd", name);
	snprintf(dev, sizeof(dev), "/dev/mtd%d", num);
	at_root(path, sizeof(path), dev);
	p->fd = open(path, write ? O_RDWR : O_RDONLY);
	if (p->fd < 0)
		return say("%s (%s): %s", path, name, strerror(errno));
	p->mtd = ioctl(p->fd, MEMGETINFO, &mi) == 0;
	if (p->mtd) {
		p->size = mi.size;
		p->erase = mi.erasesize;
	}
	if (!p->erase || p->size % p->erase)
		return say("%s: erase size %u does not divide size %u", name,
			   p->erase, p->size);
	return 0;
}

static void part_close(struct part *p)
{
	if (p->fd >= 0)
		close(p->fd);
	p->fd = -1;
}

/* Erase [@from, @to) of @p, both on erase blocks. A file is filled with 0xff. */
static int part_erase(struct part *p, uint32_t from, uint32_t to)
{
	if (from >= to)
		return 0;
	if (p->mtd) {
		struct erase_info_user e = { .start = from, .length = to - from };

		if (ioctl(p->fd, MEMERASE, &e) != 0)
			return say("%s: erase %#x+%#x: %s", p->name, from,
				   to - from, strerror(errno));
	} else {
		static uint8_t ff[CHUNK];
		uint32_t off;

		memset(ff, 0xff, sizeof(ff));
		for (off = from; off < to; off += CHUNK) {
			size_t n = to - off < CHUNK ? to - off : CHUNK;

			if (pwrite(p->fd, ff, n, off) != (ssize_t)n)
				return say("%s: %s", p->name, strerror(errno));
		}
	}
	return 0;
}

/* Write @len bytes at @off, erasing the blocks ahead of them first. */
static int part_write(struct part *p, uint32_t off, const void *buf, size_t len)
{
	uint32_t end = off + (uint32_t)len;

	if (end > p->size)
		return say("%s: %u bytes do not fit its %u", p->name, end,
			   p->size);
	if (end > p->erased) {
		uint32_t to = end % p->erase ? end + p->erase - end % p->erase : end;

		if (part_erase(p, p->erased, to))
			return -1;
		p->erased = to;
	}
	if (pwrite(p->fd, buf, len, off) != (ssize_t)len)
		return say("%s: write at %#x: %s", p->name, off, strerror(errno));
	return 0;
}

static int part_read(struct part *p, uint32_t off, void *buf, size_t len)
{
	if (pread(p->fd, buf, len, off) != (ssize_t)len)
		return say("%s: read at %#x: %s", p->name, off, strerror(errno));
	return 0;
}

/* Read up to @len bytes, as many as there are before the end. */
static ssize_t read_full(int fd, void *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = read(fd, (uint8_t *)buf + got, len - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (!n)
			break;
		got += (size_t)n;
	}
	return (ssize_t)got;
}

static int write_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = write(fd, (const uint8_t *)buf + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

/*
 * A key whose signatures the stick trusts: a line of KEYS, the word ed25519,
 * the raw public key in base64 and the key's name, the rest of the line.
 */
struct key {
	uint8_t id[SHA256_LEN];		/* the SHA-256 of the raw key */
	uint8_t raw[UPD_KEY];
	char text[48];			/* the raw key in base64 */
	char name[KEY_NAME];
};

/* The next word of *@p, which it moves past: its length, 0 at the end. */
static size_t word(char **p, char **start)
{
	size_t n;

	*p += strspn(*p, " \t");
	*start = *p;
	n = strcspn(*p, " \t");
	*p += n;
	return n;
}

/* The keys of KEYS in @keys: their number. A line that is no key is said and left out. */
static unsigned int keys_load(struct key *keys)
{
	char path[512], line[256];
	unsigned int n = 0, at = 0;
	FILE *f;

	at_root(path, sizeof(path), KEYS);
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *type, *text, *name;
		size_t tlen, xlen, nlen, i;
		struct key *k = &keys[n];
		struct sha256 s;
		bool ok;

		at++;
		line[strcspn(line, "\r\n")] = '\0';
		tlen = word(&p, &type);
		if (!tlen || *type == '#')
			continue;
		xlen = word(&p, &text);
		name = p + strspn(p, " \t");
		nlen = strlen(name);
		while (nlen && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t'))
			nlen--;
		ok = tlen == 7 && !strncmp(type, "ed25519", 7) && xlen < sizeof(k->text) &&
		     base64_decode(text, xlen, k->raw, UPD_KEY) == UPD_KEY &&
		     nlen && nlen < KEY_NAME;
		for (i = 0; ok && i < nlen; i++)
			ok = name[i] >= 0x20 && name[i] <= 0x7e;
		if (!ok) {
			say("%s, line %u: not \"ed25519 KEY NAME\": left out", KEYS, at);
			continue;
		}
		if (n == KEYS_MAX) {
			say("%s, line %u: more than %u keys: left out", KEYS, at, KEYS_MAX);
			continue;
		}
		snprintf(k->text, sizeof(k->text), "%.*s", (int)xlen, text);
		snprintf(k->name, sizeof(k->name), "%.*s", (int)nlen, name);
		sha256_init(&s);
		sha256_update(&s, k->raw, UPD_KEY);
		sha256_final(&s, k->id);
		n++;
	}
	fclose(f);
	return n;
}

static void hex(const uint8_t *b, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", b[i]);
}

/* What a file, or a bank, is signed with, as the status says it. */
enum sigstate { SIG_NONE, SIG_VALID, SIG_UNKNOWN, SIG_BAD };

static const char *const sig_word[] = { "none", "valid", "unknown", "bad" };

struct verdict {
	enum sigstate state;
	char signer[KEY_NAME];		/* the key's name, for a valid one */
	uint8_t sha[SHA256_LEN];	/* the SHA-256 of the whole file */
};

/*
 * An update file as it goes by, front to back, from a file or out of a bank:
 * the SHA-256 of all of it, which the trailer holds, that of the part a
 * signature signs, up to the SquashFS image's bytes_used, and the signature,
 * when the root filesystem's part ends in one.
 */
struct sink {
	struct sha256 s, signed_part;
	bool at_used;			/* signed_part is taken */
	uint32_t used, rootfs_len;
	uint8_t sig[UPD_SIG];
};

static void sink_init(struct sink *k, const uint8_t raw[UPD_HEADER], uint32_t rootfs_len)
{
	memset(k, 0, sizeof(*k));
	sha256_init(&k->s);
	sha256_update(&k->s, raw, UPD_HEADER);
	k->rootfs_len = rootfs_len;
}

/*
 * @n bytes at @off of the root filesystem's part, which come in order;
 * @k->used, the SquashFS image's bytes_used, is set before the first.
 */
static void sink_rootfs(struct sink *k, uint32_t off, const uint8_t *buf, size_t n)
{
	uint32_t end = off + (uint32_t)n, from, to;

	if (!k->at_used && end >= k->used) {
		size_t head = k->used - off;

		sha256_update(&k->s, buf, head);
		k->signed_part = k->s;
		k->at_used = true;
		sha256_update(&k->s, buf + head, n - head);
	} else {
		sha256_update(&k->s, buf, n);
	}
	if (k->rootfs_len != k->used + UPD_SIG)
		return;
	from = off > k->used ? off : k->used;
	to = end < k->used + UPD_SIG ? end : k->used + UPD_SIG;
	if (from < to)
		memcpy(k->sig + (from - k->used), buf + (from - off), to - from);
}

/*
 * The end: in @digest the SHA-256 that the trailer holds, in @v the
 * SHA-256 of the whole file, that trailer included, and what the signature
 * says, checked with @keys.
 */
static void sink_end(struct sink *k, const struct key *keys, unsigned int nkeys,
		     uint8_t digest[SHA256_LEN], struct verdict *v)
{
	struct sha256 whole = k->s;
	uint8_t part[SHA256_LEN];
	struct upd_sig sig;
	unsigned int i;
	int err;

	memset(v, 0, sizeof(*v));
	sha256_final(&k->s, digest);
	sha256_update(&whole, digest, SHA256_LEN);
	sha256_final(&whole, v->sha);
	v->state = SIG_NONE;
	if (!k->at_used || k->rootfs_len != k->used + UPD_SIG)
		return;
	err = upd_sig_parse(k->sig, &sig);
	if (err == -ENOENT)
		return;
	if (err) {
		/* A signature of a format this stick does not know, or a damaged one. */
		v->state = err == -EPROTONOSUPPORT ? SIG_UNKNOWN : SIG_BAD;
		return;
	}
	v->state = SIG_UNKNOWN;
	sha256_final(&k->signed_part, part);
	for (i = 0; i < nkeys; i++) {
		if (memcmp(keys[i].id, sig.key_id, SHA256_LEN))
			continue;
		if (upd_sig_check(&sig, keys[i].raw, part)) {
			v->state = SIG_BAD;
		} else {
			v->state = SIG_VALID;
			snprintf(v->signer, sizeof(v->signer), "%s", keys[i].name);
		}
		return;
	}
}

/*
 * The bank the stick started from, by root= on the kernel's command line, or
 * -1. A start from memory, whose root filesystem is not SquashFS but the
 * image's own, ran from no bank, whatever the command line says: the boot
 * loader's RAM start passes bank 0's root= too.
 */
static int running_bank(void)
{
	char cmdline[2048], mounts[4096], want[16];
	uint32_t size, erase;
	const char *p;
	int bank;

	if (read_text("/proc/mounts", mounts, sizeof(mounts)))
		return -1;
	for (p = mounts; p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		const char *sp = strchr(p, ' ');

		if (sp && !strncmp(sp, " / ", 3))
			break;
	}
	if (!p || strncmp(strchr(p, ' '), " / squashfs ", 12))
		return -1;
	if (read_text("/proc/cmdline", cmdline, sizeof(cmdline)))
		return -1;
	for (p = cmdline; (p = strstr(p, "root=")); p += 5) {
		if (p != cmdline && p[-1] != ' ')
			continue;
		for (bank = 0; bank < 2; bank++) {
			int num = mtd_find(bank ? "r1" : "r0", &size, &erase);
			size_t len;

			if (num < 0)
				continue;
			len = (size_t)snprintf(want, sizeof(want), "31:%d", num);
			if (!strncmp(p + 5, want, len) &&
			    (p[5 + len] == ' ' || p[5 + len] == '\n' || !p[5 + len]))
				return bank;
		}
	}
	return -1;
}

/* The board, the first entry of the device tree's compatible property. */
static int board_of(char *out, size_t cap)
{
	if (read_text("/proc/device-tree/compatible", out, cap))
		return -1;
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

/* The value of @key at the start of a line of @text, without its quotes. */
static int release_field(const char *text, const char *key, char *out, size_t cap)
{
	size_t klen = strlen(key);
	const char *p;

	for (p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		const char *v, *end;

		if (strncmp(p, key, klen) || p[klen] != '=')
			continue;
		v = p + klen + 1;
		if (*v == '"')
			v++;
		end = v + strcspn(v, "\"\n");
		if (end == v)
			return -1;
		snprintf(out, cap, "%.*s", (int)(end - v), v);
		return 0;
	}
	return -1;
}

/*
 * The version of the running system: VERSION_ID of /etc/os-release, or the
 * BUILD_ID of an image that has none.
 */
static int running_version(char *out, size_t cap)
{
	char text[2048];

	if (read_text("/etc/os-release", text, sizeof(text)))
		return -1;
	if (!release_field(text, "VERSION_ID", out, cap))
		return 0;
	return release_field(text, "BUILD_ID", out, cap);
}

/* The boot loader's environment: its two copies and which is current. */
struct env {
	uint8_t copy[2][BOOTENV_SIZE];
	int cur;
};

static const char *const env_part[2] = { "env", "env2" };

static int env_load(struct env *e)
{
	int i;

	for (i = 0; i < 2; i++) {
		struct part p;
		int err = part_open(&p, env_part[i], false);

		if (!err && p.size < BOOTENV_SIZE)
			err = say("%s is smaller than an environment", p.name);
		if (!err)
			err = part_read(&p, 0, e->copy[i], BOOTENV_SIZE);
		part_close(&p);
		if (err)
			return -1;
	}
	e->cur = bootenv_current(e->copy[0], e->copy[1]);
	if (e->cur < 0)
		return say("neither copy of the boot loader's environment checks out");
	return 0;
}

/*
 * Save @data, a copy whose variables are set, as the boot loader saves: into
 * the copy that is not current, flagged active, read back, and only then the
 * current copy flagged obsolete.
 */
static int env_save(struct env *e, uint8_t *data)
{
	int next = 1 - e->cur;
	uint8_t check[BOOTENV_SIZE], obsolete = BOOTENV_OBSOLETE;
	struct part p;
	int err;

	bootenv_seal(data, BOOTENV_ACTIVE);
	err = part_open(&p, env_part[next], true);
	if (!err)
		err = part_erase(&p, 0, BOOTENV_SIZE);
	if (!err && pwrite(p.fd, data, BOOTENV_SIZE, 0) != BOOTENV_SIZE)
		err = say("%s: write: %s", p.name, strerror(errno));
	if (!err)
		err = part_read(&p, 0, check, BOOTENV_SIZE);
	if (!err && memcmp(check, data, BOOTENV_SIZE))
		err = say("%s does not read back as written", p.name);
	part_close(&p);
	if (err)
		return -1;

	err = part_open(&p, env_part[e->cur], true);
	if (!err && pwrite(p.fd, &obsolete, 1, BOOTENV_FLAG_OFFSET) != 1)
		err = say("%s: flag: %s", p.name, strerror(errno));
	part_close(&p);
	if (err)
		return -1;

	memcpy(e->copy[next], data, BOOTENV_SIZE);
	e->copy[e->cur][BOOTENV_FLAG_OFFSET] = BOOTENV_OBSOLETE;
	e->cur = next;
	return 0;
}

/*
 * Set the variables of @sets, "KEY=VALUE" or "KEY=" to remove one, in one
 * save. A change that leaves the environment as it was saves nothing.
 */
static int env_change(struct env *e, const char *const *sets, int n)
{
	static uint8_t data[BOOTENV_SIZE];
	int i, err;

	memcpy(data, e->copy[e->cur], BOOTENV_SIZE);
	for (i = 0; i < n; i++) {
		char key[128], *eq = strchr(sets[i], '=');
		size_t klen = eq ? (size_t)(eq - sets[i]) : 0;

		if (!eq || !klen || klen >= sizeof(key))
			return say("not KEY=VALUE: %s", sets[i]);
		memcpy(key, sets[i], klen);
		key[klen] = '\0';
		err = bootenv_set(data, key, eq[1] ? eq + 1 : NULL);
		if (err)
			return say("%s: %s", key, strerror(-err));
	}
	if (!memcmp(data + BOOTENV_HEADER, e->copy[e->cur] + BOOTENV_HEADER,
		    BOOTENV_DATA))
		return 0;
	return env_save(e, data);
}

static int env_set_bank(struct env *e, const char *a, const char *b)
{
	const char *sets[2] = { a, b };

	return env_change(e, sets, b ? 2 : 1);
}

static long env_number(struct env *e, const char *key, long otherwise)
{
	const char *v = bootenv_get(e->copy[e->cur], key);
	char *end;
	long n;

	if (!v)
		return otherwise;
	n = strtol(v, &end, 10);
	return *v && !*end ? n : otherwise;
}

/*
 * The shipped firmware splits a bank as 1328 KiB of kernel and the rest
 * root filesystem (boot-contract.rst); the loader names that split at every
 * start, fl_kernel1_sz and fl_kernel2_sz in hexadecimal.
 */
#define SHIPPED_KERNEL_SIZE 0x14c000u

/* Where the shipped firmware's root filesystem would start in bank @n. */
static uint32_t shipped_split(struct env *e, bool have_env, int n)
{
	const char *v = have_env ? bootenv_get(e->copy[e->cur],
					       n ? "fl_kernel2_sz" : "fl_kernel1_sz") : NULL;
	char *end;
	unsigned long sz;

	if (!v)
		return SHIPPED_KERNEL_SIZE;
	sz = strtoul(v, &end, 16);
	return *v && !*end && sz && sz < UINT32_MAX ? (uint32_t)sz : SHIPPED_KERNEL_SIZE;
}

/*
 * What a bank holds: a kernel image and a root filesystem that check out,
 * or the shipped firmware: a kernel image, no root filesystem where this
 * system keeps it, and one where the shipped firmware's split puts it.
 */
struct bank {
	bool kernel, rootfs, shipped;
	bool sig;		/* a signature follows the root filesystem */
	uint32_t kernel_len, rootfs_len, time, data_crc;
	uint32_t kernel_size, rootfs_size;
};

/* Bank @n; with @shipped_at, whether it holds the shipped firmware. */
static void bank_probe(int n, struct bank *b, uint32_t shipped_at)
{
	uint8_t head[SQUASHFS_SUPER];
	uint32_t used;
	struct part p;

	memset(b, 0, sizeof(*b));
	if (!part_open(&p, n ? "k1" : "k0", false)) {
		b->kernel_size = p.size;
		b->kernel = !part_read(&p, 0, head, UIMAGE_HEADER) &&
			    !uimage_check(head, &b->kernel_len, &b->data_crc,
					  &b->time) &&
			    b->kernel_len <= p.size;
		/* Its root filesystem starts inside this system's kernel partition. */
		b->shipped = shipped_at && b->kernel && b->kernel_len <= shipped_at &&
			     (uint64_t)shipped_at + SQUASHFS_SUPER <= p.size &&
			     !part_read(&p, shipped_at, head, SQUASHFS_SUPER) &&
			     !squashfs_check(head, &used);
	}
	part_close(&p);
	if (!part_open(&p, n ? "r1" : "r0", false)) {
		b->rootfs_size = p.size;
		b->rootfs = !part_read(&p, 0, head, SQUASHFS_SUPER) &&
			    !squashfs_check(head, &b->rootfs_len) &&
			    b->rootfs_len <= p.size;
		b->sig = b->rootfs && (uint64_t)b->rootfs_len + UPD_SIG <= p.size &&
			 !part_read(&p, b->rootfs_len, head, 4) &&
			 head[0] == 'O' && head[1] == 'N' && head[2] == 'U' && head[3] == 'S';
	}
	part_close(&p);
	if (b->rootfs)
		b->shipped = false;
}

/*
 * The version of bank @n as its update file carries it: the running
 * system's own, or else the one the boot loader's environment @e, when
 * @have_env, keeps for it, or "unknown".
 */
static void bank_version(int n, int running, struct env *e, bool have_env,
			 char *out, size_t cap)
{
	const char *v = NULL;
	char key[24];

	if (n == running && !running_version(out, cap))
		return;
	snprintf(key, sizeof(key), "sw_version%d", n);
	if (have_env)
		v = bootenv_get(e->copy[e->cur], key);
	snprintf(out, cap, "%s", v && *v ? v : "unknown");
}

/* The header of bank @n, which @b holds a system, as its update file: 0, or -1. */
static int bank_header(int n, const struct bank *b, int running, struct env *e,
		       bool have_env, struct upd_header *h, uint8_t raw[UPD_HEADER])
{
	memset(h, 0, sizeof(*h));
	h->kernel_len = b->kernel_len;
	h->rootfs_len = b->rootfs_len + (b->sig ? UPD_SIG : 0);
	h->time = b->time;
	if (board_of(h->board, sizeof(h->board)))
		return say("no /proc/device-tree/compatible: the board is not known");
	bank_version(n, running, e, have_env, h->version, sizeof(h->version));
	upd_build(raw, h);
	return 0;
}

/*
 * What a bank has read as is kept under DIGESTS, one file a bank, with the
 * mark of what the bank held then: its kernel image's lengths, CRC and time,
 * its root filesystem's length, whether a signature follows it, and its
 * version. The status gives the reading while the mark is the bank's; a
 * write forgets it, and /run is empty at each start.
 */
static void bank_mark(const struct bank *b, const char *version, char *out, size_t cap)
{
	snprintf(out, cap, "%08" PRIx32 "-%" PRIu32 "-%" PRIu32 "-%" PRIu32 "-%d-%s",
		 b->data_crc, b->kernel_len, b->rootfs_len, b->time, b->sig, version);
}

static void digest_path(int n, char *out, size_t cap)
{
	char rel[64];

	snprintf(rel, sizeof(rel), DIGESTS "/bank%d", n);
	at_root(out, cap, rel);
}

static void digest_keep(int n, const char *mark, const struct verdict *v)
{
	char dir[512], path[512], tmp[520], sha[2 * SHA256_LEN + 1];
	FILE *f;

	at_root(dir, sizeof(dir), DIGESTS);
	if (mkdir(dir, 0755) && errno != EEXIST)
		return;
	digest_path(n, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	hex(v->sha, SHA256_LEN, sha);
	fprintf(f, "MARK=%s\nSHA256=%s\nSIGNATURE=%s\nSIGNER=%s\n", mark, sha,
		sig_word[v->state], v->signer);
	if (fclose(f) || rename(tmp, path))
		unlink(tmp);
}

static void digest_forget(int n)
{
	char path[512];

	digest_path(n, path, sizeof(path));
	unlink(path);
}

/* The reading of bank @n kept under DIGESTS, while @mark is the bank's: 0, or -1. */
static int digest_kept(int n, const char *mark, char *text, size_t cap)
{
	char rel[64], kept[UPD_TEXT + 64];

	snprintf(rel, sizeof(rel), DIGESTS "/bank%d", n);
	if (read_text(rel, text, cap) ||
	    release_field(text, "MARK", kept, sizeof(kept)) || strcmp(kept, mark))
		return -1;
	return 0;
}

static int cmd_status(void)
{
	int running = running_bank(), n;
	struct env e;
	bool have_env = !env_load(&e);
	char version[UPD_TEXT];

	printf("RUNNING=%d\n", running);
	if (have_env) {
		long commit = env_number(&e, "sw_commit", -1);
		long tryactive = env_number(&e, "sw_tryactive", 2);
		long tried = env_number(&e, "onu_trial", -1);

		printf("COMMITTED=%ld\n", commit);
		printf("NEXT=%ld\n", tryactive == 0 || tryactive == 1 ? tryactive : commit);
		/* The trial runs, or the loader took it and another bank runs. */
		if (tried >= 0 && tryactive == 2 && tried == running && tried != commit)
			printf("TRIAL=%ld\n", tried);
		if (tried >= 0 && tryactive == 2 && tried != running && running >= 0)
			printf("FAILED=%ld\n", tried);
	}
	for (n = 0; n < 2; n++) {
		struct bank b;
		const char *state;
		char key[16];

		bank_probe(n, &b, shipped_split(&e, have_env, n));
		snprintf(key, sizeof(key), "sw_valid%d", n);
		if (b.shipped)
			state = "shipped";
		else if (!b.kernel || !b.rootfs)
			state = "empty";
		else if (have_env && env_number(&e, key, 1) == 0)
			state = "incomplete";
		else
			state = "valid";
		printf("BANK%d_STATE=%s\n", n, state);
		if (n == running && !running_version(version, sizeof(version))) {
			printf("BANK%d_VERSION=%s\n", n, version);
		} else if (have_env) {
			const char *v;

			snprintf(key, sizeof(key), "sw_version%d", n);
			v = bootenv_get(e.copy[e.cur], key);
			if (v)
				printf("BANK%d_VERSION=%s\n", n, v);
		}
		if (b.kernel)
			printf("BANK%d_TIME=%" PRIu32 "\n", n, b.time);
		printf("BANK%d_KERNEL_SIZE=%" PRIu32 "\n", n, b.kernel_size);
		printf("BANK%d_ROOTFS_SIZE=%" PRIu32 "\n", n, b.rootfs_size);
		if (b.kernel)
			printf("BANK%d_KERNEL_USED=%" PRIu32 "\n", n, b.kernel_len);
		if (b.rootfs)
			printf("BANK%d_ROOTFS_USED=%" PRIu32 "\n", n, b.rootfs_len);
		if (!strcmp(state, "valid")) {
			char mark[UPD_TEXT + 64], text[512], v[KEY_NAME + 8];

			bank_version(n, running, &e, have_env, version, sizeof(version));
			bank_mark(&b, version, mark, sizeof(mark));
			if (!digest_kept(n, mark, text, sizeof(text))) {
				if (!release_field(text, "SHA256", v, sizeof(v)))
					printf("BANK%d_SHA256=%s\n", n, v);
				if (!release_field(text, "SIGNATURE", v, sizeof(v)))
					printf("BANK%d_SIGNATURE=%s\n", n, v);
				if (!release_field(text, "SIGNER", v, sizeof(v)))
					printf("BANK%d_SIGNER=%s\n", n, v);
			}
		}
	}
	{
		struct key keys[KEYS_MAX];
		unsigned int i, nkeys = keys_load(keys);
		char id[2 * SHA256_LEN + 1];

		for (i = 0; i < nkeys; i++) {
			hex(keys[i].id, SHA256_LEN, id);
			printf("KEY=%s %s %s\n", id, keys[i].text, keys[i].name);
		}
	}
	return 0;
}

/*
 * Take the header of an update file from @in into @h and @raw, and check it
 * against this board and, with @k and @r, the partitions it is to go into.
 */
static int take_header(int in, struct upd_header *h, uint8_t raw[UPD_HEADER],
		       const struct part *k, const struct part *r)
{
	char board[UPD_TEXT];
	int err;

	if (read_full(in, raw, UPD_HEADER) != UPD_HEADER)
		return say("the file is shorter than an update header");
	err = upd_parse(raw, h);
	if (err == -EPROTONOSUPPORT)
		return say("an update file of another format version");
	if (err)
		return say("not an update file, or a damaged one");
	if (!board_of(board, sizeof(board)) && strcmp(board, h->board))
		return say("an update for %s, not for this board, %s",
			   h->board, board);
	if (h->kernel_len < UIMAGE_HEADER || h->rootfs_len < SQUASHFS_SUPER)
		return say("an image is too short");
	if (k && (h->kernel_len > k->size || h->rootfs_len > r->size))
		return say("the images, %u and %u bytes, do not fit the bank's %u and %u",
			   h->kernel_len, h->rootfs_len, k->size, r->size);
	return 0;
}

/*
 * Take the images and the trailer of the update file whose header @h and
 * @raw are, from @in: check them and, with @k and @r, write them there. The
 * trailer is checked last, after every byte has gone by; @checked gets the
 * digest it held, and @v the file's signature, checked with @keys, and its
 * SHA-256.
 */
static int take_images(int in, const struct upd_header *h,
		       const uint8_t raw[UPD_HEADER], struct part *k,
		       struct part *r, const struct key *keys, unsigned int nkeys,
		       uint8_t checked[SHA256_LEN], struct verdict *v)
{
	static uint8_t buf[CHUNK];
	uint8_t digest[SHA256_LEN], trailer[UPD_TRAILER];
	uint32_t uimage_len = 0, data_crc = 0, utime, crc;
	struct sink sk;
	int part;

	sink_init(&sk, raw, h->rootfs_len);
	crc = (uint32_t)crc32(0L, Z_NULL, 0);
	for (part = 0; part < 2; part++) {
		uint32_t len = part ? h->rootfs_len : h->kernel_len, off = 0;
		struct part *to = part ? r : k;

		while (off < len) {
			size_t want = len - off < CHUNK ? len - off : CHUNK;
			ssize_t n = read_full(in, buf, want);

			if (n != (ssize_t)want)
				return say("the file ends inside the %s image",
					   part ? "root filesystem" : "kernel");
			if (!off && !part &&
			    uimage_check(buf, &uimage_len, &data_crc, &utime))
				return say("the kernel image is not a uImage");
			if (!off && part && squashfs_check(buf, &sk.used))
				return say("the root filesystem is not a SquashFS image");
			if (!part) {
				size_t skip = off < UIMAGE_HEADER ? UIMAGE_HEADER - off : 0;

				if (skip < want)
					crc = (uint32_t)crc32(crc, buf + skip,
							      (unsigned int)(want - skip));
				sha256_update(&sk.s, buf, want);
			} else {
				sink_rootfs(&sk, off, buf, want);
			}
			if (to && part_write(to, off, buf, want))
				return -1;
			off += (uint32_t)want;
		}
	}
	if (uimage_len != h->kernel_len)
		return say("the kernel image is %u bytes, its header says %u",
			   h->kernel_len, uimage_len);
	if (crc != data_crc)
		return say("the kernel image's data does not match its CRC");
	if (sk.used > h->rootfs_len)
		return say("the root filesystem claims %u bytes, the file has %u",
			   sk.used, h->rootfs_len);
	if (read_full(in, trailer, UPD_TRAILER) != UPD_TRAILER)
		return say("the file ends before its checksum");
	if (read_full(in, buf, 1) != 0)
		return say("the file goes on after its checksum");
	sink_end(&sk, keys, nkeys, digest, v);
	if (memcmp(digest, trailer, SHA256_LEN))
		return say("the checksum does not match: the file is damaged");
	if (v->state == SIG_BAD)
		return say("the signature does not match the file: it was changed after it was signed");
	memcpy(checked, digest, SHA256_LEN);
	return 0;
}

/* What @v says, as KEY=VALUE lines: the SHA-256, the signature, its key. */
static void print_verdict(const char *prefix, const struct verdict *v)
{
	char sha[2 * SHA256_LEN + 1];

	hex(v->sha, SHA256_LEN, sha);
	printf("%sSHA256=%s\n%sSIGNATURE=%s\n", prefix, sha, prefix, sig_word[v->state]);
	if (v->state == SIG_VALID)
		printf("%sSIGNER=%s\n", prefix, v->signer);
}

static int open_input(const char *file)
{
	int fd;

	if (!strcmp(file, "-"))
		return 0;
	fd = open(file, O_RDONLY);
	if (fd < 0)
		say("%s: %s", file, strerror(errno));
	return fd;
}

static int cmd_check(const char *file)
{
	uint8_t digest[SHA256_LEN], raw[UPD_HEADER];
	struct key keys[KEYS_MAX];
	unsigned int nkeys = keys_load(keys);
	struct upd_header h;
	struct verdict v;
	int in = open_input(file), err;

	if (in < 0)
		return 1;
	err = take_header(in, &h, raw, NULL, NULL);
	if (!err)
		err = take_images(in, &h, raw, NULL, NULL, keys, nkeys, digest, &v);
	if (in)
		close(in);
	if (err)
		return 1;
	printf("VERSION=%s\nBOARD=%s\nTIME=%" PRIu64 "\nKERNEL=%u\nROOTFS=%u\n",
	       h.version, h.board, h.time, h.kernel_len, h.rootfs_len);
	print_verdict("", &v);
	return 0;
}

/* SHA-256 of the header @raw of @h and the images as the bank holds them. */
static int bank_digest(const struct upd_header *h, const uint8_t raw[UPD_HEADER],
		       struct part *k, struct part *r, uint8_t digest[SHA256_LEN])
{
	static uint8_t buf[CHUNK];
	struct sha256 s;
	int part;

	sha256_init(&s);
	sha256_update(&s, raw, UPD_HEADER);
	for (part = 0; part < 2; part++) {
		uint32_t len = part ? h->rootfs_len : h->kernel_len, off;
		struct part *from = part ? r : k;

		for (off = 0; off < len; off += CHUNK) {
			size_t n = len - off < CHUNK ? len - off : CHUNK;

			if (part_read(from, off, buf, n))
				return -1;
			sha256_update(&s, buf, n);
		}
	}
	sha256_final(&s, digest);
	return 0;
}

/*
 * Keep what the file written into bank @n, of header @raw, said as the
 * bank's reading, when the bank gives that header back as its update file,
 * so that the bank's file is the one written, byte for byte.
 */
static void keep_written(int n, int running, struct env *e, const uint8_t raw[UPD_HEADER],
			 const struct verdict *v)
{
	uint8_t back[UPD_HEADER];
	char mark[UPD_TEXT + 64];
	struct upd_header h;
	struct bank b;

	bank_probe(n, &b, 0);
	if (!b.kernel || !b.rootfs || bank_header(n, &b, running, e, true, &h, back) ||
	    memcmp(back, raw, UPD_HEADER))
		return;
	bank_mark(&b, h.version, mark, sizeof(mark));
	digest_keep(n, mark, v);
}

static int cmd_write(const char *file, int bank)
{
	int running = running_bank(), target, in, err;
	char valid[24], version[UPD_TEXT + 32];
	uint8_t want[SHA256_LEN], got[SHA256_LEN], raw[UPD_HEADER];
	struct key keys[KEYS_MAX];
	unsigned int nkeys = keys_load(keys);
	struct upd_header h;
	struct part k = { .fd = -1 }, r = { .fd = -1 };
	struct verdict v;
	struct env e;

	if (bank >= 0 && bank == running) {
		say("bank %d runs: an update goes into the other one", bank);
		return 1;
	}
	if (bank < 0 && running < 0) {
		say("the stick started from no bank (a start from memory): name the bank");
		return 1;
	}
	target = bank >= 0 ? bank : 1 - running;
	if (env_load(&e))
		return 1;
	in = open_input(file);
	if (in < 0)
		return 1;

	/* A file that is not for this bank leaves the bank as it is. */
	err = part_open(&k, target ? "k1" : "k0", true);
	if (!err)
		err = part_open(&r, target ? "r1" : "r0", true);
	if (!err)
		err = take_header(in, &h, raw, &k, &r);
	if (err) {
		part_close(&k);
		part_close(&r);
		if (in)
			close(in);
		return 1;
	}

	snprintf(valid, sizeof(valid), "sw_valid%d=0", target);
	snprintf(version, sizeof(version), "sw_version%d=", target);
	if (env_number(&e, "onu_trial", -1) == target) {
		const char *sets[3] = { valid, version, "onu_trial=" };

		err = env_change(&e, sets, 3);
	} else {
		err = env_set_bank(&e, valid, version);
	}
	digest_forget(target);
	if (!err)
		err = take_images(in, &h, raw, &k, &r, keys, nkeys, want, &v);
	/* What follows the images in their partitions is erased too. */
	if (!err)
		err = part_erase(&k, k.erased, k.size);
	if (!err)
		err = part_erase(&r, r.erased, r.size);
	/* Read back: the bank has to give the digest the file carried. */
	if (!err)
		err = bank_digest(&h, raw, &k, &r, got);
	if (!err && memcmp(want, got, SHA256_LEN))
		err = say("bank %d does not read back as written", target);
	if (in)
		close(in);
	if (err) {
		/* A bank that did not come out whole starts nothing. */
		if (k.fd >= 0 && k.erase)
			part_erase(&k, 0, k.erase);
		part_close(&k);
		part_close(&r);
		say("bank %d is left empty", target);
		return 1;
	}
	part_close(&k);
	part_close(&r);

	snprintf(valid, sizeof(valid), "sw_valid%d=1", target);
	snprintf(version, sizeof(version), "sw_version%d=%s", target, h.version);
	if (env_set_bank(&e, valid, version))
		return 1;
	keep_written(target, running, &e, raw, &v);
	printf("BANK=%d\nVERSION=%s\n", target, h.version);
	print_verdict("", &v);
	return 0;
}

/* Pass @len bytes of @from into @k, the root filesystem's with @rootfs, and to @out but -1. */
static int pass(struct part *from, uint32_t len, int out, struct sink *k, bool rootfs)
{
	static uint8_t buf[CHUNK];
	uint32_t off;

	for (off = 0; off < len; off += CHUNK) {
		size_t n = len - off < CHUNK ? len - off : CHUNK;

		if (part_read(from, off, buf, n))
			return -1;
		if (rootfs)
			sink_rootfs(k, off, buf, n);
		else
			sha256_update(&k->s, buf, n);
		if (out >= 0 && write_full(out, buf, n))
			return say("the output: %s", strerror(errno));
	}
	return 0;
}

/*
 * Bank @n as an update file, to @out but -1: in @v its signature, checked
 * with @keys, and its SHA-256, which are kept under DIGESTS too.
 */
static int bank_file(int n, int out, const struct key *keys, unsigned int nkeys,
		     struct verdict *v)
{
	int running = running_bank(), err = 0;
	uint8_t raw[UPD_HEADER], digest[SHA256_LEN];
	struct part k = { .fd = -1 }, r = { .fd = -1 };
	char mark[UPD_TEXT + 64];
	struct upd_header h;
	struct sink sk;
	struct bank b;
	struct env e;
	bool have_env;

	bank_probe(n, &b, 0);
	if (!b.kernel || !b.rootfs)
		return say("bank %d holds no system", n);
	have_env = !env_load(&e);
	if (bank_header(n, &b, running, &e, have_env, &h, raw))
		return -1;
	sink_init(&sk, raw, h.rootfs_len);
	sk.used = b.rootfs_len;
	if (out >= 0 && write_full(out, raw, UPD_HEADER))
		err = say("the output: %s", strerror(errno));
	if (!err)
		err = part_open(&k, n ? "k1" : "k0", false);
	if (!err)
		err = part_open(&r, n ? "r1" : "r0", false);
	if (!err)
		err = pass(&k, h.kernel_len, out, &sk, false);
	if (!err)
		err = pass(&r, h.rootfs_len, out, &sk, true);
	part_close(&k);
	part_close(&r);
	if (err)
		return -1;
	sink_end(&sk, keys, nkeys, digest, v);
	if (out >= 0 && write_full(out, digest, SHA256_LEN))
		return say("the output: %s", strerror(errno));
	bank_mark(&b, h.version, mark, sizeof(mark));
	digest_keep(n, mark, v);
	return 0;
}

static int cmd_read(int bank)
{
	struct key keys[KEYS_MAX];
	unsigned int nkeys = keys_load(keys);
	struct verdict v;

	if (bank < 0)
		bank = running_bank();
	if (bank < 0) {
		say("the bank the stick started from is not known; name one");
		return 1;
	}
	return bank_file(bank, 1, keys, nkeys, &v) ? 1 : 0;
}

/*
 * Read bank @bank, or each bank that holds a system of this kind, whole: its
 * SHA-256 and signature, which the status gives from then on.
 */
static int cmd_digest(int bank)
{
	struct key keys[KEYS_MAX];
	unsigned int nkeys = keys_load(keys);
	int n, failed = 0;

	/* It runs beside the stick's work, which comes first. */
	if (setpriority(PRIO_PROCESS, 0, 19))
		say("its priority stays: %s", strerror(errno));
	for (n = 0; n < 2; n++) {
		char prefix[16];
		struct verdict v;
		struct bank b;

		if (bank >= 0 && n != bank)
			continue;
		bank_probe(n, &b, 0);
		if (bank < 0 && (!b.kernel || !b.rootfs))
			continue;
		if (bank_file(n, -1, keys, nkeys, &v)) {
			failed = 1;
			continue;
		}
		snprintf(prefix, sizeof(prefix), "BANK%d_", n);
		print_verdict(prefix, &v);
	}
	return failed;
}

static int cmd_try(int bank)
{
	int running = running_bank();
	char set[24], mark[24], key[16];
	struct bank b;
	struct env e;

	if (bank < 0) {
		if (running < 0) {
			say("the bank the stick started from is not known; name one");
			return 1;
		}
		bank = 1 - running;
	}
	bank_probe(bank, &b, 0);
	if (env_load(&e))
		return 1;
	snprintf(key, sizeof(key), "sw_valid%d", bank);
	if (!b.kernel || !b.rootfs || env_number(&e, key, 1) == 0) {
		say("bank %d holds no whole system", bank);
		return 1;
	}
	snprintf(set, sizeof(set), "sw_tryactive=%d", bank);
	snprintf(mark, sizeof(mark), "onu_trial=%d", bank);
	if (env_set_bank(&e, set, mark))
		return 1;
	printf("NEXT=%d\n", bank);
	return 0;
}

static int cmd_commit(void)
{
	int running = running_bank();
	char set[24];
	struct env e;

	if (running < 0) {
		say("the bank the stick started from is not known (no root=31:N)");
		return 1;
	}
	if (env_load(&e))
		return 1;
	snprintf(set, sizeof(set), "sw_commit=%d", running);
	{
		/*
		 * The loader's multicast listener off as well: it writes any
		 * image sent to the host link, from anyone, by the shipped split,
		 * which would write a root filesystem over this system's kernel
		 * (boot-contract.rst). Every way in ends in a commit.
		 */
		const char *sets[4] = { set, "sw_tryactive=2", "onu_trial=", "mupgrade_en=0" };

		if (env_change(&e, sets, 4))
			return 1;
	}
	printf("COMMITTED=%d\n", running);
	return 0;
}

/* Whether the data of the kernel image of @bank, as @b found it, has its CRC. */
static bool kernel_whole(int bank, const struct bank *b)
{
	static uint8_t buf[CHUNK];
	uLong crc = crc32(0L, Z_NULL, 0);
	uint32_t off;
	struct part k;
	bool ok = false;

	if (!part_open(&k, bank ? "k1" : "k0", false)) {
		for (off = UIMAGE_HEADER; off < b->kernel_len; off += CHUNK) {
			size_t n = b->kernel_len - off < CHUNK ? b->kernel_len - off : CHUNK;

			if (part_read(&k, off, buf, n))
				break;
			crc = crc32(crc, buf, (uInt)n);
		}
		ok = off >= b->kernel_len && (uint32_t)crc == b->data_crc;
	}
	part_close(&k);
	return ok;
}

/*
 * Have the boot loader start the shipped firmware of @bank from now on: a
 * bank that holds it whole, as far as can be told, a kernel image whose data
 * checks out and a root filesystem where the shipped firmware's split puts
 * it. There is no trial: the shipped firmware does not tell the loader that
 * it came up, so a bank that did not start would be left only to the boot
 * loader's prompt (recovery.rst).
 */
static int cmd_return(int bank)
{
	int running = running_bank();
	char set[24];
	struct bank b;
	struct env e;

	if (bank < 0) {
		if (running < 0) {
			say("the bank the stick started from is not known; name one");
			return 1;
		}
		bank = 1 - running;
	}
	if (env_load(&e))
		return 1;
	bank_probe(bank, &b, shipped_split(&e, true, bank));
	if (!b.shipped) {
		say("bank %d holds no shipped firmware", bank);
		return 1;
	}
	if (!kernel_whole(bank, &b)) {
		say("the kernel image of bank %d does not check out", bank);
		return 1;
	}
	snprintf(set, sizeof(set), "sw_commit=%d", bank);
	{
		const char *sets[3] = { set, "sw_tryactive=2", "onu_trial=" };

		if (env_change(&e, sets, 3))
			return 1;
	}
	printf("COMMITTED=%d\n", bank);
	return 0;
}

/*
 * The partitions in their order on the flash, which they cover whole (the
 * device tree): a copy of all of them is a copy of the chip.
 */
static const char *const FLASH_ORDER[] = { "boot", "env", "env2", "config",
					     "k0",   "r0",  "k1",   "r1" };

static int cmd_backup(void)
{
	struct sink sk;
	size_t i;

	memset(&sk, 0, sizeof(sk));
	sha256_init(&sk.s);
	for (i = 0; i < ARRAY_SIZE(FLASH_ORDER); i++) {
		struct part p;
		int err = part_open(&p, FLASH_ORDER[i], false);

		if (!err)
			err = pass(&p, p.size, 1, &sk, false);
		part_close(&p);
		if (err)
			return 1;
	}
	return 0;
}

static int cmd_env(char *const *args, int n)
{
	struct env e;
	int i, sets = 0;

	if (env_load(&e))
		return 1;
	for (i = 0; i < n; i++)
		sets += !!strchr(args[i], '=');
	if (sets && sets != n) {
		say("either KEY=VALUE to change, or KEY to print, not both");
		return 2;
	}
	if (sets)
		return env_change(&e, (const char *const *)args, n) ? 1 : 0;
	if (!n) {
		size_t pos = 0;
		const char *var;

		while ((var = bootenv_next(e.copy[e.cur], &pos)))
			puts(var);
		return 0;
	}
	for (i = 0; i < n; i++) {
		const char *v = bootenv_get(e.copy[e.cur], args[i]);

		if (!v) {
			say("%s is not set", args[i]);
			return 1;
		}
		printf("%s=%s\n", args[i], v);
	}
	return 0;
}

static int help(void)
{
	fputs("usage: onu-update status | check FILE | write FILE [BANK] | read [BANK] |\n"
	      "                  digest [BANK] | try [BANK] | commit | return [BANK] |\n"
	      "                  backup | env [KEY | KEY=VALUE]...\n",
	      stderr);
	return 2;
}

/* BANK, 0 or 1, from @arg, or -1 for none given. */
static int bank_arg(const char *arg, int *bank)
{
	if (!arg) {
		*bank = -1;
		return 0;
	}
	if ((arg[0] != '0' && arg[0] != '1') || arg[1])
		return -1;
	*bank = arg[0] - '0';
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : NULL;
	int bank;

	if (getenv("ONU_ROOT"))
		root = getenv("ONU_ROOT");
	if (!cmd)
		return help();
	if (!strcmp(cmd, "status") && argc == 2)
		return cmd_status();
	if (!strcmp(cmd, "check") && argc == 3)
		return cmd_check(argv[2]);
	if (!strcmp(cmd, "write") && (argc == 3 || argc == 4) &&
	    !bank_arg(argc == 4 ? argv[3] : NULL, &bank))
		return cmd_write(argv[2], bank);
	if (!strcmp(cmd, "read") && argc <= 3 && !bank_arg(argv[2], &bank))
		return cmd_read(bank);
	if (!strcmp(cmd, "digest") && argc <= 3 && !bank_arg(argv[2], &bank))
		return cmd_digest(bank);
	if (!strcmp(cmd, "try") && argc <= 3 && !bank_arg(argv[2], &bank))
		return cmd_try(bank);
	if (!strcmp(cmd, "commit") && argc == 2)
		return cmd_commit();
	if (!strcmp(cmd, "return") && argc <= 3 && !bank_arg(argv[2], &bank))
		return cmd_return(bank);
	if (!strcmp(cmd, "backup") && argc == 2)
		return cmd_backup();
	if (!strcmp(cmd, "env"))
		return cmd_env(argv + 2, argc - 2);
	return help();
}
