#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of onu-update, on a flash made of files under ONU_ROOT: the
# partitions of the stick's layout, smaller, with a /proc/mtd that names them
# and a command line, a device tree and an os-release as a boot gives them.
# The update files are packed by the image build's mkupdate.py, so that its
# format and onu-update's are checked against each other; python3 also makes
# the stand-ins for a kernel image and a root filesystem.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$HERE/build}
MKUPDATE=$HERE/../br2-external/board/dfp-34x-2c3/mkupdate.py
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

# The exit status of the last run is in rc, its output in out and its
# messages in err.
run() {
	out=$("$BUILD/onu-update" "$@" 2>"$T/err")
	rc=$?
	err=$(cat "$T/err")
}

# The images and the environment, made by python3.
FIXTURES='
import struct, sys, zlib
kind, out = sys.argv[1], sys.argv[2]
if kind == "uimage":
    data = bytes((i * 7 + int(sys.argv[3])) & 0xff for i in range(int(sys.argv[4])))
    head = struct.pack(">IIIIIIIBBBB32s", 0x27051956, 0, int(sys.argv[5]), len(data),
                       0x80000000, 0x80000000, zlib.crc32(data), 5, 5, 2, 3, b"Linux-test")
    head = head[:4] + struct.pack(">I", zlib.crc32(head)) + head[8:]
    open(out, "wb").write(head + data)
elif kind == "squashfs":
    n = int(sys.argv[4])
    body = bytearray(bytes((i * 13 + int(sys.argv[3])) & 0xff for i in range(n)))
    body[0:4] = b"hsqs"
    body[40:48] = struct.pack("<Q", n)
    open(out, "wb").write(bytes(body))
elif kind == "env":
    flag = int(sys.argv[3])
    data = b"".join(v.encode() + b"\0" for v in sorted(sys.argv[4:])) + b"\0"
    data = data.ljust(0x2000 - 5, b"\0")
    open(out, "wb").write(struct.pack(">I", zlib.crc32(data)) + bytes([flag]) + data)
'

fixture() {
	python3 -c "$FIXTURES" "$@"
}

# Bytes @3 for @2 from offset @1 of a file, as hexadecimal.
bytes_at() {
	od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'
}

setup() {
	T=$(mktemp -d)
	export ONU_ROOT=$T/root
	mkdir -p "$ONU_ROOT/proc/device-tree" "$ONU_ROOT/dev" "$ONU_ROOT/etc"
	cat >"$ONU_ROOT/proc/mtd" <<-EOF
	dev:    size   erasesize  name
	mtd0: 00001000 00001000 "boot"
	mtd1: 00002000 00001000 "env"
	mtd2: 00002000 00001000 "env2"
	mtd3: 00001000 00001000 "config"
	mtd4: 00010000 00001000 "k0"
	mtd5: 00010000 00001000 "r0"
	mtd6: 00010000 00001000 "k1"
	mtd7: 00010000 00001000 "r1"
	EOF
	for n in 0 1 2 3 4 5 6 7; do
		size=$(awk -v m="mtd$n:" '$1 == m { print $2 }' "$ONU_ROOT/proc/mtd")
		dd if=/dev/zero bs=4096 count=$(($(printf '%d' 0x$size) / 4096)) 2>/dev/null |
			tr '\000' '\377' >"$ONU_ROOT/dev/mtd$n"
	done
	printf 'console=ttyS0,115200 mtdparts=x root=31:5\n' >"$ONU_ROOT/proc/cmdline"
	printf '/dev/root / squashfs ro,relatime 0 0\nproc /proc proc rw 0 0\n' >"$ONU_ROOT/proc/mounts"
	printf 'odi,dfp-34x-2c3\000realtek,rtl9602c\000' >"$ONU_ROOT/proc/device-tree/compatible"
	printf 'NAME=test\nBUILD_ID=0123456789ab\n' >"$ONU_ROOT/etc/os-release"
	# The loader's environment as the board keeps it: env2 current.
	fixture env "$ONU_ROOT/dev/mtd1" 0 bootdelay=5 sw_commit=0 sw_tryactive=2
	fixture env "$ONU_ROOT/dev/mtd2" 1 bootdelay=5 sw_active=0 sw_commit=0 \
		sw_tryactive=2 sw_valid0=1 sw_valid1=1 sw_version1=V1.0-210702
	# Bank 0 holds the running system.
	fixture uimage "$T/k-old" 1 30000 1759000000
	fixture squashfs "$T/r-old" 1 20000
	dd if="$T/k-old" of="$ONU_ROOT/dev/mtd4" conv=notrunc 2>/dev/null
	dd if="$T/r-old" of="$ONU_ROOT/dev/mtd5" conv=notrunc 2>/dev/null
	# An update, its kernel stamped with the same time as its header.
	fixture uimage "$T/k-new" 2 40000 1759800000
	fixture squashfs "$T/r-new" 2 30000
	python3 "$MKUPDATE" --board odi,dfp-34x-2c3 --version 7c5fba548abc \
		--time 1759800000 "$T/k-new" "$T/r-new" "$T/update.bin"
}

teardown() {
	rm -rf "$T"
}

# The value of KEY in the boot loader's environment, through onu-update.
envget() {
	"$BUILD/onu-update" env "$1" 2>/dev/null | sed "s/^$1=//"
}

echo "the banks as a boot leaves them"
setup
run status
check '[ $rc -eq 0 ]'
check 'echo "$out" | grep -qx RUNNING=0'
check 'echo "$out" | grep -qx COMMITTED=0'
check 'echo "$out" | grep -qx NEXT=0'
check 'echo "$out" | grep -qx BANK0_STATE=valid'
check 'echo "$out" | grep -qx BANK0_VERSION=0123456789ab'
check 'echo "$out" | grep -qx BANK0_TIME=1759000000'
check 'echo "$out" | grep -qx BANK1_STATE=empty'
check 'echo "$out" | grep -qx BANK1_KERNEL_SIZE=65536'
check 'echo "$out" | grep -qx BANK0_KERNEL_USED=30064'
check 'echo "$out" | grep -qx BANK0_ROOTFS_USED=20000'
check '! echo "$out" | grep -q BANK1_KERNEL_USED'
teardown

echo "a bank in the shipped firmware's split reads as shipped, not empty"
setup
# The shipped split, as the loader names it: a kernel of 32 KiB here, its
# root filesystem right after it, inside this system's kernel partition.
fixture env "$ONU_ROOT/dev/mtd2" 2 bootdelay=5 sw_active=0 sw_commit=0 \
	sw_tryactive=2 sw_valid0=1 sw_valid1=1 sw_version1=V1.0-210702 \
	fl_kernel1_sz=8000 fl_kernel2_sz=8000
fixture uimage "$T/k-shipped" 4 20000 1663932999
fixture squashfs "$T/r-shipped" 4 10000
dd if="$T/k-shipped" of="$ONU_ROOT/dev/mtd6" conv=notrunc 2>/dev/null
dd if="$T/r-shipped" of="$ONU_ROOT/dev/mtd6" bs=4096 seek=8 conv=notrunc 2>/dev/null
run status
check '[ $rc -eq 0 ] && echo "$out" | grep -qx BANK1_STATE=shipped'
check 'echo "$out" | grep -qx BANK1_VERSION=V1.0-210702'
check 'echo "$out" | grep -qx BANK0_STATE=valid'
# Where the loader does not name the split, the shipped 1328 KiB, beyond
# these partitions: nothing is found there.
fixture env "$ONU_ROOT/dev/mtd2" 3 bootdelay=5 sw_active=0 sw_commit=0 \
	sw_tryactive=2 sw_valid0=1 sw_valid1=1
run status
check 'echo "$out" | grep -qx BANK1_STATE=empty'
# A bank of this system that holds such bytes as well is this system's.
fixture env "$ONU_ROOT/dev/mtd2" 4 bootdelay=5 sw_active=0 sw_commit=0 \
	sw_tryactive=2 sw_valid0=1 sw_valid1=1 fl_kernel2_sz=8000
dd if="$T/r-old" of="$ONU_ROOT/dev/mtd7" conv=notrunc 2>/dev/null
run status
check 'echo "$out" | grep -qx BANK1_STATE=valid'
teardown

echo "an update file checks out, and damaged ones do not"
setup
run check "$T/update.bin"
check '[ $rc -eq 0 ]'
check 'echo "$out" | grep -qx VERSION=7c5fba548abc'
check 'echo "$out" | grep -qx BOARD=odi,dfp-34x-2c3'
run check - <"$T/update.bin"
check '[ $rc -eq 0 ]'
cp "$T/update.bin" "$T/bad.bin"
printf 'X' | dd of="$T/bad.bin" bs=1 seek=50000 conv=notrunc 2>/dev/null
run check "$T/bad.bin"
check '[ $rc -eq 1 ] && echo "$err" | grep -q "checksum does not match"'
head -c 60000 "$T/update.bin" >"$T/short.bin"
run check "$T/short.bin"
check '[ $rc -eq 1 ] && echo "$err" | grep -q "ends inside"'
python3 "$MKUPDATE" --board other,board --version x --time 1 "$T/k-new" "$T/r-new" "$T/other.bin"
run check "$T/other.bin"
check '[ $rc -eq 1 ] && echo "$err" | grep -q "not for this board"'
teardown

echo "an update goes into the bank that does not run, and reads back"
setup
cp "$ONU_ROOT/dev/mtd4" "$T/k0-before"
run write "$T/update.bin"
check '[ $rc -eq 0 ]'
check 'echo "$out" | grep -qx BANK=1'
check 'cmp -s "$ONU_ROOT/dev/mtd4" "$T/k0-before"'
check 'cmp -s -n 40064 "$ONU_ROOT/dev/mtd6" "$T/k-new"'
check 'cmp -s -n 30000 "$ONU_ROOT/dev/mtd7" "$T/r-new"'
# What follows the images is erased.
check '[ "$(bytes_at "$ONU_ROOT/dev/mtd6" 40064 4)" = ffffffff ]'
check '[ "$(bytes_at "$ONU_ROOT/dev/mtd7" 65532 4)" = ffffffff ]'
check '[ "$(envget sw_valid1)" = 1 ]'
check '[ "$(envget sw_version1)" = 7c5fba548abc ]'
# Two saves, sw_valid1=0 and then 1: one copy is flagged current, the other
# obsolete.
check '[ "$(bytes_at "$ONU_ROOT/dev/mtd1" 4 1)$(bytes_at "$ONU_ROOT/dev/mtd2" 4 1)" = 0001 ]'
run status
check 'echo "$out" | grep -qx BANK1_STATE=valid'
check 'echo "$out" | grep -qx BANK1_VERSION=7c5fba548abc'
check 'echo "$out" | grep -qx BANK1_KERNEL_USED=40064'
check 'echo "$out" | grep -qx BANK1_ROOTFS_USED=30000'
check 'echo "$out" | grep -qx NEXT=0'
# The bank read out is the file that went in, byte for byte.
run read 1
"$BUILD/onu-update" read 1 >"$T/back.bin" 2>/dev/null
check 'cmp -s "$T/back.bin" "$T/update.bin"'
# The running bank reads out with its own version.
"$BUILD/onu-update" read >"$T/running.bin" 2>/dev/null
run check "$T/running.bin"
check '[ $rc -eq 0 ] && echo "$out" | grep -qx VERSION=0123456789ab'
teardown

echo "a trial of the new bank, and its commit once it runs"
setup
"$BUILD/onu-update" write "$T/update.bin" >/dev/null 2>&1
run try
check '[ $rc -eq 0 ] && echo "$out" | grep -qx NEXT=1'
check '[ "$(envget sw_tryactive)" = 1 ] && [ "$(envget onu_trial)" = 1 ]'
check '[ "$(envget sw_commit)" = 0 ]'
# The boot loader starts bank 1 once: it sets sw_tryactive back to 2.
"$BUILD/onu-update" env sw_tryactive=2 sw_active=1 2>/dev/null
printf 'console=ttyS0,115200 root=31:7\n' >"$ONU_ROOT/proc/cmdline"
run status
check 'echo "$out" | grep -qx RUNNING=1'
check 'echo "$out" | grep -qx COMMITTED=0'
check 'echo "$out" | grep -qx NEXT=0'
check 'echo "$out" | grep -qx TRIAL=1 && ! echo "$out" | grep -q FAILED'
run commit
check '[ $rc -eq 0 ]'
check '[ "$(envget sw_commit)" = 1 ]'
check '[ -z "$(envget onu_trial)" ]'
check '[ "$(envget mupgrade_en)" = 0 ]'
run status
check 'echo "$out" | grep -qx COMMITTED=1 && echo "$out" | grep -qx NEXT=1'
check '! echo "$out" | grep -q "TRIAL\|FAILED"'
# From bank 1 the next update goes into bank 0.
run write "$T/update.bin"
check '[ $rc -eq 0 ] && echo "$out" | grep -qx BANK=0'
check 'cmp -s -n 40064 "$ONU_ROOT/dev/mtd4" "$T/k-new"'
teardown

echo "a trial that did not come up is reported, until the bank gets a new system"
setup
"$BUILD/onu-update" write "$T/update.bin" >/dev/null 2>&1
"$BUILD/onu-update" try >/dev/null 2>&1
# The loader took the trial, the tried system failed, and the watchdog or a
# panic restarted the stick into bank 0, the committed one.
"$BUILD/onu-update" env sw_tryactive=2 sw_active=0 2>/dev/null
run status
check 'echo "$out" | grep -qx RUNNING=0 && echo "$out" | grep -qx FAILED=1'
check '! echo "$out" | grep -q TRIAL='
"$BUILD/onu-update" write "$T/update.bin" >/dev/null 2>&1
run status
check '! echo "$out" | grep -q FAILED && [ -z "$(envget onu_trial)" ]'
teardown

echo "an update that fails leaves its bank empty, and no trial of it"
setup
cp "$T/update.bin" "$T/bad.bin"
printf 'X' | dd of="$T/bad.bin" bs=1 seek=50000 conv=notrunc 2>/dev/null
run write "$T/bad.bin"
check '[ $rc -eq 1 ]'
check '[ "$(envget sw_valid1)" = 0 ]'
check '[ "$(bytes_at "$ONU_ROOT/dev/mtd6" 0 4)" = ffffffff ]'
run status
check 'echo "$out" | grep -qx BANK1_STATE=empty'
run try
check '[ $rc -eq 1 ] && echo "$err" | grep -q "no whole system"'
check '[ "$(envget sw_tryactive)" = 2 ]'
teardown

echo "a file for another board, or too big, leaves the bank as it was"
setup
"$BUILD/onu-update" write "$T/update.bin" >/dev/null 2>&1
cp "$ONU_ROOT/dev/mtd6" "$T/k1-before"
python3 "$MKUPDATE" --board other,board --version x --time 1 "$T/k-new" "$T/r-new" "$T/other.bin"
run write "$T/other.bin"
check '[ $rc -eq 1 ]'
check 'cmp -s "$ONU_ROOT/dev/mtd6" "$T/k1-before"'
check '[ "$(envget sw_valid1)" = 1 ]'
fixture uimage "$T/k-big" 3 70000 1
python3 "$MKUPDATE" --board odi,dfp-34x-2c3 --version big --time 1 "$T/k-big" "$T/r-new" "$T/big.bin"
run write "$T/big.bin"
check '[ $rc -eq 1 ] && echo "$err" | grep -q "do not fit"'
check 'cmp -s "$ONU_ROOT/dev/mtd6" "$T/k1-before"'
teardown

echo "after a start from memory no bank runs, and the bank is named"
setup
printf 'rootfs / rootfs rw 0 0\n' >"$ONU_ROOT/proc/mounts"
run status
check 'echo "$out" | grep -qx RUNNING=-1'
run write "$T/update.bin"
check '[ $rc -eq 1 ] && echo "$err" | grep -q "name the bank"'
check '[ "$(bytes_at "$ONU_ROOT/dev/mtd6" 0 4)" = ffffffff ]'
run commit
check '[ $rc -eq 1 ] && [ "$(envget sw_commit)" = 0 ]'
run write "$T/update.bin" 1
check '[ $rc -eq 0 ] && echo "$out" | grep -qx BANK=1'
check 'cmp -s -n 40064 "$ONU_ROOT/dev/mtd6" "$T/k-new"'
teardown

echo "the running bank is never written, even when named"
setup
run write "$T/update.bin" 0
check '[ $rc -eq 1 ] && echo "$err" | grep -q "runs"'
check 'cmp -s -n 30064 "$ONU_ROOT/dev/mtd4" "$T/k-old"'
teardown

echo "the boot loader's environment: print and change"
setup
run env bootdelay
check '[ $rc -eq 0 ] && [ "$out" = bootdelay=5 ]'
run env bootdelay=3 mupgrade_en=0
check '[ $rc -eq 0 ] && [ "$(envget bootdelay)" = 3 ] && [ "$(envget mupgrade_en)" = 0 ]'
# One save: it went into env, now current, and env2 is flagged obsolete.
check '[ "$(bytes_at "$ONU_ROOT/dev/mtd1" 4 1)" = 01 ] && [ "$(bytes_at "$ONU_ROOT/dev/mtd2" 4 1)" = 00 ]'
run env mupgrade_en=
check '[ $rc -eq 0 ]'
run env mupgrade_en
check '[ $rc -eq 1 ]'
run env bootdelay sw_commit=1
check '[ $rc -eq 2 ]'
run env
check 'echo "$out" | grep -qx sw_tryactive=2'
# A change to what is already there saves nothing.
cp "$ONU_ROOT/dev/mtd1" "$T/env-before"
cp "$ONU_ROOT/dev/mtd2" "$T/env2-before"
run env bootdelay=3
check 'cmp -s "$ONU_ROOT/dev/mtd1" "$T/env-before" && cmp -s "$ONU_ROOT/dev/mtd2" "$T/env2-before"'
# Neither copy checks out: nothing is changed.
printf 'XXXX' | dd of="$ONU_ROOT/dev/mtd1" conv=notrunc 2>/dev/null
printf 'XXXX' | dd of="$ONU_ROOT/dev/mtd2" conv=notrunc 2>/dev/null
run env bootdelay=4
check '[ $rc -eq 1 ] && echo "$err" | grep -q "neither copy"'
teardown

echo "test_onu_update: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
