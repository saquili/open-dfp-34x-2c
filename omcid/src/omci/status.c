// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Names of the status codes, for log lines. */

#include "omcid/status.h"

const char *omci_status_str(enum omci_status status)
{
	switch (status) {
	case OMCI_STATUS_SIGNED:
		break;
	case OMCI_OK:
		return "OMCI_OK";
	case OMCI_DECLINED:
		return "OMCI_DECLINED";
	case OMCI_ERR_INVAL:
		return "OMCI_ERR_INVAL";
	case OMCI_ERR_NOMEM:
		return "OMCI_ERR_NOMEM";
	case OMCI_ERR_TRUNCATED:
		return "OMCI_ERR_TRUNCATED";
	case OMCI_ERR_PROTO:
		return "OMCI_ERR_PROTO";
	case OMCI_ERR_MIC:
		return "OMCI_ERR_MIC";
	case OMCI_ERR_ME_UNKNOWN:
		return "OMCI_ERR_ME_UNKNOWN";
	case OMCI_ERR_INST_UNKNOWN:
		return "OMCI_ERR_INST_UNKNOWN";
	case OMCI_ERR_ATTR_UNKNOWN:
		return "OMCI_ERR_ATTR_UNKNOWN";
	case OMCI_ERR_EXISTS:
		return "OMCI_ERR_EXISTS";
	case OMCI_ERR_BUSY:
		return "OMCI_ERR_BUSY";
	case OMCI_ERR_HW:
		return "OMCI_ERR_HW";
	case OMCI_ERR_AGAIN:
		return "interrupted, try again";
	case OMCI_ERR_IO:
		return "OMCI_ERR_IO";
	}
	return "OMCI_ERR_?";
}
