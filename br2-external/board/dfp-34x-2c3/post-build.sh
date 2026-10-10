#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# post-build.sh: what the build system runs on the target directory before it
# makes the images.
#
# Usage: post-build.sh TARGET_DIR
#
# It takes out the programs of iproute2 that the stick does not run, and the
# command-line tools of the network layer; it cuts the shared libraries to what
# the programs use, with tools/lib-trim, when the tree holds the shared C
# library; and it writes into /etc/os-release, as
# BUILD_ID, the commit of this repository that the image is built from: the
# first twelve digits of its hash, followed by "-dirty" when the working tree
# holds changes that are not committed, or "unknown" when the sources are not a
# git checkout. The status daemon and the device tests read it, so that a
# result names the build it was measured on. Two builds of one commit write the
# same line.
#
# The cut needs what the build system gives a post-build script: HOST_DIR and
# STAGING_DIR for the cross tools, BUILD_DIR for uClibc-ng's build directory.
# GIT names another git, which the tests use.

set -eu

[ $# -eq 1 ] || { echo "usage: post-build.sh TARGET_DIR" >&2; exit 2; }
release=$1/etc/os-release
[ -f "$release" ] || { echo "post-build.sh: no $release" >&2; exit 1; }

top=$(cd "$(dirname "$0")/../../.." && pwd)
git=${GIT:-git}
if rev=$("$git" -C "$top" rev-parse --short=12 HEAD 2>/dev/null); then
	if [ -n "$("$git" -C "$top" status --porcelain 2>/dev/null)" ]; then
		rev=$rev-dirty
	fi
else
	rev=unknown
fi

# The package installs every program of iproute2. The stick runs ip, tc and
# devlink, from the boot script and onu-mode, and keeps bridge for a person
# who looks at the bridge's VLANs; the others would add some 3 MB, unpacked,
# to a root filesystem that has to fit the partition r0.
for prog in ctstat dcb genl ifstat lnstat nstat rdma routel rtacct rtmon rtstat ss tipc vdpa; do
	rm -f "$1/sbin/$prog"
done

# The network layer installs a command-line tool of each part: ubus, uci and
# jshn, with jshn's shell library. A person at the stick has the ssh server and
# BusyBox, and the panel for the rest; netifd and ubusd need none of them, and
# lib-trim then takes out the library only jshn loads.
rm -f "$1/usr/bin/ubus" "$1/usr/bin/uci" "$1/usr/bin/jshn"
rm -rf "$1/usr/share/libubox"

# The programs share one C library (docs/adr/0011). The build system installs
# it whole, with every function it offers, and libgcc_s, which no program
# needs; lib-trim removes what nothing loads, links the C library again with
# what the programs import, and fails the build when a program would miss a
# library or a symbol.
for libc in "$1"/lib/libuClibc-*.so; do
	[ -f "$libc" ] || continue
	: "${HOST_DIR:?post-build.sh: HOST_DIR is not set; the build system sets it}"
	: "${STAGING_DIR:?post-build.sh: STAGING_DIR is not set; the build system sets it}"
	: "${BUILD_DIR:?post-build.sh: BUILD_DIR is not set; the build system sets it}"
	tuple=$(basename "$(dirname "$STAGING_DIR")")
	"$top/tools/lib-trim/lib-trim" --cross "$HOST_DIR/bin/$tuple-" \
		--uclibc "$BUILD_DIR/$(basename "$libc" .so | sed 's/^libuClibc-/uclibc-/')" "$1"
done

grep -v '^BUILD_ID=' "$release" > "$release.new" || true
echo "BUILD_ID=$rev" >> "$release.new"
mv "$release.new" "$release"
