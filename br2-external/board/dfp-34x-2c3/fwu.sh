#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# fwu.sh of web-install.tar, the package of open-dfp-34x-2c that the shipped
# firmware's web page installs (ADR 0014, step 9). The shipped firmware's
# configd runs it as root, through /etc/scripts/fwu_starter.sh, as
#
#   sh fwu.sh BANK FILE
#
# once the page took FILE; BANK is the bank that firmware did not start from.
# It writes this system into that bank in the shipped firmware's split of it:
# k, the first 1328 KiB of this system's bank, into k<BANK>, and r, the rest,
# into r<BANK>, so that the kernel and the root filesystem land where this
# system's own split puts them. It reads both partitions back, notes the
# bank's version for the boot loader and has the loader try the bank once.
#
# It exits 1 on success too: configd then does not make the bank the one the
# loader starts for good, and restarts the stick all the same. This system
# makes its bank the committed one itself once it is up (onu-trial); a system
# that does not come up is restarted by the watchdog into the shipped
# firmware's bank. The page says that the update failed.
#
# Each image is written into its partition opened for reading and writing,
# which never truncates what it opens, so that a partition and a file that
# stands in for one read back the same.
#
# The shipped firmware's BusyBox has no dd, head or wc, so the images come
# whole in FILE, and the digests of the partitions as they read after the
# write, the images and the erased flash after them, are written into this
# script when the package is made (mkweb.py). FWU_ROOT, when set, is put in
# front of /proc/mtd and the devices, and nv, flash_eraseall and md5sum are
# looked up in PATH, for the tests.

K_MD5=@K_MD5@
R_MD5=@R_MD5@
VERSION=@VERSION@

say() {
	echo "fwu.sh: $*"
}

bank=$1
file=$2
case $bank in
0 | 1) ;;
*)
	say "no bank $bank"
	exit 2
	;;
esac
[ -f "$file" ] || {
	say "no package $file"
	exit 2
}

# The shipped firmware's partitions of the bank, by name and size: 1328 KiB
# of kernel, 2512 KiB of root filesystem.
k=$(sed -n "s/^\(mtd[0-9]*\): 0014c000 [0-9a-f]* \"k$bank\"$/\1/p" "${FWU_ROOT:-}/proc/mtd")
r=$(sed -n "s/^\(mtd[0-9]*\): 00274000 [0-9a-f]* \"r$bank\"$/\1/p" "${FWU_ROOT:-}/proc/mtd")
[ -n "$k" ] && [ -n "$r" ] || {
	say "k$bank and r$bank are not the partitions of this stick's shipped firmware"
	exit 2
}
k=${FWU_ROOT:-}/dev/$k
r=${FWU_ROOT:-}/dev/$r

say "open-dfp-34x-2c $VERSION into bank $bank"
# A bank half written is no bank to try.
nv setenv sw_valid$bank 0
flash_eraseall "$k" && tar -xOf "$file" k 1<>"$k" || {
	say "writing k$bank failed"
	exit 3
}
flash_eraseall "$r" && tar -xOf "$file" r 1<>"$r" || {
	say "writing r$bank failed"
	exit 3
}
[ "$(md5sum <"$k" | cut -d ' ' -f 1)" = "$K_MD5" ] || {
	say "k$bank does not read back as written"
	exit 3
}
[ "$(md5sum <"$r" | cut -d ' ' -f 1)" = "$R_MD5" ] || {
	say "r$bank does not read back as written"
	exit 3
}
nv setenv sw_version$bank "$VERSION"
nv setenv sw_valid$bank 1
nv setenv onu_trial $bank
nv setenv sw_tryactive $bank
say "bank $bank is tried at the next start; the page says the update failed"
exit 1
