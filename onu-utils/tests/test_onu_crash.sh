#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-crash, with a directory in place of /sys/fs/pstore and
# a stand-in for logger.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
CRASH=$HERE/scripts/onu-crash
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

T=$(mktemp -d)
export ONU_ROOT=$T/root
mkdir -p "$ONU_ROOT/sys/fs/pstore" "$T/bin"
printf '#!/bin/sh\necho "logger $*" >>"%s/log"\n' "$T" >"$T/bin/logger"
chmod +x "$T/bin/logger"
PATH=$T/bin:$PATH

echo "the records go, and nothing else"
printf 'Panic#1 Part1\n' >"$ONU_ROOT/sys/fs/pstore/dmesg-ramoops-0"
printf 'boot\n' >"$ONU_ROOT/sys/fs/pstore/console-ramoops-0"
"$CRASH" clear
rc=$?
check '[ $rc -eq 0 ] && [ -z "$(ls "$ONU_ROOT/sys/fs/pstore")" ]'
check 'grep -q "2 records" "$T/log"'
check '[ -d "$ONU_ROOT/sys/fs/pstore" ]'

echo "none to remove is no error, and says nothing"
: >"$T/log"
"$CRASH" clear
rc=$?
check '[ $rc -eq 0 ] && [ ! -s "$T/log" ]'

echo "no pstore at all is no error either"
rm -rf "$ONU_ROOT/sys/fs/pstore"
"$CRASH" clear
rc=$?
check '[ $rc -eq 0 ]'

echo "anything else is a mistake"
"$CRASH" 2>/dev/null
rc=$?
check '[ $rc -eq 2 ]'
"$CRASH" show 2>/dev/null
rc=$?
check '[ $rc -eq 2 ]'

rm -rf "$T"
echo "test_onu_crash: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
