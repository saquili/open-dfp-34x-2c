/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * What dhcp6d has, as it publishes it on ubus (ADR 0014): the prefix
 * delegated to it, the host it delegated the prefix on to, and whether that
 * host is bound and the prefix routed to it. bus.h sends it as blobmsg.
 */

#ifndef DHCP6D_STATE_H
#define DHCP6D_STATE_H

#include <stdbool.h>

struct dhcp6d_state {
	/* the name of the object, dhcp6d.NAME */
	const char *name;
	const char *wan, *lan;
	/* the prefix with its length, and the host's link-local address; NULL for none */
	const char *prefix, *host;
	bool bound, routed;
};

#endif
