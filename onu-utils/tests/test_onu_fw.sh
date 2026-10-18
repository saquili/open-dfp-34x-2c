#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of onu-fw against the kernel it runs on, read back with nft, on dummy
# interfaces named as the stick's: sfp, a VLAN on it, and a GEM interface.
# A dummy interface carries no flow in hardware, so the kernel refuses the
# flowtable and the table falls back to the CPU, which is tried here; the
# flowtable itself needs the stick, and is tried there. Making the
# interfaces needs root with CAP_NET_ADMIN, as in a container started with
# --cap-add NET_ADMIN; without that, or without nft, the test says that it
# skipped.

set -u
FW=${BUILD:-$(cd "$(dirname "$0")/.." && pwd)/build}/onu-fw
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

if ! command -v nft >/dev/null 2>&1 || ! nft list tables >/dev/null 2>&1 ||
	[ -e /sys/class/net/sfp ] || ! ip link add sfp type dummy 2>/dev/null; then
	echo "test_onu_fw: SKIPPED: it needs root with CAP_NET_ADMIN, nft, and no interface sfp"
	exit 0
fi
T=$(mktemp -d)
cleanup() {
	nft delete table inet onu 2>/dev/null
	ip link del net1 2>/dev/null
	ip link del sfp 2>/dev/null
	ip link del gem257 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT
ip link add gem257 type dummy && ip link add link sfp name net1 type vlan id 100 || exit 1

run() {
	out=$("$FW" "$@" 2>"$T/err")
	rc=$?
	err=$(cat "$T/err")
}

table() {
	nft list table inet onu 2>/dev/null
}

has() {
	table | grep -qF -- "$1"
}

echo "the NAT and the protection of the local networks"
printf 'lan net1 192.168.1.0/24\nlan sfp 10.0.0.0/8\nwan gem257\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && echo "$err" | grep -q "flowtable was refused"'
check 'has "chain forward" && has "chain postrouting"'
check 'has "ct state established,related accept"'
check "has 'oifname \"ppp*\" tcp flags syn tcp option maxseg size set rt mtu'"
check "has 'iifname \"ppp*\" oifname \"net1\" ct state new drop'"
check "has 'oifname \"ppp*\" ip saddr 192.168.1.0/24 masquerade'"
check "has 'oifname \"ppp*\" ip saddr 10.0.0.0/8 masquerade'"
check '! has "flowtable" && ! has "flow add"'

echo "a table replaces the one there is, whole"
printf 'lan net1 172.16.0.0/12\noffload off\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check "has 'ip saddr 172.16.0.0/12 masquerade' && ! has '192.168.1.0' && ! has '10.0.0.0'"
check '[ "$(nft list tables | grep -c "inet onu")" -eq 1 ]'

echo "what is no table changes nothing"
printf 'lan net1 172.16.0.1/12\n' >"$T/bad"
run apply "$T/bad"
check '[ $rc -eq 2 ] && echo "$err" | grep -q "no network of the table"'
check "has 'ip saddr 172.16.0.0/12 masquerade'"
run apply "$T/none"
check '[ $rc -eq 2 ]'
run frobnicate
check '[ $rc -eq 2 ]'

echo "the table taken away, and again"
run clear
check '[ $rc -eq 0 ] && ! nft list tables | grep -q "inet onu"'
run clear
check '[ $rc -eq 0 ]'

echo "test_onu_fw: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
