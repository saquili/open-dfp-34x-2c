#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of onu-drop: the account a program runs as, and the powers it keeps.
# It must start as root, and run as an account that is not root's, so the
# tests need root and an account such as "nobody"; without them the test says
# that it skipped. The command line is tried without either.

set -u
DROP=${BUILD:-$(cd "$(dirname "$0")/.." && pwd)/build}/onu-drop
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

# A wrong command line is 2 and says how it goes.
for line in "" "nobody" "-c" "-c net_raw" "-c net_raw nobody" "-c net_raw,sys_admin nobody true" \
	"-c , nobody true" "-c net_raw, nobody true" "-c ,net_raw nobody true" \
	"-c net_raw,,net_admin nobody true" "-c NET_RAW nobody true"; do
	# $line is split into words on purpose.
	# shellcheck disable=SC2086
	err=$("$DROP" $line 2>&1 >/dev/null)
	rc=$?
	check '[ $rc -eq 2 ] && echo "$err" | grep -q "^usage: onu-drop "'
done

if [ "$(id -u)" != 0 ] || ! id nobody >/dev/null 2>&1; then
	echo "test_onu_drop: $checks checks, $failures failed; the rest SKIPPED: it needs root and the account nobody"
	[ "$failures" -eq 0 ]
	exit
fi

# The word of /proc/self/status named $1, as the program that is run sees it.
caps() {
	"$DROP" "$@" sh -c 'sed -n "s/^$0:[[:space:]]*//p" /proc/self/status' "$WORD"
}
status() {
	WORD=$1
	shift
	caps "$@"
}
mask() {
	printf '%016x' "$1"
}

check '[ "$("$DROP" nobody id -u)" != 0 ]'
check '[ "$("$DROP" nobody id -u)" = "$(id -u nobody)" ]'
check '[ "$("$DROP" nobody id -g)" = "$(id -g nobody)" ]'
# Without -c it keeps none.
for word in CapInh CapPrm CapEff CapBnd CapAmb; do
	check '[ "$(status $word nobody)" = 0000000000000000 ]'
done
# CAP_NET_RAW is 13, CAP_NET_BIND_SERVICE 10, CAP_NET_ADMIN 12, CAP_SYS_TIME 25.
raw=$(mask $((1 << 13)))
for word in CapInh CapPrm CapEff CapBnd CapAmb; do
	check '[ "$(status $word -c net_raw nobody)" = "$raw" ]'
done
# Two of them: a container does not hold net_admin and sys_time to give.
both=$(mask $(((1 << 13) | (1 << 10))))
for word in CapPrm CapEff CapBnd CapAmb; do
	check '[ "$(status $word -c net_raw,net_bind_service nobody)" = "$both" ]'
done
# What it runs keeps them, and can gain no more.
check '[ "$("$DROP" -c net_raw nobody "$DROP" -c net_raw nobody id -u 2>&1)" = "onu-drop: it is started as root" ]'
check '[ "$("$DROP" -c net_raw nobody sh -c "sh -c \"sed -n s/^CapEff:.//p /proc/self/status\"")" = "$raw" ]'
nnp=$("$DROP" nobody sed -n 's/^NoNewPrivs:[[:space:]]*//p' /proc/self/status)
check '[ "$nnp" = 1 ]'
# What cannot be done is an error, and the program is not run.
err=$("$DROP" root true 2>&1)
rc=$?
check '[ $rc -eq 1 ] && echo "$err" | grep -q "root is no account to run as"'
err=$("$DROP" nosuchaccount0 true 2>&1)
rc=$?
check '[ $rc -eq 1 ] && echo "$err" | grep -q "nosuchaccount0 is no account to run as"'
err=$("$DROP" nobody /nonexistent/program 2>&1)
rc=$?
check '[ $rc -eq 1 ] && echo "$err" | grep -q "^onu-drop: /nonexistent/program: "'

echo "test_onu_drop: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
