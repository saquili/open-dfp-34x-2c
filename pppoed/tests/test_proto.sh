#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of pppoed as netifd's protocol handler, as netifd runs it: with
# pppoed-none, which prints what it would send to netifd. BUILD names the
# directory of the build.

set -u
P=${BUILD:-build}/pppoed-none
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

run() {
	out=$("$P" "$@" 2>/dev/null)
	rc=$?
}

echo "what the protocol takes"
run '' dump
check '[ $rc -eq 0 ] && echo "$out" | grep -q "^{\"name\":\"pppoed\",\"config\":\[\[\"session\",5\]"'

echo "setup: netifd runs pppoed for the session, on the interface's device"
run pppoed setup ses1 '{ "session": 1, "family": "ipv4", "pass": true, "defaultroute": true }' wan1410
check '[ $rc -eq 0 ]'
check '[ "$out" = "notify ses1 {\"action\":1,\"command\":[\"/usr/sbin/pppoed\",\"up\",\"-i\",\"wan1410\",\"-f\",\"/run/onu-pppoe1\",\"-M\",\"-N\",\"ses1\",\"-o\",\"/run/session/1\",\"-w\",\"0\",\"-4\",\"-P\",\"-D\"]}" ]'
run pppoed setup ses3 '{ "session": 3, "family": "both", "pass": false, "defaultroute": false }' gem1024
check 'echo "$out" | grep -q "\"-i\",\"gem1024\",\"-f\",\"/run/onu-pppoe3\"" && echo "$out" | grep -q "\"-w\",\"6\"\]}$"'
check '! echo "$out" | grep -q -e "\"-4\"" -e "\"-6\"" -e "\"-P\"" -e "\"-D\""'
run pppoed setup ses2 '{ "session": 2, "family": "ipv6" }' wan1410
check 'echo "$out" | grep -q "\"-6\"\]}$"'

echo "teardown: netifd ends it"
run pppoed teardown ses1 '{ "session": 1 }' wan1410
check '[ $rc -eq 0 ] && [ "$out" = "notify ses1 {\"action\":2,\"signal\":15}" ]'

echo "no session, no device, or no interface: nothing is run"
run pppoed setup ses9 '{ "session": 9 }' wan1410
check '[ $rc -ne 0 ] && [ -z "$out" ]'
run pppoed setup ses1 '{ "session": 1 }'
check '[ $rc -ne 0 ] && [ -z "$out" ]'
run pppoed setup
check '[ $rc -ne 0 ] && [ -z "$out" ]'

echo "up: what a killed session left is gone before the next starts"
dir=$(mktemp -d)
for f in ppp ipv4 ipv6 dns peer; do echo stale >"$dir/$f"; done
"$P" up -i nosuch0 -o "$dir" -w 0 >/dev/null 2>&1
rc=$?
check '[ $rc -ne 0 ] && [ -z "$(ls "$dir")" ]'
rm -rf "$dir"

echo "up under netifd: the session ends with netifd, and only then"
dir=$(mktemp -d)
gone() {
	! [ -d "/proc/$1" ] || grep -q "^State:.*Z" "/proc/$1/status"
}
# Each parent ends after a second; the children would wait five.
sh -c "\"$P\" up -N ses1 -i nosuch0 -w 5 >/dev/null 2>&1 & echo \$! >\"$dir/netifd\"; sleep 1"
sh -c "\"$P\" up -i nosuch0 -w 5 >/dev/null 2>&1 & echo \$! >\"$dir/alone\"; sleep 1"
sleep 1
check 'gone "$(cat "$dir/netifd")"'
check '! gone "$(cat "$dir/alone")"'
kill "$(cat "$dir/alone")" 2>/dev/null
rm -rf "$dir"

echo "test_proto: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
