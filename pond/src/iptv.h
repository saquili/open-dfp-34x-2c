/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * A test of a group of television, /api/iptv/test: a POST of the lines
 * GROUP=ADDRESS, an IPv4 multicast group, VLAN=VID, the VLAN a set-top box
 * would join it in, none for untagged, and SECONDS=N, 1 to 30, 10 when not
 * given. omcid joins the group from the stick and counts its bytes (its
 * method "tv_test", mcast/agent.h); pond reads the count every 50 ms, which
 * gives the time of the first bytes, ends the test after the seconds asked
 * and answers {"group","vlan","seconds","bytes","rate_bps","first_ms"},
 * first_ms null when nothing came. One test at a time; the request is
 * answered later (later.h), and the daemon serves the others meanwhile.
 */

#ifndef POND_IPTV_H
#define POND_IPTV_H

#include <stdbool.h>

#include "mongoose.h"

/* Start a test for the request @hm on @c, or refuse it. */
void iptv_test(struct mg_connection *c, struct mg_http_message *hm);

/* Read the count of the test under way, and end it when its time is up. */
void iptv_poll(void);

/* Whether a test runs: the daemon then waits at most 50 ms for a request. */
bool iptv_running(void);

#endif
