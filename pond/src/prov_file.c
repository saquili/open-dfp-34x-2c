// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The providers read from files: the system, the image, the log, the optics. */

#define _GNU_SOURCE

#include "prov.h"
#include "providers.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int prov_uptime(struct prov_buf *out)
{
	char t[128];
	double up = 0, idle = 0;
	int err = prov_slurp("/proc/uptime", t, sizeof(t), NULL);

	if (err)
		return err;
	if (sscanf(t, "%lf %lf", &up, &idle) != 2)
		return -EPROTO;

	pv_open(out, '{');
	pv_key(out, "up_s");
	pv_fixed(out, (long long)(up * 100.0 + 0.5), 2);
	pv_comma(out);
	pv_key(out, "idle_s");
	pv_fixed(out, (long long)(idle * 100.0 + 0.5), 2);
	pv_close(out, '}');
	return 0;
}

int prov_memory(struct prov_buf *out)
{
	char t[4096];
	int err = prov_slurp("/proc/meminfo", t, sizeof(t), NULL);

	if (err)
		return err;

	pv_open(out, '{');
	pv_key(out, "total_kb");
	pv_num(out, prov_field(t, "MemTotal", -1));
	pv_comma(out);
	pv_key(out, "free_kb");
	pv_num(out, prov_field(t, "MemFree", -1));
	pv_comma(out);
	pv_key(out, "available_kb");
	pv_num(out, prov_field(t, "MemAvailable", -1));
	pv_close(out, '}');
	return 0;
}

int prov_kernel(struct prov_buf *out)
{
	char t[512];
	int err = prov_read_line("/proc/version", t, sizeof(t));

	if (err)
		return err;
	pv_str(out, t);
	return 0;
}

int prov_model(struct prov_buf *out)
{
	char t[128];
	int err = prov_slurp("/proc/device-tree/model", t, sizeof(t), NULL);

	if (err)
		return err;
	pv_str(out, t);
	return 0;
}

static void osrel_field(const char *text, const char *key, char *out,
			size_t cap)
{
	size_t klen = strlen(key);
	const char *p = text;

	out[0] = '\0';
	while (p && *p) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			const char *v = p + klen + 1;
			size_t n = 0;
			bool quoted = (*v == '"');

			if (quoted)
				v++;
			while (v[n] && v[n] != '\n' &&
			       !(quoted && v[n] == '"') && n + 1 < cap)
				n++;
			memcpy(out, v, n);
			out[n] = '\0';
			return;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

int prov_image(struct prov_buf *out)
{
	char t[2048], v[256];
	int err = prov_slurp("/etc/os-release", t, sizeof(t), NULL);

	if (err)
		return err;

	pv_open(out, '{');
	osrel_field(t, "PRETTY_NAME", v, sizeof(v));
	pv_key(out, "name");
	pv_str(out, v);
	pv_comma(out);
	osrel_field(t, "VERSION", v, sizeof(v));
	pv_key(out, "version");
	pv_str(out, v);
	pv_comma(out);
	osrel_field(t, "BUILD_ID", v, sizeof(v));
	pv_key(out, "build");
	pv_str(out, v);
	pv_comma(out);
	osrel_field(t, "VARIANT", v, sizeof(v));
	pv_key(out, "variant");
	pv_str(out, v);
	pv_close(out, '}');
	return 0;
}

#define POND_LOG_LINES 200

int prov_log(struct prov_buf *out)
{
	char rec[2048];
	unsigned int count = 0;
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	lseek(fd, 0, SEEK_DATA);

	pv_open(out, '[');
	for (;;) {
		ssize_t n = read(fd, rec, sizeof(rec) - 1);
		char *text;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0 || count >= POND_LOG_LINES)
			break;
		rec[n] = '\0';

		text = strchr(rec, ';');
		if (!text)
			continue;
		*text++ = '\0';

		if (count)
			pv_comma(out);
		pv_open(out, '{');
		pv_key(out, "prio");
		pv_num(out, strtoll(rec, NULL, 10));
		pv_comma(out);
		pv_key(out, "usec");
		{
			const char *f = rec;
			unsigned int i;

			for (i = 0; i < 2 && f; i++) {
				f = strchr(f, ',');
				if (f)
					f++;
			}
			pv_num(out, f ? strtoll(f, NULL, 10) : -1);
		}
		pv_comma(out);
		pv_key(out, "text");
		{
			char *nl = strchr(text, '\n');

			if (nl)
				*nl = '\0';
			pv_str(out, text);
		}
		pv_close(out, '}');
		count++;

		if (out->overflow)
			break;
	}
	pv_close(out, ']');
	close(fd);
	return 0;
}

int prov_health(struct prov_buf *out)
{
	pv_raw(out, "{\"ok\":true}");
	return 0;
}

static int optics_hwmon_dir(char *out, size_t cap)
{
	static const char *const base = "/sys/class/hwmon";
	char rooted[512];
	struct dirent *de;
	DIR *d = opendir(prov_path(base, rooted, sizeof(rooted)));
	int err = -ENODEV;

	if (!d)
		return -errno;

	while ((de = readdir(d))) {
		char path[640], name[64];

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s/name", base, de->d_name);
		if (prov_read_line(path, name, sizeof(name)))
			continue;
		if (strcmp(name, "rtl9602c_optics"))
			continue;
		snprintf(out, cap, "%s/%s", base, de->d_name);
		err = 0;
		break;
	}

	closedir(d);
	return err;
}

static int optics_value(const char *dir, const char *file, long long *val)
{
	char path[640], t[64];
	int err;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	err = prov_read_line(path, t, sizeof(t));
	if (err)
		return err;
	*val = strtoll(t, NULL, 10);
	return 0;
}

int prov_optics(struct prov_buf *out)
{
	static const struct {
		const char *key;
		const char *file;
		unsigned int decimals;
	} fields[] = {

		{ "temperature_c", "temp1_input", 3 },
		{ "vcc_v", "in0_input", 3 },
		{ "bias_ma", "curr1_input", 0 },
		{ "tx_power_uw", "power1_input", 0 },
		{ "rx_power_uw", "power2_input", 0 },
	};
	char dir[512];
	unsigned int i;
	int err = optics_hwmon_dir(dir, sizeof(dir));

	if (err)
		return err;

	pv_open(out, '{');
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		long long v = 0;

		if (i)
			pv_comma(out);
		pv_key(out, fields[i].key);

		if (optics_value(dir, fields[i].file, &v))
			pv_null(out);
		else if (fields[i].decimals)
			pv_fixed(out, v, fields[i].decimals);
		else
			pv_num(out, v);
	}
	pv_close(out, '}');
	return 0;
}
