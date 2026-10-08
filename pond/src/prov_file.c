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
	pv_comma(out);
	pv_key(out, "buffers_kb");
	pv_num(out, prov_field(t, "Buffers", -1));
	pv_comma(out);
	pv_key(out, "cached_kb");
	pv_num(out, prov_field(t, "Cached", -1));
	pv_close(out, '}');
	return 0;
}

/*
 * The first line of a file that may be longer than @cap, such as /proc/stat
 * with the interrupt counts of a large system.
 */
static int first_line(const char *path, char *out, size_t cap)
{
	char rooted[512];
	ssize_t n;
	int fd = open(prov_path(path, rooted, sizeof(rooted)), O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	do
		n = read(fd, out, cap - 1);
	while (n < 0 && errno == EINTR);
	close(fd);
	if (n < 0)
		return -errno;
	out[n] = '\0';
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

/*
 * The processor: the clock ticks it spent in each state since the start, from
 * the first line of /proc/stat, which a reader turns into a share of busy
 * time over the interval between two reads; the load averages and the
 * runnable and existing threads of /proc/loadavg; and its model, from
 * /proc/cpuinfo.
 */
int prov_cpu(struct prov_buf *out)
{
	static const char *const names[] = { "user", "nice", "system", "idle",
					     "iowait", "irq", "softirq", "steal" };
	unsigned long long v[ARRAY_SIZE(names)] = { 0 };
	char t[4096], model[64] = "", *m;
	double load[3] = { 0 };
	unsigned int running = 0, threads = 0, i;
	int err = first_line("/proc/stat", t, sizeof(t));

	if (err)
		return err;
	if (sscanf(t, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1],
		   &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
		return -EPROTO;
	err = prov_read_line("/proc/loadavg", t, sizeof(t));
	if (err)
		return err;
	if (sscanf(t, "%lf %lf %lf %u/%u", &load[0], &load[1], &load[2], &running,
		   &threads) != 5)
		return -EPROTO;
	if (!prov_slurp("/proc/cpuinfo", t, sizeof(t), NULL)) {
		m = strstr(t, "cpu model");
		if (m)
			m = strchr(m, ':');
		if (m) {
			m += 1 + strspn(m + 1, " \t");
			snprintf(model, sizeof(model), "%.*s", (int)strcspn(m, "\n"), m);
		}
	}

	pv_open(out, '{');
	pv_key(out, "ticks");
	pv_open(out, '{');
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (i)
			pv_comma(out);
		pv_key(out, names[i]);
		pv_num(out, (long long)v[i]);
	}
	pv_close(out, '}');
	pv_comma(out);
	pv_key(out, "load");
	pv_open(out, '[');
	for (i = 0; i < 3; i++) {
		if (i)
			pv_comma(out);
		pv_fixed(out, (long long)(load[i] * 100.0 + 0.5), 2);
	}
	pv_close(out, ']');
	pv_comma(out);
	pv_key(out, "running");
	pv_num(out, running);
	pv_comma(out);
	pv_key(out, "threads");
	pv_num(out, threads);
	pv_comma(out);
	pv_key(out, "model");
	pv_str(out, model);
	pv_close(out, '}');
	return 0;
}

/* The name the stick goes by. */
int prov_hostname(struct prov_buf *out)
{
	char t[128];
	int err = prov_read_line("/proc/sys/kernel/hostname", t, sizeof(t));

	if (err)
		return err;
	pv_str(out, t);
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
