// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The settings of the stick's data path: KEY=value lines in /run/onu.conf,
 * which onu-mode applies, and the account of each of the stick's PPPoE
 * sessions, 1 to 7, in /run/onu-pppoeN, as "user secret", readable by root
 * only. What the settings mean, their defaults and the rules that bind them
 * together, is onucfg's (ADR 0014): pond has "onucfg settings" read them in
 * full and "onucfg check" judge a change, and knows none of the rules
 * itself.
 *
 * A read gives the settings, each session with its user, whether a secret is
 * set (never the secret), and what onu-mode and dhcp6d brought up for it
 * (/run/session/N), what it hands down among it, the address handed on, and
 * what became of the settings in the configuration store, which onu-store
 * records in /run/onu-store: "loaded" at boot, "defaults" when the store
 * held none, "saved" or "failed" after a change, "none" without a store. A
 * write takes KEY=value lines, has onucfg judge them over the settings in
 * force, writes what it gives back and the accounts, and starts "onu-mode
 * apply" without waiting for it; until it ends, a write is refused.
 * POND_APPLY and POND_ONUCFG name other programs to start, for the tests.
 */

#define _GNU_SOURCE

#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONF "/run/onu.conf"
#define CREDS "/run/onu-pppoe"
#define SESSION_DIR "/run/session"
#define SESSIONS 7
#define TEXT_MAX 8192
#define VALUE_MAX 64
#define USER_MAX 63
#define SECRET_MAX 255

struct account {
	char user[USER_MAX + 1];
	char secret[SECRET_MAX + 1];
};

/* The "onu-mode apply" that runs, or 0; how the last one ended, or -1. */
static pid_t g_apply;
static int g_apply_status = -1;

static const char *program(const char *env, const char *otherwise)
{
	const char *p = getenv(env);

	return p && *p ? p : otherwise;
}

/* Text after the @len bytes of @buf, which holds @cap; @len counts past it. */
static void append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*len >= cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf + *len, cap - *len, fmt, ap);
	va_end(ap);
	*len += n > 0 ? (size_t)n : 0;
}

/* PPPOEn_USER or PPPOEn_SECRET, and PPPOE_USER of before for the first. */
static bool account_key(const char *name, int *n, bool *user)
{
	const char *rest;

	if (strncmp(name, "PPPOE", 5))
		return false;
	if (name[5] == '_') {
		*n = 0;
		rest = name + 6;
	} else if (name[5] >= '1' && name[5] <= '0' + SESSIONS && name[6] == '_') {
		*n = name[5] - '1';
		rest = name + 7;
	} else {
		return false;
	}
	if (!strcmp(rest, "USER"))
		*user = true;
	else if (!strcmp(rest, "SECRET"))
		*user = false;
	else
		return false;
	return true;
}

static const char *creds_path(int n, char *buf, size_t cap)
{
	snprintf(buf, cap, "%s%d", CREDS, n + 1);
	return buf;
}

/* The account of each session, from its file; an empty user for none. */
static void accounts_load(struct account acct[SESSIONS])
{
	char text[512], path[64];

	memset(acct, 0, sizeof(*acct) * SESSIONS);
	for (int n = 0; n < SESSIONS; n++) {
		struct account *a = &acct[n];

		if (!prov_slurp(creds_path(n, path, sizeof(path)), text,
				sizeof(text), NULL) &&
		    sscanf(text, "%63s %255s", a->user, a->secret) != 2) {
			a->user[0] = '\0';
			a->secret[0] = '\0';
		}
	}
	explicit_bzero(text, sizeof(text));
}

/* The accounts, as onucfg takes them, after the @len bytes of @buf. */
static void accounts_text(const struct account acct[SESSIONS], char *buf,
			  size_t cap, size_t *len)
{
	for (int n = 0; n < SESSIONS; n++)
		if (*acct[n].user)
			append(buf, cap, len, "PPPOE%d_USER=%s\nPPPOE%d_SECRET=%s\n",
			       n + 1, acct[n].user, n + 1, acct[n].secret);
}

/* Whether @line is KEY=VALUE, the key in capitals, digits and '_'. */
static bool setting_line(const char *line)
{
	size_t k = strspn(line, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");

	return k && line[k] == '=';
}

/*
 * Run "onucfg @verb" on @input: 0 when it took the settings, which are in
 * @out, one KEY=VALUE line each; -EINVAL when it refused them, with why in
 * @out; -EIO when it could not run.
 */
static int onucfg(const char *verb, const char *input, char *out, size_t cap)
{
	const char *args[] = { verb, NULL };
	int rc = prov_exec(program("POND_ONUCFG", "onucfg"), args, input, out, cap);

	if (rc < 0 || rc >= 126)
		return -EIO;
	return rc ? -EINVAL : 0;
}

/*
 * The settings in force, in full, as onucfg reads them, with the accounts of
 * @acct: KEY=VALUE lines in @out, and no other.
 */
static int settings_read(const struct account acct[SESSIONS], char *out,
			 size_t cap)
{
	char in[TEXT_MAX], got[TEXT_MAX], *line, *save = NULL;
	size_t len = 0, olen = 0;
	int rc;

	if (prov_slurp(CONF, in, sizeof(in), &len)) {
		len = 0;
		in[0] = '\0';
	}
	if (len && in[len - 1] != '\n')
		append(in, sizeof(in), &len, "\n");
	accounts_text(acct, in, sizeof(in), &len);
	if (len >= sizeof(in)) {
		rc = -E2BIG;
		goto out;
	}
	rc = onucfg("settings", in, got, sizeof(got));
	if (rc)
		goto out;
	/* What it said of a line it left out is no setting. */
	out[0] = '\0';
	for (line = strtok_r(got, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (setting_line(line))
			append(out, cap, &olen, "%s\n", line);
	if (olen >= cap)
		rc = -E2BIG;
out:
	explicit_bzero(in, sizeof(in));
	return rc;
}

/* The first line of what onucfg said, without its name, as the reason. */
static void reason(const char *said, char *err, size_t errcap)
{
	size_t n;

	if (!strncmp(said, "onucfg: ", 8))
		said += 8;
	n = strcspn(said, "\n");
	snprintf(err, errcap, "%.*s", (int)n, said);
}

/* Write @text to @path through a new file renamed over it, with @mode. */
static int write_file(const char *path, const char *text, mode_t mode)
{
	char rooted[512], tmp[520];
	const char *dst = prov_path(path, rooted, sizeof(rooted));
	size_t len = strlen(text);
	int fd, err = 0;

	snprintf(tmp, sizeof(tmp), "%s.new", dst);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return -errno;
	if (fchmod(fd, mode))
		err = -errno;
	else if (write(fd, text, len) != (ssize_t)len)
		err = -EIO;
	if (close(fd) && !err)
		err = -errno;
	if (!err && rename(tmp, dst))
		err = -errno;
	if (err)
		unlink(tmp);
	return err;
}

void prov_settings_reap(void)
{
	int st;
	pid_t r;

	if (g_apply <= 0)
		return;
	r = waitpid(g_apply, &st, WNOHANG);
	if (r == g_apply) {
		g_apply_status = WIFEXITED(st) ? WEXITSTATUS(st) :
						 128 + WTERMSIG(st);
		g_apply = 0;
	} else if (r < 0) {
		g_apply = 0;
	}
}

/* Start "onu-mode apply", with the signals a program starts with. */
static int apply(void)
{
	const char *prog = getenv("POND_APPLY");
	sigset_t none;
	pid_t pid;

	if (!prog || !*prog)
		prog = "/usr/sbin/onu-mode";
	pid = fork();
	if (pid < 0)
		return -errno;
	if (!pid) {
		signal(SIGPIPE, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execl(prog, prog, "apply", (char *)NULL);
		_exit(127);
	}
	g_apply = pid;
	g_apply_status = -1;
	return 0;
}

/* A fact of session @n that onu-mode wrote, as a string or null. */
static void pv_session_fact(struct prov_buf *out, int n, const char *fact)
{
	char path[64], line[128];

	snprintf(path, sizeof(path), "%s/%d/%s", SESSION_DIR, n + 1, fact);
	if (!prov_read_line(path, line, sizeof(line)))
		pv_str(out, line);
	else
		pv_null(out);
}

/* Whether session @n hands @fam down, as onu-mode wrote it. */
static bool hands(int n, const char *fam)
{
	char path[64], line[64];
	size_t len = strlen(fam);

	snprintf(path, sizeof(path), "%s/%d/hands", SESSION_DIR, n + 1);
	if (prov_read_line(path, line, sizeof(line)))
		return false;
	for (const char *p = line; (p = strstr(p, fam)); p += len)
		if ((p == line || p[-1] == ' ') && (!p[len] || p[len] == ' '))
			return true;
	return false;
}

/*
 * Each session with an account: its user, whether a secret is set, its
 * settings of @text (SESSIONn_<KEY> as "key"), and what onu-mode brought up.
 */
static void pv_sessions(struct prov_buf *out, const struct account acct[SESSIONS],
			const char *text)
{
	bool first = true;

	pv_open(out, '[');
	for (int n = 0; n < SESSIONS; n++) {
		char prefix[16];
		size_t plen;

		if (!*acct[n].user)
			continue;
		if (!first)
			pv_comma(out);
		first = false;
		pv_open(out, '{');
		pv_key(out, "id");
		pv_num(out, n + 1);
		pv_comma(out);
		pv_key(out, "user");
		pv_str(out, acct[n].user);
		pv_comma(out);
		pv_key(out, "secret_set");
		pv_bool(out, *acct[n].secret != '\0');
		plen = (size_t)snprintf(prefix, sizeof(prefix), "SESSION%d_", n + 1);
		for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
			const char *eq = strchr(p, '=');
			size_t vlen = strcspn(eq ? eq + 1 : p, "\n");
			char name[32];
			size_t klen;

			if (strncmp(p, prefix, plen) || !eq)
				continue;
			klen = (size_t)(eq - p) - plen;
			if (klen >= sizeof(name))
				continue;
			for (size_t i = 0; i < klen; i++) {
				char c = p[plen + i];

				name[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
			}
			name[klen] = '\0';
			pv_comma(out);
			pv_key(out, name);
			pv_strn(out, eq + 1, vlen);
		}
		pv_comma(out);
		pv_key(out, "hands_on");
		pv_bool(out, hands(n, "ipv4"));
		pv_comma(out);
		pv_key(out, "dev");
		pv_session_fact(out, n, "dev");
		pv_comma(out);
		pv_key(out, "gem_dev");
		pv_session_fact(out, n, "gem");
		pv_comma(out);
		pv_key(out, "ppp");
		pv_session_fact(out, n, "ppp");
		pv_comma(out);
		pv_key(out, "ipv4");
		pv_session_fact(out, n, "ipv4");
		pv_comma(out);
		pv_key(out, "ipv6");
		pv_session_fact(out, n, "ipv6");
		/* What dhcp6d took for the session, and the host it went to. */
		pv_comma(out);
		pv_key(out, "prefix");
		pv_session_fact(out, n, "prefix");
		pv_comma(out);
		pv_key(out, "prefix_host");
		pv_session_fact(out, n, "host");
		/* Where it hands down, and the gateway the router is given there. */
		pv_comma(out);
		pv_key(out, "lan");
		pv_session_fact(out, n, "lan");
		pv_comma(out);
		pv_key(out, "gateway");
		pv_session_fact(out, n, "gateway");
		pv_comma(out);
		pv_key(out, "dns");
		pv_session_fact(out, n, "dns");
		pv_close(out, '}');
	}
	pv_close(out, ']');
}

int prov_settings(struct prov_buf *out)
{
	struct account acct[SESSIONS];
	char text[TEXT_MAX], line[128];
	bool first = true;
	int rc;

	prov_settings_reap();
	accounts_load(acct);
	rc = settings_read(acct, text, sizeof(text));
	if (rc) {
		explicit_bzero(acct, sizeof(acct));
		return rc;
	}

	pv_open(out, '{');
	pv_key(out, "settings");
	pv_open(out, '{');
	for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		const char *eq = strchr(p, '=');
		char name[32];
		size_t klen;

		if (!eq || !strncmp(p, "SESSION", 7))
			continue;
		klen = (size_t)(eq - p);
		if (klen >= sizeof(name))
			continue;
		memcpy(name, p, klen);
		name[klen] = '\0';
		if (!first)
			pv_comma(out);
		first = false;
		pv_key(out, name);
		pv_strn(out, eq + 1, strcspn(eq + 1, "\n"));
	}
	pv_close(out, '}');
	pv_comma(out);

	pv_key(out, "sessions");
	pv_sessions(out, acct, text);
	pv_comma(out);
	explicit_bzero(acct, sizeof(acct));

	pv_key(out, "mode");
	if (!prov_read_line("/run/onu-mode", line, sizeof(line)))
		pv_str(out, line);
	else
		pv_null(out);
	pv_comma(out);

	/* The bench's router mode: its WAN. */
	pv_key(out, "wan");
	if (!prov_read_line("/run/onu-wan", line, sizeof(line))) {
		char gem[32], dev[32], via[32];

		if (sscanf(line, "%31s %31s %31s", gem, dev, via) == 3) {
			pv_open(out, '{');
			pv_key(out, "gem");
			pv_str(out, gem);
			pv_comma(out);
			pv_key(out, "dev");
			pv_str(out, dev);
			pv_comma(out);
			pv_key(out, "out");
			pv_str(out, via);
			pv_close(out, '}');
		} else {
			pv_null(out);
		}
	} else {
		pv_null(out);
	}
	pv_comma(out);

	pv_key(out, "dns");
	if (!prov_read_line("/run/onu-dns", line, sizeof(line)))
		pv_str(out, line);
	else
		pv_null(out);
	pv_comma(out);

	/* The address a session handed to the router, and where. */
	pv_key(out, "pass");
	if (!prov_read_line("/run/onu-pass", line, sizeof(line))) {
		char lan[32];

		pv_open(out, '{');
		pv_key(out, "address");
		pv_str(out, line);
		pv_comma(out);
		pv_key(out, "lan");
		if (!prov_read_line("/run/onu-lan", lan, sizeof(lan)))
			pv_str(out, lan);
		else
			pv_null(out);
		/* The concentrator's address, when it is the router's gateway. */
		pv_comma(out);
		pv_key(out, "peer_gateway");
		if (!prov_read_line("/run/onu-pass-proxy", lan, sizeof(lan)))
			pv_str(out, lan);
		else
			pv_null(out);
		pv_close(out, '}');
	} else {
		pv_null(out);
	}
	pv_comma(out);

	pv_key(out, "store");
	if (!prov_read_line("/run/onu-store", line, sizeof(line)))
		pv_str(out, line);
	else
		pv_null(out);
	pv_comma(out);

	pv_key(out, "applying");
	pv_bool(out, g_apply > 0);
	pv_comma(out);
	pv_key(out, "last_apply_status");
	if (g_apply_status < 0)
		pv_null(out);
	else
		pv_num(out, g_apply_status);
	pv_close(out, '}');
	return 0;
}

int prov_settings_set(const char *val, char *err, size_t errcap)
{
	struct account acct[SESSIONS];
	char text[TEXT_MAX], in[TEXT_MAX], out[TEXT_MAX], file[TEXT_MAX];
	char *line, *save = NULL;
	bool changed[SESSIONS] = { false };
	size_t len, flen = 0;
	int rc = -EINVAL;

	prov_settings_reap();
	if (g_apply > 0) {
		snprintf(err, errcap, "the last change is still being applied");
		return -EBUSY;
	}
	if (strlen(val) >= sizeof(text)) {
		snprintf(err, errcap, "the settings are too long");
		return -EINVAL;
	}
	snprintf(text, sizeof(text), "%s", val);
	accounts_load(acct);

	/* The settings in force, then the change over them, then the accounts. */
	rc = settings_read(acct, in, sizeof(in));
	if (rc) {
		snprintf(err, errcap, "onucfg: the settings in force cannot be read");
		goto out;
	}
	len = strlen(in);
	for (line = strtok_r(text, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		char *eq = strchr(line, '=');
		bool user;
		int n;

		/*
		 * Every line goes to onucfg, which judges an account too; the
		 * accounts as they then are follow, and are the files.
		 */
		if (eq) {
			*eq = '\0';
			if (account_key(line, &n, &user)) {
				struct account *a = &acct[n];
				const char *v = eq + 1;

				changed[n] = true;
				/* An empty user removes the session's account. */
				if (user && !*v)
					memset(a, 0, sizeof(*a));
				else if (strlen(v) <= (user ? USER_MAX : SECRET_MAX))
					memcpy(user ? a->user : a->secret, v, strlen(v) + 1);
			}
			*eq = '=';
		}
		append(in, sizeof(in), &len, "%s\n", line);
	}
	accounts_text(acct, in, sizeof(in), &len);
	if (len >= sizeof(in)) {
		snprintf(err, errcap, "the settings are too long");
		rc = -EINVAL;
		goto out;
	}

	rc = onucfg("check", in, out, sizeof(out));
	if (rc) {
		if (rc == -EINVAL)
			reason(out, err, errcap);
		else
			snprintf(err, errcap, "onucfg: it does not run");
		goto out;
	}

	for (int n = 0; n < SESSIONS; n++) {
		char path[64], rooted[256];

		if (!changed[n])
			continue;
		creds_path(n, path, sizeof(path));
		if (*acct[n].user) {
			snprintf(file, sizeof(file), "%s %s\n", acct[n].user,
				 acct[n].secret);
			rc = write_file(path, file, 0600);
		} else if (unlink(prov_path(path, rooted, sizeof(rooted))) &&
			   errno != ENOENT) {
			rc = -errno;
		} else {
			rc = 0;
		}
		if (rc) {
			snprintf(err, errcap, "%s: %s", path, strerror(-rc));
			goto out;
		}
	}

	append(file, sizeof(file), &flen, "# The settings of the data path, from pond.\n%s", out);
	if (flen >= sizeof(file)) {
		snprintf(err, errcap, "the settings are too long");
		rc = -EINVAL;
		goto out;
	}
	rc = write_file(CONF, file, 0644);
	if (rc) {
		snprintf(err, errcap, "%s: %s", CONF, strerror(-rc));
		goto out;
	}

	rc = apply();
	if (rc)
		snprintf(err, errcap, "onu-mode: %s", strerror(-rc));
out:
	explicit_bzero(acct, sizeof(acct));
	explicit_bzero(text, sizeof(text));
	explicit_bzero(in, sizeof(in));
	explicit_bzero(file, sizeof(file));
	return rc;
}

/*
 * The panel's views of the settings. The panel names no mode: the stick
 * passes the OLT's services to the router in any case, and holds PPPoE
 * sessions itself when it has accounts.
 */

/* A KEY=VALUE line of @text, or NULL; @out holds the value. */
static const char *line_value(const char *text, const char *key, char *out,
			      size_t cap)
{
	size_t n = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, n) && p[n] == '=') {
			const char *v = p + n + 1;
			size_t len = strcspn(v, "\r\n");

			if (len >= cap)
				return NULL;
			memcpy(out, v, len);
			out[len] = '\0';
			return out;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

/*
 * A session of the stick's own: ID, 1 to 7, or none for the first free one;
 * USER and SECRET, its account, an empty USER removing the session (an
 * absent SECRET keeps the one there is); VLAN, MAC, GEM, FAMILY, PASS and
 * HOST_VLAN. onucfg judges them, and takes over the WAN's settings of before,
 * which the first session took when it had none of its own; the bench's
 * router mode is left as it is.
 */
int prov_session_set(const char *val, char *err, size_t errcap)
{
	char id[VALUE_MAX], user[USER_MAX + 2], secret[SECRET_MAX + 2],
		vlan[VALUE_MAX], mac[VALUE_MAX], gem[VALUE_MAX], fam[VALUE_MAX],
		pass[VALUE_MAX], host[VALUE_MAX], text[TEXT_MAX];
	const char *i = line_value(val, "ID", id, sizeof(id));
	const char *u = line_value(val, "USER", user, sizeof(user));
	const char *sec = line_value(val, "SECRET", secret, sizeof(secret));
	const char *v = line_value(val, "VLAN", vlan, sizeof(vlan));
	const char *m = line_value(val, "MAC", mac, sizeof(mac));
	const char *g = line_value(val, "GEM", gem, sizeof(gem));
	const char *f = line_value(val, "FAMILY", fam, sizeof(fam));
	const char *p = line_value(val, "PASS", pass, sizeof(pass));
	const char *h = line_value(val, "HOST_VLAN", host, sizeof(host));
	struct account acct[SESSIONS];
	int n = 0;
	size_t len = 0;
	int rc;

	accounts_load(acct);
	if (i) {
		if (strlen(i) != 1 || *i < '1' || *i > '0' + SESSIONS) {
			snprintf(err, errcap, "ID: 1 to %d", SESSIONS);
			explicit_bzero(acct, sizeof(acct));
			return -EINVAL;
		}
		n = *i - '0';
	} else {
		for (int k = 0; k < SESSIONS && !n; k++)
			if (!*acct[k].user)
				n = k + 1;
		if (!n || !u || !*u) {
			snprintf(err, errcap, n ? "a new session needs USER" :
						  "all %d sessions are taken",
				 SESSIONS);
			explicit_bzero(acct, sizeof(acct));
			return -EINVAL;
		}
	}
	if (!*acct[n - 1].user && (!u || !*u)) {
		snprintf(err, errcap, "session %d has no account", n);
		explicit_bzero(acct, sizeof(acct));
		return -EINVAL;
	}
	explicit_bzero(acct, sizeof(acct));

	if (u && !*u) {
		append(text, sizeof(text), &len, "PPPOE%d_USER=\n", n);
	} else {
		if (u)
			append(text, sizeof(text), &len, "PPPOE%d_USER=%s\n", n, u);
		if (sec && *sec)
			append(text, sizeof(text), &len, "PPPOE%d_SECRET=%s\n", n, sec);
		if (v)
			append(text, sizeof(text), &len, "SESSION%d_VLAN=%s\n", n, v);
		if (m)
			append(text, sizeof(text), &len, "SESSION%d_MAC=%s\n", n, m);
		if (g)
			append(text, sizeof(text), &len, "SESSION%d_GEM=%s\n", n, g);
		if (f)
			append(text, sizeof(text), &len, "SESSION%d_FAMILY=%s\n", n, f);
		if (p)
			append(text, sizeof(text), &len, "SESSION%d_PASS=%s\n", n, p);
		if (h)
			append(text, sizeof(text), &len, "SESSION%d_HOST_VLAN=%s\n", n, h);
	}
	if (!len)
		text[0] = '\0';

	rc = prov_settings_set(text, err, errcap);
	explicit_bzero(secret, sizeof(secret));
	explicit_bzero(text, sizeof(text));
	return rc;
}
