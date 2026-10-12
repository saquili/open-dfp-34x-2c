/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * dhcp6d on ubus (ADR 0014): the object dhcp6d.NAME, whose method "status"
 * gives the state last published (state.h). bus-ubus.c serves it, as blobmsg
 * made straight from the state; bus-none.c, for a build without libubus,
 * which the tests make, serves nothing. A change is told to the program of -x
 * (main.c), which is how the stick follows it; ubus carries no event of it.
 */

#ifndef DHCP6D_BUS_H
#define DHCP6D_BUS_H

/* Connect and add the object dhcp6d.@name: 0, or a negative errno. */
int bus_open(const char *name);

/* The socket to wait on for ubusd's messages, or -1 without a connection. */
int bus_fd(void);

/* The messages from ubusd that wait; a lost connection is closed. */
void bus_handle(void);

struct dhcp6d_state;

/* The state @s from now on, which "status" gives. */
void bus_publish(const struct dhcp6d_state *s);

void bus_close(void);

#endif
