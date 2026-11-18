/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* applyutil.h: what applyutil.c gives the other parts of onu-apply. */

#ifndef APPLYUTIL_H
#define APPLYUTIL_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
/* A value of a setting or of a fact of /run, and a line of a file. */
#define VAL 512

/* Text that grows: what goes to a file or to a program. */
struct buf {
	char *p;
	size_t len, cap;
};

/* A list of words or lines. */
struct words {
	char **v;
	size_t n, cap;
};

/* A map of text to text, as awk's arrays. */
struct map {
	struct words k, v;
};

/* How a program runs (run()). */
enum {
	RUN_QUIET_ERR = 1, /* 2>/dev/null */
	RUN_QUIET_OUT = 2, /* >/dev/null */
};

#define PATHF(buf, ...) pathf(buf, sizeof(buf), __VA_ARGS__)

extern char g_root[PATH_MAX];

__attribute((noreturn)) void out_of_memory(void);
__attribute((format(printf, 1, 2))) void log_msg(const char *fmt, ...);
void buf_add(struct buf *b, const char *s, size_t n);
void buf_puts(struct buf *b, const char *s);
__attribute((format(printf, 2, 3))) void buf_printf(struct buf *b, const char *fmt, ...);
const char *buf_str(const struct buf *b);
void buf_reset(struct buf *b);
void buf_free(struct buf *b);
void buf_chomp(struct buf *b);
void words_addn(struct words *w, const char *s, size_t len);
void words_add(struct words *w, const char *s);
void words_free(struct words *w);
void words_split(struct words *w, const char *s);
void words_lines(struct words *w, const char *s);
void words_fields(struct words *w, const char *s, const char *seps);
long num_of(const char *s);
void words_sort(struct words *w, bool numeric, bool unique);
bool words_has(const struct words *w, const char *s);
void words_join(const struct words *w, const char *sep, struct buf *b);
__attribute((format(printf, 3, 4))) char *pathf(char *out, size_t cap, const char *fmt, ...);
void copy_val(char *out, const char *s);
__attribute((format(printf, 2, 3))) char *valf(char *out, const char *fmt, ...);
bool starts(const char *s, const char *prefix);
bool exists(const char *path);
bool is_dir(const char *path);
bool readable(const char *path);
bool nonempty(const char *path);
bool cat(const char *path, struct buf *b);
char *cat_val(const char *path, char *out);
void read_words(const char *path, char *a, char *rest);
char *read_line(const char *path, char *out);
bool write_text(const char *path, const char *text, size_t len);
bool write_line(const char *path, const char *s);
void rm_f(const char *path);
void rm_rf(const char *path);
bool mkdir_p(const char *path);
bool copy_file(const char *from, const char *to);
bool copy_tree(const char *from, const char *to);
void dir_names(const char *path, struct words *w);
bool give_group(const char *path, const char *group, mode_t mode);
void sleep_tenths(int tenths);
__attribute((sentinel)) int run(int flags, const char *prog, ...);
__attribute((sentinel)) int run_in(int flags, const struct buf *in, const char *prog, ...);
__attribute((sentinel)) int run_out(int flags, struct buf *out, const char *prog, ...);
bool command_v(const char *name, char *out, size_t cap);
bool have(const char *name);
bool only_of(const char *s, const char *set);
void map_set(struct map *m, const char *k, const char *v);
const char *map_get(const struct map *m, const char *k);
void map_free(struct map *m);
char *lower(const char *s, char *out);
const char *field(const struct words *w, size_t i);
const char *after_colon(const char *s);
uint32_t ip_num(const char *s);
char *num_ip(uint32_t n, char *out);
uint32_t len_mask(long len);
long addr_len(const char *addr);
char *addr_only(const char *addr, char *out);
bool in_list(const int *list, int count, int n);
const char *keyed(const char *s, const char *prefix, bool need);

#endif
