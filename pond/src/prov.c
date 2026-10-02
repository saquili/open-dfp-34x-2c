// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The JSON writers and the file helpers of prov.h. */

#include "prov.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool room(struct prov_buf *b, size_t need)
{
	if (b->overflow)
		return false;
	if (b->len + need + 1 > b->cap) {
		b->overflow = true;
		if (b->cap)
			b->buf[b->len] = '\0';
		return false;
	}
	return true;
}

void pv_raw(struct prov_buf *b, const char *s)
{
	size_t n = strlen(s);

	if (!room(b, n))
		return;
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

void pv_strn(struct prov_buf *b, const char *s, size_t n)
{
	size_t i;

	if (!room(b, 2))
		return;
	b->buf[b->len++] = '"';
	for (i = 0; i < n && s[i]; i++) {
		unsigned char c = (unsigned char)s[i];
		char esc[7];
		const char *add = esc;
		size_t addn = 1;

		switch (c) {
		case '"':
			add = "\\\"";
			addn = 2;
			break;
		case '\\':
			add = "\\\\";
			addn = 2;
			break;
		case '\n':
			add = "\\n";
			addn = 2;
			break;
		case '\r':
			add = "\\r";
			addn = 2;
			break;
		case '\t':
			add = "\\t";
			addn = 2;
			break;
		default:
			if (c < 0x20 || c == 0x7f) {
				snprintf(esc, sizeof(esc), "\\u%04x", c);
				addn = 6;
			} else {
				esc[0] = (char)c;
				addn = 1;
			}
		}
		if (!room(b, addn + 1))
			return;
		memcpy(b->buf + b->len, add, addn);
		b->len += addn;
	}
	if (!room(b, 1))
		return;
	b->buf[b->len++] = '"';
	b->buf[b->len] = '\0';
}

void pv_str(struct prov_buf *b, const char *s)
{
	pv_strn(b, s, strlen(s));
}

void pv_num(struct prov_buf *b, long long v)
{
	char t[24];

	snprintf(t, sizeof(t), "%lld", v);
	pv_raw(b, t);
}

void pv_fixed(struct prov_buf *b, long long scaled, unsigned int decimals)
{
	long long div = 1;
	unsigned int i;
	char t[40];

	for (i = 0; i < decimals; i++)
		div *= 10;
	if (scaled < 0)
		snprintf(t, sizeof(t), "-%lld.%0*lld", -scaled / div,
			 (int)decimals, -scaled % div);
	else
		snprintf(t, sizeof(t), "%lld.%0*lld", scaled / div,
			 (int)decimals, scaled % div);
	pv_raw(b, t);
}

void pv_bool(struct prov_buf *b, bool v)
{
	pv_raw(b, v ? "true" : "false");
}

void pv_null(struct prov_buf *b)
{
	pv_raw(b, "null");
}

void pv_open(struct prov_buf *b, char brace)
{
	char t[2] = { brace, '\0' };

	pv_raw(b, t);
}

void pv_close(struct prov_buf *b, char brace)
{
	char t[2] = { brace, '\0' };

	pv_raw(b, t);
}

void pv_key(struct prov_buf *b, const char *k)
{
	pv_str(b, k);
	pv_raw(b, ":");
}

void pv_comma(struct prov_buf *b)
{
	pv_raw(b, ",");
}

const struct prov *prov_find(const char *id)
{
	size_t i;

	for (i = 0; i < pond_nprovs; i++)
		if (!strcmp(pond_provs[i].id, id))
			return &pond_provs[i];
	return NULL;
}

bool prov_present(const struct prov *p)
{
	return p && !(p->flags & PROV_ABSENT) && (p->read || p->write);
}

const char *prov_path(const char *path, char *buf, size_t cap)
{
	const char *root = getenv("POND_ROOT");

	if (!root || !*root)
		return path;
	snprintf(buf, cap, "%s%s", root, path);
	return buf;
}

int prov_slurp(const char *path, char *out, size_t cap, size_t *len)
{
	size_t got = 0;
	int fd;
	char rooted[512];

	if (cap < 2)
		return -EINVAL;
	fd = open(prov_path(path, rooted, sizeof(rooted)),
		  O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	for (;;) {
		ssize_t n = read(fd, out + got, cap - 1 - got);

		if (n < 0) {
			int err = -errno;

			if (errno == EINTR)
				continue;
			close(fd);
			return err;
		}
		if (n == 0)
			break;
		got += (size_t)n;
		if (got >= cap - 1) {
			close(fd);
			return -EFBIG;
		}
	}
	close(fd);
	out[got] = '\0';
	if (len)
		*len = got;
	return 0;
}

int prov_read_line(const char *path, char *out, size_t cap)
{
	char *nl;
	int err = prov_slurp(path, out, cap, NULL);

	if (err)
		return err;
	nl = strchr(out, '\n');
	if (nl)
		*nl = '\0';
	return 0;
}

long long prov_field(const char *text, const char *key, long long absent)
{
	size_t klen = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, klen) && p[klen] == ':') {
			const char *v = p + klen + 1;

			while (*v == ' ' || *v == '\t')
				v++;
			return strtoll(v, NULL, 10);
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return absent;
}
