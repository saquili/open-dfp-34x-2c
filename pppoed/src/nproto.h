/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * pppoed as a protocol of netifd, "pppoed" (ADR 0014): the JSON text of what
 * it tells netifd, which the libraries that send it over ubus take, so that
 * it is checked on any host.
 *
 * netifd runs the protocol's handler, /lib/netifd/proto/pppoed.sh, which is
 * pppoed itself, as "pppoed.sh '' dump" for what the protocol takes, and as
 * "pppoed.sh pppoed setup|teardown IFACE CONFIG [DEVICE]" for an interface.
 * At setup it has netifd run pppoed for the session (action 1), so that
 * netifd keeps it and starts it again when it ends; at teardown it has
 * netifd end it (action 2). pppoed tells netifd what the session brought
 * (action 0): its interface, its address unless it is handed on, a default
 * route when asked for and the name servers. netifd puts them in place and
 * holds them, as it takes away from an interface it holds what it did not
 * put there itself.
 */

#ifndef PPPOED_NPROTO_H
#define PPPOED_NPROTO_H

#include <stdbool.h>
#include <stddef.h>

/**
 * struct nproto_link - What a session brought, for netifd.
 * @ifname: the ppp interface
 * @local: the IPv4 address, or NULL without IPv4
 * @peer: the peer's IPv4 address, or NULL
 * @dns: up to two name servers, or NULL
 * @session: the PPPoE session's ID
 * @pass: whether the address is handed on to a host behind the stick
 *        rather than put on @ifname
 * @defaultroute: whether the stick's default route goes through @ifname
 */
struct nproto_link {
	const char *ifname;
	const char *local;
	const char *peer;
	const char *dns[2];
	unsigned int session;
	bool pass;
	bool defaultroute;
};

/* What "dump" prints: the protocol's name and its one setting, the session. */
int nproto_dump_json(char *buf, size_t cap);

/* Action 1: netifd runs @argv (NULL-terminated) as the interface's task. */
int nproto_run_json(char *buf, size_t cap, const char *const argv[]);

/* Action 2: netifd ends the interface's task, as a peer expects. */
int nproto_kill_json(char *buf, size_t cap);

/* Action 0: the link is up with what @l holds. */
int nproto_up_json(char *buf, size_t cap, const struct nproto_link *l);

/*
 * The session of the interface's configuration @config, the JSON text netifd
 * gives the handler: 1 to 7, or -1 without one in that range.
 */
int nproto_session(const char *config);

/* The setting @key of @config, true or false: 1, 0, or -1 without it. */
int nproto_bool(const char *config, const char *key);

/*
 * The setting @key of @config, a string, into @out of @cap bytes: its length,
 * or -1 without it or when it does not fit.
 */
int nproto_string(const char *config, const char *key, char *out, size_t cap);

/* Send @json to netifd's interface @iface (notify_proto): 0 or -errno. */
int nproto_send(const char *iface, const char *json);

#endif
