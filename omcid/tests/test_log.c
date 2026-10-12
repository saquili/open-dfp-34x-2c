// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the log: its lines, and its cut when the file it is appended to grows past the limit. */

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "sys/log.h"

/* The limit in log.c: 256 KiB. */
#define LOG_LIMIT (256 * 1024)

struct log_file {
	char path[64];
	int saved;
	int fd;
};

/* Point the log at a file appended to, as the shell does for the daemon's. */
static bool log_file_open(struct log_file *lf)
{
	strcpy(lf->path, "/tmp/omcid-test-log.XXXXXX");
	lf->fd = mkstemp(lf->path);
	if (lf->fd < 0)
		return false;
	(void)fcntl(lf->fd, F_SETFL, O_APPEND);
	fflush(stderr);
	lf->saved = dup(STDERR_FILENO);
	return lf->saved >= 0 && dup2(lf->fd, STDERR_FILENO) >= 0;
}

static void log_file_close(struct log_file *lf)
{
	fflush(stderr);
	(void)dup2(lf->saved, STDERR_FILENO);
	(void)close(lf->saved);
	(void)close(lf->fd);
	(void)unlink(lf->path);
}

static off_t log_size(const struct log_file *lf)
{
	struct stat st;

	return fstat(lf->fd, &st) == 0 ? st.st_size : -1;
}

/* Whether the file holds @needle. */
static bool log_holds(const struct log_file *lf, const char *needle)
{
	char buf[4096];
	ssize_t n;

	(void)lseek(lf->fd, 0, SEEK_SET);
	n = read(lf->fd, buf, sizeof(buf) - 1u);
	if (n < 0)
		return false;
	buf[n] = '\0';
	return strstr(buf, needle) != NULL;
}

static void pad(const struct log_file *lf, size_t bytes)
{
	char block[4096];

	memset(block, 'x', sizeof(block));
	while (bytes > 0) {
		const size_t n = bytes < sizeof(block) ? bytes : sizeof(block);

		CHECK_EQ(write(lf->fd, block, n), (ssize_t)n);
		bytes -= n;
	}
}

static void test_a_line_has_its_level_its_place_and_its_text(void)
{
	struct log_file lf;

	CHECK(log_file_open(&lf));
	OMCI_WARN("two %s", "words");
	fflush(stderr);
	CHECK(log_holds(&lf, "[WRN] test_log.c:"));
	CHECK(log_holds(&lf, ": two words\n"));
	log_file_close(&lf);
}

static void test_a_log_under_its_limit_is_left_alone(void)
{
	struct log_file lf;

	CHECK(log_file_open(&lf));
	pad(&lf, LOG_LIMIT - 1024u);
	OMCI_INFO("still under");
	fflush(stderr);
	CHECK(log_size(&lf) > (off_t)(LOG_LIMIT - 1024u));
	log_file_close(&lf);
}

static void test_a_log_past_its_limit_is_cut_and_says_so(void)
{
	struct log_file lf;

	CHECK(log_file_open(&lf));
	pad(&lf, LOG_LIMIT + 1024u);
	OMCI_INFO("one more");
	fflush(stderr);
	CHECK(log_size(&lf) < 512);
	CHECK(log_holds(&lf, "the log passed 262144 bytes and was cut"));
	OMCI_INFO("and on");
	fflush(stderr);
	CHECK(log_holds(&lf, ": and on\n"));
	CHECK(log_size(&lf) < 1024);
	log_file_close(&lf);
}

int main(void)
{
	RUN(test_a_line_has_its_level_its_place_and_its_text);
	RUN(test_a_log_under_its_limit_is_left_alone);
	RUN(test_a_log_past_its_limit_is_cut_and_says_so);

	return OMCI_T_REPORT("log");
}
