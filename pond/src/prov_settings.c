// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The settings of the stick's mode, bridge, router or passthrough: KEY=value
 * lines in /run/onu.conf, which onu-mode applies, and the PPPoE user and
 * secret in /run/onu-pppoe, as "user secret", readable by root only.
 *
 * A read gives the settings, the PPPoE user and whether a secret is set,
 * never the secret, what onu-mode last brought up and what became of the
 * settings in the configuration store, which onu-store records in
 * /run/onu-store: "loaded" at boot, "defaults" when the store held none,
 * "saved" or "failed" after a change, "none" without a store. A write takes
 * KEY=value lines, checks every value and the settings as a whole, writes
 * both files and starts "onu-mode apply" without waiting for it; until it
 * ends, a write is refused. POND_APPLY names another program to start, for
 * the tests.
 */

#define _GNU_SOURCE

#include "prov.h"
#include "providers.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONF "/run/onu.conf"
#define CREDS "/run/onu-pppoe"
#define TEXT_MAX 4096
#define VALUE_MAX 64
#define USER_MAX 63
#define SECRET_MAX 255

enum kind {
	K_MODE,
	K_PROTO,
	K_GEM,
	K_VLAN,
	K_PREFIX,
	K_PREFIX_OPT,
	K_ADDR,
	K_ADDR_OPT,
	K_ONOFF,
	K_MAC_OPT,
};

/* The keys of /run/onu.conf, as onu-mode reads them, and their defaults. */
enum key {
	MODE,
	WAN_GEM,
	WAN_VLAN,
	WAN_PROTO,
	WAN_ADDRESS,
	WAN_GATEWAY,
	WAN_MAC,
	LAN_ADDRESS,
	LAN_DHCP,
	LAN_DHCP_START,
	LAN_DHCP_END,
	LAN_VLAN,
	PASS_GATEWAY,
	NKEYS
};

static const struct {
	const char *name;
	enum kind kind;
	const char *dflt;
} g_keys[NKEYS] = {
	[MODE] = { "MODE", K_MODE, "bridge" },
	[WAN_GEM] = { "WAN_GEM", K_GEM, "auto" },
	[WAN_VLAN] = { "WAN_VLAN", K_VLAN, "" },
	[WAN_PROTO] = { "WAN_PROTO", K_PROTO, "dhcp" },
	[WAN_ADDRESS] = { "WAN_ADDRESS", K_PREFIX_OPT, "" },
	[WAN_GATEWAY] = { "WAN_GATEWAY", K_ADDR_OPT, "" },
	[WAN_MAC] = { "WAN_MAC", K_MAC_OPT, "" },
	[LAN_ADDRESS] = { "LAN_ADDRESS", K_PREFIX, "192.168.1.1/24" },
	[LAN_DHCP] = { "LAN_DHCP", K_ONOFF, "on" },
	[LAN_DHCP_START] = { "LAN_DHCP_START", K_ADDR, "192.168.1.100" },
	[LAN_DHCP_END] = { "LAN_DHCP_END", K_ADDR, "192.168.1.199" },
	[LAN_VLAN] = { "LAN_VLAN", K_VLAN, "" },
	[PASS_GATEWAY] = { "PASS_GATEWAY", K_ADDR, "169.254.0.1" },
};

struct settings {
	char val[NKEYS][VALUE_MAX];
	char user[USER_MAX + 1];
	char secret[SECRET_MAX + 1];
};

/* The "onu-mode apply" that runs, or 0; how the last one ended, or -1. */
static pid_t g_apply;
static int g_apply_status = -1;

static bool number(const char *s, unsigned long lo, unsigned long hi,
		   unsigned long *out)
{
	size_t n = strlen(s);
	unsigned long v;

	if (!n || n > 5 || strspn(s, "0123456789") != n || (n > 1 && *s == '0'))
		return false;
	v = strtoul(s, NULL, 10);
	if (out)
		*out = v;
	return v >= lo && v <= hi;
}

/* a.b.c.d, in decimal without leading zeros. */
static bool ipv4(const char *s, uint32_t *ip)
{
	uint32_t v = 0;

	for (int part = 0; part < 4; part++) {
		size_t n = strspn(s, "0123456789");
		char num[4];
		unsigned long octet;

		if (!n || n > 3)
			return false;
		memcpy(num, s, n);
		num[n] = '\0';
		if (!number(num, 0, 255, &octet))
			return false;
		v = v << 8 | (uint32_t)octet;
		s += n;
		if (part < 3 && *s++ != '.')
			return false;
	}
	if (ip)
		*ip = v;
	return *s == '\0';
}

/* a.b.c.d/len, with a length from 1 to 32. */
static bool prefix(const char *s, uint32_t *ip, uint32_t *mask)
{
	char addr[VALUE_MAX];
	const char *slash = strchr(s, '/');
	unsigned long len;

	if (!slash || (size_t)(slash - s) >= sizeof(addr))
		return false;
	memcpy(addr, s, (size_t)(slash - s));
	addr[slash - s] = '\0';
	if (!ipv4(addr, ip) || !number(slash + 1, 1, 32, &len))
		return false;
	if (mask)
		*mask = len == 32 ? 0xffffffffu : ~(0xffffffffu >> len);
	return true;
}

/*
 * xx:xx:xx:xx:xx:xx, in hexadecimal, an address one station may send from:
 * neither a group address nor all zeros.
 */
static bool mac(const char *s)
{
	unsigned long first = 0;
	bool zero = true;

	if (strlen(s) != 17)
		return false;
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':')
				return false;
		} else if (!strchr("0123456789abcdefABCDEF", s[i])) {
			return false;
		} else if (s[i] != '0') {
			zero = false;
		}
	}
	first = strtoul(s, NULL, 16);
	return !zero && !(first & 1);
}

static bool valid(enum kind kind, const char *v)
{
	switch (kind) {
	case K_MODE:
		return !strcmp(v, "bridge") || !strcmp(v, "router") ||
		       !strcmp(v, "passthrough");
	case K_PROTO:
		return !strcmp(v, "pppoe") || !strcmp(v, "dhcp") ||
		       !strcmp(v, "static");
	case K_GEM:
		return !strcmp(v, "auto") || number(v, 0, 4095, NULL);
	case K_VLAN:
		return !*v || number(v, 1, 4094, NULL);
	case K_PREFIX:
		return prefix(v, NULL, NULL);
	case K_PREFIX_OPT:
		return !*v || prefix(v, NULL, NULL);
	case K_ADDR:
		return ipv4(v, NULL);
	case K_ADDR_OPT:
		return !*v || ipv4(v, NULL);
	case K_ONOFF:
		return !strcmp(v, "on") || !strcmp(v, "off");
	case K_MAC_OPT:
		return !*v || mac(v);
	}
	return false;
}

/* A user or a secret: printable, without blanks, which separate the two. */
static bool word(const char *v, size_t max)
{
	size_t n = strlen(v);

	if (!n || n > max)
		return false;
	for (size_t i = 0; i < n; i++)
		if (v[i] <= ' ' || v[i] > '~')
			return false;
	return true;
}

static int key_index(const char *name)
{
	for (int k = 0; k < NKEYS; k++)
		if (!strcmp(name, g_keys[k].name))
			return k;
	return -1;
}

/* The settings in force: the defaults, then the files. */
static void settings_load(struct settings *s)
{
	char text[TEXT_MAX], *line, *save = NULL;

	memset(s, 0, sizeof(*s));
	for (int k = 0; k < NKEYS; k++)
		snprintf(s->val[k], VALUE_MAX, "%s", g_keys[k].dflt);

	if (!prov_slurp(CONF, text, sizeof(text), NULL)) {
		for (line = strtok_r(text, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char *eq = strchr(line, '=');
			int k;

			if (!eq)
				continue;
			*eq = '\0';
			k = key_index(line);
			if (k >= 0 && valid(g_keys[k].kind, eq + 1))
				snprintf(s->val[k], VALUE_MAX, "%s", eq + 1);
		}
	}

	if (!prov_slurp(CREDS, text, sizeof(text), NULL) &&
	    sscanf(text, "%63s %255s", s->user, s->secret) != 2) {
		s->user[0] = '\0';
		s->secret[0] = '\0';
	}
	explicit_bzero(text, sizeof(text));
}

/* Whether the settings make sense together; if not, why in @err. */
static bool coherent(const struct settings *s, char *err, size_t errcap)
{
	uint32_t ip, mask, start, end, gw;

	if (!strcmp(s->val[WAN_PROTO], "static")) {
		if (!prefix(s->val[WAN_ADDRESS], &ip, &mask) ||
		    !ipv4(s->val[WAN_GATEWAY], &gw)) {
			snprintf(err, errcap,
				 "a static WAN needs WAN_ADDRESS and WAN_GATEWAY");
			return false;
		}
		if ((gw & mask) != (ip & mask) || gw == ip) {
			snprintf(err, errcap,
				 "WAN_GATEWAY is not another address of WAN_ADDRESS's network");
			return false;
		}
	}
	if (!strcmp(s->val[MODE], "passthrough")) {
		if (strcmp(s->val[WAN_PROTO], "pppoe")) {
			snprintf(err, errcap,
				 "passthrough hands on a PPPoE session's address: WAN_PROTO=pppoe");
			return false;
		}
		ipv4(s->val[PASS_GATEWAY], &gw);
		if (!gw || gw == 0xffffffffu || gw >> 24 == 127 || gw >> 28 == 14) {
			snprintf(err, errcap,
				 "PASS_GATEWAY is not an address a station can have");
			return false;
		}
	}
	if (!strcmp(s->val[WAN_PROTO], "pppoe") && (!*s->user || !*s->secret)) {
		snprintf(err, errcap,
			 "PPPoE needs PPPOE_USER and PPPOE_SECRET");
		return false;
	}

	prefix(s->val[LAN_ADDRESS], &ip, &mask);
	ipv4(s->val[LAN_DHCP_START], &start);
	ipv4(s->val[LAN_DHCP_END], &end);
	if ((start & mask) != (ip & mask) || (end & mask) != (ip & mask) ||
	    start > end) {
		snprintf(err, errcap,
			 "LAN_DHCP_START to LAN_DHCP_END is not a range of LAN_ADDRESS's network");
		return false;
	}
	return true;
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

int prov_settings(struct prov_buf *out)
{
	struct settings s;
	char line[128];

	prov_settings_reap();
	settings_load(&s);

	pv_open(out, '{');
	pv_key(out, "settings");
	pv_open(out, '{');
	for (int k = 0; k < NKEYS; k++) {
		if (k)
			pv_comma(out);
		pv_key(out, g_keys[k].name);
		pv_str(out, s.val[k]);
	}
	pv_close(out, '}');
	pv_comma(out);

	pv_key(out, "pppoe");
	pv_open(out, '{');
	pv_key(out, "user");
	pv_str(out, s.user);
	pv_comma(out);
	pv_key(out, "secret_set");
	pv_bool(out, *s.secret != '\0');
	pv_close(out, '}');
	pv_comma(out);
	explicit_bzero(&s, sizeof(s));

	pv_key(out, "mode");
	if (!prov_read_line("/run/onu-mode", line, sizeof(line)))
		pv_str(out, line);
	else
		pv_null(out);
	pv_comma(out);

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

	/* In passthrough mode, the address handed to the router, and where. */
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
	struct settings s;
	char text[TEXT_MAX], file[TEXT_MAX];
	char *line, *save = NULL;
	bool creds = false;
	size_t len;
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
	settings_load(&s);

	for (line = strtok_r(text, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		char *eq = strchr(line, '=');
		const char *v;
		int k;

		if (!eq) {
			snprintf(err, errcap, "a line without '='");
			goto out;
		}
		*eq = '\0';
		v = eq + 1;

		if (!strcmp(line, "PPPOE_USER") || !strcmp(line, "PPPOE_SECRET")) {
			bool user = line[6] == 'U';
			int max = user ? USER_MAX : SECRET_MAX;

			if (!word(v, max)) {
				snprintf(err, errcap,
					 "%s: printable, without blanks, up to %d characters",
					 line, max);
				goto out;
			}
			memcpy(user ? s.user : s.secret, v, strlen(v) + 1);
			creds = true;
			continue;
		}

		k = key_index(line);
		if (k < 0) {
			snprintf(err, errcap, "%s: no such setting", line);
			goto out;
		}
		if (!valid(g_keys[k].kind, v)) {
			snprintf(err, errcap, "%s: not a value of it", line);
			goto out;
		}
		snprintf(s.val[k], VALUE_MAX, "%s", v);
	}

	if (creds && (!*s.user || !*s.secret)) {
		snprintf(err, errcap, "PPPoE needs PPPOE_USER and PPPOE_SECRET");
		goto out;
	}
	if (!coherent(&s, err, errcap))
		goto out;

	if (creds) {
		snprintf(file, sizeof(file), "%s %s\n", s.user, s.secret);
		rc = write_file(CREDS, file, 0600);
		if (rc) {
			snprintf(err, errcap, "%s: %s", CREDS, strerror(-rc));
			goto out;
		}
	}

	len = (size_t)snprintf(file, sizeof(file),
			       "# The settings of the mode, from pond.\n");
	for (int k = 0; k < NKEYS; k++)
		len += (size_t)snprintf(file + len, sizeof(file) - len, "%s=%s\n",
					g_keys[k].name, s.val[k]);
	rc = write_file(CONF, file, 0644);
	if (rc) {
		snprintf(err, errcap, "%s: %s", CONF, strerror(-rc));
		goto out;
	}

	rc = apply();
	if (rc)
		snprintf(err, errcap, "onu-mode: %s", strerror(-rc));
out:
	explicit_bzero(&s, sizeof(s));
	explicit_bzero(text, sizeof(text));
	explicit_bzero(file, sizeof(file));
	return rc;
}

/*
 * The panel's two views of the settings. The panel names no mode: the
 * stick passes the OLT's services to the router in any case, and holds a
 * PPPoE session itself when it has an account. Until onu-mode works the same
 * way, an account means passthrough mode and none means bridge mode.
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

int prov_pppoe_set(const char *val, char *err, size_t errcap)
{
	char user[USER_MAX + 2], secret[SECRET_MAX + 2], vlan[VALUE_MAX],
		mac[VALUE_MAX], text[TEXT_MAX];
	const char *u = line_value(val, "USER", user, sizeof(user));
	const char *sec = line_value(val, "SECRET", secret, sizeof(secret));
	const char *v = line_value(val, "VLAN", vlan, sizeof(vlan));
	const char *m = line_value(val, "MAC", mac, sizeof(mac));
	size_t len = 0;
	int rc;

	if ((v && !valid(K_VLAN, v)) || (m && !valid(K_MAC_OPT, m))) {
		snprintf(err, errcap, "VLAN: 1 to 4094 or none; MAC: xx:xx:xx:xx:xx:xx or none");
		return -EINVAL;
	}
	if (u && *u && !word(u, USER_MAX)) {
		snprintf(err, errcap, "USER: printable, without blanks, up to %d characters",
			 USER_MAX);
		return -EINVAL;
	}

	if (u && !*u) {
		/* No account: the stick leaves the session to the router. */
		len += (size_t)snprintf(text + len, sizeof(text) - len,
					"MODE=bridge\nWAN_PROTO=dhcp\n");
		char path[256];

		if (unlink(prov_path(CREDS, path, sizeof(path))) && errno != ENOENT) {
			int e = errno;

			snprintf(err, errcap, "%s: %s", CREDS, strerror(e));
			return -e;
		}
	} else if (u) {
		len += (size_t)snprintf(text + len, sizeof(text) - len,
					"MODE=passthrough\nWAN_PROTO=pppoe\nPPPOE_USER=%s\n", u);
		if (sec && *sec)
			len += (size_t)snprintf(text + len, sizeof(text) - len,
						"PPPOE_SECRET=%s\n", sec);
	}
	if (v)
		len += (size_t)snprintf(text + len, sizeof(text) - len, "WAN_VLAN=%s\n", v);
	if (m)
		len += (size_t)snprintf(text + len, sizeof(text) - len, "WAN_MAC=%s\n", m);
	if (!len) {
		snprintf(err, errcap, "nothing to change");
		return -EINVAL;
	}

	rc = prov_settings_set(text, err, errcap);
	explicit_bzero(secret, sizeof(secret));
	explicit_bzero(text, sizeof(text));
	return rc;
}

int prov_link_set(const char *val, char *err, size_t errcap)
{
	char vlan[VALUE_MAX], gw[VALUE_MAX], text[TEXT_MAX];
	const char *v = line_value(val, "VLAN", vlan, sizeof(vlan));
	const char *g = line_value(val, "GATEWAY", gw, sizeof(gw));
	size_t len = 0;

	if (v)
		len += (size_t)snprintf(text + len, sizeof(text) - len, "LAN_VLAN=%s\n", v);
	if (g)
		len += (size_t)snprintf(text + len, sizeof(text) - len, "PASS_GATEWAY=%s\n", g);
	if (!len) {
		snprintf(err, errcap, "nothing to change");
		return -EINVAL;
	}
	return prov_settings_set(text, err, errcap);
}
