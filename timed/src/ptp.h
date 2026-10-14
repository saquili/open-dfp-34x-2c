/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The PTP slave (IEEE 1588-2008, an ordinary clock that is never a master,
 * with the delay request-response mechanism): the messages it reads, the one
 * it sends, the choice of the master and the offset from it.
 *
 * Times are the receiver's own stamps of the messages, taken by the caller
 * in the clock that is to be set. That clock counts UTC (the system clock),
 * or the master's own timescale (a PTP hardware clock, which a master on the
 * PTP timescale keeps in TAI): ptp_set_local_timescale(). The messages are the
 * same over layer 2 and over UDP; the caller strips the Ethernet or the UDP
 * header.
 */

#ifndef TIMED_PTP_H
#define TIMED_PTP_H

#include "timed.h"

#define PTP_HEADER_LEN 34
#define PTP_DELAY_REQ_LEN 44

/* Message types (IEEE 1588-2008, table 19). */
#define PTP_SYNC 0x0
#define PTP_DELAY_REQ 0x1
#define PTP_FOLLOW_UP 0x8
#define PTP_DELAY_RESP 0x9
#define PTP_ANNOUNCE 0xb

/* The seconds the PTP timescale (TAI) is ahead of UTC when the master does not say. */
#define PTP_UTC_OFFSET_DEFAULT 37

/* A port: the clock identity of a node and its port number. */
struct ptp_id {
	uint8_t clock[8];
	uint16_t port;
};

/* What an Announce says of the master, in the order that decides between two (clause 9.3.2.4). */
struct ptp_dataset {
	uint8_t priority1;
	uint8_t clock_class;
	uint8_t accuracy;
	uint16_t variance;
	uint8_t priority2;
	uint8_t grandmaster[8];
	uint16_t steps_removed;
};

enum ptp_event {
	PTP_EV_NONE = 0,
	PTP_EV_MASTER,		/* a master chosen, or another one */
	PTP_EV_OFFSET,		/* a new offset from the master */
};

struct ptp_result {
	enum ptp_event ev;
	ns_t offset;		/* the local clock less the master's, in UTC */
	ns_t delay;		/* the mean path delay in use, 0 until measured */
	bool delay_known;
};

struct ptp_slave {
	uint8_t domain;
	struct ptp_id own;
	uint16_t request_seq;

	bool have_master;
	struct ptp_id master;
	struct ptp_dataset master_ds;
	ns_t announce_at;	/* local monotonic time of the last Announce */
	int utc_offset;		/* seconds, TAI less UTC */
	bool ptp_timescale;	/* the master's times are TAI */
	bool local_timescale;	/* the clock to be set counts the master's timescale, not UTC */
	int announce_log_interval;
	int sync_log_interval;	/* the master's own, 2^n seconds, from the headers */

	/* The Sync waiting for its Follow_Up, or complete. */
	bool sync_wait;
	uint16_t sync_seq;
	ns_t sync_t2;		/* local receive time of the Sync */
	ns_t sync_corr;		/* correctionField of the Sync, in ns */
	ns_t sync_ms;		/* t2 - t1 of the last complete Sync, TAI-corrected; valid with have_ms */
	bool have_ms;

	/* The Delay_Req sent and waiting for its Delay_Resp. */
	bool request_wait;
	uint16_t request_sent_seq;
	ns_t request_t3;
	bool request_t3_known;	/* false until the time of sending is known (ptp_delay_req_sent()) */
	ns_t delay;		/* mean path delay */
	bool delay_known;
};

void ptp_slave_init(struct ptp_slave *s, uint8_t domain, const uint8_t clock_id[8]);

/*
 * Read a message that arrived at local time @rx (the clock to be set), when
 * the monotonic clock read @mono. Return true with @r filled when it gave an
 * event.
 */
bool ptp_rx(struct ptp_slave *s, const uint8_t *msg, size_t len, ns_t rx, ns_t mono,
	    struct ptp_result *r);

/* Whether the master was heard of within three of its announce intervals (at least 8 s) before @mono. */
bool ptp_master_alive(const struct ptp_slave *s, ns_t mono);

/* Forget the master (it is gone). */
void ptp_master_lost(struct ptp_slave *s);

/* The local clock was stepped: the times in flight are of another clock. */
void ptp_clock_stepped(struct ptp_slave *s);

/*
 * Build a Delay_Req into @buf, which holds PTP_DELAY_REQ_LEN bytes, and note
 * @t3 as the local time of its sending. Return its length, 0 without a master.
 */
size_t ptp_delay_req(struct ptp_slave *s, uint8_t *buf, ns_t t3);

/*
 * Build a Delay_Req whose time of sending is known only once it has left: a
 * hardware stamp, given with ptp_delay_req_sent(). A Delay_Resp that comes
 * before it is not used.
 */
size_t ptp_delay_req_later(struct ptp_slave *s, uint8_t *buf);

/* The Delay_Req last built left at local time @t3. */
void ptp_delay_req_sent(struct ptp_slave *s, ns_t t3);

/*
 * Whether the clock to be set counts the master's timescale (true: a PHC) or
 * UTC (false: the system clock, the default). A change drops what is in
 * flight, as a step does.
 */
void ptp_set_local_timescale(struct ptp_slave *s, bool master);

/* What a time of the master's timescale is ahead of UTC: the UTC offset on the PTP timescale, else 0. */
ns_t ptp_utc_shift(const struct ptp_slave *s);

/* A node's clock identity from its MAC address (IEEE EUI-64: ff:fe in the middle). */
void ptp_clock_id_from_mac(uint8_t id[8], const uint8_t mac[6]);

#endif
