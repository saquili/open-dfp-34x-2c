/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The protocol of the control socket. A request is 12 bytes; a response is
 * an 8-byte header and up to 512 bytes of payload. Every field is
 * big-endian. A client that asked to follow the frames then receives one
 * response per frame, each carrying a tap record.
 */

#ifndef OMCI_CTL_PROTO_H
#define OMCI_CTL_PROTO_H

#include <stdint.h>

/* The first four bytes of every request and response: "OMCI". */
#define OMCI_CTL_MAGIC 0x4F4D4349u

/*
 * On the stick omcid runs as an account of its own, in a directory of its own,
 * where everything it writes is: its socket, the MIB it keeps (-m), the
 * identity it reads, and the files that tell onu-apply what the OLT set up.
 */
#define OMCI_RUN_DIR "/run/omcid"
#define OMCI_CTL_SOCKET_PATH OMCI_RUN_DIR "/ctl"

/* A request: magic, operation, class, instance and an argument, at these offsets. */
#define OMCI_CTL_REQ_LEN 12u
#define OMCI_CTL_REQ_MAGIC 0u
#define OMCI_CTL_REQ_OP 4u
#define OMCI_CTL_REQ_CLASS 6u
#define OMCI_CTL_REQ_INSTANCE 8u
#define OMCI_CTL_REQ_ARG 10u

/* A response header: magic, status and payload length, at these offsets. */
#define OMCI_CTL_RSP_LEN 8u
#define OMCI_CTL_RSP_MAGIC 0u
#define OMCI_CTL_RSP_STATUS 4u
#define OMCI_CTL_RSP_PAYLEN 6u

#define OMCI_CTL_PAYLOAD_MAX 512u

/**
 * enum omci_ctl_op - The operations.
 * @OMCI_CTL_OP_ME_GET: the attribute values of an instance, end to end
 * @OMCI_CTL_OP_ME_LIST: the instances of a class, two bytes each
 * @OMCI_CTL_OP_STATS: the counters of the engine, three words
 * @OMCI_CTL_OP_TAP: follow the frames received and sent
 * @OMCI_CTL_OP_VLAN_FATES: what became of each VLAN rule, a page from the
 *	record the argument names
 * @OMCI_CTL_OP_ME_DUMP: every attribute of an instance, each a record of its
 *	own: a table attribute's rows as well, which %OMCI_CTL_OP_ME_GET
 *	leaves out
 */
enum omci_ctl_op {

	OMCI_CTL_OP_ME_GET = 1,

	OMCI_CTL_OP_ME_LIST = 2,

	OMCI_CTL_OP_STATS = 3,

	OMCI_CTL_OP_TAP = 4,

	OMCI_CTL_OP_VLAN_FATES = 5,

	OMCI_CTL_OP_ME_DUMP = 6,
};

/*
 * A record of a dump: the attribute's number, with %OMCI_CTL_DUMP_TABLE set
 * for a table, the length of its value, two bytes, and the value; a table's
 * value is its rows end to end. An instance whose records do not all fit
 * the payload is answered %OMCI_CTL_ERR_TOO_BIG.
 */
#define OMCI_CTL_DUMP_HDR_LEN 3u
#define OMCI_CTL_DUMP_TABLE 0x80u

/* The fates of a VLAN rule, as enum omci_vlan_fate of the daemon defines them. */
#define OMCI_CTL_VLAN_FATE_PROGRAMMED 0u
#define OMCI_CTL_VLAN_FATE_COLLAPSED 1u
#define OMCI_CTL_VLAN_FATE_SHADOWED 2u
#define OMCI_CTL_VLAN_FATE_OVER_BUDGET 3u
#define OMCI_CTL_VLAN_FATE_UNSUPPORTED 4u
#define OMCI_CTL_VLAN_FATE_INERT 5u

#define OMCI_CTL_VLAN_FATE_REFUSED 6u
#define OMCI_CTL_VLAN_FATE_LOCAL 7u

/*
 * A page of fates: the total and the records that did not fit, then
 * records of 12 bytes: instance, row, fate, matched VID, network VID, and
 * the matched and network VIDs of the rule that took precedence.
 */
#define OMCI_CTL_VLAN_HDR_LEN 4u
#define OMCI_CTL_VLAN_TOTAL 0u
#define OMCI_CTL_VLAN_OVERFLOW 2u
#define OMCI_CTL_VLAN_REC_LEN 12u
#define OMCI_CTL_VLAN_PAGE_MAX \
	((OMCI_CTL_PAYLOAD_MAX - OMCI_CTL_VLAN_HDR_LEN) / OMCI_CTL_VLAN_REC_LEN)

/* A tap record: verdict, message type, frames lost before it, the frame. */
#define OMCI_CTL_TAP_VERDICT 0u
#define OMCI_CTL_TAP_DETAIL 1u
#define OMCI_CTL_TAP_DROPPED 2u
#define OMCI_CTL_TAP_FRAME 4u
#define OMCI_CTL_TAP_REC_LEN 52u

/**
 * enum omci_ctl_tap_verdict - What happened to a frame.
 * @OMCI_CTL_TAP_RX_PARSE: received, not a frame of the protocol
 * @OMCI_CTL_TAP_RX_MIC: received, integrity check wrong
 * @OMCI_CTL_TAP_RX_ARAK: received, request and answer bits wrong
 * @OMCI_CTL_TAP_RX_REPLAY: received again; the stored answer was sent
 * @OMCI_CTL_TAP_RX_DISPATCH: received and executed
 * @OMCI_CTL_TAP_TX: sent
 * @OMCI_CTL_TAP_RX_ACTION: received, an action the class does not accept
 * @OMCI_CTL_TAP_RX_UNKNOWN_ME: received, a class the device lacks
 * @OMCI_CTL_TAP_RX_UNHANDLED: received, a message type not executed
 */
enum omci_ctl_tap_verdict {
	OMCI_CTL_TAP_RX_PARSE = 0,
	OMCI_CTL_TAP_RX_MIC = 1,
	OMCI_CTL_TAP_RX_ARAK = 2,
	OMCI_CTL_TAP_RX_REPLAY = 3,
	OMCI_CTL_TAP_RX_DISPATCH = 4,
	OMCI_CTL_TAP_TX = 5,
	OMCI_CTL_TAP_RX_ACTION = 6,
	OMCI_CTL_TAP_RX_UNKNOWN_ME = 7,
	OMCI_CTL_TAP_RX_UNHANDLED = 8,
};

/**
 * enum omci_ctl_status - The status of a response.
 * @OMCI_CTL_OK: done
 * @OMCI_CTL_ERR_OP: an operation that does not exist
 * @OMCI_CTL_ERR_NOT_FOUND: no such instance
 * @OMCI_CTL_ERR_TOO_BIG: the answer does not fit the payload
 * @OMCI_CTL_ERR_PROTO: a request without the magic number
 */
enum omci_ctl_status {
	OMCI_CTL_OK = 0,
	OMCI_CTL_ERR_OP = 1,
	OMCI_CTL_ERR_NOT_FOUND = 2,
	OMCI_CTL_ERR_TOO_BIG = 3,
	OMCI_CTL_ERR_PROTO = 4,
};

/* The payload of the counters: handled, rejected and answered frames. */
#define OMCI_CTL_STATS_LEN 12u

#endif
