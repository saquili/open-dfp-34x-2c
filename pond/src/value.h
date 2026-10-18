/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The values pond serves, written as replies straight into the send buffer
 * of the connection that asked: no buffer of the daemon's own holds a value,
 * and none is copied. The send buffer grows with the reply, twice its size
 * at a time, and goes back once the reply has gone (value_sent()), so that a
 * large value costs memory only while it is sent, and as much as it is.
 *
 * A value is at most VALUE_MAX bytes of JSON. The largest is the settings
 * (prov_settings.c), and of them the devices with a lease: up to 512 in each
 * of the 8 local networks, 108 bytes each as udhcpd writes them, and 208
 * with a name of 20 bytes that each need \u00XX. With the most of every
 * other part as well, each string as long as it may be and escaped as much,
 * the settings come to 990,304 bytes, 580,704 with the names of the leases as
 * udhcpd writes them; the largest the store takes, built in the tests, to
 * 938,744. 96 port forwards and 96 rules of the firewall will add 66,782,
 * and VALUE_MAX leaves a quarter of a megabyte over the sum. A value that
 * would pass it is not served: the request gets an error that says so, and
 * the daemon's log a line.
 */

#ifndef POND_VALUE_H
#define POND_VALUE_H

#include "prov.h"

#include "mongoose.h"

/* The most a value may be, and a reply of several (/api/getm). */
#define VALUE_MAX (1280u * 1024u)
#define VALUE_REPLY_MAX (VALUE_MAX + 256u * 1024u)

/* A send buffer this large or smaller is kept for the next reply. */
#define VALUE_KEEP (32u * 1024u)

/*
 * Answer @c with the value of @p, {"id", "value"}: from the point @since on
 * when @from is not NULL, an error when it cannot be read or is too large.
 */
void value_get(struct mg_connection *c, const struct prov *p,
	       const struct prov_from *from, unsigned long since);

/*
 * Answer @c with the values @ids names, apart by commas, as an object: a
 * slow one, or one that cannot be read or is too large, as an error of its
 * own, an unknown one as null.
 */
void value_getm(struct mg_connection *c, struct mg_str ids);

/* Answer @c with the list of values. */
void value_schema(struct mg_connection *c);

/* A reply of @c has gone: give a large send buffer back. */
void value_sent(struct mg_connection *c);

#endif
