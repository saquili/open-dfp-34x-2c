/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The flash's two banks and the updates of the system, which onu-update of
 * onu-utils carries out. prov_firmware.c holds what the value "firmware"
 * shows, the banks and the update under way; firmware.c the HTTP routes that
 * upload an update file, download one and start the other bank.
 */

#ifndef POND_FIRMWARE_H
#define POND_FIRMWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/* What an upload is doing; FW_DONE and FW_FAILED stay until the next one. */
enum fw_state {
	FW_IDLE,
	FW_RECEIVING,	/* the body streams into onu-update */
	FW_WRITING,	/* the body is in; onu-update checks and reads back */
	FW_DONE,
	FW_FAILED,
};

struct fw_upload {
	enum fw_state state;
	size_t expected;	/* the body's Content-Length */
	size_t received;	/* handed to onu-update so far */
	int bank;		/* the bank written, -1 before onu-update says */
	char version[64];	/* the version written */
	char message[256];	/* onu-update's last message, when it failed */
};

extern struct fw_upload fw_upload;

/*
 * onu-update's status, @args { "status", NULL }, or its try of a bank,
 * { "try", BANK or NULL for the other }, through onu-act (bus.h): its
 * standard output and error, together, in @out, NUL-terminated.
 * Return: its exit status, or a negative errno when it could not run.
 */
int fw_run(const char *const *args, char *out, size_t cap);

/* The value of KEY in KEY=VALUE lines @text, in @out; false when absent. */
bool fw_field(const char *text, const char *key, char *out, size_t cap);

struct mg_connection;
struct mg_http_message;

/*
 * The routes of firmware.c. firmware_upload() takes over a connection whose
 * headers are in, once the request is let through; firmware_try() answers and
 * says whether to restart; firmware_poll() runs at every turn of the loop.
 */
void firmware_upload(struct mg_connection *c, struct mg_http_message *hm);
void firmware_download(struct mg_connection *c);
bool firmware_try(struct mg_connection *c, struct mg_http_message *hm);
void firmware_poll(void);

#endif
