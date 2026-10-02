/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Count the log lines a test causes. */

#ifndef OMCI_T_LOG_CAPTURE_H
#define OMCI_T_LOG_CAPTURE_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define OMCI_T_CAPTURE_LOG(out, body)                                      \
	do {                                                               \
		(out)[0] = '\0';                                           \
		fflush(stderr);                                            \
		FILE *cap_f_ = tmpfile();                                  \
		const int cap_save_ = (cap_f_ != NULL) ? dup(2) : -1;      \
		if (cap_save_ >= 0)                                        \
			(void)dup2(fileno(cap_f_), 2);                     \
		body fflush(stderr);                                       \
		if (cap_save_ >= 0) {                                      \
			(void)dup2(cap_save_, 2);                          \
			(void)close(cap_save_);                            \
			rewind(cap_f_);                                    \
			const size_t cap_n_ =                              \
				fread((out), 1, sizeof(out) - 1u, cap_f_); \
			(out)[cap_n_] = '\0';                              \
		}                                                          \
		if (cap_f_ != NULL)                                        \
			(void)fclose(cap_f_);                              \
	} while (0)

static inline unsigned int omci_t_log_count(const char *hay, const char *needle)
{
	unsigned int n = 0;

	for (const char *p = strstr(hay, needle); p != NULL;
	     p = strstr(p + 1, needle))
		n++;
	return n;
}

#endif
