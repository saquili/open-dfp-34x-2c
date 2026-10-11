#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# post-image.sh: the two flash images of the stick.
#
# Usage: post-image.sh IMAGES_DIR
#
# Buildroot runs it last, with the images directory as its argument and these
# variables in the environment: TARGET_DIR (the root filesystem as it will be
# packed), HOST_DIR (the tools built for the build machine) and, because the
# defconfig asks for a reproducible build, SOURCE_DATE_EPOCH. It leaves in the
# images directory:
#
#   k0.uImage    the kernel image that the boot loader loads for bank 0: the
#                kernel's uImage, unchanged
#   r0.squashfs  the root filesystem for bank 0, a SquashFS image
#   update.bin   the two in one update file, which the stick's onu-update
#                writes into the bank it did not start from: made by
#                mkupdate.py, with the board, the version (BUILD_ID of the
#                image's /etc/os-release) and the build's time in its header
#   web-install.tar
#                the two in the package that the web page of the stick's
#                shipped firmware installs into the bank it did not start
#                from: made by mkweb.py, with fwu.sh (ADR 0014, step 9)
#   SHA256SUMS   the SHA-256 digests of those four files, in the form that
#                "sha256sum -c" reads
#
# The root filesystem image is made here, and not by the build system's own
# SquashFS target, so that its properties are written down in one place. It is
# compressed with xz, the smallest of the formats the kernel reads, in blocks
# of 64 KiB, which the kernel unpacks one at a time. Every file is owned by root
# and carries the build's time, so that the same tree gives the same image
# whoever builds it and when; no extended attribute of the build machine's file
# system gets in; nothing is added for NFS export or to pad the image to a
# block; and the file with which the build system marks its target directory as
# not being a root filesystem is left out. docs/handbook/building.rst has the
# sizes and what to do with the images.
#
# The image is made from a copy of the target tree, which finish-root.sh
# finishes (/etc/shadow becomes a link to the file the boot writes); the target
# directory of the build is not touched. The files of one bench, an ssh key or
# other addresses, do not belong in the repository: a directory named by
# OPEN_DFP_34X_2C_LOCAL_OVERLAY is laid over the copy too, so the next build
# without the variable gives the image without those files. (The build system's
# own overlays copy their files into the target directory and never remove
# them.)
#
# MKSQUASHFS names another mksquashfs, which the tests use; PYTHON the Python
# that runs mkupdate.py and mkweb.py (python3).

set -eu

die() {
	echo "post-image: $*" >&2
	exit 1
}

[ $# -ge 1 ] || die "usage: post-image.sh IMAGES_DIR"
images=$1
target=${TARGET_DIR:?"TARGET_DIR is not set; Buildroot sets it"}
epoch=${SOURCE_DATE_EPOCH:?"SOURCE_DATE_EPOCH is not set; the defconfig has to ask for a reproducible build"}
mksquashfs=${MKSQUASHFS:-${HOST_DIR:?"HOST_DIR is not set; Buildroot sets it"}/bin/mksquashfs}

python=${PYTHON:-python3}
board=odi,dfp-34x-2c3

bench=${OPEN_DFP_34X_2C_LOCAL_OVERLAY:-}

[ -d "$images" ] || die "$images is not a directory"
[ -d "$target" ] || die "$target is not a directory"
[ -f "$images/uImage" ] || die "no kernel image $images/uImage"
[ -x "$mksquashfs" ] || die "no mksquashfs at $mksquashfs"
[ -z "$bench" ] || [ -d "$bench" ] || die "OPEN_DFP_34X_2C_LOCAL_OVERLAY, $bench, is not a directory"

# Each image is made under a name of its own and moved into place, so that an
# error never leaves half an image under the name that is flashed.
scratch=
rm -f "$images/k0.uImage.part" "$images/r0.squashfs.part"
trap 'rm -f "$images/k0.uImage.part" "$images/r0.squashfs.part" "$images/SHA256SUMS.part"
	[ -z "$scratch" ] || rm -rf "$scratch"' EXIT

scratch=$(mktemp -d "${TMPDIR:-/tmp}/post-image.XXXXXX") || die "cannot make a scratch directory"
cp -RPp "$target/." "$scratch/"
[ -z "$bench" ] || cp -RPp "$bench/." "$scratch/"
root=$scratch
sh "$(dirname "$0")/finish-root.sh" "$root" || die "finish-root.sh failed"

cp "$images/uImage" "$images/k0.uImage.part"
mv "$images/k0.uImage.part" "$images/k0.uImage"

# mksquashfs refuses a time on its command line while SOURCE_DATE_EPOCH is set,
# and with only the variable it keeps the time of a file that is older than
# the epoch. The time is given on the command line, for every file alike.
(
	unset SOURCE_DATE_EPOCH
	"$mksquashfs" "$root" "$images/r0.squashfs.part" \
		-noappend -quiet -no-progress -reproducible \
		-comp xz -b 64K -nopad -no-xattrs -no-exports \
		-all-root -root-mode 755 \
		-all-time "$epoch" -mkfs-time "$epoch" \
		-e THIS_IS_NOT_YOUR_ROOT_FILESYSTEM
)
mv "$images/r0.squashfs.part" "$images/r0.squashfs"

# The version is the one the image names itself by; mkupdate.py writes its
# output under a name of its own and moves it into place.
version=$(sed -n 's/^BUILD_ID=//p' "$root/etc/os-release" 2>/dev/null || true)
[ -n "$version" ] || die "no BUILD_ID in $root/etc/os-release"
"$python" "$(dirname "$0")/mkupdate.py" --board "$board" --version "$version" \
	--time "$epoch" "$images/k0.uImage" "$images/r0.squashfs" "$images/update.bin" ||
	die "mkupdate.py failed"
"$python" "$(dirname "$0")/mkweb.py" --version "$version" --time "$epoch" \
	"$images/k0.uImage" "$images/r0.squashfs" "$images/web-install.tar" ||
	die "mkweb.py failed"

(cd "$images" && sha256sum k0.uImage r0.squashfs update.bin web-install.tar) > "$images/SHA256SUMS.part"
mv "$images/SHA256SUMS.part" "$images/SHA256SUMS"

size() {
	wc -c < "$1" | tr -d ' '
}
echo "post-image: $(size "$images/k0.uImage") bytes in k0.uImage, $(size "$images/r0.squashfs") bytes in r0.squashfs, $(size "$images/update.bin") bytes in update.bin, $(size "$images/web-install.tar") bytes in web-install.tar"
