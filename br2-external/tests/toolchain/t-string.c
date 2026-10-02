/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The string and memory functions. The copy, move and fill routines are checked
 * at every alignment of source and destination against a loop of single bytes.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "check.h"

#define MAX 300

static unsigned char src[MAX + 16], dst[MAX + 16], ref[MAX + 16];

static void pattern(unsigned char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(i * 13 + 1);
}

int main(void)
{
	size_t n, so, dof;
	size_t k;
	char buf[80];
	char *tok, *save;
	char *copy;
	char line[] = "alpha,beta;;gamma";

	pattern(src, sizeof(src));
	for (so = 0; so < 4; so++) {
		for (dof = 0; dof < 4; dof++) {
			for (n = 0; n <= MAX; n += (n < 40 ? 1 : 17)) {
				memset(dst, 0xee, sizeof(dst));
				memset(ref, 0xee, sizeof(ref));
				memcpy(dst + dof, src + so, n);
				for (k = 0; k < n; k++)
					ref[dof + k] = src[so + k];
				CHECK(memcmp(dst, ref, sizeof(dst)) == 0);

				memset(dst, 0xee, sizeof(dst));
				memset(ref, 0xee, sizeof(ref));
				memset(dst + dof, 0x5a, n);
				for (k = 0; k < n; k++)
					ref[dof + k] = 0x5a;
				CHECK(memcmp(dst, ref, sizeof(dst)) == 0);
			}
		}
	}

	pattern(dst, 64);				/* overlapping moves, both ways */
	memmove(dst + 3, dst, 40);
	pattern(ref, 64);
	for (k = 40; k-- > 0;)
		ref[3 + k] = ref[k];
	CHECK(memcmp(dst, ref, 64) == 0);
	pattern(dst, 64);
	memmove(dst, dst + 5, 40);
	pattern(ref, 64);
	for (k = 0; k < 40; k++)
		ref[k] = ref[5 + k];
	CHECK(memcmp(dst, ref, 64) == 0);

	CHECK(strlen("") == 0 && strlen("abcdef") == 6 && strnlen("abcdef", 3) == 3);
	strcpy(buf, "hello");
	strcat(buf, ", world");
	CHECK(strcmp(buf, "hello, world") == 0);
	strncpy(buf, "abc", 8);
	CHECK(memcmp(buf, "abc\0\0\0\0\0", 8) == 0);
	CHECK(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("same", "same") == 0);
	CHECK(strncmp("abcdef", "abcxyz", 3) == 0 && strncmp("abcdef", "abcxyz", 4) < 0);
	CHECK(strcasecmp("HeLLo", "hello") == 0 && strncasecmp("HELP", "helium", 3) == 0);
	CHECK(memcmp("\x01\x02\xff", "\x01\x02\x00", 3) > 0);
	CHECK(strchr("a/b/c", '/') == strchr("a/b/c", 0x2f) && *strrchr("a/b/c", '/') == '/'
	      && strrchr("a/b/c", '/')[1] == 'c' && strchr("abc", 'z') == NULL);
	CHECK(strstr("find the needle here", "needle") != NULL && strstr("abc", "abd") == NULL);
	CHECK(strspn("aabbcd", "ab") == 4 && strcspn("hello world", " ") == 5);
	CHECK(strpbrk("path/to:file", ":/") != NULL && *strpbrk("path/to:file", ":/") == '/');
	CHECK(memchr("abcdef", 'd', 6) != NULL && memchr("abcdef", 'z', 6) == NULL);

	copy = strdup("duplicate");
	CHECK(copy != NULL && strcmp(copy, "duplicate") == 0);
	free(copy);

	tok = strtok_r(line, ",;", &save);
	CHECK(tok && strcmp(tok, "alpha") == 0);
	tok = strtok_r(NULL, ",;", &save);
	CHECK(tok && strcmp(tok, "beta") == 0);
	tok = strtok_r(NULL, ",;", &save);
	CHECK(tok && strcmp(tok, "gamma") == 0);
	CHECK(strtok_r(NULL, ",;", &save) == NULL);

	CHECK(atoi("  -123") == -123 && strtol("0x7fffffff", NULL, 16) == 2147483647L);
	CHECK(strtoul("4294967295", NULL, 10) == 4294967295UL);
	CHECK(strtoll("-9223372036854775807", NULL, 10) == -9223372036854775807LL);
	CHECK(strtoull("18446744073709551615", NULL, 10) == 18446744073709551615ULL);
	CHECK(strtol("077", NULL, 0) == 63 && strtol("12abc", &save, 10) == 12 && *save == 'a');
	DONE("string");
}
