// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* bus.h without ubus, for a build without libubus: nothing is published. */

#include "bus.h"

int bus_open(const char *name)
{
	(void)name;
	return 0;
}

int bus_fd(void)
{
	return -1;
}

void bus_handle(void)
{
}

void bus_publish(const struct dhcp6d_state *s)
{
	(void)s;
}

void bus_close(void)
{
}
