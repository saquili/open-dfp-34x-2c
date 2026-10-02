// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the factory configuration reader, on JFFS2 images made here. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <linux/jffs2.h>
#include <zlib.h>

#include "factory.h"

static int failures;
static int checks;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static uint8_t g_img[16384];
static size_t g_len;

static void put16(size_t at, uint16_t v)
{
	g_img[at] = (uint8_t)(v >> 8);
	g_img[at + 1] = (uint8_t)v;
}

static void put32(size_t at, uint32_t v)
{
	g_img[at] = (uint8_t)(v >> 24);
	g_img[at + 1] = (uint8_t)(v >> 16);
	g_img[at + 2] = (uint8_t)(v >> 8);
	g_img[at + 3] = (uint8_t)v;
}

/* Start a node of @type and @size bytes on a four-byte boundary. */
static size_t node(uint16_t type, size_t size)
{
	const size_t at = (g_len + 3u) & ~(size_t)3u;

	memset(g_img + g_len, 0xff, at - g_len);
	memset(g_img + at, 0, size);
	put16(at, JFFS2_MAGIC_BITMASK);
	put16(at + 2, type);
	put32(at + 4, (uint32_t)size);
	g_len = at + size;
	return at;
}

static void dirent(const char *name, uint32_t ino, uint32_t version)
{
	const size_t nlen = strlen(name);
	const size_t at = node(JFFS2_NODETYPE_DIRENT,
			       sizeof(struct jffs2_raw_dirent) + nlen);

	put32(at + offsetof(struct jffs2_raw_dirent, version), version);
	put32(at + offsetof(struct jffs2_raw_dirent, ino), ino);
	g_img[at + offsetof(struct jffs2_raw_dirent, nsize)] = (uint8_t)nlen;
	memcpy(g_img + at + sizeof(struct jffs2_raw_dirent), name, nlen);
}

static void inode(uint32_t ino, uint32_t version, uint32_t offset,
		  const char *text, uint8_t compr)
{
	const size_t dsize = strlen(text);
	uint8_t packed[1024];
	uLongf csize = sizeof(packed);

	if (compr == JFFS2_COMPR_ZLIB)
		(void)compress2(packed, &csize, (const Bytef *)text, dsize, 9);
	else
		csize = dsize;

	const size_t at = node(JFFS2_NODETYPE_INODE,
			       sizeof(struct jffs2_raw_inode) + csize);

	put32(at + offsetof(struct jffs2_raw_inode, ino), ino);
	put32(at + offsetof(struct jffs2_raw_inode, version), version);
	put32(at + offsetof(struct jffs2_raw_inode, offset), offset);
	put32(at + offsetof(struct jffs2_raw_inode, csize), (uint32_t)csize);
	put32(at + offsetof(struct jffs2_raw_inode, dsize), (uint32_t)dsize);
	g_img[at + offsetof(struct jffs2_raw_inode, compr)] = compr;
	memcpy(g_img + at + sizeof(struct jffs2_raw_inode),
	       compr == JFFS2_COMPR_ZLIB ? (const void *)packed : text, csize);
}

static void reset(void)
{
	memset(g_img, 0xff, sizeof(g_img));
	g_len = 0;
}

static void test_a_compressed_file_is_read_back(void)
{
	uint8_t out[256];
	size_t n = 0;
	char v[32];

	reset();
	dirent("unit.xml", 7, 1);
	inode(7, 1, 0,
	      "<V Name=\"A\" Value=\"one\"/><V Name=\"B\" Value=\"2\"/>",
	      JFFS2_COMPR_ZLIB);

	CHECK(factory_file(g_img, g_len, "unit.xml", out, sizeof(out) - 1,
			   &n) == 0);
	out[n] = '\0';
	CHECK(factory_value((const char *)out, "A", v, sizeof(v)) == 0);
	CHECK(strcmp(v, "one") == 0);
	CHECK(factory_value((const char *)out, "B", v, sizeof(v)) == 0);
	CHECK(strcmp(v, "2") == 0);
	CHECK(factory_value((const char *)out, "C", v, sizeof(v)) == -ENOENT);
}

static void test_the_newer_write_wins_whatever_the_order_on_flash(void)
{
	uint8_t out[256];
	size_t n = 0;

	reset();
	dirent("f", 3, 1);
	inode(3, 5, 0, "NEW", JFFS2_COMPR_NONE);
	inode(3, 2, 0, "OLD-TEXT", JFFS2_COMPR_ZLIB);

	CHECK(factory_file(g_img, g_len, "f", out, sizeof(out), &n) == 0);
	CHECK(n == 8);
	CHECK(memcmp(out, "NEW-TEXT", 8) == 0);
}

static void test_the_newest_name_points_at_the_file(void)
{
	uint8_t out[64];
	size_t n = 0;

	reset();
	dirent("f", 3, 1);
	dirent("f", 4, 2);
	inode(3, 1, 0, "first", JFFS2_COMPR_NONE);
	inode(4, 1, 0, "second", JFFS2_COMPR_NONE);

	CHECK(factory_file(g_img, g_len, "f", out, sizeof(out), &n) == 0);
	CHECK(n == 6 && memcmp(out, "second", 6) == 0);
}

static void test_what_cannot_be_read_says_why(void)
{
	uint8_t out[8];
	size_t n = 0;

	reset();
	dirent("f", 3, 1);
	inode(3, 1, 0, "too long for the room", JFFS2_COMPR_NONE);
	CHECK(factory_file(g_img, g_len, "f", out, sizeof(out), &n) == -EFBIG);
	CHECK(factory_file(g_img, g_len, "g", out, sizeof(out), &n) == -ENOENT);

	reset();
	dirent("f", 3, 1);
	inode(3, 1, 0, "x", 99);
	CHECK(factory_file(g_img, g_len, "f", out, sizeof(out), &n) ==
	      -ENOTSUP);

	reset();
	dirent("f", 3, 1);
	inode(3, 1, 0, "abc", JFFS2_COMPR_ZLIB);
	g_img[g_len - 2] ^= 0x55;
	CHECK(factory_file(g_img, g_len, "f", out, sizeof(out), &n) == -EILSEQ);
}

static void test_values_without_an_end_are_refused(void)
{
	char v[4];

	CHECK(factory_value("Name=\"A\"", "A", v, sizeof(v)) == -EILSEQ);
	CHECK(factory_value("Name=\"A\" Value=\"1", "A", v, sizeof(v)) ==
	      -EILSEQ);
	CHECK(factory_value("Name=\"A\" Value=\"12345\"", "A", v, sizeof(v)) ==
	      -EFBIG);
	CHECK(factory_value("Name=\"AB\" Value=\"1\"", "A", v, sizeof(v)) ==
	      -ENOENT);
}

int main(void)
{
	test_a_compressed_file_is_read_back();
	test_the_newer_write_wins_whatever_the_order_on_flash();
	test_the_newest_name_points_at_the_file();
	test_what_cannot_be_read_says_why();
	test_values_without_an_end_are_refused();
	printf("test_factory: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
