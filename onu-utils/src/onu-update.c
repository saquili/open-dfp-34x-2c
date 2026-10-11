// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-update: the two banks of the stick's flash and the updates that go
 * into them.
 *
 *   onu-update status          the banks, as KEY=VALUE lines: each one's
 *                              partitions' sizes and what its images use
 *   onu-update check FILE      check an update file (update.h), writing
 *                              nothing
 *   onu-update write FILE [BANK]
 *                              write an update file into the bank the stick
 *                              did not start from, and read it back; BANK,
 *                              0 or 1, names it after a start from memory
 *   onu-update read [BANK]     an update file of a bank, the running one by
 *                              default, on the standard output
 *   onu-update try [BANK]      have the boot loader start BANK, the other one
 *                              by default, once, at the next start
 *   onu-update commit          have the boot loader start the running bank
 *                              from now on
 *   onu-update env [KEY | KEY=VALUE]...
 *                              print the boot loader's environment, some of
 *                              its variables, or change them in one save
 *
 * FILE "-" is the standard input, so an update can stream in without being
 * held whole: the stick has some 25 MB of memory.
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

#include <mtd/mtd-user.h>
#include <zlib.h>

#include "bootenv.h"
#include "sha256.h"
#include "update.h"

#define CHUNK 0x10000u

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

/* BUILD_ID of /etc/os-release, the version of the running system. */
static int build_id(char *out, size_t cap)
{
	char text[2048], *p, *end;

	if (read_text("/etc/os-release", text, sizeof(text)))
		return -1;
	p = strstr(text, "BUILD_ID=");
	if (!p || (p != text && p[-1] != '\n'))
		return -1;
	p += 9;
	if (*p == '"')
		p++;
	end = p + strcspn(p, "\"\n");
	*end = '\0';
	if (!*p)
		return -1;
	snprintf(out, cap, "%s", p);
	return 0;
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
	}
	part_close(&p);
	if (b->rootfs)
		b->shipped = false;
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
		if (n == running && !build_id(version, sizeof(version))) {
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
 * digest it held.
 */
static int take_images(int in, const struct upd_header *h,
		       const uint8_t raw[UPD_HEADER], struct part *k,
		       struct part *r, uint8_t checked[SHA256_LEN])
{
	static uint8_t buf[CHUNK];
	uint8_t digest[SHA256_LEN], trailer[UPD_TRAILER];
	uint32_t uimage_len = 0, data_crc = 0, utime, used = 0, crc;
	struct sha256 s;
	int part;

	sha256_init(&s);
	sha256_update(&s, raw, UPD_HEADER);
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
			if (!off && part && squashfs_check(buf, &used))
				return say("the root filesystem is not a SquashFS image");
			if (!part) {
				size_t skip = off < UIMAGE_HEADER ? UIMAGE_HEADER - off : 0;

				if (skip < want)
					crc = (uint32_t)crc32(crc, buf + skip,
							      (unsigned int)(want - skip));
			}
			sha256_update(&s, buf, want);
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
	if (used > h->rootfs_len)
		return say("the root filesystem claims %u bytes, the file has %u",
			   used, h->rootfs_len);
	if (read_full(in, trailer, UPD_TRAILER) != UPD_TRAILER)
		return say("the file ends before its checksum");
	if (read_full(in, buf, 1) != 0)
		return say("the file goes on after its checksum");
	sha256_final(&s, digest);
	if (memcmp(digest, trailer, SHA256_LEN))
		return say("the checksum does not match: the file is damaged");
	memcpy(checked, digest, SHA256_LEN);
	return 0;
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
	struct upd_header h;
	int in = open_input(file), err;

	if (in < 0)
		return 1;
	err = take_header(in, &h, raw, NULL, NULL);
	if (!err)
		err = take_images(in, &h, raw, NULL, NULL, digest);
	if (in)
		close(in);
	if (err)
		return 1;
	printf("VERSION=%s\nBOARD=%s\nTIME=%" PRIu64 "\nKERNEL=%u\nROOTFS=%u\n",
	       h.version, h.board, h.time, h.kernel_len, h.rootfs_len);
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

static int cmd_write(const char *file, int bank)
{
	int running = running_bank(), target, in, err;
	char valid[24], version[UPD_TEXT + 32];
	uint8_t want[SHA256_LEN], got[SHA256_LEN], raw[UPD_HEADER];
	struct upd_header h;
	struct part k = { .fd = -1 }, r = { .fd = -1 };
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
	if (!err)
		err = take_images(in, &h, raw, &k, &r, want);
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
	printf("BANK=%d\nVERSION=%s\n", target, h.version);
	return 0;
}

/* Copy @len bytes of @from to @out, into @s as well. */
static int stream(struct part *from, uint32_t len, int out, struct sha256 *s)
{
	static uint8_t buf[CHUNK];
	uint32_t off;

	for (off = 0; off < len; off += CHUNK) {
		size_t n = len - off < CHUNK ? len - off : CHUNK;

		if (part_read(from, off, buf, n))
			return -1;
		sha256_update(s, buf, n);
		if (write_full(out, buf, n))
			return say("the output: %s", strerror(errno));
	}
	return 0;
}

static int cmd_read(int bank)
{
	int running = running_bank();
	uint8_t raw[UPD_HEADER], digest[SHA256_LEN];
	struct upd_header h = { 0 };
	struct part k = { .fd = -1 }, r = { .fd = -1 };
	struct sha256 s;
	struct bank b;
	struct env e;
	int err = 0;

	if (bank < 0)
		bank = running;
	if (bank < 0) {
		say("the bank the stick started from is not known; name one");
		return 1;
	}
	bank_probe(bank, &b, 0);
	if (!b.kernel || !b.rootfs) {
		say("bank %d holds no system", bank);
		return 1;
	}
	h.kernel_len = b.kernel_len;
	h.rootfs_len = b.rootfs_len;
	h.time = b.time;
	if (board_of(h.board, sizeof(h.board))) {
		say("no /proc/device-tree/compatible: the board is not known");
		return 1;
	}
	if (bank != running || build_id(h.version, sizeof(h.version))) {
		char key[16];
		const char *v = NULL;

		snprintf(key, sizeof(key), "sw_version%d", bank);
		if (!env_load(&e))
			v = bootenv_get(e.copy[e.cur], key);
		snprintf(h.version, sizeof(h.version), "%s", v && *v ? v : "unknown");
	}

	upd_build(raw, &h);
	sha256_init(&s);
	sha256_update(&s, raw, UPD_HEADER);
	if (write_full(1, raw, UPD_HEADER))
		err = say("the output: %s", strerror(errno));
	if (!err)
		err = part_open(&k, bank ? "k1" : "k0", false);
	if (!err)
		err = part_open(&r, bank ? "r1" : "r0", false);
	if (!err)
		err = stream(&k, h.kernel_len, 1, &s);
	if (!err)
		err = stream(&r, h.rootfs_len, 1, &s);
	part_close(&k);
	part_close(&r);
	if (err)
		return 1;
	sha256_final(&s, digest);
	if (write_full(1, digest, SHA256_LEN)) {
		say("the output: %s", strerror(errno));
		return 1;
	}
	return 0;
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
		const char *sets[3] = { set, "sw_tryactive=2", "onu_trial=" };

		if (env_change(&e, sets, 3))
			return 1;
	}
	printf("COMMITTED=%d\n", running);
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
	      "                  try [BANK] | commit | env [KEY | KEY=VALUE]...\n",
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
	if (!strcmp(cmd, "try") && argc <= 3 && !bank_arg(argv[2], &bank))
		return cmd_try(bank);
	if (!strcmp(cmd, "commit") && argc == 2)
		return cmd_commit();
	if (!strcmp(cmd, "env"))
		return cmd_env(argv + 2, argc - 2);
	return help();
}
