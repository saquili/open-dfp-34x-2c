# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Read by /etc/profile for the console shell and for ssh logins. The root file
# system is read-only, so the history of a shell is kept in memory.
HISTFILE=/tmp/.ash_history
PS1='\w # '
