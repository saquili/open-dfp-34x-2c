/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * pond answers each request for a value of the device with a provider: a
 * function that reads the value from the kernel and writes it as JSON. The
 * table of providers is in table.c; the JSON writers and the file helpers
 * are here. A provider returns 0, or a negative errno.
 */

#ifndef POND_PROV_H
#define POND_PROV_H

#include <stdbool.h>
#include <stddef.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/**
 * struct prov_buf - A JSON text being written.
 * @buf: where it is written
 * @cap: room in @buf
 * @len: what is written so far
 * @overflow: set when something did not fit; the text is then not used
 */
struct prov_buf {
	char *buf;
	size_t cap;
	size_t len;
	bool overflow;
};

/* Write JSON: raw text, strings, numbers, a fixed-point number, true or false, null. */
void pv_raw(struct prov_buf *b, const char *s);
void pv_str(struct prov_buf *b, const char *s);
void pv_strn(struct prov_buf *b, const char *s, size_t n);
void pv_num(struct prov_buf *b, long long v);
void pv_fixed(struct prov_buf *b, long long scaled, unsigned int decimals);
void pv_bool(struct prov_buf *b, bool v);
void pv_null(struct prov_buf *b);

/* Open or close an object or array, write a key, a comma between members. */
void pv_open(struct prov_buf *b, char brace);
void pv_close(struct prov_buf *b, char brace);
void pv_key(struct prov_buf *b, const char *k);
void pv_comma(struct prov_buf *b);

/**
 * enum prov_flags - What a provider is like.
 * @PROV_VOLATILE: its value changes from one read to the next
 * @PROV_SLOW: it takes long to read, and is only read on its own
 * @PROV_ABSENT: it is listed but not there on this device
 */
enum prov_flags {

	PROV_VOLATILE = 1u << 0,

	PROV_SLOW = 1u << 1,

	PROV_ABSENT = 1u << 2,
};

/**
 * struct prov - A value of the device.
 * @id: its name in the API
 * @cat: the group it is shown in
 * @label: what it is, in a few words
 * @read: write the value; NULL when it cannot be read
 * @write: change it; NULL when it cannot be changed
 * @flags: &enum prov_flags
 */
struct prov {
	const char *id;
	const char *cat;
	const char *label;
	int (*read)(struct prov_buf *out);
	int (*write)(const char *val, char *err, size_t errcap);
	unsigned int flags;
};

extern const struct prov pond_provs[];
extern const size_t pond_nprovs;

/* Find a provider by name, and tell whether it is there. */
const struct prov *prov_find(const char *id);
bool prov_present(const struct prov *p);

/*
 * Files: a path under POND_ROOT when it is set (the tests point it at
 * tests/fixtures), the whole of a file, its first line, and a number from a
 * "key: value" text.
 */
const char *prov_path(const char *path, char *buf, size_t cap);
int prov_slurp(const char *path, char *out, size_t cap, size_t *len);
int prov_read_line(const char *path, char *out, size_t cap);
long long prov_field(const char *text, const char *key, long long absent);

#endif
