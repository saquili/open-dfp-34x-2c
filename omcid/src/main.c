// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The daemon of this build: frames on the standard input and output. */

#include <unistd.h>

#include "daemon.h"
#include "omcid/ctl_proto.h"

int main(int argc, char **argv)
{
	return omcid_run(argc, argv, STDIN_FILENO, STDOUT_FILENO,
			 OMCI_CTL_SOCKET_PATH);
}
