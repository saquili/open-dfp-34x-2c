// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The reader of the factory configuration of factory.h. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <linux/jffs2.h>
#include <zlib.h>

#include "factory.h"

/* The most nodes of one file, and the most directory entries, it looks at. */
#define MAX_NODES 256u
#define MAX_DIRENTS 256u

struct data_node {
	uint32_t version;
	size_t at;
};

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

#define DIRENT(field) offsetof(struct jffs2_raw_dirent, field)
#define INODE(field) offsetof(struct jffs2_raw_inode, field)

/*
 * The next node at or after @off: its offset, or @len when there is none.
 * Nodes start on a four-byte boundary; anything without the magic number is
 * erased or stale flash, and is stepped over.
 */
static size_t next_node(const uint8_t *img, size_t len, size_t off)
{
	for (; off + sizeof(struct jffs2_unknown_node) <= len; off += 4) {
		const uint32_t totlen = be32(img + off + 4);

		if (be16(img + off) == JFFS2_MAGIC_BITMASK &&
		    totlen >= sizeof(struct jffs2_unknown_node) &&
		    totlen <= len - off)
			return off;
	}
	return len;
}

static size_t node_end(const uint8_t *img, size_t off)
{
	return off + ((be32(img + off + 4) + 3u) & ~(size_t)3u);
}

/* The inode of the newest directory entry named @name, or 0. */
static uint32_t find_inode(const uint8_t *img, size_t len, const char *name)
{
	const size_t nlen = strlen(name);
	uint32_t ino = 0, newest = 0;
	bool found = false;

	for (size_t off = next_node(img, len, 0); off < len;
	     off = next_node(img, len, node_end(img, off))) {
		if (be16(img + off + 2) != JFFS2_NODETYPE_DIRENT ||
		    off + sizeof(struct jffs2_raw_dirent) > len)
			continue;

		const uint8_t nsize = img[off + DIRENT(nsize)];
		const size_t name_at = off + sizeof(struct jffs2_raw_dirent);
		const uint32_t version = be32(img + off + DIRENT(version));

		if (nsize != nlen || name_at + nsize > len ||
		    memcmp(img + name_at, name, nlen) != 0)
			continue;
		if (!found || version >= newest) {
			ino = be32(img + off + DIRENT(ino));
			newest = version;
			found = true;
		}
	}
	return ino;
}

static int inflate_node(const uint8_t *in, size_t inlen, uint8_t *out,
			size_t outlen)
{
	z_stream z = {
		.next_in = (Bytef *)in,
		.avail_in = (uInt)inlen,
		.next_out = out,
		.avail_out = (uInt)outlen,
	};
	int rc;

	if (inflateInit(&z) != Z_OK)
		return -ENOMEM;
	rc = inflate(&z, Z_FINISH);
	inflateEnd(&z);
	return rc == Z_STREAM_END && z.total_out == outlen ? 0 : -EILSEQ;
}

/* Lay down the data of one inode node. */
static int apply_node(const uint8_t *img, size_t len, size_t off, uint8_t *out,
		      size_t cap, size_t *outlen)
{
	const uint32_t at = be32(img + off + INODE(offset));
	const uint32_t csize = be32(img + off + INODE(csize));
	const uint32_t dsize = be32(img + off + INODE(dsize));
	const uint8_t compr = img[off + INODE(compr)];
	const size_t data = off + sizeof(struct jffs2_raw_inode);
	int err = 0;

	if (data + csize > len || at > cap || dsize > cap - at)
		return -EFBIG;

	switch (compr) {
	case JFFS2_COMPR_NONE:
		if (csize < dsize)
			return -EILSEQ;
		memcpy(out + at, img + data, dsize);
		break;
	case JFFS2_COMPR_ZERO:
		memset(out + at, 0, dsize);
		break;
	case JFFS2_COMPR_ZLIB:
		err = inflate_node(img + data, csize, out + at, dsize);
		break;
	default:
		return -ENOTSUP;
	}
	if (!err && at + dsize > *outlen)
		*outlen = at + dsize;
	return err;
}

int factory_file(const uint8_t *img, size_t len, const char *name, uint8_t *out,
		 size_t cap, size_t *outlen)
{
	struct data_node nodes[MAX_NODES];
	size_t n = 0;
	const uint32_t ino = find_inode(img, len, name);

	*outlen = 0;
	if (ino == 0)
		return -ENOENT;

	for (size_t off = next_node(img, len, 0); off < len;
	     off = next_node(img, len, node_end(img, off))) {
		if (be16(img + off + 2) != JFFS2_NODETYPE_INODE ||
		    off + sizeof(struct jffs2_raw_inode) > len ||
		    be32(img + off + INODE(ino)) != ino)
			continue;
		if (n == MAX_NODES)
			return -EFBIG;
		nodes[n].version = be32(img + off + INODE(version));
		nodes[n].at = off;
		n++;
	}

	/* The older a node, the earlier it is laid down. */
	for (size_t i = 1; i < n; i++)
		for (size_t j = i;
		     j > 0 && nodes[j - 1].version > nodes[j].version; j--) {
			const struct data_node t = nodes[j];

			nodes[j] = nodes[j - 1];
			nodes[j - 1] = t;
		}

	memset(out, 0, cap);
	for (size_t i = 0; i < n; i++) {
		const int err =
			apply_node(img, len, nodes[i].at, out, cap, outlen);

		if (err)
			return err;
	}
	return *outlen ? 0 : -ENOENT;
}

int factory_value(const char *doc, const char *name, char *out, size_t cap)
{
	const size_t nlen = strlen(name);

	for (const char *p = strstr(doc, "Name=\""); p != NULL;
	     p = strstr(p, "Name=\"")) {
		p += strlen("Name=\"");
		if (strncmp(p, name, nlen) != 0 || p[nlen] != '"')
			continue;

		const char *v = strstr(p + nlen, "Value=\"");

		if (v == NULL)
			return -EILSEQ;
		v += strlen("Value=\"");

		const char *end = strchr(v, '"');

		if (end == NULL)
			return -EILSEQ;
		if ((size_t)(end - v) + 1 > cap)
			return -EFBIG;
		memcpy(out, v, (size_t)(end - v));
		out[end - v] = '\0';
		return 0;
	}
	return -ENOENT;
}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The five escapes of XML and what each stands for. */
static const struct {
	const char *name;
	char c;
} ENTITIES[] = {
	{ "&amp;", '&' }, { "&lt;", '<' },   { "&gt;", '>' },
	{ "&quot;", '"' }, { "&apos;", '\'' },
};

/* The escape at @p: its index in ENTITIES, or ARRAY_SIZE(ENTITIES). */
static size_t entity_at(const char *p)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(ENTITIES); i++)
		if (strncmp(p, ENTITIES[i].name, strlen(ENTITIES[i].name)) ==
		    0)
			break;
	return i;
}

int factory_text(char *value)
{
	char *out = value;

	/* Every '&' first, so that a value changes whole or not at all. */
	for (const char *p = strchr(value, '&'); p != NULL;
	     p = strchr(p + 1, '&'))
		if (entity_at(p) == ARRAY_SIZE(ENTITIES))
			return -EILSEQ;

	for (const char *p = value; *p != '\0';) {
		const size_t i = *p == '&' ? entity_at(p) : ARRAY_SIZE(ENTITIES);

		if (i < ARRAY_SIZE(ENTITIES)) {
			*out++ = ENTITIES[i].c;
			p += strlen(ENTITIES[i].name);
		} else {
			*out++ = *p++;
		}
	}
	*out = '\0';
	return 0;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int factory_mac(const char *value, char *out, size_t cap)
{
	const size_t len = strlen(value);
	const bool colons = len == 17;
	bool zero = true;
	uint8_t a[6];

	if (len != 12 && !colons)
		return -EINVAL;
	for (size_t i = 0; i < 6; i++) {
		const char *p = value + i * (colons ? 3 : 2);
		const int hi = hex_digit(p[0]);
		const int lo = hex_digit(p[1]);

		if (hi < 0 || lo < 0 || (colons && i < 5 && p[2] != ':'))
			return -EINVAL;
		a[i] = (uint8_t)(hi << 4 | lo);
		zero = zero && a[i] == 0;
	}
	/* The low bit of the first octet marks a group address. */
	if (zero || (a[0] & 1))
		return -EINVAL;
	if (cap < 18)
		return -EFBIG;
	snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2],
		 a[3], a[4], a[5]);
	return 0;
}

/*
 * A dotted quad, each part 0 to 255 without a leading zero, which some
 * readers take for octal. Return: false for another form.
 */
static bool quad(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	for (int i = 0; i < 4; i++) {
		unsigned int n = 0;
		int digits = 0;

		if (s[0] == '0' && s[1] >= '0' && s[1] <= '9')
			return false;
		while (*s >= '0' && *s <= '9' && digits < 3) {
			n = n * 10 + (unsigned int)(*s++ - '0');
			digits++;
		}
		if (digits == 0 || n > 255)
			return false;
		v = v << 8 | n;
		if (i < 3 && *s++ != '.')
			return false;
	}
	if (*s != '\0')
		return false;
	*out = v;
	return true;
}

int factory_prefix(const char *addr, const char *mask, char *out, size_t cap)
{
	unsigned int len = 0;
	uint32_t a, m, host;

	if (!quad(addr, &a) || !quad(mask, &m))
		return -EINVAL;
	while (len < 32 && (m & (UINT32_C(0x80000000) >> len)))
		len++;
	if (len < 8 || len > 30 || m != (uint32_t)(~UINT32_C(0) << (32 - len)))
		return -EINVAL;
	host = a & ~m;
	if (a >> 24 == 0 || a >> 24 == 127 || a >> 24 >= 224 || host == 0 ||
	    host == ~m)
		return -EINVAL;
	if (cap < 19)
		return -EFBIG;
	snprintf(out, cap, "%u.%u.%u.%u/%u", a >> 24, a >> 16 & 255u,
		 a >> 8 & 255u, a & 255u, len);
	return 0;
}
