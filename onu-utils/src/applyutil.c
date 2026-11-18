// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyutil: what onu-apply needs that the shell gave its script for nothing:
 * text that grows, lists of words and a map of them, the files of /run and
 * /sys as the shell's tests look at them, addresses of IPv4 as numbers, and
 * the programs it runs, with their input and their output.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "applyutil.h"

/* The root of the files it reads and writes, ONU_ROOT's (applyconf's paths()). */
char g_root[PATH_MAX];

__attribute((noreturn)) void out_of_memory(void)
{
	fputs("onu-apply: out of memory\n", stderr);
	exit(1);
}

__attribute((format(printf, 1, 2))) void log_msg(const char *fmt, ...)
{
	char msg[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(stderr, "onu-apply: %s\n", msg);
	syslog(LOG_NOTICE, "%s", msg);
}

void buf_add(struct buf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 256;

		while (b->len + n + 1 > cap)
			cap *= 2;
		b->p = realloc(b->p, cap);
		if (!b->p)
			out_of_memory();
		b->cap = cap;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
}

void buf_puts(struct buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

__attribute((format(printf, 2, 3))) void buf_printf(struct buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (b->len + (size_t)n + 1 > b->cap) {
		buf_add(b, "", 0);
		while (b->len + (size_t)n + 1 > b->cap) {
			b->p = realloc(b->p, b->cap * 2);
			if (!b->p)
				out_of_memory();
			b->cap *= 2;
		}
	}
	va_start(ap, fmt);
	vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

const char *buf_str(const struct buf *b)
{
	return b->p ? b->p : "";
}

void buf_reset(struct buf *b)
{
	b->len = 0;
	if (b->p)
		b->p[0] = '\0';
}

void buf_free(struct buf *b)
{
	free(b->p);
	b->p = NULL;
	b->len = 0;
	b->cap = 0;
}

/* The trailing newlines off, as the shell's command substitution takes them. */
void buf_chomp(struct buf *b)
{
	while (b->len && b->p[b->len - 1] == '\n')
		b->p[--b->len] = '\0';
}

void words_addn(struct words *w, const char *s, size_t len)
{
	if (w->n == w->cap) {
		w->cap = w->cap ? w->cap * 2 : 16;
		w->v = realloc(w->v, w->cap * sizeof(*w->v));
		if (!w->v)
			out_of_memory();
	}
	w->v[w->n] = strndup(s, len);
	if (!w->v[w->n])
		out_of_memory();
	w->n++;
}

void words_add(struct words *w, const char *s)
{
	words_addn(w, s, strlen(s));
}

void words_free(struct words *w)
{
	for (size_t i = 0; i < w->n; i++)
		free(w->v[i]);
	free(w->v);
	w->v = NULL;
	w->n = 0;
	w->cap = 0;
}

/* The words of $1, apart by blanks and newlines, as the shell splits them. */
void words_split(struct words *w, const char *s)
{
	while (*s) {
		size_t n;

		s += strspn(s, " \t\n");
		n = strcspn(s, " \t\n");
		if (n)
			words_addn(w, s, n);
		s += n;
	}
}

/* The lines of $1, an empty one too but for one after the last newline. */
void words_lines(struct words *w, const char *s)
{
	while (*s) {
		size_t n = strcspn(s, "\n");

		words_addn(w, s, n);
		s += n;
		if (*s)
			s++;
	}
}

/* The fields of $1 apart by any of SEPS, empty ones kept, as awk's split. */
void words_fields(struct words *w, const char *s, const char *seps)
{
	for (;;) {
		size_t n = strcspn(s, seps);

		words_addn(w, s, n);
		if (!s[n])
			break;
		s += n + 1;
	}
}

long num_of(const char *s)
{
	return strtol(s, NULL, 10);
}

static int cmp_words(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sort -n: by the number each begins with, then as text. */
static int cmp_numbers(const void *a, const void *b)
{
	long x = num_of(*(char *const *)a), y = num_of(*(char *const *)b);

	if (x != y)
		return x < y ? -1 : 1;
	return cmp_words(a, b);
}

/* sort, sort -n, and with UNIQUE sort -u or sort -un. */
void words_sort(struct words *w, bool numeric, bool unique)
{
	size_t k = 0;

	if (w->n < 2)
		return;
	qsort(w->v, w->n, sizeof(*w->v), numeric ? cmp_numbers : cmp_words);
	if (!unique)
		return;
	for (size_t i = 0; i < w->n; i++) {
		bool same = k && (numeric ? num_of(w->v[k - 1]) == num_of(w->v[i]) :
					    !strcmp(w->v[k - 1], w->v[i]));

		if (same)
			free(w->v[i]);
		else
			w->v[k++] = w->v[i];
	}
	w->n = k;
}

bool words_has(const struct words *w, const char *s)
{
	for (size_t i = 0; i < w->n; i++)
		if (!strcmp(w->v[i], s))
			return true;
	return false;
}

void words_join(const struct words *w, const char *sep, struct buf *b)
{
	for (size_t i = 0; i < w->n; i++) {
		if (i)
			buf_puts(b, sep);
		buf_puts(b, w->v[i]);
	}
}

__attribute((format(printf, 3, 4))) char *pathf(char *out, size_t cap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(out, cap, fmt, ap);
	va_end(ap);
	return out;
}

void copy_val(char *out, const char *s)
{
	valf(out, "%s", s);
}

/* A value into OUT, of VAL bytes, and the rest of a longer one left out. */
char *valf(char *out, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(out, VAL, fmt, ap);
	va_end(ap);
	return out;
}

bool starts(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

/* The files of /run and /sys, as the tests of the shell look at them. */

bool exists(const char *path)
{
	struct stat st;

	return !stat(path, &st);
}

bool is_dir(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

bool readable(const char *path)
{
	return !access(path, R_OK);
}

bool nonempty(const char *path)
{
	struct stat st;

	return !stat(path, &st) && st.st_size > 0;
}

/* The whole of the file $1 into B, its trailing newlines off; false unread. */
bool cat(const char *path, struct buf *b)
{
	char chunk[4096];
	ssize_t n;
	int fd;

	buf_reset(b);
	buf_add(b, "", 0);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	while ((n = read(fd, chunk, sizeof(chunk))) > 0)
		buf_add(b, chunk, (size_t)n);
	close(fd);
	buf_chomp(b);
	return n == 0;
}

/* The file $1 as one value, $(cat FILE), or nothing. */
char *cat_val(const char *path, char *out)
{
	struct buf b = { 0 };

	cat(path, &b);
	copy_val(out, buf_str(&b));
	buf_free(&b);
	return out;
}

/*
 * The first line of the file $1, as "read -r A REST" splits it: the first word
 * into A and the rest, its blanks at either end off, into REST; with REST
 * NULL the whole line, its blanks at either end off. Nothing for a file that
 * is not there.
 */
void read_words(const char *path, char *a, char *rest)
{
	struct buf b = { 0 };
	const char *s, *e;
	size_t n;

	a[0] = '\0';
	if (rest)
		rest[0] = '\0';
	if (!cat(path, &b) && !b.len) {
		buf_free(&b);
		return;
	}
	s = buf_str(&b);
	n = strcspn(s, "\n");
	e = s + n;
	while (s < e && (*s == ' ' || *s == '\t'))
		s++;
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		e--;
	if (!rest) {
		snprintf(a, VAL, "%.*s", (int)(e - s), s);
	} else {
		n = strcspn(s, " \t");
		if (s + n > e)
			n = (size_t)(e - s);
		snprintf(a, VAL, "%.*s", (int)n, s);
		s += n;
		while (s < e && (*s == ' ' || *s == '\t'))
			s++;
		snprintf(rest, VAL, "%.*s", (int)(e - s), s);
	}
	buf_free(&b);
}

char *read_line(const char *path, char *out)
{
	read_words(path, out, NULL);
	return out;
}

/* $2 into the file $1, created or emptied, with a newline: echo "$2" >$1. */
bool write_text(const char *path, const char *text, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	bool ok;

	if (fd < 0)
		return false;
	ok = write(fd, text, len) == (ssize_t)len;
	return !close(fd) && ok;
}

bool write_line(const char *path, const char *s)
{
	struct buf b = { 0 };
	bool ok;

	buf_puts(&b, s);
	buf_puts(&b, "\n");
	ok = write_text(path, b.p, b.len);
	buf_free(&b);
	return ok;
}

void rm_f(const char *path)
{
	unlink(path);
}

/* rm -rf $1. */
void rm_rf(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *e;

	if (lstat(path, &st))
		return;
	if (!S_ISDIR(st.st_mode)) {
		unlink(path);
		return;
	}
	dir = opendir(path);
	if (dir) {
		while ((e = readdir(dir))) {
			char sub[PATH_MAX];

			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			rm_rf(PATHF(sub, "%s/%s", path, e->d_name));
		}
		closedir(dir);
	}
	rmdir(path);
}

/* mkdir -p $1. */
bool mkdir_p(const char *path)
{
	char p[PATH_MAX];

	snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = '\0';
		if (mkdir(p, 0777) && errno != EEXIST)
			return false;
		*s = '/';
	}
	return !mkdir(p, 0777) || (errno == EEXIST && is_dir(p));
}

/*
 * cp $1 $2: a file that is there keeps its mode, a new one takes $1's, less
 * the umask.
 */
bool copy_file(const char *from, const char *to)
{
	char chunk[4096];
	struct stat st;
	bool ok = true;
	ssize_t n;
	int in, out;

	in = open(from, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return false;
	if (fstat(in, &st)) {
		close(in);
		return false;
	}
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
	if (out < 0) {
		close(in);
		return false;
	}
	while (ok && (n = read(in, chunk, sizeof(chunk))) > 0)
		ok = write(out, chunk, (size_t)n) == n;
	if (n < 0)
		ok = false;
	close(in);
	return !close(out) && ok;
}

/* cp -R $1 $2, $2 not there yet. */
bool copy_tree(const char *from, const char *to)
{
	struct stat st;
	struct dirent *e;
	bool ok = true;
	DIR *dir;

	if (stat(from, &st))
		return false;
	if (!S_ISDIR(st.st_mode))
		return copy_file(from, to);
	if (mkdir(to, st.st_mode & 07777))
		return false;
	dir = opendir(from);
	if (!dir)
		return false;
	while (ok && (e = readdir(dir))) {
		char a[PATH_MAX], b[PATH_MAX];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		ok = copy_tree(PATHF(a, "%s/%s", from, e->d_name),
			       PATHF(b, "%s/%s", to, e->d_name));
	}
	closedir(dir);
	return ok;
}

/* The names in the directory $1, but those that begin with a dot, sorted. */
void dir_names(const char *path, struct words *w)
{
	DIR *dir = opendir(path);
	struct dirent *e;

	if (!dir)
		return;
	while ((e = readdir(dir)))
		if (e->d_name[0] != '.')
			words_add(w, e->d_name);
	closedir(dir);
	words_sort(w, false, false);
}

/*
 * The group $1's number, as $ONU_ROOT/etc/group gives it: the image's, and
 * the tests'.
 */
static bool group_id(const char *name, gid_t *gid)
{
	char path[PATH_MAX], line[512];
	size_t len = strlen(name);
	bool found = false;
	FILE *f;

	f = fopen(PATHF(path, "%s/etc/group", g_root), "re");
	if (!f)
		return false;
	while (!found && fgets(line, sizeof(line), f)) {
		char *gid_s;

		if (strncmp(line, name, len) || line[len] != ':')
			continue;
		gid_s = strchr(line + len + 1, ':');
		if (gid_s && isdigit((unsigned char)gid_s[1])) {
			*gid = (gid_t)strtoul(gid_s + 1, NULL, 10);
			found = true;
		}
	}
	fclose(f);
	return found;
}

/* chown :$2 $1 && chmod $3 $1. */
bool give_group(const char *path, const char *group, mode_t mode)
{
	gid_t gid;

	return group_id(group, &gid) && !chown(path, (uid_t)-1, gid) && !chmod(path, mode);
}

void sleep_tenths(int tenths)
{
	struct timespec ts = { .tv_sec = tenths / 10, .tv_nsec = (tenths % 10) * 100000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

/* The programs it runs. */

/*
 * The program ARGV, found by PATH, with IN on its standard input when there is
 * one and its standard output into OUT when there is one, its trailing
 * newlines off: its exit status, 127 when it cannot be run.
 */
static int run_argv(int flags, const struct buf *in, struct buf *out, char *const argv[])
{
	int pin[2] = { -1, -1 }, pout[2] = { -1, -1 };
	int status = 0;
	pid_t pid;

	fflush(NULL);
	if ((in && pipe2(pin, O_CLOEXEC)) || (out && pipe2(pout, O_CLOEXEC))) {
		log_msg("%s: no pipe: %s", argv[0], strerror(errno));
		return 127;
	}
	pid = fork();
	if (pid < 0) {
		log_msg("%s: not started: %s", argv[0], strerror(errno));
		return 127;
	}
	if (!pid) {
		int null = open("/dev/null", O_RDWR);

		signal(SIGPIPE, SIG_DFL);
		if (in)
			dup2(pin[0], 0);
		if (out)
			dup2(pout[1], 1);
		else if (flags & RUN_QUIET_OUT)
			dup2(null, 1);
		if (flags & RUN_QUIET_ERR)
			dup2(null, 2);
		execvp(argv[0], argv);
		if (!(flags & RUN_QUIET_ERR))
			fprintf(stderr, "onu-apply: %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (in) {
		size_t done = 0;

		close(pin[0]);
		while (done < in->len) {
			ssize_t n = write(pin[1], in->p + done, in->len - done);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			done += (size_t)n;
		}
		close(pin[1]);
	}
	if (out) {
		char chunk[4096];
		ssize_t n;

		close(pout[1]);
		buf_reset(out);
		buf_add(out, "", 0);
		while ((n = read(pout[0], chunk, sizeof(chunk))) != 0) {
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			buf_add(out, chunk, (size_t)n);
		}
		close(pout[0]);
		buf_chomp(out);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return 127;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128 + WTERMSIG(status);
}

#define RUN_ARGS_MAX 40

static int run_va(int flags, const struct buf *in, struct buf *out, const char *prog, va_list ap)
{
	char *argv[RUN_ARGS_MAX + 1];
	size_t n = 0;
	const char *arg;

	argv[n++] = (char *)prog;
	while ((arg = va_arg(ap, const char *)) && n < RUN_ARGS_MAX)
		argv[n++] = (char *)arg;
	argv[n] = NULL;
	return run_argv(flags, in, out, argv);
}

/* The program PROG with the arguments that follow, to a NULL. */
__attribute((sentinel)) int run(int flags, const char *prog, ...)
{
	va_list ap;
	int rc;

	va_start(ap, prog);
	rc = run_va(flags, NULL, NULL, prog, ap);
	va_end(ap);
	return rc;
}

/* ... with IN on its standard input. */
__attribute((sentinel)) int run_in(int flags, const struct buf *in, const char *prog, ...)
{
	va_list ap;
	int rc;

	va_start(ap, prog);
	rc = run_va(flags, in, NULL, prog, ap);
	va_end(ap);
	return rc;
}

/* ... and its standard output into OUT, $(PROG ...). */
__attribute((sentinel)) int run_out(int flags, struct buf *out, const char *prog, ...)
{
	va_list ap;
	int rc;

	va_start(ap, prog);
	rc = run_va(flags, NULL, out, prog, ap);
	va_end(ap);
	return rc;
}

/* command -v $1: where PATH finds the program, into OUT. */
bool command_v(const char *name, char *out, size_t cap)
{
	const char *path = getenv("PATH");

	while (path && *path) {
		size_t n = strcspn(path, ":");
		char full[PATH_MAX];

		snprintf(full, sizeof(full), "%.*s/%s", (int)n, n ? path : ".", name);
		if (!access(full, X_OK) && !is_dir(full)) {
			snprintf(out, cap, "%s", full);
			return true;
		}
		path += n;
		if (*path)
			path++;
	}
	return false;
}

bool have(const char *name)
{
	char where[PATH_MAX];

	return command_v(name, where, sizeof(where));
}

bool only_of(const char *s, const char *set)
{
	return s[strspn(s, set)] == '\0';
}

void map_set(struct map *m, const char *k, const char *v)
{
	for (size_t i = 0; i < m->k.n; i++)
		if (!strcmp(m->k.v[i], k)) {
			char *copy = strdup(v);

			if (!copy)
				out_of_memory();
			free(m->v.v[i]);
			m->v.v[i] = copy;
			return;
		}
	words_add(&m->k, k);
	words_add(&m->v, v);
}

const char *map_get(const struct map *m, const char *k)
{
	for (size_t i = 0; i < m->k.n; i++)
		if (!strcmp(m->k.v[i], k))
			return m->v.v[i];
	return NULL;
}

void map_free(struct map *m)
{
	words_free(&m->k);
	words_free(&m->v);
}

char *lower(const char *s, char *out)
{
	size_t i = 0;

	for (; s[i] && i < VAL - 1; i++)
		out[i] = (char)tolower((unsigned char)s[i]);
	out[i] = '\0';
	return out;
}

/* Field I of W, nothing past the last, as awk's $N. */
const char *field(const struct words *w, size_t i)
{
	return i < w->n ? w->v[i] : "";
}

/* What follows the first ':' of $1, or $1 without one: ${1#*:}. */
const char *after_colon(const char *s)
{
	const char *c = strchr(s, ':');

	return c ? c + 1 : s;
}

/* The address a.b.c.d as a number, and the number as an address. */
uint32_t ip_num(const char *s)
{
	unsigned int a = 0, b = 0, c = 0, d = 0;

	/* What is missing of a.b.c.d is 0, as the shell's arithmetic takes it. */
	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) < 1)
		return 0;
	return (uint32_t)((a << 24) | (b << 16) | (c << 8) | d);
}

char *num_ip(uint32_t n, char *out)
{
	valf(out, "%u.%u.%u.%u", n >> 24 & 255, n >> 16 & 255, n >> 8 & 255, n & 255);
	return out;
}

/* The mask of a prefix length $1, as a number. */
uint32_t len_mask(long len)
{
	if (len <= 0)
		return 0;
	if (len >= 32)
		return 0xffffffffu;
	return (uint32_t)(0xffffffffu << (32 - len));
}

/* The prefix length of an address a.b.c.d/LEN: 32 without one. */
long addr_len(const char *addr)
{
	const char *slash = strchr(addr, '/');

	return slash ? num_of(slash + 1) : 32;
}

/* The address of a.b.c.d/LEN without its length, as ${1%/x} takes it. */
char *addr_only(const char *addr, char *out)
{
	const char *slash = strrchr(addr, '/');

	valf(out, "%.*s", (int)(slash ? slash - addr : (long)strlen(addr)), addr);
	return out;
}

bool in_list(const int *list, int count, int n)
{
	for (int i = 0; i < count; i++)
		if (list[i] == n)
			return true;
	return false;
}

/* Whether $1 is PREFIX, digits (one at least when NEED), '=' and a character: the number's end. */
const char *keyed(const char *s, const char *prefix, bool need)
{
	size_t len = strlen(prefix), d;

	if (strncmp(s, prefix, len))
		return NULL;
	d = strspn(s + len, "0123456789");
	if ((need && !d) || s[len + d] != '=')
		return NULL;
	return s + len + d;
}
