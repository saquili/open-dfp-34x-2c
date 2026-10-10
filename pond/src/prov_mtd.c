// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The provider of the flash: the partitions, both copies of the boot
 * loader's environment (each with a CRC-32 and, in a redundant layout, a
 * flag byte that says which copy is newer), and the two firmware banks the
 * environment selects between.
 */

#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <mtd/mtd-user.h>
#endif

#define POND_MTD_PARTS 32
#define POND_MTD_NAME 32

#define POND_ENV_MAX 16384

#define POND_DIFF_MAX 24

#define POND_MTDBLOCK_MAJOR 31

struct mtd_part {
	char name[POND_MTD_NAME];
	int index;
	unsigned long long size;
	unsigned long long erasesize;
};

struct mtd_table {
	struct mtd_part p[POND_MTD_PARTS];
	unsigned int dropped;
	unsigned int n;
};

/* The flash's size: the partitions cover the whole chip. */
static unsigned long long mtd_chip_size(const struct mtd_table *t)
{
	unsigned long long size = 0;

	for (unsigned int i = 0; i < t->n; i++)
		size += t->p[i].size;
	return size;
}

static int mtd_table_read(struct mtd_table *t)
{
	char text[4096];
	const char *line;
	int err = prov_slurp("/proc/mtd", text, sizeof(text), NULL);

	if (err)
		return err;

	t->n = 0;
	for (line = text; *line;) {
		const char *nl = strchr(line, '\n');
		struct mtd_part *p = &t->p[t->n];

		if (sscanf(line, "mtd%d: %llx %llx \"%31[^\"]\"", &p->index,
			   &p->size, &p->erasesize, p->name) == 4) {
			if (t->n < POND_MTD_PARTS)
				t->n++;
			else
				t->dropped++;
		}
		if (!nl)
			break;
		line = nl + 1;
	}

	return t->n ? 0 : -EPROTO;
}

static const struct mtd_part *mtd_by_name(const struct mtd_table *t,
					  const char *name)
{
	unsigned int i;

	for (i = 0; i < t->n; i++)
		if (!strcmp(t->p[i].name, name))
			return &t->p[i];
	return NULL;
}

static const struct mtd_part *mtd_by_index(const struct mtd_table *t, int index)
{
	unsigned int i;

	for (i = 0; i < t->n; i++)
		if (t->p[i].index == index)
			return &t->p[i];
	return NULL;
}

struct mtd_driver {
	bool valid;
	int err;
	long long type_code;
	const char *type_name;
	bool writable;
};

/* The driver's flag of a partition that takes writes (mtd/mtd-abi.h). */
#ifndef MTD_WRITEABLE
#define MTD_WRITEABLE 0x400
#endif

/*
 * What the MTD driver says of partition @index, from /sys/class/mtd/mtdN,
 * which any user may read: its type, as MEMGETINFO names it, and whether it
 * takes writes. The device itself, root's alone, is not opened.
 */
static void mtd_driver_query(int index, struct mtd_driver *o)
{
	static const struct {
		const char *sys;
		const char *name;
		long long code;
	} types[] = {
		{ "absent", "absent", 0 },	{ "ram", "ram", 1 },
		{ "rom", "rom", 2 },		{ "nor", "norflash", 3 },
		{ "nand", "nandflash", 4 },	{ "dataflash", "dataflash", 6 },
		{ "ubi", "ubivolume", 7 },	{ "mlc-nand", "mlcnandflash", 8 },
	};
	char path[64], type[32], flags[32];
	unsigned int i;

	snprintf(path, sizeof(path), "/sys/class/mtd/mtd%d/type", index);
	o->err = prov_read_line(path, type, sizeof(type));
	if (o->err)
		return;
	snprintf(path, sizeof(path), "/sys/class/mtd/mtd%d/flags", index);
	o->err = prov_read_line(path, flags, sizeof(flags));
	if (o->err)
		return;
	o->type_name = "unknown";
	o->type_code = -1;
	for (i = 0; i < ARRAY_SIZE(types); i++)
		if (!strcmp(type, types[i].sys)) {
			o->type_name = types[i].name;
			o->type_code = types[i].code;
		}
	o->writable = (strtoul(flags, NULL, 16) & MTD_WRITEABLE) != 0;
	o->valid = true;
}

struct env_copy {
	const char *part;
	int index;
	int err;
	size_t len;
	unsigned int hdr;
	unsigned int flag;
	bool have_flag;
	bool crc_ok;
	char *buf;
};

static uint32_t env_crc32(const unsigned char *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	size_t i;
	unsigned int k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
	}
	return c ^ 0xffffffffu;
}

static void env_read(struct env_copy *e, const struct mtd_table *t,
		     const char *part, char *buf, size_t cap)
{
	static const unsigned int try_hdr[] = { 5, 4 };
	const struct mtd_part *p = mtd_by_name(t, part);
	char dev[32];
	uint32_t want;
	unsigned int i;

	memset(e, 0, sizeof(*e));
	e->part = part;
	e->index = -1;
	e->buf = buf;

	if (!p) {
		e->err = -ENOENT;
		return;
	}
	e->index = p->index;
	snprintf(dev, sizeof(dev), "/dev/mtd%d", p->index);
	e->err = prov_slurp(dev, buf, cap, &e->len);
	if (e->err)
		return;
	if (e->len < 8) {
		e->err = -EPROTO;
		return;
	}

	want = ((uint32_t)(unsigned char)buf[0] << 24) |
	       ((uint32_t)(unsigned char)buf[1] << 16) |
	       ((uint32_t)(unsigned char)buf[2] << 8) |
	       (uint32_t)(unsigned char)buf[3];

	for (i = 0; i < ARRAY_SIZE(try_hdr); i++) {
		unsigned int h = try_hdr[i];

		if (env_crc32((const unsigned char *)buf + h, e->len - h) !=
		    want)
			continue;
		e->hdr = h;
		e->crc_ok = true;
		e->have_flag = (h == 5);
		e->flag = (unsigned char)buf[4];
		return;
	}
}

static const char *env_get_n(const struct env_copy *e, const char *key,
			     size_t klen)
{
	size_t p;

	if (!e->crc_ok)
		return NULL;
	for (p = e->hdr; p < e->len && e->buf[p]; p += strlen(e->buf + p) + 1)
		if (!strncmp(e->buf + p, key, klen) && e->buf[p + klen] == '=')
			return e->buf + p + klen + 1;
	return NULL;
}

static const char *env_get(const struct env_copy *e, const char *key)
{
	return e ? env_get_n(e, key, strlen(key)) : NULL;
}

static const struct env_copy *env_live(const struct env_copy *a,
				       const struct env_copy *b)
{
	if (!a->crc_ok && !b->crc_ok)
		return NULL;
	if (!b->crc_ok)
		return a;
	if (!a->crc_ok)
		return b;
	if (!a->have_flag || !b->have_flag)
		return a;
	if (a->flag == 255 && b->flag == 0)
		return b;
	if (b->flag == 255 && a->flag == 0)
		return a;
	/* The higher flag, and of two equal ones the second copy, as the loader. */
	return a->flag > b->flag ? a : b;
}

struct env_diff {
	struct prov_buf *out;
	unsigned int n;
	unsigned int emitted;
};

static void diff_note(struct env_diff *d, const char *name, size_t nlen)
{
	d->n++;
	if (d->emitted >= POND_DIFF_MAX)
		return;
	if (d->emitted)
		pv_comma(d->out);
	pv_strn(d->out, name, nlen);
	d->emitted++;
}

static void diff_pass(struct env_diff *d, const struct env_copy *a,
		      const struct env_copy *b, bool compare_values)
{
	size_t p;

	for (p = a->hdr; p < a->len && a->buf[p]; p += strlen(a->buf + p) + 1) {
		const char *ent = a->buf + p;
		const char *eq = strchr(ent, '=');
		const char *other;
		size_t klen;

		if (!eq)
			continue;
		klen = (size_t)(eq - ent);
		other = env_get_n(b, ent, klen);
		if (!other)
			diff_note(d, ent, klen);
		else if (compare_values && strcmp(other, eq + 1))
			diff_note(d, ent, klen);
	}
}

int prov_uboot_bank(const char *value)
{
	return strcmp(value ? value : "", "0") ? 1 : 0;
}

static bool root_token(const char *cmdline, char *out, size_t cap)
{
	const char *r = cmdline ? strstr(cmdline, "root=") : NULL;
	size_t n = 0;

	out[0] = '\0';

	while (r && r != cmdline && r[-1] != ' ' && r[-1] != '\t')
		r = strstr(r + 1, "root=");
	if (!r)
		return false;
	r += 5;
	while (r[n] && r[n] != ' ' && r[n] != '\t' && r[n] != '\n' &&
	       n + 1 < cap)
		n++;
	memcpy(out, r, n);
	out[n] = '\0';
	return true;
}

int prov_root_mtd(const char *cmdline)
{
	char tok[64];
	char *end;
	long major;

	if (!root_token(cmdline, tok, sizeof(tok)))
		return -1;
	if (!strncmp(tok, "/dev/mtdblock", 13))
		return (tok[13] >= '0' && tok[13] <= '9') ?
			       (int)strtol(tok + 13, NULL, 10) :
			       -1;
	if (tok[0] < '0' || tok[0] > '9')
		return -1;
	major = strtol(tok, &end, 10);
	if (*end != ':' || major != POND_MTDBLOCK_MAJOR)
		return -1;
	return (int)strtol(end + 1, NULL, 10);
}

int prov_rootfs_bank(const char *part)
{
	if (!part)
		return -1;
	if (!strcmp(part, "r0"))
		return 0;
	if (!strcmp(part, "r1"))
		return 1;
	return -1;
}

static void emit_num_or_null(struct prov_buf *out, int v)
{
	if (v < 0)
		pv_null(out);
	else
		pv_num(out, v);
}

static void emit_str_or_null(struct prov_buf *out, const char *s)
{
	if (s)
		pv_str(out, s);
	else
		pv_null(out);
}

static void emit_partitions(struct prov_buf *out, const struct mtd_table *t)
{
	unsigned int i;

	pv_open(out, '[');
	for (i = 0; i < t->n; i++) {
		struct mtd_driver d = { false, 0, 0, NULL, false };

		mtd_driver_query(t->p[i].index, &d);

		if (i)
			pv_comma(out);
		pv_open(out, '{');
		pv_key(out, "name");
		pv_str(out, t->p[i].name);
		pv_comma(out);
		pv_key(out, "mtd");
		pv_num(out, t->p[i].index);
		pv_comma(out);

		pv_key(out, "size_b");
		pv_num(out, (long long)t->p[i].size);
		pv_comma(out);
		pv_key(out, "type");
		emit_str_or_null(out, d.valid ? d.type_name : NULL);
		pv_comma(out);
		pv_key(out, "type_code");
		if (d.valid)
			pv_num(out, d.type_code);
		else
			pv_null(out);
		pv_comma(out);
		pv_key(out, "writable");
		if (d.valid)
			pv_bool(out, d.writable);
		else
			pv_null(out);
		if (!d.valid) {
			pv_comma(out);
			pv_key(out, "unreadable");
			pv_str(out, strerror(-d.err));
		}
		pv_close(out, '}');

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
}

static void emit_env_copy(struct prov_buf *out, const struct env_copy *e)
{
	pv_open(out, '{');
	pv_key(out, "part");
	pv_str(out, e->part);
	pv_comma(out);
	pv_key(out, "mtd");
	if (e->index >= 0)
		pv_num(out, e->index);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "bytes");
	if (e->err)
		pv_null(out);
	else
		pv_num(out, (long long)e->len);
	pv_comma(out);

	pv_key(out, "layout");
	if (!e->crc_ok)
		pv_null(out);
	else
		pv_str(out, e->have_flag ? "redundant" : "simple");
	pv_comma(out);
	pv_key(out, "flag");
	if (e->crc_ok && e->have_flag)
		pv_num(out, e->flag);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "crc_ok");
	if (e->err)
		pv_null(out);
	else
		pv_bool(out, e->crc_ok);
	if (e->err) {
		pv_comma(out);
		pv_key(out, "unreadable");
		pv_str(out, strerror(-e->err));
	}
	pv_close(out, '}');
}

static void emit_env(struct prov_buf *out, const struct env_copy *a,
		     const struct env_copy *b, const struct env_copy *live)
{
	struct env_diff d = { out, 0, 0 };

	pv_open(out, '{');
	pv_key(out, "copies");
	pv_open(out, '[');
	emit_env_copy(out, a);
	pv_comma(out);
	emit_env_copy(out, b);
	pv_close(out, ']');

	pv_comma(out);
	pv_key(out, "in_use");
	emit_str_or_null(out, live ? live->part : NULL);

	pv_comma(out);
	pv_key(out, "differ");
	if (!a->crc_ok || !b->crc_ok) {
		pv_null(out);
		pv_comma(out);
		pv_key(out, "differ_n");
		pv_null(out);
	} else {
		pv_open(out, '[');
		diff_pass(&d, a, b, true);
		diff_pass(&d, b, a, false);
		pv_close(out, ']');
		pv_comma(out);
		pv_key(out, "differ_n");
		pv_num(out, d.n);
	}
	pv_close(out, '}');
}

static void emit_selection(struct prov_buf *out, const struct env_copy *live)
{
	const char *active = env_get(live, "sw_active");
	const char *commit = env_get(live, "sw_commit");
	const char *tryact = env_get(live, "sw_tryactive");

	bool armed = live && strcmp(tryact ? tryact : "", "2") != 0;

	pv_open(out, '{');

	pv_key(out, "active_bank");
	if (active && !strcmp(active, "0"))
		pv_num(out, 0);
	else if (active && !strcmp(active, "1"))
		pv_num(out, 1);
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "commit_bank");
	if (live)
		pv_num(out, prov_uboot_bank(commit));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "trial_bank");
	if (live && armed)
		pv_num(out, prov_uboot_bank(tryact));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "next_boot_bank");
	if (live)
		pv_num(out, prov_uboot_bank(armed ? tryact : commit));
	else
		pv_null(out);

	pv_comma(out);
	pv_key(out, "raw");
	pv_open(out, '{');
	pv_key(out, "sw_active");
	emit_str_or_null(out, active);
	pv_comma(out);
	pv_key(out, "sw_commit");
	emit_str_or_null(out, commit);
	pv_comma(out);
	pv_key(out, "sw_tryactive");
	emit_str_or_null(out, tryact);
	pv_close(out, '}');
	pv_close(out, '}');
}

static void emit_banks(struct prov_buf *out, const struct mtd_table *t,
		       const struct env_copy *live)
{
	static const char *const kern[] = { "k0", "k1" };
	static const char *const root[] = { "r0", "r1" };
	unsigned int i;

	pv_open(out, '[');
	for (i = 0; i < 2; i++) {
		char key[16];
		const struct mtd_part *kp = mtd_by_name(t, kern[i]);
		const struct mtd_part *rp = mtd_by_name(t, root[i]);

		if (i)
			pv_comma(out);
		pv_open(out, '{');
		pv_key(out, "bank");
		pv_num(out, i);
		pv_comma(out);
		pv_key(out, "kernel_part");
		emit_str_or_null(out, kp ? kp->name : NULL);
		pv_comma(out);
		pv_key(out, "rootfs_part");
		emit_str_or_null(out, rp ? rp->name : NULL);

		pv_comma(out);
		pv_key(out, "valid_recorded");
		snprintf(key, sizeof(key), "sw_valid%u", i);
		emit_str_or_null(out, env_get(live, key));

		pv_comma(out);
		pv_key(out, "crc_recorded");
		snprintf(key, sizeof(key), "sw_crc%u", i);
		emit_str_or_null(out, env_get(live, key));

		pv_comma(out);
		pv_key(out, "version_recorded");
		snprintf(key, sizeof(key), "sw_version%u", i);
		emit_str_or_null(out, env_get(live, key));
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

static void emit_rootfs(struct prov_buf *out, const struct mtd_table *t)
{
	char cmdline[1024], tok[64];
	const struct mtd_part *p;
	int idx;
	int err = prov_read_line("/proc/cmdline", cmdline, sizeof(cmdline));

	if (err) {
		char why[96];

		snprintf(why, sizeof(why), "{\"error\":\"%s\"}",
			 strerror(-err));
		pv_raw(out, why);
		return;
	}

	root_token(cmdline, tok, sizeof(tok));
	idx = prov_root_mtd(cmdline);
	p = idx >= 0 ? mtd_by_index(t, idx) : NULL;

	pv_open(out, '{');
	pv_key(out, "root");
	emit_str_or_null(out, tok[0] ? tok : NULL);
	pv_comma(out);
	pv_key(out, "mtd");
	emit_num_or_null(out, idx);
	pv_comma(out);
	pv_key(out, "partition");
	emit_str_or_null(out, p ? p->name : NULL);
	pv_comma(out);
	pv_key(out, "bank");
	emit_num_or_null(out, prov_rootfs_bank(p ? p->name : NULL));
	pv_close(out, '}');
}

int prov_banks(struct prov_buf *out)
{
	static char abuf[POND_ENV_MAX];
	static char bbuf[POND_ENV_MAX];
	struct mtd_table t;
	struct env_copy a, b;
	const struct env_copy *live;
	int err = mtd_table_read(&t);

	if (err)
		return err;

	env_read(&a, &t, "env", abuf, sizeof(abuf));
	env_read(&b, &t, "env2", bbuf, sizeof(bbuf));
	live = env_live(&a, &b);

	pv_open(out, '{');
	pv_key(out, "flash");
	pv_open(out, '{');
	pv_key(out, "size_src");
	pv_str(out, "/proc/mtd");
	pv_comma(out);
	pv_key(out, "erasesize_reported_b");
	pv_num(out, (long long)t.p[0].erasesize);
	pv_comma(out);
	/*
	 * A read-only mapping (map_rom) reports the whole chip as its
	 * erasesize and erases nothing; a flash driver reports its sector.
	 */
	pv_key(out, "erase_block_b");
	if (t.n && t.p[0].erasesize < mtd_chip_size(&t))
		pv_num(out, (long long)t.p[0].erasesize);
	else
		pv_null(out);
	pv_close(out, '}');

	pv_comma(out);
	pv_key(out, "partitions");
	emit_partitions(out, &t);

	pv_comma(out);
	pv_key(out, "partitions_dropped");
	pv_num(out, t.dropped);

	pv_comma(out);
	pv_key(out, "env");
	emit_env(out, &a, &b, live);

	pv_comma(out);
	pv_key(out, "selection");
	emit_selection(out, live);

	pv_comma(out);
	pv_key(out, "banks");
	emit_banks(out, &t, live);

	pv_comma(out);
	pv_key(out, "rootfs");
	emit_rootfs(out, &t);

	pv_close(out, '}');
	return 0;
}
