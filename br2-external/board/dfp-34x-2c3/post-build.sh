#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# post-build.sh: what the build system runs on the target directory before it
# makes the images.
#
# Usage: post-build.sh TARGET_DIR
#
# It takes out the programs of iproute2, which the stick does not run, the
# command-line tools of the network layer and the tools that diagnose a stick,
# setting aside iproute2's ip, tc, devlink, bridge and dcb, and ethtool, with the
# latter for the RAM image, and leaves /sbin/ip to BusyBox; it takes out the
# skeleton's /etc/resolv.conf, a link to a file nothing writes, as the stick
# asks no name server; it cuts the shared libraries to what the programs use,
# with tools/lib-trim, when the tree holds the shared C library; and it writes
# into /etc/os-release, as BUILD_ID, the commit of this repository that the
# image is built from: the first twelve digits of its hash, followed by
# "-dirty" when the working tree holds changes that are not committed, or
# "unknown" when the sources are not a git checkout. The status daemon and the
# device tests read it, so that a result names the build it was measured on.
# Two builds of one commit write the same line.
#
# The cut needs what the build system gives a post-build script: HOST_DIR and
# STAGING_DIR for the cross tools, BUILD_DIR for uClibc-ng's build directory.
# BASE_DIR names the build's directory, where the tools of diagnosis are set
# aside; without it they go with the rest. GIT names another git, which the
# tests use.

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

# The package installs every program of iproute2, and the stick runs none of
# them: onu-nl does what the boot script and the scripts of onu-utils ask of
# ip, tc and devlink (ADR 0014, step 9g); BusyBox's ip stays built in for a
# person at the console, and no script runs it. The RAM image carries ip, tc,
# devlink, bridge and dcb for diagnosis, below; the others would add some 3 MB,
# unpacked, to it.
for prog in ctstat genl ifstat lnstat nstat rdma routel rtacct rtmon rtstat ss tipc vdpa; do
	rm -f "$1/sbin/$prog"
done

# The network layer installs a command-line tool of each part: ubus, uci and
# jshn, with jshn's shell library. A person at the stick has the ssh server and
# BusyBox, and the panel for the rest; netifd and ubusd need none of them, and
# lib-trim then takes out the library only jshn loads.
#
# A stick is diagnosed started from RAM, and its flash holds the image of the
# final test, so the tools of diagnosis, omcid's omcitap and omcivlan, the
# network layer's ubus, the flash's flashcp, flash_erase and mtd_debug,
# ethtool, and iproute2's tc, devlink, bridge and dcb with the tables of tc and
# of the names ip uses, are set aside in $BASE_DIR/diag, which "make ram-image"
# lays over its copy of this directory; uci and jshn go.
diag=${BASE_DIR:+$BASE_DIR/diag}
for prog in usr/bin/omcitap usr/bin/omcivlan usr/bin/ubus usr/sbin/flashcp \
	usr/sbin/flash_erase usr/sbin/mtd_debug usr/sbin/ethtool sbin/tc sbin/devlink \
	sbin/bridge sbin/dcb usr/lib/tc usr/share/iproute2; do
	[ -e "$1/$prog" ] || continue
	if [ -n "$diag" ]; then
		mkdir -p "$diag/${prog%/*}"
		rm -rf "${diag:?}/$prog"
		mv "$1/$prog" "$diag/$prog"
	else
		rm -rf "${1:?}/$prog"
	fi
done

# iproute2's ip stands where BusyBox's would. It goes to the RAM image as
# ip-full, so that /sbin/ip is BusyBox's there, as from the flash, and
# becomes BusyBox's again. A second run finds BusyBox's link.
if [ -f "$1/sbin/ip" ] && [ ! -L "$1/sbin/ip" ]; then
	if [ -n "$diag" ]; then
		mkdir -p "$diag/usr/sbin"
		mv "$1/sbin/ip" "$diag/usr/sbin/ip-full"
	else
		rm -f "$1/sbin/ip"
	fi
	[ ! -e "$1/bin/busybox" ] || ln -s ../bin/busybox "$1/sbin/ip"
fi
rm -f "$1/usr/bin/uci" "$1/usr/bin/jshn"
rm -rf "$1/usr/share/libubox"
# The stick runs offline: it asks no name server, so it has no resolver
# configuration. The skeleton links /etc/resolv.conf to a file in /tmp that
# nothing writes.
rm -f "$1/etc/resolv.conf"

# The programs share one C library (docs/adr/0011). The build system installs
# it whole, with every function it offers, and libgcc_s, which no program
# needs; lib-trim removes what nothing loads, links the C library again with
# what the programs import, and fails the build when a program would miss a
# library or a symbol. The tools of diagnosis are not among the programs it
# cuts for, so they get the C library whole, as the build system installs it
# into the staging directory, stripped as for the target.
for libc in "$1"/lib/libuClibc-*.so; do
	[ -f "$libc" ] || continue
	: "${HOST_DIR:?post-build.sh: HOST_DIR is not set; the build system sets it}"
	: "${STAGING_DIR:?post-build.sh: STAGING_DIR is not set; the build system sets it}"
	: "${BUILD_DIR:?post-build.sh: BUILD_DIR is not set; the build system sets it}"
	tuple=$(basename "$(dirname "$STAGING_DIR")")
	if [ -n "$diag" ]; then
		mkdir -p "$diag/lib"
		"$HOST_DIR/bin/$tuple-strip" --remove-section=.comment --remove-section=.note \
			-o "$diag/lib/${libc##*/}" "$STAGING_DIR/lib/${libc##*/}"
	fi
	"$top/tools/lib-trim/lib-trim" --cross "$HOST_DIR/bin/$tuple-" \
		--uclibc "$BUILD_DIR/$(basename "$libc" .so | sed 's/^libuClibc-/uclibc-/')" "$1"
done

# The programs that face a network run as accounts of their own, without a
# password or a shell, which no one logs in with (ADR 0014): pond, the status
# daemon, ntp, the time daemon timed's, the DHCPv6 daemon dhcp6d, the PPPoE
# client pppoed and the OMCI daemon omcid, started by onu-drop with the powers
# each needs. The image is made from a copy of this directory (post-image.sh), not
# by the build system's own making of accounts, so they are written here,
# once, with the uid and gid 200 to 204. /etc/shadow becomes
# /etc/shadow.image, from which the boot writes every account but root's as
# it is.
account() {
	[ -f "$1" ] || return 0
	# A line of an earlier build is replaced, as a tree that is built again
	# keeps what the last build wrote; the file keeps its mode and owner.
	{ grep -v "^$2:" "$1" || true; echo "$3"; } >"$1.new" && cat "$1.new" >"$1"
	rm -f "$1.new"
}
account "$1/etc/passwd" pond "pond:x:200:200:The status daemon:/var/empty:/bin/false"
account "$1/etc/group" pond "pond:x:200:"
account "$1/etc/shadow" pond "pond:*:::::::"
account "$1/etc/passwd" ntp "ntp:x:201:201:The time daemon:/var/empty:/bin/false"
account "$1/etc/group" ntp "ntp:x:201:"
account "$1/etc/shadow" ntp "ntp:*:::::::"
account "$1/etc/passwd" dhcp6d "dhcp6d:x:202:202:The DHCPv6 daemon:/var/empty:/bin/false"
account "$1/etc/group" dhcp6d "dhcp6d:x:202:"
account "$1/etc/shadow" dhcp6d "dhcp6d:*:::::::"
account "$1/etc/passwd" pppoed "pppoed:x:203:203:The PPPoE client:/var/empty:/bin/false"
account "$1/etc/group" pppoed "pppoed:x:203:"
account "$1/etc/shadow" pppoed "pppoed:*:::::::"
account "$1/etc/passwd" omcid "omcid:x:204:204:The OMCI daemon:/var/empty:/bin/false"
account "$1/etc/group" omcid "omcid:x:204:"
account "$1/etc/shadow" omcid "omcid:*:::::::"
# The group onu: the accounts of the programs that write into a session's
# directory, which onu-apply makes the group's.
account "$1/etc/group" onu "onu:x:210:dhcp6d,pppoed"

grep -v '^BUILD_ID=' "$release" > "$release.new" || true
echo "BUILD_ID=$rev" >> "$release.new"
mv "$release.new" "$release"
