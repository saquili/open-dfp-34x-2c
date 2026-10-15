// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-svc: the stick's programs as procd's services, which procd starts
 * again when they end.
 *
 *   onu-svc start NAME [-r THRESHOLD,TIMEOUT,RETRY] [-l | -o FILE] [-u USER] -- PROGRAM [ARG...]
 *                       the service NAME runs PROGRAM: procd starts it, and
 *                       again TIMEOUT seconds after it ends; a run shorter
 *                       than THRESHOLD seconds is a crash, and after RETRY
 *                       crashes in a row procd gives up (0: never). The
 *                       default is 60,2,0. -l puts the program's output in
 *                       the system log, -o appends it to FILE; -u runs it
 *                       as the account USER, with its groups. A service
 *                       that runs already with another program or other
 *                       values is started again with these; with the same,
 *                       nothing happens.
 *   onu-svc sync NAME DIR
 *                       the service NAME runs one program for each file of
 *                       DIR, the file's name the instance's and its line
 *                       what start takes after NAME, its words split on
 *                       blanks: procd starts what is new, ends what has no
 *                       file any more, starts again what changed, and leaves
 *                       the rest alone; DIR without files ends them all.
 *                       After procd took it, sync reads DIR again, and sends
 *                       again while DIR holds something else, so that of two
 *                       at once the last has procd hold what DIR holds. A file
 *                       whose name begins with '.' is none of them, so that
 *                       one is written under such a name and renamed into
 *                       place.
 *   onu-svc stop NAME   procd ends the program and forgets the service
 *   onu-svc list [NAME] a line for each service: its name, "running" and
 *                       the program's process, or "stopped" and its exit
 *                       status, and how often procd started it again
 *
 * The services live in procd, as long as procd runs; there is no file of
 * them. procd is asked over ubus, on ubus's own socket.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "svcmsg.h"

#define TIMEOUT_MS 3000

static int help(void)
{
	fputs("usage: onu-svc start NAME [-r THRESHOLD,TIMEOUT,RETRY] [-l | -o FILE] [-u USER] --\n"
	      "                       PROGRAM [ARG...]\n"
	      "       onu-svc sync NAME DIR\n"
	      "       onu-svc stop NAME\n"
	      "       onu-svc list [NAME]\n", stderr);
	return 2;
}

enum { INST_RUNNING, INST_PID, INST_EXIT, INST_RESPAWNS, __INST_MAX };

static const struct blobmsg_policy inst_policy[__INST_MAX] = {
	[INST_RUNNING] = { "running", BLOBMSG_TYPE_BOOL },
	[INST_PID] = { "pid", BLOBMSG_TYPE_INT32 },
	[INST_EXIT] = { "exit_code", BLOBMSG_TYPE_INT32 },
	[INST_RESPAWNS] = { "respawns", BLOBMSG_TYPE_INT32 },
};

static const struct blobmsg_policy instances_policy = { "instances", BLOBMSG_TYPE_TABLE };

/* A line for each instance of each service of procd's answer to "list". */
static void print_list(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *svc, *inst, *instances, *tb[__INST_MAX];
	size_t rem, rem2;

	blobmsg_for_each_attr(svc, msg, rem) {
		blobmsg_parse(&instances_policy, 1, &instances, blobmsg_data(svc),
			      blobmsg_data_len(svc));
		if (!instances) {
			printf("%s stopped\n", blobmsg_name(svc));
			continue;
		}
		blobmsg_for_each_attr(inst, instances, rem2) {
			blobmsg_parse(inst_policy, __INST_MAX, tb, blobmsg_data(inst),
				      blobmsg_data_len(inst));
			printf("%s", blobmsg_name(svc));
			if (strcmp(blobmsg_name(inst), blobmsg_name(svc)))
				printf("/%s", blobmsg_name(inst));
			if (tb[INST_RUNNING] && blobmsg_get_bool(tb[INST_RUNNING]))
				printf(" running %u", tb[INST_PID] ? blobmsg_get_u32(tb[INST_PID]) : 0);
			else
				printf(" stopped %u", tb[INST_EXIT] ? blobmsg_get_u32(tb[INST_EXIT]) : 0);
			if (tb[INST_RESPAWNS])
				printf(" respawns %u", blobmsg_get_u32(tb[INST_RESPAWNS]));
			printf("\n");
		}
	}
}

/* The writer of svcmsg.h that builds blobmsg into a buffer of libubox. */
struct blob_writer {
	struct blob_buf *b;
	void *open[4];
	int depth;
};

static int bw_open(void *ctx, const char *name, bool array)
{
	struct blob_writer *w = ctx;
	void *c;

	if (w->depth == (int)ARRAY_SIZE(w->open))
		return -1;
	c = array ? blobmsg_open_array(w->b, name) : blobmsg_open_table(w->b, name);
	if (!c)
		return -1;
	w->open[w->depth++] = c;
	return 0;
}

static int bw_close(void *ctx)
{
	struct blob_writer *w = ctx;

	if (!w->depth)
		return -1;
	blobmsg_close_table(w->b, w->open[--w->depth]);
	return 0;
}

static int bw_string(void *ctx, const char *name, const char *value)
{
	struct blob_writer *w = ctx;

	return blobmsg_add_string(w->b, name, value) ? -1 : 0;
}

static int bw_boolean(void *ctx, const char *name, bool value)
{
	struct blob_writer *w = ctx;

	return blobmsg_add_u8(w->b, name, value) ? -1 : 0;
}

/* A writer into @b, which it begins anew. */
static struct svc_writer writer(struct blob_buf *b, struct blob_writer *bw)
{
	blob_buf_init(b, 0);
	*bw = (struct blob_writer){ .b = b };
	return (struct svc_writer){
		.open = bw_open, .close = bw_close, .string = bw_string,
		.boolean = bw_boolean, .ctx = bw,
	};
}

/* Call @method of procd's "service" with the message @msg. */
static int call(const char *method, struct blob_attr *msg, ubus_data_handler_t cb)
{
	struct ubus_context *ctx;
	uint32_t id;
	int ret;

	ctx = ubus_connect(NULL);
	if (!ctx) {
		fprintf(stderr, "onu-svc: no ubus\n");
		return 1;
	}
	ret = ubus_lookup_id(ctx, "service", &id);
	if (!ret)
		ret = ubus_invoke(ctx, id, method, msg, cb, NULL, TIMEOUT_MS);
	if (ret)
		fprintf(stderr, "onu-svc: procd's %s: %s\n", method, ubus_strerror(ret));
	ubus_free(ctx);
	return ret ? 1 : 0;
}

static int start(int argc, char **argv)
{
	struct svc_respawn r = SVC_RESPAWN_DEFAULT;
	const char *name = argv[1];
	const char *out = NULL, *user = NULL;
	struct blob_buf b = { 0 };
	struct blob_writer bw;
	struct svc_writer w;
	bool log = false;
	int ch, ret;

	if (argc < 2 || !svc_name_ok(name))
		return help();
	optind = 2;
	while ((ch = getopt(argc, argv, "+r:lo:u:")) != -1) {
		switch (ch) {
		case 'r':
			if (svc_parse_respawn(optarg, &r))
				return help();
			break;
		case 'l':
			log = true;
			break;
		case 'o':
			out = optarg;
			break;
		case 'u':
			if (!svc_name_ok(optarg))
				return help();
			user = optarg;
			break;
		default:
			return help();
		}
	}
	/* The program follows "--", which getopt has taken. */
	if (optind >= argc || strcmp(argv[optind - 1], "--") || (log && out))
		return help();
	w = writer(&b, &bw);
	if (svc_set_msg(&w, name, &r, log, out, user, (const char *const *)(argv + optind))) {
		fprintf(stderr, "onu-svc: no room for the message of %s\n", name);
		ret = 1;
	} else {
		ret = call("set", b.head, NULL);
	}
	blob_buf_free(&b);
	return ret;
}

#define INSTANCES_MAX 32
#define LINE_MAX_ 1024
#define NAME_MAX_ 64

static int by_name(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/*
 * The service @name of the instances of the files of @dir, as a message in
 * @b, which it begins anew: 0, or 1 with what is wrong said.
 */
static int dir_msg(const char *name, const char *dir, struct blob_buf *b)
{
	static char names[INSTANCES_MAX][NAME_MAX_];
	static char lines[INSTANCES_MAX][LINE_MAX_];
	static const char *words[INSTANCES_MAX][SVC_WORDS_MAX];
	struct svc_instance inst[INSTANCES_MAX];
	struct blob_writer bw;
	struct svc_writer w = writer(b, &bw);
	struct dirent *e;
	size_t n = 0;
	int fd, ret = 1;
	DIR *d;

	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	d = fd < 0 ? NULL : fdopendir(fd);
	if (!d) {
		fprintf(stderr, "onu-svc: %s: %s\n", dir, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (!svc_name_ok(e->d_name) || strlen(e->d_name) >= NAME_MAX_) {
			fprintf(stderr, "onu-svc: %s/%s: not an instance's name\n", dir, e->d_name);
			goto out;
		}
		if (n == INSTANCES_MAX) {
			fprintf(stderr, "onu-svc: %s: more than %d instances\n", dir, INSTANCES_MAX);
			goto out;
		}
		memcpy(names[n++], e->d_name, strlen(e->d_name) + 1);
	}
	qsort(names, n, sizeof(names[0]), by_name);
	for (size_t i = 0; i < n; i++) {
		int f = openat(dirfd(d), names[i], O_RDONLY | O_CLOEXEC);
		ssize_t got = f < 0 ? -1 : read(f, lines[i], sizeof(lines[i]) - 1);

		if (f >= 0)
			close(f);
		/* One removed meanwhile is no longer one of them. */
		if (got < 0 && errno == ENOENT) {
			memmove(names[i], names[i + 1], (n - i - 1) * sizeof(names[0]));
			n--;
			i--;
			continue;
		}
		if (got < 0 || (size_t)got == sizeof(lines[i]) - 1) {
			fprintf(stderr, "onu-svc: %s/%s: %s\n", dir, names[i],
				got < 0 ? strerror(errno) : "too long");
			goto out;
		}
		lines[i][got] = '\0';
		lines[i][strcspn(lines[i], "\n")] = '\0';
		if (svc_parse_instance(lines[i], names[i], &inst[i], words[i], SVC_WORDS_MAX)) {
			fprintf(stderr, "onu-svc: %s/%s: not [-r T,TO,R] [-l | -o FILE] -- PROGRAM...\n",
				dir, names[i]);
			goto out;
		}
	}
	if (svc_service_msg(&w, name, inst, n)) {
		fprintf(stderr, "onu-svc: no room for the instances of %s\n", name);
		goto out;
	}
	ret = 0;
out:
	closedir(d);
	return ret;
}

/*
 * The service @name of the files of @dir. Two syncs may run at once, and the
 * stick's kernel has no file locks: each reads @dir again after procd took
 * what it sent, and sends again while @dir holds something else, so that the
 * last to send has procd hold what @dir holds.
 */
static int sync_dir(const char *name, const char *dir)
{
	struct blob_buf bufs[2] = { 0 }, *now = &bufs[0], *sent = NULL, *t;
	int ret = 1;

	if (!svc_name_ok(name))
		return help();
	for (int round = 0; round < 8; round++) {
		if (dir_msg(name, dir, now))
			goto out;
		if (sent && blob_attr_equal(now->head, sent->head)) {
			ret = 0;
			goto out;
		}
		if (call("set", now->head, NULL))
			goto out;
		t = sent ? sent : &bufs[1];
		sent = now;
		now = t;
	}
	fprintf(stderr, "onu-svc: %s kept changing\n", dir);
out:
	blob_buf_free(&bufs[0]);
	blob_buf_free(&bufs[1]);
	return ret;
}

/* @method of procd's "service" for the service @name, or for all without. */
static int by_service(const char *method, const char *name, ubus_data_handler_t cb)
{
	struct blob_buf b = { 0 };
	struct blob_writer bw;
	struct svc_writer w = writer(&b, &bw);
	int ret = 1;

	if (!name || !svc_name_msg(&w, name))
		ret = call(method, b.head, cb);
	blob_buf_free(&b);
	return ret;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return help();
	if (!strcmp(argv[1], "start"))
		return start(argc - 1, argv + 1);
	if (!strcmp(argv[1], "sync")) {
		if (argc != 4)
			return help();
		return sync_dir(argv[2], argv[3]);
	}
	if (!strcmp(argv[1], "stop")) {
		if (argc != 3 || !svc_name_ok(argv[2]))
			return help();
		return by_service("delete", argv[2], NULL);
	}
	if (!strcmp(argv[1], "list")) {
		if (argc > 3 || (argc == 3 && !svc_name_ok(argv[2])))
			return help();
		return by_service("list", argc == 3 ? argv[2] : NULL, print_list);
	}
	return help();
}
