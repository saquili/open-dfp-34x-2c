// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* bus.h from files, for the tests and a build without libubus. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "prov.h"

int bus_status(const char *object, const char *const *paths, size_t n,
	       char (*out)[BUS_VALUE])
{
	char path[160], text[2048];

	snprintf(path, sizeof(path), "/run/ubus/%s", object);
	for (size_t i = 0; i < n; i++)
		out[i][0] = '\0';
	if (prov_slurp(path, text, sizeof(text), NULL))
		return -ENOENT;
	for (const char *line = text; *line; line += strcspn(line, "\n") + (line[strcspn(line, "\n")] != 0)) {
		size_t len = strcspn(line, "\n"), k;

		for (size_t i = 0; i < n; i++) {
			k = strlen(paths[i]);
			if (len > k && !strncmp(line, paths[i], k) && line[k] == '=')
				snprintf(out[i], BUS_VALUE, "%.*s", (int)(len - k - 1), line + k + 1);
		}
	}
	return 0;
}

int bus_call(const char *object, const char *method,
	     const struct bus_arg *args, size_t nargs, char *out, size_t cap)
{
	char path[160];
	size_t len = 0;
	int err;

	(void)args;
	(void)nargs;
	snprintf(path, sizeof(path), "/run/ubus/%s.%s", object, method);
	err = prov_slurp(path, out, cap, &len);
	if (err)
		return err;
	while (len > 0 && out[len - 1] == '\n')
		out[--len] = '\0';
	return (int)len;
}
