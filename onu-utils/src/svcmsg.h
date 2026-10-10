/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The messages of onu-svc to procd's "service" object, as JSON text: the
 * libraries that send them take JSON, and text can be checked on any host.
 */

#ifndef ONU_SVCMSG_H
#define ONU_SVCMSG_H

#include <stdbool.h>
#include <stddef.h>

/**
 * struct svc_respawn - When procd starts a service's program again.
 * @threshold: a run shorter than this many seconds counts as a crash
 * @timeout: the seconds procd waits before it starts the program again
 * @retry: the crashes in a row after which procd gives up, 0 for never
 */
struct svc_respawn {
	unsigned int threshold;
	unsigned int timeout;
	unsigned int retry;
};

/* What onu-svc asks for when it is not told: never give up, 2 s apart. */
#define SVC_RESPAWN_DEFAULT { .threshold = 60, .timeout = 2, .retry = 0 }

/* Whether @name is a service's name: letters, digits, '-' and '_'. */
bool svc_name_ok(const char *name);

/* Read "THRESHOLD,TIMEOUT,RETRY" into @r; -1 when it is not that. */
int svc_parse_respawn(const char *s, struct svc_respawn *r);

/**
 * struct svc_instance - One program of a service.
 * @name: the instance's name, of a service name's letters (svc_name_ok())
 * @respawn: when procd starts it again
 * @log: whether its output goes to the system log
 * @out: a file its output is appended to, or NULL
 * @argv: the program and its arguments, NULL-terminated
 */
struct svc_instance {
	const char *name;
	struct svc_respawn respawn;
	bool log;
	const char *out;
	const char *const *argv;
};

/* The most words an instance's line has, the program and its arguments. */
#define SVC_WORDS_MAX 32

/*
 * Read the instance @name of the line @line, "[-r THRESHOLD,TIMEOUT,RETRY]
 * [-l | -o FILE] -- PROGRAM [ARG...]" as onu-svc start takes them, its words
 * split on blanks, into @inst; @line is cut into its words, which @words, of
 * @cap pointers, points at. 0, or -1 when it is not that.
 */
int svc_parse_instance(char *line, const char *name, struct svc_instance *inst,
		       const char **words, size_t cap);

/*
 * The service @name with the @n instances of @inst, which replace those it
 * has: procd starts a new one, ends one that is not among them, starts one
 * again whose program or values changed, and leaves the rest alone. With
 * none, procd ends every program of the service. As svc_set_json().
 */
int svc_service_json(char *buf, size_t cap, const char *name,
		     const struct svc_instance *inst, size_t n);

/*
 * The service @name with one instance of the same name, which runs @argv
 * (NULL-terminated, at least the program), started again as @r says, with
 * its output in the system log when @log, or appended to the file @out when
 * that is not NULL: procd has the output only to its log, so the program
 * then runs from a shell that sends it there. Written into @buf of @cap
 * bytes; returns the length, or -1 when it does not fit.
 */
int svc_set_json(char *buf, size_t cap, const char *name, const struct svc_respawn *r,
		 bool log, const char *out, const char *const argv[]);

/* {"name": @name}, for "delete" and "list"; as svc_set_json(). */
int svc_name_json(char *buf, size_t cap, const char *name);

#endif
