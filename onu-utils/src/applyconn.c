// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * applyconn: what the sessions and the IPoE connections share: their GEM
 * ports, their interfaces, the drops that keep the host side's networks from
 * the operator's side on them, and the frames of the other GEM ports handed to
 * them.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "applyconf.h"
#include "applyconn.h"
#include "applyomcid.h"
#include "applyutil.h"

/*
 * The drops on a session's own interface take the priorities 1 to GUARD_PREFS
 * of its ingress (omcid/target/tcprio.h), and no other writer has that
 * interface.
 */
#define GUARD_PREFS 9

/*
 * The address the PON side sends from where a session or the WAN names
 * none: the host link's, made locally administered (or, already so, with its
 * last bit turned). On the PON side the switch looks a frame up by the PON
 * port's PVID, where the host link's own address has its entry, with the
 * router bit the L3 engine needs from the host's side: with one address on
 * both sides, the frames the engine routed down from the PON went nowhere
 * once the PON port dropped the unicast no entry knows (measured), while an
 * address of the PON side's own gets an entry of its own.
 */
char *pon_mac(char *out)
{
	char base[VAL], path[PATH_MAX];
	const char *colon, *last;

	out[0] = '\0';
	cat_val(PATHF(path, "%s/sfp/address", NET), base);
	if (!*base)
		return out;
	colon = strchr(base, ':');
	valf(out, "%02lx:%s", strtoul(base, NULL, 16) | 2, colon ? colon + 1 : base);
	if (strcmp(out, base))
		return out;
	last = strrchr(base, ':');
	valf(out, "%.*s:%02lx", (int)(last ? last - base : (long)strlen(base)), base,
	     strtoul(last ? last + 1 : base, NULL, 16) ^ 1);
	return out;
}

/* The Port-IDs of the GEM ports `pon gem` lists that carry both ways. */
static void gem_ids_both(struct words *ids)
{
	struct words lines = { 0 };
	struct buf out = { 0 };

	run_out(RUN_QUIET_ERR, &out, "pon", "gem", NULL);
	words_lines(&lines, buf_str(&out));
	for (size_t i = 0; i < lines.n; i++) {
		struct words f = { 0 };

		if (strstr(lines.v[i], " multicast"))
			continue;
		words_split(&f, lines.v[i]);
		if (!strcmp(field(&f, 3), "both") && *field(&f, 2))
			words_add(ids, f.v[2]);
		words_free(&f);
	}
	words_free(&lines);
	buf_free(&out);
	words_sort(ids, true, false);
}

/*
 * The GEM interface for the setting $1: gemN for N, or for auto (or nothing)
 * the one of the lowest Port-ID of the GEM ports that carry both ways and are
 * not multicast, as the driver says (pon gem): a session sends and receives,
 * and the multicast port, which OLTs usually give 4095, is neither. The
 * driver's list is asked up to three times, 0.2 s apart: while the OLT
 * provisions the ports, or omcid programs them again, there may be none for a
 * moment, and a session that finds none goes down and stays down until the
 * next change tells onu-apply to look again.
 */
static char *gem_of(const char *g, char *out)
{
	char path[PATH_MAX];

	out[0] = '\0';
	if (*g && strcmp(g, "auto")) {
		if (exists(PATHF(path, "%s/gem%s", NET, g)))
			valf(out, "gem%s", g);
		return out;
	}
	for (int tries = 0; tries < 3; tries++) {
		struct words ids = { 0 };

		gem_ids_both(&ids);
		for (size_t i = 0; i < ids.n; i++)
			if (exists(PATHF(path, "%s/gem%s", NET, ids.v[i]))) {
				valf(out, "gem%s", ids.v[i]);
				words_free(&ids);
				return out;
			}
		words_free(&ids);
		if (tries < 2)
			sleep_tenths(2);
	}
	return out;
}

/*
 * The GEM interface of a connection with the GEM setting $1 in the VLAN $2:
 * the one its GEM setting names; for auto (or nothing), where it has a VLAN,
 * the GEM port that the OLT lets that VLAN on the fibre through, as omcid
 * publishes them in /run/omcid/gem-vlans ("VID PORT-ID" lines), the lowest
 * Port-ID of them; else the one gem_of() gives. A session given the VID by
 * which the operator names its service so follows the service to another VID
 * on the fibre and to another GEM port.
 */
char *conn_gem(const char *g, const char *v, char *out)
{
	if ((!*g || !strcmp(g, "auto")) && *v) {
		struct words lines = { 0 }, ids = { 0 };
		char path[PATH_MAX];
		struct buf b = { 0 };

		cat(PATHF(path, "%s/run/omcid/gem-vlans", g_root), &b);
		words_lines(&lines, buf_str(&b));
		for (size_t i = 0; i < lines.n; i++) {
			struct words f = { 0 };

			words_split(&f, lines.v[i]);
			if (f.n && !strcmp(f.v[0], v) && *field(&f, 1))
				words_add(&ids, f.v[1]);
			words_free(&f);
		}
		words_sort(&ids, true, false);
		for (size_t i = 0; i < ids.n; i++)
			if (exists(PATHF(path, "%s/gem%s", NET, ids.v[i]))) {
				valf(out, "gem%s", ids.v[i]);
				words_free(&ids);
				words_free(&lines);
				buf_free(&b);
				return out;
			}
		words_free(&ids);
		words_free(&lines);
		buf_free(&b);
	}
	return gem_of(g, out);
}

/*
 * The stick's networks of the host side: the management pair, one that waits,
 * the address on the SFP port, and the links that hand a session's address on.
 */
static void host_nets(struct words *out)
{
	char path[PATH_MAX];
	struct buf text = { 0 }, nets = { 0 };
	struct words lines = { 0 };

	cat(PATHF(path, "%s/run/onu-mgmt", g_root), &text);
	words_lines(&lines, buf_str(&text));
	for (size_t i = 0; i < lines.n; i++) {
		const char *s = lines.v[i];

		if (starts(s, "ADDRESS=") && s[8])
			buf_printf(&nets, "%s\n", s + 8);
		else if (starts(s, "DATA_ADDRESS=") && s[13])
			buf_printf(&nets, "%s\n", s + 13);
	}
	words_free(&lines);
	cat(PATHF(path, "%s/run/onu-trial.d/mgmt", g_root), &text);
	words_lines(&lines, buf_str(&text));
	for (size_t i = 0; i < lines.n; i++)
		if (starts(lines.v[i], "ADDRESS=") && lines.v[i][8])
			buf_printf(&nets, "%s\n", lines.v[i] + 8);
	buf_puts(&nets, "169.254.0.0/16\n");
	words_split(out, buf_str(&nets));
	words_free(&lines);
	buf_free(&text);
	buf_free(&nets);
}

/*
 * A packet that comes in on session interface $1 to a network of the host
 * side is dropped before the stick takes it as its own or passes it on to the
 * host, whoever sends it: the operator's end of a session can address one.
 * The filters take priorities 1 to GUARD_PREFS of the interface's ingress.
 */
void guard(const char *dev)
{
	struct words prefs = { 0 }, nets = { 0 };
	struct buf out = { 0 };
	int pref = 1;

	if (run(RUN_QUIET_ERR, "onu-nl", "clsact", "add", dev, NULL)) {
		log_msg("%s: the host side is not kept from the session", dev);
		return;
	}
	run_out(RUN_QUIET_ERR, &out, "onu-nl", "filter", "prefs", dev, NULL);
	words_split(&prefs, buf_str(&out));
	for (size_t i = 0; i < prefs.n; i++) {
		char *end;
		long p = strtol(prefs.v[i], &end, 10);

		if (*end || p <= GUARD_PREFS)
			run(RUN_QUIET_ERR, "onu-nl", "filter", "del", dev, "pref", prefs.v[i],
			    NULL);
	}
	host_nets(&nets);
	for (size_t i = 0; i < nets.n; i++, pref++) {
		char p[16];

		if (pref > GUARD_PREFS) {
			log_msg("%s: more networks than priorities: %s is not kept from the session",
				dev, nets.v[i]);
			break;
		}
		snprintf(p, sizeof(p), "%d", pref);
		if (run(RUN_QUIET_ERR, "onu-nl", "filter", "add", dev, "pref", p, "dst", nets.v[i],
			"drop",
			NULL))
			log_msg("%s: packets to %s are not kept from the session", dev, nets.v[i]);
	}
	words_free(&prefs);
	words_free(&nets);
	buf_free(&out);
}

void feeds_down(void)
{
	char path[PATH_MAX];
	struct words ports = { 0 };
	struct buf b = { 0 };

	cat(PATHF(path, "%s/run/onu-feeds", g_root), &b);
	words_split(&ports, buf_str(&b));
	for (size_t i = 0; i < ports.n; i++)
		run(RUN_QUIET_ERR, "onu-nl", "clsact", "del", ports.v[i], NULL);
	rm_f(path);
	rm_f(PATHF(path, "%s/run/onu-feeds.key", g_root));
	words_free(&ports);
	buf_free(&b);
}

/*
 * The GEM ports other than the sessions'. Without sessions omcid bridges
 * them with the host link; what they carry in a session's VLAN is the
 * session's. The OLT sends the network side's broadcasts by the multicast GEM
 * port, and an answer by that port, which cannot send, is lost. And where the
 * GEM ports of an IEEE 802.1p mapper divide a VLAN's priorities, the OLT sends
 * a session's frames down by more than one of them: an operator's sent the
 * PPPoE discovery and control frames by the GEM port of priorities 6 and 7
 * while the session sent by that of priority 0, and the session never came up
 * (measured). So the frames of every other GEM port are handed to the
 * session's GEM interface as they come in: feeds_sync(TO, VID:GEM...) hands
 * every frame to the GEM interface TO, or, with pairs, only the frames of each
 * VID to its GEM interface, and leaves the rest to the OLT's bridge, or to
 * nobody: the same operator sent a television channel by the multicast GEM
 * port, 7 Mb/s that no one had asked for (measured), which a session has no
 * use for. The frames the switch routes in hardware never reach these rules.
 */
static void feeds_sync(const char *to, const struct words *pairs)
{
	char path[PATH_MAX], keyfile[PATH_MAX], feeds[PATH_MAX];
	struct words lines = { 0 }, ports = { 0 };
	struct buf out = { 0 }, key = { 0 }, have = { 0 };

	run_out(RUN_QUIET_ERR, &out, "pon", "gem", NULL);
	words_lines(&lines, buf_str(&out));
	for (size_t i = 0; i < lines.n; i++) {
		struct words f = { 0 };

		words_split(&f, lines.v[i]);
		if (f.n && strcmp(f.v[0], to))
			words_add(&ports, f.v[0]);
		words_free(&f);
	}
	words_sort(&ports, false, false);
	/*
	 * The interfaces by name and index. When the OLT provisions its GEM ports
	 * again, their interfaces come back under the same names without the
	 * filters the old ones had, and only the index tells them apart; by name
	 * alone the filters stayed away and the session's discovery frames with
	 * them, until the stick restarted (measured on the operator's fibre).
	 */
	for (size_t i = 0; i <= ports.n; i++) {
		const char *dev = i ? ports.v[i - 1] : to;
		char idx[VAL];

		cat_val(PATHF(path, "%s/%s/ifindex", NET, dev), idx);
		buf_printf(&key, "%s:%s ", dev, idx);
	}
	words_join(pairs, " ", &key);
	PATHF(keyfile, "%s/run/onu-feeds.key", g_root);
	cat(keyfile, &have);
	if (!strcmp(buf_str(&key), buf_str(&have)))
		goto out;
	feeds_down();
	PATHF(feeds, "%s/run/onu-feeds", g_root);
	for (size_t i = 0; i < ports.n; i++) {
		const char *port = ports.v[i];
		struct words rules = { 0 };
		char all[VAL];
		int fd;

		if (!pairs->n) {
			snprintf(all, sizeof(all), ":%s", to);
			words_add(&rules, all);
		} else {
			/* A session's own GEM port keeps its frames. */
			for (size_t p = 0; p < pairs->n; p++)
				if (strcmp(after_colon(pairs->v[p]), port))
					words_add(&rules, pairs->v[p]);
			if (!rules.n)
				continue;
		}
		run(RUN_QUIET_ERR, "onu-nl", "clsact", "add", port, NULL);
		fd = open(feeds, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
		if (fd >= 0) {
			if (write(fd, port, strlen(port)) < 0 || write(fd, "\n", 1) < 0)
				log_msg("%s: not noted among the feeds", port);
			close(fd);
		}
		for (size_t r = 0; r < rules.n; r++) {
			const char *rule = rules.v[r], *gem = after_colon(rule);
			size_t vlen = strcspn(rule, ":");
			char vid[VAL];

			if (!vlen) {
				run(0, "onu-nl", "filter", "add", port, "redirect", gem, NULL);
			} else {
				snprintf(vid, sizeof(vid), "%.*s", (int)vlen, rule);
				run(0, "onu-nl", "filter", "add", port, "vlan", vid, "redirect",
				    gem, NULL);
			}
		}
		words_free(&rules);
	}
	write_line(keyfile, buf_str(&key));
out:
	words_free(&lines);
	words_free(&ports);
	buf_free(&out);
	buf_free(&key);
	buf_free(&have);
}

/*
 * Whether a session or an IPoE connection that runs, other than the one of
 * the directory $1, is on the interface $2.
 */
bool dev_shared(const char *dir, const char *dev)
{
	const char *const bases[] = { SESSIONS, IPOE_DIR };
	bool shared = false;

	for (size_t b = 0; b < ARRAY_SIZE(bases) && !shared; b++) {
		struct words names = { 0 };

		dir_names(bases[b], &names);
		for (size_t i = 0; i < names.n && !shared; i++) {
			char path[PATH_MAX], own[PATH_MAX], v[VAL];

			PATHF(path, "%s/%s/dev", bases[b], names.v[i]);
			if (!readable(path) || !strcmp(path, PATHF(own, "%s/dev", dir)))
				continue;
			shared = !strcmp(cat_val(path, v), dev);
		}
		words_free(&names);
	}
	return shared;
}

/*
 * The frames of the other GEM ports handed to the GEM interfaces of the
 * sessions and the IPoE connections (feeds_sync()).
 */
void feeds_all(void)
{
	const char *const bases[] = { SESSIONS, IPOE_DIR };
	struct words names[2] = { { 0 } }, held = { 0 }, pairs = { 0 };
	struct buf all = { 0 }, one = { 0 };
	char first[VAL];

	for (size_t b = 0; b < ARRAY_SIZE(bases); b++) {
		dir_names(bases[b], &names[b]);
		for (size_t i = 0; i < names[b].n; i++) {
			char path[PATH_MAX];
			int fd = open(PATHF(path, "%s/%s/gem", bases[b], names[b].v[i]),
				      O_RDONLY | O_CLOEXEC);
			char chunk[512];
			ssize_t n;

			if (fd < 0)
				continue;
			while ((n = read(fd, chunk, sizeof(chunk))) > 0)
				buf_add(&all, chunk, (size_t)n);
			close(fd);
		}
	}
	snprintf(first, sizeof(first), "%.*s", (int)strcspn(buf_str(&all), "\n"), buf_str(&all));
	if (!*first) {
		feeds_down();
	} else if (held_vlans(&held)) {
		for (size_t b = 0; b < ARRAY_SIZE(bases); b++)
			for (size_t i = 0; i < names[b].n; i++) {
				char path[PATH_MAX], g[VAL], v[VAL], pair[2 * VAL + 2];
				int n = (int)num_of(names[b].v[i]);

				if (!readable(PATHF(path, "%s/%s/gem", bases[b], names[b].v[i])))
					continue;
				read_line(path, g);
				if (b == 0)
					session_vlan(n, v);
				else
					ipoe_vlan(n, v);
				snprintf(pair, sizeof(pair), "%s:%s", v, g);
				buf_printf(&one, "%s ", pair);
			}
		words_split(&pairs, buf_str(&one));
		words_sort(&pairs, false, true);
		feeds_sync(first, &pairs);
	} else {
		feeds_sync(first, &pairs);
	}
	words_free(&names[0]);
	words_free(&names[1]);
	words_free(&held);
	words_free(&pairs);
	buf_free(&all);
	buf_free(&one);
}
