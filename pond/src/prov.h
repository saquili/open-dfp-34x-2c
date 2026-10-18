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
 * @max: the room @grow may give @buf at most, the text's end included
 * @grow: give @buf at least @cap bytes of room, what is written kept, and
 *	say whether it did; NULL for a buffer of a fixed size
 * @ctx: the caller's, for @grow
 *
 * A buffer that grows is as large as its text, and no larger than @max: a
 * value takes the memory it needs only while it is written and sent, and
 * one that would pass @max overflows, as a fixed buffer does when full.
 */
struct prov_buf {
	char *buf;
	size_t cap;
	size_t len;
	bool overflow;
	size_t max;
	bool (*grow)(struct prov_buf *b, size_t cap);
	void *ctx;
};

/*
 * Make room in @b for @n more bytes, as far as it grows: for a provider
 * that has another program write into the buffer, which it cannot grow.
 * pv_room() is the room there is now, for a text and its end, which such a
 * provider gives that program: none once @b has overflowed.
 */
void pv_reserve(struct prov_buf *b, size_t n);
size_t pv_room(const struct prov_buf *b);

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

/*
 * The end of a change that a request waits for: @ctx as given, 0 or a
 * negative errno, and for an error why, or NULL.
 */
typedef void (*prov_done)(void *ctx, int rc, const char *err);

/**
 * struct prov - A value of the device.
 * @id: its name in the API
 * @cat: the group it is shown in
 * @label: what it is, in a few words
 * @read: write the value; NULL when it cannot be read
 * @write: change it; NULL when it cannot be changed. A change that has
 *	onu-act run a program is not waited for: it returns -EINPROGRESS, and
 *	@done is called with @ctx when it ends. Any other return is the end,
 *	with why in @err for an error, and @done is not called.
 * @flags: &enum prov_flags
 */
struct prov {
	const char *id;
	const char *cat;
	const char *label;
	int (*read)(struct prov_buf *out);
	int (*write)(const char *val, char *err, size_t errcap, prov_done done,
		     void *ctx);
	unsigned int flags;
};

/**
 * struct prov_change - A change under way, for a provider that has one at a
 *	time: whom its end is told to.
 * @done: the caller's, NULL while none is under way
 * @ctx: the caller's
 */
struct prov_change {
	prov_done done;
	void *ctx;
};

/*
 * Take @c for a change that @done is told the end of: 0, or -EBUSY, with
 * why in @err, while another is under way.
 */
int prov_change_take(struct prov_change *c, prov_done done, void *ctx,
		     char *err, size_t errcap);

/* Give @c up without telling anyone: the change did not start. */
void prov_change_drop(struct prov_change *c);

/* End the change of @c with @rc and @err, and tell its caller. */
void prov_change_end(struct prov_change *c, int rc, const char *err);

/**
 * struct prov_wait - A change waited for, by a caller that may block.
 * @ended: whether it has ended
 * @rc: how
 * @err: where why goes, @errcap bytes
 */
struct prov_wait {
	bool ended;
	int rc;
	char *err;
	size_t errcap;
};

/* The &prov_done of a &struct prov_wait, which is @ctx. */
void prov_waited(void *ctx, int rc, const char *err);

/*
 * Wait for the change whose start returned @rc and tells @w its end: the
 * result of the change. The tests, and the callers that may block, use it.
 */
int prov_wait(int rc, struct prov_wait *w);

extern const struct prov pond_provs[];
extern const size_t pond_nprovs;

/* Find a provider by name, and tell whether it is there. */
const struct prov *prov_find(const char *id);

/**
 * struct prov_from - A value that is a log a reader follows: read from the
 *	point the reader names with since=N in the query. Its &struct prov
 *	reads it from the start.
 * @id: its name in the API, as its &struct prov has it
 * @read: write the value from the point @since on
 */
struct prov_from {
	const char *id;
	int (*read)(struct prov_buf *out, unsigned long since);
};

extern const struct prov_from pond_provs_from[];
extern const size_t pond_nprovs_from;

/* Find how to read a value from a point, or NULL for a value that has none. */
const struct prov_from *prov_find_from(const char *id);
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

/*
 * Run @prog (found on PATH) with @args, a NULL-ended list, @input on its
 * standard input when not NULL, and its output, both streams, in @out:
 * its exit status, 128 and the signal for one that killed it, or a
 * negative errno when it could not run.
 */
int prov_exec(const char *prog, const char *const *args, const char *input,
	      char *out, size_t cap);

#endif
