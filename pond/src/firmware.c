// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The HTTP routes of the updates:
 *
 *   POST /api/firmware/upload    an update file as the request's body: it
 *                                goes into the bank that does not run
 *   GET  /api/firmware/download  the running bank as an update file
 *   POST /api/firmware/try       start a bank once, the one the body names
 *                                or the other, by a restart
 *
 * An update file is some 3.7 MB and the stick has 25 MB of memory, so the
 * body of an upload is never gathered. Once its headers are in, the
 * connection leaves the HTTP parser, and what arrives goes to the standard
 * input of "onu-update write -" as fast as the flash takes it: while the
 * pipe is full, the connection stops reading and TCP holds the sender back.
 * onu-update checks the header before it writes and the checksum after,
 * and answers; the answer, and what the value "firmware" shows of the
 * upload, come from its exit status and its last message. One upload runs
 * at a time. A download streams the output of "onu-update read" the same
 * way, in chunks, as fast as the browser takes it. onu-act runs onu-update
 * (bus.h): pond passes it one end of the pipe, and holds the other.
 */

#define _GNU_SOURCE

#include "auth.h"
#include "bus.h"
#include "firmware.h"
#include "prov.h"

#include "mongoose.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The most an update file can be: two banks of 3840 KiB and a little. */
#define FW_MAX (4u << 20)
/* What the connection may hold before it stops reading. */
#define FW_HOLD (64u * 1024u)
#define FW_CHUNK (16u * 1024u)

static struct {
	bool running;			/* onu-update write - has not ended */
	int in;				/* the pipe to its standard input */
	struct mg_connection *c;	/* the sender, NULL once it is gone */
} up = { .in = -1 };

static struct {
	bool running;			/* onu-update read has not ended */
	bool ended;			/* it has, and its end waits to be told */
	int status;
	int out;			/* the pipe from its standard output */
	struct mg_connection *c;
} down = { .out = -1 };

/*
 * What onu-update said, @text: its fields, and its complaints, the first,
 * which is the cause, and the last, what became of the bank.
 */
static void read_said(const char *text)
{
	static const char tag[] = "onu-update: ";
	const char *first, *last = NULL, *p;
	char bank[8];

	if (fw_field(text, "BANK", bank, sizeof(bank)))
		fw_upload.bank = atoi(bank);
	fw_field(text, "VERSION", fw_upload.version, sizeof(fw_upload.version));
	first = strstr(text, tag);
	for (p = first; p; p = strstr(p + 1, tag))
		last = p;
	if (!first)
		return;
	first += strlen(tag);
	last += strlen(tag);
	if (first == last)
		snprintf(fw_upload.message, sizeof(fw_upload.message), "%.*s",
			 (int)strcspn(first, "\n"), first);
	else
		snprintf(fw_upload.message, sizeof(fw_upload.message), "%.*s; %.*s",
			 (int)strcspn(first, "\n"), first, (int)strcspn(last, "\n"), last);
}

static void reply(struct mg_connection *c, int code, const char *what,
		  const char *detail)
{
	char body[512];
	struct prov_buf b = { .buf = body, .cap = sizeof(body) };

	pv_open(&b, '{');
	pv_key(&b, code == 200 ? "ok" : "error");
	if (code == 200)
		pv_bool(&b, true);
	else
		pv_str(&b, what);
	pv_comma(&b);
	pv_key(&b, "detail");
	pv_str(&b, detail);
	pv_close(&b, '}');
	mg_http_reply(c, code,
		      "Content-Type: application/json\r\n"
		      "Cache-Control: no-store\r\n"
		      "Connection: close\r\n",
		      "%s", body);
}

/* Hand what the sender gave to onu-update, as much as the pipe takes. */
static void upload_pump(struct mg_connection *c)
{
	while (up.in >= 0 && c->recv.len && fw_upload.received < fw_upload.expected) {
		size_t want = fw_upload.expected - fw_upload.received;
		ssize_t n;

		if (want > c->recv.len)
			want = c->recv.len;
		n = write(up.in, c->recv.buf, want);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		if (n < 0) {
			/* onu-update has stopped reading: it refused the file. */
			close(up.in);
			up.in = -1;
			break;
		}
		mg_iobuf_del(&c->recv, 0, (size_t)n);
		fw_upload.received += (size_t)n;
	}
	if (up.in >= 0 && fw_upload.received == fw_upload.expected) {
		close(up.in);
		up.in = -1;
		fw_upload.state = FW_WRITING;
	}
	/* Bytes past the body, or while onu-update is gone, are dropped. */
	if (up.in < 0)
		mg_iobuf_del(&c->recv, 0, c->recv.len);
	c->is_full = c->recv.len >= FW_HOLD;
}

static void upload_fn(struct mg_connection *c, int ev, void *ev_data)
{
	(void)ev_data;
	if (ev == MG_EV_READ || ev == MG_EV_POLL) {
		if (c == up.c)
			upload_pump(c);
	} else if (ev == MG_EV_CLOSE && c == up.c) {
		up.c = NULL;
		if (up.in >= 0) {
			/* The sender went before the end: onu-update sees it end early. */
			close(up.in);
			up.in = -1;
		}
	}
}

static void upload_ended(void *ctx, int status, const char *said);

/*
 * Root's password, which writing an update and starting the other bank
 * need as well as a session (onu-act wants it too): the header
 * X-Pond-Password, percent-encoded, checked again into @pw. True when it
 * is root's; otherwise the refusal has been sent.
 */
static bool confirmed(struct mg_connection *c, struct mg_http_message *hm,
		      char *pw, size_t cap)
{
	struct mg_str *h = mg_http_get_header(hm, "X-Pond-Password");
	char from[48];
	int retry = 0, rc;

	pw[0] = '\0';
	if (h && mg_url_decode(h->buf, h->len, pw, cap, 0) < 0)
		pw[0] = '\0';
	mg_snprintf(from, sizeof(from), "%M", mg_print_ip, &c->rem);
	rc = auth_confirm(pw, from, time(NULL), &retry);
	if (!rc)
		return true;
	explicit_bzero(pw, cap);
	if (rc == -EAGAIN)
		reply(c, 429, "too many attempts", "wait half a minute and try again");
	else if (rc == -ENOKEY)
		reply(c, 423, "no store", "the settings store cannot be read");
	else
		reply(c, 403, "wrong password", "root's password is needed for this");
	return false;
}

void firmware_upload(struct mg_connection *c, struct mg_http_message *hm)
{
	struct mg_str *cl = mg_http_get_header(hm, "Content-Length");
	unsigned long long len = 0;
	char num[24], pw[AUTH_SECRET_MAX + 1];
	int fds[2], rc;

	if (up.running || down.running) {
		reply(c, 409, "busy", "an update or a download is under way");
		c->is_draining = 1;
		return;
	}
	if (cl && cl->len < sizeof(num)) {
		memcpy(num, cl->buf, cl->len);
		num[cl->len] = '\0';
		len = strtoull(num, NULL, 10);
	}
	if (!len || len > FW_MAX) {
		reply(c, 413, "not an update file",
		      "the body has to be an update file of at most 4 MiB, with a Content-Length");
		c->is_draining = 1;
		return;
	}
	if (!confirmed(c, hm, pw, sizeof(pw))) {
		c->is_draining = 1;
		return;
	}

	memset(&fw_upload, 0, sizeof(fw_upload));
	fw_upload.bank = -1;
	fw_upload.expected = (size_t)len;
	rc = pipe2(fds, O_CLOEXEC) ? -errno : 0;
	if (!rc) {
		/* The reading end goes to onu-act, whatever comes of it. */
		rc = bus_act_start("update_write", fds[0], pw, upload_ended, NULL);
		if (rc)
			close(fds[1]);
	}
	explicit_bzero(pw, sizeof(pw));
	if (rc) {
		fw_upload.state = FW_FAILED;
		snprintf(fw_upload.message, sizeof(fw_upload.message),
			 "onu-update did not start: %s", strerror(-rc));
		reply(c, 500, "not started", fw_upload.message);
		c->is_draining = 1;
		return;
	}
	up.in = fds[1];
	fcntl(up.in, F_SETFL, O_NONBLOCK);
	up.running = true;
	fw_upload.state = FW_RECEIVING;
	up.c = c;
	/* The headers out of the buffer: mongoose then lets the body be ours. */
	mg_iobuf_del(&c->recv, 0, hm->head.len);
	c->fn = upload_fn;
	upload_pump(c);
}

/* Send the sender its answer, when onu-update has finished. */
static void upload_ended(void *ctx, int status, const char *said)
{
	(void)ctx;
	up.running = false;
	if (up.in >= 0) {
		close(up.in);
		up.in = -1;
	}
	read_said(said);
	fw_upload.state = status ? FW_FAILED : FW_DONE;
	if (status == -EACCES)
		snprintf(fw_upload.message, sizeof(fw_upload.message),
			 "root's password was refused");
	else if (status && !fw_upload.message[0])
		snprintf(fw_upload.message, sizeof(fw_upload.message),
			 "onu-update ended with status %d", status);
	if (!up.c)
		return;
	if (status)
		reply(up.c, 400, "not written", fw_upload.message);
	else
		reply(up.c, 200, "written", fw_upload.version);
	up.c->is_draining = 1;
	up.c = NULL;
}

static void download_fn(struct mg_connection *c, int ev, void *ev_data)
{
	static char buf[FW_CHUNK];

	(void)ev_data;
	if (c != down.c)
		return;
	if ((ev == MG_EV_POLL || ev == MG_EV_WRITE) && down.out >= 0) {
		while (c->send.len < FW_HOLD) {
			ssize_t n = read(down.out, buf, sizeof(buf));

			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0)
				break;
			if (!n) {
				close(down.out);
				down.out = -1;
				break;
			}
			mg_http_write_chunk(c, buf, (size_t)n);
		}
	} else if (ev == MG_EV_CLOSE) {
		down.c = NULL;
		/* onu-update read ends at its next write. */
		if (down.out >= 0) {
			close(down.out);
			down.out = -1;
		}
	}
}

/* The end of onu-update read, told once its output has all been sent. */
static void download_ended(void *ctx, int status, const char *said)
{
	(void)ctx;
	(void)said;
	down.running = false;
	down.ended = true;
	down.status = status;
}

void firmware_download(struct mg_connection *c)
{
	static const char *const status_args[] = { "status", NULL };
	char text[2048], running[8], key[32], version[64] = "";
	int fds[2], rc;

	if (up.running || down.running) {
		reply(c, 409, "busy", "an update or a download is under way");
		return;
	}
	if (fw_run(status_args, NULL, text, sizeof(text)) == 0 &&
	    fw_field(text, "RUNNING", running, sizeof(running))) {
		snprintf(key, sizeof(key), "BANK%s_VERSION", running);
		fw_field(text, key, version, sizeof(version));
	}
	if (!version[0] || strspn(version, "0123456789abcdefghijklmnopqrstuvwxyz-._") !=
				   strlen(version))
		snprintf(version, sizeof(version), "%s", "system");

	rc = pipe2(fds, O_CLOEXEC) ? -errno : 0;
	if (!rc) {
		/* The writing end goes to onu-act, whatever comes of it. */
		rc = bus_act_start("update_read", fds[1], NULL, download_ended, NULL);
		if (rc)
			close(fds[0]);
	}
	if (rc) {
		reply(c, 500, "not started", strerror(-rc));
		return;
	}
	down.out = fds[0];
	fcntl(down.out, F_SETFL, O_NONBLOCK);
	down.running = true;
	down.ended = false;
	down.c = c;
	c->fn = download_fn;
	mg_printf(c,
		  "HTTP/1.1 200 OK\r\n"
		  "Content-Type: application/octet-stream\r\n"
		  "Content-Disposition: attachment; filename=\"buchos-gpon-%s.bin\"\r\n"
		  "Cache-Control: no-store\r\n"
		  "X-Content-Type-Options: nosniff\r\n"
		  "Transfer-Encoding: chunked\r\n"
		  "Connection: close\r\n\r\n",
		  version);
}

/* The download's end: the last chunk when onu-update did well, a cut otherwise. */
static void download_done(int status)
{
	if (!down.c)
		return;
	if (down.out >= 0)
		return;
	if (status)
		down.c->is_closing = 1;
	else {
		mg_http_write_chunk(down.c, "", 0);
		down.c->is_draining = 1;
	}
	down.c = NULL;
}

bool firmware_try(struct mg_connection *c, struct mg_http_message *hm)
{
	const char *args[3] = { "try", NULL, NULL };
	char out[512], bank[2] = "", pw[AUTH_SECRET_MAX + 1];
	int rc;

	if (up.running) {
		reply(c, 409, "busy", "an update is under way");
		return false;
	}
	if (hm->body.len == 1 && (hm->body.buf[0] == '0' || hm->body.buf[0] == '1')) {
		bank[0] = hm->body.buf[0];
		bank[1] = '\0';
		args[1] = bank;
	} else if (hm->body.len) {
		reply(c, 400, "not a bank", "the body is 0, 1 or nothing for the other bank");
		return false;
	}
	if (!confirmed(c, hm, pw, sizeof(pw)))
		return false;
	rc = fw_run(args, pw, out, sizeof(out));
	explicit_bzero(pw, sizeof(pw));
	if (rc == -EACCES) {
		reply(c, 403, "wrong password", "root's password is needed for this");
		return false;
	}
	if (rc) {
		char *why = strstr(out, "onu-update: ");

		why = why ? why + strlen("onu-update: ") : out;
		why[strcspn(why, "\n")] = '\0';
		reply(c, 409, "not armed", why);
		return false;
	}
	reply(c, 200, "armed", "the stick restarts into the bank once");
	return true;
}

void firmware_poll(void)
{
	if (down.ended && down.out < 0) {
		down.ended = false;
		download_done(down.status);
	}
}
