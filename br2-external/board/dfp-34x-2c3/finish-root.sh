#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# finish-root.sh: what a copy of the target tree needs before it becomes the
# root filesystem of an image, the flash's (post-image.sh) or the RAM boot's
# (make ram-image).
#
# Usage: finish-root.sh ROOT
#
# Root's password is kept in the configuration store, and the boot writes the
# accounts, root's with that password, to /run/shadow (onu-config), the root
# filesystem being read-only. /etc/shadow, which login and the ssh server
# read, becomes a link to it; the accounts the build system wrote stay as
# /etc/shadow.image, from which onu-config takes the others. The work is done
# on a copy because the build system edits /etc/shadow of the target tree at
# every build and cannot edit it through a link that points nowhere there.

set -eu

[ $# -eq 1 ] || { echo "usage: finish-root.sh ROOT" >&2; exit 2; }
etc=$1/etc

if [ -L "$etc/shadow" ]; then
	echo "finish-root.sh: $etc/shadow is a link already; the copy was finished before" >&2
	exit 1
fi
[ -f "$etc/shadow" ] || { echo "finish-root.sh: no $etc/shadow" >&2; exit 1; }
mv "$etc/shadow" "$etc/shadow.image"
chmod 600 "$etc/shadow.image"
ln -s ../run/shadow "$etc/shadow"
