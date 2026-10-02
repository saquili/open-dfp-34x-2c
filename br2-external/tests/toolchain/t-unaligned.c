/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Reads and writes of 16, 32 and 64 bits at every byte offset, through memcpy
 * and through a packed structure. The compiler turns these into the unaligned
 * load and store instructions; the bytes are compared with values assembled one
 * byte at a time in big-endian order.
 */
#include <stdint.h>
#include <string.h>
#include "check.h"

struct __attribute__((packed)) wire {
	uint8_t tag;
	uint16_t len;
	uint32_t id;
	uint64_t stamp;
};

static uint64_t be_load(const unsigned char *p, unsigned int n)
{
	uint64_t v = 0;

	while (n--)
		v = (v << 8) | *p++;
	return v;
}

static int only_changed(const unsigned char *out, unsigned int from, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < 32; i++)
		if ((i < from || i >= from + n) && out[i] != 0xEE)
			return 0;
	return 1;
}

int main(void)
{
	unsigned char buf[32], out[32];
	unsigned int off, i;
	struct wire w;
	const struct wire *pw;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(0x10 + i * 3);

	for (off = 0; off < 8; off++) {
		volatile unsigned int o = off;
		uint16_t h;
		uint32_t l;
		uint64_t q;

		memcpy(&h, buf + o, 2);			/* loads: the core is big endian */
		memcpy(&l, buf + o, 4);
		memcpy(&q, buf + o, 8);
		CHECK(h == be_load(buf + off, 2));
		CHECK(l == be_load(buf + off, 4));
		CHECK(q == be_load(buf + off, 8));

		h = 0xBEEF;				/* stores touch exactly their bytes */
		memset(out, 0xEE, sizeof(out));
		memcpy(out + 8 + o, &h, 2);
		CHECK(be_load(out + 8 + off, 2) == 0xBEEF && only_changed(out, 8 + off, 2));
		l = 0xCAFEF00D;
		memset(out, 0xEE, sizeof(out));
		memcpy(out + 8 + o, &l, 4);
		CHECK(be_load(out + 8 + off, 4) == 0xCAFEF00D && only_changed(out, 8 + off, 4));
		q = 0x0123456789ABCDEFULL;
		memset(out, 0xEE, sizeof(out));
		memcpy(out + 8 + o, &q, 8);
		CHECK(be_load(out + 8 + off, 8) == 0x0123456789ABCDEFULL
		      && only_changed(out, 8 + off, 8));
	}

	memset(&w, 0, sizeof(w));
	w.tag = 0x7e;
	w.len = 0x1234;
	w.id = 0xA1B2C3D4;
	w.stamp = 0x1122334455667788ULL;
	CHECK(sizeof(w) == 15);
	memcpy(buf, &w, sizeof(w));
	CHECK(buf[0] == 0x7e && buf[1] == 0x12 && buf[2] == 0x34);
	CHECK(buf[3] == 0xA1 && buf[4] == 0xB2 && buf[5] == 0xC3 && buf[6] == 0xD4);
	CHECK(buf[7] == 0x11 && buf[14] == 0x88);

	pw = (const struct wire *)buf;			/* read it back field by field */
	CHECK(pw->len == 0x1234 && pw->id == 0xA1B2C3D4 && pw->stamp == 0x1122334455667788ULL);
	memmove(buf + 1, buf, sizeof(w));		/* the same structure one byte on */
	pw = (const struct wire *)(buf + 1);
	CHECK(pw->tag == 0x7e && pw->len == 0x1234 && pw->id == 0xA1B2C3D4
	      && pw->stamp == 0x1122334455667788ULL);
	DONE("unaligned");
}
