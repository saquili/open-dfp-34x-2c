/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * libomcid: the client side of the daemon's control socket. A process
 * holds one connection at a time. The functions that talk to the daemon
 * return 0 on success, -1 with errno set when the exchange failed, or a
 * positive &enum omci_ctl_status the daemon answered with.
 */

#ifndef OMCID_LIBOMCID_H
#define OMCID_LIBOMCID_H

#include <stddef.h>
#include <stdint.h>

struct omcid_conn;

/**
 * struct omcid_conn_stats - The counters of the daemon's engine.
 * @handled: frames received and handled
 * @rejected: frames dropped as malformed
 * @answered: frames sent
 */
struct omcid_conn_stats {
	uint32_t handled;
	uint32_t rejected;
	uint32_t answered;
};

/**
 * struct omcid_vlan_fate_entry - What became of one VLAN rule.
 * @instance: the instance of class 171 that holds it
 * @row: its place in the rule table
 * @fate: one of the OMCI_CTL_VLAN_FATE_* values
 * @match_cvid: the VID it matches on the subscriber side
 * @net_vid: the VID it gives the frame towards the network
 * @rival_match: for a rule that lost to an earlier one, that rule's match
 * @rival_net: and that rule's network VID
 */
struct omcid_vlan_fate_entry {
	uint16_t instance;
	uint8_t row;
	uint8_t fate;
	uint16_t match_cvid;
	uint16_t net_vid;
	uint16_t rival_match;
	uint16_t rival_net;
};

/**
 * omcid_connect() - Connect to the daemon.
 * @sock: the path of its socket, or NULL for %OMCI_CTL_SOCKET_PATH
 *
 * Reads and writes time out after five seconds.
 *
 * Return: the connection, or NULL with errno set (EBUSY while another is open).
 */
struct omcid_conn *omcid_connect(const char *sock);

/**
 * omcid_disconnect() - Close the connection.
 * @conn: the connection, or NULL
 */
void omcid_disconnect(struct omcid_conn *conn);

/**
 * omcid_me_get() - Read the attribute values of an instance.
 * @conn: the connection
 * @class_id: the class
 * @instance: the instance
 * @out: the values, end to end in attribute order, tables left out
 * @cap: room in @out; a longer answer is cut
 * @n: the bytes written; may be NULL
 *
 * Return: 0, -1, or %OMCI_CTL_ERR_NOT_FOUND.
 */
int omcid_me_get(struct omcid_conn *conn, uint16_t class_id, uint16_t instance,
		 uint8_t *out, size_t cap, size_t *n);

/**
 * omcid_me_list() - List the instances of a class.
 * @conn: the connection
 * @class_id: the class
 * @out: the instances
 * @cap: room in @out
 * @n: the instances written; may be NULL
 *
 * Return: 0, -1, or %OMCI_CTL_ERR_TOO_BIG.
 */
int omcid_me_list(struct omcid_conn *conn, uint16_t class_id, uint16_t *out,
		  size_t cap, size_t *n);

/**
 * omcid_stats() - Read the counters of the engine.
 * @conn: the connection
 * @out: the counters
 *
 * Return: 0 or -1.
 */
int omcid_stats(struct omcid_conn *conn, struct omcid_conn_stats *out);

/**
 * struct omcid_tap_record - A frame the daemon received or sent.
 * @verdict: an &enum omci_ctl_tap_verdict
 * @detail: the message type
 * @dropped: frames lost just before this one, because the client was slow
 * @frame: the frame
 */
struct omcid_tap_record {
	uint8_t verdict;
	uint8_t detail;
	uint16_t dropped;
	uint8_t frame[48];
};

/**
 * omcid_tap_subscribe() - Start following the frames.
 * @conn: the connection; from now on it carries tap records only
 *
 * Return: 0 or -1.
 */
int omcid_tap_subscribe(struct omcid_conn *conn);

/**
 * omcid_tap_next() - Wait for the next frame.
 * @conn: a connection that follows the frames
 * @out: the record
 *
 * Return: 1 with a record, 0 when the wait timed out, -1 on failure.
 */
int omcid_tap_next(struct omcid_conn *conn, struct omcid_tap_record *out);

/**
 * omcid_vlan_fate_list() - Read what became of every VLAN rule.
 * @conn: the connection
 * @out: the records
 * @cap: room in @out
 * @n: the records written; may be NULL
 * @total: the records the daemon holds; may be NULL
 * @overflow: the rules the daemon had no room to record; may be NULL
 *
 * Return: 0 or -1.
 */
int omcid_vlan_fate_list(struct omcid_conn *conn,
			 struct omcid_vlan_fate_entry *out, size_t cap,
			 size_t *n, size_t *total, unsigned int *overflow);

#endif
