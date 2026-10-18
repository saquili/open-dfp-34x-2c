#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of onu-fw against the kernel it runs on, read back with nft, on dummy
# interfaces named as the stick's: sfp, a VLAN on it, a GEM interface and a
# VLAN on that, the interface of an IPoE connection.
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
	nft delete table inet ref 2>/dev/null
	ip link del net1 2>/dev/null
	ip link del wan421 2>/dev/null
	ip link del sfp 2>/dev/null
	ip link del gem257 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT
ip link add gem257 type dummy && ip link add link sfp name net1 type vlan id 100 &&
	ip link add link gem257 name wan421 type vlan id 421 || exit 1

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
printf 'lan net1 192.168.1.0/24\nlan sfp 10.0.0.0/8\nwan gem257\nout ppp*\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && echo "$err" | grep -q "flowtable was refused"'
check 'has "chain forward" && has "chain postrouting"'
check 'has "ct state established,related accept"'
check "has 'oifname \"ppp*\" tcp flags syn tcp option maxseg size set rt mtu'"
check "has 'iifname \"ppp*\" oifname \"net1\" ct state new drop'"
check "has 'oifname \"ppp*\" ip saddr 192.168.1.0/24 masquerade'"
check "has 'oifname \"ppp*\" ip saddr 10.0.0.0/8 masquerade'"
check '! has "flowtable" && ! has "flow add"'
check '! has "chain prerouting" && ! has "ct status dnat"'

echo "out by a session and by an IPoE connection"
printf 'lan net1 192.168.1.0/24\nwan gem257\nout ppp*\nout wan421\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ]'
check "has 'oifname \"wan421\" tcp flags syn tcp option maxseg size set rt mtu'"
check "has 'iifname \"wan421\" oifname \"net1\" ct state new drop'"
check "has 'oifname \"wan421\" ip saddr 192.168.1.0/24 masquerade'"
check "has 'oifname \"ppp*\" ip saddr 192.168.1.0/24 masquerade'"

echo "the ports forwarded and a DMZ"
{
	printf 'lan net1 192.168.1.0/24\nwan gem257\nout ppp*\nout wan421\n'
	printf 'fwd ppp0 203.0.113.24 tcp 8080 192.168.1.20 80\n'
	printf 'fwd ppp0 203.0.113.24 udp 6000-6010 192.168.1.20 6000\n'
	printf 'fwd wan421 192.0.2.10 tcp 443 192.168.1.21 443\n'
	printf 'dmz wan421 192.0.2.10 192.168.1.30\n'
} >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && echo "$err" | grep -q "flowtable was refused"'
check 'has "chain prerouting" && has "type nat hook prerouting priority dstnat; policy accept;"'
check "has 'iifname \"ppp0\" ip daddr 203.0.113.24 tcp dport 8080 dnat ip to 192.168.1.20:80'"
check "has 'iifname \"ppp0\" ip daddr 203.0.113.24 udp dport 6000-6010 dnat ip to 192.168.1.20'"
check "has 'iifname \"wan421\" ip daddr 192.0.2.10 tcp dport 443 dnat ip to 192.168.1.21:443'"
check "has 'iifname \"wan421\" ip daddr 192.0.2.10 udp dport 68 accept'"
check "has 'iifname \"wan421\" ip daddr 192.0.2.10 meta l4proto != icmp dnat ip to 192.168.1.30'"
# What the forwards took is let through after the answers, before the drops.
at() {
	table | grep -nF -- "$1" | head -n 1 | cut -d: -f1
}
check '[ "$(at "ct state established,related accept")" -lt "$(at "ct status dnat accept")" ] &&
	[ "$(at "ct status dnat accept")" -lt "$(at "ct state new drop")" ]'
# The DMZ comes after the forwards of its connection.
check '[ "$(at "tcp dport 443 dnat")" -lt "$(at "udp dport 68 accept")" ] &&
	[ "$(at "udp dport 68 accept")" -lt "$(at "l4proto != icmp")" ]'

echo "the forwards as nft itself makes them"
nft add table inet ref &&
	nft add chain inet ref prerouting '{ type nat hook prerouting priority dstnat; }' &&
	nft add chain inet ref forward '{ type filter hook forward priority 0; }' &&
	nft add rule inet ref prerouting iifname "ppp0" ip daddr 203.0.113.24 tcp dport 8080 \
		dnat ip to 192.168.1.20:80 &&
	nft add rule inet ref prerouting iifname "ppp0" ip daddr 203.0.113.24 udp dport 6000-6010 \
		dnat ip to 192.168.1.20 &&
	nft add rule inet ref prerouting iifname "wan421" ip daddr 192.0.2.10 tcp dport 443 \
		dnat ip to 192.168.1.21:443 &&
	nft add rule inet ref prerouting iifname "wan421" ip daddr 192.0.2.10 udp dport 68 accept &&
	nft add rule inet ref prerouting iifname "wan421" ip daddr 192.0.2.10 \
		meta l4proto != icmp dnat ip to 192.168.1.30 &&
	nft add rule inet ref forward ct status dnat accept
# The expressions of the rules of a chain, as the kernel gives them back.
bytecode() {
	nft --debug=netlink list chain inet "$1" "$2" 2>/dev/null | grep '^  \['
}
check '[ -n "$(bytecode ref prerouting)" ] &&
	[ "$(bytecode onu prerouting)" = "$(bytecode ref prerouting)" ]'
check '[ "$(bytecode onu forward | grep -A3 "ct load status")" = "$(bytecode ref forward)" ]'
nft delete table inet ref

echo "the longest table: every network by every way out, every forward of each protocol"
{
	for i in 1 2 3 4 5 6 7 8; do
		echo "lan net1 192.168.$i.0/24"
	done
	echo "out ppp*"
	for i in 1 2 3 4 5 6; do
		echo "out wan$((3000 + i))"
	done
	i=0
	while [ $i -lt 96 ]; do
		echo "fwd averylongname1 203.0.113.24 tcp $((10000 + 10 * i))-$((10009 + 10 * i)) 192.168.1.20 $((10000 + 10 * i))"
		echo "fwd averylongname1 203.0.113.24 udp $((10000 + 10 * i))-$((10009 + 10 * i)) 192.168.1.20 $((10000 + 10 * i))"
		i=$((i + 1))
	done
	for i in 1 2 3 4 5 6 7; do
		echo "dmz averylongname$i 203.0.113.$i 192.168.1.30"
	done
	echo "offload off"
} >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check '[ "$(table | grep -c "dnat ip to")" -eq $((192 + 7)) ]'

echo "a table replaces the one there is, whole"
printf 'lan net1 172.16.0.0/12\nout ppp*\noffload off\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check "has 'ip saddr 172.16.0.0/12 masquerade' && ! has '192.168.1.0' && ! has '10.0.0.0'"
check '! has "chain prerouting" && ! has "dnat"'
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
