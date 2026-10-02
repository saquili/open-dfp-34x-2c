/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The status that the daemon's functions return. */

#ifndef OMCI_STATUS_H
#define OMCI_STATUS_H

#include <stdint.h>

/**
 * enum omci_status - Outcome of an operation.
 * @OMCI_STATUS_SIGNED: never returned; it makes the type signed
 * @OMCI_OK: done
 * @OMCI_DECLINED: the request is valid, but the device does not carry it
 *	out, for instance a VLAN rule the hardware cannot express
 * @OMCI_ERR_INVAL: a bad argument
 * @OMCI_ERR_NOMEM: out of memory, or a table is full
 * @OMCI_ERR_TRUNCATED: the input ended early
 * @OMCI_ERR_PROTO: not a frame of the protocol, or a malformed one
 * @OMCI_ERR_MIC: the message integrity check does not match
 * @OMCI_ERR_ME_UNKNOWN: a managed entity class the device does not have
 * @OMCI_ERR_INST_UNKNOWN: an instance that does not exist
 * @OMCI_ERR_ATTR_UNKNOWN: an attribute the class does not have
 * @OMCI_ERR_EXISTS: the instance exists already
 * @OMCI_ERR_BUSY: the device cannot do it now
 * @OMCI_ERR_HW: the hardware refused
 * @OMCI_ERR_IO: a system call failed
 * @OMCI_ERR_AGAIN: interrupted; try again
 */
enum omci_status {

	OMCI_STATUS_SIGNED = INT32_MIN,

	OMCI_OK = 0,

	OMCI_DECLINED = 1,

	OMCI_ERR_INVAL = 2,
	OMCI_ERR_NOMEM = 3,
	OMCI_ERR_TRUNCATED = 4,
	OMCI_ERR_PROTO = 5,
	OMCI_ERR_MIC = 6,
	OMCI_ERR_ME_UNKNOWN = 7,
	OMCI_ERR_INST_UNKNOWN = 8,
	OMCI_ERR_ATTR_UNKNOWN = 9,
	OMCI_ERR_EXISTS = 10,
	OMCI_ERR_BUSY = 11,
	OMCI_ERR_HW = 12,
	OMCI_ERR_IO = 13,
	OMCI_ERR_AGAIN = 14,

};

/**
 * omci_status_str() - Name a status, for a log line.
 * @status: the status
 *
 * Return: a constant string.
 */
const char *omci_status_str(enum omci_status status);

#endif
