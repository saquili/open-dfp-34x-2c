// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The value "firmware": the two banks as onu-update tells them, with the
 * SHA-256 and the signature of each once it has read them, which bank runs,
 * which the boot loader starts next, a trial under way or one that failed,
 * the keys whose signatures the stick trusts, and the upload of firmware.c.
 */

#include "bus.h"
#include "firmware.h"
#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct fw_upload fw_upload = { .bank = -1 };

int fw_run(const char *const *args, const char *password, char *out, size_t cap)
{
	static const char *const names[] = { "bank", "password" };

	if (!strcmp(args[0], "status"))
		return bus_act("update_status", NULL, NULL, 0, NULL, out, cap);
	if (!strcmp(args[0], "try") || !strcmp(args[0], "return")) {
		const char *const values[] = { args[1], password };

		return bus_act(!strcmp(args[0], "try") ? "update_try" : "update_return", names,
			       values, 2, NULL, out, cap);
	}
	return -EINVAL;
}

bool fw_field(const char *text, const char *key, char *out, size_t cap)
{
	size_t klen = strlen(key);
	const char *p;

	for (p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			size_t len = strcspn(p + klen + 1, "\n");

			if (len >= cap)
				len = cap - 1;
			memcpy(out, p + klen + 1, len);
			out[len] = '\0';
			return true;
		}
	}
	return false;
}

/* A bank number of the status, 0 or 1, or null. */
static void pv_bank(struct prov_buf *out, const char *text, const char *key)
{
	char v[16];

	if (fw_field(text, key, v, sizeof(v)) && (!strcmp(v, "0") || !strcmp(v, "1")))
		pv_num(out, v[0] - '0');
	else
		pv_null(out);
}

static void pv_field_num(struct prov_buf *out, const char *text, const char *key)
{
	char v[32], *end;
	long long n;

	if (fw_field(text, key, v, sizeof(v))) {
		n = strtoll(v, &end, 10);
		if (*v && !*end) {
			pv_num(out, n);
			return;
		}
	}
	pv_null(out);
}

static void pv_field_str(struct prov_buf *out, const char *text, const char *key)
{
	char v[128];

	if (fw_field(text, key, v, sizeof(v)))
		pv_str(out, v);
	else
		pv_null(out);
}

/* A text of the upload, or null while it is empty. */
static void pv_text(struct prov_buf *out, const char *text)
{
	if (text[0])
		pv_str(out, text);
	else
		pv_null(out);
}

/*
 * The keys of the status's KEY= lines, each "ID KEY NAME": the SHA-256 of the
 * raw public key in hex, the key in base64, and its name.
 */
static void pv_keys(struct prov_buf *out, const char *text)
{
	const char *p;
	bool first = true;

	pv_open(out, '[');
	for (p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		char line[256], *id, *key, *name, *end;
		size_t len;

		if (strncmp(p, "KEY=", 4))
			continue;
		len = strcspn(p + 4, "\n");
		if (len >= sizeof(line))
			continue;
		memcpy(line, p + 4, len);
		line[len] = '\0';
		id = line;
		key = strchr(id, ' ');
		if (!key)
			continue;
		*key++ = '\0';
		name = strchr(key, ' ');
		if (!name)
			continue;
		*name++ = '\0';
		end = name + strlen(name);
		if (strlen(id) != 64 || !*key || name == end)
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "name");
		pv_str(out, name);
		pv_comma(out);
		pv_key(out, "id");
		pv_str(out, id);
		pv_comma(out);
		pv_key(out, "public");
		pv_str(out, key);
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

static const char *const state_name[] = {
	[FW_IDLE] = "idle",
	[FW_RECEIVING] = "receiving",
	[FW_WRITING] = "writing",
	[FW_DONE] = "done",
	[FW_FAILED] = "failed",
};

int prov_firmware(struct prov_buf *out)
{
	static const char *const args[] = { "status", NULL };
	static char text[4096];
	int rc = fw_run(args, NULL, text, sizeof(text)), bank;

	if (rc < 0)
		return rc;
	if (rc)
		return -EIO;

	pv_open(out, '{');
	pv_key(out, "running");
	pv_bank(out, text, "RUNNING");
	pv_comma(out);
	pv_key(out, "committed");
	pv_bank(out, text, "COMMITTED");
	pv_comma(out);
	pv_key(out, "next");
	pv_bank(out, text, "NEXT");
	pv_comma(out);
	pv_key(out, "trial");
	pv_bank(out, text, "TRIAL");
	pv_comma(out);
	pv_key(out, "failed");
	pv_bank(out, text, "FAILED");
	pv_comma(out);
	pv_key(out, "banks");
	pv_open(out, '[');
	for (bank = 0; bank < 2; bank++) {
		char key[32];

		if (bank)
			pv_comma(out);
		pv_open(out, '{');
		pv_key(out, "bank");
		pv_num(out, bank);
		pv_comma(out);
		pv_key(out, "state");
		snprintf(key, sizeof(key), "BANK%d_STATE", bank);
		pv_field_str(out, text, key);
		pv_comma(out);
		pv_key(out, "version");
		snprintf(key, sizeof(key), "BANK%d_VERSION", bank);
		pv_field_str(out, text, key);
		pv_comma(out);
		pv_key(out, "time");
		snprintf(key, sizeof(key), "BANK%d_TIME", bank);
		pv_field_num(out, text, key);
		pv_comma(out);
		pv_key(out, "kernel_size_b");
		snprintf(key, sizeof(key), "BANK%d_KERNEL_SIZE", bank);
		pv_field_num(out, text, key);
		pv_comma(out);
		pv_key(out, "rootfs_size_b");
		snprintf(key, sizeof(key), "BANK%d_ROOTFS_SIZE", bank);
		pv_field_num(out, text, key);
		pv_comma(out);
		pv_key(out, "kernel_used_b");
		snprintf(key, sizeof(key), "BANK%d_KERNEL_USED", bank);
		pv_field_num(out, text, key);
		pv_comma(out);
		pv_key(out, "rootfs_used_b");
		snprintf(key, sizeof(key), "BANK%d_ROOTFS_USED", bank);
		pv_field_num(out, text, key);
		pv_comma(out);
		pv_key(out, "sha256");
		snprintf(key, sizeof(key), "BANK%d_SHA256", bank);
		pv_field_str(out, text, key);
		pv_comma(out);
		pv_key(out, "signature");
		snprintf(key, sizeof(key), "BANK%d_SIGNATURE", bank);
		pv_field_str(out, text, key);
		pv_comma(out);
		pv_key(out, "signer");
		snprintf(key, sizeof(key), "BANK%d_SIGNER", bank);
		pv_field_str(out, text, key);
		pv_close(out, '}');
	}
	pv_close(out, ']');
	pv_comma(out);
	pv_key(out, "keys");
	pv_keys(out, text);
	pv_comma(out);
	pv_key(out, "update");
	pv_open(out, '{');
	pv_key(out, "state");
	pv_str(out, state_name[fw_upload.state]);
	pv_comma(out);
	pv_key(out, "received_b");
	pv_num(out, (long long)fw_upload.received);
	pv_comma(out);
	pv_key(out, "expected_b");
	pv_num(out, (long long)fw_upload.expected);
	pv_comma(out);
	pv_key(out, "bank");
	if (fw_upload.bank >= 0)
		pv_num(out, fw_upload.bank);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "version");
	if (fw_upload.version[0])
		pv_str(out, fw_upload.version);
	else
		pv_null(out);
	pv_comma(out);
	pv_key(out, "message");
	pv_text(out, fw_upload.message);
	pv_comma(out);
	pv_key(out, "sha256");
	pv_text(out, fw_upload.sha256);
	pv_comma(out);
	pv_key(out, "signature");
	pv_text(out, fw_upload.signature);
	pv_comma(out);
	pv_key(out, "signer");
	pv_text(out, fw_upload.signer);
	pv_close(out, '}');
	pv_close(out, '}');
	return 0;
}
