#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-net, with stand-ins for pidof, onu-nl and logger. The
# stand-in netifd is a shell that notes each SIGHUP in $T/hups; pidof names
# it while $T/netifd-runs is there. The stand-in onu-nl answers
# "addr-wait DEV ADDRESS SECONDS" as the real one does, from the address in
# $T/addr-DEV, which is in the file or not (the real one is tried in
# test_onu_nl).

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
NET=$HERE/scripts/onu-net
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

setup() {
	T=$(mktemp -d)
	export ONU_ROOT=$T/root
	mkdir -p "$ONU_ROOT/run" "$T/bin"
	sh -c 'trap "echo hup >>\"$1/hups\"" HUP; while :; do sleep 0.1; done' sh "$T" &
	NETIFD=$!
	printf '#!/bin/sh\n[ "$1" = netifd ] && [ -e "%s/netifd-runs" ] && echo %s\nexit 0\n' \
		"$T" "$NETIFD" >"$T/bin/pidof"
	printf '#!/bin/sh\necho "onu-nl $*" >>"%s/calls"\n[ "$1" = addr-wait ] || exit 2\ngrep -q -F "$3" "%s/addr-$2" 2>/dev/null || exit 1\n' \
		"$T" "$T" >"$T/bin/onu-nl"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	kill "$NETIFD" 2>/dev/null
	rm -rf "$T"
}

hups() {
	sleep 0.3
	wc -l <"$T/hups" 2>/dev/null | tr -d ' ' || echo 0
}

echo "the parts make the whole, in the order of their names"
setup
printf "config interface 'b'\n" | "$NET" set b
rc=$?
check '[ $rc -eq 0 ]'
printf "config interface 'a'\n" | "$NET" set a
check '[ "$(cat "$ONU_ROOT/run/network.d/a")" = "config interface '"'a'"'" ]'
check '[ "$(cat "$ONU_ROOT/run/config/network")" = "$(printf "config interface '"'a'"'\nconfig interface '"'b'"'")" ]'
printf "config interface 'a2'\n" | "$NET" set a
check 'grep -q a2 "$ONU_ROOT/run/config/network" && ! grep -q "'"'a'"'" "$ONU_ROOT/run/config/network"'
"$NET" del b
rc=$?
check '[ $rc -eq 0 ] && ! grep -q "'"'b'"'" "$ONU_ROOT/run/config/network"'
check '[ ! -e "$ONU_ROOT/run/network.d/b" ]'
check '[ -z "$(ls -A "$ONU_ROOT/run/config" | grep -v "^network$")" ]'
check '[ ! -e "$T/hups" ]'
teardown

echo "netifd, when it runs, is told with SIGHUP"
setup
: >"$T/netifd-runs"
printf "config interface 'a'\n" | "$NET" set a
check '[ "$(hups)" -eq 1 ]'
"$NET" del a
check '[ "$(hups)" -eq 2 ]'
check '[ ! -s "$ONU_ROOT/run/config/network" ]'
check 'kill -0 "$NETIFD"'
teardown

echo "what is not a name of a part changes nothing"
setup
for bad in "" "A" "a/b" "../x" ".a" "a b"; do
	printf 'x\n' | "$NET" set "$bad" 2>/dev/null
	rc=$?
	check '[ $rc -eq 2 ]'
	"$NET" del "$bad" 2>/dev/null
	rc=$?
	check '[ $rc -eq 2 ]'
done
check '[ ! -e "$ONU_ROOT/run/config/network" ]'
check '[ -z "$(ls -A "$ONU_ROOT/run/network.d" 2>/dev/null)" ]'
teardown

echo "waiting for an address"
setup
echo "198.51.100.2/30" >"$T/addr-mgmt"
"$NET" wait mgmt 198.51.100.2/30 1
rc=$?
check '[ $rc -eq 0 ] && grep -qx "onu-nl addr-wait mgmt 198.51.100.2/30 1" "$T/calls"'
"$NET" wait mgmt 198.51.100.2/31 0
rc=$?
check '[ $rc -eq 1 ]'
"$NET" wait other 198.51.100.2/30 0
rc=$?
check '[ $rc -eq 1 ]'
# The seconds default to ten, and the wait is the kernel's, not this script's.
"$NET" wait mgmt 198.51.100.2/30
check 'grep -qx "onu-nl addr-wait mgmt 198.51.100.2/30 10" "$T/calls"'
for bad in "mgmt" "mgmt 198.51.100.2/30 x" "mgmt 198.51.100.2/30 -1"; do
	# shellcheck disable=SC2086
	"$NET" wait $bad 2>/dev/null
	rc=$?
	check '[ $rc -eq 2 ]'
done
teardown

echo "test_onu_net: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
